// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
#include "legacy_units.h"
#include "card_gamecube.h"
#include <stdbool.h>

#define TEST_SKIP  77
#define FZERO      "47465A45"
#define FZERO_DISC "F-Zero GX (USA).rvz"
#define LIB_FOLDER "User/GC/USA/Card A/"
#define LIB_RAW    "User/GC/MemoryCardA.USA.raw"
#define SA_FOLDER  "GC/USA/Card A/"
#define NFSU2      "47554745"
#define NFSU2_ISO  "NFSU2 (USA).iso"
#define NFSU2_FILE SA_FOLDER "69-GUGE-NFSU2.gci"
#define GCI_MAKER  0x04u
#define GCI_NAME   0x08u
#define GCI_MTIME  0x28u
#define GCI_COPY   0x35u

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static corpus_table g_manifest;

static uint8_t *sample(const char *id, const char *path, size_t *len) {
    return corpus_sample(&g_manifest, "ngc", id, path, len);
}

static const char *const FZERO_FILES[] = {
    "8P-GFZE-f_zero.dat.gci", "8P-GFZE-f_zero.dat0.gci", "8P-GFZE-fzc.dat.gci",
    "8P-GFZE-fze020000200076747D46574372.dat.gci", "8P-GFZE-fze020000200076748122361635.dat.gci",
    "8P-GFZE-fze020000200076749DE3A9786D.dat.gci",
};

/* Puts the F-Zero GX .gci set into `root` under `folder`, skipping `skip`. */
static bool put_fzero(mem_root *root, const char *folder, const char *skip) {
    for (size_t i = 0; i < 6; i++) {
        if (skip && strcmp(FZERO_FILES[i], skip) == 0) continue;
        size_t len = 0;
        uint8_t *data = sample("fzero-gx-dolphin-gci-set", FZERO_FILES[i], &len);
        if (!data) return false;
        char path[512];
        snprintf(path, sizeof(path), "%s%s", folder, FZERO_FILES[i]);
        root_put(root, path, data, len);
        free(data);
    }
    return true;
}

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    sigil_save_option  options[2];
} game;

static void make_game(game *g, mem_root *root, const char *layout, const char *title_id, const char *serial,
                      const char *content) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V3;
    g->result.platform = SIGIL_PLATFORM_GAMECUBE;
    snprintf(g->result.title_id, sizeof(g->result.title_id), "%s", title_id);
    snprintf(g->result.raw_serial, sizeof(g->result.raw_serial), "%s", serial);
    snprintf(g->result.save_id, sizeof(g->result.save_id), "%s", serial);
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = "gamecube";
    g->req.save.content_path = content;
    g->req.save.result = &g->result;
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
    g->req.save.open = root_open;
    g->req.save.open_ctx = root;
    g->req.write = root_write;
    g->req.remove = root_remove;
    g->req.write_ctx = root;
}

static void raw_mode(game *g) {
    g->options[0].key = "SlotA";
    g->options[0].value = "1";
    g->req.save.options = g->options;
    g->req.save.option_count = 1;
}

static void refresh(game *g, mem_root *root) {
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
}

static size_t zip_count(const uint8_t *data, size_t len) {
    sigil_zip_member *m = NULL;
    size_t n = 0;
    if (sigil_zip_read_mem(data, len, 1u << 26, &m, &n) != SIGIL_OK) return 0;
    sigil_zip_members_free(m, n);
    return n;
}

/* A copy of `gci` with `n` bytes at `at` replaced by `bytes`. */
static uint8_t *gci_with(const uint8_t *gci, size_t len, size_t at, const void *bytes, size_t n) {
    uint8_t *out = (uint8_t *)malloc(len);
    memcpy(out, gci, len);
    memcpy(out + at, bytes, n);
    return out;
}

/* The unit of the one game in `root`, collected under `layout`. */
static sigil_sync_result *unit_from(mem_root *root, const char *layout, const char *title_id, const char *serial,
                                    const char *content, bool raw) {
    game g;
    make_game(&g, root, layout, title_id, serial, content);
    if (raw) raw_mode(&g);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data) {
        sigil_sync_result_free(r);
        return NULL;
    }
    return r;
}

static sigil_sync_result *nfsu2_unit(void) {
    size_t len = 0;
    uint8_t *gci = sample("nfsu2-gci", NULL, &len);
    if (!gci) return NULL;
    mem_root root = {0};
    root_put(&root, NFSU2_FILE, gci, len);
    sigil_sync_result *r = unit_from(&root, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO, false);
    root_free(&root);
    free(gci);
    return r;
}

/* Restores `old` and `unit` into empty roots as dolphin_standalone keeps
 * them, raw card mode or GCI folder, and wants the same files from both. */
static void expect_same_restore(const char *where, const char *title_id, const char *serial, const char *content,
                                bool raw, const uint8_t *old, size_t old_len, const sigil_sync_result *unit) {
    mem_root from_old = {0}, from_unit = {0};
    game a, b;
    make_game(&a, &from_old, "dolphin_standalone", title_id, serial, content);
    make_game(&b, &from_unit, "dolphin_standalone", title_id, serial, content);
    if (raw) {
        raw_mode(&a);
        raw_mode(&b);
    }
    sigil_sync_result *ra = NULL, *rb = NULL;
    if (!old || !unit) {
        fail(where, "setup failed");
    } else {
        int rc_old = sigil_restore(&a.req, old, old_len, &ra);
        int rc_unit = sigil_restore(&b.req, unit->data, unit->len, &rb);
        char what[64];
        snprintf(what, sizeof(what), "restore failed: old %d, unit %d", rc_old, rc_unit);
        if (rc_old != SIGIL_OK || rc_unit != SIGIL_OK) fail(where, what);
        else if (!roots_same(&from_old, &from_unit)) fail(where, "the old upload restores other files than sigil's unit");
    }
    sigil_sync_result_free(ra);
    sigil_sync_result_free(rb);
    root_free(&from_old);
    root_free(&from_unit);
}

/* Argosy uploaded a game's .gci files as the one file, or a flat zip of them
 * under their names in Dolphin's GCI folder. */
static void check_argosy_uploads(const sigil_sync_result *nfsu2) {
    size_t gci_len = 0, zip_len = 0;
    uint8_t *gci = sample("nfsu2-gci", NULL, &gci_len);
    expect_same_restore("argosy gamecube raw gci", NFSU2, "GUGE", NFSU2_ISO, false, gci, gci_len, nfsu2);
    expect_same_restore("argosy gamecube raw gci to a card", NFSU2, "GUGE", NFSU2_ISO, true, gci, gci_len, nfsu2);
    free(gci);

    mem_root root = {0};
    uint8_t *data[6] = { 0 };
    size_t lens[6] = { 0 };
    bool have = put_fzero(&root, SA_FOLDER, NULL);
    for (size_t i = 0; have && i < 6; i++) {
        data[i] = sample("fzero-gx-dolphin-gci-set", FZERO_FILES[i], &lens[i]);
        have = data[i] != NULL;
    }
    sigil_sync_result *fzero = have ? unit_from(&root, "dolphin_standalone", FZERO, "GFZE", FZERO_DISC, false) : NULL;
    uint8_t *zip = have ? legacy_zip(FZERO_FILES, data, lens, 6, &zip_len) : NULL;
    expect_same_restore("argosy gamecube zip", FZERO, "GFZE", FZERO_DISC, false, zip, zip_len, fzero);
    free(zip);
    sigil_sync_result_free(fzero);
    for (size_t i = 0; i < 6; i++) free(data[i]);
    root_free(&root);
}

static uint32_t first_block_of(const uint8_t *card, size_t len, const char *name) {
    sigil_card_listing *l = NULL;
    uint32_t first = 0xFFFF;
    if (sigil_gamecube_card_list(card, len, &l) != SIGIL_OK) return first;
    for (size_t i = 0; i < l->entry_count; i++) {
        if (strcmp(l->entries[i].name, name) == 0) first = l->entries[i].first_block;
    }
    sigil_card_listing_free(l);
    return first;
}

static size_t card_saves_of(const uint8_t *card, size_t len, const char *owner) {
    sigil_card_listing *l = NULL;
    size_t n = 0;
    if (sigil_gamecube_card_list(card, len, &l) != SIGIL_OK) return 0;
    for (size_t i = 0; i < l->entry_count; i++) n += strcmp(l->entries[i].owner_id, owner) == 0;
    sigil_card_listing_free(l);
    return n;
}

/* Dolphin's default GCI folder: each save is a file. The unit is the game's
 * .gci files as Dolphin names them; a stale duplicate (f_zero.dat0, which
 * Dolphin refuses at load) and another game's file stay out. */
static void check_folder_collect(sigil_sync_result **kept) {
    mem_root root = {0};
    size_t len = 0;
    uint8_t *nfsu2 = sample("nfsu2-gci", NULL, &len);
    if (!put_fzero(&root, LIB_FOLDER, NULL) || !nfsu2) { fail("folder collect", "samples missing"); root_free(&root); free(nfsu2); return; }
    root_put(&root, LIB_FOLDER "69-GUGE-NFSU2.gci", nfsu2, len);
    game g;
    make_game(&g, &root, "dolphin", FZERO, "GFZE", FZERO_DISC);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || r->shape != SIGIL_SAVE_SHAPE_MULTI || zip_count(r->data, r->len) != 5) {
        fail("folder collect", "the unit isn't the game's five distinct saves");
    } else {
        *kept = r;
        r = NULL;
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(nfsu2);
}

