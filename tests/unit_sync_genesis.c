// SPDX-License-Identifier: MPL-2.0
/* Mega Drive, 32X, Master System and Game Gear saves through collect and
 * restore: Genesis Plus GX's file, which stops at the last byte that isn't
 * 0xFF, and the file restore pads with 0xFF to PicoDrive's size give one
 * identity, PicoDrive's own 0x00 fill crosses as it is, and restore writes
 * each core's own form. */
#include "legacy_units.h"
#include <stdio.h>

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    const char        *ids[1];
} game;

static void make_game(game *g, mem_root *root, const char *platform, const char *layout) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V4;
    g->ids[0] = "Game";
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = platform;
    g->req.save.content_path = "Game.bin";
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

static sigil_sync_result *collect(mem_root *root, const char *platform, const char *layout) {
    game g;
    make_game(&g, root, platform, layout);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK) {
        sigil_sync_result_free(r);
        return NULL;
    }
    return r;
}

static int restore(mem_root *root, const char *platform, const char *layout, const uint8_t *unit, size_t len,
                   bool overwrite) {
    game g;
    make_game(&g, root, platform, layout);
    g.req.overwrite_local = overwrite;
    sigil_sync_result *w = NULL;
    int rc = sigil_restore(&g.req, unit, len, &w);
    sigil_sync_result_free(w);
    return rc;
}

/* A cart's save window of `window` bytes as a core fills it, the game's
 * bytes on the odd addresses up to `used` (8-bit SRAM on the low data
 * lines), or on every address for a 16-bit RAM or an EEPROM. */
static uint8_t *window_of(size_t window, size_t used, bool odd_only, uint8_t fill) {
    uint8_t *b = (uint8_t *)malloc(window);
    memset(b, fill, window);
    for (size_t i = 0; i < used; i++) {
        if (odd_only && !(i & 1)) continue;
        b[i] = (uint8_t)(i * 7 + 3) == 0xFF ? 0x5A : (uint8_t)(i * 7 + 3);
    }
    return b;
}

static size_t without_ff(const uint8_t *b, size_t len) {
    while (len && b[len - 1] == 0xFF) len--;
    return len;
}

static bool holds(mem_root *root, const char *path, const uint8_t *data, size_t len) {
    mem_file *f = root_find(root, path);
    return f && f->len == len && memcmp(f->data, data, len) == 0;
}

/* A Mega Drive save from Genesis Plus GX is the unit as its file holds it;
 * restored for PicoDrive next to PicoDrive's own file, it is padded with 0xFF
 * to that file's size, collects back to the same identity, and restores for
 * Genesis Plus GX to the file Genesis Plus GX wrote. */
static void check_gpgx_to_picodrive(void) {
    const char *where = "Mega Drive, Genesis Plus GX to PicoDrive";
    uint8_t *window = window_of(0x10000, 0x25C, true, 0xFF);
    size_t gpgx_len = without_ff(window, 0x10000);
    char ident[33], gpgx_md5[33];
    sigil_md5_of(window, gpgx_len, ident);
    memcpy(gpgx_md5, ident, sizeof(gpgx_md5));

    mem_root gpgx = {0};
    root_put(&gpgx, "Game.srm", window, gpgx_len);
    sigil_sync_result *r = collect(&gpgx, "genesis", "genesis_plus_gx");
    if (!r || r->len != gpgx_len || memcmp(r->data, window, gpgx_len) != 0 || r->shape != SIGIL_SAVE_SHAPE_SINGLE ||
        strcmp(r->artifact, "Game.srm") != 0 || strcmp(r->identity_hash, ident) != 0 ||
        strcmp(r->content_hash, gpgx_md5) != 0) {
        fail(where, "Genesis Plus GX's file collects as it is, named after the content");
    }

    uint8_t *own = window_of(0x10000, 0x40, true, 0x00);
    mem_root pico = {0};
    root_put(&pico, "Game.srm", own, 0x10000);
    if (!r || restore(&pico, "genesis", "picodrive", r->data, r->len, true) != SIGIL_OK ||
        !holds(&pico, "Game.srm", window, 0x10000) || pico.count != 1) {
        fail(where, "restore pads to the size of PicoDrive's file with 0xFF, so PicoDrive reads what GPGX read");
    }

    sigil_sync_result *back = collect(&pico, "genesis", "picodrive");
    if (!back || back->len != 0x10000 || strcmp(back->identity_hash, ident) != 0) {
        fail(where, "PicoDrive's padded file collects to the same identity");
    }

    mem_root again = {0};
    if (!back || restore(&again, "genesis", "genesis_plus_gx", back->data, back->len, true) != SIGIL_OK ||
        !holds(&again, "Game.srm", window, gpgx_len)) {
        fail(where, "the PicoDrive unit restores to the file Genesis Plus GX wrote");
    }

    mem_root empty = {0};
    if (!r || restore(&empty, "genesis", "picodrive", r->data, r->len, true) != SIGIL_OK ||
        !holds(&empty, "Game.srm", window, gpgx_len)) {
        fail(where, "with no PicoDrive file there, restore writes the RAM as it came");
    }
    sigil_sync_result_free(r);
    sigil_sync_result_free(back);
    root_free(&gpgx);
    root_free(&pico);
    root_free(&again);
    root_free(&empty);
    free(window);
    free(own);
}

