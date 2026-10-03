// SPDX-License-Identifier: MPL-2.0
/* PCSX2 folder cards. A folder card is a directory named as the card file
 * would be, holding _pcsx2_superblock and one directory per save
 * (MemoryCardFolder.cpp). sync loads it as a card holding the folders the
 * game sees, so the card path works on it unchanged, and writes back only
 * the folders of the game and of each companion the restore carries. */
#include "sync_internal.h"

#define FOLDER_CARD_FILE_MAX (8u * 1024u * 1024u)

bool sigil_sync_is_folder_card(const sigil_sync_request *req, const char *dir) {
    size_t n = strlen(dir);
    for (size_t i = 0; i < req->save.listing_count; i++) {
        const char *p = req->save.listing[i];
        if (p && strncmp(p, dir, n) == 0 && p[n] == '/') return true;
    }
    return false;
}

/* The save folder `path` sits in under `dir`/, into `name`; false for a file
 * of the card itself, such as _pcsx2_superblock. */
static bool save_folder_of_path(const char *dir, const char *path, char name[SIGIL_CARD_NAME_MAX]) {
    size_t n = strlen(dir);
    if (strncmp(path, dir, n) != 0 || path[n] != '/') return false;
    const char *start = path + n + 1;
    const char *slash = strchr(start, '/');
    if (!slash || (size_t)(slash - start) >= SIGIL_CARD_NAME_MAX || slash == start) return false;
    snprintf(name, SIGIL_CARD_NAME_MAX, "%.*s", (int)(slash - start), start);
    return true;
}

/* PCSX2 shows a game the folders its filter names and the system folders
 * every game reads (MemoryCardFolder.cpp, the DATA-SYSTEM and BWNETCNF
 * filter); of those, the card needs the game's, its companions' and the
 * system's. */
static bool shown_to_game(const sigil_sync_request *req, const char *name) {
    if (strstr(name, "DATA-SYSTEM") || strstr(name, "BWNETCNF")) return true;
    char owner[SIGIL_CARD_OWNER_MAX];
    sigil_card_sony_owner(name, owner);
    return owner[0] && (sigil_sync_owned_by_game(req, owner) || sigil_sync_companion_of(req, owner) != SIZE_MAX);
}

/* Reads the files of save folder `name` on folder card `dir` and packs them. */
static int read_save_folder(const sigil_sync_request *req, const char *dir, const char *name, sigil_ps2_save *out) {
    char prefix[SIGIL_SAVE_PATH_MAX];
    snprintf(prefix, sizeof(prefix), "%s/%s/", dir, name);
    size_t plen = strlen(prefix);
    sigil_ps2_folder_file *files = calloc(req->save.listing_count + 1, sizeof(*files));
    if (!files) return SIGIL_ERR_OOM;
    size_t n = 0;
    int rc = SIGIL_OK;
    for (size_t i = 0; i < req->save.listing_count && rc == SIGIL_OK; i++) {
        const char *p = req->save.listing[i];
        if (!p || strncmp(p, prefix, plen) != 0 || strlen(p + plen) >= PS2_FOLDER_PATH_MAX) continue;
        int read = sigil_sync_read_file(req, p, FOLDER_CARD_FILE_MAX, &files[n].data, &files[n].len);
        if (read == SIGIL_ERR_NOT_FOUND) continue;
        rc = read;
        if (rc == SIGIL_OK) snprintf(files[n++].path, PS2_FOLDER_PATH_MAX, "%s", p + plen);
    }
    if (rc == SIGIL_OK) rc = n ? sigil_ps2_pack(name, files, n, out) : SIGIL_ERR_NOT_FOUND;
    sigil_ps2_folder_files_free(files, n);
    return rc;
}

static int compare_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* The save folders under `dir` the game sees, one name each, sorted. */
static size_t shown_folders(const sigil_sync_request *req, const char *dir, char (**out)[SIGIL_CARD_NAME_MAX]) {
    *out = calloc(req->save.listing_count + 1, SIGIL_CARD_NAME_MAX);
    if (!*out) return 0;
    size_t n = 0;
    for (size_t i = 0; i < req->save.listing_count; i++) {
        char name[SIGIL_CARD_NAME_MAX];
        if (!req->save.listing[i] || !save_folder_of_path(dir, req->save.listing[i], name)) continue;
        bool seen = false;
        for (size_t k = 0; k < n && !seen; k++) seen = strcmp((*out)[k], name) == 0;
        if (!seen && shown_to_game(req, name)) snprintf((*out)[n++], SIGIL_CARD_NAME_MAX, "%s", name);
    }
    qsort(*out, n, SIGIL_CARD_NAME_MAX, compare_names);
    return n;
}

