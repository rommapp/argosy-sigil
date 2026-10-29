// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

/* PSP UMD_DATA.BIN encodes the disc id followed by `|` and other fields,
 * e.g. "ULUS-10064|0123456789ABCDEF|...". The dashed form is what the disc
 * carries; PSP's SAVEDATA folder convention drops the dash. */

static int parse_disc_id(const uint8_t *buf, size_t len,
                         char raw[32], char canonical[32]) {
    const uint8_t *pipe = (const uint8_t *)memchr(buf, '|', len);
    if (!pipe) return SIGIL_ERR_NOT_FOUND;
    size_t cand_len = (size_t)(pipe - buf);

    while (cand_len > 0 && (buf[cand_len - 1] == ' ' || buf[cand_len - 1] == '\t')) cand_len--;
    size_t start = 0;
    while (start < cand_len && (buf[start] == ' ' || buf[start] == '\t')) start++;
    cand_len -= start;
    if (cand_len > 31) return SIGIL_ERR_NOT_FOUND;

    char tmp[32];
    memcpy(tmp, buf + start, cand_len);
    tmp[cand_len] = '\0';

    if (cand_len != 9 && cand_len != 10) return SIGIL_ERR_NOT_FOUND;
    for (int i = 0; i < 4; i++) {
        if (!sigil_is_upper(tmp[i])) return SIGIL_ERR_NOT_FOUND;
    }
    size_t digits_off = 4;
    if (cand_len == 10) {
        if (tmp[4] != '-') return SIGIL_ERR_NOT_FOUND;
        digits_off = 5;
    }
    for (int i = 0; i < 5; i++) {
        if (!sigil_is_dig(tmp[digits_off + i])) return SIGIL_ERR_NOT_FOUND;
    }

    if (cand_len == 9) {
        memcpy(raw, tmp, 4);
        raw[4] = '-';
        memcpy(raw + 5, tmp + 4, 5);
        raw[10] = '\0';
    } else {
        memcpy(raw, tmp, 10);
        raw[10] = '\0';
    }

    memcpy(canonical, tmp, 4);
    memcpy(canonical + 4, tmp + digits_off, 5);
    canonical[9] = '\0';
    return SIGIL_OK;
}

/* A game passes its save prefix to the savedata utility as a string compiled
 * into its executable, and a Store re-release often keeps the disc release's
 * prefix while UMD_DATA.BIN carries the Store id. Only a readable executable
 * is scanned: BOOT.BIN when it is a real ELF, else a decrypted EBOOT.BIN. */

#define PSP_SERIAL_LEN       9
#define PSP_SERIAL_MAX_SPAN  10
#define PSP_MAX_CANDIDATES   8
#define PSP_EXE_MAX_BYTES    (64u * 1024u * 1024u)

typedef struct {
    char ids[PSP_MAX_CANDIDATES][PSP_SERIAL_LEN + 1];
    int  count;
    bool overflow;
} psp_serial_set;

static bool is_serial_char(uint8_t c) {
    return sigil_is_upper((char)c) || sigil_is_dig((char)c);
}

static bool all_digits(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!sigil_is_dig((char)p[i])) return false;
    }
    return true;
}

/* Reads a serial starting at p: UL, UC or NP, two letters, then five digits,
 * optionally dashed after the letters. Writes the undashed form. */
static bool read_serial_at(const uint8_t *p, size_t avail, char out[PSP_SERIAL_LEN + 1]) {
    if (avail < PSP_SERIAL_LEN) return false;
    bool disc = p[0] == 'U' && (p[1] == 'L' || p[1] == 'C');
    bool store = p[0] == 'N' && p[1] == 'P';
    if (!disc && !store) return false;
    if (!sigil_is_upper((char)p[2]) || !sigil_is_upper((char)p[3])) return false;

    const uint8_t *digits;
    if (all_digits(p + 4, 5)) {
        digits = p + 4;
    } else if (avail >= PSP_SERIAL_MAX_SPAN && p[4] == '-' && all_digits(p + 5, 5)) {
        digits = p + 5;
    } else {
        return false;
    }
    memcpy(out, p, 4);
    memcpy(out + 4, digits, 5);
    out[PSP_SERIAL_LEN] = '\0';
    return true;
}

static void add_serial(psp_serial_set *set, const char id[PSP_SERIAL_LEN + 1]) {
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->ids[i], id) == 0) return;
    }
    if (set->count == PSP_MAX_CANDIDATES) {
        set->overflow = true;
        return;
    }
    memcpy(set->ids[set->count++], id, PSP_SERIAL_LEN + 1);
}

static bool set_contains(const psp_serial_set *set, const char *id) {
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->ids[i], id) == 0) return true;
    }
    return false;
}

static bool is_elf(const uint8_t *sector) {
    return sector[0] == 0x7F && sector[1] == 'E' && sector[2] == 'L' && sector[3] == 'F';
}

/* Collects the serials in a file, carrying the last few bytes of each sector
 * into the next so a serial split across two sectors is still read. */
