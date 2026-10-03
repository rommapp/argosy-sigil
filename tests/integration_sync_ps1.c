// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
#include "mem_root.h"
#include "card_ps1.h"
#include <stdbool.h>

#define TEST_SKIP 77

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

/* ---- requests ---------------------------------------------------------------- */

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    sigil_save_option  options[2];
} game;

static void make_game(game *g, mem_root *root, const char *layout, const char *content, const char *title_id,
                      const char *const *ids, size_t id_count) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V3;
    g->result.platform = SIGIL_PLATFORM_PSX;
    snprintf(g->result.title_id, sizeof(g->result.title_id), "%s", title_id);
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = "psx";
    g->req.save.content_path = content;
    g->req.save.result = &g->result;
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
    g->req.save.open = root_open;
    g->req.save.open_ctx = root;
    g->req.game_ids = ids;
    g->req.game_id_count = id_count;
    g->req.write = root_write;
    g->req.write_ctx = root;
}

static void refresh_listing(game *g, mem_root *root) {
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
}

/* ---- samples ------------------------------------------------------------------ */

static corpus_table g_manifest, g_entries;

static uint8_t *sample(const char *id, size_t *len) {
    return corpus_sample(&g_manifest, "psx", id, NULL, len);
}

/* The unit holds exactly the entries.tsv rows of `id` that the game owns,
 * each with the data md5 entries.tsv records. */
static void check_unit_entries(const char *where, const uint8_t *unit, size_t len, const char *id,
                               const char *const *owners, size_t owner_count) {
    if (!unit || len != PS1_CARD_SIZE) { fail(where, "no PS1 card unit"); return; }
    sigil_card_listing *l = NULL;
    if (sigil_ps1_card_list(unit, SIGIL_CARD_FORMAT_PS1_RAW, &l) != SIGIL_OK) { fail(where, "unit did not list"); return; }
    size_t expected = 0;
    for (size_t r = 0; r < g_entries.nrows; r++) {
        if (strcmp(corpus_get(&g_entries, r, "id"), id) != 0) continue;
        const char *raw = corpus_get(&g_entries, r, "owner_id");
        char owner[16] = "";
        if (strlen(raw) == 10) { memcpy(owner, raw, 11); owner[4] = '-'; }
        bool mine = false;
        for (size_t o = 0; o < owner_count; o++) mine = mine || strcmp(owners[o], owner) == 0;
        if (!mine) continue;
        expected++;
        const char *name = corpus_get(&g_entries, r, "entry");
        const sigil_card_entry *e = NULL;
        for (size_t i = 0; i < l->entry_count && !e; i++) {
            if (strcmp(l->entries[i].name, name) == 0) e = &l->entries[i];
        }
        if (!e) { fail(where, "an owned save is missing from the unit"); continue; }
        uint8_t *data = (uint8_t *)malloc((size_t)e->blocks * PS1_BLOCK_SIZE);
        char md5[33] = "";
        if (sigil_ps1_entry_data(unit, e->first_block, e->blocks, data) == SIGIL_OK) {
            sigil_md5_of(data, (size_t)e->blocks * PS1_BLOCK_SIZE, md5);
        }
        free(data);
        if (strcmp(md5, corpus_get(&g_entries, r, "data_md5")) != 0) fail(where, "a save's data changed in the unit");
    }
    if (expected != l->entry_count) fail(where, "the unit holds saves the game doesn't own");
    sigil_card_listing_free(l);
}

static const char *const XENOGEARS[] = { "SLUS-00664", "SLUS-00669" };
static const char *const MEGAMAN[] = { "SLUS-01334" };
static const char *const FF_ORIGINS[] = { "SLUS-01541" };

/* Disc 2 finds the saves written under disc 1's product code. */
static void check_collect_own_card(sigil_sync_result **kept) {
    size_t len = 0;
    uint8_t *card = sample("xenogears-full-mcd", &len);
    if (!card) return;
    mem_root root = {0};
    root_put(&root, "Xenogears (USA) (Disc 2).srm", card, len);
    game g;
    make_game(&g, &root, "pcsx_rearmed", "Xenogears (USA) (Disc 2).cue", "SLUS-00669", XENOGEARS, 2);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK) {
        fail("collect own card", "collect failed");
    } else {
        check_unit_entries("collect own card", r->data, r->len, "xenogears-full-mcd", XENOGEARS, 2);
        if (strcmp(r->artifact, "Xenogears (USA) (Disc 2).srm") != 0) fail("collect own card", "artifact name");
        char md5[33];
        sigil_md5_of(r->data, r->len, md5);
        if (strcmp(md5, r->content_hash) != 0) fail("collect own card", "content hash is not the unit's md5");
        if (!r->changed || !r->state) fail("collect own card", "a first collect is not a change, or no state");
        *kept = r;
        r = NULL;
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(card);
}

