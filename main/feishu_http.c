#include "feishu_http.h"

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "feishu_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "feishu_http";
static esp_http_client_handle_t s_stream_client;

typedef struct response_chunk {
    struct response_chunk *next;
    size_t length;
    char data[];
} response_chunk_t;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    size_t limit;
    esp_err_t error;
    bool segmented;
    response_chunk_t *head;
    response_chunk_t *tail;
} response_buffer_t;

typedef struct {
    FILE *file;
    size_t length;
    size_t limit;
    esp_err_t error;
} download_file_t;

static esp_err_t download_event(esp_http_client_event_t *event)
{
    download_file_t *download = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || download->error != ESP_OK) {
        return download->error;
    }
    size_t length = (size_t)event->data_len;
    if (download->length > download->limit ||
        length > download->limit - download->length) {
        download->error = ESP_ERR_INVALID_SIZE;
    } else if (length > 0 &&
               fwrite(event->data, 1, length, download->file) != length) {
        download->error = ESP_FAIL;
    } else {
        download->length += length;
    }
    return download->error;
}

static esp_err_t append_response(response_buffer_t *buffer, const char *data,
                                 size_t length)
{
    size_t required;
    size_t capacity;
    char *next;

    if (length == 0) return ESP_OK;
    if (buffer->length > buffer->limit || length > buffer->limit - buffer->length) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (buffer->segmented) {
        response_chunk_t *chunk = malloc(sizeof(*chunk) + length);
        if (chunk == NULL) return ESP_ERR_NO_MEM;
        chunk->next = NULL;
        chunk->length = length;
        memcpy(chunk->data, data, length);
        if (buffer->tail == NULL) buffer->head = chunk;
        else buffer->tail->next = chunk;
        buffer->tail = chunk;
        buffer->length += length;
        return ESP_OK;
    }
    required = buffer->length + length + 1;
    if (required > buffer->capacity) {
        capacity = buffer->capacity == 0 ? 1024 : buffer->capacity;
        while (capacity < required && capacity < buffer->limit + 1) {
            capacity *= 2;
        }
        if (capacity > buffer->limit + 1) capacity = buffer->limit + 1;
        if (capacity < required) return ESP_ERR_INVALID_SIZE;
        next = realloc(buffer->data, capacity);
        if (next == NULL) return ESP_ERR_NO_MEM;
        buffer->data = next;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return ESP_OK;
}

static void free_response_chunks(response_buffer_t *buffer)
{
    response_chunk_t *chunk = buffer->head;
    while (chunk != NULL) {
        response_chunk_t *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    buffer->head = NULL;
    buffer->tail = NULL;
}

static esp_err_t join_response_chunks(response_buffer_t *buffer)
{
    char *joined = malloc(buffer->length + 1);
    size_t offset = 0;
    if (joined == NULL) return ESP_ERR_NO_MEM;
    for (response_chunk_t *chunk = buffer->head; chunk != NULL;
         chunk = chunk->next) {
        memcpy(joined + offset, chunk->data, chunk->length);
        offset += chunk->length;
    }
    joined[offset] = '\0';
    buffer->data = joined;
    buffer->capacity = buffer->length + 1;
    return ESP_OK;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_buffer_t *buffer = event->user_data;

    if (event->event_id == HTTP_EVENT_ON_DATA && buffer->error == ESP_OK) {
        buffer->error = append_response(buffer, event->data,
                                        (size_t)event->data_len);
    }
    return buffer->error;
}

typedef struct {
    char *access;
    size_t access_capacity;
    char *refresh;
    size_t refresh_capacity;
    char *error_text;
    size_t error_capacity;
    char key[32];
    size_t key_length;
    char *active_output;
    size_t active_capacity;
    size_t active_length;
    size_t total_length;
    bool in_string;
    bool escaped;
    bool string_is_value;
    bool key_ready;
    bool expecting_value;
    esp_err_t error;
} oauth_extract_t;

static void oauth_select_output(oauth_extract_t *extract)
{
    extract->active_output = NULL;
    extract->active_capacity = 0;
    extract->active_length = 0;
    if (strcmp(extract->key, "access_token") == 0) {
        extract->active_output = extract->access;
        extract->active_capacity = extract->access_capacity;
    } else if (strcmp(extract->key, "refresh_token") == 0) {
        extract->active_output = extract->refresh;
        extract->active_capacity = extract->refresh_capacity;
    } else if (strcmp(extract->key, "error") == 0) {
        extract->active_output = extract->error_text;
        extract->active_capacity = extract->error_capacity;
    }
}

static void oauth_append_char(oauth_extract_t *extract, char value)
{
    if (extract->active_output != NULL) {
        if (extract->active_length + 1 >= extract->active_capacity) {
            extract->error = ESP_ERR_INVALID_SIZE;
            return;
        }
        extract->active_output[extract->active_length++] = value;
    } else if (!extract->string_is_value &&
               extract->key_length + 1 < sizeof(extract->key)) {
        extract->key[extract->key_length++] = value;
    }
}

static void oauth_parse_bytes(oauth_extract_t *extract, const char *data,
                              size_t length)
{
    for (size_t i = 0; i < length && extract->error == ESP_OK; ++i) {
        char value = data[i];
        ++extract->total_length;
        if (extract->total_length > 32768) {
            extract->error = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (extract->in_string) {
            if (extract->escaped) {
                extract->escaped = false;
                oauth_append_char(extract, value);
            } else if (value == '\\') {
                extract->escaped = true;
            } else if (value == '"') {
                extract->in_string = false;
                if (extract->active_output != NULL) {
                    extract->active_output[extract->active_length] = '\0';
                } else if (!extract->string_is_value) {
                    extract->key[extract->key_length] = '\0';
                    extract->key_ready = true;
                }
            } else {
                oauth_append_char(extract, value);
            }
            continue;
        }
        if (value == '"') {
            extract->in_string = true;
            extract->escaped = false;
            extract->string_is_value = extract->expecting_value;
            extract->key_length = 0;
            extract->active_output = NULL;
            extract->active_capacity = 0;
            extract->active_length = 0;
            if (extract->expecting_value) oauth_select_output(extract);
            extract->expecting_value = false;
        } else if (value == ':' && extract->key_ready) {
            extract->expecting_value = true;
            extract->key_ready = false;
        } else if (value != ' ' && value != '\t' && value != '\r' &&
                   value != '\n') {
            if (extract->expecting_value) extract->expecting_value = false;
            if (value == ',' || value == '}' || value == ']') {
                extract->key_ready = false;
            }
        }
    }
}

static esp_err_t oauth_event(esp_http_client_event_t *event)
{
    oauth_extract_t *extract = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && extract->error == ESP_OK) {
        oauth_parse_bytes(extract, event->data, (size_t)event->data_len);
    }
    return extract->error;
}

esp_err_t feishu_http_oauth_request(const char *url, const char *json_body,
                                    char *access_token, size_t access_capacity,
                                    char *refresh_token, size_t refresh_capacity,
                                    char *oauth_error, size_t error_capacity,
                                    int *status_code, size_t *response_length)
{
    oauth_extract_t extract = {
        .access = access_token,
        .access_capacity = access_capacity,
        .refresh = refresh_token,
        .refresh_capacity = refresh_capacity,
        .error_text = oauth_error,
        .error_capacity = error_capacity,
        .error = ESP_OK,
    };
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = oauth_event,
        .user_data = &extract,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .disable_auto_redirect = false,
    };
    if (url == NULL || json_body == NULL || status_code == NULL ||
        response_length == NULL || oauth_error == NULL || error_capacity < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (access_token != NULL && access_capacity > 0) access_token[0] = '\0';
    if (refresh_token != NULL && refresh_capacity > 0) refresh_token[0] = '\0';
    oauth_error[0] = '\0';
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type",
                               "application/json; charset=utf-8");
    size_t body_length = strlen(json_body);
    esp_err_t err = esp_http_client_open(client, (int)body_length);
    // esp_http_client_perform writes the complete post field in one transport
    // call. A ~7 KiB refresh token can stall that path on ESP32-C3. Send it in
    // bounded pieces and then read the response through the same event parser.
    for (size_t offset = 0; err == ESP_OK && offset < body_length;) {
        size_t remaining = body_length - offset;
        int piece = (int)(remaining > 1024 ? 1024 : remaining);
        int written = esp_http_client_write(client, json_body + offset, piece);
        if (written <= 0) err = ESP_ERR_HTTP_WRITE_DATA;
        else offset += (size_t)written;
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) {
        err = ESP_ERR_HTTP_FETCH_HEADER;
    }
    char discard[512];
    while (err == ESP_OK) {
        int read = esp_http_client_read(client, discard, sizeof(discard));
        if (read < 0) err = ESP_FAIL;
        if (read <= 0) break;
    }
    *status_code = esp_http_client_get_status_code(client);
    *response_length = extract.total_length;
    esp_http_client_cleanup(client);
    if (err == ESP_OK && extract.error != ESP_OK) err = extract.error;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OAuth stream failed: %s, HTTP %d, bytes %u",
                 esp_err_to_name(err), *status_code,
                 (unsigned)*response_length);
    }
    return err;
}

