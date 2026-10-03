// SPDX-License-Identifier: MPL-2.0
/* Stitching: a game that reads an earlier title's save gets that save on its
 * card or volume as a companion. Companion saves go on with the game's own,
 * stay out of the game's unit, and come back as the companion's own unit. */
#include "save_corpus.h"
#include "mem_root.h"
#include "card_ps1.h"
#include "card_saturn.h"
#include <stdbool.h>

#define TEST_SKIP 77

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static corpus_table g_psx, g_saturn;

static uint8_t *sample(const corpus_table *manifest, const char *platform, const char *id, const char *path, size_t *len) {
    return corpus_sample(manifest, platform, id, path, len);
}

typedef struct {
    sigil_sync_request   req;
    sigil_result         result;
    const char          *ids[1];
    sigil_sync_companion companion;
    const char          *companion_ids[1];
} game;

static void make_game(game *g, mem_root *root, const char *layout, const char *platform, int platform_id,
                      const char *content, const char *id) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V3;
    g->result.platform = platform_id;
    snprintf(g->result.title_id, sizeof(g->result.title_id), "%s", id);
    g->ids[0] = id;
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
}

static void with_companion(game *g, const char *id, const sigil_sync_result *unit) {
    g->companion_ids[0] = id;
    g->companion.game_ids = g->companion_ids;
    g->companion.game_id_count = 1;
    g->companion.unit = unit ? unit->data : NULL;
    g->companion.unit_len = unit ? unit->len : 0;
    g->req.companions = &g->companion;
    g->req.companion_count = 1;
}

static void refresh(game *g, mem_root *root) {
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
}

static size_t ps1_saves_of(const uint8_t *card, size_t len, const char *owner) {
    sigil_card_listing *l = NULL;
    sigil_io *io = mem_root_io(card, len);
    size_t n = 0;
    if (sigil_card_list(io, &l) == SIGIL_OK) {
        for (size_t i = 0; i < l->entry_count; i++) n += strcmp(l->entries[i].owner_id, owner) == 0;
    }
    sigil_card_listing_free(l);
    sigil_io_close(io);
    return n;
}

#define MML2 "SLUS-01334"
#define MML1 "SLUS-00453"
#define MML2_CUE "Mega Man Legends 2 (USA).cue"

/* PS1, ids on the card: the companion's save goes on the game's own card,
 * collect returns it as the companion's unit, and a change the game made to
 * it reads as the companion's change, not the game's. */
