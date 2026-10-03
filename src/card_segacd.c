// SPDX-License-Identifier: MPL-2.0
#include "card_segacd.h"
#include <stdlib.h>

/* Format block layout and the ECC scheme follow superctr/buram (MIT), which
 * reverse-engineered them from the Mega CD BIOS. */
#define FORMAT_VOLUME        0x00u
#define FORMAT_UNKNOWN       0x0Bu
#define FORMAT_FREE          0x10u
#define FORMAT_FILES         0x18u
#define FORMAT_SIGNATURE     0x20u
#define FORMAT_SIGNATURE_LEN 32u
#define FORMAT_COPIES        4u

static const uint8_t FORMAT_VOLUME_NAME[SEGACD_NAME_LEN] = "___________";
static const uint8_t FORMAT_UNKNOWN_BYTES[5] = { 0x00, 0x00, 0x00, 0x00, 0x40 };
static const uint8_t SIGNATURE[FORMAT_SIGNATURE_LEN] = "SEGA_CD_ROM\0\1\0\0\0RAM_CARTRIDGE___";

#define ENTRY_SIZE    16u
#define ENTRY_FLAG    11u
#define ENTRY_START   12u
#define ENTRY_BLOCKS  14u

#define UNIT_MAGIC    "SCDU"
#define UNIT_NAME     0x04u
#define UNIT_FLAG     0x0Fu
#define UNIT_BLOCKS   0x10u

#define SEGACD_MAX_FILE (2u * SEGACD_MAX_CART_SIZE)

/* ---- ECC ------------------------------------------------------------------ */

#define CRC_POLY      0x1021u
#define GF8_POLY      0x11Du
#define GF6_POLY      0x43u
#define CODE_LEN      8u
#define CODE_DATA     6u
#define ECC_BUF_LEN   36u

static uint16_t crc16(const uint8_t *p, size_t len) {
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; b++) crc = (uint16_t)((crc & 0x8000u) ? (crc << 1) ^ CRC_POLY : crc << 1);
    }
    return crc;
}

typedef struct {
    unsigned bits;
    unsigned poly;
    uint8_t  inv_alpha_plus_1;
    uint8_t  alpha_pow[CODE_LEN];
} gf_field;

static const gf_field GF8 = { 8, GF8_POLY, 0xF4, { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 } };
static const gf_field GF6 = { 6, GF6_POLY, 0x3E, { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x03, 0x06 } };

static uint8_t gf_mul(const gf_field *f, unsigned a, unsigned b) {
    unsigned r = 0;
    while (b) {
        if (b & 1u) r ^= a;
        b >>= 1;
        a <<= 1;
        if (a & (1u << f->bits)) a ^= f->poly;
    }
    return (uint8_t)r;
}

/* A code word c0..c7 is valid when sum(c_i) = 0 and sum(c_i * a^(7-i)) = 0,
 * the two syndromes buram's decoder checks; c6 and c7 are the parity. */
static void syndromes(const gf_field *f, const uint8_t c[CODE_LEN], unsigned n, uint8_t *s0, uint8_t *s1) {
    *s0 = 0;
    *s1 = 0;
    for (unsigned i = 0; i < n; i++) {
        *s0 ^= c[i];
        *s1 ^= gf_mul(f, c[i], f->alpha_pow[CODE_LEN - 1 - i]);
    }
}

static void rs_encode(const gf_field *f, uint8_t c[CODE_LEN]) {
    uint8_t a, b;
    syndromes(f, c, CODE_DATA, &a, &b);
    c[6] = gf_mul(f, (uint8_t)(a ^ b), f->inv_alpha_plus_1);
    c[7] = (uint8_t)(a ^ c[6]);
}

static bool rs_correct(const gf_field *f, uint8_t c[CODE_LEN]) {
    uint8_t s0, s1;
    syndromes(f, c, CODE_LEN, &s0, &s1);
    if (s0 == 0 && s1 == 0) return true;
    if (s0 == 0 || s1 == 0) return false;
    for (unsigned k = 0; k < CODE_LEN; k++) {
        if (gf_mul(f, s0, f->alpha_pow[k]) == s1) {
            c[CODE_LEN - 1 - k] ^= s0;
            return true;
        }
    }
    return false;
}

