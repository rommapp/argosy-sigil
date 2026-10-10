// SPDX-License-Identifier: MPL-2.0
/* A Wii disc's save_id comes from its game partition's ticket, read through
 * each container: a plain image, WBFS with its clusters out of order, and WIA
 * and RVZ in every compression sigil decodes, RVZ packing included. A disc
 * that installs a channel carries 00010004; one whose ticket can't be read
 * gets no save_id rather than a guessed category. */
#include "sigil.h"
#include "sigil_internal.h"
#include <LzmaEnc.h>
#include <stdio.h>
#include <stdlib.h>
#include <zstd.h>

#define PARTITION      0x50000u
#define DISC_LEN       0x58000u
#define CHUNK          0x8000u
#define JUNK_AT        0x48000u   /* a run of padding the RVZ packs from a seed */
#define JUNK_LEN       0x1000u

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

/* A disc with the game partition at PARTITION, listed in partition group
 * `group` after an update partition, its ticket naming `high` and the code. */
static uint8_t *make_disc(uint32_t high, const char *code, uint32_t group, uint32_t game_type) {
    uint8_t *d = (uint8_t *)calloc(1, DISC_LEN);
    memcpy(d, code, 4);
    put_be32(d + 0x18, 0x5D1C9EA3u);
    uint32_t table = 0x40020;
    put_be32(d + 0x40000 + group * 8, 2);
    put_be32(d + 0x40000 + group * 8 + 4, table >> 2);
    put_be32(d + table, 0x48000 >> 2);
    put_be32(d + table + 4, 1);
    put_be32(d + table + 8, PARTITION >> 2);
    put_be32(d + table + 12, game_type);
    put_be32(d + 0x48000 + 0x1DC, 0x00010008);
    put_be32(d + PARTITION + 0x1DC, high);
    memcpy(d + PARTITION + 0x1E0, code, 4);
    return d;
}

typedef struct { const uint8_t *buf; size_t len; } mem_ctx;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_ctx *m = (mem_ctx *)ctx;
    if (off >= m->len) return 0;
    size_t n = len < m->len - (size_t)off ? len : m->len - (size_t)off;
    memcpy(buf, m->buf + off, n);
    return (int)n;
}

static int64_t mem_size(void *ctx) { return (int64_t)((mem_ctx *)ctx)->len; }

static void expect(const char *where, const uint8_t *file, size_t len, const char *name, const char *want_save) {
    mem_ctx ctx = { file, len };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, name, SIGIL_PLATFORM_WII, NULL, &r);
    char what[160];
    snprintf(what, sizeof(what), "rc %d, save_id '%s', want '%s'", rc, rc == SIGIL_OK ? r.save_id : "", want_save);
    if (rc != SIGIL_OK || strcmp(r.save_id, want_save) != 0 || r.usage != SIGIL_USAGE_FOLDER_SPLIT) fail(where, what);
}

/* ---- WBFS ---------------------------------------------------------------- */

/* The disc's 32 KiB clusters stored in reverse order after the header. */
static uint8_t *make_wbfs(const uint8_t *disc, size_t *len) {
    size_t clusters = DISC_LEN / CHUNK, first = 2;
    *len = (first + clusters) * CHUNK;
    uint8_t *f = (uint8_t *)calloc(1, *len);
    memcpy(f, "WBFS", 4);
    put_be32(f + 4, (uint32_t)(*len / 512));
    f[8] = 9;
    f[9] = 15;
    f[12] = 1;
    memcpy(f + 512, disc, 0x100);
    for (size_t i = 0; i < clusters; i++) {
        size_t at = first + clusters - 1 - i;
        put_be16(f + 512 + 0x100 + i * 2, (uint16_t)at);
        memcpy(f + at * CHUNK, disc + i * CHUNK, CHUNK);
    }
    return f;
}

/* ---- WIA and RVZ --------------------------------------------------------- */

enum { NONE = 0, PURGE = 1, BZIP2 = 2, LZMA = 3, ZSTD = 5 };

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} buf;

