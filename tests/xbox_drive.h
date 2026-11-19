// SPDX-License-Identifier: MPL-2.0
/* Xbox hard-drive images for the tests, built and checked by code that shares
 * nothing with src/disk_image.c and src/fatx.c. A drive is kept as the bytes
 * the console sees, formatted and filled here, then written out as qcow2
 * (some clusters compressed, as the image xemu publishes is). The reader
 * takes an image apart the same independent way: it recounts every
 * qcow2 reference against the stored counts, as `qemu-img check` does, and
 * walks every FATX chain. */
#ifndef SIGIL_TEST_XBOX_DRIVE_H
#define SIGIL_TEST_XBOX_DRIVE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

#define XB_CHUNK      65536u
#define XB_DISK_SIZE  0x200000000ull   /* 8 GiB, a retail drive as xemu's image has it */
#define XB_CHUNKS     ((size_t)(XB_DISK_SIZE / XB_CHUNK))
#define XB_E          0xABE80000ull
#define XB_E_SIZE     0x1312D6000ull
#define XB_RETAIL_SIZE (XB_E + XB_E_SIZE)   /* the smallest retail drive ends where E: does */
#define XB_CLUSTER    16384u
#define XB_FAT        (XB_E + 0x1000u)
#define XB_FAT_BYTES  ((((XB_E_SIZE / XB_CLUSTER + 1) * 4) + 4095) & ~4095ull)
#define XB_DATA       (XB_FAT + XB_FAT_BYTES)
#define XB_CLUSTERS   ((uint32_t)((XB_E_SIZE - XB_FAT_BYTES - 0x1000u) / XB_CLUSTER + 1))
#define XB_SLOT       64u
#define XB_DIR        0x10u

/* ---- the drive as the console sees it ------------------------------------------ */

typedef struct {
    uint8_t **chunks;   /* XB_CHUNKS of them; NULL reads as zeros */
} xb_guest;

static void xb_init(xb_guest *g) {
    g->chunks = (uint8_t **)calloc(XB_CHUNKS, sizeof(*g->chunks));
}

static void xb_free(xb_guest *g) {
    for (size_t i = 0; g->chunks && i < XB_CHUNKS; i++) free(g->chunks[i]);
    free(g->chunks);
    g->chunks = NULL;
}

static void xb_get(const xb_guest *g, uint64_t off, void *buf, size_t len) {
    uint8_t *out = (uint8_t *)buf;
    while (len) {
        size_t at = (size_t)(off % XB_CHUNK), n = XB_CHUNK - at < len ? XB_CHUNK - at : len;
        const uint8_t *c = g->chunks[off / XB_CHUNK];
        if (c) memcpy(out, c + at, n);
        else memset(out, 0, n);
        off += n;
        out += n;
        len -= n;
    }
}

static void xb_set(xb_guest *g, uint64_t off, const void *buf, size_t len) {
    const uint8_t *in = (const uint8_t *)buf;
    while (len) {
        size_t at = (size_t)(off % XB_CHUNK), n = XB_CHUNK - at < len ? XB_CHUNK - at : len;
        uint8_t **c = &g->chunks[off / XB_CHUNK];
        if (!*c) *c = (uint8_t *)calloc(1, XB_CHUNK);
        memcpy(*c + at, in, n);
        off += n;
        in += n;
        len -= n;
    }
}

static void xb_copy(const xb_guest *from, xb_guest *to) {
    xb_init(to);
    for (size_t i = 0; i < XB_CHUNKS; i++) {
        if (!from->chunks[i]) continue;
        to->chunks[i] = (uint8_t *)malloc(XB_CHUNK);
        memcpy(to->chunks[i], from->chunks[i], XB_CHUNK);
    }
}

/* `len` bytes at `off` read the same on both drives; chunks neither holds are zeros on both. */
static bool xb_same_bytes(const xb_guest *a, const xb_guest *b, uint64_t off, size_t len) {
    static const uint8_t zeros[XB_CHUNK];
    while (len) {
        size_t at = (size_t)(off % XB_CHUNK), n = XB_CHUNK - at < len ? XB_CHUNK - at : len;
        const uint8_t *x = a->chunks[off / XB_CHUNK], *y = b->chunks[off / XB_CHUNK];
        if ((x || y) && memcmp((x ? x : zeros) + at, (y ? y : zeros) + at, n) != 0) return false;
        off += n;
        len -= n;
    }
    return true;
}

/* The first byte at which two drives differ, or UINT64_MAX when they read the same. */
static uint64_t xb_first_difference(const xb_guest *a, const xb_guest *b) {
    static const uint8_t zeros[XB_CHUNK];
    for (size_t i = 0; i < XB_CHUNKS; i++) {
        const uint8_t *x = a->chunks[i] ? a->chunks[i] : zeros, *y = b->chunks[i] ? b->chunks[i] : zeros;
        if (x == y || memcmp(x, y, XB_CHUNK) == 0) continue;
        for (size_t k = 0; k < XB_CHUNK; k++) {
            if (x[k] != y[k]) return (uint64_t)i * XB_CHUNK + k;
        }
    }
    return UINT64_MAX;
}

