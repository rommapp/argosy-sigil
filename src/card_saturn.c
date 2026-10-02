// SPDX-License-Identifier: MPL-2.0
#include "card_saturn.h"
#include <stdlib.h>
#include <zlib.h>

#define SATURN_MAGIC            "BackUpRam Format"
#define SATURN_MAGIC_LEN        16u
#define SATURN_ARCHIVE_MARK     0x80000000u
#define SATURN_BLOCK_HEADER     4u
#define SATURN_FIELDS_LEN       30u
#define SATURN_LIST_OFFSET      (SATURN_BLOCK_HEADER + SATURN_FIELDS_LEN)
#define SATURN_RESERVED_BLOCKS  2u
#define SATURN_MAX_FILE         (2u * SATURN_YABASANSHIRO_SIZE)
#define SATURN_MAX_GZIP         (SATURN_MAX_FILE + SATURN_MAX_FILE / 16u)

#define ARCHIVE_NAME     0x04u
#define ARCHIVE_LANGUAGE 0x0Fu
#define ARCHIVE_COMMENT  0x10u
#define ARCHIVE_DATE     0x1Au
#define ARCHIVE_SIZE     0x1Eu

/* .BUP header offsets, per slinga-homebrew Save-Game-BUP-Scripts bup_header.h. */
#define BUP_MAGIC        "Vmem"
#define BUP_NAME         0x10u
#define BUP_COMMENT      0x1Cu
#define BUP_LANGUAGE     0x27u
#define BUP_DATE         0x28u
#define BUP_SIZE         0x2Cu
#define BUP_BLOCKS       0x30u
#define BUP_DATE_2       0x34u

#define GZIP_HEADER_SIZE  10u
#define GZIP_TRAILER_SIZE 8u
#define GZIP_FLAGS        3u
#define READ_CHUNK        (64u * 1024u)

static bool grow(uint8_t **buf, size_t *room, size_t need, size_t cap) {
    if (need <= *room) return true;
    size_t next = *room ? *room : READ_CHUNK;
    while (next < need) next *= 2;
    if (next > cap) next = cap;
    if (next < need) return false;
    uint8_t *bigger = (uint8_t *)realloc(*buf, next);
    if (!bigger) return false;
    *buf = bigger;
    *room = next;
    return true;
}

int sigil_bram_read_all(const sigil_io *io, size_t cap, uint8_t **out, size_t *len) {
    if (!io || !io->read || !out || !len) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    *len = 0;
    int64_t size = io->size ? io->size(io->ctx) : -1;
    if (size > (int64_t)cap) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    uint8_t *buf = NULL;
    size_t room = 0, have = 0;
    for (;;) {
        if (have > cap) { free(buf); return SIGIL_ERR_UNSUPPORTED_FORMAT; }
        if (!grow(&buf, &room, have + 1, cap + 1)) { free(buf); return SIGIL_ERR_OOM; }
        int n = io->read(io->ctx, have, buf + have, room - have);
        if (n < 0) { free(buf); return SIGIL_ERR_IO; }
        if (n == 0) break;
        have += (size_t)n;
    }
    *out = buf;
    *len = have;
    return SIGIL_OK;
}

bool sigil_bram_collapse(uint8_t *buf, size_t len, sigil_bram_storage *storage) {
    if (!buf || !storage || len < 2 || len % 2 != 0) return false;
    bool constant = true, repeated = true;
    for (size_t i = 0; i < len && (constant || repeated); i += 2) {
        if (buf[i] != buf[0]) constant = false;
        if (buf[i] != buf[i + 1]) repeated = false;
    }
    if (!constant && !repeated) return false;
    storage->expanded = true;
    storage->filler = constant ? buf[0] : -1;
    for (size_t i = 0; i < len / 2; i++) buf[i] = buf[2 * i + 1];
    return true;
}

