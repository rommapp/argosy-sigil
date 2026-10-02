// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"
#include "zlib.h"
#include <stdlib.h>

#define LOCAL_SIG    0x04034b50u
#define CENTRAL_SIG  0x02014b50u
#define END_SIG      0x06054b50u
#define LOCAL_SIZE   30u
#define CENTRAL_SIZE 46u
#define END_SIZE     22u
#define DOS_DATE_1980_01_01 0x0021u
#define METHOD_STORED  0u
#define METHOD_DEFLATE 8u
#define VERSION_20     20u

void sigil_zip_members_free(sigil_zip_member *members, size_t count) {
    for (size_t i = 0; members && i < count; i++) free(members[i].data);
    free(members);
}

static uint32_t crc_of(const uint8_t *data, size_t len) {
    uLong crc = crc32(0L, Z_NULL, 0);
    return (uint32_t)crc32(crc, data, (uInt)len);
}

int sigil_zip_store(const sigil_zip_member *members, size_t count, uint8_t **out, size_t *len) {
    if (!out || !len || (count && !members)) return SIGIL_ERR_INVALID_ARG;
    size_t total = END_SIZE;
    for (size_t i = 0; i < count; i++) {
        size_t name = strlen(members[i].name);
        if (name == 0 || name > 0xFFFF || members[i].len > 0xFFFFFFFFu) return SIGIL_ERR_INVALID_ARG;
        total += LOCAL_SIZE + CENTRAL_SIZE + 2 * name + members[i].len;
    }
    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (!buf) return SIGIL_ERR_OOM;
    uint32_t *offsets = (uint32_t *)calloc(count ? count : 1, sizeof(*offsets));
    if (!offsets) { free(buf); return SIGIL_ERR_OOM; }

    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        const sigil_zip_member *m = &members[i];
        uint16_t name = (uint16_t)strlen(m->name);
        uint32_t crc = crc_of(m->data, m->len);
        offsets[i] = (uint32_t)n;
        uint8_t *h = buf + n;
        sigil_write_le32(h, LOCAL_SIG);
        sigil_write_le16(h + 4, VERSION_20);
        sigil_write_le16(h + 12, DOS_DATE_1980_01_01);
        sigil_write_le32(h + 14, crc);
        sigil_write_le32(h + 18, (uint32_t)m->len);
        sigil_write_le32(h + 22, (uint32_t)m->len);
        sigil_write_le16(h + 26, name);
        memcpy(h + LOCAL_SIZE, m->name, name);
        if (m->len) memcpy(h + LOCAL_SIZE + name, m->data, m->len);
        n += LOCAL_SIZE + name + m->len;
    }
    size_t central = n;
    for (size_t i = 0; i < count; i++) {
        const sigil_zip_member *m = &members[i];
        uint16_t name = (uint16_t)strlen(m->name);
        uint8_t *h = buf + n;
        sigil_write_le32(h, CENTRAL_SIG);
        sigil_write_le16(h + 4, VERSION_20);
        sigil_write_le16(h + 6, VERSION_20);
        sigil_write_le16(h + 14, DOS_DATE_1980_01_01);
        sigil_write_le32(h + 16, crc_of(m->data, m->len));
        sigil_write_le32(h + 20, (uint32_t)m->len);
        sigil_write_le32(h + 24, (uint32_t)m->len);
        sigil_write_le16(h + 28, name);
        sigil_write_le32(h + 42, offsets[i]);
        memcpy(h + CENTRAL_SIZE, m->name, name);
        n += CENTRAL_SIZE + name;
    }
    uint8_t *e = buf + n;
    sigil_write_le32(e, END_SIG);
    sigil_write_le16(e + 8, (uint16_t)count);
    sigil_write_le16(e + 10, (uint16_t)count);
    sigil_write_le32(e + 12, (uint32_t)(n - central));
    sigil_write_le32(e + 16, (uint32_t)central);
    n += END_SIZE;
    free(offsets);
    *out = buf;
    *len = n;
    return SIGIL_OK;
}

