// SPDX-License-Identifier: MPL-2.0
/* Collect and restore for Game Boy and Game Boy Color carts. Cart RAM is the
 * same bytes in every emulator; the MBC3 clock is not (clock_gb.h). The unit
 * is the RAM alone for a cart without a clock, and for a cart with one a zip
 * of save.sram and, when the emulator kept a clock, clock.rtc in the 48-byte
 * VBA layout. Its identity covers the RAM only, so a clock that ticked isn't
 * a new save. */
#include "sync_internal.h"
#include "clock_gb.h"
#include <time.h>

#define GB_MAX_RAM   (128u * 1024u)
#define GB_SRAM_NAME "save.sram"
#define GB_CLOCK_NAME "clock.rtc"

/* The clock format a core writes, by layout id; -1 when sigil doesn't know the core. */
static int core_clock(const char *layout) {
    static const struct { const char *layout; int format; } CORES[] = {
        { "gambatte", SIGIL_GB_CLOCK_GAMBATTE },
        { "mgba", SIGIL_GB_CLOCK_MGBA },
        { "sameboy", SIGIL_GB_CLOCK_SAMEBOY_LIBRETRO },
        { "tgbdual", SIGIL_GB_CLOCK_TGB_DUAL },
        { "vbam", SIGIL_GB_CLOCK_VBA },
    };
    for (size_t i = 0; layout && i < sizeof(CORES) / sizeof(CORES[0]); i++) {
        if (strcmp(CORES[i].layout, layout) == 0) return CORES[i].format;
    }
    return -1;
}

/* The clock format of an .rtc of `len` bytes: the core's when it writes that
 * size, else the one format of that size (48 bytes: VBA). */
static int clock_format(const char *layout, size_t len) {
    int core = core_clock(layout);
    if (core >= 0 && sigil_gb_clock_size((sigil_gb_clock_format)core) == len) return core;
    static const sigil_gb_clock_format BY_SIZE[] = {
        SIGIL_GB_CLOCK_VBA, SIGIL_GB_CLOCK_VBA32, SIGIL_GB_CLOCK_GAMBATTE, SIGIL_GB_CLOCK_MESEN2,
        SIGIL_GB_CLOCK_SAMEBOY_LIBRETRO, SIGIL_GB_CLOCK_TGB_DUAL,
    };
    for (size_t i = 0; i < sizeof(BY_SIZE) / sizeof(BY_SIZE[0]); i++) {
        if (sigil_gb_clock_size(BY_SIZE[i]) == len) return BY_SIZE[i];
    }
    return -1;
}

/* A game's save: its cart RAM and, when known, its clock. */
typedef struct {
    uint8_t       *ram;
    size_t         ram_len;
    bool           has_clock;
    sigil_gb_clock clock;
} gb_save;

static void gb_free(gb_save *s) {
    free(s->ram);
    memset(s, 0, sizeof(*s));
}

static bool clock_cart(const sigil_sync_request *req) {
    uint32_t features = req->save.result ? req->save.result->features : req->save.features;
    return (features & SIGIL_FEATURE_RTC) != 0;
}

/* Cart RAM sizes are multiples of 512 bytes; standalone mGBA, VBA-M, SameBoy
 * and Gearboy append the clock to the .sav. The footer's length, or 0. */
static size_t footer_of(size_t len) {
    if (len % 512 == 0) return 0;
    if (len > 48 && (len - 48) % 512 == 0) return 48;
    if (len > 44 && (len - 44) % 512 == 0) return 44;
    return 0;
}

/* Takes RAM, with a clock footer when it has one. */
static int take_ram(gb_save *s, const uint8_t *data, size_t len) {
    size_t footer = footer_of(len);
    free(s->ram);
    s->ram_len = len - footer;
    s->ram = (uint8_t *)malloc(s->ram_len ? s->ram_len : 1);
    if (!s->ram) return SIGIL_ERR_OOM;
    memcpy(s->ram, data, s->ram_len);
    if (!footer) return SIGIL_OK;
    bool lossy = false;
    sigil_gb_clock_format format = footer == 48 ? SIGIL_GB_CLOCK_VBA : SIGIL_GB_CLOCK_VBA32;
    s->has_clock = sigil_gb_clock_read(format, data + s->ram_len, footer, 0, &s->clock, &lossy) == SIGIL_OK;
    return SIGIL_OK;
}