/* Dolphin reads the folder's files in name order, so of two files with one
 * identity the first by name is the save and the other is stale, whatever
 * order the listing gives: a restore that writes (fzc.dat is missing) keeps
 * f_zero.dat and drops f_zero.dat0. */
static void check_folder_order(const sigil_sync_result *sorted) {
    if (!sorted) return;
    mem_root root = {0};
    for (size_t i = 6; i-- > 0;) {
        if (strcmp(FZERO_FILES[i], "8P-GFZE-fzc.dat.gci") == 0) continue;
        size_t len = 0;
        uint8_t *data = sample("fzero-gx-dolphin-gci-set", FZERO_FILES[i], &len);
        if (!data) { fail("folder order", "setup failed"); root_free(&root); return; }
        char path[512];
        snprintf(path, sizeof(path), LIB_FOLDER "%s", FZERO_FILES[i]);
        root_put(&root, path, data, len);
        free(data);
    }
    game g;
    make_game(&g, &root, "dolphin", FZERO, "GFZE", FZERO_DISC);
    g.req.overwrite_local = 1;
    sigil_sync_result *r = NULL;
    if (sigil_restore(&g.req, sorted->data, sorted->len, &r) != SIGIL_OK ||
        !root_find(&root, LIB_FOLDER "8P-GFZE-f_zero.dat.gci") || root_find(&root, LIB_FOLDER "8P-GFZE-f_zero.dat0.gci")) {
        fail("folder order", "a listing in another order kept the other file of a duplicate pair");
    }
    sigil_sync_result_free(r);
    root_free(&root);
}

/* The unit restored into an empty Dolphin folder lands as Dolphin's file
 * names and collects back to the same saves; a game's file the unit lacks is
 * removed, and so is a stale duplicate. */
static void check_folder_restore(const sigil_sync_result *unit) {
    if (!unit) return;
    mem_root empty = {0};
    game g;
    make_game(&g, &empty, "dolphin", FZERO, "GFZE", FZERO_DISC);
    sigil_sync_result *r = NULL, *back = NULL;
    if (sigil_restore(&g.req, unit->data, unit->len, &r) != SIGIL_OK || empty.count != 5 ||
        !root_find(&empty, LIB_FOLDER "8P-GFZE-fzc.dat.gci")) {
        fail("folder restore", "the saves didn't land as Dolphin's files");
    } else {
        refresh(&g, &empty);
        g.req.state = r->state;
        g.req.state_len = r->state_len;
        if (sigil_collect(&g.req, &back) != SIGIL_OK || strcmp(back->identity_hash, unit->identity_hash) != 0 || back->changed) {
            fail("folder restore", "saves differ after the trip");
        }
    }
    sigil_sync_result_free(back);
    sigil_sync_result_free(r);
    root_free(&empty);

    mem_root four = {0};
    put_fzero(&four, LIB_FOLDER, "8P-GFZE-fzc.dat.gci");
    game h;
    make_game(&h, &four, "dolphin", FZERO, "GFZE", FZERO_DISC);
    sigil_sync_result *smaller = NULL;
    if (sigil_collect(&h.req, &smaller) == SIGIL_OK && smaller->data) {
        mem_root full = {0};
        put_fzero(&full, LIB_FOLDER, NULL);
        game k;
        make_game(&k, &full, "dolphin", FZERO, "GFZE", FZERO_DISC);
        k.req.overwrite_local = 1;
        r = NULL;
        if (sigil_restore(&k.req, smaller->data, smaller->len, &r) != SIGIL_OK) {
            fail("folder restore", "restore over a fuller folder failed");
        } else if (root_find(&full, LIB_FOLDER "8P-GFZE-fzc.dat.gci") || root_find(&full, LIB_FOLDER "8P-GFZE-f_zero.dat0.gci") ||
                   full.count != 4) {
            fail("folder restore", "a save the unit lacks, or a stale duplicate, is still there");
        }
        k.req.remove = NULL;
        sigil_sync_result_free(r);
        r = NULL;
        root_free(&full);
        put_fzero(&full, LIB_FOLDER, NULL);
        make_game(&k, &full, "dolphin", FZERO, "GFZE", FZERO_DISC);
        k.req.overwrite_local = 1;
        k.req.remove = NULL;
        if (sigil_restore(&k.req, smaller->data, smaller->len, &r) != SIGIL_ERR_INVALID_ARG || full.writes != 0) {
            fail("folder restore", "a restore that has to remove files ran without a remove callback");
        }
        sigil_sync_result_free(r);
        root_free(&full);
    } else {
        fail("folder restore", "setup failed");
    }
    sigil_sync_result_free(smaller);
    root_free(&four);
}

/* A raw card (SlotA = 1): collect lifts the game's saves off the shared
 * card; restore puts them back without touching other games' saves, and a
 * fresh card is made when there is none. F-Zero GX's save binds to the card
 * serial, and its identity doesn't move with the card. */
static void check_raw_card(const sigil_sync_result *folder_unit) {
    size_t len = 0;
    uint8_t *card = sample("card-raw-usa", NULL, &len);
    if (!card || !folder_unit) { free(card); fail("raw card", "setup failed"); return; }
    mem_root root = {0};
    root_put(&root, LIB_RAW, card, len);
    game g;
    make_game(&g, &root, "dolphin", FZERO, "GFZE", FZERO_DISC);
    raw_mode(&g);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || r->shape != SIGIL_SAVE_SHAPE_SINGLE ||
        strcmp(r->artifact, "8P-GFZE-f_zero.dat.gci") != 0) {
        fail("raw card", "the game's one save isn't a single .gci named as Dolphin names it");
    } else {
        mem_root empty = {0};
        game fresh;
        make_game(&fresh, &empty, "dolphin", FZERO, "GFZE", FZERO_DISC);
        raw_mode(&fresh);
        sigil_sync_result *restored = NULL, *back = NULL;
        if (sigil_restore(&fresh.req, r->data, r->len, &restored) != SIGIL_OK || !root_find(&empty, LIB_RAW)) {
            fail("raw card", "restore into an empty root didn't make the card");
        } else {
            mem_file *made = root_find(&empty, LIB_RAW);
            sigil_card_listing *l = NULL;
            uint32_t first = 0xFFFF;
            if (sigil_gamecube_card_list(made->data, made->len, &l) == SIGIL_OK) {
                for (size_t i = 0; i < l->entry_count; i++) {
                    if (strcmp(l->entries[i].name, "f_zero.dat") == 0) first = l->entries[i].first_block;
                }
            }
            sigil_card_listing_free(l);
            uint8_t *copy = (uint8_t *)malloc(made->len);
            memcpy(copy, made->data, made->len);
            if (first == 0xFFFF || sigil_gamecube_bind_serial(copy, made->len, first) != SIGIL_OK ||
                memcmp(copy, made->data, made->len) != 0) {
                fail("raw card", "F-Zero's save on the new card isn't bound to that card");
            }
            free(copy);
            refresh(&fresh, &empty);
            fresh.req.state = restored->state;
            fresh.req.state_len = restored->state_len;
            if (sigil_collect(&fresh.req, &back) != SIGIL_OK || strcmp(back->identity_hash, r->identity_hash) != 0) {
                fail("raw card", "F-Zero's save reads as a different save on another card");
            }
        }
        sigil_sync_result_free(back);
        sigil_sync_result_free(restored);
        root_free(&empty);
    }
    sigil_sync_result_free(r);
    r = NULL;

    g.req.overwrite_local = 1;
    if (sigil_restore(&g.req, folder_unit->data, folder_unit->len, &r) != SIGIL_OK) {
        fail("raw card", "the folder unit didn't go onto the raw card");
    } else {
        mem_file *f = root_find(&root, LIB_RAW);
        if (card_saves_of(f->data, f->len, FZERO) != 5) fail("raw card", "the card doesn't hold the game's five saves");
        static const char *const OTHERS[] = { "474D5345", "474E3345", "47494B45", "47454445", "47525345" };
        for (size_t i = 0; i < 5; i++) {
            if (card_saves_of(f->data, f->len, OTHERS[i]) != card_saves_of(card, len, OTHERS[i])) {
                fail("raw card", "another game's save went missing");
            }
        }
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(card);
}

/* Dolphin keeps a Japanese game's saves under JAP, and standalone Dolphin
 * roots its paths at the User folder. */
static void check_regions_and_standalone(void) {
    size_t len = 0;
    uint8_t *bleach = sample("bleach-gc-jp-gci", NULL, &len);
    if (!bleach) return;
    mem_root root = {0};
    root_put(&root, "GC/JAP/Card A/8P-GIGJ-bleach.gci", bleach, len);
    game g;
    make_game(&g, &root, "dolphin_standalone", "4749474A", "GIGJ", "Bleach GC (Japan).iso");
    sigil_sync_result *r = NULL, *restored = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data) {
        fail("regions", "a Japanese game's save under GC/JAP wasn't found");
    } else {
        mem_root empty = {0};
        game fresh;
        make_game(&fresh, &empty, "dolphin_standalone", "4749474A", "GIGJ", "Bleach GC (Japan).iso");
        if (sigil_restore(&fresh.req, r->data, r->len, &restored) != SIGIL_OK || empty.count != 1 ||
            strncmp(empty.files[0].path, "GC/JAP/Card A/", 14) != 0) {
            fail("regions", "the save didn't go under GC/JAP/Card A");
        }
        root_free(&empty);
    }
    sigil_sync_result_free(restored);
    sigil_sync_result_free(r);
    root_free(&root);
    free(bleach);
}

