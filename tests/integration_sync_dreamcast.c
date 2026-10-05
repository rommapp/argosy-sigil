// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
#include "legacy_units.h"
#include "card_dreamcast.h"
#include <stdbool.h>

#define TEST_SKIP   77
#define GAME_ID     "T13301N"
#define CONTENT     "Gundam Side Story 0079 - Rise from the Ashes (USA).gdi"
#define STEM        "Gundam Side Story 0079 - Rise from the Ashes (USA)"
#define PER_GAME    "reicast_per_content_vmus"
#define GUNDAM_SAVE "GUNDAM_US_01"
#define DCI_TIME    0x10u
#define DCI_DATA    (VMU_DIR_ENTRY_SIZE + 64u)

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

static bool find_entry(const uint8_t *vmu, const char *name, uint32_t *first, uint32_t *blocks) {
    sigil_card_listing *l = NULL;
    bool found = false;
    if (sigil_dreamcast_card_list(vmu, &l) != SIGIL_OK) return false;
    for (size_t i = 0; i < l->entry_count && !found; i++) {
        if (strcmp(l->entries[i].name, name) != 0) continue;
        *first = l->entries[i].first_block;
        *blocks = l->entries[i].blocks;
        found = true;
    }
    sigil_card_listing_free(l);
    return found;
}

/* The .dci of the save `name` on `vmu`, or NULL. */
static uint8_t *save_of(const uint8_t *vmu, const char *name, size_t *len) {
    uint32_t first = 0, blocks = 0;
    if (!find_entry(vmu, name, &first, &blocks)) return NULL;
    *len = sigil_dreamcast_dci_size(blocks);
    uint8_t *dci = (uint8_t *)malloc(*len);
    if (sigil_dreamcast_extract(vmu, first, blocks, dci) != SIGIL_OK) { free(dci); return NULL; }
    return dci;
}

static bool same_save(const uint8_t *a, const uint8_t *b, const char *name) {
    size_t a_len = 0, b_len = 0;
    uint8_t *x = save_of(a, name, &a_len), *y = save_of(b, name, &b_len);
    bool same = x && y && a_len == b_len && memcmp(x, y, a_len) == 0;
    free(x);
    free(y);
    return same;
}

/* A copy of `vmu` with `dci` written on it, or NULL when it doesn't fit. */
static uint8_t *vmu_with(const uint8_t *vmu, const uint8_t *dci, size_t len) {
    uint8_t *out = (uint8_t *)malloc(VMU_CARD_SIZE);
    memcpy(out, vmu, VMU_CARD_SIZE);
    if (sigil_dreamcast_inject(out, dci, len) != SIGIL_OK) { free(out); return NULL; }
    return out;
}

/* `vmu` with the save `name` written again, byte `at` of its .dci flipped, as
 * a game saving over it would. */
static uint8_t *rewritten(const uint8_t *vmu, const char *name, size_t at) {
    size_t len = 0;
    uint32_t first = 0, blocks = 0;
    uint8_t *dci = find_entry(vmu, name, &first, &blocks) ? save_of(vmu, name, &len) : NULL;
    if (!dci || at >= len) { free(dci); return NULL; }
    uint8_t *gone = (uint8_t *)malloc(VMU_CARD_SIZE);
    memcpy(gone, vmu, VMU_CARD_SIZE);
    uint8_t *out = NULL;
    if (sigil_dreamcast_delete(gone, first) == SIGIL_OK) {
        dci[at] ^= 0x01;
        out = vmu_with(gone, dci, len);
    }
    free(gone);
    free(dci);
    return out;
}

static uint8_t *blank_vmu(void) {
    uint8_t *vmu = (uint8_t *)malloc(VMU_CARD_SIZE);
    sigil_dreamcast_format(vmu);
    return vmu;
}

static size_t zip_count(const uint8_t *zip, size_t len) {
    sigil_zip_member *m = NULL;
    size_t n = 0;
    if (sigil_zip_read_mem(zip, len, 1u << 26, &m, &n) != SIGIL_OK) return 0;
    sigil_zip_members_free(m, n);
    return n;
}

