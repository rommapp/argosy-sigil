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

/* `card` with its save named `name` replaced by `mcs`, or `mcs` added when
 * `name` is NULL. */
static uint8_t *card_with(const uint8_t *card, const char *name, const uint8_t *mcs, size_t mcs_len) {
    uint8_t *out = (uint8_t *)malloc(PS1_CARD_SIZE);
    if (!out) return NULL;
    memcpy(out, card, PS1_CARD_SIZE);
    sigil_card_listing *l = NULL;
    if (name && sigil_ps1_card_list(out, SIGIL_CARD_FORMAT_PS1_RAW, &l) == SIGIL_OK) {
        for (size_t i = 0; i < l->entry_count; i++) {
            if (strcmp(l->entries[i].name, name) == 0) sigil_ps1_delete(out, l->entries[i].first_block);
        }
    }
    sigil_card_listing_free(l);
    if (mcs && sigil_ps1_inject(out, mcs, mcs_len) != SIGIL_OK) { free(out); return NULL; }
    return out;
}

/* A fresh card holding the given saves. */
static uint8_t *card_of(const uint8_t *const *mcs, const size_t *len, size_t count) {
    uint8_t *out = (uint8_t *)malloc(PS1_CARD_SIZE);
    if (!out) return NULL;
    sigil_ps1_format(out);
    for (size_t i = 0; i < count; i++) {
        if (sigil_ps1_inject(out, mcs[i], len[i]) != SIGIL_OK) { free(out); return NULL; }
    }
    return out;
}

#define MML2_CARD "Mega Man Legends 2 (USA).srm"
#define MML2_CUE  "Mega Man Legends 2 (USA).cue"

/* The conflict rule in each arrangement, with the state of a sync: local
 * saves already equal to what the restore brings are not a conflict and
 * aren't written; local saves changed to something else are a conflict until
 * the user says to overwrite; a card the game's saves were deleted from
 * since takes the restore. */
static void check_conflict_rules(void) {
    size_t len = 0;
    uint8_t *card = sample("megaman-bad-link-mcd", &len);
    if (!card) return;
    mem_root root = {0};
    root_put(&root, MML2_CARD, card, len);
    game g;
    make_game(&g, &root, "pcsx_rearmed", MML2_CUE, "SLUS-01334", MEGAMAN, 1);
    sigil_sync_result *synced = NULL;
    size_t mcs_len = 0;
    uint8_t *mcs = NULL, *b = NULL, *c = NULL, *unit_b = NULL, *unit_c = NULL, *local_b = NULL, *emptied = NULL;
    if (sigil_collect(&g.req, &synced) != SIGIL_OK || !(mcs = extract_named(synced->data, "BASLUS-01334", &mcs_len))) {
        fail("conflict rules", "setup failed");
        goto done;
    }
    b = (uint8_t *)malloc(mcs_len);
    c = (uint8_t *)malloc(mcs_len);
    memcpy(b, mcs, mcs_len);
    memcpy(c, mcs, mcs_len);
    b[PS1_FRAME_SIZE + 100] ^= 0xFF;
    c[PS1_FRAME_SIZE + 200] ^= 0xFF;
    const uint8_t *one[1] = { b };
    unit_b = card_of(one, &mcs_len, 1);
    one[0] = c;
    unit_c = card_of(one, &mcs_len, 1);
    local_b = card_with(card, "BASLUS-01334", b, mcs_len);
    emptied = card_with(card, "BASLUS-01334", NULL, 0);
    if (!unit_b || !unit_c || !local_b || !emptied) { fail("conflict rules", "setup failed"); goto done; }
    g.req.state = synced->state;
    g.req.state_len = synced->state_len;

    struct { const char *what; const uint8_t *local; const uint8_t *unit; int overwrite; int rc; int writes; } CASES[] = {
        { "local already equals the restore", local_b, unit_b, 0, SIGIL_OK, 0 },
        { "local changed to something else", local_b, unit_c, 0, SIGIL_ERR_CONFLICT, 0 },
        { "local changed, the user overwrites", local_b, unit_c, 1, SIGIL_OK, 1 },
        { "local saves deleted since the sync", emptied, unit_b, 0, SIGIL_OK, 1 },
        { "local unchanged", card, unit_b, 0, SIGIL_OK, 1 },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        root_put(&root, MML2_CARD, CASES[i].local, PS1_CARD_SIZE);
        root.writes = 0;
        g.req.overwrite_local = CASES[i].overwrite;
        sigil_sync_result *r = NULL;
        int rc = sigil_restore(&g.req, CASES[i].unit, PS1_CARD_SIZE, &r);
        if (rc != CASES[i].rc || root.writes != CASES[i].writes || (rc == SIGIL_ERR_CONFLICT && (!r || !r->conflict))) {
            fail("conflict rules", CASES[i].what);
        }
        sigil_sync_result_free(r);
    }
done:
    free(emptied);
    free(local_b);
    free(unit_c);
    free(unit_b);
    free(c);
    free(b);
    free(mcs);
    sigil_sync_result_free(synced);
    root_free(&root);
    free(card);
}

