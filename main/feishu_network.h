#pragma once

#include "esp_err.h"

#include <stdbool.h>

esp_err_t feishu_network_start(unsigned timeout_ms);
esp_err_t feishu_network_forget_wifi(void);
void feishu_network_stop(void);
bool feishu_network_connected(void);