static int gzip_wrap(const uint8_t *in, size_t len, const uint8_t header[GZIP_HEADER_SIZE],
                     uint8_t **out, size_t *out_len) {
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return SIGIL_ERR_OOM;
    }
    size_t bound = deflateBound(&z, (uLong)len);
    uint8_t *buf = (uint8_t *)malloc(GZIP_HEADER_SIZE + bound + GZIP_TRAILER_SIZE);
    if (!buf) { deflateEnd(&z); return SIGIL_ERR_OOM; }
    memcpy(buf, header, GZIP_HEADER_SIZE);
    buf[GZIP_FLAGS] = 0;
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)len;
    z.next_out = buf + GZIP_HEADER_SIZE;
    z.avail_out = (uInt)bound;
    int zrc = deflate(&z, Z_FINISH);
    size_t body = (size_t)z.total_out;
    deflateEnd(&z);
    if (zrc != Z_STREAM_END) { free(buf); return SIGIL_ERR_IO; }
    uint8_t *trailer = buf + GZIP_HEADER_SIZE + body;
    sigil_write_le32(trailer, (uint32_t)crc32(crc32(0L, Z_NULL, 0), in, (uInt)len));
    sigil_write_le32(trailer + 4, (uint32_t)len);
    *out = buf;
    *out_len = GZIP_HEADER_SIZE + body + GZIP_TRAILER_SIZE;
    return SIGIL_OK;
}

int sigil_bram_store(const uint8_t *volume, size_t len, const sigil_bram_storage *storage,
                     uint8_t **out, size_t *out_len) {
    if (!volume || !storage || !out || !out_len) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    *out_len = 0;
    size_t flat_len = storage->expanded ? len * 2 : len;
    uint8_t *flat = (uint8_t *)malloc(flat_len ? flat_len : 1);
    if (!flat) return SIGIL_ERR_OOM;
    if (storage->expanded) {
        for (size_t i = 0; i < len; i++) {
            flat[2 * i] = storage->filler < 0 ? volume[i] : (uint8_t)storage->filler;
            flat[2 * i + 1] = volume[i];
        }
    } else {
        memcpy(flat, volume, len);
    }
    if (!storage->gzip) {
        *out = flat;
        *out_len = flat_len;
        return SIGIL_OK;
    }
    int rc = gzip_wrap(flat, flat_len, storage->gzip_header, out, out_len);
    free(flat);
    return rc;
}

static int gunzip_all(const uint8_t *in, size_t in_len, size_t cap, uint8_t **out, size_t *out_len) {
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return SIGIL_ERR_OOM;
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)in_len;
    uint8_t *buf = NULL;
    size_t room = 0, have = 0;
    int rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
    for (;;) {
        if (!grow(&buf, &room, have + 1, cap + 1)) {
            rc = room > cap ? SIGIL_ERR_UNSUPPORTED_FORMAT : SIGIL_ERR_OOM;
            break;
        }
        z.next_out = buf + have;
        z.avail_out = (uInt)(room - have);
        int zrc = inflate(&z, Z_NO_FLUSH);
        have = room - z.avail_out;
        if (zrc == Z_STREAM_END) {
            rc = (z.avail_in == 0 && have <= cap) ? SIGIL_OK : SIGIL_ERR_UNSUPPORTED_FORMAT;
            break;
        }
        if (zrc != Z_OK || have > cap) break;
        if (z.avail_out != 0 && z.avail_in == 0) break;
    }
    inflateEnd(&z);
    if (rc != SIGIL_OK) { free(buf); return rc; }
    *out = buf;
    *out_len = have;
    return SIGIL_OK;
}

static bool magic_at(const uint8_t *buf, size_t len, size_t stride) {
    if (len < SATURN_MAGIC_LEN * stride) return false;
    for (size_t i = 0; i < SATURN_MAGIC_LEN; i++) {
        if (buf[i * stride + stride - 1] != (uint8_t)SATURN_MAGIC[i]) return false;
    }
    return true;
}

enum { AS_ANY, AS_CART, AS_INTERNAL };

/* Block sizes the BIOS gives each device: 64 bytes for internal memory of
 * any size, 512 for backup carts up to 16 Mbit and 1024 for 32 Mbit (Kronos
 * bios.c GetDeviceStats). A 4 MiB file is Yaba Sanshiro's internal volume
 * unless read as a cart. */
