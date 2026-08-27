// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

#include <stdlib.h>

#define HFS0_ROOT_OFFSET 0xF000
#define HFS0_ENTRY_SIZE  64

#define MAX_SWITCH_FILE_COUNT  4096
#define MAX_SWITCH_STRING_TABLE (1024 * 1024)

static int hfs0_find_partition(const sigil_io *io, uint64_t part_off,
                                const char *target_name,
                                uint64_t *out_off, uint64_t *out_size) {
    uint8_t hdr[16];
    int rc = sigil_io_read_exact(io, part_off, hdr, 16);
    if (rc != SIGIL_OK) return rc;
    if (memcmp(hdr, "HFS0", 4) != 0) return SIGIL_ERR_NOT_FOUND;

    uint32_t file_count        = sigil_read_le32(hdr + 4);
    uint32_t string_table_size = sigil_read_le32(hdr + 8);
    if (file_count > MAX_SWITCH_FILE_COUNT || string_table_size > MAX_SWITCH_STRING_TABLE) {
        return SIGIL_ERR_NOT_FOUND;
    }

    size_t entries_size = (size_t)file_count * HFS0_ENTRY_SIZE;
    uint8_t *entries = (uint8_t *)malloc(entries_size);
    if (!entries) return SIGIL_ERR_OOM;
    rc = sigil_io_read_exact(io, part_off + 16, entries, entries_size);
    if (rc != SIGIL_OK) { free(entries); return rc; }

    uint8_t *string_table = (uint8_t *)malloc(string_table_size);
    if (!string_table) { free(entries); return SIGIL_ERR_OOM; }
    rc = sigil_io_read_exact(io, part_off + 16 + entries_size,
                              string_table, string_table_size);
    if (rc != SIGIL_OK) { free(entries); free(string_table); return rc; }

    uint64_t data_start = part_off + 16 + entries_size + string_table_size;
    size_t target_len = strlen(target_name);

    rc = SIGIL_ERR_NOT_FOUND;
    for (uint32_t i = 0; i < file_count; i++) {
        const uint8_t *e = entries + i * HFS0_ENTRY_SIZE;
        uint64_t fo = sigil_read_le64(e);
        uint64_t fs = sigil_read_le64(e + 8);
        uint32_t name_off = sigil_read_le32(e + 16);
        if (name_off >= string_table_size) continue;

        size_t name_len = 0;
        while (name_off + name_len < string_table_size
               && string_table[name_off + name_len] != 0) name_len++;
        if (name_len == target_len
            && memcmp(string_table + name_off, target_name, target_len) == 0) {
            *out_off = data_start + fo;
            *out_size = fs;
            rc = SIGIL_OK;
            break;
        }
    }

    free(entries);
    free(string_table);
    return rc;
}

static bool hfs0_name_is_nca(const uint8_t *name, size_t name_len) {
    if (name_len < 4) return false;
    return name[name_len - 4] == '.'
        && (name[name_len - 3] | 0x20) == 'n'
        && (name[name_len - 2] | 0x20) == 'c'
        && (name[name_len - 1] | 0x20) == 'a';
}

/* CNMT-preferred pass over HFS0 NCA entries; NOT_FOUND when no key material
 * or no Meta NCA yields a parseable content-meta. */
static int hfs0_try_cnmt(const sigil_io *io, uint64_t data_start,
                         const uint8_t *entries, uint32_t file_count,
                         const uint8_t *string_table, uint32_t string_table_size,
                         const uint8_t *header_key, const sigil_support *sup,
                         sigil_switch_title *out) {
    for (uint32_t i = 0; i < file_count; i++) {
        const uint8_t *e = entries + i * HFS0_ENTRY_SIZE;
        uint64_t fo = sigil_read_le64(e);
        uint64_t fs = sigil_read_le64(e + 8);
        uint32_t name_off = sigil_read_le32(e + 16);
        if (name_off >= string_table_size) continue;

        size_t name_len = 0;
        while (name_off + name_len < string_table_size
               && string_table[name_off + name_len] != 0) name_len++;
        if (!hfs0_name_is_nca(string_table + name_off, name_len)) continue;
        if (fs < SIGIL_NCA_HEADER_SIZE) continue;

        uint8_t raw[SIGIL_NCA_HEADER_SIZE];
        if (sigil_io_read_exact(io, data_start + fo, raw,
                                SIGIL_NCA_HEADER_SIZE) != SIGIL_OK) continue;

        uint8_t dec[SIGIL_NCA_HEADER_SIZE];
        if (sigil_nca_decrypt_header(raw, header_key, dec) != SIGIL_OK) continue;
        if (dec[0x205] != 1) continue; /* not a Meta NCA */

        if (sigil_cnmt_from_meta_nca(io, data_start + fo, dec, sup, out) == SIGIL_OK) {
            return SIGIL_OK;
        }
    }
    return SIGIL_ERR_NOT_FOUND;
}

