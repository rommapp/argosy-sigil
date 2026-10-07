// SPDX-License-Identifier: MPL-2.0
/* Volume platforms (Saturn, Sega CD, Dreamcast): the game's volumes, who
 * each save on them belongs to, and placing a unit's saves on them by swap
 * (managed) or inject (unmanaged). */
#include "sync_internal.h"

typedef struct {
    sigil_volume_target target;
    char                key[SYNC_KEY_MAX];   /* escaped state key of the volume */
    void               *card;                /* NULL when the file doesn't exist yet */
    int                 format;
} volume_file;

typedef struct {
    const sigil_sync_kind *kind;
    volume_file            files[SIGIL_VOLUME_TARGETS_MAX];
    size_t                 count;
} volume_set;

static void volume_set_free(volume_set *s) {
    for (size_t i = 0; i < s->count; i++) {
        if (s->files[i].card) s->kind->free_card(s->files[i].card);
    }
    s->count = 0;
}

/* The game's volume files, loaded. A file there that doesn't read as its
 * volume is SIGIL_ERR_DAMAGED, naming it in r->problem: sigil never formats
 * over saves it can't read, so repair doesn't change that. An empty file is
 * no volume yet. */
static int gather_volumes(const sigil_sync_ctx *x, volume_set *s, sigil_sync_result *r) {
    memset(s, 0, sizeof(*s));
    s->kind = x->kind;
    sigil_volume_target targets[SIGIL_VOLUME_TARGETS_MAX];
    size_t count = 0;
    int rc = sigil_save_volume_targets(&x->req->save, targets, &count);
    for (size_t i = 0; i < count && rc == SIGIL_OK; i++) {
        volume_file *f = &s->files[s->count++];
        f->target = targets[i];
        char raw[SYNC_KEY_MAX];
        snprintf(raw, sizeof(raw), "%s/%s/%s", x->kind->platform, x->req->save.layout ? x->req->save.layout : "",
                 f->target.path);
        sigil_sync_escape(raw, f->key, sizeof(f->key));
        sigil_io *io = sigil_save_open(x->req->save.open, x->req->save.open_ctx, f->target.path);
        if (!io) {
            if (sigil_sync_listed(x->req, f->target.path)) rc = SIGIL_ERR_IO;
            continue;
        }
        int64_t size = io->size ? io->size(io->ctx) : -1;
        if (size != 0) rc = x->kind->load(io, f->target.device, &f->card, &f->format);
        sigil_io_close(io);
        if (rc == SIGIL_ERR_UNSUPPORTED_FORMAT) {
            snprintf(r->problem, sizeof(r->problem), "%s", f->target.path);
            rc = SIGIL_ERR_DAMAGED;
        }
    }
    if (rc != SIGIL_OK) volume_set_free(s);
    return rc;
}

/* The save-name table gives the game this save by one of its ids. */
/* The save-name table gives `name` to the game with `title_id` (or NULL) and `game_ids`. */
static bool named_for(const sigil_sync_ctx *x, const char *name, const char *title_id, const char *const *game_ids,
                      size_t count) {
    const char *ids[1 + 64];
    size_t n = 0;
    if (title_id && title_id[0]) ids[n++] = title_id;
    for (size_t i = 0; i < count && n < sizeof(ids) / sizeof(ids[0]); i++) ids[n++] = game_ids[i];
    return n > 0 && sigil_save_names_match(sigil_save_name_table, sigil_save_name_table_count, x->kind->platform, name,
                                           ids, n);
}

static bool in_name_table(const sigil_sync_ctx *x, const char *name) {
    const sigil_result *game = x->req->save.result;
    return named_for(x, name, game ? game->title_id : NULL, x->req->game_ids, x->req->game_id_count);
}

/* The request companion the save-name table gives `name` to, or SIZE_MAX. */
static size_t named_companion(const sigil_sync_ctx *x, const char *name) {
    for (size_t c = 0; c < x->req->companion_count; c++) {
        const sigil_sync_companion *k = &x->req->companions[c];
        if (named_for(x, name, NULL, k->game_ids, k->game_id_count)) return c;
    }
    return SIZE_MAX;
}

