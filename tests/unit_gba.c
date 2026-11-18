// SPDX-License-Identifier: MPL-2.0
/* GBA identification: the header check, and the save chip and clock the
 * ROM's Nintendo library ids name. */
#include "sigil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_LEN    0x40000
#define BIG_LEN    (0x100000 + 0x8000)   /* past the reader's 1 MiB chunk */
#define COMPLEMENT 0xBD

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

static void write_header(uint8_t *rom, size_t len) {
    memset(rom, 0, len);
    memcpy(rom + 0xA0, "POKEMON EMER", 12);
    memcpy(rom + 0xAC, "BPEE", 4);
    memcpy(rom + 0xB0, "01", 2);
    rom[0xB2] = 0x96;
    uint8_t x = 0;
    for (int i = 0xA0; i < COMPLEMENT; i++) x = (uint8_t)(x - rom[i]);
    rom[COMPLEMENT] = (uint8_t)(x - 0x19);
}

static int extract(const uint8_t *rom, size_t len, const char *name, const char *slug, const sigil_options *opts,
                   sigil_result *r) {
    mem_ctx ctx = { rom, len };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    return sigil_extract_from_io(&io, name, sigil_platform_from_slug(slug), opts, r);
}

/* `ids` are placed in the ROM one after another from 0x1000, each NUL-terminated. */
static int expect(const char *const *ids, size_t count, uint32_t want, const char *label) {
    uint8_t *rom = (uint8_t *)malloc(ROM_LEN);
    write_header(rom, ROM_LEN);
    size_t at = 0x1000;
    for (size_t i = 0; i < count; i++) {
        memcpy(rom + at, ids[i], strlen(ids[i]));
        at += strlen(ids[i]) + 1;
    }
    sigil_result r;
    int rc = extract(rom, ROM_LEN, "Game.gba", NULL, NULL, &r);
    free(rom);
    if (rc != SIGIL_OK || r.features != want || r.platform != SIGIL_PLATFORM_GBA ||
        r.usage != SIGIL_USAGE_FILE_PREFIX || r.source != SIGIL_SOURCE_BINARY || r.title_id[0] || r.save_id[0]) {
        fprintf(stderr, "FAIL %s: rc=%d features=%#x platform=%d usage=%d source=%d\n", label, rc, r.features,
                (int)r.platform, (int)r.usage, (int)r.source);
        return 1;
    }
    return 0;
}

int main(void) {
    int fails = 0;
    const uint32_t RTC = SIGIL_FEATURE_RTC, SRAM = SIGIL_FEATURE_GBA_SRAM, FLASH = SIGIL_FEATURE_GBA_FLASH,
                   FLASH1M = SIGIL_FEATURE_GBA_FLASH1M, EEPROM = SIGIL_FEATURE_GBA_EEPROM;

    const char *sram[] = { "SRAM_V113" }, *sram_f[] = { "SRAM_F_V100" }, *flash[] = { "FLASH_V126" },
               *flash512[] = { "FLASH512_V131" }, *eeprom[] = { "EEPROM_V124" },
               *emerald[] = { "FLASH1M_V103", "SIIRTC_V001" }, *boktai[] = { "EEPROM_V122", "SIIRTC_V001" },
               *two[] = { "EEPROM_V124", "SRAM_V113", "SIIRTC_V001" }, *same_kind[] = { "FLASH_V126", "FLASH512_V131" },
               *no_digits[] = { "EEPROM_V", "SRAM_Vx13" };
    fails += expect(sram, 1, SRAM, "SRAM_V");
    fails += expect(sram_f, 1, SRAM, "SRAM_F_V");
    fails += expect(flash, 1, FLASH, "FLASH_V");
    fails += expect(flash512, 1, FLASH, "FLASH512_V");
    fails += expect(eeprom, 1, EEPROM, "EEPROM_V");
    fails += expect(emerald, 2, FLASH1M | RTC, "FLASH1M_V and the clock");
    fails += expect(boktai, 2, EEPROM | RTC, "EEPROM_V and the clock");
    fails += expect(same_kind, 2, FLASH, "two ids for one kind of chip");
    fails += expect(two, 3, RTC, "two kinds of chip name none, the clock stays");
    fails += expect(no_digits, 2, 0, "an id needs its three-digit version");
    fails += expect(NULL, 0, 0, "no save library");

    /* An id the reader's 1 MiB chunks split is still found. */
    {
        uint8_t *rom = (uint8_t *)malloc(BIG_LEN);
        write_header(rom, BIG_LEN);
        memcpy(rom + 0x100000 - 5, "FLASH1M_V103", 12);
        sigil_result r;
        if (extract(rom, BIG_LEN, "Game.gba", "gba", NULL, &r) != SIGIL_OK || r.features != FLASH1M) {
            fprintf(stderr, "FAIL an id across the chunk boundary: features=%#x\n", r.features);
            fails++;
        }
        memset(rom + 0x100000 - 5, 0, 12);
        memcpy(rom + BIG_LEN - 11, "SRAM_V113", 9);
        if (extract(rom, BIG_LEN, "Game.gba", "gba", NULL, &r) != SIGIL_OK || r.features != SRAM) {
            fprintf(stderr, "FAIL an id at the ROM's end: features=%#x\n", r.features);
            fails++;
        }
        free(rom);
    }

    /* The slug and the extension both reach the reader. */
    if (sigil_platform_from_slug("gba") != SIGIL_PLATFORM_GBA ||
        strcmp(sigil_platform_to_slug(SIGIL_PLATFORM_GBA), "gba") != 0) {
        fprintf(stderr, "FAIL gba slug\n");
        fails++;
    }

    /* A header whose complement check fails isn't a GBA header. */
    {
        uint8_t *rom = (uint8_t *)malloc(ROM_LEN);
        write_header(rom, ROM_LEN);
        rom[COMPLEMENT] ^= 0xFF;
        sigil_options opts = { SIGIL_OPTIONS_V1, NULL, 0 };
        sigil_result r;
        int rc = extract(rom, ROM_LEN, "junk.gba", NULL, &opts, &r);
        if (rc != SIGIL_ERR_UNSUPPORTED_FORMAT) {
            fprintf(stderr, "FAIL bad complement: rc=%d\n", rc);
            fails++;
        }
        write_header(rom, ROM_LEN);
        rom[0xB2] = 0;
        rc = extract(rom, ROM_LEN, "junk.gba", NULL, &opts, &r);
        if (rc != SIGIL_ERR_UNSUPPORTED_FORMAT) {
            fprintf(stderr, "FAIL fixed byte: rc=%d\n", rc);
            fails++;
        }
        free(rom);
    }

    printf("gba: %d failures\n", fails);
    return fails ? 1 : 0;
}
