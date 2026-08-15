// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_TEST_THREEDS_FIXTURE_H
#define SIGIL_TEST_THREEDS_FIXTURE_H

/* Builds Z3DS containers from the layout documented in azahar
 * src/common/file_derived.h (Z3DSFileHeader) and file_derived.cpp. The zstd
 * payload uses stored (raw) blocks so no compressor is needed; a decoder
 * cannot tell the difference. NOTE: a container written here proves sigil
 * agrees with our own reading of the struct, NOT that it agrees with a file
 * Azahar produced. Only an Azahar-compressed ROM proves that. */

#include "sigil.h"
#include <stdlib.h>
#include <string.h>

#define Z3DS_ZSTD_FRAME_MAGIC   0xFD2FB528u
#define Z3DS_SKIPPABLE_MAGIC    0x184D2A5Eu
#define Z3DS_SEEKABLE_MAGIC     0x8F92EAB1u
#define Z3DS_MAX_TEST_FRAMES    512

typedef struct {
    const uint8_t *buf;
    size_t         len;
} mem_ctx;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_ctx *m = (mem_ctx *)ctx;
    if (off >= m->len) return 0;
    size_t avail = m->len - (size_t)off;
    size_t n = len < avail ? len : avail;
    memcpy(buf, m->buf + off, n);
    return (int)n;
}

static int64_t mem_size(void *ctx) { return (int64_t)((mem_ctx *)ctx)->len; }

static void fx_put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void fx_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void fx_put_le64(uint8_t *p, uint64_t v) {
    fx_put_le32(p, (uint32_t)(v & 0xFFFFFFFFu));
    fx_put_le32(p + 4, (uint32_t)(v >> 32));
}

/* Emits one zstd frame holding `len` bytes of stored data. Returns bytes written. */
static size_t fx_write_zstd_frame(uint8_t *dst, const uint8_t *src, size_t len) {
    size_t pos = 0;
    fx_put_le32(dst + pos, Z3DS_ZSTD_FRAME_MAGIC);
    pos += 4;
    dst[pos++] = 0xA0; /* single-segment, 4-byte frame content size, no checksum */
    fx_put_le32(dst + pos, (uint32_t)len);
    pos += 4;

    size_t emitted = 0;
    do {
        size_t blk = len - emitted;
        if (blk > 0x10000) blk = 0x10000;
        uint32_t hdr = (uint32_t)(blk << 3);
        if (emitted + blk >= len) hdr |= 1u;
        dst[pos++] = (uint8_t)(hdr & 0xFF);
        dst[pos++] = (uint8_t)((hdr >> 8) & 0xFF);
        dst[pos++] = (uint8_t)((hdr >> 16) & 0xFF);
        if (blk) memcpy(dst + pos, src + emitted, blk);
        pos += blk;
        emitted += blk;
    } while (emitted < len);

    return pos;
}

/* Wraps `payload` in a Z3DS container. `frame_size` 0 means one frame.
 * Caller frees the returned buffer. */
static uint8_t *z3ds_build(const uint8_t *payload, size_t payload_len,
                           const char underlying_magic[4],
                           const uint8_t *metadata, size_t metadata_len,
                           size_t frame_size, size_t *out_len) {
    if (frame_size == 0) frame_size = payload_len ? payload_len : 1;

    size_t meta_total = (metadata_len + 0xF) & ~(size_t)0xF;
    size_t cap = 0x20 + meta_total + payload_len + 0x20000 + Z3DS_MAX_TEST_FRAMES * 32;
    uint8_t *buf = (uint8_t *)calloc(1, cap);
    if (!buf) return NULL;

    uint32_t frame_csize[Z3DS_MAX_TEST_FRAMES];
    uint32_t frame_dsize[Z3DS_MAX_TEST_FRAMES];
    size_t   frames = 0;

    if (metadata && metadata_len) memcpy(buf + 0x20, metadata, metadata_len);

    size_t payload_off = 0x20 + meta_total;
    size_t pos = payload_off;
    size_t consumed = 0;

    do {
        size_t n = payload_len - consumed;
        if (n > frame_size) n = frame_size;
        size_t written = fx_write_zstd_frame(buf + pos, payload + consumed, n);
        if (frames < Z3DS_MAX_TEST_FRAMES) {
            frame_csize[frames] = (uint32_t)written;
            frame_dsize[frames] = (uint32_t)n;
        }
        frames++;
        pos += written;
        consumed += n;
    } while (consumed < payload_len && frames < Z3DS_MAX_TEST_FRAMES);

    uint32_t table_size = (uint32_t)(frames * 8 + 9);
    fx_put_le32(buf + pos, Z3DS_SKIPPABLE_MAGIC);
    pos += 4;
    fx_put_le32(buf + pos, table_size);
    pos += 4;
    for (size_t i = 0; i < frames; i++) {
        fx_put_le32(buf + pos, frame_csize[i]);
        pos += 4;
        fx_put_le32(buf + pos, frame_dsize[i]);
        pos += 4;
    }
    fx_put_le32(buf + pos, (uint32_t)frames);
    pos += 4;
    buf[pos++] = 0;
    fx_put_le32(buf + pos, Z3DS_SEEKABLE_MAGIC);
    pos += 4;

    /* Upstream counts the seek-table frame in compressed_size; endStream
     * writes it through the same counter. */
    size_t compressed_len = pos - payload_off;

    memcpy(buf, "Z3DS", 4);
    memcpy(buf + 4, underlying_magic, 4);
    buf[8] = 1;
    buf[9] = 0;
    fx_put_le16(buf + 10, 0x20);
    fx_put_le32(buf + 12, (uint32_t)meta_total);
    fx_put_le64(buf + 16, compressed_len);
    fx_put_le64(buf + 24, payload_len);

    *out_len = pos;
    return buf;
}

#endif
