// SPDX-License-Identifier: MPL-2.0
#include "rzip.h"
#include "card_saturn.h"
#include <stdlib.h>
#include <zlib.h>
#include <zstd.h>

#define RZIP_DEFLATE      1
#define RZIP_ZSTD         2
#define RZIP_CHUNK_HEADER 4u
#define RZIP_MAX_CHUNK    (64u * 1024u * 1024u)   /* libretro-common's own bound */
#define RZIP_MAX_FILE     (256u * 1024u * 1024u)

bool sigil_rzip_is(const uint8_t *data, size_t len) {
    return len >= SIGIL_RZIP_HEADER_SIZE && memcmp(data, "#RZIPv", 6) == 0 &&
           (data[6] == RZIP_DEFLATE || data[6] == RZIP_ZSTD) && data[7] == '#';
}

static bool inflate_chunk(const uint8_t *in, size_t in_len, uint8_t *out, size_t room, size_t *got) {
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit(&z) != Z_OK) return false;
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)in_len;
    z.next_out = out;
    z.avail_out = (uInt)room;
    int rc = inflate(&z, Z_FINISH);
    *got = room - z.avail_out;
    inflateEnd(&z);
    return rc == Z_STREAM_END;
}

int sigil_rzip_decode(const uint8_t *data, size_t len, size_t cap, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (!sigil_rzip_is(data, len)) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    uint32_t chunk = sigil_read_le32(data + 8);
    uint64_t total = sigil_read_le64(data + 12);
    if (!chunk || chunk > RZIP_MAX_CHUNK || !total || total > cap) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    uint8_t *buf = (uint8_t *)malloc((size_t)total);
    if (!buf) return SIGIL_ERR_OOM;
    size_t at = SIGIL_RZIP_HEADER_SIZE, have = 0;
    bool ok = true;
    while (ok && have < total) {
        ok = at + RZIP_CHUNK_HEADER <= len;
        uint32_t size = ok ? sigil_read_le32(data + at) : 0;
        at += RZIP_CHUNK_HEADER;
        ok = ok && size && size <= len - at && size <= (uint64_t)chunk * 2;
        size_t room = total - have < chunk ? (size_t)(total - have) : chunk, got = 0;
        if (ok && data[6] == RZIP_DEFLATE) {
            ok = inflate_chunk(data + at, size, buf + have, room, &got);
        } else if (ok) {
            size_t n = ZSTD_decompress(buf + have, room, data + at, size);
            ok = !ZSTD_isError(n);
            got = ok ? n : 0;
        }
        ok = ok && got > 0 && (got == room || have + got == total);
        have += got;
        at += size;
    }
    if (!ok || have != total) {
        free(buf);
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
    *out = buf;
    *out_len = (size_t)total;
    return SIGIL_OK;
}

typedef struct {
    uint8_t *data;
    size_t   len;
} mem_stream;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_stream *m = (mem_stream *)ctx;
    if (off >= m->len) return 0;
    size_t n = m->len - (size_t)off < len ? m->len - (size_t)off : len;
    memcpy(buf, m->data + off, n);
    return (int)n;
}

static int64_t mem_size(void *ctx) {
    return (int64_t)((mem_stream *)ctx)->len;
}

static void mem_close(void *ctx) {
    mem_stream *m = (mem_stream *)ctx;
    free(m->data);
    free(m);
}

/* `io`'s bytes, decompressed, as a stream of their own; NULL to keep `io`. */
static sigil_io *decompressed(const sigil_io *io) {
    uint8_t *raw = NULL, *plain = NULL;
    size_t raw_len = 0, plain_len = 0;
    if (sigil_bram_read_all(io, RZIP_MAX_FILE, &raw, &raw_len) != SIGIL_OK) return NULL;
    int rc = sigil_rzip_decode(raw, raw_len, RZIP_MAX_FILE, &plain, &plain_len);
    free(raw);
    if (rc != SIGIL_OK) return NULL;
    mem_stream *m = (mem_stream *)malloc(sizeof(*m));
    sigil_io *out = (sigil_io *)calloc(1, sizeof(*out));
    if (!m || !out) {
        free(m);
        free(out);
        free(plain);
        return NULL;
    }
    m->data = plain;
    m->len = plain_len;
    out->read = mem_read;
    out->size = mem_size;
    out->close = mem_close;
    out->ctx = m;
    return out;
}

sigil_io *sigil_save_open(sigil_save_open_fn open, void *ctx, const char *path) {
    sigil_io *io = open ? open(ctx, path) : NULL;
    uint8_t head[SIGIL_RZIP_HEADER_SIZE];
    size_t got = 0;
    if (!io || sigil_io_read_upto(io, 0, head, sizeof(head), &got) != SIGIL_OK || !sigil_rzip_is(head, got)) return io;
    sigil_io *plain = decompressed(io);
    if (!plain) return io;
    sigil_io_close(io);
    return plain;
}
