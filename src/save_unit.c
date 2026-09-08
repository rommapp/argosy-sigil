// SPDX-License-Identifier: MPL-2.0
#include "save_layout.h"
#include <stdlib.h>
#include <stdio.h>

#define UNIT_MAX_MEMBERS  64
#define UNIT_MAX_UNKEYED  16
#define HASH_IO_BUF       (64u * 1024u)

/* ---- base name ---------------------------------------------------------- */

static const char *last_separator(const char *s) {
    const char *slash = strrchr(s, '/');
    const char *bslash = strrchr(s, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    return slash;
}

const char *sigil_content_stem(const char *content_name, char *out, size_t cap) {
    if (!out || cap == 0) return out;
    out[0] = '\0';
    if (!content_name) return out;

    const char *name = content_name;
    const char *hash = strrchr(name, '#');
    if (hash) name = hash + 1;
    const char *sep = last_separator(name);
    if (sep) name = sep + 1;

    size_t len = strlen(name);
    const char *dot = strrchr(name, '.');
    if (dot && dot != name) len = (size_t)(dot - name);
    if (len >= cap) len = cap - 1;
    memcpy(out, name, len);
    out[len] = '\0';
    return out;
}

/* ---- options and template expansion ------------------------------------- */

static const char *option_value(const sigil_save_request *req, const char *key) {
    for (size_t i = 0; i < req->option_count; i++) {
        if (req->options[i].key && strcmp(req->options[i].key, key) == 0) return req->options[i].value;
    }
    return NULL;
}

static bool condition_holds(const sigil_save_request *req, const char *key,
                            const char *value, bool holds_when_absent) {
    if (!key) return true;
    const char *actual = option_value(req, key);
    if (!actual) return holds_when_absent;
    return strcmp(actual, value) == 0;
}

/* Genesis Plus GX names the cart file by its size option, but with its own
 * spelling of each size (libretro.c check_variables). */
static const char *gpgx_cart_size_name(const char *value) {
    if (!value) return "4Mbit";
    if (strcmp(value, "128k") == 0) return "128Kbit";
    if (strcmp(value, "256k") == 0) return "256Kbit";
    if (strcmp(value, "512k") == 0) return "512Kbit";
    if (strcmp(value, "1meg") == 0) return "1Mbit";
    if (strcmp(value, "2meg") == 0) return "2Mbit";
    if (strcmp(value, "4meg") == 0) return "4Mbit";
    return NULL;
}

typedef struct {
    const sigil_save_request *req;
    char stem[SIGIL_SAVE_ENTRY_MAX];
} expand_ctx;

static const char *variable_value(const expand_ctx *ctx, const char *name, size_t len) {
    const sigil_save_request *req = ctx->req;
    if (len == 4 && strncmp(name, "stem", 4) == 0) return ctx->stem;
    if (len == 6 && strncmp(name, "romset", 6) == 0) return ctx->stem;
    if (len == 8 && strncmp(name, "title_id", 8) == 0) return req->result ? req->result->title_id : NULL;
    if (len == 7 && strncmp(name, "save_id", 7) == 0) return req->result ? req->result->save_id : NULL;
    if (len == 9 && strncmp(name, "cart_size", 9) == 0) {
        return gpgx_cart_size_name(option_value(req, "genesis_plus_gx_cart_size"));
    }
    if (len == 13 && strncmp(name, "nvram_version", 13) == 0) {
        const char *v = option_value(req, "opera_nvram_version");
        return v ? v : "0";
    }
    if (len == 10 && strncmp(name, "left_index", 10) == 0) {
        const char *v = option_value(req, "beetle_psx_hw_memcard_left_index");
        return v ? v : "0";
    }
    if (len == 11 && strncmp(name, "right_index", 11) == 0) {
        const char *v = option_value(req, "beetle_psx_hw_memcard_right_index");
        return v ? v : "1";
    }
    return NULL;
}

/* Expands `{var}` references. Fails when a variable has no value, so a
 * template that needs a title id the caller could not supply never turns into
 * a name with a hole in it. */
static bool expand_template(const expand_ctx *ctx, const char *template_, char *out, size_t cap) {
    size_t n = 0;
    const char *p = template_;
    while (*p) {
        if (*p == '{') {
            const char *close = strchr(p, '}');
            if (!close) return false;
            const char *value = variable_value(ctx, p + 1, (size_t)(close - p - 1));
            if (!value || !*value) return false;
            size_t vlen = strlen(value);
            if (n + vlen >= cap) return false;
            memcpy(out + n, value, vlen);
            n += vlen;
            p = close + 1;
        } else {
            if (n + 1 >= cap) return false;
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
    return n > 0;
}

/* ---- listing --------------------------------------------------------------- */

static bool listing_has(const sigil_save_request *req, const char *path) {
    for (size_t i = 0; i < req->listing_count; i++) {
        if (req->listing[i] && strcmp(req->listing[i], path) == 0) return true;
    }
    return false;
}

static bool path_has_prefix(const char *path, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(path, prefix, n) == 0 && path[n] != '\0';
}

/* ---- unit assembly --------------------------------------------------------- */

typedef struct {
    sigil_save_member members[UNIT_MAX_MEMBERS];
    size_t            member_count;
    sigil_save_member expected[UNIT_MAX_MEMBERS];
    size_t            expected_count;
    char              unkeyed[UNIT_MAX_UNKEYED][SIGIL_SAVE_PATH_MAX];
    size_t            unkeyed_count;
    bool              folder;
} unit_builder;

static void add_member(unit_builder *b, const char *path, const char *entry, int role, int present) {
    sigil_save_member *list = present ? b->members : b->expected;
    size_t *count = present ? &b->member_count : &b->expected_count;
    if (*count >= UNIT_MAX_MEMBERS) return;
    sigil_save_member *m = &list[*count];
    memset(m, 0, sizeof(*m));
    strncpy(m->path, path, SIGIL_SAVE_PATH_MAX - 1);
    strncpy(m->entry, entry, SIGIL_SAVE_ENTRY_MAX - 1);
    m->role = role;
    m->present = present;
    (*count)++;
}

/* A folder member's entries are named from the folder's parent, so the
 * archive holds `<folder>/<file>` the way a zipped save folder does. */
static void add_folder_members(unit_builder *b, const sigil_save_request *req,
                               const char *folder_path, int role) {
    size_t prefix_len = strlen(folder_path);
    const char *parent_end = folder_path + prefix_len - 1;
    while (parent_end > folder_path && parent_end[-1] != '/') parent_end--;
    size_t parent_len = (size_t)(parent_end - folder_path);

    for (size_t i = 0; i < req->listing_count; i++) {
        const char *path = req->listing[i];
        if (!path || !path_has_prefix(path, folder_path)) continue;
        if (path[strlen(path) - 1] == '/') continue;
        add_member(b, path, path + parent_len, role, 1);
        b->folder = true;
    }
}

static void file_entry_name(const char *path, char *out, size_t cap) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strncpy(out, base, cap - 1);
    out[cap - 1] = '\0';
}

static int collect(const sigil_layout *layout, const sigil_save_request *req,
                   const expand_ctx *ctx, uint32_t features, unit_builder *b) {
    char path[SIGIL_SAVE_PATH_MAX];
    char entry[SIGIL_SAVE_ENTRY_MAX];

    for (size_t i = 0; i < layout->member_count; i++) {
        const sigil_layout_member *lm = &layout->members[i];
        if (!condition_holds(req, lm->opt_key, lm->opt_value, lm->opt_default)) continue;
        if (!expand_template(ctx, lm->template_, path, sizeof(path))) continue;

        if (path[strlen(path) - 1] == '/') {
            add_folder_members(b, req, path, lm->role);
            continue;
        }

        file_entry_name(path, entry, sizeof(entry));
        if (listing_has(req, path)) {
            add_member(b, path, entry, lm->role, 1);
        } else if (lm->role == SIGIL_SAVE_ROLE_RTC && (features & SIGIL_FEATURE_RTC)) {
            add_member(b, path, entry, lm->role, 0);
        }
    }

    for (size_t i = 0; i < layout->shared_count; i++) {
        const sigil_layout_shared *ls = &layout->shared[i];
        if (!condition_holds(req, ls->opt_key, ls->opt_value, ls->opt_default)) continue;
        if (!expand_template(ctx, ls->template_, path, sizeof(path))) continue;
        if (!listing_has(req, path)) continue;
        if (b->unkeyed_count >= UNIT_MAX_UNKEYED) break;
        strncpy(b->unkeyed[b->unkeyed_count], path, SIGIL_SAVE_PATH_MAX - 1);
        b->unkeyed[b->unkeyed_count][SIGIL_SAVE_PATH_MAX - 1] = '\0';
        b->unkeyed_count++;
    }
    return SIGIL_OK;
}

/* ---- hashing --------------------------------------------------------------- */

static int md5_stream(const sigil_io *io, char out_hex[33]) {
    uint8_t *buf = (uint8_t *)malloc(HASH_IO_BUF);
    if (!buf) return SIGIL_ERR_OOM;
    sigil_md5 m;
    sigil_md5_init(&m);
    uint64_t off = 0;
    for (;;) {
        int got = io->read(io->ctx, off, buf, HASH_IO_BUF);
        if (got < 0) { free(buf); return SIGIL_ERR_IO; }
        if (got == 0) break;
        sigil_md5_update(&m, buf, (size_t)got);
        off += (uint64_t)got;
    }
    free(buf);
    uint8_t digest[16];
    sigil_md5_final(&m, digest);
    sigil_md5_hex(digest, out_hex);
    return SIGIL_OK;
}

typedef struct {
    char   name[SIGIL_SAVE_PATH_MAX];
    char   md5[33];
} hashed_entry;

typedef struct {
    hashed_entry *entries;
    size_t        count;
    size_t        cap;
} entry_list;

static int entry_list_add(entry_list *l, const char *name, const char *md5) {
    if (l->count == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 16;
        hashed_entry *n = (hashed_entry *)realloc(l->entries, ncap * sizeof(*n));
        if (!n) return SIGIL_ERR_OOM;
        l->entries = n;
        l->cap = ncap;
    }
    strncpy(l->entries[l->count].name, name, SIGIL_SAVE_PATH_MAX - 1);
    l->entries[l->count].name[SIGIL_SAVE_PATH_MAX - 1] = '\0';
    memcpy(l->entries[l->count].md5, md5, 33);
    l->count++;
    return SIGIL_OK;
}

static int entry_compare(const void *a, const void *b) {
    return strcmp(((const hashed_entry *)a)->name, ((const hashed_entry *)b)->name);
}

/* RomM _compute_zip_hash: md5 of "name:md5\n..." over the entries sorted by
 * name, directories excluded. The entry order is byte order, which is what
 * Python's sorted() yields for the same UTF-8 names. */
static void combined_hash(entry_list *l, char out_hex[33]) {
    qsort(l->entries, l->count, sizeof(hashed_entry), entry_compare);
    sigil_md5 m;
    sigil_md5_init(&m);
    for (size_t i = 0; i < l->count; i++) {
        if (i > 0) sigil_md5_update(&m, "\n", 1);
        sigil_md5_update(&m, l->entries[i].name, strlen(l->entries[i].name));
        sigil_md5_update(&m, ":", 1);
        sigil_md5_update(&m, l->entries[i].md5, 32);
    }
    uint8_t digest[16];
    sigil_md5_final(&m, digest);
    sigil_md5_hex(digest, out_hex);
}

static int on_zip_entry(void *ctx, const char *name, const char *md5_hex) {
    return entry_list_add((entry_list *)ctx, name, md5_hex);
}

static int hash_unit(const sigil_save_request *req, sigil_save_unit *unit) {
    unit->content_hash[0] = '\0';
    if (!req->open || unit->member_count == 0) return SIGIL_OK;

    if (unit->shape == SIGIL_SAVE_SHAPE_SINGLE) {
        sigil_io *io = req->open(req->open_ctx, unit->members[0].path);
        if (!io) return SIGIL_ERR_IO;
        int rc;
        if (sigil_io_is_zip(io)) {
            entry_list l = { NULL, 0, 0 };
            rc = sigil_zip_hash_entries(io, on_zip_entry, &l);
            if (rc == SIGIL_OK) combined_hash(&l, unit->content_hash);
            free(l.entries);
        } else {
            rc = md5_stream(io, unit->content_hash);
        }
        sigil_io_close(io);
        return rc;
    }

    entry_list l = { NULL, 0, 0 };
    int rc = SIGIL_OK;
    for (size_t i = 0; i < unit->member_count && rc == SIGIL_OK; i++) {
        sigil_io *io = req->open(req->open_ctx, unit->members[i].path);
        if (!io) { rc = SIGIL_ERR_IO; break; }
        char hex[33];
        rc = md5_stream(io, hex);
        sigil_io_close(io);
        if (rc == SIGIL_OK) rc = entry_list_add(&l, unit->members[i].entry, hex);
    }
    if (rc == SIGIL_OK) combined_hash(&l, unit->content_hash);
    free(l.entries);
    return rc;
}

/* ---- public ---------------------------------------------------------------- */

static void artifact_name(sigil_save_unit *unit) {
    unit->artifact[0] = '\0';
    if (unit->member_count == 0) return;
    if (unit->shape == SIGIL_SAVE_SHAPE_SINGLE) {
        const char *base = strrchr(unit->members[0].path, '/');
        base = base ? base + 1 : unit->members[0].path;
        strncpy(unit->artifact, base, SIGIL_SAVE_ENTRY_MAX - 1);
        return;
    }
    if (unit->shape == SIGIL_SAVE_SHAPE_FOLDER) {
        snprintf(unit->artifact, SIGIL_SAVE_ENTRY_MAX, "%s.zip", unit->key);
        return;
    }
    const char *base = strrchr(unit->members[0].path, '/');
    base = base ? base + 1 : unit->members[0].path;
    snprintf(unit->artifact, SIGIL_SAVE_ENTRY_MAX, "%s.zip", base);
}

int sigil_save_resolve(const sigil_save_request *req, sigil_save_unit **out) {
    if (!req || !out || req->struct_version != SIGIL_SAVE_REQUEST_V1) return SIGIL_ERR_INVALID_ARG;
    if (!req->content_name || !req->listing) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;

    expand_ctx ctx;
    ctx.req = req;
    sigil_content_stem(req->content_name, ctx.stem, sizeof(ctx.stem));
    if (ctx.stem[0] == '\0') return SIGIL_ERR_INVALID_ARG;

    uint32_t features = req->features;
    if (req->result && req->result->struct_version >= SIGIL_RESULT_V3) features |= req->result->features;

    const sigil_layout *layout = sigil_layout_find(req->layout, req->platform);

    unit_builder *b = (unit_builder *)calloc(1, sizeof(*b));
    if (!b) return SIGIL_ERR_OOM;
    collect(layout, req, &ctx, features, b);

    sigil_save_unit *unit = (sigil_save_unit *)calloc(1, sizeof(*unit));
    if (!unit) { free(b); return SIGIL_ERR_OOM; }
    unit->struct_version = SIGIL_SAVE_UNIT_V1;
    strncpy(unit->key, ctx.stem, SIGIL_SAVE_ENTRY_MAX - 1);

    if (b->member_count > 0) {
        unit->members = (sigil_save_member *)calloc(b->member_count, sizeof(sigil_save_member));
        if (!unit->members) { free(b); sigil_save_unit_free(unit); return SIGIL_ERR_OOM; }
        memcpy(unit->members, b->members, b->member_count * sizeof(sigil_save_member));
        unit->member_count = b->member_count;
    }
    if (b->expected_count > 0) {
        unit->expected = (sigil_save_member *)calloc(b->expected_count, sizeof(sigil_save_member));
        if (!unit->expected) { free(b); sigil_save_unit_free(unit); return SIGIL_ERR_OOM; }
        memcpy(unit->expected, b->expected, b->expected_count * sizeof(sigil_save_member));
        unit->expected_count = b->expected_count;
    }
    if (b->unkeyed_count > 0) {
        unit->unkeyed = calloc(b->unkeyed_count, SIGIL_SAVE_PATH_MAX);
        if (!unit->unkeyed) { free(b); sigil_save_unit_free(unit); return SIGIL_ERR_OOM; }
        memcpy(unit->unkeyed, b->unkeyed, b->unkeyed_count * SIGIL_SAVE_PATH_MAX);
        unit->unkeyed_count = b->unkeyed_count;
    }

    if (unit->member_count == 0)      unit->shape = SIGIL_SAVE_SHAPE_NONE;
    else if (b->folder)               unit->shape = SIGIL_SAVE_SHAPE_FOLDER;
    else if (unit->member_count == 1) unit->shape = SIGIL_SAVE_SHAPE_SINGLE;
    else                              unit->shape = SIGIL_SAVE_SHAPE_MULTI;
    free(b);

    artifact_name(unit);

    int rc = hash_unit(req, unit);
    if (rc != SIGIL_OK) { sigil_save_unit_free(unit); return rc; }

    *out = unit;
    return SIGIL_OK;
}

void sigil_save_unit_free(sigil_save_unit *unit) {
    if (!unit) return;
    free(unit->members);
    free(unit->expected);
    free(unit->unkeyed);
    free(unit);
}
