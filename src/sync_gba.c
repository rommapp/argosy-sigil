// SPDX-License-Identifier: MPL-2.0
/* Collect and restore for Game Boy Advance carts. The save chip's bytes are
 * the same in every emulator; the file around them is not:
 * - mGBA's libretro core reports 128 KiB until the game addresses the chip,
 *   then the chip's size; an EEPROM is 512 bytes until the game addresses
 *   past them (mgba at e31759b2: src/platform/libretro/libretro.c
 *   retro_get_memory_size; src/gba/savedata.c GBASavedataInitEEPROM,
 *   _ensureEeprom).
 * - VBA-M's libretro core writes the chip's size; an EEPROM is 512 bytes
 *   until the game addresses 8 KiB, or the override table's size
 *   (visualboyadvance-m v2.2.3: src/libretro/libretro.cpp
 *   load_image_preferences, retro_get_memory_size;
 *   src/core/gba/gbaEeprom.cpp eepromWrite).
 * - gpSP pads every save to 128 KiB with 0xFF (libretro/gpsp at 5819380:
 *   libretro/libretro.c retro_get_memory_size, retro_load_game).
 * - VBA Next writes 136 KiB with an EEPROM's bytes at 128 KiB (libretro/
 *   vba-next at 788192f: libretro/libretro.c libretro_save_buf; src/memory.c
 *   eepromData).
 * - Standalone mGBA writes the chip's size, then on a clock cart the clock
 *   (mGBA 0.10.5: src/gba/savedata.c GBASavedataRTCWrite), and grows a
 *   shorter file to 8 KiB with zeros for the EEPROM games its override table
 *   names (GBASavedataInitEEPROM, src/gba/overrides.c).
 * The unit is the chip's bytes at the chip's size:
 * on a cart without a clock one file, on a cart with one a zip of save.sram
 * and, when the emulator kept the clock, clock.rtc in standalone mGBA's
 * 16-byte layout. Its identity covers the chip only, so a clock that ticked
 * isn't a new save. */
#include "sync_internal.h"

#define GBA_EEPROM_4K   512u              /* 4 Kbit EEPROM */
#define GBA_EEPROM_64K  8192u             /* 64 Kbit EEPROM */
#define GBA_SRAM        32768u
#define GBA_FLASH       65536u
#define GBA_FLASH1M     131072u
#define GBA_PADDED      GBA_FLASH1M       /* gpSP's file for every chip */
#define VBA_NEXT_FILE   0x22000u          /* SRAM or flash at 0, an EEPROM at 0x20000 */
#define VBA_NEXT_EEPROM 0x20000u
#define GBA_CLOCK_LEN   16u               /* mGBA's GBASavedataRTCBuffer */
#define GBA_SRAM_NAME   "save.sram"
#define GBA_CLOCK_NAME  "clock.rtc"

/* The sizes a GBA save chip comes in, smallest first. */
static const size_t CHIP_SIZES[] = { GBA_EEPROM_4K, GBA_EEPROM_64K, GBA_SRAM, GBA_FLASH, GBA_FLASH1M };
#define CHIP_SIZE_COUNT (sizeof(CHIP_SIZES) / sizeof(CHIP_SIZES[0]))

/* A game's save: the chip's bytes and, when an emulator kept it, the clock. */
typedef struct {
    uint8_t *chip;
    size_t   chip_len;
    size_t   file_len;   /* the save's length in its file, without a clock footer */
    bool     eeprom;
    bool     has_clock;
    uint8_t  clock[GBA_CLOCK_LEN];
} gba_save;

static void gba_free(gba_save *s) {
    free(s->chip);
    memset(s, 0, sizeof(*s));
}

static uint32_t features_of(const sigil_sync_request *req) {
    uint32_t f = req->save.features;
    if (req->save.result && req->save.result->struct_version >= SIGIL_RESULT_V3) f |= req->save.result->features;
    return f;
}

static bool all_ff(const uint8_t *p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (p[i] != 0xFF) return false;
    }
    return true;
}

/* The chip the ROM names, as its size; 0 when the ROM's facts don't say. An
 * EEPROM's size is in the game's code, so it starts at the larger. */
static size_t named_chip(uint32_t features) {
    if (features & SIGIL_FEATURE_GBA_SRAM) return GBA_SRAM;
    if (features & SIGIL_FEATURE_GBA_FLASH) return GBA_FLASH;
    if (features & SIGIL_FEATURE_GBA_FLASH1M) return GBA_FLASH1M;
    if (features & SIGIL_FEATURE_GBA_EEPROM) return GBA_EEPROM_64K;
    return 0;
}