static uint32_t block_size_for(size_t size, int as) {
    bool internal = size == SATURN_INTERNAL_SIZE || size == SATURN_YABASANSHIRO_SIZE;
    bool cart = size == SATURN_CART_SIZE || size == 2u * SATURN_CART_SIZE || size == 4u * SATURN_CART_SIZE ||
                size == 8u * SATURN_CART_SIZE;
    if (as == AS_CART) return !cart ? 0 : size == 8u * SATURN_CART_SIZE ? 1024u : 512u;
    if (internal) return 64u;
    return as == AS_ANY && cart ? 512u : 0;
}

static int read_file(const sigil_io *io, sigil_saturn_volume *vol, uint8_t **buf, size_t *len) {
    uint8_t head[2];
    int n = io->read(io->ctx, 0, head, sizeof(head));
    if (n < 0) return SIGIL_ERR_IO;
    if (n < 2 || head[0] != 0x1F || head[1] != 0x8B) return sigil_bram_read_all(io, SATURN_MAX_FILE, buf, len);

    uint8_t *packed = NULL;
    size_t packed_len = 0;
    int rc = sigil_bram_read_all(io, SATURN_MAX_GZIP, &packed, &packed_len);
    if (rc != SIGIL_OK) return rc;
    if (packed_len < GZIP_HEADER_SIZE) { free(packed); return SIGIL_ERR_UNSUPPORTED_FORMAT; }
    vol->storage.gzip = true;
    memcpy(vol->storage.gzip_header, packed, GZIP_HEADER_SIZE);
    rc = gunzip_all(packed, packed_len, SATURN_MAX_FILE, buf, len);
    free(packed);
    return rc;
}

static int load_as(const sigil_io *io, sigil_saturn_volume *vol, int as) {
    if (!io || !io->read || !vol) return SIGIL_ERR_INVALID_ARG;
    memset(vol, 0, sizeof(*vol));

    uint8_t head[2 * SATURN_MAGIC_LEN];
    int n = io->read(io->ctx, 0, head, sizeof(head));
    if (n < 0) return SIGIL_ERR_IO;
    bool gzip = n >= 2 && head[0] == 0x1F && head[1] == 0x8B;
    if (!gzip && !magic_at(head, (size_t)n, 1) && !magic_at(head, (size_t)n, 2)) {
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }

    uint8_t *buf = NULL;
    size_t len = 0;
    int rc = read_file(io, vol, &buf, &len);
    if (rc != SIGIL_OK) return rc;
    if (magic_at(buf, len, 2)) {
        if (!sigil_bram_collapse(buf, len, &vol->storage)) { free(buf); return SIGIL_ERR_UNSUPPORTED_FORMAT; }
        len /= 2;
    }
    uint32_t block_size = block_size_for(len, as);
    if (!magic_at(buf, len, 1) || block_size == 0) { free(buf); return SIGIL_ERR_UNSUPPORTED_FORMAT; }
    vol->data = buf;
    vol->size = len;
    vol->block_size = block_size;
    return SIGIL_OK;
}

int sigil_saturn_volume_load(const sigil_io *io, sigil_saturn_volume *vol) { return load_as(io, vol, AS_ANY); }
int sigil_saturn_volume_load_cart(const sigil_io *io, sigil_saturn_volume *vol) { return load_as(io, vol, AS_CART); }
int sigil_saturn_volume_load_internal(const sigil_io *io, sigil_saturn_volume *vol) { return load_as(io, vol, AS_INTERNAL); }

void sigil_saturn_volume_free(sigil_saturn_volume *vol) {
    if (!vol) return;
    free(vol->data);
    vol->data = NULL;
    vol->size = 0;
}

static int format_as(sigil_saturn_volume *vol, size_t size, const sigil_bram_storage *storage, int as) {
    uint32_t block_size = block_size_for(size, as);
    if (!vol || !storage || block_size == 0) return SIGIL_ERR_INVALID_ARG;
    uint8_t *data = (uint8_t *)calloc(1, size);
    if (!data) return SIGIL_ERR_OOM;
    for (uint32_t i = 0; i < block_size; i++) data[i] = (uint8_t)SATURN_MAGIC[i % SATURN_MAGIC_LEN];
    vol->data = data;
    vol->size = size;
    vol->block_size = block_size;
    vol->storage = *storage;
    return SIGIL_OK;
}

