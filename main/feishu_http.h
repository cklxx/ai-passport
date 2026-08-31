#pragma once

#include "esp_err.h"
#include "esp_http_client.h"

#include <stddef.h>

typedef struct {
    char *body;
    size_t length;
    int status_code;
} feishu_http_response_t;

esp_err_t feishu_http_request(esp_http_client_method_t method,
                              const char *url, const char *bearer_token,
                              const char *json_body, size_t response_limit,
                              feishu_http_response_t *response);
// Memory-bounded OAuth response reader. It extracts only the large token
// strings and the small error string while HTTPS data is streaming.
esp_err_t feishu_http_oauth_request(const char *url, const char *json_body,
                                    char *access_token, size_t access_capacity,
                                    char *refresh_token, size_t refresh_capacity,
                                    char *oauth_error, size_t error_capacity,
                                    int *status_code, size_t *response_length);
esp_err_t feishu_http_stream_request(const char *url, const char *bearer_token,
                                     const char *json_body,
                                     size_t response_limit,
                                     feishu_http_response_t *response);
esp_err_t feishu_http_download_file(const char *url, const char *bearer_token,
                                    const char *path, size_t limit,
                                    int *status_code, size_t *length);
void feishu_http_stream_close(void);
void feishu_http_response_free(feishu_http_response_t *response);
