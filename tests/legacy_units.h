// SPDX-License-Identifier: MPL-2.0
/* Units in the shapes clients uploaded before they used sigil's, and the
 * check that one restores to the same files as sigil's own unit. */
#ifndef SIGIL_TEST_LEGACY_UNITS_H
#define SIGIL_TEST_LEGACY_UNITS_H

#include "mem_root.h"
#include "sigil_internal.h"
#include <stdbool.h>

#define LEGACY_ZIP_MAX 32

/* A zip of `n` files under the names given, bytes as given. */
static uint8_t *legacy_zip(const char *const *names, uint8_t *const *data, const size_t *lens, size_t n, size_t *len) {
    sigil_zip_member m[LEGACY_ZIP_MAX];
    if (n > LEGACY_ZIP_MAX) return NULL;
    for (size_t i = 0; i < n; i++) {
        snprintf(m[i].name, sizeof(m[i].name), "%s", names[i]);
        m[i].data = data[i];
        m[i].len = lens[i];
    }
    uint8_t *out = NULL;
    return sigil_zip_store(m, n, &out, len) == SIGIL_OK ? out : NULL;
}

/* `data` with the hardcore marker Argosy appended to uploads before it used
 * sigil: {"h":true,"v":1}, its length as a little-endian u32, then
 * "ARGOSY" 01 00 (argosy-launcher SaveArchiver.appendHardcoreTrailer). */
static uint8_t *with_hardcore_marker(const uint8_t *data, size_t len, size_t *out_len) {
    static const char JSON[] = "{\"h\":true,\"v\":1}";
    static const uint8_t MAGIC[] = { 'A', 'R', 'G', 'O', 'S', 'Y', 0x01, 0x00 };
    size_t json = sizeof(JSON) - 1;
    uint8_t *out = (uint8_t *)malloc(len + json + 4 + sizeof(MAGIC));
    if (!out) return NULL;
    memcpy(out, data, len);
    memcpy(out + len, JSON, json);
    sigil_write_le32(out + len + json, (uint32_t)json);
    memcpy(out + len + json + 4, MAGIC, sizeof(MAGIC));
    *out_len = len + json + 4 + sizeof(MAGIC);
    return out;
}

/* `a` and `b` hold the same paths with the same bytes. */
static bool roots_same(mem_root *a, mem_root *b) {
    if (a->count != b->count) return false;
    for (size_t i = 0; i < b->count; i++) {
        mem_file *f = root_find(a, b->files[i].path);
        if (!f || f->len != b->files[i].len || memcmp(f->data, b->files[i].data, f->len) != 0) return false;
    }
    return true;
}

#endif
