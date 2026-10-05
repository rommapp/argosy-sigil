// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include "sigil_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_LEN 0x1000

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

typedef enum { ORDER_Z64, ORDER_V64, ORDER_N64 } byte_order;

/* A big-endian (.z64) cart: the PI magic, the title at 0x20, the game code at
 * 0x3B (category, two-letter id, region) and the version at 0x3F. */
static void build_z64(uint8_t *rom, const char *code, uint8_t version) {
    memset(rom, 0, ROM_LEN);
    const uint8_t magic[4] = { 0x80, 0x37, 0x12, 0x40 };
    memcpy(rom, magic, 4);
    memcpy(rom + 0x20, "SUPER MARIO 64      ", 20);
    memcpy(rom + 0x3B, code, 4);
    rom[0x3F] = version;
}

/* The same bytes as a .v64 (16-bit words swapped) or .n64 (32-bit words reversed) dump. */
static void reorder(uint8_t *rom, byte_order order) {
    for (size_t i = 0; order != ORDER_Z64 && i < ROM_LEN; i += 4) {
        uint8_t w[4] = { rom[i], rom[i + 1], rom[i + 2], rom[i + 3] };
        if (order == ORDER_V64) {
            rom[i] = w[1]; rom[i + 1] = w[0]; rom[i + 2] = w[3]; rom[i + 3] = w[2];
        } else {
            rom[i] = w[3]; rom[i + 1] = w[2]; rom[i + 2] = w[1]; rom[i + 3] = w[0];
        }
    }
}

/* The MD5 of `rom` (a .z64 image) as `order` lays it out, in uppercase hex. */
static void md5_as(const uint8_t *rom, byte_order order, char out[33]) {
    uint8_t copy[ROM_LEN];
    memcpy(copy, rom, ROM_LEN);
    reorder(copy, order);
    sigil_md5_of(copy, ROM_LEN, out);
    for (char *c = out; *c; c++) *c = (char)toupper((unsigned char)*c);
}

static int extract(const uint8_t *rom, const char *name, sigil_platform hint, sigil_result *r) {
    mem_ctx ctx = { rom, ROM_LEN };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_options opts = { SIGIL_OPTIONS_V1, NULL, 0 };
    return sigil_extract_from_io(&io, name, hint, &opts, r);
}

static int expect_code(const char *label, byte_order order, const char *name, sigil_platform hint, const char *code,
                       const char *want_id) {
    uint8_t rom[ROM_LEN];
    build_z64(rom, code, 0x01);
    reorder(rom, order);
    sigil_result r;
    int rc = extract(rom, name, hint, &r);
    if (rc != SIGIL_OK || r.platform != SIGIL_PLATFORM_N64 || strcmp(r.title_id, want_id) != 0 ||
        strcmp(r.raw_serial, want_id) != 0 || strcmp(r.save_id, want_id) != 0 || r.usage != SIGIL_USAGE_FILE_PREFIX ||
        r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "FAIL %s: rc=%d platform=%d title_id='%s' raw='%s' save_id='%s' usage=%d\n", label, rc,
                (int)r.platform, r.title_id, r.raw_serial, r.save_id, (int)r.usage);
        return 1;
    }
    uint8_t z64[ROM_LEN];
    build_z64(z64, code, 0x01);
    char want_md5[33], want_md5_n64[33];
    md5_as(z64, ORDER_Z64, want_md5);
    md5_as(z64, ORDER_N64, want_md5_n64);
    if (r.struct_version < SIGIL_RESULT_V4 || strcmp(r.n64_header, "SUPER MARIO 64") != 0 ||
        strcmp(r.n64_md5, want_md5) != 0 || strcmp(r.n64_md5_n64, want_md5_n64) != 0) {
        fprintf(stderr, "FAIL %s: header='%s' md5=%s (want %s) md5_n64=%s (want %s)\n", label, r.n64_header, r.n64_md5,
                want_md5, r.n64_md5_n64, want_md5_n64);
        return 1;
    }
    return 0;
}

/* A header name outside printable ASCII (half-width katakana on Japanese
 * carts) has no single spelling on disk, so it is left empty. */
static int expect_header(const char *label, const uint8_t name[20], const char *want) {
    uint8_t rom[ROM_LEN];
    build_z64(rom, "NSMJ", 0);
    memcpy(rom + 0x20, name, 20);
    sigil_result r;
    if (extract(rom, "game.z64", SIGIL_PLATFORM_AUTO, &r) != SIGIL_OK || strcmp(r.n64_header, want) != 0) {
        fprintf(stderr, "FAIL %s: header='%s' want '%s'\n", label, r.n64_header, want);
        return 1;
    }
    return 0;
}

int main(void) {
    int fails = 0;
    fails += expect_code("z64 by extension", ORDER_Z64, "Super Mario 64 (USA).z64", SIGIL_PLATFORM_AUTO, "NSME", "NSME");
    fails += expect_code("v64 by extension", ORDER_V64, "Super Mario 64 (USA).v64", SIGIL_PLATFORM_AUTO, "NSME", "NSME");
    fails += expect_code("n64 by extension", ORDER_N64, "Super Mario 64 (USA).n64", SIGIL_PLATFORM_AUTO, "NSME", "NSME");
    fails += expect_code("v64 named n64 by the caller", ORDER_V64, "game.bin", SIGIL_PLATFORM_N64, "NTEA", "NTEA");
    fails += expect_code("homebrew with no game code", ORDER_Z64, "demo.z64", SIGIL_PLATFORM_AUTO, "\0\0\0\0", "");
    fails += expect_code("a game code with a byte outside letters and digits", ORDER_Z64, "odd.z64",
                         SIGIL_PLATFORM_AUTO, "N\x01ME", "");

    fails += expect_header("katakana header", (const uint8_t *)"\xc4\xde\xd7\xb4\xd3\xdd   \0\0\0\0\0\0\0\0\0\0\0", "");
    fails += expect_header("header padded with NULs", (const uint8_t *)"ZELDA\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", "ZELDA");
    fails += expect_header("header with inner spaces", (const uint8_t *)"THE LEGEND OF ZELDA ", "THE LEGEND OF ZELDA");

    uint8_t rom[ROM_LEN];
    memset(rom, 0xFF, sizeof(rom));
    sigil_result r;
    if (extract(rom, "junk.z64", SIGIL_PLATFORM_AUTO, &r) != SIGIL_ERR_UNSUPPORTED_FORMAT) {
        fprintf(stderr, "FAIL a file without the cart magic was accepted\n");
        fails++;
    }
    if (sigil_platform_from_slug("n64") != SIGIL_PLATFORM_N64 || strcmp(sigil_platform_to_slug(SIGIL_PLATFORM_N64), "n64")) {
        fprintf(stderr, "FAIL slug\n");
        fails++;
    }

    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("unit_n64: ok\n");
    return 0;
}
