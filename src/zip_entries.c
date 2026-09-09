// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"
#include <stdlib.h>
#include <zlib.h>

#define ZE_EOCD_SIG            0x06054b50u
#define ZE_EOCD64_SIG          0x06064b50u
#define ZE_EOCD64_LOCATOR_SIG  0x07064b50u
#define ZE_CENTRAL_SIG         0x02014b50u
#define ZE_LOCAL_SIG           0x04034b50u

#define ZE_EOCD_MIN            22
#define ZE_EOCD64_LOCATOR_SIZE 20
#define ZE_CENTRAL_HEADER      46
#define ZE_LOCAL_HEADER        30
#define ZE_EOCD_SEARCH_MAX     (65535 + ZE_EOCD_MIN)
#define ZE_METHOD_STORE        0
#define ZE_METHOD_DEFLATE      8
#define ZE_ZIP64_EXTRA_ID      0x0001
#define ZE_U32_MAX             0xFFFFFFFFu
#define ZE_MAX_CENTRAL_DIR     (16u * 1024u * 1024u)
#define ZE_MAX_NAME            512
#define ZE_BUF                 (64u * 1024u)

static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int read_all(const sigil_io *io, uint64_t off, void *buf, size_t len) {
    return sigil_io_read_exact(io, off, buf, len);
}

typedef struct {
    uint64_t central_off;
    uint64_t central_size;
} dir_loc;

static int find_central_dir(const sigil_io *io, dir_loc *out) {
    int64_t size = io->size ? io->size(io->ctx) : -1;
    if (size < ZE_EOCD_MIN) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    size_t window = (uint64_t)size < ZE_EOCD_SEARCH_MAX ? (size_t)size : ZE_EOCD_SEARCH_MAX;

    uint8_t *buf = (uint8_t *)malloc(window);
    if (!buf) return SIGIL_ERR_OOM;
    uint64_t window_off = (uint64_t)size - window;
    if (read_all(io, window_off, buf, window) != SIGIL_OK) { free(buf); return SIGIL_ERR_IO; }

    int64_t eocd = -1;
    for (int64_t i = (int64_t)window - ZE_EOCD_MIN; i >= 0; i--) {
        if (sigil_read_le32(buf + i) == ZE_EOCD_SIG) { eocd = i; break; }
    }
    if (eocd < 0) { free(buf); return SIGIL_ERR_UNSUPPORTED_FORMAT; }

    out->central_size = sigil_read_le32(buf + eocd + 12);
    out->central_off  = sigil_read_le32(buf + eocd + 16);

    if (out->central_off == ZE_U32_MAX || out->central_size == ZE_U32_MAX) {
        int64_t loc = eocd - ZE_EOCD64_LOCATOR_SIZE;
        if (loc < 0 || sigil_read_le32(buf + loc) != ZE_EOCD64_LOCATOR_SIG) {
            free(buf);
            return SIGIL_ERR_UNSUPPORTED_FORMAT;
        }
        uint64_t eocd64_off = sigil_read_le64(buf + loc + 8);
        free(buf);
        uint8_t rec[56];
        if (read_all(io, eocd64_off, rec, sizeof(rec)) != SIGIL_OK) return SIGIL_ERR_IO;
        if (sigil_read_le32(rec) != ZE_EOCD64_SIG) return SIGIL_ERR_UNSUPPORTED_FORMAT;
        out->central_size = sigil_read_le64(rec + 40);
        out->central_off  = sigil_read_le64(rec + 48);
        return SIGIL_OK;
    }
    free(buf);
    return SIGIL_OK;
}

static void apply_zip64_extra(const uint8_t *extra, size_t extra_len,
                              uint64_t *uncomp, uint64_t *comp, uint64_t *local_off) {
    size_t p = 0;
    while (p + 4 <= extra_len) {
        uint16_t id   = rd16(extra + p);
        uint16_t size = rd16(extra + p + 2);
        if (p + 4 + size > extra_len) return;
        if (id == ZE_ZIP64_EXTRA_ID) {
            const uint8_t *v = extra + p + 4;
            size_t left = size;
            if (*uncomp == ZE_U32_MAX && left >= 8) { *uncomp = sigil_read_le64(v); v += 8; left -= 8; }
            if (*comp == ZE_U32_MAX && left >= 8)   { *comp = sigil_read_le64(v);   v += 8; left -= 8; }
            if (*local_off == ZE_U32_MAX && left >= 8) *local_off = sigil_read_le64(v);
            return;
        }
        p += 4 + size;
    }
}

bool sigil_io_is_zip(const sigil_io *io) {
    if (!io || !io->read) return false;
    dir_loc loc;
    return find_central_dir(io, &loc) == SIGIL_OK;
}

