// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

#define PS2_SAVE_PREFIX     "BA"
#define PS2_SAVE_PREFIX_LEN 2

/* Second letter of the memory-card directory prefix, keyed off the serial's
 * region letter (SLUS->BA, SLES->BE, SLPS/SLPM/SLKA->BI). Games create their
 * save folders with this prefix; anything after the serial (AC04, SYS, RD0)
 * is a per-artifact suffix, so save_id is the prefix stem and usage is
 * folder-prefix: consumers enumerate every folder starting with it. */
static const char *ps2_region_prefix(const char title_id[32]) {
    switch (title_id[2]) {
    case 'E': return "BE";
    case 'P': case 'J': case 'K': return "BI";
    default:  return PS2_SAVE_PREFIX;
    }
}

void sigil_ps2_save_id_stem(const char title_id[32], char out_save_id[32]) {
    size_t len = strlen(title_id);
    if (len + PS2_SAVE_PREFIX_LEN >= 32) {
        out_save_id[0] = '\0';
        return;
    }
    memcpy(out_save_id, ps2_region_prefix(title_id), PS2_SAVE_PREFIX_LEN);
    memcpy(out_save_id + PS2_SAVE_PREFIX_LEN, title_id, len);
    out_save_id[PS2_SAVE_PREFIX_LEN + len] = '\0';
}

int sigil_extract_ps2(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out) {
    (void)filename_hint;
    (void)opts;

    sigil_result_init(out);
    out->platform = SIGIL_PLATFORM_PS2;
    out->usage = SIGIL_USAGE_FOLDER_PREFIX;

    uint32_t root_lba, root_len;
    int rc = sigil_iso_read_pvd_root(io, &root_lba, &root_len);
    if (rc != SIGIL_OK) return rc;

    sigil_iso_file_loc cnf;
    rc = sigil_iso_find_file(io, root_lba, root_len, "SYSTEM.CNF", &cnf);
    if (rc != SIGIL_OK) return rc;

    uint8_t buf[SIGIL_ISO_SECTOR_SIZE];
    rc = sigil_iso_read_sector(io, cnf.lba, buf);
    if (rc != SIGIL_OK) return rc;

    size_t scan_len = cnf.length < SIGIL_ISO_SECTOR_SIZE ? cnf.length : SIGIL_ISO_SECTOR_SIZE;
    rc = sigil_cnf_parse_boot(buf, scan_len, "BOOT2", out->raw_serial, out->title_id);
    if (rc != SIGIL_OK) return rc;

    out->source = SIGIL_SOURCE_BINARY;
    sigil_ps2_save_id_stem(out->title_id, out->save_id);
    return SIGIL_OK;
}
