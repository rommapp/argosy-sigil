// SPDX-License-Identifier: MPL-2.0
/* sigil_collect and sigil_restore: the path each platform takes, the
 * conflict rule, and the state each call leaves. */
#include "sync_internal.h"

static sigil_sync_result *new_result(void) {
    sigil_sync_result *r = (sigil_sync_result *)calloc(1, sizeof(*r));
    if (r) r->struct_version = SIGIL_SYNC_RESULT_V1;
    return r;
}

/* Gives the caller the result on success and with each refusal that reports
 * through it (a conflict, the save or file at fault); frees it otherwise. */
static int hand_back(sigil_sync_result *r, int rc, sigil_sync_result **out) {
    bool reports = rc == SIGIL_OK || rc == SIGIL_ERR_CONFLICT || rc == SIGIL_ERR_NO_SPACE || rc == SIGIL_ERR_REGION ||
                   rc == SIGIL_ERR_DAMAGED || rc == SIGIL_ERR_NO_TARGET || rc == SIGIL_ERR_AMBIGUOUS;
    if (reports && r) *out = r;
    else sigil_sync_result_free(r);
    return rc;
}

static bool request_valid(const sigil_sync_request *req) {
    return req && req->struct_version == SIGIL_SYNC_REQUEST_V1 && req->save.open && req->save.content_path &&
           req->save.listing;
}

/* ---- collect ------------------------------------------------------------------ */

/* A view of `saves` holding companion `c`'s alone, relabelled as the game's
 * so the unit builders take them. It shares the saves; free only `items`. */
static int companion_view(const sigil_sync_saves *saves, size_t c, sigil_sync_saves *view) {
    sigil_sync_saves_init(view, saves->kind);
    view->items = (sigil_sync_save *)calloc(saves->count ? saves->count : 1, sizeof(sigil_sync_save));
    if (!view->items) return SIGIL_ERR_OOM;
    for (size_t i = 0; i < saves->count; i++) {
        const sigil_sync_save *o = &saves->items[i];
        if (o->owner != SYNC_OWN_COMPANION || o->companion != c) continue;
        view->items[view->count] = *o;
        view->items[view->count].owner = SYNC_OWN_GAME;
        view->count++;
    }
    view->cap = saves->count;
    return SIGIL_OK;
}

/* Each companion's saves found with the game's, as the companion's unit, and
 * the state's record of what each companion last synced. */
static int collect_companions(sigil_sync_ctx *x, const sigil_sync_saves *saves, const sigil_sync_sources *src,
                              sigil_sync_result *r) {
    size_t n = x->req->companion_count;
    if (!n) return SIGIL_OK;
    r->companions = (sigil_sync_companion_result *)calloc(n, sizeof(*r->companions));
    if (!r->companions) return SIGIL_ERR_OOM;
    r->companion_count = n;
    int rc = SIGIL_OK;
    for (size_t c = 0; c < n && rc == SIGIL_OK; c++) {
        sigil_sync_companion_result *out = &r->companions[c];
        sigil_sync_saves view;
        rc = companion_view(saves, c, &view);
        char artifact[SIGIL_SAVE_ENTRY_MAX];
        int shape = 0;
        if (rc == SIGIL_OK) {
            rc = sigil_sync_build_unit(&view, SYNC_OWN_GAME, src, false, "companion", &out->data, &out->len, &shape,
                                       artifact, out->content_hash);
        }
        if (rc == SIGIL_OK && out->data) rc = sigil_sync_identity_of(&view, out->identity_hash);
        free(view.items);
        const char *last = sigil_sync_state_get(&x->state, "synced", x->companion_keys[c]);
        out->changed = strcmp(last ? last : "", out->identity_hash) != 0;
        if (rc == SIGIL_OK && out->identity_hash[0]) {
            rc = sigil_sync_state_put(&x->state, "synced", x->companion_keys[c], out->identity_hash);
        }
    }
    return rc;
}

/* A card unit keeps the artifact name the card file gave it; other units
 * are named for their volume or file, or the content. */