/* Who a save on volume file `f` belongs to: the user's claim, then the owner
 * the state learned (on a per-game file only a companion's counts), then
 * the per-game file's game, then in managed mode the game a shared volume
 * was swapped in for, then the save-name table, the game's ids before each
 * companion's. */
static int volume_owner(const sigil_sync_ctx *x, const volume_file *f, const char *name, char other[SYNC_KEY_MAX],
                        size_t *companion) {
    other[0] = '\0';
    *companion = SIZE_MAX;
    if (sigil_sync_claimed(x->req, name)) return SYNC_OWN_GAME;
    const char *learned = sigil_sync_owner_get(&x->state, f->key, name);
    if (learned) {
        if (strcmp(learned, x->game) == 0) return SYNC_OWN_GAME;
        size_t c = sigil_sync_companion_by_key(x, learned);
        if (c != SIZE_MAX || !f->target.per_game) {
            snprintf(other, SYNC_KEY_MAX, "%s", learned);
            *companion = c;
            return c != SIZE_MAX ? SYNC_OWN_COMPANION : SYNC_OWN_OTHER;
        }
    }
    if (f->target.per_game) return SYNC_OWN_GAME;
    const char *prepared = sigil_sync_state_get(&x->state, "prepared", f->key);
    if (x->req->mode == SIGIL_SYNC_MANAGED && prepared && strcmp(prepared, x->game) == 0) return SYNC_OWN_GAME;
    if (in_name_table(x, name)) return SYNC_OWN_GAME;
    size_t c = named_companion(x, name);
    if (c == SIZE_MAX) return SYNC_OWN_NONE;
    snprintf(other, SYNC_KEY_MAX, "%s", x->companion_keys[c]);
    *companion = c;
    return SYNC_OWN_COMPANION;
}

/* A corrupt save on `f` that may be the game's or a companion's: one the
 * volume no longer names, or one whose name the owner rules give the game
 * or a companion (on a per-game volume, every name). Reading on would take
 * it as deleted. */
static int corrupt_own_save(const sigil_sync_ctx *x, const volume_file *f, bool *found) {
    sigil_card_listing *listing = NULL;
    int rc = x->kind->list(f->card, f->format, &listing);
    if (rc != SIGIL_OK) return rc == SIGIL_ERR_OOM ? rc : SIGIL_ERR_DAMAGED;
    *found = listing->corrupt_count > listing->corrupt_entry_count;
    char other[SYNC_KEY_MAX];
    for (size_t i = 0; i < listing->corrupt_entry_count && !*found; i++) {
        size_t c = SIZE_MAX;
        int owner = volume_owner(x, f, listing->corrupt_entries[i].name, other, &c);
        *found = owner == SYNC_OWN_GAME || owner == SYNC_OWN_COMPANION;
    }
    sigil_card_listing_free(listing);
    return SIGIL_OK;
}

/* Every save on the game's volumes, with its owner. A volume holding a
 * corrupt or unreadable save of the game or a companion is SIGIL_ERR_DAMAGED
 * naming the volume. */
static int gather_volume_saves(const sigil_sync_ctx *x, const volume_set *s, sigil_sync_saves *out,
                               sigil_sync_result *r) {
    sigil_sync_saves_init(out, s->kind);
    int rc = SIGIL_OK;
    for (size_t v = 0; v < s->count && rc == SIGIL_OK; v++) {
        const volume_file *f = &s->files[v];
        if (!f->card) continue;
        bool corrupt = false;
        rc = corrupt_own_save(x, f, &corrupt);
        if (rc == SIGIL_OK && corrupt) rc = SIGIL_ERR_DAMAGED;
        size_t before = out->count;
        if (rc == SIGIL_OK) rc = sigil_sync_add_card_saves(x, f->card, f->format, f->target.device, v, SYNC_WHO_LOCAL, out);
        for (size_t i = before; i < out->count; i++) {
            sigil_sync_save *o = &out->items[i];
            o->owner = volume_owner(x, f, o->name, o->other, &o->companion);
        }
        if (rc == SIGIL_ERR_DAMAGED) snprintf(r->problem, sizeof(r->problem), "%s", f->target.path);
    }
    if (rc != SIGIL_OK) sigil_sync_saves_free(out);
    return rc;
}

/* The state learns who owns each save of the game on a shared volume, and
 * each companion's save on any volume, so collect can tell them apart. */
