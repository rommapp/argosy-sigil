// SPDX-License-Identifier: MPL-2.0
#include "card_ps2.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define PS2_MAGIC           "Sony PS2 Memory Card Format "
#define PS2_MAGIC_LEN       28
#define PS2_PAGE_LEN        512u
#define PS2_SPARE_LEN       16u
#define PS2_MAX_CLUSTERS    (1u << 20)
#define PS2_ENTRY_SIZE      512u

#define PS2_FAT_ALLOCATED   0x80000000u
#define PS2_FAT_CHAIN_END   0x7FFFFFFFu

#define PS2_MODE_EXISTS     0x8000u
#define PS2_MODE_DIR        0x0020u
#define PS2_MODE_FILE       0x0010u

#define PS2_ENTRY_MODE      0x00
#define PS2_ENTRY_LENGTH    0x04
#define PS2_ENTRY_CLUSTER   0x10
#define PS2_ENTRY_NAME      0x40
#define PS2_NAME_LEN        32u

static bool cluster_in_card(const sigil_ps2_card *card, uint32_t absolute) {
    return absolute < card->clusters_per_card;
}

static const uint8_t *cluster_at(const sigil_ps2_card *card, uint32_t absolute) {
    return card->clusters + (size_t)absolute * card->cluster_size;
}

/* The FAT entry for relative cluster `n`, through the indirect FAT. False when
 * a table it passes through lies outside the card. */
static bool fat_entry(const sigil_ps2_card *card, uint32_t n, uint32_t *value) {
    uint32_t per_cluster = card->cluster_size / 4;
    uint32_t fat_index = n / per_cluster;
    uint32_t ifc_slot = fat_index / per_cluster;
    if (ifc_slot >= PS2_IFC_SLOTS) return false;
    uint32_t indirect = card->ifc[ifc_slot];
    if (!cluster_in_card(card, indirect)) return false;
    uint32_t fat_cluster = sigil_read_le32(cluster_at(card, indirect) + (fat_index % per_cluster) * 4);
    if (!cluster_in_card(card, fat_cluster)) return false;
    *value = sigil_read_le32(cluster_at(card, fat_cluster) + (n % per_cluster) * 4);
    return true;
}

/* Walks the chain from relative cluster `first`. Returns its length, or 0 when
 * it runs into a free cluster, leaves the card, or loops. `order` may be NULL;
 * otherwise it has room for card->alloc_end entries. */
static uint32_t walk_chain(const sigil_ps2_card *card, uint32_t first, uint32_t *order) {
    uint32_t cluster = first;
    for (uint32_t count = 0; count < card->alloc_end; ) {
        if (cluster >= card->alloc_end) return 0;
        if (order) order[count] = cluster;
        count++;
        uint32_t value;
        if (!fat_entry(card, cluster, &value) || !(value & PS2_FAT_ALLOCATED)) return 0;
        uint32_t next = value & ~PS2_FAT_ALLOCATED;
        if (next == PS2_FAT_CHAIN_END) return count;
        cluster = next;
    }
    return 0;
}

/* Copies `len` bytes of the chain starting at relative cluster `first`. */
static bool read_chain(const sigil_ps2_card *card, uint32_t first, uint8_t *out, size_t len) {
    uint32_t *order = (uint32_t *)malloc((size_t)card->alloc_end * sizeof(uint32_t));
    if (!order) return false;
    uint32_t count = walk_chain(card, first, order);
    bool ok = count > 0 && (uint64_t)count * card->cluster_size >= len;
    for (uint32_t i = 0; ok && i < count && (size_t)i * card->cluster_size < len; i++) {
        uint32_t absolute = order[i] + card->alloc_offset;
        if (!cluster_in_card(card, absolute)) { ok = false; break; }
        size_t at = (size_t)i * card->cluster_size;
        size_t n = len - at < card->cluster_size ? len - at : card->cluster_size;
        memcpy(out + at, cluster_at(card, absolute), n);
    }
    free(order);
    return ok;
}

/* Reads the `count` directory entries of the directory starting at relative
 * cluster `first`. `*out` is malloc'd. */
static bool read_dir(const sigil_ps2_card *card, uint32_t first, uint32_t count, uint8_t **out) {
    if (count == 0 || count > card->alloc_end * (card->cluster_size / PS2_ENTRY_SIZE)) return false;
    size_t len = (size_t)count * PS2_ENTRY_SIZE;
    *out = (uint8_t *)malloc(len);
    if (!*out) return false;
    if (!read_chain(card, first, *out, len)) { free(*out); *out = NULL; return false; }
    return true;
}

static uint32_t root_entry_count(const sigil_ps2_card *card) {
    uint8_t first[PS2_ENTRY_SIZE];
    if (!read_chain(card, card->rootdir_cluster, first, sizeof(first))) return 0;
    return sigil_read_le32(first + PS2_ENTRY_LENGTH);
}

/* A card file shorter than its superblock says is not a card. */
static int read_fully(const sigil_io *io, uint64_t off, uint8_t *buf, size_t len) {
    size_t got = 0;
    int rc = sigil_io_read_upto(io, off, buf, len, &got);
    if (rc != SIGIL_OK) return rc;
    return got == len ? SIGIL_OK : SIGIL_ERR_UNSUPPORTED_FORMAT;
}

