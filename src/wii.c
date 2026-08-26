// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

/* GameCube/Wii disc header: 4-byte ASCII gameId at offset 0, followed by a
 * magic word identifying the console. Containers keep that header intact but
 * move it: RVZ at 0x58, WBFS at the start of its first hd sector. Save form is
 * the hex encoding of the 4 ASCII bytes. */

#define WII_MAGIC 0x5D1C9EA3u
#define GC_MAGIC  0xC2339F3Du

/* Each console stamps its magic at its own offset in the disc header. */
#define WII_MAGIC_OFF 0x18
#define GC_MAGIC_OFF  0x1C

/* WBFS container header: "WBFS", u32 sector count, then the two shift values. */
#define WBFS_HD_SEC_SZ_S_OFF 8

static int read_be32(const sigil_io *io, uint64_t off, uint32_t *out) {
    uint8_t b[4];
    int rc = sigil_io_read_exact(io, off, b, sizeof(b));
    if (rc != SIGIL_OK) return rc;
    *out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
    return SIGIL_OK;
}

/* True when a real disc header starts at `off`. Guards against reading a
 * container's own bytes as a game id: "WBFS" is four uppercase characters and
 * passes every character check, so only the magic word tells them apart. */
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

static int extract_gameid(const sigil_io *io, uint64_t off, char raw[32], char canonical[32]) {
    uint8_t bytes[4];
    int rc = sigil_io_read_exact(io, off, bytes, sizeof(bytes));
    if (rc != SIGIL_OK) return rc;

    for (int i = 0; i < 4; i++) {
        if (!(sigil_is_upper((char)bytes[i]) || sigil_is_dig((char)bytes[i]))) {
            return SIGIL_ERR_NOT_FOUND;
        }
    }

    memcpy(raw, bytes, 4);
    raw[4] = '\0';
    sigil_hex_encode_4(bytes, canonical);
    return SIGIL_OK;
}

/* WBFS stores the disc image in remapped blocks behind a container header. The
 * first hd sector holds that header, so the wrapped disc header begins at the
 * sector size, which the container records as a shift. */
static int wbfs_disc_header_off(const sigil_io *io, uint64_t *out) {
    uint8_t shift;
    int rc = sigil_io_read_exact(io, WBFS_HD_SEC_SZ_S_OFF, &shift, 1);
    if (rc != SIGIL_OK) return rc;
    if (shift < 9 || shift > 16) return SIGIL_ERR_NOT_FOUND;
    *out = (uint64_t)1 << shift;
    return SIGIL_OK;
}

/* Dolphin builds the Wii NAND save directory with fmt "{:08x}"
 * (Source/Core/Common/NandPaths.cpp:65 GetTitleDataPath), so the on-disk name
 * of RZDE is 525a4445, not 525A4445. On case-sensitive storage the uppercase
 * form is a second directory beside the emulator's own and the saves split, so
 * save_id carries the lowercase path form while title_id and raw_serial keep
 * the canonical uppercase one. GameCube is excluded: its artifacts are .gci
 * files matched by prefix, not a NAND directory. */
static void wii_save_id(const char title_id[32], char out_save_id[32]) {
    sigil_lower_copy(title_id, out_save_id, 32);
}

static int extract_wii_or_gc(const sigil_io *io, sigil_platform platform,
                              sigil_result *out) {
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
