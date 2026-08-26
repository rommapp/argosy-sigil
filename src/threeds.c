// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

#if SIGIL_WITH_3DS

#include <stdlib.h>
#include <zstd.h>

/* NCSD: header magic at 0x100, partition table at 0x120, entries of
 * {u32 offset, u32 size} in 0x200-byte media units (azahar
 * src/core/file_sys/ncch_container.cpp:26,210 fixes the unit at 0x200).
 * NCCH: magic at 0x100 of the NCCH itself, program id at +0x118
 * (azahar src/core/file_sys/ncch_container.h:43 NCCH_Header). A stock NCSD
 * puts partition 0 at 0x4000, which is why the flat 0x4118 works for .3ds. */

#define NCSD_MAGIC_OFFSET         0x100
#define NCSD_PARTITION_TABLE      0x120
#define NCSD_MEDIA_UNIT           0x200
#define NCSD_PARTITION0_FALLBACK  0x4000
#define NCCH_MAGIC_OFFSET         0x100
#define NCCH_PROGRAM_ID_OFFSET    0x118
#define PROGRAM_ID_LEN            8

/* Z3DS wrapper (azahar src/common/file_derived.h, struct Z3DSFileHeader,
 * static_assert sizeof == 0x20). Fields are written as a raw struct dump
 * (file_derived.cpp:512), so every scalar is host-native little-endian:
 *   0x00 magic[4] 'Z','3','D','S'   0x04 underlying_magic[4]
 *   0x08 version u8                 0x09 reserved u8
 *   0x0A header_size u16            0x0C metadata_size u32
 *   0x10 compressed_size u64        0x18 uncompressed_size u64
 * Metadata sits at header_size for metadata_size bytes and the seekable-zstd
 * payload at header_size + metadata_size (file_derived.cpp:745,778). */

#define Z3DS_HEADER_SIZE   0x20
#define Z3DS_VERSION       1
#define Z3DS_MAX_PREFIX    (4u * 1024u * 1024u)
#define ZSTD_CHUNK_SIZE    (64 * 1024)

typedef enum {
    THREEDS_INNER_UNKNOWN = 0,
    THREEDS_INNER_NCSD,
    THREEDS_INNER_NCCH,
    THREEDS_INNER_HOMEBREW,
} threeds_inner;

typedef struct {
    uint8_t  underlying_magic[4];
    uint16_t header_size;
    uint32_t metadata_size;
    uint64_t uncompressed_size;
} z3ds_header;

/* Random access over the logical (decompressed) 3DS image. A plain file reads
 * straight through; a Z3DS payload is streamed into a growable prefix, so only
 * as much as the program id needs is ever decompressed. */
typedef struct {
    const sigil_io *io;
    bool            compressed;
    uint64_t        in_off;
    ZSTD_DCtx      *dctx;
    uint8_t        *in_buf;
    size_t          in_len;
    size_t          in_pos;
    uint8_t        *prefix;
    size_t          prefix_len;
    size_t          prefix_cap;
    bool            exhausted;
} threeds_image;

static bool retail_3ds(const char *tid) {
    return tid[0] == '0' && tid[1] == '0' && tid[2] == '0' && tid[3] == '4';
}

