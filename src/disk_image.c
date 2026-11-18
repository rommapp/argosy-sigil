// SPDX-License-Identifier: MPL-2.0
/* Ported from CabinetOS's frontend/src/xboxhdd.cpp (MIT; the notice is in
 * licenses/cabinetos-MIT.txt). */
#include "disk_image.h"
#include "zlib.h"
#include <stdlib.h>

/* ---- the file, with staged writes --------------------------------------------- */

#define STAGE_PAGE 4096u

/* One page of the file as it will be. `data` is NULL in an empty slot. */
typedef struct {
    uint64_t index;
    uint8_t *data;
} stage_page;

#define QCOW2_MAGIC          0x514649FBu
#define QCOW2_HEADER_V2      72u
#define QCOW2_HEADER_V3      104u
#define QCOW2_OFFSET_MASK    0x00FFFFFFFFFFFE00ull
#define QCOW2_COPIED         (1ull << 63)
#define QCOW2_COMPRESSED     (1ull << 62)
#define QCOW2_ZERO           1ull
#define QCOW2_MAX_L1_BYTES   (32u * 1024u * 1024u)   /* qemu's own bound */
#define QCOW2_INCOMPAT_DIRTY       (1ull << 0)
#define QCOW2_INCOMPAT_CORRUPT     (1ull << 1)
#define QCOW2_INCOMPAT_COMPRESSION (1ull << 3)
#define QCOW2_AUTOCLEAR_OFFSET 88u

struct sigil_disk {
    const sigil_io *io;
    uint64_t        file_len;    /* the file as it is */
    uint64_t        new_len;     /* the file with the staged writes */
    stage_page     *slots;       /* open addressing, a power of two */
    size_t          slot_count;
    size_t          staged;

    bool            qcow2;
    unsigned        cluster_bits;
    uint64_t        cluster_size;
    uint64_t        size;        /* the drive's size */
    uint32_t        l1_size;
    uint64_t        l1_off;
    uint64_t       *l1;
    uint64_t      **l2;          /* by L1 index, read when first needed */
    uint64_t        rt_off;
    uint64_t        rt_entries;
    uint64_t        next_cluster;   /* where the next new cluster goes: the end of the file */
    bool            autoclear;      /* the header holds autoclear bits a writer must clear */
    uint8_t        *cluster;        /* scratch, one cluster */
};

static stage_page *stage_find(const sigil_disk *d, uint64_t index) {
    if (!d->slot_count) return NULL;
    size_t mask = d->slot_count - 1;
    for (size_t i = (size_t)(index * 0x9E3779B97F4A7C15ull >> 20) & mask;; i = (i + 1) & mask) {
        stage_page *p = &d->slots[i];
        if (!p->data) return NULL;
        if (p->index == index) return p;
    }
}

static void stage_place(stage_page *slots, size_t count, uint64_t index, uint8_t *data) {
    size_t mask = count - 1;
    size_t i = (size_t)(index * 0x9E3779B97F4A7C15ull >> 20) & mask;
    while (slots[i].data) i = (i + 1) & mask;
    slots[i].index = index;
    slots[i].data = data;
}

/* Takes ownership of `data`. */
static int stage_add(sigil_disk *d, uint64_t index, uint8_t *data) {
    if ((d->staged + 1) * 2 > d->slot_count) {
        size_t count = d->slot_count ? d->slot_count * 2 : 256;
        stage_page *grown = (stage_page *)calloc(count, sizeof(*grown));
        if (!grown) {
            free(data);
            return SIGIL_ERR_OOM;
        }
        for (size_t i = 0; i < d->slot_count; i++) {
            if (d->slots[i].data) stage_place(grown, count, d->slots[i].index, d->slots[i].data);
        }
        free(d->slots);
        d->slots = grown;
        d->slot_count = count;
    }
    stage_place(d->slots, d->slot_count, index, data);
    d->staged++;
    return SIGIL_OK;
}

/* The file as it is: bytes past its end read as zeros, as a cluster qemu
 * allocated and never filled does. */
