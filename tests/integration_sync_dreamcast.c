// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
#include "mem_root.h"
#include "card_dreamcast.h"
#include <stdbool.h>

#define TEST_SKIP   77
#define GAME_ID     "T-13301N"
#define CONTENT     "Gundam Side Story 0079 - Rise from the Ashes (USA).gdi"
#define STEM        "Gundam Side Story 0079 - Rise from the Ashes (USA)"
#define PER_GAME    "reicast_per_content_vmus"

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static corpus_table g_manifest;

static uint8_t *sample(const char *id, size_t *len) {
    return corpus_sample(&g_manifest, "dc", id, NULL, len);
}

static size_t entry_count(const uint8_t *vmu, size_t len) {
    sigil_io *io = mem_root_io(vmu, len);
    sigil_card_listing *l = NULL;
    size_t n = sigil_card_list(io, &l) == SIGIL_OK ? l->entry_count : 0;
    sigil_card_listing_free(l);
    sigil_io_close(io);
    return n;
}

static bool has_name(const uint8_t *vmu, size_t len, const char *name) {
    sigil_io *io = mem_root_io(vmu, len);
    sigil_card_listing *l = NULL;
    bool found = false;
    if (sigil_card_list(io, &l) == SIGIL_OK) {
        for (size_t i = 0; i < l->entry_count && !found; i++) found = strcmp(l->entries[i].name, name) == 0;
    }
    sigil_card_listing_free(l);
    sigil_io_close(io);
    return found;
}

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    sigil_save_option  options[2];
} game;

static void make_game(game *g, mem_root *root, const char *layout, const char *title_id, int mode) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V3;
    g->result.platform = SIGIL_PLATFORM_DREAMCAST;
    snprintf(g->result.title_id, sizeof(g->result.title_id), "%s", title_id);
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = "dc";
    g->req.save.content_path = CONTENT;
    g->req.save.result = &g->result;
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
    g->req.save.open = root_open;
    g->req.save.open_ctx = root;
    g->req.write = root_write;
    g->req.write_ctx = root;
    g->req.mode = mode;
}

static void add_option(game *g, const char *key, const char *value) {
    g->options[g->req.save.option_count].key = key;
    g->options[g->req.save.option_count].value = value;
    g->req.save.option_count++;
    g->req.save.options = g->options;
}

static void refresh(game *g, mem_root *root) {
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
}

/* Collects from `path` in `root`, restores the unit into an empty root
 * under the same settings, checks it landed at `path`, and collects it back. */
static void round_trip(const char *where, mem_root *root, const char *layout, const char *title_id, const char *opt_key,
                       const char *opt_value, const char *path, const char *want_artifact) {
    game g;
    make_game(&g, root, layout, title_id, SIGIL_SYNC_MANAGED);
    if (opt_key) add_option(&g, opt_key, opt_value);
    sigil_sync_result *r = NULL, *restored = NULL, *back = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data || r->holding) {
        fail(where, "collect from the game's own VMU failed");
    } else {
        if (strcmp(r->artifact, want_artifact) != 0) fail(where, "artifact name");
        mem_root empty = {0};
        game fresh;
        make_game(&fresh, &empty, layout, title_id, SIGIL_SYNC_MANAGED);
        if (opt_key) add_option(&fresh, opt_key, opt_value);
        if (sigil_restore(&fresh.req, r->data, r->len, &restored) != SIGIL_OK) {
            fail(where, "restore into an empty root failed");
        } else {
            mem_file *f = root_find(&empty, path);
            if (!f || f->len != VMU_CARD_SIZE) fail(where, "the VMU didn't go where flycast reads it");
            refresh(&fresh, &empty);
            fresh.req.state = restored->state;
            fresh.req.state_len = restored->state_len;
            if (sigil_collect(&fresh.req, &back) != SIGIL_OK || strcmp(back->identity_hash, r->identity_hash) != 0 || back->changed) {
                fail(where, "saves differ after the trip, or read as changed");
            }
        }
        root_free(&empty);
    }
    sigil_sync_result_free(back);
    sigil_sync_result_free(restored);
    sigil_sync_result_free(r);
}

/* flycast libretro with "VMU A1" keeps A1 per game, named by the disc's
 * product number with ` /\:*?|<>` made `_`. */
static void check_per_game_a1(void) {
    size_t len = 0;
    uint8_t *vmu = sample("gundam-0079-flycast", &len);
    if (!vmu) return;
    mem_root root = {0};
    root_put(&root, GAME_ID ".A1.bin", vmu, len);
    round_trip("flycast A1", &root, "flycast", GAME_ID, PER_GAME, "VMU A1", GAME_ID ".A1.bin", "vmu_A1.bin");
    root_free(&root);

    root_put(&root, "MK_51000__X.A1.bin", vmu, len);
    round_trip("flycast game id", &root, "flycast", "MK 51000 /X", PER_GAME, "VMU A1", "MK_51000__X.A1.bin", "vmu_A1.bin");
    root_free(&root);

    root_put(&root, STEM ".A1.bin", vmu, len);
    game g;
    make_game(&g, &root, "flycast", GAME_ID, SIGIL_SYNC_MANAGED);
    add_option(&g, PER_GAME, "VMU A1");
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data || entry_count(r->data, r->len) != entry_count(vmu, len)) {
        fail("flycast legacy name", "the VMU under the content's name wasn't read");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(vmu);
}

/* "All VMUs" gives each port a per-game file; a game with saves on two
 * travels as a zip naming each port. */
