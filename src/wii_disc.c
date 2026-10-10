// SPDX-License-Identifier: MPL-2.0
/* The title id a Wii disc's game partition carries in its ticket, read
 * through the container the disc is stored in: a plain image, WBFS, or
 * Dolphin's WIA and RVZ. Dolphin names a disc's save folder after this id
 * (VolumeWii::GetTitleID, NandPaths GetTitleDataPath), and a disc that
 * installs a channel carries 00010004 where most carry 00010000.
 *
 * WBFS follows Dolphin's WbfsBlob.cpp. WIA and RVZ follow Dolphin's
 * docs/WiaAndRvz.md and WIABlob.cpp; the partition table and the ticket sit
 * in the raw data the format stores as is, so this reads raw data alone. */
#include "sigil_internal.h"
#include <LzmaDec.h>
#include <stdlib.h>
#include <zstd.h>

#define PARTITION_TABLE        0x40000u
#define PARTITION_GROUPS       4u
#define PARTITION_MAX          64u
#define PARTITION_TYPE_GAME    0u
#define TICKET_TITLE_ID        0x1DCu
#define TITLE_GAME             0x00010000u
#define TITLE_GAME_WITH_CHANNEL 0x00010004u

#define WII_SECTOR_SIZE        0x8000u
#define WII_SECTOR_COUNT       (143432u * 2u)

#define WBFS_HD_SHIFT          8u
#define WBFS_SECTOR_SHIFT      9u
#define WBFS_DISC_TABLE        12u
#define WBFS_DISC_HEADER_SIZE  0x100u

#define WIA_HEAD_SIZE          0x48u
#define WIA_DISC_SIZE          0xDCu
#define WIA_DISC_HEADER_SIZE   0x80u
#define WIA_RAW_ENTRY_SIZE     0x18u
#define WIA_GROUP_SIZE         8u
#define RVZ_GROUP_SIZE         12u
#define WIA_TABLE_MAX          (1u << 20)
#define WIA_CHUNK_MAX          (64u << 20)
#define SHA1_SIZE              20u

enum { DISC_ISO, DISC_WBFS, DISC_WIA, DISC_RVZ };
enum { COMPRESS_NONE, COMPRESS_PURGE, COMPRESS_BZIP2, COMPRESS_LZMA, COMPRESS_LZMA2, COMPRESS_ZSTD };

typedef struct {
    uint64_t offset;
    uint64_t size;
    uint32_t group_index;
    uint32_t group_count;
} raw_entry;

typedef struct {
    uint64_t file_offset;
    uint32_t size;
    bool     compressed;
    uint32_t packed_size;
} group_entry;

typedef struct {
    const sigil_io *io;
    int             kind;
    /* WBFS */
    uint64_t        wbfs_sector;
    unsigned        wbfs_shift;
    uint64_t        wlba_table;
    uint64_t        wlba_count;
    /* WIA and RVZ */
    uint32_t        compression;
    uint32_t        chunk_size;
    uint8_t         disc_header[WIA_DISC_HEADER_SIZE];
    uint8_t         lzma_props[LZMA_PROPS_SIZE];
    bool            has_lzma_props;
    raw_entry      *raws;
    size_t          raw_count;
    group_entry    *groups;
    size_t          group_count;
    uint8_t        *chunk;          /* the last group decoded, and which one */
    size_t          chunk_len;
    size_t          chunk_group;
} disc;

/* ---- RVZ packing -------------------------------------------------------- */

/* Dolphin's LaggedFibonacciGenerator: words kept in output form, each word's
 * bytes emitted most significant first. */
#define LFG_K    521u
#define LFG_J    32u
#define LFG_SEED 17u

typedef struct {
    uint32_t b[LFG_K];
    size_t   pos;
} lfg;

static void lfg_forward(lfg *g) {
    for (size_t i = 0; i < LFG_J; i++) g->b[i] ^= g->b[i + LFG_K - LFG_J];
    for (size_t i = LFG_J; i < LFG_K; i++) g->b[i] ^= g->b[i - LFG_J];
}