/* PicoDrive's save keeps its 0x00 fill: Genesis Plus GX reads it as it is,
 * and the save crosses back to PicoDrive unchanged. */
static void check_picodrive_to_gpgx(void) {
    const char *where = "Mega Drive, PicoDrive to Genesis Plus GX";
    uint8_t *own = window_of(0x4000, 0x300, true, 0x00);
    char md5[33];
    sigil_md5_of(own, 0x4000, md5);
    mem_root pico = {0};
    root_put(&pico, "Game.srm", own, 0x4000);
    sigil_sync_result *r = collect(&pico, "genesis", "picodrive");
    if (!r || r->len != 0x4000 || memcmp(r->data, own, 0x4000) != 0 || strcmp(r->identity_hash, md5) != 0) {
        fail(where, "PicoDrive's file collects as it is");
    }
    mem_root gpgx = {0};
    sigil_sync_result *back = NULL;
    if (!r || restore(&gpgx, "genesis", "genesis_plus_gx", r->data, r->len, true) != SIGIL_OK ||
        !holds(&gpgx, "Game.srm", own, 0x4000)) {
        fail(where, "Genesis Plus GX gets the RAM with its 0x00 fill");
    } else if (!(back = collect(&gpgx, "genesis", "genesis_plus_gx")) || strcmp(back->identity_hash, md5) != 0) {
        fail(where, "it collects back to the same identity");
    }
    mem_root again = {0};
    if (!back || restore(&again, "genesis", "picodrive", back->data, back->len, true) != SIGIL_OK ||
        !holds(&again, "Game.srm", own, 0x4000)) {
        fail(where, "it restores for PicoDrive byte for byte");
    }
    sigil_sync_result_free(r);
    sigil_sync_result_free(back);
    root_free(&pico);
    root_free(&gpgx);
    root_free(&again);
    free(own);
}

/* A Master System or Game Gear cart's RAM is 32 KiB in PicoDrive whatever the
 * cart, so restore pads to it from nothing; a Mega Drive or 32X cart's size is
 * the ROM header's, and with no PicoDrive file there the RAM goes as it came. */
static void check_8bit_size(void) {
    static const struct { const char *platform; size_t size; } CASES[] = {
        { "sms", 0x8000 }, { "mastersystem", 0x8000 }, { "gamegear", 0x8000 }, { "gg", 0x8000 },
        { "genesis", 0 }, { "megadrive", 0 }, { "32x", 0 }, { "sega32", 0 },
    };
    uint8_t *window = window_of(0x10000, 0x2001, false, 0xFF);
    window[0x2000] = 0x00;
    size_t gpgx_len = without_ff(window, 0x10000);
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        char where[64];
        snprintf(where, sizeof(where), "%s from nothing", CASES[i].platform);
        mem_root root = {0};
        size_t want = CASES[i].size ? CASES[i].size : gpgx_len;
        if (restore(&root, CASES[i].platform, "picodrive", window, gpgx_len, true) != SIGIL_OK ||
            !holds(&root, "Game.srm", window, want)) {
            fail(where, CASES[i].size ? "PicoDrive's file is 32 KiB, 0xFF past the save" : "the RAM goes as it came");
        }
        root_free(&root);
    }
    free(window);
}

