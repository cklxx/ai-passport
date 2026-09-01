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
    assert(voice_pack_ctrl(b, sizeof(b), VOICE_CTRL_ERASE_BEGIN) == 2 && b[1] == 5);
    assert(voice_pack_ctrl(b, sizeof(b), VOICE_CTRL_ERASE_END) == 2 && b[1] == 6);
    assert(voice_ctrl_valid(VOICE_CTRL_ERASE_END));
    // STATS travels device -> PC with a payload behind it, so it must never pass as
    // a bare key code — island_agent.py reads code 7 as "unpack 24 more bytes".
    assert(!voice_ctrl_valid(VOICE_CTRL_STATS));
    assert(voice_pack_ctrl(b, sizeof(b), VOICE_CTRL_STATS) == 0);

    // bad code / small buffer rejected
    assert(voice_pack_ctrl(b, sizeof(b), (voice_ctrl_t)0) == 0);
    assert(voice_pack_ctrl(b, sizeof(b), (voice_ctrl_t)99) == 0);
    assert(voice_pack_ctrl(b, 1, VOICE_CTRL_SEND) == 0);
}

static void test_valid(void)
{
    assert(!voice_ctrl_valid(0) && voice_ctrl_valid(1) && voice_ctrl_valid(4));
    assert(voice_ctrl_valid(6) && !voice_ctrl_valid(7) && !voice_ctrl_valid(-1));
}

// These bytes are the wire contract: tools/island_agent.py asserts the same
// vectors against its own table, so a change to either encoder that is not made
// on both sides fails here rather than arriving as distorted audio.
static void test_ulaw_vectors(void)
{
    static const int16_t in[]  = { 0, 1, -1, 100, -100, 1000, -1000,
                                   8000, -8000, 32767, -32768, 3, -3, 255, -256 };
    static const uint8_t out[] = { 0xFF, 0xFF, 0x7F, 0xF2, 0x72, 0xCE, 0x4E,
                                   0xA0, 0x20, 0x80, 0x00, 0xFF, 0x7F, 0xE7, 0x67 };
    for (size_t i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
        assert(voice_ulaw_encode(in[i]) == out[i]);
    }
}

static void test_ulaw_properties(void)
{
    // Monotonic: a louder sample never decodes quieter. A broken segment search
    // still round-trips plausibly, so ordering is what actually catches it.
    for (int32_t v = -32768; v < 32752; v += 16) {
        int16_t a = voice_ulaw_decode(voice_ulaw_encode((int16_t)v));
        int16_t b = voice_ulaw_decode(voice_ulaw_encode((int16_t)(v + 16)));
        assert(a <= b);
    }
    // Silence stays silent, and the sign survives.
    assert(voice_ulaw_decode(voice_ulaw_encode(0)) == 0);
    assert(voice_ulaw_decode(voice_ulaw_encode(5000)) > 0);
    assert(voice_ulaw_decode(voice_ulaw_encode(-5000)) < 0);
    // Quantization error stays inside mu-law's segment width — about 4% of the
    // magnitude plus the smallest step. 2 dB-SNR encoders fail this by 10x.
    for (int32_t v = -32000; v <= 32000; v += 37) {
        int32_t d = voice_ulaw_decode(voice_ulaw_encode((int16_t)v)) - v;
        if (d < 0) d = -d;
        int32_t m = v < 0 ? -v : v;
        assert(d <= m / 16 + 64);
    }
    // Every code decodes into the range mu-law can represent.
    for (int c = 0; c < 256; c++) {
        int16_t s = voice_ulaw_decode((uint8_t)c);
        assert(s >= -32124 && s <= 32124);
    }
}

int main(void)
{
    test_audio_header();
    test_ctrl();
    test_valid();
    test_ulaw_vectors();
    test_ulaw_properties();
    return 0;
}
