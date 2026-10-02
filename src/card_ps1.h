// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_CARD_PS1_H
#define SIGIL_CARD_PS1_H

#include "sigil_internal.h"

#define PS1_CARD_SIZE    (128u * 1024u)
#define PS1_BLOCK_SIZE   8192u
#define PS1_FRAME_SIZE   128u
#define PS1_DATA_BLOCKS  15u
#define PS1_NAME_LEN     20u

/**
 * Reads a PS1 memory card in any supported wrapper into `image`, a full raw
 * card. A card shorter than its format's size reads as if the missing tail
 * were unused. Sets `*format` to a sigil_card_format value.
 */
int sigil_ps1_card_load(const sigil_io *io, uint8_t image[PS1_CARD_SIZE], int *format);

/**
 * Lists the live saves on a raw card image into `out`, in directory order.
 * Saves whose block chain is broken are counted in `corrupt_count` and not
 * listed.
 */
int sigil_ps1_card_list(const uint8_t image[PS1_CARD_SIZE], int format, sigil_card_listing **out);

/**
 * Copies the save starting at data block `first_block` (1-15) into `out`,
 * one full block per chain link in chain order. `out` must hold `blocks`
 * blocks, as the listing reports them.
 */
int sigil_ps1_entry_data(const uint8_t image[PS1_CARD_SIZE], uint32_t first_block,
                         uint32_t blocks, uint8_t *out);

/** Loads and lists a PS1 card; SIGIL_ERR_UNSUPPORTED_FORMAT when it isn't one. */
int sigil_ps1_card_list_io(const sigil_io *io, sigil_card_listing **out);

/** Bytes a save of `blocks` blocks takes in .mcs form. */
size_t sigil_ps1_mcs_size(uint32_t blocks);

/**
 * Writes the save starting at `first_block` in .mcs form: its directory
 * frame with the link cleared, then its blocks in chain order. `out` holds
 * sigil_ps1_mcs_size(blocks) bytes.
 */
int sigil_ps1_extract(const uint8_t image[PS1_CARD_SIZE], uint32_t first_block,
                      uint32_t blocks, uint8_t *out);

/** Makes `image` an empty formatted card, as PS1 emulators write one. */
void sigil_ps1_format(uint8_t image[PS1_CARD_SIZE]);

/**
 * Adds the .mcs save `mcs` to the card, taking the lowest free blocks.
 * SIGIL_ERR_NO_SPACE when the card lacks room; SIGIL_ERR_EXISTS when a save
 * with the same name is there; SIGIL_ERR_UNSUPPORTED_FORMAT when `mcs` isn't
 * a single save. On any error the card is left unchanged.
 */
int sigil_ps1_inject(uint8_t image[PS1_CARD_SIZE], const uint8_t *mcs, size_t len);

/** Marks the save starting at `first_block` deleted, freeing its blocks. */
int sigil_ps1_delete(uint8_t image[PS1_CARD_SIZE], uint32_t first_block);

/**
 * SIGIL_OK when the card holds a live save named as in `mcs` whose .mcs
 * form equals `mcs` byte for byte; SIGIL_ERR_NOT_FOUND otherwise.
 */
int sigil_ps1_verify(const uint8_t image[PS1_CARD_SIZE], const uint8_t *mcs, size_t len);

#endif
