// Direct-to-Feishu messenger demo. Network and ASR work never runs in the
// button/LVGL task; the physical OK key is the final send confirmation.
#include "demo.h"
#include "bsp_battery.h"
#include "feishu_api.h"
#include "feishu_asr.h"
#include "feishu_network.h"
#include "feishu_store.h"
#include "ui_pixel.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "demo_feishu";

typedef enum {
    WORK_INIT = 0,
    WORK_LOAD_MESSAGES,
    WORK_LOAD_IMAGE,
    WORK_RECORD,
    WORK_SEND,
    WORK_RESET_WIFI,
    WORK_UNBIND,
    WORK_EXIT,
} work_command_t;

typedef struct {
    feishu_nav_t nav;
    feishu_chat_t chats[FEISHU_MAX_CHATS];
    feishu_message_t messages[FEISHU_MAX_MESSAGES];
    char reply[FEISHU_REPLY_MAX];
    char reply_uuid[41];
    char status[96];
    bool loading;
    bool ready;
    bool closing;
    bool cancel_recording;
    bool image_ready;
    unsigned image_version;
    volatile bool stop_recording;
    volatile unsigned elapsed_ms;
    int battery_percent;
} feishu_ui_state_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_title;
static lv_obj_t *s_page_info;
static lv_obj_t *s_battery;
static lv_obj_t *s_content;
static lv_obj_t *s_image;
static lv_obj_t *s_hint;
static lv_obj_t *s_header_rule;
#define FEISHU_VISIBLE_ROWS 4
static lv_obj_t *s_rows[FEISHU_VISIBLE_ROWS];
static lv_obj_t *s_row_avatar[FEISHU_VISIBLE_ROWS];
static lv_obj_t *s_row_avatar_text[FEISHU_VISIBLE_ROWS];
static lv_obj_t *s_row_text[FEISHU_VISIBLE_ROWS];
static lv_obj_t *s_row_subtext[FEISHU_VISIBLE_ROWS];
static lv_obj_t *s_row_mark[FEISHU_VISIBLE_ROWS];
static lv_timer_t *s_timer;
static QueueHandle_t s_work_queue;
static SemaphoreHandle_t s_state_lock;
static SemaphoreHandle_t s_worker_done;
static TaskHandle_t s_worker;
static feishu_ui_state_t s_state;
static feishu_api_session_t s_session;
static bool s_image_cache_mounted;

#define FEISHU_IMAGE_PATH "/feishu/current.jpg"
#define FEISHU_IMAGE_LVGL_PATH "S:current.jpg"
#define FEISHU_IMAGE_MAX_BYTES (384U * 1024U)
#define FEISHU_MESSAGE_ROWS 3
#define FEISHU_REFRESH_BATCH 1

static esp_err_t ensure_image_cache(void)
{
    if (s_image_cache_mounted) return ESP_OK;
    esp_vfs_spiffs_conf_t config = {
        .base_path = "/feishu",
        .partition_label = "feishu_img",
        .max_files = 2,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&config);
    if (err == ESP_OK) s_image_cache_mounted = true;
    return err;
}

static void make_reply_uuid(char output[41])
{
    snprintf(output, 41, "%08lx-%08lx-%08lx-%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random());
}

static void set_status(const char *text, bool loading)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    feishu_utf8_copy(s_state.status, sizeof(s_state.status), text);
    s_state.loading = loading;
    xSemaphoreGive(s_state_lock);
}

static uint64_t seen_time(const feishu_seen_t *seen, size_t count,
                          const char *chat_id)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(seen[i].chat_id, chat_id) == 0) {
            return seen[i].last_seen_time_ms;
        }
    }
    return 0;
}

static esp_err_t load_messages_for_chat(size_t chat_index, bool mark_seen)
{
    feishu_message_t *messages = calloc(FEISHU_MAX_MESSAGES,
                                        sizeof(*messages));
    char chat_id[FEISHU_CHAT_ID_MAX];
    size_t count = 0;
    char selected_id[FEISHU_MESSAGE_ID_MAX] = { 0 };
    bool followed_latest = true;
    esp_err_t err;

    if (messages == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (chat_index >= s_state.nav.chat_count) {
        xSemaphoreGive(s_state_lock);
        free(messages);
        return ESP_ERR_INVALID_ARG;
    }
    feishu_utf8_copy(chat_id, sizeof(chat_id), s_state.chats[chat_index].chat_id);
    if (s_state.nav.message_count > 0 &&
        s_state.nav.message_index < s_state.nav.message_count) {
        followed_latest = s_state.nav.message_index + 1 ==
                          s_state.nav.message_count;
        feishu_utf8_copy(selected_id, sizeof(selected_id),
                         s_state.messages[s_state.nav.message_index].message_id);
    }
    xSemaphoreGive(s_state_lock);

    err = feishu_api_list_messages(&s_session, chat_id, messages,
                                   FEISHU_MAX_MESSAGES, &count);
    if (err != ESP_OK) {
        free(messages);
        return err;
    }

    uint64_t latest = feishu_latest_visible_time(messages, count);
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    memcpy(s_state.messages, messages, sizeof(s_state.messages));
    s_state.nav.message_count = count;
    s_state.nav.message_index = count == 0 ? 0 : count - 1;
    if (!followed_latest && selected_id[0] != '\0') {
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(messages[i].message_id, selected_id) == 0) {
                s_state.nav.message_index = i;
                break;
            }
        }
    }
    s_state.chats[chat_index].latest_time_ms = latest;
    if (count > 0) {
        feishu_utf8_copy(s_state.chats[chat_index].summary,
                         sizeof(s_state.chats[chat_index].summary),
                         messages[count - 1].text);
    }
    if (mark_seen) s_state.chats[chat_index].unread_count = 0;
    xSemaphoreGive(s_state_lock);
    if (mark_seen && latest > 0) feishu_store_mark_seen(chat_id, latest);
    free(messages);
    return ESP_OK;
}