static int learn_owners(sigil_sync_ctx *x, const volume_set *s, const sigil_sync_saves *saves) {
    int rc = SIGIL_OK;
    for (size_t i = 0; i < saves->count && rc == SIGIL_OK; i++) {
        const sigil_sync_save *o = &saves->items[i];
        if (o->card == SIZE_MAX) continue;
        bool game = o->owner == SYNC_OWN_GAME && !s->files[o->card].target.per_game;
        if (!game && o->owner != SYNC_OWN_COMPANION) continue;
        rc = sigil_sync_owner_put(&x->state, s->files[o->card].key, o->name, game ? x->game : o->other);
    }
    return rc;
}

/* Records, per shared volume, every save on it and the ones with no owner. */
static int note_volumes(sigil_sync_ctx *x, const volume_set *s, const sigil_sync_saves *saves) {
    int rc = SIGIL_OK;
    for (size_t v = 0; v < s->count && rc == SIGIL_OK; v++) {
        if (s->files[v].target.per_game) continue;
        char all[33], none[33];
        rc = sigil_sync_identity_where(saves, -1, NULL, -1, v, all);
        if (rc == SIGIL_OK) rc = sigil_sync_identity_where(saves, SYNC_OWN_NONE, NULL, -1, v, none);
        if (rc == SIGIL_OK) rc = sigil_sync_state_put(&x->state, "seen", s->files[v].key, all);
        if (rc == SIGIL_OK) {
            bool any_none = sigil_sync_count_where(saves, SYNC_OWN_NONE, v) > 0;
            rc = sigil_sync_state_put(&x->state, "held", s->files[v].key, any_none ? none : NULL);
        }
    }
    return rc;
}

/* A save with no known owner that is new or rewritten since the last
 * collect saw its volume. None is, on a volume no collect has seen. */
static bool unowned_changed(const sigil_sync_ctx *x, const volume_set *s, const sigil_sync_save *o, const char *md5) {
    const char *key = s->files[o->card].key;
    if (!sigil_sync_state_get(&x->state, "seen", key)) return false;
    const char *before = sigil_sync_unowned_get(&x->state, key, o->name);
    return !before || strcmp(before, md5) != 0;
}

/* The names of the saves with no known owner, for the user to claim: those
 * new or rewritten since the last collect first, counted in
 * `unowned_changed`. The state keeps each one's hash for the next collect.
 * Call before note_volumes, which records the volumes as seen. */
static int note_unowned(sigil_sync_ctx *x, const volume_set *s, const sigil_sync_saves *saves, sigil_sync_result *r) {
    size_t unowned = sigil_sync_count_where(saves, SYNC_OWN_NONE, SIZE_MAX);
    char (*md5)[33] = unowned ? calloc(saves->count, 33) : NULL;
    if (unowned && (!md5 || !(r->unowned = calloc(unowned, SIGIL_CARD_NAME_MAX)))) {
        free(md5);
        return SIGIL_ERR_OOM;
    }
    int rc = SIGIL_OK;
    for (size_t i = 0; i < saves->count && rc == SIGIL_OK; i++) {
        if (saves->items[i].owner == SYNC_OWN_NONE) rc = x->kind->identity(saves->items[i].save, md5[i]);
    }
    for (int changed = 1; changed >= 0 && rc == SIGIL_OK; changed--) {
        for (size_t i = 0; i < saves->count; i++) {
            const sigil_sync_save *o = &saves->items[i];
            if (o->owner != SYNC_OWN_NONE || unowned_changed(x, s, o, md5[i]) != (changed == 1)) continue;
            snprintf(r->unowned[r->unowned_count++], SIGIL_CARD_NAME_MAX, "%s", o->name);
            r->unowned_changed += changed;
        }
    }
    for (size_t v = 0; v < s->count && rc == SIGIL_OK; v++) {
        if (!s->files[v].target.per_game) rc = sigil_sync_unowned_clear(&x->state, s->files[v].key);
    }
    for (size_t i = 0; i < saves->count && rc == SIGIL_OK; i++) {
        const sigil_sync_save *o = &saves->items[i];
        if (o->owner == SYNC_OWN_NONE) rc = sigil_sync_unowned_put(&x->state, s->files[o->card].key, o->name, md5[i]);
    }
    free(md5);
    return rc;
}