/* The file already there may be one Genesis Plus GX wrote, trimmed, into a
 * saves folder both cores share, so it can be shorter than the save. Restore
 * for PicoDrive never writes fewer bytes than the save holds: on Mega Drive
 * and 32X, past a file that can't hold it, the save goes as it came, and on
 * Master System and Game Gear PicoDrive gets its 32 KiB whatever is there.
 * A file that holds the whole save sets the size on Mega Drive and 32X. */
static void check_stale_short_file(void) {
    static const struct { const char *platform; size_t want; } CASES[] = {
        { "genesis", 0 }, { "32x", 0 }, { "sms", 0x8000 }, { "gamegear", 0x8000 },
    };
    uint8_t *save = window_of(0x10000, 0x300, true, 0xFF);
    size_t save_len = without_ff(save, 0x10000);
    uint8_t *stale = window_of(0x10000, 0x40, true, 0xFF);
    size_t stale_len = without_ff(stale, 0x10000);
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        char where[64];
        snprintf(where, sizeof(where), "%s next to a stale %zu-byte file", CASES[i].platform, stale_len);
        mem_root root = {0};
        root_put(&root, "Game.srm", stale, stale_len);
        size_t want = CASES[i].want ? CASES[i].want : save_len;
        if (restore(&root, CASES[i].platform, "picodrive", save, save_len, true) != SIGIL_OK ||
            !holds(&root, "Game.srm", save, want)) {
            fail(where, CASES[i].want ? "PicoDrive's file is 32 KiB, the whole save in it"
                                      : "the whole save goes as it came, not cut to the stale file");
        }
        root_free(&root);
    }

    uint8_t *own = window_of(0x10000, 0, false, 0x00);
    mem_root pico = {0};
    root_put(&pico, "Game.srm", own, 0x400);
    if (restore(&pico, "genesis", "picodrive", save, 0x10000, true) != SIGIL_OK ||
        !holds(&pico, "Game.srm", save, 0x400)) {
        fail("Mega Drive next to PicoDrive's 1 KiB file", "a file that holds the whole save sets the size");
    }
    root_free(&pico);

    mem_root wide = {0};
    root_put(&wide, "Game.srm", own, 0x10000);
    if (restore(&wide, "gamegear", "picodrive", save, save_len, true) != SIGIL_OK ||
        !holds(&wide, "Game.srm", save, 0x8000)) {
        fail("Game Gear next to a 64 KiB file", "PicoDrive's file is its 32 KiB");
    }
    root_free(&wide);
    free(save);
    free(stale);
    free(own);
}

/* Restore writes no more than the core keeps: a Game Gear save padded to 64 KiB
 * by another frontend, with a stray byte at its end, goes to PicoDrive at its
 * 32 KiB, and a RAM past 64 KiB goes to Genesis Plus GX at its 64 KiB. */
static void check_past_the_core(void) {
    uint8_t *window = window_of(0x10000, 0xF00, false, 0xFF);
    window[0xFFFF] = 0x96;
    mem_root pico = {0};
    if (restore(&pico, "gamegear", "picodrive", window, 0x10000, true) != SIGIL_OK ||
        !holds(&pico, "Game.srm", window, 0x8000)) {
        fail("past 32 KiB", "PicoDrive's Game Gear file is its 32 KiB");
    }
    root_free(&pico);
    uint8_t *big = window_of(0x20000, 0x20000, false, 0xFF);
    mem_root gpgx = {0};
    if (restore(&gpgx, "genesis", "genesis_plus_gx", big, 0x20000, true) != SIGIL_OK ||
        !holds(&gpgx, "Game.srm", big, 0x10000)) {
        fail("past 64 KiB", "Genesis Plus GX's file stops at its 64 KiB window");
    }
    root_free(&gpgx);
    free(window);
    free(big);
}