/* Each 6-bit code word takes one column of the 8x8 byte grid: five bytes a
 * diagonal step of 9 apart, one more from this table, and parity in rows 6
 * and 7, all in the top six bits. */
static const uint8_t RS6_LAST[CODE_LEN] = { 0x2D, 0x2E, 0x2F, 0x08, 0x11, 0x1A, 0x23, 0x2C };

static unsigned rs6_pos(unsigned column, unsigned i) {
    if (i < 5) return column + i * 9;
    if (i == 5) return RS6_LAST[column];
    return (i == 6 ? 0x30u : 0x38u) + column;
}

static void rs6_get(const uint8_t *blk, unsigned column, uint8_t c[CODE_LEN]) {
    for (unsigned i = 0; i < CODE_LEN; i++) c[i] = (uint8_t)(blk[rs6_pos(column, i)] >> 2);
}

static void rs6_put(uint8_t *blk, unsigned column, const uint8_t c[CODE_LEN]) {
    for (unsigned i = 0; i < CODE_LEN; i++) {
        unsigned p = rs6_pos(column, i);
        blk[p] = (uint8_t)((c[i] << 2) | (blk[p] & 3u));
    }
}

/* Each 8-bit code word takes one column's eight bytes as bit planes: symbol j
 * holds bit 7-j of every row, row 0 in the top bit, so the parity symbols
 * land in the low two bits of each byte. */
static void rs8_get(const uint8_t *blk, unsigned column, uint8_t c[CODE_LEN]) {
    for (unsigned j = 0; j < CODE_LEN; j++) {
        uint8_t s = 0;
        for (unsigned r = 0; r < CODE_LEN; r++) s = (uint8_t)((s << 1) | ((blk[column + 8 * r] >> (7 - j)) & 1u));
        c[j] = s;
    }
}

static void rs8_put(uint8_t *blk, unsigned column, const uint8_t c[CODE_LEN]) {
    for (unsigned r = 0; r < CODE_LEN; r++) {
        uint8_t v = 0;
        for (unsigned j = 0; j < CODE_LEN; j++) v = (uint8_t)((v << 1) | ((c[j] >> (7 - r)) & 1u));
        blk[column + 8 * r] = v;
    }
}

void sigil_segacd_encode_block(const uint8_t in[SEGACD_PAYLOAD_SIZE], uint8_t out[SEGACD_BLOCK_SIZE]) {
    uint8_t buf[ECC_BUF_LEN];
    uint16_t crc = crc16(in, SEGACD_PAYLOAD_SIZE);
    buf[0] = (uint8_t)(crc >> 8);
    buf[1] = (uint8_t)crc;
    memcpy(buf + 2, in, SEGACD_PAYLOAD_SIZE);
    buf[34] = (uint8_t)~buf[0];
    buf[35] = (uint8_t)~buf[1];

    memset(out, 0, SEGACD_BLOCK_SIZE);
    for (unsigned g = 0; g < ECC_BUF_LEN / 3; g++) {
        uint32_t v = ((uint32_t)buf[3 * g] << 16) | ((uint32_t)buf[3 * g + 1] << 8) | buf[3 * g + 2];
        for (unsigned k = 0; k < 4; k++) out[4 * g + k] = (uint8_t)(((v >> (18 - 6 * k)) & 0x3Fu) << 2);
    }
    uint8_t c[CODE_LEN];
    for (unsigned column = 0; column < CODE_LEN; column++) {
        rs6_get(out, column, c);
        rs_encode(&GF6, c);
        rs6_put(out, column, c);
    }
    for (unsigned column = 0; column < CODE_LEN; column++) {
        rs8_get(out, column, c);
        rs_encode(&GF8, c);
        rs8_put(out, column, c);
    }
}