int sigil_sync_collect_volumes(sigil_sync_ctx *x, sigil_sync_result *r) {
    volume_set vols;
    int rc = gather_volumes(x, &vols, r);
    if (rc != SIGIL_OK) return rc;
    sigil_sync_saves saves;
    rc = gather_volume_saves(x, &vols, &saves, r);
    if (rc != SIGIL_OK) { volume_set_free(&vols); return rc; }

    sigil_sync_sources src;
    memset(&src, 0, sizeof(src));
    for (size_t v = 0; v < vols.count; v++) src.source[vols.files[v].target.device] = vols.files[v].card;
    char holding_artifact[SIGIL_SAVE_ENTRY_MAX], holding_hash[33];
    int holding_shape = 0;
    rc = sigil_sync_finish_collect(x, &saves, &src, r);
    if (rc == SIGIL_OK) {
        rc = sigil_sync_build_unit(&saves, SYNC_OWN_NONE, &src, true, "holding", &r->holding, &r->holding_len,
                                   &holding_shape, holding_artifact, holding_hash);
    }
    if (rc == SIGIL_OK) rc = note_unowned(x, &vols, &saves, r);
    if (rc == SIGIL_OK) rc = learn_owners(x, &vols, &saves);
    if (rc == SIGIL_OK) rc = note_volumes(x, &vols, &saves);
    sigil_sync_saves_free(&saves);
    volume_set_free(&vols);
    return rc;
}

/* Managed: a shared volume can be swapped only when every save on it that
 * isn't the game's has been passed on: another game's saves match what that
 * game last synced, and saves with no owner match the last holding unit. A
 * corrupt save can't be passed on, so a shared volume holding one is
 * SIGIL_ERR_DAMAGED naming it. */
static int check_swappable(const sigil_sync_ctx *x, const volume_set *s, const sigil_sync_saves *local,
                           sigil_sync_result *r) {
    for (size_t v = 0; v < s->count; v++) {
        if (s->files[v].target.per_game || !s->files[v].card) continue;
        sigil_card_listing *listing = NULL;
        int rc = x->kind->list(s->files[v].card, s->files[v].format, &listing);
        if (rc != SIGIL_OK) return rc;
        bool corrupt = listing->corrupt_count > 0;
        sigil_card_listing_free(listing);
        if (corrupt) {
            snprintf(r->problem, sizeof(r->problem), "%s", s->files[v].target.path);
            return SIGIL_ERR_DAMAGED;
        }
    }
    for (size_t v = 0; v < s->count; v++) {
        if (s->files[v].target.per_game || sigil_sync_count_where(local, SYNC_OWN_NONE, v) == 0) continue;
        char none[33];
        int rc = sigil_sync_identity_where(local, SYNC_OWN_NONE, NULL, -1, v, none);
        if (rc != SIGIL_OK) return rc;
        const char *held = sigil_sync_state_get(&x->state, "held", s->files[v].key);
        if (!held || strcmp(held, none) != 0) return SIGIL_ERR_UNCOLLECTED;
    }
    for (size_t i = 0; i < local->count; i++) {
        if (local->items[i].owner != SYNC_OWN_OTHER) continue;
        char theirs[33];
        int rc = sigil_sync_identity_where(local, SYNC_OWN_OTHER, local->items[i].other, -1, SIZE_MAX, theirs);
        if (rc != SIGIL_OK) return rc;
        const char *synced = sigil_sync_state_get(&x->state, "synced", local->items[i].other);
        if (!synced || strcmp(synced, theirs) != 0) return SIGIL_ERR_UNCOLLECTED;
    }
    return SIGIL_OK;
}

/* Unmanaged: a shared volume takes an inject only when nothing on it changed
 * since the last collect saw it. */
static int check_unchanged(const sigil_sync_ctx *x, const volume_set *s, const sigil_sync_saves *local) {
    for (size_t v = 0; v < s->count; v++) {
        if (s->files[v].target.per_game || sigil_sync_count_where(local, -1, v) == 0) continue;
        char all[33];
        int rc = sigil_sync_identity_where(local, -1, NULL, -1, v, all);
        if (rc != SIGIL_OK) return rc;
        const char *seen = sigil_sync_state_get(&x->state, "seen", s->files[v].key);
        if (!seen || strcmp(seen, all) != 0) return SIGIL_ERR_UNCOLLECTED;
    }
    return SIGIL_OK;
}

