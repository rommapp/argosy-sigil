// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

/* SNES carts have no title id either. The internal header sits at 0x7FC0 for
 * LoROM, 0xFFC0 for HiROM and 0x40FFC0 for ExHiROM, and a copier may have
 * prepended 512 bytes, so the file size settles the skew and the header's own
 * checksum/complement pair settles the base. snes9x builds
 * (ROMType << 8) | ROMSpeed from header bytes 0x16 and 0x15 and enables its
 * clock chips on 0x5535 (S-RTC) and 0xF93A (SPC7110 with RTC), which is the
 * whole of what a save unit needs to know (snes9x memmap.cpp InitROM). */

#define SNES_COPIER_HEADER   512
#define SNES_HEADER_LEN      0x40
#define SNES_ROM_SPEED       0x15
#define SNES_ROM_TYPE        0x16
#define SNES_COMPLEMENT      0x1C
#define SNES_CHECKSUM        0x1E

#define SNES_ID_SRTC         0x5535
#define SNES_ID_SPC7110_RTC  0xF93A

static const uint64_t SNES_HEADER_BASES[] = { 0x40FFC0, 0xFFC0, 0x7FC0 };
static const size_t SNES_HEADER_BASE_COUNT = sizeof(SNES_HEADER_BASES) / sizeof(SNES_HEADER_BASES[0]);

static bool snes_header_valid(const uint8_t *h) {
    uint16_t complement = (uint16_t)(h[SNES_COMPLEMENT] | (h[SNES_COMPLEMENT + 1] << 8));
    uint16_t checksum   = (uint16_t)(h[SNES_CHECKSUM] | (h[SNES_CHECKSUM + 1] << 8));
    return (uint16_t)(complement ^ checksum) == 0xFFFF;
}

/* Larger bases first: an ExHiROM image also carries plausible bytes at the
 * HiROM base, so the deepest header that validates is the real one. */
static int snes_find_header(const sigil_io *io, uint64_t skew, uint8_t out[SNES_HEADER_LEN]) {
    int64_t size = io->size ? io->size(io->ctx) : -1;
    for (size_t i = 0; i < SNES_HEADER_BASE_COUNT; i++) {
        uint64_t off = SNES_HEADER_BASES[i] + skew;
        if (size >= 0 && off + SNES_HEADER_LEN > (uint64_t)size) continue;
        if (sigil_io_read_exact(io, off, out, SNES_HEADER_LEN) != SIGIL_OK) continue;
        if (snes_header_valid(out)) return SIGIL_OK;
    }
    return SIGIL_ERR_UNSUPPORTED_FORMAT;
}

int sigil_extract_snes(const sigil_io *io, const char *filename_hint,
                       const sigil_options *opts, sigil_result *out) {
    (void)filename_hint;
    (void)opts;

    sigil_result_init(out);
    out->platform = SIGIL_PLATFORM_SNES;
    out->usage    = SIGIL_USAGE_FILE_PREFIX;

    if (!io || !io->read) return SIGIL_ERR_INVALID_ARG;

    int64_t size = io->size ? io->size(io->ctx) : -1;
    uint64_t skew = (size > 0 && (size % 1024) == SNES_COPIER_HEADER) ? SNES_COPIER_HEADER : 0;

    uint8_t header[SNES_HEADER_LEN];
    int rc = snes_find_header(io, skew, header);
    if (rc != SIGIL_OK) return rc;

    uint32_t identifier = ((uint32_t)header[SNES_ROM_TYPE] << 8) | header[SNES_ROM_SPEED];
    if (identifier == SNES_ID_SRTC || identifier == SNES_ID_SPC7110_RTC) {
        out->features |= SIGIL_FEATURE_RTC;
    }
    out->source = SIGIL_SOURCE_BINARY;
    return SIGIL_OK;
}
