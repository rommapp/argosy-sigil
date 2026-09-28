// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_CARD_GAMECUBE_H
#define SIGIL_CARD_GAMECUBE_H

#include "card.h"

#define GC_BLOCK_SIZE     0x2000u
#define GC_SYSTEM_BLOCKS  5u
#define GC_DENTRY_SIZE    0x40u
#define GC_DIR_ENTRIES    127u
#define GC_FILENAME_LEN   32u
#define GC_MAX_CARD_SIZE  (2048u * GC_BLOCK_SIZE)

/** Loads and lists a GameCube card image; SIGIL_ERR_UNSUPPORTED_FORMAT when it isn't one. */
int sigil_gamecube_card_list_io(const sigil_io *io, sigil_card_listing **out);

/**
 * Reads a raw GameCube card (.raw, .gcp) into a malloc'd `*image` of `*size`
 * bytes, which the caller frees. The card must be one of the six sizes, carry
 * a valid header, and hold at least one valid directory and one valid block
 * allocation table. SIGIL_ERR_UNSUPPORTED_FORMAT otherwise.
 */
int sigil_gamecube_card_load(const sigil_io *io, uint8_t **image, size_t *size);

/**
 * Lists the live saves on a card image into `out`, in directory order, from
 * the directory and block allocation table the console would use. Saves whose
 * block chain is broken are counted in `corrupt_count` and not listed.
 */
int sigil_gamecube_card_list(const uint8_t *image, size_t size, sigil_card_listing **out);

/**
 * Copies the save starting at data block `first_block` into `out`, one full
 * block per chain link in chain order. `out` must hold `blocks` blocks, as
 * the listing reports them.
 */
int sigil_gamecube_entry_data(const uint8_t *image, size_t size, uint32_t first_block,
                              uint32_t blocks, uint8_t *out);

/** Bytes a save of `blocks` blocks takes in .gci form. */
size_t sigil_gamecube_gci_size(uint32_t blocks);

/**
 * Writes the save starting at `first_block` in .gci form: its directory entry
 * as the card holds it, then its blocks in chain order. This is what Dolphin's
 * export writes, so the entry's first-block field names the block the save
 * starts at on this card. `out` holds sigil_gamecube_gci_size(blocks) bytes.
 */
int sigil_gamecube_extract(const uint8_t *image, size_t size, uint32_t first_block,
                           uint32_t blocks, uint8_t *out);

/**
 * Makes `image` an empty formatted card of `size` bytes, with the serial and
 * format time zeroed so every format of one size is identical. `shift_jis`
 * marks a Japanese card. SIGIL_ERR_INVALID_ARG when `size` is not a card size.
 */
int sigil_gamecube_format(uint8_t *image, size_t size, bool shift_jis);

/**
 * Adds the .gci save `gci` to the card, taking the first free directory slot
 * and the lowest free blocks, and writes the new directory and allocation
 * table to both copies. The save's bytes go on the card unchanged apart from
 * its first-block field. SIGIL_ERR_NOT_FOUND when the card lacks room;
 * SIGIL_ERR_INVALID_ARG when a save with the same game code, maker code and
 * file name is already there; SIGIL_ERR_UNSUPPORTED_FORMAT when `gci` isn't a
 * single .gci save. In every failure the card is left unchanged.
 */
int sigil_gamecube_inject(uint8_t *image, size_t size, const uint8_t *gci, size_t len);

/** Removes the save starting at `first_block`, freeing and erasing its blocks. */
int sigil_gamecube_delete(uint8_t *image, size_t size, uint32_t first_block);

/**
 * SIGIL_OK when the card holds a live save with the identity of `gci` whose
 * .gci form equals `gci` byte for byte, first-block field aside;
 * SIGIL_ERR_NOT_FOUND otherwise.
 */
int sigil_gamecube_verify(const uint8_t *image, size_t size, const uint8_t *gci, size_t len);

/**
 * Converts a single save in .gci, GameShark .gcs or MaxDrive .sav form to
 * .gci in `out`, which holds at least `len` bytes, and sets `*out_len`. A .gcs
 * takes its block count from its length, since GameShark doesn't always store
 * it. SIGIL_ERR_UNSUPPORTED_FORMAT when `save` is none of these.
 */
int sigil_gamecube_to_gci(const uint8_t *save, size_t len, uint8_t *out, size_t *out_len);

/**
 * Rewrites the card serial that F-Zero GX stores inside its f_zero.dat so the
 * save starting at `first_block` accepts this card, and reseals the save's
 * checksum. Other saves are left unchanged.
 */
int sigil_gamecube_bind_serial(uint8_t *image, size_t size, uint32_t first_block);

/**
 * The card's additive checksum over `len` bytes of big-endian 16-bit words,
 * and the same over their complements.
 */
void sigil_gamecube_checksum(const uint8_t *data, size_t len, uint16_t *sum, uint16_t *inverse);

#endif