static void lfg_seed(lfg *g, const uint8_t seed[LFG_SEED * 4]) {
    for (size_t i = 0; i < LFG_SEED; i++) g->b[i] = sigil_read_be32(seed + i * 4);
    for (size_t i = LFG_SEED; i < LFG_K; i++) g->b[i] = (g->b[i - 17] << 23) ^ (g->b[i - 16] >> 9) ^ g->b[i - 1];
    for (size_t i = 0; i < LFG_K; i++) g->b[i] = (g->b[i] & 0xFF00FFFFu) | ((g->b[i] >> 2) & 0x00FF0000u);
    for (int i = 0; i < 4; i++) lfg_forward(g);
    g->pos = 0;
}

static void lfg_skip(lfg *g, size_t count) {
    g->pos += count;
    while (g->pos >= LFG_K * 4) {
        lfg_forward(g);
        g->pos -= LFG_K * 4;
    }
}

static void lfg_bytes(lfg *g, uint8_t *out, size_t count) {
    for (size_t i = 0; i < count; i++) {
        out[i] = (uint8_t)(g->b[g->pos / 4] >> (24 - 8 * (g->pos % 4)));
        if (++g->pos == LFG_K * 4) {
            lfg_forward(g);
            g->pos = 0;
        }
    }
}

/* Decodes RVZ packing: runs of stored bytes and runs of padding regenerated
 * from a seed. `data_offset` is where the group starts in its data, which
 * places the generator within its 32 KiB block. */
static int rvz_unpack(const uint8_t *in, size_t in_len, uint64_t data_offset, uint8_t *out, size_t out_len) {
    size_t at = 0, done = 0;
    lfg g;
    while (done < out_len) {
        if (in_len - at < 4) return SIGIL_ERR_IO;
        uint32_t size = sigil_read_be32(in + at);
        at += 4;
        bool junk = (size & 0x80000000u) != 0;
        size &= 0x7FFFFFFFu;
        if (size > out_len - done) return SIGIL_ERR_IO;
        if (junk) {
            if (in_len - at < LFG_SEED * 4) return SIGIL_ERR_IO;
            lfg_seed(&g, in + at);
            at += LFG_SEED * 4;
            lfg_skip(&g, (size_t)(data_offset % WII_SECTOR_SIZE));
            lfg_bytes(&g, out + done, size);
        } else {
            if (in_len - at < size) return SIGIL_ERR_IO;
            memcpy(out + done, in + at, size);
            at += size;
        }
        done += size;
        data_offset += size;
    }
    return at == in_len ? SIGIL_OK : SIGIL_ERR_IO;
}

/* ---- WIA and RVZ decompression ------------------------------------------ */

static void *lzma_alloc(ISzAllocPtr p, size_t size) {
    (void)p;
    return malloc(size);
}

static void lzma_free(ISzAllocPtr p, void *address) {
    (void)p;
    free(address);
}

/* PURGE: runs of data at offsets, zeros between, then a SHA-1 of them. */
static int purge_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) {
    if (in_len < SHA1_SIZE) return SIGIL_ERR_IO;
    size_t end = in_len - SHA1_SIZE, at = 0;
    memset(out, 0, out_len);
    while (at < end) {
        if (end - at < 8) return SIGIL_ERR_IO;
        uint32_t offset = sigil_read_be32(in + at);
        uint32_t size = sigil_read_be32(in + at + 4);
        at += 8;
        if (size > end - at || offset > out_len || size > out_len - offset) return SIGIL_ERR_IO;
        memcpy(out + offset, in + at, size);
        at += size;
    }
    return SIGIL_OK;
}