int sigil_saturn_volume_format(sigil_saturn_volume *vol, size_t size, const sigil_bram_storage *storage) {
    return format_as(vol, size, storage, AS_ANY);
}

int sigil_saturn_volume_format_cart(sigil_saturn_volume *vol, size_t size, const sigil_bram_storage *storage) {
    return format_as(vol, size, storage, AS_CART);
}

int sigil_saturn_volume_write(const sigil_saturn_volume *vol, uint8_t **out, size_t *len) {
    if (!vol || !vol->data) return SIGIL_ERR_INVALID_ARG;
    return sigil_bram_store(vol->data, vol->size, &vol->storage, out, len);
}

static uint32_t volume_blocks(const sigil_saturn_volume *vol) {
    return (uint32_t)(vol->size / vol->block_size);
}

static const uint8_t *block_at(const sigil_saturn_volume *vol, uint32_t block) {
    return vol->data + (size_t)block * vol->block_size;
}

static uint8_t *block_mut(sigil_saturn_volume *vol, uint32_t block) {
    return vol->data + (size_t)block * vol->block_size;
}

/* Reads a save's byte stream: the archive block from its block list on, then
 * each listed block past its header, in list order. The list is read from the
 * same stream, so `listed` grows while the list is being parsed. */
typedef struct {
    const sigil_saturn_volume *vol;
    uint32_t                   archive;
    const uint16_t            *list;
    uint32_t                   listed;
    uint32_t                   link;
    uint32_t                   off;
} saturn_cursor;

static saturn_cursor cursor_start(const sigil_saturn_volume *vol, uint32_t archive,
                                  const uint16_t *list, uint32_t listed) {
    saturn_cursor c = { vol, archive, list, listed, 0, SATURN_LIST_OFFSET };
    return c;
}

static bool cursor_read(saturn_cursor *c, uint8_t *dst, size_t n) {
    uint32_t bs = c->vol->block_size;
    while (n > 0) {
        if (c->off == bs) {
            if (c->link >= c->listed) return false;
            c->link++;
            c->off = SATURN_BLOCK_HEADER;
        }
        size_t take = bs - c->off;
        if (take > n) take = n;
        if (dst) {
            uint32_t block = c->link == 0 ? c->archive : c->list[c->link - 1];
            memcpy(dst, block_at(c->vol, block) + c->off, take);
            dst += take;
        }
        c->off += (uint32_t)take;
        n -= take;
    }
    return true;
}

static uint64_t cursor_left(const saturn_cursor *c) {
    uint32_t bs = c->vol->block_size;
    return (uint64_t)(bs - c->off) + (uint64_t)(c->listed - c->link) * (bs - SATURN_BLOCK_HEADER);
}

/** Blocks a save of `size` bytes takes: the fields, one list slot per block
 * (the archive block's slot is the terminator) and the data. */
static uint64_t blocks_needed(uint32_t block_size, uint32_t size) {
    uint64_t per_block = block_size - SATURN_BLOCK_HEADER - 2u;
    return (SATURN_FIELDS_LEN + (uint64_t)size + per_block - 1) / per_block;
}

/** Per-volume scan state: which save owns each block, and a list buffer. */
typedef struct {
    uint32_t *owner;
    uint16_t *list;
    uint32_t  blocks;
    uint32_t  corrupt;
} saturn_map;

static int map_open(const sigil_saturn_volume *vol, saturn_map *m) {
    memset(m, 0, sizeof(*m));
    m->blocks = volume_blocks(vol);
    m->owner = (uint32_t *)calloc(m->blocks, sizeof(uint32_t));
    m->list = (uint16_t *)malloc((size_t)m->blocks * sizeof(uint16_t));
    if (!m->owner || !m->list) {
        free(m->owner);
        free(m->list);
        return SIGIL_ERR_OOM;
    }
    return SIGIL_OK;
}

