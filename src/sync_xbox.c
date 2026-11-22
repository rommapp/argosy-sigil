// SPDX-License-Identifier: MPL-2.0
/* Collect and restore on the original Xbox. The console keeps every game's
 * saves on its hard drive, in E:\UDATA\<title id>\ and E:\TDATA\<title id>\,
 * and an emulator keeps that drive as one image file. The unit is a zip of
 * the game's two folders as they sit on E:, `UDATA/<id>/...` and
 * `TDATA/<id>/...`, each file with the time the drive has for it: the tree
 * X1 BOX's own save manager exports for a game and takes back
 * (izzy2lost/xemu at 57bc6d6, XboxSaveManager.kt exportZip and importZip).
 * Restore replaces those two folders on the drive and nothing else on it,
 * and writes the image back as the parts that changed where the request
 * can write part of a file, as the whole file where it can't. */
#include "fatx.h"
#include "sync_internal.h"

#define XBOX_MAX_MEMBER (512u * 1024u * 1024u)
#define XBOX_MAX_UNIT   0x7FFFFFFFu   /* what a zip without zip64 holds, with room for its records */
#define XBOX_MAX_DEPTH  32u
#define XBOX_ID_MAX     sizeof(((sigil_result *)0)->save_id)
#define COMPARE_CHUNK   (1u << 20)

static const char *const FOLDERS[] = { "UDATA", "TDATA" };
#define FOLDER_COUNT (sizeof(FOLDERS) / sizeof(FOLDERS[0]))

/* ---- the game's files ---------------------------------------------------------- */

typedef struct {
    sigil_zip_member *items;    /* named as in a unit */
    sigil_zip_stamp  *stamps;
    size_t            count;
    size_t            cap;
    uint64_t          bytes;
} file_list;

static void files_free(file_list *l) {
    if (l->items) sigil_zip_members_free(l->items, l->count);
    free(l->stamps);
    memset(l, 0, sizeof(*l));
}

/* Takes `data`. */
static int files_add(file_list *l, const char *name, uint8_t *data, size_t len, sigil_zip_stamp stamp) {
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        sigil_zip_member *items = (sigil_zip_member *)realloc(l->items, cap * sizeof(*items));
        if (items) l->items = items;
        sigil_zip_stamp *stamps = (sigil_zip_stamp *)realloc(l->stamps, cap * sizeof(*stamps));
        if (stamps) l->stamps = stamps;
        if (!items || !stamps) {
            free(data);
            return SIGIL_ERR_OOM;
        }
        l->cap = cap;
    }
    sigil_zip_member *m = &l->items[l->count];
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->data = data;
    m->len = len;
    l->stamps[l->count] = stamp;
    l->count++;
    l->bytes += len;
    return SIGIL_OK;
}

typedef struct {
    sigil_zip_member member;
    sigil_zip_stamp  stamp;
} sorted_file;

static int by_name(const void *a, const void *b) {
    return strcmp(((const sorted_file *)a)->member.name, ((const sorted_file *)b)->member.name);
}

/* By name in byte order, so the same saves give the same unit whatever order the drive holds them in. */
static int files_sort(file_list *l) {
    sorted_file *all = (sorted_file *)malloc((l->count ? l->count : 1) * sizeof(*all));
    if (!all) return SIGIL_ERR_OOM;
    for (size_t i = 0; i < l->count; i++) {
        all[i].member = l->items[i];
        all[i].stamp = l->stamps[i];
    }
    qsort(all, l->count, sizeof(*all), by_name);
    for (size_t i = 0; i < l->count; i++) {
        l->items[i] = all[i].member;
        l->stamps[i] = all[i].stamp;
    }
    free(all);
    return SIGIL_OK;
}

/* The hash over each file's unit name and bytes: RomM's hash of the unit. "" for no files. */
static int files_identity(const file_list *l, char out[33]) {
    out[0] = '\0';
    return l->count ? sigil_sync_members_identity(l->items, l->count, out) : SIGIL_OK;
}

/* FATX counts years from 2000, zip from 1980. */
static sigil_zip_stamp zip_stamp(const sigil_fatx_entry *e) {
    unsigned year = (e->date >> 9) + 20u;
    if (year > 127) year = 127;
    sigil_zip_stamp s = { e->time, (uint16_t)((year << 9) | (e->date & 0x1FFu)) };
    return s;
}

