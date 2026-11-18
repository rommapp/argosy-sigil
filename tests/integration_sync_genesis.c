// SPDX-License-Identifier: MPL-2.0
/* Mega Drive, Master System, Game Gear and 32X sync against saves Genesis Plus
 * GX and PicoDrive wrote: each core's save crosses to the other's file and back
 * to the same identity, the file restore writes for Genesis Plus GX is the one
 * Genesis Plus GX itself wrote after loading it, a newer save next to an older,
 * shorter file reaches PicoDrive whole, and a 32X save comes back to PicoDrive
 * as it was. */
#include "save_corpus.h"
#include "legacy_units.h"

#define TEST_SKIP 77

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static corpus_table g_genesis;

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    const char        *ids[1];
} game;

static void make_game(game *g, mem_root *root, const char *platform, const char *layout, const char *content) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V4;
    g->ids[0] = content;
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = platform;
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

static sigil_sync_result *collect(mem_root *root, const char *platform, const char *layout, const char *content) {
    game g;
    make_game(&g, root, platform, layout, content);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK) {
        sigil_sync_result_free(r);
        return NULL;
    }
    return r;
}

static bool restore(mem_root *root, const char *platform, const char *layout, const char *content,
                    const uint8_t *unit, size_t len) {
    game g;
    make_game(&g, root, platform, layout, content);
    sigil_sync_result *w = NULL;
    int rc = sigil_restore(&g.req, unit, len, &w);
    sigil_sync_result_free(w);
    return rc == SIGIL_OK;
}

static size_t without_ff(const uint8_t *b, size_t len) {
    while (len && b[len - 1] == 0xFF) len--;
    return len;
}

/* Genesis Plus GX's save restores for PicoDrive (32 KiB with 0xFF past the
 * save on Game Gear, as it came on Mega Drive) and collects back to its
 * identity; PicoDrive's save restores for Genesis Plus GX as the file Genesis
 * Plus GX wrote after loading it, which collects to PicoDrive's identity. */
static void check_crossing(const char *platform, const char *gpgx_id, const char *pico_id, const char *back_id,
                           const char *content, const char *srm, size_t pico_size) {
    size_t gpgx_len = 0, pico_len = 0, back_len = 0;
    uint8_t *gpgx = corpus_sample(&g_genesis, "genesis", gpgx_id, NULL, &gpgx_len);
    uint8_t *pico = corpus_sample(&g_genesis, "genesis", pico_id, NULL, &pico_len);
    uint8_t *back = corpus_sample(&g_genesis, "genesis", back_id, NULL, &back_len);
    if (!gpgx || !pico || !back) {
        free(gpgx);
        free(pico);
        free(back);
        return;
    }
    mem_root g = {0};
    root_put(&g, srm, gpgx, gpgx_len);
    sigil_sync_result *a = collect(&g, platform, "genesis_plus_gx", content);
    mem_root p = {0};
    mem_file *f = NULL;
    sigil_sync_result *again = NULL;
    size_t want = pico_size ? pico_size : gpgx_len;
    if (!a || a->len != gpgx_len || memcmp(a->data, gpgx, gpgx_len) != 0) {
        fail(gpgx_id, "Genesis Plus GX's save is the unit as it is");
    } else if (!restore(&p, platform, "picodrive", content, a->data, a->len) || !(f = root_find(&p, srm)) ||
               f->len != want || memcmp(f->data, gpgx, gpgx_len) != 0 || without_ff(f->data, f->len) != gpgx_len) {
        fail(gpgx_id, "restored for PicoDrive, it is the save with 0xFF up to PicoDrive's size");
    } else if (!(again = collect(&p, platform, "picodrive", content)) ||
               strcmp(again->identity_hash, a->identity_hash) != 0) {
        fail(gpgx_id, "PicoDrive's file collects back to the same identity");
    }

    mem_root pr = {0}, gr = {0};
    root_put(&pr, srm, pico, pico_len);
    sigil_sync_result *b = collect(&pr, platform, "picodrive", content);
    sigil_sync_result *b_back = NULL;
    if (!b || b->len != pico_len || memcmp(b->data, pico, pico_len) != 0) {
        fail(pico_id, "PicoDrive's save is the unit as it is");
    } else if (!restore(&gr, platform, "genesis_plus_gx", content, b->data, b->len) || !(f = root_find(&gr, srm)) ||
               f->len != back_len || memcmp(f->data, back, back_len) != 0) {
        fail(pico_id, "restored for Genesis Plus GX, it is the file Genesis Plus GX wrote after loading it");
    } else if (!(b_back = collect(&gr, platform, "genesis_plus_gx", content)) ||
               strcmp(b_back->identity_hash, b->identity_hash) != 0) {
        fail(pico_id, "Genesis Plus GX's file collects back to the same identity");
    }
    sigil_sync_result_free(a);
    sigil_sync_result_free(again);
    sigil_sync_result_free(b);
    sigil_sync_result_free(b_back);
    root_free(&g);
    root_free(&p);
    root_free(&pr);
    root_free(&gr);
    free(gpgx);
    free(pico);
    free(back);
}