/* A unit speaks only for its game: another game's save inside it isn't
 * placed, a unit with none of the game's saves is refused, and so is one
 * holding a broken save. The title id alone names the game's saves. */
static void check_unit_rules(void) {
    size_t len = 0, mml1_len = 0, mml2_len = 0;
    uint8_t *card = sample("megaman-bad-link-mcd", &len);
    if (!card) return;
    uint8_t *mml1 = extract_named(card, "BASLUS-00453", &mml1_len);
    uint8_t *mml2 = extract_named(card, "BASLUS-01334", &mml2_len);
    const uint8_t *both[2] = { mml1, mml2 };
    size_t both_len[2] = { mml1_len, mml2_len };
    uint8_t *mixed = mml1 && mml2 ? card_of(both, both_len, 2) : NULL;
    uint8_t *other = mml1 ? card_of(both, both_len, 1) : NULL;
    if (!mixed || !other) { fail("unit rules", "setup failed"); goto done; }

    mem_root root = {0};
    game g;
    make_game(&g, &root, "pcsx_rearmed", MML2_CUE, "SLUS-01334", MEGAMAN, 1);
    sigil_sync_result *r = NULL;
    if (sigil_restore(&g.req, mixed, PS1_CARD_SIZE, &r) != SIGIL_OK) {
        fail("unit rules", "a unit holding another game's save too was refused");
    } else {
        mem_file *f = root_find(&root, MML2_CARD);
        size_t got = 0;
        uint8_t *placed = f ? extract_named(f->data, "BASLUS-00453", &got) : NULL;
        if (!f || placed) fail("unit rules", "another game's save inside the unit was placed");
        free(placed);
    }
    sigil_sync_result_free(r);
    root_free(&root);

    struct { const char *what; const uint8_t *unit; int rc; } REFUSED[] = {
        { "a unit with none of the game's saves", other, SIGIL_ERR_NOT_FOUND },
        { "a unit holding a broken save", card, SIGIL_ERR_UNSUPPORTED_FORMAT },
    };
    for (size_t i = 0; i < sizeof(REFUSED) / sizeof(REFUSED[0]); i++) {
        mem_root empty = {0};
        game h;
        make_game(&h, &empty, "pcsx_rearmed", MML2_CUE, "SLUS-01334", MEGAMAN, 1);
        r = NULL;
        if (sigil_restore(&h.req, REFUSED[i].unit, PS1_CARD_SIZE, &r) != REFUSED[i].rc || empty.writes != 0) {
            fail("unit rules", REFUSED[i].what);
        }
        sigil_sync_result_free(r);
        root_free(&empty);
    }

    mem_root src = {0};
    root_put(&src, MML2_CARD, card, len);
    game t;
    make_game(&t, &src, "pcsx_rearmed", MML2_CUE, "SLUS-01334", NULL, 0);
    r = NULL;
    size_t got = 0;
    uint8_t *found = NULL;
    if (sigil_collect(&t.req, &r) != SIGIL_OK || !r->data || !(found = extract_named(r->data, "BASLUS-01334", &got))) {
        fail("unit rules", "the title id alone didn't name the game's save");
    }
    free(found);
    sigil_sync_result_free(r);
    root_free(&src);
done:
    free(other);
    free(mixed);
    free(mml2);
    free(mml1);
    free(card);
}

/* A save that sat on the shared slot-2 card goes back to it; the game's own
 * card isn't given a copy. */
