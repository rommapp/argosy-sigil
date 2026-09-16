// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

static const uint8_t *memmem_bounded(const uint8_t *hay, size_t hay_len,
                                      const char *needle, size_t needle_len) {
    if (needle_len == 0 || hay_len < needle_len) return NULL;
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) return hay + i;
    }
    return NULL;
}

static bool starts_with(const uint8_t *p, size_t n, const char *prefix) {
    size_t plen = strlen(prefix);
    if (n < plen) return false;
    return memcmp(p, prefix, plen) == 0;
}

static bool is_letter(uint8_t c) {
    return sigil_is_upper(sigil_to_upper((char)c));
}

#define SERIAL_DIGITS 5

/* Matches LLLL, one separator, then five digits with at most one dot among
 * them, and returns the matched length (0 for no match). SLUS_005.94 is the
 * usual shape, but retail boot lines also read slus_005.94 (Metal Gear Solid),
 * SLUS_00.220 (Battle Arena Toshinden 2), SCUS-941.02 (Kileak) and
 * SLUSP012.06 (Dragon Warrior VII). */
static size_t match_serial_at(const uint8_t *p, size_t avail, char digits[SERIAL_DIGITS]) {
    if (avail < 4 + 1 + SERIAL_DIGITS) return 0;
    if (!is_letter(p[0]) || !is_letter(p[1])
        || !is_letter(p[2]) || !is_letter(p[3])) return 0;
    if (p[4] != '_' && p[4] != '.' && p[4] != '-' && !is_letter(p[4])) return 0;

    size_t i = 5;
    size_t n = 0;
    bool dot = false;
    while (i < avail && n < SERIAL_DIGITS) {
        if (sigil_is_dig((char)p[i])) {
            digits[n++] = (char)p[i];
        } else if (p[i] == '.' && !dot && n > 0) {
            dot = true;
        } else {
            return 0;
        }
        i++;
    }
    if (n < SERIAL_DIGITS) return 0;
    if (i < avail && sigil_is_dig((char)p[i])) return 0;
    return i;
}

int sigil_cnf_parse_boot(const uint8_t *cnf, size_t len,
                         const char *boot_key,
                         char raw[32], char canonical[32]) {
    size_t key_len = strlen(boot_key);

    size_t off = 0;
    while (off + key_len <= len) {
        const uint8_t *hit = memmem_bounded(cnf + off, len - off, boot_key, key_len);
        if (!hit) return SIGIL_ERR_NOT_FOUND;
        size_t pos = (size_t)(hit - cnf) + key_len;
        off = pos;

        /* Reject matches embedded inside longer tokens like "BOOT2" when the
         * caller asked for "BOOT". */
        if (pos >= len) continue;
        if (cnf[pos] != ' ' && cnf[pos] != '\t' && cnf[pos] != '=') continue;

        while (pos < len && (cnf[pos] == ' ' || cnf[pos] == '\t')) pos++;
        if (pos >= len || cnf[pos] != '=') continue;
        pos++;
        while (pos < len && (cnf[pos] == ' ' || cnf[pos] == '\t')) pos++;

        if (starts_with(cnf + pos, len - pos, "cdrom0:")) pos += 7;
        else if (starts_with(cnf + pos, len - pos, "cdrom:")) pos += 6;
        if (pos < len && cnf[pos] == '\\') pos++;

        size_t line_end = pos;
        while (line_end < len && cnf[line_end] != '\n' && cnf[line_end] != '\r') line_end++;

        /* Scan the bounded line rather than its start; this handles
         * subdirectory paths like cdrom:\MARL\SLUS_010.73;1 (Rhapsody). */
        char digits[SERIAL_DIGITS];
        size_t serial_pos = 0;
        size_t serial_len = 0;
        for (size_t i = pos; i < line_end; i++) {
            if (i > pos && (is_letter(cnf[i - 1]) || sigil_is_dig((char)cnf[i - 1]))) continue;
            serial_len = match_serial_at(cnf + i, line_end - i, digits);
            if (serial_len) {
                serial_pos = i;
                break;
            }
        }
        if (!serial_len) continue;

        memcpy(raw, cnf + serial_pos, serial_len);
        raw[serial_len] = '\0';
        for (size_t i = 0; i < 4; i++) canonical[i] = sigil_to_upper((char)cnf[serial_pos + i]);
        canonical[4] = '-';
        memcpy(canonical + 5, digits, SERIAL_DIGITS);
        canonical[5 + SERIAL_DIGITS] = '\0';
        return SIGIL_OK;
    }
    return SIGIL_ERR_NOT_FOUND;
}
