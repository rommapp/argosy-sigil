// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

/* Game Boy carts have no title id; saves are named after the content file.
 * The header still decides what the save unit holds: byte 0x147 names the
 * mapper, and four mapper types carry a real-time clock that a libretro core
 * exposes as RETRO_MEMORY_RTC. gambatte, mGBA and VBA-M all key that region on
 * exactly these values (gambatte cartridge_libretro.cpp hasRtc, mGBA
 * GB_MBC3_RTC, VBA-M gbRTCPresent). */

#define GB_HEADER_OFF     0x100
#define GB_HEADER_LEN     0x50
#define GB_CART_TYPE      0x147
#define GB_CHECKSUM       0x14D
#define GB_CHECKSUM_FIRST 0x134
#define GB_CHECKSUM_LAST  0x14C

#define GB_MBC3_TIMER_BATTERY     0x0F
#define GB_MBC3_TIMER_RAM_BATTERY 0x10
#define GB_TAMA5                  0xFD
#define GB_HUC3                   0xFE

static bool gb_cart_has_rtc(uint8_t cart_type) {
    return cart_type == GB_MBC3_TIMER_BATTERY
        || cart_type == GB_MBC3_TIMER_RAM_BATTERY
        || cart_type == GB_TAMA5
        || cart_type == GB_HUC3;
}

/* The boot ROM refuses a cart whose header checksum fails, so a mismatch
 * means these bytes are not a Game Boy header and 0x147 is noise. */
static bool gb_header_checksum_ok(const uint8_t *rom) {
    uint8_t x = 0;
    for (uint32_t i = GB_CHECKSUM_FIRST; i <= GB_CHECKSUM_LAST; i++) {
        x = (uint8_t)(x - rom[i] - 1);
    }
    return x == rom[GB_CHECKSUM];
}

int sigil_extract_gb(const sigil_io *io, const char *filename_hint,
                     const sigil_options *opts, sigil_result *out) {
    (void)opts;

    sigil_result_init(out);
    out->platform = SIGIL_PLATFORM_GB;
    out->usage    = SIGIL_USAGE_FILE_PREFIX;

    if (!io || !io->read) return SIGIL_ERR_INVALID_ARG;

    char ext[16];
    sigil_lower_ext(filename_hint, ext);
    if (strcmp(ext, "gbc") == 0) out->platform = SIGIL_PLATFORM_GBC;

    uint8_t rom[GB_HEADER_OFF + GB_HEADER_LEN];
    if (sigil_io_read_exact(io, 0, rom, sizeof(rom)) != SIGIL_OK) return SIGIL_ERR_IO;
    if (!gb_header_checksum_ok(rom)) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    if (gb_cart_has_rtc(rom[GB_CART_TYPE])) out->features |= SIGIL_FEATURE_RTC;
    out->source = SIGIL_SOURCE_BINARY;
    return SIGIL_OK;
}
