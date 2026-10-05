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
