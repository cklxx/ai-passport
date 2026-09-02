// Path B voice input — the device is a BLE wireless microphone for the PC.
//
// The device has no Wi-Fi in its use environment, so audio streams over a BLE
// GATT service (voice_ble.c) direct to a paired PC running the recv-ble agent.
// 16 kHz mu-law is sent frame by frame — stateless per sample, so a dropped BLE
// notify costs only that frame instead of corrupting the stream (ADPCM would
// drift), while one byte per sample halves the frames per second the link must
// carry. See VOICE_CHUNK_SAMPLES for why the frame RATE is the binding limit.
//
//   OK   = start / stop the mic
//   DOWN = 发送  (control -> PC injects Enter)
//   UP   = 删除  (control -> PC injects Backspace)
//
// Audio never touches the LVGL/button task: a worker owns capture + encode +
// notify. Capture streams in small blocks; no whole-recording buffer.
#include "demo.h"

#include "bsp_audio.h"
#include "bsp_display.h"     // 低功耗模式调背光,见 VOICE_DIM_AFTER_MS
#include "bsp_battery.h"
#include "island_quota.h"
#include "mascot.h"
#include "ui_pixel.h"
#include "voice_ble.h"
#include "voice_proto.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "demo_voice";

#define VOICE_SAMPLE_RATE 16000
// BLE lets roughly one notify through per connection event, so the NUMBER of
// frames per second — not their size, and not bandwidth — is what the link
// bounds, and a shortfall is a hole in the PC's mic stream that a streaming ASR
// reads as a sentence end.
//
// 480 samples of mu-law is 480 bytes, the same 482-byte packet that 240 samples
// of 16-bit PCM produced, at half the frame rate: 33.3/s instead of 66.7/s. The
// PCM form measured 85-92% delivered when macOS granted a 15 ms connection
// interval and fell to 47-56% — almost exactly half — after a reconnect
// renegotiated it to 30 ms, with the device's own pool_dry counter going from
// ~19 to ~235 per session as unsent notifications filled the mbuf pool. The
// device cannot make macOS grant more events, so it sends fewer, larger-payload
// frames instead. See voice_ulaw_encode for what the compression costs.
//
// Do not raise this to fill the MTU further: 252 samples (506 of 507 available
// bytes) was tried and measured WORSE, 89% -> 69%, because a fuller packet costs
// more of the controller's ACL buffers. Headroom is worth more than the slots.
#define VOICE_CHUNK_SAMPLES 480
#define VOICE_CHUNK_MS (VOICE_CHUNK_SAMPLES * 1000 / VOICE_SAMPLE_RATE)   // 30 ms

// ST_CONNECTING here means "advertising / waiting for the PC to subscribe".
typedef enum { ST_CONNECTING, ST_IDLE, ST_RECORDING, ST_ERROR } voice_state_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_title;
static lv_obj_t *s_big;
static lv_obj_t *s_sub;
static lv_obj_t *s_battery;
static lv_obj_t *s_island;
static lv_obj_t *s_mascot;
// Which frame is on screen. LVGL redraws only invalidated areas and skips the
// refresh entirely when nothing is invalid, so an unchanged image costs 0 us per
// tick — but swapping the source invalidates all 96x96 px, which at a 240x20 draw
// buffer is 5 SPI flushes (~0.96 ms). Only swap when the frame actually changes:
// this screen shares its single core with the BLE audio worker, and redraw time
// comes straight out of the microphone's frame budget.
static const lv_image_dsc_t *s_shown_frame;
static int s_band;               // level band currently shown, for hysteresis
static lv_timer_t *s_timer;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_worker_done;
static TaskHandle_t s_worker;

