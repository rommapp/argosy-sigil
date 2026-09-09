// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CERT_SIZE   0x0A00
#define TICKET_SIZE 0x02A4
#define TMD_SIZE    0x0208
#define WAD_LEN     0x2000

static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static size_t align64(size_t v) { return (v + 63) & ~(size_t)63; }

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

static void build_wad(uint8_t *buf, uint16_t type, uint32_t ticket_size,
                      const uint8_t ticket_id[8], const uint8_t tmd_id[8]) {
    memset(buf, 0, WAD_LEN);
    write_be32(buf + 0x00, 0x20);
    buf[0x04] = (uint8_t)(type >> 8);
    buf[0x05] = (uint8_t)type;
    write_be32(buf + 0x08, CERT_SIZE);
    write_be32(buf + 0x10, ticket_size);
    write_be32(buf + 0x14, TMD_SIZE);

    size_t ticket_off = align64(0x20) + align64(CERT_SIZE);
    if (ticket_size > 0 && ticket_id) memcpy(buf + ticket_off + 0x1DC, ticket_id, 8);
    size_t tmd_off = align64(ticket_off + ticket_size);
    if (tmd_id) memcpy(buf + tmd_off + 0x18C, tmd_id, 8);
}

static int run(const uint8_t *buf, const char *filename, sigil_result *r) {
    mem_ctx ctx = { buf, WAD_LEN };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_platform hint = strstr(filename, ".wad") ? SIGIL_PLATFORM_AUTO : SIGIL_PLATFORM_WII;
    return sigil_extract_from_io(&io, filename, hint, NULL, r);
}

static int expect(const uint8_t *buf, const char *want_id, const char *want_raw,
                  const char *want_save, const char *label) {
    sigil_result r;
    int rc = run(buf, "channel.wad", &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL %s: rc=%d\n", label, rc);
        return 1;
    }
    if (r.platform != SIGIL_PLATFORM_WII || r.usage != SIGIL_USAGE_FOLDER_SPLIT || r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "FAIL %s: platform=%d usage=%d source=%d\n", label, (int)r.platform, (int)r.usage, (int)r.source);
        return 1;
    }
    if (strcmp(r.title_id, want_id) != 0 || strcmp(r.raw_serial, want_raw) != 0 || strcmp(r.save_id, want_save) != 0) {
        fprintf(stderr, "FAIL %s: title_id='%s' raw='%s' save_id='%s'\n", label, r.title_id, r.raw_serial, r.save_id);
        return 1;
    }
    return 0;
}

static int expect_rc(const uint8_t *buf, const char *filename, int want, const char *label) {
    sigil_result r;
    int rc = run(buf, filename, &r);
    if (rc != want) {
        fprintf(stderr, "FAIL %s: rc=%d want %d\n", label, rc, want);
        return 1;
    }
    return 0;
}

int main(void) {
    static uint8_t buf[WAD_LEN];
    int fails = 0;

    const uint8_t wiiware[8] = { 0x00, 0x01, 0x00, 0x01, 'W', 'K', 'T', 'E' };
    const uint8_t channel[8] = { 0x00, 0x01, 0x00, 0x04, 'R', 'F', 'N', 'E' };
    const uint8_t ios[8]     = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x21 };
    const uint8_t other[8]   = { 0x00, 0x01, 0x00, 0x01, 'Z', 'Z', 'Z', 'Z' };

    build_wad(buf, 0x4973, TICKET_SIZE, wiiware, other);
    fails += expect(buf, "00010001574B5445", "WKTE", "00010001/574b5445", "ticket id wins");

    build_wad(buf, 0x4973, 0, NULL, wiiware);
    fails += expect(buf, "00010001574B5445", "WKTE", "00010001/574b5445", "tmd fallback without ticket");

    build_wad(buf, 0x4973, TICKET_SIZE, ios, wiiware);
    fails += expect(buf, "00010001574B5445", "WKTE", "00010001/574b5445", "tmd fallback past a non-ascii ticket id");

    build_wad(buf, 0x426B, TICKET_SIZE, channel, NULL);
    fails += expect(buf, "0001000452464E45", "RFNE", "00010004/52464e45", "backup type keeps the category half");

    build_wad(buf, 0x4973, TICKET_SIZE, ios, ios);
    fails += expect_rc(buf, "ios.wad", SIGIL_ERR_NOT_FOUND, "system title has no game id");

    build_wad(buf, 0x5858, TICKET_SIZE, wiiware, wiiware);
    fails += expect_rc(buf, "notes.wad", SIGIL_ERR_NOT_FOUND, "unknown wad type is not a wad");

    memset(buf, 0, WAD_LEN);
    memcpy(buf, "RZTE", 4);
    fails += expect_rc(buf, "disc.iso", SIGIL_OK, "disc header still resolves");

    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("unit_wii_wad: ok\n");
    return 0;
}
