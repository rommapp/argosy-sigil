// SPDX-License-Identifier: MPL-2.0
#include "card_dreamcast.h"
#include <stdlib.h>

/* Layout from mc.pp.se/dc/vms/flashmem.html: the root block is the last one,
 * the FAT holds one little-endian u16 per block, and directory entries are 32
 * bytes. */
#define VMU_ROOT_BLOCK      255u
#define VMU_FAT_FREE        0xFFFCu
#define VMU_FAT_END         0xFFFAu
#define VMU_TYPE_EMPTY      0x00u
#define VMU_TYPE_DATA       0x33u
#define VMU_TYPE_GAME       0xCCu
#define VMU_SLOTS_PER_BLOCK (VMU_BLOCK_SIZE / VMU_DIR_ENTRY_SIZE)

#define ROOT_FILL_LEN   16u
#define ROOT_FILL       0x55u
#define ROOT_COLOUR     0x10u
#define ROOT_TIME       0x30u
#define ROOT_LAST_BLOCK 0x40u
#define ROOT_ROOT_BLOCK 0x44u
#define ROOT_FAT_BLOCK  0x46u
#define ROOT_FAT_SIZE   0x48u
#define ROOT_DIR_BLOCK  0x4Au
#define ROOT_DIR_SIZE   0x4Cu
#define ROOT_USER_SIZE  0x50u
#define ROOT_FIELD_52   0x52u
#define ROOT_FIELD_56   0x56u

#define ENTRY_FIRST_BLOCK 0x02u
#define ENTRY_NAME        0x04u
#define ENTRY_TIME        0x10u
#define ENTRY_SIZE        0x18u
#define ENTRY_HEADER      0x1Au

#define VMI_SIZE          108u
#define VMI_CHECKSUM      0x00u
#define VMI_TIME          0x44u
#define VMI_RESOURCE_NAME 0x50u
#define VMI_FILENAME      0x58u
#define VMI_FILE_MODE     0x64u
#define VMI_FILE_SIZE     0x68u
#define VMI_MODE_GAME     0x02u
#define VMI_MODE_PROTECT  0x01u

typedef struct {
    uint32_t fat_block;
    uint32_t dir_block;
    uint32_t dir_blocks;
    int      dir_step;
    uint32_t user_blocks;
} vmu_layout;

static const uint8_t *block_at(const uint8_t *image, uint32_t block) {
    return image + (size_t)block * VMU_BLOCK_SIZE;
}

static uint8_t *block_mut(uint8_t *image, uint32_t block) {
    return image + (size_t)block * VMU_BLOCK_SIZE;
}

static uint16_t fat_get(const uint8_t *image, const vmu_layout *l, uint32_t block) {
    return sigil_read_le16(block_at(image, l->fat_block) + 2 * block);
}

static void fat_set(uint8_t *image, const vmu_layout *l, uint32_t block, uint32_t value) {
    sigil_write_le16(block_mut(image, l->fat_block) + 2 * block, value);
}

static bool range_clear(uint32_t low, uint32_t high, const vmu_layout *l) {
    return low >= l->user_blocks && high < VMU_ROOT_BLOCK &&
           (l->fat_block < low || l->fat_block > high);
}

/* The standard directory runs down from its recorded block. Some tools record
 * the lowest block and write upwards instead; the direction is the one whose
 * blocks stay clear of the user area, the FAT and the root. */
static bool read_layout(const uint8_t *image, vmu_layout *l) {
    const uint8_t *root = block_at(image, VMU_ROOT_BLOCK);
    for (uint32_t i = 0; i < ROOT_FILL_LEN; i++) {
        if (root[i] != ROOT_FILL) return false;
    }
    l->fat_block = sigil_read_le16(root + ROOT_FAT_BLOCK);
    l->dir_block = sigil_read_le16(root + ROOT_DIR_BLOCK);
    l->dir_blocks = sigil_read_le16(root + ROOT_DIR_SIZE);
    l->user_blocks = sigil_read_le16(root + ROOT_USER_SIZE);
    if (sigil_read_le16(root + ROOT_FAT_SIZE) != 1) return false;
    if (l->fat_block >= VMU_ROOT_BLOCK || l->user_blocks == 0 || l->user_blocks > l->fat_block) return false;
    if (l->dir_blocks == 0 || l->dir_block >= VMU_ROOT_BLOCK) return false;

    if (l->dir_block + 1 >= l->dir_blocks &&
        range_clear(l->dir_block + 1 - l->dir_blocks, l->dir_block, l)) {
        l->dir_step = -1;
    } else if (range_clear(l->dir_block, l->dir_block + l->dir_blocks - 1, l)) {
        l->dir_step = 1;
    } else {
        return false;
    }
    return fat_get(image, l, VMU_ROOT_BLOCK) == VMU_FAT_END &&
           fat_get(image, l, l->fat_block) == VMU_FAT_END;
}