/* Removes the game's saves, and those of each companion the restore carries
 * a unit for, from `card`, listing again after each removal, since a Sega
 * CD delete moves the saves after it. */
static int remove_game_saves(const sigil_sync_ctx *x, const volume_file *f, void *card) {
    for (;;) {
        sigil_card_listing *listing = NULL;
        int rc = x->kind->list(card, f->format, &listing);
        if (rc != SIGIL_OK) return rc;
        const sigil_card_entry *found = NULL;
        char other[SYNC_KEY_MAX];
        for (size_t i = 0; i < listing->entry_count && !found; i++) {
            size_t c = SIZE_MAX;
            int owner = volume_owner(x, f, listing->entries[i].name, other, &c);
            bool replaced = owner == SYNC_OWN_GAME || (owner == SYNC_OWN_COMPANION && sigil_sync_companion_restored(x, c));
            if (replaced) found = &listing->entries[i];
        }
        if (found) rc = x->kind->remove(card, found);
        sigil_card_listing_free(listing);
        if (rc != SIGIL_OK || !found) return rc;
    }
}

/* The hash over every save on `card`, as the `seen` record keeps it. */
static int card_identity(const sigil_sync_ctx *x, const void *card, int format, int device, char out[33]) {
    sigil_sync_saves all;
    sigil_sync_saves_init(&all, x->kind);
    int rc = sigil_sync_add_card_saves(x, card, format, device, 0, SYNC_WHO_GAME, &all);
    if (rc == SIGIL_OK) rc = sigil_sync_identity_where(&all, -1, NULL, -1, SIZE_MAX, out);
    sigil_sync_saves_free(&all);
    return rc;
}

/* Builds each volume's next contents: managed swaps in a fresh volume
 * holding the game's saves and its kept companions'; unmanaged injects into
 * the volume as it is. `seen` gets each volume's identity afterwards. */
static int place_on_volumes(sigil_sync_ctx *x, volume_set *s, const sigil_sync_saves *local,
                            sigil_sync_saves *incoming, const size_t sizes[SIGIL_DEVICE_COUNT], bool already_there,
                            sigil_sync_result *r, char seen[SIGIL_VOLUME_TARGETS_MAX][33]) {
    bool managed = x->req->mode == SIGIL_SYNC_MANAGED;
    int rc = SIGIL_OK;
    for (size_t i = 0; i < incoming->count && rc == SIGIL_OK; i++) {
        incoming->items[i].card = SIZE_MAX;
        for (size_t v = 0; v < s->count; v++) {
            if (s->files[v].target.device == incoming->items[i].device) incoming->items[i].card = v;
        }
        if (incoming->items[i].card == SIZE_MAX) {
            snprintf(r->problem, sizeof(r->problem), "%s", sigil_sync_device_name(incoming->items[i].device));
            rc = SIGIL_ERR_NO_TARGET;
        }
    }
    void *next[SIGIL_VOLUME_TARGETS_MAX] = { NULL };
    bool write[SIGIL_VOLUME_TARGETS_MAX] = { false };
    for (size_t v = 0; v < s->count && rc == SIGIL_OK; v++) {
        volume_file *f = &s->files[v];
        bool others = sigil_sync_count_where(local, SYNC_OWN_OTHER, v) + sigil_sync_count_where(local, SYNC_OWN_NONE, v) > 0;
        bool receives = false;
        for (size_t i = 0; i < incoming->count; i++) receives = receives || incoming->items[i].card == v;
        if (managed) write[v] = already_there ? others : (f->card || receives);
        else write[v] = !already_there && (receives || sigil_sync_count_where(local, SYNC_OWN_GAME, v) > 0);
        if (!write[v]) continue;
        int format = 0;
        if (managed || !f->card) {
            int device = f->target.device;
            size_t size = f->target.new_size ? f->target.new_size
                          : sizes[device]    ? sizes[device]
                                             : x->kind->unit_size(device, NULL);
            rc = x->kind->blank(&next[v], &format, device, size, f->target.form, f->card);
        } else {
            uint8_t *image = NULL;
            size_t len = 0;
            rc = x->kind->image(f->card, &image, &len);
            if (rc == SIGIL_OK) rc = sigil_sync_load_bytes(x->kind, image, len, f->target.device, &next[v], &format);
            free(image);
            if (rc == SIGIL_OK) rc = remove_game_saves(x, f, next[v]);
        }
        if (!f->card) f->format = format;
        for (size_t i = 0; i < incoming->count && rc == SIGIL_OK; i++) {
            if (incoming->items[i].card != v) continue;
            rc = x->kind->inject(next[v], incoming->items[i].save);
            if (rc == SIGIL_ERR_NO_SPACE) sigil_sync_note_overflow(x, next[v], format, &incoming->items[i], r);
        }
        for (size_t i = 0; managed && i < local->count && rc == SIGIL_OK; i++) {
            const sigil_sync_save *o = &local->items[i];
            if (o->card != v || o->owner != SYNC_OWN_COMPANION || sigil_sync_companion_restored(x, o->companion)) continue;
            rc = x->kind->inject(next[v], o->save);
            if (rc == SIGIL_ERR_NO_SPACE) sigil_sync_note_overflow(x, next[v], format, o, r);
        }
    }
    for (size_t v = 0; v < s->count && rc == SIGIL_OK; v++) {
        if (write[v]) {
            rc = sigil_sync_write_and_verify(x, s->files[v].target.path, s->files[v].target.device, next[v], incoming, v);
        }
    }
    for (size_t v = 0; v < s->count && rc == SIGIL_OK; v++) {
        const void *final = write[v] ? next[v] : s->files[v].card;
        if (final) rc = card_identity(x, final, s->files[v].format, s->files[v].target.device, seen[v]);
        else sigil_named_hash(NULL, 0, seen[v]);
    }
    for (size_t v = 0; v < SIGIL_VOLUME_TARGETS_MAX; v++) {
        if (next[v]) x->kind->free_card(next[v]);
    }
    return rc;
}