/* Whether the zip's member `member` is a VMU holding the save `name` and no
 * save in `absent`. */
static bool member_holds(const uint8_t *zip, size_t len, const char *member, const char *name, const char *absent) {
    sigil_zip_member *m = NULL;
    size_t n = 0;
    bool holds = false;
    if (sigil_zip_read_mem(zip, len, 1u << 26, &m, &n) != SIGIL_OK) return false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(m[i].name, member) != 0) continue;
        holds = m[i].len == VMU_CARD_SIZE && has_name(m[i].data, m[i].len, name) &&
                !has_name(m[i].data, m[i].len, absent);
    }
    sigil_zip_members_free(m, n);
    return holds;
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

/* Collects from `path` in `root`, restores the unit under the same settings
 * into a root holding only an empty VMU at `path` when it is a legacy name
 * (flycast creates the product-number name), else into an empty root, checks
 * it landed at `path`, and collects it back. */
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
        if (title_id[0] && strncmp(path, STEM, strlen(STEM)) == 0) {
            uint8_t *blank = blank_vmu();
            root_put(&empty, path, blank, VMU_CARD_SIZE);
            free(blank);
        }
        game fresh;
        make_game(&fresh, &empty, layout, title_id, SIGIL_SYNC_MANAGED);
        if (opt_key) add_option(&fresh, opt_key, opt_value);
        if (sigil_restore(&fresh.req, r->data, r->len, &restored) != SIGIL_OK || empty.count != 1) {
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

    root_put(&root, "A_B_C_D_E_F_G_H_I_J.A1.bin", vmu, len);
    round_trip("flycast game id", &root, "flycast", "A B/C\\D:E*F?G|H<I>J", PER_GAME, "VMU A1",
               "A_B_C_D_E_F_G_H_I_J.A1.bin", "vmu_A1.bin");
    root_free(&root);

    root_put(&root, STEM ".A1.bin", vmu, len);
    round_trip("flycast no game id", &root, "flycast", "", PER_GAME, "VMU A1", STEM ".A1.bin", "vmu_A1.bin");
    root_free(&root);

    root_put(&root, STEM ".A1.bin", vmu, len);
    round_trip("flycast legacy name", &root, "flycast", GAME_ID, PER_GAME, "VMU A1", STEM ".A1.bin", "vmu_A1.bin");
    root_free(&root);
    free(vmu);
}

/* "All VMUs" gives each port a per-game file, named by the product number or
 * by the content's legacy name; a game with saves on two travels as a zip
 * naming each port, and each port's VMU goes back to the file it came from. */
