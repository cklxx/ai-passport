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

static const char *TAG = "main";

// 0 = voice input (home); 1 = onboarding (Feishu bind); 2 = Feishu messenger.
static int s_active = 0;

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

    if (s_active == 0) {                        // voice = home (offline BLE mic)
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            demo_voice_exit();
            s_active = 1;                        // opt-in: go online, bind Feishu
            product_onboarding_enter(onboarding_complete);
        } else {
            demo_voice_key(btn, ev);
        }
    } else if (s_active == 1) {                  // onboarding (networked)
        product_onboarding_key(btn, ev);
    } else {                                     // Feishu messenger
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
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