static int z3ds_parse_header(const uint8_t raw[Z3DS_HEADER_SIZE], z3ds_header *out) {
    if (memcmp(raw, "Z3DS", 4) != 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    if (raw[8] != Z3DS_VERSION) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    memcpy(out->underlying_magic, raw + 4, 4);
    out->header_size       = (uint16_t)((uint16_t)raw[10] | ((uint16_t)raw[11] << 8));
    out->metadata_size     = sigil_read_le32(raw + 12);
    out->uncompressed_size = sigil_read_le64(raw + 24);

    if (out->header_size < Z3DS_HEADER_SIZE) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    return SIGIL_OK;
}

static void image_init_plain(threeds_image *img, const sigil_io *io) {
    memset(img, 0, sizeof(*img));
    img->io = io;
}

static int image_init_compressed(threeds_image *img, const sigil_io *io, uint64_t payload_off) {
    memset(img, 0, sizeof(*img));
    img->io         = io;
    img->compressed = true;
    img->in_off     = payload_off;
    img->dctx       = ZSTD_createDCtx();
    if (!img->dctx) return SIGIL_ERR_OOM;
    img->in_buf = (uint8_t *)malloc(ZSTD_CHUNK_SIZE);
    if (!img->in_buf) {
        ZSTD_freeDCtx(img->dctx);
        img->dctx = NULL;
        return SIGIL_ERR_OOM;
    }
    return SIGIL_OK;
}

static void image_free(threeds_image *img) {
    if (img->dctx) ZSTD_freeDCtx(img->dctx);
    free(img->in_buf);
    free(img->prefix);
    memset(img, 0, sizeof(*img));
}

static int image_extend(threeds_image *img, size_t need) {
    if (need > Z3DS_MAX_PREFIX) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    if (img->prefix_len >= need) return SIGIL_OK;

    if (img->prefix_cap < need) {
        size_t cap = img->prefix_cap ? img->prefix_cap : (size_t)ZSTD_CHUNK_SIZE;
        while (cap < need) cap *= 2;
        uint8_t *grown = (uint8_t *)realloc(img->prefix, cap);
        if (!grown) return SIGIL_ERR_OOM;
        img->prefix     = grown;
        img->prefix_cap = cap;
    }

    while (img->prefix_len < need && !img->exhausted) {
        if (img->in_pos >= img->in_len) {
            int got = img->io->read(img->io->ctx, img->in_off, img->in_buf, ZSTD_CHUNK_SIZE);
            if (got <= 0) {
                img->exhausted = true;
                break;
            }
            img->in_off += (uint64_t)got;
            img->in_len = (size_t)got;
            img->in_pos = 0;
        }

        ZSTD_inBuffer  in  = { img->in_buf, img->in_len, img->in_pos };
        ZSTD_outBuffer out = { img->prefix, need, img->prefix_len };

        size_t r = ZSTD_decompressStream(img->dctx, &out, &in);
        bool progressed = in.pos > img->in_pos || out.pos > img->prefix_len;
        img->in_pos     = in.pos;
        img->prefix_len = out.pos;

        if (ZSTD_isError(r) || !progressed) img->exhausted = true;
    }

    return img->prefix_len >= need ? SIGIL_OK : SIGIL_ERR_NOT_FOUND;
}

static int image_read(threeds_image *img, uint64_t off, void *buf, size_t len) {
    if (!img->compressed) return sigil_io_read_exact(img->io, off, buf, len);
    if (off > Z3DS_MAX_PREFIX || len > Z3DS_MAX_PREFIX - off) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    int rc = image_extend(img, (size_t)(off + len));
    if (rc != SIGIL_OK) return rc;
    memcpy(buf, img->prefix + (size_t)off, len);
    return SIGIL_OK;
}

static threeds_inner inner_from_magic(const uint8_t magic[4]) {
    if (memcmp(magic, "NCSD", 4) == 0) return THREEDS_INNER_NCSD;
    if (memcmp(magic, "NCCH", 4) == 0) return THREEDS_INNER_NCCH;
    if (memcmp(magic, "3DSX", 4) == 0) return THREEDS_INNER_HOMEBREW;
    return THREEDS_INNER_UNKNOWN;
}

static threeds_inner inner_from_content(threeds_image *img) {
    uint8_t head[4];
    if (image_read(img, 0, head, sizeof(head)) == SIGIL_OK) {
        if (memcmp(head, "3DSX", 4) == 0) return THREEDS_INNER_HOMEBREW;
        if (head[0] == 0x7F && memcmp(head + 1, "ELF", 3) == 0) return THREEDS_INNER_HOMEBREW;
    }
    uint8_t magic[4];
    if (image_read(img, NCSD_MAGIC_OFFSET, magic, sizeof(magic)) == SIGIL_OK) {
        if (memcmp(magic, "NCSD", 4) == 0) return THREEDS_INNER_NCSD;
        if (memcmp(magic, "NCCH", 4) == 0) return THREEDS_INNER_NCCH;
    }
    return THREEDS_INNER_UNKNOWN;
}

static threeds_inner inner_from_ext(const char *ext) {
    if (strcmp(ext, "cxi") == 0 || strcmp(ext, "zcxi") == 0 || strcmp(ext, "app") == 0) {
        return THREEDS_INNER_NCCH;
    }
    if (strcmp(ext, "3dsx") == 0 || strcmp(ext, "z3dsx") == 0
        || strcmp(ext, "elf") == 0 || strcmp(ext, "axf") == 0) {
        return THREEDS_INNER_HOMEBREW;
    }
    return THREEDS_INNER_NCSD;
}

static uint64_t ncsd_partition0_offset(threeds_image *img) {
    uint8_t magic[4];
    if (image_read(img, NCSD_MAGIC_OFFSET, magic, sizeof(magic)) != SIGIL_OK) {
        return NCSD_PARTITION0_FALLBACK;
    }
    if (memcmp(magic, "NCSD", 4) != 0) return NCSD_PARTITION0_FALLBACK;

    uint8_t entry[4];
    if (image_read(img, NCSD_PARTITION_TABLE, entry, sizeof(entry)) != SIGIL_OK) {
        return NCSD_PARTITION0_FALLBACK;
    }
    uint64_t off = (uint64_t)sigil_read_le32(entry) * NCSD_MEDIA_UNIT;
    return off ? off : NCSD_PARTITION0_FALLBACK;
}

static int read_program_id(threeds_image *img, threeds_inner inner, uint8_t out[PROGRAM_ID_LEN]) {
    uint64_t ncch_off = (inner == THREEDS_INNER_NCSD) ? ncsd_partition0_offset(img) : 0;
    return image_read(img, ncch_off + NCCH_PROGRAM_ID_OFFSET, out, PROGRAM_ID_LEN);
}

int sigil_extract_3ds(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out) {
    if (!io) return SIGIL_ERR_INVALID_ARG;

    sigil_result_init(out);
    out->platform = SIGIL_PLATFORM_3DS;
    out->usage = SIGIL_USAGE_FOLDER_SPLIT;

    char ext[16];
    sigil_lower_ext(filename_hint, ext);

    uint8_t wrapper[Z3DS_HEADER_SIZE];
    z3ds_header z;
    bool wrapped = sigil_io_read_exact(io, 0, wrapper, sizeof(wrapper)) == SIGIL_OK
                   && z3ds_parse_header(wrapper, &z) == SIGIL_OK;

    threeds_image img;
    threeds_inner inner = THREEDS_INNER_UNKNOWN;
    int rc;

    if (wrapped) {
        rc = image_init_compressed(&img, io, (uint64_t)z.header_size + z.metadata_size);
        if (rc != SIGIL_OK) return rc;
        inner = inner_from_magic(z.underlying_magic);
    } else {
        image_init_plain(&img, io);
    }

    if (inner == THREEDS_INNER_UNKNOWN) inner = inner_from_content(&img);
    if (inner == THREEDS_INNER_UNKNOWN) inner = inner_from_ext(ext);

    if (inner == THREEDS_INNER_HOMEBREW) {
        image_free(&img);
        return SIGIL_ERR_NOT_FOUND;
    }

    uint8_t pid[PROGRAM_ID_LEN];
    rc = read_program_id(&img, inner, pid);
    image_free(&img);
    if (rc != SIGIL_OK) return rc;

    char tid[17];
    sigil_hex_encode_8(pid, tid, true);

    bool allow_hb = opts && (opts->flags & SIGIL_FLAG_3DS_ALLOW_HOMEBREW);
    if (!allow_hb && !retail_3ds(tid)) return SIGIL_ERR_NOT_FOUND;

    memcpy(out->title_id,  tid, 17);
    memcpy(out->raw_serial, tid, 17);

    /* save_id is the on-disk save location, not the flat id: the 3DS nests it
     * as title/<high 8>/<low 8>/, so emit 00040000/00033500 and the consumer
     * places it without knowing the split. usage=folder-split marks it as a
     * '/'-separated path; the sdmc/.../title/ root above is the emulator's.
     *
     * Lowercase is upstream-exact, not cosmetic: azahar writes the two segments
     * with fmt "{:08x}" (src/core/hle/service/am/am.cpp:1324), and Android
     * internal storage is case-sensitive, so an uppercase segment becomes a
     * second directory beside the emulator's own and the saves split. title_id
     * stays uppercase; only the path-shaped value is cased to match. */
    for (int i = 0; i < 8; i++) out->save_id[i] = sigil_to_lower(tid[i]);
    out->save_id[8] = '/';
    for (int i = 0; i < 8; i++) out->save_id[9 + i] = sigil_to_lower(tid[8 + i]);
    out->save_id[17] = '\0';

    out->source = SIGIL_SOURCE_BINARY;
    return SIGIL_OK;
}

#endif
