// main/main.c —— FoloToy AI Passport: boot straight into the product.
//
// There is no debug menu in the shipping product. Boot goes directly to the
// voice (BLE wireless-mic) home, which needs no network — the device's use
// environment has no Wi-Fi. Feishu is opt-in: OK long-press from voice runs the
// networked onboarding (Wi-Fi + Feishu bind) and then opens the messenger.
// Button semantics inside the messenger are owned by demo_feishu.c; the OK
// long-press "back" is intercepted here so the messenger stays at its root.
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "demo.h"
#include "product_onboarding.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"     // OK 长按按时长分档,见 on_key

static const char *TAG = "main";

// 0 = voice input (home); 1 = onboarding (Feishu bind); 2 = Feishu messenger.
static int s_active = 0;

// OK 长按离开当前屏幕所需的按住时长。组件的全局长按阈值(1 秒)归 UP 的按住删除
// 所有 —— 那里的等待被直接感知,必须短。离开屏幕会中断录音,要难得多才对。
#define OK_EXIT_HOLD_MS 2500

// Onboarding finished binding Feishu → open the messenger.
static void onboarding_complete(void) {
    product_onboarding_exit();
    s_active = 2;
    demo_feishu_enter();
}

// 按键回调运行在 button 组件的任务里,操作 LVGL 必须加锁。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    ESP_LOGI(TAG, "on_key btn=%d ev=%d active=%d", btn, ev, s_active);
    if (!bsp_lvgl_lock(500)) return;

    // OK 长按离开当前屏幕。组件对一次按住只发一个 LONG,所以「按了多久」只能靠
    // 按住期间的 HOLD 心跳来数:LONG(1 秒)记下起点,任一次 HOLD 距起点满
    // OK_EXIT_HOLD_MS 才真正离开。松手清零,没按满就什么都不发生。
    static int64_t s_ok_long_us;             // OK 进入长按的时刻,0 = 未按住
    bool ok_exit = false;
    if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_LONG) {
            s_ok_long_us = esp_timer_get_time();
        } else if (ev == BSP_BTN_HOLD && s_ok_long_us != 0) {
            ok_exit = esp_timer_get_time() - s_ok_long_us >=
                      (int64_t)OK_EXIT_HOLD_MS * 1000;
            if (ok_exit) s_ok_long_us = 0;   // 只触发一次
        } else {
            s_ok_long_us = 0;                // PRESS/RELEASE/其它:重新开始
        }
    }

    if (s_active == 0) {                        // voice = home (offline BLE mic)
        // Any key wakes the screen first, then does its own job. Waking here rather
        // than inside demo_voice_key means the long-press exit below also wakes,
        // and a press is never consumed just to turn the backlight up.
        demo_voice_wake();
        if (ok_exit) {
            demo_voice_exit();
            s_active = 1;                        // opt-in: go online, bind Feishu
            product_onboarding_enter(onboarding_complete);
        } else {
            demo_voice_key(btn, ev);
        }
    } else if (s_active == 1) {                  // onboarding (networked)
        product_onboarding_key(btn, ev);
    } else {                                     // Feishu messenger
        if (ok_exit) {
            // Let Feishu consume back navigation; at its root, return to voice.
            if (!demo_feishu_back()) {
                demo_feishu_exit();
                s_active = 0;
                demo_voice_enter();
            }
        } else {
            demo_feishu_key(btn, ev);
        }
    }
    bsp_lvgl_unlock();
}

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy AI Passport 启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是唯一 UI 载体,失败就没有产品可言 —— 打清楚日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    bool buttons_ok = (bsp_button_init(on_key, NULL) == ESP_OK);
    bool audio_ok = (bsp_audio_init() == ESP_OK);
    bool battery_ok = (bsp_battery_init() == ESP_OK);

    if (bsp_lvgl_lock(1000)) {
        demo_voice_enter();          // home = offline BLE mic; Feishu is opt-in
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "就绪:Button=%d Audio=%d Battery=%d",
             buttons_ok, audio_ok, battery_ok);
}