int sigil_ps2_card_load(const sigil_io *io, sigil_ps2_card *card) {
    if (!io || !io->read || !card) return SIGIL_ERR_INVALID_ARG;
    memset(card, 0, sizeof(*card));

    uint8_t super[PS2_PAGE_LEN];
    int rc = read_fully(io, 0, super, sizeof(super));
    if (rc != SIGIL_OK) return rc == SIGIL_ERR_IO ? rc : SIGIL_ERR_UNSUPPORTED_FORMAT;
    if (memcmp(super, PS2_MAGIC, PS2_MAGIC_LEN) != 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    uint32_t page_len = sigil_read_le16(super + 0x28);
    uint32_t pages_per_cluster = sigil_read_le16(super + 0x2A);
    uint32_t clusters = sigil_read_le32(super + 0x30);
    if (page_len != PS2_PAGE_LEN || pages_per_cluster == 0 || pages_per_cluster > 16) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    if (clusters == 0 || clusters > PS2_MAX_CLUSTERS) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    card->cluster_size = page_len * pages_per_cluster;
    card->clusters_per_card = clusters;
    card->alloc_offset = sigil_read_le32(super + 0x34);
    card->alloc_end = sigil_read_le32(super + 0x38);
    card->rootdir_cluster = sigil_read_le32(super + 0x3C);
    for (uint32_t i = 0; i < PS2_IFC_SLOTS; i++) card->ifc[i] = sigil_read_le32(super + 0x50 + i * 4);
    if (card->alloc_offset >= clusters || card->alloc_end == 0 ||
        card->alloc_end > clusters - card->alloc_offset) {
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }

    uint64_t pages = (uint64_t)clusters * pages_per_cluster;
    int64_t size = io->size ? io->size(io->ctx) : -1;
    if (size == (int64_t)(pages * (page_len + PS2_SPARE_LEN))) card->ecc = 1;
    else if (size != (int64_t)(pages * page_len)) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    card->clusters = (uint8_t *)malloc((size_t)pages * page_len);
    if (!card->clusters) return SIGIL_ERR_OOM;
    card->dirty = (uint8_t *)calloc((size_t)pages, 1);
    if (!card->dirty) { sigil_ps2_card_free(card); return SIGIL_ERR_OOM; }
    if (card->ecc) {
        card->spare = (uint8_t *)malloc((size_t)pages * PS2_SPARE_LEN);
        if (!card->spare) { sigil_ps2_card_free(card); return SIGIL_ERR_OOM; }
    }
    uint32_t stride = page_len + (card->ecc ? PS2_SPARE_LEN : 0);
    for (uint64_t p = 0; p < pages; p++) {
        rc = read_fully(io, p * stride, card->clusters + p * page_len, page_len);
        if (rc == SIGIL_OK && card->ecc) {
            rc = read_fully(io, p * stride + page_len, card->spare + p * PS2_SPARE_LEN, PS2_SPARE_LEN);
        }
        if (rc != SIGIL_OK) { sigil_ps2_card_free(card); return rc; }
    }
    return SIGIL_OK;
}

void sigil_ps2_card_free(sigil_ps2_card *card) {
    if (!card) return;
    free(card->clusters);
    free(card->spare);
    free(card->dirty);
    card->clusters = NULL;
    card->spare = NULL;
    card->dirty = NULL;
}

static uint32_t clusters_for(uint32_t bytes, uint32_t cluster_size) {
    return (uint32_t)(((uint64_t)bytes + cluster_size - 1) / cluster_size);
}

/* Clusters a save folder takes: its own directory entries plus each file's
 * data. False when the folder or a file doesn't read. */
static bool save_clusters(const sigil_ps2_card *card, uint32_t first, uint32_t count, uint32_t *total) {
    uint8_t *entries = NULL;
    if (!read_dir(card, first, count, &entries)) return false;
    uint32_t sum = clusters_for(count * PS2_ENTRY_SIZE, card->cluster_size);
    bool ok = true;
    for (uint32_t i = 2; i < count && ok; i++) {
        const uint8_t *e = entries + (size_t)i * PS2_ENTRY_SIZE;
        uint16_t mode = sigil_read_le16(e + PS2_ENTRY_MODE);
        if (!(mode & PS2_MODE_EXISTS) || !(mode & PS2_MODE_FILE)) continue;
        uint32_t length = sigil_read_le32(e + PS2_ENTRY_LENGTH);
        uint32_t need = clusters_for(length, card->cluster_size);
        if (need > 0 && walk_chain(card, sigil_read_le32(e + PS2_ENTRY_CLUSTER), NULL) < need) ok = false;
        sum += need;
    }
    free(entries);
    *total = sum;
    return ok;
}

int sigil_ps2_card_list(const sigil_ps2_card *card, sigil_card_listing **out) {
    if (!card || !card->clusters || !out) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    uint32_t count = root_entry_count(card);
    uint8_t *root = NULL;
    if (!read_dir(card, card->rootdir_cluster, count, &root)) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    sigil_card_listing *listing = sigil_card_listing_new(SIGIL_CARD_FORMAT_PS2, count);
    if (!listing) { free(root); return SIGIL_ERR_OOM; }
    listing->total_blocks = card->alloc_end;
    for (uint32_t n = 0; n < card->alloc_end; n++) {
        uint32_t value;
        if (fat_entry(card, n, &value) && !(value & PS2_FAT_ALLOCATED)) listing->free_blocks++;
    }
    listing->free_slots = listing->free_blocks;

    for (uint32_t i = 2; i < count; i++) {
        const uint8_t *e = root + (size_t)i * PS2_ENTRY_SIZE;
        uint16_t mode = sigil_read_le16(e + PS2_ENTRY_MODE);
        if (!(mode & PS2_MODE_EXISTS) || !(mode & PS2_MODE_DIR)) continue;
        uint32_t first = sigil_read_le32(e + PS2_ENTRY_CLUSTER);
        uint32_t blocks = 0;
        if (!save_clusters(card, first, sigil_read_le32(e + PS2_ENTRY_LENGTH), &blocks)) {
            listing->corrupt_count++;
            continue;
        }
        sigil_card_entry *entry = &listing->entries[listing->entry_count++];
        memcpy(entry->name, e + PS2_ENTRY_NAME, PS2_NAME_LEN);
        entry->name[PS2_NAME_LEN] = '\0';
        sigil_card_sony_owner(entry->name, entry->owner_id);
        entry->blocks = blocks;
        entry->first_block = first;
    }
    free(root);
    *out = listing;
    return SIGIL_OK;
}

/* A folder's own "." entry records no length, so its entry count comes from
 * the root entry that points at it. 0 when no root entry does. */
static uint32_t save_entry_count(const sigil_ps2_card *card, uint32_t first_cluster) {
    uint32_t count = root_entry_count(card);
    uint8_t *root = NULL;
    if (!read_dir(card, card->rootdir_cluster, count, &root)) return 0;
    uint32_t found = 0;
    for (uint32_t i = 2; i < count && !found; i++) {
        const uint8_t *e = root + (size_t)i * PS2_ENTRY_SIZE;
        uint16_t mode = sigil_read_le16(e + PS2_ENTRY_MODE);
        if ((mode & PS2_MODE_EXISTS) && (mode & PS2_MODE_DIR) &&
            sigil_read_le32(e + PS2_ENTRY_CLUSTER) == first_cluster) {
            found = sigil_read_le32(e + PS2_ENTRY_LENGTH);
        }
    }
    free(root);
    return found;
}

int sigil_ps2_save_data(const sigil_ps2_card *card, uint32_t first_cluster,
                        uint8_t **out, size_t *len) {
    if (!card || !out || !len) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    *len = 0;
    uint32_t count = save_entry_count(card, first_cluster);
    uint8_t *entries = NULL;
    if (!read_dir(card, first_cluster, count, &entries)) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    size_t total = 0;
    for (uint32_t i = 2; i < count; i++) {
        const uint8_t *e = entries + (size_t)i * PS2_ENTRY_SIZE;
        if ((sigil_read_le16(e + PS2_ENTRY_MODE) & (PS2_MODE_EXISTS | PS2_MODE_FILE)) == (PS2_MODE_EXISTS | PS2_MODE_FILE)) {
            total += sigil_read_le32(e + PS2_ENTRY_LENGTH);
        }
    }
    uint8_t *data = (uint8_t *)malloc(total ? total : 1);
    if (!data) { free(entries); return SIGIL_ERR_OOM; }

    size_t at = 0;
    int rc = SIGIL_OK;
    for (uint32_t i = 2; i < count && rc == SIGIL_OK; i++) {
        const uint8_t *e = entries + (size_t)i * PS2_ENTRY_SIZE;
        if ((sigil_read_le16(e + PS2_ENTRY_MODE) & (PS2_MODE_EXISTS | PS2_MODE_FILE)) != (PS2_MODE_EXISTS | PS2_MODE_FILE)) continue;
        uint32_t length = sigil_read_le32(e + PS2_ENTRY_LENGTH);
        if (length > 0 && !read_chain(card, sigil_read_le32(e + PS2_ENTRY_CLUSTER), data + at, length)) {
            rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
        }
        at += length;
    }
    free(entries);
    if (rc != SIGIL_OK) { free(data); return rc; }
    *out = data;
    *len = total;
    return SIGIL_OK;
}

#define PS2_FORMAT_CLUSTERS     8192u
#define PS2_PAGES_PER_CLUSTER   2u
#define PS2_PAGES_PER_BLOCK     16u
#define PS2_FORMAT_IFC          8u
#define PS2_FORMAT_FAT_FIRST    9u
#define PS2_FORMAT_ALLOC_OFFSET 41u
#define PS2_FORMAT_ALLOC_END    8135u
#define PS2_FORMAT_BACKUP1      1023u
#define PS2_FORMAT_BACKUP2      1022u
#define PS2_CARD_TYPE           2u
#define PS2_CARD_FLAGS          0x2Bu
#define PS2_FORMATTED_AT        0x16u
#define PS2_FORMATTED_BYTE      0x6Fu
#define PS2_MODE_ROOT_SELF      0x8427u
#define PS2_MODE_ROOT_PARENT    0xA426u

static uint8_t *cluster_mut(sigil_ps2_card *card, uint32_t absolute) {
    return card->clusters + (size_t)absolute * card->cluster_size;
}

static void write_dir_entry(uint8_t *e, uint16_t mode, uint32_t length, uint32_t cluster,
                            const uint8_t tod[PS2_TOD_SIZE], const char *name) {
    memset(e, 0, PS2_ENTRY_SIZE);
    sigil_write_le16(e + PS2_ENTRY_MODE, mode);
    sigil_write_le32(e + PS2_ENTRY_LENGTH, length);
    memcpy(e + 0x08, tod, PS2_TOD_SIZE);
    sigil_write_le32(e + PS2_ENTRY_CLUSTER, cluster);
    memcpy(e + 0x18, tod, PS2_TOD_SIZE);
    memcpy(e + PS2_ENTRY_NAME, name, strlen(name));
}

/* The superblock page of an empty 8 MB card, over zeroed bytes. */
static void write_superblock(uint8_t *super) {
    memcpy(super, PS2_MAGIC, PS2_MAGIC_LEN);
    memcpy(super + PS2_MAGIC_LEN, "1.2.0.0", 7);
    sigil_write_le16(super + 0x28, PS2_PAGE_LEN);
    sigil_write_le16(super + 0x2A, PS2_PAGES_PER_CLUSTER);
    sigil_write_le16(super + 0x2C, PS2_PAGES_PER_BLOCK);
    sigil_write_le16(super + 0x2E, 0xFF00);
    sigil_write_le32(super + 0x30, PS2_FORMAT_CLUSTERS);
    sigil_write_le32(super + 0x34, PS2_FORMAT_ALLOC_OFFSET);
    sigil_write_le32(super + 0x38, PS2_FORMAT_ALLOC_END);
    sigil_write_le32(super + 0x3C, 0);
    sigil_write_le32(super + 0x40, PS2_FORMAT_BACKUP1);
    sigil_write_le32(super + 0x44, PS2_FORMAT_BACKUP2);
    sigil_write_le32(super + 0x50, PS2_FORMAT_IFC);
    memset(super + 0xD0, 0xFF, 0x80);
    super[0x150] = PS2_CARD_TYPE;
    super[0x151] = PS2_CARD_FLAGS;
}

void sigil_ps2_folder_superblock(uint8_t out[PS2_FOLDER_SUPERBLOCK_SIZE]) {
    memset(out, 0, PS2_FOLDER_SUPERBLOCK_SIZE);
    write_superblock(out);
}

bool sigil_ps2_folder_superblock_usable(const uint8_t *data, size_t len) {
    return data && len >= PS2_FOLDER_SUPERBLOCK_SIZE && data[PS2_FORMATTED_AT] == PS2_FORMATTED_BYTE;
}

int sigil_ps2_card_format(sigil_ps2_card *card, const uint8_t tod[PS2_TOD_SIZE]) {
    if (!card || !tod) return SIGIL_ERR_INVALID_ARG;
    memset(card, 0, sizeof(*card));
    card->cluster_size = PS2_PAGE_LEN * PS2_PAGES_PER_CLUSTER;
    card->clusters_per_card = PS2_FORMAT_CLUSTERS;
    card->alloc_offset = PS2_FORMAT_ALLOC_OFFSET;
    card->alloc_end = PS2_FORMAT_ALLOC_END;
    card->rootdir_cluster = 0;
    card->ecc = 1;
    for (uint32_t i = 0; i < PS2_IFC_SLOTS; i++) card->ifc[i] = 0;
    card->ifc[0] = PS2_FORMAT_IFC;
    size_t pages = (size_t)PS2_FORMAT_CLUSTERS * PS2_PAGES_PER_CLUSTER;
    card->clusters = (uint8_t *)calloc(PS2_FORMAT_CLUSTERS, card->cluster_size);
    card->spare = (uint8_t *)calloc(pages, PS2_SPARE_LEN);
    card->dirty = (uint8_t *)malloc(pages);
    if (!card->clusters || !card->spare || !card->dirty) { sigil_ps2_card_free(card); return SIGIL_ERR_OOM; }
    memset(card->dirty, 1, pages);

    write_superblock(cluster_mut(card, 0));

    uint32_t per_cluster = card->cluster_size / 4;
    uint32_t fat_clusters = PS2_FORMAT_ALLOC_OFFSET - PS2_FORMAT_FAT_FIRST;
    uint8_t *indirect = cluster_mut(card, PS2_FORMAT_IFC);
    memset(indirect, 0xFF, card->cluster_size);
    for (uint32_t i = 0; i < fat_clusters; i++) sigil_write_le32(indirect + i * 4, PS2_FORMAT_FAT_FIRST + i);
    for (uint32_t n = 0; n < fat_clusters * per_cluster; n++) {
        uint32_t value = (n == 0 || n >= PS2_FORMAT_ALLOC_END) ? 0xFFFFFFFFu : PS2_FAT_CHAIN_END;
        sigil_write_le32(cluster_mut(card, PS2_FORMAT_FAT_FIRST + n / per_cluster) + (n % per_cluster) * 4, value);
    }

    uint8_t *root = cluster_mut(card, PS2_FORMAT_ALLOC_OFFSET);
    write_dir_entry(root, PS2_MODE_ROOT_SELF, 2, 0, tod, ".");
    write_dir_entry(root + PS2_ENTRY_SIZE, PS2_MODE_ROOT_PARENT, 0, 0, tod, "..");

    uint32_t clusters_per_block = PS2_PAGES_PER_BLOCK / PS2_PAGES_PER_CLUSTER;
    memset(cluster_mut(card, PS2_FORMAT_BACKUP2 * clusters_per_block), 0xFF,
           (size_t)clusters_per_block * card->cluster_size);
    size_t erased_page = (size_t)PS2_FORMAT_BACKUP2 * PS2_PAGES_PER_BLOCK;
    memset(card->dirty + erased_page, 0, PS2_PAGES_PER_BLOCK);
    memset(card->spare + erased_page * PS2_SPARE_LEN, 0xFF, (size_t)PS2_PAGES_PER_BLOCK * PS2_SPARE_LEN);
    return SIGIL_OK;
}

/* Column parities of one byte, laid out as the PS2 ECC table has them: bits
 * 0-2 and 4-6 are the even and odd halves of each bit pairing, bit 7 the
 * byte's own parity. */
static uint8_t ecc_column_bits(uint8_t b) {
    static const uint8_t groups[6] = { 0x55, 0x33, 0x0F, 0xAA, 0xCC, 0xF0 };
    static const uint8_t positions[6] = { 0, 1, 2, 4, 5, 6 };
    uint8_t out = 0;
    for (int i = 0; i < 6; i++) {
        uint8_t v = b & groups[i];
        v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
        if (v & 1) out |= (uint8_t)(1u << positions[i]);
    }
    uint8_t p = b;
    p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    if (p & 1) out |= 0x80;
    return out;
}

void sigil_ps2_ecc(const uint8_t chunk[128], uint8_t ecc[3]) {
    uint8_t column = 0, line_even = 0, line_odd = 0;
    for (uint8_t i = 0; i < 128; i++) {
        uint8_t c = ecc_column_bits(chunk[i]);
        column ^= c;
        if (c & 0x80) {
            line_even ^= (uint8_t)~i;
            line_odd ^= i;
        }
    }
    ecc[0] = (uint8_t)(~column & 0x77);
    ecc[1] = (uint8_t)(~line_even & 0x7F);
    ecc[2] = (uint8_t)(~line_odd & 0x7F);
}

size_t sigil_ps2_card_file_size(const sigil_ps2_card *card) {
    size_t pages = (size_t)card->clusters_per_card * (card->cluster_size / PS2_PAGE_LEN);
    return pages * (PS2_PAGE_LEN + (card->ecc ? PS2_SPARE_LEN : 0));
}

int sigil_ps2_card_write(const sigil_ps2_card *card, uint8_t *out) {
    if (!card || !card->clusters || !out) return SIGIL_ERR_INVALID_ARG;
    size_t pages = (size_t)card->clusters_per_card * (card->cluster_size / PS2_PAGE_LEN);
    size_t stride = PS2_PAGE_LEN + (card->ecc ? PS2_SPARE_LEN : 0);
    for (size_t p = 0; p < pages; p++) {
        const uint8_t *page = card->clusters + p * PS2_PAGE_LEN;
        uint8_t *dst = out + p * stride;
        memcpy(dst, page, PS2_PAGE_LEN);
        if (!card->ecc) continue;
        if (!card->dirty[p]) {
            memcpy(dst + PS2_PAGE_LEN, card->spare + p * PS2_SPARE_LEN, PS2_SPARE_LEN);
            continue;
        }
        memset(dst + PS2_PAGE_LEN, 0, PS2_SPARE_LEN);
        for (int chunk = 0; chunk < 4; chunk++) sigil_ps2_ecc(page + chunk * 128, dst + PS2_PAGE_LEN + chunk * 3);
    }
    return SIGIL_OK;
}

#define PS2_ENTRY_PARENT_INDEX 0x14
#define PS2_ENTRY_CREATED      0x08
#define PS2_ENTRY_MODIFIED     0x18
#define PS2_PAD                0xFF

static void mark_cluster_dirty(sigil_ps2_card *card, uint32_t absolute) {
    uint32_t pages = card->cluster_size / PS2_PAGE_LEN;
    memset(card->dirty + (size_t)absolute * pages, 1, pages);
}

static bool fat_slot(sigil_ps2_card *card, uint32_t n, uint8_t **slot, uint32_t *fat_cluster) {
    uint32_t per_cluster = card->cluster_size / 4;
    uint32_t fat_index = n / per_cluster;
    uint32_t ifc_slot = fat_index / per_cluster;
    if (ifc_slot >= PS2_IFC_SLOTS || !cluster_in_card(card, card->ifc[ifc_slot])) return false;
    uint32_t fc = sigil_read_le32(cluster_at(card, card->ifc[ifc_slot]) + (fat_index % per_cluster) * 4);
    if (!cluster_in_card(card, fc)) return false;
    *slot = cluster_mut(card, fc) + (n % per_cluster) * 4;
    *fat_cluster = fc;
    return true;
}

static bool fat_set(sigil_ps2_card *card, uint32_t n, uint32_t value) {
    uint8_t *slot;
    uint32_t fc;
    if (!fat_slot(card, n, &slot, &fc)) return false;
    sigil_write_le32(slot, value);
    mark_cluster_dirty(card, fc);
    return true;
}

static bool cluster_is_free(const sigil_ps2_card *card, uint32_t n) {
    uint32_t value;
    return fat_entry(card, n, &value) && !(value & PS2_FAT_ALLOCATED);
}

/* Hands out the card's free clusters lowest first, and links each into the
 * chain its caller is growing. */
typedef struct {
    sigil_ps2_card *card;
    uint32_t        next;
} cluster_allocator;

static bool allocate(cluster_allocator *a, uint32_t after, bool chained, uint32_t *out) {
    while (a->next < a->card->alloc_end && !cluster_is_free(a->card, a->next)) a->next++;
    if (a->next >= a->card->alloc_end) return false;
    uint32_t n = a->next++;
    if (!fat_set(a->card, n, 0xFFFFFFFFu)) return false;
    if (chained && !fat_set(a->card, after, PS2_FAT_ALLOCATED | n)) return false;
    *out = n;
    return true;
}

static uint8_t *relative_cluster(sigil_ps2_card *card, uint32_t n) {
    uint32_t absolute = n + card->alloc_offset;
    mark_cluster_dirty(card, absolute);
    return cluster_mut(card, absolute);
}

static uint32_t entries_per_cluster(const sigil_ps2_card *card) {
    return card->cluster_size / PS2_ENTRY_SIZE;
}

/* The cluster holding entry `index` of the directory whose chain is `chain`. */
static uint8_t *entry_slot(sigil_ps2_card *card, const uint32_t *chain, uint32_t index) {
    return relative_cluster(card, chain[index / entries_per_cluster(card)]) +
           (size_t)(index % entries_per_cluster(card)) * PS2_ENTRY_SIZE;
}

static bool tod_later(const uint8_t *a, const uint8_t *b) {
    static const int order[6] = { 7, 6, 5, 4, 3, 2 };
    if (a[order[0]] != b[order[0]]) return a[order[0]] > b[order[0]];
    for (int i = 1; i < 6; i++) {
        if (a[order[i]] != b[order[i]]) return a[order[i]] > b[order[i]];
    }
    return a[1] > b[1];
}

static uint32_t clusters_needed(const sigil_ps2_card *card, const sigil_ps2_save *save, uint32_t root_slot,
                                uint32_t root_count) {
    uint32_t per = entries_per_cluster(card);
    uint32_t need = (root_slot == root_count && root_count % per == 0) ? 1 : 0;
    need += clusters_for((uint32_t)(save->file_count + 2) * PS2_ENTRY_SIZE, card->cluster_size);
    for (size_t i = 0; i < save->file_count; i++) {
        need += clusters_for(sigil_read_le32(save->files[i].entry + PS2_ENTRY_LENGTH), card->cluster_size);
    }
    return need;
}

static uint32_t free_clusters(const sigil_ps2_card *card) {
    uint32_t n = 0;
    for (uint32_t c = 0; c < card->alloc_end; c++) {
        if (cluster_is_free(card, c)) n++;
    }
    return n;
}

/* The first root slot a deleted folder left free, or `root_count` when there
 * is none and the new folder goes on the end. */
static uint32_t root_slot_for_new(const sigil_ps2_card *card, uint32_t root_count) {
    uint8_t *root = NULL;
    if (!read_dir(card, card->rootdir_cluster, root_count, &root)) return root_count;
    uint32_t slot = root_count;
    for (uint32_t i = 2; i < root_count && slot == root_count; i++) {
        if (!(sigil_read_le16(root + (size_t)i * PS2_ENTRY_SIZE + PS2_ENTRY_MODE) & PS2_MODE_EXISTS)) slot = i;
    }
    free(root);
    return slot;
}

static bool name_in_root(const sigil_ps2_card *card, const uint8_t *entry, uint32_t root_count) {
    uint8_t *root = NULL;
    if (!read_dir(card, card->rootdir_cluster, root_count, &root)) return false;
    bool found = false;
    for (uint32_t i = 2; i < root_count && !found; i++) {
        const uint8_t *e = root + (size_t)i * PS2_ENTRY_SIZE;
        found = (sigil_read_le16(e + PS2_ENTRY_MODE) & PS2_MODE_EXISTS) &&
                memcmp(e + PS2_ENTRY_NAME, entry + PS2_ENTRY_NAME, PS2_NAME_LEN) == 0;
    }
    free(root);
    return found;
}

void sigil_ps2_save_free(sigil_ps2_save *save) {
    if (!save) return;
    for (size_t i = 0; i < save->file_count; i++) free(save->files[i].data);
    free(save->files);
    save->files = NULL;
    save->file_count = 0;
}

/* The root entry pointing at the folder starting at `first_cluster`, copied
 * into `out` with its index. */
static bool root_entry_for(const sigil_ps2_card *card, uint32_t first_cluster, uint8_t out[PS2_ENTRY_SIZE],
                           uint32_t *index) {
    uint32_t count = root_entry_count(card);
    uint8_t *root = NULL;
    if (!read_dir(card, card->rootdir_cluster, count, &root)) return false;
    bool found = false;
    for (uint32_t i = 2; i < count && !found; i++) {
        const uint8_t *e = root + (size_t)i * PS2_ENTRY_SIZE;
        uint16_t mode = sigil_read_le16(e + PS2_ENTRY_MODE);
        if ((mode & PS2_MODE_EXISTS) && (mode & PS2_MODE_DIR) &&
            sigil_read_le32(e + PS2_ENTRY_CLUSTER) == first_cluster) {
            memcpy(out, e, PS2_ENTRY_SIZE);
            if (index) *index = i;
            found = true;
        }
    }
    free(root);
    return found;
}

int sigil_ps2_extract(const sigil_ps2_card *card, uint32_t first_cluster, sigil_ps2_save *out) {
    if (!card || !card->clusters || !out) return SIGIL_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!root_entry_for(card, first_cluster, out->entry, NULL)) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    uint32_t count = sigil_read_le32(out->entry + PS2_ENTRY_LENGTH);
    uint8_t *entries = NULL;
    if (count < 2 || !read_dir(card, first_cluster, count, &entries)) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    memcpy(out->self, entries, PS2_ENTRY_SIZE);
    memcpy(out->parent, entries + PS2_ENTRY_SIZE, PS2_ENTRY_SIZE);

    out->files = (sigil_ps2_file *)calloc(count, sizeof(sigil_ps2_file));
    int rc = out->files ? SIGIL_OK : SIGIL_ERR_OOM;
    for (uint32_t i = 2; i < count && rc == SIGIL_OK; i++) {
        const uint8_t *e = entries + (size_t)i * PS2_ENTRY_SIZE;
        uint16_t mode = sigil_read_le16(e + PS2_ENTRY_MODE);
        if (!(mode & PS2_MODE_EXISTS)) continue;
        if (!(mode & PS2_MODE_FILE)) { rc = SIGIL_ERR_UNSUPPORTED_FORMAT; break; }
        sigil_ps2_file *f = &out->files[out->file_count++];
        memcpy(f->entry, e, PS2_ENTRY_SIZE);
        uint32_t length = sigil_read_le32(e + PS2_ENTRY_LENGTH);
        f->data = (uint8_t *)malloc(length ? length : 1);
        if (!f->data) rc = SIGIL_ERR_OOM;
        else if (length && !read_chain(card, sigil_read_le32(e + PS2_ENTRY_CLUSTER), f->data, length)) {
            rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
        }
    }
    free(entries);
    if (rc != SIGIL_OK) sigil_ps2_save_free(out);
    return rc;
}

