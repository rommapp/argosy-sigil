// SPDX-License-Identifier: MPL-2.0
/* Xbox saves on xemu's drive image: a game's UDATA and TDATA folders are
 * collected off the FATX partition E: inside a qcow2 or raw image and
 * restored into another drive that holds other games' saves, changing
 * nothing else on it. Drives are built and read back by xbox_drive.h, which
 * shares no code with the library. */
#include "sync_internal.h"
#include "mem_root.h"
#include "xbox_drive.h"

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

#define CHECK(cond, where, what) do { if (!(cond)) fail(where, what); } while (0)

#define DRIVE "xbox_hdd.qcow2"
#define HALO  "4D530004"   /* the game under test; the console spells its folder 4d530004 */
#define OTHER "454D0009"
#define THIRD "53450007"

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    sigil_save_option  option;
} game;

static void make_game(game *g, mem_root *root, const char *layout, const char *save_id) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V4;
    g->result.platform = SIGIL_PLATFORM_XBOX;
    snprintf(g->result.title_id, sizeof(g->result.title_id), "MS-004");
    snprintf(g->result.save_id, sizeof(g->result.save_id), "%s", save_id);
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = "xbox";
    g->req.save.content_path = "Halo (USA).iso";
    g->req.save.result = &g->result;
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
    g->req.save.open = root_open;
    g->req.save.open_ctx = root;
    g->req.write = root_write;
    g->req.remove = root_remove;
    g->req.write_ctx = root;
    if (strcmp(layout, "xemu") == 0) {   /* xemu has no drive until its hdd_path setting names one */
        g->option.key = "hdd_path";
        g->option.value = DRIVE;
        g->req.save.options = &g->option;
        g->req.save.option_count = 1;
    }
}

/* ---- drives ------------------------------------------------------------------- */

static uint8_t *pattern(size_t len, unsigned seed) {
    uint8_t *data = (uint8_t *)malloc(len);
    for (size_t i = 0; i < len; i++) {
        seed = seed * 1103515245u + 12345u;
        data[i] = (uint8_t)(seed >> 16);
    }
    return data;
}

static void put_pattern(xb_guest *g, const char *path, size_t len, unsigned seed) {
    uint8_t *data = pattern(len, seed);
    xb_put(g, path, data, len);
    free(data);
}

/* A formatted drive with what a console leaves outside any game's saves:
 * its config sector, bytes on the cache and system partitions, and a file
 * on E: beside the save folders. No restore may change any of it. */
static void blank_drive(xb_guest *g) {
    xb_init(g);
    xb_format(g);
    xb_set(g, 0x600, "RFRB", 4);
    uint8_t *junk = pattern(70000, 99);
    xb_set(g, 0x80000 + 0x9000, junk, 70000);
    xb_set(g, 0x8CA80000ull + 0x150000, junk, 70000);
    xb_set(g, XB_E + XB_E_SIZE + 0x1000, junk, 3000);
    free(junk);
}

/* Halo's saves as the console writes them: a title folder with its two
 * metadata files, two save folders, one file of more than a cluster, and a
 * file under TDATA. */
static void put_halo(xb_guest *g, unsigned seed) {
    xb_put_text(g, "UDATA/4d530004/TitleMeta.xbx", "TitleName=Halo\r\n");
    put_pattern(g, "UDATA/4d530004/TitleImage.xbx", 10240, seed + 1);
    xb_put_text(g, "UDATA/4d530004/0A1B2C3D4E5F/SaveMeta.xbx", "Name=Checkpoint\r\n");
    put_pattern(g, "UDATA/4d530004/0A1B2C3D4E5F/SaveImage.xbx", 4096, seed + 2);
    put_pattern(g, "UDATA/4d530004/0A1B2C3D4E5F/blam.sav", 40000, seed + 3);
    xb_put_text(g, "UDATA/4d530004/FFEEDDCCBBAA/SaveMeta.xbx", "Name=Profile\r\n");
    put_pattern(g, "UDATA/4d530004/FFEEDDCCBBAA/profile.dat", 16384, seed + 4);
    put_pattern(g, "TDATA/4d530004/$c/content.bin", 20000, seed + 5);
}

static void put_others(xb_guest *g) {
    xb_put_text(g, "CACHE/LocalCache00.bin", "the console's own");
    xb_put_text(g, "UDATA/454d0009/TitleMeta.xbx", "TitleName=FlatOut\r\n");
    put_pattern(g, "UDATA/454d0009/1A3F1B2E4ADD/options.sav", 8192, 21);
    put_pattern(g, "UDATA/454d0009/491B2E414E22/player001.sav", 50000, 22);
    xb_put_text(g, "UDATA/53450007/80482ED3557F/SaveMeta.xbx", "Name=PDO\r\n");
    put_pattern(g, "UDATA/53450007/80482ED3557F/PDOSYSTEM.DAT", 16384, 23);
    put_pattern(g, "TDATA/53450007/scratch.bin", 300, 24);
}

static void put_drive(mem_root *root, const char *name, const xb_guest *g, unsigned bits, unsigned compress_every,
                      unsigned version) {
    uint8_t *img = NULL;
    size_t len = 0;
    xb_qcow2(g, bits, compress_every, version, &img, &len);
    root_put(root, name, img, len);
    free(img);
}

/* Reads the drive in `root` back; false, with a failure noted, when its tables don't hold. */
static bool load_drive(mem_root *root, const char *name, xb_guest *g, xb_tree *t, const char *where) {
    mem_file *f = root_find(root, name);
    if (!f) {
        fail(where, "the drive is gone");
        xb_init(g);
        memset(t, 0, sizeof(*t));
        return false;
    }
    xb_check check;
    xb_read_image(f->data, f->len, g, &check);
    xb_walk(g, t);
    CHECK(check.faults == 0, where, "the qcow2 reference counts or tables are wrong");
    CHECK(check.leaks == 0, where, "the qcow2 file counts clusters nothing refers to");
    CHECK(t->faults == 0, where, "E:'s FAT chains are wrong");
    return check.faults == 0 && t->faults == 0;
}

/* ---- what a restore may change -------------------------------------------------- */

static void only_game_changed(const xb_guest *before, const xb_guest *after, const char *id, const char *where) {
    if (!xb_only_game_changed(before, after, id)) fail(where, "the restore changed something that isn't the game's");
}

