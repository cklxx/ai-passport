#include "feishu_store.h"

#include "cJSON.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdlib.h>
#include <string.h>

#define FEISHU_NVS_NAMESPACE "feishu"
#define FEISHU_NVS_APP_ID "app_id"
#define FEISHU_NVS_APP_SECRET "app_secret"
#define FEISHU_NVS_REFRESH "refresh"
#define FEISHU_NVS_OWNER_VERSION "owner_v"
#define FEISHU_OWNER_VERSION 1
#define FEISHU_NVS_SEEN "seen"
#define FEISHU_CACHE_PARTITION "feishu_cache"
#define FEISHU_CACHE_NAMESPACE "tokens"
#define FEISHU_CACHE_ACCESS "access"

static bool s_cache_ready;

static esp_err_t require_owner_provisioning(nvs_handle_t handle)
{
    uint8_t version = 0;
    esp_err_t err = nvs_get_u8(handle, FEISHU_NVS_OWNER_VERSION, &version);

    if (err != ESP_OK) return err;
    return version == FEISHU_OWNER_VERSION ? ESP_OK : ESP_ERR_INVALID_VERSION;
}

static esp_err_t cache_prepare(void)
{
    if (s_cache_ready) return ESP_OK;
    esp_err_t err = nvs_flash_init_partition(FEISHU_CACHE_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // This partition is only a disposable access-token cache.
        err = nvs_flash_erase_partition(FEISHU_CACHE_PARTITION);
        if (err == ESP_OK) err = nvs_flash_init_partition(FEISHU_CACHE_PARTITION);
    }
    if (err == ESP_OK) s_cache_ready = true;
    return err;
}

static esp_err_t load_string(nvs_handle_t handle, const char *key,
                             char *value, size_t capacity)
{
    size_t required = capacity;
    esp_err_t err = nvs_get_str(handle, key, value, &required);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        value[0] = '\0';
        return err;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH) return ESP_ERR_INVALID_SIZE;
    return err;
}

static esp_err_t load_token_blob(nvs_handle_t handle, char *value,
                                 size_t capacity)
{
    size_t required = capacity;
    esp_err_t err = nvs_get_blob(handle, FEISHU_NVS_REFRESH, value, &required);

    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        // Backward compatibility for short tokens stored by older firmware.
        return load_string(handle, FEISHU_NVS_REFRESH, value, capacity);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) value[0] = '\0';
    if (err == ESP_OK && (required == 0 || required > capacity ||
                          value[required - 1] != '\0')) {
        value[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

esp_err_t feishu_store_load_credentials(feishu_credentials_t *credentials)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (credentials == NULL) return ESP_ERR_INVALID_ARG;
    memset(credentials, 0, sizeof(*credentials));
    err = nvs_open(FEISHU_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    // Credentials from firmware predating owner provisioning have no marker.
    // Refuse them so an OTA upgrade cannot silently retain a developer's app.
    err = require_owner_provisioning(handle);
    if (err == ESP_OK) {
        err = load_string(handle, FEISHU_NVS_APP_ID, credentials->app_id,
                          sizeof(credentials->app_id));
    }
    if (err == ESP_OK) {
        err = load_string(handle, FEISHU_NVS_APP_SECRET,
                          credentials->app_secret,
                          sizeof(credentials->app_secret));
    }
    if (err == ESP_OK) {
        err = load_token_blob(handle, credentials->refresh_token,
                              sizeof(credentials->refresh_token));
    }
    nvs_close(handle);
    return err;
}

esp_err_t feishu_store_load_app_credentials(feishu_credentials_t *credentials)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (credentials == NULL) return ESP_ERR_INVALID_ARG;
    memset(credentials, 0, sizeof(*credentials));
    err = nvs_open(FEISHU_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    err = require_owner_provisioning(handle);
    if (err == ESP_OK) {
        err = load_string(handle, FEISHU_NVS_APP_ID, credentials->app_id,
                          sizeof(credentials->app_id));
    }
    if (err == ESP_OK) {
        err = load_string(handle, FEISHU_NVS_APP_SECRET,
                          credentials->app_secret,
                          sizeof(credentials->app_secret));
    }
    nvs_close(handle);
    if (err != ESP_OK || strncmp(credentials->app_id, "cli_", 4) != 0 ||
        credentials->app_secret[0] == '\0') {
        memset(credentials, 0, sizeof(*credentials));
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }
    return ESP_OK;
}

esp_err_t feishu_store_save_app_credentials(
    const feishu_credentials_t *credentials)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (credentials == NULL ||
        strncmp(credentials->app_id, "cli_", 4) != 0 ||
        credentials->app_secret[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    err = nvs_open(FEISHU_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, FEISHU_NVS_APP_ID, credentials->app_id);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, FEISHU_NVS_APP_SECRET,
                          credentials->app_secret);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, FEISHU_NVS_OWNER_VERSION,
                         FEISHU_OWNER_VERSION);
    }
    // Importing another app invalidates every token from the previous app.
    if (err == ESP_OK) {
        esp_err_t next = nvs_erase_key(handle, FEISHU_NVS_REFRESH);
        if (next != ESP_OK && next != ESP_ERR_NVS_NOT_FOUND) err = next;
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) err = feishu_store_clear_access_token();
    return err;
}