static int inflate_member(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) {
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return SIGIL_ERR_OOM;
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)in_len;
    z.next_out = out;
    z.avail_out = (uInt)out_len;
    int zrc = inflate(&z, Z_FINISH);
    bool whole = zrc == Z_STREAM_END && z.total_out == out_len;
    inflateEnd(&z);
    return whole ? SIGIL_OK : SIGIL_ERR_UNSUPPORTED_FORMAT;
}

/* The end-of-central-directory record, searched back from the end past a
 * comment of up to 64 KiB. */
static const uint8_t *find_end(const uint8_t *zip, size_t len) {
    if (len < END_SIZE) return NULL;
    size_t lowest = len > END_SIZE + 0xFFFF ? len - END_SIZE - 0xFFFF : 0;
    for (size_t at = len - END_SIZE + 1; at-- > lowest;) {
        if (sigil_read_le32(zip + at) == END_SIG && at + END_SIZE + sigil_read_le16(zip + at + 20) == len) return zip + at;
    }
    return NULL;
}

int sigil_zip_read_mem(const uint8_t *zip, size_t len, size_t max_member, sigil_zip_member **out, size_t *count) {
    if (!zip || !out || !count) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    *count = 0;
    const uint8_t *end = find_end(zip, len);
    if (!end) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    size_t entries = sigil_read_le16(end + 10);
    size_t dir_size = sigil_read_le32(end + 12), dir_off = sigil_read_le32(end + 16);
    if (dir_off > len || dir_size > len - dir_off) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    sigil_zip_member *members = (sigil_zip_member *)calloc(entries ? entries : 1, sizeof(*members));
    if (!members) return SIGIL_ERR_OOM;
    int rc = SIGIL_OK;
    size_t n = 0, at = dir_off;
    for (size_t i = 0; i < entries && rc == SIGIL_OK; i++) {
        rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
        if (at + CENTRAL_SIZE > dir_off + dir_size || sigil_read_le32(zip + at) != CENTRAL_SIG) break;
        const uint8_t *c = zip + at;
        uint16_t flags = sigil_read_le16(c + 8), method = sigil_read_le16(c + 10);
        uint32_t crc = sigil_read_le32(c + 16), comp = sigil_read_le32(c + 20), size = sigil_read_le32(c + 24);
        size_t name_len = sigil_read_le16(c + 28), extra = sigil_read_le16(c + 30), comment = sigil_read_le16(c + 32);
        size_t local = sigil_read_le32(c + 42);
        at += CENTRAL_SIZE + name_len + extra + comment;
        if (at > dir_off + dir_size || name_len == 0 || name_len >= SIGIL_SAVE_ENTRY_MAX) break;
        if ((flags & 0x1) || (method != METHOD_STORED && method != METHOD_DEFLATE) || size > max_member) break;
        if (local > len || len - local < LOCAL_SIZE || sigil_read_le32(zip + local) != LOCAL_SIG) break;
        size_t data = local + LOCAL_SIZE + sigil_read_le16(zip + local + 26) + sigil_read_le16(zip + local + 28);
        if (data > len || comp > len - data) break;
        if (c[CENTRAL_SIZE + name_len - 1] == '/') { rc = SIGIL_OK; continue; }

        sigil_zip_member *m = &members[n];
        memcpy(m->name, c + CENTRAL_SIZE, name_len);
        m->name[name_len] = '\0';
        m->len = size;
        m->data = (uint8_t *)malloc(size ? size : 1);
        if (!m->data) { rc = SIGIL_ERR_OOM; break; }
        if (method == METHOD_STORED) rc = comp == size ? (memcpy(m->data, zip + data, size), SIGIL_OK) : SIGIL_ERR_UNSUPPORTED_FORMAT;
        else rc = inflate_member(zip + data, comp, m->data, size);
        if (rc == SIGIL_OK && crc_of(m->data, size) != crc) rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
        if (rc != SIGIL_OK) { free(m->data); m->data = NULL; break; }
        n++;
    }
    if (rc != SIGIL_OK) { sigil_zip_members_free(members, n); return rc; }
    *out = members;
    *count = n;
    return SIGIL_OK;
}
