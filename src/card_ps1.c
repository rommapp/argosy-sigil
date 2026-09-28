// SPDX-License-Identifier: MPL-2.0
#include "card_ps1.h"
#include "card.h"
#include <stdlib.h>

#define PS1_STATE_FIRST        0x51u
#define PS1_STATE_MIDDLE       0x52u
#define PS1_STATE_LAST         0x53u
#define PS1_STATE_FREE         0xA0u
#define PS1_STATE_DELETED_LAST 0xA3u
#define PS1_LINK_END           0xFFFFu

#define GME_HEADER_SIZE 0xF40u
#define VMP_HEADER_SIZE 0x80u

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

int sigil_ps1_card_load(const sigil_io *io, uint8_t image[PS1_CARD_SIZE], int *format) {
    if (!io || !io->read || !image || !format) return SIGIL_ERR_INVALID_ARG;

    uint8_t magic[12];
    size_t got = 0;
    int rc = sigil_io_read_upto(io, 0, magic, sizeof(magic), &got);
    if (rc != SIGIL_OK) return rc;

    uint64_t offset;
    if (got >= 4 && memcmp(magic, "\0PMV", 4) == 0) {
        offset = VMP_HEADER_SIZE;
        *format = SIGIL_CARD_FORMAT_PS1_VMP;
    } else if (got >= 11 && memcmp(magic, "123-456-STD", 11) == 0) {
        offset = GME_HEADER_SIZE;
        *format = SIGIL_CARD_FORMAT_PS1_GME;
    } else if (got >= 2 && memcmp(magic, "MC", 2) == 0) {
        int64_t size = io->size ? io->size(io->ctx) : -1;
        if (size > (int64_t)PS1_CARD_SIZE) return SIGIL_ERR_UNSUPPORTED_FORMAT;
        offset = 0;
        *format = SIGIL_CARD_FORMAT_PS1_RAW;
    } else {
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }

    memset(image, 0, PS1_CARD_SIZE);
    rc = sigil_io_read_upto(io, offset, image, PS1_CARD_SIZE, &got);
    if (rc != SIGIL_OK) return rc;
    if (got < PS1_BLOCK_SIZE || memcmp(image, "MC", 2) != 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
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

        uint32_t blocks = walk_chain(image, block, NULL);
        if (blocks == 0) {
            listing->corrupt_count++;
            continue;
        }
        sigil_card_entry *e = &entries[listing->entry_count++];
        memcpy(e->name, frame_at(image, block) + 0x0A, PS1_NAME_LEN);
        e->name[PS1_NAME_LEN] = '\0';
        sigil_card_sony_owner(e->name, e->owner_id);
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

int sigil_ps1_inject(uint8_t image[PS1_CARD_SIZE], const uint8_t *mcs, size_t len) {
    if (!image) return SIGIL_ERR_INVALID_ARG;
    uint32_t blocks = mcs_blocks(mcs, len);
    if (blocks == 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    uint32_t chosen[PS1_DATA_BLOCKS];
    uint32_t found = 0;
    for (uint32_t block = 1; block <= PS1_DATA_BLOCKS && found < blocks; block++) {
        if (frame_is_free(image, block)) chosen[found++] = block;
    }
    if (found < blocks) return SIGIL_ERR_NOT_FOUND;

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