static int take_clock(gb_save *s, int format, const uint8_t *data, size_t len) {
    bool lossy = false;
    if (format < 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    int rc = sigil_gb_clock_read((sigil_gb_clock_format)format, data, len, (int64_t)time(NULL), &s->clock, &lossy);
    s->has_clock = rc == SIGIL_OK;
    return rc;
}

/* The game's files: the RAM file and the clock file the layout names. */
typedef struct {
    char ram[SIGIL_SAVE_PATH_MAX];
    bool ram_present;
    char rtc[SIGIL_SAVE_PATH_MAX];   /* "" when the layout keeps no clock file */
    bool rtc_present;
} gb_files;

static int files_of(const sigil_sync_request *req, gb_files *f) {
    memset(f, 0, sizeof(*f));
    sigil_save_unit *unit = NULL;
    int rc = sigil_save_resolve(&req->save, &unit);
    if (rc != SIGIL_OK) return rc;
    for (size_t i = 0; i < unit->member_count + unit->expected_count; i++) {
        bool present = i < unit->member_count;
        const sigil_save_member *m = present ? &unit->members[i] : &unit->expected[i - unit->member_count];
        bool rtc = m->role == SIGIL_SAVE_ROLE_RTC;
        char *path = rtc ? f->rtc : f->ram;
        if (path[0]) continue;
        snprintf(path, SIGIL_SAVE_PATH_MAX, "%s", m->path);
        if (rtc) f->rtc_present = present;
        else f->ram_present = present;
    }
    sigil_save_unit_free(unit);
    return SIGIL_OK;
}

static int read_local(const sigil_sync_request *req, const gb_files *f, gb_save *s) {
    memset(s, 0, sizeof(*s));
    uint8_t *data = NULL;
    size_t len = 0;
    int rc = SIGIL_OK;
    if (f->ram_present) {
        rc = sigil_sync_read_file(req, f->ram, GB_MAX_RAM + 64, &data, &len);
        if (rc == SIGIL_OK) rc = take_ram(s, data, len);
        free(data);
    }
    if (rc == SIGIL_OK && f->rtc_present) {
        data = NULL;
        rc = sigil_sync_read_file(req, f->rtc, 64, &data, &len);
        if (rc == SIGIL_OK) take_clock(s, clock_format(req->save.layout, len), data, len);
        free(data);
    }
    return rc == SIGIL_ERR_NOT_FOUND ? SIGIL_ERR_IO : rc;
}

/* The save's identity: the RAM's md5, or for a clock cart RomM's zip hash over save.sram alone. */
static void identity_of(const sigil_sync_request *req, const gb_save *s, char out[33]) {
    out[0] = '\0';
    if (!s->ram) return;
    sigil_named_md5 part;
    snprintf(part.name, sizeof(part.name), "%s", GB_SRAM_NAME);
    sigil_md5_of(s->ram, s->ram_len, part.md5);
    if (clock_cart(req)) sigil_named_hash(&part, 1, out);
    else memcpy(out, part.md5, 33);
}

static void artifact_of(const sigil_sync_request *req, bool zip, sigil_sync_result *r) {
    char stem[SIGIL_SAVE_ENTRY_MAX];
    sigil_content_stem(req->save.content_path, stem, sizeof(stem));
    snprintf(r->artifact, sizeof(r->artifact), "%s%s", stem, zip ? ".zip" : ".srm");
}

int sigil_sync_collect_gb(sigil_sync_ctx *x, sigil_sync_result *r) {
    gb_files f;
    gb_save s;
    memset(&s, 0, sizeof(s));
    int rc = files_of(x->req, &f);
    if (rc == SIGIL_OK) rc = read_local(x->req, &f, &s);
    if (rc == SIGIL_OK && s.ram) {
        identity_of(x->req, &s, r->identity_hash);
        if (!clock_cart(x->req)) {
            r->data = (uint8_t *)malloc(s.ram_len ? s.ram_len : 1);
            if (!r->data) rc = SIGIL_ERR_OOM;
            else memcpy(r->data, s.ram, s.ram_len);
            r->len = s.ram_len;
            memcpy(r->content_hash, r->identity_hash, sizeof(r->content_hash));
            r->shape = SIGIL_SAVE_SHAPE_SINGLE;
        } else {
            uint8_t clock[48];
            bool lossy = false;
            sigil_zip_member members[2] = { { GB_SRAM_NAME, s.ram, s.ram_len }, { GB_CLOCK_NAME, clock, sizeof(clock) } };
            size_t n = s.has_clock && sigil_gb_clock_write(SIGIL_GB_CLOCK_VBA, &s.clock, clock, &lossy) == SIGIL_OK ? 2 : 1;
            sigil_named_md5 parts[2];
            for (size_t i = 0; i < n; i++) {
                snprintf(parts[i].name, sizeof(parts[i].name), "%s", members[i].name);
                sigil_md5_of(members[i].data, members[i].len, parts[i].md5);
            }
            sigil_named_hash(parts, n, r->content_hash);
            rc = sigil_zip_store(members, n, &r->data, &r->len);
            r->shape = SIGIL_SAVE_SHAPE_MULTI;
        }
        artifact_of(x->req, r->shape == SIGIL_SAVE_SHAPE_MULTI, r);
    }
    gb_free(&s);
    return rc;
}

/* ---- restore ------------------------------------------------------------------ */

static bool ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), m = strlen(tail);
    return n >= m && strcmp(s + n - m, tail) == 0;
}

/* The save a unit carries: the neutral RAM or zip, a raw .srm or .sav
 * (with its clock footer), or a zip of an emulator's .srm and .rtc. */
