// SPDX-License-Identifier: MPL-2.0
#include "card_gamecube.h"
#include <stdio.h>
#include <stdlib.h>

/* Card layout per YAGCD section 12 and the SDK's CARDDir/CARDID structures. */
#define HEADER_SIZE_MBITS   0x22u
#define HEADER_ENCODING     0x24u
#define HEADER_CHECKSUM     0x1FCu

#define DIR_BLOCK_FIRST     1u
#define DIR_COUNTER         0x1FFAu
#define DIR_CHECKSUM        0x1FFCu

#define BAT_BLOCK_FIRST     3u
#define BAT_CHECKSUM        0x0u
#define BAT_COUNTER         0x4u
#define BAT_FREE_BLOCKS     0x6u
#define BAT_LAST_ALLOCATED  0x8u
#define BAT_MAP             0xAu
#define BAT_FREE            0x0000u
#define BAT_CHAIN_END       0xFFFFu

#define DENTRY_GAMECODE     0x00u
#define DENTRY_MAKERCODE_END 0x06u
#define DENTRY_FILENAME     0x08u
#define DENTRY_FIRST_BLOCK  0x36u
#define DENTRY_BLOCK_COUNT  0x38u
#define DENTRY_EMPTY        0xFFu

#define GC_MAX_DATA_BLOCKS  (GC_MAX_CARD_SIZE / GC_BLOCK_SIZE - GC_SYSTEM_BLOCKS)

/* GameShark .gcs and MaxDrive .sav, per Dolphin's documented import formats. */
#define GCS_DENTRY 0x110u
#define GCS_DATA   0x150u
#define SAV_DENTRY 0x80u
#define SAV_DATA   0xC0u

typedef struct {
    uint32_t total_blocks;
    uint32_t dir_block;
    uint32_t bat_block;
} card_view;

void sigil_gamecube_checksum(const uint8_t *data, size_t len, uint16_t *sum, uint16_t *inverse) {
    uint16_t s = 0, inv = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint16_t word = sigil_read_be16(data + i);
        s = (uint16_t)(s + word);
        inv = (uint16_t)(inv + (uint16_t)~word);
    }
    /* The SDK's __CARDCheckSum stores 0xFFFF as 0. */
    *sum = s == 0xFFFF ? 0 : s;
    *inverse = inv == 0xFFFF ? 0 : inv;
}

static bool checksum_matches(const uint8_t *data, size_t len, const uint8_t *stored) {
    uint16_t sum, inverse;
    sigil_gamecube_checksum(data, len, &sum, &inverse);
    return sigil_read_be16(stored) == sum && sigil_read_be16(stored + 2) == inverse;
}

static void seal(const uint8_t *data, size_t len, uint8_t *stored) {
    uint16_t sum, inverse;
    sigil_gamecube_checksum(data, len, &sum, &inverse);
    sigil_write_be16(stored, sum);
    sigil_write_be16(stored + 2, inverse);
}

static void seal_header(uint8_t *header) {
    seal(header, HEADER_CHECKSUM, header + HEADER_CHECKSUM);
}

static void seal_dir(uint8_t *dir) {
    seal(dir, DIR_CHECKSUM, dir + DIR_CHECKSUM);
}

static void seal_bat(uint8_t *bat) {
    seal(bat + BAT_COUNTER, GC_BLOCK_SIZE - BAT_COUNTER, bat + BAT_CHECKSUM);
}

static uint32_t total_blocks_for_size(uint64_t size) {
    if (size % GC_BLOCK_SIZE != 0) return 0;
    uint64_t blocks = size / GC_BLOCK_SIZE;
    for (uint64_t valid = 64; valid <= 2048; valid *= 2) {
        if (blocks == valid) return (uint32_t)blocks;
    }
    return 0;
}

