// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"
#include <stdlib.h>

/* A PS3 title is identified by TITLE_ID in PARAM.SFO. The file is reached
 * three ways and all of them end in the same parse: the caller hands over the
 * SFO directly, hands over a game folder that a recursive search resolves to
 * one, or hands over a disc image.
 *
 * On a disc the SFO lives at PS3_GAME/PARAM.SFO, with a copy at the root on
 * some releases. aPS3e looks in exactly those two places
 * (MainActivity.java:1008) and so does this. */

#define PS3_SFO_MAX_BYTES (256u * 1024u)
#define PS3_SFO_MAGIC     0x46535000u  /* "\0PSF" little-endian */

#define PS3_GAME_DIR      "PS3_GAME"
#define PS3_SFO_NAME      "PARAM.SFO"

/* Reads an SFO of `len` bytes at `off` and pulls TITLE_ID out of it. */
static int ps3_title_from_sfo(const sigil_io *io, uint64_t off, size_t len,
                              char *out, size_t out_cap) {
    if (len == 0) return SIGIL_ERR_NOT_FOUND;
    if (len > PS3_SFO_MAX_BYTES) len = PS3_SFO_MAX_BYTES;

    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) return SIGIL_ERR_OOM;

    int got = io->read(io->ctx, off, buf, len);
    if (got <= 0) { free(buf); return SIGIL_ERR_IO; }

    int rc = sigil_sfo_get_string(buf, (size_t)got, "TITLE_ID", out, out_cap);
    free(buf);
    return rc;
}

/* Locates PARAM.SFO inside a disc image. PS3 discs carry a plain ISO9660
 * descriptor for their directory structure, so the existing helpers walk it
 * without needing a UDF reader. */
static int ps3_find_sfo_in_iso(const sigil_io *io, uint64_t *out_off, size_t *out_len) {
    uint32_t root_lba, root_len;
    int rc = sigil_iso_read_pvd_root(io, &root_lba, &root_len);
    if (rc != SIGIL_OK) return rc;

    sigil_iso_file_loc loc;

    sigil_iso_file_loc game_dir;
    if (sigil_iso_find_file(io, root_lba, root_len, PS3_GAME_DIR, &game_dir) == SIGIL_OK
        && sigil_iso_find_file(io, game_dir.lba, game_dir.length,
                               PS3_SFO_NAME, &loc) == SIGIL_OK) {
        *out_off = (uint64_t)loc.lba * SIGIL_ISO_SECTOR_SIZE;
        *out_len = loc.length;
        return SIGIL_OK;
    }

    /* Some releases also drop a copy beside PS3_GAME rather than inside it. */
    if (sigil_iso_find_file(io, root_lba, root_len, PS3_SFO_NAME, &loc) == SIGIL_OK) {
        *out_off = (uint64_t)loc.lba * SIGIL_ISO_SECTOR_SIZE;
        *out_len = loc.length;
        return SIGIL_OK;
    }

    return SIGIL_ERR_NOT_FOUND;
}

int sigil_extract_ps3(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out) {
    (void)filename_hint;
    (void)opts;

    sigil_result_init(out);
    out->platform     = SIGIL_PLATFORM_PS3;
    /* Saves land in dev_hdd0/home/<user>/savedata under directories that start
     * with the title id and carry a per-artifact suffix (BCUS98233GAMEDATA,
     * BCUS98233-AUTOSAVE), so consumers enumerate by prefix rather than
     * expecting one exact folder. */
    out->usage        = SIGIL_USAGE_FOLDER_PREFIX;
    out->experimental = 1;

    if (!io || !io->read || !io->size) return SIGIL_ERR_INVALID_ARG;

    int64_t total = io->size(io->ctx);
    if (total <= 0) return SIGIL_ERR_IO;

    /* Decide from the magic which of the two shapes this is, so a disc image
     * is never parsed as though its first sector were an SFO. */
    uint64_t sfo_off = 0;
    size_t   sfo_len = (size_t)((uint64_t)total < PS3_SFO_MAX_BYTES
                                ? (uint64_t)total : PS3_SFO_MAX_BYTES);

    uint8_t magic[4];
    bool is_bare_sfo = sigil_io_read_exact(io, 0, magic, sizeof(magic)) == SIGIL_OK
                       && sigil_read_le32(magic) == PS3_SFO_MAGIC;

    if (!is_bare_sfo) {
        int rc = ps3_find_sfo_in_iso(io, &sfo_off, &sfo_len);
        if (rc != SIGIL_OK) return rc;
    }

    char title_id[32] = {0};
    int rc = ps3_title_from_sfo(io, sfo_off, sfo_len, title_id, sizeof(title_id));
    if (rc != SIGIL_OK) return rc;
    if (title_id[0] == '\0') return SIGIL_ERR_NOT_FOUND;

    size_t tid_len = strlen(title_id);
    if (tid_len >= sizeof(out->title_id)) return SIGIL_ERR_NOT_FOUND;

    memcpy(out->title_id, title_id, tid_len + 1);
    memcpy(out->raw_serial, title_id, tid_len + 1);
    out->source = SIGIL_SOURCE_BINARY;
    return SIGIL_OK;
}
