#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FEISHU_MAX_CHATS 10
#define FEISHU_MAX_MESSAGES 8
#define FEISHU_CHAT_ID_MAX 64
#define FEISHU_MESSAGE_ID_MAX 64
#define FEISHU_RESOURCE_KEY_MAX 128
#define FEISHU_CHAT_NAME_MAX 64
#define FEISHU_SENDER_NAME_MAX 64
#define FEISHU_TEXT_MAX 256
#define FEISHU_REPLY_MAX 512

typedef enum {
    FEISHU_MESSAGE_TEXT = 0,
    FEISHU_MESSAGE_AUDIO,
    FEISHU_MESSAGE_IMAGE,
    FEISHU_MESSAGE_FILE,
    FEISHU_MESSAGE_OTHER,
} feishu_message_type_t;

typedef struct {
    char chat_id[FEISHU_CHAT_ID_MAX];
    char name[FEISHU_CHAT_NAME_MAX];
    char summary[FEISHU_TEXT_MAX];
    uint64_t latest_time_ms;
    uint32_t unread_count;
    bool p2p;
} feishu_chat_t;

typedef struct {
    char message_id[FEISHU_MESSAGE_ID_MAX];
    char resource_key[FEISHU_RESOURCE_KEY_MAX];
    char sender_id[FEISHU_CHAT_ID_MAX];
    char sender_name[FEISHU_SENDER_NAME_MAX];
    char text[FEISHU_TEXT_MAX];
    uint64_t create_time_ms;
    feishu_message_type_t type;
    bool mine;
    bool deleted;
} feishu_message_t;

typedef struct {
    char chat_id[FEISHU_CHAT_ID_MAX];
    uint64_t last_seen_time_ms;
} feishu_seen_t;

typedef enum {
    FEISHU_PAGE_CHATS = 0,
    FEISHU_PAGE_MESSAGES,
    FEISHU_PAGE_MESSAGE_DETAIL,
    FEISHU_PAGE_REPLY_READY,
    FEISHU_PAGE_RECORDING,
    FEISHU_PAGE_TRANSCRIBING,
    FEISHU_PAGE_REVIEW,
    FEISHU_PAGE_STATUS,
    FEISHU_PAGE_IMAGE,
    FEISHU_PAGE_SETTINGS,
    FEISHU_PAGE_CONFIRM,
} feishu_page_t;

typedef enum {
    FEISHU_NAV_UP = 0,
    FEISHU_NAV_DOWN,
    FEISHU_NAV_OK,
    FEISHU_NAV_BACK,
} feishu_nav_event_t;

typedef enum {
    FEISHU_NAV_NONE = 0,
    FEISHU_NAV_LOAD_MESSAGES,
    FEISHU_NAV_LOAD_IMAGE,
    FEISHU_NAV_START_RECORDING,
    FEISHU_NAV_STOP_RECORDING,
    FEISHU_NAV_SEND_REPLY,
    FEISHU_NAV_RETRY_RECORDING,
    FEISHU_NAV_CANCEL_REPLY,
    FEISHU_NAV_RESET_WIFI,
    FEISHU_NAV_UNBIND,
    FEISHU_NAV_EXIT,
} feishu_nav_action_t;

typedef struct {
    feishu_page_t page;
    size_t chat_index;
    size_t message_index;
    size_t chat_count;
    size_t message_count;
    bool composer_selected;
    bool reply_to_message;
    size_t settings_index;
    size_t confirm_action;
} feishu_nav_t;

uint32_t feishu_unread_count(const feishu_message_t *messages, size_t count,
                             uint64_t last_seen_time_ms);
uint64_t feishu_latest_visible_time(const feishu_message_t *messages,
                                    size_t count);
size_t feishu_utf8_copy(char *dst, size_t dst_size, const char *src);
feishu_nav_action_t feishu_nav_handle(feishu_nav_t *nav,
                                      feishu_nav_event_t event);