static uint32_t xb_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void xb_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ---- FATX on E:, written here as the console's kernel writes it ----------------- */

static uint64_t xb_cluster_off(uint32_t c) {
    return XB_DATA + (uint64_t)(c - 1) * XB_CLUSTER;
}

static uint32_t xb_fat(const xb_guest *g, uint32_t c) {
    uint8_t raw[4];
    xb_get(g, XB_FAT + (uint64_t)c * 4, raw, 4);
    return xb_le32(raw);
}

static void xb_set_fat(xb_guest *g, uint32_t c, uint32_t v) {
    uint8_t raw[4];
    xb_put_le32(raw, v);
    xb_set(g, XB_FAT + (uint64_t)c * 4, raw, 4);
}

/* A formatted drive: FATX superblocks where the console has its partitions,
 * E: with an empty root, as xemu's published image comes. */
static void xb_format(xb_guest *g) {
    static const uint64_t partitions[] = { 0x80000ull, 0x2EE80000ull, 0x5DC80000ull, 0x8CA80000ull, XB_E };
    uint8_t sb[0x1000];
    memset(sb, 0xFF, sizeof(sb));
    memcpy(sb, "FATX", 4);
    xb_put_le32(sb + 4, 0x0D03A0u);
    xb_put_le32(sb + 8, XB_CLUSTER / 512);
    xb_put_le32(sb + 12, 1);
    sb[16] = sb[17] = 0;
    for (size_t i = 0; i < sizeof(partitions) / sizeof(partitions[0]); i++) xb_set(g, partitions[i], sb, sizeof(sb));
    xb_set_fat(g, 0, 0xFFFFFFF8u);
    xb_set_fat(g, 1, 0xFFFFFFFFu);
    uint8_t *root = (uint8_t *)malloc(XB_CLUSTER);
    memset(root, 0xFF, XB_CLUSTER);
    xb_set(g, xb_cluster_off(1), root, XB_CLUSTER);
    free(root);
}

static uint32_t xb_take(xb_guest *g) {
    for (uint32_t c = 2; c < XB_CLUSTERS; c++) {
        if (xb_fat(g, c) == 0) {
            xb_set_fat(g, c, 0xFFFFFFFFu);
            return c;
        }
    }
    fprintf(stderr, "xb_take: E: is full\n");
    abort();
}

/* Marks every free cluster of E: but `keep` of them used, as a full drive is. */
static void xb_fill(xb_guest *g, uint32_t keep) {
    uint32_t left = 0;
    for (uint32_t c = XB_CLUSTERS - 1; c >= 2; c--) {
        if (xb_fat(g, c) != 0) continue;
        if (left < keep) left++;
        else xb_set_fat(g, c, 0xFFFFFFFFu);
    }
}

static void xb_stamp(uint8_t *raw, uint16_t time, uint16_t date) {
    for (size_t i = 0; i < 3; i++) {
        raw[52 + i * 4] = (uint8_t)time;
        raw[53 + i * 4] = (uint8_t)(time >> 8);
        raw[54 + i * 4] = (uint8_t)date;
        raw[55 + i * 4] = (uint8_t)(date >> 8);
    }
}

/* Adds an entry to the directory at cluster `dir`, in its first deleted or end slot. */
static void xb_add_entry(xb_guest *g, uint32_t dir, const char *name, uint8_t attr, uint32_t first, uint32_t size,
                         uint16_t time, uint16_t date) {
    uint8_t raw[XB_SLOT];
    memset(raw, 0xFF, sizeof(raw));
    raw[0] = (uint8_t)strlen(name);
    raw[1] = attr;
    memcpy(raw + 2, name, strlen(name));
    xb_put_le32(raw + 44, first);
    xb_put_le32(raw + 48, size);
    xb_stamp(raw, time, date);
    for (uint32_t c = dir;;) {
        for (uint32_t i = 0; i < XB_CLUSTER; i += XB_SLOT) {
            uint8_t len;
            xb_get(g, xb_cluster_off(c) + i, &len, 1);
            if (len != 0xE5 && len != 0xFF && len != 0x00) continue;
            xb_set(g, xb_cluster_off(c) + i, raw, sizeof(raw));
            return;
        }
        uint32_t next = xb_fat(g, c);
        if (next >= 0xFFFFFFF0u) {
            next = xb_take(g);
            xb_set_fat(g, c, next);
            uint8_t *fresh = (uint8_t *)malloc(XB_CLUSTER);
            memset(fresh, 0xFF, XB_CLUSTER);
            xb_set(g, xb_cluster_off(next), fresh, XB_CLUSTER);
            free(fresh);
        }
        c = next;
    }
}

