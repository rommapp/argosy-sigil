// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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

/* WBFS container header followed by the wrapped disc header one hd sector in. */
static void build_wbfs(uint8_t *buf, size_t len, const char *game_id, bool with_magic) {
    memset(buf, 0, len);
    memcpy(buf, "WBFS", 4);
    write_be32(buf + 4, 0x1000);
    buf[8] = 9;                              /* hd_sec_sz_s -> 512 */
    buf[9] = 21;                             /* wbfs_sec_sz_s */
    memcpy(buf + HD_SEC_SZ, game_id, 4);
    if (with_magic) write_be32(buf + HD_SEC_SZ + 0x18, 0x5D1C9EA3u);
}

static int expect_id(const uint8_t *buf, size_t len, const char *filename,
                     const char *want_id, const char *want_raw,
                     const char *want_save, const char *label) {
    mem_ctx ctx = { buf, len };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, filename, SIGIL_PLATFORM_WII, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL %s: rc=%d\n", label, rc);
        return 1;
    }
    if (strcmp(r.title_id, want_id) != 0 || strcmp(r.raw_serial, want_raw) != 0) {
        fprintf(stderr, "FAIL %s: got title_id='%s' raw='%s'\n", label, r.title_id, r.raw_serial);
        return 1;
    }
    if (strcmp(r.save_id, want_save) != 0) {
        fprintf(stderr, "FAIL %s: got save_id='%s' (want '%s')\n", label, r.save_id, want_save);
        return 1;
    }
    if (r.usage != SIGIL_USAGE_FOLDER_EXACT) {
        fprintf(stderr, "FAIL %s: usage=%d\n", label, (int)r.usage);
        return 1;
    }
    return 0;
}

int main(void) {
    uint8_t buf[2048];

    /* Tales of Symphonia: Dawn of the New World, the WBFS conversion we
     * validated on-device. RT4E -> 52543445, matching its RVZ dump. */
    build_wbfs(buf, sizeof(buf), "RT4E", true);
    if (expect_id(buf, sizeof(buf), "game.wbfs", "52543445", "RT4E", "52543445", "wbfs")) return 1;

    /* Without the Wii magic there is no disc header to trust. Reading offset 0
     * anyway would yield the container's own "WBFS" as a game id. */
    build_wbfs(buf, sizeof(buf), "RT4E", false);
    {
        mem_ctx ctx = { buf, sizeof(buf) };
        sigil_io io = { mem_read, mem_size, NULL, &ctx };
        sigil_result r;
        int rc = sigil_extract_from_io(&io, "game.wbfs", SIGIL_PLATFORM_WII, NULL, &r);
        if (rc == SIGIL_OK) {
            fprintf(stderr, "FAIL headerless wbfs: accepted title_id='%s'\n", r.title_id);
            return 1;
        }
    }

    /* Raw ISO and RVZ paths must keep working. */
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "RT4E", 4);
    write_be32(buf + 0x18, 0x5D1C9EA3u);
    if (expect_id(buf, sizeof(buf), "game.iso", "52543445", "RT4E", "52543445", "iso")) return 1;

    memset(buf, 0, sizeof(buf));
    memcpy(buf, "RVZ\x01", 4);
    memcpy(buf + 0x58, "RT4E", 4);
    if (expect_id(buf, sizeof(buf), "game.rvz", "52543445", "RT4E", "52543445", "rvz")) return 1;

    /* RT4E hexes to digits only, so it cannot catch a case bug. Twilight
     * Princess (RZDE) hexes to 525A4445: Dolphin creates Wii/title/00010000/
     * 525a4445, and title_id must stay uppercase while save_id follows it. */
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "RZDE", 4);
    write_be32(buf + 0x18, 0x5D1C9EA3u);
    if (expect_id(buf, sizeof(buf), "game.iso", "525A4445", "RZDE", "525a4445", "hex letters")) {
        return 1;
    }

    build_wbfs(buf, sizeof(buf), "RZDE", true);
    if (expect_id(buf, sizeof(buf), "game.wbfs", "525A4445", "RZDE", "525a4445",
                  "hex letters (wbfs)")) {
        return 1;
    }

    /* GameCube shares the extractor but not the NAND layout: its artifacts are
     * .gci files matched by prefix, so it keeps the uppercase default. */
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "GZLE", 4);
    write_be32(buf + 0x1C, 0xC2339F3Du);
    {
        mem_ctx ctx = { buf, sizeof(buf) };
        sigil_io io = { mem_read, mem_size, NULL, &ctx };
        sigil_result r;
        int rc = sigil_extract_from_io(&io, "game.iso", SIGIL_PLATFORM_GAMECUBE, NULL, &r);
        if (rc != SIGIL_OK) {
            fprintf(stderr, "FAIL gamecube: rc=%d\n", rc);
            return 1;
        }
        if (strcmp(r.title_id, "475A4C45") != 0 || strcmp(r.save_id, "475A4C45") != 0) {
            fprintf(stderr, "FAIL gamecube: title_id='%s' save_id='%s'\n",
                    r.title_id, r.save_id);
            return 1;
        }
    }

#ifdef SIGIL_TEST_FILENAME_FALLBACK
    /* The filename fallback reaches the same field, so a container sigil cannot
     * parse must still yield the lowercase directory name. */
    memset(buf, 0, sizeof(buf));
    {
        mem_ctx ctx = { buf, sizeof(buf) };
        sigil_io io = { mem_read, mem_size, NULL, &ctx };
        sigil_result r;
        int rc = sigil_extract_from_io(&io, "Twilight Princess [RZDE].iso",
                                       SIGIL_PLATFORM_WII, NULL, &r);
        if (rc != SIGIL_OK) {
            fprintf(stderr, "FAIL filename fallback: rc=%d\n", rc);
            return 1;
        }
        if (strcmp(r.title_id, "525A4445") != 0 || strcmp(r.save_id, "525a4445") != 0
            || r.source != SIGIL_SOURCE_FILENAME) {
            fprintf(stderr, "FAIL filename fallback: title_id='%s' save_id='%s' source=%d\n",
                    r.title_id, r.save_id, (int)r.source);
            return 1;
        }
    }
#endif

    printf("ok unit_wii_wbfs\n");
    return 0;
}
