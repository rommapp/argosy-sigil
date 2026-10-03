// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_CARD_SEGACD_H
#define SIGIL_CARD_SEGACD_H

#include "card_saturn.h"

#define SEGACD_BLOCK_SIZE     64u
#define SEGACD_INTERNAL_SIZE  (8u * 1024u)
#define SEGACD_MAX_CART_SIZE  (512u * 1024u)
#define SEGACD_PICODRIVE_SIZE (SEGACD_INTERNAL_SIZE + 64u * 1024u)
#define SEGACD_NAME_LEN       11u
#define SEGACD_PAYLOAD_SIZE   32u

/**
 * sigil's single-save form for Sega CD backup RAM, a .scd unit: a 20-byte
 * header, then the save's blocks exactly as the volume stores them, 64 bytes
 * each, still ECC-encoded when the save is protected. Copying the stored
 * blocks keeps the unit lossless without decoding and re-encoding.
 *
 *   0x00  "SCDU"
 *   0x04  name, 11 bytes as stored
 *   0x0F  protect flag: 0x00 plain, nonzero ECC-encoded (the BIOS writes 0xFF)
 *   0x10  u16 big-endian block count
 *   0x12  u16 zero
 *   0x14  block count x 64 bytes
 */
#define SEGACD_UNIT_HEADER_SIZE 20u

/**
 * A Sega CD backup RAM volume held collapsed in memory: 8 KiB internal or a
 * RAM cart of 16 KiB to 512 KiB, 64-byte blocks. The last block holds the
 * format block, directory entries grow down from the block below it, and
 * save data grows up from block 1 in directory order with no gaps.
 */
typedef struct {
    uint8_t           *data;
    size_t             size;
    sigil_bram_storage storage;
    uint8_t           *other;      /* the rest of a picodrive combined file, written back unchanged */
    size_t             other_size;
    bool               other_first; /* true when `other` precedes the volume in the file */
} sigil_segacd_volume;

/**
 * Reads a Sega CD volume, raw or byte-expanded, recognised by the format
 * block in its last 64 bytes. A picodrive combined file (8 KiB internal BRAM
 * followed by a 64 KiB RAM cart, 0x12000 bytes) loads as its internal
 * volume; the cart part is kept in `other` and written back unchanged, so
 * its saves are not listed or changed through this volume. Load them with
 * sigil_segacd_volume_load_cart.
 */
int sigil_segacd_volume_load(const sigil_io *io, sigil_segacd_volume *vol);

/**
 * Reads the RAM cart part of a picodrive combined file, keeping the internal
 * part in `other`. SIGIL_ERR_UNSUPPORTED_FORMAT for any other file.
 */
int sigil_segacd_volume_load_cart(const sigil_io *io, sigil_segacd_volume *vol);

/** Releases the volume's buffers. */
void sigil_segacd_volume_free(sigil_segacd_volume *vol);

/**
 * Makes `vol` an empty volume of `size` collapsed bytes (8 KiB to 512 KiB, a
 * power of two), written back in the form `storage` describes, as the BIOS
 * formats one: every byte zero except the format block. `vol` receives new
 * buffers; release them with sigil_segacd_volume_free.
 */
int sigil_segacd_volume_format(sigil_segacd_volume *vol, size_t size, const sigil_bram_storage *storage);

/** Writes the volume in the form it was read or formatted in; the caller frees `*out`. */
int sigil_segacd_volume_write(const sigil_segacd_volume *vol, uint8_t **out, size_t *len);

/**
 * Lists the saves on a volume in directory order. `free_blocks` is the free
 * count the BIOS keeps for the saves the directory holds, which some tools
 * leave stale in the format block; when an entry doesn't decode it is the
 * stored count. `free_slots` is how many one-block saves still fit, each
 * needing a directory entry. Entries whose directory block or protected
 * data fails ECC or CRC, or whose blocks leave the data area, are counted in
 * `corrupt_count` and not listed.
 */
int sigil_segacd_list(const sigil_segacd_volume *vol, sigil_card_listing **out);

/**
 * Copies the data of the save starting at `first_block` into a malloc'd
 * buffer the caller frees: 64 bytes per block for a plain save, the 32
 * decoded bytes per block for a protected one.
 */
int sigil_segacd_entry_data(const sigil_segacd_volume *vol, uint32_t first_block,
                            uint8_t **out, size_t *len);

/** Loads and lists a Sega CD backup RAM volume; SIGIL_ERR_UNSUPPORTED_FORMAT when it isn't one. */
int sigil_segacd_card_list_io(const sigil_io *io, sigil_card_listing **out);

/** Bytes a save of `blocks` blocks takes in .scd unit form. */
size_t sigil_segacd_unit_size(uint32_t blocks);

/**
 * Writes the save starting at `first_block` as a .scd unit. `out` holds
 * sigil_segacd_unit_size(blocks) bytes, with `blocks` as the listing reports it.
 */
int sigil_segacd_extract(const sigil_segacd_volume *vol, uint32_t first_block,
                         uint32_t blocks, uint8_t *out);

/**
 * Appends the .scd unit `unit` after the last save and adds its directory
 * entry, rewriting the four copies of the free-block and file counts from
 * the directory.
 * SIGIL_ERR_NO_SPACE when the volume lacks room for the data and the entry;
 * SIGIL_ERR_EXISTS when a save of that name exists;
 * SIGIL_ERR_UNSUPPORTED_FORMAT when `unit` isn't one save, its protected
 * blocks don't decode, or the directory holds entries that don't decode or
 * saves that aren't back to back. On any error the volume is left unchanged.
 */
int sigil_segacd_inject(sigil_segacd_volume *vol, const uint8_t *unit, size_t len);

/** Blocks sigil_segacd_inject takes for `unit` on `vol`, a new directory block included; 0 when
 * `unit` isn't one save or the volume's counts don't read. */
uint32_t sigil_segacd_cost(const sigil_segacd_volume *vol, const uint8_t *unit, size_t len);

/**
 * Removes the save starting at `first_block` the way the BIOS does: later
 * saves and their entries move down to close the gap, the freed data blocks
 * are zeroed and the vacated entry cleared. SIGIL_ERR_INVALID_ARG when no
 * save starts there; SIGIL_ERR_UNSUPPORTED_FORMAT, changing nothing, when the
 * directory holds entries that don't decode or saves that aren't back to back.
 */
int sigil_segacd_delete(sigil_segacd_volume *vol, uint32_t first_block);

/**
 * SIGIL_OK when the volume holds a save named as in `unit` whose .scd form
 * equals `unit` byte for byte; SIGIL_ERR_NOT_FOUND otherwise.
 */
int sigil_segacd_verify(const sigil_segacd_volume *vol, const uint8_t *unit, size_t len);

/**
 * Decodes one ECC-encoded 64-byte block into its 32-byte payload, correcting
 * a single bad symbol per code word as the BIOS does. False when the block
 * holds an error it cannot correct or both CRC copies disagree with the payload.
 */
bool sigil_segacd_decode_block(const uint8_t in[SEGACD_BLOCK_SIZE], uint8_t out[SEGACD_PAYLOAD_SIZE]);

/** Encodes a 32-byte payload into a 64-byte block with its CRCs and ECC. */
void sigil_segacd_encode_block(const uint8_t in[SEGACD_PAYLOAD_SIZE], uint8_t out[SEGACD_BLOCK_SIZE]);

#endif
