// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR       2048
#define PRODUCT_OFF  0x40
#define PRODUCT_LEN  10
#define TRACK3_LBA   601

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

/* IP.BIN at `lba`: hardware id at 0x00, 10 raw product bytes at 0x40. */
static void write_ip_bin(uint8_t *buf, size_t lba, const uint8_t product[PRODUCT_LEN]) {
    uint8_t *s = buf + lba * SECTOR;
    memcpy(s, "SEGA SEGAKATANA ", 16);
    memcpy(s + 0x10, "SEGA ENTERPRISES", 16);
    memcpy(s + PRODUCT_OFF, product, PRODUCT_LEN);
}

static int expect_product(const uint8_t *buf, size_t len, const char *filename,
                          sigil_platform hint, const char *want, const char *label) {
    mem_ctx ctx = { buf, len };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, filename, hint, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL %s: rc=%d\n", label, rc);
        return 1;
    }
    if (strcmp(r.title_id, want) != 0 || strcmp(r.raw_serial, want) != 0
        || strcmp(r.save_id, want) != 0) {
        fprintf(stderr, "FAIL %s: title_id='%s' raw='%s' save_id='%s'\n",
                label, r.title_id, r.raw_serial, r.save_id);
        return 1;
    }
    if (r.platform != SIGIL_PLATFORM_DREAMCAST || r.usage != SIGIL_USAGE_FILE_PREFIX
        || r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "FAIL %s: platform=%d usage=%d source=%d\n",
                label, (int)r.platform, (int)r.usage, (int)r.source);
        return 1;
    }
    return 0;
}

static int expect_reject(const uint8_t *buf, size_t len, const char *label) {
    mem_ctx ctx = { buf, len };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, "game.chd", SIGIL_PLATFORM_DREAMCAST, NULL, &r);
    if (rc == SIGIL_OK) {
        fprintf(stderr, "FAIL %s: accepted title_id='%s'\n", label, r.title_id);
        return 1;
    }
    return 0;
}

int main(void) {
    size_t len = (TRACK3_LBA + 4) * SECTOR;
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) return 1;

    if (sigil_platform_from_slug("dc") != SIGIL_PLATFORM_DREAMCAST
        || sigil_platform_from_slug("dreamcast") != SIGIL_PLATFORM_DREAMCAST
        || strcmp(sigil_platform_to_slug(SIGIL_PLATFORM_DREAMCAST), "dreamcast") != 0) {
        fprintf(stderr, "FAIL slug: dc/dreamcast do not resolve\n");
        free(buf);
        return 1;
    }

    /* Bare data track: IP.BIN at offset 0, product padded with spaces. */
    memset(buf, 0, len);
    write_ip_bin(buf, 0, (const uint8_t *)"T-8111N   ");
    if (expect_product(buf, len, "track03.bin", SIGIL_PLATFORM_DREAMCAST,
                       "T-8111N", "bare data track")) { free(buf); return 1; }

    /* GD-ROM in a CHD: tracks 1-2 first, data track a few thousand frames in. */
    memset(buf, 0, len);
    memcpy(buf, "\x01\x02\x03\x04", 4);
    write_ip_bin(buf, TRACK3_LBA, (const uint8_t *)"MK-51035  ");
    if (expect_product(buf, len, "game.chd", SIGIL_PLATFORM_DREAMCAST,
                       "MK-51035", "chd third track")) { free(buf); return 1; }

    /* Garbage after the terminator: flycast cuts at the first NUL. */
    memset(buf, 0, len);
    write_ip_bin(buf, 0, (const uint8_t *)"T-9701N\0" "XZ");
    if (expect_product(buf, len, "game.chd", SIGIL_PLATFORM_DREAMCAST,
                       "T-9701N", "nul truncation")) { free(buf); return 1; }

    /* Extension sniffing resolves Dreamcast without an explicit hint. */
    memset(buf, 0, len);
    write_ip_bin(buf, 0, (const uint8_t *)"HDR-0038  ");
    if (expect_product(buf, len, "game.cdi", SIGIL_PLATFORM_AUTO,
                       "HDR-0038", "cdi sniff")) { free(buf); return 1; }

    /* No IP.BIN anywhere: the bounded scan must give up, not invent an id. */
    memset(buf, 0x5A, len);
    if (expect_reject(buf, len, "no magic")) { free(buf); return 1; }

    /* Magic present but the product field is blank padding. */
    memset(buf, 0, len);
    write_ip_bin(buf, 0, (const uint8_t *)"          ");
    if (expect_reject(buf, len, "blank product")) { free(buf); return 1; }

    free(buf);
    printf("ok unit_dreamcast\n");
    return 0;
}