/* A zip date from before 2000, as sigil's own fixed one is, is the drive's earliest. */
static sigil_zip_stamp fatx_stamp(sigil_zip_stamp zip) {
    unsigned year = zip.date >> 9;
    sigil_zip_stamp s = { zip.time, (uint16_t)(((year - 20u) << 9) | (zip.date & 0x1FFu)) };
    if (year < 20 || !(zip.date & 0x1Fu) || !(zip.date & 0x1E0u)) {
        s.time = 0;
        s.date = (1u << 5) | 1u;
    }
    return s;
}

static int read_tree(sigil_fatx *fs, uint32_t dir, const char *prefix, unsigned depth, file_list *out) {
    if (depth > XBOX_MAX_DEPTH) return SIGIL_ERR_DAMAGED;
    sigil_fatx_entry *entries = NULL;
    size_t n = 0;
    int rc = sigil_fatx_list(fs, dir, &entries, &n);
    for (size_t i = 0; rc == SIGIL_OK && i < n; i++) {
        const sigil_fatx_entry *e = &entries[i];
        char name[SIGIL_SAVE_ENTRY_MAX];
        bool folder = (e->attr & SIGIL_FATX_ATTR_DIR) != 0;
        if (snprintf(name, sizeof(name), "%s%s%s", prefix, e->name, folder ? "/" : "") >= (int)sizeof(name)) {
            rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
        } else if (folder) {
            rc = read_tree(fs, e->first, name, depth + 1, out);
        } else if (e->size > XBOX_MAX_MEMBER || out->bytes + e->size > XBOX_MAX_UNIT) {
            rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
        } else {
            uint8_t *data = NULL;
            size_t len = 0;
            rc = sigil_fatx_read(fs, e, &data, &len);
            if (rc == SIGIL_OK) rc = files_add(out, name, data, len, zip_stamp(e));
        }
    }
    free(entries);
    return rc;
}

/* The directory `name` in directory `dir`; `*found` is false when there is none. */
static int find_dir(sigil_fatx *fs, uint32_t dir, const char *name, sigil_fatx_entry *out, bool *found) {
    int rc = sigil_fatx_find(fs, dir, name, out);
    *found = rc == SIGIL_OK;
    if (rc == SIGIL_ERR_NOT_FOUND) return SIGIL_OK;
    /* A file where the console keeps a folder: not a drive it wrote. */
    if (rc == SIGIL_OK && !(out->attr & SIGIL_FATX_ATTR_DIR)) return SIGIL_ERR_DAMAGED;
    return rc;
}

/* The game's folder under UDATA or TDATA; `*found` is false when the drive has none. */
static int game_dir(sigil_fatx *fs, const char *folder, const char *id, sigil_fatx_entry *out, bool *found) {
    sigil_fatx_entry top;
    int rc = find_dir(fs, sigil_fatx_root(fs), folder, &top, found);
    if (rc != SIGIL_OK || !*found) return rc;
    return find_dir(fs, top.first, id, out, found);
}

/* Every file of the game on the drive, under its unit name, sorted. */
static int game_files(sigil_fatx *fs, const char *id, file_list *out) {
    memset(out, 0, sizeof(*out));
    int rc = SIGIL_OK;
    for (size_t f = 0; rc == SIGIL_OK && f < FOLDER_COUNT; f++) {
        sigil_fatx_entry dir;
        bool found = false;
        rc = game_dir(fs, FOLDERS[f], id, &dir, &found);
        if (rc != SIGIL_OK || !found) continue;
        char prefix[SIGIL_SAVE_ENTRY_MAX];
        snprintf(prefix, sizeof(prefix), "%s/%s/", FOLDERS[f], id);
        rc = read_tree(fs, dir.first, prefix, 0, out);
    }
    if (rc == SIGIL_OK) rc = files_sort(out);
    if (rc != SIGIL_OK) files_free(out);
    return rc;
}

/* ---- the drive ------------------------------------------------------------------ */

typedef struct {
    char        path[SIGIL_SAVE_PATH_MAX];
    sigil_io   *io;
    sigil_disk *disk;
    sigil_fatx *fs;
} drive;