/* The first cluster of entry `name` in directory `dir`, compared without case; 0 when it has none. */
static uint32_t xb_lookup(const xb_guest *g, uint32_t dir, const char *name, uint64_t *slot) {
    for (uint32_t c = dir; c >= 1 && c < XB_CLUSTERS; c = xb_fat(g, c)) {
        for (uint32_t i = 0; i < XB_CLUSTER; i += XB_SLOT) {
            uint8_t raw[XB_SLOT];
            xb_get(g, xb_cluster_off(c) + i, raw, sizeof(raw));
            if (raw[0] == 0xFF || raw[0] == 0x00) return 0;
            if (raw[0] == 0xE5 || raw[0] != strlen(name)) continue;
            bool same = true;
            for (size_t k = 0; same && k < raw[0]; k++) {
                char a = (char)raw[2 + k], b = name[k];
                if (a >= 'a' && a <= 'z') a = (char)(a - 32);
                if (b >= 'a' && b <= 'z') b = (char)(b - 32);
                same = a == b;
            }
            if (!same) continue;
            if (slot) *slot = xb_cluster_off(c) + i;
            return xb_le32(raw + 44);
        }
    }
    return 0;
}

static uint32_t xb_mkdir(xb_guest *g, uint32_t dir, const char *name, uint16_t time, uint16_t date) {
    uint32_t c = xb_take(g);
    uint8_t *fresh = (uint8_t *)malloc(XB_CLUSTER);
    memset(fresh, 0xFF, XB_CLUSTER);
    xb_set(g, xb_cluster_off(c), fresh, XB_CLUSTER);
    free(fresh);
    xb_add_entry(g, dir, name, XB_DIR, c, 0, time, date);
    return c;
}

#define XB_TIME(h, m, s)   ((uint16_t)(((h) << 11) | ((m) << 5) | ((s) / 2)))
#define XB_DATE(y, mo, d)  ((uint16_t)((((y) - 2000) << 9) | ((mo) << 5) | (d)))

/* Writes a file at `path` on E: ("UDATA/4d530004/ABC/save.dat"), making the folders it needs. */
static void xb_put_at(xb_guest *g, const char *path, const void *data, size_t len, uint16_t time, uint16_t date) {
    uint32_t dir = 1;
    char part[64];
    const char *s = path;
    for (;;) {
        size_t n = strcspn(s, "/");
        memcpy(part, s, n);
        part[n] = '\0';
        if (!s[n]) break;
        uint32_t next = xb_lookup(g, dir, part, NULL);
        dir = next ? next : xb_mkdir(g, dir, part, time, date);
        s += n + 1;
    }
    uint32_t first = 0, last = 0;
    for (size_t done = 0; done < len; done += XB_CLUSTER) {
        uint32_t c = xb_take(g);
        if (last) xb_set_fat(g, last, c);
        else first = c;
        last = c;
        xb_set(g, xb_cluster_off(c), (const uint8_t *)data + done, len - done < XB_CLUSTER ? len - done : XB_CLUSTER);
    }
    xb_add_entry(g, dir, part, 0, first, (uint32_t)len, time, date);
}

static void xb_put(xb_guest *g, const char *path, const void *data, size_t len) {
    xb_put_at(g, path, data, len, XB_TIME(21, 27, 22), XB_DATE(2026, 10, 15));
}

static void xb_put_text(xb_guest *g, const char *path, const char *text) {
    xb_put(g, path, text, strlen(text));
}

/* ---- FATX read back ------------------------------------------------------------- */

typedef struct {
    char     path[512];
    uint8_t  attr;
    uint32_t first;
    uint32_t size;
    uint16_t time;
    uint16_t date;
    uint64_t slot;
} xb_node;

typedef struct {
    xb_node *items;
    size_t   count;
    size_t   cap;
    int      faults;
    uint32_t lost;   /* clusters the FAT marks used that no file or folder holds: wasted, not harmful */
    uint8_t *used;   /* one flag per cluster a file or folder holds */
} xb_tree;

static void xb_tree_free(xb_tree *t) {
    free(t->items);
    free(t->used);
    memset(t, 0, sizeof(*t));
}

static void xb_claim(xb_tree *t, const xb_guest *g, uint32_t first, uint32_t need, const char *path) {
    uint32_t have = 0;
    for (uint32_t c = first; c >= 1 && c < XB_CLUSTERS && have <= XB_CLUSTERS; c = xb_fat(g, c)) {
        if (t->used[c]) {
            fprintf(stderr, "FATX: cluster %u of %s belongs to two files\n", c, path);
            t->faults++;
            return;
        }
        if (xb_fat(g, c) == 0) {
            fprintf(stderr, "FATX: %s runs into free cluster %u\n", path, c);
            t->faults++;
            return;
        }
        t->used[c] = 1;
        have++;
        if (xb_fat(g, c) >= 0xFFFFFFF0u) break;
    }
    if (need != UINT32_MAX && have != need) {
        fprintf(stderr, "FATX: %s needs %u clusters and has %u\n", path, need, have);
        t->faults++;
    }
}