static int scan_serials(const sigil_io *io, const sigil_iso_file_loc *file, psp_serial_set *set) {
    uint8_t win[PSP_SERIAL_MAX_SPAN + SIGIL_ISO_SECTOR_SIZE];
    size_t have = 0;
    uint8_t before = 0;
    uint32_t remaining = file->length < PSP_EXE_MAX_BYTES ? file->length : PSP_EXE_MAX_BYTES;

    for (uint32_t lba = file->lba; remaining > 0; lba++) {
        int rc = sigil_iso_read_sector(io, lba, win + have);
        if (rc != SIGIL_OK) return rc;
        size_t n = remaining < SIGIL_ISO_SECTOR_SIZE ? remaining : SIGIL_ISO_SECTOR_SIZE;
        have += n;
        remaining -= (uint32_t)n;

        size_t limit = remaining == 0 ? have
                     : (have > PSP_SERIAL_MAX_SPAN ? have - PSP_SERIAL_MAX_SPAN : 0);
        for (size_t i = 0; i < limit; i++) {
            uint8_t prev = i == 0 ? before : win[i - 1];
            char id[PSP_SERIAL_LEN + 1];
            if (!is_serial_char(prev) && read_serial_at(win + i, have - i, id)) {
                add_serial(set, id);
            }
        }
        if (limit > 0) before = win[limit - 1];
        memmove(win, win + limit, have - limit);
        have -= limit;
    }
    return SIGIL_OK;
}

static bool find_readable_executable(const sigil_io *io, uint32_t root_lba, uint32_t root_len,
                                     sigil_iso_file_loc *out) {
    sigil_iso_file_loc game, sysdir;
    if (sigil_iso_find_file(io, root_lba, root_len, "PSP_GAME", &game) != SIGIL_OK) return false;
    if (sigil_iso_find_file(io, game.lba, game.length, "SYSDIR", &sysdir) != SIGIL_OK) return false;

    static const char *const names[] = { "BOOT.BIN", "EBOOT.BIN" };
    uint8_t head[SIGIL_ISO_SECTOR_SIZE];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        sigil_iso_file_loc exe;
        if (sigil_iso_find_file(io, sysdir.lba, sysdir.length, names[i], &exe) != SIGIL_OK) continue;
        if (exe.length < 4) continue;
        if (sigil_iso_read_sector(io, exe.lba, head) != SIGIL_OK) continue;
        if (is_elf(head)) {
            *out = exe;
            return true;
        }
    }
    return false;
}

static bool is_store_serial(const char *id) {
    return id[0] == 'N' && id[1] == 'P';
}

/* Disc releases also build their prefix at run time and carry stray serials
 * (placeholders, other regions, development ids), so only a Store id is
 * replaced, and only by a real disc-release id. */
static bool is_disc_release_serial(const char *id) {
    if (id[0] != 'U' || (id[1] != 'L' && id[1] != 'C')) return false;
    return strcmp(id + 4, "00000") != 0;
}

/* save_id is set only for a Store release whose executable never names the
 * disc id and names exactly one disc-release serial; every other outcome
 * leaves it to the default, the disc id. */
static void resolve_save_prefix(const sigil_io *io, uint32_t root_lba, uint32_t root_len,
                                sigil_result *out) {
    if (!is_store_serial(out->title_id)) return;

    sigil_iso_file_loc exe;
    if (!find_readable_executable(io, root_lba, root_len, &exe)) return;

    psp_serial_set set = {0};
    if (scan_serials(io, &exe, &set) != SIGIL_OK) return;
    if (set.overflow || set.count != 1 || set_contains(&set, out->title_id)) return;
    if (!is_disc_release_serial(set.ids[0])) return;
    memcpy(out->save_id, set.ids[0], PSP_SERIAL_LEN + 1);
}

int sigil_extract_psp(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out) {
    (void)filename_hint;
    (void)opts;

    sigil_result_init(out);
    out->platform = SIGIL_PLATFORM_PSP;
    out->usage = SIGIL_USAGE_FOLDER_PREFIX;

    uint32_t root_lba, root_len;
    int rc = sigil_iso_read_pvd_root(io, &root_lba, &root_len);
    if (rc != SIGIL_OK) return rc;

    sigil_iso_file_loc umd;
    rc = sigil_iso_find_file(io, root_lba, root_len, "UMD_DATA.BIN", &umd);
    if (rc != SIGIL_OK) return rc;

    uint8_t buf[SIGIL_ISO_SECTOR_SIZE];
    rc = sigil_iso_read_sector(io, umd.lba, buf);
    if (rc != SIGIL_OK) return rc;

    size_t scan_len = umd.length < SIGIL_ISO_SECTOR_SIZE ? umd.length : SIGIL_ISO_SECTOR_SIZE;
    if (scan_len < 10) return SIGIL_ERR_NOT_FOUND;

    rc = parse_disc_id(buf, scan_len, out->raw_serial, out->title_id);
    if (rc != SIGIL_OK) return rc;

    out->source = SIGIL_SOURCE_BINARY;
    resolve_save_prefix(io, root_lba, root_len, out);
    return SIGIL_OK;
}