esp_err_t feishu_store_save_credentials(const feishu_credentials_t *credentials)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (credentials == NULL || credentials->app_id[0] == '\0' ||
        credentials->app_secret[0] == '\0' ||
        credentials->refresh_token[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    err = nvs_open(FEISHU_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, FEISHU_NVS_APP_ID, credentials->app_id);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, FEISHU_NVS_APP_SECRET,
                          credentials->app_secret);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, FEISHU_NVS_REFRESH,
                           credentials->refresh_token,
                           strlen(credentials->refresh_token) + 1);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, FEISHU_NVS_OWNER_VERSION,
                         FEISHU_OWNER_VERSION);
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static bool json_string(cJSON *root, const char *name, char *output,
                        size_t output_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        item->valuestring[0] == '\0' || strlen(item->valuestring) >= output_size) {
        return false;
    }
    memcpy(output, item->valuestring, strlen(item->valuestring) + 1);
    return true;
}

esp_err_t feishu_store_save_app_credentials_json(const uint8_t *json,
                                                 size_t length)
{
    feishu_credentials_t *credentials;
    cJSON *root;
    bool valid;

    if (json == NULL || length == 0 || length > 512) {
        return ESP_ERR_INVALID_ARG;
    }
    credentials = calloc(1, sizeof(*credentials));
    if (credentials == NULL) return ESP_ERR_NO_MEM;
    root = cJSON_ParseWithLength((const char *)json, length);
    if (root == NULL) {
        free(credentials);
        return ESP_ERR_INVALID_ARG;
    }
    valid = json_string(root, "app_id", credentials->app_id,
                        sizeof(credentials->app_id)) &&
            json_string(root, "app_secret", credentials->app_secret,
                        sizeof(credentials->app_secret));
    cJSON_Delete(root);
    esp_err_t err = valid ? feishu_store_save_app_credentials(credentials) :
                            ESP_ERR_INVALID_ARG;
    memset(credentials, 0, sizeof(*credentials));
    free(credentials);
    return err;
}