static uint32_t slot_count(const vmu_layout *l) {
    return l->dir_blocks * VMU_SLOTS_PER_BLOCK;
}

static size_t slot_offset(const vmu_layout *l, uint32_t slot) {
    int64_t block = (int64_t)l->dir_block + (int64_t)l->dir_step * (int64_t)(slot / VMU_SLOTS_PER_BLOCK);
    return (size_t)block * VMU_BLOCK_SIZE + (size_t)(slot % VMU_SLOTS_PER_BLOCK) * VMU_DIR_ENTRY_SIZE;
}

static bool is_file(const uint8_t *entry) {
    return entry[0] == VMU_TYPE_DATA || entry[0] == VMU_TYPE_GAME;
}

/* Follows the FAT from `first` and returns the chain's length in blocks, or 0
 * when a link leaves the user area or revisits a block. `order`, when given,
 * receives the blocks in chain order. */
static uint32_t walk_chain(const uint8_t *image, const vmu_layout *l, uint32_t first,
                           uint32_t order[VMU_BLOCKS]) {
    uint8_t seen[VMU_BLOCKS] = {0};
    uint32_t block = first;
    uint32_t count = 0;
    for (;;) {
        if (block >= l->user_blocks || seen[block]) return 0;
        seen[block] = 1;
        if (order) order[count] = block;
        count++;
        uint16_t next = fat_get(image, l, block);
        if (next == VMU_FAT_END) return count;
        block = next;
    }
}

static uint32_t entry_chain(const uint8_t *image, const vmu_layout *l, const uint8_t *entry,
                            uint32_t order[VMU_BLOCKS]) {
    uint32_t count = walk_chain(image, l, sigil_read_le16(entry + ENTRY_FIRST_BLOCK), order);
    return count == sigil_read_le16(entry + ENTRY_SIZE) ? count : 0;
}

/* The first listed file in directory order that starts at `first_block` and
 * has an intact chain, or SIZE_MAX. */
static size_t find_by_block(const uint8_t *image, const vmu_layout *l, uint32_t first_block) {
    for (uint32_t s = 0; s < slot_count(l); s++) {
        size_t off = slot_offset(l, s);
        const uint8_t *entry = image + off;
        if (!is_file(entry) || sigil_read_le16(entry + ENTRY_FIRST_BLOCK) != first_block) continue;
        if (entry_chain(image, l, entry, NULL) != 0) return off;
    }
    return SIZE_MAX;
}