bool sigil_segacd_decode_block(const uint8_t in[SEGACD_BLOCK_SIZE], uint8_t out[SEGACD_PAYLOAD_SIZE]) {
    uint8_t blk[SEGACD_BLOCK_SIZE];
    memcpy(blk, in, SEGACD_BLOCK_SIZE);
    uint8_t c[CODE_LEN];
    for (unsigned column = 0; column < CODE_LEN; column++) {
        rs8_get(blk, column, c);
        if (!rs_correct(&GF8, c)) return false;
        rs8_put(blk, column, c);
    }
    for (unsigned column = 0; column < CODE_LEN; column++) {
        rs6_get(blk, column, c);
        if (!rs_correct(&GF6, c)) return false;
        rs6_put(blk, column, c);
    }
    uint8_t buf[ECC_BUF_LEN];
    for (unsigned g = 0; g < ECC_BUF_LEN / 3; g++) {
        uint32_t v = 0;
        for (unsigned k = 0; k < 4; k++) v = (v << 6) | (uint32_t)(blk[4 * g + k] >> 2);
        buf[3 * g] = (uint8_t)(v >> 16);
        buf[3 * g + 1] = (uint8_t)(v >> 8);
        buf[3 * g + 2] = (uint8_t)v;
    }
    uint16_t crc = crc16(buf + 2, SEGACD_PAYLOAD_SIZE);
    uint16_t first = (uint16_t)((buf[0] << 8) | buf[1]);
    uint16_t second = (uint16_t)~((buf[34] << 8) | buf[35]);
    if (crc != first && crc != second) return false;
    memcpy(out, buf + 2, SEGACD_PAYLOAD_SIZE);
    return true;
}

/* ---- Volume --------------------------------------------------------------- */

static bool volume_size_valid(size_t size) {
    for (size_t s = SEGACD_INTERNAL_SIZE; s <= SEGACD_MAX_CART_SIZE; s *= 2) {
        if (s == size) return true;
    }
    return false;
}

static bool signature_at(const uint8_t *buf, size_t len, size_t end, size_t stride) {
    size_t block = SEGACD_BLOCK_SIZE * stride;
    if (end > len || end < block) return false;
    const uint8_t *p = buf + end - block + FORMAT_SIGNATURE * stride + (stride - 1);
    for (size_t i = 0; i < FORMAT_SIGNATURE_LEN; i++) {
        if (p[i * stride] != SIGNATURE[i]) return false;
    }
    return true;
}

static uint32_t volume_blocks(const sigil_segacd_volume *vol) {
    return (uint32_t)(vol->size / SEGACD_BLOCK_SIZE);
}

static uint8_t *format_block(const sigil_segacd_volume *vol) {
    return vol->data + vol->size - SEGACD_BLOCK_SIZE;
}

/* The BIOS writes each count four times; a count stands when three copies agree. */
static int32_t read_count(const uint8_t *p) {
    for (unsigned i = 0; i < 2; i++) {
        unsigned agree = 0;
        for (unsigned j = 0; j < FORMAT_COPIES; j++) {
            if (sigil_read_be16(p + 2 * j) == sigil_read_be16(p + 2 * i)) agree++;
        }
        if (agree >= 3) return sigil_read_be16(p + 2 * i);
    }
    return -1;
}

static void write_count(uint8_t *p, uint32_t v) {
    for (unsigned j = 0; j < FORMAT_COPIES; j++) sigil_write_be16(p + 2 * j, v);
}

static uint32_t dir_blocks(uint32_t files) {
    return (files + 1) / 2;
}

static uint32_t data_end(const sigil_segacd_volume *vol, uint32_t files) {
    return volume_blocks(vol) - 1 - dir_blocks(files);
}

/** The free count the BIOS keeps for `files` saves ending before block
 * `next`: the blocks between the data and the directory, less one held for
 * the next entry's directory block when every directory block is full. */
static uint32_t bios_free(const sigil_segacd_volume *vol, uint32_t files, uint32_t next) {
    uint32_t end = data_end(vol, files);
    uint32_t held = (files & 1u) ? 0 : 1;
    return end >= next + held ? end - next - held : 0;
}

typedef struct {
    uint32_t files;
    uint32_t free;
} volume_counts;

