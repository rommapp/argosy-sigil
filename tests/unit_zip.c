// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* Builds real zip files around a synthetic Xbox image and reads the identifier
 * back out of them, so the member is located through the same central
 * directory and local header parsing a downloaded set would exercise. Both
 * storage methods are covered: deflate drives the streaming path with its
 * emulated seeking, store drives the direct-read shortcut. */

#define XDVDFS_SECTOR  2048
#define VD_SECTOR      32
#define ROOT_SECTOR    33
#define XBE_SECTOR     34
#define IMAGE_SECTORS  40
#define IMAGE_BYTES    (IMAGE_SECTORS * XDVDFS_SECTOR)

#define SAMPLE_TITLE_ID 0x5454001Bu
#define SAMPLE_SERIAL   "TT-027"
#define MEMBER_NAME     "Robotech - Invasion (USA).xiso"

static void write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint8_t *build_image(void) {
    uint8_t *img = calloc(1, IMAGE_BYTES);
    if (!img) return NULL;

    uint8_t *vd = img + VD_SECTOR * XDVDFS_SECTOR;
    memcpy(vd, "MICROSOFT*XBOX*MEDIA", 20);
    memcpy(vd + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);
    write_le32(vd + 0x14, ROOT_SECTOR);
    write_le32(vd + 0x18, XDVDFS_SECTOR);

    uint8_t *t = img + ROOT_SECTOR * XDVDFS_SECTOR;
    write_le16(t + 0, 0);
    write_le16(t + 2, 0);
    write_le32(t + 4, XBE_SECTOR);
    write_le32(t + 8, XDVDFS_SECTOR);
    t[12] = 0x80;
    t[13] = 11;
    memcpy(t + 14, "default.xbe", 11);

    uint8_t *xbe = img + XBE_SECTOR * XDVDFS_SECTOR;
    memcpy(xbe, "XBEH", 4);
    write_le32(xbe + 0x104, 0x00010000);
    write_le32(xbe + 0x118, 0x00010184);
    write_le32(xbe + 0x184, 492);
    write_le32(xbe + 0x184 + 0x08, SAMPLE_TITLE_ID);
    return img;
}

/* Minimal single-member zip: local header, member data, one central directory
 * record, end-of-central-directory. The local extra field is deliberately
 * non-empty so a reader that assumes the central directory's extra length
 * would land in the wrong place. */
typedef struct {
    const char    *name;
    const uint8_t *data;
    size_t         len;
} zip_member;

static int deflate_member(const uint8_t *data, size_t data_len, int method,
                          uint8_t **out, size_t *out_len) {
    uint8_t *payload = NULL;
    size_t payload_len = 0;

    if (method == 0) {
        payload = (uint8_t *)malloc(data_len);
        if (!payload) return -1;
        memcpy(payload, data, data_len);
        payload_len = data_len;
    } else {
        uLongf cap = compressBound((uLong)data_len) + 64;
        payload = (uint8_t *)malloc(cap);
        if (!payload) return -1;

        z_stream z;
        memset(&z, 0, sizeof(z));
        if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                         -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            free(payload);
            return -1;
        }
        z.next_in   = (Bytef *)data;
        z.avail_in  = (uInt)data_len;
        z.next_out  = payload;
        z.avail_out = (uInt)cap;
        if (deflate(&z, Z_FINISH) != Z_STREAM_END) {
            deflateEnd(&z);
            free(payload);
            return -1;
        }
        payload_len = cap - z.avail_out;
        deflateEnd(&z);
    }

    *out = payload;
    *out_len = payload_len;
    return 0;
}

/* Members are written in the order given, and the central directory records
 * them in that same order, so a test can reproduce the ordering a real dump
 * has rather than relying on whichever entry happens to come first. */
