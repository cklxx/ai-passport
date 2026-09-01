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
