// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include "test_fs.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Builds PS3 packages (.pkg) the way RPCS3 reads them (rpcs3/Crypto/unpkg.h
 * and unpkg.cpp at c05ef18) and checks the folder name sigil takes as the
 * title id: the nine characters at 0x37 of the content id, or the name a
 * metadata packet 0xA gives instead. Each package carries the metadata
 * packets 30 of 33 real packages carry, in their order and sizes. The bytes
 * past the metadata stand in for the encrypted data and are never read.
 * Expected ids are written out by hand. */

#define PKG_LEN         0x800u
#define PKG_META        0xC0u
#define PKG_RETAIL      0x8000u
#define PKG_DEBUG       0x0000u
#define PKG_PLATFORM_PS3  0x0001u
#define PKG_PLATFORM_PSP  0x0002u

static int failures = 0;

typedef struct { const uint8_t *buf; size_t len; } mem_ctx;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_ctx *m = (mem_ctx *)ctx;
    if (off >= m->len) return 0;
    size_t avail = m->len - (size_t)off;
    size_t n = len < avail ? len : avail;
    memcpy(buf, m->buf + off, n);
    return (int)n;
}
static int64_t mem_size(void *ctx) { return (int64_t)((mem_ctx *)ctx)->len; }

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v) {
    put_be16(p, (uint16_t)(v >> 16));
    put_be16(p + 2, (uint16_t)v);
}
static void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

/* A metadata packet: its id, the size it declares, and the bytes written for
 * it (fewer than `size` when the test wants it to run past the file). */
typedef struct {
    uint32_t       id;
    uint32_t       size;
    const uint8_t *data;
    uint32_t       data_len;
} packet;

/* DRM type, content type, package type, package size, revision, QA digest,
 * software revision, 0x9: what real packages carry, minus their values. */
static const packet REAL_PACKETS[] = {
    { 0x1, 0x04, NULL, 0x04 }, { 0x2, 0x04, NULL, 0x04 }, { 0x3, 0x04, NULL, 0x04 },
    { 0x4, 0x08, NULL, 0x08 }, { 0x5, 0x04, NULL, 0x04 }, { 0x7, 0x18, NULL, 0x18 },
    { 0x8, 0x08, NULL, 0x08 }, { 0x9, 0x08, NULL, 0x08 },
};
#define REAL_COUNT (sizeof(REAL_PACKETS) / sizeof(REAL_PACKETS[0]))

/* A 0xA packet's body: eight bytes RPCS3 skips, then the folder name. */
static size_t install_dir(uint8_t *out, size_t cap, const char *name) {
    memset(out, 0, cap);
    size_t n = strlen(name);
    memcpy(out + 8, name, n);
    return 8 + n;
}

/* A package of PKG_LEN bytes: the header fields RPCS3's read_header checks,
 * the content id at 0x30, the real packets followed by `extra`, and noise
 * from the end of the metadata on. Returns where the metadata ends. */
static size_t build_pkg(uint8_t *p, uint16_t release, uint16_t platform, const char *content_id,
                        const packet *extra, size_t n_extra) {
    memset(p, 0, PKG_LEN);
    memcpy(p, "\x7FPKG", 4);
    put_be16(p + 0x04, release);
    put_be16(p + 0x06, platform);
    put_be32(p + 0x08, PKG_META);                         /* meta_offset */
    put_be32(p + 0x0C, (uint32_t)(REAL_COUNT + n_extra)); /* meta_count */
    put_be32(p + 0x14, 1);                                /* file_count */
    put_be64(p + 0x18, PKG_LEN);                          /* pkg_size */
    memcpy(p + 0x30, content_id, strlen(content_id));

    size_t at = PKG_META;
    for (size_t i = 0; i < REAL_COUNT + n_extra; i++) {
        const packet *k = i < REAL_COUNT ? &REAL_PACKETS[i] : &extra[i - REAL_COUNT];
        put_be32(p + at, k->id);
        put_be32(p + at + 4, k->size);
        at += 8;
        if (k->data) memcpy(p + at, k->data, k->data_len);
        else memset(p + at, 0x11, k->data_len);
        at += k->data_len;
    }
    size_t meta_end = at;
    size_t data_off = (meta_end + 0x3F) & ~(size_t)0x3F;
    put_be32(p + 0x10, (uint32_t)(meta_end - PKG_META));  /* meta_size */
    put_be64(p + 0x20, data_off);                         /* data_offset */
    put_be64(p + 0x28, PKG_LEN - data_off);               /* data_size */
    uint32_t x = 0x9E3779B9u;
    for (size_t i = meta_end; i < PKG_LEN; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        p[i] = (uint8_t)x;
    }
    return meta_end;
}

/* What RomM asks: the ps3 platform named, no filename fallback. */
static int extract(const uint8_t *buf, size_t len, sigil_result *r) {
    mem_ctx ctx = { buf, len };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_options opts = { .struct_version = SIGIL_OPTIONS_V1, .flags = 0 };
    return sigil_extract_from_io(&io, "game.pkg", SIGIL_PLATFORM_PS3, &opts, r);
}

