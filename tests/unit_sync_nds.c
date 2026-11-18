// SPDX-License-Identifier: MPL-2.0
/* Nintendo DS saves through collect and restore: melonDS's raw file and
 * DeSmuME's .dsv give one unit, the raw chip bytes, and restore writes each
 * emulator's own file, with the footer DeSmuME writes on the .dsv. */
#include "legacy_units.h"
#include <stdio.h>

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

#define NOTE   "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:"
#define COOKIE "|-DESMUME SAVE-|"
#define FOOTER 122u

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    const char        *ids[1];
} game;

static void make_game(game *g, mem_root *root, const char *layout) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V4;
    g->ids[0] = "Game";
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = "nds";
    g->req.save.content_path = "Game.nds";
    g->req.save.result = &g->result;
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
    g->req.save.open = root_open;
    g->req.save.open_ctx = root;
    g->req.game_ids = g->ids;
    g->req.game_id_count = 1;
    g->req.write = root_write;
    g->req.remove = root_remove;
    g->req.write_ctx = root;
    g->req.overwrite_local = 1;
}

static sigil_sync_result *collect(mem_root *root, const char *layout) {
    game g;
    make_game(&g, root, layout);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK) {
        sigil_sync_result_free(r);
        return NULL;
    }
    return r;
}

static int restore(mem_root *root, const char *layout, const uint8_t *unit, size_t len, bool overwrite) {
    game g;
    make_game(&g, root, layout);
    g.req.overwrite_local = overwrite;
    sigil_sync_result *w = NULL;
    int rc = sigil_restore(&g.req, unit, len, &w);
    sigil_sync_result_free(w);
    return rc;
}

/* A save of `len` bytes no two sizes share a prefix of. */
static uint8_t *save_of(size_t len) {
    uint8_t *b = (uint8_t *)malloc(len);
    for (size_t i = 0; i < len; i++) b[i] = (uint8_t)(i * 31 + len / 512 + (i >> 9));
    return b;
}

/* `raw` padded with 0xFF to `padded` and DeSmuME's footer, field by field from mc.cpp ensure. */
static uint8_t *dsv_of(const uint8_t *raw, size_t len, uint32_t padded, uint32_t type, uint32_t addr_size,
                       size_t *out_len) {
    *out_len = padded + FOOTER;
    uint8_t *b = (uint8_t *)malloc(*out_len);
    memcpy(b, raw, len);
    memset(b + len, 0xFF, padded - len);
    uint8_t *p = b + padded;
    memcpy(p, NOTE, 82);
    uint32_t fields[6] = { (uint32_t)len, padded, type, addr_size, padded, 0 };
    for (size_t i = 0; i < 6; i++) sigil_write_le32(p + 82 + 4 * i, fields[i]);
    memcpy(p + 106, COOKIE, 16);
    return b;
}

/* For each chip size: the raw save from melonDS is the unit; restore for
 * DeSmuME writes the .dsv DeSmuME writes for that chip; that .dsv collects
 * back to the save and restores to melonDS's file. DeSmuME has no 128 KiB
 * type and pads such a save to 256 KiB, as it pads its own. */