static void check_all_vmus_named(const char *where, const char *prefix) {
    size_t len = 0, other_len = 0;
    uint8_t *a1 = sample("gundam-0079-flycast", &len);
    uint8_t *b1 = sample("vmoooo-vmu", &other_len);
    uint8_t *blank = blank_vmu();
    if (!a1 || !b1) { free(a1); free(b1); free(blank); return; }
    char a1_path[SIGIL_SAVE_PATH_MAX], b1_path[SIGIL_SAVE_PATH_MAX];
    snprintf(a1_path, sizeof(a1_path), "%s.A1.bin", prefix);
    snprintf(b1_path, sizeof(b1_path), "%s.B1.bin", prefix);
    mem_root root = {0};
    root_put(&root, a1_path, a1, len);
    root_put(&root, b1_path, b1, other_len);
    game g;
    make_game(&g, &root, "flycast", GAME_ID, SIGIL_SYNC_MANAGED);
    add_option(&g, PER_GAME, "All VMUs");
    sigil_sync_result *r = NULL, *restored = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || r->shape != SIGIL_SAVE_SHAPE_MULTI || strcmp(r->artifact, STEM ".zip") != 0) {
        fail(where, "two ports didn't make a zip");
    } else if (zip_count(r->data, r->len) != 2 ||
               !member_holds(r->data, r->len, "vmu_A1.bin", GUNDAM_SAVE, "SONICADV__VM") ||
               !member_holds(r->data, r->len, "vmu_B1.bin", "SONICADV__VM", GUNDAM_SAVE)) {
        fail(where, "the zip's members don't name the port each VMU came from");
    } else {
        mem_root back = {0};
        root_put(&back, a1_path, blank, VMU_CARD_SIZE);
        root_put(&back, b1_path, blank, VMU_CARD_SIZE);
        game fresh;
        make_game(&fresh, &back, "flycast", GAME_ID, SIGIL_SYNC_MANAGED);
        add_option(&fresh, PER_GAME, "All VMUs");
        mem_file *fa = NULL, *fb = NULL;
        if (sigil_restore(&fresh.req, r->data, r->len, &restored) != SIGIL_OK || back.count != 2 ||
            !(fa = root_find(&back, a1_path)) || !(fb = root_find(&back, b1_path))) {
            fail(where, "restore didn't write both ports to the files there");
        } else if (!same_save(fa->data, a1, GUNDAM_SAVE) || entry_count(fa->data, fa->len) != 1 ||
                   !same_save(fb->data, b1, "SONICADV__VM") || entry_count(fb->data, fb->len) != 1) {
            fail(where, "a port's saves went to another port's file");
        }
        root_free(&back);
    }
    sigil_sync_result_free(restored);
    sigil_sync_result_free(r);
    root_free(&root);
    free(a1);
    free(b1);
    free(blank);
}

static void check_all_vmus(void) {
    check_all_vmus_named("flycast all VMUs", GAME_ID);
    check_all_vmus_named("flycast all VMUs legacy", STEM);
}

/* Restores `old` and sigil's own unit collected from `root` into empty
 * roots under the same option, and wants the same files from both. */
static void expect_same_restore(const char *where, mem_root *root, const char *opt_value, const uint8_t *old,
                                size_t old_len) {
    game g;
    make_game(&g, root, "flycast", GAME_ID, SIGIL_SYNC_MANAGED);
    add_option(&g, PER_GAME, opt_value);
    sigil_sync_result *unit = NULL, *ra = NULL, *rb = NULL;
    mem_root from_old = {0}, from_unit = {0};
    game a, b;
    make_game(&a, &from_old, "flycast", GAME_ID, SIGIL_SYNC_MANAGED);
    make_game(&b, &from_unit, "flycast", GAME_ID, SIGIL_SYNC_MANAGED);
    add_option(&a, PER_GAME, opt_value);
    add_option(&b, PER_GAME, opt_value);
    if (sigil_collect(&g.req, &unit) != SIGIL_OK || !unit->data) {
        fail(where, "collect failed");
    } else if (sigil_restore(&a.req, old, old_len, &ra) != SIGIL_OK || sigil_restore(&b.req, unit->data, unit->len, &rb) != SIGIL_OK) {
        fail(where, "restore failed");
    } else if (!roots_same(&from_old, &from_unit)) {
        fail(where, "the old upload restores other files than sigil's unit");
    }
    sigil_sync_result_free(ra);
    sigil_sync_result_free(rb);
    sigil_sync_result_free(unit);
    root_free(&from_old);
    root_free(&from_unit);
}

/* Argosy uploaded flycast's per-game A1 as the raw file, and RetroArch's VMUs
 * as a flat zip under their on-disk names. */
static void check_argosy_uploads(void) {
    size_t a1_len = 0, b1_len = 0, zip_len = 0;
    uint8_t *a1 = sample("gundam-0079-flycast", &a1_len);
    uint8_t *b1 = sample("vmoooo-vmu", &b1_len);
    if (!a1 || !b1) { free(a1); free(b1); return; }
    mem_root one = {0};
    root_put(&one, GAME_ID ".A1.bin", a1, a1_len);
    expect_same_restore("argosy dreamcast raw A1", &one, "VMU A1", a1, a1_len);
    root_free(&one);

    mem_root two = {0};
    root_put(&two, GAME_ID ".A1.bin", a1, a1_len);
    root_put(&two, GAME_ID ".B1.bin", b1, b1_len);
    const char *names[] = { GAME_ID ".A1.bin", GAME_ID ".B1.bin" };
    uint8_t *data[] = { a1, b1 };
    size_t lens[] = { a1_len, b1_len };
    uint8_t *zip = legacy_zip(names, data, lens, 2, &zip_len);
    if (zip) expect_same_restore("argosy dreamcast zip", &two, "All VMUs", zip, zip_len);
    free(zip);
    root_free(&two);
    free(a1);
    free(b1);
}