/* An I2C EEPROM's bytes sit from address 0 in both cores; PicoDrive keeps 8 KiB of them. */
static void check_eeprom(void) {
    const char *where = "Mega Drive EEPROM";
    uint8_t *window = window_of(0x2000, 128, false, 0xFF);
    mem_root gpgx = {0};
    root_put(&gpgx, "Game.srm", window, 128);
    sigil_sync_result *r = collect(&gpgx, "genesis", "genesis_plus_gx");
    uint8_t *own = window_of(0x2000, 0, false, 0x00);
    mem_root pico = {0};
    root_put(&pico, "Game.srm", own, 0x2000);
    if (!r || restore(&pico, "genesis", "picodrive", r->data, r->len, true) != SIGIL_OK ||
        !holds(&pico, "Game.srm", window, 0x2000)) {
        fail(where, "the 128 bytes restore into PicoDrive's 8 KiB, 0xFF past them");
    }
    sigil_sync_result *back = collect(&pico, "genesis", "picodrive");
    if (!r || !back || strcmp(back->identity_hash, r->identity_hash) != 0) fail(where, "one identity in both cores");
    sigil_sync_result_free(r);
    sigil_sync_result_free(back);
    root_free(&gpgx);
    root_free(&pico);
    free(window);
    free(own);
}