static int write_zip_multi(const char *path, const zip_member *members,
                           size_t count, int method) {
    const uint8_t local_extra[8] = { 0xEE, 0xFF, 0x04, 0x00, 1, 2, 3, 4 };

    uint8_t **payloads = calloc(count, sizeof(*payloads));
    size_t   *plens    = calloc(count, sizeof(*plens));
    long     *offsets  = calloc(count, sizeof(*offsets));
    if (!payloads || !plens || !offsets) {
        free(payloads); free(plens); free(offsets);
        return -1;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) { free(payloads); free(plens); free(offsets); return -1; }

    for (size_t i = 0; i < count; i++) {
        if (deflate_member(members[i].data, members[i].len, method,
                           &payloads[i], &plens[i]) != 0) {
            fclose(fp);
            for (size_t j = 0; j < i; j++) free(payloads[j]);
            free(payloads); free(plens); free(offsets);
            return -1;
        }
        offsets[i] = ftell(fp);

        uint16_t name_len = (uint16_t)strlen(members[i].name);
        uLong crc = crc32(0, members[i].data, (uInt)members[i].len);

        uint8_t lh[30];
        memset(lh, 0, sizeof(lh));
        write_le32(lh, 0x04034b50u);
        write_le16(lh + 4, 20);
        write_le16(lh + 8, (uint16_t)method);
        write_le32(lh + 14, (uint32_t)crc);
        write_le32(lh + 18, (uint32_t)plens[i]);
        write_le32(lh + 22, (uint32_t)members[i].len);
        write_le16(lh + 26, name_len);
        write_le16(lh + 28, (uint16_t)sizeof(local_extra));
        fwrite(lh, 1, sizeof(lh), fp);
        fwrite(members[i].name, 1, name_len, fp);
        fwrite(local_extra, 1, sizeof(local_extra), fp);
        fwrite(payloads[i], 1, plens[i], fp);
    }

    long central_off = ftell(fp);
    for (size_t i = 0; i < count; i++) {
        uint16_t name_len = (uint16_t)strlen(members[i].name);
        uLong crc = crc32(0, members[i].data, (uInt)members[i].len);

        uint8_t ch[46];
        memset(ch, 0, sizeof(ch));
        write_le32(ch, 0x02014b50u);
        write_le16(ch + 4, 20);
        write_le16(ch + 6, 20);
        write_le16(ch + 10, (uint16_t)method);
        write_le32(ch + 16, (uint32_t)crc);
        write_le32(ch + 20, (uint32_t)plens[i]);
        write_le32(ch + 24, (uint32_t)members[i].len);
        write_le16(ch + 28, name_len);
        write_le32(ch + 42, (uint32_t)offsets[i]);
        fwrite(ch, 1, sizeof(ch), fp);
        fwrite(members[i].name, 1, name_len, fp);
    }
    long central_end = ftell(fp);

    uint8_t eocd[22];
    memset(eocd, 0, sizeof(eocd));
    write_le32(eocd, 0x06054b50u);
    write_le16(eocd + 8, (uint16_t)count);
    write_le16(eocd + 10, (uint16_t)count);
    write_le32(eocd + 12, (uint32_t)(central_end - central_off));
    write_le32(eocd + 16, (uint32_t)central_off);
    fwrite(eocd, 1, sizeof(eocd), fp);

    fclose(fp);
    for (size_t i = 0; i < count; i++) free(payloads[i]);
    free(payloads); free(plens); free(offsets);
    return 0;
}

static int write_zip_named(const char *path, const char *name,
                           const uint8_t *data, size_t data_len, int method) {
    zip_member m = { name, data, data_len };
    return write_zip_multi(path, &m, 1, method);
}

static int write_zip_two(const char *path,
                         const char *name_a, const uint8_t *data_a, size_t len_a,
                         const char *name_b, const uint8_t *data_b, size_t len_b) {
    zip_member m[2] = { { name_a, data_a, len_a }, { name_b, data_b, len_b } };
    return write_zip_multi(path, m, 2, 8);
}

static int write_zip(const char *path, const uint8_t *data, size_t data_len,
                     int method) {
    return write_zip_named(path, MEMBER_NAME, data, data_len, method);
}

