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
static bool system_folder(const char *name) {
    return strstr(name, "DATA-SYSTEM") || strstr(name, "BWNETCNF");
}

static bool game_or_companion(const sigil_sync_request *req, const char *name) {
    char owner[SIGIL_CARD_OWNER_MAX];
    sigil_card_sony_owner(name, owner);
    return owner[0] && (sigil_sync_owned_by_game(req, owner) || sigil_sync_companion_of(req, owner) != SIZE_MAX);
}

static bool shown_to_game(const sigil_sync_request *req, const char *name) {
    return system_folder(name) || game_or_companion(req, name);
}

#define PCSX2_INDEX "_pcsx2_index"

/* Reads the files of save folder `name` on folder card `dir` and packs them,
 * leaving its _pcsx2_index out when `skip_index`. */
static int read_save_folder(const sigil_sync_request *req, const char *dir, const char *name, bool skip_index,
                            sigil_ps2_save *out) {
    char prefix[SIGIL_SAVE_PATH_MAX];
    snprintf(prefix, sizeof(prefix), "%s/%s/", dir, name);
    size_t plen = strlen(prefix);
    sigil_ps2_folder_file *files = calloc(req->save.listing_count + 1, sizeof(*files));
    if (!files) return SIGIL_ERR_OOM;
    size_t n = 0;
    int rc = SIGIL_OK;
    for (size_t i = 0; i < req->save.listing_count && rc == SIGIL_OK; i++) {
        const char *p = req->save.listing[i];
        if (!p || strncmp(p, prefix, plen) != 0) continue;
        if (strlen(p + plen) >= PS2_FOLDER_PATH_MAX) { rc = SIGIL_ERR_UNSUPPORTED_FORMAT; break; }
        if (skip_index && strcmp(p + plen, PCSX2_INDEX) == 0) continue;
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

/* Reads save folder `name` as PCSX2 would show it. A folder whose index
 * doesn't parse but whose files do is damaged: SIGIL_ERR_DAMAGED naming the
 * index, or with repair, the folder without it. A folder of the game or a
 * companion that sigil can't pack (a subdirectory, a name too long for a card
 * entry) still shows in PCSX2, so it is damaged too, naming the folder, and
 * repair can't change that, even when its name also matches PCSX2's system
 * filter; another system folder sigil can't pack is left off
 * (SIGIL_ERR_NOT_FOUND). */
static int read_shown_folder(const sigil_sync_ctx *x, const char *dir, const char *name, sigil_sync_cards *s,
                             sigil_ps2_save *out) {
    int rc = read_save_folder(x->req, dir, name, false, out);
    if (rc != SIGIL_ERR_UNSUPPORTED_FORMAT) return rc;
    rc = read_save_folder(x->req, dir, name, true, out);
    if (rc == SIGIL_ERR_UNSUPPORTED_FORMAT) {
        if (system_folder(name) && !game_or_companion(x->req, name)) return SIGIL_ERR_NOT_FOUND;
        snprintf(s->problem, sizeof(s->problem), "%s/%s", dir, name);
        return SIGIL_ERR_DAMAGED;
    }
    if (rc != SIGIL_OK || x->req->repair) return rc;
    sigil_ps2_save_free(out);
    snprintf(s->problem, sizeof(s->problem), "%s/%s/%s", dir, name, PCSX2_INDEX);
    return SIGIL_ERR_DAMAGED;
}

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
        int read = read_shown_folder(x, dir, names[i], s, &save);
        if (read == SIGIL_ERR_NOT_FOUND) continue;
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

/* The save folder a zip member sits in and its path inside the folder, for a
 * zip of PCSX2 save folders: <folder>/<path>, or <card>.ps2/<folder>/<path>
 * when the zip was taken at the card. False for the card's own files and a
 * member outside any folder. */
static bool folder_member(const char *name, char folder[SIGIL_CARD_NAME_MAX], const char **path) {
    const char *slash = strchr(name, '/');
    if (slash && slash - name > 4 && strncmp(slash - 4, ".ps2", 4) == 0) {
        name = slash + 1;
        slash = strchr(name, '/');
    }
    if (!slash || slash == name || (size_t)(slash - name) >= SIGIL_CARD_NAME_MAX || !slash[1]) return false;
    snprintf(folder, SIGIL_CARD_NAME_MAX, "%.*s", (int)(slash - name), name);
    *path = slash + 1;
    return true;
}

int sigil_sync_folder_zip_card(const sigil_sync_ctx *x, const sigil_zip_member *members, size_t count, void **card,
                               int *format) {
    int rc = x->kind->blank(card, format, SIGIL_DEVICE_NONE, 0, SIGIL_FORM_RAW, NULL);
    sigil_ps2_folder_file *files = rc == SIGIL_OK ? calloc(count + 1, sizeof(*files)) : NULL;
    bool *done = rc == SIGIL_OK ? calloc(count + 1, sizeof(*done)) : NULL;
    if (rc == SIGIL_OK && (!files || !done)) rc = SIGIL_ERR_OOM;
    for (size_t i = 0; i < count && rc == SIGIL_OK; i++) {
        char folder[SIGIL_CARD_NAME_MAX], other[SIGIL_CARD_NAME_MAX];
        const char *path = NULL;
        if (done[i] || !folder_member(members[i].name, folder, &path)) continue;
        size_t n = 0;
        for (size_t k = i; k < count && rc == SIGIL_OK; k++) {
            const char *in = NULL;
            if (done[k] || !folder_member(members[k].name, other, &in) || strcmp(other, folder) != 0) continue;
            done[k] = true;
            if (strlen(in) >= PS2_FOLDER_PATH_MAX) { rc = SIGIL_ERR_UNSUPPORTED_FORMAT; break; }
            snprintf(files[n].path, PS2_FOLDER_PATH_MAX, "%s", in);
            files[n].data = members[k].data;
            files[n].len = members[k].len;
            n++;
        }
        sigil_ps2_save save;
        if (rc == SIGIL_OK) rc = sigil_ps2_pack(folder, files, n, &save);
        if (rc == SIGIL_OK) {
            rc = sigil_ps2_inject((sigil_ps2_card *)*card, &save);
            sigil_ps2_save_free(&save);
        }
    }
    free(files);
    free(done);
    if (rc != SIGIL_OK && *card) {
        x->kind->free_card(*card);
        *card = NULL;
    }
    return rc;
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
        if (!sigil_sync_file_holds(x->req, path, files[i].data, files[i].len)) {
            rc = sigil_sync_put(x->req, path, files[i].data, files[i].len);
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
    sigil_sync_paths doomed = { calloc(2 * x->req->save.listing_count + 1, SIGIL_SAVE_PATH_MAX), 0 };
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
    size_t files_doomed = doomed.count;
    for (size_t i = 0; rc == SIGIL_OK && i < x->req->save.listing_count; i++) {
        char name[SIGIL_CARD_NAME_MAX];
        const char *p = x->req->save.listing[i];
        if (!p || !save_folder_of_path(f->path, p, name) || !rewritten(x, name)) continue;
        bool kept = false;
        for (size_t k = 0; k < listing->entry_count && !kept; k++) kept = strcmp(listing->entries[k].name, name) == 0;
        if (!kept) snprintf(doomed.paths[doomed.count++], SIGIL_SAVE_PATH_MAX, "%s", p);
    }
    /* A dropped folder's directory goes after its files, as "dir/name/":
     * PCSX2 shows any directory its filter names, empty or not. */
    for (size_t i = files_doomed, end = doomed.count; rc == SIGIL_OK && i < end; i++) {
        char name[SIGIL_CARD_NAME_MAX], dir[SIGIL_SAVE_PATH_MAX];
        if (!save_folder_of_path(f->path, doomed.paths[i], name)) continue;
        snprintf(dir, sizeof(dir), "%s/%s/", f->path, name);
        bool listed = false;
        for (size_t k = end; k < doomed.count && !listed; k++) listed = strcmp(doomed.paths[k], dir) == 0;
        if (!listed) snprintf(doomed.paths[doomed.count++], SIGIL_SAVE_PATH_MAX, "%s", dir);
    }
    sigil_card_listing_free(listing);
    *removes = doomed.count;
    for (size_t i = 0; rc == SIGIL_OK && apply && i < doomed.count; i++) rc = sigil_sync_drop(x->req, doomed.paths[i]);
    free(doomed.paths);
    return rc;
}

static void superblock_path(const char *dir, char out[SIGIL_SAVE_PATH_MAX]) {
    snprintf(out, SIGIL_SAVE_PATH_MAX, "%s/_pcsx2_superblock", dir);
}

/* PCSX2 reads the card's _pcsx2_superblock as formatted; it hides every save
 * on a card whose superblock is missing, empty or short. */
static int read_superblock(const sigil_sync_ctx *x, const char *dir, bool *usable) {
    char path[SIGIL_SAVE_PATH_MAX];
    superblock_path(dir, path);
    uint8_t *have = NULL;
    size_t have_len = 0;
    int rc = sigil_sync_read_file(x->req, path, PS2_FOLDER_SUPERBLOCK_SIZE + 1, &have, &have_len);
    *usable = rc == SIGIL_OK && sigil_ps2_folder_superblock_usable(have, have_len);
    free(have);
    return rc == SIGIL_ERR_IO || rc == SIGIL_ERR_OOM ? rc : SIGIL_OK;
}

/* The listing holds a save folder on folder card `dir`. */
static bool holds_saves(const sigil_sync_request *req, const char *dir) {
    char name[SIGIL_CARD_NAME_MAX];
    for (size_t i = 0; i < req->save.listing_count; i++) {
        if (req->save.listing[i] && save_folder_of_path(dir, req->save.listing[i], name)) return true;
    }
    return false;
}

/* Writes a full formatted _pcsx2_superblock to folder card `dir` when the one
 * there isn't usable. */
static int ensure_superblock(const sigil_sync_ctx *x, const char *dir) {
    bool usable = false;
    int rc = read_superblock(x, dir, &usable);
    if (rc != SIGIL_OK || usable) return rc;
    char path[SIGIL_SAVE_PATH_MAX];
    superblock_path(dir, path);
    uint8_t sb[PS2_FOLDER_SUPERBLOCK_SIZE];
    sigil_ps2_folder_superblock(sb);
    return sigil_sync_put(x->req, path, sb, sizeof(sb));
}

/* A card with no saves and no usable superblock is new, and restore formats
 * it; one that holds saves behind an unusable superblock is damaged. */
int sigil_sync_check_folder_card(const sigil_sync_ctx *x, const sigil_sync_card_file *f, size_t *removes,
                                 sigil_sync_result *r) {
    *removes = 0;
    bool usable = false;
    int rc = read_superblock(x, f->path, &usable);
    if (rc != SIGIL_OK) return rc;
    if (!x->req->repair && holds_saves(x->req, f->path) && !usable) {
        superblock_path(f->path, r->problem);
        return SIGIL_ERR_DAMAGED;
    }
    return sync_folder_card(x, f, false, removes);
}

int sigil_sync_write_folder_card(const sigil_sync_ctx *x, const sigil_sync_card_file *f) {
    size_t removes = 0;
    int rc = ensure_superblock(x, f->path);
    return rc == SIGIL_OK ? sync_folder_card(x, f, true, &removes) : rc;
}
