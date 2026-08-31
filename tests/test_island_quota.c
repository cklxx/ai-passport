#include "island_quota.h"

#include <assert.h>
#include <string.h>

// Build a valid packet with a correct XOR trailer, then let tests corrupt it.
static void make_packet(uint8_t buf[7], uint8_t used, uint32_t resets_at)
{
    buf[0] = ISLAND_QUOTA_MAGIC;
    buf[1] = used;
    buf[2] = resets_at & 0xFF;
    buf[3] = resets_at >> 8 & 0xFF;
    buf[4] = resets_at >> 16 & 0xFF;
    buf[5] = resets_at >> 24 & 0xFF;
    buf[6] = 0;
    for (int i = 0; i < 6; ++i) buf[6] ^= buf[i];
}

static void test_parse_valid(void)
{
    uint8_t buf[7];
    island_quota_t q = { .remaining_pct = 42, .resets_at = 7 };

    make_packet(buf, 30, 1893456000u);  // 30% used -> 70% remaining
    assert(island_quota_parse(buf, 7, &q));
    assert(q.remaining_pct == 70);
    assert(q.resets_at == 1893456000u);

    make_packet(buf, 0, 100);
    assert(island_quota_parse(buf, 7, &q) && q.remaining_pct == 100);
    make_packet(buf, 100, 100);
    assert(island_quota_parse(buf, 7, &q) && q.remaining_pct == 0);
}

static void test_parse_unknown(void)
{
    uint8_t buf[7];
    island_quota_t q = { .remaining_pct = 50, .resets_at = 9 };
    make_packet(buf, ISLAND_QUOTA_UNKNOWN, 12345);  // resets bytes ignored
    assert(island_quota_parse(buf, 7, &q));
    assert(q.remaining_pct == -1 && q.resets_at == 0);
}

static void test_parse_rejects(void)
{
    uint8_t buf[7];
    island_quota_t q = { .remaining_pct = 77, .resets_at = 88 };

    // NULLs and wrong length.
    make_packet(buf, 30, 100);
    assert(!island_quota_parse(NULL, 7, &q));
    assert(!island_quota_parse(buf, 7, NULL));
    assert(!island_quota_parse(buf, 6, &q));
    assert(!island_quota_parse(buf, 8, &q));

    // Bad magic.
    make_packet(buf, 30, 100); buf[0] = 0x00;
    assert(!island_quota_parse(buf, 7, &q));

    // Corrupted byte breaks the XOR fold.
    make_packet(buf, 30, 100); buf[3] ^= 0x20;
    assert(!island_quota_parse(buf, 7, &q));

    // Out-of-range used (99 < x < 255) with a valid XOR must still reject.
    make_packet(buf, 150, 100);
    assert(!island_quota_parse(buf, 7, &q));

    // A rejected packet must not clobber the caller's last-good value.
    assert(q.remaining_pct == 77 && q.resets_at == 88);
}

static void test_countdown(void)
{
    char s[24];
    const uint32_t now = 1000000;

    island_quota_countdown(s, sizeof(s), 0, now);
    assert(strcmp(s, "未知") == 0);
    island_quota_countdown(s, sizeof(s), now - 5, now);
    assert(strcmp(s, "即将重置") == 0);
    island_quota_countdown(s, sizeof(s), now + 30, now);  // <1min
    assert(strcmp(s, "即将重置") == 0);
    island_quota_countdown(s, sizeof(s), now + 5 * 60, now);
    assert(strcmp(s, "5分") == 0);
    island_quota_countdown(s, sizeof(s), now + (2 * 3600 + 13 * 60), now);
    assert(strcmp(s, "2时13分") == 0);
    island_quota_countdown(s, sizeof(s), now + (3 * 86400 + 4 * 3600), now);
    assert(strcmp(s, "3天4时") == 0);

    // Zero-size buffer must be safe and write nothing.
    assert(island_quota_countdown(s, 0, now + 60, now) == 0);
}

int main(void)
{
    test_parse_valid();
    test_parse_unknown();
    test_parse_rejects();
    test_countdown();
    return 0;
}
