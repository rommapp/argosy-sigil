// SPDX-License-Identifier: MPL-2.0
#include "save_profiles.h"
#include "rzip.h"
#include <ctype.h>
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

const char *sigil_content_stem(const char *content_path, char *out, size_t cap) {
    if (!out || cap == 0) return out;
    out[0] = '\0';
    if (!content_path) return out;

    const char *name = content_path;
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

const char *sigil_save_option_value(const sigil_save_request *req, const char *key) {
    for (size_t i = 0; i < req->option_count; i++) {
        if (req->options[i].key && strcmp(req->options[i].key, key) == 0) return req->options[i].value;
    }
    return NULL;
}

static bool condition_holds(const sigil_save_request *req, const char *key,
                            const char *value, bool holds_when_absent) {
    if (!key) return true;
    const char *actual = sigil_save_option_value(req, key);
    if (!actual) return holds_when_absent;
    return strcmp(actual, value) == 0;
}

/* A member or shared file of a row, with the options that select it. */
typedef struct {
    const char *template_;
    const char *keys[2];
    const char *values[2];
    bool        defaults[2];
    int         shared;
} row_file;

static row_file member_file(const sigil_layout_member *lm) {
    return (row_file){ lm->template_, { lm->opt_key, lm->opt2_key }, { lm->opt_value, lm->opt2_value },
                       { lm->opt_default, lm->opt2_default }, 0 };
}

static row_file shared_file(const sigil_layout_shared *ls) {
    return (row_file){ ls->template_, { ls->opt_key, ls->opt2_key }, { ls->opt_value, ls->opt2_value },
                       { ls->opt_default, ls->opt2_default }, 1 };
}

static bool file_applies(const sigil_save_request *req, const row_file *f) {
    return condition_holds(req, f->keys[0], f->values[0], f->defaults[0]) &&
           condition_holds(req, f->keys[1], f->values[1], f->defaults[1]);
}

static bool member_applies(const sigil_save_request *req, const sigil_layout_member *lm) {
    row_file f = member_file(lm);
    return file_applies(req, &f);
}

static bool shared_applies(const sigil_save_request *req, const sigil_layout_shared *ls) {
    row_file f = shared_file(ls);
    return file_applies(req, &f);
}

/* genesis_plus_gx's RAM cart: genesis_plus_gx_cart_size names the size, the
 * cart file carries it in its name, and the file is the cart's bytes. */
typedef struct {
    const char *value;
    const char *name;
    uint32_t    bytes;
} gpgx_cart;

static const gpgx_cart GPGX_CARTS[] = {
    { "128k", "128Kbit", 16u * 1024u },
    { "256k", "256Kbit", 32u * 1024u },
    { "512k", "512Kbit", 64u * 1024u },
    { "1meg", "1Mbit", 128u * 1024u },
    { "2meg", "2Mbit", 256u * 1024u },
    { "4meg", "4Mbit", 512u * 1024u },
};

/* The core options template variables read, with the value each takes when
 * the request gives none. Beetle PSX's card indexes take the build's prefix:
 * beetle_psx_ on the software core, beetle_psx_hw_ otherwise. */
typedef struct {
    const char *var;
    const char *key;
    const char *fallback;
    bool        beetle_prefix;
} variable_option;

static const variable_option VARIABLE_OPTIONS[] = {
    { "cart_size", "genesis_plus_gx_cart_size", "4meg", false },
    { "nvram_version", "opera_nvram_version", "0", false },
    { "pcsx2_slot1", "Slot1_Filename", "Mcd001.ps2", false },
    { "pcsx2_slot2", "Slot2_Filename", "Mcd002.ps2", false },
    { "left_index", "memcard_left_index", "0", true },
    { "right_index", "memcard_right_index", "1", true },
};

const char *sigil_save_variable_option(const char *layout, const char *var, size_t len, char key[64],
                                       const char **fallback) {
    for (size_t i = 0; i < sizeof(VARIABLE_OPTIONS) / sizeof(VARIABLE_OPTIONS[0]); i++) {
        const variable_option *o = &VARIABLE_OPTIONS[i];
        if (strlen(o->var) != len || strncmp(o->var, var, len) != 0) continue;
        bool software = layout && strcmp(layout, "mednafen_psx") == 0;
        snprintf(key, 64, "%s%s", o->beetle_prefix ? (software ? "beetle_psx_" : "beetle_psx_hw_") : "", o->key);
        *fallback = o->fallback;
        return key;
    }
    return NULL;
}

/* The request's value for the option variable `var` (`len` bytes) reads, or
 * its fallback; NULL for a variable no option sets. */
static const char *variable_option_value(const sigil_save_request *req, const char *var, size_t len) {
    char key[64];
    const char *fallback = NULL;
    if (!sigil_save_variable_option(req->layout, var, len, key, &fallback)) return NULL;
    const char *v = sigil_save_option_value(req, key);
    return v && *v ? v : fallback;
}

size_t sigil_gpgx_cart_values(const char **out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < sizeof(GPGX_CARTS) / sizeof(GPGX_CARTS[0]) && n < cap; i++) out[n++] = GPGX_CARTS[i].value;
    return n;
}

