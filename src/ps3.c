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
 * (MainActivity.java:1008) and so does this.
 *
 * A PSN package (.pkg) is the exception. Its PARAM.SFO sits in the encrypted
 * data, but the header and the metadata are in the clear, and the title id is
 * read from them, with no key. */

#define PS3_SFO_MAX_BYTES (256u * 1024u)

#define PS3_GAME_DIR      "PS3_GAME"
#define PS3_SFO_NAME      "PARAM.SFO"

/* The package header as RPCS3 reads it (PKGHeader in rpcs3/Crypto/unpkg.h at
 * c05ef18): the "\x7FPKG" magic, a big-endian release type at 0x04 (0x8000
 * retail, 0x0000 debug) and platform at 0x06 (0x0001 PS3, 0x0002 PSP and
 * Vita), the metadata's offset at 0x08 and its packet count at 0x0C, and the
 * content id at 0x30 (UP9000-NPUA80083_00-FLOWERPS3PRIVATE).
 * package_reader::read_header in unpkg.cpp refuses any other release type or
 * platform. */
#define PS3_PKG_MAGIC          "\x7FPKG"
#define PS3_PKG_RELEASE        0x04u
#define PS3_PKG_PLATFORM       0x06u
#define PS3_PKG_META_OFFSET    0x08u
#define PS3_PKG_META_COUNT     0x0Cu
#define PS3_PKG_TITLE_ID       0x37u
#define PS3_PKG_TITLE_ID_LEN   9u
#define PS3_PKG_HEADER_READ    (PS3_PKG_TITLE_ID + PS3_PKG_TITLE_ID_LEN)
#define PS3_PKG_RETAIL         0x8000u
#define PS3_PKG_DEBUG          0x0000u
#define PS3_PKG_PLATFORM_PS3   0x0001u
#define PS3_PKG_PACKET_HEAD    8u
#define PS3_PKG_INSTALL_DIR    0xAu
#define PS3_PKG_INSTALL_SKIP   8u
#define PS3_PKG_MAX_PACKETS    64u   /* sigil's own bound, so a crafted count can't stall a scan; RPCS3 has none */

/* The longest save folder name: CELL_SAVEDATA_DIRNAME_SIZE (32, in
 * rpcs3/Emu/Cell/Modules/cellSaveData.h at c05ef18) less its terminator. */
#define PS3_SAVE_DIRNAME_MAX   31u

/* Whether `len` bytes can begin a save folder's name. cellSaveData.cpp at
 * c05ef18 checks every save folder it lists (line 240) and the dirName a game
 * asks for (line 502) with sysutil_check_name_string (cellSysutil.cpp): 1 to
 * 31 characters from A to Z, 0 to 9, '-' and '_', with no '_' first. The title
 * id begins each of a game's save folder names. */
static bool ps3_save_folder_name(const char *s, size_t len) {
    if (len == 0 || len > PS3_SAVE_DIRNAME_MAX) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!(sigil_is_upper(c) || sigil_is_dig(c) || c == '-' || (c == '_' && i > 0))) return false;
    }
    return true;
}

/* Reads a package's title id: the name of the folder it installs into, as
 * package_reader::read_metadata in unpkg.cpp at c05ef18 settles it. That is
 * the nine characters at 0x37, the content id's title id (NPUA80083), unless a
 * metadata packet 0xA names another (lines 250 to 253 and 423 to 432). The
 * packets follow one another from the metadata offset, each a big-endian id
 * and size and then that many bytes, and the walk steps over each by its size
 * (line 551). A 0xA larger than eight bytes replaces the name with its text
 * after the first eight, up to a NUL; a smaller one is skipped, and a later
 * 0xA replaces an earlier one.
 *
 * A PSP or Vita package, or a release type RPCS3 doesn't install, is not a PS3
 * game. A packet table that runs past the file, or that counts more than
 * PS3_PKG_MAX_PACKETS packets, is not a package sigil reads, and a name that
 * can't begin a save folder's name gives no id. */