static void check_ps1(void) {
    size_t len = 0;
    uint8_t *card = sample(&g_psx, "psx", "megaman-bad-link-mcd", NULL, &len);
    if (!card) return;
    mem_root src = {0};
    root_put(&src, "Mega Man Legends (USA).srm", card, len);
    game a, b;
    make_game(&a, &src, "pcsx_rearmed", "psx", SIGIL_PLATFORM_PSX, "Mega Man Legends (USA).cue", MML1);
    sigil_sync_result *first = NULL, *second = NULL;
    sigil_collect(&a.req, &first);
    root_free(&src);
    root_put(&src, "Mega Man Legends 2 (USA).srm", card, len);
    make_game(&b, &src, "pcsx_rearmed", "psx", SIGIL_PLATFORM_PSX, MML2_CUE, MML2);
    sigil_collect(&b.req, &second);
    if (!first || !first->data || !second || !second->data) {
        fail("ps1 companions", "setup failed");
    } else {
        mem_root root = {0};
        game g;
        make_game(&g, &root, "pcsx_rearmed", "psx", SIGIL_PLATFORM_PSX, MML2_CUE, MML2);
        with_companion(&g, MML1, first);
        sigil_sync_result *r = NULL, *c = NULL;
        if (sigil_restore(&g.req, second->data, second->len, &r) != SIGIL_OK) {
            fail("ps1 companions", "restore with a companion failed");
        } else {
            mem_file *f = root_find(&root, "Mega Man Legends 2 (USA).srm");
            if (!f || ps1_saves_of(f->data, f->len, MML1) != 1 || ps1_saves_of(f->data, f->len, MML2) != 1) {
                fail("ps1 companions", "the card doesn't hold the game's save and the companion's");
            }
            refresh(&g, &root);
            g.req.state = r->state;
            g.req.state_len = r->state_len;
            g.companion.unit = NULL;
            g.companion.unit_len = 0;
            if (sigil_collect(&g.req, &c) != SIGIL_OK || c->changed || strcmp(c->identity_hash, second->identity_hash) != 0) {
                fail("ps1 companions", "the companion's save went into the game's unit");
            } else if (c->companion_count != 1 || !c->companions[0].data ||
                       strcmp(c->companions[0].identity_hash, first->identity_hash) != 0 || c->companions[0].changed) {
                fail("ps1 companions", "collect didn't return the companion's save as its unit");
            }
            sigil_sync_result_free(c);
            c = NULL;

            f = root_find(&root, "Mega Man Legends 2 (USA).srm");
            sigil_card_listing *l = NULL;
            if (sigil_ps1_card_list(f->data, SIGIL_CARD_FORMAT_PS1_RAW, &l) == SIGIL_OK) {
                for (size_t i = 0; i < l->entry_count; i++) {
                    if (strcmp(l->entries[i].owner_id, MML1) == 0) {
                        f->data[(size_t)l->entries[i].first_block * PS1_BLOCK_SIZE + 300] ^= 0xFF;
                    }
                }
            }
            sigil_card_listing_free(l);
            if (sigil_collect(&g.req, &c) != SIGIL_OK || c->changed || c->companion_count != 1 || !c->companions[0].changed) {
                fail("ps1 companions", "a change to the companion's save didn't read as the companion's");
            }
            sigil_sync_result_free(c);
            c = NULL;

            f = root_find(&root, "Mega Man Legends 2 (USA).srm");
            l = NULL;
            if (sigil_ps1_card_list(f->data, SIGIL_CARD_FORMAT_PS1_RAW, &l) == SIGIL_OK) {
                for (size_t i = 0; i < l->entry_count; i++) {
                    if (strcmp(l->entries[i].owner_id, MML2) == 0) {
                        f->data[(size_t)l->entries[i].first_block * PS1_BLOCK_SIZE + 300] ^= 0xFF;
                    }
                }
            }
            sigil_card_listing_free(l);
            sigil_sync_result *again = NULL;
            g.req.overwrite_local = 1;
            int writes = root.writes;
            int rc = sigil_restore(&g.req, second->data, second->len, &again);
            f = root_find(&root, "Mega Man Legends 2 (USA).srm");
            if (rc != SIGIL_OK || root.writes == writes || ps1_saves_of(f->data, f->len, MML1) != 1) {
                fail("ps1 companions", "a restore without the companion's unit dropped its save");
            }
            sigil_sync_result_free(again);
        }
        sigil_sync_result_free(r);
        root_free(&root);

        size_t full_len = 0;
        uint8_t *full = sample(&g_psx, "psx", "xenogears-full-mcd", NULL, &full_len);
        if (full) {
            mem_root crowded = {0};
            root_put(&crowded, "Mega Man Legends 2 (USA).srm", full, full_len);
            game h;
            make_game(&h, &crowded, "pcsx_rearmed", "psx", SIGIL_PLATFORM_PSX, MML2_CUE, MML2);
            with_companion(&h, MML1, first);
            h.req.overwrite_local = 1;
            r = NULL;
            if (sigil_restore(&h.req, second->data, second->len, &r) != SIGIL_ERR_NO_SPACE || !r || !r->overflow[0] ||
                r->overflow_blocks == 0 || crowded.writes != 0) {
                fail("ps1 companions", "a full card didn't name the save that overflowed and the shortfall");
            }
            sigil_sync_result_free(r);
            root_free(&crowded);
            free(full);
        }
    }
    sigil_sync_result_free(first);
    sigil_sync_result_free(second);
    root_free(&src);
    free(card);
}

/* A volume with one .BUP, written raw. */
static uint8_t *volume_with(const char *bup_id, const char *bup_path, size_t *len) {
    size_t bup_len = 0;
    uint8_t *bup = sample(&g_saturn, "saturn", bup_id, bup_path, &bup_len);
    sigil_saturn_volume v;
    sigil_bram_storage raw;
    memset(&raw, 0, sizeof(raw));
    raw.filler = -1;
    uint8_t *out = NULL;
    if (bup && sigil_saturn_volume_format(&v, SATURN_INTERNAL_SIZE, &raw) == SIGIL_OK) {
        if (sigil_saturn_inject(&v, bup, bup_len) != SIGIL_OK || sigil_saturn_volume_write(&v, &out, len) != SIGIL_OK) out = NULL;
        sigil_saturn_volume_free(&v);
    }
    free(bup);
    return out;
}

static size_t volume_names(const uint8_t *data, size_t len, const char *name) {
    sigil_card_listing *l = NULL;
    sigil_io *io = mem_root_io(data, len);
    size_t n = 0;
    if (sigil_card_list(io, &l) == SIGIL_OK) {
        for (size_t i = 0; i < l->entry_count; i++) n += strcmp(l->entries[i].name, name) == 0;
    }
    sigil_card_listing_free(l);
    sigil_io_close(io);
    return n;
}

/* Saturn, no ids: the companion's save goes on the game's own per-game
 * volume, and collect still tells it apart by the owner restore recorded. */
