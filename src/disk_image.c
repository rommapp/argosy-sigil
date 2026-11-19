// SPDX-License-Identifier: MPL-2.0
/* Ported from CabinetOS's frontend/src/xboxhdd.cpp (MIT; the notice is in
 * licenses/cabinetos-MIT.txt). */
#include "disk_image.h"
#include "zlib.h"
#include <stdlib.h>

/* ---- the file, with staged writes --------------------------------------------- */

#define STAGE_PAGE 4096u

/* The steps a restore in parts writes in, in order (sigil_disk_runs). Each
 * phase's bytes go before the table entries that point at them, every count
 * a new cluster gains goes before any table points at it, and every count a
 * replaced cluster loses goes after the tables have stopped pointing at it.
 * That is qemu's own order (qemu at v9.2.0: block/qcow2-cluster.c
 * perform_cow, "Before we update the L2 table to actually point to the new
 * cluster, we need to be sure that the refcounts have been increased and COW
 * was handled"; block/qcow2-refcount.c qcow2_alloc_bytes, "refcount blocks
 * must be flushed before the caller's L2 table updates"; update_refcount, a
 * decrease makes the refcount blocks wait for the L2 tables), with the file
 * flushed between steps (block/qcow2-cache.c qcow2_cache_flush). */
#define STEP_COUNT_UP    0u                                /* the counts new clusters gain */
#define STEP_DATA(phase) (1u + 2u * (phase))               /* a phase's bytes */
#define STEP_MAP(phase)  (2u + 2u * (phase))               /* the table entries that map them */
#define STEP_COUNT_DOWN  (1u + 2u * SIGIL_DISK_PHASES)     /* the counts replaced clusters lose */
#define STEPS            (STEP_COUNT_DOWN + 1u)

/* One page of the file as each step leaves it: ver[s] is the page once every
 * write up to step s is made, NULL where step s doesn't change it (the page
 * then reads as the step before does, or as the file). */
typedef struct {
    uint64_t index;
    bool     used;
    uint8_t *ver[STEPS];
} stage_page;

#define QCOW2_MAGIC          0x514649FBu
#define QCOW2_HEADER_V2      72u
#define QCOW2_HEADER_V3      104u
#define QCOW2_OFFSET_MASK    0x00FFFFFFFFFFFE00ull
#define QCOW2_COPIED         (1ull << 63)
#define QCOW2_COMPRESSED     (1ull << 62)
#define QCOW2_ZERO           1ull
#define QCOW2_MAX_L1_BYTES   (32u * 1024u * 1024u)   /* block/qcow2.h QCOW_MAX_L1_SIZE */
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
    unsigned        phase;       /* what the writes staged now belong to */

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
    uint8_t        *cluster;        /* scratch, one cluster */
};

static size_t stage_slot(uint64_t index, size_t mask) {
    return (size_t)(index * 0x9E3779B97F4A7C15ull >> 20) & mask;
}

static stage_page *stage_find(const sigil_disk *d, uint64_t index) {
    if (!d->slot_count) return NULL;
    size_t mask = d->slot_count - 1;
    for (size_t i = stage_slot(index, mask);; i = (i + 1) & mask) {
        stage_page *p = &d->slots[i];
        if (!p->used) return NULL;
        if (p->index == index) return p;
    }
}

static void stage_place(stage_page *slots, size_t count, const stage_page *page) {
    size_t mask = count - 1;
    size_t i = stage_slot(page->index, mask);
    while (slots[i].used) i = (i + 1) & mask;
    slots[i] = *page;
}

/* The page as step `step` leaves it; NULL when no step up to it changes the page. */
static uint8_t *page_at(const stage_page *p, unsigned step) {
    for (unsigned s = step + 1; s-- > 0;) {
        if (p->ver[s]) return p->ver[s];
    }
    return NULL;
}

/* The page once every step is made. */
static uint8_t *page_last(const stage_page *p) {
    return page_at(p, STEPS - 1);
}

/* The first step that changes the page. */
static unsigned page_first(const stage_page *p) {
    unsigned s = 0;
    while (s + 1 < STEPS && !p->ver[s]) s++;
    return s;
}