static int hfs0_extract_title_from_partition(const sigil_io *io,
                                              uint64_t part_off,
                                              const uint8_t *header_key_or_null,
                                              const sigil_support *sup_or_null,
                                              sigil_switch_title *out) {
    memset(out, 0, sizeof(*out));

    uint8_t hdr[16];
    int rc = sigil_io_read_exact(io, part_off, hdr, 16);
    if (rc != SIGIL_OK) return rc;
    if (memcmp(hdr, "HFS0", 4) != 0) return SIGIL_ERR_NOT_FOUND;

    uint32_t file_count        = sigil_read_le32(hdr + 4);
    uint32_t string_table_size = sigil_read_le32(hdr + 8);
    if (file_count > MAX_SWITCH_FILE_COUNT || string_table_size > MAX_SWITCH_STRING_TABLE) {
        return SIGIL_ERR_NOT_FOUND;
    }

    size_t entries_size = (size_t)file_count * HFS0_ENTRY_SIZE;
    uint8_t *entries = (uint8_t *)malloc(entries_size ? entries_size : 1);
    if (!entries) return SIGIL_ERR_OOM;
    rc = sigil_io_read_exact(io, part_off + 16, entries, entries_size);
    if (rc != SIGIL_OK) { free(entries); return rc; }

    uint8_t *string_table = (uint8_t *)malloc(string_table_size ? string_table_size : 1);
    if (!string_table) { free(entries); return SIGIL_ERR_OOM; }
    rc = sigil_io_read_exact(io, part_off + 16 + entries_size,
                              string_table, string_table_size);
    if (rc != SIGIL_OK) { free(entries); free(string_table); return rc; }

    uint64_t data_start = part_off + 16 + entries_size + string_table_size;

    /* Preferred: authoritative per-content facts from the CNMT. */
    if (header_key_or_null && sup_or_null) {
        if (hfs0_try_cnmt(io, data_start, entries, file_count,
                          string_table, string_table_size,
                          header_key_or_null, sup_or_null, out) == SIGIL_OK) {
            free(entries);
            free(string_table);
            return SIGIL_OK;
        }
    }

    bool needs_key = false;
    rc = SIGIL_ERR_NOT_FOUND;
    for (uint32_t i = 0; i < file_count; i++) {
        const uint8_t *e = entries + i * HFS0_ENTRY_SIZE;
        uint64_t fo = sigil_read_le64(e);
        uint64_t fs = sigil_read_le64(e + 8);
        uint32_t name_off = sigil_read_le32(e + 16);
        if (name_off >= string_table_size) continue;

        size_t name_len = 0;
        while (name_off + name_len < string_table_size
               && string_table[name_off + name_len] != 0) name_len++;
        if (!hfs0_name_is_nca(string_table + name_off, name_len)) continue;
        if (fs < SIGIL_NCA_HEADER_SIZE) continue;

        uint8_t header[SIGIL_NCA_HEADER_SIZE];
        if (sigil_io_read_exact(io, data_start + fo, header, SIGIL_NCA_HEADER_SIZE) != SIGIL_OK) continue;
        int nrc = sigil_nca_title_from_raw_header(header, header_key_or_null,
                                                  out->title_id);
        if (nrc == SIGIL_OK) {
            rc = SIGIL_OK;
            break;
        }
        if (nrc == SIGIL_ERR_NEEDS_KEY) needs_key = true;
    }
    if (rc != SIGIL_OK && needs_key) rc = SIGIL_ERR_NEEDS_KEY;

    free(entries);
    free(string_table);
    return rc;
}

int sigil_extract_xci(const sigil_io *io, const sigil_options *opts,
                      sigil_result *out) {
    uint64_t secure_off, secure_size;
    int rc = hfs0_find_partition(io, HFS0_ROOT_OFFSET, "secure",
                                  &secure_off, &secure_size);
    if (rc != SIGIL_OK) return rc;
    (void)secure_size;

    uint8_t hkey[32];
    const sigil_support *sup = (opts && opts->support) ? opts->support : NULL;
    bool have_key = (sup && sigil_resolve_header_key(sup, hkey) == SIGIL_OK);

    sigil_switch_title t;
    rc = hfs0_extract_title_from_partition(io, secure_off,
                                            have_key ? hkey : NULL,
                                            have_key ? sup : NULL, &t);
    if (rc != SIGIL_OK) return rc;

    sigil_apply_switch_title(out, &t);
    return SIGIL_OK;
}