static void expect_id(const char *name, const uint8_t *buf, size_t len, const char *want) {
    sigil_result r;
    memset(&r, 0, sizeof(r));
    int rc = extract(buf, len, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL %s: rc=%d (%s)\n", name, rc, sigil_strerror(rc));
        failures++;
        return;
    }
    if (strcmp(r.title_id, want) != 0 || strcmp(r.save_id, want) != 0
        || strcmp(r.raw_serial, want) != 0) {
        fprintf(stderr, "FAIL %s: title_id='%s' save_id='%s' raw_serial='%s', want %s\n",
                name, r.title_id, r.save_id, r.raw_serial, want);
        failures++;
        return;
    }
    if (r.platform != SIGIL_PLATFORM_PS3 || r.usage != SIGIL_USAGE_FOLDER_PREFIX
        || r.source != SIGIL_SOURCE_BINARY || r.experimental != 1) {
        fprintf(stderr, "FAIL %s: platform=%d usage=%d source=%d experimental=%d\n",
                name, (int)r.platform, (int)r.usage, (int)r.source, r.experimental);
        failures++;
        return;
    }
    printf("ok %s -> %s\n", name, r.title_id);
}

static void expect_refused(const char *name, const uint8_t *buf, size_t len, int want_rc) {
    sigil_result r;
    memset(&r, 0, sizeof(r));
    int rc = extract(buf, len, &r);
    if (rc != want_rc || r.title_id[0] != '\0' || r.save_id[0] != '\0') {
        fprintf(stderr, "FAIL %s: rc=%d title_id='%s' save_id='%s', want rc=%d and no id\n",
                name, rc, r.title_id, r.save_id, want_rc);
        failures++;
        return;
    }
    printf("ok %s refused (%s)\n", name, sigil_strerror(rc));
}

/* The same package through a path, as RomM's scan hands it over, and without
 * a platform: PSP and Vita packages share the extension and the magic, so the
 * extension alone names no platform. */
static void check_path(const uint8_t *buf) {
    char dir[512], path[600];
    if (test_temp_dir(dir, sizeof(dir)) != 0) {
        fprintf(stderr, "FAIL path: no temp folder\n");
        failures++;
        return;
    }
    snprintf(path, sizeof(path), "%s/Flower.pkg", dir);
    FILE *f = test_fopen(path, "wb");
    if (!f || fwrite(buf, 1, PKG_LEN, f) != PKG_LEN) {
        fprintf(stderr, "FAIL path: can't write %s\n", path);
        if (f) fclose(f);
        failures++;
        test_remove_tree(dir);
        return;
    }
    fclose(f);

    sigil_options opts = { .struct_version = SIGIL_OPTIONS_V1, .flags = 0 };
    sigil_result r;
    memset(&r, 0, sizeof(r));
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_PS3, &opts, &r);
    if (rc != SIGIL_OK || strcmp(r.title_id, "NPUA80083") != 0
        || strcmp(r.save_id, "NPUA80083") != 0) {
        fprintf(stderr, "FAIL path ps3: rc=%d title_id='%s' save_id='%s'\n",
                rc, r.title_id, r.save_id);
        failures++;
    } else {
        printf("ok path ps3 -> %s\n", r.title_id);
    }

    memset(&r, 0, sizeof(r));
    rc = sigil_extract_from_path(path, SIGIL_PLATFORM_AUTO, &opts, &r);
    if (rc != SIGIL_ERR_UNKNOWN_PLATFORM) {
        fprintf(stderr, "FAIL path auto: rc=%d platform=%d title_id='%s', want unknown platform\n",
                rc, (int)r.platform, r.title_id);
        failures++;
    } else {
        printf("ok path auto needs the ps3 hint\n");
    }
    test_remove_tree(dir);
}

#define FLOWER   "UP9000-NPUA80083_00-FLOWERPS3PRIVATE"
#define A_DLC    "UP0101-NPUB30097_00-SOMEDLCPACKAGE01"

