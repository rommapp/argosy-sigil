// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"
#include <stdio.h>

#define WII_MAGIC 0x5D1C9EA3u
#define GC_MAGIC  0xC2339F3Du

#define WII_MAGIC_OFF 0x18
#define GC_MAGIC_OFF  0x1C

#define WBFS_HD_SEC_SZ_S_OFF 8

#define WAD_HEADER_SIZE         0x20
#define WAD_TYPE_OFF            0x04
#define WAD_CERT_SIZE_OFF       0x08
#define WAD_TICKET_SIZE_OFF     0x10
#define WAD_TMD_SIZE_OFF        0x14
#define WAD_TICKET_TITLE_ID_OFF 0x1DC
#define WAD_TMD_TITLE_ID_OFF    0x18C
#define WAD_TYPE_INSTALLABLE    0x4973
#define WAD_TYPE_BOOT2          0x6962
#define WAD_TYPE_BACKUP         0x426B

static int read_be32(const sigil_io *io, uint64_t off, uint32_t *out) {
    uint8_t b[4];
    int rc = sigil_io_read_exact(io, off, b, sizeof(b));
    if (rc != SIGIL_OK) return rc;
    *out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
    return SIGIL_OK;
}

static bool disc_header_at(const sigil_io *io, uint64_t off) {
    uint32_t magic;
    if (read_be32(io, off + WII_MAGIC_OFF, &magic) == SIGIL_OK && magic == WII_MAGIC) {
        return true;
    }
    if (read_be32(io, off + GC_MAGIC_OFF, &magic) == SIGIL_OK && magic == GC_MAGIC) {
        return true;
    }
    return false;
}

static bool ascii_game_id(const uint8_t *bytes) {
    for (int i = 0; i < 4; i++) {
        if (!(sigil_is_upper((char)bytes[i]) || sigil_is_dig((char)bytes[i]))) return false;
    }
    return true;
}

static int extract_gameid(const sigil_io *io, uint64_t off, char raw[32], char canonical[32]) {
    uint8_t bytes[4];
    int rc = sigil_io_read_exact(io, off, bytes, sizeof(bytes));
    if (rc != SIGIL_OK) return rc;
    if (!ascii_game_id(bytes)) return SIGIL_ERR_NOT_FOUND;

    memcpy(raw, bytes, 4);
    raw[4] = '\0';
    sigil_hex_encode_4(bytes, canonical);
    return SIGIL_OK;
}

static int wbfs_disc_header_off(const sigil_io *io, uint64_t *out) {
    uint8_t shift;
    int rc = sigil_io_read_exact(io, WBFS_HD_SEC_SZ_S_OFF, &shift, 1);
    if (rc != SIGIL_OK) return rc;
    if (shift < 9 || shift > 16) return SIGIL_ERR_NOT_FOUND;
    *out = (uint64_t)1 << shift;
    return SIGIL_OK;
}

static void wii_save_id(const char title_id[32], char out_save_id[32]) {
    sigil_lower_copy(title_id, out_save_id, 32);
}

static uint64_t wad_align(uint64_t v) {
    return (v + 63) & ~(uint64_t)63;
}

static bool wad_header_at_zero(const sigil_io *io) {
    uint32_t size;
    uint8_t type[2];
    if (read_be32(io, 0, &size) != SIGIL_OK || size != WAD_HEADER_SIZE) return false;
    if (sigil_io_read_exact(io, WAD_TYPE_OFF, type, sizeof(type)) != SIGIL_OK) return false;
    uint16_t t = (uint16_t)((type[0] << 8) | type[1]);
    return t == WAD_TYPE_INSTALLABLE || t == WAD_TYPE_BOOT2 || t == WAD_TYPE_BACKUP;
}

static int wad_title_id_at(const sigil_io *io, uint64_t off, uint8_t id[8]) {
    int rc = sigil_io_read_exact(io, off, id, 8);
    if (rc != SIGIL_OK) return rc;
    return ascii_game_id(id + 4) ? SIGIL_OK : SIGIL_ERR_NOT_FOUND;
}

