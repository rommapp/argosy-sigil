// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdio.h>
#include <string.h>

/* Synthetic Game Boy headers: the checksum over 0x134..0x14C is written so
 * the boot-ROM check passes, then one cart-type byte at 0x147 per case. */

#define ROM_LEN      0x8000
#define CART_TYPE    0x147
#define CHECKSUM     0x14D

typedef struct { const uint8_t *buf; size_t len; } mem_ctx;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_ctx *m = (mem_ctx *)ctx;
    if (off >= m->len) return 0;
    size_t avail = m->len - (size_t)off;
    size_t n = len < avail ? len : avail;
    memcpy(buf, m->buf + off, n);
    return (int)n;
}
static int64_t mem_size(void *ctx) { return (int64_t)((mem_ctx *)ctx)->len; }

static void write_header(uint8_t *rom, uint8_t cart_type) {
    memset(rom, 0, ROM_LEN);
    memcpy(rom + 0x134, "POKEMON CRYSTAL", 15);
    rom[CART_TYPE] = cart_type;
    rom[0x148] = 0x06;
    rom[0x149] = 0x03;
    uint8_t x = 0;
    for (int i = 0x134; i <= 0x14C; i++) x = (uint8_t)(x - rom[i] - 1);
    rom[CHECKSUM] = x;
}

static int expect(uint8_t cart_type, const char *name, const char *slug,
                  int want_rc, int want_rtc, int want_platform, const char *label) {
    uint8_t rom[ROM_LEN];
    write_header(rom, cart_type);
    mem_ctx ctx = { rom, sizeof(rom) };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, name, sigil_platform_from_slug(slug), NULL, &r);
    if (rc != want_rc) {
        fprintf(stderr, "FAIL %s: rc=%d want %d\n", label, rc, want_rc);
        return 1;
    }
    if (rc != SIGIL_OK) return 0;
    int has_rtc = (r.features & SIGIL_FEATURE_RTC) ? 1 : 0;
    if (has_rtc != want_rtc || (int)r.platform != want_platform
        || r.usage != SIGIL_USAGE_FILE_PREFIX || r.source != SIGIL_SOURCE_BINARY
        || r.title_id[0] != '\0' || r.save_id[0] != '\0'
        || r.struct_version < SIGIL_RESULT_V3) {
        fprintf(stderr, "FAIL %s: rtc=%d platform=%d usage=%d source=%d title='%s' v=%u\n",
                label, has_rtc, (int)r.platform, (int)r.usage, (int)r.source,
                r.title_id, r.struct_version);
        return 1;
    }
    return 0;
}

int main(void) {
    int fails = 0;
    fails += expect(0x10, "Pokemon - Crystal Version (USA).gbc", NULL, SIGIL_OK, 1, SIGIL_PLATFORM_GBC, "mbc3 timer ram battery");
    fails += expect(0x0F, "clock.gb", NULL, SIGIL_OK, 1, SIGIL_PLATFORM_GB, "mbc3 timer battery");
    fails += expect(0xFE, "Robopon - Sun Version (USA).gbc", "gbc", SIGIL_OK, 1, SIGIL_PLATFORM_GBC, "huc3");
    fails += expect(0xFD, "tamagotchi.gb", NULL, SIGIL_OK, 1, SIGIL_PLATFORM_GB, "tama5");
    fails += expect(0x13, "Pokemon - Yellow Version (USA).gbc", NULL, SIGIL_OK, 0, SIGIL_PLATFORM_GBC, "mbc3 ram battery, no timer");
    fails += expect(0x1B, "Pokemon - Red Version (USA).gb", NULL, SIGIL_OK, 0, SIGIL_PLATFORM_GB, "mbc5 ram battery");
    fails += expect(0x00, "Tetris (World).gb", NULL, SIGIL_OK, 0, SIGIL_PLATFORM_GB, "rom only");
    fails += expect(0x10, "clock.gb", "gbc", SIGIL_OK, 1, SIGIL_PLATFORM_GBC, "platform hint wins over extension");

    {
        uint8_t rom[ROM_LEN];
        write_header(rom, 0x10);
        rom[CHECKSUM] ^= 0xFF;
        mem_ctx ctx = { rom, sizeof(rom) };
        sigil_io io = { mem_read, mem_size, NULL, &ctx };
        sigil_result r;
        sigil_options opts = { SIGIL_OPTIONS_V1, NULL, 0 };
        int rc = sigil_extract_from_io(&io, "junk.gb", SIGIL_PLATFORM_GB, &opts, &r);
        if (rc == SIGIL_OK) {
            fprintf(stderr, "FAIL bad checksum accepted\n");
            fails++;
        }
    }

    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("unit_gb: ok\n");
    return 0;
}
