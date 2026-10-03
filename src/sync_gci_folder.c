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
 * else Dolphin's name for it, with a 0 inserted before .gci for as long as
 * another game's file has the name, as Dolphin's GCI folder writes one
 * (GCMemcardDirectory FlushToFile). */
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
    for (int zeros = 0; zeros < 10 && sigil_sync_listed(x->req, out) && base + 6 < SIGIL_SAVE_PATH_MAX; zeros++, base++) {
        snprintf(out + base, SIGIL_SAVE_PATH_MAX - base, "0.gci");
    }
}

/* The restore replaces this local file: it is the game's, or a companion's
 * the request carries a unit for, and the incoming saves lack it. */
static bool file_goes(const sigil_sync_ctx *x, const sigil_sync_save *o, const sigil_sync_saves *incoming) {
    bool replaced = o->owner == SYNC_OWN_GAME ||
                    (o->owner == SYNC_OWN_COMPANION && sigil_sync_companion_restored(x, o->companion));
    return replaced && !sigil_sync_saves_has(incoming, o->name, SIGIL_DEVICE_NONE);
}

/* Dolphin's GCI folder is a 2043-block card; it keeps a tenth of its 2048
 * blocks free when loading other games' files, and stops past 112 saves. */
#define FOLDER_TOTAL_BLOCKS   (GC_MAX_CARD_SIZE / GC_BLOCK_SIZE)
#define FOLDER_RESERVED       (FOLDER_TOTAL_BLOCKS / 10u)
#define FOLDER_OTHERS_MAX     112u
#define GCI_BLOCK_COUNT       0x38u

typedef struct {
    const char            *path;
    char                   owner[SIGIL_CARD_OWNER_MAX];
    uint32_t               blocks;
    const sigil_sync_save *incoming;   /* the incoming save written there, or NULL for a file kept */
} folder_file;

static int compare_folder_files(const void *a, const void *b) {
    return strcmp(((const folder_file *)a)->path, ((const folder_file *)b)->path);
}

static void gci_facts(const uint8_t *gci, folder_file *f) {
    snprintf(f->owner, sizeof(f->owner), "%02X%02X%02X%02X", gci[0], gci[1], gci[2], gci[3]);
    f->blocks = sigil_read_be16(gci + GCI_BLOCK_COUNT);
}

static bool path_in(const char *path, const char (*paths)[SIGIL_SAVE_PATH_MAX], size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(paths[i], path) == 0) return true;
    }
    return false;
}

/* The folder as the restore leaves it, loaded as Dolphin loads it
 * (GCMemcardDirectory): the running game's files first, then other games'
 * in name order while each leaves FOLDER_RESERVED blocks free, stopping past
 * FOLDER_OTHERS_MAX saves. A companion is another game. SIGIL_ERR_NO_SPACE,
 * naming the first incoming save that wouldn't load, when one wouldn't. */