static int base_read(const sigil_disk *d, uint64_t off, uint8_t *buf, size_t len) {
    size_t got = 0;
    if (off < d->file_len) {
        size_t want = d->file_len - off < len ? (size_t)(d->file_len - off) : len;
        int rc = sigil_io_read_upto(d->io, off, buf, want, &got);
        if (rc != SIGIL_OK) return rc;
        if (got != want) return SIGIL_ERR_IO;
    }
    memset(buf + got, 0, len - got);
    return SIGIL_OK;
}

static int file_read(const sigil_disk *d, uint64_t off, uint8_t *buf, size_t len) {
    while (len) {
        size_t at = (size_t)(off % STAGE_PAGE);
        size_t n = STAGE_PAGE - at < len ? STAGE_PAGE - at : len;
        const stage_page *p = stage_find(d, off / STAGE_PAGE);
        if (p) {
            memcpy(buf, p->data + at, n);
        } else {
            int rc = base_read(d, off, buf, n);
            if (rc != SIGIL_OK) return rc;
        }
        off += n;
        buf += n;
        len -= n;
    }
    return SIGIL_OK;
}

static int file_write(sigil_disk *d, uint64_t off, const uint8_t *buf, size_t len) {
    if (off + len > d->new_len) d->new_len = off + len;
    while (len) {
        size_t at = (size_t)(off % STAGE_PAGE);
        size_t n = STAGE_PAGE - at < len ? STAGE_PAGE - at : len;
        uint64_t index = off / STAGE_PAGE;
        stage_page *p = stage_find(d, index);
        if (p) {
            memcpy(p->data + at, buf, n);
        } else {
            uint8_t *page = (uint8_t *)malloc(STAGE_PAGE);
            if (!page) return SIGIL_ERR_OOM;
            int rc = base_read(d, index * STAGE_PAGE, page, STAGE_PAGE);
            if (rc == SIGIL_OK && memcmp(page + at, buf, n) != 0) {
                memcpy(page + at, buf, n);
                rc = stage_add(d, index, page);
            } else {
                free(page);
            }
            if (rc != SIGIL_OK) return rc;
        }
        off += n;
        buf += n;
        len -= n;
    }
    return SIGIL_OK;
}

static uint64_t align_up(const sigil_disk *d, uint64_t off) {
    return (off + d->cluster_size - 1) & ~(d->cluster_size - 1);
}

/* ---- qcow2 -------------------------------------------------------------------- */

/* A cluster the tables point at starts on a cluster boundary inside the file. */
static bool host_valid(const sigil_disk *d, uint64_t host) {
    return host && !(host & (d->cluster_size - 1)) && host < align_up(d, d->new_len);
}