int sigil_sync_finish_collect(sigil_sync_ctx *x, const sigil_sync_saves *saves, const sigil_sync_sources *src,
                              sigil_sync_result *r) {
    char stem[SIGIL_SAVE_ENTRY_MAX], unused[SIGIL_SAVE_ENTRY_MAX];
    sigil_content_stem(x->req->save.content_path, stem, sizeof(stem));
    char *artifact = x->kind->has_ids && !x->kind->save_files ? unused : r->artifact;
    int rc = sigil_sync_build_unit(saves, SYNC_OWN_GAME, src, false, stem, &r->data, &r->len, &r->shape, artifact,
                                   r->content_hash);
    if (rc == SIGIL_OK && r->data) rc = sigil_sync_identity_of(saves, r->identity_hash);
    if (rc == SIGIL_OK) rc = collect_companions(x, saves, src, r);
    return rc;
}

/* The game's cards and the saves of the game and its companions on them,
 * naming in the result a damaged file they refused on. */
static int gather_cards(sigil_sync_ctx *x, sigil_sync_cards *cards, sigil_sync_saves *saves, sigil_sync_result *r) {
    int rc = sigil_sync_gather_cards(x, cards, r->artifact);
    if (rc == SIGIL_OK) {
        rc = sigil_sync_gather_card_saves(x, cards, saves);
        if (rc != SIGIL_OK) sigil_sync_cards_free(cards);
    }
    if (rc == SIGIL_ERR_DAMAGED || rc == SIGIL_ERR_AMBIGUOUS) snprintf(r->problem, sizeof(r->problem), "%s", cards->problem);
    return rc;
}

static int collect_cards(sigil_sync_ctx *x, sigil_sync_result *r) {
    sigil_sync_cards cards;
    sigil_sync_saves saves;
    int rc = gather_cards(x, &cards, &saves, r);
    if (rc != SIGIL_OK) return rc;
    sigil_sync_cards_free(&cards);
    rc = sigil_sync_finish_collect(x, &saves, NULL, r);
    sigil_sync_saves_free(&saves);
    return rc;
}

static int collect_folder(sigil_sync_ctx *x, const char *folder, sigil_sync_result *r) {
    sigil_sync_saves saves;
    sigil_sync_paths stale;
    int rc = sigil_sync_folder_saves(x, folder, &saves, &stale);
    if (rc != SIGIL_OK) return rc;
    rc = sigil_sync_finish_collect(x, &saves, NULL, r);
    sigil_sync_saves_free(&saves);
    free(stale.paths);
    return rc;
}

static int collect_any(sigil_sync_ctx *x, sigil_sync_result *r) {
    if (!x->kind->has_ids) return sigil_sync_collect_volumes(x, r);
    if (x->kind->save_files) {
        char folder[SIGIL_SAVE_PATH_MAX];
        int rc = sigil_sync_save_folder_of(x, folder);
        if (rc != SIGIL_OK) return rc;
        if (folder[0]) return collect_folder(x, folder, r);
    }
    return collect_cards(x, r);
}

/* Sets `changed` and the new state. After an unmanaged restore, a collect
 * that finds the saves the restore replaced asks for the restore again. */
static int finish(sigil_sync_ctx *x, sigil_sync_result *r, bool collecting) {
    const char *last = sigil_sync_state_get(&x->state, "synced", x->game);
    r->changed = strcmp(last ? last : "", r->identity_hash) != 0;
    int rc = SIGIL_OK;
    const char *restored = collecting ? sigil_sync_state_get(&x->state, "restored", x->game) : NULL;
    if (restored) {
        const char *tab = strchr(restored, '\t');
        size_t new_len = tab ? (size_t)(tab - restored) : strlen(restored);
        const char *old = tab ? tab + 1 : "";
        bool survived = new_len == strlen(r->identity_hash) && strncmp(restored, r->identity_hash, new_len) == 0;
        if (!survived && strcmp(old, r->identity_hash) == 0) {
            r->restore_again = 1;
            r->changed = 0;
        } else {
            rc = sigil_sync_state_put(&x->state, "restored", x->game, NULL);
        }
    }
    if (rc == SIGIL_OK && !r->restore_again) {
        rc = sigil_sync_state_put(&x->state, "synced", x->game, r->identity_hash[0] ? r->identity_hash : NULL);
    }
    if (rc == SIGIL_OK) rc = sigil_sync_state_blob(&x->state, &r->state, &r->state_len);
    return rc;
}

