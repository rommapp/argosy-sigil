// SPDX-License-Identifier: MPL-2.0
/* Collect and restore for N64 cartridges. The unit is the neutral form, a
 * zip of the regions the game uses (n64_save.h); the files on disk are the
 * emulator's own: the libretro cores' one .srm, mupen64plus's .eep, .sra,
 * .fla and four-pak .mpk, or Project64's with a .mpk per controller. */
#include "sync_internal.h"
#include "n64_save.h"
#include <ctype.h>

/* A .srm with a 64DD disk appended (parallel_n64) */
#define N64_MAX_FILE (N64_BLOB_SIZE + 0x4400000u)

/* What one emulator file holds. */
typedef enum { FILE_OTHER = 0, FILE_BLOB, FILE_REGION, FILE_PAKS } file_kind;

typedef struct {
    char path[SIGIL_SAVE_PATH_MAX];   /* "" when the emulator keeps no such file for the game */
    bool present;
} n64_file;

/* The game's files on disk, one slot per kind: the .srm, each region's own
 * file, and mupen64plus's four-pak .mpk. */
typedef struct {
    n64_file blob;
    n64_file region[N64_REGION_COUNT];
    n64_file paks;
    bool     per_controller;   /* Project64: a .mpk per controller, <name>_Cont_<n>.mpk */
    char     base[SIGIL_SAVE_PATH_MAX];   /* a present file's path without its extension, or "" */
} n64_files;

static bool ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), m = strlen(tail);
    if (n < m) return false;
    for (size_t i = 0; i < m; i++) {
        if (tolower((unsigned char)s[n - m + i]) != tolower((unsigned char)tail[i])) return false;
    }
    return true;
}

/* The pak a Project64 <name>_Cont_<n>.mpk holds, or -1. */
static int controller_of(const char *path) {
    size_t n = strlen(path);
    if (n < 11 || !ends_with(path, ".mpk") || strncmp(path + n - 11, "_Cont_", 6) != 0) return -1;
    char c = path[n - 5];
    return c >= '1' && c <= '4' ? N64_PAK1 + (c - '1') : -1;
}

static file_kind kind_of(const char *path, int *region) {
    *region = controller_of(path);
    if (*region >= 0) return FILE_REGION;
    if (ends_with(path, ".srm")) return FILE_BLOB;
    if (ends_with(path, ".mpk")) return FILE_PAKS;
    *region = ends_with(path, ".eep") ? N64_EEPROM : ends_with(path, ".sra") ? N64_SRAM
            : ends_with(path, ".fla") ? N64_FLASH : -1;
    return *region >= 0 ? FILE_REGION : FILE_OTHER;
}

static n64_file *slot_for(n64_files *f, file_kind kind, int region) {
    if (kind == FILE_BLOB) return &f->blob;
    if (kind == FILE_PAKS) return &f->paks;
    return kind == FILE_REGION ? &f->region[region] : NULL;
}

/* The game's files the layout names: present ones from the listing, and the
 * path of each the emulator would create where sigil can spell it. Two
 * present files of one kind are SIGIL_ERR_AMBIGUOUS, both in `problem`. */
static int files_of(const sigil_sync_request *req, n64_files *f, char problem[SIGIL_SAVE_PATH_MAX]) {
    memset(f, 0, sizeof(*f));
    f->per_controller = req->save.layout && strcmp(req->save.layout, "project64") == 0;
    sigil_save_unit *unit = NULL;
    int rc = sigil_save_resolve(&req->save, &unit);
    if (rc != SIGIL_OK) return rc;
    for (size_t i = 0; i < unit->member_count + unit->expected_count && rc == SIGIL_OK; i++) {
        bool present = i < unit->member_count;
        const sigil_save_member *m = present ? &unit->members[i] : &unit->expected[i - unit->member_count];
        int region = -1;
        n64_file *slot = slot_for(f, kind_of(m->path, &region), region);
        if (!slot) continue;
        if (slot->present && present) {
            snprintf(problem, SIGIL_SAVE_PATH_MAX, "%s\n%s", slot->path, m->path);
            rc = SIGIL_ERR_AMBIGUOUS;
        } else if (!slot->present) {
            snprintf(slot->path, sizeof(slot->path), "%s", m->path);
            slot->present = present;
        }
        if (present && !f->base[0]) {
            const char *dot = strrchr(m->path, '.');
            int cut = controller_of(m->path) >= 0 ? 11 : dot ? (int)strlen(dot) : 0;
            snprintf(f->base, sizeof(f->base), "%.*s", (int)strlen(m->path) - cut, m->path);
        }
    }
    sigil_save_unit_free(unit);
    return rc;
}