/* A folder whose index doesn't parse is left off, as an unreadable card is. */
int sigil_sync_load_folder_card(const sigil_sync_ctx *x, const char *dir, sigil_sync_cards *s) {
    if (s->count >= SYNC_MAX_CARD_FILES) return SIGIL_OK;
    sigil_sync_card_file *f = &s->files[s->count];
    memset(f, 0, sizeof(*f));
    int rc = x->kind->blank(&f->card, &f->format, SIGIL_DEVICE_NONE, 0, SIGIL_FORM_RAW, NULL);
    if (rc != SIGIL_OK) return rc;
    char (*names)[SIGIL_CARD_NAME_MAX] = NULL;
    size_t n = shown_folders(x->req, dir, &names);
    if (!names) rc = SIGIL_ERR_OOM;
    for (size_t i = 0; i < n && rc == SIGIL_OK; i++) {
        sigil_ps2_save save;
        int read = read_save_folder(x->req, dir, names[i], &save);
        if (read == SIGIL_ERR_UNSUPPORTED_FORMAT || read == SIGIL_ERR_NOT_FOUND) continue;
        rc = read;
        if (rc == SIGIL_OK) {
            rc = sigil_ps2_inject((sigil_ps2_card *)f->card, &save);
            sigil_ps2_save_free(&save);
        }
    }
    free(names);
    if (rc != SIGIL_OK) { x->kind->free_card(f->card); return rc; }
    snprintf(f->path, sizeof(f->path), "%s", dir);
    f->folder = true;
    f->primary = strcmp(dir, s->primary_path) == 0;
    s->count++;
    return SIGIL_OK;
}

/* The restore rewrites save folder `name`: it is the game's, or a
 * companion's the restore carries a unit for. */
static bool rewritten(const sigil_sync_ctx *x, const char *name) {
    char owner[SIGIL_CARD_OWNER_MAX];
    sigil_card_sony_owner(name, owner);
    return owner[0] && (sigil_sync_owned_by_game(x->req, owner) ||
                        sigil_sync_companion_restored(x, sigil_sync_companion_of(x->req, owner)));
}

static bool unpacked_has(const sigil_ps2_folder_file *files, size_t n, const char *rel) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(files[i].path, rel) == 0) return true;
    }
    return false;
}

/* Adds to `doomed` the files under `prefix` the unpacked folder `files`
 * lacks, or all of them when `files` is NULL. */
static void doom_files(const sigil_sync_ctx *x, const char *prefix, const sigil_ps2_folder_file *files, size_t n,
                       sigil_sync_paths *doomed) {
    size_t plen = strlen(prefix);
    for (size_t i = 0; i < x->req->save.listing_count; i++) {
        const char *p = x->req->save.listing[i];
        if (!p || strncmp(p, prefix, plen) != 0 || (files && unpacked_has(files, n, p + plen))) continue;
        snprintf(doomed->paths[doomed->count++], SIGIL_SAVE_PATH_MAX, "%s", p);
    }
}

/* Writes save folder `save` under `prefix` as PCSX2 keeps it, file by file
 * where the bytes differ, and reads each back; with `apply` false it writes
 * nothing. Its files the save lacks go in `doomed`. */
static int write_save_folder(const sigil_sync_ctx *x, const char *prefix, const sigil_ps2_save *save, bool apply,
                             sigil_sync_paths *doomed) {
    sigil_ps2_folder_file *files = NULL;
    size_t n = 0;
    int rc = sigil_ps2_unpack(save, &files, &n);
    if (rc != SIGIL_OK) return rc;
    sigil_ps2_save back;
    char name[33], want[33], got[33];
    snprintf(name, sizeof(name), "%.32s", (const char *)save->entry + 0x40);
    rc = sigil_ps2_pack(name, files, n, &back);
    if (rc == SIGIL_OK) {
        sigil_ps2_save_md5(save, want);
        sigil_ps2_save_md5(&back, got);
        sigil_ps2_save_free(&back);
        if (strcmp(want, got) != 0) rc = SIGIL_ERR_IO;
    }
    for (size_t i = 0; i < n && rc == SIGIL_OK && apply; i++) {
        char path[SIGIL_SAVE_PATH_MAX];
        snprintf(path, sizeof(path), "%s%s", prefix, files[i].path);
        if (sigil_sync_file_holds(x->req, path, files[i].data, files[i].len)) continue;
        if (x->req->write(x->req->write_ctx, path, files[i].data, files[i].len) != 0 ||
            !sigil_sync_file_holds(x->req, path, files[i].data, files[i].len)) {
            rc = SIGIL_ERR_IO;
        }
    }
    if (rc == SIGIL_OK) doom_files(x, prefix, files, n, doomed);
    sigil_ps2_folder_files_free(files, n);
    return rc;
}

