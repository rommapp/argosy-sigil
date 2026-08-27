// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Builds a minimal ISO9660 image shaped like a PS3 disc and reads TITLE_ID out
 * of PS3_GAME/PARAM.SFO through it, covering the two-level directory walk that
 * a bare-SFO test would not reach. */

#define SECTOR       2048
#define PVD_LBA      16
#define ROOT_LBA     20
#define GAME_DIR_LBA 21
#define SFO_LBA      22
#define IMAGE_LBAS   23

#define SAMPLE_TITLE_ID "BCUS98233"

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

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* ISO9660 stores extents both-endian, so each field is written twice even
 * though only the little-endian half is read back. */
static size_t dir_record(uint8_t *p, uint32_t lba, uint32_t length,
                         const char *name, bool is_dir) {
    size_t name_len = strlen(name);
    size_t rec = 33 + name_len;
    if (rec & 1) rec++;                     /* records are even-aligned */

    memset(p, 0, rec);
    p[0] = (uint8_t)rec;
    put_le32(p + 2,  lba);
    put_be32(p + 6,  lba);
    put_le32(p + 10, length);
    put_be32(p + 14, length);
    p[25] = is_dir ? 0x02 : 0x00;
    p[32] = (uint8_t)name_len;
    memcpy(p + 33, name, name_len);
    return rec;
}

/* Single-entry PARAM.SFO carrying TITLE_ID. */
static size_t build_sfo(uint8_t *p, const char *title_id) {
    const char *key = "TITLE_ID";
    size_t key_len  = strlen(key) + 1;
    size_t val_len  = strlen(title_id) + 1;

    uint32_t index_off = 0x14;
    uint32_t key_table = index_off + 16;
    uint32_t data_table = (uint32_t)(key_table + key_len);
    /* Align the data table the way real files do, so a parser that assumes
     * alignment is not accidentally satisfied by a packed fixture. */
    data_table = (data_table + 3) & ~3u;

    put_le32(p + 0x00, 0x46535000u);        /* "\0PSF" */
    put_le32(p + 0x04, 0x00000101u);        /* version 1.1 */
    put_le32(p + 0x08, key_table);
    put_le32(p + 0x0C, data_table);
    put_le32(p + 0x10, 1);                  /* one entry */

    uint8_t *e = p + index_off;
    e[0] = 0; e[1] = 0;                     /* key offset */
    e[2] = 0x04; e[3] = 0x02;               /* utf8 string */
    put_le32(e + 4,  (uint32_t)val_len);
    put_le32(e + 8,  (uint32_t)val_len);
    put_le32(e + 12, 0);                    /* data offset */

    memcpy(p + key_table, key, key_len);
    memcpy(p + data_table, title_id, val_len);
    return data_table + val_len;
}

int main(void) {
    size_t len = IMAGE_LBAS * SECTOR;
    uint8_t *img = calloc(1, len);
    if (!img) return 1;

    size_t sfo_len = build_sfo(img + SFO_LBA * SECTOR, SAMPLE_TITLE_ID);

    /* Root directory: the "." entry, then PS3_GAME. */
    uint8_t *root = img + ROOT_LBA * SECTOR;
    size_t pos = dir_record(root, ROOT_LBA, SECTOR, "\1", true);
    dir_record(root + pos, GAME_DIR_LBA, SECTOR, "PS3_GAME", true);

    /* PS3_GAME directory holding PARAM.SFO. */
    uint8_t *game = img + GAME_DIR_LBA * SECTOR;
    pos = dir_record(game, GAME_DIR_LBA, SECTOR, "\1", true);
    dir_record(game + pos, SFO_LBA, (uint32_t)sfo_len, "PARAM.SFO", false);

    /* Primary volume descriptor with the root record embedded at +156. */
    uint8_t *pvd = img + PVD_LBA * SECTOR;
    pvd[0] = 0x01;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 0x01;
    dir_record(pvd + 156, ROOT_LBA, SECTOR, "\0", true);

    mem_ctx ctx = { img, len };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, "game.iso", SIGIL_PLATFORM_PS3, NULL, &r);
    free(img);

    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL ps3_iso: rc=%d\n", rc);
        return 1;
    }
    if (strcmp(r.title_id, SAMPLE_TITLE_ID) != 0
        || strcmp(r.save_id, SAMPLE_TITLE_ID) != 0) {
        fprintf(stderr, "FAIL ps3_iso id: title_id='%s' save_id='%s'\n",
                r.title_id, r.save_id);
        return 1;
    }
    if (r.usage != SIGIL_USAGE_FOLDER_PREFIX) {
        fprintf(stderr, "FAIL ps3_iso usage: got %d\n", (int)r.usage);
        return 1;
    }
    if (r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "FAIL ps3_iso: expected binary source\n");
        return 1;
    }
    printf("ok unit_ps3_iso (title_id=%s via PS3_GAME/PARAM.SFO)\n", r.title_id);
    return 0;
}
