#pragma once

#include "esp_err.h"
#include "feishu_store.h"

#include <stdint.h>

#define FEISHU_BIND_URL_MAX 512
#define FEISHU_DEVICE_CODE_MAX 256

typedef struct {
    char device_code[FEISHU_DEVICE_CODE_MAX];
    char verification_url[FEISHU_BIND_URL_MAX];
    uint32_t expires_in;
    uint32_t interval;
} feishu_binding_request_t;

typedef enum {
    FEISHU_BINDING_PENDING = 0,
    FEISHU_BINDING_COMPLETE,
    FEISHU_BINDING_SLOW_DOWN,
    FEISHU_BINDING_EXPIRED,
    FEISHU_BINDING_FAILED,
} feishu_binding_status_t;

bool feishu_binding_app_configured(void);
esp_err_t feishu_binding_begin(feishu_binding_request_t *request);
esp_err_t feishu_binding_poll(const feishu_binding_request_t *request,
                              feishu_binding_status_t *status,
                              feishu_credentials_t *credentials);
// Consumes the short-lived access token returned by a just-completed device
// authorization. This avoids immediately rotating the new refresh token.
bool feishu_binding_take_access_token(char *output, size_t capacity);