static int read_into(const sigil_sync_request *req, const char *path, uint8_t **data, size_t *len) {
    int rc = sigil_sync_read_file(req, path, N64_MAX_FILE, data, len);
    return rc == SIGIL_ERR_NOT_FOUND ? SIGIL_ERR_IO : rc;
}

/* The game's save as the files on disk hold it. */
static int read_local(const sigil_sync_request *req, const n64_files *f, n64_save *s) {
    memset(s, 0, sizeof(*s));
    s->eeprom_len = N64_EEPROM_4K;
    uint8_t *data = NULL;
    size_t len = 0;
    int rc = SIGIL_OK;
    if (f->blob.present) {
        rc = read_into(req, f->blob.path, &data, &len);
        if (rc == SIGIL_OK) n64_take_blob(s, data, len);
        free(data);
    }
    if (rc == SIGIL_OK && f->paks.present) {
        data = NULL;
        rc = read_into(req, f->paks.path, &data, &len);
        for (size_t p = 0; rc == SIGIL_OK && p < N64_PAKS; p++) {
            size_t at = p * N64_PAK_SIZE;
            n64_take(s, N64_PAK1 + (int)p, data + (len > at ? at : 0), len > at ? len - at : 0);
        }
        free(data);
    }
    for (int r = 0; r < N64_REGION_COUNT && rc == SIGIL_OK; r++) {
        if (!f->region[r].present) continue;
        data = NULL;
        rc = read_into(req, f->region[r].path, &data, &len);
        if (rc == SIGIL_OK) n64_take(s, r, data, len);
        free(data);
    }
    return rc;
}

/* The neutral unit's members and RomM's hash over them; `members` borrow `s`. */
static size_t neutral_members(n64_save *s, sigil_zip_member members[N64_REGION_COUNT], char identity[33]) {
    sigil_named_md5 parts[N64_REGION_COUNT];
    size_t n = 0;
    for (int r = 0; r < N64_REGION_COUNT; r++) {
        if (!s->present[r]) continue;
        snprintf(members[n].name, sizeof(members[n].name), "%s", n64_region_name(r));
        members[n].data = n64_region_bytes(s, r, &members[n].len);
        snprintf(parts[n].name, sizeof(parts[n].name), "%s", members[n].name);
        sigil_md5_of(members[n].data, members[n].len, parts[n].md5);
        n++;
    }
    identity[0] = '\0';
    if (n) sigil_named_hash(parts, n, identity);
    return n;
}

static void artifact_of(const sigil_sync_request *req, sigil_sync_result *r) {
    char stem[SIGIL_SAVE_ENTRY_MAX];
    sigil_content_stem(req->save.content_path, stem, sizeof(stem));
    snprintf(r->artifact, sizeof(r->artifact), "%s.zip", stem);
}

int sigil_sync_collect_n64(sigil_sync_ctx *x, sigil_sync_result *r) {
    n64_files f;
    int rc = files_of(x->req, &f, r->problem);
    n64_save *s = rc == SIGIL_OK ? (n64_save *)calloc(1, sizeof(*s)) : NULL;
    if (rc == SIGIL_OK && !s) rc = SIGIL_ERR_OOM;
    if (rc == SIGIL_OK) rc = read_local(x->req, &f, s);
    sigil_zip_member members[N64_REGION_COUNT];
    size_t n = rc == SIGIL_OK ? neutral_members(s, members, r->identity_hash) : 0;
    if (n) {
        rc = sigil_zip_store(members, n, &r->data, &r->len);
        memcpy(r->content_hash, r->identity_hash, sizeof(r->content_hash));
        r->shape = SIGIL_SAVE_SHAPE_MULTI;
        artifact_of(x->req, r);
    }
    free(s);
    return rc;
}