int main(void) {
    uint8_t *pkg = malloc(PKG_LEN);
    if (!pkg) return 1;
    uint8_t body[64], body2[64];
    size_t end;

    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, FLOWER, NULL, 0);
    expect_id("retail", pkg, PKG_LEN, "NPUA80083");
    check_path(pkg);

    build_pkg(pkg, PKG_DEBUG, PKG_PLATFORM_PS3, "EP9000-NPEA00143_00-DEADNATFULL00001", NULL, 0);
    expect_id("debug", pkg, PKG_LEN, "NPEA00143");

    /* The header and the metadata are all that is read: a package cut short
     * right after its metadata still names its game. */
    end = build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, "EP9000-NPEA00014_00-STARDUSTFULL0001", NULL, 0);
    expect_id("cut after the metadata", pkg, end, "NPEA00014");
    expect_refused("cut inside the metadata", pkg, end - 4, SIGIL_ERR_UNSUPPORTED_FORMAT);
    expect_refused("cut inside the content id", pkg, 0x3C, SIGIL_ERR_IO);

    /* RPCS3 installs into the folder a 0xA packet names, over the content id's. */
    packet dir = { 0xA, 0, body, 0 };
    dir.size = dir.data_len = (uint32_t)install_dir(body, 0x20, "NPUB30096") + 7;
    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, A_DLC, &dir, 1);
    expect_id("0xA names the folder", pkg, PKG_LEN, "NPUB30096");

    /* A 0xA of eight bytes or fewer is skipped, by its size, and the
     * content id's name stands until a 0xA that has one. */
    packet too_short = { 0xA, 8, body2, 8 };
    memset(body2, 0x41, 8);
    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, A_DLC, &too_short, 1);
    expect_id("0xA of eight bytes ignored", pkg, PKG_LEN, "NPUB30097");
    packet then[2] = { { 0xA, 4, body2, 4 }, dir };
    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, A_DLC, then, 2);
    expect_id("short 0xA skipped, the next read", pkg, PKG_LEN, "NPUB30096");

    /* Each 0xA replaces the name before it. */
    packet later = { 0xA, 0, body2, 0 };
    later.size = later.data_len = (uint32_t)install_dir(body2, 0x20, "NPUB30095");
    packet both[2] = { dir, later };
    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, A_DLC, both, 2);
    expect_id("the later 0xA wins", pkg, PKG_LEN, "NPUB30095");

    packet bad = { 0xA, 0, body2, 0 };
    bad.size = bad.data_len = (uint32_t)install_dir(body2, 0x20, "../../xyz");
    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, A_DLC, &bad, 1);
    expect_refused("0xA names a path", pkg, PKG_LEN, SIGIL_ERR_NOT_FOUND);

    packet empty = { 0xA, 12, body2, 12 };
    memset(body2, 0, 12);
    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, A_DLC, &empty, 1);
    expect_refused("0xA names nothing", pkg, PKG_LEN, SIGIL_ERR_NOT_FOUND);

    packet longer = { 0xA, 0, body2, 0 };
    longer.size = longer.data_len = (uint32_t)install_dir(body2, sizeof(body2), "NPUB30096ABCDEFGHIJKLMNOPQRSTUVW");
    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, A_DLC, &longer, 1);
    expect_refused("0xA name of 32 characters", pkg, PKG_LEN, SIGIL_ERR_NOT_FOUND);
    longer.size = longer.data_len = (uint32_t)install_dir(body2, sizeof(body2), "NPUB30096ABCDEFGHIJKLMNOPQRSTUV");
    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, A_DLC, &longer, 1);
    expect_id("0xA name of 31 characters", pkg, PKG_LEN, "NPUB30096ABCDEFGHIJKLMNOPQRSTUV");

    /* sigil reads at most 64 packets: the real ones and 0xB packets after. */
    packet fill[64 - REAL_COUNT + 1];
    for (size_t i = 0; i < sizeof(fill) / sizeof(fill[0]); i++) fill[i] = (packet){ 0xB, 8, NULL, 8 };
    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, FLOWER, fill, 64 - REAL_COUNT);
    expect_id("64 packets read", pkg, PKG_LEN, "NPUA80083");
    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, FLOWER, fill, 64 - REAL_COUNT + 1);
    expect_refused("65 packets", pkg, PKG_LEN, SIGIL_ERR_UNSUPPORTED_FORMAT);

    packet runs = { 0x9, 0x10000, NULL, 8 };
    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, FLOWER, &runs, 1);
    expect_refused("packet runs past the file", pkg, PKG_LEN, SIGIL_ERR_UNSUPPORTED_FORMAT);

    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, FLOWER, NULL, 0);
    put_be32(pkg + 0x08, PKG_LEN - 4);
    expect_refused("metadata past the file", pkg, PKG_LEN, SIGIL_ERR_UNSUPPORTED_FORMAT);

    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PSP, "UP9000-NPUG80114_00-LOCOROCOMIDNIGHT", NULL, 0);
    expect_refused("PSP package", pkg, PKG_LEN, SIGIL_ERR_UNSUPPORTED_FORMAT);

    build_pkg(pkg, 0x4000, PKG_PLATFORM_PS3, FLOWER, NULL, 0);
    expect_refused("unknown release type", pkg, PKG_LEN, SIGIL_ERR_UNSUPPORTED_FORMAT);

    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, "", NULL, 0);
    expect_refused("no content id", pkg, PKG_LEN, SIGIL_ERR_NOT_FOUND);

    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, "UP9000-npua80083_00-FLOWERPS3PRIVATE", NULL, 0);
    expect_refused("lowercase id", pkg, PKG_LEN, SIGIL_ERR_NOT_FOUND);

    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, "UP9000-NPUA8008", NULL, 0);
    expect_refused("short content id", pkg, PKG_LEN, SIGIL_ERR_NOT_FOUND);

    build_pkg(pkg, PKG_RETAIL, PKG_PLATFORM_PS3, "UP9000-../../xyz_00-FLOWERPS3PRIVATE", NULL, 0);
    expect_refused("path in the id", pkg, PKG_LEN, SIGIL_ERR_NOT_FOUND);

    free(pkg);
    if (failures) {
        fprintf(stderr, "unit_ps3_pkg: %d failed\n", failures);
        return 1;
    }
    printf("ok unit_ps3_pkg\n");
    return 0;
}