static int extract_wad(const sigil_io *io, sigil_result *out) {
    sigil_result_init(out);
    out->platform = SIGIL_PLATFORM_WII;
    out->usage = SIGIL_USAGE_FOLDER_SPLIT;

    uint32_t cert_size, ticket_size, tmd_size;
    int rc = read_be32(io, WAD_CERT_SIZE_OFF, &cert_size);
    if (rc == SIGIL_OK) rc = read_be32(io, WAD_TICKET_SIZE_OFF, &ticket_size);
    if (rc == SIGIL_OK) rc = read_be32(io, WAD_TMD_SIZE_OFF, &tmd_size);
    if (rc != SIGIL_OK) return rc;

    uint64_t ticket_off = wad_align(WAD_HEADER_SIZE) + wad_align(cert_size);
    uint8_t id[8];
    rc = SIGIL_ERR_NOT_FOUND;
    if (ticket_size > 0) rc = wad_title_id_at(io, ticket_off + WAD_TICKET_TITLE_ID_OFF, id);
    if (rc != SIGIL_OK && tmd_size > 0) {
        rc = wad_title_id_at(io, wad_align(ticket_off + ticket_size) + WAD_TMD_TITLE_ID_OFF, id);
    }
    if (rc != SIGIL_OK) return rc;

    char hi[9], lo[9];
    sigil_hex_encode_4(id, hi);
    sigil_hex_encode_4(id + 4, lo);
    memcpy(out->title_id, hi, 8);
    memcpy(out->title_id + 8, lo, 9);
    memcpy(out->raw_serial, id + 4, 4);
    out->raw_serial[4] = '\0';

    char hi_lower[9], lo_lower[9];
    sigil_lower_copy(hi, hi_lower, sizeof(hi_lower));
    sigil_lower_copy(lo, lo_lower, sizeof(lo_lower));
    snprintf(out->save_id, sizeof(out->save_id), "%s/%s", hi_lower, lo_lower);

    out->source = SIGIL_SOURCE_BINARY;
    return SIGIL_OK;
}

static int extract_wii_or_gc(const sigil_io *io, sigil_platform platform,
                              sigil_result *out) {
    if (platform == SIGIL_PLATFORM_WII && wad_header_at_zero(io)) return extract_wad(io, out);

    sigil_result_init(out);
    out->platform = platform;
    out->usage = (platform == SIGIL_PLATFORM_WII)
        ? SIGIL_USAGE_FOLDER_EXACT
        : SIGIL_USAGE_FILE_PREFIX;

    uint8_t magic[4];
    int rc = sigil_io_read_exact(io, 0, magic, sizeof(magic));
    if (rc != SIGIL_OK) return rc;

    uint64_t id_off;
    if (memcmp(magic, "WBFS", 4) == 0) {
        rc = wbfs_disc_header_off(io, &id_off);
        if (rc != SIGIL_OK) return rc;
        if (!disc_header_at(io, id_off)) return SIGIL_ERR_NOT_FOUND;
    } else if (memcmp(magic, "RVZ", 3) == 0) {
        id_off = 0x58;
    } else {
        id_off = 0x00;
    }

    rc = extract_gameid(io, id_off, out->raw_serial, out->title_id);
    if (rc != SIGIL_OK) return rc;

    if (platform == SIGIL_PLATFORM_WII) wii_save_id(out->title_id, out->save_id);

    out->source = SIGIL_SOURCE_BINARY;
    return SIGIL_OK;
}

int sigil_extract_wii(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out) {
    (void)filename_hint;
    (void)opts;
    return extract_wii_or_gc(io, SIGIL_PLATFORM_WII, out);
}

int sigil_extract_gamecube(const sigil_io *io, const char *filename_hint,
                           const sigil_options *opts, sigil_result *out) {
    (void)filename_hint;
    (void)opts;
    return extract_wii_or_gc(io, SIGIL_PLATFORM_GAMECUBE, out);
}