/* Dolphin escapes characters a file name can't hold, control characters
 * among them, as __xx__, and a double underscore as __5f____5f__. */
static void check_escaped_names(void) {
    static const struct { const char *name; size_t len; const char *file; } NAMES[] = {
        { "a/b__c?", 8, "69-GUGE-a__2f__b__5f____5f__c__3f__.gci" },
        { "d\x01\"*:<>\\|\x7f", 11, "69-GUGE-d__01____22____2a____3a____3c____3e____5c____7c____7f__.gci" },
        /* A USA save's name is CP1252: é is U+00E9, 0x80 the euro sign; 0x81 has no character. */
        { "NFSU\xE9\x80\x81", 8, "69-GUGE-NFSU\xC3\xA9\xE2\x82\xAC__81__.gci" },
        /* 0x8A is Š (U+0160) and 0x83 ƒ (U+0192): two UTF-8 bytes past U+00FF. */
        { "S\x8A\x83", 3, "69-GUGE-S\xC5\xA0\xC6\x92.gci" },
    };
    size_t len = 0;
    uint8_t *gci = sample("nfsu2-gci", NULL, &len);
    if (!gci) return;
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        memset(gci + GCI_NAME, 0, 32);
        memcpy(gci + GCI_NAME, NAMES[i].name, NAMES[i].len);
        mem_root src = {0};
        root_put(&src, SA_FOLDER "x.gci", gci, len);
        sigil_sync_result *r = unit_from(&src, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO, false), *restored = NULL;
        char want[SIGIL_SAVE_PATH_MAX];
        snprintf(want, sizeof(want), SA_FOLDER "%s", NAMES[i].file);
        if (!r) {
            fail("escaped names", "collect failed");
        } else {
            if (strcmp(r->artifact, NAMES[i].file) != 0) fail("escaped names", "artifact isn't escaped as Dolphin escapes");
            mem_root empty = {0};
            game fresh;
            make_game(&fresh, &empty, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO);
            if (sigil_restore(&fresh.req, r->data, r->len, &restored) != SIGIL_OK || !root_find(&empty, want)) {
                fail("escaped names", "the file isn't named as Dolphin names it");
            }
            root_free(&empty);
        }
        sigil_sync_result_free(restored);
        sigil_sync_result_free(r);
        root_free(&src);
    }
    free(gci);

    /* A Japanese save's name is Shift-JIS, which Dolphin decodes; sigil
     * escapes those bytes, so the name is always UTF-8 and Dolphin, which
     * loads every .gci in the folder, still loads it. */
    uint8_t *jp = sample("bleach-gc-jp-gci", NULL, &len);
    if (!jp) return;
    memset(jp + GCI_NAME, 0, 32);
    memcpy(jp + GCI_NAME, "bl\x82\xA0", 4);
    mem_root src = {0};
    root_put(&src, "GC/JAP/Card A/x.gci", jp, len);
    sigil_sync_result *r = unit_from(&src, "dolphin_standalone", "4749474A", "GIGJ", "Bleach GC (Japan).iso", false);
    if (!r || strcmp(r->artifact, "8P-GIGJ-bl__82____a0__.gci") != 0) {
        fail("escaped names", "a Japanese save's Shift-JIS bytes weren't escaped");
    }
    sigil_sync_result_free(r);
    root_free(&src);
    free(jp);
}

/* A companion in Dolphin's GCI folder: its .gci files go in beside the game's,
 * stay out of the game's unit, come back as its own unit, and stay put when a
 * later restore carries no unit for it. */
static void check_folder_companion(const sigil_sync_result *fzero) {
    size_t len = 0;
    uint8_t *gci = sample("nfsu2-gci", NULL, &len);
    if (!gci || !fzero) { free(gci); fail("folder companion", "setup failed"); return; }
    mem_root src = {0};
    root_put(&src, "GC/USA/Card A/69-GUGE-NFSU2.gci", gci, len);
    game a;
    make_game(&a, &src, "dolphin_standalone", "47554745", "GUGE", "NFSU2 (USA).iso");
    sigil_sync_result *own = NULL;
    sigil_collect(&a.req, &own);
    if (!own || !own->data) { fail("folder companion", "setup collect failed"); goto done; }

    mem_root root = {0};
    game g;
    const char *companion_ids[] = { FZERO };
    sigil_sync_companion companion = { companion_ids, 1, fzero->data, fzero->len };
    make_game(&g, &root, "dolphin_standalone", "47554745", "GUGE", "NFSU2 (USA).iso");
    g.req.companions = &companion;
    g.req.companion_count = 1;
    sigil_sync_result *r = NULL, *c = NULL;
    if (sigil_restore(&g.req, own->data, own->len, &r) != SIGIL_OK || root.count != 6 ||
        !root_find(&root, "GC/USA/Card A/8P-GFZE-fzc.dat.gci")) {
        fail("folder companion", "the companion's files didn't go in beside the game's");
    } else {
        refresh(&g, &root);
        g.req.state = r->state;
        g.req.state_len = r->state_len;
        companion.unit = NULL;
        companion.unit_len = 0;
        if (sigil_collect(&g.req, &c) != SIGIL_OK || strcmp(c->identity_hash, own->identity_hash) != 0 || c->changed) {
            fail("folder companion", "the companion's saves went into the game's unit");
        } else if (c->companion_count != 1 || !c->companions[0].data ||
                   strcmp(c->companions[0].identity_hash, fzero->identity_hash) != 0 || c->companions[0].changed) {
            fail("folder companion", "collect didn't return the companion's saves as its unit");
        }
        sigil_sync_result_free(c);
        c = NULL;
        sigil_sync_result *again = NULL;
        mem_file *mine = NULL;
        for (size_t i = 0; i < root.count; i++) {
            if (strstr(root.files[i].path, "/69-GUGE-")) mine = &root.files[i];
        }
        if (!mine) { fail("folder companion", "the game's file isn't there"); }
        else mine->data[mine->len - 1] ^= 0xFF;
        g.req.overwrite_local = 1;
        int writes = root.writes;
        if (sigil_restore(&g.req, own->data, own->len, &again) != SIGIL_OK || root.writes == writes || root.count != 6 ||
            root.removes != 0) {
            fail("folder companion", "a restore without the companion's unit removed its files");
        }
        sigil_sync_result_free(again);
    }
    sigil_sync_result_free(r);
    root_free(&root);

    size_t jp_len = 0;
    uint8_t *jp = sample("bleach-gc-jp-gci", NULL, &jp_len);
    mem_root jp_src = {0};
    if (jp) root_put(&jp_src, "GC/JAP/Card A/8P-GIGJ-bleach.gci", jp, jp_len);
    game b;
    make_game(&b, &jp_src, "dolphin_standalone", "4749474A", "GIGJ", "Bleach GC (Japan).iso");
    sigil_sync_result *bleach = NULL;
    if (!jp || sigil_collect(&b.req, &bleach) != SIGIL_OK || !bleach->data) {
        fail("folder companion", "Japanese setup failed");
    } else {
        mem_root other = {0};
        const char *jp_ids[] = { "4749474A" };
        sigil_sync_companion foreign = { jp_ids, 1, bleach->data, bleach->len };
        make_game(&g, &other, "dolphin_standalone", "47554745", "GUGE", "NFSU2 (USA).iso");
        g.req.companions = &foreign;
        g.req.companion_count = 1;
        r = NULL;
        if (sigil_restore(&g.req, own->data, own->len, &r) != SIGIL_ERR_REGION || other.writes != 0) {
            fail("folder companion", "a companion from another region went on the game's card");
        } else if (!r || strncmp(r->problem, "4749474A-", 9) != 0) {
            fail("folder companion", "the region refusal doesn't name the companion's save");
        }
        sigil_sync_result_free(r);
        root_free(&other);
    }
    sigil_sync_result_free(bleach);
    root_free(&jp_src);
    free(jp);
done:
    sigil_sync_result_free(own);
    root_free(&src);
    free(gci);
}

/* Another game's file under the name Dolphin gives the incoming save: the save
 * goes in under the name with a 0 before .gci, another 0 for each name still
 * taken, as Dolphin's GCI folder names one, and the other game's file stays
 * as it was. */
