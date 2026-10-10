// SPDX-License-Identifier: MPL-2.0
/* Wii saves on Dolphin: the title's data/ folder travels as <code>/data/...,
 * for a disc and a WAD (each under the category its save_id carries), from the
 * libretro core's User/ and the standalone User folder, and Argosy's uploads
 * of the whole title folder restore without its installed content/. */
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

static void make_game(game *g, mem_root *root, const char *layout, const char *title_id, const char *save_id) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V4;
    g->result.platform = SIGIL_PLATFORM_WII;
    snprintf(g->result.title_id, sizeof(g->result.title_id), "%s", title_id);
    snprintf(g->result.save_id, sizeof(g->result.save_id), "%s", save_id);
    g->ids[0] = title_id;
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = "wii";
    g->req.save.content_path = "Game (USA).rvz";
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

static void put_text(mem_root *root, const char *path, const char *text) {
    root_put(root, path, (const uint8_t *)text, strlen(text));
}

static bool holds(mem_root *root, const char *path, const char *text) {
    mem_file *f = root_find(root, path);
    return f && f->len == strlen(text) && memcmp(f->data, text, f->len) == 0;
}

/* The unit's member names, joined with ',' in name order. */
static void names_of(const sigil_sync_result *r, char *out, size_t cap) {
    sigil_zip_member *m = NULL;
    size_t n = 0;
    out[0] = '\0';
    if (!r || !r->data || sigil_zip_read_mem(r->data, r->len, 1u << 20, &m, &n) != SIGIL_OK) return;
    for (size_t i = 0; i < n; i++) {
        size_t used = strlen(out);
        snprintf(out + used, cap - used, "%s%s", i ? "," : "", m[i].name);
    }
    sigil_zip_members_free(m, n);
}

#define DISC "User/Wii/title/00010000/52534245/"

/* A disc's save on the libretro core: data/ travels, content/, another
 * title's save and the GameCube card beside it don't; restore writes it back. */
static void check_disc(void) {
    mem_root root = {0};
    put_text(&root, DISC "data/banner.bin", "banner");
    put_text(&root, DISC "data/rs_save.dat", "progress");
    put_text(&root, DISC "content/title.tmd", "installed");
    put_text(&root, "User/Wii/title/00010000/53545645/data/other.dat", "another title");
    put_text(&root, "User/GC/USA/Card A/01-GALE-gzle.gci", "a gamecube save");
    game g;
    make_game(&g, &root, "dolphin", "52534245", "00010000/52534245");
    sigil_sync_result *r = NULL;
    char names[256];
    if (sigil_collect(&g.req, &r) != SIGIL_OK) fail("wii disc", "collect failed");
    names_of(r, names, sizeof(names));
    if (strcmp(names, "52534245/data/banner.bin,52534245/data/rs_save.dat") != 0) fail("wii disc", names);

    mem_root fresh = {0};
    game t;
    make_game(&t, &fresh, "dolphin", "52534245", "00010000/52534245");
    sigil_sync_result *w = NULL;
    if (!r || sigil_restore(&t.req, r->data, r->len, &w) != SIGIL_OK || !holds(&fresh, DISC "data/rs_save.dat", "progress") ||
        !holds(&fresh, DISC "data/banner.bin", "banner") || fresh.count != 2) {
        fail("wii disc", "restore didn't write the data folder where Dolphin reads it");
    }

    /* The same unit into standalone Dolphin's User folder. */
    mem_root standalone = {0};
    make_game(&t, &standalone, "dolphin_standalone", "52534245", "00010000/52534245");
    sigil_sync_result *ws = NULL;
    if (!r || sigil_restore(&t.req, r->data, r->len, &ws) != SIGIL_OK ||
        !holds(&standalone, "Wii/title/00010000/52534245/data/rs_save.dat", "progress")) {
        fail("wii disc", "standalone restore didn't write Wii/title/00010000/<code>/data/");
    }

    /* A result persisted before discs carried their category: the code alone
     * places the save under 00010000. */
    mem_root legacy = {0};
    make_game(&t, &legacy, "dolphin", "52534245", "52534245");
    sigil_sync_result *wl = NULL;
    if (!r || sigil_restore(&t.req, r->data, r->len, &wl) != SIGIL_OK || !holds(&legacy, DISC "data/rs_save.dat", "progress")) {
        fail("wii disc", "a save_id of the code alone didn't restore under 00010000");
    }

    sigil_sync_result_free(wl);
    sigil_sync_result_free(ws);
    sigil_sync_result_free(w);
    sigil_sync_result_free(r);
    root_free(&legacy);
    root_free(&standalone);
    root_free(&fresh);
    root_free(&root);
}

/* Mario Kart Wii installs a channel, so its ticket and its save folder carry
 * 00010004; a folder under 00010000 with the same code isn't its save. */