/* Takes the save in `data`, any emulator's file: mGBA's clock footer comes
 * off, VBA Next's EEPROM comes from 128 KiB, and padding comes off down to
 * the chip's size. The size is the chip the ROM names, else the file's own
 * length; it never drops a byte that isn't 0xFF, growing to the next chip
 * size instead. An EEPROM keeps the size of a file that is exactly one; from
 * a padded file it is 512 bytes unless something past them isn't 0xFF. An
 * empty file holds no save. */
static int take_save(uint32_t features, const uint8_t *data, size_t len, gba_save *s) {
    if (!len) return SIGIL_OK;
    if (len > GBA_CLOCK_LEN && len % 512 == GBA_CLOCK_LEN) {
        len -= GBA_CLOCK_LEN;
        memcpy(s->clock, data + len, GBA_CLOCK_LEN);
        s->has_clock = true;
    }
    s->file_len = len;
    size_t named = named_chip(features);
    bool eeprom = (features & SIGIL_FEATURE_GBA_EEPROM) != 0;
    bool exact = len == GBA_EEPROM_4K || len == GBA_EEPROM_64K;
    if (len == VBA_NEXT_FILE) {
        exact = false;
        bool ram_blank = all_ff(data, VBA_NEXT_EEPROM);
        bool eeprom_blank = all_ff(data + VBA_NEXT_EEPROM, GBA_EEPROM_64K);
        if (eeprom || (!named && ram_blank && !eeprom_blank)) {
            data += VBA_NEXT_EEPROM;
            len = GBA_EEPROM_64K;
            eeprom = true;
        } else {
            len = VBA_NEXT_EEPROM;
        }
    }
    if (len > GBA_FLASH1M) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    if (!named) eeprom = eeprom || len == GBA_EEPROM_4K || len == GBA_EEPROM_64K;
    size_t size = eeprom ? GBA_EEPROM_64K : named ? named : len;
    for (size_t i = 0; i < CHIP_SIZE_COUNT && size < len && !all_ff(data + size, len - size); i++) {
        if (CHIP_SIZES[i] > size) size = CHIP_SIZES[i];
    }
    if (size > GBA_EEPROM_64K) eeprom = false;
    if (eeprom && exact) {
        size = len;
    } else if (eeprom && size == GBA_EEPROM_64K &&
               (len <= GBA_EEPROM_4K || all_ff(data + GBA_EEPROM_4K, (len < size ? len : size) - GBA_EEPROM_4K))) {
        size = GBA_EEPROM_4K;
    }
    s->chip = (uint8_t *)malloc(size);
    if (!s->chip) return SIGIL_ERR_OOM;
    memset(s->chip, 0xFF, size);
    memcpy(s->chip, data, len < size ? len : size);
    s->chip_len = size;
    s->eeprom = eeprom;
    return SIGIL_OK;
}

/* The game's save file the layout names, there or expected. GBA cores keep
 * no clock file: mGBA's libretro core exposes RETRO_MEMORY_RTC for GB carts
 * alone. */
typedef struct {
    char path[SIGIL_SAVE_PATH_MAX];
    bool present;
} gba_file;

static int file_of(const sigil_sync_request *req, gba_file *f) {
    memset(f, 0, sizeof(*f));
    sigil_save_unit *unit = NULL;
    int rc = sigil_save_resolve(&req->save, &unit);
    if (rc != SIGIL_OK) return rc;
    for (size_t i = 0; i < unit->member_count + unit->expected_count && !f->path[0]; i++) {
        bool present = i < unit->member_count;
        const sigil_save_member *m = present ? &unit->members[i] : &unit->expected[i - unit->member_count];
        if (m->role != SIGIL_SAVE_ROLE_PRIMARY) continue;
        snprintf(f->path, sizeof(f->path), "%s", m->path);
        f->present = present;
    }
    sigil_save_unit_free(unit);
    return SIGIL_OK;
}

static int read_local(const sigil_sync_request *req, const gba_file *f, gba_save *s) {
    memset(s, 0, sizeof(*s));
    if (!f->present) return SIGIL_OK;
    uint8_t *data = NULL;
    size_t len = 0;
    int rc = sigil_sync_read_file(req, f->path, VBA_NEXT_FILE + GBA_CLOCK_LEN, &data, &len);
    if (rc == SIGIL_OK) rc = take_save(features_of(req), data, len, s);
    free(data);
    return rc == SIGIL_ERR_NOT_FOUND ? SIGIL_ERR_IO : rc;
}

