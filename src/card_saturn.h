// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_CARD_SATURN_H
#define SIGIL_CARD_SATURN_H

#include "card.h"

#define SATURN_INTERNAL_SIZE      (32u * 1024u)
#define SATURN_CART_SIZE          (512u * 1024u)
#define SATURN_YABASANSHIRO_SIZE  (4u * 1024u * 1024u)
#define SATURN_NAME_LEN           11u
#define SATURN_COMMENT_LEN        10u
#define SATURN_BUP_HEADER_SIZE    64u

/**
 * How a backup RAM volume was stored on disk, so a changed volume goes back
 * in the form the emulator wrote. Shared by the Saturn and Sega CD readers.
 */
typedef struct {
    bool    gzip;             /* the whole file is one gzip member (standalone Mednafen carts) */
    uint8_t gzip_header[10];  /* that member's header, written back with its flags cleared */
    bool    expanded;         /* each volume byte takes a 16-bit word, in the odd byte (Yabause, Yaba Sanshiro) */
    int     filler;           /* the even byte of each expanded word, or -1 when it repeats the odd byte */
} sigil_bram_storage;

/**
 * Collapses the expanded buffer `buf` of `len` bytes in place into its odd
 * bytes and records the filler in `storage`. False, leaving `buf` untouched,
 * when the even bytes are neither one constant nor copies of the odd bytes,
 * since such a volume could not be written back as it was read.
 */
bool sigil_bram_collapse(uint8_t *buf, size_t len, sigil_bram_storage *storage);

/**
 * Writes the collapsed volume `volume` in the form `storage` describes into
 * a malloc'd buffer the caller frees.
 */
int sigil_bram_store(const uint8_t *volume, size_t len, const sigil_bram_storage *storage,
                     uint8_t **out, size_t *out_len);

/**
 * Reads the whole of `io` into a malloc'd buffer. SIGIL_ERR_UNSUPPORTED_FORMAT
 * when the stream is longer than `cap` bytes.
 */
int sigil_bram_read_all(const sigil_io *io, size_t cap, uint8_t **out, size_t *len);

/**
 * A Saturn backup RAM volume held collapsed in memory: 32 KiB internal and
 * 4 MiB Yaba Sanshiro volumes use 64-byte blocks, carts up to 2 MiB 512-byte
 * blocks and 4 MiB carts 1024-byte blocks. Block 0 holds the "BackUpRam Format" signature and block 1 is
 * unused. Each save starts at an archive block naming it and listing its
 * other blocks; there is no directory.
 */
typedef struct {
    uint8_t           *data;
    size_t             size;
    uint32_t           block_size;
    sigil_bram_storage storage;
} sigil_saturn_volume;

/**
 * Reads a Saturn volume in any stored form: raw, gzipped by standalone
 * Mednafen, or expanded by Yabause and Yaba Sanshiro. SIGIL_ERR_UNSUPPORTED_FORMAT
 * when the stream is not one; the collapsed size must be 32 KiB or 4 MiB (internal
 * memory) or 512 KiB, 1 MiB or 2 MiB (a cart). A 4 MiB cart reads only through
 * sigil_saturn_volume_load_cart.
 */
int sigil_saturn_volume_load(const sigil_io *io, sigil_saturn_volume *vol);

/**
 * Reads a volume known to be a backup cart: 512 KiB, 1 MiB or 2 MiB with
 * 512-byte blocks, or 4 MiB with 1024-byte blocks, as the BIOS sizes them.
 * SIGIL_ERR_UNSUPPORTED_FORMAT for any other size.
 */
int sigil_saturn_volume_load_cart(const sigil_io *io, sigil_saturn_volume *vol);

/** Reads a volume known to be internal memory: 32 KiB, or Yaba Sanshiro's 4 MiB, 64-byte blocks. */
int sigil_saturn_volume_load_internal(const sigil_io *io, sigil_saturn_volume *vol);

/** Releases the volume's buffer. */
void sigil_saturn_volume_free(sigil_saturn_volume *vol);

/**
 * Makes `vol` an empty volume of `size` collapsed bytes (32 KiB, 512 KiB or
 * 4 MiB) that is written back in the form `storage` describes: block 0
 * filled with the signature and every other byte zero, as Mednafen formats one.
 * `vol` receives a new buffer; release it with sigil_saturn_volume_free.
 */
int sigil_saturn_volume_format(sigil_saturn_volume *vol, size_t size, const sigil_bram_storage *storage);

/** As sigil_saturn_volume_format for a backup cart of 512 KiB, 1 MiB, 2 MiB or 4 MiB. */
int sigil_saturn_volume_format_cart(sigil_saturn_volume *vol, size_t size, const sigil_bram_storage *storage);

/** Writes the volume in the form it was read or formatted in; the caller frees `*out`. */
int sigil_saturn_volume_write(const sigil_saturn_volume *vol, uint8_t **out, size_t *len);

/**
 * Lists the saves on a volume in block order. Saves whose block list leaves
 * the volume, repeats a block, claims another save's block or cannot hold the
 * size it records are counted in `corrupt_count` and not listed. With no
 * directory, `free_slots` equals `free_blocks`.
 */
int sigil_saturn_list(const sigil_saturn_volume *vol, sigil_card_listing **out);

/**
 * Copies the data of the save whose archive block is `first_block`, trimmed
 * to the size it records, into a malloc'd buffer the caller frees.
 */
int sigil_saturn_entry_data(const sigil_saturn_volume *vol, uint32_t first_block,
                            uint8_t **out, size_t *len);

/** Loads and lists a Saturn backup RAM volume; SIGIL_ERR_UNSUPPORTED_FORMAT when it isn't one. */
int sigil_saturn_card_list_io(const sigil_io *io, sigil_card_listing **out);

/**
 * Writes the save whose archive block is `first_block` as a .BUP into a
 * malloc'd buffer the caller frees: the 64-byte "Vmem" header, then the data.
 * A volume keeps one date and no save id, statistics or name terminator, so
 * the header's save id and statistics are zero, the name ends in NUL, both
 * date fields hold the entry's date and the block-count field holds the
 * blocks the save uses on this volume.
 */
int sigil_saturn_extract(const sigil_saturn_volume *vol, uint32_t first_block,
                         uint8_t **out, size_t *len);

/**
 * Adds the .BUP `bup` to the volume, taking the lowest free blocks, and sizes
 * it for this volume's block size whatever block count the .BUP records.
 * SIGIL_ERR_NO_SPACE when the volume lacks room; SIGIL_ERR_EXISTS when
 * a save of that name exists; SIGIL_ERR_UNSUPPORTED_FORMAT when `bup` isn't
 * one save. On any error the volume is left unchanged.
 */
int sigil_saturn_inject(sigil_saturn_volume *vol, const uint8_t *bup, size_t len);

/** Zeroes every block of the save whose archive block is `first_block`, freeing them. */
int sigil_saturn_delete(sigil_saturn_volume *vol, uint32_t first_block);

/**
 * SIGIL_OK when the volume holds a save with the name, language, comment,
 * date, size and data of the .BUP `bup`; SIGIL_ERR_NOT_FOUND otherwise. The
 * header fields a volume doesn't keep are not compared.
 */
int sigil_saturn_verify(const sigil_saturn_volume *vol, const uint8_t *bup, size_t len);

/**
 * The MD5 over a .BUP's name, comment, language, size and data. The dates
 * and the block count, which depends on the volume's block size, leave it
 * alone.
 */
void sigil_saturn_bup_md5(const uint8_t *bup, size_t len, char out[33]);

#endif
