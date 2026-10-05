// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

#include <ctype.h>
#include <stdlib.h>

#define N64_HEADER_LEN 0x40
#define N64_NAME       0x20
#define N64_NAME_LEN   20
#define N64_GAME_CODE  0x3B
#define N64_CODE_LEN   4
#define N64_HASH_CHUNK (1u << 20)

typedef enum { N64_Z64, N64_V64, N64_N64 } n64_order;

/* The first word of every cart, the PI domain setting, in each dump's byte order. */
static bool n64_order_of(const uint8_t *h, n64_order *order) {
    static const uint8_t Z64[4] = { 0x80, 0x37, 0x12, 0x40 };
    static const uint8_t V64[4] = { 0x37, 0x80, 0x40, 0x12 };
    static const uint8_t N64[4] = { 0x40, 0x12, 0x37, 0x80 };
    if (memcmp(h, Z64, 4) == 0) *order = N64_Z64;
    else if (memcmp(h, V64, 4) == 0) *order = N64_V64;
    else if (memcmp(h, N64, 4) == 0) *order = N64_N64;
    else return false;
    return true;
}

/* Rewrites `h` in place as the cart's own big-endian (.z64) order. */
static void n64_to_z64(uint8_t *h, size_t len, n64_order order) {
    for (size_t i = 0; order != N64_Z64 && i + 4 <= len; i += 4) {
        uint8_t w[4] = { h[i], h[i + 1], h[i + 2], h[i + 3] };
        if (order == N64_V64) {
            h[i] = w[1]; h[i + 1] = w[0]; h[i + 2] = w[3]; h[i + 3] = w[2];
        } else {
            h[i] = w[3]; h[i + 1] = w[2]; h[i + 2] = w[1]; h[i + 3] = w[0];
        }
    }
}

/* Reverses each 32-bit word of a .z64 run, giving the .n64 order Project64 hashes. */
static void z64_to_n64(uint8_t *h, size_t len) {
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint8_t a = h[i], b = h[i + 1];
        h[i] = h[i + 3];
        h[i + 1] = h[i + 2];
        h[i + 2] = b;
        h[i + 3] = a;
    }
}

static void upper_hex(sigil_md5 *m, char out[33]) {
    uint8_t digest[16];
    sigil_md5_final(m, digest);
    sigil_md5_hex(digest, out);
    for (char *c = out; *c; c++) *c = (char)toupper((unsigned char)*c);
}

/* The whole ROM's MD5 in .z64 order and in .n64 order. */
static int n64_hashes(const sigil_io *io, n64_order order, sigil_result *out) {
    uint8_t *z64 = (uint8_t *)malloc(N64_HASH_CHUNK);
    uint8_t *n64 = (uint8_t *)malloc(N64_HASH_CHUNK);
    if (!z64 || !n64) {
        free(z64);
        free(n64);
        return SIGIL_ERR_OOM;
    }
    sigil_md5 mz, mn;
    sigil_md5_init(&mz);
    sigil_md5_init(&mn);
    int rc = SIGIL_OK;
    for (uint64_t off = 0;;) {
        size_t got = 0;
        rc = sigil_io_read_upto(io, off, z64, N64_HASH_CHUNK, &got);
        if (rc != SIGIL_OK || got == 0) break;
        n64_to_z64(z64, got, order);
        memcpy(n64, z64, got);
        z64_to_n64(n64, got);
        sigil_md5_update(&mz, z64, got);
        sigil_md5_update(&mn, n64, got);
        off += got;
    }
    free(z64);
    free(n64);
    if (rc != SIGIL_OK) return rc;
    upper_hex(&mz, out->n64_md5);
    upper_hex(&mn, out->n64_md5_n64);
    return SIGIL_OK;
}

/* The header name with trailing spaces and NULs dropped, or "" when it holds
 * anything outside printable ASCII. */
static void n64_header_name(const uint8_t *h, char out[N64_NAME_LEN + 1]) {
    size_t n = N64_NAME_LEN;
    while (n > 0 && (h[N64_NAME + n - 1] == ' ' || h[N64_NAME + n - 1] == '\0')) n--;
    for (size_t i = 0; i < n; i++) {
        if (h[N64_NAME + i] < 0x20 || h[N64_NAME + i] > 0x7E) n = 0;
    }
    memcpy(out, h + N64_NAME, n);
    out[n] = '\0';
}

static bool n64_code_valid(const uint8_t *code) {
    for (size_t i = 0; i < N64_CODE_LEN; i++) {
        uint8_t c = code[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

int sigil_extract_n64(const sigil_io *io, const char *filename_hint, const sigil_options *opts, sigil_result *out) {
    (void)filename_hint;
    (void)opts;

    sigil_result_init(out);
    out->platform = SIGIL_PLATFORM_N64;
    out->usage    = SIGIL_USAGE_FILE_PREFIX;

    if (!io || !io->read) return SIGIL_ERR_INVALID_ARG;

    uint8_t header[N64_HEADER_LEN];
    if (sigil_io_read_exact(io, 0, header, sizeof(header)) != SIGIL_OK) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    n64_order order;
    if (!n64_order_of(header, &order)) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    n64_to_z64(header, sizeof(header), order);

    const uint8_t *code = header + N64_GAME_CODE;
    if (n64_code_valid(code)) {
        memcpy(out->title_id, code, N64_CODE_LEN);
        out->title_id[N64_CODE_LEN] = '\0';
        memcpy(out->raw_serial, code, N64_CODE_LEN);
        out->raw_serial[N64_CODE_LEN] = '\0';
    }
    n64_header_name(header, out->n64_header);
    int rc = n64_hashes(io, order, out);
    if (rc != SIGIL_OK) return rc;
    out->source = SIGIL_SOURCE_BINARY;
    return SIGIL_OK;
}
