#pragma once

#include "esp_err.h"
#include "feishu_model.h"
#include "feishu_store.h"

#include <stddef.h>
#include <stdint.h>

#define FEISHU_TENANT_TOKEN_MAX 2048

typedef struct {
    char user_access_token[FEISHU_TOKEN_MAX];
    char tenant_access_token[FEISHU_TENANT_TOKEN_MAX];
    char app_id[FEISHU_APP_ID_MAX];
    char app_secret[FEISHU_APP_SECRET_MAX];
    char self_open_id[FEISHU_CHAT_ID_MAX];
} feishu_api_session_t;

esp_err_t feishu_api_authenticate(feishu_api_session_t *session,
                                  feishu_credentials_t *credentials);
esp_err_t feishu_api_list_chats(feishu_api_session_t *session,
                                feishu_chat_t *chats, size_t capacity,
                                size_t *count);
esp_err_t feishu_api_list_messages(feishu_api_session_t *session,
                                   const char *chat_id,
                                   feishu_message_t *messages,
                                   size_t capacity, size_t *count);
esp_err_t feishu_api_reply_text(feishu_api_session_t *session,
                                const char *message_id, const char *text,
                                const char *idempotency_key);
esp_err_t feishu_api_send_text(feishu_api_session_t *session,
                               const char *chat_id, const char *text,
                               const char *idempotency_key);
esp_err_t feishu_api_download_image(feishu_api_session_t *session,
                                    const feishu_message_t *message,
                                    const char *path, size_t limit);
esp_err_t feishu_api_asr_stream_packet(feishu_api_session_t *session,
                                       const char stream_id[17],
                                       uint32_t sequence_id, int action,
                                       const int16_t *pcm, size_t samples,
                                       char *recognition, size_t recognition_size);
void feishu_api_asr_stream_close(void);