/* A unit with saves on B1, restored where flycast keeps only A1 per game,
 * has nowhere to put them: restore refuses naming the volume, writing
 * nothing, so the client can ask the user to change the setting. */
static void check_no_target(void) {
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
    sigil_sync_result *unit = NULL, *r = NULL;
    if (sigil_collect(&g.req, &unit) != SIGIL_OK || !unit->data) {
        fail("no target", "setup failed");
    } else {
        mem_root other = {0};
        game a1_only;
        make_game(&a1_only, &other, "flycast", GAME_ID, SIGIL_SYNC_MANAGED);
        add_option(&a1_only, PER_GAME, "VMU A1");
        if (sigil_restore(&a1_only.req, unit->data, unit->len, &r) != SIGIL_ERR_NO_TARGET || !r ||
            strcmp(r->problem, "vmu_B1.bin") != 0 || other.writes != 0) {
            fail("no target", "a unit with B1 saves restored under VMU A1 didn't refuse naming B1");
        }
        root_free(&other);
    }
    sigil_sync_result_free(r);
    sigil_sync_result_free(unit);
    root_free(&root);
    free(a1);
    free(b1);
}

/* Under "VMU A1" only A1 is per game; a per-game B1 file there isn't the
 * game's. */
static void check_a1_only(void) {
    size_t len = 0, other_len = 0;
    uint8_t *a1 = sample("gundam-0079-flycast", &len);
    uint8_t *b1 = sample("vmoooo-vmu", &other_len);
    if (!a1 || !b1) { free(a1); free(b1); return; }
    mem_root root = {0};
    root_put(&root, GAME_ID ".A1.bin", a1, len);
    root_put(&root, GAME_ID ".B1.bin", b1, other_len);
    game g;
    make_game(&g, &root, "flycast", GAME_ID, SIGIL_SYNC_MANAGED);
    add_option(&g, PER_GAME, "VMU A1");
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || r->shape == SIGIL_SAVE_SHAPE_MULTI || !r->data ||
        has_name(r->data, r->len, "SONICADV__VM")) {
        fail("flycast VMU A1", "a per-game B1 joined the unit");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(a1);
    free(b1);
}

/* With the product-number file and the legacy one both there, flycast reads
 * the product-number file, so sigil reads and writes that one alone. */