/* Stages page `index`, changed by step `step` into `data`, which it takes. */
static int stage_add(sigil_disk *d, uint64_t index, unsigned step, uint8_t *data) {
    if ((d->staged + 1) * 2 > d->slot_count) {
        size_t count = d->slot_count ? d->slot_count * 2 : 256;
        stage_page *grown = (stage_page *)calloc(count, sizeof(*grown));
        if (!grown) {
            free(data);
            return SIGIL_ERR_OOM;
        }
        for (size_t i = 0; i < d->slot_count; i++) {
            if (d->slots[i].used) stage_place(grown, count, &d->slots[i]);
        }
        free(d->slots);
        d->slots = grown;
        d->slot_count = count;
    }
    stage_page page;
    memset(&page, 0, sizeof(page));
    page.index = index;
    page.used = true;
    page.ver[step] = data;
    stage_place(d->slots, d->slot_count, &page);
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

/* Page `index` as the file holds it, malloc'd. */
static int base_page(const sigil_disk *d, uint64_t index, uint8_t **out) {
    *out = (uint8_t *)malloc(STAGE_PAGE);
    if (!*out) return SIGIL_ERR_OOM;
    int rc = base_read(d, index * STAGE_PAGE, *out, STAGE_PAGE);
    if (rc != SIGIL_OK) {
        free(*out);
        *out = NULL;
    }
    return rc;
}

static int file_read(const sigil_disk *d, uint64_t off, uint8_t *buf, size_t len) {
    while (len) {
        size_t at = (size_t)(off % STAGE_PAGE);
        size_t n = STAGE_PAGE - at < len ? STAGE_PAGE - at : len;
        const stage_page *p = stage_find(d, off / STAGE_PAGE);
        if (p) {
            memcpy(buf, page_last(p) + at, n);
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

/* Puts `n` bytes at `at` of staged page `p` into step `step` and every later
 * step that changes the page. Nothing is staged when each of those already
 * reads as `buf` there. */
static int page_write(const sigil_disk *d, stage_page *p, unsigned step, size_t at, const uint8_t *buf, size_t n) {
    uint8_t *base = NULL, *was = page_at(p, step);
    if (!was) {
        int rc = base_page(d, p->index, &base);
        if (rc != SIGIL_OK) return rc;
        was = base;
    }
    bool same = memcmp(was + at, buf, n) == 0;
    for (unsigned s = step + 1; same && s < STEPS; s++) same = !p->ver[s] || memcmp(p->ver[s] + at, buf, n) == 0;
    if (same) {
        free(base);
        return SIGIL_OK;
    }
    if (!p->ver[step]) {
        if (base) {
            p->ver[step] = base;
            base = NULL;
        } else {
            p->ver[step] = (uint8_t *)malloc(STAGE_PAGE);
            if (!p->ver[step]) return SIGIL_ERR_OOM;
            memcpy(p->ver[step], was, STAGE_PAGE);
        }
    }
    free(base);
    for (unsigned s = step; s < STEPS; s++) {
        if (p->ver[s]) memcpy(p->ver[s] + at, buf, n);
    }
    return SIGIL_OK;
}

/* Stages every page of the new cluster at `off` as step `step` leaves it,
 * zeros where nothing is written yet. A page wholly past the file's old end
 * goes out once ahead of everything (sigil_disk_runs), as its first step
 * leaves it, so that step must be no later than the one that first points at
 * the cluster: else a page first written in a later phase would show that
 * phase as soon as the cluster is mapped. */
static int stage_cluster(sigil_disk *d, uint64_t off, unsigned step) {
    for (uint64_t index = off / STAGE_PAGE; index <= (off + d->cluster_size - 1) / STAGE_PAGE; index++) {
        stage_page *p = stage_find(d, index);
        if (p && page_at(p, step)) continue;
        uint8_t *page = NULL;
        int rc = base_page(d, index, &page);
        if (rc == SIGIL_OK && p) {
            p->ver[step] = page;
        } else if (rc == SIGIL_OK) {
            rc = stage_add(d, index, step, page);
        }
        if (rc != SIGIL_OK) return rc;
    }
    return SIGIL_OK;
}

/* Stages a write that step `step` makes. */
static int file_write(sigil_disk *d, unsigned step, uint64_t off, const uint8_t *buf, size_t len) {
    if (off + len > d->new_len) d->new_len = off + len;
    while (len) {
        size_t at = (size_t)(off % STAGE_PAGE);
        size_t n = STAGE_PAGE - at < len ? STAGE_PAGE - at : len;
        uint64_t index = off / STAGE_PAGE;
        stage_page *p = stage_find(d, index);
        int rc = SIGIL_OK;
        if (p) {
            rc = page_write(d, p, step, at, buf, n);
        } else {
            uint8_t *page = NULL;
            rc = base_page(d, index, &page);
            if (rc == SIGIL_OK && memcmp(page + at, buf, n) != 0) {
                memcpy(page + at, buf, n);
                rc = stage_add(d, index, step, page);
            } else {
                free(page);
            }
        }
        if (rc != SIGIL_OK) return rc;
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
        /* A version 3 header holds at least the fields through header_length,
         * 104 bytes (qcow2.rst "header_length"; block/qcow2.c qcow2_do_open at
         * v0.8.136 refuses less as "qcow2 header too short"). */
        if (header_len < QCOW2_HEADER_V3) return SIGIL_ERR_DAMAGED;
        if (incompatible & QCOW2_INCOMPAT_CORRUPT) return SIGIL_ERR_DAMAGED;
        /* The compression bit only says the header names a type, which must
         * still be zlib. Every other bit is a feature outside this code. */
        if (incompatible & ~QCOW2_INCOMPAT_COMPRESSION) return SIGIL_ERR_UNSUPPORTED_FORMAT;
        if ((incompatible & QCOW2_INCOMPAT_COMPRESSION) && header_len > QCOW2_HEADER_V3 && h[QCOW2_HEADER_V3] != 0) {
            return SIGIL_ERR_UNSUPPORTED_FORMAT;
        }
        if (sigil_read_be32(h + 96) != 4) return SIGIL_ERR_UNSUPPORTED_FORMAT;   /* 16-bit reference counts */
        /* Autoclear bit 0 says the bitmaps extension holds consistent dirty
         * bitmaps, which a write here would make stale; bit 1 comes only with
         * an external data file, refused above, and the rest are reserved
         * (qcow2.rst "autoclear_features" and "Bitmaps extension"). */
        if (sigil_read_be64(h + QCOW2_AUTOCLEAR_OFFSET) != 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
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
        rc = stage_cluster(d, block, STEP_COUNT_UP);
        if (rc == SIGIL_OK) rc = file_write(d, STEP_COUNT_UP, d->rt_off + ti * 8, raw, sizeof(raw));
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
    return file_write(d, delta > 0 ? STEP_COUNT_UP : STEP_COUNT_DOWN, where, raw, sizeof(raw));
}

/* A new cluster at the end of the file, zeroed and counted once, its pages
 * staged at `step`, no later than the step that maps it. */
static int allocate(sigil_disk *d, unsigned step, uint64_t *out) {
    uint64_t off = d->next_cluster;
    d->next_cluster += d->cluster_size;
    if (off + d->cluster_size > d->new_len) d->new_len = off + d->cluster_size;
    int rc = stage_cluster(d, off, step);
    if (rc == SIGIL_OK) rc = refcount_add(d, off >> d->cluster_bits, 1);
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
        rc = allocate(d, STEP_MAP(d->phase), &made);   /* an L2 table, which the L1 entry maps */
        if (rc != SIGIL_OK) return rc;
        d->l1[i1] = made | QCOW2_COPIED;
        sigil_write_be64(raw, d->l1[i1]);
        rc = file_write(d, STEP_MAP(d->phase), d->l1_off + i1 * 8, raw, sizeof(raw));
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
    if (own && !(entry & QCOW2_ZERO)) return file_write(d, STEP_DATA(d->phase), host + at, buf, n);

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
        if (!own) rc = allocate(d, STEP_DATA(d->phase), &host);
    }
    if (rc == SIGIL_OK) rc = file_write(d, STEP_DATA(d->phase), host, whole, (size_t)d->cluster_size);
    free(whole);
    if (rc != SIGIL_OK) return rc;
    table[index % per_table] = host | QCOW2_COPIED;
    sigil_write_be64(raw, host | QCOW2_COPIED);
    rc = file_write(d, STEP_MAP(d->phase), (d->l1[i1] & QCOW2_OFFSET_MASK) + (index % per_table) * 8, raw,
                    sizeof(raw));
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
    for (size_t i = 0; i < d->slot_count; i++) {
        for (unsigned s = 0; s < STEPS; s++) free(d->slots[i].ver[s]);
    }
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
    if (!d->qcow2) return file_write(d, STEP_DATA(d->phase), off, (const uint8_t *)buf, len);
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
    return SIGIL_OK;
}

bool sigil_disk_changed(const sigil_disk *d) {
    return d->staged > 0;
}

int sigil_disk_set_phase(sigil_disk *d, unsigned phase) {
    if (phase < d->phase || phase >= SIGIL_DISK_PHASES) return SIGIL_ERR_INVALID_ARG;
    d->phase = phase;
    return SIGIL_OK;
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
        if (!p->used || p->index * STAGE_PAGE >= total) continue;
        size_t at = (size_t)(p->index * STAGE_PAGE);
        memcpy(buf + at, page_last(p), total - at < STAGE_PAGE ? total - at : STAGE_PAGE);
    }
    if (rc != SIGIL_OK) {
        free(buf);
        return rc;
    }
    *out = buf;
    *len = total;
    return SIGIL_OK;
}

void sigil_disk_detach(sigil_disk *d) {
    d->io = NULL;
}

#define RUN_MAX (1u << 20)

static int compare_index(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* Bytes gathered into one run, sent when the next don't follow on or it is full. */
typedef struct {
    sigil_disk_run_fn each;
    void             *ctx;
    uint8_t          *buf;
    uint64_t          off;
    size_t            len;
} run_out;

static int run_send(run_out *r) {
    if (!r->len) return SIGIL_OK;
    int rc = r->each(r->ctx, r->off, r->buf, r->len);
    r->len = 0;
    return rc;
}

static int run_add(run_out *r, uint64_t off, const uint8_t *data, size_t len) {
    if (r->len && (off != r->off + r->len || r->len + len > RUN_MAX)) {
        int rc = run_send(r);
        if (rc != SIGIL_OK) return rc;
    }
    if (!r->len) r->off = off;
    memcpy(r->buf + r->len, data, len);
    r->len += len;
    return SIGIL_OK;
}

int sigil_disk_runs(sigil_disk *d, bool in_steps, sigil_disk_run_fn each, void *ctx) {
    static const uint8_t zeros[STAGE_PAGE];
    uint64_t *staged = (uint64_t *)malloc((d->staged ? d->staged : 1) * sizeof(*staged));
    run_out out = { each, ctx, (uint8_t *)malloc(RUN_MAX), 0, 0 };
    if (!staged || !out.buf) {
        free(staged);
        free(out.buf);
        return SIGIL_ERR_OOM;
    }
    size_t n = 0;
    for (size_t i = 0; i < d->slot_count; i++) {
        if (d->slots[i].used) staged[n++] = d->slots[i].index;
    }
    qsort(staged, n, sizeof(*staged), compare_index);
    /* The page holding the file's end, the first wholly past it, and the end of the file as it will be. */
    uint64_t end_page = d->file_len / STAGE_PAGE, past = (d->file_len + STAGE_PAGE - 1) / STAGE_PAGE;
    uint64_t pages = (d->new_len + STAGE_PAGE - 1) / STAGE_PAGE;
    int rc = SIGIL_OK;
    /* What the file gains past its end, which nothing in it refers to yet: as
     * each page's first step leaves it, or as it ends up. */
    for (uint64_t page = end_page; rc == SIGIL_OK && page < pages; page++) {
        uint64_t from = page * STAGE_PAGE < d->file_len ? d->file_len : page * STAGE_PAGE;
        uint64_t to = (page + 1) * STAGE_PAGE < d->new_len ? (page + 1) * STAGE_PAGE : d->new_len;
        const stage_page *p = stage_find(d, page);
        if (from >= to || (!in_steps && p && page < past)) continue;
        const uint8_t *data = !p ? zeros : in_steps ? p->ver[page_first(p)] : page_last(p);
        rc = run_add(&out, from, data + (from - page * STAGE_PAGE), (size_t)(to - from));
    }
    if (rc == SIGIL_OK) rc = run_send(&out);
    /* Then the pages each step changes, a step's runs ended before the next
     * step's begin; or, not in steps, each page inside the file as it ends up.
     * A page wholly past the end went out above as its first step leaves it. */
    for (unsigned step = 0; rc == SIGIL_OK && step < (in_steps ? STEPS : 1u); step++) {
        for (size_t i = 0; rc == SIGIL_OK && i < n; i++) {
            const stage_page *p = stage_find(d, staged[i]);
            const uint8_t *data = in_steps ? p->ver[step] : page_last(p);
            if (!data || (staged[i] >= past && (!in_steps || step == page_first(p)))) continue;
            uint64_t off = staged[i] * STAGE_PAGE;
            rc = run_add(&out, off, data, (size_t)(d->new_len - off < STAGE_PAGE ? d->new_len - off : STAGE_PAGE));
        }
        if (rc == SIGIL_OK) rc = run_send(&out);
    }
    free(staged);
    free(out.buf);
    return rc;
}