static void append(buf *b, const void *data, size_t len) {
    if (b->len + len > b->cap) {
        b->cap = (b->len + len) * 2;
        b->data = (uint8_t *)realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

static void align4(buf *b) {
    static const uint8_t zero[4] = {0};
    if (b->len % 4) append(b, zero, 4 - b->len % 4);
}

static uint8_t g_lzma_props[LZMA_PROPS_SIZE];

static void *lz_alloc(ISzAllocPtr p, size_t size) {
    (void)p;
    return malloc(size);
}

static void lz_free(ISzAllocPtr p, void *address) {
    (void)p;
    free(address);
}

static buf compress(int method, const uint8_t *data, size_t len) {
    buf out = {0};
    if (method == NONE) {
        append(&out, data, len);
    } else if (method == PURGE) {
        uint8_t seg[8];
        put_be32(seg, 0);
        put_be32(seg + 4, (uint32_t)len);
        append(&out, seg, 8);
        append(&out, data, len);
        uint8_t sha[20] = {0};
        append(&out, sha, sizeof(sha));
    } else if (method == ZSTD) {
        out.cap = ZSTD_compressBound(len);
        out.data = (uint8_t *)malloc(out.cap);
        out.len = ZSTD_compress(out.data, out.cap, data, len, 3);
    } else if (method == LZMA) {
        CLzmaEncProps props;
        LzmaEncProps_Init(&props);
        props.dictSize = 1u << 16;
        ISzAlloc alloc = { lz_alloc, lz_free };
        SizeT dest = len + len / 2 + 1024, props_size = LZMA_PROPS_SIZE;
        out.data = (uint8_t *)malloc(dest);
        LzmaEncode(out.data, &dest, data, len, &props, g_lzma_props, &props_size, 0, NULL, &alloc, &alloc);
        out.len = dest;
        out.cap = dest;
    } else {
        uint8_t garbage[16] = { 'B', 'Z', 'h', '9' };
        append(&out, garbage, sizeof(garbage));
    }
    return out;
}

/* Group `i`'s RVZ packing: stored bytes, with the padding run that starts at
 * JUNK_AT on the disc given as a seed. */
static buf rvz_pack(const uint8_t *disc, size_t start, size_t len) {
    buf out = {0};
    uint8_t word[4];
    size_t at = start, end = start + len;
    while (at < end) {
        bool junk = at == JUNK_AT;
        size_t run = junk ? JUNK_LEN : (at < JUNK_AT && end > JUNK_AT ? JUNK_AT - at : end - at);
        put_be32(word, (uint32_t)run | (junk ? 0x80000000u : 0));
        append(&out, word, 4);
        if (junk) {
            uint8_t seed[68];
            for (size_t i = 0; i < sizeof(seed); i++) seed[i] = (uint8_t)(i * 7 + 1);
            append(&out, seed, sizeof(seed));
        } else {
            append(&out, disc + at, run);
        }
        at += run;
    }
    return out;
}

/* `disc` as a WIA or RVZ: one raw data entry from 0x80 to the end, in
 * `chunk`-sized groups. With RVZ and 32 KiB groups, the group holding the
 * ticket is stored uncompressed while the one holding the partition table is
 * compressed, and the group holding JUNK_AT is packed. */
static uint8_t *make_wia(const uint8_t *disc, bool rvz, int method, uint32_t chunk, size_t *len) {
    size_t groups = (DISC_LEN + chunk - 1) / chunk;
    size_t entry = rvz ? 12 : 8;
    buf data = {0};
    uint8_t *table = (uint8_t *)calloc(groups, entry);
    size_t data_start = 0x400;
    for (size_t i = 0; i < groups; i++) {
        size_t start = i * chunk, glen = DISC_LEN - start < chunk ? DISC_LEN - start : chunk;
        bool packed = rvz && start <= JUNK_AT && JUNK_AT < start + glen;
        bool stored = rvz && chunk == CHUNK && start == PARTITION;
        buf raw = packed ? rvz_pack(disc, start, glen) : (buf){ (uint8_t *)disc + start, glen, 0 };
        buf c = compress(stored ? NONE : method, raw.data, raw.len);
        align4(&data);
        uint8_t *e = table + i * entry;
        put_be32(e, (uint32_t)((data_start + data.len) >> 2));
        put_be32(e + 4, (uint32_t)c.len | (rvz && !stored ? 0x80000000u : 0));
        if (rvz) put_be32(e + 8, packed ? (uint32_t)raw.len : 0);
        append(&data, c.data, c.len);
        free(c.data);
        if (packed) free(raw.data);
    }
    uint8_t raw_entry[0x18];
    put_be64(raw_entry, 0x80);
    put_be64(raw_entry + 8, DISC_LEN - 0x80);
    put_be32(raw_entry + 16, 0);
    put_be32(raw_entry + 20, (uint32_t)groups);
    buf raws = compress(method, raw_entry, sizeof(raw_entry));
    buf groups_c = compress(method, table, groups * entry);

    size_t raws_at = data_start + ((data.len + 3) & ~(size_t)3);
    size_t groups_at = raws_at + ((raws.len + 3) & ~(size_t)3);
    *len = groups_at + groups_c.len;
    uint8_t *f = (uint8_t *)calloc(1, *len);
    memcpy(f, rvz ? "RVZ\x01" : "WIA\x01", 4);
    put_be32(f + 4, 0x01000000);
    put_be32(f + 8, 0x00090000);
    put_be32(f + 0x0C, 0xDC);
    put_be64(f + 0x24, DISC_LEN);
    put_be64(f + 0x2C, *len);
    uint8_t *h = f + 0x48;
    put_be32(h, 2);
    put_be32(h + 0x04, (uint32_t)method);
    put_be32(h + 0x0C, chunk);
    memcpy(h + 0x10, disc, 0x80);
    put_be32(h + 0x94, 0x30);
    put_be32(h + 0xB4, 1);
    put_be64(h + 0xB8, raws_at);
    put_be32(h + 0xC0, (uint32_t)raws.len);
    put_be32(h + 0xC4, (uint32_t)groups);
    put_be64(h + 0xC8, groups_at);
    put_be32(h + 0xD0, (uint32_t)groups_c.len);
    if (method == LZMA) {
        h[0xD4] = LZMA_PROPS_SIZE;
        memcpy(h + 0xD5, g_lzma_props, LZMA_PROPS_SIZE);
    }
    memcpy(f + data_start, data.data, data.len);
    memcpy(f + raws_at, raws.data, raws.len);
    memcpy(f + groups_at, groups_c.data, groups_c.len);
    free(data.data);
    free(raws.data);
    free(groups_c.data);
    free(table);
    return f;
}

static void check_containers(const char *code, uint32_t high, const char *want) {
    uint8_t *disc = make_disc(high, code, 0, 0);
    char where[64];
    snprintf(where, sizeof(where), "%s iso", code);
    expect(where, disc, DISC_LEN, "game.iso", want);

    size_t len = 0;
    uint8_t *wbfs = make_wbfs(disc, &len);
    snprintf(where, sizeof(where), "%s wbfs", code);
    expect(where, wbfs, len, "game.wbfs", want);
    free(wbfs);

    static const struct { bool rvz; int method; uint32_t chunk; const char *name; } WIAS[] = {
        { true, ZSTD, CHUNK, "rvz zstd" },          { true, NONE, CHUNK, "rvz none" },
        { true, ZSTD, 4 * CHUNK, "rvz zstd 128k" }, { false, NONE, 0x200000, "wia none" },
        { false, PURGE, 0x200000, "wia purge" },    { false, LZMA, 0x200000, "wia lzma" },
    };
    for (size_t i = 0; i < sizeof(WIAS) / sizeof(WIAS[0]); i++) {
        uint8_t *f = make_wia(disc, WIAS[i].rvz, WIAS[i].method, WIAS[i].chunk, &len);
        snprintf(where, sizeof(where), "%s %s", code, WIAS[i].name);
        expect(where, f, len, WIAS[i].rvz ? "game.rvz" : "game.wia", want);
        free(f);
    }
    free(disc);
}

static void check_no_guess(void) {
    uint8_t *disc = make_disc(0x00010000, "RSPE", 0, 1);
    expect("no game partition", disc, DISC_LEN, "game.iso", "");
    free(disc);

    disc = make_disc(0x00010001, "RSPE", 0, 0);
    expect("ticket names no disc title", disc, DISC_LEN, "game.iso", "");
    free(disc);

    disc = make_disc(0x00010004, "RMCE", 0, 0);
    size_t len = 0;
    uint8_t *f = make_wia(disc, false, BZIP2, 0x200000, &len);
    expect("wia bzip2", f, len, "game.wia", "");
    free(f);
    free(disc);

    disc = make_disc(0x00010004, "RMCE", 2, 0);
    expect("game partition in the third group", disc, DISC_LEN, "game.iso", "00010004/524d4345");
    free(disc);
}

int main(void) {
    check_containers("RMCE", 0x00010004, "00010004/524d4345");
    check_containers("RSPE", 0x00010000, "00010000/52535045");
    check_no_guess();
    printf("wii disc: %d failures\n", g_fails);
    return g_fails ? 1 : 0;
}