static int check_dolphin_loads(const sigil_sync_ctx *x, const char *folder, const sigil_sync_saves *local,
                               const sigil_sync_paths *stale, const sigil_sync_saves *incoming,
                               char (*targets)[SIGIL_SAVE_PATH_MAX], sigil_sync_result *r) {
    const sigil_sync_request *req = x->req;
    const char **listed = NULL;
    size_t listed_count = gci_files(req, folder, &listed);
    folder_file *files = (folder_file *)calloc(listed_count + incoming->count + 1, sizeof(folder_file));
    int rc = listed && files ? SIGIL_OK : SIGIL_ERR_OOM;
    size_t n = 0;
    for (size_t i = 0; i < listed_count && rc == SIGIL_OK; i++) {
        const char *p = listed[i];
        bool goes = path_in(p, (const char (*)[SIGIL_SAVE_PATH_MAX])stale->paths, stale->count) ||
                    path_in(p, (const char (*)[SIGIL_SAVE_PATH_MAX])targets, incoming->count);
        for (size_t k = 0; k < local->count && !goes; k++) {
            goes = strcmp(local->items[k].path, p) == 0 && file_goes(x, &local->items[k], incoming);
        }
        uint8_t *data = NULL;
        size_t len = 0;
        if (goes || sigil_sync_read_file(req, p, GC_MAX_CARD_SIZE + GC_DENTRY_SIZE, &data, &len) != SIGIL_OK) continue;
        if (len >= GC_DENTRY_SIZE) {
            files[n].path = p;
            gci_facts(data, &files[n]);
            n++;
        }
        free(data);
    }
    for (size_t i = 0; i < incoming->count && rc == SIGIL_OK; i++) {
        const sigil_sync_blob *b = (const sigil_sync_blob *)incoming->items[i].save;
        files[n].path = targets[i];
        files[n].incoming = &incoming->items[i];
        gci_facts(b->data, &files[n]);
        n++;
    }
    if (rc == SIGIL_OK) qsort(files, n, sizeof(folder_file), compare_folder_files);

    uint32_t free_blocks = FOLDER_TOTAL_BLOCKS - GC_SYSTEM_BLOCKS, loaded = 0;
    for (int pass = 0; pass < 2 && rc == SIGIL_OK; pass++) {
        for (size_t i = 0; i < n && rc == SIGIL_OK; i++) {
            const folder_file *f = &files[i];
            bool game = sigil_sync_owned_by_game(req, f->owner);
            if (game != (pass == 0)) continue;
            bool loads;
            uint32_t short_by = 0;
            if (game) {
                loads = f->blocks <= free_blocks && loaded < GC_DIR_ENTRIES;
                if (!loads && f->blocks > free_blocks) short_by = f->blocks - free_blocks;
            } else if (loaded > FOLDER_OTHERS_MAX) {
                loads = false;
            } else {
                loads = free_blocks >= f->blocks + FOLDER_RESERVED;
                if (!loads) short_by = f->blocks + FOLDER_RESERVED - free_blocks;
            }
            if (loads) {
                free_blocks -= f->blocks;
                loaded++;
            } else if (f->incoming) {
                snprintf(r->problem, sizeof(r->problem), "%s", f->incoming->name);
                r->blocks_short = short_by;
                rc = SIGIL_ERR_NO_SPACE;
            }
        }
    }
    free(files);
    free(listed);
    return rc;
}

int sigil_sync_place_in_folder(const sigil_sync_ctx *x, const char *folder, const sigil_sync_saves *local,
                               const sigil_sync_paths *stale, const sigil_sync_saves *incoming, sigil_sync_result *r) {
    const sigil_sync_request *req = x->req;
    size_t removes = stale->count;
    for (size_t i = 0; i < local->count; i++) removes += file_goes(x, &local->items[i], incoming);
    if (removes && !req->remove) return SIGIL_ERR_INVALID_ARG;

    char (*targets)[SIGIL_SAVE_PATH_MAX] = calloc(incoming->count + 1, SIGIL_SAVE_PATH_MAX);
    if (!targets) return SIGIL_ERR_OOM;
    for (size_t i = 0; i < incoming->count; i++) folder_target(x, folder, local, &incoming->items[i], targets[i]);
    int rc = check_dolphin_loads(x, folder, local, stale, incoming, targets, r);
    for (size_t i = 0; i < incoming->count && rc == SIGIL_OK; i++) {
        const sigil_sync_blob *b = (const sigil_sync_blob *)incoming->items[i].save;
        if (req->write(req->write_ctx, targets[i], b->data, b->len) != 0 ||
            !sigil_sync_file_holds(req, targets[i], b->data, b->len)) {
            rc = SIGIL_ERR_IO;
        }
    }
    free(targets);
    for (size_t i = 0; i < local->count && rc == SIGIL_OK; i++) {
        if (!file_goes(x, &local->items[i], incoming)) continue;
        if (req->remove(req->write_ctx, local->items[i].path) != 0) rc = SIGIL_ERR_IO;
    }
    for (size_t i = 0; i < stale->count && rc == SIGIL_OK; i++) {
        if (req->remove(req->write_ctx, stale->paths[i]) != 0) rc = SIGIL_ERR_IO;
    }
    return rc;
}
