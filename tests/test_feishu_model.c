#include "feishu_model.h"

#include <assert.h>
#include <string.h>

static void test_unread(void)
{
    feishu_message_t messages[] = {
        { .create_time_ms = 300, .mine = false },
        { .create_time_ms = 250, .mine = true },
        { .create_time_ms = 200, .mine = false, .deleted = true },
        { .create_time_ms = 100, .mine = false },
    };

    assert(feishu_unread_count(messages, 4, 150) == 1);
    assert(feishu_unread_count(messages, 4, 0) == 2);
    assert(feishu_latest_visible_time(messages, 4) == 300);
}

static void test_utf8_copy(void)
{
    char output[8];

    assert(feishu_utf8_copy(output, sizeof(output), "你好世界") == 6);
    assert(strcmp(output, "你好") == 0);
}

static void test_navigation(void)
{
    feishu_nav_t nav = {
        .page = FEISHU_PAGE_CHATS,
        .chat_count = 2,
        .message_count = 3,
    };

    assert(feishu_nav_handle(&nav, FEISHU_NAV_DOWN) == FEISHU_NAV_NONE);
    assert(nav.chat_index == 1);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_DOWN) == FEISHU_NAV_NONE);
    assert(nav.chat_index == 1);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_LOAD_MESSAGES);
    assert(nav.page == FEISHU_PAGE_MESSAGES);
    assert(nav.composer_selected);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_NONE);
    assert(nav.page == FEISHU_PAGE_REPLY_READY);
    assert(!nav.reply_to_message);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_START_RECORDING);
    assert(nav.page == FEISHU_PAGE_RECORDING);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_STOP_RECORDING);
    nav.page = FEISHU_PAGE_MESSAGES;
    nav.composer_selected = true;
    assert(feishu_nav_handle(&nav, FEISHU_NAV_UP) == FEISHU_NAV_NONE);
    assert(!nav.composer_selected && nav.message_index == 2);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_NONE);
    assert(nav.page == FEISHU_PAGE_MESSAGE_DETAIL);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_NONE);
    assert(nav.page == FEISHU_PAGE_REPLY_READY && nav.reply_to_message);
    nav.page = FEISHU_PAGE_IMAGE;
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_NONE);
    assert(nav.page == FEISHU_PAGE_REPLY_READY);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_BACK) == FEISHU_NAV_NONE);
    assert(nav.page == FEISHU_PAGE_MESSAGES);
    nav.page = FEISHU_PAGE_REVIEW;
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_SEND_REPLY);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_UP) == FEISHU_NAV_RETRY_RECORDING);
    assert(nav.page == FEISHU_PAGE_RECORDING);

    nav.page = FEISHU_PAGE_CHATS;
    assert(feishu_nav_handle(&nav, FEISHU_NAV_BACK) == FEISHU_NAV_NONE);
    assert(nav.page == FEISHU_PAGE_SETTINGS);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_NONE);
    assert(nav.page == FEISHU_PAGE_CONFIRM);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_RESET_WIFI);
    nav.page = FEISHU_PAGE_SETTINGS;
    nav.settings_index = 1;
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_NONE);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_UNBIND);
}

int main(void)
{
    test_unread();
    test_utf8_copy();
    test_navigation();
    return 0;
}
