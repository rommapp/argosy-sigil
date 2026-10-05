// SPDX-License-Identifier: MPL-2.0
/* N64 cartridge saves between the emulators' files and the neutral form:
 * eeprom, pak1 to pak4, sram and flash, each in the order the N64 bus reads
 * it, present ones only. docs/platforms/n64.md, "Save formats". */
#ifndef SIGIL_N64_SAVE_H
#define SIGIL_N64_SAVE_H

#include "sigil_internal.h"

#define N64_EEPROM_4K   0x200u
#define N64_EEPROM_MAX  0x800u
#define N64_PAK_SIZE    0x8000u
#define N64_PAKS        4u
#define N64_SRAM_SIZE   0x8000u
#define N64_FLASH_SIZE  0x20000u
#define N64_BLOB_SIZE   0x48800u   /* the libretro cores' .srm */

typedef enum {
    N64_EEPROM = 0,
    N64_PAK1, N64_PAK2, N64_PAK3, N64_PAK4,
    N64_SRAM,
    N64_FLASH,
    N64_REGION_COUNT
} n64_region;

/* A game's save, region by region, in bus order. A region the game doesn't
 * use has `present` false and holds nothing. */
typedef struct {
    uint8_t eeprom[N64_EEPROM_MAX];
    size_t  eeprom_len;               /* 0x200 or 0x800 */
    uint8_t pak[N64_PAKS][N64_PAK_SIZE];
    uint8_t sram[N64_SRAM_SIZE];
    uint8_t flash[N64_FLASH_SIZE];
    bool    present[N64_REGION_COUNT];
} n64_save;

/** The neutral member name of `region`: "eeprom", "pak1".., "sram", "flash". */
const char *n64_region_name(int region);
/** The region a neutral member name stands for, or -1. */
int n64_region_of(const char *name);
/** The region's bytes and length in `s`. */
uint8_t *n64_region_bytes(n64_save *s, int region, size_t *len);

/** Reverses each 4-byte group: the emulators keep SRAM and flash that way. Its own inverse. */
void n64_swap32(uint8_t *data, size_t len);

/** A controller pak as mupen64plus formats a new one, serial all zero (as the libretro cores' first one). */
void n64_pak_formatted(uint8_t out[N64_PAK_SIZE]);

/**
 * Takes one region from an emulator's file (`data`, `len` bytes, the order
 * the emulators store it). A short file reads as 0xFF past its end, as
 * Project64 leaves its files. Marks the region present when the game wrote
 * to it: any byte other than 0x00 and 0xFF, or for a pak an index entry in use.
 */
void n64_take(n64_save *s, int region, const uint8_t *data, size_t len);

/**
 * Takes a neutral member as given: bus order, present. False when its size
 * isn't the region's (EEPROM: 0x200 or 0x800).
 */
bool n64_take_neutral(n64_save *s, int region, const uint8_t *data, size_t len);

/** Takes every region of a libretro .srm; bytes past 0x48800 (a 64DD disk) are ignored. */
void n64_take_blob(n64_save *s, const uint8_t *blob, size_t len);

/**
 * The region as an emulator stores it, at its full size: SRAM and flash
 * word-swapped, EEPROM at 0x800 padded with 0xFF. `out` holds the region's
 * full size.
 */
size_t n64_give(const n64_save *s, int region, uint8_t *out);

/**
 * A libretro .srm holding `s`. Regions `s` lacks keep their bytes from
 * `existing` when it is a .srm whose region the game didn't use either, and
 * are blank otherwise (0xFF, a formatted pak). `out` holds N64_BLOB_SIZE.
 * `n64_save` is large: keep it off the stack.
 */
int n64_give_blob(const n64_save *s, const uint8_t *existing, size_t existing_len, uint8_t out[N64_BLOB_SIZE]);

#endif