static void check_channel_disc(void) {
    mem_root root = {0};
    put_text(&root, "Wii/title/00010004/524d4345/data/rksys.dat", "time trials");
    put_text(&root, "Wii/title/00010000/524d4345/data/rksys.dat", "a folder Dolphin never wrote");
    game g;
    make_game(&g, &root, "dolphin_standalone", "524D4345", "00010004/524d4345");
    sigil_sync_result *r = NULL;
    char names[256];
    if (sigil_collect(&g.req, &r) != SIGIL_OK) fail("wii channel disc", "collect failed");
    names_of(r, names, sizeof(names));
    sigil_zip_member *m = NULL;
    size_t n = 0;
    bool right = r && r->data && sigil_zip_read_mem(r->data, r->len, 1u << 20, &m, &n) == SIGIL_OK && n == 1 &&
                 m[0].len == 11 && memcmp(m[0].data, "time trials", 11) == 0;
    if (m) sigil_zip_members_free(m, n);
    if (strcmp(names, "524d4345/data/rksys.dat") != 0 || !right) fail("wii channel disc", "not the save under 00010004");

    mem_root fresh = {0};
    game t;
    make_game(&t, &fresh, "dolphin", "524D4345", "00010004/524d4345");
    sigil_sync_result *w = NULL;
    if (!r || sigil_restore(&t.req, r->data, r->len, &w) != SIGIL_OK ||
        !holds(&fresh, "User/Wii/title/00010004/524d4345/data/rksys.dat", "time trials") || fresh.count != 1) {
        fail("wii channel disc", "restore didn't write under 00010004");
    }
    sigil_sync_result_free(w);
    sigil_sync_result_free(r);
    root_free(&fresh);
    root_free(&root);
}

/* A WAD's save id carries its category: only that category's folder is the game's. */
static void check_wad(void) {
    mem_root root = {0};
    put_text(&root, "Wii/title/00010001/574b5445/data/save.bin", "wiiware");
    put_text(&root, "Wii/title/00010000/574b5445/data/save.bin", "a disc with the same code");
    game g;
    make_game(&g, &root, "dolphin_standalone", "00010001574B5445", "00010001/574b5445");
    sigil_sync_result *r = NULL;
    char names[256];
    if (sigil_collect(&g.req, &r) != SIGIL_OK) fail("wii wad", "collect failed");
    names_of(r, names, sizeof(names));
    sigil_zip_member *m = NULL;
    size_t n = 0;
    bool right = r && r->data && sigil_zip_read_mem(r->data, r->len, 1u << 20, &m, &n) == SIGIL_OK && n == 1 &&
                 m[0].len == 7 && memcmp(m[0].data, "wiiware", 7) == 0;
    if (m) sigil_zip_members_free(m, n);
    if (strcmp(names, "574b5445/data/save.bin") != 0 || !right) fail("wii wad", "not the save in the WAD's own category");

    mem_root fresh = {0};
    game t;
    make_game(&t, &fresh, "dolphin_standalone", "00010001574B5445", "00010001/574b5445");
    sigil_sync_result *w = NULL;
    if (!r || sigil_restore(&t.req, r->data, r->len, &w) != SIGIL_OK ||
        !holds(&fresh, "Wii/title/00010001/574b5445/data/save.bin", "wiiware")) {
        fail("wii wad", "restore didn't write under the WAD's category");
    }
    sigil_sync_result_free(w);
    sigil_sync_result_free(r);
    root_free(&fresh);
    root_free(&root);
}

/* Argosy zipped the whole title folder: its content/ is left out, its data/ restores as sigil's unit does. */
static void check_argosy_upload(void) {
    const char *names[] = { "52534245/data/banner.bin", "52534245/data/rs_save.dat", "52534245/content/title.tmd" };
    uint8_t *data[] = { (uint8_t *)"banner", (uint8_t *)"progress", (uint8_t *)"installed" };
    size_t lens[] = { 6, 8, 9 }, zip_len = 0;
    uint8_t *zip = legacy_zip(names, data, lens, 3, &zip_len);
    mem_root root = {0};
    game g;
    make_game(&g, &root, "dolphin", "52534245", "00010000/52534245");
    sigil_sync_result *w = NULL;
    if (!zip || sigil_restore(&g.req, zip, zip_len, &w) != SIGIL_OK || !holds(&root, DISC "data/rs_save.dat", "progress") ||
        root_find(&root, DISC "content/title.tmd") || root.count != 2) {
        fail("argosy wii upload", "content/ wasn't left out, or data/ didn't restore");
    }
    sigil_sync_result_free(w);
    root_free(&root);
    free(zip);
}

int main(void) {
    check_disc();
    check_channel_disc();
    check_wad();
    check_argosy_upload();
    printf("wii sync: %d failures\n", g_fails);
    return g_fails ? 1 : 0;
}
