// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

#include <stdlib.h>

#define MAX_SWITCH_FILE_COUNT  4096
#define MAX_SWITCH_STRING_TABLE (1024 * 1024)

int sigil_extract_xci(const sigil_io *io, const sigil_options *opts,
                      sigil_result *out);

int sigil_pfs0_parse_header(const uint8_t hdr[16], sigil_pfs0_layout *out) {
    if (memcmp(hdr, "PFS0", 4) != 0) return SIGIL_ERR_NOT_FOUND;

    uint32_t file_count        = sigil_read_le32(hdr + 4);
    uint32_t string_table_size = sigil_read_le32(hdr + 8);
    if (file_count > MAX_SWITCH_FILE_COUNT || string_table_size > MAX_SWITCH_STRING_TABLE) {
        return SIGIL_ERR_NOT_FOUND;
    }

    out->file_count        = file_count;
    out->string_table_size = string_table_size;
    out->entries_off       = 16;
    out->string_table_off  = 16 + (uint64_t)file_count * 24;
    out->data_off          = out->string_table_off + string_table_size;
    return SIGIL_OK;
}

static bool name_has_nca_ext(const char *name, size_t name_len) {
    if (name_len < 4) return false;
    return name[name_len - 4] == '.'
        && (name[name_len - 3] | 0x20) == 'n'
        && (name[name_len - 2] | 0x20) == 'c'
        && (name[name_len - 1] | 0x20) == 'a';
}

static int scan_string_table_for_nca_title(const uint8_t *table, size_t len,
                                            char out_title_id[17]) {
    if (len < 20) return SIGIL_ERR_NOT_FOUND;
    for (size_t i = 0; i + 20 <= len; i++) {
        bool ok = true;
        for (int k = 0; k < 16; k++) {
            if (!sigil_is_hex((char)table[i + k])) { ok = false; break; }
        }
        if (!ok) continue;
        if (table[i + 16] != '.' ||
            (table[i + 17] != 'n' && table[i + 17] != 'N') ||
            (table[i + 18] != 'c' && table[i + 18] != 'C') ||
            (table[i + 19] != 'a' && table[i + 19] != 'A')) continue;

        char tmp[17];
        for (int k = 0; k < 16; k++) tmp[k] = sigil_to_upper((char)table[i + k]);
        tmp[16] = '\0';
        if (tmp[0] == '0' && tmp[1] == '1') {
            memcpy(out_title_id, tmp, 17);
            return SIGIL_OK;
        }
    }
    return SIGIL_ERR_NOT_FOUND;
}

/* Iterate PFS0 NCA entries and, for the Meta NCA, run the CNMT path. Requires
 * a header key and a prod.keys source; returns NOT_FOUND when neither applies
 * so callers cleanly fall through to the program_id/rights_id path. */
static int pfs0_try_cnmt(const sigil_io *io, uint64_t data_start,
                         const uint8_t *entries, uint32_t file_count,
                         const uint8_t *string_table, uint32_t string_table_size,
                         const uint8_t *header_key, const sigil_support *sup,
                         sigil_switch_title *out) {
    for (uint32_t i = 0; i < file_count; i++) {
        const uint8_t *e = entries + i * 24;
        uint64_t file_offset = sigil_read_le64(e);
        uint64_t file_size   = sigil_read_le64(e + 8);
        uint32_t name_offset = sigil_read_le32(e + 16);
        if (name_offset >= string_table_size) continue;

        uint32_t name_end = name_offset;
        while (name_end < string_table_size && string_table[name_end] != 0) name_end++;
        size_t name_len = name_end - name_offset;
        const char *name = (const char *)string_table + name_offset;
        if (!name_has_nca_ext(name, name_len)) continue;
        if (file_size < SIGIL_NCA_HEADER_SIZE) continue;

        uint8_t raw[SIGIL_NCA_HEADER_SIZE];
        if (sigil_io_read_exact(io, data_start + file_offset, raw,
                                SIGIL_NCA_HEADER_SIZE) != SIGIL_OK) continue;

        uint8_t dec[SIGIL_NCA_HEADER_SIZE];
        if (sigil_nca_decrypt_header(raw, header_key, dec) != SIGIL_OK) continue;
        if (dec[0x205] != 1) continue; /* not a Meta NCA */

        if (sigil_cnmt_from_meta_nca(io, data_start + file_offset, dec, sup,
                                     out) == SIGIL_OK) {
            return SIGIL_OK;
        }
    }
    return SIGIL_ERR_NOT_FOUND;
}