static void map_close(saturn_map *m) {
    free(m->owner);
    free(m->list);
}

static bool parse_entry(const sigil_saturn_volume *vol, saturn_map *m, uint32_t archive,
                        uint32_t *listed, uint32_t *size) {
    uint32_t mark = archive + 1;
    m->owner[archive] = mark;
    saturn_cursor c = cursor_start(vol, archive, m->list, 0);
    bool ok = true;
    for (;;) {
        uint8_t raw[2];
        if (!cursor_read(&c, raw, sizeof(raw))) { ok = false; break; }
        uint16_t block = sigil_read_be16(raw);
        if (block == 0) break;
        if (block < SATURN_RESERVED_BLOCKS || block >= m->blocks || m->owner[block] != 0 ||
            sigil_read_be32(block_at(vol, block)) != 0) {
            ok = false;
            break;
        }
        m->owner[block] = mark;
        m->list[c.listed++] = block;
    }
    *size = sigil_read_be32(block_at(vol, archive) + ARCHIVE_SIZE);
    if (ok && cursor_left(&c) < *size) ok = false;
    if (!ok) {
        for (uint32_t i = 0; i < c.listed; i++) m->owner[m->list[i]] = 0;
        return false;
    }
    *listed = c.listed;
    return true;
}

/** Called for each live save in block order; returning false stops the scan
 * with `m->list` still holding that save's block list. */
typedef bool (*saturn_visit)(void *ctx, const sigil_saturn_volume *vol, uint32_t archive,
                             uint32_t listed, uint32_t size);

static void map_scan(const sigil_saturn_volume *vol, saturn_map *m, saturn_visit visit, void *ctx) {
    for (uint32_t block = SATURN_RESERVED_BLOCKS; block < m->blocks; block++) {
        if (sigil_read_be32(block_at(vol, block)) != SATURN_ARCHIVE_MARK) continue;
        uint32_t listed = 0, size = 0;
        if (!parse_entry(vol, m, block, &listed, &size)) {
            m->corrupt++;
            continue;
        }
        if (visit && !visit(ctx, vol, block, listed, size)) return;
    }
}

typedef struct {
    uint32_t target;
    bool     found;
    uint32_t listed;
    uint32_t size;
} find_ctx;

static bool find_visit(void *ctx, const sigil_saturn_volume *vol, uint32_t archive,
                       uint32_t listed, uint32_t size) {
    (void)vol;
    find_ctx *f = (find_ctx *)ctx;
    if (archive != f->target) return true;
    f->found = true;
    f->listed = listed;
    f->size = size;
    return false;
}

/** Scans up to the save whose archive block is `first_block`. SIGIL_ERR_INVALID_ARG
 * when no live save starts there. On success `m->list` holds its block list. */
static int find_entry(const sigil_saturn_volume *vol, saturn_map *m, uint32_t first_block, find_ctx *f) {
    if (!vol || !vol->data) return SIGIL_ERR_INVALID_ARG;
    int rc = map_open(vol, m);
    if (rc != SIGIL_OK) return rc;
    memset(f, 0, sizeof(*f));
    f->target = first_block;
    map_scan(vol, m, find_visit, f);
    if (!f->found) {
        map_close(m);
        return SIGIL_ERR_INVALID_ARG;
    }
    return SIGIL_OK;
}

static bool read_data(const sigil_saturn_volume *vol, uint32_t archive, const uint16_t *list,
                      uint32_t listed, uint32_t size, uint8_t *out) {
    saturn_cursor c = cursor_start(vol, archive, list, listed);
    return cursor_read(&c, NULL, ((size_t)listed + 1) * 2) && cursor_read(&c, out, size);
}

typedef struct {
    sigil_card_listing *listing;
} list_ctx;