static const gpgx_cart *gpgx_cart_for(const sigil_save_request *req) {
    const char *value = variable_option_value(req, "cart_size", 9);
    for (size_t i = 0; i < sizeof(GPGX_CARTS) / sizeof(GPGX_CARTS[0]); i++) {
        if (strcmp(GPGX_CARTS[i].value, value) == 0) return &GPGX_CARTS[i];
    }
    return NULL;
}

/* The size a file the core creates from `template_` has: the row's, or the
 * cart size the core's option names. */
static uint32_t new_file_size(const sigil_save_request *req, const char *template_, uint32_t row_size) {
    if (!strstr(template_, "{cart_size}")) return row_size;
    const gpgx_cart *cart = gpgx_cart_for(req);
    return cart ? cart->bytes : row_size;
}

typedef struct {
    const sigil_save_request *req;
    char stem[SIGIL_SAVE_ENTRY_MAX];
    char dc_vmu_id[sizeof(((sigil_result *)0)->title_id)];
    char disc_id[sizeof(((sigil_result *)0)->title_id)];
    char pcsx_serial[sizeof(((sigil_result *)0)->raw_serial)];
    char n64_md5_8[9];
    char n64_md5_lower[sizeof(((sigil_result *)0)->n64_md5)];
} expand_ctx;

/* The N64 fields of a result new enough to carry them, else NULL. */
static const sigil_result *n64_result(const sigil_save_request *req) {
    return req->result && req->result->struct_version >= SIGIL_RESULT_V4 ? req->result : NULL;
}

/* mupen64plus names a save with the MD5's first eight digits; M64Plus FZ
 * names the game's folder with the whole MD5 in lowercase. */
static void n64_md5_forms(const sigil_save_request *req, char md5_8[9], char *lower, size_t lower_cap) {
    const sigil_result *r = n64_result(req);
    const char *md5 = r ? r->n64_md5 : "";
    snprintf(md5_8, 9, "%s", strlen(md5) >= 8 ? md5 : "");
    size_t n = 0;
    for (; md5[n] && n + 1 < lower_cap; n++) lower[n] = (char)tolower((unsigned char)md5[n]);
    lower[n] = '\0';
}

#define PCSX_CDROM_ID_MAX 9

/* pcsx_rearmed's card name for a disc: the boot file's letters and digits as
 * written, cut at nine (libpcsxcore/misc.c CheckCdrom), with a dash before the
 * first digit (frontend/libretro.c get_dash_serial). raw_serial keeps the boot
 * file as written; title_id stands in when there is none. */