/* Grows `chain` by one cluster when `index` is the first entry of a new one. */
static bool reserve_entry(cluster_allocator *a, uint32_t *chain, uint32_t *chain_len, uint32_t index) {
    uint32_t per = entries_per_cluster(a->card);
    if (index % per != 0 || index / per < *chain_len) return true;
    uint32_t n;
    if (!allocate(a, chain[*chain_len - 1], true, &n)) return false;
    uint8_t *c = relative_cluster(a->card, n);
    memset(c, PS2_PAD, a->card->cluster_size);
    chain[(*chain_len)++] = n;
    return true;
}

int sigil_ps2_inject(sigil_ps2_card *card, const sigil_ps2_save *save) {
    if (!card || !card->clusters || !card->dirty || !save) return SIGIL_ERR_INVALID_ARG;
    uint32_t root_count = root_entry_count(card);
    if (root_count < 2) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    if (name_in_root(card, save->entry, root_count)) return SIGIL_ERR_EXISTS;
    uint32_t slot = root_slot_for_new(card, root_count);
    if (clusters_needed(card, save, slot, root_count) > free_clusters(card)) return SIGIL_ERR_NO_SPACE;

    uint32_t max_chain = card->alloc_end;
    uint32_t *root_chain = (uint32_t *)malloc((size_t)max_chain * sizeof(uint32_t));
    uint32_t *dir_chain = (uint32_t *)malloc((size_t)max_chain * sizeof(uint32_t));
    if (!root_chain || !dir_chain) { free(root_chain); free(dir_chain); return SIGIL_ERR_OOM; }
    uint32_t root_len = walk_chain(card, card->rootdir_cluster, root_chain);
    cluster_allocator a = { card, 0 };
    int rc = SIGIL_OK;

    if (slot == root_count && !reserve_entry(&a, root_chain, &root_len, root_count)) rc = SIGIL_ERR_NO_SPACE;
    uint32_t dir_len = 0;
    if (rc == SIGIL_OK) {
        uint32_t first;
        if (!allocate(&a, 0, false, &first)) rc = SIGIL_ERR_NO_SPACE;
        else {
            dir_chain[dir_len++] = first;
            uint8_t *c = relative_cluster(card, first);
            memset(c, PS2_PAD, card->cluster_size);
        }
    }
    if (rc == SIGIL_OK) {
        uint8_t *root_entry = entry_slot(card, root_chain, slot);
        memcpy(root_entry, save->entry, PS2_ENTRY_SIZE);
        sigil_write_le32(root_entry + PS2_ENTRY_LENGTH, (uint32_t)save->file_count + 2);
        sigil_write_le32(root_entry + PS2_ENTRY_CLUSTER, dir_chain[0]);
        sigil_write_le32(root_entry + PS2_ENTRY_PARENT_INDEX, 0);

        uint8_t *self = entry_slot(card, dir_chain, 0);
        memcpy(self, save->self, PS2_ENTRY_SIZE);
        sigil_write_le32(self + PS2_ENTRY_PARENT_INDEX, slot);
        memcpy(entry_slot(card, dir_chain, 1), save->parent, PS2_ENTRY_SIZE);
    }
    for (size_t i = 0; i < save->file_count && rc == SIGIL_OK; i++) {
        uint32_t index = (uint32_t)i + 2;
        if (!reserve_entry(&a, dir_chain, &dir_len, index)) { rc = SIGIL_ERR_NO_SPACE; break; }
        const sigil_ps2_file *f = &save->files[i];
        uint32_t length = sigil_read_le32(f->entry + PS2_ENTRY_LENGTH);
        uint32_t first = 0, prev = 0;
        for (uint32_t k = 0; k < clusters_for(length, card->cluster_size); k++) {
            uint32_t n;
            if (!allocate(&a, prev, k > 0, &n)) { rc = SIGIL_ERR_NO_SPACE; break; }
            if (k == 0) first = n;
            uint8_t *c = relative_cluster(card, n);
            size_t at = (size_t)k * card->cluster_size;
            size_t take = length - at < card->cluster_size ? length - at : card->cluster_size;
            memset(c, PS2_PAD, card->cluster_size);
            memcpy(c, f->data + at, take);
            prev = n;
        }
        uint8_t *e = entry_slot(card, dir_chain, index);
        memcpy(e, f->entry, PS2_ENTRY_SIZE);
        sigil_write_le32(e + PS2_ENTRY_CLUSTER, length ? first : 0);
    }
    if (rc == SIGIL_OK) {
        uint8_t *root_self = entry_slot(card, root_chain, 0);
        if (slot == root_count) sigil_write_le32(root_self + PS2_ENTRY_LENGTH, root_count + 1);
        if (tod_later(save->self + PS2_ENTRY_CREATED, root_self + PS2_ENTRY_MODIFIED)) {
            memcpy(root_self + PS2_ENTRY_MODIFIED, save->self + PS2_ENTRY_CREATED, PS2_TOD_SIZE);
        }
    }
    free(root_chain);
    free(dir_chain);
    return rc;
}

