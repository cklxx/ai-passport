#include "feishu_binding.h"

#include "cJSON.h"
#include "esp_log.h"
#include "feishu_http.h"
#include "feishu_model.h"
#include <stdlib.h>
#include <string.h>

#define FEISHU_DEVICE_AUTH_URL \
    "https://accounts.feishu.cn/oauth/v1/device_authorization"
#define FEISHU_DEVICE_TOKEN_URL \
    "https://open.feishu.cn/open-apis/authen/v2/oauth/token"
#define FEISHU_PRODUCT_SCOPES \
    "im:chat:readonly im:message im:message.p2p_msg:get_as_user " \
    "im:message.group_msg:get_as_user " \
    "speech_to_text:speech offline_access"

static const char *TAG = "feishu_binding";
static char *s_pending_access_token;

static void clear_pending_access_token(void)
{
    if (s_pending_access_token != NULL) {
        memset(s_pending_access_token, 0, FEISHU_TOKEN_MAX);
        free(s_pending_access_token);
        s_pending_access_token = NULL;
    }
}

bool feishu_binding_take_access_token(char *output, size_t capacity)
{
    bool available = false;
    if (output != NULL && s_pending_access_token != NULL) {
        size_t length = strlen(s_pending_access_token);
        if (length > 0 && length < capacity) {
            memcpy(output, s_pending_access_token, length + 1);
            available = true;
        }
    }
    clear_pending_access_token();
    return available;
}

static bool copy_string(cJSON *object, const char *name, char *output,
                        size_t capacity)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        item->valuestring[0] == '\0' || strlen(item->valuestring) >= capacity) {
        return false;
    }
    memcpy(output, item->valuestring, strlen(item->valuestring) + 1);
    return true;
}

static bool log_begin_failure(esp_err_t transport_err,
                              const feishu_http_response_t *response)
{
    if (transport_err != ESP_OK) {
        ESP_LOGW(TAG, "device authorization transport failed: %s, HTTP %d",
                 esp_err_to_name(transport_err), response->status_code);
        return false;
    }
    char oauth_error[64] = { 0 };
    int api_code = 0;
    cJSON *root = response->body == NULL ? NULL :
                  cJSON_ParseWithLength(response->body, response->length);
    if (root != NULL) {
        copy_string(root, "error", oauth_error, sizeof(oauth_error));
        cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        if (cJSON_IsNumber(code)) api_code = code->valueint;
    }
    // Only public protocol identifiers and numeric status are logged. Never
    // log the response body: some upstream errors may echo request fields.
    ESP_LOGW(TAG,
             "device authorization rejected: error=%s, code=%d, HTTP %d, bytes=%u",
             oauth_error[0] == '\0' ? "unknown" : oauth_error, api_code,
             response->status_code, (unsigned)response->length);
    bool invalid_client = strcmp(oauth_error, "invalid_client") == 0;
    cJSON_Delete(root);
    return invalid_client;
}