static void pcsx_serial(const sigil_save_request *req, char *out, size_t cap) {
    out[0] = '\0';
    if (!req->result) return;
    const char *id = req->result->raw_serial[0] ? req->result->raw_serial : req->result->title_id;
    char alnum[PCSX_CDROM_ID_MAX + 1];
    size_t n = 0;
    for (; *id && n < PCSX_CDROM_ID_MAX; id++) {
        if (isalnum((unsigned char)*id)) alnum[n++] = *id;
    }
    alnum[n] = '\0';
    size_t d = 0;
    bool dashed = false;
    for (size_t s = 0; alnum[s] && d + 1 < cap; d++) {
        if (!dashed && isdigit((unsigned char)alnum[s])) {
            out[d] = '-';
            dashed = true;
            continue;
        }
        out[d] = alnum[s++];
    }
    out[d] = '\0';
}

/* A PSP EBOOT's DISC_ID: the save id's letters and digits (SLUS-01040 is
 * SLUS01040), the title id's when there is none. One EBOOT holds every disc
 * of a set under one DISC_ID, so a later disc names that folder by save_id. */
static void disc_id(const sigil_save_request *req, char *out, size_t cap) {
    size_t n = 0;
    const char *id = !req->result ? "" : req->result->save_id[0] ? req->result->save_id : req->result->title_id;
    for (; *id && n + 1 < cap; id++) {
        if (isalnum((unsigned char)*id)) out[n++] = *id;
    }
    out[n] = '\0';
}

/* flycast's per-game VMU name: the product number with ` /\:*?|<>` made `_`
 * (flycast shell/libretro/oslib.cpp getVmuPath). */
static void dc_vmu_id(const sigil_save_request *req, char *out, size_t cap) {
    out[0] = '\0';
    if (!req->result || !req->result->title_id[0]) return;
    snprintf(out, cap, "%s", req->result->title_id);
    for (char *c = out; *c; c++) {
        if (strchr(" /\\:*?|<>", *c)) *c = '_';
    }
}

char sigil_gc_region_letter(const sigil_result *result) {
    if (!result) return 0;
    if (strlen(result->raw_serial) >= 4) return result->raw_serial[3];
    unsigned value = 0;
    if (strlen(result->title_id) >= 8 && sscanf(result->title_id + 6, "%2x", &value) == 1) return (char)value;
    return 0;
}

const char *sigil_gc_region_folder(char letter) {
    if (!letter) return NULL;
    if (letter == 'E') return "USA";
    if (letter == 'J' || letter == 'K') return "JAP";
    return "EUR";
}

static const char *gc_region(const sigil_save_request *req) {
    return sigil_gc_region_folder(sigil_gc_region_letter(req->result));
}

static void expand_ctx_init(expand_ctx *ctx, const sigil_save_request *req) {
    ctx->req = req;
    sigil_content_stem(req->content_path, ctx->stem, sizeof(ctx->stem));
    dc_vmu_id(req, ctx->dc_vmu_id, sizeof(ctx->dc_vmu_id));
    disc_id(req, ctx->disc_id, sizeof(ctx->disc_id));
    pcsx_serial(req, ctx->pcsx_serial, sizeof(ctx->pcsx_serial));
    n64_md5_forms(req, ctx->n64_md5_8, ctx->n64_md5_lower, sizeof(ctx->n64_md5_lower));
}