static int run_case(const char *label, int method, const uint8_t *img) {
    char path[512];
    snprintf(path, sizeof(path), "%s/sigil_zip_%d.zip",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", method);

    if (write_zip(path, img, IMAGE_BYTES, method) != 0) {
        fprintf(stderr, "FAIL %s: could not write fixture\n", label);
        return 1;
    }

    sigil_result r;
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_AUTO, NULL, &r);
    remove(path);

    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL %s: rc=%d\n", label, rc);
        return 1;
    }
    if (strcmp(r.title_id, SAMPLE_SERIAL) != 0 || strcmp(r.save_id, "5454001B") != 0) {
        fprintf(stderr, "FAIL %s: title_id='%s' save_id='%s'\n",
                label, r.title_id, r.save_id);
        return 1;
    }
    if (r.platform != SIGIL_PLATFORM_XBOX || r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "FAIL %s: platform=%d source=%d\n",
                label, (int)r.platform, (int)r.source);
        return 1;
    }
    return 0;
}

/* Reading the same stream backwards forces the decoder restart that emulated
 * seeking depends on; without it the second read would return the wrong bytes
 * or fail outright. */
static int test_backward_seek(const uint8_t *img) {
    char path[512];
    snprintf(path, sizeof(path), "%s/sigil_zip_seek.zip",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (write_zip(path, img, IMAGE_BYTES, 8) != 0) return 1;

    char inner[512];
    sigil_io *io = sigil_io_open_zip(path, inner, sizeof(inner));
    if (!io) {
        fprintf(stderr, "FAIL backward_seek: open failed\n");
        remove(path);
        return 1;
    }

    int bad = 0;
    uint8_t late[20], early[4];
    if (io->read(io->ctx, VD_SECTOR * XDVDFS_SECTOR, late, sizeof(late)) != (int)sizeof(late)
        || memcmp(late, "MICROSOFT*XBOX*MEDIA", 20) != 0) {
        fprintf(stderr, "FAIL backward_seek: forward read wrong\n");
        bad = 1;
    }
    if (!bad && (io->read(io->ctx, 0, early, sizeof(early)) != (int)sizeof(early)
                 || memcmp(early, img, 4) != 0)) {
        fprintf(stderr, "FAIL backward_seek: rewind read wrong\n");
        bad = 1;
    }
    if (!bad && strcmp(inner, MEMBER_NAME) != 0) {
        fprintf(stderr, "FAIL backward_seek: inner name '%s'\n", inner);
        bad = 1;
    }

    sigil_io_close(io);
    remove(path);
    return bad;
}

#if SIGIL_TEST_FILENAME_FALLBACK
/* A PS Vita release is a .zip whose identifier lives in the archive's own file
 * name, and whose members name no console. The container reader must not
 * swallow it and report that nothing inside looks like a game. Only meaningful
 * when the filename scanner is in the build. */
static int test_vita_zip_still_resolves_by_name(const uint8_t *img) {
    char dir[400], path[512];
    snprintf(dir, sizeof(dir), "%s", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    snprintf(path, sizeof(path), "%s/Some Game [PCSE12345].zip", dir);
    if (write_zip_named(path, "app/eboot.bin", img, IMAGE_BYTES, 8) != 0) return 1;

    sigil_result r;
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_PSVITA, NULL, &r);
    if (rc != SIGIL_OK || r.platform != SIGIL_PLATFORM_PSVITA
        || strcmp(r.title_id, "PCSE12345") != 0
        || r.source != SIGIL_SOURCE_FILENAME) {
        fprintf(stderr, "FAIL vita_zip hinted: rc=%d platform=%d title_id='%s'\n",
                rc, (int)r.platform, r.title_id);
        remove(path);
        return 1;
    }

    /* Without a hint the generic scanner claims the serial for another
     * platform, which is what it did before archives were readable. What
     * matters here is only that the id still comes out of the file name
     * rather than being lost to a failed look inside. */
    rc = sigil_extract_from_path(path, SIGIL_PLATFORM_AUTO, NULL, &r);
    remove(path);
    if (rc != SIGIL_OK || strcmp(r.title_id, "PCSE12345") != 0
        || r.source != SIGIL_SOURCE_FILENAME) {
        fprintf(stderr, "FAIL vita_zip auto: rc=%d title_id='%s' source=%d\n",
                rc, r.title_id, (int)r.source);
        return 1;
    }
    return 0;
}
#endif

/* Addressing a member by path suffix is what reaches a file buried under a
 * directory whose name varies per title, and it must not be fooled into
 * returning the largest member instead. */
static int test_member_by_suffix(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/sigil_zip_member.zip",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");

    static const char payload[] = "small-metadata-file";
    if (write_zip_named(path, "app/PCSE00695/sce_sys/param.sfo",
                        (const uint8_t *)payload, sizeof(payload) - 1, 8) != 0) {
        return 1;
    }

    sigil_io *io = sigil_io_open_zip_member(path, "sce_sys/param.sfo");
    int bad = 0;
    if (!io) {
        fprintf(stderr, "FAIL member_by_suffix: not found\n");
        bad = 1;
    } else {
        char buf[64] = {0};
        int n = io->read(io->ctx, 0, buf, sizeof(buf) - 1);
        if (n != (int)(sizeof(payload) - 1) || strcmp(buf, payload) != 0) {
            fprintf(stderr, "FAIL member_by_suffix: got %d bytes '%s'\n", n, buf);
            bad = 1;
        }
        sigil_io_close(io);
    }

    if (!bad) {
        sigil_io *miss = sigil_io_open_zip_member(path, "nothing/here.bin");
        if (miss) {
            fprintf(stderr, "FAIL member_by_suffix: matched a missing member\n");
            sigil_io_close(miss);
            bad = 1;
        }
    }

    remove(path);
    return bad;
}

