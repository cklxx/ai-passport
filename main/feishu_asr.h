#pragma once

#include "esp_err.h"
#include "feishu_api.h"

#include <stdbool.h>

esp_err_t feishu_asr_record(feishu_api_session_t *session,
                            volatile bool *stop_requested,
                            volatile unsigned *elapsed_ms,
                            char *recognition, size_t recognition_size);
