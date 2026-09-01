#pragma once

// Path B voice input: the device is a wireless mic for the PC. It captures 16k
// mono PCM and broadcasts it over Wi-Fi UDP; a PC agent feeds it into a virtual
// audio device (e.g. VB-Cable) so 豆包/any input method transcribes it, and
// injects Enter/Backspace on the send/delete control packets. Three keys:
// DOWN = start/stop mic, OK = 发送 (Enter), UP = 删除 (Backspace),
// UP long-press = 全部删除 (clear the line).
//
// This module is the pure wire contract (framing only), so it runs host tests
// and the PC agent (tools/island_agent.py) stays byte-for-byte in sync.
//
// Packets, device -> PC, UDP broadcast:
//   audio: [0x56 'V'][0x00 type][seq_lo][seq_hi][PCM int16 little-endian...]
//   ctrl:  [0x56 'V'][code]            code in {1 send, 2 delete, 3 start,
//                                                4 stop, 5 delete-all}
//   stats: [6][ovf u16][first_frame_ms u16][read_ms u16][send_ms u16][retry_ms u16]
//          Sent once after STOP so the PC can report where the device's loop time
//          went without a USB cable attached. Diagnosing a rate shortfall needs
//          these numbers, and needing a cable to read them means they are unavailable
//          exactly when the device is being used normally.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VOICE_MAGIC 0x56
#define VOICE_AUDIO_HEADER_LEN 4
#define VOICE_CTRL_LEN 2

enum {
    VOICE_TYPE_AUDIO = 0x00,
};

typedef enum {
    VOICE_CTRL_SEND = 1,    // PC injects Enter
    VOICE_CTRL_DELETE = 2,  // PC injects Backspace
    VOICE_CTRL_START = 3,   // mic stream opening
    VOICE_CTRL_STOP = 4,    // mic stream closing
    VOICE_CTRL_DELETE_ALL = 5,  // PC clears the whole line (select-all + delete)
    VOICE_CTRL_STATS = 6,       // device -> PC, followed by a stats payload
} voice_ctrl_t;

// Write the 4-byte audio header into buf (>= VOICE_AUDIO_HEADER_LEN). PCM bytes
// are appended by the caller. Returns bytes written, 0 on bad args.
size_t voice_pack_audio_header(uint8_t *buf, size_t cap, uint16_t seq);

// Write a 2-byte control packet. Returns bytes written, 0 on bad args/code.
size_t voice_pack_ctrl(uint8_t *buf, size_t cap, voice_ctrl_t code);

bool voice_ctrl_valid(int code);
