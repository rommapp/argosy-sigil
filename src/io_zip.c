// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <zlib.h>

/* Games travel as `.zip`, so an identifier that can only be read from a loose
 * image is one the caller cannot use. This presents the archive's largest
 * member as a normal sigil_io.
 *
 * The awkward part is that sigil_io is random access while deflate is a
 * forward-only stream. Seeking is emulated: a read at or ahead of the current
 * position inflates and discards up to it, and a read behind it restarts the
 * decoder from the entry's first byte. Extractors that walk a disc image touch
 * a handful of ascending offsets, so a restart is rare and never more than one
 * per extraction. A stored (uncompressed) member skips all of it and reads
 * through directly. */

#define ZIP_EOCD_SIG            0x06054b50u
#define ZIP_EOCD64_SIG          0x06064b50u
#define ZIP_EOCD64_LOCATOR_SIG  0x07064b50u
#define ZIP_CENTRAL_SIG         0x02014b50u
#define ZIP_LOCAL_SIG           0x04034b50u

#define ZIP_EOCD_MIN            22
#define ZIP_EOCD64_LOCATOR_SIZE 20
#define ZIP_CENTRAL_HEADER      46
#define ZIP_LOCAL_HEADER        30

/* The comment that may trail the end-of-central-directory record is a 16-bit
 * length, so the record starts within this many bytes of the file's end. */
#define ZIP_EOCD_SEARCH_MAX     (65535 + ZIP_EOCD_MIN)

#define ZIP_METHOD_STORE        0
#define ZIP_METHOD_DEFLATE      8

#define ZIP_ZIP64_EXTRA_ID      0x0001
#define ZIP_U32_MAX             0xFFFFFFFFu

#define ZIP_IO_BUF              (64u * 1024u)
/* A central directory large enough to exceed this is not a game archive. */
#define ZIP_MAX_CENTRAL_DIR     (16u * 1024u * 1024u)
#define ZIP_MAX_NAME            512

typedef struct {
    FILE    *fp;
    uint64_t data_off;          /* first byte of the member's stream */
    uint64_t comp_size;
    uint64_t uncomp_size;
    uint16_t method;

    z_stream  z;
    bool      z_active;
    uint64_t  pos;              /* uncompressed bytes already produced */
    uint64_t  comp_consumed;
    uint8_t  *in_buf;
    uint8_t  *skip_buf;
} zip_ctx;