static void others_intact(const xb_guest *before, const xb_guest *after, const char *id, const char *where) {
    if (xb_others_changed(before, after, id)) fail(where, "another game's files aren't as they were");
}

/* ---- units -------------------------------------------------------------------- */

static void names_of(const uint8_t *unit, size_t len, char *out, size_t cap) {
    sigil_zip_member *m = NULL;
    size_t n = 0;
    out[0] = '\0';
    if (!unit || sigil_zip_read_mem(unit, len, 1u << 24, &m, &n) != SIGIL_OK) return;
    for (size_t i = 0; i < n; i++) {
        size_t used = strlen(out);
        snprintf(out + used, cap - used, "%s%s", i ? "," : "", m[i].name);
    }
    sigil_zip_members_free(m, n);
}

/* The unit's files are on the drive under E: with the same bytes. */
static void unit_on_drive(const uint8_t *unit, size_t len, const xb_guest *g, const xb_tree *t, const char *where) {
    sigil_zip_member *m = NULL;
    size_t n = 0;
    if (!unit || sigil_zip_read_mem(unit, len, 1u << 24, &m, &n) != SIGIL_OK) {
        fail(where, "the unit isn't a zip");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (!xb_file_is(g, t, m[i].name, m[i].data, m[i].len)) fail(where, m[i].name);
    }
    sigil_zip_members_free(m, n);
}

#define HALO_NAMES                                                                                           \
    "TDATA/4d530004/$c/content.bin,UDATA/4d530004/0A1B2C3D4E5F/SaveImage.xbx,"                               \
    "UDATA/4d530004/0A1B2C3D4E5F/SaveMeta.xbx,UDATA/4d530004/0A1B2C3D4E5F/blam.sav,"                         \
    "UDATA/4d530004/FFEEDDCCBBAA/SaveMeta.xbx,UDATA/4d530004/FFEEDDCCBBAA/profile.dat,"                      \
    "UDATA/4d530004/TitleImage.xbx,UDATA/4d530004/TitleMeta.xbx"

/* Collects Halo from a drive holding it beside other games. */
static sigil_sync_result *collect_halo(unsigned seed, unsigned bits, unsigned compress_every, const char *where) {
    xb_guest g;
    blank_drive(&g);
    put_others(&g);
    put_halo(&g, seed);
    mem_root root = {0};
    put_drive(&root, DRIVE, &g, bits, compress_every, 3);
    game game;
    make_game(&game, &root, "xemu", HALO);
    sigil_sync_result *r = NULL;
    int rc = sigil_collect(&game.req, &r);
    CHECK(rc == SIGIL_OK && r && r->data, where, "collect found no saves");
    CHECK(root.writes == 0, where, "collect wrote to the save root");
    root_free(&root);
    xb_free(&g);
    return r;
}

/* ---- collect ------------------------------------------------------------------ */

static void check_collect(void) {
    const char *where = "collect";
    sigil_sync_result *r = collect_halo(1, 16, 2, where);
    if (!r || !r->data) return;
    char names[1024];
    names_of(r->data, r->len, names, sizeof(names));
    CHECK(strcmp(names, HALO_NAMES) == 0, where, names);
    CHECK(r->shape == SIGIL_SAVE_SHAPE_FOLDER, where, "the unit isn't a folder unit");
    CHECK(strcmp(r->artifact, "4D530004.zip") == 0, where, r->artifact);
    CHECK(r->identity_hash[0] && strcmp(r->identity_hash, r->content_hash) == 0, where, "the hashes are missing");
    CHECK(r->changed == 1, where, "a first collect isn't a change");

    /* The same saves give the same unit whatever the image's cluster size,
     * and whether its clusters are compressed. */
    sigil_sync_result *small = collect_halo(1, 12, 0, where), *packed = collect_halo(1, 16, 1, where);
    CHECK(small && small->len == r->len && memcmp(small->data, r->data, r->len) == 0, where,
          "4 KiB clusters gave another unit");
    CHECK(packed && packed->len == r->len && memcmp(packed->data, r->data, r->len) == 0, where,
          "an all-compressed image gave another unit");
    sigil_sync_result *changed = collect_halo(2, 16, 2, where);
    CHECK(changed && strcmp(changed->identity_hash, r->identity_hash) != 0, where, "other saves hash the same");

    /* Each file carries the time the drive has for it: FATX counts years from 2000, zip from 1980. */
    sigil_zip_stamp *stamps = NULL;
    size_t n = 0;
    CHECK(sigil_zip_get_stamps(r->data, r->len, &stamps, &n) == SIGIL_OK && n == 8, where, "the unit has no times");
    for (size_t i = 0; stamps && i < n; i++) {
        if (stamps[i].time != XB_TIME(21, 27, 22) || stamps[i].date != (uint16_t)(XB_DATE(2026, 10, 15) + (20u << 9))) {
            fail(where, "a file's time isn't the drive's");
        }
    }
    free(stamps);
    sigil_sync_result_free(changed);
    sigil_sync_result_free(packed);
    sigil_sync_result_free(small);
    sigil_sync_result_free(r);
}

/* A drive without the game's folders, a root without a drive, and a folder
 * the console spelled in capitals. */
static void check_collect_edges(void) {
    const char *where = "collect edges";
    xb_guest g;
    blank_drive(&g);
    put_others(&g);
    mem_root root = {0};
    put_drive(&root, DRIVE, &g, 16, 0, 3);
    game game;
    make_game(&game, &root, "xemu", HALO);
    sigil_sync_result *r = NULL;
    CHECK(sigil_collect(&game.req, &r) == SIGIL_OK && r && !r->data && !r->identity_hash[0], where,
          "a game with no folder on the drive has saves");
    sigil_sync_result_free(r);

    mem_root empty = {0};
    make_game(&game, &empty, "xemu", HALO);
    r = NULL;
    CHECK(sigil_collect(&game.req, &r) == SIGIL_OK && r && !r->data, where, "a root with no drive has saves");
    sigil_sync_result_free(r);

    xb_put_text(&g, "udata/4D530004/Save/DATA.BIN", "capitals");
    put_drive(&root, DRIVE, &g, 16, 0, 3);
    make_game(&game, &root, "xemu", "4d530004");
    r = NULL;
    char names[256];
    CHECK(sigil_collect(&game.req, &r) == SIGIL_OK && r && r->data, where, "the folder in capitals wasn't found");
    names_of(r ? r->data : NULL, r ? r->len : 0, names, sizeof(names));
    CHECK(strcmp(names, "UDATA/4d530004/Save/DATA.BIN") == 0, where, names);
    sigil_sync_result_free(r);

    /* No game: the request has to say whose saves. */
    make_game(&game, &root, "xemu", "");
    r = NULL;
    CHECK(sigil_collect(&game.req, &r) == SIGIL_ERR_INVALID_ARG, where, "a request without a save id was taken");
    root_free(&empty);
    root_free(&root);
    xb_free(&g);
}

