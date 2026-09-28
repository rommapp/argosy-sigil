// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
#include "clock_gb.h"
#include <stdbool.h>

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static void put32(uint8_t *p, uint32_t v) { sigil_write_le32(p, v); }

static void vba_bytes(uint8_t out[48], const uint32_t current[5], const uint32_t latched[5], uint64_t stamp) {
    for (int i = 0; i < 5; i++) put32(out + 4 * i, current[i]);
    for (int i = 0; i < 5; i++) put32(out + 20 + 4 * i, latched[i]);
    put32(out + 40, (uint32_t)stamp);
    put32(out + 44, (uint32_t)(stamp >> 32));
}

static bool same_time(const sigil_gb_clock *c, unsigned days, unsigned h, unsigned m, unsigned s,
                      bool halted, bool carry, int64_t stamp) {
    return c->days == days && c->hours == h && c->minutes == m && c->seconds == s &&
           c->halted == halted && c->carry == carry && c->stamp == stamp;
}

/* The real mGBA Crystal clock from the samples: current and latched both
 * 19:11:31 on day 0, stamped 1788923818 (manifest notes, read by hex). With
 * the slots equal, the neutral VBA form writes the same 48 bytes back. */
static void check_crystal(void) {
    char path[1024];
    if (corpus_sample_path("gbc", "crystal-mgba", "Pokemon - Crystal Version (USA, Europe) (Rev 1).rtc",
                           path, sizeof(path)) != 0) return;
    size_t len = 0;
    uint8_t *rtc = corpus_read_file(path, &len);
    if (!rtc) return;
    sigil_gb_clock c;
    bool lossy = true;
    uint8_t back[48];
    if (sigil_gb_clock_read(SIGIL_GB_CLOCK_MGBA, rtc, len, 0, &c, &lossy) != SIGIL_OK) {
        fail("crystal-mgba", "the clock did not read");
    } else {
        if (!same_time(&c, 0, 19, 11, 31, false, false, 1788923818)) fail("crystal-mgba", "clock values differ");
        if (lossy) fail("crystal-mgba", "an mGBA clock read as lossy");
        if (sigil_gb_clock_write(SIGIL_GB_CLOCK_VBA, &c, back, &lossy) != SIGIL_OK || memcmp(back, rtc, 48) != 0) {
            fail("crystal-mgba", "the neutral form differs from mGBA's bytes although its slots agree");
        }
    }
    free(rtc);

    if (corpus_sample_path("gbc", "crystal-sav-footer-constructed", "Pokemon - Crystal Version (USA, Europe) (Rev 1).sav",
                           path, sizeof(path)) != 0) return;
    uint8_t *sav = corpus_read_file(path, &len);
    if (!sav) return;
    if (len != 32768 + 48 || sigil_gb_clock_read(SIGIL_GB_CLOCK_VBA, sav + 32768, 48, 0, &c, &lossy) != SIGIL_OK ||
        !same_time(&c, 0, 19, 11, 31, false, false, 1788923818)) {
        fail("crystal-sav-footer-constructed", "the footer clock differs");
    }
    free(sav);
}

/* DH bit 0 is day bit 8, bit 6 halt, bit 7 carry; VBA-M may hold the whole
 * day count in DL. */
static void check_vba_fields(void) {
    uint8_t b[48];
    sigil_gb_clock c;
    bool lossy;
    const uint32_t flags[5] = { 1, 2, 3, 0x2A, 0xC1 };
    vba_bytes(b, flags, flags, 1700000000ULL);
    if (sigil_gb_clock_read(SIGIL_GB_CLOCK_VBA, b, 48, 0, &c, &lossy) != SIGIL_OK ||
        !same_time(&c, 0x12A, 3, 2, 1, true, true, 1700000000)) {
        fail("vba", "DH bits misread");
    }
    uint8_t back[48];
    if (sigil_gb_clock_write(SIGIL_GB_CLOCK_VBA, &c, back, &lossy) != SIGIL_OK || memcmp(back, b, 48) != 0 || lossy) {
        fail("vba", "a VBA clock doesn't round-trip");
    }
    const uint32_t unmasked[5] = { 0, 0, 0, 300, 0 };
    vba_bytes(b, unmasked, unmasked, 0);
    if (sigil_gb_clock_read(SIGIL_GB_CLOCK_VBA, b, 48, 0, &c, &lossy) != SIGIL_OK || c.days != 300) {
        fail("vba", "an unmasked VBA-M day count misread");
    }
    if (sigil_gb_clock_read(SIGIL_GB_CLOCK_VBA, b, 47, 0, &c, &lossy) != SIGIL_ERR_UNSUPPORTED_FORMAT) {
        fail("vba", "a short clock was accepted");
    }
}

