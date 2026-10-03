// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
#include "mem_root.h"
#include "card_gamecube.h"
#include <stdbool.h>

#define TEST_SKIP  77
#define FZERO      "47465A45"
#define FZERO_DISC "F-Zero GX (USA).rvz"
#define LIB_FOLDER "User/GC/USA/Card A/"
#define LIB_RAW    "User/GC/MemoryCardA.USA.raw"

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

/* Dolphin escapes characters a file name can't hold as __xx__, and a double
 * underscore as __5f____5f__. */
static void check_escaped_names(void) {
    size_t len = 0;
    uint8_t *gci = sample("nfsu2-gci", NULL, &len);
    if (!gci) return;
    memcpy(gci + 0x08, "a/b__c?", 8);
    mem_root src = {0};
    root_put(&src, "GC/USA/Card A/x.gci", gci, len);
    game g;
    make_game(&g, &src, "dolphin_standalone", "47554745", "GUGE", "NFSU2 (USA).iso");
    sigil_sync_result *r = NULL, *restored = NULL;
    if (sigil_collect(&g.req, &r) == SIGIL_OK && r->data) {
        if (strcmp(r->artifact, "69-GUGE-a__2f__b__5f____5f__c__3f__.gci") != 0) fail("escaped names", "artifact isn't escaped as Dolphin escapes");
        mem_root empty = {0};
        game fresh;
        make_game(&fresh, &empty, "dolphin_standalone", "47554745", "GUGE", "NFSU2 (USA).iso");
        if (sigil_restore(&fresh.req, r->data, r->len, &restored) != SIGIL_OK ||
            !root_find(&empty, "GC/USA/Card A/69-GUGE-a__2f__b__5f____5f__c__3f__.gci")) {
            fail("escaped names", "the file isn't named as Dolphin names it");
        }
        root_free(&empty);
    } else {
        fail("escaped names", "collect failed");
    }
    sigil_sync_result_free(restored);
    sigil_sync_result_free(r);
    root_free(&src);
    free(gci);
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
        if (sigil_restore(&g.req, own->data, own->len, &r) != SIGIL_ERR_INVALID_ARG || other.writes != 0) {
            fail("folder companion", "a companion from another region went on the game's card");
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
    check_raw_card(folder_unit);
    check_regions_and_standalone();
    check_escaped_names();
    check_folder_companion(folder_unit);
    sigil_sync_result_free(folder_unit);

    corpus_free(&g_manifest);
    printf("gamecube sync: %d failures\n", g_fails);
    return corpus_exit(g_fails);
}