/* ---- restore ------------------------------------------------------------------ */

static int take_member(n64_save *s, const sigil_zip_member *m) {
    int region = n64_region_of(m->name);
    if (region >= 0) return n64_take_neutral(s, region, m->data, m->len) ? SIGIL_OK : SIGIL_ERR_UNSUPPORTED_FORMAT;
    switch (kind_of(m->name, &region)) {
    case FILE_BLOB:
        n64_take_blob(s, m->data, m->len);
        return SIGIL_OK;
    case FILE_PAKS:
        for (size_t p = 0; p < N64_PAKS; p++) {
            size_t at = p * N64_PAK_SIZE;
            n64_take(s, N64_PAK1 + (int)p, m->data + (m->len > at ? at : 0), m->len > at ? m->len - at : 0);
        }
        return SIGIL_OK;
    case FILE_REGION:
        n64_take(s, region, m->data, m->len);
        return SIGIL_OK;
    default:
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
}

/* The save a unit carries: the neutral zip, a zip of an emulator's files,
 * a libretro .srm, or a lone EEPROM file. Sets RomM's content hash: over
 * the members for a zip, over the `received_len` bytes as they came otherwise. */
static int read_unit(const uint8_t *unit, size_t len, size_t received_len, n64_save *s, sigil_sync_result *r) {
    memset(s, 0, sizeof(*s));
    s->eeprom_len = N64_EEPROM_4K;
    if (len >= 4 && sigil_read_le32(unit) == 0x04034b50u) {
        sigil_zip_member *members = NULL;
        size_t count = 0;
        int rc = sigil_zip_read_mem(unit, len, N64_MAX_FILE, &members, &count);
        sigil_named_md5 *parts = rc == SIGIL_OK ? (sigil_named_md5 *)calloc(count ? count : 1, sizeof(*parts)) : NULL;
        if (rc == SIGIL_OK && !parts) rc = SIGIL_ERR_OOM;
        for (size_t i = 0; i < count && rc == SIGIL_OK; i++) {
            snprintf(parts[i].name, sizeof(parts[i].name), "%s", members[i].name);
            sigil_md5_of(members[i].data, members[i].len, parts[i].md5);
            rc = take_member(s, &members[i]);
            if (rc != SIGIL_OK) snprintf(r->problem, sizeof(r->problem), "%s", members[i].name);
        }
        if (rc == SIGIL_OK && count) sigil_named_hash(parts, count, r->content_hash);
        free(parts);
        if (members) sigil_zip_members_free(members, count);
        return rc;
    }
    sigil_md5_of(unit, received_len, r->content_hash);
    if (len >= N64_BLOB_SIZE) n64_take_blob(s, unit, len);
    else if (len == N64_EEPROM_4K || len == N64_EEPROM_MAX) n64_take(s, N64_EEPROM, unit, len);
    else return SIGIL_ERR_UNSUPPORTED_FORMAT;
    return SIGIL_OK;
}

static bool any_present(const n64_save *s, int first, int last) {
    for (int r = first; r <= last; r++) {
        if (s->present[r]) return true;
    }
    return false;
}

/* A file restore writes or removes. */
typedef struct {
    char     path[SIGIL_SAVE_PATH_MAX];
    uint8_t *data;   /* NULL: remove the file */
    size_t   len;
} n64_write;

typedef struct {
    n64_write items[N64_REGION_COUNT + 2];
    size_t    count;
} n64_writes;

static void writes_free(n64_writes *w) {
    for (size_t i = 0; i < w->count; i++) free(w->items[i].data);
}

/* The path of a file restore creates: the layout's own, else `base` with `tail`. */
static bool new_path(const n64_file *file, const char *base, const char *tail, char out[SIGIL_SAVE_PATH_MAX]) {
    if (file->path[0]) return snprintf(out, SIGIL_SAVE_PATH_MAX, "%s", file->path) < SIGIL_SAVE_PATH_MAX;
    return base[0] && snprintf(out, SIGIL_SAVE_PATH_MAX, "%s%s", base, tail) < SIGIL_SAVE_PATH_MAX;
}

static int add_write(n64_writes *w, const char *path, uint8_t *data, size_t len) {
    n64_write *item = &w->items[w->count++];
    snprintf(item->path, sizeof(item->path), "%s", path);
    item->data = data;
    item->len = len;
    return SIGIL_OK;
}

/* One file holding `region` of `s`, or its removal when `s` lacks it and the file is there. */
static int plan_region(n64_writes *w, const n64_files *f, const n64_save *s, int region, const char *tail) {
    const n64_file *file = &f->region[region];
    if (!s->present[region]) return file->present ? add_write(w, file->path, NULL, 0) : SIGIL_OK;
    char path[SIGIL_SAVE_PATH_MAX];
    if (!new_path(file, f->base, tail, path)) return SIGIL_ERR_NO_TARGET;
    uint8_t *data = (uint8_t *)malloc(N64_FLASH_SIZE);
    if (!data) return SIGIL_ERR_OOM;
    return add_write(w, path, data, n64_give(s, region, data));
}

/* mupen64plus's .mpk: the unit's paks, and for each it lacks the pak already
 * there when the game never used it, else a formatted one. */
static int plan_paks(const sigil_sync_request *req, n64_writes *w, const n64_files *f, const n64_save *s,
                     const n64_save *local) {
    if (!any_present(s, N64_PAK1, N64_PAK4)) return f->paks.present ? add_write(w, f->paks.path, NULL, 0) : SIGIL_OK;
    char path[SIGIL_SAVE_PATH_MAX];
    if (!new_path(&f->paks, f->base, ".mpk", path)) return SIGIL_ERR_NO_TARGET;
    uint8_t *data = (uint8_t *)malloc(N64_PAKS * N64_PAK_SIZE);
    uint8_t *old = NULL;
    size_t old_len = 0;
    int rc = data ? SIGIL_OK : SIGIL_ERR_OOM;
    if (rc == SIGIL_OK && f->paks.present) rc = read_into(req, f->paks.path, &old, &old_len);
    for (int p = 0; p < (int)N64_PAKS && rc == SIGIL_OK; p++) {
        uint8_t *at = data + (size_t)p * N64_PAK_SIZE;
        bool keep = old && old_len >= (size_t)(p + 1) * N64_PAK_SIZE && !local->present[N64_PAK1 + p];
        if (s->present[N64_PAK1 + p]) n64_give(s, N64_PAK1 + p, at);
        else if (keep) memcpy(at, old + (size_t)p * N64_PAK_SIZE, N64_PAK_SIZE);
        else n64_pak_formatted(at);
    }
    free(old);
    if (rc != SIGIL_OK) {
        free(data);
        return rc;
    }
    return add_write(w, path, data, N64_PAKS * N64_PAK_SIZE);
}

/* The libretro .srm: the unit's regions over what is there, a 64DD disk after it kept. */
static int plan_blob(const sigil_sync_request *req, n64_writes *w, const n64_files *f, const n64_save *s) {
    if (!f->blob.path[0]) return SIGIL_ERR_NO_TARGET;
    uint8_t *old = NULL;
    size_t old_len = 0;
    int rc = f->blob.present ? read_into(req, f->blob.path, &old, &old_len) : SIGIL_OK;
    size_t len = old_len > N64_BLOB_SIZE ? old_len : N64_BLOB_SIZE;
    uint8_t *data = rc == SIGIL_OK ? (uint8_t *)malloc(len) : NULL;
    if (rc == SIGIL_OK && !data) rc = SIGIL_ERR_OOM;
    if (rc == SIGIL_OK) rc = n64_give_blob(s, old, old_len, data);
    if (rc == SIGIL_OK && len > N64_BLOB_SIZE) memcpy(data + N64_BLOB_SIZE, old + N64_BLOB_SIZE, len - N64_BLOB_SIZE);
    free(old);
    if (rc != SIGIL_OK) {
        free(data);
        return rc;
    }
    return add_write(w, f->blob.path, data, len);
}

static int plan(const sigil_sync_request *req, n64_writes *w, const n64_files *f, const n64_save *s,
                const n64_save *local) {
    if (f->blob.path[0]) return plan_blob(req, w, f, s);
    static const char *const TAILS[N64_REGION_COUNT] = {
        ".eep", "_Cont_1.mpk", "_Cont_2.mpk", "_Cont_3.mpk", "_Cont_4.mpk", ".sra", ".fla",
    };
    int rc = SIGIL_OK;
    for (int r = 0; r < N64_REGION_COUNT && rc == SIGIL_OK; r++) {
        bool pak = r >= N64_PAK1 && r <= N64_PAK4;
        if (!pak || f->per_controller) rc = plan_region(w, f, s, r, TAILS[r]);
    }
    if (rc == SIGIL_OK && !f->per_controller) rc = plan_paks(req, w, f, s, local);
    return rc;
}

/* The first member of the unit, named for SIGIL_ERR_NO_TARGET. */
static void first_member(const n64_save *s, char problem[SIGIL_SAVE_PATH_MAX]) {
    for (int r = 0; r < N64_REGION_COUNT; r++) {
        if (s->present[r]) {
            snprintf(problem, SIGIL_SAVE_PATH_MAX, "%s", n64_region_name(r));
            return;
        }
    }
}

static int apply(const sigil_sync_request *req, const n64_writes *w) {
    for (size_t i = 0; i < w->count; i++) {
        if (!w->items[i].data && !req->remove) return SIGIL_ERR_INVALID_ARG;
    }
    int rc = SIGIL_OK;
    for (size_t i = 0; i < w->count && rc == SIGIL_OK; i++) {
        const n64_write *item = &w->items[i];
        if (!item->data) rc = sigil_sync_drop(req, item->path);
        else if (!sigil_sync_file_holds(req, item->path, item->data, item->len)) {
            rc = sigil_sync_put(req, item->path, item->data, item->len);
        }
    }
    return rc;
}

int sigil_sync_restore_n64(sigil_sync_ctx *x, const uint8_t *unit, size_t len, size_t received_len,
                           sigil_sync_result *r, char local_identity[33]) {
    n64_save *in = (n64_save *)calloc(1, sizeof(*in)), *local = (n64_save *)calloc(1, sizeof(*local));
    n64_files f;
    n64_writes w = { 0 };
    int rc = in && local ? read_unit(unit, len, received_len, in, r) : SIGIL_ERR_OOM;
    sigil_zip_member members[N64_REGION_COUNT];
    if (rc == SIGIL_OK && !neutral_members(in, members, r->identity_hash)) rc = SIGIL_ERR_NOT_FOUND;
    if (rc == SIGIL_OK) {
        r->shape = len >= 4 && sigil_read_le32(unit) == 0x04034b50u ? SIGIL_SAVE_SHAPE_MULTI : SIGIL_SAVE_SHAPE_SINGLE;
        artifact_of(x->req, r);
        rc = files_of(x->req, &f, r->problem);
    }
    if (rc == SIGIL_OK) rc = read_local(x->req, &f, local);
    if (rc == SIGIL_OK) neutral_members(local, members, local_identity);
    bool already = false;
    if (rc == SIGIL_OK &&
        sigil_sync_blocks_restore(x, local_identity, r->identity_hash,
                                  sigil_sync_state_get(&x->state, "synced", x->game), &already)) {
        r->conflict = 1;
        rc = SIGIL_ERR_CONFLICT;
    }
    if (rc == SIGIL_OK && !already) {
        rc = plan(x->req, &w, &f, in, local);
        if (rc == SIGIL_ERR_NO_TARGET) first_member(in, r->problem);
        if (rc == SIGIL_OK) rc = apply(x->req, &w);
    }
    writes_free(&w);
    free(in);
    free(local);
    return rc;
}