int sigil_ps2_delete(sigil_ps2_card *card, uint32_t first_cluster) {
    if (!card || !card->clusters || !card->dirty) return SIGIL_ERR_INVALID_ARG;
    sigil_ps2_save save;
    uint32_t index = 0;
    uint8_t entry[PS2_ENTRY_SIZE];
    if (!root_entry_for(card, first_cluster, entry, &index)) return SIGIL_ERR_INVALID_ARG;
    int rc = sigil_ps2_extract(card, first_cluster, &save);
    if (rc != SIGIL_OK) return rc;

    uint32_t *chain = (uint32_t *)malloc((size_t)card->alloc_end * sizeof(uint32_t));
    if (!chain) { sigil_ps2_save_free(&save); return SIGIL_ERR_OOM; }
    for (size_t i = 0; i <= save.file_count; i++) {
        uint32_t start = i < save.file_count ? sigil_read_le32(save.files[i].entry + PS2_ENTRY_CLUSTER) : first_cluster;
        bool has_data = i == save.file_count || sigil_read_le32(save.files[i].entry + PS2_ENTRY_LENGTH) > 0;
        uint32_t len = has_data ? walk_chain(card, start, chain) : 0;
        for (uint32_t k = 0; k < len; k++) fat_set(card, chain[k], PS2_FAT_CHAIN_END);
    }
    uint32_t root_len = walk_chain(card, card->rootdir_cluster, chain);
    if (index / entries_per_cluster(card) < root_len) {
        uint8_t *e = entry_slot(card, chain, index);
        sigil_write_le16(e + PS2_ENTRY_MODE, (uint16_t)(sigil_read_le16(e + PS2_ENTRY_MODE) & ~PS2_MODE_EXISTS));
    }
    free(chain);
    sigil_ps2_save_free(&save);
    return SIGIL_OK;
}