/* mGBA's time goes with the latched slot, so that slot is the clock. */
static void check_mgba_pairing(void) {
    const uint32_t current[5] = { 10, 0, 0, 0, 0 };
    const uint32_t latched[5] = { 5, 0, 0, 0, 0 };
    uint8_t b[48], back[48];
    vba_bytes(b, current, latched, 1000);
    sigil_gb_clock c;
    bool lossy;
    if (sigil_gb_clock_read(SIGIL_GB_CLOCK_MGBA, b, 48, 0, &c, &lossy) != SIGIL_OK || c.seconds != 5) {
        fail("mgba", "mGBA's latched slot is not the clock");
    }
    if (sigil_gb_clock_read(SIGIL_GB_CLOCK_VBA, b, 48, 0, &c, &lossy) != SIGIL_OK || c.seconds != 10) {
        fail("vba", "the VBA current slot is not the clock");
    }
    if (sigil_gb_clock_write(SIGIL_GB_CLOCK_MGBA, &c, back, &lossy) != SIGIL_OK ||
        sigil_read_le32(back + 20) != 10 || sigil_read_le32(back) != 10) {
        fail("mgba", "writing mGBA doesn't put the clock in its latched slot");
    }
}

/* gambatte keeps only the base time the counter runs from. */
static void check_gambatte(void) {
    uint8_t b[8] = { 0x40, 0x42, 0x0F, 0x00, 0, 0, 0, 0 };   /* 1000000 */
    sigil_gb_clock c;
    bool lossy;
    if (sigil_gb_clock_read(SIGIL_GB_CLOCK_GAMBATTE, b, 8, 0, &c, &lossy) != SIGIL_OK ||
        !same_time(&c, 0, 0, 0, 0, false, false, 1000000) || !lossy) {
        fail("gambatte", "base time misread");
    }
    sigil_gb_clock later;
    memset(&later, 0, sizeof(later));
    later.days = 1;
    later.minutes = 1;
    later.seconds = 5;
    later.stamp = 2000000;
    uint8_t out[8];
    const uint8_t expect[8] = { 0xBF, 0x32, 0x1D, 0x00, 0, 0, 0, 0 };   /* 2000000 - 86465 = 1913535 */
    if (sigil_gb_clock_write(SIGIL_GB_CLOCK_GAMBATTE, &later, out, &lossy) != SIGIL_OK || memcmp(out, expect, 8) != 0 || lossy) {
        fail("gambatte", "base time miswritten");
    }
    later.halted = true;
    if (sigil_gb_clock_write(SIGIL_GB_CLOCK_GAMBATTE, &later, out, &lossy) != SIGIL_OK || !lossy) {
        fail("gambatte", "a halted clock wrote without reporting the loss");
    }
}

/* Mesen2: five register bytes, then milliseconds big-endian. */
static void check_mesen2(void) {
    const uint8_t b[13] = { 5, 4, 3, 2, 0x01, 0x00, 0x00, 0x00, 0xE8, 0xD4, 0xA5, 0x11, 0xF4 };  /* 1000000000500 ms */
    sigil_gb_clock c;
    bool lossy;
    if (sigil_gb_clock_read(SIGIL_GB_CLOCK_MESEN2, b, 13, 0, &c, &lossy) != SIGIL_OK ||
        !same_time(&c, 0x102, 3, 4, 5, false, false, 1000000000)) {
        fail("mesen2", "clock misread");
    }
}

/* TGB Dual keeps emulated seconds only, so the clock is taken as of `now`. */
static void check_tgb(void) {
    const uint8_t b[4] = { 0xCD, 0x5F, 0x01, 0x00 };  /* 90061 = 1 day 01:01:01 */
    sigil_gb_clock c;
    bool lossy;
    if (sigil_gb_clock_read(SIGIL_GB_CLOCK_TGB_DUAL, b, 4, 1234, &c, &lossy) != SIGIL_OK ||
        !same_time(&c, 1, 1, 1, 1, false, false, 1234) || !lossy) {
        fail("tgb", "emulated seconds misread");
    }
}

/* SameBoy libretro: registers as bytes, its last second at 0x10. */
static void check_sameboy_libretro(void) {
    uint8_t b[32];
    memset(b, 0, sizeof(b));
    const uint8_t regs[10] = { 1, 2, 3, 4, 0x41, 1, 2, 3, 4, 0x41 };
    memcpy(b, regs, 10);
    b[16] = 0x40; b[17] = 0xE2; b[18] = 0x01;   /* 123456 */
    sigil_gb_clock c;
    bool lossy;
    uint8_t back[32];
    if (sigil_gb_clock_read(SIGIL_GB_CLOCK_SAMEBOY_LIBRETRO, b, 32, 0, &c, &lossy) != SIGIL_OK ||
        !same_time(&c, 0x104, 3, 2, 1, true, false, 123456)) {
        fail("sameboy", "clock misread");
    } else if (sigil_gb_clock_write(SIGIL_GB_CLOCK_SAMEBOY_LIBRETRO, &c, back, &lossy) != SIGIL_OK || memcmp(back, b, 32) != 0) {
        fail("sameboy", "a SameBoy clock doesn't round-trip");
    }
}

int main(void) {
    check_crystal();
    check_vba_fields();
    check_mgba_pairing();
    check_gambatte();
    check_mesen2();
    check_tgb();
    check_sameboy_libretro();
    printf("gb clocks: %d failures\n", g_fails);
    return g_fails ? 1 : 0;
}