esp_err_t feishu_store_save_credentials_json(const uint8_t *json, size_t length)
{
    feishu_credentials_t *credentials;
    cJSON *root;
    bool valid;

    if (json == NULL || length == 0 || length > FEISHU_TOKEN_MAX + 1024) {
        return ESP_ERR_INVALID_ARG;
    }
    credentials = calloc(1, sizeof(*credentials));
    if (credentials == NULL) return ESP_ERR_NO_MEM;
    root = cJSON_ParseWithLength((const char *)json, length);
    if (root == NULL) {
        free(credentials);
        return ESP_ERR_INVALID_ARG;
    }
    valid = json_string(root, "app_id", credentials->app_id,
                        sizeof(credentials->app_id)) &&
            json_string(root, "app_secret", credentials->app_secret,
                        sizeof(credentials->app_secret)) &&
            json_string(root, "refresh_token", credentials->refresh_token,
                        sizeof(credentials->refresh_token));
    cJSON_Delete(root);
    if (!valid || strncmp(credentials->app_id, "cli_", 4) != 0) {
        memset(credentials, 0, sizeof(*credentials));
        free(credentials);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = feishu_store_save_credentials(credentials);
    memset(credentials, 0, sizeof(*credentials));
    free(credentials);
    return err;
}

esp_err_t feishu_store_save_refresh_token(const char *refresh_token)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (refresh_token == NULL || refresh_token[0] == '\0' ||
        strlen(refresh_token) >= FEISHU_TOKEN_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    err = nvs_open(FEISHU_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, FEISHU_NVS_REFRESH, refresh_token,
                       strlen(refresh_token) + 1);
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        // Updating a ~7 KiB blob atomically temporarily requires space for
        // both generations. This small partition cannot hold two copies, so
        // reclaim the old generation while the new token is still in RAM.
        esp_err_t erase_err = nvs_erase_key(handle, FEISHU_NVS_REFRESH);
        if (erase_err == ESP_OK || erase_err == ESP_ERR_NVS_NOT_FOUND) {
            erase_err = nvs_commit(handle);
        }
        if (erase_err == ESP_OK) {
            err = nvs_set_blob(handle, FEISHU_NVS_REFRESH, refresh_token,
                               strlen(refresh_token) + 1);
        } else {
            err = erase_err;
        }
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t feishu_store_load_access_token(char *access_token, size_t capacity)
{
    nvs_handle_t handle;
    size_t required = capacity;
    esp_err_t err;

    if (access_token == NULL || capacity < 2) return ESP_ERR_INVALID_ARG;
    access_token[0] = '\0';
    err = cache_prepare();
    if (err != ESP_OK) return err;
    err = nvs_open_from_partition(FEISHU_CACHE_PARTITION,
                                  FEISHU_CACHE_NAMESPACE, NVS_READONLY,
                                  &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(handle, FEISHU_CACHE_ACCESS, access_token, &required);
    nvs_close(handle);
    if (err == ESP_OK && (required == 0 || required > capacity ||
                          access_token[required - 1] != '\0')) {
        access_token[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

esp_err_t feishu_store_save_access_token(const char *access_token)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (access_token == NULL || access_token[0] == '\0' ||
        strlen(access_token) >= FEISHU_TOKEN_MAX) return ESP_ERR_INVALID_ARG;
    err = cache_prepare();
    if (err != ESP_OK) return err;
    err = nvs_open_from_partition(FEISHU_CACHE_PARTITION,
                                  FEISHU_CACHE_NAMESPACE, NVS_READWRITE,
                                  &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, FEISHU_CACHE_ACCESS, access_token,
                       strlen(access_token) + 1);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t feishu_store_clear_access_token(void)
{
    nvs_handle_t handle;
    esp_err_t err = cache_prepare();
    if (err != ESP_OK) return err;
    err = nvs_open_from_partition(FEISHU_CACHE_PARTITION,
                                  FEISHU_CACHE_NAMESPACE, NVS_READWRITE,
                                  &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(handle, FEISHU_CACHE_ACCESS);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t feishu_store_clear_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(FEISHU_NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK) return err;
    err = nvs_erase_key(handle, FEISHU_NVS_APP_ID);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) {
        esp_err_t next = nvs_erase_key(handle, FEISHU_NVS_APP_SECRET);
        if (next != ESP_OK && next != ESP_ERR_NVS_NOT_FOUND) err = next;
    }
    if (err == ESP_OK) {
        esp_err_t next = nvs_erase_key(handle, FEISHU_NVS_REFRESH);
        if (next != ESP_OK && next != ESP_ERR_NVS_NOT_FOUND) err = next;
    }
    if (err == ESP_OK) {
        esp_err_t next = nvs_erase_key(handle, FEISHU_NVS_OWNER_VERSION);
        if (next != ESP_OK && next != ESP_ERR_NVS_NOT_FOUND) err = next;
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) err = feishu_store_clear_access_token();
    return err;
}

esp_err_t feishu_store_load_seen(feishu_seen_t *seen, size_t capacity,
                                 size_t *count)
{
    nvs_handle_t handle;
    size_t bytes;
    esp_err_t err;

    if (seen == NULL || count == NULL || capacity == 0) return ESP_ERR_INVALID_ARG;
    *count = 0;
    bytes = capacity * sizeof(*seen);
    err = nvs_open(FEISHU_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(handle, FEISHU_NVS_SEEN, seen, &bytes);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK || bytes % sizeof(*seen) != 0) {
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    *count = bytes / sizeof(*seen);
    return ESP_OK;
}

esp_err_t feishu_store_mark_seen(const char *chat_id, uint64_t time_ms)
{
    feishu_seen_t seen[FEISHU_MAX_CHATS] = { 0 };
    size_t count = 0;
    size_t index = 0;
    nvs_handle_t handle;
    esp_err_t err;

    if (chat_id == NULL || chat_id[0] == '\0') return ESP_ERR_INVALID_ARG;
    err = feishu_store_load_seen(seen, FEISHU_MAX_CHATS, &count);
    if (err != ESP_OK) return err;
    for (index = 0; index < count; ++index) {
        if (strcmp(seen[index].chat_id, chat_id) == 0) break;
    }
    if (index == count) {
        if (count < FEISHU_MAX_CHATS) {
            ++count;
        } else {
            index = 0;
            for (size_t i = 1; i < count; ++i) {
                if (seen[i].last_seen_time_ms < seen[index].last_seen_time_ms) {
                    index = i;
                }
            }
        }
        feishu_utf8_copy(seen[index].chat_id, sizeof(seen[index].chat_id), chat_id);
    }
    if (time_ms > seen[index].last_seen_time_ms) {
        seen[index].last_seen_time_ms = time_ms;
    }

    err = nvs_open(FEISHU_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, FEISHU_NVS_SEEN, seen, count * sizeof(*seen));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