static void check_sizes(void) {
    static const struct { uint32_t len, padded, type, addr_size; } CHIPS[] = {
        { 512u, 512u, 1, 1 },                 /* EEPROM 4 kbit */
        { 8192u, 8192u, 2, 2 },               /* EEPROM 64 kbit */
        { 32768u, 32768u, 4, 2 },             /* FRAM 256 kbit */
        { 65536u, 65536u, 3, 2 },             /* EEPROM 512 kbit */
        { 131072u, 262144u, 5, 3 },           /* EEPROM 1 Mbit, melonDS only */
        { 262144u, 262144u, 5, 3 },           /* FLASH 2 Mbit */
        { 524288u, 524288u, 6, 3 },           /* FLASH 4 Mbit */
        { 1048576u, 1048576u, 7, 3 },         /* FLASH 8 Mbit */
        { 8388608u, 8388608u, 10, 3 },        /* 64 Mbit */
    };
    for (size_t i = 0; i < sizeof(CHIPS) / sizeof(CHIPS[0]); i++) {
        char where[48];
        snprintf(where, sizeof(where), "%u-byte save", CHIPS[i].len);
        uint8_t *raw = save_of(CHIPS[i].len);
        size_t dsv_len = 0;
        uint8_t *dsv = dsv_of(raw, CHIPS[i].len, CHIPS[i].padded, CHIPS[i].type, CHIPS[i].addr_size, &dsv_len);
        char raw_md5[33];
        sigil_md5_of(raw, CHIPS[i].len, raw_md5);

        mem_root melon = {0};
        root_put(&melon, "Game.sav", raw, CHIPS[i].len);
        sigil_sync_result *r = collect(&melon, "melonds");
        if (!r || r->len != CHIPS[i].len || memcmp(r->data, raw, r->len) != 0 || r->shape != SIGIL_SAVE_SHAPE_SINGLE ||
            strcmp(r->artifact, "Game.srm") != 0 || strcmp(r->identity_hash, raw_md5) != 0 ||
            strcmp(r->content_hash, raw_md5) != 0) {
            fail(where, "melonDS's file collects as the raw save, named after the content");
        }

        mem_root desmume = {0};
        mem_file *f = NULL;
        if (!r || restore(&desmume, "desmume", r->data, r->len, true) != SIGIL_OK ||
            !(f = root_find(&desmume, "Game.dsv")) || f->len != dsv_len || memcmp(f->data, dsv, dsv_len) != 0 ||
            desmume.count != 1) {
            fail(where, "restore for DeSmuME writes the .dsv DeSmuME writes");
        }

        sigil_sync_result *back = collect(&desmume, "desmume");
        if (!back || back->len != CHIPS[i].padded || memcmp(back->data, raw, CHIPS[i].len) != 0) {
            fail(where, "DeSmuME's .dsv collects as the save without its footer");
        }

        mem_root again = {0};
        if (!back || restore(&again, "melonds", back->data, back->len, true) != SIGIL_OK ||
            !(f = root_find(&again, "Game.sav")) || f->len != CHIPS[i].padded || memcmp(f->data, back->data, back->len) != 0) {
            fail(where, "the DeSmuME unit restores to melonDS's file, raw");
        }

        mem_root legacy = {0};
        if (restore(&legacy, "melonds", dsv, dsv_len, true) != SIGIL_OK || !(f = root_find(&legacy, "Game.sav")) ||
            f->len != CHIPS[i].padded || memcmp(f->data, dsv, CHIPS[i].padded) != 0) {
            fail(where, "a .dsv uploaded as it is restores to melonDS's file without the footer");
        }

        sigil_sync_result_free(r);
        sigil_sync_result_free(back);
        root_free(&melon);
        root_free(&desmume);
        root_free(&again);
        root_free(&legacy);
        free(raw);
        free(dsv);
    }
}