/* ---- restore ------------------------------------------------------------------ */

/* A second console's drive: other games' saves, and an older Halo save. */
static void second_drive(xb_guest *g) {
    blank_drive(g);
    xb_put_text(g, "UDATA/4d530004/TitleMeta.xbx", "TitleName=Halo\r\n");
    put_pattern(g, "UDATA/4d530004/111111111111/old.sav", 70000, 77);
    put_others(g);
    put_pattern(g, "TDATA/4d530004/stale.bin", 5000, 78);
    put_pattern(g, "UDATA/41560002/AAAAAAAAAAAA/later.sav", 33000, 79);
}

static void check_restore(unsigned bits, unsigned compress_every, unsigned version, const char *where) {
    sigil_sync_result *unit = collect_halo(1, 16, 2, where);
    if (!unit || !unit->data) return;
    xb_guest before;
    second_drive(&before);
    mem_root root = {0};
    put_drive(&root, DRIVE, &before, bits, compress_every, version);
    game game;
    make_game(&game, &root, "xemu", HALO);
    game.req.overwrite_local = 1;
    sigil_sync_result *r = NULL;
    int rc = sigil_restore(&game.req, unit->data, unit->len, &r);
    CHECK(rc == SIGIL_OK && r, where, "restore failed");
    CHECK(root.writes == 1 && strcmp(root.first_write, DRIVE) == 0 && root.removes == 0, where,
          "restore didn't write the drive, once");
    CHECK(r && strcmp(r->identity_hash, unit->identity_hash) == 0 && strcmp(r->content_hash, unit->content_hash) == 0,
          where, "the restored hashes aren't the unit's");
    CHECK(r && r->changed == 0 && strcmp(r->artifact, "4D530004.zip") == 0, where, "the result isn't the unit's");

    xb_guest after;
    xb_tree tree;
    if (load_drive(&root, DRIVE, &after, &tree, where)) {
        unit_on_drive(unit->data, unit->len, &after, &tree, where);
        CHECK(!xb_node_at(&tree, "UDATA/4d530004/111111111111/old.sav") && !xb_node_at(&tree, "TDATA/4d530004/stale.bin"),
              where, "the game's old files are still there");
        others_intact(&before, &after, "4d530004", where);
        only_game_changed(&before, &after, "4d530004", where);
    }
    xb_tree_free(&tree);

    /* The restored drive gives the unit back, byte for byte, times included. */
    sigil_sync_result *again = NULL;
    make_game(&game, &root, "xemu", HALO);
    game.req.state = r ? r->state : NULL;
    game.req.state_len = r ? r->state_len : 0;
    CHECK(sigil_collect(&game.req, &again) == SIGIL_OK && again && again->len == unit->len &&
              memcmp(again->data, unit->data, unit->len) == 0,
          where, "collecting the restored drive gave another unit");
    CHECK(again && again->changed == 0, where, "the restored saves read as changed");

    /* Restoring what is already there writes nothing. */
    sigil_sync_result *twice = NULL;
    int writes = root.writes;
    CHECK(sigil_restore(&game.req, unit->data, unit->len, &twice) == SIGIL_OK && root.writes == writes, where,
          "restoring the same saves wrote the drive again");
    sigil_sync_result_free(twice);
    sigil_sync_result_free(again);
    sigil_sync_result_free(r);
    sigil_sync_result_free(unit);
    root_free(&root);
    xb_free(&after);
    xb_free(&before);
}

/* The image xemu publishes: every cluster compressed, E: formatted and
 * empty, no UDATA or TDATA folder yet. */
static void check_restore_blank(void) {
    const char *where = "restore into a blank drive";
    sigil_sync_result *unit = collect_halo(1, 16, 2, where);
    if (!unit || !unit->data) return;
    xb_guest before;
    blank_drive(&before);
    mem_root root = {0};
    put_drive(&root, DRIVE, &before, 16, 1, 3);
    game game;
    make_game(&game, &root, "xemu", HALO);
    sigil_sync_result *r = NULL;
    CHECK(sigil_restore(&game.req, unit->data, unit->len, &r) == SIGIL_OK, where, "restore failed");
    xb_guest after;
    xb_tree tree;
    if (load_drive(&root, DRIVE, &after, &tree, where)) {
        unit_on_drive(unit->data, unit->len, &after, &tree, where);
        only_game_changed(&before, &after, "4d530004", where);
        const xb_node *udata = xb_node_at(&tree, "UDATA"), *save = xb_node_at(&tree, "UDATA/4d530004/0A1B2C3D4E5F");
        CHECK(udata && (udata->attr & XB_DIR) && xb_node_at(&tree, "TDATA"), where, "UDATA and TDATA weren't made");
        CHECK(save && save->time == XB_TIME(21, 27, 22) && save->date == XB_DATE(2026, 10, 15), where,
              "a save folder doesn't carry its files' time");
    }
    xb_tree_free(&tree);
    sigil_sync_result_free(r);
    sigil_sync_result_free(unit);
    root_free(&root);
    xb_free(&after);
    xb_free(&before);
}

/* Both ways: console one's save goes to console two, the game saves again
 * there, and that save comes back to console one, each drive keeping its
 * other games. */
