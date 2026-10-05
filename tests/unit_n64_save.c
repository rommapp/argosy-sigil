// SPDX-License-Identifier: MPL-2.0
/* The N64 save converter on synthetic bytes: byte order, which regions count
 * as used, EEPROM size, and the libretro .srm layout. */
#include "n64_save.h"
#include <stdio.h>
#include <stdlib.h>

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static n64_save *new_save(void) {
    n64_save *s = (n64_save *)calloc(1, sizeof(*s));
    if (!s) abort();
    s->eeprom_len = N64_EEPROM_4K;
    return s;
}

static void check_swap32(void) {
    uint8_t b[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    const uint8_t want[8] = { 4, 3, 2, 1, 8, 7, 6, 5 };
    n64_swap32(b, sizeof(b));
    if (memcmp(b, want, sizeof(b)) != 0) fail("swap32", "each 4-byte group reversed");
    n64_swap32(b, sizeof(b));
    if (b[0] != 1 || b[7] != 8) fail("swap32", "not its own inverse");
}

/* mupen64plus's formatted pak (mempak.c format_mempak) with a zero serial. */
static void check_formatted_pak(void) {
    uint8_t *pak = (uint8_t *)malloc(N64_PAK_SIZE);
    n64_pak_formatted(pak);
    const uint8_t tail[8] = { 0x00, 0x01, 0x01, 0x00, 0x01, 0x01, 0xFE, 0xF1 };
    if (memcmp(pak + 0x38, tail, 8) != 0 || memcmp(pak + 0xD8, tail, 8) != 0) fail("formatted pak", "id blocks");
    if (pak[0x101] != 0x71 || pak[0x10B] != 0x03 || pak[0x1FF] != 0x03 || pak[0x108] != 0) fail("formatted pak", "index");
    if (memcmp(pak + 0x100, pak + 0x200, 0x100) != 0) fail("formatted pak", "index backup");
    n64_save *s = new_save();
    n64_take(s, N64_PAK1, pak, N64_PAK_SIZE);
    if (s->present[N64_PAK1]) fail("formatted pak", "a pak with no notes counts as used");
    pak[0x10B] = 0x05;   /* inode 5 now a note's first page */
    pak[0x500] = 0x42;
    n64_take(s, N64_PAK1, pak, N64_PAK_SIZE);
    if (!s->present[N64_PAK1]) fail("formatted pak", "a pak with a note counts as unused");
    free(pak);
    free(s);
}

/* SRAM is word-swapped on disk; the neutral form is bus order. */
static void check_sram_order(void) {
    uint8_t *disk = (uint8_t *)malloc(N64_SRAM_SIZE);
    memset(disk, 0xFF, N64_SRAM_SIZE);
    const uint8_t word[4] = { 0x44, 0x33, 0x22, 0x11 };
    memcpy(disk + 8, word, 4);
    n64_save *s = new_save();
    n64_take(s, N64_SRAM, disk, N64_SRAM_SIZE);
    if (!s->present[N64_SRAM] || s->sram[8] != 0x11 || s->sram[11] != 0x44) fail("sram", "neutral is bus order");
    uint8_t *back = (uint8_t *)malloc(N64_SRAM_SIZE);
    if (n64_give(s, N64_SRAM, back) != N64_SRAM_SIZE || memcmp(back, disk, N64_SRAM_SIZE) != 0) {
        fail("sram", "doesn't go back to the bytes it came from");
    }
    free(back);
    free(disk);
    free(s);
}

/* Blank regions in either fill aren't used; a short Project64 file reads 0xFF past its end. */
static void check_presence(void) {
    n64_save *s = new_save();
    uint8_t zeros[64] = { 0 };
    n64_take(s, N64_FLASH, zeros, sizeof(zeros));
    if (s->present[N64_FLASH]) fail("presence", "0x00 holes count as used");
    uint8_t written[5] = { 0, 0, 0, 0, 0x5A };
    n64_take(s, N64_FLASH, written, sizeof(written));
    if (!s->present[N64_FLASH] || s->flash[7] != 0x5A || s->flash[8] != 0xFF) {
        fail("presence", "a short flash file, padded with 0xFF, swapped");
    }
    free(s);
}

/* EEPROM is 4 Kbit unless the tail holds data; given back at 0x800. */
static void check_eeprom_size(void) {
    uint8_t eep[N64_EEPROM_MAX];
    memset(eep, 0xFF, sizeof(eep));
    eep[3] = 0x10;
    n64_save *s = new_save();
    n64_take(s, N64_EEPROM, eep, sizeof(eep));
    if (s->eeprom_len != N64_EEPROM_4K || !s->present[N64_EEPROM]) fail("eeprom", "blank tail reads as 4 Kbit");
    eep[0x600] = 0x20;
    n64_take(s, N64_EEPROM, eep, sizeof(eep));
    if (s->eeprom_len != N64_EEPROM_MAX) fail("eeprom", "data past 0x200 reads as 16 Kbit");
    n64_take(s, N64_EEPROM, eep, N64_EEPROM_4K);
    uint8_t back[N64_EEPROM_MAX];
    if (n64_give(s, N64_EEPROM, back) != N64_EEPROM_MAX || back[3] != 0x10 || back[0x200] != 0xFF) {
        fail("eeprom", "given back at 0x800 padded with 0xFF");
    }
    if (n64_take_neutral(s, N64_EEPROM, eep, 0x400) || n64_take_neutral(s, N64_SRAM, eep, 0x200)) {
        fail("eeprom", "a neutral member of the wrong size is taken");
    }
    free(s);
}

/* The .srm puts each region at its offset; regions the unit lacks keep an
 * unused old region and are blank otherwise. */
static void check_blob(void) {
    n64_save *s = new_save();
    uint8_t eep[N64_EEPROM_4K];
    memset(eep, 0x11, sizeof(eep));
    n64_take_neutral(s, N64_EEPROM, eep, sizeof(eep));
    uint8_t *old = (uint8_t *)malloc(N64_BLOB_SIZE), *out = (uint8_t *)malloc(N64_BLOB_SIZE);
    memset(old, 0xFF, N64_BLOB_SIZE);
    for (size_t p = 0; p < N64_PAKS; p++) n64_pak_formatted(old + 0x800 + p * N64_PAK_SIZE);
    old[0x800 + 0x30] = 0x99;          /* pak 1's serial: unused, kept */
    old[0x20800 + 4] = 0x77;           /* SRAM in use by an older save: blanked */
    if (n64_give_blob(s, old, N64_BLOB_SIZE, out) != SIGIL_OK) fail("blob", "give failed");
    if (out[0] != 0x11 || out[0x1FF] != 0x11 || out[0x200] != 0xFF) fail("blob", "eeprom at 0x0, padded");
    if (out[0x830] != 0x99) fail("blob", "an unused pak already there isn't kept");
    if (out[0x20804] != 0xFF) fail("blob", "a region the unit lacks keeps another save's data");
    if (n64_give_blob(s, NULL, 0, out) != SIGIL_OK || out[0x830] != 0x00 || out[0x901] != 0x71) {
        fail("blob", "a new .srm's paks are formatted");
    }
    n64_save *back = new_save();
    n64_take_blob(back, out, N64_BLOB_SIZE);
    bool only_eeprom = back->present[N64_EEPROM];
    for (int r = N64_PAK1; r < N64_REGION_COUNT; r++) only_eeprom = only_eeprom && !back->present[r];
    if (!only_eeprom || back->eeprom_len != N64_EEPROM_4K || memcmp(back->eeprom, eep, sizeof(eep)) != 0) {
        fail("blob", "reading it back gives other regions");
    }
    free(back);
    free(old);
    free(out);
    free(s);
}

static void check_names(void) {
    for (int r = 0; r < N64_REGION_COUNT; r++) {
        if (n64_region_of(n64_region_name(r)) != r) fail("names", n64_region_name(r));
    }
    if (n64_region_of("save.sram") != -1) fail("names", "an unknown name maps to a region");
}

int main(void) {
    check_swap32();
    check_formatted_pak();
    check_sram_order();
    check_presence();
    check_eeprom_size();
    check_blob();
    check_names();
    printf("n64 save: %d failures\n", g_fails);
    return g_fails ? 1 : 0;
}