static char *request_body(const feishu_credentials_t *credentials,
                          const char *device_code)
{
    cJSON *root = cJSON_CreateObject();
    char *body;

    if (root == NULL) return NULL;
    cJSON_AddStringToObject(root, "client_id", credentials->app_id);
    cJSON_AddStringToObject(root, "client_secret", credentials->app_secret);
    if (device_code == NULL) {
        cJSON_AddStringToObject(root, "scope", FEISHU_PRODUCT_SCOPES);
    } else {
        cJSON_AddStringToObject(root, "grant_type",
                               "urn:ietf:params:oauth:grant-type:device_code");
        cJSON_AddStringToObject(root, "device_code", device_code);
    }
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

bool feishu_binding_app_configured(void)
{
    feishu_credentials_t *credentials = calloc(1, sizeof(*credentials));
    if (credentials == NULL) return false;
    bool configured = feishu_store_load_app_credentials(credentials) == ESP_OK;
    memset(credentials, 0, sizeof(*credentials));
    free(credentials);
    return configured;
}

esp_err_t feishu_binding_begin(feishu_binding_request_t *request)
{
    feishu_http_response_t response = { 0 };
    cJSON *root = NULL;
    cJSON *number;
    char *body;
    feishu_credentials_t *credentials = NULL;
    bool valid;
    esp_err_t err;

    if (request == NULL) return ESP_ERR_INVALID_ARG;
    credentials = calloc(1, sizeof(*credentials));
    if (credentials == NULL) return ESP_ERR_NO_MEM;
    if (feishu_store_load_app_credentials(credentials) != ESP_OK) {
        free(credentials);
        return ESP_ERR_INVALID_STATE;
    }
    clear_pending_access_token();
    memset(request, 0, sizeof(*request));
    body = request_body(credentials, NULL);
    memset(credentials, 0, sizeof(*credentials));
    free(credentials);
    if (body == NULL) return ESP_ERR_NO_MEM;
    err = feishu_http_request(HTTP_METHOD_POST, FEISHU_DEVICE_AUTH_URL, NULL,
                              body, 4096, &response);
    memset(body, 0, strlen(body));
    free(body);
    if (err != ESP_OK || response.status_code < 200 ||
        response.status_code >= 300) {
        bool invalid_client = log_begin_failure(err, &response);
        if (err == ESP_OK) {
            // This code is consumed only by onboarding to reopen owner-app
            // provisioning. Transport/server failures must never erase a
            // valid application configuration.
            err = invalid_client ? ESP_ERR_INVALID_ARG : ESP_FAIL;
        }
        goto done;
    }
    root = cJSON_ParseWithLength(response.body, response.length);
    if (root == NULL) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    valid = copy_string(root, "device_code", request->device_code,
                        sizeof(request->device_code));
    // Always prefer the complete URI. Some Feishu responses also contain a
    // generic verification URI that requires the user code to be entered.
    valid = (copy_string(root, "verification_uri_complete",
                         request->verification_url,
                         sizeof(request->verification_url)) ||
             copy_string(root, "verification_url", request->verification_url,
                         sizeof(request->verification_url)) ||
             copy_string(root, "verification_uri", request->verification_url,
                         sizeof(request->verification_url))) && valid;
    number = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
    request->expires_in = cJSON_IsNumber(number) ? (uint32_t)number->valuedouble : 600;
    number = cJSON_GetObjectItemCaseSensitive(root, "interval");
    request->interval = cJSON_IsNumber(number) ? (uint32_t)number->valuedouble : 5;
    if (request->interval < 2) request->interval = 2;
    err = valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    if (!valid) {
        ESP_LOGW(TAG,
                 "device authorization success response missing required fields: HTTP %d, bytes=%u",
                 response.status_code, (unsigned)response.length);
    }

done:
    cJSON_Delete(root);
    feishu_http_response_free(&response);
    return err;
}

esp_err_t feishu_binding_poll(const feishu_binding_request_t *request,
                              feishu_binding_status_t *status,
                              feishu_credentials_t *credentials)
{
    char *body;
    char oauth_error[64] = { 0 };
    int status_code = 0;
    size_t response_length = 0;
    char *access_token = NULL;
    esp_err_t err;

    if (request == NULL || status == NULL || credentials == NULL ||
        request->device_code[0] == '\0') return ESP_ERR_INVALID_ARG;
    *status = FEISHU_BINDING_FAILED;
    credentials->refresh_token[0] = '\0';
    access_token = calloc(1, FEISHU_TOKEN_MAX);
    if (access_token == NULL) return ESP_ERR_NO_MEM;
    if (feishu_store_load_app_credentials(credentials) != ESP_OK) {
        memset(access_token, 0, FEISHU_TOKEN_MAX);
        free(access_token);
        return ESP_ERR_INVALID_STATE;
    }
    body = request_body(credentials, request->device_code);
    if (body == NULL) {
        free(access_token);
        return ESP_ERR_NO_MEM;
    }
    err = feishu_http_oauth_request(
        FEISHU_DEVICE_TOKEN_URL, body, access_token, FEISHU_TOKEN_MAX,
        credentials->refresh_token, sizeof(credentials->refresh_token),
        oauth_error, sizeof(oauth_error), &status_code, &response_length);
    memset(body, 0, strlen(body));
    free(body);
    if (err != ESP_OK) {
        memset(access_token, 0, FEISHU_TOKEN_MAX);
        free(access_token);
        return err;
    }
    ESP_LOGI(TAG, "OAuth poll HTTP %d, response bytes %u",
             status_code, (unsigned)response_length);
    if (status_code >= 200 && status_code < 300) {
        if (access_token[0] != '\0' && credentials->refresh_token[0] == '\0') {
            ESP_LOGW(TAG,
                     "OAuth grant has no refresh token; enable refresh-token access for the application");
            memset(access_token, 0, FEISHU_TOKEN_MAX);
            free(access_token);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (access_token[0] == '\0') {
            ESP_LOGW(TAG, "OAuth success response has no access token");
            free(access_token);
            return ESP_ERR_INVALID_RESPONSE;
        }
        err = feishu_store_save_credentials(credentials);
        if (err == ESP_OK) {
            esp_err_t cache_err = feishu_store_save_access_token(access_token);
            if (cache_err != ESP_OK) {
                ESP_LOGW(TAG, "access-token cache failed: %s",
                         esp_err_to_name(cache_err));
            }
            clear_pending_access_token();
            s_pending_access_token = access_token;
            access_token = NULL;
        }
        *status = err == ESP_OK ? FEISHU_BINDING_COMPLETE : FEISHU_BINDING_FAILED;
        if (access_token != NULL) {
            memset(access_token, 0, FEISHU_TOKEN_MAX);
            free(access_token);
        }
        return err;
    }
    if (oauth_error[0] != '\0') {
        if (strcmp(oauth_error, "authorization_pending") == 0) {
            *status = FEISHU_BINDING_PENDING;
            err = ESP_OK;
        } else if (strcmp(oauth_error, "slow_down") == 0) {
            *status = FEISHU_BINDING_SLOW_DOWN;
            err = ESP_OK;
        } else if (strcmp(oauth_error, "expired_token") == 0 ||
                   strcmp(oauth_error, "expired_device_code") == 0) {
            *status = FEISHU_BINDING_EXPIRED;
            err = ESP_OK;
        } else {
            // OAuth error identifiers are public protocol values and safe to
            // log; tokens, device codes and response bodies remain redacted.
            ESP_LOGW(TAG, "OAuth poll rejected: %s (HTTP %d)",
                     oauth_error, status_code);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "OAuth poll rejected: HTTP %d", status_code);
        err = ESP_FAIL;
    }
    memset(access_token, 0, FEISHU_TOKEN_MAX);
    free(access_token);
    return err;
}