static void check_both_ways(void) {
    const char *where = "both ways";
    xb_guest one, two;
    blank_drive(&one);
    put_others(&one);
    put_halo(&one, 1);
    second_drive(&two);
    mem_root root_one = {0}, root_two = {0};
    put_drive(&root_one, DRIVE, &one, 16, 3, 3);
    put_drive(&root_two, DRIVE, &two, 12, 0, 3);

    game g1, g2;
    make_game(&g1, &root_one, "xemu", HALO);
    sigil_sync_result *c1 = NULL;
    CHECK(sigil_collect(&g1.req, &c1) == SIGIL_OK && c1 && c1->data, where, "collect on console one failed");
    make_game(&g2, &root_two, "xemu", HALO);
    g2.req.overwrite_local = 1;
    sigil_sync_result *r2 = NULL;
    CHECK(c1 && sigil_restore(&g2.req, c1->data, c1->len, &r2) == SIGIL_OK, where, "restore on console two failed");

    /* The game runs on console two and saves: one file rewritten, one added. */
    xb_guest played;
    xb_tree tree;
    if (!load_drive(&root_two, DRIVE, &played, &tree, where)) return;
    const xb_node *profile = xb_node_at(&tree, "UDATA/4d530004/FFEEDDCCBBAA/profile.dat");
    CHECK(profile != NULL, where, "the save isn't on console two");
    if (profile) {
        uint8_t *fresh = pattern(16384, 500);
        xb_set(&played, xb_cluster_off(profile->first), fresh, 16384);
        free(fresh);
    }
    xb_put_at(&played, "UDATA/4d530004/999999999999/second.sav", "made on console two", 19, XB_TIME(9, 5, 0),
              XB_DATE(2026, 11, 17));
    xb_tree_free(&tree);
    put_drive(&root_two, DRIVE, &played, 16, 0, 2);

    make_game(&g2, &root_two, "xemu", HALO);
    g2.req.state = r2 ? r2->state : NULL;
    g2.req.state_len = r2 ? r2->state_len : 0;
    sigil_sync_result *c2 = NULL;
    CHECK(sigil_collect(&g2.req, &c2) == SIGIL_OK && c2 && c2->data && c2->changed == 1, where,
          "the new save on console two wasn't collected as a change");

    /* Back on console one, whose saves are as its last collect left them. */
    make_game(&g1, &root_one, "xemu", HALO);
    g1.req.state = c1 ? c1->state : NULL;
    g1.req.state_len = c1 ? c1->state_len : 0;
    sigil_sync_result *r1 = NULL;
    CHECK(c2 && sigil_restore(&g1.req, c2->data, c2->len, &r1) == SIGIL_OK, where, "restore back on console one failed");
    xb_guest back;
    if (load_drive(&root_one, DRIVE, &back, &tree, where)) {
        CHECK(xb_file_is(&back, &tree, "UDATA/4d530004/999999999999/second.sav", "made on console two", 19), where,
              "console two's new save didn't come back");
        const xb_node *second = xb_node_at(&tree, "UDATA/4d530004/999999999999/second.sav");
        CHECK(second && second->time == XB_TIME(9, 5, 0) && second->date == XB_DATE(2026, 11, 17), where,
              "the new save lost its time");
        if (c2) unit_on_drive(c2->data, c2->len, &back, &tree, where);
        others_intact(&one, &back, "4d530004", where);
        only_game_changed(&one, &back, "4d530004", where);
    }
    xb_tree_free(&tree);
    sigil_sync_result_free(r1);
    sigil_sync_result_free(c2);
    sigil_sync_result_free(r2);
    sigil_sync_result_free(c1);
    root_free(&root_one);
    root_free(&root_two);
    xb_free(&back);
    xb_free(&played);
    xb_free(&one);
    xb_free(&two);
}

static void member(sigil_zip_member *m, const char *name, const char *text) {
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->data = (uint8_t *)text;
    m->len = strlen(text);
}

/* Restores a unit of `n` members into the second drive; returns sigil's code and leaves the result in `*out`. */
static int restore_members(const sigil_zip_member *members, size_t n, mem_root *root, sigil_sync_result **out) {
    xb_guest g;
    second_drive(&g);
    put_drive(root, DRIVE, &g, 16, 2, 3);
    xb_free(&g);
    uint8_t *unit = NULL;
    size_t len = 0;
    sigil_zip_store(members, n, &unit, &len);
    game game;
    make_game(&game, root, "xemu", HALO);
    game.req.overwrite_local = 1;
    int rc = sigil_restore(&game.req, unit, len, out);
    free(unit);
    return rc;
}

/* A zip as a client made it before it used sigil: the console's two folders
 * as they sit on E:, in whatever case, another game's among them. */
static void check_units_from_before(void) {
    const char *where = "units from before sigil";
    sigil_zip_member members[4];
    member(&members[0], "udata/4D530004/ABCDEF012345/save.dat", "progress");
    member(&members[1], "UDATA\\4d530004\\TitleMeta.xbx", "TitleName=Halo\r\n");
    member(&members[2], "UDATA/454d0009/1A3F1B2E4ADD/options.sav", "another game's, left alone");
    member(&members[3], "readme.txt", "not a save");
    mem_root root = {0};
    sigil_sync_result *r = NULL;
    CHECK(restore_members(members, 4, &root, &r) == SIGIL_OK, where, "restore failed");
    xb_guest after, before;
    xb_tree tree;
    second_drive(&before);
    if (load_drive(&root, DRIVE, &after, &tree, where)) {
        CHECK(xb_file_is(&after, &tree, "UDATA/4d530004/ABCDEF012345/save.dat", "progress", 8), where,
              "the save isn't where the console keeps it");
        CHECK(xb_file_is(&after, &tree, "UDATA/4d530004/TitleMeta.xbx", "TitleName=Halo\r\n", 16), where,
              "a member with backslashes wasn't placed");
        CHECK(!xb_node_at(&tree, "readme.txt") && !xb_node_at(&tree, "TDATA/4d530004"), where,
              "something that isn't the game's save was written");
        others_intact(&before, &after, "4d530004", where);
        only_game_changed(&before, &after, "4d530004", where);
        /* A unit made without times dates its files at the drive's earliest. */
        const xb_node *save = xb_node_at(&tree, "UDATA/4d530004/ABCDEF012345/save.dat");
        CHECK(save && save->time == 0 && save->date == XB_DATE(2000, 1, 1), where, "a file without a time got one");
    }
    xb_tree_free(&tree);
    sigil_sync_result_free(r);
    root_free(&root);
    xb_free(&after);
    xb_free(&before);
}

