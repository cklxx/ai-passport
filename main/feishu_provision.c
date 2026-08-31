#include "feishu_provision.h"

#include "feishu_store.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROVISION_PREFIX "FAP-FEISHU/1 "
#define PROVISION_READY "FAP-FEISHU/1 READY"
#define PROVISION_OK "FAP-FEISHU/1 OK"
#define PROVISION_ERROR "FAP-FEISHU/1 ERROR"
#define PROVISION_LINE_MAX 768
#define PROVISION_JSON_MAX 512

static const char *TAG = "feishu_provision";
static TaskHandle_t s_task;
static volatile bool s_received;
static volatile bool s_stop;

static bool process_line(char *line, uint8_t *json)
{
    if (strncmp(line, PROVISION_PREFIX, strlen(PROVISION_PREFIX)) != 0) {
        return false;
    }
    const unsigned char *encoded =
        (const unsigned char *)(line + strlen(PROVISION_PREFIX));
    size_t encoded_length = strlen((const char *)encoded);
    size_t json_length = 0;
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (encoded_length > 0 &&
        mbedtls_base64_decode(json, PROVISION_JSON_MAX, &json_length,
                              encoded, encoded_length) == 0 &&
        json_length > 0 && json_length <= PROVISION_JSON_MAX) {
        json[json_length] = '\0';
        result = feishu_store_save_app_credentials_json(json, json_length);
    }
    memset(json, 0, PROVISION_JSON_MAX + 1);
    memset(line, 0, PROVISION_LINE_MAX);
    puts(result == ESP_OK ? PROVISION_OK : PROVISION_ERROR);
    return result == ESP_OK;
}

static void provision_task(void *argument)
{
    (void)argument;
    bool accepted = false;
    char *line = calloc(1, PROVISION_LINE_MAX);
    uint8_t *json = calloc(1, PROVISION_JSON_MAX + 1);
    if (line == NULL || json == NULL) {
        ESP_LOGE(TAG, "could not allocate USB provisioning buffers");
        goto finished;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    puts(PROVISION_READY);
    size_t used = 0;
    while (!s_stop && !accepted) {
        char value;
        ssize_t count = read(STDIN_FILENO, &value, 1);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (count <= 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (value == '\n' || value == '\r') {
            if (used > 0) {
                line[used] = '\0';
                accepted = process_line(line, json);
                used = 0;
            }
        } else if (used + 1 < PROVISION_LINE_MAX) {
            line[used++] = value;
        } else {
            memset(line, 0, PROVISION_LINE_MAX);
            used = 0;
            puts(PROVISION_ERROR);
        }
    }

finished:
    if (line != NULL) {
        memset(line, 0, PROVISION_LINE_MAX);
        free(line);
    }
    if (json != NULL) {
        memset(json, 0, PROVISION_JSON_MAX + 1);
        free(json);
    }
    s_received = accepted;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t feishu_provision_start(void)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    s_received = false;
    s_stop = false;
    return xTaskCreate(provision_task, "feishu_provision", 4096, NULL, 4,
                       &s_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void feishu_provision_stop(void)
{
    if (s_task != NULL) {
        s_stop = true;
        for (unsigned attempt = 0; attempt < 20 && s_task != NULL; ++attempt) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_task != NULL) {
            ESP_LOGW(TAG, "USB provisioning task did not stop in time");
        }
    }
}

bool feishu_provision_credentials_received(void)
{
    return s_received;
}