static void check_id_file_wins(void) {
    static const struct {
        const char *layout, *key, *value, *id_path, *stem_path;
    } CASES[] = {
        { "flycast", PER_GAME, "VMU A1", GAME_ID ".A1.bin", STEM ".A1.bin" },
        { "flycast", PER_GAME, "All VMUs", GAME_ID ".A1.bin", STEM ".A1.bin" },
        { "flycast_standalone", NULL, NULL, GAME_ID "_vmu_save_A1.bin", STEM "_vmu_save_A1.bin" },
    };
    size_t len = 0, nine_len = 0;
    uint8_t *gundam = sample("gundam-0079-flycast", &len);
    uint8_t *nine = sample("vmu-a1-nine-games", &nine_len);
    uint8_t *edited = gundam ? rewritten(gundam, GUNDAM_SAVE, DCI_DATA) : NULL;
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]) && edited && nine; i++) {
        mem_root root = {0};
        root_put(&root, CASES[i].id_path, gundam, len);
        root_put(&root, CASES[i].stem_path, nine, nine_len);
        game g;
        make_game(&g, &root, CASES[i].layout, GAME_ID, SIGIL_SYNC_MANAGED);
        if (CASES[i].key) add_option(&g, CASES[i].key, CASES[i].value);
        sigil_sync_result *r = NULL, *restored = NULL;
        mem_file *f = NULL;
        if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data || !has_name(r->data, r->len, GUNDAM_SAVE) ||
            has_name(r->data, r->len, "PJUSTICE_SYS")) {
            fail(CASES[i].id_path, "the legacy file was read over the product-number one");
        } else {
            g.req.state = r->state;
            g.req.state_len = r->state_len;
            if (sigil_restore(&g.req, edited, VMU_CARD_SIZE, &restored) != SIGIL_OK || root.writes != 1 ||
                !(f = root_find(&root, CASES[i].id_path)) || !same_save(f->data, edited, GUNDAM_SAVE) ||
                memcmp(root_find(&root, CASES[i].stem_path)->data, nine, nine_len) != 0) {
                fail(CASES[i].id_path, "restore didn't write the product-number file alone");
            }
        }
        sigil_sync_result_free(restored);
        sigil_sync_result_free(r);
        root_free(&root);
    }
    if (!edited || !nine) fail("flycast id file", "setup failed");
    free(edited);
    free(nine);
    free(gundam);
}

/* A save written again unchanged keeps the unit's identity; new data in it
 * doesn't. */
static void check_identity(void) {
    size_t len = 0;
    uint8_t *vmu = sample("gundam-0079-flycast", &len);
    if (!vmu) return;
    uint8_t *touched = rewritten(vmu, GUNDAM_SAVE, DCI_TIME);
    uint8_t *edited = rewritten(vmu, GUNDAM_SAVE, DCI_DATA);
    mem_root root = {0};
    sigil_sync_result *first = NULL, *same = NULL, *other = NULL;
    game g;
    make_game(&g, &root, "flycast", GAME_ID, SIGIL_SYNC_MANAGED);
    add_option(&g, PER_GAME, "VMU A1");
    root_put(&root, GAME_ID ".A1.bin", vmu, len);
    refresh(&g, &root);
    if (!touched || !edited || sigil_collect(&g.req, &first) != SIGIL_OK || !first->data) {
        fail("dreamcast identity", "setup failed");
    } else {
        g.req.state = first->state;
        g.req.state_len = first->state_len;
        root_put(&root, GAME_ID ".A1.bin", touched, VMU_CARD_SIZE);
        if (sigil_collect(&g.req, &same) != SIGIL_OK || same->changed ||
            strcmp(same->identity_hash, first->identity_hash) != 0) {
            fail("dreamcast identity", "a save written again with only a new time read as changed");
        }
        root_put(&root, GAME_ID ".A1.bin", edited, VMU_CARD_SIZE);
        if (sigil_collect(&g.req, &other) != SIGIL_OK || !other->changed ||
            strcmp(other->identity_hash, first->identity_hash) == 0) {
            fail("dreamcast identity", "a save with new data didn't read as changed");
        }
    }
    sigil_sync_result_free(other);
    sigil_sync_result_free(same);
    sigil_sync_result_free(first);
    root_free(&root);
    free(edited);
    free(touched);
    free(vmu);
}

/* A VMU that reads back with a byte of a save's data wrong fails the restore,
 * though its directory entry reads back right. */
static void check_faulty_write(void) {
    size_t len = 0;
    uint8_t *vmu = sample("gundam-0079-flycast", &len);
    if (!vmu) return;
    mem_root root = {0};
    root_put(&root, GAME_ID ".A1.bin", vmu, len);
    game g;
    make_game(&g, &root, "flycast", GAME_ID, SIGIL_SYNC_MANAGED);
    add_option(&g, PER_GAME, "VMU A1");
    sigil_sync_result *r = NULL, *restored = NULL;
    uint32_t first = 0, blocks = 0;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data || !find_entry(r->data, GUNDAM_SAVE, &first, &blocks)) {
        fail("dreamcast faulty write", "setup failed");
    } else {
        mem_root empty = {0};
        empty.corrupt_write = 1;
        empty.corrupt_at = (size_t)first * VMU_BLOCK_SIZE + 64u;
        game fresh;
        make_game(&fresh, &empty, "flycast", GAME_ID, SIGIL_SYNC_MANAGED);
        add_option(&fresh, PER_GAME, "VMU A1");
        if (sigil_restore(&fresh.req, r->data, r->len, &restored) != SIGIL_ERR_IO) {
            fail("dreamcast faulty write", "a VMU that read back wrong passed the restore");
        }
        root_free(&empty);
    }
    sigil_sync_result_free(restored);
    sigil_sync_result_free(r);
    root_free(&root);
    free(vmu);
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