/* ---- refusals ----------------------------------------------------------------- */

static void check_refusals(void) {
    const char *where = "refusals";
    sigil_zip_member m[3];
    mem_root root = {0};
    sigil_sync_result *r = NULL;

    member(&m[0], "UDATA/454d0009/1A3F1B2E4ADD/options.sav", "another game's");
    CHECK(restore_members(m, 1, &root, &r) == SIGIL_ERR_NOT_FOUND && root.writes == 0, where,
          "a unit without the game's saves was taken");
    sigil_sync_result_free(r);
    r = NULL;
    root_free(&root);

    member(&m[0], "UDATA/4d530004/a-name-of-forty-three-characters-is-too-long", "x");
    CHECK(restore_members(m, 1, &root, &r) == SIGIL_ERR_NO_TARGET && r && strcmp(r->problem, m[0].name) == 0 &&
              root.writes == 0,
          where, "a name FATX can't hold was taken");
    sigil_sync_result_free(r);
    r = NULL;
    root_free(&root);

    member(&m[0], "UDATA/4d530004/a:b", "x");
    CHECK(restore_members(m, 1, &root, &r) == SIGIL_ERR_NO_TARGET && root.writes == 0, where,
          "a name with a character FATX refuses was taken");
    sigil_sync_result_free(r);
    r = NULL;
    root_free(&root);

    member(&m[0], "UDATA/4d530004/Save.dat", "one");
    member(&m[1], "UDATA/4d530004/SAVE.DAT", "two");
    CHECK(restore_members(m, 2, &root, &r) == SIGIL_ERR_UNSUPPORTED_FORMAT && root.writes == 0, where,
          "two members for one file were taken");
    root_free(&root);

    member(&m[0], "UDATA/4d530004/save", "a file");
    member(&m[1], "UDATA/4d530004/save/inside.dat", "in a folder of the same name");
    CHECK(restore_members(m, 2, &root, &r) == SIGIL_ERR_UNSUPPORTED_FORMAT && root.writes == 0, where,
          "a file and a folder of one name were taken");
    root_free(&root);

    member(&m[0], "UDATA/4d530004/Save/a.dat", "one");
    member(&m[1], "UDATA/4d530004/SAVE/b.dat", "two");
    CHECK(restore_members(m, 2, &root, &r) == SIGIL_ERR_UNSUPPORTED_FORMAT && root.writes == 0, where,
          "a folder spelled two ways was taken");
    root_free(&root);

    /* Not a zip. */
    xb_guest g;
    second_drive(&g);
    put_drive(&root, DRIVE, &g, 16, 2, 3);
    game game;
    make_game(&game, &root, "xemu", HALO);
    game.req.overwrite_local = 1;
    CHECK(sigil_restore(&game.req, (const uint8_t *)"not a zip", 9, &r) == SIGIL_ERR_UNSUPPORTED_FORMAT && root.writes == 0,
          where, "a unit that isn't a zip was taken");
    root_free(&root);

    /* No drive: sigil doesn't make one. */
    sigil_sync_result *unit = collect_halo(1, 16, 2, where);
    mem_root empty = {0};
    make_game(&game, &empty, "xemu", HALO);
    r = NULL;
    CHECK(unit && sigil_restore(&game.req, unit->data, unit->len, &r) == SIGIL_ERR_NO_TARGET && r &&
              strcmp(r->problem, DRIVE) == 0 && empty.writes == 0,
          where, "a restore without a drive didn't name the drive it needs");
    sigil_sync_result_free(r);
    r = NULL;

    /* A full E:. Two clusters are free, the unit needs more. */
    xb_fill(&g, 2);
    put_drive(&root, DRIVE, &g, 16, 2, 3);
    mem_file *f = root_find(&root, DRIVE);
    size_t len = f->len;
    uint8_t *copy = (uint8_t *)malloc(len);
    memcpy(copy, f->data, len);
    sigil_zip_member big;
    uint8_t *data = pattern(200000, 5);
    memset(&big, 0, sizeof(big));
    snprintf(big.name, sizeof(big.name), "UDATA/4d530004/ABCDEF012345/big.sav");
    big.data = data;
    big.len = 200000;
    uint8_t *zip = NULL;
    size_t zip_len = 0;
    sigil_zip_store(&big, 1, &zip, &zip_len);
    make_game(&game, &root, "xemu", HALO);
    game.req.overwrite_local = 1;
    int rc = sigil_restore(&game.req, zip, zip_len, &r);
    CHECK(rc == SIGIL_ERR_NO_SPACE && r && strcmp(r->problem, big.name) == 0 && r->blocks_short > 0 && root.writes == 0,
          where, "a save that doesn't fit wasn't refused with its name");
    f = root_find(&root, DRIVE);
    CHECK(f && f->len == len && memcmp(f->data, copy, len) == 0, where, "a refused restore changed the drive");
    sigil_sync_result_free(r);
    r = NULL;
    free(zip);
    free(data);
    free(copy);

    /* Companions go on cards and volumes. */
    static const char *const ids[] = { "MS-005" };
    sigil_sync_companion companion = { ids, 1, NULL, 0 };
    make_game(&game, &root, "xemu", HALO);
    game.req.companions = &companion;
    game.req.companion_count = 1;
    CHECK(sigil_collect(&game.req, &r) == SIGIL_ERR_INVALID_ARG, where, "a companion was taken");

    sigil_sync_result_free(unit);
    root_free(&empty);
    root_free(&root);
    xb_free(&g);
}