/* Other games' saves on the card stay out of the unit. */
static void check_collect_skips_other_games(void) {
    size_t len = 0;
    uint8_t *card = sample("megaman-bad-link-mcd", &len);
    if (!card) return;
    mem_root root = {0};
    root_put(&root, "Mega Man Legends 2 (USA).srm", card, len);
    game g;
    make_game(&g, &root, "pcsx_rearmed", "Mega Man Legends 2 (USA).cue", "SLUS-01334", MEGAMAN, 1);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK) fail("collect other games", "collect failed");
    else check_unit_entries("collect other games", r->data, r->len, "megaman-bad-link-mcd", MEGAMAN, 1);
    sigil_sync_result_free(r);
    root_free(&root);
    free(card);
}

/* The game's saves on the shared slot-2 card come along. */
static void check_collect_shared_card(void) {
    size_t own_len = 0, shared_len = 0;
    uint8_t *own = sample("digimon-world-2-mcr", &own_len);
    uint8_t *shared = sample("ff-origins-mcr", &shared_len);
    if (own && shared) {
        mem_root root = {0};
        root_put(&root, "Final Fantasy Origins (USA).srm", own, own_len);
        root_put(&root, "pcsx-card2.mcd", shared, shared_len);
        game g;
        make_game(&g, &root, "pcsx_rearmed", "Final Fantasy Origins (USA).cue", "SLUS-01541", FF_ORIGINS, 1);
        sigil_sync_result *r = NULL;
        if (sigil_collect(&g.req, &r) != SIGIL_OK) fail("collect shared card", "collect failed");
        else check_unit_entries("collect shared card", r->data, r->len, "ff-origins-mcr", FF_ORIGINS, 1);
        sigil_sync_result_free(r);
        root_free(&root);
    }
    free(own);
    free(shared);
}

/* With the state from the last collect, an unchanged card is not a change and
 * a changed one is. */
static void check_changed(const sigil_sync_result *first) {
    size_t len = 0;
    uint8_t *card = sample("xenogears-full-mcd", &len);
    if (!card || !first) { free(card); return; }
    mem_root root = {0};
    root_put(&root, "Xenogears (USA) (Disc 2).srm", card, len);
    game g;
    make_game(&g, &root, "pcsx_rearmed", "Xenogears (USA) (Disc 2).cue", "SLUS-00669", XENOGEARS, 2);
    g.req.state = first->state;
    g.req.state_len = first->state_len;
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || r->changed) fail("changed", "an unchanged card reads as changed");
    sigil_sync_result_free(r);

    mem_file *f = root_find(&root, "Xenogears (USA) (Disc 2).srm");
    sigil_ps1_delete(f->data, 1);
    r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->changed) fail("changed", "a deleted save doesn't read as a change");
    sigil_sync_result_free(r);
    root_free(&root);
    free(card);
}

/* Every disc of a set keeps its sync under one key, so collecting disc 2
 * after syncing disc 1 is not a change. */
static void check_discs_share_state(const sigil_sync_result *first) {
    size_t len = 0;
    uint8_t *card = sample("xenogears-full-mcd", &len);
    if (!card || !first) { free(card); return; }
    mem_root root = {0};
    root_put(&root, "Xenogears (USA) (Disc 1).srm", card, len);
    game g;
    make_game(&g, &root, "pcsx_rearmed", "Xenogears (USA) (Disc 1).cue", "SLUS-00664", XENOGEARS, 2);
    g.req.state = first->state;
    g.req.state_len = first->state_len;
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || r->changed) fail("discs share state", "disc 1 reads as changed after disc 2 synced");
    sigil_sync_result_free(r);

    g.req.save.platform = "ps1";
    r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || r->changed) fail("discs share state", "another spelling of the platform lost the state");
    sigil_sync_result_free(r);
    root_free(&root);
    free(card);
}

/* A unit restored into an empty root, under another core, collects back to
 * the same saves. */
