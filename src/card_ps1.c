// SPDX-License-Identifier: MPL-2.0
#include "card_ps1.h"
#include "card.h"
#include "aes.h"
#include <stdlib.h>

#define PS1_STATE_FIRST        0x51u
#define PS1_STATE_MIDDLE       0x52u
#define PS1_STATE_LAST         0x53u
#define PS1_STATE_FREE         0xA0u
#define PS1_STATE_DELETED_LAST 0xA3u
#define PS1_LINK_END           0xFFFFu

#define VMP_SEED       0x0Cu
#define VMP_SIGNATURE  0x20u
#define VMP_SIGN_LEN   20u
#define GME_STATES     0x16u
#define GME_LINKS      0x26u
#define GME_COMMENTS   0x40u
#define GME_COMMENT    0x100u

static const uint8_t *frame_at(const uint8_t *image, uint32_t block) {
    return image + block * PS1_FRAME_SIZE;
}

static uint32_t frame_state(const uint8_t *image, uint32_t block) {
    return sigil_read_le32(frame_at(image, block));
}

static uint16_t frame_link(const uint8_t *image, uint32_t block) {
    return sigil_read_le16(frame_at(image, block) + 8);
}

/* Walks the chain from `first` and returns its length in blocks, or 0 when a
 * link leaves the card, revisits a block, or lands on a block that isn't part
 * of a live save. `order`, when given, receives the blocks in chain order. */
static uint32_t walk_chain(const uint8_t *image, uint32_t first, uint32_t order[PS1_DATA_BLOCKS]) {
    uint16_t seen = 0;
    uint32_t block = first;
    uint32_t count = 0;
    for (;;) {
        seen |= (uint16_t)(1u << block);
        if (order) order[count] = block;
        count++;
        uint16_t link = frame_link(image, block);
        if (link == PS1_LINK_END) return count;
        if (link >= PS1_DATA_BLOCKS) return 0;
        uint32_t next = (uint32_t)link + 1;
        if (seen & (1u << next)) return 0;
        uint32_t state = frame_state(image, next);
        if (state != PS1_STATE_MIDDLE && state != PS1_STATE_LAST) return 0;
        block = next;
    }
}

/* Reads the card behind whatever wrapper the file has; `header`, when given,
 * receives the wrapper's header. */
