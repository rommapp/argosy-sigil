// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

#include <stdlib.h>

/* A .wua is a ZArchive, the same container Xenia writes as .zar for Xbox 360,
 * so the footer and name-table parsing live in src/zarchive.c and are shared.
 * Wii U only needs the directory listing: the title id is the name of a
 * top-level directory, so file contents are never decompressed here. */

#define WUA_FILE_ENTRY    SIGIL_ZAR_TREE_ENTRY
#define WUA_MAX_METADATA  SIGIL_ZAR_MAX_METADATA

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

    sigil_zar_footer ft;
    int rc = sigil_zar_read_footer(io, &ft);
    if (rc != SIGIL_OK) return rc;

    if (ft.names_size > WUA_MAX_METADATA || ft.file_tree_size > WUA_MAX_METADATA) {
        return SIGIL_ERR_NOT_FOUND;
    }

    uint8_t *names = (uint8_t *)malloc(ft.names_size);
    if (!names) return SIGIL_ERR_OOM;
    rc = sigil_io_read_exact(io, ft.names_off, names, (size_t)ft.names_size);
    if (rc != SIGIL_OK) { free(names); return rc; }

    uint8_t *tree = (uint8_t *)malloc(ft.file_tree_size);
    if (!tree) { free(names); return SIGIL_ERR_OOM; }
    rc = sigil_io_read_exact(io, ft.file_tree_off, tree, (size_t)ft.file_tree_size);
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
        if (sigil_zar_read_name(names, (size_t)ft.names_size, name_off,
                                name, sizeof(name)) == 0) continue;

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
