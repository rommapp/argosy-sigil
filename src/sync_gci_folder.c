// SPDX-License-Identifier: MPL-2.0
/* Dolphin's GCI folder: each save is a .gci file of its own in
 * GC/<region>/Card A/. */
#include "sync_internal.h"

int sigil_sync_save_folder_of(const sigil_sync_ctx *x, char folder[SIGIL_SAVE_PATH_MAX]) {
    folder[0] = '\0';
    sigil_volume_target targets[SIGIL_VOLUME_TARGETS_MAX];
    size_t count = 0;
    int rc = sigil_save_volume_targets(&x->req->save, targets, &count);
    for (size_t i = 0; i < count && rc == SIGIL_OK; i++) {
        if (targets[i].device == SIGIL_DEVICE_GC_FOLDER) snprintf(folder, SIGIL_SAVE_PATH_MAX, "%s", targets[i].path);
    }
    return rc;
}

static int compare_paths(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Files directly in `folder` with the .gci extension Dolphin loads, sorted. */
static size_t gci_files(const sigil_sync_request *req, const char *folder, const char ***out) {
    size_t prefix = strlen(folder), n = 0;
    *out = (const char **)calloc(req->save.listing_count + 1, sizeof(char *));
    if (!*out) return 0;
    for (size_t i = 0; i < req->save.listing_count; i++) {
        const char *p = req->save.listing[i];
        if (!p || strncmp(p, folder, prefix) != 0 || strchr(p + prefix, '/')) continue;
        size_t len = strlen(p);
        if (len <= prefix + 4 || strcmp(p + len - 4, ".gci") != 0) continue;
        (*out)[n++] = p;
    }
    qsort(*out, n, sizeof(char *), compare_paths);
    return n;
}

/* Adds the save in `data`, read from `path`, to `out` when it is the game's
 * or a companion's; a second file with an identity already seen, which
 * Dolphin refuses at load, goes in `stale`. */
static int add_gci(const sigil_sync_ctx *x, const char *path, const uint8_t *data, size_t len, sigil_sync_saves *out,
                   sigil_sync_paths *stale) {
    void *save = NULL;
    if (x->kind->file_to_save(data, len, &save) != SIGIL_OK) return SIGIL_OK;
    const sigil_sync_blob *b = (const sigil_sync_blob *)save;
    char owner[SIGIL_CARD_OWNER_MAX];
    snprintf(owner, sizeof(owner), "%02X%02X%02X%02X", b->data[0], b->data[1], b->data[2], b->data[3]);
    char key[SIGIL_CARD_NAME_MAX];
    x->kind->save_key(save, key);
    bool game = sigil_sync_owned_by_game(x->req, owner);
    size_t companion = game ? SIZE_MAX : sigil_sync_companion_of(x->req, owner);
    if (!game && companion == SIZE_MAX) {
        x->kind->free_save(save);
        return SIGIL_OK;
    }
    if (sigil_sync_saves_has(out, key, SIGIL_DEVICE_NONE)) {
        snprintf(stale->paths[stale->count++], SIGIL_SAVE_PATH_MAX, "%s", path);
        x->kind->free_save(save);
        return SIGIL_OK;
    }
    sigil_sync_save *o = sigil_sync_saves_push(out);
    if (!o) { x->kind->free_save(save); return SIGIL_ERR_OOM; }
    snprintf(o->name, sizeof(o->name), "%s", key);
    snprintf(o->path, sizeof(o->path), "%s", path);
    o->save = save;
    o->card = SIZE_MAX;
    o->owner = SYNC_OWN_GAME;
    o->companion = SIZE_MAX;
    if (companion != SIZE_MAX) sigil_sync_set_companion(x, o, companion);
    out->count++;
    return SIGIL_OK;
}

int sigil_sync_folder_saves(const sigil_sync_ctx *x, const char *folder, sigil_sync_saves *out,
                            sigil_sync_paths *stale) {
    sigil_sync_saves_init(out, x->kind);
    const char **files = NULL;
    size_t n = gci_files(x->req, folder, &files);
    stale->paths = calloc(n + 1, SIGIL_SAVE_PATH_MAX);
    stale->count = 0;
    int rc = files && stale->paths ? SIGIL_OK : SIGIL_ERR_OOM;
    for (size_t i = 0; i < n && rc == SIGIL_OK; i++) {
        uint8_t *data = NULL;
        size_t len = 0;
        if (sigil_sync_read_file(x->req, files[i], GC_MAX_CARD_SIZE + GC_DENTRY_SIZE, &data, &len) == SIGIL_OK) {
            rc = add_gci(x, files[i], data, len, out, stale);
        }
        free(data);
    }
    free(files);
    if (rc != SIGIL_OK) {
        sigil_sync_saves_free(out);
        free(stale->paths);
        stale->paths = NULL;
    }
    return rc;
}

/* Where an incoming save goes: over the game's file holding its identity,
 * else Dolphin's name for it, with a digit before .gci when another game's
 * file already has that name, as Dolphin numbers one. */
static void folder_target(const sigil_sync_ctx *x, const char *folder, const sigil_sync_saves *local,
                          const sigil_sync_save *o, char out[SIGIL_SAVE_PATH_MAX]) {
    for (size_t i = 0; i < local->count; i++) {
        if (strcmp(local->items[i].name, o->name) == 0) {
            snprintf(out, SIGIL_SAVE_PATH_MAX, "%s", local->items[i].path);
            return;
        }
    }
    char name[SIGIL_SAVE_PATH_MAX];
    x->kind->file_name(o->save, name, sizeof(name));
    snprintf(out, SIGIL_SAVE_PATH_MAX, "%s%s", folder, name);
    size_t base = strlen(out) - 4;
    for (int digit = 0; digit < 10 && sigil_sync_listed(x->req, out); digit++) {
        snprintf(out + base, SIGIL_SAVE_PATH_MAX - base, "%d.gci", digit);
    }
}

/* The restore replaces this local file: it is the game's, or a companion's
 * the request carries a unit for, and the incoming saves lack it. */
static bool file_goes(const sigil_sync_ctx *x, const sigil_sync_save *o, const sigil_sync_saves *incoming) {
    bool replaced = o->owner == SYNC_OWN_GAME ||
                    (o->owner == SYNC_OWN_COMPANION && sigil_sync_companion_restored(x, o->companion));
    return replaced && !sigil_sync_saves_has(incoming, o->name, SIGIL_DEVICE_NONE);
}

int sigil_sync_place_in_folder(const sigil_sync_ctx *x, const char *folder, const sigil_sync_saves *local,
                               const sigil_sync_paths *stale, const sigil_sync_saves *incoming) {
    const sigil_sync_request *req = x->req;
    size_t removes = stale->count;
    for (size_t i = 0; i < local->count; i++) removes += file_goes(x, &local->items[i], incoming);
    if (removes && !req->remove) return SIGIL_ERR_INVALID_ARG;

    int rc = SIGIL_OK;
    for (size_t i = 0; i < incoming->count && rc == SIGIL_OK; i++) {
        const sigil_sync_blob *b = (const sigil_sync_blob *)incoming->items[i].save;
        char path[SIGIL_SAVE_PATH_MAX];
        folder_target(x, folder, local, &incoming->items[i], path);
        if (req->write(req->write_ctx, path, b->data, b->len) != 0 || !sigil_sync_file_holds(req, path, b->data, b->len)) {
            rc = SIGIL_ERR_IO;
        }
    }
    for (size_t i = 0; i < local->count && rc == SIGIL_OK; i++) {
        if (!file_goes(x, &local->items[i], incoming)) continue;
        if (req->remove(req->write_ctx, local->items[i].path) != 0) rc = SIGIL_ERR_IO;
    }
    for (size_t i = 0; i < stale->count && rc == SIGIL_OK; i++) {
        if (req->remove(req->write_ctx, stale->paths[i]) != 0) rc = SIGIL_ERR_IO;
    }
    return rc;
}