/* Brings folder card `f` on disk in line with its card for the folders the
 * restore rewrites, and removes their files the card lacks. Without `apply`
 * it only counts the files that would go. */
static int sync_folder_card(const sigil_sync_ctx *x, const sigil_sync_card_file *f, bool apply, size_t *removes) {
    *removes = 0;
    sigil_sync_paths doomed = { calloc(x->req->save.listing_count + 1, SIGIL_SAVE_PATH_MAX), 0 };
    sigil_card_listing *listing = NULL;
    int rc = doomed.paths ? x->kind->list(f->card, f->format, &listing) : SIGIL_ERR_OOM;
    for (size_t i = 0; rc == SIGIL_OK && i < listing->entry_count; i++) {
        const sigil_card_entry *e = &listing->entries[i];
        if (!rewritten(x, e->name)) continue;
        void *save = NULL;
        rc = x->kind->extract(f->card, e, &save);
        char prefix[SIGIL_SAVE_PATH_MAX];
        snprintf(prefix, sizeof(prefix), "%s/%s/", f->path, e->name);
        if (rc == SIGIL_OK) rc = write_save_folder(x, prefix, (const sigil_ps2_save *)save, apply, &doomed);
        if (save) x->kind->free_save(save);
    }
    for (size_t i = 0; rc == SIGIL_OK && i < x->req->save.listing_count; i++) {
        char name[SIGIL_CARD_NAME_MAX];
        const char *p = x->req->save.listing[i];
        if (!p || !save_folder_of_path(f->path, p, name) || !rewritten(x, name)) continue;
        bool kept = false;
        for (size_t k = 0; k < listing->entry_count && !kept; k++) kept = strcmp(listing->entries[k].name, name) == 0;
        if (!kept) snprintf(doomed.paths[doomed.count++], SIGIL_SAVE_PATH_MAX, "%s", p);
    }
    sigil_card_listing_free(listing);
    *removes = doomed.count;
    for (size_t i = 0; rc == SIGIL_OK && apply && i < doomed.count; i++) {
        if (x->req->remove(x->req->write_ctx, doomed.paths[i]) != 0) rc = SIGIL_ERR_IO;
    }
    free(doomed.paths);
    return rc;
}

/* Writes a full formatted _pcsx2_superblock to folder card `dir` when the one
 * there is missing or PCSX2 wouldn't read it as formatted; PCSX2 hides every
 * save on such a card. */
static int ensure_superblock(const sigil_sync_ctx *x, const char *dir) {
    char path[SIGIL_SAVE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/_pcsx2_superblock", dir);
    uint8_t *have = NULL;
    size_t have_len = 0;
    sigil_sync_read_file(x->req, path, PS2_FOLDER_SUPERBLOCK_SIZE + 1, &have, &have_len);
    bool usable = have && sigil_ps2_folder_superblock_usable(have, have_len);
    free(have);
    if (usable) return SIGIL_OK;
    uint8_t sb[PS2_FOLDER_SUPERBLOCK_SIZE];
    sigil_ps2_folder_superblock(sb);
    if (x->req->write(x->req->write_ctx, path, sb, sizeof(sb)) != 0 ||
        !sigil_sync_file_holds(x->req, path, sb, sizeof(sb))) {
        return SIGIL_ERR_IO;
    }
    return SIGIL_OK;
}

int sigil_sync_folder_card_removals(const sigil_sync_ctx *x, const sigil_sync_card_file *f, size_t *removes) {
    return sync_folder_card(x, f, false, removes);
}

int sigil_sync_write_folder_card(const sigil_sync_ctx *x, const sigil_sync_card_file *f) {
    size_t removes = 0;
    int rc = ensure_superblock(x, f->path);
    return rc == SIGIL_OK ? sync_folder_card(x, f, true, &removes) : rc;
}
