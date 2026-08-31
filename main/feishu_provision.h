#pragma once

#include "esp_err.h"

#include <stdbool.h>

// Owner provisioning is available only while onboarding is waiting for an
// app. The task reads a bounded, versioned line from the USB Serial/JTAG
// console and stores the owner-provided app credentials in NVS.
esp_err_t feishu_provision_start(void);
void feishu_provision_stop(void);
bool feishu_provision_credentials_received(void);

