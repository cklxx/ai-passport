#include "island_quota.h"

#include <stdio.h>

bool island_quota_parse(const uint8_t *buf, size_t len, island_quota_t *out)
{
    // Accept both lengths: a sender predating the Codex byte still parses, and
    // its packets simply carry no Codex number.
    if (buf == NULL || out == NULL ||
        (len != ISLAND_QUOTA_PACKET_LEN && len != ISLAND_QUOTA_PACKET_LEN_V1)) {
        return false;
    }
    uint8_t fold = 0;
    for (size_t i = 0; i < len; ++i) fold ^= buf[i];
    if (fold != 0 || buf[0] != ISLAND_QUOTA_MAGIC) return false;

    // Codex occupies the byte before the checksum, so it exists only in the
    // longer packet. Decode it first: a Claude "unknown" must not discard it.
    int8_t codex = -1;
    if (len == ISLAND_QUOTA_PACKET_LEN) {
        uint8_t cu = buf[6];
        if (cu != ISLAND_QUOTA_UNKNOWN) {
            if (cu > 100) return false;
            codex = (int8_t)(100 - cu);
        }
    }
    out->codex_remaining_pct = codex;

    uint8_t used = buf[1];
    if (used == ISLAND_QUOTA_UNKNOWN) {
        out->remaining_pct = -1;
        out->resets_at = 0;
        return true;
    }
    if (used > 100) return false;
    out->remaining_pct = (int8_t)(100 - used);
    out->resets_at = (uint32_t)buf[2] | (uint32_t)buf[3] << 8 |
                     (uint32_t)buf[4] << 16 | (uint32_t)buf[5] << 24;
    return true;
}

size_t island_quota_countdown(char *dst, size_t dst_size, uint32_t resets_at,
                              uint32_t now)
{
    if (dst == NULL || dst_size == 0) return 0;
    int n;
    if (resets_at == 0) {
        n = snprintf(dst, dst_size, "未知");
    } else if (resets_at <= now) {
        n = snprintf(dst, dst_size, "即将重置");
    } else {
        uint32_t s = resets_at - now;
        unsigned d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60;
        if (d > 0)      n = snprintf(dst, dst_size, "%u天%u时", d, h);
        else if (h > 0) n = snprintf(dst, dst_size, "%u时%u分", h, m);
        else if (m > 0) n = snprintf(dst, dst_size, "%u分", m);
        else            n = snprintf(dst, dst_size, "即将重置");
    }
    // snprintf may report a would-be length past the buffer; clamp to written.
    if (n < 0) { dst[0] = '\0'; return 0; }
    return (size_t)n < dst_size ? (size_t)n : dst_size - 1;
}