int sigil_collect(const sigil_sync_request *req, sigil_sync_result **out) {
    if (!out) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    if (!request_valid(req)) return SIGIL_ERR_INVALID_ARG;
    const sigil_sync_kind *kind = sigil_sync_kind_for(req);
    if (!kind) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    sigil_sync_ctx x;
    int rc = sigil_sync_ctx_open(&x, req, kind);
    sigil_sync_result *r = rc == SIGIL_OK ? new_result() : NULL;
    if (rc == SIGIL_OK && !r) rc = SIGIL_ERR_OOM;
    if (rc == SIGIL_OK) rc = collect_any(&x, r);
    if (rc == SIGIL_OK) rc = finish(&x, r, true);
    sigil_sync_ctx_close(&x);
    return hand_back(r, rc, out);
}

/* ---- restore ------------------------------------------------------------------ */

/* Saves on disk that changed since the last sync (`last`) stop a restore
 * that would replace them, unless the user chose to overwrite them. `same`
 * is true when they already are what the restore brings. */
static bool blocks_restore(const sigil_sync_ctx *x, const char *local, const char *incoming, const char *last,
                           bool *same) {
    *same = strcmp(local, incoming) == 0;
    bool changed = local[0] && strcmp(local, last ? last : "") != 0;
    return !*same && changed && !x->req->overwrite_local;
}

int sigil_sync_check_restore(const sigil_sync_ctx *x, const sigil_sync_saves *local, const sigil_sync_saves *incoming,
                             sigil_sync_result *r, char local_identity[33], bool *already_there) {
    int rc = sigil_sync_identity_or_empty(local, SYNC_OWN_GAME, NULL, local_identity);
    bool blocked = rc == SIGIL_OK &&
                   blocks_restore(x, local_identity, r->identity_hash,
                                  sigil_sync_state_get(&x->state, "synced", x->game), already_there);
    for (size_t c = 0; c < x->req->companion_count && rc == SIGIL_OK; c++) {
        if (!sigil_sync_companion_restored(x, c)) continue;
        const char *key = x->companion_keys[c];
        char mine[33], theirs[33];
        rc = sigil_sync_identity_or_empty(local, SYNC_OWN_COMPANION, key, mine);
        if (rc == SIGIL_OK) rc = sigil_sync_identity_or_empty(incoming, SYNC_OWN_COMPANION, key, theirs);
        if (rc != SIGIL_OK) break;
        bool same = false;
        blocked = blocks_restore(x, mine, theirs, sigil_sync_state_get(&x->state, "synced", key), &same) || blocked;
        *already_there = *already_there && same;
    }
    if (rc == SIGIL_OK && blocked) {
        r->conflict = 1;
        rc = SIGIL_ERR_CONFLICT;
    }
    return rc;
}

/* The state records what each companion now holds, as the restore put it. */
static int note_companions(sigil_sync_ctx *x, const sigil_sync_saves *incoming) {
    int rc = SIGIL_OK;
    for (size_t c = 0; c < x->req->companion_count && rc == SIGIL_OK; c++) {
        if (!sigil_sync_companion_restored(x, c)) continue;
        char theirs[33];
        rc = sigil_sync_identity_or_empty(incoming, SYNC_OWN_COMPANION, x->companion_keys[c], theirs);
        if (rc == SIGIL_OK) rc = sigil_sync_state_put(&x->state, "synced", x->companion_keys[c], theirs[0] ? theirs : NULL);
    }
    return rc;
}

static int restore_cards(sigil_sync_ctx *x, sigil_sync_saves *incoming, sigil_sync_result *r,
                         char local_identity[33]) {
    sigil_sync_cards cards;
    sigil_sync_saves local;
    int rc = gather_cards(x, &cards, &local, r);
    if (rc != SIGIL_OK) return rc;
    bool already_there = false;
    rc = sigil_sync_check_restore(x, &local, incoming, r, local_identity, &already_there);
    sigil_sync_saves_free(&local);
    bool repairing = false;
    for (size_t c = 0; c < cards.count; c++) repairing = repairing || cards.files[c].changed;
    if (rc == SIGIL_OK && (!already_there || repairing)) rc = sigil_sync_place_on_cards(x, &cards, incoming, r);
    sigil_sync_cards_free(&cards);
    return rc;
}