static bool list_visit(void *ctx, const sigil_saturn_volume *vol, uint32_t archive,
                       uint32_t listed, uint32_t size) {
    (void)size;
    sigil_card_listing *listing = ((list_ctx *)ctx)->listing;
    sigil_card_entry *e = &listing->entries[listing->entry_count++];
    const uint8_t *name = block_at(vol, archive) + ARCHIVE_NAME;
    size_t n = 0;
    while (n < SATURN_NAME_LEN && name[n] != 0) n++;
    memcpy(e->name, name, n);
    e->name[n] = '\0';
    e->blocks = listed + 1;
    e->first_block = archive;
    return true;
}

int sigil_saturn_list(const sigil_saturn_volume *vol, sigil_card_listing **out) {
    if (!vol || !vol->data || !out) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    uint32_t blocks = volume_blocks(vol);
    size_t archives = 0;
    for (uint32_t block = SATURN_RESERVED_BLOCKS; block < blocks; block++) {
        if (sigil_read_be32(block_at(vol, block)) == SATURN_ARCHIVE_MARK) archives++;
    }
    saturn_map m;
    int rc = map_open(vol, &m);
    if (rc != SIGIL_OK) return rc;
    sigil_card_listing *listing = sigil_card_listing_new(SIGIL_CARD_FORMAT_SATURN_BACKUP, archives);
    if (!listing) { map_close(&m); return SIGIL_ERR_OOM; }

    list_ctx ctx = { listing };
    map_scan(vol, &m, list_visit, &ctx);
    listing->total_blocks = blocks - SATURN_RESERVED_BLOCKS;
    for (uint32_t block = SATURN_RESERVED_BLOCKS; block < blocks; block++) {
        if (m.owner[block] == 0) listing->free_blocks++;
    }
    listing->free_slots = listing->free_blocks;
    listing->corrupt_count = m.corrupt;
    map_close(&m);
    *out = listing;
    return SIGIL_OK;
}

int sigil_saturn_entry_data(const sigil_saturn_volume *vol, uint32_t first_block,
                            uint8_t **out, size_t *len) {
    if (!out || !len) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    *len = 0;
    saturn_map m;
    find_ctx f;
    int rc = find_entry(vol, &m, first_block, &f);
    if (rc != SIGIL_OK) return rc;
    uint8_t *data = (uint8_t *)malloc(f.size ? f.size : 1);
    if (!data) rc = SIGIL_ERR_OOM;
    else if (!read_data(vol, first_block, m.list, f.listed, f.size, data)) rc = SIGIL_ERR_INVALID_ARG;
    map_close(&m);
    if (rc != SIGIL_OK) { free(data); return rc; }
    *out = data;
    *len = f.size;
    return SIGIL_OK;
}

int sigil_saturn_card_list_io(const sigil_io *io, sigil_card_listing **out) {
    if (!out) return SIGIL_ERR_INVALID_ARG;
    sigil_saturn_volume vol;
    int rc = sigil_saturn_volume_load(io, &vol);
    if (rc == SIGIL_OK) rc = sigil_saturn_list(&vol, out);
    sigil_saturn_volume_free(&vol);
    return rc;
}

int sigil_saturn_extract(const sigil_saturn_volume *vol, uint32_t first_block,
                         uint8_t **out, size_t *len) {
    if (!out || !len) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    *len = 0;
    saturn_map m;
    find_ctx f;
    int rc = find_entry(vol, &m, first_block, &f);
    if (rc != SIGIL_OK) return rc;
    size_t total = SATURN_BUP_HEADER_SIZE + (size_t)f.size;
    uint8_t *bup = (uint8_t *)calloc(1, total);
    if (!bup) {
        rc = SIGIL_ERR_OOM;
    } else if (!read_data(vol, first_block, m.list, f.listed, f.size, bup + SATURN_BUP_HEADER_SIZE)) {
        rc = SIGIL_ERR_INVALID_ARG;
    } else {
        const uint8_t *archive = block_at(vol, first_block);
        memcpy(bup, BUP_MAGIC, 4);
        memcpy(bup + BUP_NAME, archive + ARCHIVE_NAME, SATURN_NAME_LEN);
        memcpy(bup + BUP_COMMENT, archive + ARCHIVE_COMMENT, SATURN_COMMENT_LEN);
        bup[BUP_LANGUAGE] = archive[ARCHIVE_LANGUAGE];
        memcpy(bup + BUP_DATE, archive + ARCHIVE_DATE, 4);
        memcpy(bup + BUP_SIZE, archive + ARCHIVE_SIZE, 4);
        sigil_write_be16(bup + BUP_BLOCKS, f.listed + 1);
        memcpy(bup + BUP_DATE_2, archive + ARCHIVE_DATE, 4);
    }
    map_close(&m);
    if (rc != SIGIL_OK) { free(bup); return rc; }
    *out = bup;
    *len = total;
    return SIGIL_OK;
}

