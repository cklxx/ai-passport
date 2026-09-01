#include "voice_proto.h"

bool voice_ctrl_valid(int code)
{
    return code >= VOICE_CTRL_SEND && code <= VOICE_CTRL_DELETE_ALL;
}

size_t voice_pack_audio_header(uint8_t *buf, size_t cap, uint16_t seq)
{
    if (buf == NULL || cap < VOICE_AUDIO_HEADER_LEN) return 0;
    buf[0] = VOICE_MAGIC;
    buf[1] = VOICE_TYPE_AUDIO;
    buf[2] = seq & 0xFF;
    buf[3] = seq >> 8 & 0xFF;
    return VOICE_AUDIO_HEADER_LEN;
}

size_t voice_pack_ctrl(uint8_t *buf, size_t cap, voice_ctrl_t code)
{
    if (buf == NULL || cap < VOICE_CTRL_LEN || !voice_ctrl_valid(code)) return 0;
    buf[0] = VOICE_MAGIC;
    buf[1] = (uint8_t)code;
    return VOICE_CTRL_LEN;
}

// G.711 mu-law, the standard algorithm. Sign is carried separately, the magnitude
// gets a 33-unit bias so the smallest segment is well-defined, and the result is
// stored as a 3-bit segment (exponent) plus a 4-bit mantissa, then inverted — the
// inversion is part of the standard and makes silence transmit as 0xFF.
//
// The bit_length-6 form of the segment search is the subtle part: after the bias
// the magnitude spans 33..8191, and segment N covers up to 63, 127, 255 ... 8191,
// so the segment is the bit length minus six. Getting this wrong still produces
// plausible-looking bytes and a working round trip, just with the wrong scale —
// an early version used minus eight and measured 2 dB SNR instead of 35.
uint8_t voice_ulaw_encode(int16_t sample)
{
    // 0x7F vs 0xFF is the sign, applied as the final inversion mask.
    uint8_t mask = sample < 0 ? 0x7F : 0xFF;
    // -32768 has no positive counterpart in int16_t; widen before negating.
    int32_t s = sample < 0 ? -(int32_t)sample : sample;
    int32_t m = (s >> 2) + 33;              // drop 2 bits, add the bias (0x84 >> 2)
    if (m > 8159 + 33) m = 8159 + 33;
    int seg = 0;
    for (int32_t t = m >> 6; t != 0; t >>= 1) seg++;
    if (seg >= 8) return (uint8_t)(0x7F ^ mask);
    return (uint8_t)((((seg << 4) | ((m >> (seg + 1)) & 0x0F))) ^ mask);
}

int16_t voice_ulaw_decode(uint8_t code)
{
    uint8_t u = (uint8_t)~code;
    int32_t t = (int32_t)((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
}