static void check_saturn(void) {
    size_t own_len = 0, other_len = 0;
    uint8_t *own = sample(&g_saturn, "saturn", "rayman-bkr-bcr", "Rayman (USA) (R2)-internal.bkr", &own_len);
    uint8_t *other = volume_with("three-dirty-dwarves-bup", NULL, &other_len);
    if (!own || !other) { free(own); free(other); fail("saturn companions", "setup failed"); return; }
    mem_root a_root = {0}, b_root = {0};
    root_put(&a_root, "Three Dirty Dwarves (USA).srm", other, other_len);
    root_put(&b_root, "Rayman (USA) (R2).srm", own, own_len);
    game a, b;
    make_game(&a, &a_root, "mednafen_saturn", "saturn", 0, "Three Dirty Dwarves (USA).cue", "T-30401H");
    make_game(&b, &b_root, "mednafen_saturn", "saturn", 0, "Rayman (USA) (R2).cue", "T-17701G");
    sigil_sync_result *dwarves = NULL, *rayman = NULL;
    sigil_collect(&a.req, &dwarves);
    sigil_collect(&b.req, &rayman);
    if (!dwarves || !dwarves->data || !rayman || !rayman->data) {
        fail("saturn companions", "setup collect failed");
    } else {
        mem_root root = {0};
        game g;
        make_game(&g, &root, "mednafen_saturn", "saturn", 0, "Rayman (USA) (R2).cue", "T-17701G");
        with_companion(&g, "T-30401H", dwarves);
        sigil_sync_result *r = NULL, *c = NULL;
        if (sigil_restore(&g.req, rayman->data, rayman->len, &r) != SIGIL_OK) {
            fail("saturn companions", "restore with a companion failed");
        } else {
            mem_file *f = root_find(&root, "Rayman (USA) (R2).srm");
            if (!f || volume_names(f->data, f->len, "THREE_DIRTY") != 1 || volume_names(f->data, f->len, "RAYMAN_NTS1") != 1) {
                fail("saturn companions", "the volume doesn't hold the game's save and the companion's");
            }
            refresh(&g, &root);
            g.req.state = r->state;
            g.req.state_len = r->state_len;
            g.companion.unit = NULL;
            g.companion.unit_len = 0;
            if (sigil_collect(&g.req, &c) != SIGIL_OK || strcmp(c->identity_hash, rayman->identity_hash) != 0 ||
                c->companion_count != 1 || strcmp(c->companions[0].identity_hash, dwarves->identity_hash) != 0) {
                fail("saturn companions", "collect didn't split the game's save from the companion's");
            }
            sigil_sync_result_free(c);

            sigil_sync_result *again = NULL;
            f = root_find(&root, "Rayman (USA) (R2).srm");
            sigil_card_listing *l = NULL;
            sigil_io *io = mem_root_io(f->data, f->len);
            if (sigil_card_list(io, &l) == SIGIL_OK) {
                for (size_t i = 0; i < l->entry_count; i++) {
                    if (strncmp(l->entries[i].name, "RAYMAN", 6) == 0) {
                        f->data[(size_t)l->entries[i].first_block * 64 + 40] ^= 0xFF;
                    }
                }
            }
            sigil_card_listing_free(l);
            sigil_io_close(io);
            g.req.overwrite_local = 1;
            int writes = root.writes;
            int rc = sigil_restore(&g.req, rayman->data, rayman->len, &again);
            f = root_find(&root, "Rayman (USA) (R2).srm");
            if (rc != SIGIL_OK || root.writes == writes || volume_names(f->data, f->len, "THREE_DIRTY") != 1) {
                fail("saturn companions", "a restore without the companion's unit dropped its save");
            }
            sigil_sync_result_free(again);
        }
        sigil_sync_result_free(r);
        root_free(&root);
    }
    sigil_sync_result_free(dwarves);
    sigil_sync_result_free(rayman);
    root_free(&a_root);
    root_free(&b_root);
    free(own);
    free(other);
}

int main(void) {
    char path[1024];
    if (corpus_platform_path("psx", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_psx) != 0 ||
        corpus_platform_path("saturn", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_saturn) != 0) {
        fprintf(stderr, "SKIP: manifests missing\n");
        return TEST_SKIP;
    }
    if (!corpus_present(&g_psx, "psx", "megaman-bad-link-mcd") || !corpus_present(&g_saturn, "saturn", "rayman-bkr-bcr")) {
        fprintf(stderr, "SKIP: samples missing\n");
        return TEST_SKIP;
    }

    check_ps1();
    check_saturn();

    corpus_free(&g_psx);
    corpus_free(&g_saturn);
    printf("companion sync: %d failures\n", g_fails);
    return corpus_exit(g_fails);
}