static bool same_file(const sigil_ps2_file *a, const sigil_ps2_file *b) {
    uint8_t ea[PS2_ENTRY_SIZE], eb[PS2_ENTRY_SIZE];
    memcpy(ea, a->entry, PS2_ENTRY_SIZE);
    memcpy(eb, b->entry, PS2_ENTRY_SIZE);
    sigil_write_le32(ea + PS2_ENTRY_CLUSTER, 0);
    sigil_write_le32(eb + PS2_ENTRY_CLUSTER, 0);
    if (memcmp(ea, eb, PS2_ENTRY_SIZE) != 0) return false;
    uint32_t length = sigil_read_le32(a->entry + PS2_ENTRY_LENGTH);
    return memcmp(a->data, b->data, length) == 0;
}

int sigil_ps2_verify(const sigil_ps2_card *card, const sigil_ps2_save *save) {
    if (!card || !card->clusters || !save) return SIGIL_ERR_INVALID_ARG;
    uint32_t count = root_entry_count(card);
    uint8_t *root = NULL;
    if (!read_dir(card, card->rootdir_cluster, count, &root)) return SIGIL_ERR_NOT_FOUND;
    int rc = SIGIL_ERR_NOT_FOUND;
    for (uint32_t i = 2; i < count && rc != SIGIL_OK; i++) {
        const uint8_t *e = root + (size_t)i * PS2_ENTRY_SIZE;
        if (!(sigil_read_le16(e + PS2_ENTRY_MODE) & PS2_MODE_EXISTS)) continue;
        if (memcmp(e + PS2_ENTRY_NAME, save->entry + PS2_ENTRY_NAME, PS2_NAME_LEN) != 0) continue;
        sigil_ps2_save found;
        if (sigil_ps2_extract(card, sigil_read_le32(e + PS2_ENTRY_CLUSTER), &found) != SIGIL_OK) continue;
        bool same = found.file_count == save->file_count;
        for (size_t k = 0; same && k < found.file_count; k++) same = same_file(&found.files[k], &save->files[k]);
        sigil_ps2_save_free(&found);
        if (same) rc = SIGIL_OK;
    }
    free(root);
    return rc;
}