/* Local saves that changed since the last sync stop a restore unless the user chose to overwrite them. */
static void check_conflict(void) {
    const char *where = "conflict";
    sigil_sync_result *unit = collect_halo(1, 16, 2, where);
    xb_guest g;
    second_drive(&g);
    mem_root root = {0};
    put_drive(&root, DRIVE, &g, 16, 2, 3);
    game game;
    make_game(&game, &root, "xemu", HALO);
    sigil_sync_result *r = NULL;
    int rc = unit ? sigil_restore(&game.req, unit->data, unit->len, &r) : SIGIL_OK;
    CHECK(rc == SIGIL_ERR_CONFLICT && r && r->conflict == 1 && root.writes == 0, where,
          "saves no sync has seen were overwritten");
    sigil_sync_result_free(r);
    r = NULL;
    game.req.overwrite_local = 1;
    CHECK(unit && sigil_restore(&game.req, unit->data, unit->len, &r) == SIGIL_OK && root.writes == 1, where,
          "the user's choice to overwrite wasn't followed");

    /* With the state of that restore, a newer unit goes in without asking. */
    sigil_sync_result *newer = collect_halo(2, 16, 2, where);
    make_game(&game, &root, "xemu", HALO);
    game.req.state = r ? r->state : NULL;
    game.req.state_len = r ? r->state_len : 0;
    sigil_sync_result *r2 = NULL;
    CHECK(newer && sigil_restore(&game.req, newer->data, newer->len, &r2) == SIGIL_OK && root.writes == 2, where,
          "saves unchanged since the last sync stopped a restore");
    sigil_sync_result_free(r2);
    sigil_sync_result_free(newer);
    sigil_sync_result_free(r);
    sigil_sync_result_free(unit);
    root_free(&root);
    xb_free(&g);
}

/* ---- images sigil refuses or can't read ------------------------------------------ */

static void patch32(mem_root *root, size_t at, uint32_t v) {
    xb_put_be32(root_find(root, DRIVE)->data + at, v);
}

static void check_image(const char *what, int want, void (*patch)(mem_root *), bool names_drive) {
    xb_guest g;
    blank_drive(&g);
    put_others(&g);
    put_halo(&g, 1);
    mem_root root = {0};
    put_drive(&root, DRIVE, &g, 16, 2, 3);
    patch(&root);
    game game;
    make_game(&game, &root, "xemu", HALO);
    game.req.overwrite_local = 1;
    sigil_sync_result *r = NULL;
    int rc = sigil_collect(&game.req, &r);
    CHECK(rc == want, what, "collect took the image");
    CHECK(!names_drive || (r && strcmp(r->problem, DRIVE) == 0), what, "collect didn't name the drive");
    sigil_sync_result_free(r);
    r = NULL;
    sigil_sync_result *unit = collect_halo(2, 16, 2, what);
    rc = unit ? sigil_restore(&game.req, unit->data, unit->len, &r) : want;
    CHECK(rc == want && root.writes == 0, what, "restore took the image");
    sigil_sync_result_free(r);
    sigil_sync_result_free(unit);
    root_free(&root);
    xb_free(&g);
}

static void with_backing_file(mem_root *root) { patch32(root, 12, 0x200); }
static void encrypted(mem_root *root) { patch32(root, 32, 1); }
static void with_snapshot(mem_root *root) { patch32(root, 60, 1); }
static void left_dirty(mem_root *root) { patch32(root, 76, 1); }
static void marked_corrupt(mem_root *root) { patch32(root, 76, 2); }
static void external_data(mem_root *root) { patch32(root, 76, 4); }
static void extended_l2(mem_root *root) { patch32(root, 76, 16); }
static void wide_refcounts(mem_root *root) { patch32(root, 96, 6); }
static void version_4(mem_root *root) { patch32(root, 4, 4); }
static void short_header(mem_root *root) { patch32(root, 100, 72); }
static void dirty_bitmaps(mem_root *root) { patch32(root, 92, 1); }
static void reserved_autoclear(mem_root *root) { patch32(root, 92, 4); }
static void zstd(mem_root *root) {
    patch32(root, 76, 8);
    root_find(root, DRIVE)->data[104] = 1;
}
static void no_e(mem_root *root) {
    /* Not a console's drive: E: carries no FATX superblock. The image is rebuilt without one. */
    xb_guest g;
    xb_check check;
    mem_file *f = root_find(root, DRIVE);
    xb_read_image(f->data, f->len, &g, &check);
    xb_set(&g, XB_E, "NOPE", 4);
    put_drive(root, DRIVE, &g, 16, 2, 3);
    xb_free(&g);
}
static void cut_short(mem_root *root) {
    mem_file *f = root_find(root, DRIVE);
    f->len = 200000;
}
static void broken_chain(mem_root *root) {
    /* The second cluster of a save's file reads as free: the save can't be read whole. */
    xb_guest g;
    xb_check check;
    xb_tree tree;
    mem_file *f = root_find(root, DRIVE);
    xb_read_image(f->data, f->len, &g, &check);
    xb_walk(&g, &tree);
    const xb_node *n = xb_node_at(&tree, "UDATA/4d530004/0A1B2C3D4E5F/blam.sav");
    xb_set_fat(&g, xb_fat(&g, n->first), 0);
    xb_tree_free(&tree);
    put_drive(root, DRIVE, &g, 16, 2, 3);
    xb_free(&g);
}
static void chain_into_bad(mem_root *root) {
    /* A save folder that fills its one cluster, so nothing in the cluster ends it, and whose FAT
     * entry holds the bad-cluster marker where the end of its chain belongs. */
    xb_guest g;
    xb_check check;
    xb_tree tree;
    mem_file *f = root_find(root, DRIVE);
    xb_read_image(f->data, f->len, &g, &check);
    for (unsigned i = 0; i < XB_CLUSTER / XB_SLOT; i++) {
        char path[64];
        snprintf(path, sizeof(path), "UDATA/4d530004/FULL/f%03u", i);
        xb_put_text(&g, path, "x");
    }
    xb_walk(&g, &tree);
    const xb_node *n = xb_node_at(&tree, "UDATA/4d530004/FULL");
    xb_set_fat(&g, n->first, 0xFFFFFFF7u);
    xb_tree_free(&tree);
    put_drive(root, DRIVE, &g, 16, 2, 3);
    xb_free(&g);
}