static const char *variable_value(const expand_ctx *ctx, const char *name, size_t len) {
    const sigil_save_request *req = ctx->req;
    if (len == 4 && strncmp(name, "stem", 4) == 0) return ctx->stem;
    if (len == 6 && strncmp(name, "romset", 6) == 0) return ctx->stem;
    if (len == 8 && strncmp(name, "title_id", 8) == 0) return req->result ? req->result->title_id : NULL;
    if (len == 7 && strncmp(name, "save_id", 7) == 0) return req->result ? req->result->save_id : NULL;
    if (len == 9 && strncmp(name, "dc_vmu_id", 9) == 0) return ctx->dc_vmu_id;
    if (len == 7 && strncmp(name, "disc_id", 7) == 0) return ctx->disc_id;
    if (len == 11 && strncmp(name, "pcsx_serial", 11) == 0) return ctx->pcsx_serial;
    if (len == 9 && strncmp(name, "gc_region", 9) == 0) return gc_region(req);
    if (len == 10 && strncmp(name, "n64_header", 10) == 0) return n64_result(req) ? req->result->n64_header : NULL;
    if (len == 9 && strncmp(name, "n64_md5_8", 9) == 0) return ctx->n64_md5_8;
    if (len == 13 && strncmp(name, "n64_md5_lower", 13) == 0) return ctx->n64_md5_lower;
    if (len == 11 && strncmp(name, "n64_md5_n64", 11) == 0) return n64_result(req) ? req->result->n64_md5_n64 : NULL;
    if (len == 9 && strncmp(name, "cart_size", 9) == 0) {
        const gpgx_cart *cart = gpgx_cart_for(req);
        return cart ? cart->name : NULL;
    }
    return variable_option_value(req, name, len);
}

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