static int qcow2_open(sigil_disk *d) {
    uint8_t h[QCOW2_HEADER_V3 + 8];
    if (d->file_len < QCOW2_HEADER_V2) return SIGIL_ERR_DAMAGED;
    int rc = base_read(d, 0, h, sizeof(h));
    if (rc != SIGIL_OK) return rc;
    uint32_t version = sigil_read_be32(h + 4);
    if (version != 2 && version != 3) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    if (sigil_read_be64(h + 8) != 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;    /* a backing file */
    d->cluster_bits = sigil_read_be32(h + 20);
    if (d->cluster_bits < 9 || d->cluster_bits > 21) return SIGIL_ERR_DAMAGED;
    d->cluster_size = 1ull << d->cluster_bits;
    d->size = sigil_read_be64(h + 24);
    if (sigil_read_be32(h + 32) != 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;   /* encrypted */
    d->l1_size = sigil_read_be32(h + 36);
    d->l1_off = sigil_read_be64(h + 40);
    d->rt_off = sigil_read_be64(h + 48);
    d->rt_entries = (uint64_t)sigil_read_be32(h + 56) * d->cluster_size / 8;
    if (sigil_read_be32(h + 60) != 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;   /* internal snapshots */
    if (version == 3) {
        uint64_t incompatible = sigil_read_be64(h + 72);
        uint32_t header_len = sigil_read_be32(h + 100);
        if (incompatible & QCOW2_INCOMPAT_CORRUPT) return SIGIL_ERR_DAMAGED;
        /* The compression bit only says the header names a type, which must
         * still be zlib. Every other bit is a feature outside this code. */
        if (incompatible & ~QCOW2_INCOMPAT_COMPRESSION) return SIGIL_ERR_UNSUPPORTED_FORMAT;
        if ((incompatible & QCOW2_INCOMPAT_COMPRESSION) && header_len > QCOW2_HEADER_V3 && h[QCOW2_HEADER_V3] != 0) {
            return SIGIL_ERR_UNSUPPORTED_FORMAT;
        }
        if (sigil_read_be32(h + 96) != 4) return SIGIL_ERR_UNSUPPORTED_FORMAT;   /* 16-bit reference counts */
        d->autoclear = sigil_read_be64(h + QCOW2_AUTOCLEAR_OFFSET) != 0;
    }
    uint64_t l1_bytes = (uint64_t)d->l1_size * 8;
    if (l1_bytes > QCOW2_MAX_L1_BYTES || !d->l1_off || (d->l1_off & 511) || d->l1_off > d->file_len ||
        l1_bytes > d->file_len - d->l1_off) {
        return SIGIL_ERR_DAMAGED;
    }
    if (!d->rt_entries || !d->rt_off || (d->rt_off & (d->cluster_size - 1)) || d->rt_off > d->file_len ||
        d->rt_entries * 8 > d->file_len - d->rt_off) {
        return SIGIL_ERR_DAMAGED;
    }
    d->next_cluster = align_up(d, d->file_len);
    d->cluster = (uint8_t *)malloc((size_t)d->cluster_size);
    d->l1 = (uint64_t *)calloc(d->l1_size ? d->l1_size : 1, sizeof(*d->l1));
    d->l2 = (uint64_t **)calloc(d->l1_size ? d->l1_size : 1, sizeof(*d->l2));
    uint8_t *raw = (uint8_t *)malloc(l1_bytes ? (size_t)l1_bytes : 1);
    if (!d->cluster || !d->l1 || !d->l2 || !raw) {
        free(raw);
        return SIGIL_ERR_OOM;
    }
    rc = base_read(d, d->l1_off, raw, (size_t)l1_bytes);
    for (uint32_t i = 0; rc == SIGIL_OK && i < d->l1_size; i++) d->l1[i] = sigil_read_be64(raw + (size_t)i * 8);
    free(raw);
    return rc;
}

/* The L2 table of L1 entry `i1`, or NULL in `*table` when it has none. */
static int l2_table(sigil_disk *d, uint64_t i1, uint64_t **table) {
    *table = NULL;
    if (i1 >= d->l1_size) return SIGIL_OK;
    uint64_t off = d->l1[i1] & QCOW2_OFFSET_MASK;
    if (!off) return SIGIL_OK;
    if (!d->l2[i1]) {
        if (!host_valid(d, off)) return SIGIL_ERR_DAMAGED;
        size_t entries = (size_t)(d->cluster_size / 8);
        uint64_t *t = (uint64_t *)malloc(entries * sizeof(*t));
        if (!t) return SIGIL_ERR_OOM;
        int rc = file_read(d, off, d->cluster, (size_t)d->cluster_size);
        if (rc != SIGIL_OK) {
            free(t);
            return rc;
        }
        for (size_t i = 0; i < entries; i++) t[i] = sigil_read_be64(d->cluster + i * 8);
        d->l2[i1] = t;
    }
    *table = d->l2[i1];
    return SIGIL_OK;
}

/* Where a compressed cluster's bytes are in the file, and how many. */
static void compressed_extent(const sigil_disk *d, uint64_t entry, uint64_t *host, uint64_t *len) {
    unsigned shift = 62 - (d->cluster_bits - 8);
    *host = entry & ((1ull << shift) - 1);
    uint64_t sectors = ((entry >> shift) & ((1ull << (d->cluster_bits - 8)) - 1)) + 1;
    *len = sectors * 512 - (*host & 511);
}

/* The whole cluster an L2 entry stands for, into `out`. */
static int cluster_bytes(sigil_disk *d, uint64_t entry, uint8_t *out) {
    size_t size = (size_t)d->cluster_size;
    if (entry & QCOW2_COMPRESSED) {
        uint64_t host, len;
        compressed_extent(d, entry, &host, &len);
        if (host >= d->new_len) return SIGIL_ERR_DAMAGED;
        uint8_t *packed = (uint8_t *)malloc((size_t)len);
        if (!packed) return SIGIL_ERR_OOM;
        /* The last compressed cluster may end short of its sector count. */
        int rc = file_read(d, host, packed, (size_t)len);
        z_stream z;
        memset(&z, 0, sizeof(z));
        if (rc == SIGIL_OK && inflateInit2(&z, -12) != Z_OK) rc = SIGIL_ERR_OOM;
        if (rc == SIGIL_OK) {
            z.next_in = packed;
            z.avail_in = (uInt)len;
            z.next_out = out;
            z.avail_out = (uInt)size;
            int zrc = inflate(&z, Z_FINISH);
            /* The length is in sectors, so the stream may end before it. */
            bool whole = z.avail_out == 0 && (zrc == Z_STREAM_END || zrc == Z_OK || zrc == Z_BUF_ERROR);
            inflateEnd(&z);
            if (!whole) rc = SIGIL_ERR_DAMAGED;
        }
        free(packed);
        return rc;
    }
    uint64_t host = entry & QCOW2_OFFSET_MASK;
    if (!host || (entry & QCOW2_ZERO)) {
        memset(out, 0, size);
        return SIGIL_OK;
    }
    if (!host_valid(d, host)) return SIGIL_ERR_DAMAGED;
    return file_read(d, host, out, size);
}

static int qcow2_read(sigil_disk *d, uint64_t index, size_t at, uint8_t *buf, size_t n) {
    uint64_t per_table = d->cluster_size / 8;
    uint64_t *table = NULL;
    int rc = l2_table(d, index / per_table, &table);
    if (rc != SIGIL_OK) return rc;
    uint64_t entry = table ? table[index % per_table] : 0;
    if (!(entry & QCOW2_COMPRESSED)) {
        uint64_t host = entry & QCOW2_OFFSET_MASK;
        if (!host || (entry & QCOW2_ZERO)) {
            memset(buf, 0, n);
            return SIGIL_OK;
        }
        if (!host_valid(d, host)) return SIGIL_ERR_DAMAGED;
        return file_read(d, host + at, buf, n);
    }
    rc = cluster_bytes(d, entry, d->cluster);
    if (rc == SIGIL_OK) memcpy(buf, d->cluster + at, n);
    return rc;
}

static int refcount_add(sigil_disk *d, uint64_t cluster, int delta);

/* Where the reference count of host cluster `cluster` is kept; 0 in `*where`
 * when its block doesn't exist and `make` is false, which reads as a count
 * of 0. A block made here goes at the end of the file and counts itself. */
static int refcount_where(sigil_disk *d, uint64_t cluster, bool make, uint64_t *where) {
    uint64_t per_block = d->cluster_size / 2;
    uint64_t ti = cluster / per_block;
    *where = 0;
    /* A table too small for the file would have to move; nothing xemu writes needs that. */
    if (ti >= d->rt_entries) return make ? SIGIL_ERR_UNSUPPORTED_FORMAT : SIGIL_OK;
    uint8_t raw[8];
    int rc = file_read(d, d->rt_off + ti * 8, raw, sizeof(raw));
    if (rc != SIGIL_OK) return rc;
    uint64_t block = sigil_read_be64(raw) & ~511ull;
    if (!block) {
        if (!make) return SIGIL_OK;
        block = d->next_cluster;
        d->next_cluster += d->cluster_size;
        if (block + d->cluster_size > d->new_len) d->new_len = block + d->cluster_size;
        sigil_write_be64(raw, block);
        rc = file_write(d, d->rt_off + ti * 8, raw, sizeof(raw));
        if (rc == SIGIL_OK) rc = refcount_add(d, block >> d->cluster_bits, 1);
        if (rc != SIGIL_OK) return rc;
    } else if (!host_valid(d, block)) {
        return SIGIL_ERR_DAMAGED;
    }
    *where = block + (cluster % per_block) * 2;
    return SIGIL_OK;
}

static int refcount_add(sigil_disk *d, uint64_t cluster, int delta) {
    uint64_t where = 0;
    int rc = refcount_where(d, cluster, delta > 0, &where);
    if (rc != SIGIL_OK) return rc;
    uint8_t raw[2] = { 0, 0 };
    if (where) rc = file_read(d, where, raw, sizeof(raw));
    if (rc != SIGIL_OK) return rc;
    int count = (int)sigil_read_be16(raw) + delta;
    /* A count the tables don't bear out: the image isn't consistent. */
    if (count < 0 || count > 0xFFFF || !where) return SIGIL_ERR_DAMAGED;
    sigil_write_be16(raw, (uint16_t)count);
    return file_write(d, where, raw, sizeof(raw));
}

/* A new cluster at the end of the file, zeroed and counted once. */
static int allocate(sigil_disk *d, uint64_t *out) {
    uint64_t off = d->next_cluster;
    d->next_cluster += d->cluster_size;
    if (off + d->cluster_size > d->new_len) d->new_len = off + d->cluster_size;
    int rc = refcount_add(d, off >> d->cluster_bits, 1);
    *out = off;
    return rc;
}

/* Drops the references an L2 entry held: one for each cluster of the file its bytes lie in. */
static int release(sigil_disk *d, uint64_t entry) {
    uint64_t host = entry & QCOW2_OFFSET_MASK, len = d->cluster_size;
    if (entry & QCOW2_COMPRESSED) compressed_extent(d, entry, &host, &len);
    else if (!host) return SIGIL_OK;
    int rc = SIGIL_OK;
    for (uint64_t c = host >> d->cluster_bits; rc == SIGIL_OK && c <= (host + len - 1) >> d->cluster_bits; c++) {
        rc = refcount_add(d, c, -1);
    }
    return rc;
}

static int qcow2_write(sigil_disk *d, uint64_t index, size_t at, const uint8_t *buf, size_t n) {
    uint64_t per_table = d->cluster_size / 8;
    uint64_t i1 = index / per_table;
    if (i1 >= d->l1_size) return SIGIL_ERR_DAMAGED;
    uint8_t raw[8];
    int rc = SIGIL_OK;
    if (!(d->l1[i1] & QCOW2_OFFSET_MASK)) {
        uint64_t made = 0;
        rc = allocate(d, &made);
        if (rc != SIGIL_OK) return rc;
        d->l1[i1] = made | QCOW2_COPIED;
        sigil_write_be64(raw, d->l1[i1]);
        rc = file_write(d, d->l1_off + i1 * 8, raw, sizeof(raw));
        if (rc != SIGIL_OK) return rc;
    } else if (!(d->l1[i1] & QCOW2_COPIED)) {
        return SIGIL_ERR_UNSUPPORTED_FORMAT;   /* an L2 table something else shares */
    }
    uint64_t *table = NULL;
    rc = l2_table(d, i1, &table);
    if (rc != SIGIL_OK) return rc;
    uint64_t entry = table[index % per_table];
    uint64_t host = entry & QCOW2_OFFSET_MASK;
    bool own = !(entry & QCOW2_COMPRESSED) && host && (entry & QCOW2_COPIED);
    if (own && !host_valid(d, host)) return SIGIL_ERR_DAMAGED;
    if (own && !(entry & QCOW2_ZERO)) return file_write(d, host + at, buf, n);

    /* Anything else becomes a whole cluster: its bytes as they read, then ours. */
    uint8_t *whole = (uint8_t *)malloc((size_t)d->cluster_size);
    if (!whole) return SIGIL_ERR_OOM;
    rc = cluster_bytes(d, entry, whole);
    if (rc == SIGIL_OK) {
        if (memcmp(whole + at, buf, n) == 0) {   /* it already reads as this */
            free(whole);
            return SIGIL_OK;
        }
        memcpy(whole + at, buf, n);
        if (!own) rc = allocate(d, &host);
    }
    if (rc == SIGIL_OK) rc = file_write(d, host, whole, (size_t)d->cluster_size);
    free(whole);
    if (rc != SIGIL_OK) return rc;
    table[index % per_table] = host | QCOW2_COPIED;
    sigil_write_be64(raw, host | QCOW2_COPIED);
    rc = file_write(d, (d->l1[i1] & QCOW2_OFFSET_MASK) + (index % per_table) * 8, raw, sizeof(raw));
    if (rc == SIGIL_OK && !own) rc = release(d, entry);
    return rc;
}

/* ---- public ------------------------------------------------------------------- */

int sigil_disk_open(const sigil_io *io, sigil_disk **out) {
    if (!io || !io->read || !io->size || !out) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    int64_t len = io->size(io->ctx);
    if (len < 0) return SIGIL_ERR_IO;
    sigil_disk *d = (sigil_disk *)calloc(1, sizeof(*d));
    if (!d) return SIGIL_ERR_OOM;
    d->io = io;
    d->file_len = d->new_len = (uint64_t)len;
    d->size = d->file_len;
    d->cluster_size = STAGE_PAGE;
    uint8_t magic[4] = { 0 };
    int rc = base_read(d, 0, magic, sizeof(magic));
    if (rc == SIGIL_OK && sigil_read_be32(magic) == QCOW2_MAGIC) {
        d->qcow2 = true;
        rc = qcow2_open(d);
    }
    if (rc != SIGIL_OK) {
        sigil_disk_close(d);
        return rc;
    }
    *out = d;
    return SIGIL_OK;
}

void sigil_disk_close(sigil_disk *d) {
    if (!d) return;
    for (size_t i = 0; i < d->slot_count; i++) free(d->slots[i].data);
    free(d->slots);
    for (uint32_t i = 0; d->l2 && i < d->l1_size; i++) free(d->l2[i]);
    free(d->l2);
    free(d->l1);
    free(d->cluster);
    free(d);
}

uint64_t sigil_disk_size(const sigil_disk *d) {
    return d->size;
}

int sigil_disk_read(sigil_disk *d, uint64_t off, void *buf, size_t len) {
    if (off > d->size || len > d->size - off) return SIGIL_ERR_DAMAGED;
    if (!d->qcow2) return file_read(d, off, (uint8_t *)buf, len);
    uint8_t *out = (uint8_t *)buf;
    while (len) {
        size_t at = (size_t)(off & (d->cluster_size - 1));
        size_t n = d->cluster_size - at < len ? (size_t)(d->cluster_size - at) : len;
        int rc = qcow2_read(d, off >> d->cluster_bits, at, out, n);
        if (rc != SIGIL_OK) return rc;
        off += n;
        out += n;
        len -= n;
    }
    return SIGIL_OK;
}

int sigil_disk_write(sigil_disk *d, uint64_t off, const void *buf, size_t len) {
    if (off > d->size || len > d->size - off) return SIGIL_ERR_DAMAGED;
    if (!d->qcow2) return file_write(d, off, (const uint8_t *)buf, len);
    const uint8_t *in = (const uint8_t *)buf;
    while (len) {
        size_t at = (size_t)(off & (d->cluster_size - 1));
        size_t n = d->cluster_size - at < len ? (size_t)(d->cluster_size - at) : len;
        int rc = qcow2_write(d, off >> d->cluster_bits, at, in, n);
        if (rc != SIGIL_OK) return rc;
        off += n;
        in += n;
        len -= n;
    }
    /* Autoclear bits describe extensions a writer that doesn't know them
     * must declare stale (bitmaps): cleared with the first change. */
    if (d->autoclear && d->staged) {
        static const uint8_t zero[8] = { 0 };
        d->autoclear = false;
        return file_write(d, QCOW2_AUTOCLEAR_OFFSET, zero, sizeof(zero));
    }
    return SIGIL_OK;
}

bool sigil_disk_changed(const sigil_disk *d) {
    return d->staged > 0;
}

int sigil_disk_file(sigil_disk *d, uint8_t **out, size_t *len) {
    *out = NULL;
    *len = 0;
    if (d->new_len > SIGIL_DISK_MAX_FILE) return SIGIL_ERR_OOM;
    size_t total = (size_t)d->new_len;
    uint8_t *buf = (uint8_t *)malloc(total ? total : 1);
    if (!buf) return SIGIL_ERR_OOM;
    int rc = SIGIL_OK;
    for (size_t at = 0; rc == SIGIL_OK && at < total;) {
        size_t n = total - at < (1u << 20) ? total - at : (1u << 20);
        rc = base_read(d, at, buf + at, n);
        at += n;
    }
    for (size_t i = 0; rc == SIGIL_OK && i < d->slot_count; i++) {
        const stage_page *p = &d->slots[i];
        if (!p->data || p->index * STAGE_PAGE >= total) continue;
        size_t at = (size_t)(p->index * STAGE_PAGE);
        memcpy(buf + at, p->data, total - at < STAGE_PAGE ? total - at : STAGE_PAGE);
    }
    if (rc != SIGIL_OK) {
        free(buf);
        return rc;
    }
    *out = buf;
    *len = total;
    return SIGIL_OK;
}
