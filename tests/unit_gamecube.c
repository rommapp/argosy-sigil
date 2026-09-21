// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdio.h>
#include <string.h>

#define HD_SEC_SZ 512

static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

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

static int expect(const uint8_t *buf, size_t len, const char *filename,
                  const char *want_title, const char *want_raw,
                  const char *want_save, sigil_source want_source,
                  const char *label) {
    mem_ctx ctx = { buf, len };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, filename, SIGIL_PLATFORM_GAMECUBE, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL %s: rc=%d\n", label, rc);
        return 1;
    }
    if (strcmp(r.title_id, want_title) != 0 || strcmp(r.raw_serial, want_raw) != 0) {
        fprintf(stderr, "FAIL %s: title_id='%s' raw_serial='%s'\n",
                label, r.title_id, r.raw_serial);
        return 1;
    }
    if (strcmp(r.save_id, want_save) != 0) {
        fprintf(stderr, "FAIL %s: save_id='%s' (want '%s')\n", label, r.save_id, want_save);
        return 1;
    }
    if (r.usage != SIGIL_USAGE_FILE_PREFIX) {
        fprintf(stderr, "FAIL %s: usage=%d (want FILE_PREFIX)\n", label, (int)r.usage);
        return 1;
    }
    if (r.source != want_source) {
        fprintf(stderr, "FAIL %s: source=%d\n", label, (int)r.source);
        return 1;
    }
    return 0;
}

/* Dolphin writes GameCube memory cards as `<maker>-<gameId>-<internal>.gci`
 * under `<region>/Card A/`, where gameId is the four ASCII characters of the
 * disc header. A consumer locating one matches on those characters, so save_id
 * is the ASCII id and not the hex rendering title_id carries. */
int main(void) {
    uint8_t buf[2048];

    memset(buf, 0, sizeof(buf));
    memcpy(buf, "GZLE", 4);
    write_be32(buf + 0x1C, 0xC2339F3Du);
    if (expect(buf, sizeof(buf), "game.iso", "475A4C45", "GZLE", "GZLE",
               SIGIL_SOURCE_BINARY, "iso")) {
        return 1;
    }

    memset(buf, 0, sizeof(buf));
    memcpy(buf, "RVZ\x01", 4);
    memcpy(buf + 0x58, "GAFE", 4);
    if (expect(buf, sizeof(buf), "game.rvz", "47414645", "GAFE", "GAFE",
               SIGIL_SOURCE_BINARY, "rvz")) {
        return 1;
    }

    memset(buf, 0, sizeof(buf));
    memcpy(buf, "WBFS", 4);
    write_be32(buf + 4, 0x1000);
    buf[8] = 9;
    buf[9] = 21;
    memcpy(buf + HD_SEC_SZ, "GM4E", 4);
    write_be32(buf + HD_SEC_SZ + 0x1C, 0xC2339F3Du);
    if (expect(buf, sizeof(buf), "game.wbfs", "474D3445", "GM4E", "GM4E",
               SIGIL_SOURCE_BINARY, "wbfs")) {
        return 1;
    }

    memset(buf, 0, sizeof(buf));
    if (expect(buf, sizeof(buf), "Animal Crossing [GAFE].iso", "47414645", "GAFE", "GAFE",
               SIGIL_SOURCE_FILENAME, "filename fallback")) {
        return 1;
    }

    printf("ok unit_gamecube\n");
    return 0;
}