void sigil_ps2_save_md5(const sigil_ps2_save *save, char out[33]) {
    sigil_md5 m;
    sigil_md5_init(&m);
    sigil_md5_update(&m, save->entry + PS2_ENTRY_NAME, PS2_NAME_LEN);
    for (size_t i = 0; i < save->file_count; i++) {
        const sigil_ps2_file *f = &save->files[i];
        uint8_t len[4];
        memcpy(len, f->entry + PS2_ENTRY_LENGTH, sizeof(len));
        sigil_md5_update(&m, f->entry + PS2_ENTRY_NAME, PS2_NAME_LEN);
        sigil_md5_update(&m, len, sizeof(len));
        sigil_md5_update(&m, f->data, sigil_read_le32(len));
    }
    uint8_t digest[16];
    sigil_md5_final(&m, digest);
    sigil_md5_hex(digest, out);
}

/* ---- PCSX2 folder cards ---------------------------------------------------- */

#define PCSX2_INDEX          "_pcsx2_index"
#define PCSX2_META_DIR       "_pcsx2_meta_directory"
#define PCSX2_META_PREFIX    "_pcsx2_meta/"
#define PCSX2_PREFIX         "_pcsx2_"
#define PCSX2_META_MIN       0x60u
#define PCSX2_DIR_MODE       0x8427u
#define PCSX2_FILE_MODE      0x8497u

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

/* A card date read as UTC, as PCSX2 reads it with timegm. */
static int64_t tod_to_unix(const uint8_t tod[PS2_TOD_SIZE]) {
    unsigned month = tod[5] ? tod[5] : 1;
    int64_t days = days_from_civil((int64_t)sigil_read_le16(tod + 6), month, tod[4] ? tod[4] : 1);
    return days * 86400 + (int64_t)tod[3] * 3600 + (int64_t)tod[2] * 60 + tod[1];
}

static void unix_to_tod(int64_t t, uint8_t tod[PS2_TOD_SIZE]) {
    int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    int64_t secs = t - days * 86400;
    int64_t y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    tod[0] = 0;
    tod[1] = (uint8_t)(secs % 60);
    tod[2] = (uint8_t)(secs / 60 % 60);
    tod[3] = (uint8_t)(secs / 3600);
    tod[4] = (uint8_t)d;
    tod[5] = (uint8_t)m;
    sigil_write_le16(tod + 6, (uint16_t)y);
}

typedef struct {
    char    name[PS2_NAME_LEN + 1];
    int64_t order;
    int64_t created;
    int64_t modified;
    bool    has_order, has_created, has_modified;
} index_row;

typedef struct {
    index_row *rows;
    size_t     count;
    size_t     room;
} index_table;

static index_row *index_add(index_table *t, const char *name) {
    if (t->count == t->room) {
        size_t next = t->room ? t->room * 2 : 8;
        index_row *grown = (index_row *)realloc(t->rows, next * sizeof(*grown));
        if (!grown) return NULL;
        t->rows = grown;
        t->room = next;
    }
    index_row *r = &t->rows[t->count++];
    memset(r, 0, sizeof(*r));
    snprintf(r->name, sizeof(r->name), "%s", name);
    return r;
}

static const index_row *index_find(const index_table *t, const char *name) {
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->rows[i].name, name) == 0) return &t->rows[i];
    }
    return NULL;
}

static void index_set(index_row *r, const char *field, int64_t value) {
    if (strcmp(field, "order") == 0) { r->order = value; r->has_order = true; }
    else if (strcmp(field, "timeCreated") == 0) { r->created = value; r->has_created = true; }
    else if (strcmp(field, "timeModified") == 0) { r->modified = value; r->has_modified = true; }
}

