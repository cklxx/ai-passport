#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char ssid[33];
    char password[17];
    char address[16];
} setup_portal_info_t;

esp_err_t setup_portal_start(setup_portal_info_t *info);
// Starts a short-lived, WPA2-protected captive portal that accepts only the
// owner's Feishu App ID and App Secret. The password is returned in info so it
// can be encoded in a Wi-Fi QR code; it is never logged.
esp_err_t setup_portal_start_feishu(setup_portal_info_t *info);
bool setup_portal_credentials_received(void);
void setup_portal_stop(void);