static void check_images(void) {
    check_image("a backing file", SIGIL_ERR_UNSUPPORTED_FORMAT, with_backing_file, false);
    check_image("an encrypted image", SIGIL_ERR_UNSUPPORTED_FORMAT, encrypted, false);
    check_image("an internal snapshot", SIGIL_ERR_UNSUPPORTED_FORMAT, with_snapshot, false);
    check_image("an image left dirty", SIGIL_ERR_UNSUPPORTED_FORMAT, left_dirty, false);
    check_image("an external data file", SIGIL_ERR_UNSUPPORTED_FORMAT, external_data, false);
    check_image("extended L2 entries", SIGIL_ERR_UNSUPPORTED_FORMAT, extended_l2, false);
    check_image("dirty bitmaps", SIGIL_ERR_UNSUPPORTED_FORMAT, dirty_bitmaps, false);
    check_image("a reserved autoclear bit", SIGIL_ERR_UNSUPPORTED_FORMAT, reserved_autoclear, false);
    check_image("64-bit reference counts", SIGIL_ERR_UNSUPPORTED_FORMAT, wide_refcounts, false);
    check_image("qcow2 version 4", SIGIL_ERR_UNSUPPORTED_FORMAT, version_4, false);
    check_image("Zstandard compression", SIGIL_ERR_UNSUPPORTED_FORMAT, zstd, false);
    check_image("an image marked corrupt", SIGIL_ERR_DAMAGED, marked_corrupt, true);
    check_image("a version 3 header shorter than 104 bytes", SIGIL_ERR_DAMAGED, short_header, true);
    check_image("a drive without E:", SIGIL_ERR_DAMAGED, no_e, true);
    check_image("an image cut short", SIGIL_ERR_DAMAGED, cut_short, true);
    check_image("a save with a broken chain", SIGIL_ERR_DAMAGED, broken_chain, true);
    check_image("a folder whose chain ends in a bad cluster", SIGIL_ERR_DAMAGED, chain_into_bad, true);
}

/* A write the client reports failed, or one that didn't store what sigil gave it, fails the restore. */
static void check_write_failures(void) {
    const char *where = "write failures";
    sigil_sync_result *unit = collect_halo(1, 16, 2, where);
    for (int corrupt = 0; unit && corrupt < 2; corrupt++) {
        xb_guest g;
        second_drive(&g);
        mem_root root = {0};
        put_drive(&root, DRIVE, &g, 16, 2, 3);
        if (corrupt) {
            root.corrupt_write = 1;
            root.corrupt_at = 70000;
        } else {
            root.fail_write = 1;
        }
        game game;
        make_game(&game, &root, "xemu", HALO);
        game.req.overwrite_local = 1;
        sigil_sync_result *r = NULL;
        CHECK(sigil_restore(&game.req, unit->data, unit->len, &r) == SIGIL_ERR_IO, where,
              corrupt ? "a write that stored other bytes passed" : "a failed write passed");
        sigil_sync_result_free(r);
        root_free(&root);
        xb_free(&g);
    }
    sigil_sync_result_free(unit);
}

/* ---- the drive's name, and X1 BOX ------------------------------------------------ */

/* xemu attaches no drive until its hdd_path setting names one, so a request
 * without it names none: refused, with a drive of the usual name left alone. */
static void check_needs_a_named_drive(void) {
    const char *where = "xemu with no hdd_path";
    xb_guest g;
    blank_drive(&g);
    put_halo(&g, 1);
    mem_root root = {0};
    put_drive(&root, DRIVE, &g, 16, 2, 3);
    game game;
    make_game(&game, &root, "xemu", HALO);
    game.req.save.options = NULL;
    game.req.save.option_count = 0;
    game.req.overwrite_local = 1;
    sigil_sync_result *r = NULL;
    CHECK(sigil_collect(&game.req, &r) == SIGIL_ERR_INVALID_ARG && (!r || !r->data), where, "collect read a drive nothing named");
    sigil_sync_result_free(r);
    r = NULL;
    sigil_sync_result *unit = collect_halo(2, 16, 2, where);
    CHECK(unit && sigil_restore(&game.req, unit->data, unit->len, &r) == SIGIL_ERR_INVALID_ARG && root.writes == 0, where,
          "restore wrote to a drive nothing named");
    sigil_sync_result_free(r);
    sigil_sync_result_free(unit);
    root_free(&root);
    xb_free(&g);
}

static void check_names(void) {
    const char *where = "the drive's name";
    xb_guest g;
    blank_drive(&g);
    put_halo(&g, 1);
    static const char *const settings[] = { "my drive.qcow2", "/home/me/.local/share/xemu/xemu/my drive.qcow2",
                                            "C:\\Users\\me\\xemu\\my drive.qcow2" };
    for (size_t i = 0; i < 3; i++) {
        mem_root root = {0};
        put_drive(&root, "my drive.qcow2", &g, 16, 2, 3);
        game game;
        make_game(&game, &root, "xemu", HALO);
        game.option.key = "hdd_path";
        game.option.value = settings[i];
        game.req.save.options = &game.option;
        game.req.save.option_count = 1;
        sigil_sync_result *r = NULL;
        CHECK(sigil_collect(&game.req, &r) == SIGIL_OK && r && r->data, where, settings[i]);
        sigil_sync_result_free(r);
        root_free(&root);
    }

    /* X1 BOX keeps the image, qcow2 or raw, as hdd.img. */
    mem_root root = {0};
    put_drive(&root, "hdd.img", &g, 16, 2, 3);
    game game;
    make_game(&game, &root, "x1box", HALO);
    sigil_sync_result *r = NULL;
    CHECK(sigil_collect(&game.req, &r) == SIGIL_OK && r && r->data, where, "X1 BOX's hdd.img wasn't read");
    sigil_sync_result_free(r);

    /* Locating reports the drive as a file every game shares, never as the game's. */
    sigil_save_unit *unit = NULL;
    CHECK(sigil_save_resolve(&game.req.save, &unit) == SIGIL_OK && unit && unit->member_count == 0 &&
              unit->unkeyed_count == 1 && strcmp(unit->unkeyed[0], "hdd.img") == 0,
          where, "locate didn't report the drive as shared");
    sigil_save_unit_free(unit);

    sigil_layout_info *rows = NULL;
    size_t n = 0, found = 0;
    CHECK(sigil_layouts("xbox", &rows, &n) == SIGIL_OK, where, "no layout rows");
    for (size_t i = 0; rows && i < n; i++) {
        bool xemu = strcmp(rows[i].id, "xemu") == 0, x1box = strcmp(rows[i].id, "x1box") == 0;
        if (!xemu && !x1box) continue;
        found++;
        CHECK(rows[i].option_count == 1 && strcmp(rows[i].options[0].key, "hdd_path") == 0 &&
                  strcmp(rows[i].options[0].default_value, xemu ? "" : "hdd.img") == 0,
              where, "a row doesn't name its drive setting");
    }
    CHECK(found == 2, where, "the xemu and x1box rows aren't listed for xbox");
    sigil_layouts_free(rows);
    root_free(&root);
    xb_free(&g);
}