/* A VMU formatted with more user blocks than a stock one keeps them across
 * a managed swap: the swapped VMU is laid out as the one it replaces. */
static void check_swap_keeps_layout(void) {
    size_t len = 0;
    uint8_t *vmu = sample("extended-blocks-vmu", &len);
    if (!vmu) return;
    sigil_card_listing *before = NULL, *after = NULL;
    mem_root root = {0};
    root_put(&root, "vmu_save_A1.bin", vmu, len);
    game g;
    make_game(&g, &root, "flycast", GAME_ID, SIGIL_SYNC_MANAGED);
    static const char *const CLAIM[] = { "VMUTOOL__OPT" };
    g.req.claimed = CLAIM;
    g.req.claimed_count = 1;
    sigil_sync_result *r = NULL, *restored = NULL;
    mem_file *f = NULL;
    uint8_t *edited = rewritten(vmu, "VMUTOOL__OPT", DCI_DATA);
    if (!edited || sigil_dreamcast_card_list(vmu, &before) != SIGIL_OK || sigil_collect(&g.req, &r) != SIGIL_OK || !r->data) {
        fail("swap layout", "setup failed");
    } else {
        g.req.claimed = NULL;
        g.req.claimed_count = 0;
        g.req.state = r->state;
        g.req.state_len = r->state_len;
        if (sigil_restore(&g.req, edited, VMU_CARD_SIZE, &restored) != SIGIL_OK || root.writes != 1 ||
            !(f = root_find(&root, "vmu_save_A1.bin")) ||
            sigil_dreamcast_card_list(f->data, &after) != SIGIL_OK || after->total_blocks != before->total_blocks ||
            after->entry_count != 1) {
            fail("swap layout", "the swapped VMU lost the user blocks the one it replaced had");
        }
    }
    sigil_card_listing_free(after);
    sigil_card_listing_free(before);
    sigil_sync_result_free(restored);
    sigil_sync_result_free(r);
    root_free(&root);
    free(edited);
    free(vmu);
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

    root_put(&root, STEM "_vmu_save_A1.bin", vmu, len);
    round_trip("flycast standalone legacy", &root, "flycast_standalone", GAME_ID, NULL, NULL, STEM "_vmu_save_A1.bin",
               "vmu_A1.bin");
    root_free(&root);
    free(vmu);

    vmu = sample("vmu-a1-nine-games", &len);
    if (!vmu) return;
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

/* Standalone flycast's per-game A1 beside the shared B1: the unit is A1, the
 * other games' saves on B1 come back in the holding unit under B1, and a
 * managed restore swaps B1 for an empty VMU. */
static void check_standalone_shared_b1(void) {
    size_t len = 0, nine_len = 0;
    uint8_t *vmu = sample("gundam-0079-flycast", &len);
    uint8_t *nine = sample("vmu-a1-nine-games", &nine_len);
    uint8_t *edited = vmu ? rewritten(vmu, GUNDAM_SAVE, DCI_DATA) : NULL;
    mem_root root = {0};
    sigil_sync_result *r = NULL, *restored = NULL;
    if (!edited || !nine) { fail("flycast standalone B1", "setup failed"); goto done; }
    root_put(&root, GAME_ID "_vmu_save_A1.bin", vmu, len);
    root_put(&root, "vmu_save_B1.bin", nine, nine_len);
    game g;
    make_game(&g, &root, "flycast_standalone", GAME_ID, SIGIL_SYNC_MANAGED);
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data || strcmp(r->artifact, "vmu_A1.bin") != 0 ||
        entry_count(r->data, r->len) != 1 || r->unowned_count != entry_count(nine, nine_len) ||
        zip_count(r->holding, r->holding_len) != 1 ||
        !member_holds(r->holding, r->holding_len, "vmu_B1.bin", "PJUSTICE_SYS", GUNDAM_SAVE)) {
        fail("flycast standalone B1", "the unit isn't A1 alone with B1's saves held back under B1");
        goto done;
    }
    g.req.state = r->state;
    g.req.state_len = r->state_len;
    mem_file *a1 = NULL, *b1 = NULL;
    if (sigil_restore(&g.req, edited, VMU_CARD_SIZE, &restored) != SIGIL_OK ||
        !(a1 = root_find(&root, GAME_ID "_vmu_save_A1.bin")) || !same_save(a1->data, edited, GUNDAM_SAVE) ||
        !(b1 = root_find(&root, "vmu_save_B1.bin")) || entry_count(b1->data, b1->len) != 0) {
        fail("flycast standalone B1", "restore didn't write A1 and swap B1 empty");
    }
done:
    sigil_sync_result_free(restored);
    sigil_sync_result_free(r);
    root_free(&root);
    free(edited);
    free(nine);
    free(vmu);
}

