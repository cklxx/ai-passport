#include "feishu_network.h"

#include "demo_radio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <string.h>

static const char *TAG = "feishu_net";

#define NETWORK_CONNECTED_BIT BIT0

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static bool s_wifi_handler_registered;
static bool s_ip_handler_registered;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_stopping;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_events != NULL) xEventGroupClearBits(s_events, NETWORK_CONNECTED_BIT);
        if (!s_stopping) esp_wifi_connect();
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id == IP_EVENT_STA_GOT_IP && s_events != NULL) {
        xEventGroupSetBits(s_events, NETWORK_CONNECTED_BIT);
    }
}

esp_err_t feishu_network_start(unsigned timeout_ms)
{
    wifi_config_t saved = { 0 };
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    EventBits_t bits;
    esp_err_t err;

    if (s_wifi_started) {
        return feishu_network_connected() ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    s_events = xEventGroupCreate();
    if (s_events == NULL) return ESP_ERR_NO_MEM;
    s_stopping = false;
    err = demo_radio_nvs_prepare();
    if (err == ESP_OK) err = demo_radio_network_prepare();
    if (err != ESP_OK) goto fail;
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    err = esp_wifi_init(&config);
    if (err != ESP_OK) goto fail;
    s_wifi_initialized = true;
    err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err == ESP_OK) err = esp_wifi_get_config(WIFI_IF_STA, &saved);
    if (err != ESP_OK) goto fail;
    if (saved.sta.ssid[0] == '\0') {
        ESP_LOGW(TAG, "no saved Wi-Fi credentials; run BLUFI Setup first");
        err = ESP_ERR_INVALID_STATE;
        goto fail;
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event, NULL,
                                              &s_wifi_handler);
    if (err != ESP_OK) goto fail;
    s_wifi_handler_registered = true;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              ip_event, NULL, &s_ip_handler);
    if (err != ESP_OK) goto fail;
    s_ip_handler_registered = true;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    s_wifi_started = true;
    bits = xEventGroupWaitBits(s_events, NETWORK_CONNECTED_BIT, pdFALSE,
                               pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if ((bits & NETWORK_CONNECTED_BIT) == 0) {
        err = ESP_ERR_TIMEOUT;
        goto fail;
    }
    // OAuth refresh and ASR upload multi-kilobyte request bodies. Modem sleep
    // can stall a long TLS write on this small device, so favor reliability.
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) goto fail;
    memset(&saved, 0, sizeof(saved));
    return ESP_OK;

fail:
    memset(&saved, 0, sizeof(saved));
    feishu_network_stop();
    return err;
}

void feishu_network_stop(void)
{
    s_stopping = true;
    if (s_wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_ip_handler_registered) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              s_ip_handler);
        s_ip_handler_registered = false;
    }
    if (s_wifi_handler_registered) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              s_wifi_handler);
        s_wifi_handler_registered = false;
    }
    if (s_wifi_initialized) {
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_netif != NULL) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
    if (s_events != NULL) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
}

bool feishu_network_connected(void)
{
    return s_events != NULL &&
           (xEventGroupGetBits(s_events) & NETWORK_CONNECTED_BIT) != 0;
}

esp_err_t feishu_network_forget_wifi(void)
{
    wifi_config_t empty = { 0 };
    if (!s_wifi_initialized) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &empty);
    return err;
}
