#include "feishu_api.h"

#include "cJSON.h"
#include "esp_log.h"
#include "feishu_http.h"
#include "feishu_binding.h"
#include "mbedtls/base64.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "feishu_api";

#define FEISHU_OAUTH_URL "https://accounts.feishu.cn/oauth/v3/token"
#define FEISHU_TENANT_TOKEN_URL \
    "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal"
#define FEISHU_USER_INFO_URL \
    "https://open.feishu.cn/open-apis/authen/v1/user_info"
#define FEISHU_CHATS_URL \
    "https://open.feishu.cn/open-apis/im/v1/chats?types=p2p%%2Cgroup" \
    "&sort_type=ByActiveTimeDesc&page_size=%u&user_id_type=open_id"
#define FEISHU_MESSAGES_URL \
    "https://open.feishu.cn/open-apis/im/v1/messages?container_id_type=chat" \
    "&container_id=%s&sort_type=ByCreateTimeDesc&page_size=%u" \
    "&with_sender_name=true"
#define FEISHU_REPLY_URL \
    "https://open.feishu.cn/open-apis/im/v1/messages/%s/reply"
#define FEISHU_SEND_URL \
    "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=chat_id"
#define FEISHU_RESOURCE_URL \
    "https://open.feishu.cn/open-apis/im/v1/messages/%s/resources/%s?type=image"
#define FEISHU_ASR_STREAM_URL \
    "https://open.feishu.cn/open-apis/speech_to_text/v1/speech/stream_recognize"