int sigil_pfs0_extract_title(const sigil_io *io, uint64_t partition_off,
                             const uint8_t *header_key_or_null,
                             const sigil_support *sup_or_null,
                             sigil_switch_title *out) {
    memset(out, 0, sizeof(*out));

    uint8_t hdr[16];
    int rc = sigil_io_read_exact(io, partition_off, hdr, 16);
    if (rc != SIGIL_OK) return rc;

    sigil_pfs0_layout lay;
    rc = sigil_pfs0_parse_header(hdr, &lay);
    if (rc != SIGIL_OK) return rc;

    size_t entries_size = (size_t)lay.file_count * 24;
    uint8_t *entries = (uint8_t *)malloc(entries_size ? entries_size : 1);
    if (!entries) return SIGIL_ERR_OOM;
    rc = sigil_io_read_exact(io, partition_off + lay.entries_off, entries, entries_size);
    if (rc != SIGIL_OK) { free(entries); return rc; }

    uint8_t *string_table = (uint8_t *)malloc(lay.string_table_size ? lay.string_table_size : 1);
    if (!string_table) { free(entries); return SIGIL_ERR_OOM; }
    rc = sigil_io_read_exact(io, partition_off + lay.string_table_off,
                             string_table, lay.string_table_size);
    if (rc != SIGIL_OK) { free(entries); free(string_table); return rc; }

    uint64_t data_start = partition_off + lay.data_off;

    /* Preferred: authoritative per-content facts from the CNMT. */
    if (header_key_or_null && sup_or_null) {
        if (pfs0_try_cnmt(io, data_start, entries, lay.file_count,
                          string_table, lay.string_table_size,
                          header_key_or_null, sup_or_null, out) == SIGIL_OK) {
            free(entries);
            free(string_table);
            return SIGIL_OK;
        }
    }

    /* Fallback: NCAs whose filenames are themselves the 16-hex title id
     * (decrypted dumps and homebrew). */
    if (scan_string_table_for_nca_title(string_table, lay.string_table_size,
                                        out->title_id) == SIGIL_OK) {
        free(entries);
        free(string_table);
        return SIGIL_OK;
    }

    /* Fallback: program_id / rights_id from the first resolvable NCA header. */
    bool needs_key = false;
    rc = SIGIL_ERR_NOT_FOUND;
    for (uint32_t i = 0; i < lay.file_count; i++) {
        const uint8_t *e = entries + i * 24;
        uint64_t file_offset = sigil_read_le64(e);
        uint64_t file_size   = sigil_read_le64(e + 8);
        uint32_t name_offset = sigil_read_le32(e + 16);
        if (name_offset >= lay.string_table_size) continue;

        uint32_t name_end = name_offset;
        while (name_end < lay.string_table_size && string_table[name_end] != 0) name_end++;
        size_t name_len = name_end - name_offset;
        const char *name = (const char *)string_table + name_offset;
        if (!name_has_nca_ext(name, name_len)) continue;
        if (file_size < SIGIL_NCA_HEADER_SIZE) continue;

        uint8_t header[SIGIL_NCA_HEADER_SIZE];
        if (sigil_io_read_exact(io, data_start + file_offset, header,
                                SIGIL_NCA_HEADER_SIZE) != SIGIL_OK) continue;

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

void sigil_apply_switch_title(sigil_result *out, const sigil_switch_title *t) {
    memcpy(out->title_id, t->title_id, 17);
    memcpy(out->raw_serial, t->title_id, 17);
    out->switch_content_type = t->content_type;
    out->title_version = t->version;
    out->source = SIGIL_SOURCE_BINARY;
}

int sigil_extract_switch(const sigil_io *io, const char *filename_hint,
                         const sigil_options *opts, sigil_result *out) {
    (void)filename_hint;
    if (!io) return SIGIL_ERR_INVALID_ARG;

    sigil_result_init(out);
    out->platform = SIGIL_PLATFORM_SWITCH;
    out->usage = SIGIL_USAGE_FOLDER_EXACT;

    uint8_t magic[4];
    int rc = sigil_io_read_exact(io, 0, magic, 4);
    if (rc != SIGIL_OK) return rc;

    uint8_t hkey[32];
    const sigil_support *sup = (opts && opts->support) ? opts->support : NULL;
    bool have_key = (sup && sigil_resolve_header_key(sup, hkey) == SIGIL_OK);

    if (memcmp(magic, "PFS0", 4) == 0) {
        sigil_switch_title t;
        rc = sigil_pfs0_extract_title(io, 0, have_key ? hkey : NULL,
                                      have_key ? sup : NULL, &t);
        if (rc != SIGIL_OK) return rc;
        sigil_apply_switch_title(out, &t);
        return SIGIL_OK;
    }

    return sigil_extract_xci(io, opts, out);
}
