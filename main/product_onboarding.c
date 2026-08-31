#include "product_onboarding.h"

#include "feishu_binding.h"
#include "feishu_network.h"
#include "feishu_provision.h"
#include "feishu_store.h"
#include "setup_portal.h"
#include "ui_pixel.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    ONBOARD_CONNECTING = 0,
    ONBOARD_WIFI_SETUP,
    ONBOARD_APP_SETUP,
    ONBOARD_BINDING,
    ONBOARD_SUCCESS,
    ONBOARD_ERROR,
} onboarding_page_t;

typedef struct {
    onboarding_page_t page;
    setup_portal_info_t portal;
    char qr_data[FEISHU_BIND_URL_MAX];
    char status[128];
    bool closing;
    bool worker_finished;
} onboarding_state_t;

static onboarding_state_t s_state;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_worker_done;
static TaskHandle_t s_worker;
static lv_obj_t *s_screen;
static lv_obj_t *s_title;
static lv_obj_t *s_body;
static lv_obj_t *s_qr;
static lv_timer_t *s_timer;
static product_onboarding_complete_cb_t s_complete;
static bool s_completion_delivered;
static onboarding_page_t s_rendered_page = (onboarding_page_t)-1;
static char s_rendered_qr[FEISHU_BIND_URL_MAX];

static void update_state(onboarding_page_t page, const char *status,
                         const char *qr_data)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.page = page;
    feishu_utf8_copy(s_state.status, sizeof(s_state.status), status);
    feishu_utf8_copy(s_state.qr_data, sizeof(s_state.qr_data),
                     qr_data == NULL ? "" : qr_data);
    xSemaphoreGive(s_lock);
}

static bool closing(void)
{
    bool value;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    value = s_state.closing;
    xSemaphoreGive(s_lock);
    return value;
}