static void check_restore_to_slot_2(void) {
    size_t own_len = 0, shared_len = 0;
    uint8_t *own = sample("digimon-world-2-mcr", &own_len);
    uint8_t *shared = sample("ff-origins-mcr", &shared_len);
    if (!own || !shared) { free(own); free(shared); return; }
    mem_root root = {0};
    root_put(&root, "Final Fantasy Origins (USA).srm", own, own_len);
    root_put(&root, "pcsx-card2.mcd", shared, shared_len);
    game g;
    make_game(&g, &root, "pcsx_rearmed", "Final Fantasy Origins (USA).cue", "SLUS-01541", FF_ORIGINS, 1);
    sigil_sync_result *unit = NULL, *r = NULL;
    sigil_card_listing *l = NULL;
    if (sigil_collect(&g.req, &unit) != SIGIL_OK || !unit->data ||
        sigil_ps1_card_list(unit->data, SIGIL_CARD_FORMAT_PS1_RAW, &l) != SIGIL_OK || l->entry_count == 0) {
        fail("restore slot 2", "setup failed");
    } else {
        size_t mcs_len = 0;
        uint8_t *mcs = extract_named(unit->data, l->entries[0].name, &mcs_len);
        mcs[PS1_FRAME_SIZE + 100] ^= 0xFF;
        const uint8_t *one[1] = { mcs };
        uint8_t *newer = card_of(one, &mcs_len, 1);
        g.req.state = unit->state;
        g.req.state_len = unit->state_len;
        if (!newer || sigil_restore(&g.req, newer, PS1_CARD_SIZE, &r) != SIGIL_OK) {
            fail("restore slot 2", "restore failed");
        } else {
            mem_file *slot2 = root_find(&root, "pcsx-card2.mcd"), *mine = root_find(&root, "Final Fantasy Origins (USA).srm");
            size_t got = 0;
            uint8_t *on_own = extract_named(mine->data, l->entries[0].name, &got);
            if (sigil_ps1_verify(slot2->data, mcs, mcs_len) != SIGIL_OK) fail("restore slot 2", "the newer save isn't on the slot-2 card");
            if (on_own) fail("restore slot 2", "the game's own card was given a copy");
            free(on_own);
        }
        free(newer);
        free(mcs);
    }
    sigil_card_listing_free(l);
    sigil_sync_result_free(r);
    sigil_sync_result_free(unit);
    root_free(&root);
    free(own);
    free(shared);
}

/* A write that fails, or that reads back different from what went out, is
 * an I/O error, never success. */
static void check_faulty_writes(const sigil_sync_result *first) {
    if (!first) return;
    for (int corrupt = 0; corrupt < 2; corrupt++) {
        mem_root root = {0};
        if (corrupt) {
            root.corrupt_write = 1;
            root.corrupt_at = PS1_BLOCK_SIZE + 300;
        } else {
            root.fail_write = 1;
        }
        game g;
        make_game(&g, &root, "pcsx_rearmed", "Xenogears (USA) (Disc 1).cue", "SLUS-00664", XENOGEARS, 2);
        sigil_sync_result *r = NULL;
        if (sigil_restore(&g.req, first->data, first->len, &r) != SIGIL_ERR_IO) {
            fail("faulty writes", corrupt ? "a card that read back corrupted passed" : "a failed write passed");
        }
        sigil_sync_result_free(r);
        root_free(&root);
    }
}

#define BUGS_CUE  "Bugs Bunny - Lost in Time (Europe).cue"
#define BUGS_CARD "Bugs Bunny - Lost in Time (Europe).srm"
static const char *const BUGS[] = { "SLES-01726" };

/* The game's own save with a broken chain is damaged, not missing: collect
 * would otherwise read it as deleted, and restore can't put a save beside
 * one of the same name. Another game's broken save doesn't stop the sync. */
