// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdio.h>
#include <string.h>

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

/* The title field 0x134-0x142 as rgbfix writes it: the title, then any
 * manufacturer code, padded with 0x00. */
typedef struct { const char *bytes; size_t len; uint8_t destination; } title_field;

static const title_field JAPANESE_CRYSTAL = { "POKEMON CRYSTAL", 15, 0x00 };

static void write_header(uint8_t *rom, uint8_t cart_type, const title_field *title) {
    memset(rom, 0, ROM_LEN);
    memcpy(rom + 0x134, title->bytes, title->len);
    rom[CART_TYPE] = cart_type;
    rom[0x148] = 0x06;
    rom[0x149] = 0x03;
    rom[0x14A] = title->destination;
    uint8_t x = 0;
    for (int i = 0x134; i <= 0x14C; i++) x = (uint8_t)(x - rom[i] - 1);
    rom[CHECKSUM] = x;
}

static int expect_titled(uint8_t cart_type, const title_field *title, const char *name, const char *slug,
                         int want_rc, uint32_t want_features, int want_platform, const char *label) {
    uint8_t rom[ROM_LEN];
    write_header(rom, cart_type, title);
    mem_ctx ctx = { rom, sizeof(rom) };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, name, sigil_platform_from_slug(slug), NULL, &r);
    if (rc != want_rc) {
        fprintf(stderr, "FAIL %s: rc=%d want %d\n", label, rc, want_rc);
        return 1;
    }
    if (rc != SIGIL_OK) return 0;
    if (r.features != want_features || (int)r.platform != want_platform
        || r.usage != SIGIL_USAGE_FILE_PREFIX || r.source != SIGIL_SOURCE_BINARY
        || r.title_id[0] != '\0' || r.save_id[0] != '\0'
        || r.struct_version < SIGIL_RESULT_V3) {
        fprintf(stderr, "FAIL %s: features=%u platform=%d usage=%d source=%d title='%s' v=%u\n",
                label, r.features, (int)r.platform, (int)r.usage, (int)r.source,
                r.title_id, r.struct_version);
        return 1;
    }
    return 0;
}

static int expect(uint8_t cart_type, const char *name, const char *slug,
                  int want_rc, uint32_t want_features, int want_platform, const char *label) {
    return expect_titled(cart_type, &JAPANESE_CRYSTAL, name, slug, want_rc, want_features, want_platform, label);
}

/* Title fields and cart types from the rgbfix flags in the Makefiles of
 * pret/pokered at ba9c056, pokeyellow at b452f2a, pokegold at 2bbfba6 and
 * pokecrystal at 9ce4c4e; -j sets destination 0x01. */