static int read_unit(const uint8_t *unit, size_t len, size_t received_len, gb_save *s, sigil_sync_result *r) {
    memset(s, 0, sizeof(*s));
    if (len < 4 || sigil_read_le32(unit) != 0x04034b50u) {
        sigil_md5_of(unit, received_len, r->content_hash);
        r->shape = SIGIL_SAVE_SHAPE_SINGLE;
        return len ? take_ram(s, unit, len) : SIGIL_ERR_NOT_FOUND;
    }
    sigil_zip_member *members = NULL;
    size_t count = 0;
    int rc = sigil_zip_read_mem(unit, len, GB_MAX_RAM + 64, &members, &count);
    sigil_named_md5 *parts = rc == SIGIL_OK ? (sigil_named_md5 *)calloc(count ? count : 1, sizeof(*parts)) : NULL;
    if (rc == SIGIL_OK && !parts) rc = SIGIL_ERR_OOM;
    for (size_t i = 0; i < count && rc == SIGIL_OK; i++) {
        const sigil_zip_member *m = &members[i];
        snprintf(parts[i].name, sizeof(parts[i].name), "%s", m->name);
        sigil_md5_of(m->data, m->len, parts[i].md5);
        if (strcmp(m->name, GB_SRAM_NAME) == 0 || ends_with(m->name, ".srm") || ends_with(m->name, ".sav")) {
            rc = take_ram(s, m->data, m->len);
        } else if (strcmp(m->name, GB_CLOCK_NAME) == 0) {
            rc = take_clock(s, SIGIL_GB_CLOCK_VBA, m->data, m->len);
        } else if (ends_with(m->name, ".rtc")) {
            rc = take_clock(s, clock_format(NULL, m->len), m->data, m->len);
        } else {
            rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
        }
        if (rc != SIGIL_OK) snprintf(r->problem, sizeof(r->problem), "%s", m->name);
    }
    if (rc == SIGIL_OK && count) sigil_named_hash(parts, count, r->content_hash);
    if (rc == SIGIL_OK && !s->ram) rc = SIGIL_ERR_NOT_FOUND;
    r->shape = SIGIL_SAVE_SHAPE_MULTI;
    free(parts);
    if (members) sigil_zip_members_free(members, count);
    return rc;
}

/* The clock file the restore writes: the format of the .rtc already there,
 * else the core's. -1 when there's nothing to write. */
static int target_clock(const sigil_sync_request *req, const gb_files *f, const gb_save *in) {
    if (!in->has_clock || !f->rtc[0]) return -1;
    if (f->rtc_present) {
        uint8_t *data = NULL;
        size_t len = 0;
        int format = sigil_sync_read_file(req, f->rtc, 64, &data, &len) == SIGIL_OK ? clock_format(req->save.layout, len) : -1;
        free(data);
        if (format >= 0) return format;
    }
    return core_clock(req->save.layout);
}

int sigil_sync_restore_gb(sigil_sync_ctx *x, const uint8_t *unit, size_t len, size_t received_len,
                          sigil_sync_result *r, char local_identity[33]) {
    gb_save in, local;
    gb_files f;
    memset(&local, 0, sizeof(local));
    int rc = read_unit(unit, len, received_len, &in, r);
    if (rc == SIGIL_OK) {
        identity_of(x->req, &in, r->identity_hash);
        artifact_of(x->req, r->shape == SIGIL_SAVE_SHAPE_MULTI, r);
        rc = files_of(x->req, &f);
    }
    if (rc == SIGIL_OK) rc = read_local(x->req, &f, &local);
    if (rc == SIGIL_OK) identity_of(x->req, &local, local_identity);
    bool already = false;
    if (rc == SIGIL_OK && sigil_sync_blocks_restore(x, local_identity, r->identity_hash,
                                                    sigil_sync_state_get(&x->state, "synced", x->game), &already)) {
        r->conflict = 1;
        rc = SIGIL_ERR_CONFLICT;
    }
    if (rc == SIGIL_OK && !f.ram[0]) {
        snprintf(r->problem, sizeof(r->problem), "%s", GB_SRAM_NAME);
        rc = SIGIL_ERR_NO_TARGET;
    }
    if (rc == SIGIL_OK && !already && !sigil_sync_file_holds(x->req, f.ram, in.ram, in.ram_len)) {
        rc = sigil_sync_put(x->req, f.ram, in.ram, in.ram_len);
    }
    int format = rc == SIGIL_OK && !already ? target_clock(x->req, &f, &in) : -1;
    if (format >= 0) {
        uint8_t clock[64];
        bool lossy = false;
        rc = sigil_gb_clock_write((sigil_gb_clock_format)format, &in.clock, clock, &lossy);
        size_t clock_len = sigil_gb_clock_size((sigil_gb_clock_format)format);
        if (rc == SIGIL_OK && !sigil_sync_file_holds(x->req, f.rtc, clock, clock_len)) {
            rc = sigil_sync_put(x->req, f.rtc, clock, clock_len);
        }
    }
    gb_free(&in);
    gb_free(&local);
    return rc;
}
