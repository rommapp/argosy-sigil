// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Synthetic SNES images with one internal header whose checksum and
 * complement agree, placed at the LoROM, HiROM or ExHiROM base, with and
 * without a 512-byte copier header. */

#define LOROM_BASE   0x7FC0
#define HIROM_BASE   0xFFC0
#define EXHIROM_BASE 0x40FFC0
#define ROM_SPEED    0x15
#define ROM_TYPE     0x16
#define COMPLEMENT   0x1C
#define CHECKSUM     0x1E

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

static void write_header(uint8_t *rom, size_t base, uint8_t speed, uint8_t type) {
    uint8_t *h = rom + base;
    memcpy(h, "TEST CARTRIDGE       ", 21);
    h[ROM_SPEED] = speed;
    h[ROM_TYPE]  = type;
    h[COMPLEMENT]     = 0x34;
    h[COMPLEMENT + 1] = 0x12;
    h[CHECKSUM]       = 0xCB;
    h[CHECKSUM + 1]   = 0xED;
}

static int expect(size_t rom_len, size_t skew, size_t base, uint8_t speed, uint8_t type,
                  int want_rc, int want_rtc, const char *label) {
    size_t len = rom_len + skew;
    uint8_t *rom = (uint8_t *)calloc(len, 1);
    if (!rom) return 1;
    memset(rom, 0xFF, len);
    write_header(rom, base + skew, speed, type);

    mem_ctx ctx = { rom, len };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, "game.sfc", SIGIL_PLATFORM_SNES, NULL, &r);
    free(rom);

    if (rc != want_rc) {
        fprintf(stderr, "FAIL %s: rc=%d want %d\n", label, rc, want_rc);
        return 1;
    }
    if (rc != SIGIL_OK) return 0;
    int has_rtc = (r.features & SIGIL_FEATURE_RTC) ? 1 : 0;
    if (has_rtc != want_rtc || r.platform != SIGIL_PLATFORM_SNES
        || r.usage != SIGIL_USAGE_FILE_PREFIX || r.title_id[0] != '\0') {
        fprintf(stderr, "FAIL %s: rtc=%d platform=%d usage=%d title='%s'\n",
                label, has_rtc, (int)r.platform, (int)r.usage, r.title_id);
        return 1;
    }
    return 0;
}

int main(void) {
    int fails = 0;
    const size_t MB = 1024 * 1024;

    fails += expect(5 * MB, 0, EXHIROM_BASE, 0x35, 0x55, SIGIL_OK, 1, "exhirom s-rtc (Daikaijuu Monogatari II)");
    fails += expect(4 * MB, 0, HIROM_BASE, 0x3A, 0xF9, SIGIL_OK, 1, "hirom spc7110 rtc (Tengai Makyou Zero)");
    fails += expect(4 * MB, 0, HIROM_BASE, 0x3A, 0xF5, SIGIL_OK, 0, "hirom spc7110 without rtc");
    fails += expect(3 * MB, 0, HIROM_BASE, 0x31, 0x02, SIGIL_OK, 0, "hirom ram battery (Daikaijuu Monogatari)");
    fails += expect(512 * 1024, 0, LOROM_BASE, 0x20, 0x02, SIGIL_OK, 0, "lorom ram battery (F-Zero)");
    fails += expect(5 * MB, 512, EXHIROM_BASE, 0x35, 0x55, SIGIL_OK, 1, "exhirom s-rtc behind a copier header");
    fails += expect(512 * 1024, 512, LOROM_BASE, 0x20, 0x02, SIGIL_OK, 0, "lorom behind a copier header");

    {
        size_t len = 512 * 1024;
        uint8_t *rom = (uint8_t *)malloc(len);
        memset(rom, 0xFF, len);
        mem_ctx ctx = { rom, len };
        sigil_io io = { mem_read, mem_size, NULL, &ctx };
        sigil_result r;
        sigil_options opts = { SIGIL_OPTIONS_V1, NULL, 0 };
        int rc = sigil_extract_from_io(&io, "junk.sfc", SIGIL_PLATFORM_SNES, &opts, &r);
        free(rom);
        if (rc == SIGIL_OK) {
            fprintf(stderr, "FAIL image without a valid header accepted\n");
            fails++;
        }
    }

    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("unit_snes: ok\n");
    return 0;
}