static void check_broken_own_save(void) {
    size_t len = 0;
    uint8_t *mcs = sample("bugs-bunny-mcs", &len);
    const uint8_t *one[] = { mcs };
    size_t lens[] = { len };
    uint8_t *card = mcs ? card_of(one, lens, 1) : NULL;
    uint8_t *unit = mcs ? card_of(one, lens, 1) : NULL;
    size_t digimon_len = 0;
    uint8_t *digimon = sample("digimon-world-2-mcr", &digimon_len);
    char digimon_id[SIGIL_CARD_OWNER_MAX] = "";
    sigil_card_listing *dl = NULL;
    if (digimon && sigil_ps1_card_list(digimon, SIGIL_CARD_FORMAT_PS1_RAW, &dl) == SIGIL_OK && dl->entry_count) {
        snprintf(digimon_id, sizeof(digimon_id), "%s", dl->entries[0].owner_id);
    }
    sigil_card_listing_free(dl);
    const char *const digimon_ids[] = { digimon_id };
    if (!card || !unit || !digimon_id[0]) {
        fail("broken own save", "setup failed");
        free(card); free(unit); free(digimon); free(mcs);
        return;
    }
    uint8_t *link = card + PS1_FRAME_SIZE + 8;
    link[0] = 9;
    link[1] = 0;
    static const char *const OTHER[] = { "SLUS-00001" };
    /* own: 2 the save is a companion's, 1 the game's, 0 another game's. */
    for (int own = 2; own >= 0; own--) {
        mem_root root = {0};
        root_put(&root, BUGS_CARD, card, PS1_CARD_SIZE);
        game g;
        const char *id = own == 2 ? digimon_id : own == 1 ? "SLES-01726" : "SLUS-00001";
        make_game(&g, &root, "pcsx_rearmed", BUGS_CUE, id, own == 2 ? digimon_ids : own == 1 ? BUGS : OTHER, 1);
        sigil_sync_companion bugs = { BUGS, 1, NULL, 0 };
        if (own == 2) {
            g.req.companions = &bugs;
            g.req.companion_count = 1;
        }
        sigil_sync_result *seen = NULL, *r = NULL;
        int collected = sigil_collect(&g.req, &seen);
        if (own == 2) bugs.unit = unit, bugs.unit_len = PS1_CARD_SIZE;
        int restored = own ? sigil_restore(&g.req, own == 2 ? digimon : unit, PS1_CARD_SIZE, &r) : SIGIL_ERR_DAMAGED;
        if (own && (collected != SIGIL_ERR_DAMAGED || strcmp(seen->problem, BUGS_CARD) != 0 ||
                    restored != SIGIL_ERR_DAMAGED || root.writes != 0)) {
            fail("broken own save", own == 1 ? "the game's broken save read as missing"
                                             : "a companion's broken save read as missing");
        }
        if (!own && collected != SIGIL_OK) fail("broken own save", "another game's broken save stopped the collect");
        sigil_sync_result_free(seen);
        sigil_sync_result_free(r);
        root_free(&root);
    }
    free(card);
    free(unit);
    free(digimon);
    free(mcs);
}

#define VMP_SIGNATURE_AT 0x20u

static size_t saves_on(const uint8_t *file, size_t len, sigil_ps1_file *out) {
    sigil_io *io = mem_root_io(file, len);
    sigil_card_listing *l = NULL;
    size_t n = SIZE_MAX;
    if (sigil_ps1_file_load(io, out) == SIGIL_OK && sigil_ps1_card_list(out->image, out->format, &l) == SIGIL_OK) {
        n = l->entry_count;
    }
    sigil_card_listing_free(l);
    sigil_io_close(io);
    return n;
}

/* A restore onto a DexDrive .gme or a PSP/Vita .vmp writes the card back in
 * its own form with the other games' saves kept; a .vmp whose signature
 * doesn't match its card is damaged until the client asks for a repair. */
#define CT_DIR "PSP/SAVEDATA/SLUS01363/"
static const char *const CT_FILES[] = { "CONFIG.BIN", "ICON0.PNG", "PARAM.SFO", "SCEVMC0.VMP", "SCEVMC1.VMP" };
static const char *const CHRONO_TRIGGER[] = { "SLUS-01363" };

/* A PS Vita's own PS1 save folder (Chrono Trigger): the console's signatures
 * read, and a restore that has to make the card anew writes the .vmp the
 * Vita wrote, byte for byte. Its seed is the one sigil gives a new card, so
 * the card, header and signature all compare. The Vita leaves the card's
 * write-test frame zero. */