static void check_restore_into_empty_root(const sigil_sync_result *first) {
    if (!first) return;
    mem_root root = {0};
    game g;
    make_game(&g, &root, "mednafen_psx_hw", "Xenogears (USA) (Disc 1).cue", "SLUS-00664", XENOGEARS, 2);
    sigil_save_option method = { "beetle_psx_hw_use_mednafen_memcard0_method", "mednafen" };
    g.req.save.options = &method;
    g.req.save.option_count = 1;
    sigil_sync_result *r = NULL;
    if (sigil_restore(&g.req, first->data, first->len, &r) != SIGIL_OK) {
        fail("restore empty root", "restore failed");
    } else if (!root_find(&root, "Xenogears (USA) (Disc 1).0.mcr")) {
        fail("restore empty root", "the card didn't go where Beetle keeps it");
    } else {
        refresh_listing(&g, &root);
        g.req.state = r->state;
        g.req.state_len = r->state_len;
        sigil_sync_result *back = NULL;
        if (sigil_collect(&g.req, &back) != SIGIL_OK) {
            fail("restore empty root", "collect after restore failed");
        } else {
            if (strcmp(back->identity_hash, first->identity_hash) != 0) fail("restore empty root", "saves differ after the trip");
            if (back->changed) fail("restore empty root", "a restored unit reads as a local change");
        }
        sigil_sync_result_free(back);
    }
    sigil_sync_result_free(r);
    root_free(&root);
}

static uint8_t *extract_named(const uint8_t *image, const char *name, size_t *len) {
    sigil_card_listing *l = NULL;
    uint8_t *mcs = NULL;
    if (sigil_ps1_card_list(image, SIGIL_CARD_FORMAT_PS1_RAW, &l) != SIGIL_OK) return NULL;
    for (size_t i = 0; i < l->entry_count && !mcs; i++) {
        if (strcmp(l->entries[i].name, name) != 0) continue;
        *len = sigil_ps1_mcs_size(l->entries[i].blocks);
        mcs = (uint8_t *)malloc(*len);
        if (sigil_ps1_extract(image, l->entries[i].first_block, l->entries[i].blocks, mcs) != SIGIL_OK) { free(mcs); mcs = NULL; }
    }
    sigil_card_listing_free(l);
    return mcs;
}

/* Restoring a newer save of one game leaves every other game's save on the
 * card byte for byte; restoring over saves that changed since the last sync
 * is refused until the user says to overwrite. */
static void check_restore_keeps_other_games(void) {
    size_t len = 0;
    uint8_t *card = sample("megaman-bad-link-mcd", &len);
    if (!card) return;
    mem_root root = {0};
    root_put(&root, "Mega Man Legends 2 (USA).srm", card, len);
    game g;
    make_game(&g, &root, "pcsx_rearmed", "Mega Man Legends 2 (USA).cue", "SLUS-01334", MEGAMAN, 1);

    sigil_sync_result *collected = NULL;
    if (sigil_collect(&g.req, &collected) != SIGIL_OK || !collected->data) {
        fail("restore other games", "collect failed");
        sigil_sync_result_free(collected);
        root_free(&root);
        free(card);
        return;
    }
    size_t mcs_len = 0;
    uint8_t *mcs = extract_named(collected->data, "BASLUS-01334", &mcs_len);
    uint8_t *newer = (uint8_t *)malloc(PS1_CARD_SIZE);
    size_t other_len[2] = {0, 0};
    uint8_t *before[2] = { extract_named(card, "BASLUS-00453", &other_len[0]), extract_named(card, "BASLUS-01395", &other_len[1]) };
    if (mcs && newer && before[0] && before[1]) {
        mcs[PS1_FRAME_SIZE + 100] ^= 0xFF;
        sigil_ps1_format(newer);
        sigil_ps1_inject(newer, mcs, mcs_len);

        game fresh = g;
        fresh.req.state = NULL;
        fresh.req.state_len = 0;
        sigil_sync_result *r = NULL;
        int rc = sigil_restore(&fresh.req, newer, PS1_CARD_SIZE, &r);
        if (rc != SIGIL_ERR_CONFLICT || !r || !r->conflict || root.writes != 0) {
            fail("restore other games", "saves never synced here were overwritten without asking");
        }
        sigil_sync_result_free(r);

        g.req.state = collected->state;
        g.req.state_len = collected->state_len;
        r = NULL;
        if (sigil_restore(&g.req, newer, PS1_CARD_SIZE, &r) != SIGIL_OK || root.writes != 1) {
            fail("restore other games", "restore of an unchanged game failed");
        } else {
            mem_file *f = root_find(&root, "Mega Man Legends 2 (USA).srm");
            for (int i = 0; i < 2; i++) {
                size_t after_len = 0;
                uint8_t *after = extract_named(f->data, i ? "BASLUS-01395" : "BASLUS-00453", &after_len);
                if (!after || after_len != other_len[i] || memcmp(after, before[i], after_len) != 0) {
                    fail("restore other games", "another game's save changed");
                }
                free(after);
            }
            if (sigil_ps1_verify(f->data, mcs, mcs_len) != SIGIL_OK) fail("restore other games", "the newer save isn't on the card");
            sigil_card_listing *l = NULL;
            if (sigil_ps1_card_list(f->data, SIGIL_CARD_FORMAT_PS1_RAW, &l) != SIGIL_OK || l->corrupt_count != 1) {
                fail("restore other games", "the broken save that wasn't the game's was touched");
            }
            sigil_card_listing_free(l);
        }
        sigil_sync_result_free(r);
    } else {
        fail("restore other games", "setup failed");
    }
    free(before[0]);
    free(before[1]);
    free(newer);
    free(mcs);
    sigil_sync_result_free(collected);
    root_free(&root);
    free(card);
}