/* A cart with a clock: the ROM says so, or an emulator kept one. */
static bool clock_cart(const sigil_sync_request *req, const gba_save *a, const gba_save *b) {
    return (features_of(req) & SIGIL_FEATURE_RTC) || a->has_clock || (b && b->has_clock);
}

/* The save's identity: the chip's md5, or for a clock cart RomM's zip hash over save.sram alone. */
static void identity_of(bool clocked, const gba_save *s, char out[33]) {
    out[0] = '\0';
    if (!s->chip) return;
    sigil_named_md5 part;
    snprintf(part.name, sizeof(part.name), "%s", GBA_SRAM_NAME);
    sigil_md5_of(s->chip, s->chip_len, part.md5);
    if (clocked) sigil_named_hash(&part, 1, out);
    else memcpy(out, part.md5, 33);
}

static void artifact_of(const sigil_sync_request *req, bool zip, sigil_sync_result *r) {
    char stem[SIGIL_SAVE_ENTRY_MAX];
    sigil_content_stem(req->save.content_path, stem, sizeof(stem));
    snprintf(r->artifact, sizeof(r->artifact), "%s%s", stem, zip ? ".zip" : ".srm");
}

int sigil_sync_collect_gba(sigil_sync_ctx *x, sigil_sync_result *r) {
    gba_file f;
    gba_save s;
    memset(&s, 0, sizeof(s));
    int rc = file_of(x->req, &f);
    if (rc == SIGIL_OK) rc = read_local(x->req, &f, &s);
    if (rc == SIGIL_OK && s.chip) {
        bool clocked = clock_cart(x->req, &s, NULL);
        identity_of(clocked, &s, r->identity_hash);
        if (!clocked) {
            r->data = s.chip;
            r->len = s.chip_len;
            s.chip = NULL;
            memcpy(r->content_hash, r->identity_hash, sizeof(r->content_hash));
            r->shape = SIGIL_SAVE_SHAPE_SINGLE;
        } else {
            sigil_zip_member members[2] = { { GBA_SRAM_NAME, s.chip, s.chip_len },
                                            { GBA_CLOCK_NAME, s.clock, GBA_CLOCK_LEN } };
            size_t n = s.has_clock ? 2 : 1;
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
    gba_free(&s);
    return rc;
}

/* ---- restore ------------------------------------------------------------------ */

static bool ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), m = strlen(tail);
    return n >= m && strcmp(s + n - m, tail) == 0;
}

/* The save a unit carries: sigil's own, any emulator's file as clients
 * uploaded it before sigil, or a zip of one emulator file. */
static int read_unit(uint32_t features, const uint8_t *unit, size_t len, size_t received_len, gba_save *s,
                     sigil_sync_result *r) {
    memset(s, 0, sizeof(*s));
    if (len < 4 || sigil_read_le32(unit) != 0x04034b50u) {
        sigil_md5_of(unit, received_len, r->content_hash);
        r->shape = SIGIL_SAVE_SHAPE_SINGLE;
        return len ? take_save(features, unit, len, s) : SIGIL_ERR_NOT_FOUND;
    }
    sigil_zip_member *members = NULL;
    size_t count = 0;
    int rc = sigil_zip_read_mem(unit, len, VBA_NEXT_FILE + GBA_CLOCK_LEN, &members, &count);
    sigil_named_md5 *parts = rc == SIGIL_OK ? (sigil_named_md5 *)calloc(count ? count : 1, sizeof(*parts)) : NULL;
    if (rc == SIGIL_OK && !parts) rc = SIGIL_ERR_OOM;
    const sigil_zip_member *clock = NULL;
    for (size_t i = 0; i < count && rc == SIGIL_OK; i++) {
        const sigil_zip_member *m = &members[i];
        snprintf(parts[i].name, sizeof(parts[i].name), "%s", m->name);
        sigil_md5_of(m->data, m->len, parts[i].md5);
        if (strcmp(m->name, GBA_SRAM_NAME) == 0 || ends_with(m->name, ".srm") || ends_with(m->name, ".sav")) {
            gba_free(s);
            rc = take_save(features, m->data, m->len, s);
        } else if (strcmp(m->name, GBA_CLOCK_NAME) == 0 && m->len == GBA_CLOCK_LEN) {
            clock = m;
        } else {
            rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
        }
        if (rc != SIGIL_OK) snprintf(r->problem, sizeof(r->problem), "%s", m->name);
    }
    if (rc == SIGIL_OK && clock) {
        memcpy(s->clock, clock->data, GBA_CLOCK_LEN);
        s->has_clock = true;
    }
    if (rc == SIGIL_OK && count) sigil_named_hash(parts, count, r->content_hash);
    if (rc == SIGIL_OK && !s->chip) rc = SIGIL_ERR_NOT_FOUND;
    r->shape = SIGIL_SAVE_SHAPE_MULTI;
    free(parts);
    if (members) sigil_zip_members_free(members, count);
    return rc;
}