/* A raw image is the console's drive byte for byte, read here through a stream over the test's drive. */
static uint64_t g_raw_size = XB_DISK_SIZE;
static int raw_read(void *ctx, uint64_t off, void *buf, size_t len) {
    if (off >= g_raw_size) return 0;
    if (len > g_raw_size - off) len = (size_t)(g_raw_size - off);
    if (len > (1u << 30)) len = 1u << 30;
    xb_get((const xb_guest *)ctx, off, buf, len);
    return (int)len;
}
static int64_t raw_size(void *ctx) {
    (void)ctx;
    return (int64_t)g_raw_size;
}
static int g_raw_writes = 0;
static sigil_io *raw_open(void *ctx, const char *path) {
    if (strcmp(path, "hdd.img") != 0) return NULL;
    sigil_io *io = (sigil_io *)calloc(1, sizeof(*io));
    io->read = raw_read;
    io->size = raw_size;
    io->ctx = ctx;
    return io;
}
static int raw_write(void *ctx, const char *path, const uint8_t *data, size_t len) {
    (void)ctx;
    (void)path;
    (void)data;
    (void)len;
    g_raw_writes++;
    return -1;
}

static void check_raw(uint64_t size, const char *where) {
    g_raw_size = size;
    g_raw_writes = 0;
    xb_guest g;
    blank_drive(&g);
    put_others(&g);
    put_halo(&g, 1);
    static const char *const listing[] = { "hdd.img" };
    mem_root unused = {0};
    game game;
    make_game(&game, &unused, "x1box", HALO);
    game.req.save.listing = listing;
    game.req.save.listing_count = 1;
    game.req.save.open = raw_open;
    game.req.save.open_ctx = &g;
    game.req.write = raw_write;
    game.req.overwrite_local = 1;
    sigil_sync_result *r = NULL, *qcow = collect_halo(1, 16, 2, where);
    CHECK(sigil_collect(&game.req, &r) == SIGIL_OK && r && r->data && qcow && r->len == qcow->len &&
              memcmp(r->data, qcow->data, r->len) == 0,
          where, "a raw image and a qcow2 image of one drive gave different units");
    sigil_sync_result_free(r);
    r = NULL;

    /* Written back as one file, a raw drive is all of it held in memory: refused, nothing written. */
    sigil_sync_result *other = collect_halo(2, 16, 2, where);
    CHECK(other && sigil_restore(&game.req, other->data, other->len, &r) == SIGIL_ERR_OOM && g_raw_writes == 0, where,
          "a rewrite of the whole drive wasn't refused");
    sigil_sync_result_free(r);
    sigil_sync_result_free(other);
    sigil_sync_result_free(qcow);
    xb_free(&g);
}

/* ---- qcow2 corners ------------------------------------------------------------- */

/* A folder of more files than one directory cluster holds, folders ten deep,
 * and an empty file. */
static void check_large_tree(void) {
    const char *where = "a large tree";
    enum { FILES = 600 };
    sigil_zip_member *members = (sigil_zip_member *)calloc(FILES + 2, sizeof(*members));
    for (size_t i = 0; i < FILES; i++) {
        snprintf(members[i].name, sizeof(members[i].name), "UDATA/4d530004/ABCDEF012345/file%03u.dat", (unsigned)i);
        members[i].data = pattern(100 + i, (unsigned)i);
        members[i].len = 100 + i;
    }
    snprintf(members[FILES].name, sizeof(members[FILES].name), "TDATA/4d530004/a/b/c/d/e/f/g/h/i/j/deep.bin");
    members[FILES].data = pattern(17000, 7);
    members[FILES].len = 17000;
    snprintf(members[FILES + 1].name, sizeof(members[FILES + 1].name), "UDATA/4d530004/empty.bin");
    members[FILES + 1].data = pattern(1, 1);
    members[FILES + 1].len = 0;
    mem_root root = {0};
    sigil_sync_result *r = NULL;
    CHECK(restore_members(members, FILES + 2, &root, &r) == SIGIL_OK, where, "restore failed");
    xb_guest before, after;
    xb_tree tree;
    second_drive(&before);
    if (load_drive(&root, DRIVE, &after, &tree, where)) {
        for (size_t i = 0; i < FILES + 2; i++) {
            if (!xb_file_is(&after, &tree, members[i].name, members[i].data, members[i].len)) fail(where, members[i].name);
        }
        others_intact(&before, &after, "4d530004", where);
        only_game_changed(&before, &after, "4d530004", where);
    }
    game game;
    make_game(&game, &root, "xemu", HALO);
    sigil_sync_result *c = NULL;
    CHECK(sigil_collect(&game.req, &c) == SIGIL_OK && c && r && strcmp(c->identity_hash, r->identity_hash) == 0, where,
          "the tree didn't collect as it was restored");
    sigil_sync_result_free(c);
    sigil_sync_result_free(r);
    xb_tree_free(&tree);
    for (size_t i = 0; i < FILES + 2; i++) free(members[i].data);
    free(members);
    root_free(&root);
    xb_free(&after);
    xb_free(&before);
}

int main(void) {
    check_collect();
    check_collect_edges();
    check_restore(16, 2, 3, "restore, 64 KiB clusters, half compressed");
    check_restore(16, 1, 3, "restore, every cluster compressed");
    check_restore(16, 0, 2, "restore, qcow2 version 2");
    check_restore(12, 3, 3, "restore, 4 KiB clusters");
    check_restore(9, 0, 3, "restore, 512-byte clusters");
    check_restore_blank();
    check_both_ways();
    check_units_from_before();
    check_refusals();
    check_conflict();
    check_images();
    check_write_failures();
    check_names();
    check_needs_a_named_drive();
    check_raw(XB_DISK_SIZE, "a raw image, 8 GiB");
    check_raw(XB_RETAIL_SIZE, "a raw image that ends where E: does");
    check_large_tree();
    if (g_fails) {
        fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    printf("unit_sync_xbox: all checks passed\n");
    return 0;
}