static int restore_folder(sigil_sync_ctx *x, const char *folder, sigil_sync_saves *incoming, sigil_sync_result *r,
                          char local_identity[33]) {
    sigil_sync_saves local;
    sigil_sync_paths stale;
    int rc = sigil_sync_folder_saves(x, folder, &local, &stale);
    if (rc != SIGIL_OK) return rc;
    bool already_there = false;
    rc = sigil_sync_check_restore(x, &local, incoming, r, local_identity, &already_there);
    if (rc == SIGIL_OK && !already_there) rc = sigil_sync_place_in_folder(x, folder, &local, &stale, incoming, r);
    sigil_sync_saves_free(&local);
    free(stale.paths);
    return rc;
}

static int restore_with_ids(sigil_sync_ctx *x, sigil_sync_saves *incoming, sigil_sync_result *r,
                            char local_identity[33]) {
    if (x->kind->save_files) {
        char folder[SIGIL_SAVE_PATH_MAX];
        int rc = sigil_sync_save_folder_of(x, folder);
        if (rc != SIGIL_OK) return rc;
        if (folder[0]) return restore_folder(x, folder, incoming, r, local_identity);
    }
    return restore_cards(x, incoming, r, local_identity);
}

/* The name the restored unit travels under, as collect would give it. */
static void restored_artifact(const sigil_sync_ctx *x, const sigil_sync_saves *incoming, bool zip,
                              sigil_sync_result *r) {
    const sigil_sync_kind *kind = x->kind;
    if (kind->has_ids && !kind->save_files) return;
    char stem[SIGIL_SAVE_ENTRY_MAX];
    sigil_content_stem(x->req->save.content_path, stem, sizeof(stem));
    if (zip) snprintf(r->artifact, sizeof(r->artifact), "%s.zip", stem);
    else if (kind->save_files) kind->file_name(incoming->items[0].save, r->artifact, sizeof(r->artifact));
    else snprintf(r->artifact, sizeof(r->artifact), "%s", sigil_sync_device_name(incoming->items[0].device));
}

int sigil_restore(const sigil_sync_request *req, const uint8_t *unit, size_t unit_len, sigil_sync_result **out) {
    if (!out) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    if (!request_valid(req) || !req->write || !unit) return SIGIL_ERR_INVALID_ARG;
    const sigil_sync_kind *kind = sigil_sync_kind_for(req);
    if (!kind) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    sigil_sync_ctx x;
    int rc = sigil_sync_ctx_open(&x, req, kind);
    sigil_sync_result *r = rc == SIGIL_OK ? new_result() : NULL;
    if (rc == SIGIL_OK && !r) rc = SIGIL_ERR_OOM;

    sigil_sync_saves incoming;
    sigil_sync_saves_init(&incoming, kind);
    size_t sizes[SIGIL_DEVICE_COUNT];
    char local_identity[33] = "";
    if (rc == SIGIL_OK) rc = sigil_sync_request_saves(&x, unit, unit_len, &incoming, sizes, r);
    if (rc == SIGIL_OK) rc = sigil_sync_identity_of(&incoming, r->identity_hash);
    if (rc == SIGIL_OK) {
        sigil_md5_of(unit, unit_len, r->content_hash);
        bool zip = (!kind->has_ids || kind->save_files) && sigil_read_le32(unit) == 0x04034b50u;
        r->shape = zip ? SIGIL_SAVE_SHAPE_MULTI : SIGIL_SAVE_SHAPE_SINGLE;
        rc = kind->has_ids ? restore_with_ids(&x, &incoming, r, local_identity)
                           : sigil_sync_restore_volumes(&x, &incoming, sizes, r, local_identity);
        restored_artifact(&x, &incoming, zip, r);
    }
    if (rc == SIGIL_OK) rc = note_companions(&x, &incoming);
    sigil_sync_saves_free(&incoming);
    if (rc == SIGIL_OK && req->mode == SIGIL_SYNC_UNMANAGED) {
        char both[80];
        snprintf(both, sizeof(both), "%s\t%s", r->identity_hash, local_identity);
        rc = sigil_sync_state_put(&x.state, "restored", x.game, both);
    }
    if (rc == SIGIL_OK) rc = finish(&x, r, false);
    if (rc == SIGIL_OK) r->changed = 0;
    sigil_sync_ctx_close(&x);
    return hand_back(r, rc, out);
}

void sigil_sync_result_free(sigil_sync_result *result) {
    if (!result) return;
    free(result->data);
    free(result->state);
    free(result->holding);
    free(result->unowned);
    for (size_t c = 0; result->companions && c < result->companion_count; c++) free(result->companions[c].data);
    free(result->companions);
    free(result);
}