static void check_digit_suffix(const sigil_sync_result *nfsu2) {
    size_t len = 0;
    uint8_t *other = sample("bleach-gc-jp-gci", NULL, &len);
    if (!other || !nfsu2) { free(other); fail("digit suffix", "setup failed"); return; }
    char taken_paths[3][SIGIL_SAVE_PATH_MAX];
    int stem_len = (int)strlen(nfsu2->artifact) - 4;
    snprintf(taken_paths[0], SIGIL_SAVE_PATH_MAX, SA_FOLDER "%s", nfsu2->artifact);
    snprintf(taken_paths[1], SIGIL_SAVE_PATH_MAX, SA_FOLDER "%.*s0.gci", stem_len, nfsu2->artifact);
    snprintf(taken_paths[2], SIGIL_SAVE_PATH_MAX, SA_FOLDER "%.*s00.gci", stem_len, nfsu2->artifact);
    const char *TAKEN[] = { taken_paths[0], taken_paths[1] };
    for (size_t taken = 1; taken <= 2; taken++) {
        mem_root root = {0};
        for (size_t i = 0; i < taken; i++) root_put(&root, TAKEN[i], other, len);
        game g;
        make_game(&g, &root, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO);
        sigil_sync_result *r = NULL;
        const char *want = taken_paths[taken];
        if (sigil_restore(&g.req, nfsu2->data, nfsu2->len, &r) != SIGIL_OK || root.writes != 1 ||
            strcmp(root.first_write, want) != 0) {
            fail("digit suffix", "the save didn't go in under the next free numbered name");
        }
        bool kept = true;
        for (size_t i = 0; i < taken; i++) kept = kept && memcmp(root_find(&root, TAKEN[i])->data, other, len) == 0;
        if (!kept) fail("digit suffix", "another game's file was overwritten");
        sigil_sync_result_free(r);
        root_free(&root);
    }
    free(other);
}

/* A companion the restore carries a unit for loses the files its unit lacks,
 * as the game does. */
static void check_companion_removal(const sigil_sync_result *nfsu2) {
    mem_root four = {0};
    if (!nfsu2 || !put_fzero(&four, SA_FOLDER, "8P-GFZE-fzc.dat.gci")) { fail("companion removal", "setup failed"); root_free(&four); return; }
    sigil_sync_result *smaller = unit_from(&four, "dolphin_standalone", FZERO, "GFZE", FZERO_DISC, false);
    mem_root root = {0};
    size_t len = 0;
    uint8_t *gci = sample("nfsu2-gci", NULL, &len);
    put_fzero(&root, SA_FOLDER, NULL);
    if (gci) root_put(&root, NFSU2_FILE, gci, len);
    const char *ids[] = { FZERO };
    sigil_sync_companion companion = { ids, 1, smaller ? smaller->data : NULL, smaller ? smaller->len : 0 };
    game g;
    make_game(&g, &root, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO);
    g.req.companions = &companion;
    g.req.companion_count = 1;
    g.req.overwrite_local = 1;
    sigil_sync_result *r = NULL;
    if (!smaller || !gci || sigil_restore(&g.req, nfsu2->data, nfsu2->len, &r) != SIGIL_OK) {
        fail("companion removal", "restore failed");
    } else if (root_find(&root, SA_FOLDER "8P-GFZE-fzc.dat.gci") || root_find(&root, SA_FOLDER "8P-GFZE-f_zero.dat0.gci") ||
               !root_find(&root, SA_FOLDER "8P-GFZE-f_zero.dat.gci") || !root_find(&root, NFSU2_FILE) || root.count != 5) {
        fail("companion removal", "the companion's file its unit lacks, or its stale duplicate, is still there");
    }
    sigil_sync_result_free(r);
    sigil_sync_result_free(smaller);
    root_free(&root);
    root_free(&four);
    free(gci);
}

/* A new raw card is Dolphin's 2043-block card, Shift-JIS for a Japanese
 * game; a smaller card already there keeps its size and name. */