static int zip_read_raw(FILE *fp, uint64_t off, void *buf, size_t len) {
#if defined(_WIN32)
    if (_fseeki64(fp, (long long)off, SEEK_SET) != 0) return -1;
#else
    if (fseeko(fp, (off_t)off, SEEK_SET) != 0) return -1;
#endif
    return fread(buf, 1, len, fp) == len ? 0 : -1;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* ---- container parsing ---------------------------------------------------*/

typedef struct {
    uint64_t central_off;
    uint64_t central_size;
} zip_dir_loc;

static int zip_find_central_dir(FILE *fp, uint64_t file_size, zip_dir_loc *out) {
    size_t window = file_size < ZIP_EOCD_SEARCH_MAX ? (size_t)file_size
                                                    : ZIP_EOCD_SEARCH_MAX;
    if (window < ZIP_EOCD_MIN) return -1;

    uint8_t *buf = (uint8_t *)malloc(window);
    if (!buf) return -1;
    uint64_t window_off = file_size - window;
    if (zip_read_raw(fp, window_off, buf, window) != 0) { free(buf); return -1; }

    int64_t eocd = -1;
    for (int64_t i = (int64_t)window - ZIP_EOCD_MIN; i >= 0; i--) {
        if (sigil_read_le32(buf + i) == ZIP_EOCD_SIG) { eocd = i; break; }
    }
    if (eocd < 0) { free(buf); return -1; }

    out->central_size = sigil_read_le32(buf + eocd + 12);
    out->central_off  = sigil_read_le32(buf + eocd + 16);

    /* A redump image is larger than 4 GB, so its sizes and offsets do not fit
     * the classic fields and only the ZIP64 records carry the real values. */
    if (out->central_off == ZIP_U32_MAX || out->central_size == ZIP_U32_MAX) {
        int64_t loc = eocd - ZIP_EOCD64_LOCATOR_SIZE;
        if (loc < 0 || sigil_read_le32(buf + loc) != ZIP_EOCD64_LOCATOR_SIG) {
            free(buf);
            return -1;
        }
        uint64_t eocd64_off = sigil_read_le64(buf + loc + 8);
        free(buf);

        uint8_t rec[56];
        if (zip_read_raw(fp, eocd64_off, rec, sizeof(rec)) != 0) return -1;
        if (sigil_read_le32(rec) != ZIP_EOCD64_SIG) return -1;
        out->central_size = sigil_read_le64(rec + 40);
        out->central_off  = sigil_read_le64(rec + 48);
        return 0;
    }

    free(buf);
    return 0;
}

/* Replaces any field the central directory marked as overflowed with the
 * 64-bit value from the ZIP64 extra block. The block lists only the fields
 * that actually overflowed, in a fixed order, so each is consumed in turn. */
static void zip_apply_zip64_extra(const uint8_t *extra, size_t extra_len,
                                  uint64_t *uncomp, uint64_t *comp, uint64_t *local_off) {
    size_t p = 0;
    while (p + 4 <= extra_len) {
        uint16_t id   = rd16(extra + p);
        uint16_t size = rd16(extra + p + 2);
        if (p + 4 + size > extra_len) return;
        if (id == ZIP_ZIP64_EXTRA_ID) {
            const uint8_t *v = extra + p + 4;
            size_t left = size;
            if (*uncomp == ZIP_U32_MAX && left >= 8) {
                *uncomp = sigil_read_le64(v); v += 8; left -= 8;
            }
            if (*comp == ZIP_U32_MAX && left >= 8) {
                *comp = sigil_read_le64(v); v += 8; left -= 8;
            }
            if (*local_off == ZIP_U32_MAX && left >= 8) {
                *local_off = sigil_read_le64(v);
            }
            return;
        }
        p += 4 + size;
    }
}

typedef struct {
    uint64_t local_off;
    uint64_t comp_size;
    uint64_t uncomp_size;
    uint16_t method;
    char     name[ZIP_MAX_NAME];
} zip_entry;

/* Number of path separators, used to prefer the shallowest match. A dump can
 * carry several files of the same name at different depths: a Vita title keeps
 * its own sce_sys/param.sfo beside a second one under savedata/, and only the
 * shallower one describes the title. Anything nested deeper belongs to
 * something subordinate to it. */
static size_t zip_path_depth(const uint8_t *name, size_t name_len) {
    size_t depth = 0;
    for (size_t i = 0; i < name_len; i++) {
        if (name[i] == '/' || name[i] == '\\') depth++;
    }
    return depth;
}

static bool zip_name_ends_with(const uint8_t *name, size_t name_len,
                               const char *suffix) {
    size_t s = strlen(suffix);
    if (name_len < s) return false;
    const uint8_t *tail = name + (name_len - s);
    for (size_t i = 0; i < s; i++) {
        char a = (char)tail[i];
        if (a == '\\') a = '/';
        if (sigil_to_lower(a) != sigil_to_lower(suffix[i])) return false;
    }
    return true;
}

/* With no suffix, picks the largest non-directory member: a game archive
 * usually holds one file, and when it also carries a readme the disc image is
 * the big one. With a suffix, picks the first member whose path ends with it,
 * which is how a metadata file buried under a variable directory name is
 * reached (app/<TITLEID>/sce_sys/param.sfo). Either way the caller re-sniffs
 * the platform from the chosen name, so this never has to know what a given
 * platform's files look like. */
static int zip_select_entry(FILE *fp, const zip_dir_loc *dir, const char *suffix,
                            zip_entry *out) {
    if (dir->central_size == 0 || dir->central_size > ZIP_MAX_CENTRAL_DIR) return -1;

    uint8_t *cd = (uint8_t *)malloc((size_t)dir->central_size);
    if (!cd) return -1;
    if (zip_read_raw(fp, dir->central_off, cd, (size_t)dir->central_size) != 0) {
        free(cd);
        return -1;
    }

    bool found = false;
    size_t best_depth = 0;
    size_t p = 0;
    while (p + ZIP_CENTRAL_HEADER <= dir->central_size) {
        if (sigil_read_le32(cd + p) != ZIP_CENTRAL_SIG) break;

        uint16_t method     = rd16(cd + p + 10);
        uint64_t comp       = sigil_read_le32(cd + p + 20);
        uint64_t uncomp     = sigil_read_le32(cd + p + 24);
        uint16_t name_len   = rd16(cd + p + 28);
        uint16_t extra_len  = rd16(cd + p + 30);
        uint16_t cmt_len    = rd16(cd + p + 32);
        uint64_t local_off  = sigil_read_le32(cd + p + 42);

        size_t entry_len = ZIP_CENTRAL_HEADER + name_len + extra_len + cmt_len;
        if (p + entry_len > dir->central_size) break;

        const uint8_t *name  = cd + p + ZIP_CENTRAL_HEADER;
        const uint8_t *extra = name + name_len;
        zip_apply_zip64_extra(extra, extra_len, &uncomp, &comp, &local_off);

        bool is_dir = name_len > 0 && name[name_len - 1] == '/';
        bool wanted;
        if (suffix) {
            wanted = false;
            if (zip_name_ends_with(name, name_len, suffix)) {
                size_t depth = zip_path_depth(name, name_len);
                if (!found || depth < best_depth) {
                    best_depth = depth;
                    wanted = true;
                }
            }
        } else {
            wanted = (!found || uncomp > out->uncomp_size);
        }
        if (!is_dir && name_len > 0 && name_len < ZIP_MAX_NAME && wanted) {
            out->local_off   = local_off;
            out->comp_size   = comp;
            out->uncomp_size = uncomp;
            out->method      = method;
            memcpy(out->name, name, name_len);
            out->name[name_len] = '\0';
            found = true;
        }
        p += entry_len;
    }

    free(cd);
    return found ? 0 : -1;
}

/* The local header repeats the name and carries its own extra field, whose
 * length routinely differs from the central directory's. The stream therefore
 * cannot be located without reading it. */
static int zip_resolve_data_offset(FILE *fp, uint64_t local_off, uint64_t *out_data_off) {
    uint8_t lh[ZIP_LOCAL_HEADER];
    if (zip_read_raw(fp, local_off, lh, sizeof(lh)) != 0) return -1;
    if (sigil_read_le32(lh) != ZIP_LOCAL_SIG) return -1;
    *out_data_off = local_off + ZIP_LOCAL_HEADER + rd16(lh + 26) + rd16(lh + 28);
    return 0;
}

/* ---- streaming ------------------------------------------------------------*/

static void zip_stream_end(zip_ctx *c) {
    if (c->z_active) {
        inflateEnd(&c->z);
        c->z_active = false;
    }
}

static int zip_stream_start(zip_ctx *c) {
    zip_stream_end(c);
    memset(&c->z, 0, sizeof(c->z));
    /* Raw deflate: a zip member has no zlib wrapper of its own. */
    if (inflateInit2(&c->z, -MAX_WBITS) != Z_OK) return -1;
    c->z_active      = true;
    c->pos           = 0;
    c->comp_consumed = 0;
    return 0;
}

/* Produces up to `len` bytes at the current position. `buf` may be NULL to
 * discard, which is how a forward seek is served. */
static int64_t zip_pull(zip_ctx *c, uint8_t *buf, size_t len) {
    if (len == 0) return 0;

    c->z.next_out  = buf ? buf : c->skip_buf;
    c->z.avail_out = (uInt)(buf ? len : (len < ZIP_IO_BUF ? len : ZIP_IO_BUF));
    size_t want = c->z.avail_out;

    while (c->z.avail_out > 0) {
        if (c->z.avail_in == 0) {
            if (c->comp_consumed >= c->comp_size) break;
            uint64_t left = c->comp_size - c->comp_consumed;
            size_t n = left < ZIP_IO_BUF ? (size_t)left : ZIP_IO_BUF;
            if (zip_read_raw(c->fp, c->data_off + c->comp_consumed, c->in_buf, n) != 0) {
                return -1;
            }
            c->comp_consumed += n;
            c->z.next_in  = c->in_buf;
            c->z.avail_in = (uInt)n;
        }
        int zr = inflate(&c->z, Z_NO_FLUSH);
        if (zr == Z_STREAM_END) break;
        if (zr != Z_OK && zr != Z_BUF_ERROR) return -1;
        if (zr == Z_BUF_ERROR && c->z.avail_in == 0 && c->comp_consumed >= c->comp_size) break;
    }

    size_t produced = want - c->z.avail_out;
    c->pos += produced;
    return (int64_t)produced;
}

static int zip_seek_to(zip_ctx *c, uint64_t off) {
    if (off < c->pos) {
        if (zip_stream_start(c) != 0) return -1;
    }
    while (c->pos < off) {
        uint64_t gap = off - c->pos;
        size_t chunk = gap < ZIP_IO_BUF ? (size_t)gap : ZIP_IO_BUF;
        int64_t got = zip_pull(c, NULL, chunk);
        if (got <= 0) return -1;
    }
    return 0;
}

static int zip_io_read(void *ctx, uint64_t off, void *buf, size_t len) {
    zip_ctx *c = (zip_ctx *)ctx;
    if (off >= c->uncomp_size) return 0;

    uint64_t avail = c->uncomp_size - off;
    if (len > avail) len = (size_t)avail;

    if (c->method == ZIP_METHOD_STORE) {
        if (zip_read_raw(c->fp, c->data_off + off, buf, len) != 0) return -1;
        return (int)len;
    }

    if (zip_seek_to(c, off) != 0) return -1;

    size_t total = 0;
    while (total < len) {
        int64_t got = zip_pull(c, (uint8_t *)buf + total, len - total);
        if (got < 0) return -1;
        if (got == 0) break;
        total += (size_t)got;
    }
    return (int)total;
}

static int64_t zip_io_size(void *ctx) {
    return (int64_t)((zip_ctx *)ctx)->uncomp_size;
}

static void zip_io_close(void *ctx) {
    zip_ctx *c = (zip_ctx *)ctx;
    if (!c) return;
    zip_stream_end(c);
    if (c->fp) fclose(c->fp);
    free(c->in_buf);
    free(c->skip_buf);
    free(c);
}

static sigil_io *zip_open_impl(const char *path, const char *suffix,
                               char *out_name, size_t name_cap) {
    if (!path) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

#if defined(_WIN32)
    if (_fseeki64(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long long end = _ftelli64(fp);
#else
    if (fseeko(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    off_t end = ftello(fp);
#endif
    if (end <= 0) { fclose(fp); return NULL; }

    zip_dir_loc dir;
    if (zip_find_central_dir(fp, (uint64_t)end, &dir) != 0) { fclose(fp); return NULL; }

    zip_entry entry;
    memset(&entry, 0, sizeof(entry));
    if (zip_select_entry(fp, &dir, suffix, &entry) != 0) { fclose(fp); return NULL; }
    if (entry.method != ZIP_METHOD_STORE && entry.method != ZIP_METHOD_DEFLATE) {
        fclose(fp);
        return NULL;
    }

    uint64_t data_off;
    if (zip_resolve_data_offset(fp, entry.local_off, &data_off) != 0) {
        fclose(fp);
        return NULL;
    }

    zip_ctx *c = (zip_ctx *)calloc(1, sizeof(*c));
    if (!c) { fclose(fp); return NULL; }
    c->fp          = fp;
    c->data_off    = data_off;
    c->comp_size   = entry.comp_size;
    c->uncomp_size = entry.uncomp_size;
    c->method      = entry.method;
    c->in_buf      = (uint8_t *)malloc(ZIP_IO_BUF);
    c->skip_buf    = (uint8_t *)malloc(ZIP_IO_BUF);
    if (!c->in_buf || !c->skip_buf) { zip_io_close(c); return NULL; }

    if (entry.method == ZIP_METHOD_DEFLATE && zip_stream_start(c) != 0) {
        zip_io_close(c);
        return NULL;
    }

    sigil_io *io = (sigil_io *)calloc(1, sizeof(*io));
    if (!io) { zip_io_close(c); return NULL; }
    io->read  = zip_io_read;
    io->size  = zip_io_size;
    io->close = zip_io_close;
    io->ctx   = c;

    if (out_name && name_cap > 0) {
        size_t n = strlen(entry.name);
        if (n >= name_cap) n = name_cap - 1;
        memcpy(out_name, entry.name, n);
        out_name[n] = '\0';
    }
    return io;
}

sigil_io *sigil_io_open_zip(const char *path, char *out_name, size_t name_cap) {
    return zip_open_impl(path, NULL, out_name, name_cap);
}

sigil_io *sigil_io_open_zip_member(const char *path, const char *suffix) {
    if (!suffix) return NULL;
    return zip_open_impl(path, suffix, NULL, 0);
}