static bool header_valid(const uint8_t *header, uint32_t total_blocks) {
    return checksum_matches(header, HEADER_CHECKSUM, header + HEADER_CHECKSUM) &&
           (uint32_t)sigil_read_be16(header + HEADER_SIZE_MBITS) * 16u == total_blocks;
}

static const uint8_t *block_at(const uint8_t *image, uint32_t block) {
    return image + (size_t)block * GC_BLOCK_SIZE;
}

static uint8_t *block_mut(uint8_t *image, uint32_t block) {
    return image + (size_t)block * GC_BLOCK_SIZE;
}

static int signed_counter(uint16_t counter) {
    return counter < 0x8000u ? (int)counter : (int)counter - 0x10000;
}

/* Returns 0 or 1 for the copy the console mounts, or -1 when neither is valid.
 * With both valid, the SDK and libogc compare the counters as signed 16-bit
 * values and take the second copy only when the first is lower. */
static int current_copy(bool valid0, uint16_t counter0, bool valid1, uint16_t counter1) {
    if (valid0 && valid1) return signed_counter(counter0) - signed_counter(counter1) < 0 ? 1 : 0;
    if (valid0) return 0;
    if (valid1) return 1;
    return -1;
}

static int open_view(const uint8_t *image, size_t size, card_view *v) {
    if (!image) return SIGIL_ERR_INVALID_ARG;
    v->total_blocks = total_blocks_for_size(size);
    if (v->total_blocks == 0 || !header_valid(image, v->total_blocks)) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    const uint8_t *dir0 = block_at(image, DIR_BLOCK_FIRST);
    const uint8_t *dir1 = block_at(image, DIR_BLOCK_FIRST + 1);
    int dir = current_copy(checksum_matches(dir0, DIR_CHECKSUM, dir0 + DIR_CHECKSUM), sigil_read_be16(dir0 + DIR_COUNTER),
                           checksum_matches(dir1, DIR_CHECKSUM, dir1 + DIR_CHECKSUM), sigil_read_be16(dir1 + DIR_COUNTER));

    const uint8_t *bat0 = block_at(image, BAT_BLOCK_FIRST);
    const uint8_t *bat1 = block_at(image, BAT_BLOCK_FIRST + 1);
    size_t bat_len = GC_BLOCK_SIZE - BAT_COUNTER;
    int bat = current_copy(checksum_matches(bat0 + BAT_COUNTER, bat_len, bat0), sigil_read_be16(bat0 + BAT_COUNTER),
                           checksum_matches(bat1 + BAT_COUNTER, bat_len, bat1), sigil_read_be16(bat1 + BAT_COUNTER));

    if (dir < 0 || bat < 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    v->dir_block = DIR_BLOCK_FIRST + (uint32_t)dir;
    v->bat_block = BAT_BLOCK_FIRST + (uint32_t)bat;
    return SIGIL_OK;
}

static uint16_t bat_link(const uint8_t *bat, uint32_t block) {
    return sigil_read_be16(bat + BAT_MAP + (size_t)(block - GC_SYSTEM_BLOCKS) * 2);
}

static void set_bat_link(uint8_t *bat, uint32_t block, uint16_t link) {
    sigil_write_be16(bat + BAT_MAP + (size_t)(block - GC_SYSTEM_BLOCKS) * 2, link);
}

static const uint8_t *dentry_at(const uint8_t *dir, uint32_t index) {
    return dir + (size_t)index * GC_DENTRY_SIZE;
}

static bool dentry_live(const uint8_t *dentry) {
    return dentry[DENTRY_GAMECODE] != DENTRY_EMPTY;
}

/* Walks the allocation chain from `first` and returns its length in blocks,
 * or 0 when a link leaves the data area, revisits a block, or lands on a free
 * block. `order`, when given, receives the blocks in chain order. */
static uint32_t walk_chain(const uint8_t *bat, uint32_t total_blocks, uint32_t first, uint32_t *order) {
    if (first < GC_SYSTEM_BLOCKS || first >= total_blocks) return 0;
    uint8_t seen[GC_MAX_CARD_SIZE / GC_BLOCK_SIZE / 8] = {0};
    uint32_t block = first;
    uint32_t count = 0;
    for (;;) {
        seen[block >> 3] |= (uint8_t)(1u << (block & 7));
        if (order) order[count] = block;
        count++;
        uint16_t next = bat_link(bat, block);
        if (next == BAT_CHAIN_END) return count;
        if (next < GC_SYSTEM_BLOCKS || next >= total_blocks) return 0;
        if (seen[next >> 3] & (1u << (next & 7))) return 0;
        block = next;
    }
}

static uint32_t free_block_count(const uint8_t *bat, uint32_t total_blocks) {
    uint32_t free_blocks = 0;
    for (uint32_t block = GC_SYSTEM_BLOCKS; block < total_blocks; block++) {
        if (bat_link(bat, block) == BAT_FREE) free_blocks++;
    }
    return free_blocks;
}

/* The directory index of the live entry whose save starts at `first_block`
 * with a chain of `blocks` blocks, or -1. `blocks` 0 accepts any length. */
static int find_entry_at(const uint8_t *dir, const uint8_t *bat, uint32_t total_blocks,
                         uint32_t first_block, uint32_t blocks) {
    for (uint32_t i = 0; i < GC_DIR_ENTRIES; i++) {
        const uint8_t *d = dentry_at(dir, i);
        if (!dentry_live(d) || sigil_read_be16(d + DENTRY_FIRST_BLOCK) != first_block) continue;
        uint32_t count = sigil_read_be16(d + DENTRY_BLOCK_COUNT);
        if (blocks != 0 && count != blocks) continue;
        if (walk_chain(bat, total_blocks, first_block, NULL) != count) continue;
        return (int)i;
    }
    return -1;
}

static int find_identity(const uint8_t *dir, const uint8_t *dentry) {
    for (uint32_t i = 0; i < GC_DIR_ENTRIES; i++) {
        const uint8_t *d = dentry_at(dir, i);
        if (dentry_live(d) && memcmp(d, dentry, DENTRY_MAKERCODE_END) == 0 &&
            strncmp((const char *)d + DENTRY_FILENAME, (const char *)dentry + DENTRY_FILENAME, GC_FILENAME_LEN) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int sigil_gamecube_card_load(const sigil_io *io, uint8_t **image, size_t *size) {
    if (!io || !io->read || !image || !size) return SIGIL_ERR_INVALID_ARG;
    *image = NULL;
    *size = 0;

    int64_t stream_size = io->size ? io->size(io->ctx) : -1;
    if (stream_size >= 0 && total_blocks_for_size((uint64_t)stream_size) == 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    uint8_t header[GC_BLOCK_SIZE];
    int rc = sigil_io_read_exact(io, 0, header, sizeof(header));
    if (rc != SIGIL_OK) return stream_size >= 0 ? rc : SIGIL_ERR_UNSUPPORTED_FORMAT;

    uint32_t total_blocks = (uint32_t)sigil_read_be16(header + HEADER_SIZE_MBITS) * 16u;
    if (total_blocks_for_size((uint64_t)total_blocks * GC_BLOCK_SIZE) == 0 || !header_valid(header, total_blocks)) {
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
    size_t card_size = (size_t)total_blocks * GC_BLOCK_SIZE;
    if (stream_size >= 0 && (uint64_t)stream_size != card_size) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    if (stream_size < 0) {
        uint8_t beyond;
        if (io->read(io->ctx, card_size, &beyond, 1) > 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }

    uint8_t *card = (uint8_t *)malloc(card_size);
    if (!card) return SIGIL_ERR_OOM;
    memcpy(card, header, GC_BLOCK_SIZE);
    rc = sigil_io_read_exact(io, GC_BLOCK_SIZE, card + GC_BLOCK_SIZE, card_size - GC_BLOCK_SIZE);
    if (rc != SIGIL_OK) {
        free(card);
        return stream_size >= 0 ? rc : SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
    card_view view;
    if (open_view(card, card_size, &view) != SIGIL_OK) {
        free(card);
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
    *image = card;
    *size = card_size;
    return SIGIL_OK;
}

int sigil_gamecube_card_list(const uint8_t *image, size_t size, sigil_card_listing **out) {
    if (!out) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    card_view v;
    int rc = open_view(image, size, &v);
    if (rc != SIGIL_OK) return rc;
    const uint8_t *dir = block_at(image, v.dir_block);
    const uint8_t *bat = block_at(image, v.bat_block);

    /* How many live entries' chains reach each block: a block two entries
     * share marks both broken, since deleting either frees the other's. */
    uint8_t *claims = (uint8_t *)calloc(v.total_blocks, 1);
    uint32_t *order = (uint32_t *)malloc(GC_MAX_DATA_BLOCKS * sizeof(uint32_t));
    sigil_card_listing *listing = claims && order ? sigil_card_listing_new(SIGIL_CARD_FORMAT_GAMECUBE_RAW, GC_DIR_ENTRIES)
                                                  : NULL;
    if (!listing) { free(claims); free(order); return SIGIL_ERR_OOM; }
    listing->total_blocks = v.total_blocks - GC_SYSTEM_BLOCKS;
    listing->free_blocks = free_block_count(bat, v.total_blocks);
    for (uint32_t i = 0; i < GC_DIR_ENTRIES; i++) {
        const uint8_t *d = dentry_at(dir, i);
        if (!dentry_live(d)) continue;
        uint32_t n = walk_chain(bat, v.total_blocks, sigil_read_be16(d + DENTRY_FIRST_BLOCK), order);
        for (uint32_t k = 0; k < n; k++) {
            if (claims[order[k]] < UINT8_MAX) claims[order[k]]++;
        }
    }

    for (uint32_t i = 0; i < GC_DIR_ENTRIES; i++) {
        const uint8_t *d = dentry_at(dir, i);
        if (!dentry_live(d)) {
            listing->free_slots++;
            continue;
        }
        uint32_t first = sigil_read_be16(d + DENTRY_FIRST_BLOCK);
        uint32_t blocks = sigil_read_be16(d + DENTRY_BLOCK_COUNT);
        char name[GC_FILENAME_LEN + 1];
        size_t name_len = 0;
        while (name_len < GC_FILENAME_LEN && d[DENTRY_FILENAME + name_len] != '\0') name_len++;
        memcpy(name, d + DENTRY_FILENAME, name_len);
        name[name_len] = '\0';
        char owner[SIGIL_CARD_OWNER_MAX];
        snprintf(owner, sizeof(owner), "%02X%02X%02X%02X", d[DENTRY_GAMECODE], d[DENTRY_GAMECODE + 1],
                 d[DENTRY_GAMECODE + 2], d[DENTRY_GAMECODE + 3]);
        uint32_t walked = blocks ? walk_chain(bat, v.total_blocks, first, order) : 0;
        bool shared = false;
        for (uint32_t k = 0; k < walked && !shared; k++) shared = claims[order[k]] > 1;
        if (blocks == 0 || walked != blocks || shared) {
            sigil_card_listing_corrupt(listing, name, owner, first);
            continue;
        }
        sigil_card_entry *e = &listing->entries[listing->entry_count++];
        memcpy(e->name, name, name_len + 1);
        memcpy(e->owner_id, owner, sizeof(owner));
        e->blocks = blocks;
        e->first_block = first;
    }
    free(claims);
    free(order);
    *out = listing;
    return SIGIL_OK;
}

static int copy_chain(const uint8_t *image, const card_view *v, uint32_t first_block,
                      uint32_t blocks, uint8_t *out) {
    uint32_t order[GC_MAX_DATA_BLOCKS];
    uint32_t count = walk_chain(block_at(image, v->bat_block), v->total_blocks, first_block, order);
    if (count == 0 || count != blocks) return SIGIL_ERR_INVALID_ARG;
    for (uint32_t i = 0; i < count; i++) {
        memcpy(out + (size_t)i * GC_BLOCK_SIZE, block_at(image, order[i]), GC_BLOCK_SIZE);
    }
    return SIGIL_OK;
}

int sigil_gamecube_entry_data(const uint8_t *image, size_t size, uint32_t first_block,
                              uint32_t blocks, uint8_t *out) {
    if (!out) return SIGIL_ERR_INVALID_ARG;
    card_view v;
    int rc = open_view(image, size, &v);
    if (rc != SIGIL_OK) return rc;
    return copy_chain(image, &v, first_block, blocks, out);
}

size_t sigil_gamecube_gci_size(uint32_t blocks) {
    return GC_DENTRY_SIZE + (size_t)blocks * GC_BLOCK_SIZE;
}

int sigil_gamecube_extract(const uint8_t *image, size_t size, uint32_t first_block,
                           uint32_t blocks, uint8_t *out) {
    if (!out || blocks == 0) return SIGIL_ERR_INVALID_ARG;
    card_view v;
    int rc = open_view(image, size, &v);
    if (rc != SIGIL_OK) return rc;
    const uint8_t *dir = block_at(image, v.dir_block);
    int index = find_entry_at(dir, block_at(image, v.bat_block), v.total_blocks, first_block, blocks);
    if (index < 0) return SIGIL_ERR_INVALID_ARG;
    rc = copy_chain(image, &v, first_block, blocks, out + GC_DENTRY_SIZE);
    if (rc != SIGIL_OK) return rc;
    memcpy(out, dentry_at(dir, (uint32_t)index), GC_DENTRY_SIZE);
    return SIGIL_OK;
}

int sigil_gamecube_format(uint8_t *image, size_t size, bool shift_jis) {
    uint32_t total_blocks = total_blocks_for_size(size);
    if (!image || total_blocks == 0) return SIGIL_ERR_INVALID_ARG;
    memset(image, 0xFF, size);

    uint8_t *header = block_mut(image, 0);
    memset(header, 0, HEADER_ENCODING + 2);
    sigil_write_be16(header + HEADER_SIZE_MBITS, (uint16_t)(total_blocks / 16u));
    sigil_write_be16(header + HEADER_ENCODING, shift_jis ? 1 : 0);
    seal_header(header);

    for (uint16_t copy = 0; copy < 2; copy++) {
        uint8_t *dir = block_mut(image, DIR_BLOCK_FIRST + copy);
        sigil_write_be16(dir + DIR_COUNTER, copy);
        seal_dir(dir);

        uint8_t *bat = block_mut(image, BAT_BLOCK_FIRST + copy);
        memset(bat, 0, GC_BLOCK_SIZE);
        sigil_write_be16(bat + BAT_COUNTER, copy);
        sigil_write_be16(bat + BAT_FREE_BLOCKS, (uint16_t)(total_blocks - GC_SYSTEM_BLOCKS));
        sigil_write_be16(bat + BAT_LAST_ALLOCATED, GC_SYSTEM_BLOCKS - 1);
        seal_bat(bat);
    }
    return SIGIL_OK;
}

/* Writes `dir` and `bat`, each one update past the copy the card mounts now,
 * to both copies. */
static void commit(uint8_t *image, const card_view *v, uint8_t *dir, uint8_t *bat) {
    sigil_write_be16(dir + DIR_COUNTER, (uint16_t)(sigil_read_be16(block_at(image, v->dir_block) + DIR_COUNTER) + 1));
    seal_dir(dir);
    sigil_write_be16(bat + BAT_COUNTER, (uint16_t)(sigil_read_be16(block_at(image, v->bat_block) + BAT_COUNTER) + 1));
    sigil_write_be16(bat + BAT_FREE_BLOCKS, (uint16_t)free_block_count(bat, v->total_blocks));
    seal_bat(bat);
    for (uint32_t copy = 0; copy < 2; copy++) {
        memcpy(block_mut(image, DIR_BLOCK_FIRST + copy), dir, GC_BLOCK_SIZE);
        memcpy(block_mut(image, BAT_BLOCK_FIRST + copy), bat, GC_BLOCK_SIZE);
    }
}

static uint32_t gci_blocks(const uint8_t *gci, size_t len) {
    if (!gci || len < sigil_gamecube_gci_size(1)) return 0;
    if ((len - GC_DENTRY_SIZE) % GC_BLOCK_SIZE != 0) return 0;
    size_t blocks = (len - GC_DENTRY_SIZE) / GC_BLOCK_SIZE;
    if (blocks > GC_MAX_DATA_BLOCKS || sigil_read_be16(gci + DENTRY_BLOCK_COUNT) != blocks) return 0;
    if (!dentry_live(gci)) return 0;
    return (uint32_t)blocks;
}

uint32_t sigil_gamecube_cost(const uint8_t *gci, size_t len) {
    return gci_blocks(gci, len);
}

int sigil_gamecube_inject(uint8_t *image, size_t size, const uint8_t *gci, size_t len) {
    card_view v;
    int rc = open_view(image, size, &v);
    if (rc != SIGIL_OK) return rc;
    uint32_t blocks = gci_blocks(gci, len);
    if (blocks == 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    uint8_t dir[GC_BLOCK_SIZE];
    uint8_t bat[GC_BLOCK_SIZE];
    memcpy(dir, block_at(image, v.dir_block), GC_BLOCK_SIZE);
    memcpy(bat, block_at(image, v.bat_block), GC_BLOCK_SIZE);
    if (find_identity(dir, gci) >= 0) return SIGIL_ERR_EXISTS;

    int slot = -1;
    for (uint32_t i = 0; i < GC_DIR_ENTRIES && slot < 0; i++) {
        if (!dentry_live(dentry_at(dir, i))) slot = (int)i;
    }
    uint32_t chosen[GC_MAX_DATA_BLOCKS];
    uint32_t found = 0;
    for (uint32_t block = GC_SYSTEM_BLOCKS; block < v.total_blocks && found < blocks; block++) {
        if (bat_link(bat, block) == BAT_FREE) chosen[found++] = block;
    }
    if (slot < 0 || found < blocks) return SIGIL_ERR_NO_SPACE;

    for (uint32_t i = 0; i < blocks; i++) {
        uint16_t link = i + 1 == blocks ? (uint16_t)BAT_CHAIN_END : (uint16_t)chosen[i + 1];
        set_bat_link(bat, chosen[i], link);
        memcpy(block_mut(image, chosen[i]), gci + GC_DENTRY_SIZE + (size_t)i * GC_BLOCK_SIZE, GC_BLOCK_SIZE);
    }
    sigil_write_be16(bat + BAT_LAST_ALLOCATED, (uint16_t)chosen[blocks - 1]);
    uint8_t *dentry = dir + (size_t)slot * GC_DENTRY_SIZE;
    memcpy(dentry, gci, GC_DENTRY_SIZE);
    sigil_write_be16(dentry + DENTRY_FIRST_BLOCK, (uint16_t)chosen[0]);
    commit(image, &v, dir, bat);
    return SIGIL_OK;
}

int sigil_gamecube_delete(uint8_t *image, size_t size, uint32_t first_block) {
    card_view v;
    int rc = open_view(image, size, &v);
    if (rc != SIGIL_OK) return rc;

    uint8_t dir[GC_BLOCK_SIZE];
    uint8_t bat[GC_BLOCK_SIZE];
    memcpy(dir, block_at(image, v.dir_block), GC_BLOCK_SIZE);
    memcpy(bat, block_at(image, v.bat_block), GC_BLOCK_SIZE);
    int index = find_entry_at(dir, bat, v.total_blocks, first_block, 0);
    if (index < 0) return SIGIL_ERR_INVALID_ARG;

    uint32_t order[GC_MAX_DATA_BLOCKS];
    uint32_t count = walk_chain(bat, v.total_blocks, first_block, order);
    for (uint32_t i = 0; i < count; i++) {
        set_bat_link(bat, order[i], BAT_FREE);
        memset(block_mut(image, order[i]), 0xFF, GC_BLOCK_SIZE);
    }
    memset(dir + (size_t)index * GC_DENTRY_SIZE, DENTRY_EMPTY, GC_DENTRY_SIZE);
    commit(image, &v, dir, bat);
    return SIGIL_OK;
}

int sigil_gamecube_verify(const uint8_t *image, size_t size, const uint8_t *gci, size_t len) {
    card_view v;
    int rc = open_view(image, size, &v);
    if (rc != SIGIL_OK) return rc;
    uint32_t blocks = gci_blocks(gci, len);
    if (blocks == 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    int index = find_identity(block_at(image, v.dir_block), gci);
    if (index < 0) return SIGIL_ERR_NOT_FOUND;
    const uint8_t *dentry = dentry_at(block_at(image, v.dir_block), (uint32_t)index);
    if (sigil_read_be16(dentry + DENTRY_BLOCK_COUNT) != blocks) return SIGIL_ERR_NOT_FOUND;

    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return SIGIL_ERR_OOM;
    rc = SIGIL_ERR_NOT_FOUND;
    if (sigil_gamecube_extract(image, size, sigil_read_be16(dentry + DENTRY_FIRST_BLOCK), blocks, copy) == SIGIL_OK) {
        memcpy(copy + DENTRY_FIRST_BLOCK, gci + DENTRY_FIRST_BLOCK, 2);
        if (memcmp(copy, gci, len) == 0) rc = SIGIL_OK;
    }
    free(copy);
    return rc;
}

static bool wrapped_blocks(size_t len, size_t data_offset, size_t *blocks) {
    if (len < data_offset + GC_BLOCK_SIZE || (len - data_offset) % GC_BLOCK_SIZE != 0) return false;
    *blocks = (len - data_offset) / GC_BLOCK_SIZE;
    return *blocks <= GC_MAX_DATA_BLOCKS;
}

static void swap_pair(uint8_t *p) {
    uint8_t t = p[0];
    p[0] = p[1];
    p[1] = t;
}

int sigil_gamecube_to_gci(const uint8_t *save, size_t len, uint8_t *out, size_t *out_len) {
    if (!save || !out || !out_len) return SIGIL_ERR_INVALID_ARG;
    *out_len = 0;
    size_t blocks = 0;
    size_t data_offset;
    if (len >= 6 && memcmp(save, "GCSAVE", 6) == 0 && wrapped_blocks(len, GCS_DATA, &blocks)) {
        memcpy(out, save + GCS_DENTRY, GC_DENTRY_SIZE);
        sigil_write_be16(out + DENTRY_BLOCK_COUNT, (uint16_t)blocks);
        data_offset = GCS_DATA;
    } else if (len >= 12 && memcmp(save, "DATELGC_SAVE", 12) == 0 && wrapped_blocks(len, SAV_DATA, &blocks)) {
        memcpy(out, save + SAV_DENTRY, GC_DENTRY_SIZE);
        /* MaxDrive swaps the bytes of each 16-bit field from 0x2C on, and the
         * unused byte with the banner flags. */
        swap_pair(out + 0x06);
        for (size_t at = 0x2C; at < GC_DENTRY_SIZE; at += 2) swap_pair(out + at);
        data_offset = SAV_DATA;
    } else if (len >= GC_DENTRY_SIZE) {
        memcpy(out, save, GC_DENTRY_SIZE);
        data_offset = GC_DENTRY_SIZE;
    } else {
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
    size_t gci_len = GC_DENTRY_SIZE + (len - data_offset);
    if (gci_blocks(out, gci_len) == 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    memcpy(out + GC_DENTRY_SIZE, save + data_offset, len - data_offset);
    *out_len = gci_len;
    return SIGIL_OK;
}

/* F-Zero GX's f_zero.dat keeps the card serials at fixed offsets and a
 * CRC-16 over the first four blocks at offset 0. Offsets and the CRC were
 * checked against the f_zero.dat on the card-raw-usa sample. */
#define FZERO_BLOCKS        4u
#define FZERO_CRC_POLY      0x8408u

static bool binds_to_serial(const uint8_t *dentry) {
    static const char name[GC_FILENAME_LEN] = "f_zero.dat";
    return memcmp(dentry + DENTRY_GAMECODE, "GFZ", 3) == 0 &&
           memcmp(dentry + DENTRY_FILENAME, name, GC_FILENAME_LEN) == 0;
}

static uint8_t *chain_byte(uint8_t *image, const uint32_t *order, size_t offset) {
    return block_mut(image, order[offset / GC_BLOCK_SIZE]) + offset % GC_BLOCK_SIZE;
}

int sigil_gamecube_bind_serial(uint8_t *image, size_t size, uint32_t first_block) {
    card_view v;
    int rc = open_view(image, size, &v);
    if (rc != SIGIL_OK) return rc;
    const uint8_t *dir = block_at(image, v.dir_block);
    const uint8_t *bat = block_at(image, v.bat_block);
    int index = find_entry_at(dir, bat, v.total_blocks, first_block, 0);
    if (index < 0) return SIGIL_ERR_INVALID_ARG;
    if (!binds_to_serial(dentry_at(dir, (uint32_t)index))) return SIGIL_OK;

    uint32_t order[GC_MAX_DATA_BLOCKS];
    if (walk_chain(bat, v.total_blocks, first_block, order) < FZERO_BLOCKS) return SIGIL_ERR_INVALID_ARG;

    const uint8_t *header = block_at(image, 0);
    uint32_t serial1 = 0, serial2 = 0;
    for (size_t word = 0; word < 8; word += 2) {
        serial1 ^= sigil_read_be32(header + word * 4);
        serial2 ^= sigil_read_be32(header + (word + 1) * 4);
    }
    sigil_write_be16(chain_byte(image, order, GC_BLOCK_SIZE + 0x66), (uint16_t)(serial1 >> 16));
    sigil_write_be16(chain_byte(image, order, 3 * GC_BLOCK_SIZE + 0x1580), (uint16_t)(serial2 >> 16));
    sigil_write_be16(chain_byte(image, order, GC_BLOCK_SIZE + 0x60), (uint16_t)serial1);
    sigil_write_be16(chain_byte(image, order, GC_BLOCK_SIZE + 0x200), (uint16_t)serial2);

    uint16_t crc = 0xFFFF;
    for (size_t at = 2; at < FZERO_BLOCKS * GC_BLOCK_SIZE; at++) {
        crc ^= *chain_byte(image, order, at);
        for (int bit = 0; bit < 8; bit++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ FZERO_CRC_POLY) : (uint16_t)(crc >> 1);
    }
    sigil_write_be16(chain_byte(image, order, 0), (uint16_t)~crc);
    return SIGIL_OK;
}

int sigil_gamecube_card_list_io(const sigil_io *io, sigil_card_listing **out) {
    uint8_t *image = NULL;
    size_t size = 0;
    int rc = sigil_gamecube_card_load(io, &image, &size);
    if (rc == SIGIL_OK) rc = sigil_gamecube_card_list(image, size, out);
    free(image);
    return rc;
}