/* The identity leaves out trailing 0xFF and nothing else. */
static void check_identity(void) {
    uint8_t a[8] = { 1, 2, 3, 4, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t b[8] = { 1, 2, 3, 4, 0x00, 0x00, 0x00, 0x00 };
    mem_root ra = {0}, rb = {0}, rc = {0};
    root_put(&ra, "Game.srm", a, 8);
    root_put(&rb, "Game.srm", a, 4);
    root_put(&rc, "Game.srm", b, 8);
    sigil_sync_result *x = collect(&ra, "genesis", "picodrive");
    sigil_sync_result *y = collect(&rb, "genesis", "genesis_plus_gx");
    sigil_sync_result *z = collect(&rc, "genesis", "picodrive");
    if (!x || !y || strcmp(x->identity_hash, y->identity_hash) != 0) fail("identity", "trailing 0xFF isn't part of it");
    if (!x || !y || strcmp(x->content_hash, y->content_hash) == 0) fail("identity", "the content hash covers the bytes as sent");
    if (!x || !z || strcmp(x->identity_hash, z->identity_hash) == 0) fail("identity", "trailing 0x00 is");
    sigil_sync_result_free(x);
    sigil_sync_result_free(y);
    sigil_sync_result_free(z);
    root_free(&ra);
    root_free(&rb);
    root_free(&rc);
}

/* The same save in another core's form is written again in the request's
 * form; the same save in the same form isn't written at all. */
static void check_already_there(void) {
    uint8_t *window = window_of(0x4000, 0x100, true, 0xFF);
    size_t gpgx_len = without_ff(window, 0x4000);
    mem_root root = {0};
    root_put(&root, "Game.srm", window, 0x4000);
    if (restore(&root, "genesis", "genesis_plus_gx", window, gpgx_len, false) != SIGIL_OK ||
        !holds(&root, "Game.srm", window, gpgx_len) || root.writes != 1) {
        fail("already there", "a padded file of the same save is rewritten as Genesis Plus GX keeps it");
    }
    if (restore(&root, "genesis", "genesis_plus_gx", window, 0x4000, false) != SIGIL_OK || root.writes != 1) {
        fail("already there", "the same save in the same form isn't written");
    }
    root_free(&root);
    free(window);
}

/* A core sigil hasn't read keeps the bytes as they came. */
static void check_other_cores(void) {
    uint8_t *window = window_of(0x4000, 0x100, true, 0xFF);
    static const char *const LAYOUTS[] = { "libretro", "blastem", "gearsystem", NULL };
    for (size_t i = 0; i < 4; i++) {
        mem_root root = {0};
        if (restore(&root, i == 2 ? "sms" : "genesis", LAYOUTS[i], window, 0x4000, true) != SIGIL_OK ||
            !holds(&root, "Game.srm", window, 0x4000)) {
            fail(LAYOUTS[i] ? LAYOUTS[i] : "no layout", "the RAM is written as it came");
        }
        root_free(&root);
    }
    free(window);
}

/* An empty file holds no save; an empty unit and one larger than any cart's are refused. */
static void check_edges(void) {
    mem_root empty = {0};
    root_put(&empty, "Game.srm", (const uint8_t *)"", 0);
    sigil_sync_result *r = collect(&empty, "genesis", "genesis_plus_gx");
    if (!r || r->data || r->identity_hash[0]) fail("empty file", "holds no save");
    sigil_sync_result_free(r);
    root_free(&empty);

    mem_root none = {0};
    if (restore(&none, "genesis", "picodrive", (const uint8_t *)"", 0, true) != SIGIL_ERR_NOT_FOUND) {
        fail("empty unit", "restore refuses a unit holding no save");
    }
    size_t huge_len = 1024u * 1024u + 1;
    uint8_t *huge = (uint8_t *)calloc(1, huge_len);
    if (restore(&none, "genesis", "picodrive", huge, huge_len, true) != SIGIL_ERR_UNSUPPORTED_FORMAT || none.count) {
        fail("oversized unit", "restore refuses a unit larger than any cart's RAM and writes nothing");
    }
    free(huge);

    uint8_t ff[16];
    memset(ff, 0xFF, sizeof(ff));
    if (restore(&none, "genesis", "genesis_plus_gx", ff, sizeof(ff), true) != SIGIL_OK ||
        !holds(&none, "Game.srm", ff, sizeof(ff))) {
        fail("0xFF alone", "Genesis Plus GX's file keeps its length rather than going empty");
    }
    root_free(&none);
}

/* A round trip through the other core comes back the same size and bytes
 * whenever restore knows PicoDrive's size: its file is there, or the cart is a
 * Master System or Game Gear one. Each leg is restore, then collect of the file
 * restore wrote, as a client syncs. */
static void check_round_trips(void) {
    static const struct { const char *platform; size_t pico; bool odd_only; size_t used; bool local; } CASES[] = {
        { "genesis", 0x400, true, 0x25C, true },     /* Sonic 3's FRAM, next to PicoDrive's 1 KiB file */
        { "genesis", 0x10000, true, 0x4000, true },  /* 64 KiB window of odd bytes */
        { "genesis", 0x2000, false, 128, true },     /* EEPROM, PicoDrive's 8 KiB */
        { "32x", 0x10000, true, 0x100, true },
        { "sms", 0x8000, false, 0x1FFC, false },     /* 32 KiB known without a file */
        { "gamegear", 0x8000, false, 0xF00, false },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        char where[64];
        snprintf(where, sizeof(where), "%s round trip %zu", CASES[i].platform, i);
        uint8_t *window = window_of(CASES[i].pico, CASES[i].used, CASES[i].odd_only, 0xFF);
        size_t gpgx_len = without_ff(window, CASES[i].pico);

        /* Genesis Plus GX to PicoDrive and back. */
        mem_root p = {0}, g = {0};
        uint8_t *own = window_of(CASES[i].pico, 0, false, 0x00);
        if (CASES[i].local) root_put(&p, "Game.srm", own, CASES[i].pico);
        sigil_sync_result *a = NULL, *b = NULL;
        if (restore(&p, CASES[i].platform, "picodrive", window, gpgx_len, true) != SIGIL_OK ||
            !(a = collect(&p, CASES[i].platform, "picodrive")) ||
            restore(&g, CASES[i].platform, "genesis_plus_gx", a->data, a->len, true) != SIGIL_OK ||
            !holds(&g, "Game.srm", window, gpgx_len)) {
            fail(where, "Genesis Plus GX's file comes back the same size and bytes");
        }

        /* PicoDrive to Genesis Plus GX and back, PicoDrive's 0x00 fill included. */
        uint8_t *pico = window_of(CASES[i].pico, CASES[i].used, CASES[i].odd_only, 0x00);
        mem_root g2 = {0}, p2 = {0};
        if (CASES[i].local) root_put(&p2, "Game.srm", own, CASES[i].pico);
        if (restore(&g2, CASES[i].platform, "genesis_plus_gx", pico, CASES[i].pico, true) != SIGIL_OK ||
            !(b = collect(&g2, CASES[i].platform, "genesis_plus_gx")) ||
            restore(&p2, CASES[i].platform, "picodrive", b->data, b->len, true) != SIGIL_OK ||
            !holds(&p2, "Game.srm", pico, CASES[i].pico)) {
            fail(where, "PicoDrive's file comes back the same size and bytes");
        }
        sigil_sync_result_free(a);
        sigil_sync_result_free(b);
        root_free(&p);
        root_free(&g);
        root_free(&g2);
        root_free(&p2);
        free(window);
        free(own);
        free(pico);
    }
}

/* A zip of the one save, as RomM's neutral save.sram or an emulator's .srm,
 * restores as the raw RAM does; a zip of anything else is refused. */
static void check_zips(void) {
    uint8_t *window = window_of(0x8000, 0x100, true, 0xFF);
    size_t gpgx_len = without_ff(window, 0x8000);
    uint8_t *data[2] = { window, window };
    size_t lens[2] = { gpgx_len, gpgx_len };
    static const char *const ONE[][1] = { { "save.sram" }, { "Game.srm" }, { "Game.sav" } };
    for (size_t i = 0; i < 3; i++) {
        size_t zip_len = 0;
        uint8_t *zip = legacy_zip(ONE[i], data, lens, 1, &zip_len);
        mem_root root = {0};
        if (restore(&root, "gamegear", "picodrive", zip, zip_len, true) != SIGIL_OK ||
            !holds(&root, "Game.srm", window, 0x8000)) {
            fail(ONE[i][0], "a zip of the one save restores as the raw RAM");
        }
        root_free(&root);
        free(zip);
    }
    static const char *const TWO[] = { "save.sram", "Game.srm" };
    static const char *const OTHER[] = { "clock.rtc" };
    size_t zip_len = 0;
    uint8_t *two = legacy_zip(TWO, data, lens, 2, &zip_len);
    mem_root root = {0};
    if (restore(&root, "genesis", "genesis_plus_gx", two, zip_len, true) != SIGIL_ERR_UNSUPPORTED_FORMAT || root.count) {
        fail("zip of two", "refused, nothing written");
    }
    free(two);
    uint8_t *other = legacy_zip(OTHER, data, lens, 1, &zip_len);
    if (restore(&root, "genesis", "genesis_plus_gx", other, zip_len, true) != SIGIL_ERR_UNSUPPORTED_FORMAT || root.count) {
        fail("zip of clock.rtc", "refused, nothing written");
    }
    free(other);
    root_free(&root);
    free(window);
}

/* A save changed on disk since the last sync isn't replaced unless asked. */
static void check_conflict(void) {
    uint8_t *ours = window_of(0x4000, 0x200, true, 0x00), *theirs = window_of(0x4000, 0x200, true, 0x00);
    theirs[1] ^= 0x55;
    mem_root root = {0};
    root_put(&root, "Game.srm", ours, 0x4000);
    if (restore(&root, "genesis", "picodrive", theirs, 0x4000, false) != SIGIL_ERR_CONFLICT) {
        fail("conflict", "a changed local save refuses the restore");
    }
    if (!holds(&root, "Game.srm", ours, 0x4000)) fail("conflict", "the local file is untouched");
    if (restore(&root, "genesis", "picodrive", theirs, 0x4000, true) != SIGIL_OK ||
        !holds(&root, "Game.srm", theirs, 0x4000)) {
        fail("conflict", "overwrite_local replaces it");
    }
    root_free(&root);
    free(ours);
    free(theirs);
}

int main(void) {
    check_gpgx_to_picodrive();
    check_picodrive_to_gpgx();
    check_8bit_size();
    check_stale_short_file();
    check_eeprom();
    check_past_the_core();
    check_identity();
    check_already_there();
    check_other_cores();
    check_edges();
    check_zips();
    check_round_trips();
    check_conflict();
    printf("genesis sync: %d failures\n", g_fails);
    return g_fails ? 1 : 0;
}