static esp_err_t refresh_conversations(void)
{
    static size_t refresh_cursor;
    feishu_chat_t *chats = calloc(FEISHU_MAX_CHATS, sizeof(*chats));
    feishu_seen_t *seen = calloc(FEISHU_MAX_CHATS, sizeof(*seen));
    feishu_message_t *messages = calloc(FEISHU_MAX_MESSAGES,
                                        sizeof(*messages));
    size_t chat_count = 0;
    size_t seen_count = 0;
    esp_err_t err;

    if (chats == NULL || seen == NULL || messages == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    err = feishu_api_list_chats(&s_session, chats, FEISHU_MAX_CHATS,
                                &chat_count);
    if (err != ESP_OK) goto done;
    feishu_store_load_seen(seen, FEISHU_MAX_CHATS, &seen_count);

    // Preserve metadata for chats outside this small refresh batch. Fetching
    // ten message pages back-to-back fragments the ESP32-C3 heap enough to
    // starve a later TLS record allocation.
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    for (size_t i = 0; i < chat_count; ++i) {
        for (size_t old = 0; old < s_state.nav.chat_count; ++old) {
            if (strcmp(chats[i].chat_id, s_state.chats[old].chat_id) == 0) {
                chats[i].latest_time_ms = s_state.chats[old].latest_time_ms;
                chats[i].unread_count = s_state.chats[old].unread_count;
                feishu_utf8_copy(chats[i].summary, sizeof(chats[i].summary),
                                 s_state.chats[old].summary);
                break;
            }
        }
    }
    xSemaphoreGive(s_state_lock);

    size_t batch = chat_count < FEISHU_REFRESH_BATCH ?
                   chat_count : FEISHU_REFRESH_BATCH;
    for (size_t offset = 0; offset < batch; ++offset) {
        size_t i = chat_count == 0 ? 0 : (refresh_cursor + offset) % chat_count;
        size_t message_count = 0;
        memset(messages, 0, FEISHU_MAX_MESSAGES * sizeof(*messages));
        err = feishu_api_list_messages(&s_session, chats[i].chat_id, messages,
                                       FEISHU_MAX_MESSAGES, &message_count);
        if (err != ESP_OK) continue;
        chats[i].latest_time_ms = feishu_latest_visible_time(messages, message_count);
        uint64_t baseline = seen_time(seen, seen_count, chats[i].chat_id);
        if (baseline == 0 && chats[i].latest_time_ms > 0) {
            // The first successful sync establishes a baseline. Historical
            // messages must not all appear as new on a newly bound device.
            feishu_store_mark_seen(chats[i].chat_id, chats[i].latest_time_ms);
            chats[i].unread_count = 0;
        } else {
            chats[i].unread_count = feishu_unread_count(
                messages, message_count, baseline);
        }
        if (message_count > 0) {
            feishu_utf8_copy(chats[i].summary, sizeof(chats[i].summary),
                             messages[message_count - 1].text);
        }
    }
    if (chat_count > 0) refresh_cursor = (refresh_cursor + batch) % chat_count;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    char selected_id[FEISHU_CHAT_ID_MAX] = { 0 };
    if (s_state.nav.chat_index < s_state.nav.chat_count) {
        feishu_utf8_copy(selected_id, sizeof(selected_id),
                         s_state.chats[s_state.nav.chat_index].chat_id);
    }
    memcpy(s_state.chats, chats, sizeof(s_state.chats));
    s_state.nav.chat_count = chat_count;
    s_state.nav.chat_index = 0;
    for (size_t i = 0; i < chat_count; ++i) {
        if (strcmp(chats[i].chat_id, selected_id) == 0) {
            s_state.nav.chat_index = i;
            break;
        }
    }
    xSemaphoreGive(s_state_lock);
    err = ESP_OK;

done:
    if (seen != NULL) memset(seen, 0, FEISHU_MAX_CHATS * sizeof(*seen));
    free(messages);
    free(seen);
    free(chats);
    return err;
}

static esp_err_t load_conversation_list(void)
{
    feishu_chat_t *chats = calloc(FEISHU_MAX_CHATS, sizeof(*chats));
    size_t count = 0;
    if (chats == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = feishu_api_list_chats(&s_session, chats,
                                          FEISHU_MAX_CHATS, &count);
    if (err != ESP_OK) {
        free(chats);
        return err;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    memcpy(s_state.chats, chats, sizeof(s_state.chats));
    s_state.nav.chat_count = count;
    s_state.nav.chat_index = 0;
    xSemaphoreGive(s_state_lock);
    free(chats);
    return ESP_OK;
}

static esp_err_t initialize_feishu(void)
{
    esp_err_t err;
    feishu_credentials_t *credentials = NULL;
    set_status("正在连接网络...", true);
    err = feishu_network_start(20000);
    if (err != ESP_OK) return err;
    set_status("正在连接飞书...", true);
    credentials = calloc(1, sizeof(*credentials));
    if (credentials == NULL) return ESP_ERR_NO_MEM;
    err = feishu_store_load_credentials(credentials);
    if (err == ESP_OK) err = feishu_api_authenticate(&s_session, credentials);
    memset(credentials, 0, sizeof(*credentials));
    free(credentials);
    if (err != ESP_OK) return err;
    set_status("正在加载会话...", true);
    // Show the conversation names as soon as the first small response arrives.
    // Summaries and unread dots are enriched in the background afterwards.
    return load_conversation_list();
}

static void worker_task(void *argument)
{
    (void)argument;
    work_command_t command;
    TickType_t last_chat_refresh = 0;

    for (;;) {
        if (xQueueReceive(s_work_queue, &command,
                          pdMS_TO_TICKS(8000)) != pdTRUE) {
            feishu_page_t page;
            size_t chat_index;
            bool ready;
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            int battery_percent = bsp_battery_soc();
            if (battery_percent >= 0) s_state.battery_percent = battery_percent;
            ready = s_state.ready && !s_state.loading;
            page = s_state.nav.page;
            chat_index = s_state.nav.chat_index;
            xSemaphoreGive(s_state_lock);
            if (ready && page == FEISHU_PAGE_MESSAGES) {
                if (load_messages_for_chat(chat_index, true) != ESP_OK) {
                    ESP_LOGW(TAG, "background message refresh failed");
                }
            } else if (ready && page == FEISHU_PAGE_CHATS &&
                       xTaskGetTickCount() - last_chat_refresh >=
                           pdMS_TO_TICKS(8000)) {
                if (refresh_conversations() != ESP_OK) {
                    ESP_LOGW(TAG, "background conversation refresh failed");
                }
                last_chat_refresh = xTaskGetTickCount();
            }
            continue;
        }
        esp_err_t err = ESP_OK;
        if (command == WORK_EXIT) break;
        if (command == WORK_INIT) {
            err = initialize_feishu();
            if (err != ESP_OK) ESP_LOGW(TAG, "initialization failed: %s", esp_err_to_name(err));
            if (err == ESP_ERR_INVALID_RESPONSE) {
                // Only an explicit Feishu auth rejection reaches this code;
                // transport/TLS failures preserve the existing binding.
                feishu_store_clear_credentials();
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_state.loading = false;
            s_state.ready = err == ESP_OK;
            feishu_utf8_copy(s_state.status, sizeof(s_state.status),
                             err == ESP_OK ? "" :
                             (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NVS_NOT_FOUND ?
                              "请先完成配网和绑定" : "暂时无法连接飞书，稍后重试"));
            xSemaphoreGive(s_state_lock);
            if (err == ESP_OK && refresh_conversations() != ESP_OK) {
                ESP_LOGW(TAG, "initial conversation enrichment failed");
            }
        } else if (command == WORK_LOAD_MESSAGES) {
            size_t chat_index;
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            chat_index = s_state.nav.chat_index;
            xSemaphoreGive(s_state_lock);
            err = load_messages_for_chat(chat_index, true);
            if (err != ESP_OK) ESP_LOGW(TAG, "message load failed: %s", esp_err_to_name(err));
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_state.loading = false;
            if (err != ESP_OK) {
                s_state.nav.page = FEISHU_PAGE_STATUS;
                feishu_utf8_copy(s_state.status, sizeof(s_state.status),
                                 "无法加载消息，请稍后重试");
            } else {
                s_state.status[0] = '\0';
            }
            xSemaphoreGive(s_state_lock);
        } else if (command == WORK_LOAD_IMAGE) {
            feishu_message_t message = { 0 };
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            if (s_state.nav.message_index < s_state.nav.message_count) {
                message = s_state.messages[s_state.nav.message_index];
            } else {
                err = ESP_ERR_INVALID_STATE;
            }
            xSemaphoreGive(s_state_lock);
            if (err == ESP_OK) err = ensure_image_cache();
            if (err == ESP_OK) {
                err = feishu_api_download_image(&s_session, &message,
                                                FEISHU_IMAGE_PATH,
                                                FEISHU_IMAGE_MAX_BYTES);
            }
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_state.loading = false;
            s_state.image_ready = err == ESP_OK;
            if (err == ESP_OK) {
                ++s_state.image_version;
                s_state.status[0] = '\0';
            } else {
                feishu_utf8_copy(s_state.status, sizeof(s_state.status),
                    err == ESP_ERR_NOT_SUPPORTED ?
                    "暂不支持此图片格式，请发送 JPG 图片" :
                    err == ESP_ERR_INVALID_SIZE ?
                    "图片过大，无法在设备上显示" :
                    "图片加载失败，请稍后重试");
            }
            xSemaphoreGive(s_state_lock);
        } else if (command == WORK_RECORD) {
            char recognition[FEISHU_REPLY_MAX] = { 0 };
            err = feishu_asr_record(&s_session, &s_state.stop_recording,
                                    &s_state.elapsed_ms, recognition,
                                    sizeof(recognition));
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            if (s_state.cancel_recording) {
                s_state.cancel_recording = false;
                s_state.loading = false;
                s_state.nav.page = FEISHU_PAGE_MESSAGES;
            } else if (err == ESP_OK && recognition[0] != '\0') {
                feishu_utf8_copy(s_state.reply, sizeof(s_state.reply), recognition);
                s_state.nav.page = FEISHU_PAGE_REVIEW;
            } else {
                s_state.loading = false;
                s_state.nav.page = FEISHU_PAGE_MESSAGES;
                feishu_utf8_copy(s_state.status, sizeof(s_state.status),
                                 err == ESP_OK ? "没有识别到语音，请重试" :
                                                 "语音识别失败，请重试");
            }
            xSemaphoreGive(s_state_lock);
        } else if (command == WORK_SEND) {
            char message_id[FEISHU_MESSAGE_ID_MAX];
            char chat_id[FEISHU_CHAT_ID_MAX];
            char reply[FEISHU_REPLY_MAX];
            char reply_uuid[41];
            size_t chat_index;
            bool reply_to_message;
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            size_t message_index = s_state.nav.message_index;
            chat_index = s_state.nav.chat_index;
            reply_to_message = s_state.nav.reply_to_message;
            if (chat_index >= s_state.nav.chat_count ||
                (reply_to_message && message_index >= s_state.nav.message_count)) {
                err = ESP_ERR_INVALID_STATE;
                message_id[0] = '\0';
                chat_id[0] = '\0';
                reply[0] = '\0';
                reply_uuid[0] = '\0';
            } else {
                if (reply_to_message) {
                    feishu_utf8_copy(message_id, sizeof(message_id),
                                     s_state.messages[message_index].message_id);
                } else {
                    message_id[0] = '\0';
                }
                feishu_utf8_copy(chat_id, sizeof(chat_id),
                                 s_state.chats[chat_index].chat_id);
                feishu_utf8_copy(reply, sizeof(reply), s_state.reply);
                feishu_utf8_copy(reply_uuid, sizeof(reply_uuid),
                                 s_state.reply_uuid);
            }
            xSemaphoreGive(s_state_lock);
            if (err == ESP_OK) {
                err = reply_to_message ?
                    feishu_api_reply_text(&s_session, message_id, reply,
                                          reply_uuid) :
                    feishu_api_send_text(&s_session, chat_id, reply,
                                         reply_uuid);
            }
            if (err != ESP_OK) ESP_LOGW(TAG, "send failed: %s", esp_err_to_name(err));
            if (err == ESP_OK) load_messages_for_chat(chat_index, true);
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_state.loading = false;
            s_state.nav.page = err == ESP_OK ? FEISHU_PAGE_MESSAGES :
                                               FEISHU_PAGE_REVIEW;
            feishu_utf8_copy(s_state.status, sizeof(s_state.status),
                             err == ESP_OK ?
                             (reply_to_message ? "机器人回复已发送" :
                                                 "机器人消息已发送") :
                             "发送失败，文字已保留");
            if (err == ESP_OK) {
                memset(s_state.reply, 0, sizeof(s_state.reply));
                memset(s_state.reply_uuid, 0, sizeof(s_state.reply_uuid));
            }
            xSemaphoreGive(s_state_lock);
            memset(reply, 0, sizeof(reply));
            memset(reply_uuid, 0, sizeof(reply_uuid));
        } else if (command == WORK_RESET_WIFI) {
            err = feishu_network_forget_wifi();
            ESP_LOGI(TAG, "Wi-Fi reset requested: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        } else if (command == WORK_UNBIND) {
            err = feishu_store_clear_credentials();
            ESP_LOGI(TAG, "Feishu unbind requested: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
    }
    s_worker = NULL;
    xSemaphoreGive(s_worker_done);
    vTaskDelete(NULL);
}

static void append_line(char *buffer, size_t capacity, const char *format, ...)
{
    size_t length = strlen(buffer);
    if (length >= capacity - 1) return;
    va_list args;
    va_start(args, format);
    vsnprintf(buffer + length, capacity - length, format, args);
    va_end(args);
}

static size_t visible_start(size_t selected, size_t count)
{
    if (count <= FEISHU_VISIBLE_ROWS) return 0;
    size_t start = selected >= FEISHU_VISIBLE_ROWS - 1 ?
                   selected - (FEISHU_VISIBLE_ROWS - 1) : 0;
    if (start + FEISHU_VISIBLE_ROWS > count) start = count - FEISHU_VISIBLE_ROWS;
    return start;
}

static size_t message_visible_start(size_t selected, size_t count)
{
    if (count <= FEISHU_MESSAGE_ROWS) return 0;
    size_t start = selected >= FEISHU_MESSAGE_ROWS - 1 ?
                   selected - (FEISHU_MESSAGE_ROWS - 1) : 0;
    if (start + FEISHU_MESSAGE_ROWS > count) start = count - FEISHU_MESSAGE_ROWS;
    return start;
}

static void show_list_rows(bool visible)
{
    if (visible) lv_obj_add_flag(s_content, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_content, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < FEISHU_VISIBLE_ROWS; ++i) {
        if (visible) lv_obj_remove_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void style_list_row(size_t slot, bool selected)
{
    // A restrained navy surface keeps each row readable; focus is expressed
    // only by the high-contrast yellow key-navigation outline.
    lv_obj_set_style_bg_opa(s_rows[slot],
                            selected ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_bg_color(s_rows[slot],
        lv_color_hex(selected ? 0x142B43 : 0x102238), 0);
    lv_obj_set_style_border_width(s_rows[slot], selected ? 2 : 0, 0);
    lv_obj_set_style_border_color(s_rows[slot], lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_radius(s_rows[slot], 10, 0);
    lv_obj_set_style_text_color(s_row_text[slot], lv_color_hex(UI_PAPER), 0);
    lv_obj_set_style_bg_color(s_row_mark[slot], lv_color_hex(UI_RED), 0);
}

static void first_utf8_glyph(char output[5], const char *text,
                             const char *fallback)
{
    const unsigned char *input = (const unsigned char *)text;
    size_t bytes = 1;
    if (input == NULL || input[0] == '\0') input = (const unsigned char *)fallback;
    if ((input[0] & 0xF0) == 0xF0) bytes = 4;
    else if ((input[0] & 0xE0) == 0xE0) bytes = 3;
    else if ((input[0] & 0xC0) == 0xC0) bytes = 2;
    memcpy(output, input, bytes);
    output[bytes] = '\0';
}

static void render_chat_rows(void)
{
    static const uint32_t avatar_colors[] = {
        0x1689E8, 0x7557D9, 0x20A779, 0xD18A18,
    };
    lv_label_set_text(s_title, "飞书 | 会话");
    lv_label_set_text_fmt(s_page_info, "%u/%u",
        s_state.nav.chat_count == 0 ? 0U : (unsigned)s_state.nav.chat_index + 1,
        (unsigned)s_state.nav.chat_count);
    size_t start = visible_start(s_state.nav.chat_index, s_state.nav.chat_count);
    for (size_t slot = 0; slot < FEISHU_VISIBLE_ROWS; ++slot) {
        lv_obj_set_pos(s_rows[slot], 10, 38 + (int)slot * 58);
        lv_obj_set_size(s_rows[slot], 220, 54);
        size_t index = start + slot;
        if (index >= s_state.nav.chat_count) {
            lv_obj_add_flag(s_rows[slot], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_rows[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_row_avatar[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_row_avatar[slot], 5, 7);
        lv_obj_set_size(s_row_avatar[slot], 40, 40);
        lv_obj_set_style_bg_color(s_row_avatar[slot],
            lv_color_hex(avatar_colors[index % 4]), 0);
        char avatar[5];
        first_utf8_glyph(avatar, s_state.chats[index].name,
                         s_state.chats[index].p2p ? "聊" : "群");
        lv_label_set_text(s_row_avatar_text[slot], avatar);
        lv_obj_set_pos(s_row_text[slot], 52, 17);
        lv_obj_set_size(s_row_text[slot], 142, 20);
        lv_obj_set_style_bg_opa(s_row_text[slot], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_row_text[slot], 0, 0);
        lv_obj_set_style_pad_all(s_row_text[slot], 0, 0);
        lv_obj_set_style_text_align(s_row_text[slot], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_add_flag(s_row_subtext[slot], LV_OBJ_FLAG_HIDDEN);
        char name[61];
        feishu_utf8_copy(name, sizeof(name), s_state.chats[index].name);
        lv_label_set_text(s_row_text[slot], name);
        lv_label_set_text(s_row_subtext[slot], "");
        lv_label_set_text(s_row_mark[slot], "");
        lv_obj_set_style_bg_opa(s_row_mark[slot],
            s_state.chats[index].unread_count ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        style_list_row(slot, index == s_state.nav.chat_index);
    }
}

static void render_message_rows(void)
{
    char chat_name[55];
    feishu_utf8_copy(chat_name, sizeof(chat_name),
                     s_state.chats[s_state.nav.chat_index].name);
    lv_label_set_text_fmt(s_title, "< %s", chat_name);
    lv_label_set_text(s_page_info, s_state.nav.composer_selected ? "发送" : "");
    size_t start = message_visible_start(s_state.nav.message_index,
                                         s_state.nav.message_count);
    for (size_t slot = 0; slot < FEISHU_MESSAGE_ROWS; ++slot) {
        lv_obj_set_pos(s_rows[slot], 10, 38 + (int)slot * 58);
        lv_obj_set_size(s_rows[slot], 220, 54);
        size_t index = start + slot;
        if (index >= s_state.nav.message_count) {
            lv_obj_add_flag(s_rows[slot], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_rows[slot], LV_OBJ_FLAG_HIDDEN);
        char message[91];
        feishu_utf8_copy(message, sizeof(message), s_state.messages[index].text);
        lv_label_set_text(s_row_text[slot], message);
        lv_obj_add_flag(s_row_subtext[slot], LV_OBJ_FLAG_HIDDEN);
        bool mine = s_state.messages[index].mine;
        if (mine) lv_obj_add_flag(s_row_avatar[slot], LV_OBJ_FLAG_HIDDEN);
        else {
            lv_obj_remove_flag(s_row_avatar[slot], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_row_avatar[slot], 4, 8);
            lv_obj_set_size(s_row_avatar[slot], 30, 30);
            lv_obj_set_style_bg_color(s_row_avatar[slot],
                                      lv_color_hex(UI_SKY_DARK), 0);
            char avatar[5];
            first_utf8_glyph(avatar, s_state.messages[index].sender_name, "他");
            lv_label_set_text(s_row_avatar_text[slot], avatar);
        }
        lv_obj_set_pos(s_row_text[slot], mine ? 44 : 40, 4);
        lv_obj_set_size(s_row_text[slot], mine ? 168 : 172, 46);
        lv_obj_set_style_bg_opa(s_row_text[slot], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_row_text[slot],
            lv_color_hex(mine ? 0x2A8B55 : 0x24384E), 0);
        lv_obj_set_style_text_color(s_row_text[slot],
            lv_color_hex(UI_PAPER), 0);
        lv_obj_set_style_text_align(s_row_text[slot],
            mine ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_pad_all(s_row_text[slot], 5, 0);
        lv_obj_set_style_radius(s_row_text[slot], 8, 0);
        lv_obj_set_style_border_width(s_row_text[slot],
                                      index == s_state.nav.message_index ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_row_text[slot],
                                      lv_color_hex(UI_YELLOW), 0);
        lv_label_set_text(s_row_mark[slot], "");
        lv_obj_set_style_bg_opa(s_row_mark[slot], LV_OPA_TRANSP, 0);
        // The bubble itself already has the yellow focus outline. Keep the
        // surrounding message row transparent to avoid a double card.
        lv_obj_set_style_bg_opa(s_rows[slot], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_rows[slot], 0, 0);
        // style_list_row sets the default text color; restore bubble contrast.
        lv_obj_set_style_text_color(s_row_text[slot], lv_color_hex(UI_PAPER), 0);
    }
    size_t slot = FEISHU_MESSAGE_ROWS;
    lv_obj_remove_flag(s_rows[slot], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_row_avatar[slot], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_row_subtext[slot], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_rows[slot], 18, 222);
    lv_obj_set_size(s_rows[slot], 204, 42);
    lv_obj_set_style_bg_opa(s_rows[slot], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_rows[slot], lv_color_hex(0x102B44), 0);
    lv_obj_set_style_border_width(s_rows[slot],
                                  s_state.nav.composer_selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(s_rows[slot], lv_color_hex(
        s_state.nav.composer_selected ? UI_YELLOW : 0x34506A), 0);
    lv_obj_set_style_radius(s_rows[slot], 12, 0);
    lv_obj_set_pos(s_row_text[slot], 8, 5);
    lv_obj_set_size(s_row_text[slot], 188, 30);
    lv_obj_set_style_bg_opa(s_row_text[slot], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_row_text[slot], 0, 0);
    lv_obj_set_style_pad_all(s_row_text[slot], 2, 0);
    lv_obj_set_style_text_align(s_row_text[slot], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_row_text[slot], lv_color_hex(UI_PAPER), 0);
    lv_label_set_text(s_row_text[slot], "按确定  发消息");
    lv_label_set_text(s_row_mark[slot], "");
    lv_obj_set_style_bg_opa(s_row_mark[slot], LV_OPA_TRANSP, 0);
}

static void render(lv_timer_t *timer)
{
    (void)timer;
    char body[1200] = { 0 };
    char hint[96] = { 0 };
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);

    if (!s_state.ready) {
        show_list_rows(false);
        lv_label_set_text(s_title, "飞书");
        lv_label_set_text(s_page_info, "");
        snprintf(body, sizeof(body), "\n\n%s", s_state.status);
        snprintf(hint, sizeof(hint), "%s", s_state.loading ? "请稍候" : "长按确定：返回");
    } else if (s_state.loading) {
        show_list_rows(false);
        lv_label_set_text(s_title, "飞书");
        lv_label_set_text(s_page_info, "");
        snprintf(body, sizeof(body), "\n\n%s", s_state.status);
        snprintf(hint, sizeof(hint), "请稍候");
    } else {
        // Default the counter empty; only the list pages fill it. Battery has
        // its own label now, so nothing masks a stale value every frame.
        lv_label_set_text(s_page_info, "");
        // Prose/status screens read better centered; the settings list needs
        // left alignment for its "> " markers and is overridden below.
        lv_obj_set_style_text_align(s_content, LV_TEXT_ALIGN_CENTER, 0);
        switch (s_state.nav.page) {
        case FEISHU_PAGE_CHATS:
            show_list_rows(true);
            render_chat_rows();
            snprintf(hint, sizeof(hint), "上下选择  确定进入  长按设置");
            break;
        case FEISHU_PAGE_MESSAGES:
            show_list_rows(true);
            render_message_rows();
            snprintf(hint, sizeof(hint), "上下选择  确定进入  长按返回");
            break;
        case FEISHU_PAGE_MESSAGE_DETAIL:
            show_list_rows(false);
            lv_label_set_text(s_title, "消息详情");
            lv_label_set_text(s_page_info, "");
            if (s_state.nav.message_index < s_state.nav.message_count) {
                const feishu_message_t *message =
                    &s_state.messages[s_state.nav.message_index];
                char sender[49];
                char detail[401];
                feishu_utf8_copy(sender, sizeof(sender),
                    message->mine ? "我" : message->sender_name);
                feishu_utf8_copy(detail, sizeof(detail), message->text);
                snprintf(body, sizeof(body), "\n%s\n\n%s", sender, detail);
                snprintf(hint, sizeof(hint), message->type == FEISHU_MESSAGE_IMAGE ?
                         "确定查看图片  长按返回" :
                         "确定回复这条  长按返回");
            }
            break;
        case FEISHU_PAGE_REPLY_READY:
            show_list_rows(false);
            lv_label_set_text(s_title,
                s_state.nav.reply_to_message ? "回复消息" : "发送消息");
            lv_label_set_text(s_page_info, "");
            if (s_state.nav.reply_to_message &&
                s_state.nav.message_index < s_state.nav.message_count) {
            char sender[49];
            char target[241];
            feishu_utf8_copy(sender, sizeof(sender),
                s_state.messages[s_state.nav.message_index].mine ? "我" :
                s_state.messages[s_state.nav.message_index].sender_name);
            feishu_utf8_copy(target, sizeof(target),
                             s_state.messages[s_state.nav.message_index].text);
            snprintf(body, sizeof(body),
                     "\n准备回复\n\n%s：\n%s\n\n按确定后开始录音",
                     sender, target);
            } else {
                snprintf(body, sizeof(body),
                         "\n向当前会话发送消息\n\n按确定后开始录音\n识别文字后可确认发送");
            }
            snprintf(hint, sizeof(hint), "确定开始录音  长按返回");
            break;
        case FEISHU_PAGE_IMAGE:
            show_list_rows(false);
            lv_label_set_text(s_title, "图片消息");
            lv_label_set_text(s_page_info, "");
            if (s_state.image_ready) {
                static unsigned displayed_version;
                lv_obj_add_flag(s_content, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_image, LV_OBJ_FLAG_HIDDEN);
                if (displayed_version != s_state.image_version) {
                    lv_image_set_src(s_image, FEISHU_IMAGE_LVGL_PATH);
                    displayed_version = s_state.image_version;
                }
                snprintf(hint, sizeof(hint), "确定回复这条  长按返回");
            } else {
                snprintf(body, sizeof(body), "\n\n%s", s_state.status);
                snprintf(hint, sizeof(hint), "长按返回");
            }
            break;
        case FEISHU_PAGE_RECORDING:
            show_list_rows(false);
            lv_label_set_text(s_title,
                s_state.nav.reply_to_message ? "回复消息" : "发送消息");
            lv_label_set_text(s_page_info, "");
            if (s_state.nav.reply_to_message &&
                s_state.nav.message_index < s_state.nav.message_count) {
            char target[121];
            feishu_utf8_copy(target, sizeof(target),
                             s_state.messages[s_state.nav.message_index].text);
            snprintf(body, sizeof(body), "\n正在录音\n\n回复：%s\n\n%u.%u 秒\n\n请说话...",
                     target,
                     s_state.elapsed_ms / 1000, (s_state.elapsed_ms % 1000) / 100);
            } else {
                snprintf(body, sizeof(body), "\n正在录音\n\n%u.%u 秒\n\n请说话...",
                         s_state.elapsed_ms / 1000,
                         (s_state.elapsed_ms % 1000) / 100);
            }
            snprintf(hint, sizeof(hint), "确定停止  长按取消");
            break;
        case FEISHU_PAGE_TRANSCRIBING:
            show_list_rows(false);
            snprintf(body, sizeof(body), "\n\n正在识别语音...\n\n请稍候");
            snprintf(hint, sizeof(hint), "长按取消");
            break;
        case FEISHU_PAGE_REVIEW:
            show_list_rows(false);
            snprintf(body, sizeof(body), "%s\n\n%s%s%s",
                     s_state.nav.reply_to_message ? "确认回复" : "确认发送",
                     s_state.status[0] ? s_state.status : "",
                     s_state.status[0] ? "\n\n" : "", s_state.reply);
            snprintf(hint, sizeof(hint), "确定发送  上键重录  下键取消");
            break;
        case FEISHU_PAGE_STATUS:
            show_list_rows(false);
            snprintf(body, sizeof(body), "\n\n%s", s_state.status);
            snprintf(hint, sizeof(hint), "长按确定：返回消息");
            break;
        case FEISHU_PAGE_SETTINGS:
            show_list_rows(false);
            lv_obj_set_style_text_align(s_content, LV_TEXT_ALIGN_LEFT, 0);
            append_line(body, sizeof(body), "设置\n\n");
            append_line(body, sizeof(body), "%s 重新配置 Wi-Fi\n\n",
                        s_state.nav.settings_index == 0 ? ">" : " ");
            append_line(body, sizeof(body), "%s 解绑飞书账号\n\n",
                        s_state.nav.settings_index == 1 ? ">" : " ");
            append_line(body, sizeof(body), "%s 返回会话\n",
                        s_state.nav.settings_index == 2 ? ">" : " ");
            snprintf(hint, sizeof(hint), "上下选择  确定  长按返回");
            break;
        case FEISHU_PAGE_CONFIRM:
            show_list_rows(false);
            snprintf(body, sizeof(body), "\n确认操作\n\n%s\n\n此操作完成后设备会重启",
                     s_state.nav.confirm_action == 1 ?
                     "重新配置 Wi-Fi？\n飞书绑定会保留" :
                     "解绑飞书账号？\nWi-Fi 配置会保留");
            snprintf(hint, sizeof(hint), "确定继续  上/下取消");
            break;
        default:
            break;
        }
    }
    char battery[8];
    if (s_state.battery_percent >= 0) {
        unsigned percent = (unsigned)(s_state.battery_percent > 100 ?
                                      100 : s_state.battery_percent);
        snprintf(battery, sizeof(battery), "%u%%", percent);
    } else {
        snprintf(battery, sizeof(battery), "--%%");
    }
    lv_label_set_text(s_battery, battery);
    xSemaphoreGive(s_state_lock);
    if (!lv_obj_has_flag(s_content, LV_OBJ_FLAG_HIDDEN)) {
        lv_label_set_text(s_content, body);
    }
    lv_label_set_text(s_hint, hint);
}

static void enqueue(work_command_t command)
{
    if (s_work_queue != NULL) xQueueSend(s_work_queue, &command, 0);
}

static void handle_navigation(feishu_nav_event_t event)
{
    feishu_nav_action_t action;
    feishu_page_t previous_page;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (!s_state.ready || s_state.loading) {
        xSemaphoreGive(s_state_lock);
        return;
    }
    previous_page = s_state.nav.page;
    if (s_state.nav.page == FEISHU_PAGE_MESSAGE_DETAIL &&
        event == FEISHU_NAV_OK &&
        s_state.nav.message_index < s_state.nav.message_count &&
        s_state.messages[s_state.nav.message_index].type == FEISHU_MESSAGE_IMAGE) {
        s_state.nav.page = FEISHU_PAGE_IMAGE;
        s_state.loading = true;
        s_state.image_ready = false;
        feishu_utf8_copy(s_state.status, sizeof(s_state.status), "正在加载图片...");
        action = FEISHU_NAV_LOAD_IMAGE;
    } else {
        action = feishu_nav_handle(&s_state.nav, event);
    }
    if (action == FEISHU_NAV_LOAD_MESSAGES) {
        s_state.loading = true;
        feishu_utf8_copy(s_state.status, sizeof(s_state.status), "正在加载消息...");
    } else if (action == FEISHU_NAV_START_RECORDING ||
               action == FEISHU_NAV_RETRY_RECORDING) {
        s_state.stop_recording = false;
        s_state.cancel_recording = false;
        s_state.elapsed_ms = 0;
        s_state.status[0] = '\0';
        memset(s_state.reply_uuid, 0, sizeof(s_state.reply_uuid));
    } else if (action == FEISHU_NAV_STOP_RECORDING) {
        s_state.stop_recording = true;
    } else if (action == FEISHU_NAV_CANCEL_REPLY) {
        s_state.stop_recording = true;
        s_state.cancel_recording = true;
        if (previous_page == FEISHU_PAGE_RECORDING ||
            previous_page == FEISHU_PAGE_TRANSCRIBING) {
            s_state.loading = true;
            feishu_utf8_copy(s_state.status, sizeof(s_state.status),
                             "正在停止录音...");
        }
        memset(s_state.reply, 0, sizeof(s_state.reply));
    } else if (action == FEISHU_NAV_SEND_REPLY) {
        if (s_state.reply_uuid[0] == '\0') make_reply_uuid(s_state.reply_uuid);
        s_state.loading = true;
        feishu_utf8_copy(s_state.status, sizeof(s_state.status),
                         s_state.nav.reply_to_message ?
                         "正在发送回复..." : "正在发送消息...");
    }
    xSemaphoreGive(s_state_lock);

    if (action == FEISHU_NAV_LOAD_MESSAGES) enqueue(WORK_LOAD_MESSAGES);
    if (action == FEISHU_NAV_LOAD_IMAGE) enqueue(WORK_LOAD_IMAGE);
    if (action == FEISHU_NAV_START_RECORDING || action == FEISHU_NAV_RETRY_RECORDING) enqueue(WORK_RECORD);
    if (action == FEISHU_NAV_SEND_REPLY) enqueue(WORK_SEND);
    if (action == FEISHU_NAV_RESET_WIFI) enqueue(WORK_RESET_WIFI);
    if (action == FEISHU_NAV_UNBIND) enqueue(WORK_UNBIND);
}

void demo_feishu_enter(void)
{
    memset(&s_state, 0, sizeof(s_state));
    memset(&s_session, 0, sizeof(s_session));
    s_state.nav.page = FEISHU_PAGE_CHATS;
    s_state.battery_percent = -1;
    feishu_utf8_copy(s_state.status, sizeof(s_state.status), "正在启动...");
    s_state.loading = true;

    s_state_lock = xSemaphoreCreateMutex();
    s_worker_done = xSemaphoreCreateBinary();
    s_work_queue = xQueueCreate(4, sizeof(work_command_t));

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    s_title = lv_label_create(s_scr);
    lv_label_set_text(s_title, "飞书");
    lv_obj_set_width(s_title, 184);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_title, &lv_font_ai_passport_14, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_PAPER), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 12, 10);
    s_battery = lv_label_create(s_scr);
    lv_obj_set_width(s_battery, 40);
    lv_obj_set_style_text_font(s_battery, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_battery, lv_color_hex(UI_MUTED), 0);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -10, 10);
    s_page_info = lv_label_create(s_scr);
    lv_obj_set_width(s_page_info, 44);
    lv_obj_set_style_text_font(s_page_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_page_info, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_page_info, lv_color_hex(UI_MUTED), 0);
    lv_obj_align(s_page_info, LV_ALIGN_TOP_RIGHT, -56, 10);
    s_header_rule = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_header_rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_header_rule, 10, 34);
    lv_obj_set_size(s_header_rule, 220, 1);
    lv_obj_set_style_border_width(s_header_rule, 0, 0);
    lv_obj_set_style_pad_all(s_header_rule, 0, 0);
    lv_obj_set_style_bg_color(s_header_rule, lv_color_hex(0x34506A), 0);
    lv_obj_set_style_bg_opa(s_header_rule, LV_OPA_60, 0);
    s_content = lv_label_create(s_scr);
    lv_obj_set_size(s_content, 216, 244);
    lv_obj_set_pos(s_content, 12, 38);
    lv_obj_set_style_text_font(s_content, &lv_font_ai_passport_14, 0);
    lv_obj_set_style_text_color(s_content, lv_color_hex(UI_PAPER), 0);
    lv_label_set_long_mode(s_content, LV_LABEL_LONG_WRAP);
    s_image = lv_image_create(s_scr);
    lv_obj_set_pos(s_image, 12, 40);
    lv_obj_set_size(s_image, 216, 226);
    lv_image_set_inner_align(s_image, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < FEISHU_VISIBLE_ROWS; ++i) {
        s_rows[i] = lv_obj_create(s_scr);
        lv_obj_remove_flag(s_rows[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_rows[i], 10, 38 + (int)i * 58);
        lv_obj_set_size(s_rows[i], 220, 54);
        lv_obj_set_style_pad_all(s_rows[i], 0, 0);
        lv_obj_set_style_radius(s_rows[i], 2, 0);
        s_row_avatar[i] = lv_obj_create(s_rows[i]);
        lv_obj_remove_flag(s_row_avatar[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_row_avatar[i], 5, 7);
        lv_obj_set_size(s_row_avatar[i], 40, 40);
        lv_obj_set_style_radius(s_row_avatar[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_row_avatar[i], 0, 0);
        lv_obj_set_style_pad_all(s_row_avatar[i], 0, 0);
        s_row_avatar_text[i] = lv_label_create(s_row_avatar[i]);
        lv_obj_set_style_text_font(s_row_avatar_text[i],
                                   &lv_font_ai_passport_14, 0);
        lv_obj_set_style_text_color(s_row_avatar_text[i],
                                    lv_color_hex(UI_PAPER), 0);
        lv_obj_center(s_row_avatar_text[i]);
        s_row_text[i] = lv_label_create(s_rows[i]);
        lv_obj_set_pos(s_row_text[i], 8, 5);
        lv_obj_set_width(s_row_text[i], 184);
        lv_label_set_long_mode(s_row_text[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(s_row_text[i], &lv_font_ai_passport_14, 0);
        lv_obj_set_style_text_line_space(s_row_text[i], 3, 0);
        s_row_subtext[i] = lv_label_create(s_rows[i]);
        lv_obj_set_style_text_font(s_row_subtext[i], &lv_font_ai_passport_14, 0);
        lv_obj_set_style_text_color(s_row_subtext[i], lv_color_hex(0x94AFC5), 0);
        lv_label_set_long_mode(s_row_subtext[i], LV_LABEL_LONG_DOT);
        lv_obj_add_flag(s_row_subtext[i], LV_OBJ_FLAG_HIDDEN);
        s_row_mark[i] = lv_label_create(s_rows[i]);
        lv_obj_set_size(s_row_mark[i], 9, 9);
        lv_obj_align(s_row_mark[i], LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_set_style_radius(s_row_mark[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_row_mark[i], lv_color_hex(UI_RED), 0);
        lv_obj_set_style_bg_opa(s_row_mark[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_font(s_row_mark[i], &lv_font_ai_passport_14, 0);
        lv_obj_set_style_text_align(s_row_mark[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_hint = lv_label_create(s_scr);
    lv_obj_set_width(s_hint, 232);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_text_font(s_hint, &lv_font_ai_passport_14, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_hint, lv_color_hex(0x10243A), 0);
    lv_obj_set_style_bg_opa(s_hint, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_hint, 9, 0);
    lv_obj_set_style_pad_ver(s_hint, 5, 0);
    lv_screen_load(s_scr);

    if (s_state_lock == NULL || s_worker_done == NULL || s_work_queue == NULL) {
        s_state.loading = false;
        feishu_utf8_copy(s_state.status, sizeof(s_state.status), "Not enough memory");
        lv_label_set_text(s_content, "\n\n内存不足，请重启设备");
        lv_label_set_text(s_hint, "长按确定：返回");
        return;
    }

    s_timer = lv_timer_create(render, 100, NULL);
    // Message parsing and HTTPS each have non-trivial fixed stack peaks. Image
    // resource keys also enlarge the bounded message arrays kept on this task.
    // Message parsing needs more than the original 10 KiB stack, while a
    // 16 KiB stack leaves too little contiguous heap for Feishu's unusually
    // long OAuth Authorization header plus TLS buffers on an ESP32-C3.
    if (xTaskCreate(worker_task, "feishu", 12288, NULL, 4, &s_worker) != pdPASS) {
        s_state.loading = false;
        feishu_utf8_copy(s_state.status, sizeof(s_state.status), "内存不足，请重启设备");
        return;
    }
    enqueue(WORK_INIT);
}

void demo_feishu_exit(void)
{
    if (s_worker != NULL) {
        enqueue(WORK_EXIT);
        xSemaphoreTake(s_worker_done, pdMS_TO_TICKS(2000));
    }
    feishu_network_stop();
    if (s_timer != NULL) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr != NULL) { lv_obj_delete(s_scr); s_scr = NULL; }
    if (s_image_cache_mounted) {
        esp_vfs_spiffs_unregister("feishu_img");
        s_image_cache_mounted = false;
    }
    if (s_work_queue != NULL) { vQueueDelete(s_work_queue); s_work_queue = NULL; }
    if (s_worker_done != NULL) { vSemaphoreDelete(s_worker_done); s_worker_done = NULL; }
    if (s_state_lock != NULL) { vSemaphoreDelete(s_state_lock); s_state_lock = NULL; }
    memset(&s_session, 0, sizeof(s_session));
}

void demo_feishu_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP) handle_navigation(FEISHU_NAV_UP);
    if (btn == BSP_BTN_DOWN) handle_navigation(FEISHU_NAV_DOWN);
    if (btn == BSP_BTN_OK) handle_navigation(FEISHU_NAV_OK);
}

bool demo_feishu_back(void)
{
    bool consume = true;
    bool ready;
    bool loading;
    if (s_state_lock == NULL) return false;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    ready = s_state.ready;
    loading = s_state.loading;
    if (!ready) {
        consume = loading;
    } else if (loading) {
        consume = true;
    }
    xSemaphoreGive(s_state_lock);
    if (consume && ready && !loading) {
        handle_navigation(FEISHU_NAV_BACK);
    }
    return consume;
}
