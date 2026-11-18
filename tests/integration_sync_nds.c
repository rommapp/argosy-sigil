// SPDX-License-Identifier: MPL-2.0
/* Nintendo DS sync against saves melonDS and DeSmuME wrote, for chips of
 * 512 bytes, 8 KiB and 256 KiB: a .dsv DeSmuME made itself collects to its
 * save, restore for DeSmuME writes the footer DeSmuME writes, and each
 * emulator's save crosses to the other's file. */
#include "save_corpus.h"
#include "legacy_units.h"

#define TEST_SKIP 77
#define FOOTER 122u

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static corpus_table g_nds;

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    const char        *ids[1];
} game;

static void make_game(game *g, mem_root *root, const char *layout, const char *content) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V4;
    g->ids[0] = content;
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = "nds";
    g->req.save.content_path = content;
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

static sigil_sync_result *collect(mem_root *root, const char *layout, const char *content) {
    game g;
    make_game(&g, root, layout, content);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK) {
        sigil_sync_result_free(r);
        return NULL;
    }
    return r;
}

static bool restore(mem_root *root, const char *layout, const char *content, const uint8_t *unit, size_t len) {
    game g;
    make_game(&g, root, layout, content);
    sigil_sync_result *w = NULL;
    int rc = sigil_restore(&g.req, unit, len, &w);
    sigil_sync_result_free(w);
    return rc == SIGIL_OK;
}

/* A .dsv DeSmuME wrote from no save collects as everything before its footer,
 * and restore for DeSmuME writes the same file: the footer differs only in its
 * first field, the bytes the game had written when the file last grew, which
 * DeSmuME doesn't read back. */
static void check_desmume_footer(const char *id, const char *content, const char *file) {
    size_t len = 0;
    uint8_t *dsv = corpus_sample(&g_nds, "nds", id, NULL, &len);
    if (!dsv) return;
    mem_root root = {0};
    root_put(&root, file, dsv, len);
    sigil_sync_result *r = collect(&root, "desmume", content);
    size_t raw = len - FOOTER;
    if (!r || r->len != raw || memcmp(r->data, dsv, raw) != 0) fail(id, "collects as the save before the footer");

    mem_root fresh = {0};
    mem_file *f = NULL;
    if (!r || !restore(&fresh, "desmume", content, r->data, r->len) || !(f = root_find(&fresh, file)) || f->len != len) {
        fail(id, "restore writes the .dsv at DeSmuME's size");
    } else if (memcmp(f->data, dsv, raw + 82) != 0 || memcmp(f->data + raw + 86, dsv + raw + 86, FOOTER - 86) != 0) {
        fail(id, "restore writes DeSmuME's own footer: padSize, type, addr_size, mem_size, version, cookie");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    root_free(&fresh);
    free(dsv);
}

/* melonDS's save, restored for DeSmuME, collects back to the same unit; the
 * save DeSmuME wrote restores to melonDS's file without the footer. */
static void check_crossing(const char *melon_id, const char *desmume_id, const char *content, const char *sav,
                           const char *dsv_name) {
    size_t melon_len = 0, dsv_len = 0;
    uint8_t *melon = corpus_sample(&g_nds, "nds", melon_id, NULL, &melon_len);
    uint8_t *dsv = corpus_sample(&g_nds, "nds", desmume_id, NULL, &dsv_len);
    if (!melon || !dsv) {
        free(melon);
        free(dsv);
        return;
    }
    mem_root m = {0};
    root_put(&m, sav, melon, melon_len);
    sigil_sync_result *a = collect(&m, "melonds", content);
    mem_root d = {0};
    sigil_sync_result *back = NULL;
    if (!a || a->len != melon_len || memcmp(a->data, melon, melon_len) != 0) {
        fail(melon_id, "melonDS's save is the unit");
    } else if (!restore(&d, "desmume", content, a->data, a->len) || !(back = collect(&d, "desmume", content)) ||
               strcmp(back->identity_hash, a->identity_hash) != 0) {
        fail(melon_id, "restored for DeSmuME, it collects back to the same unit");
    }

    mem_root dr = {0}, mr = {0};
    root_put(&dr, dsv_name, dsv, dsv_len);
    sigil_sync_result *b = collect(&dr, "desmume", content);
    mem_file *f = NULL;
    if (!b || b->len != dsv_len - FOOTER || memcmp(b->data, dsv, b->len) != 0) {
        fail(desmume_id, "DeSmuME's save is the unit without the footer");
    } else if (!restore(&mr, "melonds", content, b->data, b->len) || !(f = root_find(&mr, sav)) ||
               f->len != dsv_len - FOOTER || memcmp(f->data, dsv, f->len) != 0) {
        fail(desmume_id, "restored for melonDS, it is the save without the footer");
    }
    sigil_sync_result_free(a);
    sigil_sync_result_free(back);
    sigil_sync_result_free(b);
    root_free(&m);
    root_free(&d);
    root_free(&dr);
    root_free(&mr);
    free(melon);
    free(dsv);
}

int main(void) {
    char path[1024];
    if (corpus_platform_path("nds", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_nds) != 0) {
        fprintf(stderr, "SKIP: no nds manifest\n");
        return TEST_SKIP;
    }
    if (!corpus_present(&g_nds, "nds", "sonic-rush-desmume-new")) {
        fprintf(stderr, "SKIP: nds samples missing\n");
        corpus_free(&g_nds);
        return TEST_SKIP;
    }
    check_desmume_footer("mkds-desmume-new", "Mario Kart DS.nds", "Mario Kart DS.dsv");
    check_desmume_footer("sonic-rush-desmume-new", "Sonic Rush.nds", "Sonic Rush.dsv");
    check_desmume_footer("ytg-desmume-new", "Yoshi Touch & Go (USA).nds", "Yoshi Touch & Go (USA).dsv");
    check_crossing("mkds-melonds", "mkds-desmume", "Mario Kart DS.nds", "Mario Kart DS.sav", "Mario Kart DS.dsv");
    check_crossing("sonic-rush-melonds", "sonic-rush-desmume", "Sonic Rush.nds", "Sonic Rush.sav", "Sonic Rush.dsv");
    check_crossing("ytg-melonds", "ytg-desmume", "Yoshi Touch & Go (USA).nds", "Yoshi Touch & Go (USA).sav",
                   "Yoshi Touch & Go (USA).dsv");
    corpus_free(&g_nds);
    printf("nds sync: %d failures\n", g_fails);
    return corpus_exit(g_fails);
}