static bool read_counts(const sigil_segacd_volume *vol, volume_counts *c) {
    if (!vol || !vol->data || !volume_size_valid(vol->size)) return false;
    int32_t files = read_count(format_block(vol) + FORMAT_FILES);
    int32_t free_blocks = read_count(format_block(vol) + FORMAT_FREE);
    if (files < 0 || free_blocks < 0) return false;
    if (dir_blocks((uint32_t)files) + 2 > volume_blocks(vol)) return false;
    c->files = (uint32_t)files;
    c->free = (uint32_t)free_blocks;
    return true;
}

static int finish_load(uint8_t *buf, size_t len, size_t offset, size_t size, bool other_first,
                       sigil_segacd_volume *vol) {
    uint8_t *data = (uint8_t *)malloc(size);
    size_t other_size = len - size;
    uint8_t *other = other_size ? (uint8_t *)malloc(other_size) : NULL;
    if (!data || (other_size && !other)) {
        free(data);
        free(other);
        free(buf);
        return SIGIL_ERR_OOM;
    }
    memcpy(data, buf + offset, size);
    if (other_size) memcpy(other, other_first ? buf : buf + size, other_size);
    free(buf);
    vol->data = data;
    vol->size = size;
    vol->other = other;
    vol->other_size = other_size;
    vol->other_first = other_first;
    volume_counts counts;
    if (!read_counts(vol, &counts)) {
        sigil_segacd_volume_free(vol);
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
    return SIGIL_OK;
}

static int load(const sigil_io *io, sigil_segacd_volume *vol, bool cart_part) {
    if (!io || !io->read || !vol) return SIGIL_ERR_INVALID_ARG;
    memset(vol, 0, sizeof(*vol));
    int64_t size = io->size ? io->size(io->ctx) : -1;
    if (size >= 0 && size != (int64_t)SEGACD_PICODRIVE_SIZE && !volume_size_valid((size_t)size) &&
        !(size % 2 == 0 && volume_size_valid((size_t)size / 2))) {
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
    uint8_t *buf = NULL;
    size_t len = 0;
    int rc = sigil_bram_read_all(io, SEGACD_MAX_FILE, &buf, &len);
    if (rc != SIGIL_OK) return rc;

    if (len == SEGACD_PICODRIVE_SIZE && signature_at(buf, len, SEGACD_INTERNAL_SIZE, 1) &&
        signature_at(buf, len, len, 1)) {
        if (cart_part) return finish_load(buf, len, SEGACD_INTERNAL_SIZE, len - SEGACD_INTERNAL_SIZE, true, vol);
        return finish_load(buf, len, 0, SEGACD_INTERNAL_SIZE, false, vol);
    }
    if (!cart_part && volume_size_valid(len) && signature_at(buf, len, len, 1)) {
        return finish_load(buf, len, 0, len, false, vol);
    }
    if (!cart_part && len % 2 == 0 && volume_size_valid(len / 2) && signature_at(buf, len, len, 2) &&
        sigil_bram_collapse(buf, len, &vol->storage)) {
        return finish_load(buf, len / 2, 0, len / 2, false, vol);
    }
    free(buf);
    return SIGIL_ERR_UNSUPPORTED_FORMAT;
}

int sigil_segacd_volume_load(const sigil_io *io, sigil_segacd_volume *vol) {
    return load(io, vol, false);
}

int sigil_segacd_volume_load_cart(const sigil_io *io, sigil_segacd_volume *vol) {
    return load(io, vol, true);
}

void sigil_segacd_volume_free(sigil_segacd_volume *vol) {
    if (!vol) return;
    free(vol->data);
    free(vol->other);
    memset(vol, 0, sizeof(*vol));
}

int sigil_segacd_volume_format(sigil_segacd_volume *vol, size_t size, const sigil_bram_storage *storage) {
    if (!vol || !storage || !volume_size_valid(size)) return SIGIL_ERR_INVALID_ARG;
    memset(vol, 0, sizeof(*vol));
    vol->data = (uint8_t *)calloc(1, size);
    if (!vol->data) return SIGIL_ERR_OOM;
    vol->size = size;
    vol->storage = *storage;
    uint8_t *f = format_block(vol);
    memcpy(f + FORMAT_VOLUME, FORMAT_VOLUME_NAME, SEGACD_NAME_LEN);
    memcpy(f + FORMAT_UNKNOWN, FORMAT_UNKNOWN_BYTES, sizeof(FORMAT_UNKNOWN_BYTES));
    write_count(f + FORMAT_FREE, bios_free(vol, 0, 1));
    write_count(f + FORMAT_FILES, 0);
    memcpy(f + FORMAT_SIGNATURE, SIGNATURE, FORMAT_SIGNATURE_LEN);
    return SIGIL_OK;
}

int sigil_segacd_volume_write(const sigil_segacd_volume *vol, uint8_t **out, size_t *len) {
    if (!vol || !vol->data || !out || !len) return SIGIL_ERR_INVALID_ARG;
    uint8_t *stored = NULL;
    size_t stored_len = 0;
    int rc = sigil_bram_store(vol->data, vol->size, &vol->storage, &stored, &stored_len);
    if (rc != SIGIL_OK || vol->other_size == 0) {
        *out = stored;
        *len = stored_len;
        return rc;
    }
    uint8_t *file = (uint8_t *)malloc(stored_len + vol->other_size);
    if (!file) { free(stored); return SIGIL_ERR_OOM; }
    if (vol->other_first) {
        memcpy(file, vol->other, vol->other_size);
        memcpy(file + vol->other_size, stored, stored_len);
    } else {
        memcpy(file, stored, stored_len);
        memcpy(file + stored_len, vol->other, vol->other_size);
    }
    free(stored);
    *out = file;
    *len = stored_len + vol->other_size;
    return SIGIL_OK;
}

/* ---- Directory ------------------------------------------------------------ */

typedef struct {
    uint8_t  raw[ENTRY_SIZE];
    uint32_t start;
    uint32_t blocks;
    bool     protect;
} dir_entry;

static uint8_t *slot_block(const sigil_segacd_volume *vol, uint32_t slot) {
    return vol->data + (size_t)(volume_blocks(vol) - 2 - slot / 2) * SEGACD_BLOCK_SIZE;
}

static uint32_t slot_half(uint32_t slot) {
    return (slot & 1u) ? 0 : ENTRY_SIZE;
}

static const uint8_t *data_block(const sigil_segacd_volume *vol, uint32_t block) {
    return vol->data + (size_t)block * SEGACD_BLOCK_SIZE;
}

static bool read_slot(const sigil_segacd_volume *vol, uint32_t slot, dir_entry *e) {
    uint8_t payload[SEGACD_PAYLOAD_SIZE];
    if (!sigil_segacd_decode_block(slot_block(vol, slot), payload)) return false;
    memcpy(e->raw, payload + slot_half(slot), ENTRY_SIZE);
    e->protect = e->raw[ENTRY_FLAG] != 0;
    e->start = sigil_read_be16(e->raw + ENTRY_START);
    e->blocks = sigil_read_be16(e->raw + ENTRY_BLOCKS);
    return true;
}

static void write_slot(sigil_segacd_volume *vol, uint32_t slot, const uint8_t raw[ENTRY_SIZE]) {
    uint8_t payload[SEGACD_PAYLOAD_SIZE];
    if (!(slot & 1u) || !sigil_segacd_decode_block(slot_block(vol, slot), payload)) {
        memset(payload, 0, sizeof(payload));
    }
    memcpy(payload + slot_half(slot), raw, ENTRY_SIZE);
    sigil_segacd_encode_block(payload, slot_block(vol, slot));
}

static bool blocks_decode(const sigil_segacd_volume *vol, uint32_t start, uint32_t blocks) {
    uint8_t payload[SEGACD_PAYLOAD_SIZE];
    for (uint32_t i = 0; i < blocks; i++) {
        if (!sigil_segacd_decode_block(data_block(vol, start + i), payload)) return false;
    }
    return true;
}

static bool entry_valid(const sigil_segacd_volume *vol, const volume_counts *c, const dir_entry *e) {
    if (e->start < 1 || e->blocks < 1 || e->start + e->blocks > data_end(vol, c->files)) return false;
    return !e->protect || blocks_decode(vol, e->start, e->blocks);
}

/** Reads every directory entry, requiring each to decode and the saves to
 * sit back to back from block 1 in directory order, as the BIOS keeps them,
 * and sets `c->free` from the entries rather than the stored count.
 * `entries` is malloc'd; the caller frees it. */
static int read_directory(const sigil_segacd_volume *vol, volume_counts *c, dir_entry **entries) {
    *entries = NULL;
    if (!read_counts(vol, c)) return SIGIL_ERR_INVALID_ARG;
    dir_entry *list = (dir_entry *)malloc(((size_t)c->files + 1) * sizeof(dir_entry));
    if (!list) return SIGIL_ERR_OOM;
    uint32_t next = 1;
    for (uint32_t i = 0; i < c->files; i++) {
        if (!read_slot(vol, i, &list[i]) || !entry_valid(vol, c, &list[i]) || list[i].start != next) {
            free(list);
            return SIGIL_ERR_UNSUPPORTED_FORMAT;
        }
        next += list[i].blocks;
    }
    c->free = bios_free(vol, c->files, next);
    *entries = list;
    return SIGIL_OK;
}

static uint32_t count_free_slots(const volume_counts *c) {
    uint32_t left = c->free, files = c->files, slots = 0;
    while (left >= 1 + (files & 1u)) {
        left -= 1 + (files & 1u);
        files++;
        slots++;
    }
    return slots;
}

int sigil_segacd_list(const sigil_segacd_volume *vol, sigil_card_listing **out) {
    if (!out) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    volume_counts c;
    dir_entry *entries = NULL;
    int rc = read_directory(vol, &c, &entries);
    free(entries);
    if (rc == SIGIL_ERR_OOM) return rc;
    if (rc != SIGIL_OK && !read_counts(vol, &c)) return SIGIL_ERR_INVALID_ARG;
    sigil_card_listing *listing = sigil_card_listing_new(SIGIL_CARD_FORMAT_SEGACD_BRAM, c.files);
    if (!listing) return SIGIL_ERR_OOM;
    listing->total_blocks = bios_free(vol, 0, 1);
    listing->free_blocks = c.free;
    listing->free_slots = count_free_slots(&c);
    for (uint32_t i = 0; i < c.files; i++) {
        dir_entry e;
        if (!read_slot(vol, i, &e)) {
            sigil_card_listing_corrupt(listing, NULL, NULL, 0);
            continue;
        }
        char name[SEGACD_NAME_LEN + 1];
        size_t n = 0;
        while (n < SEGACD_NAME_LEN && e.raw[n] != 0) n++;
        memcpy(name, e.raw, n);
        name[n] = '\0';
        if (!entry_valid(vol, &c, &e)) {
            sigil_card_listing_corrupt(listing, name, NULL, e.start);
            continue;
        }
        sigil_card_entry *entry = &listing->entries[listing->entry_count++];
        memcpy(entry->name, name, n + 1);
        entry->blocks = e.blocks;
        entry->first_block = e.start;
    }
    *out = listing;
    return SIGIL_OK;
}

static bool find_slot(const sigil_segacd_volume *vol, uint32_t first_block, dir_entry *e) {
    volume_counts c;
    if (!read_counts(vol, &c)) return false;
    for (uint32_t i = 0; i < c.files; i++) {
        if (read_slot(vol, i, e) && e->start == first_block && entry_valid(vol, &c, e)) return true;
    }
    return false;
}

int sigil_segacd_entry_data(const sigil_segacd_volume *vol, uint32_t first_block,
                            uint8_t **out, size_t *len) {
    if (!out || !len) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    *len = 0;
    dir_entry e;
    if (!find_slot(vol, first_block, &e)) return SIGIL_ERR_INVALID_ARG;
    size_t per_block = e.protect ? SEGACD_PAYLOAD_SIZE : SEGACD_BLOCK_SIZE;
    uint8_t *data = (uint8_t *)malloc((size_t)e.blocks * per_block);
    if (!data) return SIGIL_ERR_OOM;
    for (uint32_t i = 0; i < e.blocks; i++) {
        const uint8_t *src = data_block(vol, e.start + i);
        uint8_t *dst = data + (size_t)i * per_block;
        if (!e.protect) {
            memcpy(dst, src, SEGACD_BLOCK_SIZE);
        } else if (!sigil_segacd_decode_block(src, dst)) {
            free(data);
            return SIGIL_ERR_INVALID_ARG;
        }
    }
    *out = data;
    *len = (size_t)e.blocks * per_block;
    return SIGIL_OK;
}

int sigil_segacd_card_list_io(const sigil_io *io, sigil_card_listing **out) {
    if (!out) return SIGIL_ERR_INVALID_ARG;
    sigil_segacd_volume vol;
    int rc = sigil_segacd_volume_load(io, &vol);
    if (rc == SIGIL_OK) rc = sigil_segacd_list(&vol, out);
    sigil_segacd_volume_free(&vol);
    return rc;
}

/* ---- Units ---------------------------------------------------------------- */

size_t sigil_segacd_unit_size(uint32_t blocks) {
    return SEGACD_UNIT_HEADER_SIZE + (size_t)blocks * SEGACD_BLOCK_SIZE;
}

int sigil_segacd_extract(const sigil_segacd_volume *vol, uint32_t first_block,
                         uint32_t blocks, uint8_t *out) {
    if (!out) return SIGIL_ERR_INVALID_ARG;
    dir_entry e;
    if (!find_slot(vol, first_block, &e) || e.blocks != blocks) return SIGIL_ERR_INVALID_ARG;
    memset(out, 0, SEGACD_UNIT_HEADER_SIZE);
    memcpy(out, UNIT_MAGIC, 4);
    memcpy(out + UNIT_NAME, e.raw, SEGACD_NAME_LEN);
    out[UNIT_FLAG] = e.raw[ENTRY_FLAG];
    sigil_write_be16(out + UNIT_BLOCKS, e.blocks);
    memcpy(out + SEGACD_UNIT_HEADER_SIZE, data_block(vol, e.start), (size_t)e.blocks * SEGACD_BLOCK_SIZE);
    return SIGIL_OK;
}

static uint32_t unit_blocks(const uint8_t *unit, size_t len) {
    if (!unit || len < sigil_segacd_unit_size(1) || memcmp(unit, UNIT_MAGIC, 4) != 0) return 0;
    uint32_t blocks = sigil_read_be16(unit + UNIT_BLOCKS);
    if (blocks == 0 || len != sigil_segacd_unit_size(blocks)) return 0;
    if (unit[UNIT_FLAG] != 0) {
        uint8_t payload[SEGACD_PAYLOAD_SIZE];
        for (uint32_t i = 0; i < blocks; i++) {
            if (!sigil_segacd_decode_block(unit + sigil_segacd_unit_size(i), payload)) return 0;
        }
    }
    return blocks;
}

/* A new save takes its blocks and, when the file count is odd, a new
 * directory block: each directory block holds two entries. */
static uint32_t inject_cost(uint32_t blocks, uint32_t files) {
    return blocks + (files & 1u);
}

uint32_t sigil_segacd_cost(const sigil_segacd_volume *vol, const uint8_t *unit, size_t len) {
    uint32_t blocks = unit_blocks(unit, len);
    volume_counts c;
    if (!vol || !vol->data || blocks == 0 || !read_counts(vol, &c)) return 0;
    return inject_cost(blocks, c.files);
}

int sigil_segacd_inject(sigil_segacd_volume *vol, const uint8_t *unit, size_t len) {
    if (!vol || !vol->data) return SIGIL_ERR_INVALID_ARG;
    uint32_t blocks = unit_blocks(unit, len);
    if (blocks == 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    volume_counts c;
    dir_entry *entries = NULL;
    int rc = read_directory(vol, &c, &entries);
    if (rc != SIGIL_OK) return rc == SIGIL_ERR_OOM ? rc : SIGIL_ERR_UNSUPPORTED_FORMAT;

    uint32_t start = 1;
    for (uint32_t i = 0; i < c.files; i++) {
        if (memcmp(entries[i].raw, unit + UNIT_NAME, SEGACD_NAME_LEN) == 0) rc = SIGIL_ERR_EXISTS;
        start = entries[i].start + entries[i].blocks;
    }
    free(entries);
    if (rc != SIGIL_OK) return rc;
    if (c.free < inject_cost(blocks, c.files)) return SIGIL_ERR_NO_SPACE;

    memcpy(vol->data + (size_t)start * SEGACD_BLOCK_SIZE, unit + SEGACD_UNIT_HEADER_SIZE,
           (size_t)blocks * SEGACD_BLOCK_SIZE);
    uint8_t raw[ENTRY_SIZE];
    memcpy(raw, unit + UNIT_NAME, SEGACD_NAME_LEN);
    raw[ENTRY_FLAG] = unit[UNIT_FLAG];
    sigil_write_be16(raw + ENTRY_START, start);
    sigil_write_be16(raw + ENTRY_BLOCKS, blocks);
    write_slot(vol, c.files, raw);
    write_count(format_block(vol) + FORMAT_FREE, bios_free(vol, c.files + 1, start + blocks));
    write_count(format_block(vol) + FORMAT_FILES, c.files + 1);
    return SIGIL_OK;
}

int sigil_segacd_delete(sigil_segacd_volume *vol, uint32_t first_block) {
    if (!vol || !vol->data) return SIGIL_ERR_INVALID_ARG;
    volume_counts c;
    dir_entry *entries = NULL;
    int rc = read_directory(vol, &c, &entries);
    if (rc != SIGIL_OK) return rc;
    uint32_t k = 0;
    while (k < c.files && entries[k].start != first_block) k++;
    if (k == c.files) { free(entries); return SIGIL_ERR_INVALID_ARG; }

    uint32_t gap = entries[k].blocks;
    uint32_t end = entries[c.files - 1].start + entries[c.files - 1].blocks;
    for (uint32_t j = k + 1; j < c.files; j++) {
        dir_entry *e = &entries[j];
        memmove(vol->data + (size_t)(e->start - gap) * SEGACD_BLOCK_SIZE,
                vol->data + (size_t)e->start * SEGACD_BLOCK_SIZE, (size_t)e->blocks * SEGACD_BLOCK_SIZE);
        sigil_write_be16(e->raw + ENTRY_START, e->start - gap);
        write_slot(vol, j - 1, e->raw);
    }
    memset(vol->data + (size_t)(end - gap) * SEGACD_BLOCK_SIZE, 0, (size_t)gap * SEGACD_BLOCK_SIZE);
    uint32_t last = c.files - 1;
    if (last & 1u) {
        static const uint8_t cleared[ENTRY_SIZE] = { 0 };
        write_slot(vol, last, cleared);
    } else {
        memset(slot_block(vol, last), 0, SEGACD_BLOCK_SIZE);
    }
    write_count(format_block(vol) + FORMAT_FREE, bios_free(vol, last, end - gap));
    write_count(format_block(vol) + FORMAT_FILES, last);
    free(entries);
    return SIGIL_OK;
}

int sigil_segacd_verify(const sigil_segacd_volume *vol, const uint8_t *unit, size_t len) {
    if (!vol || !vol->data) return SIGIL_ERR_INVALID_ARG;
    uint32_t blocks = unit_blocks(unit, len);
    if (blocks == 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    volume_counts c;
    if (!read_counts(vol, &c)) return SIGIL_ERR_INVALID_ARG;
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return SIGIL_ERR_OOM;
    int rc = SIGIL_ERR_NOT_FOUND;
    for (uint32_t i = 0; i < c.files && rc != SIGIL_OK; i++) {
        dir_entry e;
        if (!read_slot(vol, i, &e) || memcmp(e.raw, unit + UNIT_NAME, SEGACD_NAME_LEN) != 0) continue;
        if (e.blocks != blocks) continue;
        if (sigil_segacd_extract(vol, e.start, blocks, copy) == SIGIL_OK && memcmp(copy, unit, len) == 0) {
            rc = SIGIL_OK;
        }
    }
    free(copy);
    return rc;
}
