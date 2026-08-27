// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XDVDFS_SECTOR    2048
#define XGD1_BASE        405798912ull

#define VD_SECTOR        32
#define ROOT_SECTOR      33
#define XBE_SECTOR       34
#define IMAGE_SECTORS    35

/* Robotech Invasion (USA), read out of a real redump: TDK Mediactive's "TT"
 * prefix with 0x1B in the low half. Anchors the formatting against a disc
 * rather than an invented value. */
#define SAMPLE_TITLE_ID  0x5454001Bu
#define SAMPLE_SERIAL    "TT-027"

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* Presents `buf` as if it began at `base` in a much larger stream, so the
 * XGD partition probe can be exercised without allocating the 387 MB of video
 * partition that sits in front of a real redump's game partition. */
typedef struct {
    const uint8_t *buf;
    size_t         len;
    uint64_t       base;
} mem_ctx;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_ctx *m = (mem_ctx *)ctx;
    if (off < m->base) return 0;
    uint64_t rel = off - m->base;
    if (rel >= m->len) return 0;
    size_t avail = m->len - (size_t)rel;
    size_t n = len < avail ? len : avail;
    memcpy(buf, m->buf + rel, n);
    return (int)n;
}

static int64_t mem_size(void *ctx) {
    mem_ctx *m = (mem_ctx *)ctx;
    return (int64_t)(m->base + m->len);
}

/* Minimal but structurally honest XBE: the certificate is addressed by virtual
 * address, so the reader has to subtract the image base to find it on disk. */
static void build_xbe(uint8_t *p, uint32_t title_id) {
    memcpy(p, "XBEH", 4);
    write_le32(p + 0x104, 0x00010000);          /* image base address */
    write_le32(p + 0x118, 0x00010184);          /* certificate virtual address */
    write_le32(p + 0x184, 492);                 /* certificate size */
    write_le32(p + 0x184 + 0x08, title_id);
}

/* Root table holding two entries so the lookup actually takes a branch instead
 * of matching the first node it reads. "default.xbe" sorts before "ZZZ", so
 * the search has to follow the left child. */
static void build_root_table(uint8_t *t, uint32_t xbe_sector, uint8_t attributes) {
    const uint32_t left_off = 0x14;

    write_le16(t + 0, (uint16_t)(left_off / 4));   /* left subtree, 4-byte units */
    write_le16(t + 2, 0);                          /* no right subtree */
    write_le32(t + 4, 99);
    write_le32(t + 8, XDVDFS_SECTOR);
    t[12] = 0x10;                                  /* directory */
    t[13] = 3;
    memcpy(t + 14, "ZZZ", 3);

    uint8_t *n = t + left_off;
    write_le16(n + 0, 0);
    write_le16(n + 2, 0);
    write_le32(n + 4, xbe_sector);
    write_le32(n + 8, XDVDFS_SECTOR);
    n[12] = attributes;
    n[13] = 11;
    memcpy(n + 14, "default.xbe", 11);
}

static uint8_t *build_image(uint32_t title_id, uint8_t xbe_attributes) {
    size_t len = IMAGE_SECTORS * XDVDFS_SECTOR;
    uint8_t *img = calloc(1, len);
    if (!img) return NULL;

    uint8_t *vd = img + VD_SECTOR * XDVDFS_SECTOR;
    memcpy(vd, "MICROSOFT*XBOX*MEDIA", 20);
    memcpy(vd + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);
    write_le32(vd + 0x14, ROOT_SECTOR);
    write_le32(vd + 0x18, XDVDFS_SECTOR);

    build_root_table(img + ROOT_SECTOR * XDVDFS_SECTOR, XBE_SECTOR, xbe_attributes);
    build_xbe(img + XBE_SECTOR * XDVDFS_SECTOR, title_id);
    return img;
}