static void check_vita_console_card(void) {
    mem_root vita = {0}, fresh = {0};
    uint8_t *files[5] = { NULL };
    size_t lens[5] = { 0 };
    bool loaded = true;
    for (size_t i = 0; i < 5; i++) {
        char path[128];
        snprintf(path, sizeof(path), CT_DIR "%s", CT_FILES[i]);
        files[i] = corpus_sample(&g_manifest, "psx", "chrono-trigger-vita-pops", path, &lens[i]);
        loaded = loaded && files[i];
        if (!files[i]) continue;
        root_put(&vita, path, files[i], lens[i]);
        if (i != 3) root_put(&fresh, path, files[i], lens[i]);
    }
    sigil_sync_result *unit = NULL, *r = NULL;
    game g;
    if (!loaded) { fail("vita console card", "setup failed"); goto done; }
    make_game(&g, &vita, "vita_pops", "Chrono Trigger.cue", "SLUS-01363", CHRONO_TRIGGER, 1);
    if (sigil_collect(&g.req, &unit) != SIGIL_OK || !unit->data || strcmp(unit->artifact, "Chrono Trigger.mcr") != 0) {
        fail("vita console card", "the Vita's signed card didn't collect");
        goto done;
    }
    make_game(&g, &fresh, "vita_pops", "Chrono Trigger.cue", "SLUS-01363", CHRONO_TRIGGER, 1);
    mem_file *made = NULL;
    if (sigil_restore(&g.req, unit->data, unit->len, &r) != SIGIL_OK ||
        !(made = root_find(&fresh, CT_DIR "SCEVMC0.VMP")) || made->len != lens[3] ||
        memcmp(made->data, files[3], lens[3]) != 0) {
        fail("vita console card", "a new .vmp differs from the one the Vita wrote");
    }
    mem_file *kept = root_find(&fresh, CT_DIR "PARAM.SFO");
    if (!kept || kept->len != lens[2] || memcmp(kept->data, files[2], lens[2]) != 0) {
        fail("vita console card", "restore rewrote the PARAM.SFO the Vita wrote");
    }
done:
    sigil_sync_result_free(r);
    sigil_sync_result_free(unit);
    root_free(&vita);
    root_free(&fresh);
    for (size_t i = 0; i < 5; i++) free(files[i]);
}

/* PARAM.SFO as a PS Vita's POPS writes it: sigil's signer gives back the
 * console's hashes at 0x10 and 0x70 on both real files, and sigil's file
 * for Xenogears matches the Vita's byte for byte outside the file list and
 * the hash at 0x20, which needs the console's own key. */
static void check_pops_param_sfo(void) {
    static const struct { const char *id, *path, *directory, *title; } REAL[] = {
        { "vita-pops-empty", "PSP/SAVEDATA/SLUS00664/PARAM.SFO", "SLUS00664", "Xenogears\xC2\xAE" },
        { "chrono-trigger-vita-pops", "PSP/SAVEDATA/SLUS01363/PARAM.SFO", "SLUS01363", "CHRONO TRIGGER\xC2\xAE" },
    };
    for (size_t i = 0; i < sizeof(REAL) / sizeof(REAL[0]); i++) {
        size_t len = 0, params = 0, params_len = 0, list = 0, list_len = 0;
        uint8_t *real = corpus_sample(&g_manifest, "psx", REAL[i].id, REAL[i].path, &len);
        uint8_t *copy = real ? (uint8_t *)malloc(len) : NULL;
        if (!copy || len != SIGIL_POPS_SFO_SIZE ||
            sigil_sfo_find(real, len, "SAVEDATA_PARAMS", &params, &params_len) != SIGIL_OK ||
            sigil_sfo_find(real, len, "SAVEDATA_FILE_LIST", &list, &list_len) != SIGIL_OK) {
            fail("pops param.sfo", "setup failed");
            free(real);
            free(copy);
            continue;
        }
        memcpy(copy, real, len);
        memset(copy + params + 0x10, 0, 16);
        memset(copy + params + 0x70, 0, 16);
        if (sigil_psp_sfo_sign(copy, len) != SIGIL_OK || memcmp(copy, real, len) != 0) {
            fail("pops param.sfo", "re-signing a Vita's PARAM.SFO didn't give back its hashes");
        }
        uint8_t built[SIGIL_POPS_SFO_SIZE];
        bool same = sigil_pops_param_sfo(REAL[i].directory, REAL[i].title, built) == SIGIL_OK;
        for (size_t at = 0; same && at < len; at++) {
            bool in_list = at >= list && at < list + list_len;
            bool in_params = at >= params && at < params + params_len;
            same = in_list || in_params || built[at] == real[at];
        }
        same = same && built[params] == real[params];
        memcpy(copy, built, len);
        same = same && sigil_psp_sfo_sign(copy, len) == SIGIL_OK && memcmp(copy, built, len) == 0;
        if (!same) fail("pops param.sfo", "a built PARAM.SFO differs from the Vita's outside the file list and the console's hash");
        free(real);
        free(copy);
    }
}

