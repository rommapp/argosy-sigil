// SPDX-License-Identifier: MPL-2.0
/* Xbox sync against real drive images: the image xemu publishes, and drives
 * xemu ran games on. Collect gives exactly the files the manifests list for
 * each title, and a title's saves restore into another drive, and into the
 * blank image, with that drive's own saves and everything else on it left
 * as they were. The images are read back by xbox_drive.h, not by sigil. */
#include "save_corpus.h"
#include "sync_internal.h"
#include "mem_root.h"
#include "xbox_drive.h"

#define TEST_SKIP 77
#define DRIVE "xbox_hdd.qcow2"

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static corpus_table g_manifest, g_entries;

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    sigil_save_option  option;
} game;

static void make_game(game *g, mem_root *root, const char *save_id) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V4;
    g->result.platform = SIGIL_PLATFORM_XBOX;
    snprintf(g->result.save_id, sizeof(g->result.save_id), "%s", save_id);
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = "xemu";
    g->req.save.platform = "xbox";
    g->req.save.content_path = "game.iso";
    g->req.save.result = &g->result;
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
    g->req.save.open = root_open;
    g->req.save.open_ctx = root;
    g->req.write = root_write;
    g->req.write_ctx = root;
    g->req.overwrite_local = 1;
    g->option.key = "hdd_path";
    g->option.value = DRIVE;
    g->req.save.options = &g->option;
    g->req.save.option_count = 1;
}

/* Puts sample `id`'s drive in `root`; false when the sample isn't on disk. */
static bool load_sample(mem_root *root, const char *id) {
    size_t len = 0;
    unsigned char *data = corpus_sample(&g_manifest, "xbox", id, NULL, &len);
    if (!data) return false;
    root_put(root, DRIVE, data, len);
    free(data);
    return true;
}

static void lower(const char *in, char *out) {
    size_t i = 0;
    for (; in[i]; i++) out[i] = in[i] >= 'A' && in[i] <= 'Z' ? (char)(in[i] + 32) : in[i];
    out[i] = '\0';
}

/* The unit holds exactly the files entries.tsv lists for `owner` on sample `id`, with their bytes. */
static void check_unit(const sigil_sync_result *r, const char *id, const char *owner, const char *where) {
    sigil_zip_member *m = NULL;
    size_t n = 0, listed = 0;
    if (!r || !r->data || sigil_zip_read_mem(r->data, r->len, 1u << 28, &m, &n) != SIGIL_OK) {
        fail(where, "collect gave no unit");
        return;
    }
    for (size_t row = 0; row < g_entries.nrows; row++) {
        if (strcmp(corpus_get(&g_entries, row, "id"), id) != 0 || strcmp(corpus_get(&g_entries, row, "owner_id"), owner) != 0) {
            continue;
        }
        listed++;
        const char *entry = corpus_get(&g_entries, row, "entry");
        bool found = false;
        for (size_t i = 0; i < n && !found; i++) {
            char md5[33];
            sigil_md5_of(m[i].data, m[i].len, md5);
            found = strcmp(m[i].name, entry) == 0 && strcmp(md5, corpus_get(&g_entries, row, "data_md5")) == 0;
        }
        if (!found) fail(where, entry);
    }
    if (listed != n || !n) fail(where, "the unit holds files the manifest doesn't list");
    sigil_zip_members_free(m, n);
}

static sigil_sync_result *collect_from(const char *id, const char *owner) {
    mem_root root = {0};
    sigil_sync_result *r = NULL;
    if (load_sample(&root, id)) {
        game g;
        make_game(&g, &root, owner);
        if (sigil_collect(&g.req, &r) != SIGIL_OK) fail(id, "collect failed");
        if (root.writes) fail(id, "collect wrote to the save root");
        check_unit(r, id, owner, id);
    }
    root_free(&root);
    return r;
}

