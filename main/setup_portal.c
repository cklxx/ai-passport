#include "setup_portal.h"

#include "demo_radio.h"
#include "feishu_store.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "setup_portal";

static httpd_handle_t s_http;
static TaskHandle_t s_dns_task;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static bool s_wifi_initialized;
static volatile bool s_dns_running;
static volatile bool s_received;
static wifi_ap_record_t s_networks[12];
static uint16_t s_network_count;

typedef enum {
    PORTAL_WIFI = 0,
    PORTAL_FEISHU,
} portal_mode_t;

static portal_mode_t s_mode;

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t questions;
    uint16_t answers;
    uint16_t authority;
    uint16_t additional;
} dns_header_t;

typedef struct __attribute__((packed)) {
    uint16_t pointer;
    uint16_t type;
    uint16_t class_value;
    uint32_t ttl;
    uint16_t length;
    uint32_t address;
} dns_answer_t;

static size_t dns_question_end(const uint8_t *packet, size_t length)
{
    size_t index = sizeof(dns_header_t);
    while (index < length && packet[index] != 0) {
        size_t label = packet[index];
        if (label > 63 || index + label + 1 >= length) return 0;
        index += label + 1;
    }
    if (index + 5 > length) return 0;
    return index + 5;
}
static void dns_task(void *argument)
{
    (void)argument;
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    struct timeval timeout = { .tv_sec = 1 };
    uint8_t packet[320];

    if (socket_fd < 0 || bind(socket_fd, (struct sockaddr *)&address,
                              sizeof(address)) != 0) {
        ESP_LOGW(TAG, "captive DNS could not start");
        if (socket_fd >= 0) close(socket_fd);
        s_dns_task = NULL;
        vTaskDelete(NULL);
    }
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    while (s_dns_running) {
        struct sockaddr_in source;
        socklen_t source_length = sizeof(source);
        int length = recvfrom(socket_fd, packet, sizeof(packet) - sizeof(dns_answer_t),
                              0, (struct sockaddr *)&source, &source_length);
        if (length <= 0) continue;
        size_t end = dns_question_end(packet, (size_t)length);
        dns_header_t *header = (dns_header_t *)packet;
        if (end == 0 || ntohs(header->questions) != 1) continue;
        header->flags = htons(0x8180);
        header->answers = htons(1);
        dns_answer_t *answer = (dns_answer_t *)(packet + length);
        answer->pointer = htons(0xc00c);
        answer->type = htons(1);
        answer->class_value = htons(1);
        answer->ttl = htonl(60);
        answer->length = htons(4);
        answer->address = inet_addr("192.168.4.1");
        sendto(socket_fd, packet, length + sizeof(*answer), 0,
               (struct sockaddr *)&source, source_length);
    }
    close(socket_fd);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void html_escape(char *output, size_t capacity, const char *input)
{
    size_t used = 0;
    for (size_t i = 0; input[i] != '\0' && used + 6 < capacity; ++i) {
        const char *replacement = NULL;
        if (input[i] == '&') replacement = "&amp;";
        if (input[i] == '<') replacement = "&lt;";
        if (input[i] == '>') replacement = "&gt;";
        if (input[i] == '\"') replacement = "&quot;";
        if (replacement != NULL) {
            size_t size = strlen(replacement);
            memcpy(output + used, replacement, size);
            used += size;
        } else if ((unsigned char)input[i] >= 0x20) {
            output[used++] = input[i];
        }
    }
    output[used] = '\0';
}

static esp_err_t root_get(httpd_req_t *request)
{
    if (s_mode == PORTAL_FEISHU) {
        httpd_resp_set_type(request, "text/html; charset=utf-8");
        return httpd_resp_send(request,
            "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
            "<meta charset=utf-8><title>AI Passport 私人飞书</title><style>body{font-family:-apple-system,"
            "sans-serif;background:#f5f4ee;margin:0;padding:28px;color:#161616}.card{max-width:480px;"
            "margin:auto;background:white;padding:24px;border-radius:18px;box-shadow:0 8px 30px #0001}"
            "h1{font-size:24px}label{display:block;margin-top:18px;font-weight:600}input,button{"
            "box-sizing:border-box;width:100%;font-size:17px;padding:13px;margin-top:8px;border:1px solid #ccc;"
            "border-radius:10px}button{background:#3370ff;color:white;border:0;font-weight:700;margin-top:24px}"
            ".tip{color:#666;line-height:1.55}.warn{background:#fff4d9;padding:12px;border-radius:10px}"
            "</style></head><body><div class=card><h1>配置自己的飞书应用</h1>"
            "<p class=tip>凭据从手机直接发送到当前设备，不经过互联网或发布者服务器。</p>"
            "<p class=warn>请只填写你本人创建或有权使用的飞书自建应用。</p>"
            "<form method=post action=/configure><label>App ID</label>"
            "<input name=app_id required maxlength=63 placeholder=cli_... autocomplete=off>"
            "<label>App Secret</label><input name=app_secret type=password required maxlength=127 "
            "autocomplete=new-password><button type=submit>写入设备并继续</button></form>"
            "</div></body></html>", HTTPD_RESP_USE_STRLEN);
    }
    char *page = calloc(1, 7168);
    size_t used = 0;
    if (page == NULL) return ESP_ERR_NO_MEM;
    used += snprintf(page + used, 7168 - used,
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<meta charset=utf-8><title>AI Passport 首次设置</title><style>body{font-family:-apple-system,"
        "sans-serif;background:#f5f4ee;margin:0;padding:28px;color:#161616}.card{max-width:480px;"
        "margin:auto;background:white;padding:24px;border-radius:18px;box-shadow:0 8px 30px #0001}"
        "h1{font-size:24px}label{display:block;margin-top:18px;font-weight:600}select,input,button{"
        "box-sizing:border-box;width:100%%;font-size:17px;padding:13px;margin-top:8px;border:1px solid #ccc;"
        "border-radius:10px}button{background:#3370ff;color:white;border:0;font-weight:700;margin-top:24px}"
        ".tip{color:#666;line-height:1.55}.prep{background:#eef4ff;padding:12px;border-radius:10px;line-height:1.55}"
        "</style></head><body><div class=card><h1>连接 Wi-Fi</h1>"
        "<p class=prep>请选择 2.4 GHz Wi-Fi。ESP32-C3 不支持 5 GHz。</p>"
        "<form method=post action=/configure><label>Wi-Fi 网络</label><select name=ssid required>");
    for (uint16_t i = 0; i < s_network_count && used < 6500; ++i) {
        char escaped[160];
        html_escape(escaped, sizeof(escaped), (const char *)s_networks[i].ssid);
        used += snprintf(page + used, 7168 - used,
                         "<option value=\"%s\">%s (%d dBm)</option>",
                         escaped, escaped, s_networks[i].rssi);
    }
    snprintf(page + used, 7168 - used,
        "</select><label>Wi-Fi 密码</label><input name=password type=password maxlength=64 "
        "autocomplete=current-password><button type=submit>连接</button></form></div></body></html>");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return err;
}

static int from_hex(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    value = (char)tolower((unsigned char)value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

static bool url_decode_field(const char *body, const char *name,
                             char *output, size_t capacity)
{
    size_t name_length = strlen(name);
    const char *cursor = body;
    while (cursor != NULL && *cursor != '\0') {
        if (strncmp(cursor, name, name_length) == 0 && cursor[name_length] == '=') {
            cursor += name_length + 1;
            size_t used = 0;
            while (*cursor != '\0' && *cursor != '&' && used + 1 < capacity) {
                if (*cursor == '+') {
                    output[used++] = ' ';
                    ++cursor;
                } else if (*cursor == '%' && from_hex(cursor[1]) >= 0 &&
                           from_hex(cursor[2]) >= 0) {
                    output[used++] = (char)((from_hex(cursor[1]) << 4) |
                                            from_hex(cursor[2]));
                    cursor += 3;
                } else {
                    output[used++] = *cursor++;
                }
            }
            output[used] = '\0';
            return used > 0;
        }
        cursor = strchr(cursor, '&');
        if (cursor != NULL) ++cursor;
    }
    return false;
}

static esp_err_t save_feishu_form(const char *body)
{
    feishu_credentials_t *credentials = calloc(1, sizeof(*credentials));
    if (credentials == NULL) return ESP_ERR_NO_MEM;
    bool valid = url_decode_field(body, "app_id", credentials->app_id,
                                  sizeof(credentials->app_id)) &&
                 url_decode_field(body, "app_secret", credentials->app_secret,
                                  sizeof(credentials->app_secret));
    if (valid) {
        char *values[] = { credentials->app_id, credentials->app_secret };
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
            char *start = values[i];
            while (*start != '\0' && isspace((unsigned char)*start)) ++start;
            if (start != values[i]) memmove(values[i], start, strlen(start) + 1);
            size_t length = strlen(values[i]);
            while (length > 0 &&
                   isspace((unsigned char)values[i][length - 1])) {
                values[i][--length] = '\0';
            }
        }
        valid = credentials->app_id[0] != '\0' &&
                credentials->app_secret[0] != '\0';
    }
    esp_err_t err = valid ? feishu_store_save_app_credentials(credentials) :
                            ESP_ERR_INVALID_ARG;
    memset(credentials, 0, sizeof(*credentials));
    free(credentials);
    return err;
}

static esp_err_t configure_post(httpd_req_t *request)
{
    char body[1024] = { 0 };
    if (s_mode == PORTAL_FEISHU) {
        int expected = request->content_len;
        int received = 0;
        if (expected <= 0 || expected >= (int)sizeof(body)) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form");
            return ESP_FAIL;
        }
        while (received < expected) {
            int count = httpd_req_recv(request, body + received,
                                       expected - received);
            if (count <= 0) {
                memset(body, 0, sizeof(body));
                return ESP_FAIL;
            }
            received += count;
        }
        esp_err_t err = save_feishu_form(body);
        memset(body, 0, sizeof(body));
        if (err != ESP_OK) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                "App ID or App Secret is invalid");
            return err;
        }
        httpd_resp_set_type(request, "text/html; charset=utf-8");
        httpd_resp_send(request,
            "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
            "<meta charset=utf-8><h2>飞书应用已保存</h2>"
            "<p>请回到设备，用飞书扫描接下来显示的授权二维码。</p>",
            HTTPD_RESP_USE_STRLEN);
        s_received = true;
        return ESP_OK;
    }
    char ssid[33] = { 0 };
    char password[65] = { 0 };
    wifi_config_t config = { 0 };
    int expected = request->content_len;
    int received = 0;

    if (expected <= 0 || expected >= (int)sizeof(body)) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form");
        return ESP_FAIL;
    }
    while (received < expected) {
        int count = httpd_req_recv(request, body + received, expected - received);
        if (count <= 0) return ESP_FAIL;
        received += count;
    }
    if (!url_decode_field(body, "ssid", ssid, sizeof(ssid))) {
        memset(password, 0, sizeof(password));
        memset(body, 0, sizeof(body));
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                            "Wi-Fi network is required");
        return ESP_FAIL;
    }
    url_decode_field(body, "password", password, sizeof(password));
    esp_err_t err;
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    memcpy(config.sta.password, password, strlen(password));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    memset(password, 0, sizeof(password));
    memset(body, 0, sizeof(body));
    if (err != ESP_OK) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Could not save Wi-Fi");
        return err;
    }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_send(request,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<meta charset=utf-8><h2>Wi-Fi 已保存</h2><p>请回到设备。</p>",
        HTTPD_RESP_USE_STRLEN);
    s_received = true;
    return ESP_OK;
}

