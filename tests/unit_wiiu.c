// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal WUA / zArchive image: a name table, a two-node file tree whose only
 * root child is the `00050000<8 hex>` title directory, and the 144-byte footer
 * that points at both. Everything the extractor reads is uncompressed, so no
 * zstd payload is needed. */

#define WUA_MAGIC       0x169F52D6u
#define WUA_VERSION_1   0x61BF3A01u
#define WUA_FOOTER_SIZE 144
#define WUA_FILE_ENTRY  16

#define IMAGE_SIZE      512
#define NAMES_OFFSET    0
#define TREE_OFFSET     64

static const char TITLE_DIR_UPPER[] = "000500001010EC00";
static const char TITLE_DIR_LOWER[] = "000500001010ec00";
static const char EXPECT_TITLE[]    = "1010EC00";
static const char EXPECT_RAW[]      = "000500001010EC00";
static const char EXPECT_SAVE[]     = "1010ec00";

static int failures = 0;

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

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)(v & 0xFFFFFFFFu));
}

static void build_wua(uint8_t *buf, const char *title_dir) {
    size_t name_len = strlen(title_dir);
    memset(buf, 0, IMAGE_SIZE);

    buf[NAMES_OFFSET] = 0;
    buf[NAMES_OFFSET + 1] = (uint8_t)name_len;
    memcpy(buf + NAMES_OFFSET + 2, title_dir, name_len);
    size_t names_size = 2 + name_len;

    put_be32(buf + TREE_OFFSET, 0);
    put_be32(buf + TREE_OFFSET + 4, 1);
    put_be32(buf + TREE_OFFSET + 8, 1);
    put_be32(buf + TREE_OFFSET + WUA_FILE_ENTRY, 1);
    size_t tree_size = 2 * WUA_FILE_ENTRY;

    uint8_t *footer = buf + IMAGE_SIZE - WUA_FOOTER_SIZE;
    put_be64(footer + 0x20, NAMES_OFFSET);
    put_be64(footer + 0x28, names_size);
    put_be64(footer + 0x30, TREE_OFFSET);
    put_be64(footer + 0x38, tree_size);
    put_be32(footer + 0x88, WUA_VERSION_1);
    put_be32(footer + 0x8C, WUA_MAGIC);
}

static void expect_ids(const char *label, const uint8_t *buf, size_t len,
                       const char *filename, sigil_source want_source) {
    mem_ctx ctx = { buf, len };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, filename, SIGIL_PLATFORM_WIIU, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL %s: rc=%d (%s)\n", label, rc, sigil_strerror(rc));
        failures++;
        return;
    }
    if (strcmp(r.title_id, EXPECT_TITLE) != 0) {
        fprintf(stderr, "FAIL %s: title_id='%s' (want '%s')\n",
                label, r.title_id, EXPECT_TITLE);
        failures++;
        return;
    }
    if (strcmp(r.save_id, EXPECT_SAVE) != 0) {
        fprintf(stderr, "FAIL %s: save_id='%s' (want '%s')\n",
                label, r.save_id, EXPECT_SAVE);
        failures++;
        return;
    }
    if (r.usage != SIGIL_USAGE_FOLDER_EXACT || r.source != want_source) {
        fprintf(stderr, "FAIL %s: usage=%d source=%d\n", label, (int)r.usage, (int)r.source);
        failures++;
        return;
    }
    printf("ok  %s\n", label);
}

int main(void) {
    uint8_t buf[IMAGE_SIZE];

    /* Mario Kart 8 (EUR): Cemu creates mlc01/usr/save/00050000/1010ec00, so
     * save_id is lowercase while title_id and raw_serial keep the uppercase
     * form the archive and the community filenames use. */
    build_wua(buf, TITLE_DIR_UPPER);
    expect_ids("wua title dir", buf, sizeof(buf), "game.wua", SIGIL_SOURCE_BINARY);
    {
        mem_ctx ctx = { buf, sizeof(buf) };
        sigil_io io = { mem_read, mem_size, NULL, &ctx };
        sigil_result r;
        int rc = sigil_extract_from_io(&io, "game.wua", SIGIL_PLATFORM_WIIU, NULL, &r);
        if (rc != SIGIL_OK || strcmp(r.raw_serial, EXPECT_RAW) != 0) {
            fprintf(stderr, "FAIL raw_serial: rc=%d raw='%s'\n", rc, r.raw_serial);
            failures++;
        } else {
            printf("ok  raw_serial stays uppercase\n");
        }
    }

    /* An archive that already names the directory in lowercase must land on the
     * same three values, or the same game splits by container. */
    build_wua(buf, TITLE_DIR_LOWER);
    expect_ids("wua title dir already lowercase", buf, sizeof(buf), "game.wua",
               SIGIL_SOURCE_BINARY);

#ifdef SIGIL_TEST_FILENAME_FALLBACK
    /* The filename fallback reaches the same field and must agree with the
     * binary path; a bracketed id is the only source for a container sigil
     * cannot parse. */
    memset(buf, 0, sizeof(buf));
    expect_ids("filename fallback", buf, sizeof(buf),
               "Mario Kart 8 [000500001010EC00].wua", SIGIL_SOURCE_FILENAME);
#endif

    if (failures) {
        fprintf(stderr, "unit_wiiu: %d failure(s)\n", failures);
        return 1;
    }
    printf("ok unit_wiiu\n");
    return 0;
}
