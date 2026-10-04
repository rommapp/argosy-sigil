// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

/* Each row pairs a product code read from the disc header in a dump with a
 * save name that a real save of that disc carries (tests/fixtures/saves) or
 * that our own scan of the disc's code found it writing
 * (docs/platforms/saturn.md, "Mapping Saturn and Sega CD saves to games" and
 * "Finding the names a disc writes"). A name two products
 * write (the Japanese and US Virtua Fighter 2 both write VFIGHTER2_X) needs
 * both rows, so it matches neither. */
const sigil_save_name_row sigil_save_name_table[] = {
    { "saturn", "T-17701G", "RAYMAN_NTS" },     /* Rayman (USA): sample rayman-bkr-bcr */
    { "saturn", "MK-81022", "PANDRA_ZWEI" },    /* Panzer Dragoon II Zwei (USA): sample pandra-zwei-bup; scan */
    { "saturn", "T-30401H", "THREE_DIRTY" },    /* Three Dirty Dwarves (USA): sample three-dirty-dwarves-bup */
    { "saturn", "MK-81803", "B_RANGERS__" },    /* Burning Rangers (USA): scan */
    { "saturn", "MK-81035", "GUARDIAN_HE" },    /* Guardian Heroes (USA): scan */
    { "segacd", "G-6005", "DW__DATA_" },        /* Dark Wizard (Japan): scan; sample dark-wizard-brm (USA) */
    { "dreamcast", "T13301N", "GUNDAM_US_" },   /* Gundam Side Story 0079 (USA): sample gundam-0079-flycast */
    { "dreamcast", "T1219N", "PJUSTICE_" },     /* Project Justice (USA): sample project-justice-dci */
};
const size_t sigil_save_name_table_count = sizeof(sigil_save_name_table) / sizeof(sigil_save_name_table[0]);

/* The id with spaces and a leading Sega CD type dropped: "GM G-6005  -00"
 * reads "G-6005-00". */
static void squeeze(const char *id, char *out, size_t cap) {
    if (strncmp(id, "GM ", 3) == 0 || strncmp(id, "AI ", 3) == 0) id += 3;
    size_t n = 0;
    for (; *id && n + 1 < cap; id++) {
        if (*id != ' ') out[n++] = *id;
    }
    out[n] = '\0';
}

/* The row's code, with nothing after it or a "-" version after it. */
static bool code_matches(const char *code, const char *id) {
    char squeezed[64];
    squeeze(id, squeezed, sizeof(squeezed));
    size_t n = strlen(code);
    return strncmp(squeezed, code, n) == 0 && (squeezed[n] == '\0' || squeezed[n] == '-');
}

static bool is_game(const sigil_save_name_row *row, const char *const *ids, size_t id_count) {
    for (size_t i = 0; i < id_count; i++) {
        if (ids[i] && code_matches(row->code, ids[i])) return true;
    }
    return false;
}

bool sigil_save_names_match(const sigil_save_name_row *rows, size_t count, const char *platform, const char *name,
                            const char *const *ids, size_t id_count) {
    bool mine = false, theirs = false;
    for (size_t i = 0; i < count; i++) {
        const sigil_save_name_row *row = &rows[i];
        if (strcmp(row->platform, platform) != 0 || strncmp(name, row->prefix, strlen(row->prefix)) != 0) continue;
        if (is_game(row, ids, id_count)) mine = true;
        else theirs = true;
    }
    return mine && !theirs;
}
