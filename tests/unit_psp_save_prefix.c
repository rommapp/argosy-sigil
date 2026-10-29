// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Builds minimal PSP disc images (UMD_DATA.BIN plus PSP_GAME/SYSDIR) and
 * checks which save prefix sigil reads out of the game's executable. */

#define SECTOR       2048
#define PVD_LBA      16
#define ROOT_LBA     20
#define GAME_DIR_LBA 21
#define SYSDIR_LBA   22
#define UMD_LBA      23
#define BOOT_LBA     24
#define EXE_SECTORS  3
#define EBOOT_LBA    (BOOT_LBA + EXE_SECTORS)
#define IMAGE_LBAS   (EBOOT_LBA + EXE_SECTORS)
#define EXE_LEN      (EXE_SECTORS * SECTOR)

#define STORE_ID "NPJH-50804"
#define DISC_DEFAULT "NPJH50804"
#define UMD_ID "ULUS-10064"

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

static size_t dir_record(uint8_t *p, uint32_t lba, uint32_t length,
                         const char *name, bool is_dir) {
    size_t name_len = strlen(name);
    size_t rec = 33 + name_len;
    if (rec & 1) rec++;
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

typedef struct {
    const char *name;
    const char *disc;
    bool boot_is_elf;
    bool eboot_is_elf;
    const char *strings[3];
    size_t first_offset;
    const char *want_save_id;
} prefix_case;

static void write_executable(uint8_t *exe, bool is_elf, const prefix_case *c) {
    if (is_elf) memcpy(exe, "\x7f" "ELF", 4);
    else memcpy(exe, "~PSP", 4);
    size_t at = c->first_offset;
    for (int i = 0; i < 3 && c->strings[i]; i++) {
        size_t n = strlen(c->strings[i]);
        memcpy(exe + at, c->strings[i], n);
        at += n + 1;
    }
}

static int run_case(const prefix_case *c) {
    size_t len = (size_t)IMAGE_LBAS * SECTOR;
    uint8_t *img = calloc(1, len);
    if (!img) return 1;

    char umd[64];
    snprintf(umd, sizeof(umd), "%s|0123456789ABCDEF|0001|G", c->disc);
    memcpy(img + UMD_LBA * SECTOR, umd, strlen(umd));
    char want_title[16];
    snprintf(want_title, sizeof(want_title), "%.4s%.5s", c->disc, c->disc + 5);

    uint8_t *root = img + ROOT_LBA * SECTOR;
    size_t pos = dir_record(root, ROOT_LBA, SECTOR, "\1", true);
    pos += dir_record(root + pos, UMD_LBA, (uint32_t)strlen(umd), "UMD_DATA.BIN", false);
    dir_record(root + pos, GAME_DIR_LBA, SECTOR, "PSP_GAME", true);

    uint8_t *game = img + GAME_DIR_LBA * SECTOR;
    pos = dir_record(game, GAME_DIR_LBA, SECTOR, "\1", true);
    dir_record(game + pos, SYSDIR_LBA, SECTOR, "SYSDIR", true);

    uint8_t *sys = img + SYSDIR_LBA * SECTOR;
    pos = dir_record(sys, SYSDIR_LBA, SECTOR, "\1", true);
    pos += dir_record(sys + pos, BOOT_LBA, EXE_LEN, "BOOT.BIN", false);
    dir_record(sys + pos, EBOOT_LBA, EXE_LEN, "EBOOT.BIN", false);

    if (c->boot_is_elf) {
        write_executable(img + BOOT_LBA * SECTOR, true, c);
    }
    write_executable(img + EBOOT_LBA * SECTOR, c->eboot_is_elf, c);

    uint8_t *pvd = img + PVD_LBA * SECTOR;
    pvd[0] = 0x01;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 0x01;
    dir_record(pvd + 156, ROOT_LBA, SECTOR, "\0", true);

    mem_ctx ctx = { img, len };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, "game.iso", SIGIL_PLATFORM_PSP, NULL, &r);
    free(img);

    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL %s: rc=%d\n", c->name, rc);
        return 1;
    }
    if (strcmp(r.title_id, want_title) != 0) {
        fprintf(stderr, "FAIL %s: title_id='%s'\n", c->name, r.title_id);
        return 1;
    }
    if (strcmp(r.save_id, c->want_save_id) != 0) {
        fprintf(stderr, "FAIL %s: save_id='%s' want '%s'\n", c->name, r.save_id, c->want_save_id);
        return 1;
    }
    printf("ok %s (save_id='%s')\n", c->name, r.save_id);
    return 0;
}

int main(void) {
    const prefix_case cases[] = {
        { "store release saving under its disc prefix", STORE_ID, true, false,
          { "ULJS00445", NULL }, 0x100, "ULJS00445" },
        { "dashed prefix is undashed", STORE_ID, true, false,
          { "ULJS-00445", NULL }, 0x100, "ULJS00445" },
        { "executable naming its own id keeps the default", STORE_ID, true, false,
          { "NPJH50804", "ULJS00445", NULL }, 0x100, DISC_DEFAULT },
        { "two foreign prefixes are ambiguous", STORE_ID, true, false,
          { "ULJS00445", "ULJS00003", NULL }, 0x100, DISC_DEFAULT },
        { "prefix split across two sectors", STORE_ID, true, false,
          { "ULJS00445", NULL }, SECTOR - 4, "ULJS00445" },
        { "prefix inside a longer word is ignored", STORE_ID, true, false,
          { "XULJS00445", NULL }, 0x100, DISC_DEFAULT },
        { "zeroed BOOT.BIN falls through to a decrypted EBOOT.BIN", STORE_ID, false, true,
          { "ULJS00445", NULL }, 0x100, "ULJS00445" },
        { "encrypted EBOOT.BIN alone is not read", STORE_ID, false, false,
          { "ULJS00445", NULL }, 0x100, DISC_DEFAULT },
        { "no serial in the executable keeps the default", STORE_ID, true, false,
          { NULL }, 0x100, DISC_DEFAULT },
        { "another Store id is never adopted", STORE_ID, true, false,
          { "NPUH10000", NULL }, 0x100, DISC_DEFAULT },
        { "an all-zero placeholder is never adopted", STORE_ID, true, false,
          { "ULJS00000", NULL }, 0x100, DISC_DEFAULT },
        { "a disc release keeps its own id whatever its executable names", UMD_ID, true, false,
          { "ULUS00000", NULL }, 0x100, "ULUS10064" },
        { "a disc release ignores another region's serial", UMD_ID, true, false,
          { "ULES00629", NULL }, 0x100, "ULUS10064" },
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        failed |= run_case(&cases[i]);
    }
    return failed;
}