static void xb_walk_dir(xb_tree *t, const xb_guest *g, uint32_t dir, const char *prefix, int depth) {
    if (depth > 40) {
        t->faults++;
        return;
    }
    for (uint32_t c = dir; c >= 1 && c < XB_CLUSTERS; c = xb_fat(g, c)) {
        for (uint32_t i = 0; i < XB_CLUSTER; i += XB_SLOT) {
            uint8_t raw[XB_SLOT];
            xb_get(g, xb_cluster_off(c) + i, raw, sizeof(raw));
            if (raw[0] == 0xFF || raw[0] == 0x00) return;
            if (raw[0] == 0xE5 || raw[0] > 42) continue;
            if (t->count == t->cap) {
                t->cap = t->cap ? t->cap * 2 : 64;
                t->items = (xb_node *)realloc(t->items, t->cap * sizeof(*t->items));
            }
            xb_node *n = &t->items[t->count++];
            memset(n, 0, sizeof(*n));
            snprintf(n->path, sizeof(n->path), "%s%.*s", prefix, (int)raw[0], (const char *)raw + 2);
            n->attr = raw[1];
            n->first = xb_le32(raw + 44);
            n->size = xb_le32(raw + 48);
            n->time = (uint16_t)(raw[56] | (raw[57] << 8));
            n->date = (uint16_t)(raw[58] | (raw[59] << 8));
            n->slot = xb_cluster_off(c) + i;
            char path[512];
            snprintf(path, sizeof(path), "%s", n->path);
            uint32_t first = n->first;
            if (n->attr & XB_DIR) {
                char below[sizeof(path) + 2];
                snprintf(below, sizeof(below), "%s/", path);
                xb_claim(t, g, first, UINT32_MAX, path);
                xb_walk_dir(t, g, first, below, depth + 1);
            } else if (n->size) {
                xb_claim(t, g, first, (n->size + XB_CLUSTER - 1) / XB_CLUSTER, path);
            }
        }
        if (xb_fat(g, c) >= 0xFFFFFFF0u) return;
    }
}

/* Every file and folder on E:, and what is wrong with its chains: `faults`
 * counts a cluster two files hold, a chain into a free cluster, a file
 * whose chain isn't its size, and a cluster a file holds that the FAT marks
 * free; `lost` counts the clusters marked used that nothing holds. */
static void xb_walk(const xb_guest *g, xb_tree *t) {
    memset(t, 0, sizeof(*t));
    t->used = (uint8_t *)calloc(XB_CLUSTERS, 1);
    uint8_t magic[4];
    xb_get(g, XB_E, magic, 4);
    if (memcmp(magic, "FATX", 4) != 0) {
        fprintf(stderr, "FATX: E: has no superblock\n");
        t->faults++;
        return;
    }
    xb_claim(t, g, 1, UINT32_MAX, "(root)");
    xb_walk_dir(t, g, 1, "", 0);
    for (uint32_t c = 1; c < XB_CLUSTERS; c++) {
        if (xb_fat(g, c) != 0 && !t->used[c]) t->lost++;
        if (xb_fat(g, c) == 0 && t->used[c]) {
            fprintf(stderr, "FATX: cluster %u is marked free and held by a file\n", c);
            t->faults++;
            return;
        }
    }
}

static const xb_node *xb_node_at(const xb_tree *t, const char *path) {
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->items[i].path, path) == 0) return &t->items[i];
    }
    return NULL;
}

/* A file's bytes, malloc'd. */
static uint8_t *xb_file_bytes(const xb_guest *g, const xb_node *n) {
    uint8_t *data = (uint8_t *)malloc(n->size ? n->size : 1);
    size_t done = 0;
    for (uint32_t c = n->first; done < n->size && c >= 1 && c < XB_CLUSTERS; c = xb_fat(g, c)) {
        size_t take = n->size - done < XB_CLUSTER ? n->size - done : XB_CLUSTER;
        xb_get(g, xb_cluster_off(c), data + done, take);
        done += take;
    }
    return data;
}

static bool xb_file_is(const xb_guest *g, const xb_tree *t, const char *path, const void *data, size_t len) {
    const xb_node *n = xb_node_at(t, path);
    if (!n || (n->attr & XB_DIR) || n->size != len) return false;
    uint8_t *have = xb_file_bytes(g, n);
    bool same = memcmp(have, data, len) == 0;
    free(have);
    return same;
}

/* `path` is folder `top`/`id` or lies in it, compared without case as FATX compares names. */
static bool xb_in_folder(const char *path, const char *top, const char *id) {
    size_t nt = strlen(top), ni = strlen(id);
    for (size_t i = 0; i < nt + 1 + ni; i++) {
        char x = path[i], y = i < nt ? top[i] : i == nt ? '/' : id[i - nt - 1];
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y || !x) return false;
    }
    return path[nt + 1 + ni] == '\0' || path[nt + 1 + ni] == '/';
}

/* Folder `top`/`id` holds the same on `a` as on `b`: its own entry's times,
 * and every file and folder below it with its name, size, bytes and times. */