/* melonDS DS has no row: RetroArch writes its save RAM raw as {stem}.srm. */
static void check_default_row(void) {
    uint8_t *raw = save_of(8192);
    mem_root root = {0};
    root_put(&root, "Game.srm", raw, 8192);
    sigil_sync_result *r = collect(&root, "melondsds");
    if (!r || r->len != 8192 || memcmp(r->data, raw, 8192) != 0) fail("melondsds", "the .srm collects as the save");
    mem_root fresh = {0};
    mem_file *f = NULL;
    if (!r || restore(&fresh, "melondsds", r->data, r->len, true) != SIGIL_OK || !(f = root_find(&fresh, "Game.srm")) ||
        f->len != 8192 || memcmp(f->data, raw, 8192) != 0 || fresh.count != 1) {
        fail("melondsds", "restore writes the .srm alone, raw");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    root_free(&fresh);
    free(raw);
}

/* A .dsv without the footer (DraStic's) is the save as it is; DeSmuME
 * wouldn't read it, so a restore of the same save writes the footer. */
static void check_footerless_dsv(void) {
    uint8_t *raw = save_of(8192);
    size_t dsv_len = 0;
    uint8_t *dsv = dsv_of(raw, 8192, 8192, 2, 2, &dsv_len);
    mem_root root = {0};
    root_put(&root, "Game.dsv", raw, 8192);
    sigil_sync_result *r = collect(&root, "desmume");
    mem_file *f = NULL;
    if (!r || r->len != 8192 || memcmp(r->data, raw, 8192) != 0) fail("footerless .dsv", "collects as the save");
    if (!r || restore(&root, "desmume", r->data, r->len, false) != SIGIL_OK || !(f = root_find(&root, "Game.dsv")) ||
        f->len != dsv_len || memcmp(f->data, dsv, dsv_len) != 0) {
        fail("footerless .dsv", "the same save restored gets DeSmuME's footer");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(raw);
    free(dsv);
}

/* An empty file holds no save; a footer with another version isn't DeSmuME's. */
static void check_edges(void) {
    mem_root empty = {0};
    root_put(&empty, "Game.sav", (const uint8_t *)"", 0);
    sigil_sync_result *r = collect(&empty, "melonds");
    if (!r || r->data || r->identity_hash[0]) fail("empty file", "holds no save");
    sigil_sync_result_free(r);
    root_free(&empty);

    uint8_t *raw = save_of(512);
    size_t dsv_len = 0;
    uint8_t *dsv = dsv_of(raw, 512, 512, 1, 1, &dsv_len);
    sigil_write_le32(dsv + dsv_len - 16 - 4, 1);
    mem_root other = {0};
    root_put(&other, "Game.dsv", dsv, dsv_len);
    r = collect(&other, "desmume");
    if (!r || r->len != dsv_len) fail("footer version 1", "taken as part of the save, as DeSmuME takes it");
    sigil_sync_result_free(r);
    root_free(&other);

    mem_root none = {0};
    if (restore(&none, "melonds", (const uint8_t *)"", 0, true) != SIGIL_ERR_NOT_FOUND) {
        fail("empty unit", "restore refuses a unit holding no save");
    }
    root_free(&none);
    free(raw);
    free(dsv);
}

/* A save changed on disk since the last sync isn't replaced unless asked. */
static void check_conflict(void) {
    uint8_t *ours = save_of(8192), *theirs = save_of(8192);
    theirs[0] ^= 0xFF;
    size_t dsv_len = 0;
    uint8_t *dsv = dsv_of(ours, 8192, 8192, 2, 2, &dsv_len);
    mem_root root = {0};
    root_put(&root, "Game.dsv", dsv, dsv_len);
    if (restore(&root, "desmume", theirs, 8192, false) != SIGIL_ERR_CONFLICT) {
        fail("conflict", "a changed local save refuses the restore");
    }
    mem_file *f = root_find(&root, "Game.dsv");
    if (!f || f->len != dsv_len || memcmp(f->data, dsv, dsv_len) != 0) fail("conflict", "the local .dsv is untouched");
    if (restore(&root, "desmume", theirs, 8192, true) != SIGIL_OK || !(f = root_find(&root, "Game.dsv")) ||
        f->len != dsv_len || memcmp(f->data, theirs, 8192) != 0) {
        fail("conflict", "overwrite_local replaces it");
    }
    root_free(&root);
    free(ours);
    free(theirs);
    free(dsv);
}

int main(void) {
    check_sizes();
    check_default_row();
    check_footerless_dsv();
    check_edges();
    check_conflict();
    printf("nds sync: %d failures\n", g_fails);
    return g_fails ? 1 : 0;
}