static const char *skip_space(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Reads one YAML scalar: single-quoted, double-quoted or plain up to any of
 * `stops`. Returns the position after it, or NULL when it doesn't fit. */
static const char *read_scalar(const char *p, const char *stops, char *out, size_t cap) {
    size_t n = 0;
    if (*p == '\'' || *p == '"') {
        char quote = *p++;
        for (;;) {
            if (!*p) return NULL;
            if (*p == quote) {
                if (quote == '\'' && p[1] == '\'') { p += 2; if (n + 1 >= cap) return NULL; out[n++] = '\''; continue; }
                p++;
                break;
            }
            if (quote == '"' && *p == '\\' && p[1]) p++;
            if (n + 1 >= cap) return NULL;
            out[n++] = *p++;
        }
    } else {
        while (*p && !strchr(stops, *p)) {
            if (n + 1 >= cap) return NULL;
            out[n++] = *p++;
        }
        while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t' || out[n - 1] == '\r')) n--;
    }
    out[n] = '\0';
    return p;
}

static bool parse_flow(const char *p, index_table *t) {
    p = skip_space(p);
    if (*p++ != '{') return false;
    for (;;) {
        p = skip_space(p);
        if (*p == '}') return true;
        char key[PS2_NAME_LEN + 8];
        p = read_scalar(p, ":", key, sizeof(key));
        if (!p || *p++ != ':') return false;
        index_row *r = index_add(t, key);
        if (!r) return false;
        p = skip_space(p);
        if (*p++ != '{') return false;
        for (;;) {
            p = skip_space(p);
            if (*p == '}') { p++; break; }
            char field[32], value[32];
            p = read_scalar(p, ":", field, sizeof(field));
            if (!p || *p++ != ':') return false;
            p = read_scalar(skip_space(p), ",}", value, sizeof(value));
            if (!p) return false;
            index_set(r, field, strtoll(value, NULL, 10));
            p = skip_space(p);
            if (*p == ',') p++;
        }
        p = skip_space(p);
        if (*p == ',') p++;
    }
}

static bool parse_block(const char *p, index_table *t) {
    index_row *current = NULL;
    while (*p) {
        const char *line = p;
        const char *eol = strchr(p, '\n');
        p = eol ? eol + 1 : line + strlen(line);
        const char *text = line;
        while (*text == ' ' || *text == '\t') text++;
        if (*text == '\n' || *text == '\r' || *text == '\0' || *text == '#') continue;
        char key[PS2_NAME_LEN + 8];
        const char *after = read_scalar(text, ":\n", key, sizeof(key));
        if (!after || *after != ':') return false;
        if (text == line) {
            current = index_add(t, key);
            if (!current) return false;
        } else {
            if (!current) return false;
            char value[32];
            if (!read_scalar(skip_space(after + 1), "\n", value, sizeof(value))) return false;
            index_set(current, key, strtoll(value, NULL, 10));
        }
    }
    return true;
}

static bool parse_index(const uint8_t *data, size_t len, index_table *t) {
    char *text = (char *)malloc(len + 1);
    if (!text) return false;
    memcpy(text, data, len);
    text[len] = '\0';
    const char *start = skip_space(text);
    bool ok = *start == '{' ? parse_flow(start, t) : parse_block(start, t);
    free(text);
    return ok;
}

static const sigil_ps2_folder_file *find_folder_file(const sigil_ps2_folder_file *files, size_t count, const char *path) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(files[i].path, path) == 0) return &files[i];
    }
    return NULL;
}

/* The entry PCSX2 builds for a file or folder it has no meta for. */
static void default_entry(uint8_t e[PS2_ENTRY_SIZE], uint16_t mode, const char *name,
                          int64_t created, int64_t modified) {
    memset(e, 0, PS2_ENTRY_SIZE);
    sigil_write_le16(e + PS2_ENTRY_MODE, mode);
    unix_to_tod(created, e + PS2_ENTRY_CREATED);
    unix_to_tod(modified, e + PS2_ENTRY_MODIFIED);
    memcpy(e + PS2_ENTRY_NAME, name, strlen(name));
}

/* Whether PCSX2 would rebuild `actual` from the index alone: every field but
 * the length, cluster and parent index it fills in itself. */
static bool rebuilds_without_meta(const uint8_t actual[PS2_ENTRY_SIZE], uint16_t default_mode) {
    uint8_t expect[PS2_ENTRY_SIZE], have[PS2_ENTRY_SIZE];
    char name[PS2_NAME_LEN + 1];
    memcpy(name, actual + PS2_ENTRY_NAME, PS2_NAME_LEN);
    name[PS2_NAME_LEN] = '\0';
    default_entry(expect, default_mode, name, tod_to_unix(actual + PS2_ENTRY_CREATED),
                  tod_to_unix(actual + PS2_ENTRY_MODIFIED));
    memcpy(have, actual, PS2_ENTRY_SIZE);
    memset(have + PS2_ENTRY_LENGTH, 0, 4);
    memset(have + PS2_ENTRY_CLUSTER, 0, 8);
    return memcmp(expect, have, PS2_ENTRY_SIZE) == 0;
}

static void entry_name(const uint8_t *e, char out[PS2_NAME_LEN + 1]) {
    memcpy(out, e + PS2_ENTRY_NAME, PS2_NAME_LEN);
    out[PS2_NAME_LEN] = '\0';
}

void sigil_ps2_folder_files_free(sigil_ps2_folder_file *files, size_t count) {
    if (!files) return;
    for (size_t i = 0; i < count; i++) free(files[i].data);
    free(files);
}

static bool add_folder_file(sigil_ps2_folder_file *files, size_t *count, const char *path,
                            const uint8_t *data, size_t len) {
    sigil_ps2_folder_file *f = &files[*count];
    if (strlen(path) >= sizeof(f->path)) return false;
    snprintf(f->path, sizeof(f->path), "%s", path);
    f->data = (uint8_t *)malloc(len ? len : 1);
    if (!f->data) return false;
    if (len) memcpy(f->data, data, len);
    f->len = len;
    (*count)++;
    return true;
}

/* Appends to a growing text buffer. */
typedef struct { char *buf; size_t len, room; bool ok; } text_out;

static void text_add(text_out *t, const char *fmt, ...) {
    if (!t->ok) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(line)) { t->ok = false; return; }
    if (t->len + (size_t)n + 1 > t->room) {
        size_t next = t->room ? t->room * 2 : 512;
        while (next < t->len + (size_t)n + 1) next *= 2;
        char *grown = (char *)realloc(t->buf, next);
        if (!grown) { t->ok = false; return; }
        t->buf = grown;
        t->room = next;
    }
    memcpy(t->buf + t->len, line, (size_t)n + 1);
    t->len += (size_t)n;
}

/* A YAML single-quoted key: quotes inside it double. */
static void quoted_key(const char *name, char *out, size_t cap) {
    size_t n = 0;
    if (n + 1 < cap) out[n++] = '\'';
    for (const char *p = name; *p && n + 3 < cap; p++) {
        out[n++] = *p;
        if (*p == '\'') out[n++] = '\'';
    }
    if (n + 1 < cap) out[n++] = '\'';
    out[n] = '\0';
}