static void drive_close(drive *v) {
    sigil_fatx_close(v->fs);
    sigil_disk_close(v->disk);
    if (v->io) sigil_io_close(v->io);
    v->fs = NULL;
    v->disk = NULL;
    v->io = NULL;
}

/* Opens `path` as a drive. A file that isn't one, or whose tables don't hold
 * together, is damaged; `problem` names it. */
static int drive_open_path(const sigil_sync_request *req, const char *path, drive *v, char problem[SIGIL_SAVE_PATH_MAX]) {
    memset(v, 0, sizeof(*v));
    snprintf(v->path, sizeof(v->path), "%s", path);
    v->io = req->save.open(req->save.open_ctx, path);
    if (!v->io) return SIGIL_ERR_IO;
    int rc = sigil_disk_open(v->io, &v->disk);
    if (rc == SIGIL_OK) rc = sigil_fatx_open(v->disk, &v->fs);
    if (rc == SIGIL_ERR_DAMAGED && problem) snprintf(problem, SIGIL_SAVE_PATH_MAX, "%s", path);
    if (rc != SIGIL_OK) drive_close(v);
    return rc;
}

/* The drive the layout row names; v->fs stays NULL when the save root holds
 * none. SIGIL_ERR_INVALID_ARG when the row can't name one: xemu's, with no
 * hdd_path in the request. */
static int drive_open(const sigil_sync_ctx *x, drive *v, sigil_sync_result *r) {
    memset(v, 0, sizeof(*v));
    char shared[SYNC_MAX_SHARED][SIGIL_SAVE_PATH_MAX];
    if (!sigil_save_shared_paths(&x->req->save, shared, NULL, SYNC_MAX_SHARED)) return SIGIL_ERR_INVALID_ARG;
    if (!sigil_sync_listed(x->req, shared[0])) {
        snprintf(v->path, sizeof(v->path), "%s", shared[0]);
        return SIGIL_OK;
    }
    return drive_open_path(x->req, shared[0], v, r->problem);
}

/* The folder the console names after the game: the title id as eight
 * lowercase hex digits. The kernel spells it so (TDATA/454d0009 on
 * flatout-cabinetos, which the game made), and X1 BOX imports into the
 * lowercase name (XboxSaveManager.kt importZip). */
static int game_id(const sigil_sync_ctx *x, char id[XBOX_ID_MAX]) {
    const sigil_result *game = x->req->save.result;
    if (!game || !game->save_id[0]) return SIGIL_ERR_INVALID_ARG;
    size_t n = 0;
    for (; game->save_id[n] && n + 1 < XBOX_ID_MAX; n++) {
        char c = game->save_id[n];
        id[n] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    id[n] = '\0';
    return sigil_fatx_name_ok(id) ? SIGIL_OK : SIGIL_ERR_INVALID_ARG;
}

static void artifact_of(const sigil_sync_ctx *x, sigil_sync_result *r) {
    snprintf(r->artifact, sizeof(r->artifact), "%s.zip", x->req->save.result->save_id);
    r->shape = SIGIL_SAVE_SHAPE_FOLDER;
}

/* ---- collect ------------------------------------------------------------------- */

int sigil_sync_collect_xbox(sigil_sync_ctx *x, sigil_sync_result *r) {
    char id[XBOX_ID_MAX];
    drive v;
    int rc = game_id(x, id);
    if (rc == SIGIL_OK) rc = drive_open(x, &v, r);
    if (rc != SIGIL_OK) return rc;
    file_list files;
    memset(&files, 0, sizeof(files));
    if (v.fs) {
        rc = game_files(v.fs, id, &files);
        if (rc == SIGIL_ERR_DAMAGED) snprintf(r->problem, sizeof(r->problem), "%s", v.path);
    }
    drive_close(&v);
    if (rc == SIGIL_OK && files.count) {
        rc = sigil_zip_store(files.items, files.count, &r->data, &r->len);
        if (rc == SIGIL_OK) rc = sigil_zip_set_stamps(r->data, r->len, files.stamps, files.count);
        if (rc == SIGIL_OK) rc = files_identity(&files, r->identity_hash);
        if (rc == SIGIL_OK) rc = sigil_sync_members_hash(files.items, files.count, r->content_hash);
        artifact_of(x, r);
    }
    files_free(&files);
    return rc;
}

/* ---- restore ------------------------------------------------------------------- */

/* The first `n` characters of `a` and `b` are the same, compared without case as FATX compares names. */
static bool same_start(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y || !x) return false;
    }
    return true;
}

