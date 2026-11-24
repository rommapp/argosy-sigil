// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_SCRATCH_GB_H
#define SIGIL_SCRATCH_GB_H

#include "sigil_internal.h"

/** The SIGIL_FEATURE_GB_SCRATCH_* bit a cart's header names, or 0. `rom` holds the header through 0x14F. */
uint32_t sigil_gb_scratch_feature(const uint8_t *rom);

/**
 * The md5 of a cart RAM, with the scratch ranges of the layout `features`
 * names read as zeros. A RAM whose size or main-data checksum doesn't match
 * that layout, or `features` naming none, hashes as it is.
 */
void sigil_gb_ram_identity(uint32_t features, const uint8_t *ram, size_t len, char out[33]);

#endif