static bool xb_folder_same(const xb_guest *a, const xb_tree *ta, const xb_guest *b, const xb_tree *tb, const char *top,
                           const char *id) {
    size_t skip = strlen(top) + 1 + strlen(id), na = 0, nb = 0;
    for (size_t k = 0; k < tb->count; k++) nb += xb_in_folder(tb->items[k].path, top, id);
    for (size_t i = 0; i < ta->count; i++) {
        const xb_node *n = &ta->items[i], *m = NULL;
        if (!xb_in_folder(n->path, top, id)) continue;
        na++;
        for (size_t k = 0; k < tb->count && !m; k++) {
            const xb_node *o = &tb->items[k];
            if (xb_in_folder(o->path, top, id) && strcmp(o->path + skip, n->path + skip) == 0) m = o;
        }
        if (!m || m->attr != n->attr || m->size != n->size || m->time != n->time || m->date != n->date) return false;
        if (!(n->attr & XB_DIR) && n->size) {
            uint8_t *x = xb_file_bytes(a, n), *y = xb_file_bytes(b, m);
            bool same = memcmp(x, y, n->size) == 0;
            free(x);
            free(y);
            if (!same) return false;
        }
    }
    return na == nb;
}

/* ---- qcow2, written ------------------------------------------------------------- */

static void xb_put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void xb_put_be64(uint8_t *p, uint64_t v) {
    xb_put_be32(p, (uint32_t)(v >> 32));
    xb_put_be32(p + 4, (uint32_t)v);
}

static uint32_t xb_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t xb_be64(const uint8_t *p) {
    return ((uint64_t)xb_be32(p) << 32) | xb_be32(p + 4);
}

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} xb_bytes;

static void xb_reserve(xb_bytes *b, size_t len) {
    if (len <= b->cap) return;
    size_t cap = b->cap ? b->cap : 1u << 20;
    while (cap < len) cap *= 2;
    b->data = (uint8_t *)realloc(b->data, cap);
    memset(b->data + b->cap, 0, cap - b->cap);
    b->cap = cap;
}

/* The drive as a qcow2 file with `1 << bits`-byte clusters. Every
 * `compress_every`th cluster holding data is stored compressed, packed end
 * to end as qemu packs them (0 for none); `version` is 2 or 3. */
static void xb_qcow2(const xb_guest *g, unsigned bits, unsigned compress_every, unsigned version, uint8_t **out,
                     size_t *out_len) {
    size_t cs = (size_t)1 << bits, per_l2 = cs / 8, per_block = cs / 2;
    size_t l1_entries = (size_t)((XB_DISK_SIZE + (uint64_t)cs * per_l2 - 1) / ((uint64_t)cs * per_l2));
    size_t l1_clusters = (l1_entries * 8 + cs - 1) / cs;
    xb_bytes f = { NULL, 0, 0 };
    uint64_t l1_off = cs, rt_off = l1_off + l1_clusters * cs;
    uint64_t end = rt_off + cs;
    xb_reserve(&f, (size_t)end);
    uint16_t *counts = (uint16_t *)calloc(1u << 22, sizeof(*counts));   /* enough for a 4 GiB file of 1 KiB clusters */
    for (uint64_t c = 0; c < end / cs; c++) counts[c] = 1;
    uint64_t *l1 = (uint64_t *)calloc(l1_entries, sizeof(*l1));
    uint64_t packed_at = 0, packed_room = 0;
    unsigned seen = 0;
    uint8_t *piece = (uint8_t *)malloc(cs), *small = (uint8_t *)malloc(cs * 2);
    for (uint64_t index = 0; index < XB_DISK_SIZE / cs; index++) {
        const uint8_t *chunk = g->chunks[index * cs / XB_CHUNK];
        if (!chunk) {
            index += XB_CHUNK / cs - 1;
            continue;
        }
        memcpy(piece, chunk + (index * cs) % XB_CHUNK, cs);
        bool empty = true;
        for (size_t k = 0; empty && k < cs; k++) empty = piece[k] == 0;
        if (empty) continue;
        size_t i1 = (size_t)(index / per_l2);
        if (!l1[i1]) {
            l1[i1] = end | (1ull << 63);
            counts[end / cs] = 1;
            end += cs;
        }
        uint64_t l2_off = l1[i1] & 0x00FFFFFFFFFFFE00ull;
        uint64_t entry = 0;
        uLongf small_len = 0;
        if (compress_every && ++seen % compress_every == 0) {
            z_stream z;
            memset(&z, 0, sizeof(z));
            deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -12, 8, Z_DEFAULT_STRATEGY);
            z.next_in = piece;
            z.avail_in = (uInt)cs;
            z.next_out = small;
            z.avail_out = (uInt)(cs * 2);
            deflate(&z, Z_FINISH);
            small_len = z.total_out;
            deflateEnd(&z);
        }
        if (small_len && small_len < cs) {
            if (packed_room < small_len) {
                /* qemu carries on in the next cluster when it follows, else starts a new one. */
                if (packed_at + packed_room != end) {
                    packed_at = end;
                    packed_room = 0;
                }
                while (packed_room < small_len) {
                    packed_room += cs;
                    end += cs;
                }
            }
            for (uint64_t c = packed_at / cs; c <= (packed_at + small_len - 1) / cs; c++) counts[c]++;
            xb_reserve(&f, (size_t)(packed_at + small_len));
            memcpy(f.data + packed_at, small, small_len);
            uint64_t sectors = (packed_at + small_len - 1) / 512 - packed_at / 512;
            entry = packed_at | (sectors << (62 - (bits - 8))) | (1ull << 62);
            packed_at += small_len;
            packed_room -= small_len;
        } else {
            xb_reserve(&f, (size_t)(end + cs));
            memcpy(f.data + end, piece, cs);
            entry = end | (1ull << 63);
            counts[end / cs] = 1;
            end += cs;
        }
        xb_reserve(&f, (size_t)(l2_off + cs));
        xb_put_be64(f.data + l2_off + (index % per_l2) * 8, entry);
    }
    /* The reference count blocks go last, and count themselves. */
    uint64_t blocks = 0;
    while (blocks * per_block < end / cs + blocks) blocks++;
    xb_reserve(&f, (size_t)(end + blocks * cs));
    for (uint64_t b = 0; b < blocks; b++) {
        counts[end / cs + b] = 1;
        xb_put_be64(f.data + rt_off + b * 8, end + b * cs);
    }
    for (uint64_t c = 0; c < end / cs + blocks; c++) {
        uint8_t *at = f.data + end + (c / per_block) * cs + (c % per_block) * 2;
        at[0] = (uint8_t)(counts[c] >> 8);
        at[1] = (uint8_t)counts[c];
    }
    end += blocks * cs;
    for (size_t i = 0; i < l1_entries; i++) xb_put_be64(f.data + l1_off + i * 8, l1[i]);
    uint8_t *h = f.data;
    xb_put_be32(h, 0x514649FBu);
    xb_put_be32(h + 4, version);
    xb_put_be32(h + 20, bits);
    xb_put_be64(h + 24, XB_DISK_SIZE);
    xb_put_be32(h + 36, (uint32_t)l1_entries);
    xb_put_be64(h + 40, l1_off);
    xb_put_be64(h + 48, rt_off);
    xb_put_be32(h + 56, 1);
    if (version == 3) {
        xb_put_be32(h + 96, 4);
        xb_put_be32(h + 100, 112);
    }
    free(counts);
    free(l1);
    free(piece);
    free(small);
    *out = f.data;
    *out_len = (size_t)end;
}

