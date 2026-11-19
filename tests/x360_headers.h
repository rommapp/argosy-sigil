// SPDX-License-Identifier: MPL-2.0
/* Xbox 360 content headers for the tests, written from Xenia's sources
 * apart from sigil's own code, so a test's expected header never comes from
 * sigil. Offsets: XCONTENT_AGGREGATE_DATA (content_manager.h in Edge at
 * 4ad875a) and XContentContainerHeader (xcontent/xcontent.h in Canary at
 * 27d11a2), big-endian. */
#ifndef SIGIL_TEST_X360_HEADERS_H
#define SIGIL_TEST_X360_HEADERS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define X360_EDGE_SIZE   0x148u
#define X360_CANARY_SIZE 0xA000u

static void x360_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t x360_get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* An ASCII name as UTF-16BE, at most 127 units then a terminator. */
static void x360_name(uint8_t *at, const char *name) {
    for (size_t i = 0; name[i] && i < 127; i++) at[2 * i + 1] = (uint8_t)name[i];
}

/* The fields both forms carry. */
typedef struct {
    uint32_t type;
    uint32_t title;
    uint8_t  name[256];   /* UTF-16BE, zero after the terminator */
    uint8_t  file[42];
} x360_fields;

/* A header's fields as Edge reads them (ContentDataFromHeader and
 * ReadContentHeaderFile): from a container, the English name else the first
 * filled one, and Xenia's own file name for a save; `package` otherwise. */
static bool x360_read(const uint8_t *h, size_t len, const char *package, x360_fields *f) {
    memset(f, 0, sizeof(*f));
    if (len >= X360_EDGE_SIZE && len < 0x971Au) {
        f->type = x360_get32(h + 0x004);
        f->title = x360_get32(h + 0x140);
        memcpy(f->name, h + 0x008, 254);
        memcpy(f->file, h + 0x108, 42);
        return true;
    }
    if (len != 0x971Au && len != X360_CANARY_SIZE) return false;
    f->type = x360_get32(h + 0x344);
    f->title = x360_get32(h + 0x360);
    for (size_t lang = 0; lang < 12; lang++) {
        if (lang >= 9 && x360_get32(h + 0x348) < 2) break;
        const uint8_t *n = lang < 9 ? h + 0x411 + lang * 256 : h + 0x541A + (lang - 9) * 256;
        if (n[0] || n[1]) {
            memcpy(f->name, n, 254);
            break;
        }
    }
    if (f->type == 1 && len >= 0x971Au + 42 && h[0x971A]) memcpy(f->file, h + 0x971A, 42);
    else memcpy(f->file, package, strlen(package) < 42 ? strlen(package) : 42);
    return true;
}

/* The 0x148 bytes Edge would write for `f`: device 1, XUID 0, padding 0. */
static void x360_edge_bytes(const x360_fields *f, uint8_t out[X360_EDGE_SIZE]) {
    memset(out, 0, X360_EDGE_SIZE);
    x360_be32(out + 0x000, 1);
    x360_be32(out + 0x004, f->type);
    memcpy(out + 0x008, f->name, 254);
    memcpy(out + 0x108, f->file, 42);
    x360_be32(out + 0x140, f->title);
}

/* The 0xA000 bytes Canary's FillHeader writes for `f`: magic CON,
 * header_size 0x971A, type, metadata_version 2, title id, one data file,
 * the English name, Xenia's file name; every other byte zero. */
static void x360_canary_bytes(const x360_fields *f, uint8_t *out) {
    memset(out, 0, X360_CANARY_SIZE);
    x360_be32(out + 0x000, 0x434F4E20u);
    x360_be32(out + 0x340, 0x971Au);
    x360_be32(out + 0x344, f->type);
    x360_be32(out + 0x348, 2);
    x360_be32(out + 0x360, f->title);
    x360_be32(out + 0x39D, 1);
    memcpy(out + 0x411, f->name, 254);
    memcpy(out + 0x971A, f->file, 42);
}

/* A header Edge made for a save named `name`/`file` of `title`. */
static uint8_t *x360_edge_header(const char *name, const char *file, uint32_t title, size_t *len) {
    x360_fields f;
    memset(&f, 0, sizeof(f));
    f.type = 1;
    f.title = title;
    x360_name(f.name, name);
    memcpy(f.file, file, strlen(file));
    uint8_t *out = (uint8_t *)malloc(X360_EDGE_SIZE);
    x360_edge_bytes(&f, out);
    *len = X360_EDGE_SIZE;
    return out;
}

/* A header Canary made itself for a save (InitializePackageHeader): its
 * fields, the maker's XUID as profile_id, and what only the container has
 * room for, a title name and thumbnail bytes. */
static uint8_t *x360_canary_header(const char *name, const char *file, uint32_t title, uint64_t profile,
                                   size_t *len) {
    x360_fields f;
    memset(&f, 0, sizeof(f));
    f.type = 1;
    f.title = title;
    x360_name(f.name, name);
    memcpy(f.file, file, strlen(file));
    uint8_t *out = (uint8_t *)malloc(X360_CANARY_SIZE);
    x360_canary_bytes(&f, out);
    x360_be32(out + 0x371, (uint32_t)(profile >> 32));
    x360_be32(out + 0x375, (uint32_t)profile);
    x360_name(out + 0x1691, "Title name");
    x360_be32(out + 0x1712, 4);
    memcpy(out + 0x171A, "PNG!", 4);
    *len = X360_CANARY_SIZE;
    return out;
}

/* `got` is what the emulator of `canary` (else Edge) writes for the save
 * whose header in the other form is `from`. */
static bool x360_converted(const uint8_t *got, size_t got_len, const uint8_t *from, size_t from_len,
                           const char *package, bool canary) {
    x360_fields f;
    if (!x360_read(from, from_len, package, &f)) return false;
    size_t want_len = canary ? X360_CANARY_SIZE : X360_EDGE_SIZE;
    uint8_t *want = (uint8_t *)malloc(want_len);
    if (canary) x360_canary_bytes(&f, want);
    else x360_edge_bytes(&f, want);
    bool same = got_len == want_len && memcmp(got, want, want_len) == 0;
    free(want);
    return same;
}

#endif
