// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_CARD_PS1_H
#define SIGIL_CARD_PS1_H

#include "sigil_internal.h"

#define PS1_CARD_SIZE    (128u * 1024u)
#define PS1_BLOCK_SIZE   8192u
#define PS1_FRAME_SIZE   128u
#define PS1_DATA_BLOCKS  15u
#define PS1_NAME_LEN     20u
#define PS1_GME_HEADER_SIZE 0xF40u
#define PS1_VMP_HEADER_SIZE 0x80u

/**
 * Reads a PS1 memory card in any supported wrapper into `image`, a full raw
 * card. A card shorter than its format's size reads as if the missing tail
 * were unused. Sets `*format` to a sigil_card_format value.
 */
int sigil_ps1_card_load(const sigil_io *io, uint8_t image[PS1_CARD_SIZE], int *format);

/**
 * A card as its file holds it: the raw card, the file's format, and for a
 * .gme or .vmp the header in front of the card. `read_directory` is the
 * directory block as read, so a .gme keeps a slot's comment only while the
 * same save holds the slot.
 */
typedef struct {
    uint8_t image[PS1_CARD_SIZE];
    int     format;
    uint8_t header[PS1_GME_HEADER_SIZE];
    uint8_t read_directory[PS1_BLOCK_SIZE];
} sigil_ps1_file;

/** Reads a card in any supported wrapper, keeping the wrapper's header. */
int sigil_ps1_file_load(const sigil_io *io, sigil_ps1_file *f);

/**
 * Makes `f` an empty card in `format`: SIGIL_CARD_FORMAT_PS1_RAW, or
 * SIGIL_CARD_FORMAT_PS1_VMP with the header a PSP writes and the seed
 * 00 01 .. 13 (any seed checks; the Vagrant Story sample carries this one).
 */
void sigil_ps1_file_format(sigil_ps1_file *f, int format);

/**
 * SIGIL_ERR_DAMAGED when a .vmp's signature doesn't match its card, which
 * the PSP and Vita refuse to load; SIGIL_OK otherwise.
 */
int sigil_ps1_file_check(const sigil_ps1_file *f);

/**
 * The file's bytes in its own format: a raw card; a .gme with the header's
 * frame copies following the directory, a comment kept only beside the save
 * it was read with, and the card at full size; a .vmp signed for its card
 * under its own seed. The caller frees `*out`.
 */
int sigil_ps1_file_write(const sigil_ps1_file *f, uint8_t **out, size_t *len);

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

/** Blocks sigil_ps1_inject takes for `mcs`; 0 when it isn't one save. */
uint32_t sigil_ps1_cost(const uint8_t *mcs, size_t len);

/** Marks the save starting at `first_block` deleted, freeing its blocks. */
int sigil_ps1_delete(uint8_t image[PS1_CARD_SIZE], uint32_t first_block);

/**
 * SIGIL_OK when the card holds a live save named as in `mcs` whose .mcs
 * form equals `mcs` byte for byte; SIGIL_ERR_NOT_FOUND otherwise.
 */
int sigil_ps1_verify(const uint8_t image[PS1_CARD_SIZE], const uint8_t *mcs, size_t len);

#endif