/* Real saves round trip through the other core and come back the same size
 * and bytes, with PicoDrive's file there (its size from its own sample) on
 * Mega Drive, and without one on Master System and Game Gear. */
static void check_round_trip(const char *platform, const char *gpgx_id, const char *pico_id, const char *content,
                             const char *srm, bool pico_file_there) {
    size_t gpgx_len = 0, pico_len = 0;
    uint8_t *gpgx = corpus_sample(&g_genesis, "genesis", gpgx_id, NULL, &gpgx_len);
    uint8_t *pico = corpus_sample(&g_genesis, "genesis", pico_id, NULL, &pico_len);
    if (!gpgx || !pico) {
        free(gpgx);
        free(pico);
        return;
    }
    uint8_t *zero = (uint8_t *)calloc(1, pico_len);
    mem_root p = {0}, g = {0}, g2 = {0}, p2 = {0};
    if (pico_file_there) {
        root_put(&p, srm, zero, pico_len);
        root_put(&p2, srm, zero, pico_len);
    }
    sigil_sync_result *a = NULL, *b = NULL;
    mem_file *f = NULL;
    if (!restore(&p, platform, "picodrive", content, gpgx, gpgx_len) || !(a = collect(&p, platform, "picodrive", content)) ||
        !restore(&g, platform, "genesis_plus_gx", content, a->data, a->len) || !(f = root_find(&g, srm)) ||
        f->len != gpgx_len || memcmp(f->data, gpgx, gpgx_len) != 0) {
        fail(gpgx_id, "through PicoDrive and back, the same size and bytes");
    }
    if (!restore(&g2, platform, "genesis_plus_gx", content, pico, pico_len) ||
        !(b = collect(&g2, platform, "genesis_plus_gx", content)) ||
        !restore(&p2, platform, "picodrive", content, b->data, b->len) || !(f = root_find(&p2, srm)) ||
        f->len != pico_len || memcmp(f->data, pico, pico_len) != 0) {
        fail(pico_id, "through Genesis Plus GX and back, the same size and bytes");
    }
    sigil_sync_result_free(a);
    sigil_sync_result_free(b);
    root_free(&p);
    root_free(&g);
    root_free(&g2);
    root_free(&p2);
    free(zero);
    free(gpgx);
    free(pico);
}

/* A newer save restored for PicoDrive next to an older Genesis Plus GX file,
 * shorter than it, as a saves folder both cores share holds: PicoDrive gets
 * the whole save, the same file restore writes into an empty folder. */
static void check_stale_file(const char *platform, const char *stale_id, const char *save_id, const char *content,
                             const char *srm) {
    size_t stale_len = 0, save_len = 0;
    uint8_t *stale = corpus_sample(&g_genesis, "genesis", stale_id, NULL, &stale_len);
    uint8_t *save = corpus_sample(&g_genesis, "genesis", save_id, NULL, &save_len);
    if (!stale || !save) {
        free(stale);
        free(save);
        return;
    }
    mem_root empty = {0}, shared = {0};
    root_put(&shared, srm, stale, stale_len);
    mem_file *e = NULL, *s = NULL;
    if (stale_len >= without_ff(save, save_len)) {
        fail(save_id, "the older file is shorter than the newer save");
    } else if (!restore(&empty, platform, "picodrive", content, save, save_len) || !(e = root_find(&empty, srm)) ||
               !restore(&shared, platform, "picodrive", content, save, save_len) || !(s = root_find(&shared, srm)) ||
               s->len != e->len || memcmp(s->data, e->data, e->len) != 0 || s->len < save_len ||
               memcmp(s->data, save, save_len) != 0) {
        fail(save_id, "next to the older, shorter file, PicoDrive gets the whole save, as into an empty folder");
    }
    root_free(&empty);
    root_free(&shared);
    free(stale);
    free(save);
}

/* A 32X save, which only PicoDrive keeps, comes back to PicoDrive byte for byte. */
static void check_picodrive_alone(const char *id, const char *content, const char *srm) {
    size_t len = 0;
    uint8_t *own = corpus_sample(&g_genesis, "genesis", id, NULL, &len);
    if (!own) return;
    mem_root p = {0}, back = {0};
    root_put(&p, srm, own, len);
    sigil_sync_result *r = collect(&p, "32x", "picodrive", content);
    mem_file *f = NULL;
    if (!r || r->len != len || !restore(&back, "32x", "picodrive", content, r->data, r->len) ||
        !(f = root_find(&back, srm)) || f->len != len || memcmp(f->data, own, len) != 0) {
        fail(id, "collects and restores for PicoDrive byte for byte");
    }
    sigil_sync_result_free(r);
    root_free(&p);
    root_free(&back);
    free(own);
}