int sigil_ps2_unpack(const sigil_ps2_save *save, sigil_ps2_folder_file **files, size_t *count) {
    if (!save || !files || !count) return SIGIL_ERR_INVALID_ARG;
    *files = NULL;
    *count = 0;
    size_t room = save->file_count * 2 + 2;
    sigil_ps2_folder_file *out = (sigil_ps2_folder_file *)calloc(room, sizeof(*out));
    if (!out) return SIGIL_ERR_OOM;
    size_t n = 0;
    int rc = SIGIL_OK;
    text_out index = { NULL, 0, 0, true };
    text_add(&index, "$ROOT:\n  timeCreated: %lld\n  timeModified: %lld\n",
             (long long)tod_to_unix(save->entry + PS2_ENTRY_CREATED),
             (long long)tod_to_unix(save->entry + PS2_ENTRY_MODIFIED));

    for (size_t i = 0; i < save->file_count && rc == SIGIL_OK; i++) {
        const sigil_ps2_file *f = &save->files[i];
        char name[PS2_NAME_LEN + 1], key[2 * PS2_NAME_LEN + 3];
        entry_name(f->entry, name);
        if (!name[0] || strchr(name, '/') || strncmp(name, PCSX2_PREFIX, strlen(PCSX2_PREFIX)) == 0) {
            rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
            break;
        }
        if (!add_folder_file(out, &n, name, f->data, sigil_read_le32(f->entry + PS2_ENTRY_LENGTH))) { rc = SIGIL_ERR_OOM; break; }
        if (!rebuilds_without_meta(f->entry, PCSX2_FILE_MODE)) {
            char meta[PS2_FOLDER_PATH_MAX];
            snprintf(meta, sizeof(meta), "%s%s", PCSX2_META_PREFIX, name);
            if (!add_folder_file(out, &n, meta, f->entry, PS2_ENTRY_SIZE)) { rc = SIGIL_ERR_OOM; break; }
        }
        quoted_key(name, key, sizeof(key));
        text_add(&index, "%s:\n  order: %zu\n  timeCreated: %lld\n  timeModified: %lld\n", key, i + 1,
                 (long long)tod_to_unix(f->entry + PS2_ENTRY_CREATED),
                 (long long)tod_to_unix(f->entry + PS2_ENTRY_MODIFIED));
    }
    if (rc == SIGIL_OK && !rebuilds_without_meta(save->entry, PCSX2_DIR_MODE) &&
        !add_folder_file(out, &n, PCSX2_META_DIR, save->entry, PS2_ENTRY_SIZE)) {
        rc = SIGIL_ERR_OOM;
    }
    if (rc == SIGIL_OK && !index.ok) rc = SIGIL_ERR_OOM;
    if (rc == SIGIL_OK && !add_folder_file(out, &n, PCSX2_INDEX, (const uint8_t *)index.buf, index.len)) rc = SIGIL_ERR_OOM;
    free(index.buf);
    if (rc != SIGIL_OK) { sigil_ps2_folder_files_free(out, n); return rc; }
    *files = out;
    *count = n;
    return SIGIL_OK;
}

typedef struct {
    const sigil_ps2_folder_file *file;
    int64_t                      order;
    size_t                       given;
} pack_item;

static int compare_items(const void *a, const void *b) {
    const pack_item *x = (const pack_item *)a, *y = (const pack_item *)b;
    if (x->order != y->order) return x->order < y->order ? -1 : 1;
    return x->given < y->given ? -1 : (x->given > y->given);
}

int sigil_ps2_pack(const char *folder, const sigil_ps2_folder_file *files, size_t count, sigil_ps2_save *out) {
    if (!folder || !out || (!files && count)) return SIGIL_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!folder[0] || strlen(folder) > PS2_NAME_LEN) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    index_table index = { NULL, 0, 0 };
    const sigil_ps2_folder_file *index_file = find_folder_file(files, count, PCSX2_INDEX);
    if (index_file && !parse_index(index_file->data, index_file->len, &index)) {
        free(index.rows);
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
    const index_row *root = index_find(&index, "$ROOT");
    if (!root) root = index_find(&index, "%ROOT");

    pack_item *items = (pack_item *)calloc(count ? count : 1, sizeof(*items));
    out->files = (sigil_ps2_file *)calloc(count ? count : 1, sizeof(sigil_ps2_file));
    if (!items || !out->files) { free(items); free(index.rows); sigil_ps2_save_free(out); return SIGIL_ERR_OOM; }

    size_t n = 0;
    int64_t legacy_order = -1;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(files[i].path, PCSX2_PREFIX, strlen(PCSX2_PREFIX)) == 0) continue;
        if (strchr(files[i].path, '/') || strlen(files[i].path) > PS2_NAME_LEN) {
            free(items); free(index.rows); sigil_ps2_save_free(out);
            return SIGIL_ERR_UNSUPPORTED_FORMAT;
        }
        const index_row *row = index_find(&index, files[i].path);
        items[n].file = &files[i];
        items[n].order = row && row->has_order ? row->order : legacy_order--;
        items[n].given = i;
        n++;
    }
    qsort(items, n, sizeof(*items), compare_items);

    int rc = SIGIL_OK;
    for (size_t i = 0; i < n && rc == SIGIL_OK; i++) {
        const sigil_ps2_folder_file *f = items[i].file;
        sigil_ps2_file *pf = &out->files[out->file_count];
        char meta_path[PS2_FOLDER_PATH_MAX];
        snprintf(meta_path, sizeof(meta_path), "%s%s", PCSX2_META_PREFIX, f->path);
        const sigil_ps2_folder_file *meta = find_folder_file(files, count, meta_path);
        if (meta && meta->len >= PCSX2_META_MIN) {
            memset(pf->entry, 0, PS2_ENTRY_SIZE);
            memcpy(pf->entry, meta->data, meta->len < PS2_ENTRY_SIZE ? meta->len : PS2_ENTRY_SIZE);
        } else {
            const index_row *row = index_find(&index, f->path);
            default_entry(pf->entry, PCSX2_FILE_MODE, f->path, row && row->has_created ? row->created : 0,
                          row && row->has_modified ? row->modified : 0);
        }
        if (f->len > UINT32_MAX) { rc = SIGIL_ERR_UNSUPPORTED_FORMAT; break; }
        sigil_write_le32(pf->entry + PS2_ENTRY_LENGTH, (uint32_t)f->len);
        sigil_write_le32(pf->entry + PS2_ENTRY_CLUSTER, 0);
        sigil_write_le32(pf->entry + PS2_ENTRY_PARENT_INDEX, 0);
        pf->data = (uint8_t *)malloc(f->len ? f->len : 1);
        if (!pf->data) { rc = SIGIL_ERR_OOM; break; }
        if (f->len) memcpy(pf->data, f->data, f->len);
        out->file_count++;
    }

    if (rc == SIGIL_OK) {
        const sigil_ps2_folder_file *meta = find_folder_file(files, count, PCSX2_META_DIR);
        if (meta && meta->len >= PCSX2_META_MIN) {
            memcpy(out->entry, meta->data, meta->len < PS2_ENTRY_SIZE ? meta->len : PS2_ENTRY_SIZE);
        } else {
            default_entry(out->entry, PCSX2_DIR_MODE, folder, root && root->has_created ? root->created : 0,
                          root && root->has_modified ? root->modified : 0);
        }
        sigil_write_le32(out->entry + PS2_ENTRY_LENGTH, (uint32_t)out->file_count + 2);
        default_entry(out->self, PCSX2_DIR_MODE, ".", 0, 0);
        memset(out->self + PS2_ENTRY_CREATED, 0, PS2_TOD_SIZE);
        memset(out->self + PS2_ENTRY_MODIFIED, 0, PS2_TOD_SIZE);
        default_entry(out->parent, PCSX2_DIR_MODE, "..", 0, 0);
        memset(out->parent + PS2_ENTRY_CREATED, 0, PS2_TOD_SIZE);
        memset(out->parent + PS2_ENTRY_MODIFIED, 0, PS2_TOD_SIZE);
    }
    free(items);
    free(index.rows);
    if (rc != SIGIL_OK) sigil_ps2_save_free(out);
    return rc;
}

int sigil_ps2_card_list_io(const sigil_io *io, sigil_card_listing **out) {
    sigil_ps2_card card;
    int rc = sigil_ps2_card_load(io, &card);
    if (rc != SIGIL_OK) return rc;
    rc = sigil_ps2_card_list(&card, out);
    sigil_ps2_card_free(&card);
    return rc;
}