/* ---- qcow2, read back and checked ------------------------------------------------ */

typedef struct {
    int      faults;   /* a reference the counts don't cover, a flag that is wrong, a table outside the file */
    uint64_t leaks;    /* clusters counted more often than they are referenced: wasted, not harmful */
} xb_check;

/* Reads an image file into the drive it holds and checks its reference counts. */
static void xb_read_image(const uint8_t *img, size_t len, xb_guest *g, xb_check *check) {
    xb_init(g);
    memset(check, 0, sizeof(*check));
    if (len < 112 || xb_be32(img) != 0x514649FBu) {
        for (size_t i = 0; i < XB_CHUNKS && (uint64_t)(i + 1) * XB_CHUNK <= len; i++) {
            static const uint8_t zeros[XB_CHUNK];
            if (memcmp(img + i * XB_CHUNK, zeros, XB_CHUNK) != 0) xb_set(g, (uint64_t)i * XB_CHUNK, img + i * XB_CHUNK, XB_CHUNK);
        }
        return;
    }
    unsigned bits = xb_be32(img + 20);
    size_t cs = (size_t)1 << bits, per_l2 = cs / 8, per_block = cs / 2;
    uint32_t l1_entries = xb_be32(img + 36);
    uint64_t l1_off = xb_be64(img + 40), rt_off = xb_be64(img + 48);
    uint64_t rt_entries = (uint64_t)xb_be32(img + 56) * cs / 8;
    uint64_t file_clusters = (len + cs - 1) / cs;
    uint32_t *need = (uint32_t *)calloc(file_clusters + 1, sizeof(*need));
#define XB_REF(off, bytes, what)                                                                   \
    for (uint64_t c_ = (off) / cs; c_ <= ((off) + (bytes) - 1) / cs; c_++) {                       \
        if (c_ >= file_clusters) {                                                                 \
            fprintf(stderr, "qcow2: %s at %#llx lies past the end of the file\n", what,           \
                    (unsigned long long)(off));                                                    \
            check->faults++;                                                                       \
            break;                                                                                 \
        }                                                                                          \
        need[c_]++;                                                                                \
    }
    XB_REF(0, cs, "the header");
    XB_REF(l1_off, (uint64_t)l1_entries * 8, "the L1 table");
    XB_REF(rt_off, rt_entries * 8, "the reference count table");
    for (uint64_t i = 0; i < rt_entries; i++) {
        uint64_t block = xb_be64(img + rt_off + i * 8) & ~511ull;
        if (block) XB_REF(block, cs, "a reference count block");
    }
    uint8_t *piece = (uint8_t *)malloc(cs);
    for (uint32_t i1 = 0; i1 < l1_entries && !check->faults; i1++) {
        uint64_t l1e = xb_be64(img + l1_off + (uint64_t)i1 * 8), l2_off = l1e & 0x00FFFFFFFFFFFE00ull;
        if (!l2_off) continue;
        XB_REF(l2_off, cs, "an L2 table");
        if (!(l1e >> 63)) {
            fprintf(stderr, "qcow2: L1 entry %u lacks the COPIED flag\n", i1);
            check->faults++;
        }
        for (size_t i2 = 0; i2 < per_l2 && !check->faults; i2++) {
            uint64_t e = xb_be64(img + l2_off + i2 * 8), index = (uint64_t)i1 * per_l2 + i2;
            if (e & (1ull << 62)) {
                unsigned shift = 62 - (bits - 8);
                uint64_t host = e & ((1ull << shift) - 1);
                uint64_t size = (((e >> shift) & ((1ull << (bits - 8)) - 1)) + 1) * 512 - (host & 511);
                XB_REF(host, size, "a compressed cluster");
                z_stream z;
                memset(&z, 0, sizeof(z));
                inflateInit2(&z, -12);
                z.next_in = (Bytef *)(img + host);
                z.avail_in = (uInt)(host + size > len ? len - host : size);
                z.next_out = piece;
                z.avail_out = (uInt)cs;
                inflate(&z, Z_FINISH);
                if (z.avail_out != 0) {
                    fprintf(stderr, "qcow2: compressed cluster %llu doesn't unpack\n", (unsigned long long)index);
                    check->faults++;
                }
                inflateEnd(&z);
                xb_set(g, index * cs, piece, cs);
                continue;
            }
            uint64_t host = e & 0x00FFFFFFFFFFFE00ull;
            if (!host) continue;
            if (host % cs) {
                fprintf(stderr, "qcow2: cluster %llu isn't on a cluster boundary\n", (unsigned long long)index);
                check->faults++;
                continue;
            }
            XB_REF(host, cs, "a data cluster");
            if (!(e >> 63)) {
                fprintf(stderr, "qcow2: data cluster %llu lacks the COPIED flag\n", (unsigned long long)index);
                check->faults++;
            }
            if (!(e & 1) && host + cs <= len) xb_set(g, index * cs, img + host, cs);
        }
    }
    for (uint64_t c = 0; c < file_clusters && !check->faults; c++) {
        uint64_t block = c / per_block < rt_entries ? xb_be64(img + rt_off + (c / per_block) * 8) & ~511ull : 0;
        uint32_t have = block ? (uint32_t)((img[block + (c % per_block) * 2] << 8) | img[block + (c % per_block) * 2 + 1]) : 0;
        if (have < need[c]) {
            fprintf(stderr, "qcow2: cluster %llu is counted %u times and referenced %u times\n",
                    (unsigned long long)c, have, need[c]);
            check->faults++;
        } else if (have > need[c]) {
            check->leaks++;
        }
    }
#undef XB_REF
    free(piece);
    free(need);
}