int sigil_dreamcast_card_load(const sigil_io *io, uint8_t image[VMU_CARD_SIZE]) {
    if (!io || !io->read || !image) return SIGIL_ERR_INVALID_ARG;
    int64_t size = io->size ? io->size(io->ctx) : -1;
    if (size >= 0 && size != (int64_t)VMU_CARD_SIZE) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    size_t got = 0;
    int rc = sigil_io_read_upto(io, 0, image, VMU_CARD_SIZE, &got);
    if (rc != SIGIL_OK) return rc;
    if (got != VMU_CARD_SIZE) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    if (size < 0) {
        uint8_t extra;
        rc = sigil_io_read_upto(io, VMU_CARD_SIZE, &extra, 1, &got);
        if (rc != SIGIL_OK) return rc;
        if (got != 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
    vmu_layout l;
    return read_layout(image, &l) ? SIGIL_OK : SIGIL_ERR_UNSUPPORTED_FORMAT;
}

/* How many files' chains reach each block, so a block two files share marks
 * both broken. */
static void count_claims(const uint8_t *image, const vmu_layout *l, uint8_t claims[VMU_BLOCKS]) {
    memset(claims, 0, VMU_BLOCKS);
    uint32_t order[VMU_BLOCKS];
    for (uint32_t s = 0; s < slot_count(l); s++) {
        const uint8_t *entry = image + slot_offset(l, s);
        if (!is_file(entry)) continue;
        uint32_t n = entry_chain(image, l, entry, order);
        for (uint32_t i = 0; i < n; i++) {
            if (claims[order[i]] < UINT8_MAX) claims[order[i]]++;
        }
    }
}

static bool chain_shared(const uint8_t *image, const vmu_layout *l, const uint8_t *entry,
                         const uint8_t claims[VMU_BLOCKS]) {
    uint32_t order[VMU_BLOCKS];
    uint32_t n = entry_chain(image, l, entry, order);
    for (uint32_t i = 0; i < n; i++) {
        if (claims[order[i]] > 1) return true;
    }
    return false;
}

int sigil_dreamcast_card_list(const uint8_t image[VMU_CARD_SIZE], sigil_card_listing **out) {
    if (!image || !out) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    vmu_layout l;
    if (!read_layout(image, &l)) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    uint8_t claims[VMU_BLOCKS];
    count_claims(image, &l, claims);

    sigil_card_listing *listing = sigil_card_listing_new(SIGIL_CARD_FORMAT_DREAMCAST_VMU, slot_count(&l));
    if (!listing) return SIGIL_ERR_OOM;
    listing->total_blocks = l.user_blocks;
    for (uint32_t b = 0; b < l.user_blocks; b++) {
        if (fat_get(image, &l, b) == VMU_FAT_FREE) listing->free_blocks++;
    }
    for (uint32_t s = 0; s < slot_count(&l); s++) {
        const uint8_t *entry = image + slot_offset(&l, s);
        if (entry[0] == VMU_TYPE_EMPTY) {
            listing->free_slots++;
            continue;
        }
        if (!is_file(entry)) continue;
        char name[VMU_NAME_LEN + 1];
        memcpy(name, entry + ENTRY_NAME, VMU_NAME_LEN);
        name[VMU_NAME_LEN] = '\0';
        uint32_t blocks = entry_chain(image, &l, entry, NULL);
        if (blocks == 0 || chain_shared(image, &l, entry, claims)) {
            sigil_card_listing_corrupt(listing, name, NULL, sigil_read_le16(entry + ENTRY_FIRST_BLOCK));
            continue;
        }
        sigil_card_entry *e = &listing->entries[listing->entry_count++];
        memcpy(e->name, name, sizeof(name));
        e->blocks = blocks;
        e->first_block = sigil_read_le16(entry + ENTRY_FIRST_BLOCK);
    }
    *out = listing;
    return SIGIL_OK;
}

int sigil_dreamcast_entry_data(const uint8_t image[VMU_CARD_SIZE], uint32_t first_block,
                               uint32_t blocks, uint8_t *out) {
    if (!image || !out) return SIGIL_ERR_INVALID_ARG;
    vmu_layout l;
    if (!read_layout(image, &l)) return SIGIL_ERR_INVALID_ARG;
    uint32_t order[VMU_BLOCKS];
    uint32_t count = walk_chain(image, &l, first_block, order);
    if (count == 0 || count != blocks) return SIGIL_ERR_INVALID_ARG;
    for (uint32_t i = 0; i < count; i++) {
        memcpy(out + (size_t)i * VMU_BLOCK_SIZE, block_at(image, order[i]), VMU_BLOCK_SIZE);
    }
    return SIGIL_OK;
}

int sigil_dreamcast_card_list_io(const sigil_io *io, sigil_card_listing **out) {
    uint8_t *image = (uint8_t *)malloc(VMU_CARD_SIZE);
    if (!image) return SIGIL_ERR_OOM;
    int rc = sigil_dreamcast_card_load(io, image);
    if (rc == SIGIL_OK) rc = sigil_dreamcast_card_list(image, out);
    free(image);
    return rc;
}

size_t sigil_dreamcast_dci_size(uint32_t blocks) {
    return VMU_DIR_ENTRY_SIZE + (size_t)blocks * VMU_BLOCK_SIZE;
}

/* DCI stores file data with each 4-byte group byte-reversed, the order
 * Nexus-style tools read it off the VMU. Reversing is its own inverse. */
static void reverse_groups(uint8_t *dst, const uint8_t *src, size_t len) {
    for (size_t i = 0; i + 4 <= len; i += 4) {
        dst[i] = src[i + 3];
        dst[i + 1] = src[i + 2];
        dst[i + 2] = src[i + 1];
        dst[i + 3] = src[i];
    }
}

int sigil_dreamcast_extract(const uint8_t image[VMU_CARD_SIZE], uint32_t first_block,
                            uint32_t blocks, uint8_t *out) {
    if (!image || !out) return SIGIL_ERR_INVALID_ARG;
    vmu_layout l;
    if (!read_layout(image, &l)) return SIGIL_ERR_INVALID_ARG;
    size_t off = find_by_block(image, &l, first_block);
    if (off == SIZE_MAX) return SIGIL_ERR_INVALID_ARG;
    uint32_t order[VMU_BLOCKS];
    uint32_t count = entry_chain(image, &l, image + off, order);
    if (count != blocks) return SIGIL_ERR_INVALID_ARG;

    memcpy(out, image + off, VMU_DIR_ENTRY_SIZE);
    sigil_write_le16(out + ENTRY_FIRST_BLOCK, 0);
    for (uint32_t i = 0; i < count; i++) {
        reverse_groups(out + sigil_dreamcast_dci_size(i), block_at(image, order[i]), VMU_BLOCK_SIZE);
    }
    return SIGIL_OK;
}

/* Card-level fields as a stock formatted VMU image carries them (the root
 * block of the pacit-game-and-data-vmu and extended-blocks-vmu samples):
 * white custom colour, a 1998-11-27 00:00:58 Friday BCD format time, the
 * standard 200-block layout, and the undocumented words at 0x52 and 0x56. */
static const uint8_t FORMAT_COLOUR[5] = { 0x01, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t FORMAT_TIME[8] = { 0x19, 0x98, 0x11, 0x27, 0x00, 0x00, 0x58, 0x04 };

#define FORMAT_FAT_BLOCK   254u
#define FORMAT_DIR_BLOCK   253u
#define FORMAT_DIR_BLOCKS  13u
#define FORMAT_USER_BLOCKS 200u

void sigil_dreamcast_format(uint8_t image[VMU_CARD_SIZE]) {
    memset(image, 0, VMU_CARD_SIZE);
    uint8_t *root = block_mut(image, VMU_ROOT_BLOCK);
    memset(root, ROOT_FILL, ROOT_FILL_LEN);
    memcpy(root + ROOT_COLOUR, FORMAT_COLOUR, sizeof(FORMAT_COLOUR));
    memcpy(root + ROOT_TIME, FORMAT_TIME, sizeof(FORMAT_TIME));
    sigil_write_le16(root + ROOT_LAST_BLOCK, VMU_ROOT_BLOCK);
    sigil_write_le16(root + ROOT_ROOT_BLOCK, VMU_ROOT_BLOCK);
    sigil_write_le16(root + ROOT_FAT_BLOCK, FORMAT_FAT_BLOCK);
    sigil_write_le16(root + ROOT_FAT_SIZE, 1);
    sigil_write_le16(root + ROOT_DIR_BLOCK, FORMAT_DIR_BLOCK);
    sigil_write_le16(root + ROOT_DIR_SIZE, FORMAT_DIR_BLOCKS);
    sigil_write_le16(root + ROOT_USER_SIZE, FORMAT_USER_BLOCKS);
    sigil_write_le16(root + ROOT_FIELD_52, 0x1F);
    sigil_write_le16(root + ROOT_FIELD_56, 0x80);

    vmu_layout l = { FORMAT_FAT_BLOCK, FORMAT_DIR_BLOCK, FORMAT_DIR_BLOCKS, -1, FORMAT_USER_BLOCKS };
    uint32_t dir_low = FORMAT_DIR_BLOCK + 1 - FORMAT_DIR_BLOCKS;
    for (uint32_t b = 0; b < dir_low; b++) fat_set(image, &l, b, VMU_FAT_FREE);
    fat_set(image, &l, dir_low, VMU_FAT_END);
    for (uint32_t b = dir_low + 1; b <= FORMAT_DIR_BLOCK; b++) fat_set(image, &l, b, b - 1);
    fat_set(image, &l, FORMAT_FAT_BLOCK, VMU_FAT_END);
    fat_set(image, &l, VMU_ROOT_BLOCK, VMU_FAT_END);
}

void sigil_dreamcast_format_like(uint8_t image[VMU_CARD_SIZE], const uint8_t like[VMU_CARD_SIZE]) {
    vmu_layout l;
    if (!like || !read_layout(like, &l)) {
        sigil_dreamcast_format(image);
        return;
    }
    memcpy(image, like, VMU_CARD_SIZE);
    for (uint32_t s = 0; s < slot_count(&l); s++) memset(image + slot_offset(&l, s), 0, VMU_DIR_ENTRY_SIZE);
    for (uint32_t b = 0; b < l.user_blocks; b++) {
        fat_set(image, &l, b, VMU_FAT_FREE);
        memset(block_mut(image, b), 0, VMU_BLOCK_SIZE);
    }
}

static uint32_t dci_blocks(const uint8_t *dci, size_t len) {
    if (!dci || len < sigil_dreamcast_dci_size(1)) return 0;
    if ((len - VMU_DIR_ENTRY_SIZE) % VMU_BLOCK_SIZE != 0) return 0;
    size_t blocks = (len - VMU_DIR_ENTRY_SIZE) / VMU_BLOCK_SIZE;
    if (blocks > VMU_BLOCKS || !is_file(dci) || sigil_read_le16(dci + ENTRY_SIZE) != blocks) return 0;
    return (uint32_t)blocks;
}

static bool name_taken(const uint8_t *image, const vmu_layout *l, const uint8_t *name) {
    for (uint32_t s = 0; s < slot_count(l); s++) {
        const uint8_t *entry = image + slot_offset(l, s);
        if (is_file(entry) && memcmp(entry + ENTRY_NAME, name, VMU_NAME_LEN) == 0) return true;
    }
    return false;
}

/* A game file runs contiguously from block 0, so a VMU holding one has no
 * room for another; the BIOS hands a data file the free blocks from the top
 * of the user area down. */
static bool choose_blocks(const uint8_t *image, const vmu_layout *l, bool game,
                          uint32_t blocks, uint32_t chosen[VMU_BLOCKS]) {
    if (blocks > l->user_blocks) return false;
    if (game) {
        for (uint32_t b = 0; b < blocks; b++) {
            if (fat_get(image, l, b) != VMU_FAT_FREE) return false;
            chosen[b] = b;
        }
        return true;
    }
    uint32_t found = 0;
    for (uint32_t b = l->user_blocks; b-- > 0 && found < blocks;) {
        if (fat_get(image, l, b) == VMU_FAT_FREE) chosen[found++] = b;
    }
    return found == blocks;
}

uint32_t sigil_dreamcast_cost(const uint8_t *dci, size_t len) {
    return dci_blocks(dci, len);
}

int sigil_dreamcast_inject(uint8_t image[VMU_CARD_SIZE], const uint8_t *dci, size_t len) {
    if (!image) return SIGIL_ERR_INVALID_ARG;
    uint32_t blocks = dci_blocks(dci, len);
    if (blocks == 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    vmu_layout l;
    if (!read_layout(image, &l)) return SIGIL_ERR_INVALID_ARG;
    if (name_taken(image, &l, dci + ENTRY_NAME)) return SIGIL_ERR_EXISTS;
    uint32_t chosen[VMU_BLOCKS];
    if (!choose_blocks(image, &l, dci[0] == VMU_TYPE_GAME, blocks, chosen)) return SIGIL_ERR_NO_SPACE;
    size_t slot = SIZE_MAX;
    for (uint32_t s = 0; s < slot_count(&l) && slot == SIZE_MAX; s++) {
        if (image[slot_offset(&l, s)] == VMU_TYPE_EMPTY) slot = slot_offset(&l, s);
    }
    if (slot == SIZE_MAX) return SIGIL_ERR_NO_SPACE;

    for (uint32_t i = 0; i < blocks; i++) {
        reverse_groups(block_mut(image, chosen[i]), dci + sigil_dreamcast_dci_size(i), VMU_BLOCK_SIZE);
        fat_set(image, &l, chosen[i], i + 1 == blocks ? VMU_FAT_END : chosen[i + 1]);
    }
    memcpy(image + slot, dci, VMU_DIR_ENTRY_SIZE);
    sigil_write_le16(image + slot + ENTRY_FIRST_BLOCK, chosen[0]);
    return SIGIL_OK;
}

int sigil_dreamcast_delete(uint8_t image[VMU_CARD_SIZE], uint32_t first_block) {
    if (!image) return SIGIL_ERR_INVALID_ARG;
    vmu_layout l;
    if (!read_layout(image, &l)) return SIGIL_ERR_INVALID_ARG;
    size_t off = find_by_block(image, &l, first_block);
    if (off == SIZE_MAX) return SIGIL_ERR_INVALID_ARG;
    uint32_t order[VMU_BLOCKS];
    uint32_t count = entry_chain(image, &l, image + off, order);
    for (uint32_t i = 0; i < count; i++) fat_set(image, &l, order[i], VMU_FAT_FREE);
    memset(image + off, 0, VMU_DIR_ENTRY_SIZE);
    return SIGIL_OK;
}

int sigil_dreamcast_verify(const uint8_t image[VMU_CARD_SIZE], const uint8_t *dci, size_t len) {
    if (!image) return SIGIL_ERR_INVALID_ARG;
    uint32_t blocks = dci_blocks(dci, len);
    if (blocks == 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    vmu_layout l;
    if (!read_layout(image, &l)) return SIGIL_ERR_INVALID_ARG;
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return SIGIL_ERR_OOM;
    int rc = SIGIL_ERR_NOT_FOUND;
    for (uint32_t s = 0; s < slot_count(&l) && rc != SIGIL_OK; s++) {
        const uint8_t *entry = image + slot_offset(&l, s);
        if (!is_file(entry) || memcmp(entry + ENTRY_NAME, dci + ENTRY_NAME, VMU_NAME_LEN) != 0) continue;
        if (entry_chain(image, &l, entry, NULL) != blocks) continue;
        /* The .dci's first-block field names where it sat before; inject
         * replaces it, so it isn't compared. */
        if (sigil_dreamcast_extract(image, sigil_read_le16(entry + ENTRY_FIRST_BLOCK), blocks, copy) == SIGIL_OK &&
            memcmp(copy, dci, ENTRY_FIRST_BLOCK) == 0 &&
            memcmp(copy + ENTRY_FIRST_BLOCK + 2, dci + ENTRY_FIRST_BLOCK + 2, len - ENTRY_FIRST_BLOCK - 2) == 0) {
            rc = SIGIL_OK;
        }
    }
    free(copy);
    return rc;
}

static bool to_bcd(uint32_t value, uint8_t *out) {
    if (value > 99) return false;
    *out = (uint8_t)(((value / 10) << 4) | (value % 10));
    return true;
}

/* VMI keeps the time in binary (u16 year, then month, day, hour, minute,
 * second, weekday); the directory entry keeps the same fields in BCD with the
 * year split into century and year. */
static bool vmi_time_to_bcd(const uint8_t *vmi_time, uint8_t out[8]) {
    uint32_t year = sigil_read_le16(vmi_time);
    bool ok = to_bcd(year / 100, &out[0]) && to_bcd(year % 100, &out[1]);
    for (uint32_t i = 0; i < 6 && ok; i++) ok = to_bcd(vmi_time[2 + i], &out[2 + i]);
    return ok;
}

int sigil_dreamcast_dci_from_vms(const uint8_t *vms, size_t vms_len,
                                 const uint8_t *vmi, size_t vmi_len, uint8_t *out) {
    if (!vms || !vmi || !out) return SIGIL_ERR_INVALID_ARG;
    if (vmi_len != VMI_SIZE || vms_len == 0 || vms_len % VMU_BLOCK_SIZE != 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    size_t blocks = vms_len / VMU_BLOCK_SIZE;
    if (blocks > VMU_BLOCKS || sigil_read_le32(vmi + VMI_FILE_SIZE) != vms_len) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    static const char SEGA[4] = { 'S', 'E', 'G', 'A' };
    for (uint32_t i = 0; i < 4; i++) {
        if (vmi[VMI_CHECKSUM + i] != (vmi[VMI_RESOURCE_NAME + i] & (uint8_t)SEGA[i])) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }

    uint8_t entry[VMU_DIR_ENTRY_SIZE] = {0};
    if (!vmi_time_to_bcd(vmi + VMI_TIME, entry + ENTRY_TIME)) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    uint16_t mode = sigil_read_le16(vmi + VMI_FILE_MODE);
    bool game = (mode & VMI_MODE_GAME) != 0;
    entry[0] = game ? VMU_TYPE_GAME : VMU_TYPE_DATA;
    entry[1] = (mode & VMI_MODE_PROTECT) ? 0xFF : 0x00;
    memcpy(entry + ENTRY_NAME, vmi + VMI_FILENAME, VMU_NAME_LEN);
    sigil_write_le16(entry + ENTRY_SIZE, (uint32_t)blocks);
    sigil_write_le16(entry + ENTRY_HEADER, game ? 1 : 0);

    memcpy(out, entry, VMU_DIR_ENTRY_SIZE);
    reverse_groups(out + VMU_DIR_ENTRY_SIZE, vms, vms_len);
    return SIGIL_OK;
}
