#include "voice_proto.h"

#include <assert.h>

static void test_audio_header(void)
{
    uint8_t b[4];
    assert(voice_pack_audio_header(b, sizeof(b), 0x1234) == 4);
    assert(b[0] == VOICE_MAGIC && b[1] == VOICE_TYPE_AUDIO);
    assert(b[2] == 0x34 && b[3] == 0x12);  // little-endian seq
    // seq wraps cleanly at 16 bits
    assert(voice_pack_audio_header(b, sizeof(b), 0xFFFF) == 4);
    assert(b[2] == 0xFF && b[3] == 0xFF);
    // too-small buffer rejected
    assert(voice_pack_audio_header(b, 3, 0) == 0);
    assert(voice_pack_audio_header(NULL, 4, 0) == 0);
}

static void test_ctrl(void)
{
    uint8_t b[2];
    assert(voice_pack_ctrl(b, sizeof(b), VOICE_CTRL_SEND) == 2);
    assert(b[0] == VOICE_MAGIC && b[1] == 1);
    assert(voice_pack_ctrl(b, sizeof(b), VOICE_CTRL_DELETE) == 2 && b[1] == 2);
    assert(voice_pack_ctrl(b, sizeof(b), VOICE_CTRL_STOP) == 2 && b[1] == 4);
    assert(voice_pack_ctrl(b, sizeof(b), VOICE_CTRL_DELETE_ALL) == 2 && b[1] == 5);
    assert(voice_ctrl_valid(VOICE_CTRL_DELETE_ALL));

    // bad code / small buffer rejected
    assert(voice_pack_ctrl(b, sizeof(b), (voice_ctrl_t)0) == 0);
    assert(voice_pack_ctrl(b, sizeof(b), (voice_ctrl_t)99) == 0);
    assert(voice_pack_ctrl(b, 1, VOICE_CTRL_SEND) == 0);
}

static void test_valid(void)
{
    assert(!voice_ctrl_valid(0) && voice_ctrl_valid(1) && voice_ctrl_valid(4));
    assert(!voice_ctrl_valid(6) && !voice_ctrl_valid(-1));
}

int main(void)
{
    test_audio_header();
    test_ctrl();
    test_valid();
    return 0;
}