/* ---- what a restore may change -------------------------------------------------- */

static bool xb_same_name(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return false;
    }
    return true;
}

/* `path` is the game's folder under UDATA or TDATA, or lies in it. */
static bool xb_is_games(const char *path, const char *id) {
    static const char *const tops[] = { "UDATA/", "TDATA/" };
    for (size_t t = 0; t < 2; t++) {
        size_t n = strlen(id);
        if (!xb_same_name(path, tops[t], 6) || strlen(path) < 6 + n || !xb_same_name(path + 6, id, n)) continue;
        if (path[6 + n] == '\0' || path[6 + n] == '/') return true;
    }
    return false;
}

static void xb_mark_chain(const xb_guest *g, uint32_t first, uint8_t *set) {
    for (uint32_t c = first; c >= 1 && c < XB_CLUSTERS && !set[c]; c = xb_fat(g, c)) {
        set[c] = 1;
        if (xb_fat(g, c) >= 0xFFFFFFF0u || xb_fat(g, c) == 0) break;
    }
}

/* The clusters the game's folders and files hold on `g`. */
static void xb_game_clusters(const xb_guest *g, const xb_tree *t, const char *id, uint8_t *set) {
    for (size_t i = 0; i < t->count; i++) {
        const xb_node *n = &t->items[i];
        if (xb_is_games(n->path, id) && ((n->attr & XB_DIR) || n->size)) xb_mark_chain(g, n->first, set);
    }
}

/* Puts every directory slot of the chain at `dir` on `before` that holds no
 * other title's live entry back into `clone` as `before` has it. */
static void xb_restore_free_slots(const xb_guest *before, xb_guest *clone, uint32_t dir, const char *id) {
    bool ended = false;
    for (uint32_t c = dir; c >= 1 && c < XB_CLUSTERS; c = xb_fat(before, c)) {
        for (uint32_t i = 0; i < XB_CLUSTER; i += XB_SLOT) {
            uint8_t raw[XB_SLOT];
            xb_get(before, xb_cluster_off(c) + i, raw, sizeof(raw));
            if (raw[0] == 0xFF || raw[0] == 0x00) ended = true;
            bool live = !ended && raw[0] != 0xE5 && raw[0] <= 42;
            bool games = live && raw[0] == strlen(id) && xb_same_name((const char *)raw + 2, id, raw[0]);
            if (!live || games) xb_set(clone, xb_cluster_off(c) + i, raw, sizeof(raw));
        }
        if (xb_fat(before, c) >= 0xFFFFFFF0u) break;
    }
}