/* The start of the path segment after `segment` when `name` begins with it; else NULL. */
static const char *after_segment(const char *name, const char *segment) {
    size_t n = strlen(segment);
    return same_start(name, segment, n) && name[n] == '/' ? name + n + 1 : NULL;
}

/* Every path segment of `rest` can be a FATX name, and it names a file. */
static bool path_ok(const char *rest) {
    char segment[SIGIL_SAVE_ENTRY_MAX];
    unsigned depth = 0;
    for (const char *s = rest;; depth++) {
        size_t n = strcspn(s, "/");
        memcpy(segment, s, n);
        segment[n] = '\0';
        if (!sigil_fatx_name_ok(segment) || depth > XBOX_MAX_DEPTH) return false;
        if (!s[n]) return true;
        s += n + 1;
    }
}

/* One of two names is the other, or a folder the other lies in, compared without case. */
static bool same_place(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    const char *shorter = na <= nb ? a : b, *longer = na <= nb ? b : a;
    size_t n = na <= nb ? na : nb;
    return same_start(shorter, longer, n) && (longer[n] == '\0' || longer[n] == '/');
}

/* The unit's files that are the game's, under the names collect gives them;
 * `content` gets RomM's hash of the unit as it came. Files of another game
 * are left out. A member FATX can't name is SIGIL_ERR_NO_TARGET, naming it;
 * two members for one place are SIGIL_ERR_UNSUPPORTED_FORMAT. */
static int read_unit(const uint8_t *unit, size_t len, const char *id, file_list *out, char content[33],
                     sigil_sync_result *r) {
    memset(out, 0, sizeof(*out));
    sigil_zip_member *members = NULL;
    sigil_zip_stamp *stamps = NULL;
    size_t count = 0, stamp_count = 0;
    int rc = sigil_zip_read_mem(unit, len, XBOX_MAX_MEMBER, &members, &count);
    if (rc == SIGIL_OK) rc = sigil_zip_get_stamps(unit, len, &stamps, &stamp_count);
    if (rc == SIGIL_OK && stamp_count != count) rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
    char hash[33] = "";
    if (rc == SIGIL_OK && count) rc = sigil_sync_members_hash(members, count, hash);
    for (size_t i = 0; rc == SIGIL_OK && i < count; i++) {
        sigil_zip_member *m = &members[i];
        char path[sizeof(m->name)];
        snprintf(path, sizeof(path), "%s", m->name);
        for (char *c = path; *c; c++) {
            if (*c == '\\') *c = '/';
        }
        for (size_t f = 0; rc == SIGIL_OK && f < FOLDER_COUNT; f++) {
            const char *in_folder = after_segment(path, FOLDERS[f]);
            const char *rest = in_folder ? after_segment(in_folder, id) : NULL;
            if (!rest) continue;
            char name[SIGIL_SAVE_ENTRY_MAX];
            if (!path_ok(rest) || snprintf(name, sizeof(name), "%s/%s/%s", FOLDERS[f], id, rest) >= (int)sizeof(name)) {
                snprintf(r->problem, sizeof(r->problem), "%s", m->name);
                rc = SIGIL_ERR_NO_TARGET;
                break;
            }
            for (size_t k = 0; k < out->count; k++) {
                if (same_place(out->items[k].name, name)) rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
            }
            if (rc != SIGIL_OK) break;
            rc = files_add(out, name, m->data, m->len, stamps[i]);
            m->data = NULL;
            break;
        }
    }
    if (rc == SIGIL_OK) {
        memcpy(content, hash, sizeof(hash));
        if (!out->count) rc = SIGIL_ERR_NOT_FOUND;
    }
    free(stamps);
    if (members) sigil_zip_members_free(members, count);
    if (rc == SIGIL_OK) rc = files_sort(out);
    if (rc != SIGIL_OK) files_free(out);
    return rc;
}