/* The bytes of the save file in the emulator's own form. gpSP: the chip,
 * then 0xFF to 128 KiB. VBA Next: 136 KiB, an EEPROM's bytes at 128 KiB and
 * anything else at 0. Standalone mGBA: the chip, then on a clock cart the
 * unit's clock, else the clock already there. Any other emulator: the chip,
 * or 0xFF to 128 KiB over a file already that long, as mGBA's libretro core
 * leaves one in RetroArch. */
static int save_file(const sigil_sync_request *req, const gba_save *in, const gba_save *local, uint8_t **out,
                     size_t *len) {
    const char *layout = req->save.layout ? req->save.layout : "";
    bool gpsp = strcmp(layout, "gpsp") == 0, vba_next = strcmp(layout, "vba_next") == 0;
    bool mgba = strcmp(layout, "mgba_standalone") == 0;
    const gba_save *timed = in->has_clock ? in : local->has_clock ? local : NULL;
    bool footer = mgba && timed;
    size_t at = vba_next && in->eeprom ? VBA_NEXT_EEPROM : 0;
    bool padded = !gpsp && !vba_next && !mgba && local->file_len == GBA_PADDED;
    size_t body = gpsp || padded ? GBA_PADDED : vba_next ? VBA_NEXT_FILE : in->chip_len;
    if (body < at + in->chip_len) body = at + in->chip_len;
    *len = body + (footer ? GBA_CLOCK_LEN : 0);
    *out = (uint8_t *)malloc(*len);
    if (!*out) return SIGIL_ERR_OOM;
    memset(*out, 0xFF, body);
    memcpy(*out + at, in->chip, in->chip_len);
    if (footer) memcpy(*out + body, timed->clock, GBA_CLOCK_LEN);
    return SIGIL_OK;
}

int sigil_sync_restore_gba(sigil_sync_ctx *x, const uint8_t *unit, size_t len, size_t received_len,
                           sigil_sync_result *r, char local_identity[33]) {
    gba_save in, local;
    gba_file f;
    memset(&local, 0, sizeof(local));
    int rc = read_unit(features_of(x->req), unit, len, received_len, &in, r);
    if (rc == SIGIL_OK) {
        artifact_of(x->req, r->shape == SIGIL_SAVE_SHAPE_MULTI, r);
        rc = file_of(x->req, &f);
    }
    if (rc == SIGIL_OK) rc = read_local(x->req, &f, &local);
    /* Both sides take the same rule, so one save gives one identity. */
    bool clocked = clock_cart(x->req, &in, &local);
    if (rc == SIGIL_OK) {
        identity_of(clocked, &in, r->identity_hash);
        identity_of(clocked, &local, local_identity);
    }
    bool already = false;
    if (rc == SIGIL_OK && sigil_sync_blocks_restore(x, local_identity, r->identity_hash,
                                                    sigil_sync_state_get(&x->state, "synced", x->game), &already)) {
        r->conflict = 1;
        rc = SIGIL_ERR_CONFLICT;
    }
    if (rc == SIGIL_OK && !f.path[0]) {
        snprintf(r->problem, sizeof(r->problem), "%s", GBA_SRAM_NAME);
        rc = SIGIL_ERR_NO_TARGET;
    }
    /* The same save in another emulator's form is written again, so the
     * file is the one this emulator reads. */
    uint8_t *file = NULL;
    size_t file_len = 0;
    if (rc == SIGIL_OK) rc = save_file(x->req, &in, &local, &file, &file_len);
    if (rc == SIGIL_OK && !sigil_sync_file_holds(x->req, f.path, file, file_len)) {
        rc = sigil_sync_put(x->req, f.path, file, file_len);
    }
    free(file);
    gba_free(&in);
    gba_free(&local);
    return rc;
}