void sigil_saturn_bup_md5(const uint8_t *bup, size_t len, char out[33]) {
    sigil_md5 m;
    sigil_md5_init(&m);
    if (len >= SATURN_BUP_HEADER_SIZE) {
        sigil_md5_update(&m, bup + BUP_NAME, SATURN_NAME_LEN);
        sigil_md5_update(&m, bup + BUP_COMMENT, SATURN_COMMENT_LEN);
        sigil_md5_update(&m, bup + BUP_LANGUAGE, 1);
        sigil_md5_update(&m, bup + BUP_SIZE, 4);
        sigil_md5_update(&m, bup + SATURN_BUP_HEADER_SIZE, len - SATURN_BUP_HEADER_SIZE);
    }
    uint8_t digest[16];
    sigil_md5_final(&m, digest);
    sigil_md5_hex(digest, out);
}

static bool bup_valid(const uint8_t *bup, size_t len) {
    if (!bup || len < SATURN_BUP_HEADER_SIZE || memcmp(bup, BUP_MAGIC, 4) != 0) return false;
    if (bup[BUP_NAME] == 0) return false;
    return len - SATURN_BUP_HEADER_SIZE == sigil_read_be32(bup + BUP_SIZE);
}

typedef struct {
    const uint8_t *name;
    bool           found;
} name_ctx;

static bool name_visit(void *ctx, const sigil_saturn_volume *vol, uint32_t archive,
                       uint32_t listed, uint32_t size) {
    (void)listed;
    (void)size;
    name_ctx *n = (name_ctx *)ctx;
    if (memcmp(block_at(vol, archive) + ARCHIVE_NAME, n->name, SATURN_NAME_LEN) != 0) return true;
    n->found = true;
    return false;
}

static void write_entry(sigil_saturn_volume *vol, const uint32_t *chosen, uint32_t count,
                        const uint8_t *stream, size_t stream_len) {
    uint32_t bs = vol->block_size;
    size_t at = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *block = block_mut(vol, chosen[i]);
        memset(block, 0, bs);
        sigil_write_be32(block, i == 0 ? SATURN_ARCHIVE_MARK : 0);
        size_t take = bs - SATURN_BLOCK_HEADER;
        if (take > stream_len - at) take = stream_len - at;
        memcpy(block + SATURN_BLOCK_HEADER, stream + at, take);
        at += take;
    }
}