static esp_err_t redirect_404(httpd_req_t *request, httpd_err_code_t error)
{
    (void)error;
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
    return httpd_resp_send(request, "Open setup", HTTPD_RESP_USE_STRLEN);
}

static void random_password(char output[17])
{
    static const char alphabet[] =
        "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    uint8_t random[12];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); ++i) {
        output[i] = alphabet[random[i] % (sizeof(alphabet) - 1)];
    }
    output[sizeof(random)] = '\0';
    memset(random, 0, sizeof(random));
}

static esp_err_t portal_start(setup_portal_info_t *info, portal_mode_t mode)
{
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t config = { 0 };
    uint8_t mac[6];
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    esp_err_t err;

    if (info == NULL) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    s_mode = mode;
    s_received = false;
    err = demo_radio_nvs_prepare();
    if (err == ESP_OK) err = demo_radio_network_prepare();
    if (err != ESP_OK) return err;
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_ap_netif == NULL || s_sta_netif == NULL) {
        setup_portal_stop();
        return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        setup_portal_stop();
        return err;
    }
    s_wifi_initialized = true;
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(info->ssid, sizeof(info->ssid),
             mode == PORTAL_FEISHU ? "FoloFeishu-%02X%02X" :
                                     "FoloPassport-%02X%02X",
             mac[4], mac[5]);
    memcpy(config.ap.ssid, info->ssid, strlen(info->ssid));
    config.ap.ssid_len = strlen(info->ssid);
    config.ap.channel = 1;
    // Both first-run Wi-Fi setup and app-only recovery carry secrets. Protect
    // every local form with a fresh password embedded only in the screen QR.
    random_password(info->password);
    memcpy(config.ap.password, info->password, strlen(info->password));
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 4;
    err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &config);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        setup_portal_stop();
        return err;
    }
    if (mode == PORTAL_WIFI) {
        wifi_scan_config_t scan = { .show_hidden = false };
        if (esp_wifi_scan_start(&scan, true) == ESP_OK) {
            s_network_count = sizeof(s_networks) / sizeof(s_networks[0]);
            esp_wifi_scan_get_ap_records(&s_network_count, s_networks);
        } else {
            s_network_count = 0;
        }
    }
    snprintf(info->address, sizeof(info->address), "192.168.4.1");
    http_config.lru_purge_enable = true;
    http_config.max_open_sockets = 5;
    if (httpd_start(&s_http, &http_config) != ESP_OK) {
        setup_portal_stop();
        return ESP_FAIL;
    }
    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET,
                               .handler = root_get };
    const httpd_uri_t ios_probe = { .uri = "/hotspot-detect.html",
                                    .method = HTTP_GET, .handler = root_get };
    const httpd_uri_t configure = { .uri = "/configure", .method = HTTP_POST,
                                    .handler = configure_post };
    httpd_register_uri_handler(s_http, &root);
    httpd_register_uri_handler(s_http, &ios_probe);
    httpd_register_uri_handler(s_http, &configure);
    httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, redirect_404);
    s_dns_running = true;
    if (xTaskCreate(dns_task, "captive_dns", 3072, NULL, 4,
                    &s_dns_task) != pdPASS) {
        s_dns_running = false;
        s_dns_task = NULL;
    }
    ESP_LOGI(TAG, "%s AP ready: %s",
             mode == PORTAL_FEISHU ? "Feishu setup" : "Wi-Fi setup",
             info->ssid);
    return ESP_OK;
}

esp_err_t setup_portal_start(setup_portal_info_t *info)
{
    return portal_start(info, PORTAL_WIFI);
}

esp_err_t setup_portal_start_feishu(setup_portal_info_t *info)
{
    return portal_start(info, PORTAL_FEISHU);
}

bool setup_portal_credentials_received(void)
{
    return s_received;
}

void setup_portal_stop(void)
{
    if (s_http != NULL) {
        httpd_stop(s_http);
        s_http = NULL;
    }
    s_dns_running = false;
    for (int i = 0; s_dns_task != NULL && i < 15; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_dns_task != NULL) {
        vTaskDelete(s_dns_task);
        s_dns_task = NULL;
    }
    if (s_wifi_initialized) {
        esp_wifi_stop();
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_ap_netif != NULL) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    if (s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    memset(s_networks, 0, sizeof(s_networks));
    s_network_count = 0;
    s_mode = PORTAL_WIFI;
}