/* Restores `unit`, title `owner`'s saves, into sample `into`, and checks the drive that comes out. */
static void restore_into(const sigil_sync_result *unit, const char *owner, const char *into, const char *where) {
    mem_root root = {0};
    if (!unit || !unit->data || !load_sample(&root, into)) {
        root_free(&root);
        return;
    }
    xb_guest before, after;
    xb_check was, is;
    mem_file *f = root_find(&root, DRIVE);
    xb_read_image(f->data, f->len, &before, &was);
    game g;
    make_game(&g, &root, owner);
    sigil_sync_result *r = NULL;
    if (sigil_restore(&g.req, unit->data, unit->len, &r) != SIGIL_OK || root.writes != 1) fail(where, "restore failed");
    f = root_find(&root, DRIVE);
    xb_read_image(f->data, f->len, &after, &is);
    xb_tree tree;
    xb_walk(&after, &tree);
    if (is.faults) fail(where, "the qcow2 reference counts or tables are wrong");
    if (is.leaks > was.leaks) fail(where, "the restore left clusters counted that nothing refers to");
    if (tree.faults) fail(where, "E:'s FAT chains are wrong");
    char folder[32];
    lower(owner, folder);
    sigil_zip_member *m = NULL;
    size_t n = 0;
    if (sigil_zip_read_mem(unit->data, unit->len, 1u << 28, &m, &n) == SIGIL_OK) {
        for (size_t i = 0; i < n; i++) {
            if (!xb_file_is(&after, &tree, m[i].name, m[i].data, m[i].len)) fail(where, m[i].name);
        }
        sigil_zip_members_free(m, n);
    }
    if (xb_others_changed(&before, &after, folder)) fail(where, "the drive's other files aren't as they were");
    if (!xb_only_game_changed(&before, &after, folder)) fail(where, "the restore changed something that isn't the game's");

    /* The restored drive gives the same unit back. */
    make_game(&g, &root, owner);
    sigil_sync_result *again = NULL;
    if (sigil_collect(&g.req, &again) != SIGIL_OK || !again || again->len != unit->len ||
        memcmp(again->data, unit->data, unit->len) != 0) {
        fail(where, "collecting the restored drive gave another unit");
    }
    sigil_sync_result_free(again);
    sigil_sync_result_free(r);
    xb_tree_free(&tree);
    xb_free(&before);
    xb_free(&after);
    root_free(&root);
}

int main(void) {
    char path[1024];
    if (corpus_platform_path("xbox", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_manifest) != 0 ||
        corpus_platform_path("xbox", "entries.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_entries) != 0) {
        fprintf(stderr, "FAIL: can't read the xbox manifests\n");
        return 1;
    }
    if (!corpus_present(&g_manifest, "xbox", "xemu-blank")) {
        printf("SKIP: no xbox drive samples in %s/xbox/files\n", SIGIL_SAVE_FIXTURES);
        return TEST_SKIP;
    }

    /* Each title of every drive the manifests list, exactly as listed. */
    sigil_sync_result *flatout = collect_from("flatout-cabinetos", "454D0009");
    sigil_sync_result *orta = collect_from("orta-xemu", "53450007");
    /* A drive both games saved on in xemu: FlatOut's saves as the kernel rewrote them, Orta's beside them. */
    sigil_sync_result_free(collect_from("orta-flatout-xemu", "454D0009"));
    sigil_sync_result_free(collect_from("orta-flatout-xemu", "53450007"));

    /* The blank image holds no saves, and neither drive holds the other's game. */
    mem_root root = {0};
    if (load_sample(&root, "xemu-blank")) {
        game g;
        make_game(&g, &root, "454D0009");
        sigil_sync_result *r = NULL;
        if (sigil_collect(&g.req, &r) != SIGIL_OK || !r || r->data) fail("xemu-blank", "the blank image has saves");
        sigil_sync_result_free(r);
    }
    root_free(&root);

    restore_into(flatout, "454D0009", "orta-xemu", "FlatOut into Orta's drive");
    restore_into(flatout, "454D0009", "xemu-blank", "FlatOut into the blank image");
    restore_into(orta, "53450007", "flatout-cabinetos", "Orta into FlatOut's drive");
    restore_into(orta, "53450007", "xemu-blank", "Orta into the blank image");

    sigil_sync_result_free(flatout);
    sigil_sync_result_free(orta);
    corpus_free(&g_manifest);
    corpus_free(&g_entries);
    if (g_fails) fprintf(stderr, "%d check(s) failed\n", g_fails);
    else printf("integration_sync_xbox: all checks passed\n");
    return corpus_exit(g_fails);
}