int sigil_saturn_inject(sigil_saturn_volume *vol, const uint8_t *bup, size_t len) {
    if (!vol || !vol->data) return SIGIL_ERR_INVALID_ARG;
    if (!bup_valid(bup, len)) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    uint32_t size = sigil_read_be32(bup + BUP_SIZE);
    uint64_t need = blocks_needed(vol->block_size, size);

    saturn_map m;
    int rc = map_open(vol, &m);
    if (rc != SIGIL_OK) return rc;
    name_ctx names = { bup + BUP_NAME, false };
    map_scan(vol, &m, name_visit, &names);
    if (names.found) { map_close(&m); return SIGIL_ERR_EXISTS; }

    uint32_t *chosen = (uint32_t *)malloc((size_t)m.blocks * sizeof(uint32_t));
    if (!chosen) { map_close(&m); return SIGIL_ERR_OOM; }
    uint32_t found = 0;
    for (uint32_t block = SATURN_RESERVED_BLOCKS; block < m.blocks && found < need; block++) {
        if (m.owner[block] == 0) chosen[found++] = block;
    }
    map_close(&m);
    if (found < need) { free(chosen); return SIGIL_ERR_NO_SPACE; }

    size_t stream_len = SATURN_FIELDS_LEN + (size_t)need * 2 + size;
    uint8_t *stream = (uint8_t *)calloc(1, stream_len);
    if (!stream) { free(chosen); return SIGIL_ERR_OOM; }
    memcpy(stream + ARCHIVE_NAME - SATURN_BLOCK_HEADER, bup + BUP_NAME, SATURN_NAME_LEN);
    stream[ARCHIVE_LANGUAGE - SATURN_BLOCK_HEADER] = bup[BUP_LANGUAGE];
    memcpy(stream + ARCHIVE_COMMENT - SATURN_BLOCK_HEADER, bup + BUP_COMMENT, SATURN_COMMENT_LEN);
    memcpy(stream + ARCHIVE_DATE - SATURN_BLOCK_HEADER, bup + BUP_DATE, 4);
    memcpy(stream + ARCHIVE_SIZE - SATURN_BLOCK_HEADER, bup + BUP_SIZE, 4);
    for (uint32_t i = 1; i < need; i++) sigil_write_be16(stream + SATURN_FIELDS_LEN + (size_t)(i - 1) * 2, chosen[i]);
    memcpy(stream + SATURN_FIELDS_LEN + (size_t)need * 2, bup + SATURN_BUP_HEADER_SIZE, size);

    write_entry(vol, chosen, (uint32_t)need, stream, stream_len);
    free(stream);
    free(chosen);
    return SIGIL_OK;
}

int sigil_saturn_delete(sigil_saturn_volume *vol, uint32_t first_block) {
    saturn_map m;
    find_ctx f;
    int rc = find_entry(vol, &m, first_block, &f);
    if (rc != SIGIL_OK) return rc;
    memset(block_mut(vol, first_block), 0, vol->block_size);
    for (uint32_t i = 0; i < f.listed; i++) memset(block_mut(vol, m.list[i]), 0, vol->block_size);
    map_close(&m);
    return SIGIL_OK;
}

typedef struct {
    const saturn_map *m;
    const uint8_t    *bup;
    uint8_t          *data;
    int               rc;
} verify_ctx;

static bool verify_visit(void *ctx, const sigil_saturn_volume *vol, uint32_t archive,
                         uint32_t listed, uint32_t size) {
    verify_ctx *v = (verify_ctx *)ctx;
    const uint8_t *a = block_at(vol, archive);
    const uint8_t *b = v->bup;
    if (memcmp(a + ARCHIVE_NAME, b + BUP_NAME, SATURN_NAME_LEN) != 0 ||
        size != sigil_read_be32(b + BUP_SIZE) ||
        a[ARCHIVE_LANGUAGE] != b[BUP_LANGUAGE] ||
        memcmp(a + ARCHIVE_COMMENT, b + BUP_COMMENT, SATURN_COMMENT_LEN) != 0 ||
        memcmp(a + ARCHIVE_DATE, b + BUP_DATE, 4) != 0) {
        return true;
    }
    if (read_data(vol, archive, v->m->list, listed, size, v->data) &&
        memcmp(v->data, b + SATURN_BUP_HEADER_SIZE, size) == 0) {
        v->rc = SIGIL_OK;
        return false;
    }
    return true;
}

int sigil_saturn_verify(const sigil_saturn_volume *vol, const uint8_t *bup, size_t len) {
    if (!vol || !vol->data) return SIGIL_ERR_INVALID_ARG;
    if (!bup_valid(bup, len)) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    saturn_map m;
    int rc = map_open(vol, &m);
    if (rc != SIGIL_OK) return rc;
    uint8_t *data = (uint8_t *)malloc(len);
    if (!data) { map_close(&m); return SIGIL_ERR_OOM; }
    verify_ctx v = { &m, bup, data, SIGIL_ERR_NOT_FOUND };
    map_scan(vol, &m, verify_visit, &v);
    free(data);
    map_close(&m);
    return v.rc;
}