static void worker_task(void *argument)
{
    (void)argument;
    esp_err_t err;
    feishu_credentials_t *credentials = NULL;

    for (;;) {
        update_state(ONBOARD_CONNECTING, "正在连接 Wi-Fi...", NULL);
        err = feishu_network_start(12000);
        if (err == ESP_OK) break;
        if (closing()) goto finished;

        update_state(ONBOARD_CONNECTING, "正在启动配网热点...", NULL);
        err = setup_portal_start(&s_state.portal);
        if (err != ESP_OK) {
            update_state(ONBOARD_ERROR, "无法启动配网，请重启设备", NULL);
            goto finished;
        }
        char wifi_qr[96];
        snprintf(wifi_qr, sizeof(wifi_qr), "WIFI:T:WPA;S:%s;P:%s;;",
                 s_state.portal.ssid, s_state.portal.password);
        update_state(ONBOARD_WIFI_SETUP, "", wifi_qr);
        while (!closing() && !setup_portal_credentials_received()) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (closing()) {
            setup_portal_stop();
            goto finished;
        }
        vTaskDelay(pdMS_TO_TICKS(800));
        setup_portal_stop();
    }

    credentials = calloc(1, sizeof(*credentials));
    if (credentials == NULL) {
        update_state(ONBOARD_ERROR, "内存不足，请重启设备", NULL);
        goto finished;
    }
    if (feishu_store_load_credentials(credentials) == ESP_OK) {
        memset(credentials, 0, sizeof(*credentials));
        free(credentials);
        credentials = NULL;
        update_state(ONBOARD_SUCCESS, "飞书已绑定", NULL);
        goto finished;
    }

configure_owner_app:
    if (!feishu_binding_app_configured()) {
        update_state(ONBOARD_CONNECTING, "正在启动手机配置热点...", NULL);
        feishu_network_stop();
        err = setup_portal_start_feishu(&s_state.portal);
        if (err != ESP_OK) {
            update_state(ONBOARD_ERROR, "无法启动手机配置，请重启设备", NULL);
            goto finished;
        }
        char app_qr[128];
        snprintf(app_qr, sizeof(app_qr), "WIFI:T:WPA;S:%s;P:%s;;",
                 s_state.portal.ssid, s_state.portal.password);
        update_state(ONBOARD_APP_SETUP, "", app_qr);
        bool usb_started = feishu_provision_start() == ESP_OK;
        while (!closing() && !setup_portal_credentials_received() &&
               !(usb_started && feishu_provision_credentials_received())) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        feishu_provision_stop();
        setup_portal_stop();
        memset(s_state.portal.password, 0, sizeof(s_state.portal.password));
        if (closing()) goto finished;
        if (!feishu_binding_app_configured()) {
            update_state(ONBOARD_ERROR, "飞书应用配置无效，请重新写入", NULL);
            goto finished;
        }
        update_state(ONBOARD_CONNECTING, "正在恢复网络连接...", NULL);
        err = feishu_network_start(12000);
        if (err != ESP_OK) {
            update_state(ONBOARD_ERROR, "无法重新连接 Wi-Fi，请重启设备", NULL);
            goto finished;
        }
    }

    while (!closing()) {
        feishu_binding_request_t request = { 0 };
        err = feishu_binding_begin(&request);
        if (err != ESP_OK) {
            if (err == ESP_ERR_INVALID_ARG) {
                // Feishu explicitly rejected client authentication. Remove
                // only Feishu state, keep Wi-Fi, and return to the phone-local
                // owner-app portal. Transient errors keep the existing app.
                esp_err_t clear_err = feishu_store_clear_credentials();
                if (clear_err != ESP_OK) {
                    update_state(ONBOARD_ERROR,
                                 "无法清除错误凭据，请重启设备", NULL);
                    goto finished;
                }
                update_state(ONBOARD_CONNECTING,
                             "App ID 或 Secret 无效，正在重新配置...", NULL);
                vTaskDelay(pdMS_TO_TICKS(1200));
                goto configure_owner_app;
            }
            update_state(ONBOARD_ERROR, "暂时无法发起飞书绑定", NULL);
            goto finished;
        }
        update_state(ONBOARD_BINDING, "等待手机确认...",
                     request.verification_url);
        uint32_t elapsed = 0;
        uint32_t interval = request.interval;
        unsigned consecutive_errors = 0;
        while (!closing() && elapsed < request.expires_in) {
            vTaskDelay(pdMS_TO_TICKS(interval * 1000));
            elapsed += interval;
            if (closing()) goto finished;
            feishu_binding_status_t status;
            err = feishu_binding_poll(&request, &status, credentials);
            if (err != ESP_OK) {
                if (err == ESP_ERR_NOT_SUPPORTED) {
                    update_state(ONBOARD_ERROR,
                                 "飞书应用未允许刷新令牌，请在开发者后台开启后重试",
                                 NULL);
                    goto finished;
                }
                // A temporary DNS/TLS/server failure must not invalidate a
                // QR code that is still active. Only stop after repeated
                // failures; normal authorization_pending is not an error.
                if (++consecutive_errors < 3) continue;
                // The device-code may have been consumed while a transient
                // response/network error occurred. Request a fresh QR instead
                // of trapping the user on an error page.
                update_state(ONBOARD_CONNECTING, "正在重新生成绑定二维码...", NULL);
                memset(credentials, 0, sizeof(*credentials));
                break;
            }
            consecutive_errors = 0;
            if (status == FEISHU_BINDING_COMPLETE) {
                memset(credentials, 0, sizeof(*credentials));
                free(credentials);
                credentials = NULL;
                update_state(ONBOARD_SUCCESS, "绑定成功", NULL);
                goto finished;
            }
            if (status == FEISHU_BINDING_SLOW_DOWN) interval += 2;
            if (status == FEISHU_BINDING_EXPIRED) break;
        }
    }

finished:
    if (credentials != NULL) {
        memset(credentials, 0, sizeof(*credentials));
        free(credentials);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.worker_finished = true;
    xSemaphoreGive(s_lock);
    s_worker = NULL;
    xSemaphoreGive(s_worker_done);
    vTaskDelete(NULL);
}

static void render(lv_timer_t *timer)
{
    (void)timer;
    onboarding_state_t snapshot;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(&snapshot, &s_state, sizeof(snapshot));
    xSemaphoreGive(s_lock);

    if (snapshot.page != s_rendered_page) {
        s_rendered_page = snapshot.page;
        s_rendered_qr[0] = '\0';
        lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        if (snapshot.page == ONBOARD_CONNECTING) {
            lv_label_set_text(s_title, "AI Passport");
            lv_label_set_text(s_body, "\n正在准备设备\n\n连接 Wi-Fi...");
        } else if (snapshot.page == ONBOARD_WIFI_SETUP) {
            lv_label_set_text(s_title, "连接 Wi-Fi");
            lv_label_set_text_fmt(s_body,
                "扫码连接热点\n%s\n\n连接后会自动打开配网页面",
                snapshot.portal.ssid);
        } else if (snapshot.page == ONBOARD_APP_SETUP) {
            lv_label_set_text(s_title, "配置私人飞书");
            lv_label_set_text(s_body,
                "手机扫码连接安全热点\n网页会自动打开\n填写自己的 App ID / Secret\n\nUSB 配置仍可作为备用");
        } else if (snapshot.page == ONBOARD_BINDING) {
            lv_label_set_text(s_title, "绑定飞书");
            lv_label_set_text(s_body,
                "用飞书扫描二维码\n确认授权后设备会自动继续\n二维码过期会自动刷新");
        } else if (snapshot.page == ONBOARD_SUCCESS) {
            lv_label_set_text(s_title, "设置完成");
            lv_label_set_text(s_body, "\nOK 飞书绑定成功\n\n正在打开消息...");
        } else {
            lv_label_set_text(s_title, "暂时无法继续");
            lv_label_set_text_fmt(s_body, "\n%s\n\n按 OK 重启重试", snapshot.status);
        }
    }
    if ((snapshot.page == ONBOARD_WIFI_SETUP || snapshot.page == ONBOARD_APP_SETUP ||
         snapshot.page == ONBOARD_BINDING) && snapshot.qr_data[0] != '\0' &&
        strcmp(snapshot.qr_data, s_rendered_qr) != 0) {
        lv_qrcode_set_data(s_qr, snapshot.qr_data);
        feishu_utf8_copy(s_rendered_qr, sizeof(s_rendered_qr), snapshot.qr_data);
        lv_obj_remove_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
    }
    if (snapshot.page == ONBOARD_SUCCESS && snapshot.worker_finished &&
        !s_completion_delivered && s_complete != NULL) {
        s_completion_delivered = true;
        s_complete();
    }
}

void product_onboarding_enter(product_onboarding_complete_cb_t callback)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.page = ONBOARD_CONNECTING;
    s_complete = callback;
    s_completion_delivered = false;
    s_rendered_page = (onboarding_page_t)-1;
    s_rendered_qr[0] = '\0';
    s_lock = xSemaphoreCreateMutex();
    s_worker_done = xSemaphoreCreateBinary();

    s_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(UI_PAPER), 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    s_title = lv_label_create(s_screen);
    lv_obj_set_width(s_title, 220);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_text_font(s_title, &lv_font_ai_passport_14, 0);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    s_qr = lv_qrcode_create(s_screen);
    lv_qrcode_set_size(s_qr, 164);
    lv_qrcode_set_dark_color(s_qr, lv_color_hex(UI_INK));
    lv_qrcode_set_light_color(s_qr, lv_color_hex(UI_PAPER));
    lv_qrcode_set_quiet_zone(s_qr, true);
    lv_obj_align(s_qr, LV_ALIGN_CENTER, 0, -4);
    lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
    s_body = lv_label_create(s_screen);
    lv_obj_set_width(s_body, 224);
    lv_obj_align(s_body, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_text_font(s_body, &lv_font_ai_passport_14, 0);
    lv_obj_set_style_text_align(s_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_screen_load(s_screen);

    if (s_lock == NULL || s_worker_done == NULL) {
        lv_label_set_text(s_body, "内存不足，请重启设备");
        return;
    }
    s_timer = lv_timer_create(render, 100, NULL);
    if (xTaskCreate(worker_task, "onboarding", 10240, NULL, 4,
                    &s_worker) != pdPASS) {
        update_state(ONBOARD_ERROR, "内存不足，请重启设备", NULL);
    }
}

void product_onboarding_exit(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state.closing = true;
        xSemaphoreGive(s_lock);
    }
    if (s_worker != NULL && s_worker_done != NULL) {
        xSemaphoreTake(s_worker_done, pdMS_TO_TICKS(3000));
    }
    setup_portal_stop();
    feishu_provision_stop();
    if (s_timer != NULL) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_screen != NULL) { lv_obj_delete(s_screen); s_screen = NULL; }
    if (s_worker_done != NULL) {
        vSemaphoreDelete(s_worker_done);
        s_worker_done = NULL;
    }
    if (s_lock != NULL) { vSemaphoreDelete(s_lock); s_lock = NULL; }
    s_complete = NULL;
}

void product_onboarding_key(bsp_btn_t button, bsp_btn_ev_t event)
{
    if (button == BSP_BTN_OK && event == BSP_BTN_CLICK && s_lock != NULL) {
        onboarding_page_t page;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        page = s_state.page;
        xSemaphoreGive(s_lock);
        if (page == ONBOARD_ERROR) esp_restart();
    }
}