/* A Vita dump can hold two files named sce_sys/param.sfo: the title's own, and
 * a second one under savedata/ that carries no TITLE_ID. Only the shallower one
 * describes the title, and the central directory lists the savedata copy first
 * in real dumps, so first-match would pick the wrong file. */
static int test_shallowest_match_wins(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/sigil_zip_depth.zip",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");

    static const char deep[]    = "savedata-copy-no-title-id";
    static const char shallow[] = "the-real-app-metadata";

    if (write_zip_two(path,
                      "app/PCSG00672/savedata/sce_sys/param.sfo",
                      (const uint8_t *)deep, sizeof(deep) - 1,
                      "app/PCSG00672/sce_sys/param.sfo",
                      (const uint8_t *)shallow, sizeof(shallow) - 1) != 0) {
        fprintf(stderr, "FAIL depth: could not write fixture\n");
        return 1;
    }

    sigil_io *io = sigil_io_open_zip_member(path, "sce_sys/param.sfo");
    int bad = 0;
    if (!io) {
        fprintf(stderr, "FAIL depth: not found\n");
        bad = 1;
    } else {
        char buf[64] = {0};
        int n = io->read(io->ctx, 0, buf, sizeof(buf) - 1);
        if (n != (int)(sizeof(shallow) - 1) || strcmp(buf, shallow) != 0) {
            fprintf(stderr, "FAIL depth: picked '%s'\n", buf);
            bad = 1;
        }
        sigil_io_close(io);
    }
    remove(path);
    return bad;
}

int main(void) {
    uint8_t *img = build_image();
    if (!img) return 1;

    int bad = 0;
    bad |= run_case("deflate", 8, img);
    bad |= run_case("store", 0, img);
    bad |= test_backward_seek(img);
#if SIGIL_TEST_FILENAME_FALLBACK
    bad |= test_vita_zip_still_resolves_by_name(img);
#endif
    bad |= test_member_by_suffix();
    bad |= test_shallowest_match_wins();
    free(img);

    if (bad) return 1;
    printf("ok unit_zip (deflate + store + rewind)\n");
    return 0;
}