bool sigil_save_listed(const sigil_save_request *req, const char *path) {
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

/* `path` fits `pattern`, where each '*' stands for any run of characters,
 * none of them '/', so a '*' never reaches into another folder. */
static bool glob_match(const char *pattern, const char *path) {
    if (*pattern == '\0') return *path == '\0';
    if (*pattern == '*') {
        for (const char *p = path;; p++) {
            if (glob_match(pattern + 1, p)) return true;
            if (*p == '\0' || *p == '/') return false;
        }
    }
    return *path == *pattern && glob_match(pattern + 1, path + 1);
}

static bool member_added(const unit_builder *b, const char *path) {
    for (size_t i = 0; i < b->member_count; i++) {
        if (strcmp(b->members[i].path, path) == 0) return true;
    }
    return false;
}

/* Every listed file a template with '*' names. A name sigil can't spell in
 * full has no file to expect, so nothing is added when none is listed. */
static void add_matching_members(unit_builder *b, const sigil_save_request *req, const char *pattern, int role) {
    char entry[SIGIL_SAVE_ENTRY_MAX];
    for (size_t i = 0; i < req->listing_count; i++) {
        const char *path = req->listing[i];
        if (!path || !glob_match(pattern, path) || member_added(b, path)) continue;
        file_entry_name(path, entry, sizeof(entry));
        add_member(b, path, entry, role, 1);
    }
}

static int collect(const sigil_layout *layout, const sigil_save_request *req,
                   const expand_ctx *ctx, uint32_t features, unit_builder *b) {
    char path[SIGIL_SAVE_PATH_MAX];
    char entry[SIGIL_SAVE_ENTRY_MAX];

    for (size_t i = 0; i < layout->member_count; i++) {
        const sigil_layout_member *lm = &layout->members[i];
        if (!member_applies(req, lm)) continue;
        if (!expand_template(ctx, lm->template_, path, sizeof(path))) continue;

        if (path[strlen(path) - 1] == '/') {
            add_folder_members(b, req, path, lm->role);
            continue;
        }
        if (strchr(path, '*')) {
            add_matching_members(b, req, path, lm->role);
            continue;
        }

        file_entry_name(path, entry, sizeof(entry));
        if (sigil_save_listed(req, path)) {
            add_member(b, path, entry, lm->role, 1);
        } else if (lm->role == SIGIL_SAVE_ROLE_PRIMARY) {
            add_member(b, path, entry, lm->role, 0);
        } else if (lm->role == SIGIL_SAVE_ROLE_RTC && (features & SIGIL_FEATURE_RTC)) {
            add_member(b, path, entry, lm->role, 0);
        }
    }

    for (size_t i = 0; i < layout->shared_count; i++) {
        const sigil_layout_shared *ls = &layout->shared[i];
        if (!shared_applies(req, ls)) continue;
        if (!expand_template(ctx, ls->template_, path, sizeof(path))) continue;
        if (!sigil_save_listed(req, path)) continue;
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
    sigil_named_md5 *entries;
    size_t           count;
    size_t           cap;
} entry_list;

static int entry_list_add(entry_list *l, const char *name, const char *md5) {
    if (l->count == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 16;
        sigil_named_md5 *n = (sigil_named_md5 *)realloc(l->entries, ncap * sizeof(*n));
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

static void combined_hash(entry_list *l, char out_hex[33]) {
    sigil_named_hash(l->entries, l->count, out_hex);
}

static int on_zip_entry(void *ctx, const char *name, const char *md5_hex) {
    return entry_list_add((entry_list *)ctx, name, md5_hex);
}

static int hash_single(sigil_save_open_fn open, void *open_ctx, const sigil_save_member *member, char out_hex[33]) {
    sigil_io *io = sigil_save_open(open, open_ctx, member->path);
    if (!io) return SIGIL_ERR_IO;
    int rc;
    if (sigil_io_is_zip(io)) {
        entry_list l = { NULL, 0, 0 };
        rc = sigil_zip_hash_entries(io, on_zip_entry, &l);
        if (rc == SIGIL_OK) combined_hash(&l, out_hex);
        free(l.entries);
    } else {
        rc = md5_stream(io, out_hex);
    }
    sigil_io_close(io);
    return rc;
}

int sigil_save_hash(sigil_save_unit *unit, sigil_save_open_fn open, void *open_ctx) {
    if (!unit || !open || unit->struct_version != SIGIL_SAVE_UNIT_V1) return SIGIL_ERR_INVALID_ARG;
    unit->content_hash[0] = '\0';
    unit->identity_hash[0] = '\0';
    if (unit->member_count == 0) return SIGIL_OK;

    if (unit->shape == SIGIL_SAVE_SHAPE_SINGLE) {
        int rc = hash_single(open, open_ctx, &unit->members[0], unit->content_hash);
        if (rc == SIGIL_OK) memcpy(unit->identity_hash, unit->content_hash, 33);
        return rc;
    }

    entry_list all = { NULL, 0, 0 };
    entry_list state = { NULL, 0, 0 };
    size_t state_count = 0;
    const sigil_save_member *state_member = NULL;
    int rc = SIGIL_OK;
    for (size_t i = 0; i < unit->member_count && rc == SIGIL_OK; i++) {
        sigil_io *io = sigil_save_open(open, open_ctx, unit->members[i].path);
        if (!io) { rc = SIGIL_ERR_IO; break; }
        char hex[33];
        rc = md5_stream(io, hex);
        sigil_io_close(io);
        if (rc != SIGIL_OK) break;
        rc = entry_list_add(&all, unit->members[i].entry, hex);
        if (rc == SIGIL_OK && unit->members[i].role != SIGIL_SAVE_ROLE_RTC) {
            rc = entry_list_add(&state, unit->members[i].entry, hex);
            state_count++;
            state_member = &unit->members[i];
        }
    }
    if (rc == SIGIL_OK) {
        combined_hash(&all, unit->content_hash);
        if (state_count == 1) rc = hash_single(open, open_ctx, state_member, unit->identity_hash);
        else combined_hash(&state, unit->identity_hash);
    }
    free(all.entries);
    free(state.entries);
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

/* A layout with profiles: the game's files in the profile's account folder
 * and the device folders, under their unit names. */
static int resolve_profiles(const sigil_save_request *req, const sigil_layout_profiles *row, sigil_save_unit **out) {
    if (!req->result || !req->result->save_id[0]) return SIGIL_ERR_INVALID_ARG;
    sigil_profile_root p;
    char problem[SIGIL_SAVE_PATH_MAX];
    int rc = sigil_profile_root_open(req, row, &p, problem);
    if (rc == SIGIL_OK && sigil_profile_undecided(&p)) rc = SIGIL_ERR_AMBIGUOUS;
    sigil_profile_file *files = NULL;
    size_t count = 0;
    if (rc == SIGIL_OK) rc = sigil_profile_files(&p, p.profile, &files, &count);
    sigil_save_unit *unit = rc == SIGIL_OK ? (sigil_save_unit *)calloc(1, sizeof(*unit)) : NULL;
    if (rc == SIGIL_OK && !unit) rc = SIGIL_ERR_OOM;
    if (rc == SIGIL_OK && count) {
        unit->members = (sigil_save_member *)calloc(count, sizeof(sigil_save_member));
        if (!unit->members) rc = SIGIL_ERR_OOM;
    }
    if (rc != SIGIL_OK) {
        free(files);
        free(unit);
        return rc;
    }
    unit->struct_version = SIGIL_SAVE_UNIT_V1;
    snprintf(unit->key, sizeof(unit->key), "%s", p.save_id);
    for (size_t i = 0; i < count; i++) {
        sigil_save_member *m = &unit->members[i];
        snprintf(m->path, sizeof(m->path), "%s", files[i].path);
        snprintf(m->entry, sizeof(m->entry), "%s", files[i].entry);
        m->role = SIGIL_SAVE_ROLE_PRIMARY;
        m->present = 1;
        m->area = files[i].area;
    }
    free(files);
    unit->member_count = count;
    unit->shape = count ? SIGIL_SAVE_SHAPE_FOLDER : SIGIL_SAVE_SHAPE_NONE;
    artifact_name(unit);
    if (req->open) rc = sigil_save_hash(unit, req->open, req->open_ctx);
    if (rc != SIGIL_OK) {
        sigil_save_unit_free(unit);
        return rc;
    }
    *out = unit;
    return SIGIL_OK;
}

int sigil_save_resolve(const sigil_save_request *req, sigil_save_unit **out) {
    if (!req || !out || req->struct_version != SIGIL_SAVE_REQUEST_V1) return SIGIL_ERR_INVALID_ARG;
    if (!req->content_path || !req->listing) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;

    expand_ctx ctx;
    expand_ctx_init(&ctx, req);
    if (ctx.stem[0] == '\0') return SIGIL_ERR_INVALID_ARG;

    uint32_t features = req->features;
    if (req->result && req->result->struct_version >= SIGIL_RESULT_V3) features |= req->result->features;

    const sigil_layout *layout = sigil_layout_find(req->layout, req->platform);
    if (layout->profiles) return resolve_profiles(req, layout->profiles, out);

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

    int rc = sigil_save_alternates(req, &unit->alternates, &unit->alternate_count);
    if (rc == SIGIL_OK && req->open) rc = sigil_save_hash(unit, req->open, req->open_ctx);
    if (rc != SIGIL_OK) { sigil_save_unit_free(unit); return rc; }

    *out = unit;
    return SIGIL_OK;
}

/* ---- alternates -------------------------------------------------------------- */

static size_t row_file_count(const sigil_layout *layout) {
    return layout->member_count + layout->shared_count;
}

static row_file row_file_at(const sigil_layout *layout, size_t i) {
    return i < layout->member_count ? member_file(&layout->members[i])
                                    : shared_file(&layout->shared[i - layout->member_count]);
}

/* A file the request's options already take, by any member or shared file. */
static bool taken_now(const sigil_layout *layout, const sigil_save_request *req, const expand_ctx *ctx,
                      const char *path) {
    char other[SIGIL_SAVE_PATH_MAX];
    for (size_t i = 0; i < row_file_count(layout); i++) {
        row_file f = row_file_at(layout, i);
        if (file_applies(req, &f) && expand_template(ctx, f.template_, other, sizeof(other)) &&
            strcmp(other, path) == 0) {
            return true;
        }
    }
    return false;
}

static bool already_listed(const sigil_save_alternate *list, size_t count, const char *path) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(list[i].path, path) == 0) return true;
    }
    return false;
}

int sigil_save_alternates(const sigil_save_request *req, sigil_save_alternate **out, size_t *count) {
    *out = NULL;
    *count = 0;
    if (!req->content_path || !req->listing) return SIGIL_OK;
    const sigil_layout *layout = sigil_layout_find(req->layout, req->platform);
    if (layout->profiles) return SIGIL_OK;
    expand_ctx ctx;
    expand_ctx_init(&ctx, req);
    if (ctx.stem[0] == '\0') return SIGIL_OK;

    sigil_save_alternate *list = NULL;
    size_t n = 0;
    char path[SIGIL_SAVE_PATH_MAX];
    for (size_t i = 0; i < row_file_count(layout); i++) {
        row_file f = row_file_at(layout, i);
        if (!expand_template(&ctx, f.template_, path, sizeof(path)) || path[strlen(path) - 1] == '/' ||
            strchr(path, '*')) {
            continue;
        }
        if (!sigil_save_listed(req, path) || taken_now(layout, req, &ctx, path) || already_listed(list, n, path)) {
            continue;
        }
        sigil_save_alternate *grown = (sigil_save_alternate *)realloc(list, (n + 1) * sizeof(*list));
        if (!grown) {
            free(list);
            return SIGIL_ERR_OOM;
        }
        list = grown;
        sigil_save_alternate *a = &list[n++];
        memset(a, 0, sizeof(*a));
        snprintf(a->path, sizeof(a->path), "%s", path);
        a->shared = f.shared;
        for (size_t k = 0; k < 2; k++) {
            if (f.keys[k] && !condition_holds(req, f.keys[k], f.values[k], f.defaults[k])) {
                a->options[a->option_count].key = f.keys[k];
                a->options[a->option_count].value = f.values[k];
                a->option_count++;
            }
        }
    }
    *out = list;
    *count = n;
    return SIGIL_OK;
}

size_t sigil_save_shared_paths(const sigil_save_request *req, char (*out)[SIGIL_SAVE_PATH_MAX], int *devices,
                               size_t cap) {
    if (!req || !req->content_path) return 0;
    expand_ctx ctx;
    expand_ctx_init(&ctx, req);
    const sigil_layout *layout = sigil_layout_find(req->layout, req->platform);
    size_t n = 0;
    for (size_t i = 0; i < layout->shared_count && n < cap; i++) {
        const sigil_layout_shared *ls = &layout->shared[i];
        if (!shared_applies(req, ls)) continue;
        if (!expand_template(&ctx, ls->template_, out[n], SIGIL_SAVE_PATH_MAX)) continue;
        if (devices) devices[n] = ls->device;
        n++;
    }
    return n;
}

/* The values of a layout's region option, the first its default, each with
 * the region it forces (0: the disc's). */
static const struct { const char *value; char region; } REGION_VALUES[] = {
    { "auto", 0 }, { "ntsc-u", 'U' }, { "pal", 'E' }, { "ntsc-j", 'J' },
};

size_t sigil_region_option_values(const char **out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < sizeof(REGION_VALUES) / sizeof(REGION_VALUES[0]) && n < cap; i++) out[n++] = REGION_VALUES[i].value;
    return n;
}

static char region_from_option(const char *value) {
    for (size_t i = 0; value && i < sizeof(REGION_VALUES) / sizeof(REGION_VALUES[0]); i++) {
        if (strcmp(value, REGION_VALUES[i].value) == 0) return REGION_VALUES[i].region;
    }
    return 0;
}

static char region_from_word(const char *word, size_t len) {
    static const struct { const char *word; char region; } WORDS[] = {
        { "USA", 'U' }, { "US", 'U' }, { "U", 'U' },
        { "Europe", 'E' }, { "EU", 'E' }, { "E", 'E' }, { "UK", 'E' }, { "Germany", 'E' },
        { "France", 'E' }, { "Spain", 'E' }, { "Italy", 'E' }, { "Australia", 'E' },
        { "Japan", 'J' }, { "JP", 'J' }, { "J", 'J' },
    };
    for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++) {
        if (strlen(WORDS[i].word) == len && strncmp(WORDS[i].word, word, len) == 0) return WORDS[i].region;
    }
    return 0;
}