static void check_wrapped_cards(void) {
    size_t mcs_len = 0;
    uint8_t *mcs = sample("bugs-bunny-mcs", &mcs_len);
    const uint8_t *one[] = { mcs };
    size_t lens[] = { mcs_len };
    uint8_t *unit = mcs ? card_of(one, lens, 1) : NULL;
    sigil_ps1_file *before = (sigil_ps1_file *)malloc(sizeof(*before));
    sigil_ps1_file *after = (sigil_ps1_file *)malloc(sizeof(*after));
    static const char *const IDS[] = { "gran-turismo-gme", "vagrant-story-vmp" };
    for (size_t i = 0; i < 2 && unit; i++) {
        size_t len = 0;
        uint8_t *file = sample(IDS[i], &len);
        if (!file) continue;
        mem_root root = {0};
        root_put(&root, BUGS_CARD, file, len);
        game g;
        make_game(&g, &root, "pcsx_rearmed", BUGS_CUE, "SLES-01726", BUGS, 1);
        sigil_sync_result *r = NULL;
        mem_file *f = NULL;
        size_t had = saves_on(file, len, before);
        if (sigil_restore(&g.req, unit, PS1_CARD_SIZE, &r) != SIGIL_OK || !(f = root_find(&root, BUGS_CARD))) {
            fail(IDS[i], "restore onto the wrapped card failed");
        } else if (saves_on(f->data, f->len, after) != had + 1 || after->format != before->format ||
                   memcmp(after->header, before->header, 0x0C) != 0 || sigil_ps1_file_check(after) != SIGIL_OK) {
            fail(IDS[i], "the card didn't keep its form and other saves, or its signature doesn't check");
        }
        sigil_sync_result_free(r);
        root_free(&root);
        free(file);
    }

    size_t len = 0;
    uint8_t *vmp = sample("vagrant-story-vmp", &len);
    if (vmp && unit) {
        vmp[PS1_VMP_HEADER_SIZE + PS1_BLOCK_SIZE + 0x100] ^= 0xFF;
        mem_root root = {0};
        root_put(&root, BUGS_CARD, vmp, len);
        game g;
        make_game(&g, &root, "pcsx_rearmed", BUGS_CUE, "SLES-01726", BUGS, 1);
        sigil_sync_result *seen = NULL, *r = NULL;
        mem_file *f = NULL;
        if (sigil_collect(&g.req, &seen) != SIGIL_ERR_DAMAGED || !seen || strcmp(seen->problem, BUGS_CARD) != 0) {
            fail("damaged vmp", "collect didn't refuse naming the card");
        }
        if (sigil_restore(&g.req, unit, PS1_CARD_SIZE, &r) != SIGIL_ERR_DAMAGED || root.writes != 0) {
            fail("damaged vmp", "restore wrote over a damaged card without a repair");
        }
        sigil_sync_result_free(seen);
        sigil_sync_result_free(r);
        seen = r = NULL;
        g.req.repair = 1;
        if (sigil_collect(&g.req, &seen) != SIGIL_OK) fail("damaged vmp", "collect with repair refused");
        if (sigil_restore(&g.req, unit, PS1_CARD_SIZE, &r) != SIGIL_OK || !(f = root_find(&root, BUGS_CARD)) ||
            saves_on(f->data, f->len, after) == SIZE_MAX || sigil_ps1_file_check(after) != SIGIL_OK) {
            fail("damaged vmp", "restore with repair didn't sign the card anew");
        }
        sigil_sync_result_free(seen);
        sigil_sync_result_free(r);
        root_free(&root);
    }

    /* A game a PSP or Vita hasn't run yet gets a signed .vmp, never a raw
     * card, and the PARAM.SFO POPS needs to read it: the folder's name, the
     * content's name as its title, signed. */
    if (unit) {
        mem_root root = {0};
        game g;
        make_game(&g, &root, "vita_pops", BUGS_CUE, "SLES-01726", BUGS, 1);
        sigil_sync_result *r = NULL;
        mem_file *f = NULL, *sfo = NULL;
        char dir[32] = "", title[64] = "";
        uint8_t resigned[SIGIL_POPS_SFO_SIZE];
        int restored = sigil_restore(&g.req, unit, PS1_CARD_SIZE, &r);
        sfo = root_find(&root, "PSP/SAVEDATA/SLES01726/PARAM.SFO");
        bool signed_sfo = sfo && sfo->len == SIGIL_POPS_SFO_SIZE;
        if (signed_sfo) {
            memcpy(resigned, sfo->data, sfo->len);
            signed_sfo = sigil_psp_sfo_sign(resigned, sfo->len) == SIGIL_OK && memcmp(resigned, sfo->data, sfo->len) == 0 &&
                         sigil_sfo_get_string(sfo->data, sfo->len, "SAVEDATA_DIRECTORY", dir, sizeof(dir)) == SIGIL_OK &&
                         sigil_sfo_get_string(sfo->data, sfo->len, "TITLE", title, sizeof(title)) == SIGIL_OK &&
                         strcmp(dir, "SLES01726") == 0 && strcmp(title, "Bugs Bunny - Lost in Time (Europe)") == 0;
        }
        if (restored != SIGIL_OK || root.count != 2 || !signed_sfo) {
            fail("vita pops", "a first restore didn't write POPS's PARAM.SFO, named and signed, beside the card");
        }
        if (!(f = root_find(&root, "PSP/SAVEDATA/SLES01726/SCEVMC0.VMP")) || f->len != PS1_VMP_HEADER_SIZE + PS1_CARD_SIZE ||
            saves_on(f->data, f->len, after) != 1 || after->format != SIGIL_CARD_FORMAT_PS1_VMP ||
            sigil_ps1_file_check(after) != SIGIL_OK) {
            fail("vita pops", "a new card isn't a signed .vmp in the game's SAVEDATA folder");
        } else if (vmp && memcmp(f->data, vmp, VMP_SIGNATURE_AT) != 0) {
            fail("vita pops", "a new .vmp's header differs from the one a console writes");
        }
        sigil_sync_result_free(r);
        r = NULL;

        /* Collect hands back the raw card, named for one. */
        sigil_sync_result *seen = NULL;
        refresh_listing(&g, &root);
        if (sigil_collect(&g.req, &seen) != SIGIL_OK || !seen->data || seen->len != PS1_CARD_SIZE ||
            memcmp(seen->data, "MC", 2) != 0 || strcmp(seen->artifact, "Bugs Bunny - Lost in Time (Europe).mcr") != 0) {
            fail("vita pops", "collect didn't hand back a raw card named as one");
        }

        /* A damaged card already holding the unit's saves: repair writes it
         * again signed, so the next collect reads it without a repair. */
        if (f && seen) {
            f = root_find(&root, "PSP/SAVEDATA/SLES01726/SCEVMC0.VMP");
            f->data[PS1_VMP_HEADER_SIZE + 15 * PS1_BLOCK_SIZE + 0x100] ^= 0xFF;
            g.req.state = seen->state;
            g.req.state_len = seen->state_len;
            g.req.repair = 1;
            sigil_sync_result *again = NULL;
            int restored = sigil_restore(&g.req, unit, PS1_CARD_SIZE, &r);
            g.req.repair = 0;
            if (restored != SIGIL_OK || sigil_collect(&g.req, &again) != SIGIL_OK) {
                fail("vita pops", "a repair restore of the saves already there left the card damaged");
            }
            sigil_sync_result_free(again);
        }
        sigil_sync_result_free(seen);
        sigil_sync_result_free(r);
        r = NULL;
        root_free(&root);

        /* A card whose signature reads back wrong fails the restore. */
        root.corrupt_write = 1;
        root.corrupt_at = VMP_SIGNATURE_AT;
        make_game(&g, &root, "vita_pops", BUGS_CUE, "SLES-01726", BUGS, 1);
        if (sigil_restore(&g.req, unit, PS1_CARD_SIZE, &r) != SIGIL_ERR_IO) {
            fail("vita pops", "a .vmp whose signature read back wrong passed the restore");
        }
        sigil_sync_result_free(r);
        root_free(&root);
    }
    free(vmp);
    free(after);
    free(before);
    free(unit);
    free(mcs);
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
    check_conflict_rules();
    check_unit_rules();
    check_restore_to_slot_2();
    check_faulty_writes(first);
    check_wrapped_cards();
    check_broken_own_save();
    check_vita_console_card();
    check_pops_param_sfo();
    sigil_sync_result_free(first);

    corpus_free(&g_manifest);
    corpus_free(&g_entries);
    printf("ps1 sync: %d failures\n", g_fails);
    return corpus_exit(g_fails);
}