static int ps3_title_from_pkg(const sigil_io *io, uint64_t total, char *out, size_t out_cap) {
    uint8_t h[PS3_PKG_HEADER_READ];
    int rc = sigil_io_read_exact(io, 0, h, sizeof(h));
    if (rc != SIGIL_OK) return rc;

    uint16_t release = sigil_read_be16(h + PS3_PKG_RELEASE);
    if (release != PS3_PKG_RETAIL && release != PS3_PKG_DEBUG) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    if (sigil_read_be16(h + PS3_PKG_PLATFORM) != PS3_PKG_PLATFORM_PS3) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    /* One byte past the longest name, so a name that runs over shows. */
    char name[PS3_SAVE_DIRNAME_MAX + 2];
    memcpy(name, h + PS3_PKG_TITLE_ID, PS3_PKG_TITLE_ID_LEN);
    size_t name_len = PS3_PKG_TITLE_ID_LEN;

    uint64_t pos = sigil_read_be32(h + PS3_PKG_META_OFFSET);
    uint32_t count = sigil_read_be32(h + PS3_PKG_META_COUNT);
    if (count > PS3_PKG_MAX_PACKETS) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    for (uint32_t i = 0; i < count; i++) {
        if (pos + PS3_PKG_PACKET_HEAD > total) return SIGIL_ERR_UNSUPPORTED_FORMAT;
        uint8_t head[PS3_PKG_PACKET_HEAD];
        rc = sigil_io_read_exact(io, pos, head, sizeof(head));
        if (rc != SIGIL_OK) return rc;
        uint32_t id = sigil_read_be32(head);
        uint32_t size = sigil_read_be32(head + 4);
        uint64_t body = pos + PS3_PKG_PACKET_HEAD;
        if (body + size > total) return SIGIL_ERR_UNSUPPORTED_FORMAT;

        if (id == PS3_PKG_INSTALL_DIR && size > PS3_PKG_INSTALL_SKIP) {
            uint32_t text = size - PS3_PKG_INSTALL_SKIP;
            size_t want = text < sizeof(name) - 1 ? text : sizeof(name) - 1;
            rc = sigil_io_read_exact(io, body + PS3_PKG_INSTALL_SKIP, name, want);
            if (rc != SIGIL_OK) return rc;
            name_len = strnlen(name, want);
        }
        pos = body + size;
    }

    if (!ps3_save_folder_name(name, name_len) || out_cap <= name_len) return SIGIL_ERR_NOT_FOUND;
    memcpy(out, name, name_len);
    out[name_len] = '\0';
    return SIGIL_OK;
}

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

    /* Decide from the magic which shape this is, so a disc image is never
     * parsed as though its first sector were an SFO or a package header. */
    uint8_t magic[4];
    bool have_magic = sigil_io_read_exact(io, 0, magic, sizeof(magic)) == SIGIL_OK;

    char title_id[32] = {0};
    int rc;
    if (have_magic && memcmp(magic, PS3_PKG_MAGIC, sizeof(magic)) == 0) {
        rc = ps3_title_from_pkg(io, (uint64_t)total, title_id, sizeof(title_id));
    } else {
        uint64_t sfo_off = 0;
        size_t   sfo_len = (size_t)((uint64_t)total < PS3_SFO_MAX_BYTES
                                    ? (uint64_t)total : PS3_SFO_MAX_BYTES);
        bool is_bare_sfo = have_magic && sigil_read_le32(magic) == SIGIL_SFO_MAGIC;
        if (!is_bare_sfo) {
            rc = ps3_find_sfo_in_iso(io, &sfo_off, &sfo_len);
            if (rc != SIGIL_OK) return rc;
        }
        rc = ps3_title_from_sfo(io, sfo_off, sfo_len, title_id, sizeof(title_id));
    }
    if (rc != SIGIL_OK) return rc;
    if (title_id[0] == '\0') return SIGIL_ERR_NOT_FOUND;

    size_t tid_len = strlen(title_id);
    if (tid_len >= sizeof(out->title_id)) return SIGIL_ERR_NOT_FOUND;

    memcpy(out->title_id, title_id, tid_len + 1);
    memcpy(out->raw_serial, title_id, tid_len + 1);
    out->source = SIGIL_SOURCE_BINARY;
    return SIGIL_OK;
}