/* A card that stores a save's size field wrong still matches the unit built
 * from it, so restoring that unit changes nothing and raises no conflict. */
static void check_unnormalized_frame(void) {
    size_t len = 0;
    uint8_t *card = sample("xenogears-full-mcd", &len);
    if (!card) return;
    memset(card + PS1_FRAME_SIZE + 4, 0, 4);
    uint8_t x = 0;
    for (size_t i = 0; i < PS1_FRAME_SIZE - 1; i++) x ^= card[PS1_FRAME_SIZE + i];
    card[2 * PS1_FRAME_SIZE - 1] = x;

    mem_root root = {0};
    root_put(&root, "Xenogears (USA) (Disc 1).srm", card, len);
    game g;
    make_game(&g, &root, "pcsx_rearmed", "Xenogears (USA) (Disc 1).cue", "SLUS-00664", XENOGEARS, 2);
    sigil_sync_result *collected = NULL, *r = NULL;
    if (sigil_collect(&g.req, &collected) != SIGIL_OK) {
        fail("unnormalized frame", "collect failed");
    } else {
        g.req.state = collected->state;
        g.req.state_len = collected->state_len;
        int rc = sigil_restore(&g.req, collected->data, collected->len, &r);
        if (rc != SIGIL_OK || root.writes != 0) fail("unnormalized frame", "restoring the unchanged unit conflicted or wrote");
    }
    sigil_sync_result_free(r);
    sigil_sync_result_free(collected);
    root_free(&root);
    free(card);
}

/* A unit that doesn't fit is refused with nothing written. */
static void check_restore_no_room(void) {
    size_t full_len = 0, other_len = 0;
    uint8_t *full = sample("xenogears-full-mcd", &full_len);
    uint8_t *other = sample("megaman-bad-link-mcd", &other_len);
    if (full && other) {
        mem_root source = {0};
        root_put(&source, "Mega Man Legends 2 (USA).srm", other, other_len);
        game from;
        make_game(&from, &source, "pcsx_rearmed", "Mega Man Legends 2 (USA).cue", "SLUS-01334", MEGAMAN, 1);
        sigil_sync_result *unit = NULL;
        if (sigil_collect(&from.req, &unit) == SIGIL_OK && unit->data) {
            mem_root root = {0};
            root_put(&root, "Mega Man Legends 2 (USA).srm", full, full_len);
            game g;
            make_game(&g, &root, "pcsx_rearmed", "Mega Man Legends 2 (USA).cue", "SLUS-01334", MEGAMAN, 1);
            sigil_sync_result *r = NULL;
            if (sigil_restore(&g.req, unit->data, unit->len, &r) != SIGIL_ERR_NO_SPACE || root.writes != 0) {
                fail("restore no room", "a full card took the unit, or something was written");
            }
            sigil_sync_result_free(r);
            root_free(&root);
        } else {
            fail("restore no room", "setup failed");
        }
        sigil_sync_result_free(unit);
        root_free(&source);
    }
    free(full);
    free(other);
}

int main(void) {
    char path[1024];
    if (corpus_platform_path("psx", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_manifest) != 0) {
        fprintf(stderr, "SKIP: no psx manifest\n");
        return TEST_SKIP;
    }
    if (corpus_platform_path("psx", "entries.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_entries) != 0) {
        fprintf(stderr, "SKIP: no psx entries\n");
        return TEST_SKIP;
    }
    if (!corpus_present(&g_manifest, "psx", "xenogears-full-mcd")) {
        fprintf(stderr, "SKIP: psx samples missing\n");
        return TEST_SKIP;
    }

    sigil_sync_result *first = NULL;
    check_collect_own_card(&first);
    check_collect_skips_other_games();
    check_collect_shared_card();
    check_changed(first);
    check_discs_share_state(first);
    check_restore_into_empty_root(first);
    check_restore_keeps_other_games();
    check_restore_no_room();
    check_unnormalized_frame();
    sigil_sync_result_free(first);

    corpus_free(&g_manifest);
    corpus_free(&g_entries);
    printf("ps1 sync: %d failures\n", g_fails);
    return corpus_exit(g_fails);
}