/* Unmanaged into the shared A1: the name table gives Gundam its save, and a
 * restore replaces that save and keeps every other game's. */
/* A data file of `blocks` zero blocks named `name`, laid out like `like`. */
static uint8_t *filler_dci(const uint8_t *like, uint32_t blocks, const char *name, size_t *len) {
    *len = sigil_dreamcast_dci_size(blocks);
    uint8_t *out = (uint8_t *)calloc(1, *len);
    if (!out) return NULL;
    memcpy(out, like, VMU_DIR_ENTRY_SIZE);
    memset(out + 0x04, ' ', 12);
    memcpy(out + 0x04, name, strlen(name));
    out[0x18] = (uint8_t)blocks;
    out[0x19] = (uint8_t)(blocks >> 8);
    return out;
}

/* A shared VMU two blocks short of Gundam's save refuses the restore with
 * that shortfall, in VMU blocks. */
static void check_no_space(void) {
    size_t len = 0, nine_len = 0, dci_len = 0, like_len = 0, fill_len = 0;
    uint8_t *vmu = sample("gundam-0079-flycast", &len);
    uint8_t *nine = sample("vmu-a1-nine-games", &nine_len);
    uint8_t *like = nine ? save_of(nine, "R2RUMBLE.001", &like_len) : NULL;
    uint32_t first = 0, blocks = 0;
    uint8_t *filled = NULL, *fill = NULL;
    sigil_card_listing *l = NULL;
    if (vmu && nine && like && find_entry(vmu, GUNDAM_SAVE, &first, &blocks) &&
        sigil_dreamcast_card_list(nine, &l) == SIGIL_OK && l->free_blocks > blocks) {
        fill = filler_dci(like, l->free_blocks - (blocks - 2), "FILLER", &fill_len);
        filled = fill ? vmu_with(nine, fill, fill_len) : NULL;
    }
    sigil_card_listing_free(l);
    mem_root root = {0};
    sigil_sync_result *seen = NULL, *r = NULL;
    if (!filled) { fail("dreamcast no space", "setup failed"); goto done; }
    root_put(&root, "vmu_save_A1.bin", filled, VMU_CARD_SIZE);
    game g;
    make_game(&g, &root, "flycast", GAME_ID, SIGIL_SYNC_UNMANAGED);
    if (sigil_collect(&g.req, &seen) != SIGIL_OK) { fail("dreamcast no space", "setup collect failed"); goto done; }
    g.req.state = seen->state;
    g.req.state_len = seen->state_len;
    if (sigil_restore(&g.req, vmu, VMU_CARD_SIZE, &r) != SIGIL_ERR_NO_SPACE || !r || root.writes != 0 ||
        strcmp(r->problem, GUNDAM_SAVE) != 0 || r->blocks_short != 2) {
        fail("dreamcast no space", "a save two blocks too big wasn't refused with that shortfall");
    }
done:
    sigil_sync_result_free(r);
    sigil_sync_result_free(seen);
    root_free(&root);
    free(filled);
    free(fill);
    free(like);
    free(nine);
    free(vmu);
}

