#pragma once

// Claude usage "island": the device shows one merged 7-day quota ring plus a
// reset countdown. The number only exists on the PC running Claude Code (its
// statusline `rate_limits.seven_day`), so a small PC agent forwards it as a
// fixed 7-byte packet over the device link (BLE or USB-serial). The device does
// no JSON parsing — this module is the whole device-side contract and is pure,
// so it runs under host tests.
//
// Wire packet, PC -> device. Two lengths are accepted so an older 7-byte sender
// keeps working — a device flashed with this build still reads it and simply has
// no Codex number to show:
//   [0]    magic 0x51 ('Q')
//   [1]    Claude used_percentage 0..100, or 0xFF = unknown/stale
//   [2..5] resets_at, unix seconds (uint32)
//   [6]    Codex used_percentage 0..100, or 0xFF = unknown   (8-byte packet only)
//   [last] XOR of every preceding byte (so XOR of the whole packet == 0)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ISLAND_QUOTA_PACKET_LEN 8      // current senders
#define ISLAND_QUOTA_PACKET_LEN_V1 7   // legacy: no Codex byte
#define ISLAND_QUOTA_MAGIC 0x51
#define ISLAND_QUOTA_UNKNOWN 0xFF

typedef struct {
    int8_t remaining_pct;        // Claude 0..100, or -1 when unknown/stale
    int8_t codex_remaining_pct;  // Codex 0..100, or -1 when unknown/absent
    uint32_t resets_at;          // unix seconds; 0 when unknown
} island_quota_t;

// Parse a wire packet. Returns true and fills *out only on a valid packet
// (right length, magic, XOR, and used_percentage in range). On false, *out is
// untouched — callers keep the last good value instead of flashing garbage.
bool island_quota_parse(const uint8_t *buf, size_t len, island_quota_t *out);

// Format the reset countdown into dst as compact CJK: "2天3时", "5时12分",
// "3分", "即将重置", or "未知". Returns strlen written (0 only if dst_size==0).
size_t island_quota_countdown(char *dst, size_t dst_size, uint32_t resets_at,
                              uint32_t now);
