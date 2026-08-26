// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

#if SIGIL_WITH_WIIU

#include <stdlib.h>

/* WUA / zArchive metadata layout (big-endian throughout). The footer at
 * end of file points to uncompressed names + file_tree sections; only
 * file *contents* go through zstd, which we don't read.
 *
 *   0x20 names_offset      0x28 names_size
 *   0x30 file_tree_offset  0x38 file_tree_size
 *   0x88 version (u32)     0x8C magic (u32)
 */

#define WUA_MAGIC         0x169F52D6u
#define WUA_VERSION_1     0x61BF3A01u
#define WUA_FOOTER_SIZE   144
#define WUA_FILE_ENTRY    16
#define WUA_MAX_METADATA  (16 * 1024 * 1024)

typedef struct {
    uint64_t names_offset, names_size;
    uint64_t file_tree_offset, file_tree_size;
} wua_footer;

static int read_footer(const sigil_io *io, wua_footer *out) {
    int64_t total = io->size(io->ctx);
    if (total < WUA_FOOTER_SIZE) return SIGIL_ERR_NOT_FOUND;

    uint8_t buf[WUA_FOOTER_SIZE];
    int rc = sigil_io_read_exact(io, (uint64_t)(total - WUA_FOOTER_SIZE),
                                  buf, WUA_FOOTER_SIZE);
    if (rc != SIGIL_OK) return rc;

    uint32_t version = sigil_read_be32(buf + 0x88);
    uint32_t magic   = sigil_read_be32(buf + 0x8C);
    if (magic != WUA_MAGIC || version != WUA_VERSION_1) return SIGIL_ERR_NOT_FOUND;

    out->names_offset     = sigil_read_be64(buf + 0x20);
    out->names_size       = sigil_read_be64(buf + 0x28);
    out->file_tree_offset = sigil_read_be64(buf + 0x30);
    out->file_tree_size   = sigil_read_be64(buf + 0x38);
    return SIGIL_OK;
}

static size_t read_name(const uint8_t *names, size_t names_len,
                        uint32_t offset, char *out, size_t out_size) {
    if (offset >= names_len) return 0;
    size_t len = names[offset];
    if (offset + 1 + len > names_len) return 0;
    if (len + 1 > out_size) return 0;
    memcpy(out, names + offset + 1, len);
    out[len] = '\0';
    return len;
}

/* Match `00050000<8 hex>` at start of `name`; emit canonical (last 8) +
 * raw (full 16) uppercase, plus the lowercase on-disk form of the canonical.
 *
 * Cemu builds the save directory with fmt "{:08x}"
 * (src/Cafe/TitleList/SaveInfo.cpp:16), so mlc01/usr/save/00050000/1010ec00 is
 * the path it creates. On case-sensitive storage an uppercase leaf becomes a
 * second directory beside it and the saves split, so only save_id is cased to
 * match; title_id and raw_serial stay uppercase. */
static bool match_wiiu_title_dir(const char *name, char canonical[9], char raw[17],
                                 char save[9]) {
    if (strlen(name) < 16) return false;
    if (memcmp(name, "00050000", 8) != 0) return false;
    for (int i = 0; i < 8; i++) {
        if (!sigil_is_hex(name[8 + i])) return false;
    }
    for (int i = 0; i < 8; i++) canonical[i] = sigil_to_upper(name[8 + i]);
    canonical[8] = '\0';
    for (int i = 0; i < 16; i++) raw[i] = sigil_to_upper(name[i]);
    raw[16] = '\0';
    sigil_lower_copy(canonical, save, 9);
    return true;
}

int sigil_extract_wiiu(const sigil_io *io, const char *filename_hint,
                       const sigil_options *opts, sigil_result *out) {
    (void)filename_hint;
    (void)opts;
    if (!io) return SIGIL_ERR_INVALID_ARG;

    sigil_result_init(out);
    out->platform = SIGIL_PLATFORM_WIIU;
    out->usage = SIGIL_USAGE_FOLDER_EXACT;

    wua_footer ft;
    int rc = read_footer(io, &ft);
    if (rc != SIGIL_OK) return rc;

    if (ft.names_size > WUA_MAX_METADATA || ft.file_tree_size > WUA_MAX_METADATA) {
        return SIGIL_ERR_NOT_FOUND;
    }

    uint8_t *names = (uint8_t *)malloc(ft.names_size);
    if (!names) return SIGIL_ERR_OOM;
    rc = sigil_io_read_exact(io, ft.names_offset, names, (size_t)ft.names_size);
    if (rc != SIGIL_OK) { free(names); return rc; }

    uint8_t *tree = (uint8_t *)malloc(ft.file_tree_size);
    if (!tree) { free(names); return SIGIL_ERR_OOM; }
    rc = sigil_io_read_exact(io, ft.file_tree_offset, tree, (size_t)ft.file_tree_size);
    if (rc != SIGIL_OK) { free(names); free(tree); return rc; }

    if (ft.file_tree_size < WUA_FILE_ENTRY) {
        free(names); free(tree);
        return SIGIL_ERR_NOT_FOUND;
    }

    /* Tree entries are 16 bytes; high bit of first u32 is the file/dir flag
     * (0 = directory). Root (index 0) must be a directory. */
    uint32_t root_name_off_and_flag = sigil_read_be32(tree);
    bool root_is_dir = (root_name_off_and_flag & 0x80000000u) == 0;
    if (!root_is_dir) {
        free(names); free(tree);
        return SIGIL_ERR_NOT_FOUND;
    }
    uint32_t root_start = sigil_read_be32(tree + 4);
    uint32_t root_count = sigil_read_be32(tree + 8);

    uint32_t total_nodes = (uint32_t)(ft.file_tree_size / WUA_FILE_ENTRY);

    rc = SIGIL_ERR_NOT_FOUND;
    for (uint32_t i = 0; i < root_count; i++) {
        uint32_t idx = root_start + i;
        if (idx >= total_nodes) break;

        const uint8_t *e = tree + (size_t)idx * WUA_FILE_ENTRY;
        uint32_t nf = sigil_read_be32(e);
        bool is_dir = (nf & 0x80000000u) == 0;
        if (!is_dir) continue;

        uint32_t name_off = nf & 0x7FFFFFFFu;
        char name[256];
        if (read_name(names, (size_t)ft.names_size, name_off, name, sizeof(name)) == 0) continue;

        char canonical[9], raw[17], save[9];
        if (match_wiiu_title_dir(name, canonical, raw, save)) {
            memcpy(out->title_id,   canonical, 9);
            memcpy(out->raw_serial, raw,       17);
            memcpy(out->save_id,    save,      9);
            out->source = SIGIL_SOURCE_BINARY;
            rc = SIGIL_OK;
            break;
        }
    }

    free(names);
    free(tree);
    return rc;
}

#endif
