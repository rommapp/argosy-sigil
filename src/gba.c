// SPDX-License-Identifier: MPL-2.0
/* Game Boy Advance carts: the header check, and the save chip and clock the
 * ROM's Nintendo library ids name. Saves key on the content file's stem, so
 * the result carries features only. */
#include "sigil_internal.h"

#include <stdlib.h>

#define GBA_HEADER_LEN    0xC0
#define GBA_FIXED         0xB2   /* 0x96 on every cart */
#define GBA_FIXED_VALUE   0x96
#define GBA_COMPLEMENT    0xBD
#define GBA_SCAN_CHUNK    (1u << 20)
#define GBA_ID_MAX        16     /* the longest id, FLASH512_V, and its three digits */

/* The ids Nintendo's libraries leave in the ROM, each followed by a
 * three-digit version (EEPROM_V124); an id counts only with its digits,
 * found at any byte. Emulators that detect the save chip from the ROM
 * search for the same ids by looser rules: VBA-M takes the prefixes with
 * no digits at 4-byte steps and picks among them by kind and order
 * (visualboyadvance-m v2.2.3 src/core/gba/gbaFlash.cpp
 * flashDetectSaveType); gpSP applies its game table first, then takes the
 * ids with no digits at any byte of the first 1 MiB; finding none there, it
 * gives a Pokemon ROM (POKEMON at 0xA0, or the game code AXV, AXP, BPE, BPR
 * or BPG) 128 KiB flash, and otherwise searches the whole ROM at 4-byte
 * steps; it ranks EEPROM over SRAM over flash (libretro/gpsp at 5819380
 * gba_memory.c load_game_config_over, detect_backup_subcircuit,
 * rom_is_pokemon_family, rom_scan_signatures_in_memory). */
static const struct { const char *id; uint32_t feature; } GBA_IDS[] = {
    { "SRAM_V", SIGIL_FEATURE_GBA_SRAM },
    { "SRAM_F_V", SIGIL_FEATURE_GBA_SRAM },
    { "FLASH_V", SIGIL_FEATURE_GBA_FLASH },
    { "FLASH512_V", SIGIL_FEATURE_GBA_FLASH },
    { "FLASH1M_V", SIGIL_FEATURE_GBA_FLASH1M },
    { "EEPROM_V", SIGIL_FEATURE_GBA_EEPROM },
    { "SIIRTC_V", SIGIL_FEATURE_RTC },
};
#define GBA_ID_COUNT (sizeof(GBA_IDS) / sizeof(GBA_IDS[0]))
#define GBA_CHIPS (SIGIL_FEATURE_GBA_SRAM | SIGIL_FEATURE_GBA_FLASH | SIGIL_FEATURE_GBA_FLASH1M | SIGIL_FEATURE_GBA_EEPROM)

/* The BIOS refuses a cart whose complement check over 0xA0..0xBC fails, so a
 * mismatch means the bytes are not a GBA header. */
static bool gba_header_ok(const uint8_t *h) {
    uint8_t x = 0;
    for (uint32_t i = 0xA0; i < GBA_COMPLEMENT; i++) x = (uint8_t)(x - h[i]);
    return h[GBA_FIXED] == GBA_FIXED_VALUE && (uint8_t)(x - 0x19) == h[GBA_COMPLEMENT];
}

static bool digits_at(const uint8_t *p) {
    return p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9';
}

/* The features of every library id that starts in buf[0..scan). The bytes
 * up to `len` may finish an id that starts before `scan`. */
static uint32_t ids_in(const uint8_t *buf, size_t scan, size_t len) {
    uint32_t found = 0;
    for (size_t i = 0; i < scan; i++) {
        uint8_t c = buf[i];
        if (c != 'S' && c != 'F' && c != 'E') continue;
        for (size_t k = 0; k < GBA_ID_COUNT; k++) {
            size_t n = strlen(GBA_IDS[k].id);
            if (i + n + 3 <= len && memcmp(buf + i, GBA_IDS[k].id, n) == 0 && digits_at(buf + i + n)) {
                found |= GBA_IDS[k].feature;
            }
        }
    }
    return found;
}

/* The ids the whole ROM carries, read in chunks that overlap by one id. */
static int gba_scan(const sigil_io *io, uint32_t *found) {
    *found = 0;
    uint8_t *buf = (uint8_t *)malloc(GBA_SCAN_CHUNK + GBA_ID_MAX);
    if (!buf) return SIGIL_ERR_OOM;
    int rc = SIGIL_OK;
    for (uint64_t off = 0;;) {
        size_t got = 0;
        rc = sigil_io_read_upto(io, off, buf, GBA_SCAN_CHUNK + GBA_ID_MAX, &got);
        if (rc != SIGIL_OK || got == 0) break;
        size_t scan = got > GBA_SCAN_CHUNK ? GBA_SCAN_CHUNK : got;
        *found |= ids_in(buf, scan, got);
        if (got <= GBA_SCAN_CHUNK) break;
        off += GBA_SCAN_CHUNK;
    }
    free(buf);
    return rc;
}

int sigil_extract_gba(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out) {
    (void)filename_hint;
    (void)opts;

    sigil_result_init(out);
    out->platform = SIGIL_PLATFORM_GBA;
    out->usage    = SIGIL_USAGE_FILE_PREFIX;

    if (!io || !io->read) return SIGIL_ERR_INVALID_ARG;

    uint8_t header[GBA_HEADER_LEN];
    if (sigil_io_read_exact(io, 0, header, sizeof(header)) != SIGIL_OK) return SIGIL_ERR_IO;
    if (!gba_header_ok(header)) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    uint32_t found = 0;
    int rc = gba_scan(io, &found);
    if (rc != SIGIL_OK) return rc;
    uint32_t chips = found & GBA_CHIPS;
    /* One kind of chip, or none: a ROM naming two can't say which it has. */
    if (chips & (chips - 1)) found &= ~GBA_CHIPS;
    out->features = found;
    out->source = SIGIL_SOURCE_BINARY;
    return SIGIL_OK;
}
