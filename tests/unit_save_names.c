// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"
#include <stdio.h>
#include <string.h>

static int g_fails = 0;

static void expect(const char *label, bool ok) {
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", label);
        g_fails++;
    }
}

static const sigil_save_name_row ROWS[] = {
    { "saturn", "MK-81022", "PANDRA_ZWEI" },
    { "saturn", "T-17701G", "RAYMAN_NTS" },
    { "saturn", "GS-9001", "VFIGHTER2_X" },
    { "saturn", "MK-81041", "VFIGHTER2_X" },
    { "segacd", "G-6005", "DW__DATA_" },
};
#define COUNT (sizeof(ROWS) / sizeof(ROWS[0]))

static bool match(const char *platform, const char *name, const char *id) {
    const char *ids[] = { id };
    return sigil_save_names_match(ROWS, COUNT, platform, name, ids, 1);
}

int main(void) {
    expect("a name the game's row names", match("saturn", "PANDRA_ZWEI", "MK-81022"));
    expect("a longer name with the row's prefix", match("saturn", "RAYMAN_NTS1", "T-17701G"));
    expect("another game's name", !match("saturn", "PANDRA_3_01", "MK-81022"));
    expect("the right name on another platform", !match("segacd", "PANDRA_ZWEI", "MK-81022"));
    expect("a game without a row", !match("saturn", "PANDRA_ZWEI", "T-99999"));
    expect("a name two products share", !match("saturn", "VFIGHTER2_X", "GS-9001"));
    expect("a Sega CD code as the header spells it", match("segacd", "DW__DATA_00", "GM G-6005  -00"));
    expect("a Sega CD code with its version", match("segacd", "DW__DATA_01", "G-6005-00"));
    expect("a Sega CD code with the AI type prefix", match("segacd", "DW__DATA_00", "AI G-6005"));
    expect("an unknown type prefix isn't dropped", !match("segacd", "DW__DATA_00", "XX G-6005"));
    expect("a code that only starts like the row's", !match("segacd", "DW__DATA_00", "G-60051"));
    expect("the shipped table names Zwei's save",
           sigil_save_names_match(sigil_save_name_table, sigil_save_name_table_count, "saturn", "PANDRA_ZWEI",
                                  (const char *const[]){ "MK-81022" }, 1));

    if (g_fails) return 1;
    printf("unit_save_names: ok\n");
    return 0;
}