/* The newest stamp among the files under folder `prefix`: the stamp the folder gets. */
static sigil_zip_stamp newest_under(const file_list *in, const char *prefix) {
    sigil_zip_stamp best = { 0, 0 };
    size_t n = strlen(prefix);
    for (size_t i = 0; i < in->count; i++) {
        sigil_zip_stamp s = in->stamps[i];
        if (strncmp(in->items[i].name, prefix, n) != 0) continue;
        if (s.date > best.date || (s.date == best.date && s.time > best.time)) best = s;
    }
    return fatx_stamp(best);
}

typedef struct {
    char     path[SIGIL_SAVE_ENTRY_MAX];   /* as a unit names it, ending in '/' */
    uint32_t cluster;
} made_dir;

typedef struct {
    sigil_fatx *fs;
    made_dir   *dirs;
    size_t      count;
    size_t      cap;
} placer;

/* The cluster of folder `path`, which ends in '/': made when it isn't there
 * yet, in its parent, which is. A folder spelled two ways in one unit is
 * SIGIL_ERR_UNSUPPORTED_FORMAT, since the drive holds it once. */
static int place_dir(placer *p, const file_list *in, const char *path, uint32_t *cluster) {
    for (size_t i = 0; i < p->count; i++) {
        if (strcasecmp(p->dirs[i].path, path) != 0) continue;
        if (strcmp(p->dirs[i].path, path) != 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
        *cluster = p->dirs[i].cluster;
        return SIGIL_OK;
    }
    char parent[SIGIL_SAVE_ENTRY_MAX], name[SIGIL_SAVE_ENTRY_MAX];
    size_t n = strlen(path) - 1;
    size_t cut = n;
    while (cut > 0 && path[cut - 1] != '/') cut--;
    memcpy(parent, path, cut);
    parent[cut] = '\0';
    memcpy(name, path + cut, n - cut);
    name[n - cut] = '\0';
    uint32_t in_dir = sigil_fatx_root(p->fs);
    int rc = cut ? place_dir(p, in, parent, &in_dir) : SIGIL_OK;
    if (rc != SIGIL_OK) return rc;
    sigil_fatx_entry there;
    bool found = false;
    rc = find_dir(p->fs, in_dir, name, &there, &found);
    if (rc != SIGIL_OK) return rc;
    if (found) {
        *cluster = there.first;
    } else {
        sigil_zip_stamp s = newest_under(in, path);
        rc = sigil_fatx_mkdir(p->fs, in_dir, name, s.time, s.date, cluster);
        if (rc != SIGIL_OK) return rc;
    }
    if (p->count == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 16;
        made_dir *grown = (made_dir *)realloc(p->dirs, cap * sizeof(*grown));
        if (!grown) return SIGIL_ERR_OOM;
        p->dirs = grown;
        p->cap = cap;
    }
    snprintf(p->dirs[p->count].path, sizeof(p->dirs[p->count].path), "%s", path);
    p->dirs[p->count].cluster = *cluster;
    p->count++;
    return SIGIL_OK;
}

/* Stages the game's two folders as the unit has them: what the drive holds
 * of them is taken out, then each file of the unit is written, its folders
 * made as they come, and the flush puts the new folders where the old ones
 * were and frees the old ones (fatx.h has the order). Nothing else on the
 * drive is touched. SIGIL_ERR_NO_SPACE, naming the file that didn't fit and
 * the clusters it lacked, when E: is too full: written in parts, the unit's
 * files need room beside the ones they replace. */
static int stage_game(sigil_fatx *fs, const char *id, const file_list *in, sigil_sync_result *r) {
    int rc = SIGIL_OK;
    for (size_t f = 0; rc == SIGIL_OK && f < FOLDER_COUNT; f++) {
        /* A drive some tool wrote can hold the folder twice, in two spellings: both go. */
        for (unsigned pass = 0; rc == SIGIL_OK && pass < 16; pass++) {
            sigil_fatx_entry dir;
            bool found = false;
            rc = game_dir(fs, FOLDERS[f], id, &dir, &found);
            if (rc != SIGIL_OK || !found) break;
            rc = sigil_fatx_remove(fs, &dir);
        }
    }
    placer p = { fs, NULL, 0, 0 };
    uint32_t cluster_size = sigil_fatx_cluster_size(fs);
    for (size_t i = 0; rc == SIGIL_OK && i < in->count; i++) {
        const sigil_zip_member *m = &in->items[i];
        char folder[SIGIL_SAVE_ENTRY_MAX];
        const char *base = strrchr(m->name, '/') + 1;
        memcpy(folder, m->name, (size_t)(base - m->name));
        folder[base - m->name] = '\0';
        uint32_t dir = 0;
        uint32_t free_before = sigil_fatx_free_clusters(fs);
        rc = place_dir(&p, in, folder, &dir);
        sigil_zip_stamp s = fatx_stamp(in->stamps[i]);
        if (rc == SIGIL_OK) rc = sigil_fatx_put(fs, dir, base, m->data, m->len, s.time, s.date);
        if (rc == SIGIL_ERR_NO_SPACE) {
            uint64_t need = (m->len + cluster_size - 1) / cluster_size;
            snprintf(r->problem, sizeof(r->problem), "%s", m->name);
            r->blocks_short = need > free_before ? (uint32_t)(need - free_before) : 0;
        }
    }
    free(p.dirs);
    if (rc == SIGIL_OK) rc = sigil_fatx_flush(fs);
    return rc;
}

/* The file at `path` holds exactly `data`, read through a fresh open. */
static bool file_holds(const sigil_sync_request *req, const char *path, const uint8_t *data, size_t len) {
    sigil_io *io = req->save.open(req->save.open_ctx, path);
    if (!io) return false;
    uint8_t *chunk = (uint8_t *)malloc(COMPARE_CHUNK);
    bool same = chunk && io->size && io->size(io->ctx) == (int64_t)len;
    for (size_t at = 0; same && at < len;) {
        size_t n = len - at < COMPARE_CHUNK ? len - at : COMPARE_CHUNK, got = 0;
        same = sigil_io_read_upto(io, at, chunk, n, &got) == SIGIL_OK && got == n && memcmp(chunk, data + at, n) == 0;
        at += n;
    }
    free(chunk);
    sigil_io_close(io);
    return same;
}

/* The drive at `path`, opened afresh, holds the game's saves with identity `identity`. */
static int verify_drive(const sigil_sync_request *req, const char *path, const char *id, const char *identity) {
    drive v;
    file_list files;
    char found[33];
    int rc = drive_open_path(req, path, &v, NULL);
    if (rc == SIGIL_OK) rc = game_files(v.fs, id, &files);
    if (rc == SIGIL_OK) {
        rc = files_identity(&files, found);
        files_free(&files);
    }
    drive_close(&v);
    return rc == SIGIL_OK && strcmp(found, identity) == 0 ? SIGIL_OK : SIGIL_ERR_IO;
}

typedef struct {
    const sigil_sync_request *req;
    const char               *path;
    sigil_io                 *io;      /* the written file, when reading it back */
    uint8_t                  *chunk;
} part_writer;

static int write_part(void *ctx, uint64_t off, const uint8_t *data, size_t len) {
    part_writer *w = (part_writer *)ctx;
    return sigil_sync_write_at(w->req)(w->req->write_ctx, w->path, off, data, len) == 0 ? SIGIL_OK : SIGIL_ERR_IO;
}

static int check_part(void *ctx, uint64_t off, const uint8_t *data, size_t len) {
    part_writer *w = (part_writer *)ctx;
    size_t got = 0;
    bool same = sigil_io_read_upto(w->io, off, w->chunk, len, &got) == SIGIL_OK && got == len &&
                memcmp(w->chunk, data, len) == 0;
    return same ? SIGIL_OK : SIGIL_ERR_IO;
}

/* Writes the parts of the drive the staged changes touch, then reads each
 * back through a fresh open. */
static int write_parts(const sigil_sync_ctx *x, drive *v) {
    part_writer w = { x->req, v->path, NULL, NULL };
    int rc = sigil_disk_runs(v->disk, true, write_part, &w);
    if (rc != SIGIL_OK) return SIGIL_ERR_IO;
    w.io = x->req->save.open(x->req->save.open_ctx, v->path);
    w.chunk = (uint8_t *)malloc(COMPARE_CHUNK);
    rc = w.io && w.chunk ? sigil_disk_runs(v->disk, false, check_part, &w) : SIGIL_ERR_IO;
    free(w.chunk);
    if (w.io) sigil_io_close(w.io);
    return rc == SIGIL_OK ? SIGIL_OK : SIGIL_ERR_IO;
}

/* Writes the drive with its staged changes through the request, then reads
 * it back: what was written, and the game's saves on it. A request that can
 * write part of a file gets only the parts that changed; any other gets the
 * whole file. The drive's stream is closed first, since a client may replace
 * or reopen the file it had open. */
static int write_drive(const sigil_sync_ctx *x, drive *v, const char *id, const char *identity) {
    int rc = sigil_sync_path_inside(v->path) ? SIGIL_OK : SIGIL_ERR_IO;
    if (rc == SIGIL_OK && sigil_sync_write_at(x->req)) {
        sigil_fatx_close(v->fs);
        v->fs = NULL;
        sigil_disk_detach(v->disk);
        sigil_io_close(v->io);
        v->io = NULL;
        rc = write_parts(x, v);
        drive_close(v);
    } else if (rc == SIGIL_OK) {
        uint8_t *file = NULL;
        size_t len = 0;
        rc = sigil_disk_file(v->disk, &file, &len);
        drive_close(v);
        if (rc == SIGIL_OK && (x->req->write(x->req->write_ctx, v->path, file, len) != 0 ||
                               !file_holds(x->req, v->path, file, len))) {
            rc = SIGIL_ERR_IO;
        }
        free(file);
    }
    if (rc == SIGIL_OK) rc = verify_drive(x->req, v->path, id, identity);
    return rc;
}

int sigil_sync_restore_xbox(sigil_sync_ctx *x, const uint8_t *unit, size_t len, sigil_sync_result *r,
                            char local_identity[33]) {
    char id[XBOX_ID_MAX];
    file_list incoming, local, staged;
    drive v;
    memset(&local, 0, sizeof(local));
    memset(&v, 0, sizeof(v));
    int rc = game_id(x, id);
    if (rc == SIGIL_OK) rc = drive_open(x, &v, r);
    if (rc == SIGIL_OK && !v.fs) {
        /* sigil makes no drive: the console's own files and its other
         * partitions are the user's. Said before the unit is read, so a
         * restore into an empty root always names the drive it needs. */
        snprintf(r->problem, sizeof(r->problem), "%s", v.path);
        rc = SIGIL_ERR_NO_TARGET;
    }
    if (rc != SIGIL_OK) return rc;
    rc = read_unit(unit, len, id, &incoming, r->content_hash, r);
    if (rc != SIGIL_OK) {
        drive_close(&v);
        return rc;
    }
    rc = files_identity(&incoming, r->identity_hash);
    artifact_of(x, r);
    if (rc == SIGIL_OK) rc = game_files(v.fs, id, &local);
    if (rc == SIGIL_ERR_DAMAGED) snprintf(r->problem, sizeof(r->problem), "%s", v.path);
    if (rc == SIGIL_OK) rc = files_identity(&local, local_identity);
    files_free(&local);
    bool same = false;
    if (rc == SIGIL_OK && sigil_sync_blocks_restore(x, local_identity, r->identity_hash,
                                                    sigil_sync_state_get(&x->state, "synced", x->game), &same)) {
        r->conflict = 1;
        rc = SIGIL_ERR_CONFLICT;
    }
    if (rc == SIGIL_OK && !same) {
        /* Written back whole, in one write no stop shows half done, the new
         * saves may take the old ones' clusters; written in parts, they may not. */
        if (!sigil_sync_write_at(x->req)) sigil_fatx_free_at_once(v.fs);
        rc = stage_game(v.fs, id, &incoming, r);
        if (rc == SIGIL_ERR_DAMAGED) snprintf(r->problem, sizeof(r->problem), "%s", v.path);
        /* What is staged must read back as the unit before any of it is written. */
        if (rc == SIGIL_OK) rc = game_files(v.fs, id, &staged);
        if (rc == SIGIL_OK) {
            char found[33];
            rc = files_identity(&staged, found);
            files_free(&staged);
            if (rc == SIGIL_OK && strcmp(found, r->identity_hash) != 0) rc = SIGIL_ERR_IO;
        }
        if (rc == SIGIL_OK) rc = write_drive(x, &v, id, r->identity_hash);
    }
    drive_close(&v);
    files_free(&incoming);
    return rc;
}