static void check_all_vmus(void) {
    size_t len = 0, other_len = 0;
    uint8_t *a1 = sample("gundam-0079-flycast", &len);
    uint8_t *b1 = sample("vmoooo-vmu", &other_len);
    if (!a1 || !b1) { free(a1); free(b1); return; }
    mem_root root = {0};
    root_put(&root, GAME_ID ".A1.bin", a1, len);
    root_put(&root, GAME_ID ".B1.bin", b1, other_len);
    game g;
    make_game(&g, &root, "flycast", GAME_ID, SIGIL_SYNC_MANAGED);
    add_option(&g, PER_GAME, "All VMUs");
    sigil_sync_result *r = NULL, *restored = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || r->shape != SIGIL_SAVE_SHAPE_MULTI || strcmp(r->artifact, STEM ".zip") != 0) {
        fail("flycast all VMUs", "two ports didn't make a zip");
    } else {
        mem_root empty = {0};
        game fresh;
        make_game(&fresh, &empty, "flycast", GAME_ID, SIGIL_SYNC_MANAGED);
        add_option(&fresh, PER_GAME, "All VMUs");
        if (sigil_restore(&fresh.req, r->data, r->len, &restored) != SIGIL_OK || !root_find(&empty, GAME_ID ".A1.bin") ||
            !root_find(&empty, GAME_ID ".B1.bin")) {
            fail("flycast all VMUs", "restore didn't write both ports");
        }
        root_free(&empty);
    }
    sigil_sync_result_free(restored);
    sigil_sync_result_free(r);
    root_free(&root);
    free(a1);
    free(b1);
}

/* flycast's default shares vmu_save_A1.bin across games: saves no one
 * claimed are held back, a claim makes the unit, and a swap leaves the
 * claimed save alone on the VMU. */
static void check_shared_a1(void) {
    size_t len = 0;
    uint8_t *shared = sample("vmu-a1-nine-games", &len);
    if (!shared) return;
    mem_root root = {0};
    root_put(&root, "vmu_save_A1.bin", shared, len);
    game g;
    make_game(&g, &root, "flycast", GAME_ID, SIGIL_SYNC_MANAGED);
    sigil_sync_result *held = NULL, *claimed = NULL, *r = NULL;
    if (sigil_collect(&g.req, &held) != SIGIL_OK || held->data || !held->holding ||
        held->unowned_count != entry_count(shared, len)) {
        fail("flycast shared", "the shared VMU wasn't held back");
    } else {
        char name[SIGIL_CARD_NAME_MAX];
        snprintf(name, sizeof(name), "%s", held->unowned[0]);
        const char *claim[] = { name };
        g.req.claimed = claim;
        g.req.claimed_count = 1;
        if (sigil_collect(&g.req, &claimed) != SIGIL_OK || !claimed->data) {
            fail("flycast shared", "a claimed save didn't make a unit");
        } else {
            g.req.claimed = NULL;
            g.req.claimed_count = 0;
            g.req.state = claimed->state;
            g.req.state_len = claimed->state_len;
            if (sigil_restore(&g.req, claimed->data, claimed->len, &r) != SIGIL_OK || root.writes != 1) {
                fail("flycast shared", "the swap failed");
            } else {
                mem_file *f = root_find(&root, "vmu_save_A1.bin");
                if (entry_count(f->data, f->len) != 1 || !has_name(f->data, f->len, name)) {
                    fail("flycast shared", "the swapped VMU doesn't hold the claimed save alone");
                }
            }
        }
    }
    sigil_sync_result_free(r);
    sigil_sync_result_free(claimed);
    sigil_sync_result_free(held);
    root_free(&root);
    free(shared);
}

/* Standalone flycast keeps A1 per game as {gameId}_vmu_save_A1.bin unless
 * PerGameVmu is off, when A1 is the shared vmu_save_A1.bin. */
static void check_standalone(void) {
    size_t len = 0;
    uint8_t *vmu = sample("gundam-0079-flycast", &len);
    if (!vmu) return;
    mem_root root = {0};
    root_put(&root, GAME_ID "_vmu_save_A1.bin", vmu, len);
    round_trip("flycast standalone", &root, "flycast_standalone", GAME_ID, NULL, NULL, GAME_ID "_vmu_save_A1.bin",
               "vmu_A1.bin");
    root_free(&root);

    root_put(&root, "vmu_save_A1.bin", vmu, len);
    game g;
    make_game(&g, &root, "flycast_standalone", GAME_ID, SIGIL_SYNC_MANAGED);
    add_option(&g, "PerGameVmu", "no");
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->holding) fail("flycast standalone shared", "the shared A1 wasn't read as shared");
    sigil_sync_result_free(r);
    root_free(&root);
    free(vmu);
}

/* The name table gives Gundam (T13301N) its save on a shared VMU. */
static void check_name_table(void) {
    size_t len = 0;
    uint8_t *vmu = sample("gundam-0079-flycast", &len);
    if (!vmu) return;
    mem_root root = {0};
    root_put(&root, "vmu_save_A1.bin", vmu, len);
    game g;
    make_game(&g, &root, "flycast", "T13301N", SIGIL_SYNC_MANAGED);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data || !has_name(r->data, r->len, "GUNDAM_US_01")) {
        fail("dreamcast name table", "Gundam's save on the shared VMU wasn't given to it");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(vmu);
}

int main(void) {
    char path[1024];
    if (corpus_platform_path("dc", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_manifest) != 0) {
        fprintf(stderr, "SKIP: no dc manifest\n");
        return TEST_SKIP;
    }
    if (!corpus_present(&g_manifest, "dc", "gundam-0079-flycast")) {
        fprintf(stderr, "SKIP: dc samples missing\n");
        corpus_free(&g_manifest);
        return TEST_SKIP;
    }

    check_per_game_a1();
    check_all_vmus();
    check_shared_a1();
    check_standalone();
    check_name_table();

    corpus_free(&g_manifest);
    printf("dreamcast sync: %d failures\n", g_fails);
    return corpus_exit(g_fails);
}