static volatile voice_state_t s_state = ST_CONNECTING;
static volatile bool s_want_record;      // UI -> worker: capture on/off
static volatile bool s_closing;
static volatile int s_pending_ctrl;      // UI -> worker: one-shot ctrl code
static volatile unsigned s_elapsed_ms;
// Worker -> render(): capture loudness 0..100. Plain volatile int: a torn read
// costs one frame of the meter, and it must NOT share s_lock with the worker's
// hot path (lock contention there measurably starved the audio stream).
static volatile int s_level;
static unsigned s_lvl_tick;              // worker-only: frames since the last RMS
#define VOICE_SILENCE_LEVEL 10           // below this the smoothed level reads quiet
// The RMS runs every other frame, so the counters below tick in 60 ms units
// regardless of the frame length. Pinning the UNIT rather than the frame count is
// what keeps the silence timeout at 3 s when the frame size changes.
#define VOICE_LVL_EVERY 2                            // 2 * 30 ms = 60 ms
#define VOICE_SILENCE_TICKS 50                       // 50 * 60 ms = 3.0 s ends a take
// One in eight samples, i.e. 60 per frame — the count the meter was tuned against.
// This runs between the capture read and the notify, so it is charged straight to
// the frame budget: sampling every sample once measurably cut delivery 99% -> 95%.
#define VOICE_LVL_STRIDE 8
static bool s_backlog;                   // worker-only: one frame awaiting a retry
static unsigned s_quiet_ticks;           // worker-only: consecutive quiet RMS ticks
static int32_t s_hp_x, s_hp_y;           // worker-only: high-pass filter state
static uint64_t s_read_us, s_send_us, s_retry_us;
// Press-to-first-frame, in microseconds. Both stamps come from esp_timer on this
// device, so this is a single-clock measurement — the PC's clock is never involved
// and there is nothing to reconcile.
static uint64_t s_first_frame_us;
static int64_t s_record_start_us;
static unsigned s_tx_frames;   // audio frames encoded (diagnostic)
static unsigned s_tx_ok;       // frames the BLE stack accepted (diagnostic)
static int s_battery_percent = -1;
static island_quota_t s_quota;
static bool s_have_quota;