/* The region the first parenthesised tag of the content's file name names:
 * "Lunar (USA).cue" and "Lunar (USA, Europe).cue" give 'U'. */
static char region_from_name(const char *content_path) {
    char stem[SIGIL_SAVE_ENTRY_MAX];
    sigil_content_stem(content_path, stem, sizeof(stem));
    for (const char *open = strchr(stem, '('); open; open = strchr(open + 1, '(')) {
        const char *word = open + 1;
        size_t len = strcspn(word, ",)");
        char region = region_from_word(word, len);
        if (region) return region;
    }
    return 0;
}

int sigil_save_volume_targets(const sigil_save_request *req, sigil_volume_target out[SIGIL_VOLUME_TARGETS_MAX],
                              size_t *count) {
    *count = 0;
    if (!req || !req->content_path) return SIGIL_ERR_INVALID_ARG;
    expand_ctx ctx;
    expand_ctx_init(&ctx, req);
    const sigil_layout *layout = sigil_layout_find(req->layout, req->platform);
    char region = layout->region_option ? region_from_option(sigil_save_option_value(req, layout->region_option)) : 0;
    if (!region) region = region_from_name(req->content_path);

    for (int device = SIGIL_DEVICE_INTERNAL; device < SIGIL_DEVICE_COUNT && *count < SIGIL_VOLUME_TARGETS_MAX; device++) {
        sigil_volume_target *t = &out[*count];
        memset(t, 0, sizeof(*t));
        bool found = false, found_present = false;
        for (size_t i = 0; i < layout->member_count && !found_present; i++) {
            const sigil_layout_member *lm = &layout->members[i];
            char path[SIGIL_SAVE_PATH_MAX];
            if (lm->device != device || !member_applies(req, lm)) continue;
            if (!expand_template(&ctx, lm->template_, path, sizeof(path))) continue;
            found_present = sigil_save_listed(req, path);
            if (found && !found_present) continue;
            found = true;
            snprintf(t->path, sizeof(t->path), "%s", path);
            t->per_game = true;
            t->form = lm->form;
            t->new_size = new_file_size(req, lm->template_, lm->new_size);
        }
        /* Among the files the region allows, the first present wins, else the
         * first; when the region allows none, the one file present. */
        size_t candidates = 0, present = 0;
        const sigil_layout_shared *chosen = NULL, *only_present = NULL;
        bool chosen_present = false;
        char chosen_path[SIGIL_SAVE_PATH_MAX] = "", present_path[SIGIL_SAVE_PATH_MAX] = "";
        for (size_t i = 0; i < layout->shared_count && !found; i++) {
            const sigil_layout_shared *ls = &layout->shared[i];
            char path[SIGIL_SAVE_PATH_MAX];
            if (ls->device != device || !shared_applies(req, ls)) continue;
            if (!expand_template(&ctx, ls->template_, path, sizeof(path))) continue;
            candidates++;
            bool here = sigil_save_listed(req, path);
            if (here) {
                present++;
                only_present = ls;
                snprintf(present_path, sizeof(present_path), "%s", path);
            }
            if ((!ls->region || ls->region == region) && (!chosen || (here && !chosen_present))) {
                chosen = ls;
                chosen_present = here;
                snprintf(chosen_path, sizeof(chosen_path), "%s", path);
            }
        }
        if (!found && candidates > 0) {
            if (!chosen && present == 1) {
                chosen = only_present;
                snprintf(chosen_path, sizeof(chosen_path), "%s", present_path);
            }
            if (!chosen) return SIGIL_ERR_NOT_FOUND;
            snprintf(t->path, sizeof(t->path), "%s", chosen_path);
            t->per_game = false;
            t->form = chosen->form;
            t->new_size = new_file_size(req, chosen->template_, chosen->new_size);
            found = true;
        }
        if (found) {
            t->device = device;
            (*count)++;
        }
    }
    return SIGIL_OK;
}

void sigil_save_unit_free(sigil_save_unit *unit) {
    if (!unit) return;
    free(unit->members);
    free(unit->expected);
    free(unit->unkeyed);
    free(unit->alternates);
    free(unit);
}
