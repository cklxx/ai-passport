#pragma once

#include "esp_err.h"
#include "feishu_model.h"

#include <stddef.h>

#define FEISHU_APP_ID_MAX 64
#define FEISHU_APP_SECRET_MAX 128
// Feishu user tokens are large opaque values. Real lark-cli credentials are
// currently about 7 KiB, so the former 2 KiB buffer was not sufficient.
#define FEISHU_TOKEN_MAX 8192

typedef struct {
    char app_id[FEISHU_APP_ID_MAX];
    char app_secret[FEISHU_APP_SECRET_MAX];
    char refresh_token[FEISHU_TOKEN_MAX];
} feishu_credentials_t;

esp_err_t feishu_store_load_credentials(feishu_credentials_t *credentials);
// App credentials are provisioned by the device owner. The refresh token may
// be empty until that app completes device authorization on this device.
esp_err_t feishu_store_load_app_credentials(feishu_credentials_t *credentials);
esp_err_t feishu_store_save_app_credentials(
    const feishu_credentials_t *credentials);
esp_err_t feishu_store_save_app_credentials_json(const uint8_t *json,
                                                 size_t length);
esp_err_t feishu_store_save_credentials(const feishu_credentials_t *credentials);
esp_err_t feishu_store_save_credentials_json(const uint8_t *json, size_t length);
esp_err_t feishu_store_save_refresh_token(const char *refresh_token);
esp_err_t feishu_store_load_access_token(char *access_token, size_t capacity);
esp_err_t feishu_store_save_access_token(const char *access_token);
esp_err_t feishu_store_clear_access_token(void);
esp_err_t feishu_store_clear_credentials(void);

esp_err_t feishu_store_load_seen(feishu_seen_t *seen, size_t capacity,
                                 size_t *count);
esp_err_t feishu_store_mark_seen(const char *chat_id, uint64_t time_ms);