static int check(const char *label, const sigil_result *r,
                 const char *want_title, const char *want_save) {
    if (strcmp(r->title_id, want_title) != 0) {
        fprintf(stderr, "FAIL %s title_id: got '%s' want '%s'\n",
                label, r->title_id, want_title);
        return 1;
    }
    if (strcmp(r->save_id, want_save) != 0) {
        fprintf(stderr, "FAIL %s save_id: got '%s' want '%s'\n",
                label, r->save_id, want_save);
        return 1;
    }
    if (r->platform != SIGIL_PLATFORM_XBOX) {
        fprintf(stderr, "FAIL %s platform: got %d\n", label, (int)r->platform);
        return 1;
    }
    if (r->usage != SIGIL_USAGE_FOLDER_EXACT) {
        fprintf(stderr, "FAIL %s usage: got %d\n", label, (int)r->usage);
        return 1;
    }
    if (r->source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "FAIL %s source: expected binary\n", label);
        return 1;
    }
    return 0;
}

static int test_bare_xbe(void) {
    uint8_t xbe[1024];
    memset(xbe, 0, sizeof(xbe));
    build_xbe(xbe, SAMPLE_TITLE_ID);

    mem_ctx ctx = { xbe, sizeof(xbe), 0 };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, "default.xbe", SIGIL_PLATFORM_AUTO, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL bare_xbe: rc=%d\n", rc);
        return 1;
    }
    return check("bare_xbe", &r, SAMPLE_SERIAL, "5454001B");
}

/* The dashboard and XDK builds carry ids whose prefix bytes are not letters;
 * those print as plain hex in both fields. */
static int test_non_printable_prefix(void) {
    uint8_t xbe[1024];
    memset(xbe, 0, sizeof(xbe));
    build_xbe(xbe, 0x00000064u);

    mem_ctx ctx = { xbe, sizeof(xbe), 0 };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, "default.xbe", SIGIL_PLATFORM_AUTO, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL hex_prefix: rc=%d\n", rc);
        return 1;
    }
    return check("hex_prefix", &r, "00000064", "00000064");
}

static int test_image_at_base(const char *label, uint64_t base, const char *name) {
    uint8_t *img = build_image(SAMPLE_TITLE_ID, 0x80);
    if (!img) return 1;

    mem_ctx ctx = { img, IMAGE_SECTORS * XDVDFS_SECTOR, base };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, name, SIGIL_PLATFORM_XBOX, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL %s: rc=%d\n", label, rc);
        free(img);
        return 1;
    }
    int bad = check(label, &r, SAMPLE_SERIAL, "5454001B");
    free(img);
    return bad;
}

/* A root entry named default.xbe but flagged as a directory is not a boot
 * image, and accepting it would read a directory table as an XBE header. */
static int test_directory_entry_rejected(void) {
    uint8_t *img = build_image(SAMPLE_TITLE_ID, 0x10);
    if (!img) return 1;

    mem_ctx ctx = { img, IMAGE_SECTORS * XDVDFS_SECTOR, 0 };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, "game.xiso", SIGIL_PLATFORM_XBOX, NULL, &r);
    free(img);
    if (rc == SIGIL_OK) {
        fprintf(stderr, "FAIL dir_entry: expected failure, got '%s'\n", r.title_id);
        return 1;
    }
    return 0;
}

/* Sets name .xiso.iso, which must resolve to Xbox rather than to the
 * ambiguous bare .iso that would need an explicit hint. */
static int test_compound_extension(void) {
    uint8_t *img = build_image(SAMPLE_TITLE_ID, 0x80);
    if (!img) return 1;

    mem_ctx ctx = { img, IMAGE_SECTORS * XDVDFS_SECTOR, 0 };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, "Some Game.xiso.iso", SIGIL_PLATFORM_AUTO, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL compound_ext: rc=%d\n", rc);
        free(img);
        return 1;
    }
    int bad = check("compound_ext", &r, SAMPLE_SERIAL, "5454001B");
    free(img);
    return bad;
}

int main(void) {
    int bad = 0;
    bad |= test_bare_xbe();
    bad |= test_non_printable_prefix();
    bad |= test_image_at_base("xiso", 0, "game.xiso");
    bad |= test_image_at_base("redump_xgd1", XGD1_BASE, "game.iso");
    bad |= test_directory_entry_rejected();
    bad |= test_compound_extension();
    if (bad) return 1;
    printf("ok unit_xbox (serial=%s save_id=5454001B)\n", SAMPLE_SERIAL);
    return 0;
}