static int scratch_titles(void) {
    const uint32_t RTC = SIGIL_FEATURE_RTC, GEN1 = SIGIL_FEATURE_GB_SCRATCH_GEN1,
                   GS = SIGIL_FEATURE_GB_SCRATCH_GOLD_SILVER, C = SIGIL_FEATURE_GB_SCRATCH_CRYSTAL;
    const title_field red = { "POKEMON RED", 11, 0x01 }, blue = { "POKEMON BLUE", 12, 0x01 },
                      yellow = { "POKEMON YELLOW", 14, 0x01 }, gold = { "POKEMON_GLDAAUE", 15, 0x01 },
                      silver = { "POKEMON_SLVAAXE", 15, 0x01 }, crystal = { "PM_CRYSTAL\0BYTE", 15, 0x01 },
                      crystal_au = { "PM_CRYSTAL\0BYTU", 15, 0x01 }, red_jp = { "POKEMON RED", 11, 0x00 },
                      gold_jp = { "POKEMON_GLDAAUJ", 15, 0x00 }, longer = { "POKEMON REDX", 12, 0x01 },
                      other_code = { "POKEMON_GLDAAUD", 15, 0x01 };
    int fails = 0;
    fails += expect_titled(0x13, &red, "Red.gb", NULL, SIGIL_OK, GEN1, SIGIL_PLATFORM_GB, "red");
    fails += expect_titled(0x13, &blue, "Blue.gb", NULL, SIGIL_OK, GEN1, SIGIL_PLATFORM_GB, "blue");
    fails += expect_titled(0x1B, &yellow, "Yellow.gbc", NULL, SIGIL_OK, GEN1, SIGIL_PLATFORM_GBC, "yellow");
    fails += expect_titled(0x10, &gold, "Gold.gbc", NULL, SIGIL_OK, RTC | GS, SIGIL_PLATFORM_GBC, "gold");
    fails += expect_titled(0x10, &silver, "Silver.gbc", NULL, SIGIL_OK, RTC | GS, SIGIL_PLATFORM_GBC, "silver");
    fails += expect_titled(0x10, &crystal, "Crystal.gbc", NULL, SIGIL_OK, RTC | C, SIGIL_PLATFORM_GBC, "crystal");
    fails += expect_titled(0x10, &crystal_au, "Crystal.gbc", NULL, SIGIL_OK, RTC | C, SIGIL_PLATFORM_GBC,
                           "crystal australia");
    fails += expect_titled(0x13, &red_jp, "Red.gb", NULL, SIGIL_OK, 0, SIGIL_PLATFORM_GB, "japanese destination");
    fails += expect_titled(0x10, &gold_jp, "Gold.gbc", NULL, SIGIL_OK, RTC, SIGIL_PLATFORM_GBC, "japanese gold");
    fails += expect_titled(0x13, &longer, "Hack.gb", NULL, SIGIL_OK, 0, SIGIL_PLATFORM_GB, "title runs longer");
    fails += expect_titled(0x10, &other_code, "Gold.gbc", NULL, SIGIL_OK, RTC, SIGIL_PLATFORM_GBC,
                           "other manufacturer code");
    return fails;
}

int main(void) {
    int fails = 0;
    const uint32_t RTC = SIGIL_FEATURE_RTC, MBC2 = SIGIL_FEATURE_MBC2;
    fails += expect(0x10, "Pokemon - Crystal Version (USA).gbc", NULL, SIGIL_OK, RTC, SIGIL_PLATFORM_GBC, "mbc3 timer ram battery");
    fails += expect(0x0F, "clock.gb", NULL, SIGIL_OK, RTC, SIGIL_PLATFORM_GB, "mbc3 timer battery");
    fails += expect(0xFE, "Robopon - Sun Version (USA).gbc", "gbc", SIGIL_OK, RTC, SIGIL_PLATFORM_GBC, "huc3");
    fails += expect(0xFD, "tamagotchi.gb", NULL, SIGIL_OK, RTC, SIGIL_PLATFORM_GB, "tama5");
    fails += expect(0x13, "Pokemon - Yellow Version (USA).gbc", NULL, SIGIL_OK, 0, SIGIL_PLATFORM_GBC, "mbc3 ram battery, no timer");
    fails += expect(0x1B, "Pokemon - Red Version (USA).gb", NULL, SIGIL_OK, 0, SIGIL_PLATFORM_GB, "mbc5 ram battery");
    fails += expect(0x00, "Tetris (World).gb", NULL, SIGIL_OK, 0, SIGIL_PLATFORM_GB, "rom only");
    fails += expect(0x10, "clock.gb", "gbc", SIGIL_OK, RTC, SIGIL_PLATFORM_GBC, "platform hint wins over extension");
    fails += expect(0x06, "Final Fantasy Legend, The (USA).gb", NULL, SIGIL_OK, MBC2, SIGIL_PLATFORM_GB, "mbc2 battery");
    fails += expect(0x05, "mbc2.gb", NULL, SIGIL_OK, MBC2, SIGIL_PLATFORM_GB, "mbc2");
    fails += scratch_titles();

    {
        uint8_t rom[ROM_LEN];
        write_header(rom, 0x10, &JAPANESE_CRYSTAL);
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