// Integer square root, bit-by-bit restoring. Keeps float sqrt out of the audio
// worker for a value that only drives a 0..100 display.
static uint32_t isqrt64(uint64_t n)
{
    uint64_t rem = 0, root = 0;
    for (int i = 0; i < 32; i++) {
        root <<= 1;
        rem = (rem << 2) | (n >> 62);
        n <<= 2;
        if (root < rem) { rem -= root | 1; root |= 2; }
    }
    return (uint32_t)(root >> 1);
}

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
    // Encoded frame, and the one frame held for a single retry. Both are mu-law
    // bytes, not samples: keeping the retry copy in encoded form means the retry
    // sends the identical bytes and re-encodes nothing.
    static uint8_t enc[VOICE_CHUNK_SAMPLES];
    static uint8_t backlog[VOICE_CHUNK_SAMPLES];
    uint16_t backlog_seq = 0;                      // valid only while s_backlog
    s_backlog = false;
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
        // Hold a SEND back while capture is still running: OK during recording means
        // stop-then-send, and Enter must not reach 豆包 before the STOP notify does.
        bool send_deferred = (ctrl == VOICE_CTRL_SEND && capturing);
        if (send_deferred) ctrl = 0;
        if (ctrl != 0 && ready && voice_ctrl_valid(ctrl)) {
            // Clear only on success: a control code that could not be sent is
            // retried next iteration rather than dropped. The stop key felt dead
            // because this cleared unconditionally and the send often failed with
            // the pool exhausted by audio.
            if (voice_ble_send_ctrl((uint8_t)ctrl)) s_pending_ctrl = 0;
        } else if (ctrl != 0 && !voice_ctrl_valid(ctrl)) {
            s_pending_ctrl = 0;             // malformed; drop it
        }

        if (s_want_record && ready && !capturing) {
            capturing = true; s_elapsed_ms = 0; s_level = 0; s_lvl_tick = 0;
            s_quiet_ticks = 0;
            bsp_audio_reset_rx_overflows();   // per-session count, see the STOP log
            voice_ble_reset_audio_seq();      // so the PC reports per-session loss
            s_hp_x = s_hp_y = 0;              // no filter ring-in from the last take
            voice_ble_reset_audio_stats();
            s_read_us = s_send_us = s_retry_us = 0;
            s_first_frame_us = 0;
            s_record_start_us = esp_timer_get_time();
            ESP_LOGI(TAG, "recording START");
            voice_ble_send_ctrl(VOICE_CTRL_START);
            set_state(ST_RECORDING);
        } else if (!s_want_record && capturing) {
            // Stop only when the USER stops. Earlier this also stopped on
            // !ready, but voice_ble_ready() just reports the connection handle
            // and can read false for a moment while the controller is busy — a
            // single blip then knocked the state back to idle, so the recording
            // screen (timer, level, "录音中") was never visible even though audio
            // kept streaming. A genuinely dropped link is handled by the
            // reconnect path below.
            capturing = false; s_level = 0;
            unsigned ovf = (unsigned)bsp_audio_rx_overflows();
            voice_ble_log_audio_stats();
            // The overflow count is in I2S DMA descriptors, and one descriptor is
            // dma_frame_num samples (bsp_audio.c) — NOT one BLE frame. The two
            // coincided while the BLE frame was also 240 samples, so this divided
            // by the right thing by accident; at 480 it would report double. This
            // is the one metric that separates a device-side drop from a link-side
            // one, which is exactly the ambiguity that misdirected debugging
            // before, so it divides by the descriptor length explicitly.
            #define I2S_DESC_MS (240 * 1000 / VOICE_SAMPLE_RATE)   // bsp dma_frame_num
            ESP_LOGI(TAG, "recording STOP %u.%us i2s_ovf=%u (%u%% of frames)",
                     s_elapsed_ms / 1000, s_elapsed_ms % 1000 / 100, ovf,
                     s_elapsed_ms ? ovf * 100 / (s_elapsed_ms / I2S_DESC_MS) : 0);
            if (ready) voice_ble_send_ctrl(VOICE_CTRL_STOP);
            if (ready) {
                // Where the loop's time went, little-endian, milliseconds. The PC
                // prints it with its own figures, which is the only way to tell a
                // device-side shortfall from a delivery one — and needing USB serial
                // to read it meant it was unavailable exactly when the device was in
                // real use.
                unsigned att = 0, acc = 0, af = 0, pf = 0, nf = 0;
                int lrc = 0;
                unsigned ovsz = 0, mtu = 0, msys = 0;
                voice_ble_audio_stats(&att, &acc, &af, &pf, &nf, &lrc,
                                      &ovsz, &mtu, &msys);
                uint16_t st16[14] = {
                    (uint16_t)(ovf > 0xFFFF ? 0xFFFF : ovf),
                    (uint16_t)(s_first_frame_us / 1000),
                    (uint16_t)(s_read_us / 1000),
                    (uint16_t)(s_send_us / 1000),
                    (uint16_t)(s_retry_us / 1000),
                    (uint16_t)(att > 0xFFFF ? 0xFFFF : att),
                    (uint16_t)(acc > 0xFFFF ? 0xFFFF : acc),
                    (uint16_t)(af > 0xFFFF ? 0xFFFF : af),
                    (uint16_t)(nf > 0xFFFF ? 0xFFFF : nf),
                    (uint16_t)(lrc < 0 ? (unsigned)(-lrc) | 0x8000 : lrc),
                    (uint16_t)(ovsz > 0xFFFF ? 0xFFFF : ovsz),
                    (uint16_t)mtu,
                    // Appended, not inserted: an older agent unpacks the first 12
                    // and ignores the tail. append_fail is the SAME dry pool as
                    // alloc_fail caught one step later, and it was the larger half
                    // of the loss while going entirely unreported.
                    (uint16_t)(pf > 0xFFFF ? 0xFFFF : pf),
                    (uint16_t)(msys > 0xFFFF ? 0xFFFF : msys),
                };
                uint8_t buf[1 + sizeof(st16)];
                buf[0] = VOICE_CTRL_STATS;
                memcpy(buf + 1, st16, sizeof(st16));
                if (!voice_ble_send_ctrl_buf(buf, sizeof(buf))) {
                    ESP_LOGW(TAG, "stats frame not sent");
                }
            }
            set_state(ready ? ST_IDLE : ST_CONNECTING);
        } else if (!capturing) {
            set_state(ready ? ST_IDLE : ST_CONNECTING);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // Preserve one rejected frame, but retry it before capturing another one.
        // This deliberately applies BLE backpressure to capture rather than using a queue.
        // One retry, then let it go. Two extremes were both worse than this middle:
        // holding a frame and blocking until the pool freed a buffer starved I2S and
        // cost runs of frames to ISR overwrite (retry 8.7 s of a 9.4 s take), while
        // dropping every refused frame outright pushed measured loss from ~0 to
        // 12-25% and the user reported recognition getting worse. So keep one frame
        // for one attempt on the next pass — that covers a momentarily dry pool,
        // which is the common case — and discard it if the second attempt fails
        // rather than waiting. Leaving headroom beats filling every slot.
        if (s_backlog) {
            (void)voice_ble_send_audio(backlog, sizeof(backlog), backlog_seq);
            s_backlog = false;          // sent or not, this frame's turn is over
        }
        int64_t t0 = esp_timer_get_time();
        if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
            s_read_us += esp_timer_get_time() - t0;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        s_read_us += esp_timer_get_time() - t0;
        s_elapsed_ms += VOICE_CHUNK_MS;
        // Send mu-law, not raw PCM: still stateless per sample, so a dropped BLE
        // notify costs only that frame instead of corrupting the stream (ADPCM
        // would drift), but at one byte per sample instead of two — which is what
        // halves the frame rate the link has to carry.
        // High-pass the frame in place, one pole at about 90 Hz. The measured
        // spectrum had 11.5% of its energy below 80 Hz and only 36% in the
        // 300-3400 Hz speech band: handling noise and body rumble that carry no
        // speech, consume the headroom that then clips, and skew the ASR's features.
        // Moving the corner to 150 Hz was tried and reverted: sub-300 Hz energy
        // measured HIGHER afterwards (61.9% -> 72.3%), which contradicts the theory,
        // so the change was not justified whatever the explanation. Measure with
        // tools/analyse_take.py before touching this again — and take several
        // recordings, since the level varies more between takes than between builds.
        // y[n] = a*(y[n-1] + x[n] - x[n-1]), a = 1 - 2*pi*fc/fs, fixed point.
        // Q15 coefficient 0.9647 for fc = 90 Hz at 16 kHz.
        for (int i = 0; i < VOICE_CHUNK_SAMPLES; i++) {
            int32_t x = pcm[i];
            int32_t y = (int32_t)(((int64_t)31610 * (s_hp_y + x - s_hp_x)) >> 15);
            s_hp_x = x;
            s_hp_y = y;
            pcm[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
        }

        // Loudness for the on-screen meter: RMS of the frame, scaled so ordinary
        // speech lands mid-range, then smoothed with a fast attack and slow
        // release so a syllable shows immediately but the bar settles instead of
        // flickering.
        // Every 4th frame only, and over a 1-in-4 sample stride: this runs in the
        // audio worker between the capture read and the BLE notify, so the work is
        // charged directly against the frame budget. Doing it per frame over every
        // sample measurably cut the delivered rate (99% -> 95%). 60 ms updates and
        // 60 samples are ample for a 12-cell bar behind a 100 ms render tick.
        if (++s_lvl_tick >= VOICE_LVL_EVERY) {
            s_lvl_tick = 0;
            uint32_t acc = 0;
            for (int i = 0; i < VOICE_CHUNK_SAMPLES; i += VOICE_LVL_STRIDE) {
                int32_t v = pcm[i] >> 4;      // keep the accumulator in 32 bits
                acc += (uint32_t)(v * v);
            }
            int lvl = (int)(isqrt64(acc / (VOICE_CHUNK_SAMPLES / VOICE_LVL_STRIDE)) * 16 / 40);
            if (lvl > 100) lvl = 100;
            int prev = s_level;
            s_level = lvl > prev ? lvl : prev - (prev - lvl) / 2;

            // Auto-stop on sustained silence, so a take ends by itself when the
            // user stops talking. The level is computed every VOICE_LVL_EVERY
            // frames, which is fixed at 60 ms, so the counter ticks in 60 ms units
            // whatever the frame length. 3 s of quiet: past a pause for thought,
            // short enough not to feel like a hang. Earlier values of 2.4 s clipped
            // people mid-sentence. DOWN stops immediately, and any speech resets
            // the count.
            if (lvl < VOICE_SILENCE_LEVEL) {
                if (++s_quiet_ticks >= VOICE_SILENCE_TICKS) s_want_record = false;
            } else {
                s_quiet_ticks = 0;
            }
        }

        uint16_t seq = voice_ble_next_audio_seq();
        // Encode after the high-pass and the RMS, so both still see linear samples:
        // the filter needs true arithmetic and the meter's thresholds were tuned on
        // PCM. mu-law is per-sample, so this is a flat 480-iteration pass with no
        // state carried between frames.
        for (int i = 0; i < VOICE_CHUNK_SAMPLES; i++) enc[i] = voice_ulaw_encode(pcm[i]);
        t0 = esp_timer_get_time();
        bool ok = voice_ble_send_audio(enc, sizeof(enc), seq);
        if (ok && s_first_frame_us == 0) {
            s_first_frame_us = esp_timer_get_time() - s_record_start_us;
        }
        s_send_us += esp_timer_get_time() - t0;
        if (!ok) {
            memcpy(backlog, enc, sizeof(enc));
            backlog_seq = seq;
            s_backlog = true;           // one retry at the top of the next pass
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_tx_frames++;
        if (ok) s_tx_ok++;
        xSemaphoreGive(s_lock);
    }

done:
    voice_ble_set_quota_cb(NULL);
    voice_ble_stop();
    s_worker = NULL;
    xSemaphoreGive(s_worker_done);
    vTaskDelete(NULL);
}

// Low-power mode, entered when the PC link has been down for a while. A 520 mAh
// battery behind a 240x320 backlight at 100% does not last, and while
// disconnected — in a pocket, or on the desk with the agent not running — nothing
// on the screen is worth that current.
//
// Only the backlight and the render cadence change. The BLE stack keeps
// advertising and the audio worker is untouched, so a reconnect and a key press
// behave exactly as before; this must not become a state the device can get stuck
// in.
#define VOICE_DIM_AFTER_MS 20000     // link down this long -> screen off
#define VOICE_DIM_PERCENT  0         // fully off. The panel itself stays on, so a
                                     // key press brings the image straight back with
                                     // nothing to redraw or re-init.
#define VOICE_BRIGHT       50        // normal brightness. 100% was never needed —
                                     // this is a 240x320 panel read at arm's length
                                     // indoors, and the backlight is the largest
                                     // draw on a 520 mAh battery.
#define VOICE_TICK_MS      100       // render period, awake
#define VOICE_TICK_DIM_MS  1000      // render period with the backlight off. Nothing
                                     // is visible, so this only has to be often
                                     // enough to notice the link coming back — the
                                     // agent's own reconnect takes longer than this.
static bool s_dimmed;
static unsigned s_idle_ms;           // render-only: how long nothing has happened

// Which states let the screen go dark. Everything except recording: waiting for
// the agent, connected-and-idle on a desk, and a failed init all show a picture
// that does not change.
static inline bool state_dims(voice_state_t st) { return st != ST_RECORDING; }

// Any key press wakes the screen, whatever else that key does. Called from on_key
// before the key is dispatched, so waking never costs the key its own action.
void demo_voice_wake(void)
{
    s_idle_ms = 0;
    if (!s_dimmed) return;
    s_dimmed = false;
    bsp_display_backlight(VOICE_BRIGHT);
    if (s_timer != NULL) lv_timer_set_period(s_timer, VOICE_TICK_MS);
}

static void render(lv_timer_t *t)
{
    (void)t;
    // With the backlight off, the only thing this tick is for is noticing the link
    // come back. Everything below draws or measures for a screen nobody can see, so
    // take the state and leave: no battery ADC conversion, no snprintf, no label
    // updates, and above all no LVGL invalidation — a redraw costs SPI flushes out
    // of the audio worker's budget on this single core.
    if (s_dimmed) {
        if (!state_dims(s_state)) demo_voice_wake();
        return;
    }
    // Outside s_lock on purpose. bsp_battery_soc() is a blocking I2C transaction
    // (~0.5 ms on a 100 kHz bus), and the audio worker takes this same mutex once
    // per 30 ms frame — see the note above s_level. s_battery_percent is written
    // only here and in enter(), never by the worker, so it never needed the lock.
    int bp = bsp_battery_soc();
    if (bp >= 0) s_battery_percent = bp;

    voice_state_t st;
    unsigned ms;
    unsigned tx, ok;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st = s_state;
    ms = s_elapsed_ms;
    tx = s_tx_frames;
    ok = s_tx_ok;
    bool have_q = s_have_quota;
    island_quota_t q = s_quota;
    xSemaphoreGive(s_lock);
    int lvl = s_level;          // plain volatile: deliberately not under s_lock,
                                // which the audio worker takes on its hot path

    // Dim on a sustained idle; come straight back the moment anything happens.
    // ST_RECORDING never dims — the timer and level meter are the whole point of
    // that screen. ST_IDLE does: connected to the agent and sitting on a desk is
    // the normal all-day state, and nothing on that screen moves. So does
    // ST_ERROR, which otherwise sits lit until the battery is flat.
    //
    // The dwell is counted in render ticks rather than from a timestamp so it does
    // not need a clock. Every tick reaching here is an awake tick — a dimmed one
    // returned at the top of render() — so VOICE_TICK_MS is the only period that
    // can be added, and the sum stops growing for good once the backlight goes
    // off. A blip that clears within the dwell costs nothing, which is why the
    // link symbol reading false for a moment (see the note in the worker) cannot
    // flicker the backlight.
    if (state_dims(st)) {
        s_idle_ms += VOICE_TICK_MS;
        if (s_idle_ms >= VOICE_DIM_AFTER_MS) {
            s_dimmed = true;
            bsp_display_backlight(VOICE_DIM_PERCENT);
            if (s_timer != NULL) lv_timer_set_period(s_timer, VOICE_TICK_DIM_MS);
        }
    } else {
        demo_voice_wake();      // recording: full brightness, dwell reset
    }

    char battery[8];
    snprintf(battery, sizeof(battery), s_battery_percent >= 0 ? "%d%%" : "--%%",
             s_battery_percent > 100 ? 100 : s_battery_percent);
    lv_label_set_text(s_battery, battery);

    // The link symbol is the header's whole job: green when the PC is connected,
    // dim when it is not. Colour, not a word, because it must be readable at a
    // glance and there is only one font size.
    lv_obj_set_style_text_color(s_title, lv_color_hex(
        st == ST_CONNECTING ? UI_MUTED : st == ST_ERROR ? UI_RED : UI_GRASS), 0);
    lv_obj_set_style_text_opa(s_title, st == ST_CONNECTING ? LV_OPA_40 : LV_OPA_COVER, 0);

    // Claude quota island: PC pushes used_percentage; the device has no synced
    // wall clock, so it shows remaining % only (no fabricated countdown).
    // Both quotas on the island, each showing remaining %. The device has no
    // synced wall clock, so no countdown is fabricated.
    // Claude's number is often absent — Claude Code only publishes rate_limits to
    // Pro/Max subscribers — so an unavailable figure shows as a dash. 未知 read as
    // a device fault for something the device never had.
    char cl[8], cx[8];
    if (!have_q || q.remaining_pct < 0) snprintf(cl, sizeof(cl), "–");
    else snprintf(cl, sizeof(cl), "%d%%", q.remaining_pct);
    if (!have_q || q.codex_remaining_pct < 0) snprintf(cx, sizeof(cx), "–");
    else snprintf(cx, sizeof(cx), "%d%%", q.codex_remaining_pct);
    lv_label_set_text_fmt(s_island, "Claude %s     Codex %s", cl, cx);

    // Mascot frame from state, and while recording from the capture level.
    // Swapping the source invalidates all 160x160 px, which at a 240x20 draw
    // buffer is 8 SPI flushes. That draw time comes straight out of the audio
    // worker's frame budget on this single core, so the level bands get hysteresis:
    // a voice hovering on a threshold otherwise re-swapped the image every render
    // tick and cost ~6 points of delivered rate.
    // Hysteresis on the level bands: a voice sitting on a threshold would otherwise
    // re-swap the image every render tick. Throttling the swap rate on top of this
    // was tried and reverted — it cost responsiveness and recovered no audio, which
    // is how we know frame swaps are not the bottleneck.
    int band = lvl > 70 ? 2 : lvl > 38 ? 1 : 0;
    if (band == s_band + 1 && lvl < (s_band == 0 ? 46 : 78)) band = s_band;
    if (band == s_band - 1 && lvl > (s_band == 2 ? 62 : 30)) band = s_band;
    s_band = band;
    const lv_image_dsc_t *want =
        st == ST_CONNECTING ? &mascot_waiting :
        st == ST_ERROR      ? &mascot_fault   :
        st == ST_IDLE       ? &mascot_idle    :
        band == 2           ? &mascot_peak    :
        band == 1           ? &mascot_loud    : &mascot_quiet;
    if (want != s_shown_frame) {
        lv_image_set_src(s_mascot, want);
        s_shown_frame = want;
    }

    switch (st) {
    case ST_CONNECTING:
        lv_label_set_text(s_big, "连接中");
        lv_label_set_text(s_sub, "等待电脑蓝牙连接");
        break;
    case ST_IDLE:
        // The lit spark already says "ready"; a word under it would only repeat
        // the picture.
        lv_label_set_text(s_big, "");
        lv_label_set_text(s_sub, "");
        break;
    case ST_RECORDING: {
        char t2[40];
        // Level bar as ASCII inside the existing label. Not new widgets: 12 extra
        // lv_obj restyled per tick once tripped the task watchdog, which also
        // froze every key (on_key waits on the LVGL lock). Not block glyphs
        // either — 12 solid U+2588 cells redrawn at 10 Hz cost so much draw time
        // that delivered audio fell to 23%. ASCII glyphs are mostly empty pixels.
        int bars = s_level * 12 / 100;
        char meter[13];
        for (int i = 0; i < 12; i++) meter[i] = i < bars ? '=' : '.';
        meter[12] = 0;
        // Show elapsed time; append a warning if the BLE link is dropping frames
        // (congested). A healthy link shows just the timer.
        bool healthy = (tx == 0 || ok * 100 >= tx * 95);
        snprintf(t2, sizeof(t2), healthy ? "%u.%us" : "%u.%us  信号弱",
                 ms / 1000, ms % 1000 / 100);
        // Stars already say "listening", so this row carries only the timer.
        lv_label_set_text(s_big, "");
        // Timer and level on one line, so the meter needs no widget of its own and
        // the quota label keeps its place.
        char line[64];   // t2 up to 40 + two spaces + 12 ASCII cells + NUL
        snprintf(line, sizeof(line), "%s   %s", t2, meter);
        lv_label_set_text(s_sub, line);
        break;
    }
    case ST_ERROR:
        lv_label_set_text(s_big, "故障");
        lv_label_set_text(s_sub, "蓝牙或音频不可用");   // a fault needs its reason
        break;
    }
}

void demo_voice_enter(void)
{
    s_state = ST_CONNECTING;
    // Enter awake: this screen is reached at boot and on the way back from Feishu,
    // and inheriting a dimmed backlight from a previous visit would look like a
    // fault. s_timer is created below at VOICE_TICK_MS to match.
    s_dimmed = false;
    s_idle_ms = 0;
    bsp_display_backlight(VOICE_BRIGHT);
    s_want_record = false;
    s_closing = false;
    s_pending_ctrl = 0;
    s_battery_percent = -1;
    s_have_quota = false;
    s_lock = xSemaphoreCreateMutex();
    s_worker_done = xSemaphoreCreateBinary();

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bg = lv_image_create(s_scr);
    lv_image_set_src(bg, &backdrop);
    lv_obj_align(bg, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(s_scr, 0, 0);

    s_title = lv_label_create(s_scr);
    lv_label_set_text(s_title, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_MUTED), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 8, 13);

    s_battery = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_battery, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_battery, lv_color_hex(UI_MUTED), 0);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -8, 13);

    // Claude quota island: a pill strip below the header.
    s_island = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_island, &lv_font_ai_passport_14, 0);
    // Quiet footer type, not a filled pill: quota is reference data, and a
    // saturated block made the least important element the loudest one.
    lv_obj_set_style_text_color(s_island, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_opa(s_island, LV_OPA_50, 0);
    lv_obj_align(s_island, LV_ALIGN_TOP_MID, 0, 268);
    lv_label_set_text(s_island, "Claude --");

    s_mascot = lv_image_create(s_scr);
    lv_image_set_src(s_mascot, &mascot_idle);
    lv_obj_align(s_mascot, LV_ALIGN_TOP_MID, 0, 50);
    s_shown_frame = &mascot_idle;
    s_band = 0;

    s_big = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_big, &lv_font_ai_passport_14, 0);
    lv_obj_set_style_text_color(s_big, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_text_align(s_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(s_big, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(s_big, 0, 0);
    lv_obj_align(s_big, LV_ALIGN_TOP_MID, 0, 212);

    s_sub = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_sub, &lv_font_ai_passport_14, 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(UI_PAPER), 0);
    lv_obj_set_style_text_align(s_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_sub, LV_ALIGN_TOP_MID, 0, 234);

    lv_screen_load(s_scr);

    if (s_lock == NULL || s_worker_done == NULL) {
        lv_label_set_text(s_sub, "内存不足，请重启设备");
        return;
    }
    s_timer = lv_timer_create(render, VOICE_TICK_MS, NULL);
    if (xTaskCreate(worker_task, "voice", 6144, NULL, 6, &s_worker) != pdPASS) {
        s_state = ST_ERROR;
    }
}

void demo_voice_exit(void)
{
    s_closing = true;
    s_want_record = false;
    // Hand the next screen a bright backlight. Leaving it dimmed would make
    // onboarding look broken, and that screen has no dim logic of its own.
    s_dimmed = false;
    bsp_display_backlight(VOICE_BRIGHT);
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
    voice_state_t st = s_state;
    if (st == ST_CONNECTING || st == ST_ERROR) return;

    // Layout follows the physical keys: DOWN (middle) toggles recording, OK
    // (bottom) sends, UP deletes.
    //
    // Toggle and delete fire on PRESS, not CLICK. BSP_BTN_CLICK maps to the
    // button component's BUTTON_SINGLE_CLICK, which is not emitted until the
    // finger LIFTS and the ~180 ms double-click discrimination window expires, so
    // recording started well after the press and the key felt unresponsive.
    // BSP_BTN_PRESS is BUTTON_PRESS_DOWN: the instant contact is made. Both
    // actions are recoverable (press again / say it again), so waiting to learn
    // whether a second click follows buys nothing.
    if (btn == BSP_BTN_DOWN) {
        if (ev == BSP_BTN_PRESS) s_want_record = !s_want_record;
        return;
    }
    if (btn == BSP_BTN_UP) {
        // Short press deletes one utterance; holding erases continuously until the
        // finger lifts. The long-press arrives as a separate event after PRESS has
        // already queued a single delete, so the erase supersedes it — one extra
        // backspace before a hold is harmless, and this keeps the short press
        // instant instead of waiting to rule out a long press.
        //
        // s_erasing is what keeps a short press from sending a stray ERASE_END:
        // RELEASE now fires for every press, not just held ones, and an unpaired
        // END would overwrite the DELETE still waiting in the one-deep ctrl slot.
        static bool s_erasing;
        if (ev == BSP_BTN_PRESS)   s_pending_ctrl = VOICE_CTRL_DELETE;
        if (ev == BSP_BTN_LONG)  { s_pending_ctrl = VOICE_CTRL_ERASE_BEGIN; s_erasing = true; }
        if (ev == BSP_BTN_RELEASE && s_erasing) {
            s_pending_ctrl = VOICE_CTRL_ERASE_END;
            s_erasing = false;
        }
        return;
    }
    // OK fires on PRESS too. It used to wait for CLICK because its long-press
    // leaves this screen for onboarding (main.c) and PRESS_DOWN also opens a long
    // press — but that cost the whole hold plus the ~180 ms double-click window on
    // every send, which is the single longest delay in the input path and the one
    // that reads as "the button is slow".
    //
    // The collision resolves the way UP's already does: LONG_PRESS_START does not
    // arrive until 1.5 s in, so a long press sends first and then leaves. Landing an
    // Enter a second and a half before abandoning the screen for onboarding is
    // harmless — it submits text the user was done with anyway — and every key on
    // this screen now behaves the same way, acting at contact and letting the
    // long-press supersede.
    if (ev == BSP_BTN_PRESS) {
        // While recording, OK means "I am done — send it": stop capture and send in
        // one press, rather than making the user stop with DOWN and then send.
        // The worker sees s_want_record go false, emits STOP, and then finds the
        // pending SEND, so the PC receives them in that order and 豆包 has finished
        // the utterance before Enter arrives.
        if (st == ST_RECORDING) s_want_record = false;
        s_pending_ctrl = VOICE_CTRL_SEND;
    }
}