esp_err_t feishu_http_request(esp_http_client_method_t method,
                              const char *url, const char *bearer_token,
                              const char *json_body, size_t response_limit,
                              feishu_http_response_t *response)
{
    response_buffer_t buffer = {
        .limit = response_limit,
        .error = ESP_OK,
        .segmented = true,
    };
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .event_handler = http_event,
        .user_data = &buffer,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client;
    esp_err_t err;
    char *authorization = NULL;

    if (url == NULL || response == NULL || response_limit == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bearer_token != NULL && bearer_token[0] != '\0') {
        // ESP HTTP serializes the request line and Authorization header into
        // this buffer. Feishu user access tokens can be over 7 KiB.
        config.buffer_size_tx = (int)strlen(bearer_token) + 512;
    }
    memset(response, 0, sizeof(*response));
    client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Accept", "application/json");
    if (bearer_token != NULL && bearer_token[0] != '\0') {
        size_t authorization_size = strlen(bearer_token) + 8;
        authorization = malloc(authorization_size);
        if (authorization == NULL) {
            esp_http_client_cleanup(client);
            return ESP_ERR_NO_MEM;
        }
        int written = snprintf(authorization, authorization_size,
                               "Bearer %s", bearer_token);
        if (written < 0 || (size_t)written >= authorization_size) {
            memset(authorization, 0, authorization_size);
            free(authorization);
            esp_http_client_cleanup(client);
            return ESP_ERR_INVALID_SIZE;
        }
        err = esp_http_client_set_header(client, "Authorization", authorization);
        memset(authorization, 0, authorization_size);
        free(authorization);
        authorization = NULL;
        if (err != ESP_OK) {
            esp_http_client_cleanup(client);
            return err;
        }
    }
    if (json_body != NULL) {
        esp_http_client_set_header(client, "Content-Type",
                                   "application/json; charset=utf-8");
        esp_http_client_set_post_field(client, json_body, strlen(json_body));
    }

    err = esp_http_client_perform(client);
    response->status_code = esp_http_client_get_status_code(client);
    // Free TLS/client allocations before asking the heap for one contiguous
    // JSON buffer. During the request the body lives in small linked chunks.
    esp_http_client_cleanup(client);
    client = NULL;
    if (err == ESP_OK && buffer.error != ESP_OK) err = buffer.error;
    if (err == ESP_OK) err = join_response_chunks(&buffer);
    if (err == ESP_OK) {
        response->body = buffer.data;
        response->length = buffer.length;
        buffer.data = NULL;
    } else {
        ESP_LOGW(TAG, "request failed: %s, HTTP %d",
                 esp_err_to_name(err), response->status_code);
    }
    if (authorization != NULL) {
        memset(authorization, 0, strlen(authorization));
        free(authorization);
    }
    free(buffer.data);
    free_response_chunks(&buffer);
    if (client != NULL) esp_http_client_cleanup(client);
    return err;
}