static int hash_stored(const sigil_io *io, uint64_t off, uint64_t len, sigil_md5 *m, uint8_t *buf) {
    while (len > 0) {
        size_t want = len < ZE_BUF ? (size_t)len : ZE_BUF;
        if (read_all(io, off, buf, want) != SIGIL_OK) return SIGIL_ERR_IO;
        sigil_md5_update(m, buf, want);
        off += want;
        len -= want;
    }
    return SIGIL_OK;
}

static int hash_deflated(const sigil_io *io, uint64_t off, uint64_t comp_len,
                         sigil_md5 *m, uint8_t *in, uint8_t *out) {
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return SIGIL_ERR_IO;

    int rc = SIGIL_OK;
    int zrc = Z_OK;
    while (zrc != Z_STREAM_END) {
        if (z.avail_in == 0) {
            if (comp_len == 0) { rc = SIGIL_ERR_UNSUPPORTED_FORMAT; break; }
            size_t want = comp_len < ZE_BUF ? (size_t)comp_len : ZE_BUF;
            if (read_all(io, off, in, want) != SIGIL_OK) { rc = SIGIL_ERR_IO; break; }
            off += want;
            comp_len -= want;
            z.next_in = in;
            z.avail_in = (uInt)want;
        }
        z.next_out = out;
        z.avail_out = ZE_BUF;
        zrc = inflate(&z, Z_NO_FLUSH);
        if (zrc != Z_OK && zrc != Z_STREAM_END) { rc = SIGIL_ERR_UNSUPPORTED_FORMAT; break; }
        sigil_md5_update(m, out, ZE_BUF - z.avail_out);
    }
    inflateEnd(&z);
    return rc;
}

int sigil_zip_hash_entries(const sigil_io *io,
                           int (*on_entry)(void *ctx, const char *name, const char *md5_hex),
                           void *ctx) {
    if (!io || !io->read || !on_entry) return SIGIL_ERR_INVALID_ARG;

    dir_loc loc;
    int rc = find_central_dir(io, &loc);
    if (rc != SIGIL_OK) return rc;
    if (loc.central_size > ZE_MAX_CENTRAL_DIR) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    uint8_t *dir = (uint8_t *)malloc((size_t)loc.central_size);
    uint8_t *in  = (uint8_t *)malloc(ZE_BUF);
    uint8_t *out = (uint8_t *)malloc(ZE_BUF);
    if (!dir || !in || !out) { free(dir); free(in); free(out); return SIGIL_ERR_OOM; }
    if (read_all(io, loc.central_off, dir, (size_t)loc.central_size) != SIGIL_OK) {
        free(dir); free(in); free(out);
        return SIGIL_ERR_IO;
    }

    size_t p = 0;
    while (rc == SIGIL_OK && p + ZE_CENTRAL_HEADER <= loc.central_size) {
        if (sigil_read_le32(dir + p) != ZE_CENTRAL_SIG) break;
        uint16_t method    = rd16(dir + p + 10);
        uint64_t comp      = sigil_read_le32(dir + p + 20);
        uint64_t uncomp    = sigil_read_le32(dir + p + 24);
        uint16_t name_len  = rd16(dir + p + 28);
        uint16_t extra_len = rd16(dir + p + 30);
        uint16_t cmt_len   = rd16(dir + p + 32);
        uint64_t local_off = sigil_read_le32(dir + p + 42);
        size_t rec = ZE_CENTRAL_HEADER + name_len + extra_len + cmt_len;
        if (p + rec > loc.central_size || name_len == 0 || name_len >= ZE_MAX_NAME) break;

        char name[ZE_MAX_NAME];
        memcpy(name, dir + p + ZE_CENTRAL_HEADER, name_len);
        name[name_len] = '\0';
        apply_zip64_extra(dir + p + ZE_CENTRAL_HEADER + name_len, extra_len, &uncomp, &comp, &local_off);
        p += rec;

        if (name[name_len - 1] == '/') continue;

        uint8_t local[ZE_LOCAL_HEADER];
        if (read_all(io, local_off, local, sizeof(local)) != SIGIL_OK) { rc = SIGIL_ERR_IO; break; }
        if (sigil_read_le32(local) != ZE_LOCAL_SIG) { rc = SIGIL_ERR_UNSUPPORTED_FORMAT; break; }
        uint64_t data_off = local_off + ZE_LOCAL_HEADER + rd16(local + 26) + rd16(local + 28);

        sigil_md5 m;
        sigil_md5_init(&m);
        if (method == ZE_METHOD_STORE)        rc = hash_stored(io, data_off, comp, &m, in);
        else if (method == ZE_METHOD_DEFLATE) rc = hash_deflated(io, data_off, comp, &m, in, out);
        else                                  rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
        if (rc != SIGIL_OK) break;

        uint8_t digest[16];
        char hex[33];
        sigil_md5_final(&m, digest);
        sigil_md5_hex(digest, hex);
        rc = on_entry(ctx, name, hex);
    }

    free(dir); free(in); free(out);
    return rc;
}