/* True when `after` differs from `before` only in what belongs to game
 * `id`: the clusters of its folders and files and their FAT entries, which
 * were its own or free before, its entry in UDATA and TDATA, and those two
 * folders themselves where the drive had none. Every other byte of the
 * drive, on any partition, must be the same. A restore that `stopped` part
 * way may also leave clusters it took marked used with nothing holding them,
 * and bytes it wrote in clusters still free, which nothing reads. */
static bool xb_only_game_changed(const xb_guest *before, const xb_guest *after, const char *id, bool stopped) {
    xb_tree tb, ta;
    xb_walk(before, &tb);
    xb_walk(after, &ta);
    uint8_t *was = (uint8_t *)calloc(XB_CLUSTERS, 1), *is = (uint8_t *)calloc(XB_CLUSTERS, 1);
    xb_game_clusters(before, &tb, id, was);
    xb_game_clusters(after, &ta, id, is);
    for (uint32_t c = 1; stopped && c < XB_CLUSTERS; c++) {
        if (xb_fat(before, c) != 0) continue;
        if (xb_fat(after, c) != 0 ? !ta.used[c] : !xb_same_bytes(before, after, xb_cluster_off(c), XB_CLUSTER)) is[c] = 1;
    }
    static const char *const tops[] = { "UDATA", "TDATA" };
    xb_guest clone;
    xb_copy(after, &clone);
    bool ok = true;
    for (size_t t = 0; t < 2; t++) {
        const xb_node *old = xb_node_at(&tb, tops[t]), *now = xb_node_at(&ta, tops[t]);
        if (old) {
            /* The folder's own chain may have grown by clusters that were free. */
            uint8_t *chain = (uint8_t *)calloc(XB_CLUSTERS, 1);
            xb_mark_chain(before, old->first, chain);
            uint32_t last = old->first;
            while (xb_fat(before, last) < 0xFFFFFFF0u) last = xb_fat(before, last);
            uint8_t raw[4];
            xb_get(before, XB_FAT + (uint64_t)last * 4, raw, 4);
            xb_set(&clone, XB_FAT + (uint64_t)last * 4, raw, 4);
            if (now) {
                for (uint32_t c = now->first; c >= 1 && c < XB_CLUSTERS; c = xb_fat(after, c)) {
                    if (!chain[c]) is[c] = 1;
                    if (xb_fat(after, c) >= 0xFFFFFFF0u) break;
                }
            }
            free(chain);
            xb_restore_free_slots(before, &clone, old->first, id);
        } else if (now) {
            xb_mark_chain(after, now->first, is);
            xb_restore_free_slots(before, &clone, 1, "");
        }
    }
    for (uint32_t c = 1; c < XB_CLUSTERS; c++) {
        if (!was[c] && !is[c]) continue;
        if (is[c] && !was[c] && xb_fat(before, c) != 0) {
            fprintf(stderr, "  cluster %u went to the game and wasn't free\n", c);
            ok = false;
        }
        uint8_t raw[4];
        xb_get(before, XB_FAT + (uint64_t)c * 4, raw, 4);
        xb_set(&clone, XB_FAT + (uint64_t)c * 4, raw, 4);
        uint8_t *data = (uint8_t *)malloc(XB_CLUSTER);
        xb_get(before, xb_cluster_off(c), data, XB_CLUSTER);
        xb_set(&clone, xb_cluster_off(c), data, XB_CLUSTER);
        free(data);
    }
    uint64_t at = xb_first_difference(before, &clone);
    if (at != UINT64_MAX) {
        fprintf(stderr, "  byte %#llx of the drive changed and isn't the game's\n", (unsigned long long)at);
        ok = false;
    }
    xb_free(&clone);
    free(was);
    free(is);
    xb_tree_free(&tb);
    xb_tree_free(&ta);
    return ok;
}

/* How many files and folders outside the game's on `before` aren't on
 * `after` with the same bytes, size, times and place; each is named. */
static int xb_others_changed(const xb_guest *before, const xb_guest *after, const char *id) {
    xb_tree tb, ta;
    int changed = 0;
    xb_walk(before, &tb);
    xb_walk(after, &ta);
    for (size_t i = 0; i < tb.count; i++) {
        const xb_node *n = &tb.items[i];
        if (xb_is_games(n->path, id)) continue;
        const xb_node *m = xb_node_at(&ta, n->path);
        bool same = m && m->attr == n->attr && m->size == n->size && m->first == n->first && m->time == n->time &&
                    m->date == n->date && m->slot == n->slot;
        if (same && !(n->attr & XB_DIR)) {
            uint8_t *x = xb_file_bytes(before, n), *y = xb_file_bytes(after, m);
            same = memcmp(x, y, n->size) == 0;
            free(x);
            free(y);
        }
        if (!same) {
            fprintf(stderr, "  %s isn't as it was\n", n->path);
            changed++;
        }
    }
    xb_tree_free(&tb);
    xb_tree_free(&ta);
    return changed;
}

#endif