static int load_card(const sigil_io *io, uint8_t image[PS1_CARD_SIZE], int *format, uint8_t *header) {
    if (!io || !io->read || !image || !format) return SIGIL_ERR_INVALID_ARG;

    uint8_t magic[12];
    size_t got = 0;
    int rc = sigil_io_read_upto(io, 0, magic, sizeof(magic), &got);
    if (rc != SIGIL_OK) return rc;

    uint64_t offset;
    if (got >= 4 && memcmp(magic, "\0PMV", 4) == 0) {
        offset = PS1_VMP_HEADER_SIZE;
        *format = SIGIL_CARD_FORMAT_PS1_VMP;
    } else if (got >= 11 && memcmp(magic, "123-456-STD", 11) == 0) {
        offset = PS1_GME_HEADER_SIZE;
        *format = SIGIL_CARD_FORMAT_PS1_GME;
    } else if (got >= 2 && memcmp(magic, "MC", 2) == 0) {
        int64_t size = io->size ? io->size(io->ctx) : -1;
        if (size > (int64_t)PS1_CARD_SIZE) return SIGIL_ERR_UNSUPPORTED_FORMAT;
        offset = 0;
        *format = SIGIL_CARD_FORMAT_PS1_RAW;
    } else {
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
    if (header && offset) {
        rc = sigil_io_read_upto(io, 0, header, (size_t)offset, &got);
        if (rc != SIGIL_OK) return rc;
        if (got != offset) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }

    memset(image, 0, PS1_CARD_SIZE);
    rc = sigil_io_read_upto(io, offset, image, PS1_CARD_SIZE, &got);
    if (rc != SIGIL_OK) return rc;
    if (got < PS1_BLOCK_SIZE || memcmp(image, "MC", 2) != 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    return SIGIL_OK;
}

int sigil_ps1_card_load(const sigil_io *io, uint8_t image[PS1_CARD_SIZE], int *format) {
    return load_card(io, image, format, NULL);
}

int sigil_ps1_file_load(const sigil_io *io, sigil_ps1_file *f) {
    if (!f) return SIGIL_ERR_INVALID_ARG;
    memset(f->header, 0, sizeof(f->header));
    int rc = load_card(io, f->image, &f->format, f->header);
    if (rc == SIGIL_OK) memcpy(f->read_directory, f->image, PS1_BLOCK_SIZE);
    return rc;
}

void sigil_ps1_file_format(sigil_ps1_file *f, int format) {
    sigil_ps1_format(f->image);
    f->format = format == SIGIL_CARD_FORMAT_PS1_VMP ? format : SIGIL_CARD_FORMAT_PS1_RAW;
    memset(f->header, 0, sizeof(f->header));
    if (f->format == SIGIL_CARD_FORMAT_PS1_VMP) {
        memcpy(f->header, "\0PMV", 4);
        sigil_write_le32(f->header + 4, PS1_VMP_HEADER_SIZE);
        for (uint8_t i = 0; i < VMP_SIGN_LEN; i++) f->header[VMP_SEED + i] = i;
    }
    memcpy(f->read_directory, f->image, PS1_BLOCK_SIZE);
}

/* The PSP's save-data key and the constant its HMAC key is masked with, as
 * the PSP and Vita sign a .vmp (and a .psv). */
static const uint8_t VMP_KEY[16] = {
    0xAB, 0x5A, 0xBC, 0x9F, 0xC1, 0xF4, 0x9D, 0xE6, 0xA0, 0x51, 0xDB, 0xAE, 0xFA, 0x51, 0x88, 0x59,
};
static const uint8_t VMP_MASK[16] = {
    0xB3, 0x0F, 0xFE, 0xED, 0xB7, 0xDC, 0x5E, 0xB7, 0x13, 0x3D, 0xA6, 0x0D, 0x1B, 0x6B, 0x2C, 0xDC,
};

/* The HMAC key a .vmp's 20-byte seed gives: the seed's first 16 bytes
 * decrypted under the save-data key and masked, then four bytes of them
 * encrypted, masked by the seed's last four. */
static void vmp_hmac_key(const uint8_t seed[VMP_SIGN_LEN], uint8_t key[VMP_SIGN_LEN]) {
    struct AES_ctx ctx;
    AES_init_ctx(&ctx, VMP_KEY);
    uint8_t dec[16], enc[16];
    memcpy(dec, seed, 16);
    memcpy(enc, seed, 16);
    AES_ECB_decrypt(&ctx, dec);
    AES_ECB_encrypt(&ctx, enc);
    for (int i = 0; i < 16; i++) key[i] = dec[i] ^ VMP_MASK[i];
    for (int i = 0; i < 4; i++) key[16 + i] = enc[i] ^ seed[16 + i];
}

/* The whole file with its signature zeroed, signed under its own seed. */
static int vmp_sign(const sigil_ps1_file *f, uint8_t mac[VMP_SIGN_LEN]) {
    size_t len = PS1_VMP_HEADER_SIZE + PS1_CARD_SIZE;
    uint8_t *file = (uint8_t *)malloc(len);
    if (!file) return SIGIL_ERR_OOM;
    memcpy(file, f->header, PS1_VMP_HEADER_SIZE);
    memset(file + VMP_SIGNATURE, 0, VMP_SIGN_LEN);
    memcpy(file + PS1_VMP_HEADER_SIZE, f->image, PS1_CARD_SIZE);
    uint8_t key[VMP_SIGN_LEN];
    vmp_hmac_key(f->header + VMP_SEED, key);
    sigil_hmac_sha1(key, sizeof(key), file, len, mac);
    free(file);
    return SIGIL_OK;
}

int sigil_ps1_file_check(const sigil_ps1_file *f) {
    if (!f) return SIGIL_ERR_INVALID_ARG;
    if (f->format != SIGIL_CARD_FORMAT_PS1_VMP) return SIGIL_OK;
    uint8_t mac[VMP_SIGN_LEN];
    int rc = vmp_sign(f, mac);
    if (rc != SIGIL_OK) return rc;
    return memcmp(mac, f->header + VMP_SIGNATURE, VMP_SIGN_LEN) == 0 ? SIGIL_OK : SIGIL_ERR_DAMAGED;
}

/* DexDrive's header copies each slot's state and link low byte, a deleted
 * frame as free (0xA0) with no link (0xFF), and keeps a 256-byte comment per
 * slot, which belongs to the save that held the slot when it was written. */
static void gme_refresh(const sigil_ps1_file *f, uint8_t *header) {
    for (uint32_t s = 0; s < PS1_DATA_BLOCKS; s++) {
        const uint8_t *now = frame_at(f->image, s + 1);
        const uint8_t *then = frame_at(f->read_directory, s + 1);
        bool deleted = now[0] > PS1_STATE_FREE && now[0] <= PS1_STATE_DELETED_LAST;
        header[GME_STATES + s] = deleted ? (uint8_t)PS1_STATE_FREE : now[0];
        header[GME_LINKS + s] = deleted ? 0xFF : now[8];
        bool same_save = now[0] == then[0] && memcmp(now + 0x0A, then + 0x0A, PS1_NAME_LEN) == 0;
        if (!same_save) memset(header + GME_COMMENTS + GME_COMMENT * s, 0, GME_COMMENT);
    }
}

int sigil_ps1_file_write(const sigil_ps1_file *f, uint8_t **out, size_t *len) {
    if (!f || !out || !len) return SIGIL_ERR_INVALID_ARG;
    size_t header = f->format == SIGIL_CARD_FORMAT_PS1_GME ? PS1_GME_HEADER_SIZE
                  : f->format == SIGIL_CARD_FORMAT_PS1_VMP ? PS1_VMP_HEADER_SIZE
                                                           : 0;
    uint8_t *file = (uint8_t *)malloc(header + PS1_CARD_SIZE);
    if (!file) return SIGIL_ERR_OOM;
    memcpy(file, f->header, header);
    memcpy(file + header, f->image, PS1_CARD_SIZE);
    if (f->format == SIGIL_CARD_FORMAT_PS1_GME) gme_refresh(f, file);
    if (f->format == SIGIL_CARD_FORMAT_PS1_VMP) {
        int rc = vmp_sign(f, file + VMP_SIGNATURE);
        if (rc != SIGIL_OK) { free(file); return rc; }
    }
    *out = file;
    *len = header + PS1_CARD_SIZE;
    return SIGIL_OK;
}

int sigil_ps1_card_list(const uint8_t image[PS1_CARD_SIZE], int format, sigil_card_listing **out) {
    if (!image || !out) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;

    sigil_card_listing *listing = sigil_card_listing_new(format, PS1_DATA_BLOCKS);
    if (!listing) return SIGIL_ERR_OOM;
    sigil_card_entry *entries = listing->entries;
    listing->total_blocks = PS1_DATA_BLOCKS;

    for (uint32_t block = 1; block <= PS1_DATA_BLOCKS; block++) {
        uint32_t state = frame_state(image, block);
        if (state >= PS1_STATE_FREE && state <= PS1_STATE_DELETED_LAST) {
            listing->free_slots++;
            continue;
        }
        if (state != PS1_STATE_FIRST) continue;

        char name[PS1_NAME_LEN + 1];
        memcpy(name, frame_at(image, block) + 0x0A, PS1_NAME_LEN);
        name[PS1_NAME_LEN] = '\0';
        char owner[SIGIL_CARD_OWNER_MAX];
        sigil_card_sony_owner(name, owner);
        uint32_t blocks = walk_chain(image, block, NULL);
        if (blocks == 0) {
            sigil_card_listing_corrupt(listing, name, owner, block);
            continue;
        }
        sigil_card_entry *e = &entries[listing->entry_count++];
        memcpy(e->name, name, sizeof(name));
        memcpy(e->owner_id, owner, sizeof(owner));
        e->blocks = blocks;
        e->first_block = block;
    }
    listing->free_blocks = listing->free_slots;
    *out = listing;
    return SIGIL_OK;
}

int sigil_ps1_entry_data(const uint8_t image[PS1_CARD_SIZE], uint32_t first_block,
                         uint32_t blocks, uint8_t *out) {
    if (!image || !out || first_block < 1 || first_block > PS1_DATA_BLOCKS) return SIGIL_ERR_INVALID_ARG;
    uint32_t order[PS1_DATA_BLOCKS];
    uint32_t count = walk_chain(image, first_block, order);
    if (count == 0 || count != blocks) return SIGIL_ERR_INVALID_ARG;
    for (uint32_t i = 0; i < count; i++) {
        memcpy(out + (size_t)i * PS1_BLOCK_SIZE, image + (size_t)order[i] * PS1_BLOCK_SIZE, PS1_BLOCK_SIZE);
    }
    return SIGIL_OK;
}

#define PS1_STATE_DELETED_FIRST  0xA1u
#define PS1_STATE_DELETED_MIDDLE 0xA2u
#define PS1_FRAME_CHECKSUM       (PS1_FRAME_SIZE - 1)
#define PS1_BROKEN_FIRST_FRAME   16u
#define PS1_BROKEN_FRAMES        20u
#define PS1_WRITE_TEST_FRAME     63u

static uint8_t *frame_mut(uint8_t *image, uint32_t frame) {
    return image + frame * PS1_FRAME_SIZE;
}

static void seal_frame(uint8_t *f) {
    uint8_t x = 0;
    for (size_t i = 0; i < PS1_FRAME_CHECKSUM; i++) x ^= f[i];
    f[PS1_FRAME_CHECKSUM] = x;
}

static void write_frame(uint8_t *f, uint32_t state, uint32_t size, uint16_t link) {
    sigil_write_le32(f, state);
    sigil_write_le32(f + 4, size);
    sigil_write_le16(f + 8, link);
}

void sigil_ps1_format(uint8_t image[PS1_CARD_SIZE]) {
    memset(image, 0, PS1_CARD_SIZE);
    uint8_t *header = frame_mut(image, 0);
    header[0] = 'M';
    header[1] = 'C';
    seal_frame(header);
    for (uint32_t block = 1; block <= PS1_DATA_BLOCKS; block++) {
        uint8_t *f = frame_mut(image, block);
        write_frame(f, PS1_STATE_FREE, 0, PS1_LINK_END);
        seal_frame(f);
    }
    for (uint32_t i = 0; i < PS1_BROKEN_FRAMES; i++) {
        uint8_t *f = frame_mut(image, PS1_BROKEN_FIRST_FRAME + i);
        write_frame(f, 0xFFFFFFFFu, 0, PS1_LINK_END);
        seal_frame(f);
    }
    memcpy(frame_mut(image, PS1_WRITE_TEST_FRAME), header, PS1_FRAME_SIZE);
}

size_t sigil_ps1_mcs_size(uint32_t blocks) {
    return PS1_FRAME_SIZE + (size_t)blocks * PS1_BLOCK_SIZE;
}

int sigil_ps1_extract(const uint8_t image[PS1_CARD_SIZE], uint32_t first_block,
                      uint32_t blocks, uint8_t *out) {
    if (!image || !out || first_block < 1 || first_block > PS1_DATA_BLOCKS) return SIGIL_ERR_INVALID_ARG;
    if (frame_state(image, first_block) != PS1_STATE_FIRST) return SIGIL_ERR_INVALID_ARG;
    int rc = sigil_ps1_entry_data(image, first_block, blocks, out + PS1_FRAME_SIZE);
    if (rc != SIGIL_OK) return rc;
    memcpy(out, frame_at(image, first_block), PS1_FRAME_SIZE);
    out[8] = out[9] = 0xFF;
    seal_frame(out);
    return SIGIL_OK;
}

static uint32_t mcs_blocks(const uint8_t *mcs, size_t len) {
    if (!mcs || len < sigil_ps1_mcs_size(1)) return 0;
    if ((len - PS1_FRAME_SIZE) % PS1_BLOCK_SIZE != 0) return 0;
    size_t blocks = (len - PS1_FRAME_SIZE) / PS1_BLOCK_SIZE;
    if (blocks > PS1_DATA_BLOCKS || sigil_read_le32(mcs) != PS1_STATE_FIRST) return 0;
    return (uint32_t)blocks;
}

static bool frame_is_free(const uint8_t *image, uint32_t block) {
    uint32_t state = frame_state(image, block);
    return state >= PS1_STATE_FREE && state <= PS1_STATE_DELETED_LAST;
}

uint32_t sigil_ps1_cost(const uint8_t *mcs, size_t len) {
    return mcs_blocks(mcs, len);
}

int sigil_ps1_inject(uint8_t image[PS1_CARD_SIZE], const uint8_t *mcs, size_t len) {
    if (!image) return SIGIL_ERR_INVALID_ARG;
    uint32_t blocks = mcs_blocks(mcs, len);
    if (blocks == 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    for (uint32_t block = 1; block <= PS1_DATA_BLOCKS; block++) {
        if (frame_state(image, block) == PS1_STATE_FIRST &&
            memcmp(frame_at(image, block) + 0x0A, mcs + 0x0A, PS1_NAME_LEN) == 0) {
            return SIGIL_ERR_EXISTS;
        }
    }

    uint32_t chosen[PS1_DATA_BLOCKS];
    uint32_t found = 0;
    for (uint32_t block = 1; block <= PS1_DATA_BLOCKS && found < blocks; block++) {
        if (frame_is_free(image, block)) chosen[found++] = block;
    }
    if (found < blocks) return SIGIL_ERR_NO_SPACE;

    for (uint32_t i = 0; i < blocks; i++) {
        uint8_t *f = frame_mut(image, chosen[i]);
        bool last = i + 1 == blocks;
        uint16_t link = last ? (uint16_t)PS1_LINK_END : (uint16_t)(chosen[i + 1] - 1);
        if (i == 0) {
            memcpy(f, mcs, PS1_FRAME_SIZE);
            write_frame(f, PS1_STATE_FIRST, blocks * PS1_BLOCK_SIZE, link);
        } else {
            memset(f, 0, PS1_FRAME_SIZE);
            write_frame(f, last ? PS1_STATE_LAST : PS1_STATE_MIDDLE, 0, link);
        }
        seal_frame(f);
        memcpy(image + (size_t)chosen[i] * PS1_BLOCK_SIZE,
               mcs + PS1_FRAME_SIZE + (size_t)i * PS1_BLOCK_SIZE, PS1_BLOCK_SIZE);
    }
    return SIGIL_OK;
}

int sigil_ps1_delete(uint8_t image[PS1_CARD_SIZE], uint32_t first_block) {
    if (!image || first_block < 1 || first_block > PS1_DATA_BLOCKS) return SIGIL_ERR_INVALID_ARG;
    if (frame_state(image, first_block) != PS1_STATE_FIRST) return SIGIL_ERR_INVALID_ARG;
    uint32_t order[PS1_DATA_BLOCKS];
    uint32_t count = walk_chain(image, first_block, order);
    if (count == 0) return SIGIL_ERR_INVALID_ARG;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *f = frame_mut(image, order[i]);
        uint32_t state = i == 0 ? PS1_STATE_DELETED_FIRST
                       : i + 1 == count ? PS1_STATE_DELETED_LAST : PS1_STATE_DELETED_MIDDLE;
        sigil_write_le32(f, state);
        seal_frame(f);
    }
    return SIGIL_OK;
}

int sigil_ps1_verify(const uint8_t image[PS1_CARD_SIZE], const uint8_t *mcs, size_t len) {
    if (!image) return SIGIL_ERR_INVALID_ARG;
    uint32_t blocks = mcs_blocks(mcs, len);
    if (blocks == 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return SIGIL_ERR_OOM;
    int rc = SIGIL_ERR_NOT_FOUND;
    for (uint32_t block = 1; block <= PS1_DATA_BLOCKS && rc != SIGIL_OK; block++) {
        if (frame_state(image, block) != PS1_STATE_FIRST) continue;
        if (memcmp(frame_at(image, block) + 0x0A, mcs + 0x0A, PS1_NAME_LEN) != 0) continue;
        if (walk_chain(image, block, NULL) != blocks) continue;
        if (sigil_ps1_extract(image, block, blocks, copy) == SIGIL_OK && memcmp(copy, mcs, len) == 0) {
            rc = SIGIL_OK;
        }
    }
    free(copy);
    return rc;
}

int sigil_ps1_card_list_io(const sigil_io *io, sigil_card_listing **out) {
    uint8_t *image = (uint8_t *)malloc(PS1_CARD_SIZE);
    if (!image) return SIGIL_ERR_OOM;
    int format = SIGIL_CARD_FORMAT_UNKNOWN;
    int rc = sigil_ps1_card_load(io, image, &format);
    if (rc == SIGIL_OK) rc = sigil_ps1_card_list(image, format, out);
    free(image);
    return rc;
}