static void check_unmanaged(void) {
    size_t len = 0, nine_len = 0, dci_len = 0;
    uint8_t *vmu = sample("gundam-0079-flycast", &len);
    uint8_t *nine = sample("vmu-a1-nine-games", &nine_len);
    uint8_t *dci = vmu ? save_of(vmu, GUNDAM_SAVE, &dci_len) : NULL;
    uint8_t *shared = nine && dci ? vmu_with(nine, dci, dci_len) : NULL;
    uint8_t *edited = vmu ? rewritten(vmu, GUNDAM_SAVE, DCI_DATA) : NULL;
    mem_root root = {0};
    sigil_sync_result *seen = NULL, *r = NULL;
    mem_file *f = NULL;
    if (!shared || !edited) { fail("dreamcast unmanaged", "setup failed"); goto done; }
    root_put(&root, "vmu_save_A1.bin", shared, VMU_CARD_SIZE);
    game g;
    make_game(&g, &root, "flycast", GAME_ID, SIGIL_SYNC_UNMANAGED);
    if (sigil_collect(&g.req, &seen) != SIGIL_OK || !seen->data || entry_count(seen->data, seen->len) != 1 ||
        !has_name(seen->data, seen->len, GUNDAM_SAVE)) {
        fail("dreamcast unmanaged", "collect didn't take Gundam's save alone");
        goto done;
    }
    g.req.state = seen->state;
    g.req.state_len = seen->state_len;
    if (sigil_restore(&g.req, edited, VMU_CARD_SIZE, &r) != SIGIL_OK || !(f = root_find(&root, "vmu_save_A1.bin"))) {
        fail("dreamcast unmanaged", "restore failed");
    } else if (entry_count(f->data, f->len) != entry_count(nine, nine_len) + 1 || !same_save(f->data, edited, GUNDAM_SAVE) ||
               !same_save(f->data, nine, "PJUSTICE_SYS") || !same_save(f->data, nine, "R2RUMBLE.001")) {
        fail("dreamcast unmanaged", "Gundam's save wasn't replaced, or another game's save changed");
    }
done:
    sigil_sync_result_free(r);
    sigil_sync_result_free(seen);
    root_free(&root);
    free(edited);
    free(shared);
    free(dci);
    free(nine);
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
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data || !has_name(r->data, r->len, GUNDAM_SAVE)) {
        fail("dreamcast name table", "Gundam's save on the shared VMU wasn't given to it");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(vmu);

    size_t nine_len = 0;
    uint8_t *nine = sample("vmu-a1-nine-games", &nine_len);
    if (!nine) return;
    root_put(&root, "vmu_save_A1.bin", nine, nine_len);
    make_game(&g, &root, "flycast", "T1219N", SIGIL_SYNC_MANAGED);
    r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data || entry_count(r->data, r->len) != 1 ||
        !has_name(r->data, r->len, "PJUSTICE_SYS") || r->unowned_count != entry_count(nine, nine_len) - 1) {
        fail("dreamcast name table", "Project Justice's save on the shared VMU wasn't given to it alone");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(nine);
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
    check_argosy_uploads();
    check_a1_only();
    check_no_target();
    check_id_file_wins();
    check_identity();
    check_faulty_write();
    check_shared_a1();
    check_standalone();
    check_standalone_shared_b1();
    check_unmanaged();
    check_swap_keeps_layout();
    check_name_table();
    check_no_space();

    corpus_free(&g_manifest);
    printf("dreamcast sync: %d failures\n", g_fails);
    return corpus_exit(g_fails);
}