esp_err_t feishu_http_download_file(const char *url, const char *bearer_token,
                                    const char *path, size_t limit,
                                    int *status_code, size_t *length)
{
    download_file_t download = { .limit = limit, .error = ESP_OK };
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = download_event,
        .user_data = &download,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = NULL;
    char *authorization = NULL;
    esp_err_t err = ESP_FAIL;

    if (url == NULL || bearer_token == NULL || bearer_token[0] == '\0' ||
        path == NULL || limit == 0 || status_code == NULL || length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *status_code = 0;
    *length = 0;
    download.file = fopen(path, "wb");
    if (download.file == NULL) return ESP_FAIL;
    config.buffer_size_tx = (int)strlen(bearer_token) + 512;
    client = esp_http_client_init(&config);
    if (client == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    size_t authorization_size = strlen(bearer_token) + 8;
    authorization = malloc(authorization_size);
    if (authorization == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    snprintf(authorization, authorization_size, "Bearer %s", bearer_token);
    err = esp_http_client_set_header(client, "Authorization", authorization);
    memset(authorization, 0, authorization_size);
    free(authorization);
    authorization = NULL;
    if (err != ESP_OK) goto done;
    esp_http_client_set_header(client, "Accept", "*/*");
    err = esp_http_client_perform(client);
    *status_code = esp_http_client_get_status_code(client);
    if (err == ESP_OK && download.error != ESP_OK) err = download.error;

done:
    if (authorization != NULL) {
        memset(authorization, 0, strlen(authorization));
        free(authorization);
    }
    if (client != NULL) esp_http_client_cleanup(client);
    fclose(download.file);
    *length = download.length;
    if (err != ESP_OK || *status_code < 200 || *status_code >= 300) {
        remove(path);
    }
    return err;
}

void feishu_http_response_free(feishu_http_response_t *response)
{
    if (response == NULL) return;
    free(response->body);
    memset(response, 0, sizeof(*response));
}

esp_err_t feishu_http_stream_request(const char *url, const char *bearer_token,
                                     const char *json_body,
                                     size_t response_limit,
                                     feishu_http_response_t *response)
{
    response_buffer_t buffer = {
        .limit = response_limit,
        .error = ESP_OK,
    };
    char *authorization = NULL;
    esp_err_t err;

    if (url == NULL || bearer_token == NULL || json_body == NULL ||
        response == NULL || response_limit == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(response, 0, sizeof(*response));
    if (s_stream_client == NULL) {
        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .event_handler = http_event,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 20000,
            .buffer_size = 2048,
            // The Authorization header contains the large user token.
            .buffer_size_tx = (int)strlen(bearer_token) + 512,
            .keep_alive_enable = true,
        };
        s_stream_client = esp_http_client_init(&config);
        if (s_stream_client == NULL) return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_user_data(s_stream_client, &buffer);
    size_t authorization_size = strlen(bearer_token) + 8;
    authorization = malloc(authorization_size);
    if (authorization == NULL) return ESP_ERR_NO_MEM;
    snprintf(authorization, authorization_size, "Bearer %s", bearer_token);
    err = esp_http_client_set_header(s_stream_client, "Authorization", authorization);
    memset(authorization, 0, authorization_size);
    free(authorization);
    authorization = NULL;
    if (err != ESP_OK) {
        feishu_http_stream_close();
        return err;
    }
    esp_http_client_set_header(s_stream_client, "Accept", "application/json");
    esp_http_client_set_header(s_stream_client, "Content-Type",
                               "application/json; charset=utf-8");
    esp_http_client_set_post_field(s_stream_client, json_body, strlen(json_body));
    err = esp_http_client_perform(s_stream_client);
    response->status_code = esp_http_client_get_status_code(s_stream_client);
    if (err == ESP_OK && buffer.error != ESP_OK) err = buffer.error;
    if (err == ESP_OK && buffer.data == NULL) {
        buffer.data = calloc(1, 1);
        if (buffer.data == NULL) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        response->body = buffer.data;
        response->length = buffer.length;
        buffer.data = NULL;
    } else {
        ESP_LOGW(TAG, "stream request failed: %s, HTTP %d",
                 esp_err_to_name(err), response->status_code);
        feishu_http_stream_close();
    }
    free(buffer.data);
    return err;
}

void feishu_http_stream_close(void)
{
    if (s_stream_client != NULL) {
        esp_http_client_cleanup(s_stream_client);
        s_stream_client = NULL;
    }
}
