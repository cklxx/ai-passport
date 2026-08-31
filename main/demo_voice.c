// Path B voice input — the device is a BLE wireless microphone for the PC.
//
// The device has no Wi-Fi in its use environment, so audio streams over a BLE
// GATT service (voice_ble.c) direct to a paired PC running the recv-ble agent.
// 16 kHz raw PCM is sent frame by frame — stateless, so a dropped BLE notify
// costs only that frame instead of corrupting the stream (ADPCM would drift).
//
//   OK   = start / stop the mic
//   DOWN = 发送  (control -> PC injects Enter)
//   UP   = 删除  (control -> PC injects Backspace)
//
// Audio never touches the LVGL/button task: a worker owns capture + encode +
// notify. Capture streams in small blocks; no whole-recording buffer.
#include "demo.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "island_quota.h"
#include "ui_pixel.h"
#include "voice_ble.h"
#include "voice_proto.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "demo_voice";

#define VOICE_SAMPLE_RATE 16000
#define VOICE_CHUNK_SAMPLES 160                       // 10 ms @ 16 kHz; 320 B PCM fits one BLE notify (MTU 512)

// ST_CONNECTING here means "advertising / waiting for the PC to subscribe".
typedef enum { ST_CONNECTING, ST_IDLE, ST_RECORDING, ST_ERROR } voice_state_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_title;
static lv_obj_t *s_big;
static lv_obj_t *s_sub;
static lv_obj_t *s_hint;
static lv_obj_t *s_battery;
static lv_obj_t *s_island;
static lv_timer_t *s_timer;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_worker_done;
static TaskHandle_t s_worker;

static volatile voice_state_t s_state = ST_CONNECTING;
static volatile bool s_want_record;      // UI -> worker: capture on/off
static volatile bool s_closing;
static volatile int s_pending_ctrl;      // UI -> worker: one-shot ctrl code
static volatile unsigned s_elapsed_ms;
static unsigned s_tx_frames;   // audio frames encoded (diagnostic)
static unsigned s_tx_ok;       // frames the BLE stack accepted (diagnostic)
static int s_battery_percent = -1;
static island_quota_t s_quota;
static bool s_have_quota;

static void set_state(voice_state_t st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = st;
    xSemaphoreGive(s_lock);
}

// BLE host task calls this when the PC writes a quota packet to the control
// characteristic. Parse and stash under the lock; render() picks it up.
static void on_quota(const uint8_t *data, size_t len)
{
    island_quota_t q;
    if (!island_quota_parse(data, len, &q)) return;
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_quota = q;
    s_have_quota = true;
    xSemaphoreGive(s_lock);
}