static cJSON *parse_success(feishu_http_response_t *response)
{
    cJSON *root;
    cJSON *code;
    cJSON *message;

    if (response->body == NULL) {
        ESP_LOGW(TAG, "HTTP status %d", response->status_code);
        return NULL;
    }
    root = cJSON_ParseWithLength(response->body, response->length);
    if (root == NULL) return NULL;
    code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (response->status_code < 200 || response->status_code >= 300) {
        ESP_LOGW(TAG, "HTTP status %d, OpenAPI code %d",
                 response->status_code,
                 cJSON_IsNumber(code) ? code->valueint : -1);
        cJSON_Delete(root);
        return NULL;
    }
    if (cJSON_IsNumber(code) && code->valueint != 0) {
        message = cJSON_GetObjectItemCaseSensitive(root, "msg");
        ESP_LOGW(TAG, "OpenAPI error %d: %s", code->valueint,
                 cJSON_IsString(message) ? message->valuestring : "unknown");
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool copy_json_string(cJSON *object, const char *name, char *output,
                             size_t output_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strlen(item->valuestring) >= output_size) {
        return false;
    }
    memcpy(output, item->valuestring, strlen(item->valuestring) + 1);
    return true;
}

static char *json_print(cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static esp_err_t refresh_user_token(feishu_api_session_t *session,
                                    feishu_credentials_t *credentials)
{
    cJSON *request = cJSON_CreateObject();
    char *body;
    char *next_refresh = NULL;
    char oauth_error[64] = { 0 };
    int status_code = 0;
    size_t response_length = 0;
    esp_err_t err = ESP_FAIL;

    if (request == NULL) return ESP_ERR_NO_MEM;
    next_refresh = calloc(1, FEISHU_TOKEN_MAX);
    if (next_refresh == NULL) {
        cJSON_Delete(request);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(request, "grant_type", "refresh_token");
    cJSON_AddStringToObject(request, "client_id", credentials->app_id);
    cJSON_AddStringToObject(request, "client_secret", credentials->app_secret);
    cJSON_AddStringToObject(request, "refresh_token", credentials->refresh_token);
    ESP_LOGI(TAG, "refresh token bytes %u",
             (unsigned)strlen(credentials->refresh_token));
    body = json_print(request);
    if (body == NULL) {
        free(next_refresh);
        return ESP_ERR_NO_MEM;
    }
    err = feishu_http_oauth_request(
        FEISHU_OAUTH_URL, body,
        session->user_access_token, sizeof(session->user_access_token),
        next_refresh, FEISHU_TOKEN_MAX, oauth_error, sizeof(oauth_error),
        &status_code, &response_length);
    memset(body, 0, strlen(body));
    free(body);
    if (err != ESP_OK) goto done;
    ESP_LOGI(TAG, "OAuth refresh HTTP %d, response bytes %u",
             status_code, (unsigned)response_length);
    if (status_code >= 500) {
        err = ESP_FAIL;
        goto done;
    }
    if (status_code < 200 || status_code >= 300 ||
        session->user_access_token[0] == '\0' || next_refresh[0] == '\0') {
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    err = feishu_store_save_refresh_token(next_refresh);
    if (err == ESP_OK) {
        feishu_utf8_copy(credentials->refresh_token,
                         sizeof(credentials->refresh_token), next_refresh);
        esp_err_t cache_err = feishu_store_save_access_token(
            session->user_access_token);
        if (cache_err != ESP_OK) {
            ESP_LOGW(TAG, "access-token cache failed: %s",
                     esp_err_to_name(cache_err));
        }
    }

done:
    if (next_refresh != NULL) {
        memset(next_refresh, 0, FEISHU_TOKEN_MAX);
        free(next_refresh);
    }
    return err;
}

static esp_err_t fetch_tenant_token(feishu_api_session_t *session)
{
    feishu_http_response_t response = { 0 };
    cJSON *request = cJSON_CreateObject();
    cJSON *root = NULL;
    char *body;
    esp_err_t err;

    if (request == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(request, "app_id", session->app_id);
    cJSON_AddStringToObject(request, "app_secret", session->app_secret);
    body = json_print(request);
    if (body == NULL) return ESP_ERR_NO_MEM;
    err = feishu_http_request(HTTP_METHOD_POST, FEISHU_TENANT_TOKEN_URL, NULL,
                              body, 4096, &response);
    memset(body, 0, strlen(body));
    free(body);
    if (err != ESP_OK) goto done;
    if (response.status_code == 401 || response.status_code == 403) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        err = ESP_FAIL;
        goto done;
    }
    root = parse_success(&response);
    if (root == NULL ||
        !copy_json_string(root, "tenant_access_token",
                          session->tenant_access_token,
                          sizeof(session->tenant_access_token))) {
        err = ESP_ERR_INVALID_RESPONSE;
    }

done:
    cJSON_Delete(root);
    feishu_http_response_free(&response);
    return err;
}

static bool user_token_rejected(const feishu_http_response_t *response)
{
    if (response->status_code == 401 || response->status_code == 403) return true;
    if (response->status_code < 200 || response->status_code >= 300 ||
        response->body == NULL) return false;
    cJSON *root = cJSON_ParseWithLength(response->body, response->length);
    cJSON *code = root == NULL ? NULL :
        cJSON_GetObjectItemCaseSensitive(root, "code");
    bool rejected = cJSON_IsNumber(code) &&
        (code->valueint == 99991663 || code->valueint == 99991664 ||
         code->valueint == 99991665 || code->valueint == 99991668);
    cJSON_Delete(root);
    return rejected;
}

static esp_err_t refresh_session_token(feishu_api_session_t *session)
{
    feishu_credentials_t *credentials = calloc(1, sizeof(*credentials));
    if (credentials == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = feishu_store_load_credentials(credentials);
    if (err == ESP_OK) err = refresh_user_token(session, credentials);
    memset(credentials, 0, sizeof(*credentials));
    free(credentials);
    return err;
}

static esp_err_t user_request(feishu_api_session_t *session,
                              esp_http_client_method_t method,
                              const char *url, const char *body,
                              size_t response_limit,
                              feishu_http_response_t *response)
{
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        esp_err_t err = feishu_http_request(method, url,
            session->user_access_token, body, response_limit, response);
        if (err != ESP_OK) return err;
        if (!user_token_rejected(response)) return ESP_OK;
        feishu_http_response_free(response);
        if (attempt > 0) return ESP_ERR_INVALID_RESPONSE;
        err = refresh_session_token(session);
        if (err != ESP_OK) return err;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t tenant_request(feishu_api_session_t *session,
                                esp_http_client_method_t method,
                                const char *url, const char *body,
                                size_t response_limit,
                                feishu_http_response_t *response)
{
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        if (session->tenant_access_token[0] == '\0') {
            esp_err_t err = fetch_tenant_token(session);
            if (err != ESP_OK) return err;
        }
        esp_err_t err = feishu_http_request(method, url,
            session->tenant_access_token, body, response_limit, response);
        if (err != ESP_OK) return err;
        if (response->status_code != 401 && response->status_code != 403) {
            return ESP_OK;
        }
        feishu_http_response_free(response);
        session->tenant_access_token[0] = '\0';
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t fetch_user_info(feishu_api_session_t *session)
{
    feishu_http_response_t response = { 0 };
    cJSON *root = NULL;
    cJSON *data;
    esp_err_t err = feishu_http_request(HTTP_METHOD_GET, FEISHU_USER_INFO_URL,
                                        session->user_access_token, NULL, 4096,
                                        &response);

    if (err != ESP_OK) goto done;
    if (response.status_code == 401 || response.status_code == 403) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        err = ESP_FAIL;
        goto done;
    }
    root = parse_success(&response);
    data = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data) ||
        !copy_json_string(data, "open_id", session->self_open_id,
                          sizeof(session->self_open_id))) {
        err = ESP_ERR_INVALID_RESPONSE;
    }

done:
    cJSON_Delete(root);
    feishu_http_response_free(&response);
    return err;
}

esp_err_t feishu_api_authenticate(feishu_api_session_t *session,
                                  feishu_credentials_t *credentials)
{
    esp_err_t err;
    bool token_verified = false;

    if (session == NULL || credentials == NULL) return ESP_ERR_INVALID_ARG;
    memset(session, 0, sizeof(*session));
    feishu_utf8_copy(session->app_id, sizeof(session->app_id),
                     credentials->app_id);
    feishu_utf8_copy(session->app_secret, sizeof(session->app_secret),
                     credentials->app_secret);
    bool have_token = feishu_binding_take_access_token(
        session->user_access_token, sizeof(session->user_access_token));
    if (have_token) {
        ESP_LOGI(TAG, "using access token from completed device binding");
    } else if (feishu_store_load_access_token(
                   session->user_access_token,
                   sizeof(session->user_access_token)) == ESP_OK) {
        have_token = true;
        ESP_LOGI(TAG, "using cached access token");
    }
    if (have_token) {
        for (unsigned attempt = 1; attempt <= 3; ++attempt) {
            err = fetch_user_info(session);
            if (err == ESP_OK || err == ESP_ERR_INVALID_RESPONSE) break;
            ESP_LOGW(TAG, "access-token verification attempt %u failed: %s",
                     attempt, esp_err_to_name(err));
            if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
        }
        token_verified = err == ESP_OK;
        if (!token_verified && err == ESP_ERR_INVALID_RESPONSE) {
            // Only an actual HTTP/auth rejection invalidates the token.
            // DNS, TLS and socket failures must leave the cached token intact.
            ESP_LOGW(TAG, "access token rejected by Feishu; refreshing");
            feishu_store_clear_access_token();
            session->user_access_token[0] = '\0';
        } else if (!token_verified) {
            return err;
        }
    }
    if (!token_verified) {
        err = ESP_FAIL;
        for (unsigned attempt = 1; attempt <= 3; ++attempt) {
            err = refresh_user_token(session, credentials);
            if (err == ESP_OK || err == ESP_ERR_INVALID_RESPONSE) break;
            ESP_LOGW(TAG, "token refresh transport attempt %u failed: %s",
                     attempt, esp_err_to_name(err));
            if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(1500 * attempt));
        }
    }
    if (err == ESP_OK && !token_verified) err = fetch_user_info(session);
    return err;
}

esp_err_t feishu_api_list_chats(feishu_api_session_t *session,
                                feishu_chat_t *chats, size_t capacity,
                                size_t *count)
{
    char url[384];
    feishu_http_response_t response = { 0 };
    cJSON *root = NULL;
    cJSON *data;
    cJSON *items;
    cJSON *item;
    size_t output_count = 0;
    esp_err_t err;

    if (session == NULL || chats == NULL || count == NULL || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    snprintf(url, sizeof(url), FEISHU_CHATS_URL, (unsigned)capacity);
    err = user_request(session, HTTP_METHOD_GET, url, NULL, 16384, &response);
    if (err != ESP_OK) goto done;
    root = parse_success(&response);
    data = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "data");
    items = data == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(data, "items");
    if (!cJSON_IsArray(items)) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    cJSON_ArrayForEach(item, items) {
        cJSON *mode;
        if (output_count >= capacity) break;
        if (!copy_json_string(item, "chat_id", chats[output_count].chat_id,
                              sizeof(chats[output_count].chat_id)) ||
            !copy_json_string(item, "name", chats[output_count].name,
                              sizeof(chats[output_count].name))) {
            continue;
        }
        mode = cJSON_GetObjectItemCaseSensitive(item, "chat_mode");
        chats[output_count].p2p = cJSON_IsString(mode) &&
            strcmp(mode->valuestring, "p2p") == 0;
        ++output_count;
    }
    *count = output_count;
    err = ESP_OK;

done:
    cJSON_Delete(root);
    feishu_http_response_free(&response);
    return err;
}

static feishu_message_type_t message_type(const char *type)
{
    if (type == NULL) return FEISHU_MESSAGE_OTHER;
    if (strcmp(type, "text") == 0 || strcmp(type, "post") == 0) {
        return FEISHU_MESSAGE_TEXT;
    }
    if (strcmp(type, "audio") == 0) return FEISHU_MESSAGE_AUDIO;
    if (strcmp(type, "image") == 0) return FEISHU_MESSAGE_IMAGE;
    if (strcmp(type, "file") == 0 || strcmp(type, "media") == 0) {
        return FEISHU_MESSAGE_FILE;
    }
    return FEISHU_MESSAGE_OTHER;
}

static void message_text(feishu_message_t *message, const char *content)
{
    cJSON *body;
    cJSON *text;

    if (message->deleted) {
        feishu_utf8_copy(message->text, sizeof(message->text), "[消息已撤回]");
        return;
    }
    if (message->type == FEISHU_MESSAGE_AUDIO) {
        feishu_utf8_copy(message->text, sizeof(message->text), "[语音消息]");
        return;
    }
    if (message->type == FEISHU_MESSAGE_IMAGE) {
        body = cJSON_Parse(content == NULL ? "{}" : content);
        text = body == NULL ? NULL :
            cJSON_GetObjectItemCaseSensitive(body, "image_key");
        if (cJSON_IsString(text)) {
            feishu_utf8_copy(message->resource_key,
                             sizeof(message->resource_key), text->valuestring);
        }
        cJSON_Delete(body);
        feishu_utf8_copy(message->text, sizeof(message->text), "[图片]");
        return;
    }
    if (message->type == FEISHU_MESSAGE_FILE) {
        feishu_utf8_copy(message->text, sizeof(message->text), "[文件]");
        return;
    }
    if (message->type == FEISHU_MESSAGE_OTHER) {
        feishu_utf8_copy(message->text, sizeof(message->text), "[暂不支持的消息]");
        return;
    }
    body = cJSON_Parse(content == NULL ? "{}" : content);
    text = body == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(body, "text");
    feishu_utf8_copy(message->text, sizeof(message->text),
                     cJSON_IsString(text) ? text->valuestring : "[富文本消息]");
    cJSON_Delete(body);
}

static void replace_mentions(feishu_message_t *message, cJSON *item)
{
    cJSON *mentions;
    const char *cursor;
    char output[FEISHU_TEXT_MAX * 2] = { 0 };
    size_t used = 0;

    if (message->type != FEISHU_MESSAGE_TEXT || message->deleted) return;
    mentions = cJSON_GetObjectItemCaseSensitive(item, "mentions");
    if (!cJSON_IsArray(mentions)) return;
    cursor = message->text;
    while (*cursor != '\0' && used + 1 < sizeof(output)) {
        bool replaced = false;
        cJSON *mention;
        cJSON_ArrayForEach(mention, mentions) {
            cJSON *key = cJSON_GetObjectItemCaseSensitive(mention, "key");
            cJSON *name = cJSON_GetObjectItemCaseSensitive(mention, "name");
            const char *match_key;
            if (!cJSON_IsString(key) || !cJSON_IsString(name) ||
                key->valuestring == NULL || name->valuestring == NULL) {
                continue;
            }
            match_key = key->valuestring;
            /*
             * Feishu currently returns keys such as "@_user_1", while some
             * message variants omit the leading '@' in mentions[].key even
             * though body.content still contains it.  Normalize both shapes
             * here so the device never exposes the protocol placeholder.
             */
            bool body_has_at = cursor[0] == '@';
            bool key_has_at = match_key[0] == '@';
            const char *body_key = cursor;
            if (body_has_at && !key_has_at) body_key++;
            if (!body_has_at && key_has_at) match_key++;
            size_t key_length = strlen(match_key);
            if (key_length > 0 && strncmp(cursor, key->valuestring,
                                          strlen(key->valuestring)) == 0) {
                int written = snprintf(output + used, sizeof(output) - used,
                                       "@%s", name->valuestring);
                if (written < 0) return;
                used += (size_t)written < sizeof(output) - used ?
                        (size_t)written : sizeof(output) - used - 1;
                cursor += key_length;
                replaced = true;
                break;
            }
            if (key_length > 0 && strncmp(body_key, match_key, key_length) == 0) {
                int written = snprintf(output + used, sizeof(output) - used,
                                       "@%s", name->valuestring);
                if (written < 0) return;
                used += (size_t)written < sizeof(output) - used ?
                        (size_t)written : sizeof(output) - used - 1;
                cursor = body_key + key_length;
                replaced = true;
                break;
            }
        }
        if (!replaced) output[used++] = *cursor++;
    }
    output[used] = '\0';
    feishu_utf8_copy(message->text, sizeof(message->text), output);
}

static bool parse_message(const feishu_api_session_t *session, cJSON *item,
                          feishu_message_t *message)
{
    cJSON *sender = cJSON_GetObjectItemCaseSensitive(item, "sender");
    cJSON *body = cJSON_GetObjectItemCaseSensitive(item, "body");
    cJSON *content = body == NULL ? NULL :
        cJSON_GetObjectItemCaseSensitive(body, "content");
    cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "msg_type");
    cJSON *create_time = cJSON_GetObjectItemCaseSensitive(item, "create_time");
    cJSON *deleted = cJSON_GetObjectItemCaseSensitive(item, "deleted");
    const char *sender_id;

    memset(message, 0, sizeof(*message));
    if (!copy_json_string(item, "message_id", message->message_id,
                          sizeof(message->message_id)) ||
        !cJSON_IsObject(sender) || !cJSON_IsString(type)) {
        return false;
    }
    copy_json_string(sender, "id", message->sender_id,
                     sizeof(message->sender_id));
    if (!copy_json_string(sender, "sender_name", message->sender_name,
                          sizeof(message->sender_name))) {
        feishu_utf8_copy(message->sender_name, sizeof(message->sender_name),
                         "未知用户");
    }
    sender_id = message->sender_id;
    message->mine = sender_id[0] != '\0' &&
                    strcmp(sender_id, session->self_open_id) == 0;
    message->deleted = cJSON_IsTrue(deleted);
    message->type = message_type(type->valuestring);
    if (cJSON_IsString(create_time)) {
        message->create_time_ms = strtoull(create_time->valuestring, NULL, 10);
    } else if (cJSON_IsNumber(create_time)) {
        message->create_time_ms = (uint64_t)create_time->valuedouble;
    }
    message_text(message, cJSON_IsString(content) ? content->valuestring : NULL);
    replace_mentions(message, item);
    return true;
}

esp_err_t feishu_api_list_messages(feishu_api_session_t *session,
                                   const char *chat_id,
                                   feishu_message_t *messages,
                                   size_t capacity, size_t *count)
{
    char url[512];
    feishu_http_response_t response = { 0 };
    cJSON *root = NULL;
    cJSON *data;
    cJSON *items;
    size_t array_size;
    size_t output_count = 0;
    esp_err_t err;

    if (session == NULL || chat_id == NULL || messages == NULL ||
        count == NULL || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    snprintf(url, sizeof(url), FEISHU_MESSAGES_URL, chat_id,
             (unsigned)capacity);
    err = user_request(session, HTTP_METHOD_GET, url, NULL, 32768, &response);
    if (err != ESP_OK) goto done;
    root = parse_success(&response);
    data = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "data");
    items = data == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(data, "items");
    if (!cJSON_IsArray(items)) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    array_size = (size_t)cJSON_GetArraySize(items);
    for (size_t offset = 0; offset < array_size && output_count < capacity;
         ++offset) {
        size_t source_index = array_size - 1 - offset;
        cJSON *item = cJSON_GetArrayItem(items, (int)source_index);
        if (parse_message(session, item, &messages[output_count])) {
            ++output_count;
        }
    }
    *count = output_count;
    err = ESP_OK;

done:
    cJSON_Delete(root);
    feishu_http_response_free(&response);
    return err;
}

esp_err_t feishu_api_reply_text(feishu_api_session_t *session,
                                const char *message_id, const char *text,
                                const char *idempotency_key)
{
    char url[256];
    feishu_http_response_t response = { 0 };
    cJSON *content = cJSON_CreateObject();
    cJSON *request = NULL;
    cJSON *root = NULL;
    char *content_json = NULL;
    char *body = NULL;
    esp_err_t err = ESP_FAIL;

    if (session == NULL || message_id == NULL || text == NULL || text[0] == '\0' ||
        idempotency_key == NULL || idempotency_key[0] == '\0' ||
        strlen(idempotency_key) > 40) {
        return ESP_ERR_INVALID_ARG;
    }
    if (content == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(content, "text", text);
    content_json = json_print(content);
    request = cJSON_CreateObject();
    if (content_json == NULL || request == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    cJSON_AddStringToObject(request, "content", content_json);
    cJSON_AddStringToObject(request, "msg_type", "text");
    cJSON_AddStringToObject(request, "uuid", idempotency_key);
    body = json_print(request);
    request = NULL;
    if (body == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    snprintf(url, sizeof(url), FEISHU_REPLY_URL, message_id);
    // Reply as the bot (app identity). The bot must be a member of the target
    // chat and the user in its availability scope, per the owner's app config.
    err = tenant_request(session, HTTP_METHOD_POST, url, body, 8192, &response);
    if (err == ESP_OK) {
        root = parse_success(&response);
        if (root == NULL) err = ESP_FAIL;
    }

done:
    free(content_json);
    free(body);
    cJSON_Delete(request);
    cJSON_Delete(root);
    feishu_http_response_free(&response);
    return err;
}

esp_err_t feishu_api_send_text(feishu_api_session_t *session,
                               const char *chat_id, const char *text,
                               const char *idempotency_key)
{
    feishu_http_response_t response = { 0 };
    cJSON *content = cJSON_CreateObject();
    cJSON *request = NULL;
    cJSON *root = NULL;
    char *content_json = NULL;
    char *body = NULL;
    esp_err_t err = ESP_FAIL;

    if (session == NULL || chat_id == NULL || chat_id[0] == '\0' ||
        text == NULL || text[0] == '\0' || idempotency_key == NULL ||
        idempotency_key[0] == '\0' || strlen(idempotency_key) > 40) {
        return ESP_ERR_INVALID_ARG;
    }
    if (content == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(content, "text", text);
    content_json = json_print(content);
    request = cJSON_CreateObject();
    if (content_json == NULL || request == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    cJSON_AddStringToObject(request, "receive_id", chat_id);
    cJSON_AddStringToObject(request, "msg_type", "text");
    cJSON_AddStringToObject(request, "content", content_json);
    cJSON_AddStringToObject(request, "uuid", idempotency_key);
    body = json_print(request);
    request = NULL;
    if (body == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    // Send as the bot (app identity); bot must be in the target chat.
    err = tenant_request(session, HTTP_METHOD_POST, FEISHU_SEND_URL, body,
                         8192, &response);
    if (err == ESP_OK) {
        root = parse_success(&response);
        if (root == NULL) err = ESP_FAIL;
    }

done:
    free(content_json);
    free(body);
    cJSON_Delete(request);
    cJSON_Delete(root);
    feishu_http_response_free(&response);
    return err;
}

esp_err_t feishu_api_download_image(feishu_api_session_t *session,
                                    const feishu_message_t *message,
                                    const char *path, size_t limit)
{
    char url[512];
    int status = 0;
    size_t length = 0;

    if (session == NULL || message == NULL || path == NULL || limit == 0 ||
        message->type != FEISHU_MESSAGE_IMAGE ||
        message->message_id[0] == '\0' || message->resource_key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    int written = snprintf(url, sizeof(url), FEISHU_RESOURCE_URL,
                           message->message_id, message->resource_key);
    if (written < 0 || (size_t)written >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        esp_err_t err = feishu_http_download_file(
            url, session->user_access_token, path, limit, &status, &length);
        if (err != ESP_OK) return err;
        if (status >= 200 && status < 300 && length > 3) {
            unsigned char signature[3] = { 0 };
            FILE *image = fopen(path, "rb");
            bool jpeg = image != NULL &&
                fread(signature, 1, sizeof(signature), image) == sizeof(signature) &&
                signature[0] == 0xff && signature[1] == 0xd8 &&
                signature[2] == 0xff;
            if (image != NULL) fclose(image);
            if (jpeg) return ESP_OK;
            remove(path);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (status != 401 && status != 403) return ESP_FAIL;
        if (attempt > 0) return ESP_ERR_INVALID_RESPONSE;
        err = refresh_session_token(session);
        if (err != ESP_OK) return err;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t feishu_api_asr_stream_packet(feishu_api_session_t *session,
                                       const char stream_id[17],
                                       uint32_t sequence_id, int action,
                                       const int16_t *pcm, size_t samples,
                                       char *recognition, size_t recognition_size)
{
    feishu_http_response_t response = { 0 };
    cJSON *request = NULL;
    cJSON *speech;
    cJSON *config;
    cJSON *root = NULL;
    cJSON *data;
    size_t encoded_size = 0;
    size_t encoded_capacity;
    unsigned char *encoded = NULL;
    char *body = NULL;
    esp_err_t err = ESP_FAIL;

    if (session == NULL || stream_id == NULL || strlen(stream_id) != 16 ||
        pcm == NULL || samples == 0 || recognition == NULL ||
        recognition_size == 0 || action < 0 || action > 3) {
        return ESP_ERR_INVALID_ARG;
    }
    if (session->tenant_access_token[0] == '\0') {
        err = fetch_tenant_token(session);
        if (err != ESP_OK) return err;
    }
    encoded_capacity = ((samples * sizeof(*pcm) + 2) / 3) * 4 + 1;
    encoded = malloc(encoded_capacity);
    if (encoded == NULL) return ESP_ERR_NO_MEM;
    if (mbedtls_base64_encode(encoded, encoded_capacity, &encoded_size,
                              (const unsigned char *)pcm,
                              samples * sizeof(*pcm)) != 0) {
        err = ESP_FAIL;
        goto done;
    }
    encoded[encoded_size] = '\0';
    request = cJSON_CreateObject();
    speech = request == NULL ? NULL : cJSON_AddObjectToObject(request, "speech");
    config = request == NULL ? NULL : cJSON_AddObjectToObject(request, "config");
    if (speech == NULL || config == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    // Keep one Base64 buffer. cJSON_CreateStringReference prevents another
    // multi-kilobyte copy before cJSON_PrintUnformatted creates the body.
    cJSON *speech_data = cJSON_CreateStringReference((const char *)encoded);
    if (speech_data == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    cJSON_AddItemToObject(speech, "speech", speech_data);
    cJSON_AddStringToObject(config, "stream_id", stream_id);
    cJSON_AddNumberToObject(config, "sequence_id", sequence_id);
    cJSON_AddNumberToObject(config, "action", action);
    cJSON_AddStringToObject(config, "format", "pcm");
    cJSON_AddStringToObject(config, "engine_type", "16k_auto");
    body = json_print(request);
    request = NULL;
    if (body == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    free(encoded);
    encoded = NULL;
    err = feishu_http_stream_request(FEISHU_ASR_STREAM_URL,
                                     session->tenant_access_token, body, 8192,
                                     &response);
    if (err != ESP_OK) goto done;
    root = parse_success(&response);
    data = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data)) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    cJSON *text = cJSON_GetObjectItemCaseSensitive(data, "recognition_text");
    if (cJSON_IsString(text) && text->valuestring != NULL) {
        feishu_utf8_copy(recognition, recognition_size, text->valuestring);
    }
    err = ESP_OK;

done:
    free(encoded);
    free(body);
    cJSON_Delete(request);
    cJSON_Delete(root);
    feishu_http_response_free(&response);
    return err;
}

void feishu_api_asr_stream_close(void)
{
    feishu_http_stream_close();
}