/* `in` decoded by `method` into exactly `out_len` bytes. */
static int decode(const disc *d, uint32_t method, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) {
    switch (method) {
    case COMPRESS_NONE:
        if (in_len < out_len) return SIGIL_ERR_IO;
        memcpy(out, in, out_len);
        return SIGIL_OK;
    case COMPRESS_PURGE:
        return purge_decode(in, in_len, out, out_len);
    case COMPRESS_LZMA: {
        if (!d->has_lzma_props) return SIGIL_ERR_UNSUPPORTED_FORMAT;
        ISzAlloc alloc = { lzma_alloc, lzma_free };
        SizeT dest_len = out_len, src_len = in_len;
        ELzmaStatus status;
        SRes res = LzmaDecode(out, &dest_len, in, &src_len, d->lzma_props, LZMA_PROPS_SIZE, LZMA_FINISH_ANY,
                              &status, &alloc);
        (void)status;
        return res == SZ_OK && dest_len == out_len ? SIGIL_OK : SIGIL_ERR_IO;
    }
    case COMPRESS_ZSTD: {
        size_t n = ZSTD_decompress(out, out_len, in, in_len);
        return !ZSTD_isError(n) && n == out_len ? SIGIL_OK : SIGIL_ERR_IO;
    }
    default:
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
}

/* `in_len` bytes of the file at `offset`, decoded by `method` into `out`. */
static int read_decoded(const disc *d, uint32_t method, uint64_t offset, size_t in_len, uint8_t *out,
                        size_t out_len) {
    uint8_t *in = (uint8_t *)malloc(in_len ? in_len : 1);
    if (!in) return SIGIL_ERR_OOM;
    int rc = sigil_io_read_exact(d->io, offset, in, in_len);
    if (rc == SIGIL_OK) rc = decode(d, method, in, in_len, out, out_len);
    free(in);
    return rc;
}

static int wia_open(disc *d, bool rvz) {
    uint8_t head[WIA_HEAD_SIZE + WIA_DISC_SIZE];
    int rc = sigil_io_read_exact(d->io, 0, head, sizeof(head));
    if (rc != SIGIL_OK) return rc;
    if (sigil_read_be32(head + 0x0C) < WIA_DISC_SIZE - 7) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    const uint8_t *h = head + WIA_HEAD_SIZE;
    d->compression = sigil_read_be32(h + 0x04);
    d->chunk_size = sigil_read_be32(h + 0x0C);
    memcpy(d->disc_header, h + 0x10, WIA_DISC_HEADER_SIZE);
    uint32_t raw_count = sigil_read_be32(h + 0xB4);
    uint64_t raw_offset = sigil_read_be64(h + 0xB8);
    uint32_t raw_size = sigil_read_be32(h + 0xC0);
    uint32_t group_count = sigil_read_be32(h + 0xC4);
    uint64_t group_offset = sigil_read_be64(h + 0xC8);
    uint32_t group_size = sigil_read_be32(h + 0xD0);
    uint8_t props_len = h[0xD4];
    if (d->compression > COMPRESS_ZSTD || (rvz && d->compression == COMPRESS_PURGE) ||
        (!rvz && d->compression == COMPRESS_ZSTD)) {
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
    if (d->compression == COMPRESS_LZMA && props_len == LZMA_PROPS_SIZE) {
        memcpy(d->lzma_props, h + 0xD5, LZMA_PROPS_SIZE);
        d->has_lzma_props = true;
    }
    if (d->chunk_size < WII_SECTOR_SIZE || d->chunk_size > WIA_CHUNK_MAX || d->chunk_size % WII_SECTOR_SIZE ||
        raw_count == 0 || raw_count > WIA_TABLE_MAX || group_count > WIA_TABLE_MAX) {
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }

    size_t raw_len = (size_t)raw_count * WIA_RAW_ENTRY_SIZE;
    size_t entry = rvz ? RVZ_GROUP_SIZE : WIA_GROUP_SIZE;
    size_t groups_len = (size_t)group_count * entry;
    uint8_t *raws = (uint8_t *)malloc(raw_len);
    uint8_t *groups = (uint8_t *)malloc(groups_len ? groups_len : 1);
    d->raws = (raw_entry *)calloc(raw_count, sizeof(raw_entry));
    d->groups = (group_entry *)calloc(group_count ? group_count : 1, sizeof(group_entry));
    rc = raws && groups && d->raws && d->groups ? SIGIL_OK : SIGIL_ERR_OOM;
    if (rc == SIGIL_OK) rc = read_decoded(d, d->compression, raw_offset, raw_size, raws, raw_len);
    if (rc == SIGIL_OK) rc = read_decoded(d, d->compression, group_offset, group_size, groups, groups_len);
    for (size_t i = 0; rc == SIGIL_OK && i < raw_count; i++) {
        const uint8_t *e = raws + i * WIA_RAW_ENTRY_SIZE;
        uint64_t offset = sigil_read_be64(e);
        uint64_t size = sigil_read_be64(e + 8);
        uint64_t skipped = offset % WII_SECTOR_SIZE;
        d->raws[i] = (raw_entry){ offset - skipped, size + skipped, sigil_read_be32(e + 16), sigil_read_be32(e + 20) };
    }
    for (size_t i = 0; rc == SIGIL_OK && i < group_count; i++) {
        const uint8_t *e = groups + i * entry;
        uint32_t size = sigil_read_be32(e + 4);
        group_entry *g = &d->groups[i];
        g->file_offset = (uint64_t)sigil_read_be32(e) << 2;
        g->compressed = !rvz || (size & 0x80000000u);
        g->size = rvz ? size & 0x7FFFFFFFu : size;
        g->packed_size = rvz ? sigil_read_be32(e + 8) : 0;
    }
    d->raw_count = raw_count;
    d->group_count = group_count;
    d->chunk_group = SIZE_MAX;
    free(raws);
    free(groups);
    return rc;
}

/* Decodes group `index`, `len` bytes of disc data starting `data_offset`
 * into its raw data entry, into d->chunk. */
static int wia_load_group(disc *d, size_t index, size_t len, uint64_t data_offset) {
    if (d->chunk_group == index && d->chunk_len == len) return SIGIL_OK;
    if (index >= d->group_count) return SIGIL_ERR_IO;
    const group_entry *g = &d->groups[index];
    uint8_t *out = (uint8_t *)realloc(d->chunk, len);
    if (!out) return SIGIL_ERR_OOM;
    d->chunk = out;
    d->chunk_group = SIZE_MAX;
    if (g->size == 0) {
        memset(out, 0, len);
    } else {
        uint32_t method = g->compressed ? d->compression : COMPRESS_NONE;
        size_t decoded_len = g->packed_size ? g->packed_size : len;
        if (decoded_len > WIA_CHUNK_MAX) return SIGIL_ERR_IO;
        uint8_t *decoded = g->packed_size ? (uint8_t *)malloc(decoded_len) : out;
        if (!decoded) return SIGIL_ERR_OOM;
        int rc = read_decoded(d, method, g->file_offset, g->size, decoded, decoded_len);
        if (rc == SIGIL_OK && g->packed_size) rc = rvz_unpack(decoded, decoded_len, data_offset, out, len);
        if (decoded != out) free(decoded);
        if (rc != SIGIL_OK) return rc;
    }
    d->chunk_group = index;
    d->chunk_len = len;
    return SIGIL_OK;
}

static int wia_read(disc *d, uint64_t offset, uint8_t *buf, size_t len) {
    if (offset < WIA_DISC_HEADER_SIZE) {
        size_t n = (size_t)(WIA_DISC_HEADER_SIZE - offset) < len ? (size_t)(WIA_DISC_HEADER_SIZE - offset) : len;
        memcpy(buf, d->disc_header + offset, n);
        offset += n;
        buf += n;
        len -= n;
    }
    while (len > 0) {
        const raw_entry *r = NULL;
        for (size_t i = 0; i < d->raw_count && !r; i++) {
            if (offset >= d->raws[i].offset && offset - d->raws[i].offset < d->raws[i].size) r = &d->raws[i];
        }
        if (!r) return SIGIL_ERR_NOT_FOUND;
        uint64_t into = offset - r->offset;
        uint64_t group = into / d->chunk_size;
        if (group >= r->group_count) return SIGIL_ERR_IO;
        uint64_t group_start = group * d->chunk_size;
        size_t group_len = (size_t)(r->size - group_start < d->chunk_size ? r->size - group_start : d->chunk_size);
        int rc = wia_load_group(d, (size_t)r->group_index + (size_t)group, group_len, group_start);
        if (rc != SIGIL_OK) return rc;
        size_t at = (size_t)(into - group_start);
        size_t n = group_len - at < len ? group_len - at : len;
        memcpy(buf, d->chunk + at, n);
        offset += n;
        buf += n;
        len -= n;
    }
    return SIGIL_OK;
}

/* ---- WBFS ---------------------------------------------------------------- */

static int wbfs_open(disc *d) {
    uint8_t head[WBFS_DISC_TABLE + 1];
    int rc = sigil_io_read_exact(d->io, 0, head, sizeof(head));
    if (rc != SIGIL_OK) return rc;
    unsigned hd_shift = head[WBFS_HD_SHIFT], shift = head[WBFS_SECTOR_SHIFT];
    if (hd_shift < 9 || hd_shift > 20 || shift < 15 || shift > 30 || head[WBFS_DISC_TABLE] == 0) {
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
    d->wbfs_shift = shift;
    d->wbfs_sector = (uint64_t)1 << shift;
    d->wlba_table = ((uint64_t)1 << hd_shift) + WBFS_DISC_HEADER_SIZE;
    d->wlba_count = ((uint64_t)WII_SECTOR_COUNT * WII_SECTOR_SIZE + d->wbfs_sector - 1) / d->wbfs_sector;
    return SIGIL_OK;
}

static int wbfs_read(const disc *d, uint64_t offset, uint8_t *buf, size_t len) {
    while (len > 0) {
        uint64_t cluster = offset >> d->wbfs_shift;
        if (cluster >= d->wlba_count) return SIGIL_ERR_IO;
        uint8_t wlba[2];
        int rc = sigil_io_read_exact(d->io, d->wlba_table + cluster * 2, wlba, sizeof(wlba));
        if (rc != SIGIL_OK) return rc;
        uint64_t within = offset & (d->wbfs_sector - 1);
        size_t n = d->wbfs_sector - within < len ? (size_t)(d->wbfs_sector - within) : len;
        rc = sigil_io_read_exact(d->io, sigil_read_be16(wlba) * d->wbfs_sector + within, buf, n);
        if (rc != SIGIL_OK) return rc;
        offset += n;
        buf += n;
        len -= n;
    }
    return SIGIL_OK;
}

/* ---- The disc ------------------------------------------------------------ */

static int disc_read(disc *d, uint64_t offset, void *buf, size_t len) {
    switch (d->kind) {
    case DISC_WBFS: return wbfs_read(d, offset, (uint8_t *)buf, len);
    case DISC_WIA:
    case DISC_RVZ: return wia_read(d, offset, (uint8_t *)buf, len);
    default: return sigil_io_read_exact(d->io, offset, buf, len);
    }
}

static int disc_be32(disc *d, uint64_t offset, uint32_t *out) {
    uint8_t b[4];
    int rc = disc_read(d, offset, b, sizeof(b));
    if (rc == SIGIL_OK) *out = sigil_read_be32(b);
    return rc;
}

/* The game partition's offset: the first partition of type 0, the groups
 * of the table in order (VolumeWii's constructor). */
static int game_partition(disc *d, uint64_t *out) {
    for (uint32_t group = 0; group < PARTITION_GROUPS; group++) {
        uint32_t count, table;
        if (disc_be32(d, PARTITION_TABLE + group * 8, &count) != SIGIL_OK ||
            disc_be32(d, PARTITION_TABLE + group * 8 + 4, &table) != SIGIL_OK) {
            continue;
        }
        for (uint32_t i = 0; i < count && i < PARTITION_MAX; i++) {
            uint32_t offset, type;
            uint64_t entry = ((uint64_t)table << 2) + (uint64_t)i * 8;
            if (disc_be32(d, entry, &offset) != SIGIL_OK || disc_be32(d, entry + 4, &type) != SIGIL_OK) continue;
            if (type == PARTITION_TYPE_GAME) {
                *out = (uint64_t)offset << 2;
                return SIGIL_OK;
            }
        }
    }
    return SIGIL_ERR_NOT_FOUND;
}

int sigil_wii_disc_title_id(const sigil_io *io, uint8_t title_id[8]) {
    disc d;
    memset(&d, 0, sizeof(d));
    d.io = io;
    uint8_t magic[4];
    int rc = sigil_io_read_exact(io, 0, magic, sizeof(magic));
    if (rc != SIGIL_OK) return rc;
    if (memcmp(magic, "WBFS", 4) == 0) {
        d.kind = DISC_WBFS;
        rc = wbfs_open(&d);
    } else if (memcmp(magic, "WIA\x01", 4) == 0) {
        d.kind = DISC_WIA;
        rc = wia_open(&d, false);
    } else if (memcmp(magic, "RVZ\x01", 4) == 0) {
        d.kind = DISC_RVZ;
        rc = wia_open(&d, true);
    } else {
        d.kind = DISC_ISO;
    }
    uint64_t partition = 0;
    if (rc == SIGIL_OK) rc = game_partition(&d, &partition);
    if (rc == SIGIL_OK) rc = disc_read(&d, partition + TICKET_TITLE_ID, title_id, 8);
    if (rc == SIGIL_OK) {
        uint32_t high = sigil_read_be32(title_id);
        if (high != TITLE_GAME && high != TITLE_GAME_WITH_CHANNEL) rc = SIGIL_ERR_NOT_FOUND;
    }
    free(d.raws);
    free(d.groups);
    free(d.chunk);
    return rc;
}