static void worker_task(void *arg)
{
    (void)arg;
    static int16_t pcm[VOICE_CHUNK_SAMPLES];
    bool capturing = false;

    if (voice_ble_start() != ESP_OK) {
        ESP_LOGW(TAG, "BLE start failed");
        set_state(ST_ERROR);
        goto done;
    }
    voice_ble_set_quota_cb(on_quota);   // PC pushes Claude quota over the same link
    if (bsp_audio_set_format(VOICE_SAMPLE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "audio format set failed");
        set_state(ST_ERROR);
        goto done;
    }

    while (!s_closing) {
        bool ready = voice_ble_ready();
        // One-shot control (send/delete) — only meaningful once connected.
        int ctrl = s_pending_ctrl;
        if (ctrl != 0) {
            s_pending_ctrl = 0;
            if (ready && voice_ctrl_valid(ctrl)) voice_ble_send_ctrl((uint8_t)ctrl);
        }

        if (s_want_record && ready && !capturing) {
            capturing = true; s_elapsed_ms = 0;
            ESP_LOGI(TAG, "recording START");
            voice_ble_send_ctrl(VOICE_CTRL_START);
            set_state(ST_RECORDING);
        } else if ((!s_want_record || !ready) && capturing) {
            capturing = false;
            ESP_LOGI(TAG, "recording STOP");
            if (ready) voice_ble_send_ctrl(VOICE_CTRL_STOP);
            set_state(ready ? ST_IDLE : ST_CONNECTING);
        } else if (!capturing) {
            set_state(ready ? ST_IDLE : ST_CONNECTING);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        // Send raw 16-bit PCM: stateless, so a dropped BLE notify costs only that
        // 10 ms frame instead of corrupting the whole stream (ADPCM would drift).
        bool ok = voice_ble_send_audio((const uint8_t *)pcm, sizeof(pcm));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_tx_frames++;
        if (ok) s_tx_ok++;
        xSemaphoreGive(s_lock);
        s_elapsed_ms += VOICE_CHUNK_SAMPLES * 1000 / VOICE_SAMPLE_RATE;
    }

done:
    voice_ble_set_quota_cb(NULL);
    voice_ble_stop();
    s_worker = NULL;
    xSemaphoreGive(s_worker_done);
    vTaskDelete(NULL);
}

static void render(lv_timer_t *t)
{
    (void)t;
    voice_state_t st;
    unsigned ms;
    unsigned tx, ok;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st = s_state;
    ms = s_elapsed_ms;
    tx = s_tx_frames;
    ok = s_tx_ok;
    int bp = bsp_battery_soc();
    if (bp >= 0) s_battery_percent = bp;
    bool have_q = s_have_quota;
    island_quota_t q = s_quota;
    xSemaphoreGive(s_lock);

    char battery[8];
    snprintf(battery, sizeof(battery), s_battery_percent >= 0 ? "%d%%" : "--%%",
             s_battery_percent > 100 ? 100 : s_battery_percent);
    lv_label_set_text(s_battery, battery);

    // Claude quota island: PC pushes used_percentage; the device has no synced
    // wall clock, so it shows remaining % only (no fabricated countdown).
    if (!have_q) {
        lv_label_set_text(s_island, "Claude --");
    } else if (q.remaining_pct < 0) {
        lv_label_set_text(s_island, "Claude 余量未知");
    } else {
        lv_label_set_text_fmt(s_island, "Claude 7天剩余 %d%%", q.remaining_pct);
    }

    switch (st) {
    case ST_CONNECTING:
        lv_label_set_text(s_big, "· · ·");
        lv_label_set_text(s_sub, "等待电脑蓝牙连接");
        lv_label_set_text(s_hint, "电脑配对 AI-Passport-Mic");
        break;
    case ST_IDLE:
        lv_label_set_text(s_big, "按住说话");
        lv_label_set_text(s_sub, "确定：开始语音输入");
        lv_label_set_text(s_hint, "确定 开始   下 发送   上 删除");
        break;
    case ST_RECORDING: {
        char t2[40];
        // Show elapsed time; append a warning if the BLE link is dropping frames
        // (congested). A healthy link shows just the timer.
        bool healthy = (tx == 0 || ok * 100 >= tx * 95);
        snprintf(t2, sizeof(t2), healthy ? "%u.%us" : "%u.%us  信号弱",
                 ms / 1000, ms % 1000 / 100);
        lv_label_set_text(s_big, "● 录音中");
        lv_label_set_text(s_sub, t2);
        lv_label_set_text(s_hint, "确定 停止   下 发送   上 删除");
        break;
    }
    case ST_ERROR:
        lv_label_set_text(s_big, "✕");
        lv_label_set_text(s_sub, "蓝牙或音频不可用");
        lv_label_set_text(s_hint, "长按确定：返回");
        break;
    }
}

void demo_voice_enter(void)
{
    s_state = ST_CONNECTING;
    s_want_record = false;
    s_closing = false;
    s_pending_ctrl = 0;
    s_battery_percent = -1;
    s_have_quota = false;
    s_lock = xSemaphoreCreateMutex();
    s_worker_done = xSemaphoreCreateBinary();

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);

    s_title = lv_label_create(s_scr);
    lv_label_set_text(s_title, "语音输入");
    lv_obj_set_style_text_font(s_title, &lv_font_ai_passport_14, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_PAPER), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 12, 10);

    s_battery = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_battery, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_battery, lv_color_hex(UI_MUTED), 0);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -12, 10);

    // Claude quota island: a pill strip below the header.
    s_island = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_island, &lv_font_ai_passport_14, 0);
    lv_obj_set_style_text_color(s_island, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_bg_color(s_island, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(s_island, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_island, 10, 0);
    lv_obj_set_style_pad_hor(s_island, 10, 0);
    lv_obj_set_style_pad_ver(s_island, 3, 0);
    lv_obj_align(s_island, LV_ALIGN_TOP_MID, 0, 34);
    lv_label_set_text(s_island, "Claude --");

    s_big = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_big, &lv_font_ai_passport_14, 0);
    lv_obj_set_style_text_color(s_big, lv_color_hex(UI_YELLOW), 0);
    lv_obj_align(s_big, LV_ALIGN_CENTER, 0, -20);

    s_sub = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_sub, &lv_font_ai_passport_14, 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(UI_PAPER), 0);
    lv_obj_align(s_sub, LV_ALIGN_CENTER, 0, 16);

    s_hint = lv_label_create(s_scr);
    lv_obj_set_width(s_hint, 232);
    lv_obj_set_style_text_font(s_hint, &lv_font_ai_passport_14, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_screen_load(s_scr);

    if (s_lock == NULL || s_worker_done == NULL) {
        lv_label_set_text(s_sub, "内存不足，请重启设备");
        return;
    }
    s_timer = lv_timer_create(render, 100, NULL);
    if (xTaskCreate(worker_task, "voice", 6144, NULL, 6, &s_worker) != pdPASS) {
        s_state = ST_ERROR;
    }
}

void demo_voice_exit(void)
{
    s_closing = true;
    s_want_record = false;
    if (s_worker != NULL && s_worker_done != NULL) {
        xSemaphoreTake(s_worker_done, pdMS_TO_TICKS(2000));
    }
    if (s_timer != NULL) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr != NULL) { lv_obj_delete(s_scr); s_scr = NULL; }
    if (s_worker_done != NULL) { vSemaphoreDelete(s_worker_done); s_worker_done = NULL; }
    if (s_lock != NULL) { vSemaphoreDelete(s_lock); s_lock = NULL; }
}

void demo_voice_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;
    voice_state_t st = s_state;
    if (st == ST_CONNECTING || st == ST_ERROR) return;
    if (btn == BSP_BTN_OK)   s_want_record = !s_want_record;
    if (btn == BSP_BTN_DOWN) s_pending_ctrl = VOICE_CTRL_SEND;
    if (btn == BSP_BTN_UP)   s_pending_ctrl = VOICE_CTRL_DELETE;
}