/* PicoDrive's own save from no save keeps its 0x00 fill through a crossing. */
static void check_picodrive_fill(const char *id, const char *content, const char *srm) {
    size_t len = 0;
    uint8_t *own = corpus_sample(&g_genesis, "genesis", id, NULL, &len);
    if (!own) return;
    mem_root p = {0}, g = {0}, back = {0};
    root_put(&p, srm, own, len);
    sigil_sync_result *r = collect(&p, "genesis", "picodrive", content);
    sigil_sync_result *x = NULL;
    mem_file *f = NULL;
    if (!r || !restore(&g, "genesis", "genesis_plus_gx", content, r->data, r->len) ||
        !(x = collect(&g, "genesis", "genesis_plus_gx", content)) ||
        !restore(&back, "genesis", "picodrive", content, x->data, x->len) || !(f = root_find(&back, srm)) ||
        f->len != len || memcmp(f->data, own, len) != 0) {
        fail(id, "crosses to Genesis Plus GX and back byte for byte");
    }
    sigil_sync_result_free(r);
    sigil_sync_result_free(x);
    root_free(&p);
    root_free(&g);
    root_free(&back);
    free(own);
}

int main(void) {
    char path[1024];
    if (corpus_platform_path("genesis", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_genesis) != 0) {
        fprintf(stderr, "SKIP: no genesis manifest\n");
        return TEST_SKIP;
    }
    if (!corpus_present(&g_genesis, "genesis", "cave-story-md-gpgx")) {
        fprintf(stderr, "SKIP: genesis sync samples missing\n");
        corpus_free(&g_genesis);
        return TEST_SKIP;
    }
    check_crossing("genesis", "cave-story-md-gpgx", "cave-story-md-picodrive", "cave-story-md-gpgx-back",
                   "doukutsu-en.gen", "doukutsu-en.srm", 0);
    check_crossing("gamegear", "defenders-of-oasis-gpgx", "defenders-of-oasis-picodrive",
                   "defenders-of-oasis-gpgx-back", "Defenders of Oasis (USA, Europe).gg",
                   "Defenders of Oasis (USA, Europe).srm", 0x8000);
    check_crossing("genesis", "sonic3-gpgx", "sonic3-picodrive", "sonic3-gpgx-back", "Sonic The Hedgehog 3 (USA).md",
                   "Sonic The Hedgehog 3 (USA).srm", 0);
    check_crossing("genesis", "wonder-boy-mw-gpgx", "wonder-boy-mw-picodrive", "wonder-boy-mw-gpgx-back",
                   "Wonder Boy in Monster World (USA, Europe).md", "Wonder Boy in Monster World (USA, Europe).srm", 0);
    check_crossing("sms", "phantasy-star-gpgx", "phantasy-star-picodrive", "phantasy-star-gpgx-back",
                   "Phantasy Star (USA, Europe).sms", "Phantasy Star (USA, Europe).srm", 0x8000);
    check_round_trip("genesis", "sonic3-gpgx", "sonic3-picodrive", "Sonic The Hedgehog 3 (USA).md",
                     "Sonic The Hedgehog 3 (USA).srm", true);
    check_round_trip("genesis", "wonder-boy-mw-gpgx", "wonder-boy-mw-picodrive",
                     "Wonder Boy in Monster World (USA, Europe).md", "Wonder Boy in Monster World (USA, Europe).srm", true);
    check_round_trip("genesis", "cave-story-md-gpgx", "cave-story-md-picodrive", "doukutsu-en.gen", "doukutsu-en.srm",
                     true);
    check_round_trip("sms", "phantasy-star-gpgx", "phantasy-star-picodrive", "Phantasy Star (USA, Europe).sms",
                     "Phantasy Star (USA, Europe).srm", false);
    check_round_trip("gamegear", "defenders-of-oasis-gpgx", "defenders-of-oasis-picodrive",
                     "Defenders of Oasis (USA, Europe).gg", "Defenders of Oasis (USA, Europe).srm", false);
    check_stale_file("gamegear", "defenders-of-oasis-gpgx", "defenders-of-oasis-gpgx-back",
                     "Defenders of Oasis (USA, Europe).gg", "Defenders of Oasis (USA, Europe).srm");
    check_stale_file("genesis", "wonder-boy-mw-gpgx", "wonder-boy-mw-gpgx-back",
                     "Wonder Boy in Monster World (USA, Europe).md", "Wonder Boy in Monster World (USA, Europe).srm");
    check_picodrive_fill("cave-story-md-picodrive-new", "doukutsu-en.gen", "doukutsu-en.srm");
    check_picodrive_alone("36-great-holes-picodrive",
                          "Golf Magazine Presents - 36 Great Holes Starring Fred Couples (Europe).32x",
                          "Golf Magazine Presents - 36 Great Holes Starring Fred Couples (Europe).srm");
    check_picodrive_alone("36-great-holes-picodrive-second",
                          "Golf Magazine Presents - 36 Great Holes Starring Fred Couples (Europe).32x",
                          "Golf Magazine Presents - 36 Great Holes Starring Fred Couples (Europe).srm");
    corpus_free(&g_genesis);
    printf("genesis sync: %d failures\n", g_fails);
    return corpus_exit(g_fails);
}
