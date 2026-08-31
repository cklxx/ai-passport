#include "feishu_model.h"

#include <string.h>

uint32_t feishu_unread_count(const feishu_message_t *messages, size_t count,
                             uint64_t last_seen_time_ms)
{
    uint32_t unread = 0;

    if (messages == NULL) return 0;
    for (size_t i = 0; i < count; ++i) {
        const feishu_message_t *message = &messages[i];
        if (!message->deleted && !message->mine &&
            message->create_time_ms > last_seen_time_ms) {
            ++unread;
        }
    }
    return unread;
}

uint64_t feishu_latest_visible_time(const feishu_message_t *messages,
                                    size_t count)
{
    uint64_t latest = 0;

    if (messages == NULL) return 0;
    for (size_t i = 0; i < count; ++i) {
        if (!messages[i].deleted && messages[i].create_time_ms > latest) {
            latest = messages[i].create_time_ms;
        }
    }
    return latest;
}

size_t feishu_utf8_copy(char *dst, size_t dst_size, const char *src)
{
    size_t length;

    if (dst == NULL || dst_size == 0) return 0;
    if (src == NULL) {
        dst[0] = '\0';
        return 0;
    }

    length = strlen(src);
    if (length >= dst_size) length = dst_size - 1;
    while (length > 0 && ((unsigned char)src[length] & 0xC0U) == 0x80U) {
        --length;
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
    return length;
}

static void move_selection(size_t *selection, size_t count, int direction)
{
    if (selection == NULL || count == 0) return;
    if (direction < 0) {
        if (*selection > 0) --*selection;
    } else if (*selection + 1 < count) {
        ++*selection;
    }
}

feishu_nav_action_t feishu_nav_handle(feishu_nav_t *nav,
                                      feishu_nav_event_t event)
{
    if (nav == NULL) return FEISHU_NAV_NONE;

    switch (nav->page) {
    case FEISHU_PAGE_CHATS:
        if (event == FEISHU_NAV_UP) move_selection(&nav->chat_index, nav->chat_count, -1);
        if (event == FEISHU_NAV_DOWN) move_selection(&nav->chat_index, nav->chat_count, 1);
        if (event == FEISHU_NAV_OK && nav->chat_count > 0) {
            nav->page = FEISHU_PAGE_MESSAGES;
            nav->message_index = 0;
            nav->composer_selected = true;
            return FEISHU_NAV_LOAD_MESSAGES;
        }
        if (event == FEISHU_NAV_BACK) {
            nav->page = FEISHU_PAGE_SETTINGS;
            nav->settings_index = 0;
        }
        break;
    case FEISHU_PAGE_MESSAGES:
        if (event == FEISHU_NAV_UP) {
            if (nav->composer_selected && nav->message_count > 0) {
                nav->composer_selected = false;
                nav->message_index = nav->message_count - 1;
            } else {
                move_selection(&nav->message_index, nav->message_count, -1);
            }
        }
        if (event == FEISHU_NAV_DOWN) {
            if (!nav->composer_selected && nav->message_count > 0 &&
                nav->message_index + 1 == nav->message_count) {
                nav->composer_selected = true;
            } else if (!nav->composer_selected) {
                move_selection(&nav->message_index, nav->message_count, 1);
            }
        }
        if (event == FEISHU_NAV_OK && nav->composer_selected) {
            nav->reply_to_message = false;
            nav->page = FEISHU_PAGE_REPLY_READY;
        } else if (event == FEISHU_NAV_OK && nav->message_count > 0) {
            nav->page = FEISHU_PAGE_MESSAGE_DETAIL;
        }
        if (event == FEISHU_NAV_BACK) nav->page = FEISHU_PAGE_CHATS;
        break;
    case FEISHU_PAGE_MESSAGE_DETAIL:
        if (event == FEISHU_NAV_OK) {
            nav->reply_to_message = true;
            nav->page = FEISHU_PAGE_REPLY_READY;
        }
        if (event == FEISHU_NAV_BACK) nav->page = FEISHU_PAGE_MESSAGES;
        break;
    case FEISHU_PAGE_REPLY_READY:
        if (event == FEISHU_NAV_OK) {
            nav->page = FEISHU_PAGE_RECORDING;
            return FEISHU_NAV_START_RECORDING;
        }
        if (event == FEISHU_NAV_BACK) nav->page = FEISHU_PAGE_MESSAGES;
        break;
    case FEISHU_PAGE_RECORDING:
        if (event == FEISHU_NAV_OK) {
            nav->page = FEISHU_PAGE_TRANSCRIBING;
            return FEISHU_NAV_STOP_RECORDING;
        }
        if (event == FEISHU_NAV_BACK) {
            nav->page = FEISHU_PAGE_MESSAGES;
            return FEISHU_NAV_CANCEL_REPLY;
        }
        break;
    case FEISHU_PAGE_TRANSCRIBING:
        if (event == FEISHU_NAV_BACK) {
            nav->page = FEISHU_PAGE_MESSAGES;
            return FEISHU_NAV_CANCEL_REPLY;
        }
        break;
    case FEISHU_PAGE_REVIEW:
        if (event == FEISHU_NAV_OK) return FEISHU_NAV_SEND_REPLY;
        if (event == FEISHU_NAV_UP) {
            nav->page = FEISHU_PAGE_RECORDING;
            return FEISHU_NAV_RETRY_RECORDING;
        }
        if (event == FEISHU_NAV_DOWN || event == FEISHU_NAV_BACK) {
            nav->page = FEISHU_PAGE_MESSAGES;
            return FEISHU_NAV_CANCEL_REPLY;
        }
        break;
    case FEISHU_PAGE_STATUS:
        if (event == FEISHU_NAV_BACK) nav->page = FEISHU_PAGE_MESSAGES;
        break;
    case FEISHU_PAGE_IMAGE:
        if (event == FEISHU_NAV_OK) {
            nav->reply_to_message = true;
            nav->page = FEISHU_PAGE_REPLY_READY;
        }
        if (event == FEISHU_NAV_BACK) nav->page = FEISHU_PAGE_MESSAGE_DETAIL;
        break;
    case FEISHU_PAGE_SETTINGS:
        if (event == FEISHU_NAV_UP) move_selection(&nav->settings_index, 3, -1);
        if (event == FEISHU_NAV_DOWN) move_selection(&nav->settings_index, 3, 1);
        if (event == FEISHU_NAV_BACK) nav->page = FEISHU_PAGE_CHATS;
        if (event == FEISHU_NAV_OK) {
            if (nav->settings_index < 2) {
                nav->confirm_action = nav->settings_index + 1;
                nav->page = FEISHU_PAGE_CONFIRM;
            } else {
                nav->page = FEISHU_PAGE_CHATS;
            }
        }
        break;
    case FEISHU_PAGE_CONFIRM:
        if (event == FEISHU_NAV_OK) {
            return nav->confirm_action == 1 ? FEISHU_NAV_RESET_WIFI :
                                              FEISHU_NAV_UNBIND;
        }
        if (event == FEISHU_NAV_UP || event == FEISHU_NAV_DOWN ||
            event == FEISHU_NAV_BACK) {
            nav->confirm_action = 0;
            nav->page = FEISHU_PAGE_SETTINGS;
        }
        break;
    default:
        break;
    }
    return FEISHU_NAV_NONE;
}