/* After a restore, the state learns the owners of the saves it placed and
 * what each shared volume now holds. */
static int note_restored(sigil_sync_ctx *x, const volume_set *s, const sigil_sync_saves *incoming,
                         char seen[SIGIL_VOLUME_TARGETS_MAX][33]) {
    bool managed = x->req->mode == SIGIL_SYNC_MANAGED;
    int rc = learn_owners(x, s, incoming);
    for (size_t v = 0; v < s->count && rc == SIGIL_OK; v++) {
        if (s->files[v].target.per_game) continue;
        if (managed) rc = sigil_sync_state_put(&x->state, "prepared", s->files[v].key, x->game);
        if (rc == SIGIL_OK) rc = sigil_sync_state_put(&x->state, "seen", s->files[v].key, seen[v]);
        if (rc == SIGIL_OK && managed) rc = sigil_sync_state_put(&x->state, "held", s->files[v].key, NULL);
    }
    return rc;
}

int sigil_sync_restore_volumes(sigil_sync_ctx *x, sigil_sync_saves *incoming, const size_t sizes[SIGIL_DEVICE_COUNT],
                               sigil_sync_result *r, char local_identity[33]) {
    volume_set vols;
    int rc = gather_volumes(x, &vols, r);
    if (rc != SIGIL_OK) return rc;
    sigil_sync_saves local;
    rc = gather_volume_saves(x, &vols, &local, r);
    if (rc != SIGIL_OK) { volume_set_free(&vols); return rc; }

    bool already_there = false;
    char seen[SIGIL_VOLUME_TARGETS_MAX][33];
    rc = sigil_sync_check_restore(x, &local, incoming, r, local_identity, &already_there);
    if (rc == SIGIL_OK) rc = x->req->mode == SIGIL_SYNC_MANAGED ? check_swappable(x, &vols, &local, r)
                                                                 : check_unchanged(x, &vols, &local);
    if (rc == SIGIL_OK) rc = place_on_volumes(x, &vols, &local, incoming, sizes, already_there, r, seen);
    if (rc == SIGIL_OK) rc = note_restored(x, &vols, incoming, seen);
    sigil_sync_saves_free(&local);
    volume_set_free(&vols);
    return rc;
}