static void check_raw_card_forms(const sigil_sync_result *raw_unit) {
    size_t len = 0;
    uint8_t *bleach = sample("bleach-gc-jp-gci", NULL, &len);
    if (!bleach || !raw_unit) { free(bleach); fail("raw card forms", "setup failed"); return; }
    mem_root src = {0};
    root_put(&src, "GC/JAP/Card A/8P-GIGJ-bleach.gci", bleach, len);
    sigil_sync_result *jp = unit_from(&src, "dolphin_standalone", "4749474A", "GIGJ", "Bleach GC (Japan).iso", false);
    mem_root root = {0};
    game g;
    make_game(&g, &root, "dolphin_standalone", "4749474A", "GIGJ", "Bleach GC (Japan).iso");
    raw_mode(&g);
    sigil_sync_result *r = NULL;
    mem_file *f = NULL;
    if (!jp || sigil_restore(&g.req, jp->data, jp->len, &r) != SIGIL_OK || !(f = root_find(&root, "GC/MemoryCardA.JAP.raw")) ||
        f->len != GC_MAX_CARD_SIZE || f->data[0x24] != 0 || f->data[0x25] != 1) {
        fail("raw card forms", "a Japanese game's new card isn't a 2043-block Shift-JIS card");
    }
    sigil_sync_result_free(r);
    r = NULL;
    root_free(&root);

    make_game(&g, &root, "dolphin", FZERO, "GFZE", FZERO_DISC);
    raw_mode(&g);
    if (sigil_restore(&g.req, raw_unit->data, raw_unit->len, &r) != SIGIL_OK || !(f = root_find(&root, LIB_RAW)) ||
        f->len != GC_MAX_CARD_SIZE || f->data[0x24] != 0 || f->data[0x25] != 0) {
        fail("raw card forms", "a USA game's new card isn't a 2043-block Windows-1252 card");
    }
    sigil_sync_result_free(r);
    r = NULL;
    root_free(&root);

    size_t small_len = 64u * GC_BLOCK_SIZE;
    uint8_t *small = (uint8_t *)malloc(small_len);
    sigil_gamecube_format(small, small_len, false);
    root_put(&root, "User/GC/MemoryCardA.USA.59.raw", small, small_len);
    make_game(&g, &root, "dolphin", FZERO, "GFZE", FZERO_DISC);
    raw_mode(&g);
    if (sigil_restore(&g.req, raw_unit->data, raw_unit->len, &r) != SIGIL_OK || root.count != 1 ||
        !(f = root_find(&root, "User/GC/MemoryCardA.USA.59.raw")) || f->len != small_len ||
        card_saves_of(f->data, f->len, FZERO) != 1) {
        fail("raw card forms", "the 59-block card there didn't take the save at its size");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(small);
    sigil_sync_result_free(jp);
    root_free(&src);
    free(bleach);
}

/* A save bigger than the whole card there is refused with the blocks it
 * lacks, not read as a missing directory slot. */
static void check_save_bigger_than_card(void) {
    size_t len = 0;
    uint8_t *gci = sample("nfsu2-gci", NULL, &len);
    if (!gci) return;
    const uint16_t blocks = 70;
    size_t big_len = GC_DENTRY_SIZE + (size_t)blocks * GC_BLOCK_SIZE;
    uint8_t *big = (uint8_t *)calloc(1, big_len);
    memcpy(big, gci, len);
    big[0x38] = (uint8_t)(blocks >> 8);
    big[0x39] = (uint8_t)blocks;
    size_t card_len = 64u * GC_BLOCK_SIZE;
    uint8_t *card = (uint8_t *)malloc(card_len);
    sigil_gamecube_format(card, card_len, false);
    mem_root root = {0};
    root_put(&root, "GC/MemoryCardA.USA.59.raw", card, card_len);
    game g;
    make_game(&g, &root, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO);
    raw_mode(&g);
    sigil_sync_result *r = NULL;
    if (sigil_restore(&g.req, big, big_len, &r) != SIGIL_ERR_NO_SPACE || !r || r->blocks_short != blocks - 59u ||
        strncmp(r->problem, NFSU2 "-", 9) != 0 || root.writes != 0) {
        fail("save bigger than card", "the refusal doesn't say how many blocks the save lacks");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(card);
    free(big);
    free(gci);
}

/* Dolphin loads the running game's .gci files first, then other games' in
 * name order while each leaves 204 of the card's blocks free
 * (GCMemcardDirectory). A companion counts as another game, so in a folder
 * holding 1830 blocks of other saves its first file wouldn't load: restore
 * refuses naming it, with the blocks it lacks, and writes nothing. */
static void check_folder_capacity(const sigil_sync_result *nfsu2, const sigil_sync_result *fzero) {
    size_t len = 0;
    uint8_t *gci = sample("nfsu2-gci", NULL, &len);
    size_t big_len = GC_DENTRY_SIZE + 610u * GC_BLOCK_SIZE;
    uint8_t *big = (uint8_t *)calloc(1, big_len);
    if (!gci || !big || !nfsu2 || !fzero) { fail("folder capacity", "setup failed"); free(gci); free(big); return; }
    mem_root root = {0};
    for (int i = 0; i < 3; i++) {
        memcpy(big, gci, GC_DENTRY_SIZE);
        memcpy(big, i == 0 ? "GOTA" : i == 1 ? "GOTB" : "GOTC", 4);
        big[0x38] = (uint8_t)(610 >> 8);
        big[0x39] = (uint8_t)(610 & 0xFF);
        char path[SIGIL_SAVE_PATH_MAX];
        snprintf(path, sizeof(path), SA_FOLDER "01-GOT%c-big.gci", 'A' + i);
        root_put(&root, path, big, big_len);
    }
    const char *ids[] = { FZERO };
    sigil_sync_companion companion = { ids, 1, fzero->data, fzero->len };
    game g;
    make_game(&g, &root, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO);
    g.req.companions = &companion;
    g.req.companion_count = 1;
    sigil_sync_result *r = NULL;
    if (sigil_restore(&g.req, nfsu2->data, nfsu2->len, &r) != SIGIL_ERR_NO_SPACE || !r ||
        strncmp(r->problem, FZERO "-", 9) != 0 || r->blocks_short != 2 || root.writes != 0) {
        fail("folder capacity", "a companion Dolphin wouldn't load went into the folder");
    }
    sigil_sync_result_free(r);
    r = NULL;
    companion.unit = NULL;
    companion.unit_len = 0;
    if (sigil_restore(&g.req, nfsu2->data, nfsu2->len, &r) != SIGIL_OK || root.writes != 1) {
        fail("folder capacity", "the game's own save, which Dolphin loads first, was refused");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(big);
    free(gci);
}

/* Dolphin uses one raw card for slot A, picked by MemoryCardSize (-1 for the
 * 2043-block card, 0 to 4 for 59 to 1019 blocks). With an old .raw beside a
 * newer .59.raw, the option picks; without it sigil can't tell which Dolphin
 * reads, so collect and restore refuse naming both. */
/* A .gci of `blocks` zero blocks for game code `code`, file `name`, on the
 * NFSU2 sample's header. */
static uint8_t *gci_of(const uint8_t *like, const char *code, const char *name, uint32_t blocks, size_t *len) {
    *len = GC_DENTRY_SIZE + (size_t)blocks * GC_BLOCK_SIZE;
    uint8_t *out = (uint8_t *)calloc(1, *len);
    if (!out) return NULL;
    memcpy(out, like, GC_DENTRY_SIZE);
    memcpy(out, code, 4);
    memset(out + GCI_NAME, 0, 32);
    memcpy(out + GCI_NAME, name, strlen(name));
    out[0x38] = (uint8_t)(blocks >> 8);
    out[0x39] = (uint8_t)blocks;
    return out;
}

static void put_gci(mem_root *root, const uint8_t *like, const char *path, const char *code, const char *name,
                    uint32_t blocks) {
    size_t len = 0;
    uint8_t *gci = gci_of(like, code, name, blocks, &len);
    if (gci) root_put(root, path, gci, len);
    free(gci);
}

/* Restores `unit` as NFSU2 into `root` with `companion` (code, unit) when
 * given; returns the code, with the result in `*out`. */
static int restore_nfsu2(mem_root *root, const uint8_t *unit, size_t len, const char *companion_code,
                         const uint8_t *companion, size_t companion_len, const char *card_size, sigil_sync_result **out) {
    game g;
    make_game(&g, root, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO);
    if (card_size) {
        g.options[0].key = "MemoryCardSize";
        g.options[0].value = card_size;
        g.req.save.options = g.options;
        g.req.save.option_count = 1;
    }
    const char *ids[] = { companion_code };
    sigil_sync_companion c = { ids, 1, companion, companion_len };
    if (companion_code) {
        g.req.companions = &c;
        g.req.companion_count = 1;
    }
    return sigil_restore(&g.req, unit, len, out);
}

/* Dolphin's GCI folder rules, each against what Dolphin does when it loads
 * the folder (GCMemcardDirectory): its size follows MemoryCardSize; it finds
 * .gci in any case; it writes over another game's file past ten inserted
 * 0s; only the disc's own code loads first; a second file of one identity
 * takes no blocks; other games keep a tenth of the data blocks free, to the
 * block; and loading stops past 112 saves. */
static void check_dolphin_folder_rules(const sigil_sync_result *nfsu2) {
    size_t len = 0;
    uint8_t *like = sample("nfsu2-gci", NULL, &len);
    if (!like || !nfsu2) { free(like); fail("dolphin folder", "setup failed"); return; }
    uint32_t own = (uint32_t)(like[0x38] << 8 | like[0x39]);
    sigil_sync_result *r = NULL;

    size_t big_len = 0;
    uint8_t *big = gci_of(like, "GUGE", "NFSU2", 100, &big_len);
    mem_root root = {0};
    if (!big || restore_nfsu2(&root, big, big_len, NULL, NULL, 0, "0", &r) != SIGIL_ERR_NO_SPACE || !r ||
        r->blocks_short != 100 - 59 || root.writes != 0) {
        fail("dolphin folder", "a save bigger than a 59-block folder card went in");
    }
    sigil_sync_result_free(r);
    r = NULL;
    root_free(&root);
    free(big);

    char upper[SIGIL_SAVE_PATH_MAX];
    snprintf(upper, sizeof(upper), SA_FOLDER "69-GUGE-NFSU2.GCI");
    memset(&root, 0, sizeof(root));
    root_put(&root, upper, like, len);
    game g;
    make_game(&g, &root, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO);
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data) fail("dolphin folder", "a .GCI file wasn't read");
    sigil_sync_result_free(r);
    r = NULL;
    root_free(&root);

    memset(&root, 0, sizeof(root));
    char path[SIGIL_SAVE_PATH_MAX];
    int stem = (int)strlen(nfsu2->artifact) - 4;
    for (int zeros = 0; zeros <= 10; zeros++) {
        snprintf(path, sizeof(path), SA_FOLDER "%.*s%.*s.gci", stem, nfsu2->artifact, zeros, "0000000000");
        put_gci(&root, like, path, "GOTA", "other", 1);
    }
    if (restore_nfsu2(&root, nfsu2->data, nfsu2->len, NULL, NULL, 0, NULL, &r) != SIGIL_ERR_EXISTS || !r ||
        strncmp(r->problem, NFSU2, 8) != 0 || root.writes != 0) {
        fail("dolphin folder", "the save went over another game's file once every name was taken");
    }
    sigil_sync_result_free(r);
    r = NULL;
    root_free(&root);

    /* Other games' files: free blocks are 2043 less these and NFSU2's own. */
    size_t fit_len = 0, over_len = 0;
    uint32_t reserve = 2043 / 10, room = 2043 - own - 1200;
    uint8_t *fit = gci_of(like, "GOTE", "comp", room - reserve, &fit_len);
    uint8_t *over = gci_of(like, "GOTE", "comp", room - reserve + 1, &over_len);
    for (int which = 0; which < 2 && fit && over; which++) {
        memset(&root, 0, sizeof(root));
        put_gci(&root, like, SA_FOLDER "01-GOTB-big.gci", "GOTB", "big", 600);
        put_gci(&root, like, SA_FOLDER "01-GOTC-big.gci", "GOTC", "big", 600);
        put_gci(&root, like, SA_FOLDER "01-GOTB-big0.gci", "GOTB", "big", 600);
        int rc = restore_nfsu2(&root, nfsu2->data, nfsu2->len, "474F5445", which ? over : fit, which ? over_len : fit_len,
                               NULL, &r);
        if (which == 0 && rc != SIGIL_OK) {
            fail("dolphin folder", "a companion leaving exactly the reserve free, past a duplicate identity, was refused");
        }
        if (which == 1 && (rc != SIGIL_ERR_NO_SPACE || !r || r->blocks_short != 1)) {
            fail("dolphin folder", "a companion one block into the reserve went in");
        }
        sigil_sync_result_free(r);
        r = NULL;
        root_free(&root);
    }
    free(fit);
    free(over);

    /* The disc's code loads first; another id of the game is another game. */
    memset(&root, 0, sizeof(root));
    put_gci(&root, like, SA_FOLDER "00-AAAA-big.gci", "AAAA", "big", 1800);
    big = gci_of(like, "GUGE", "NFSU2", 300, &big_len);
    if (!big || restore_nfsu2(&root, big, big_len, NULL, NULL, 0, NULL, &r) != SIGIL_OK) {
        fail("dolphin folder", "the running game's save didn't load ahead of another game's");
    }
    sigil_sync_result_free(r);
    r = NULL;
    root_free(&root);
    free(big);

    memset(&root, 0, sizeof(root));
    put_gci(&root, like, NFSU2_FILE, "GUGE", "NFSU2", own);
    put_gci(&root, like, SA_FOLDER "ZZ-GOTE-big.gci", "GOTE", "big", 620);
    make_game(&g, &root, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO);
    const char *both[] = { NFSU2, "474F5445" };
    g.req.game_ids = both;
    g.req.game_id_count = 2;
    sigil_sync_result *two = NULL;
    if (sigil_collect(&g.req, &two) != SIGIL_OK || !two->data) {
        fail("dolphin folder", "setup collect failed");
    } else {
        mem_root dest = {0};
        put_gci(&dest, like, SA_FOLDER "01-GOTB-big.gci", "GOTB", "big", 610);
        put_gci(&dest, like, SA_FOLDER "01-GOTC-big.gci", "GOTC", "big", 610);
        make_game(&g, &dest, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO);
        g.req.game_ids = both;
        g.req.game_id_count = 2;
        if (sigil_restore(&g.req, two->data, two->len, &r) != SIGIL_ERR_NO_SPACE || !r || strstr(r->problem, "big") == NULL) {
            fail("dolphin folder", "another id's save that Dolphin skips went in as the running game's");
        }
        sigil_sync_result_free(r);
        r = NULL;
        root_free(&dest);
    }
    sigil_sync_result_free(two);
    root_free(&root);

    /* A game file the restore removes takes no blocks. */
    memset(&root, 0, sizeof(root));
    put_gci(&root, like, SA_FOLDER "69-GUGE-AAA.gci", "GUGE", "AAA", 1500);
    big = gci_of(like, "GUGE", "NFSU2", 700, &big_len);
    make_game(&g, &root, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO);
    g.req.overwrite_local = 1;
    if (!big || sigil_restore(&g.req, big, big_len, &r) != SIGIL_OK || root.removes != 1) {
        fail("dolphin folder", "a game file the restore removes still counted against the folder");
    }
    sigil_sync_result_free(r);
    r = NULL;
    root_free(&root);
    free(big);

    /* A card directory holds 127 saves, and Dolphin loads no more of the
     * running game's: a folder of 128 collects whole, and its unit restores
     * nowhere, refused naming the 128th in name order with no blocks short,
     * as a missing directory slot reads. */
    memset(&root, 0, sizeof(root));
    for (int i = 0; i < 128; i++) {
        char name[8];
        snprintf(name, sizeof(name), "g%03d", i);
        snprintf(path, sizeof(path), SA_FOLDER "69-GUGE-%s.gci", name);
        put_gci(&root, like, path, "GUGE", name, 1);
    }
    make_game(&g, &root, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO);
    sigil_sync_result *many = NULL;
    if (sigil_collect(&g.req, &many) != SIGIL_OK || !many->data || zip_count(many->data, many->len) != 128) {
        fail("dolphin folder", "128 saves of one game didn't collect into one unit");
    } else {
        for (int raw = 0; raw < 2; raw++) {
            mem_root dest = {0};
            make_game(&g, &dest, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO);
            if (raw) raw_mode(&g);
            if (sigil_restore(&g.req, many->data, many->len, &r) != SIGIL_ERR_NO_SPACE || !r ||
                strcmp(r->problem, NFSU2 "-3639-g127") != 0 || r->blocks_short != 0 || dest.writes != 0) {
                fail("dolphin folder", raw ? "a 128th save went onto a raw card's full directory"
                                           : "a 128th save of the running game went into the folder");
            }
            sigil_sync_result_free(r);
            r = NULL;
            root_free(&dest);
        }
    }
    sigil_sync_result_free(many);
    root_free(&root);

    /* A unit holding two files of one identity keeps the first in name order
     * and drops the other, as Dolphin loads a GCI folder: the copy comes
     * first in the zip but last by name, next to the original or 126 saves
     * apart. */
    static sigil_zip_member members[129];
    for (size_t gap = 1; gap <= 126; gap += 125) {
        bool built = true;
        for (size_t i = 0; i <= gap; i++) {
            char name[8];
            snprintf(name, sizeof(name), "g%03zu", i == 0 ? (size_t)0 : i - 1);
            snprintf(members[i].name, sizeof(members[i].name), i == 0 ? "z-copy.gci" : "m%03zu.gci", i);
            members[i].data = gci_of(like, "GUGE", name, 1, &members[i].len);
            built = built && members[i].data;
            if (i == 0 && members[i].data) members[i].data[GC_DENTRY_SIZE] = 0x5A;
        }
        uint8_t *zip = NULL;
        size_t zip_len = 0;
        memset(&root, 0, sizeof(root));
        bool kept_first = false, kept_copy = false;
        int rc = built && sigil_zip_store(members, gap + 1, &zip, &zip_len) == SIGIL_OK
                     ? restore_nfsu2(&root, zip, zip_len, NULL, NULL, 0, NULL, &r)
                     : SIGIL_ERR_INVALID_ARG;
        for (size_t f = 0; f < root.count; f++) {
            const mem_file *file = &root.files[f];
            if (file->len <= GC_DENTRY_SIZE || memcmp(file->data + GCI_NAME, "g000", 5) != 0) continue;
            kept_first = kept_first || file->data[GC_DENTRY_SIZE] == 0;
            kept_copy = kept_copy || file->data[GC_DENTRY_SIZE] == 0x5A;
        }
        if (rc != SIGIL_OK || root.count != gap || !kept_first || kept_copy) {
            fail("dolphin folder", gap == 1 ? "a unit holding one save twice didn't keep the first by name"
                                            : "a unit holding one save twice, 126 saves apart, didn't keep the first by name");
        }
        sigil_sync_result_free(r);
        r = NULL;
        free(zip);
        root_free(&root);
        for (size_t i = 0; i <= gap; i++) free(members[i].data);
    }

    /* Past 112 loaded saves Dolphin stops loading other games' files. */
    memset(&root, 0, sizeof(root));
    for (int i = 0; i < 112; i++) {
        snprintf(path, sizeof(path), SA_FOLDER "00-O%03d-s.gci", i);
        char code[5];
        snprintf(code, sizeof(code), "O%03d", i);
        put_gci(&root, like, path, code, "s", 1);
    }
    size_t one_len = 0;
    uint8_t *one = gci_of(like, "GOTE", "comp", 1, &one_len);
    if (!one || restore_nfsu2(&root, nfsu2->data, nfsu2->len, "474F5445", one, one_len, NULL, &r) != SIGIL_ERR_NO_SPACE) {
        fail("dolphin folder", "a companion past the 112th save went in");
    }
    sigil_sync_result_free(r);
    r = NULL;
    free(one);
    root_free(&root);
    free(like);
}

static void check_card_size_option(const sigil_sync_result *raw_unit, const sigil_sync_result *fzero) {
    size_t len = 0;
    uint8_t *card = sample("card-raw-usa", NULL, &len);
    size_t small_len = 64u * GC_BLOCK_SIZE;
    uint8_t *small = (uint8_t *)malloc(small_len);
    if (!card || !small || !raw_unit || !fzero || sigil_gamecube_format(small, small_len, false) != SIGIL_OK ||
        sigil_gamecube_inject(small, small_len, raw_unit->data, raw_unit->len) != SIGIL_OK) {
        fail("card size option", "setup failed");
        free(card);
        free(small);
        return;
    }
    static const struct { const char *value; const char *card; } PICKS[] = {
        { "-1", LIB_RAW }, { "0", "User/GC/MemoryCardA.USA.59.raw" },
    };
    for (size_t i = 0; i < 3; i++) {
        mem_root root = {0};
        root_put(&root, LIB_RAW, card, len);
        root_put(&root, "User/GC/MemoryCardA.USA.59.raw", small, small_len);
        game g;
        make_game(&g, &root, "dolphin", FZERO, "GFZE", FZERO_DISC);
        g.options[0].key = "SlotA";
        g.options[0].value = "1";
        g.options[1].key = "MemoryCardSize";
        g.options[1].value = i < 2 ? PICKS[i].value : NULL;
        g.req.save.options = g.options;
        g.req.save.option_count = i < 2 ? 2 : 1;
        sigil_sync_result *r = NULL, *restored = NULL;
        int collected = sigil_collect(&g.req, &r);
        if (i == 2) {
            if (collected != SIGIL_ERR_AMBIGUOUS || !r || !strstr(r->problem, LIB_RAW) ||
                !strstr(r->problem, "MemoryCardA.USA.59.raw")) {
                fail("card size option", "two raw cards and no MemoryCardSize didn't refuse naming both");
            }
            if (sigil_restore(&g.req, raw_unit->data, raw_unit->len, &restored) != SIGIL_ERR_AMBIGUOUS || root.writes != 0) {
                fail("card size option", "restore picked a card without MemoryCardSize");
            }
        } else {
            g.req.overwrite_local = 1;
            if (collected != SIGIL_OK || !r->data) {
                fail("card size option", "collect with MemoryCardSize failed");
            } else if (sigil_restore(&g.req, fzero->data, fzero->len, &restored) != SIGIL_OK || root.writes != 1 ||
                       strcmp(root.first_write, PICKS[i].card) != 0) {
                fail("card size option", "restore didn't write the card MemoryCardSize picks, alone");
            }
        }
        sigil_sync_result_free(restored);
        sigil_sync_result_free(r);
        root_free(&root);
    }
    free(small);
    free(card);
}

/* A raw card restore makes when there is none is the size MemoryCardSize
 * sets, as Dolphin creates it (EXI_DeviceMemoryCard: 59 to 1019 data
 * blocks for 0 to 4, 2043 otherwise), and takes F-Zero GX's saves, bound to
 * the new card's serial. */
static void check_new_card_size(const sigil_sync_result *nfsu2, const sigil_sync_result *fzero) {
    static const struct { const char *value; const char *card; uint32_t blocks; } SIZES[] = {
        { NULL, LIB_RAW, 2048 },
        { "-1", LIB_RAW, 2048 },
        { "4", "User/GC/MemoryCardA.USA.1019.raw", 1024 },
        { "3", "User/GC/MemoryCardA.USA.507.raw", 512 },
        { "2", "User/GC/MemoryCardA.USA.251.raw", 256 },
        { "1", "User/GC/MemoryCardA.USA.123.raw", 128 },
        { "0", "User/GC/MemoryCardA.USA.59.raw", 64 },
    };
    if (!nfsu2 || !fzero) {
        fail("new card size", "setup failed");
        return;
    }
    for (size_t i = 0; i < sizeof(SIZES) / sizeof(SIZES[0]); i++) {
        for (size_t which = 0; which < 2; which++) {
            const sigil_sync_result *unit = which == 0 ? nfsu2 : fzero;
            mem_root root = {0};
            game g;
            if (which == 0) make_game(&g, &root, "dolphin", NFSU2, "GUGE", NFSU2_ISO);
            else make_game(&g, &root, "dolphin", FZERO, "GFZE", FZERO_DISC);
            raw_mode(&g);
            g.options[1].key = "MemoryCardSize";
            g.options[1].value = SIZES[i].value;
            g.req.save.option_count = SIZES[i].value ? 2 : 1;
            sigil_sync_result *r = NULL;
            int rc = sigil_restore(&g.req, unit->data, unit->len, &r);
            mem_file *card = root_find(&root, SIZES[i].card);
            sigil_card_listing *l = NULL;
            char where[64];
            snprintf(where, sizeof(where), "new card size %s, %s", SIZES[i].value ? SIZES[i].value : "unset",
                     which == 0 ? "nfsu2" : "f-zero");
            if (rc != SIGIL_OK || !card) {
                fail(where, "restore didn't write the card MemoryCardSize names");
            } else if (card->len != (size_t)SIZES[i].blocks * GC_BLOCK_SIZE) {
                fail(where, "the new card isn't the size MemoryCardSize sets");
            } else if (sigil_gamecube_card_list(card->data, card->len, &l) != SIGIL_OK ||
                       l->total_blocks != SIZES[i].blocks - GC_SYSTEM_BLOCKS || l->entry_count == 0) {
                fail(where, "the new card's header doesn't give its size, or it holds no save");
            }
            sigil_card_listing_free(l);
            sigil_sync_result_free(r);
            root_free(&root);
        }
    }
}

/* A card with every directory entry taken but blocks to spare refuses with
 * no blocks short: what it lacks is a slot. */
static void check_directory_full(const sigil_sync_result *nfsu2) {
    size_t len = 0;
    uint8_t *gci = sample("nfsu2-gci", NULL, &len);
    uint8_t *card = (uint8_t *)malloc(GC_MAX_CARD_SIZE);
    uint8_t *small = (uint8_t *)malloc(GC_DENTRY_SIZE + GC_BLOCK_SIZE);
    bool ok = gci && card && small && nfsu2 && sigil_gamecube_format(card, GC_MAX_CARD_SIZE, false) == SIGIL_OK;
    for (int i = 0; ok && i < 127; i++) {
        memset(small, 0, GC_DENTRY_SIZE + GC_BLOCK_SIZE);
        memcpy(small, gci, GC_DENTRY_SIZE);
        memcpy(small, "GOTE", 4);
        memset(small + GCI_NAME, 0, 32);
        snprintf((char *)small + GCI_NAME, 32, "slot%03d", i);
        small[0x38] = 0;
        small[0x39] = 1;
        ok = sigil_gamecube_inject(card, GC_MAX_CARD_SIZE, small, GC_DENTRY_SIZE + GC_BLOCK_SIZE) == SIGIL_OK;
    }
    if (!ok) {
        fail("directory full", "setup failed");
    } else {
        mem_root root = {0};
        root_put(&root, "GC/MemoryCardA.USA.raw", card, GC_MAX_CARD_SIZE);
        game g;
        make_game(&g, &root, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO);
        raw_mode(&g);
        sigil_sync_result *r = NULL;
        if (sigil_restore(&g.req, nfsu2->data, nfsu2->len, &r) != SIGIL_ERR_NO_SPACE || !r || r->blocks_short != 0 ||
            strncmp(r->problem, NFSU2 "-", 9) != 0 || root.writes != 0) {
            fail("directory full", "a card out of directory entries didn't refuse with no blocks short");
        }
        sigil_sync_result_free(r);
        root_free(&root);
    }
    free(small);
    free(card);
    free(gci);
}

/* A companion from another region than the game's is refused whichever
 * region each is. */
static void check_foreign_companions(const sigil_sync_result *fzero) {
    size_t len = 0, jp_len = 0;
    uint8_t *gci = sample("nfsu2-gci", NULL, &len);
    uint8_t *jp = sample("bleach-gc-jp-gci", NULL, &jp_len);
    if (!gci || !jp || !fzero) { free(gci); free(jp); fail("foreign companions", "setup failed"); return; }
    uint8_t *pal = gci_with(gci, len, 3, "P", 1);
    mem_root jp_src = {0}, pal_src = {0};
    root_put(&jp_src, "GC/JAP/Card A/8P-GIGJ-bleach.gci", jp, jp_len);
    root_put(&pal_src, "GC/EUR/Card A/69-GUGP-NFSU2.gci", pal, len);
    const struct { const char *what; mem_root *src; const char *title_id, *serial, *content; } GAMES[] = {
        { "Japanese game", &jp_src, "4749474A", "GIGJ", "Bleach GC (Japan).iso" },
        { "European game", &pal_src, "47554750", "GUGP", "NFSU2 (Europe).iso" },
    };
    const char *ids[] = { FZERO };
    for (size_t i = 0; i < 2; i++) {
        sigil_sync_result *own = unit_from(GAMES[i].src, "dolphin_standalone", GAMES[i].title_id, GAMES[i].serial,
                                           GAMES[i].content, false);
        mem_root root = {0};
        sigil_sync_companion companion = { ids, 1, fzero->data, fzero->len };
        game g;
        make_game(&g, &root, "dolphin_standalone", GAMES[i].title_id, GAMES[i].serial, GAMES[i].content);
        g.req.companions = &companion;
        g.req.companion_count = 1;
        sigil_sync_result *r = NULL;
        if (!own || sigil_restore(&g.req, own->data, own->len, &r) != SIGIL_ERR_REGION || root.writes != 0 ||
            strncmp(r->problem, FZERO "-", 9) != 0) {
            fail("foreign companions", GAMES[i].what);
        }
        sigil_sync_result_free(r);
        sigil_sync_result_free(own);
        root_free(&root);
    }
    root_free(&pal_src);
    root_free(&jp_src);
    free(pal);
    free(jp);
    free(gci);
}

/* Dolphin loads only .gci files directly in the folder: a save in a
 * subfolder or under another extension is neither collected nor removed. */
static void check_folder_filters(const sigil_sync_result *fzero) {
    size_t len = 0;
    uint8_t *gci = sample("fzero-gx-dolphin-gci-set", "8P-GFZE-fzc.dat.gci", &len);
    if (!gci || !fzero) { free(gci); fail("folder filters", "setup failed"); return; }
    uint8_t *other = gci_with(gci, len, GCI_NAME, "fzx.dat", 8);
    mem_root root = {0};
    put_fzero(&root, LIB_FOLDER, NULL);
    root_put(&root, LIB_FOLDER "old/8P-GFZE-fzx.dat.gci", other, len);
    root_put(&root, LIB_FOLDER "8P-GFZE-fzx.dat.gci.bak", other, len);
    sigil_sync_result *r = unit_from(&root, "dolphin", FZERO, "GFZE", FZERO_DISC, false);
    if (!r || zip_count(r->data, r->len) != 5) fail("folder filters", "a save outside Dolphin's load joined the unit");
    game g;
    make_game(&g, &root, "dolphin", FZERO, "GFZE", FZERO_DISC);
    g.req.overwrite_local = 1;
    sigil_sync_result *restored = NULL;
    if (sigil_restore(&g.req, fzero->data, fzero->len, &restored) != SIGIL_OK ||
        !root_find(&root, LIB_FOLDER "old/8P-GFZE-fzx.dat.gci") || !root_find(&root, LIB_FOLDER "8P-GFZE-fzx.dat.gci.bak")) {
        fail("folder filters", "restore removed a file Dolphin doesn't load");
    }
    sigil_sync_result_free(restored);
    sigil_sync_result_free(r);
    root_free(&root);
    free(other);
    free(gci);
}

/* A save's identity leaves out its time and copy counter: written again it
 * isn't a change, and new data in it is. */
static void check_identity_masks(void) {
    size_t len = 0;
    uint8_t *gci = sample("nfsu2-gci", NULL, &len);
    if (!gci) return;
    mem_root root = {0};
    root_put(&root, NFSU2_FILE, gci, len);
    game g;
    make_game(&g, &root, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO);
    sigil_sync_result *first = NULL;
    if (sigil_collect(&g.req, &first) != SIGIL_OK || !first->data) { fail("identity masks", "setup failed"); goto done; }
    g.req.state = first->state;
    g.req.state_len = first->state_len;
    const uint8_t later[4] = { 0x7F, 0x00, 0x12, 0x34 }, copies = 0x42, flipped = (uint8_t)(gci[len - 1] ^ 0xFF);
    const struct { const char *what; size_t at; const void *bytes; size_t n; bool changed; } EDITS[] = {
        { "a new time", GCI_MTIME, later, 4, false },
        { "a new copy count", GCI_COPY, &copies, 1, false },
        { "new data", len - 1, &flipped, 1, true },
    };
    for (size_t i = 0; i < sizeof(EDITS) / sizeof(EDITS[0]); i++) {
        uint8_t *edited = gci_with(gci, len, EDITS[i].at, EDITS[i].bytes, EDITS[i].n);
        root_put(&root, NFSU2_FILE, edited, len);
        sigil_sync_result *r = NULL;
        if (sigil_collect(&g.req, &r) != SIGIL_OK || (bool)r->changed != EDITS[i].changed) fail("identity masks", EDITS[i].what);
        sigil_sync_result_free(r);
        free(edited);
    }
done:
    sigil_sync_result_free(first);
    root_free(&root);
    free(gci);
}

/* Two saves with one game code and file name but different maker codes are
 * two saves, as Dolphin tells them apart. */
static void check_maker_code(void) {
    size_t len = 0;
    uint8_t *gci = sample("nfsu2-gci", NULL, &len);
    if (!gci) return;
    uint8_t *other = gci_with(gci, len, GCI_MAKER, "70", 2);
    mem_root root = {0};
    root_put(&root, NFSU2_FILE, gci, len);
    root_put(&root, SA_FOLDER "70-GUGE-NFSU2.gci", other, len);
    sigil_sync_result *r = unit_from(&root, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO, false);
    if (!r || r->shape != SIGIL_SAVE_SHAPE_MULTI || zip_count(r->data, r->len) != 2) {
        fail("maker code", "saves differing only by maker code collapsed into one");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(other);
    free(gci);
}

/* A unit collected off a raw card restores into a GCI folder, and a
 * companion's saves go onto a raw card beside the game's and come back as
 * its own unit. */
static void check_raw_and_folder_cross(const sigil_sync_result *raw_unit, const sigil_sync_result *fzero,
                                       const sigil_sync_result *nfsu2) {
    if (!raw_unit || !fzero || !nfsu2) { fail("raw and folder", "setup failed"); return; }
    mem_root root = {0};
    game g;
    make_game(&g, &root, "dolphin", FZERO, "GFZE", FZERO_DISC);
    sigil_sync_result *r = NULL, *back = NULL;
    if (sigil_restore(&g.req, raw_unit->data, raw_unit->len, &r) != SIGIL_OK || root.count != 1 ||
        !root_find(&root, LIB_FOLDER "8P-GFZE-f_zero.dat.gci")) {
        fail("raw and folder", "a raw card's unit didn't land as Dolphin's file");
    }
    sigil_sync_result_free(r);
    r = NULL;
    root_free(&root);

    const char *ids[] = { FZERO };
    sigil_sync_companion companion = { ids, 1, fzero->data, fzero->len };
    make_game(&g, &root, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO);
    raw_mode(&g);
    g.req.companions = &companion;
    g.req.companion_count = 1;
    mem_file *f = NULL;
    if (sigil_restore(&g.req, nfsu2->data, nfsu2->len, &r) != SIGIL_OK || !(f = root_find(&root, "GC/MemoryCardA.USA.raw")) ||
        card_saves_of(f->data, f->len, NFSU2) != 1 || card_saves_of(f->data, f->len, FZERO) != 5) {
        fail("raw and folder", "the companion's saves didn't go onto the raw card beside the game's");
    } else {
        refresh(&g, &root);
        g.req.state = r->state;
        g.req.state_len = r->state_len;
        companion.unit = NULL;
        companion.unit_len = 0;
        if (sigil_collect(&g.req, &back) != SIGIL_OK || strcmp(back->identity_hash, nfsu2->identity_hash) != 0 ||
            back->companion_count != 1 || strcmp(back->companions[0].identity_hash, fzero->identity_hash) != 0) {
            fail("raw and folder", "the raw card's saves didn't split back into the game's and the companion's units");
        }
    }
    sigil_sync_result_free(back);
    sigil_sync_result_free(r);
    root_free(&root);
}

/* A save that reads back wrong fails the restore, on a raw card and in the
 * GCI folder. */
/* A listed .gci the client can't open is an I/O error: collect would drop
 * that save from the unit, and restore would take its name as free and
 * write over it. */
static void check_unopenable_gci(const sigil_sync_result *nfsu2) {
    if (!nfsu2) return;
    mem_root root = {0};
    if (!put_fzero(&root, SA_FOLDER, NULL)) { root_free(&root); return; }
    root_put(&root, NFSU2_FILE, nfsu2->data, nfsu2->len);
    snprintf(root.unreadable, sizeof(root.unreadable), "%s", NFSU2_FILE);
    game g;
    make_game(&g, &root, "dolphin_standalone", NFSU2, "GUGE", NFSU2_ISO);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_ERR_IO) fail("unopenable gci", "collect dropped an unopenable save");
    sigil_sync_result_free(r);
    g.req.overwrite_local = 1;
    r = NULL;
    if (sigil_restore(&g.req, nfsu2->data, nfsu2->len, &r) != SIGIL_ERR_IO) fail("unopenable gci", "restore went ahead");
    if (root.writes || root.removes) fail("unopenable gci", "restore wrote over a file it couldn't read");
    sigil_sync_result_free(r);

    snprintf(root.unreadable, sizeof(root.unreadable), "%s%s", SA_FOLDER, FZERO_FILES[0]);
    r = NULL;
    if (sigil_restore(&g.req, nfsu2->data, nfsu2->len, &r) != SIGIL_ERR_IO) {
        fail("unopenable gci", "restore placed a save beside another game's file it couldn't read");
    }
    if (root.writes || root.removes) fail("unopenable gci", "restore wrote");
    sigil_sync_result_free(r);
    root_free(&root);
}

static void check_faulty_writes(const sigil_sync_result *raw_unit) {
    if (!raw_unit) { fail("faulty writes", "setup failed"); return; }
    mem_root clean = {0}, faulty = {0};
    game g;
    make_game(&g, &clean, "dolphin", FZERO, "GFZE", FZERO_DISC);
    raw_mode(&g);
    sigil_sync_result *r = NULL;
    uint32_t first = 0xFFFF;
    if (sigil_restore(&g.req, raw_unit->data, raw_unit->len, &r) != SIGIL_OK ||
        (first = first_block_of(clean.files[0].data, clean.files[0].len, "f_zero.dat")) == 0xFFFF) {
        fail("faulty writes", "clean restore failed");
    } else {
        sigil_sync_result_free(r);
        r = NULL;
        faulty.corrupt_write = 1;
        faulty.corrupt_at = (size_t)first * GC_BLOCK_SIZE + 100u;
        make_game(&g, &faulty, "dolphin", FZERO, "GFZE", FZERO_DISC);
        raw_mode(&g);
        if (sigil_restore(&g.req, raw_unit->data, raw_unit->len, &r) != SIGIL_ERR_IO) fail("faulty writes", "a raw card that read back wrong passed");
    }
    sigil_sync_result_free(r);
    r = NULL;
    root_free(&faulty);
    root_free(&clean);

    faulty.corrupt_write = 1;
    make_game(&g, &faulty, "dolphin", FZERO, "GFZE", FZERO_DISC);
    if (sigil_restore(&g.req, raw_unit->data, raw_unit->len, &r) != SIGIL_ERR_IO) fail("faulty writes", "a .gci that read back wrong passed");
    sigil_sync_result_free(r);
    root_free(&faulty);
}

int main(void) {
    char path[1024];
    if (corpus_platform_path("ngc", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_manifest) != 0) {
        fprintf(stderr, "SKIP: no ngc manifest\n");
        return TEST_SKIP;
    }
    if (!corpus_present(&g_manifest, "ngc", "card-raw-usa")) {
        fprintf(stderr, "SKIP: ngc samples missing\n");
        corpus_free(&g_manifest);
        return TEST_SKIP;
    }

    sigil_sync_result *folder_unit = NULL;
    check_folder_collect(&folder_unit);
    check_folder_restore(folder_unit);
    check_folder_order(folder_unit);
    check_raw_card(folder_unit);
    check_regions_and_standalone();
    check_escaped_names();
    check_folder_companion(folder_unit);

    size_t card_len = 0;
    uint8_t *card = sample("card-raw-usa", NULL, &card_len);
    mem_root card_root = {0};
    if (card) root_put(&card_root, LIB_RAW, card, card_len);
    sigil_sync_result *raw_unit = unit_from(&card_root, "dolphin", FZERO, "GFZE", FZERO_DISC, true);
    sigil_sync_result *nfsu2 = nfsu2_unit();
    check_argosy_uploads(nfsu2);
    check_digit_suffix(nfsu2);
    check_companion_removal(nfsu2);
    check_raw_card_forms(raw_unit);
    check_save_bigger_than_card();
    check_directory_full(nfsu2);
    check_unopenable_gci(nfsu2);
    check_card_size_option(raw_unit, folder_unit);
    check_new_card_size(nfsu2, raw_unit);
    check_folder_capacity(nfsu2, folder_unit);
    check_dolphin_folder_rules(nfsu2);
    check_foreign_companions(folder_unit);
    check_folder_filters(folder_unit);
    check_identity_masks();
    check_maker_code();
    check_raw_and_folder_cross(raw_unit, folder_unit, nfsu2);
    check_faulty_writes(raw_unit);
    sigil_sync_result_free(nfsu2);
    sigil_sync_result_free(raw_unit);
    root_free(&card_root);
    free(card);
    sigil_sync_result_free(folder_unit);

    corpus_free(&g_manifest);
    printf("gamecube sync: %d failures\n", g_fails);
    return corpus_exit(g_fails);
}
