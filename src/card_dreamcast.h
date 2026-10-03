// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_CARD_DREAMCAST_H
#define SIGIL_CARD_DREAMCAST_H

#include "card.h"

#define VMU_CARD_SIZE      (128u * 1024u)
#define VMU_BLOCK_SIZE     512u
#define VMU_BLOCKS         256u
#define VMU_DIR_ENTRY_SIZE 32u
#define VMU_NAME_LEN       12u

/**
 * Reads a 128 KiB VMU flash image into `image`. The image has no magic, so
 * it is recognised by its size and by a root block (block 255) carrying the
 * 0x55 format fill and a FAT, directory and user area that fit the card.
 * SIGIL_ERR_UNSUPPORTED_FORMAT for anything else.
 */
int sigil_dreamcast_card_load(const sigil_io *io, uint8_t image[VMU_CARD_SIZE]);

/**
 * Lists the files on a VMU image into `out`, in directory order. Entry names
 * are the 12 stored bytes. Files whose FAT chain leaves the user area, loops,
 * or disagrees with the size in their directory entry are counted in
 * `corrupt_count` and not listed.
 */
int sigil_dreamcast_card_list(const uint8_t image[VMU_CARD_SIZE], sigil_card_listing **out);

/**
 * Copies the file starting at `first_block` into `out`, one whole block per
 * FAT chain link in chain order. `out` must hold `blocks` blocks, as the
 * listing reports them.
 */
int sigil_dreamcast_entry_data(const uint8_t image[VMU_CARD_SIZE], uint32_t first_block,
                               uint32_t blocks, uint8_t *out);

/** Loads and lists a VMU image; SIGIL_ERR_UNSUPPORTED_FORMAT when it isn't one. */
int sigil_dreamcast_card_list_io(const sigil_io *io, sigil_card_listing **out);

/** Bytes a file of `blocks` blocks takes in .dci form. */
size_t sigil_dreamcast_dci_size(uint32_t blocks);

/**
 * Writes the file starting at `first_block` in .dci form: its 32-byte
 * directory entry with the first-block field cleared, then its blocks in
 * chain order with every 4-byte group byte-reversed. `out` holds
 * sigil_dreamcast_dci_size(blocks) bytes.
 */
int sigil_dreamcast_extract(const uint8_t image[VMU_CARD_SIZE], uint32_t first_block,
                            uint32_t blocks, uint8_t *out);

/**
 * Makes `image` an empty formatted VMU: 200 user blocks, the FAT in block
 * 254, a 13-block directory from block 253 down, and fixed card-level fields.
 */
void sigil_dreamcast_format(uint8_t image[VMU_CARD_SIZE]);

/**
 * Makes `image` an empty VMU laid out as `like` (its root block, user block
 * count, FAT and directory placement), or a stock one when `like` doesn't
 * read as a VMU.
 */
void sigil_dreamcast_format_like(uint8_t image[VMU_CARD_SIZE], const uint8_t like[VMU_CARD_SIZE]);

/**
 * Adds the .dci file `dci` to the VMU. A game file takes the contiguous
 * blocks from block 0; a data file takes free blocks from the top of the user
 * area down. SIGIL_ERR_NO_SPACE when the VMU lacks the blocks or a directory
 * slot, or already holds a game file and `dci` is another;
 * SIGIL_ERR_EXISTS when a file with the same name exists;
 * SIGIL_ERR_UNSUPPORTED_FORMAT when `dci` isn't a single file. On any error
 * the VMU is left unchanged.
 */
int sigil_dreamcast_inject(uint8_t image[VMU_CARD_SIZE], const uint8_t *dci, size_t len);

/** Blocks sigil_dreamcast_inject takes for `dci`; 0 when it isn't one save. */
uint32_t sigil_dreamcast_cost(const uint8_t *dci, size_t len);

/** Removes the file starting at `first_block`, freeing its blocks and directory slot. */
int sigil_dreamcast_delete(uint8_t image[VMU_CARD_SIZE], uint32_t first_block);

/**
 * SIGIL_OK when the VMU holds a file named as in `dci` whose .dci form equals
 * `dci` byte for byte; SIGIL_ERR_NOT_FOUND otherwise.
 */
int sigil_dreamcast_verify(const uint8_t image[VMU_CARD_SIZE], const uint8_t *dci, size_t len);

/**
 * Converts a .vms file and its 108-byte .vmi into .dci form in `out`, which
 * holds sigil_dreamcast_dci_size(vms_len / VMU_BLOCK_SIZE) bytes. The VMI
 * supplies the directory entry: name, file mode, timestamp and size.
 * SIGIL_ERR_UNSUPPORTED_FORMAT when the VMI checksum, its size field or the
 * VMS length don't hold.
 */
int sigil_dreamcast_dci_from_vms(const uint8_t *vms, size_t vms_len,
                                 const uint8_t *vmi, size_t vmi_len, uint8_t *out);

#endif
