// SPDX-License-Identifier: MPL-2.0
/* Pokemon Red, Blue, Yellow, Gold, Silver and Crystal keep scratch buffers in
 * cart RAM bank 0 that the game overwrites before it reads them back, so the
 * RAM changes on a menu or a sprite with no save. The save's identity reads
 * those ranges as zeros. */
#include "scratch_gb.h"

#define GB_TITLE_OFF      0x134
#define GB_TITLE_LEN      15     /* 0x134-0x142: the title, padded with 0x00, then any manufacturer code */
#define GB_DESTINATION    0x14A
#define GB_OVERSEAS       0x01
#define GB_SCRATCH_RAM    0x8000u

/* rgbfix -t sets the title and -i the manufacturer code, padding the rest
 * with 0x00; -j sets the destination to 0x01, outside Japan. Flags from the
 * Makefiles of pret/pokered at ba9c056, pret/pokeyellow at b452f2a,
 * pret/pokegold at 2bbfba6 and pret/pokecrystal at 9ce4c4e. */
static const struct { char title[GB_TITLE_LEN + 1]; uint32_t feature; } TITLES[] = {
    { "POKEMON RED", SIGIL_FEATURE_GB_SCRATCH_GEN1 },
    { "POKEMON BLUE", SIGIL_FEATURE_GB_SCRATCH_GEN1 },
    { "POKEMON YELLOW", SIGIL_FEATURE_GB_SCRATCH_GEN1 },
    { "POKEMON_GLDAAUE", SIGIL_FEATURE_GB_SCRATCH_GOLD_SILVER },
    { "POKEMON_SLVAAXE", SIGIL_FEATURE_GB_SCRATCH_GOLD_SILVER },
    { "PM_CRYSTAL\0BYTE", SIGIL_FEATURE_GB_SCRATCH_CRYSTAL },
    { "PM_CRYSTAL\0BYTU", SIGIL_FEATURE_GB_SCRATCH_CRYSTAL },
};

typedef enum { SUM8_COMPLEMENT, SUM16_LE } gb_checksum;

typedef struct { uint16_t first, last; } gb_range;

/* File offsets: RAM bank n starts at n * 0x2000.
 *
 * Gen 1: the three sprite buffers (pret/pokered at ba9c056 ram/sram.asm
 * sSpriteBuffer0-2, SPRITEBUFFERSIZE 0x188 in constants/gfx_constants.asm).
 * The checksum is the complement of the 8-bit sum of sGameData through
 * sGameDataEnd, stored at sMainDataCheckSum (engine/menus/save.asm
 * CalcCheckSum); PKHeX at a5311f9 SAV1Offsets.cs puts those at 0x2598 and
 * 0x3523 for the international games.
 *
 * Gold and Silver: sScratch, 0x60 tiles at the start of bank 0, and the
 * window stack at 0xB800 (pret/pokegold at 2bbfba6 ram/sram.asm,
 * layout.link). Crystal: sScratch only; its window stack is in WRAM
 * (pret/pokecrystal at 9ce4c4e ram/sram.asm, layout.link). The checksum is
 * the 16-bit sum of sGameData through sGameDataEnd, low byte first, at
 * sChecksum (pokecrystal engine/menus/save.asm SaveChecksum, Checksum);
 * PKHeX at a5311f9 SAV2Offsets.cs LoadOffsetsInternational and SAV2.cs
 * GetChecksum give 0x2009-0x2D68 and 0x2D69 for Gold and Silver,
 * 0x2009-0x2B82 and 0x2D0D for Crystal. */
static const struct {
    uint32_t    feature;
    gb_checksum checksum;
    uint16_t    data_first, data_last, checksum_at;
    gb_range    scratch[2];
    size_t      scratch_count;
} LAYOUTS[] = {
    { SIGIL_FEATURE_GB_SCRATCH_GEN1, SUM8_COMPLEMENT, 0x2598, 0x3522, 0x3523, { { 0x0000, 0x0497 } }, 1 },
    { SIGIL_FEATURE_GB_SCRATCH_GOLD_SILVER, SUM16_LE, 0x2009, 0x2D68, 0x2D69,
      { { 0x0000, 0x05FF }, { 0x1800, 0x1FFF } }, 2 },
    { SIGIL_FEATURE_GB_SCRATCH_CRYSTAL, SUM16_LE, 0x2009, 0x2B82, 0x2D0D, { { 0x0000, 0x05FF } }, 1 },
};

uint32_t sigil_gb_scratch_feature(const uint8_t *rom) {
    if (rom[GB_DESTINATION] != GB_OVERSEAS) return 0;
    for (size_t i = 0; i < sizeof(TITLES) / sizeof(TITLES[0]); i++) {
        if (memcmp(rom + GB_TITLE_OFF, TITLES[i].title, GB_TITLE_LEN) == 0) return TITLES[i].feature;
    }
    return 0;
}

static bool checksum_valid(size_t layout, const uint8_t *ram) {
    uint32_t sum = 0;
    for (uint32_t i = LAYOUTS[layout].data_first; i <= LAYOUTS[layout].data_last; i++) sum += ram[i];
    uint16_t at = LAYOUTS[layout].checksum_at;
    if (LAYOUTS[layout].checksum == SUM8_COMPLEMENT) return (uint8_t)~sum == ram[at];
    return (uint16_t)sum == (uint16_t)(ram[at] | ram[at + 1] << 8);
}

void sigil_gb_ram_identity(uint32_t features, const uint8_t *ram, size_t len, char out[33]) {
    size_t layout = sizeof(LAYOUTS) / sizeof(LAYOUTS[0]);
    for (size_t i = 0; i < sizeof(LAYOUTS) / sizeof(LAYOUTS[0]); i++) {
        if (features & LAYOUTS[i].feature) layout = i;
    }
    if (layout == sizeof(LAYOUTS) / sizeof(LAYOUTS[0]) || len != GB_SCRATCH_RAM || !checksum_valid(layout, ram)) {
        sigil_md5_of(ram, len, out);
        return;
    }
    static const uint8_t ZEROS[0x100];
    sigil_md5 m;
    sigil_md5_init(&m);
    size_t at = 0;
    for (size_t r = 0; r < LAYOUTS[layout].scratch_count; r++) {
        const gb_range *range = &LAYOUTS[layout].scratch[r];
        sigil_md5_update(&m, ram + at, range->first - at);
        for (size_t z = range->first; z <= range->last; z += sizeof(ZEROS)) {
            size_t n = range->last + 1u - z;
            sigil_md5_update(&m, ZEROS, n < sizeof(ZEROS) ? n : sizeof(ZEROS));
        }
        at = range->last + 1u;
    }
    sigil_md5_update(&m, ram + at, len - at);
    uint8_t digest[16];
    sigil_md5_final(&m, digest);
    sigil_md5_hex(digest, out);
}
