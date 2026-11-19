// SPDX-License-Identifier: MPL-2.0
/* Xbox 360 content headers on Xenia. Each save folder has a header beside it,
 * in one of two forms:
 *
 * - XCONTENT_AGGREGATE_DATA alone, 0x148 bytes, as Xenia Edge and XenDroid
 *   write it (content_manager.cc WriteContentHeaderFile, Edge at 4ad875a and
 *   XenDroid at 779680a; content_manager.h XCONTENT_AGGREGATE_DATA): device id,
 *   content type, display name, file name, XUID, title id, big-endian.
 * - The whole container header, XContentHeader then XContentMetadata, then a
 *   42-byte file name of Xenia's own, padded to 0xA000 bytes, as Xenia Canary
 *   writes it (content_manager.cc ExtractContentHeader at 27d11a2;
 *   xcontent/xcontent.h XContentContainerHeader).
 *
 * The unit carries each header as its emulator wrote it. Restore writes what
 * the layout's folder names (sigil_header_form): for Canary the container,
 * built from an aggregate as Canary's FillHeader builds it; for XenDroid the
 * aggregate, built from a container as Edge's ContentDataFromHeader reads one;
 * for Edge, which reads both and writes a header only for a save it creates,
 * the header as the unit holds it. Identity covers what both forms carry, so a
 * header moved between them is the same save. */
#include "sync_internal.h"
#include <inttypes.h>

#define AGG_SIZE        0x148u
#define AGG_DEVICE      0x000u
#define AGG_TYPE        0x004u
#define AGG_NAME        0x008u   /* 128 UTF-16BE units */
#define AGG_FILE        0x108u   /* 42 bytes, no terminator needed */
#define AGG_XUID        0x138u
#define AGG_TITLE       0x140u

#define CON_METADATA_END 0x971Au  /* sizeof(XContentHeader) + sizeof(XContentMetadata) */
#define CON_FILE_SIZE    0xA000u  /* round_up(sizeof(XContentContainerHeader), 0x1000) */
#define CON_HEADER_SIZE  0x340u
#define CON_TYPE         0x344u
#define CON_VERSION      0x348u
#define CON_TITLE        0x360u   /* execution_info.title_id */
#define CON_PROFILE      0x371u   /* profile_id */
#define CON_FILE_COUNT   0x39Du   /* data_file_count */
#define CON_NAMES        0x411u   /* display_name_raw: 9 languages of 128 UTF-16BE units */
#define CON_NAMES_EX     0x541Au  /* display_name_ex_raw: 3 more, metadata_version >= 2 */
#define CON_FILE         0x971Au  /* Xenia's own file name, 42 bytes */

#define NAME_UNITS  128u
#define FILE_BYTES  42u
#define SAVED_GAME  1u   /* XContentType::kSavedGame */

static bool magic_valid(uint32_t magic) {
    return magic == 0x434F4E20u || magic == 0x4C495645u || magic == 0x50495253u;  /* CON , LIVE, PIRS */
}

/* By content, as both emulators read it. Canary (xcontent/
 * xcontent_package_directory.cc ReadContentHeaderFile at 27d11a2) reads one
 * shorter than the container's metadata, of at least an aggregate's size, as
 * the aggregate; one of exactly the metadata's or the padded size as the
 * container, without checking the magic, and lists the save only when the
 * magic is valid (IsValidPackage); any other size not at all. Edge
 * (content_manager.cc ReadContentHeaderFile at 4ad875a) reads one of at least
 * the metadata's size whose magic is valid as the container, and any other of
 * at least an aggregate's size by its first 0x148 bytes as the aggregate.
 * Anything else the two read differently, or neither reads: SIGIL_HEADER_NONE,
 * which restore copies as it is. */
int sigil_x360_header_form(const uint8_t *h, size_t len) {
    if (!h || len < AGG_SIZE) return SIGIL_HEADER_NONE;
    if (len < CON_METADATA_END) return SIGIL_HEADER_XCONTENT_AGGREGATE;
    if ((len == CON_METADATA_END || len == CON_FILE_SIZE) && magic_valid(sigil_read_be32(h))) {
        return SIGIL_HEADER_XCONTENT_CONTAINER;
    }
    return SIGIL_HEADER_NONE;
}

/* The units of a UTF-16BE name up to its terminator, at most `max`. */
static size_t name_units(const uint8_t *name, size_t max) {
    size_t n = 0;
    while (n < max && (name[2 * n] || name[2 * n + 1])) n++;
    return n;
}

/* The display name a container header gives, as Edge's ContentDataFromHeader
 * reads it (vfs/devices/xcontent_container_device.cc at 4ad875a): English,
 * else the first language that has one, the last three only with
 * metadata_version 2. */
static const uint8_t *container_name(const uint8_t *h, size_t *units) {
    uint32_t version = sigil_read_be32(h + CON_VERSION);
    for (size_t lang = 0; lang < 12; lang++) {
        if (lang >= 9 && version < 2) break;
        const uint8_t *name = lang < 9 ? h + CON_NAMES + lang * NAME_UNITS * 2 : h + CON_NAMES_EX + (lang - 9) * NAME_UNITS * 2;
        size_t n = name_units(name, NAME_UNITS);
        if (n) {
            *units = n;
            return name;
        }
    }
    *units = 0;
    return h + CON_NAMES;
}

/* What both forms carry: content type, display name, file name, title id. */
typedef struct {
    uint32_t       type;
    uint32_t       title;
    const uint8_t *name;
    size_t         name_units;
    uint8_t        file[FILE_BYTES];
} header_fields;

/* `package` is the save folder's name, the header's own without ".header". */
static bool read_fields(const uint8_t *h, size_t len, const char *package, header_fields *f) {
    memset(f, 0, sizeof(*f));
    int form = sigil_x360_header_form(h, len);
    if (form == SIGIL_HEADER_XCONTENT_AGGREGATE) {
        f->type = sigil_read_be32(h + AGG_TYPE);
        f->title = sigil_read_be32(h + AGG_TITLE);
        f->name = h + AGG_NAME;
        f->name_units = name_units(f->name, NAME_UNITS);
        memcpy(f->file, h + AGG_FILE, FILE_BYTES);
        return true;
    }
    if (form != SIGIL_HEADER_XCONTENT_CONTAINER) return false;
    f->type = sigil_read_be32(h + CON_TYPE);
    f->title = sigil_read_be32(h + CON_TITLE);
    f->name = container_name(h, &f->name_units);
    /* A save's own name, else the folder's (Edge content_manager.cc
     * ReadContentHeaderFile at 4ad875a, Canary xcontent/xcontent_package.cc
     * GetContentMetadata at 27d11a2). */
    const uint8_t *saved = len >= CON_FILE + FILE_BYTES ? h + CON_FILE : NULL;
    if (f->type == SAVED_GAME && saved && saved[0]) {
        memcpy(f->file, saved, FILE_BYTES);
    } else if (package) {
        size_t n = strlen(package);
        memcpy(f->file, package, n < FILE_BYTES ? n : FILE_BYTES);
    }
    return true;
}

/* The aggregate as Edge fills one from a container (ContentDataFromHeader:
 * device id 1, the rest zero) and writes it (WriteContentHeaderFile): the
 * display name at most 127 units with its terminator (XCONTENT_DATA
 * set_display_name, copy_and_swap_truncating), the file name at most 42
 * bytes (set_file_name), XUID 0 as for a save a game makes by passing
 * XCONTENT_DATA (content_manager.h, xam_content.cc xeXamContentCreate). */
static void build_aggregate(const header_fields *f, uint8_t out[AGG_SIZE]) {
    memset(out, 0, AGG_SIZE);
    sigil_write_be32(out + AGG_DEVICE, 1);
    sigil_write_be32(out + AGG_TYPE, f->type);
    size_t n = f->name_units < NAME_UNITS - 1 ? f->name_units : NAME_UNITS - 1;
    memcpy(out + AGG_NAME, f->name, 2 * n);
    memcpy(out + AGG_FILE, f->file, FILE_BYTES);
    sigil_write_be32(out + AGG_TITLE, f->title);
}

/* The container as Canary's FillHeader fills one from an aggregate
 * (xcontent/xcontent_package_directory.cc at 27d11a2) in a header that
 * starts zeroed (ContentPackage header_()), and ExtractContentHeader writes
 * it padded to 0xA000 (content_manager.cc): magic CON, header_size,
 * content type, metadata_version 2, one data file, STFS (0), the title id,
 * the English display name at most 127 units with its terminator
 * (set_display_name, copy_and_swap_truncating), and Xenia's file name. */
static void build_container(const header_fields *f, uint8_t *out) {
    memset(out, 0, CON_FILE_SIZE);
    sigil_write_be32(out, 0x434F4E20u);
    sigil_write_be32(out + CON_HEADER_SIZE, CON_METADATA_END);
    sigil_write_be32(out + CON_TYPE, f->type);
    sigil_write_be32(out + CON_VERSION, 2);
    sigil_write_be32(out + CON_TITLE, f->title);
    sigil_write_be32(out + CON_FILE_COUNT, 1);
    size_t n = f->name_units < NAME_UNITS - 1 ? f->name_units : NAME_UNITS - 1;
    memcpy(out + CON_NAMES, f->name, 2 * n);
    size_t file = strnlen((const char *)f->file, FILE_BYTES);
    memcpy(out + CON_FILE, f->file, file);
}

/* Copies `h` into a new buffer. */
static int copied(const uint8_t *h, size_t len, uint8_t **out, size_t *out_len) {
    *out = (uint8_t *)malloc(len ? len : 1);
    if (!*out) return SIGIL_ERR_OOM;
    memcpy(*out, h, len);
    *out_len = len;
    return SIGIL_OK;
}

int sigil_x360_header_to(int form, const uint8_t *h, size_t len, const char *package, const char *profile,
                         uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    int from = sigil_x360_header_form(h, len);
    if (form == SIGIL_HEADER_XCONTENT_EITHER || from == SIGIL_HEADER_NONE ||
        (from == form && form == SIGIL_HEADER_XCONTENT_AGGREGATE)) {
        return copied(h, len, out, out_len);
    }
    if (from == form) {
        int rc = copied(h, len, out, out_len);
        /* For a save it makes under a profile Canary writes that profile's
         * (xcontent/xcontent_package.cc InitializePackageHeader), so under
         * another profile the header names that one. Canary hands profile_id
         * to a game as the save's XUID only through its aggregate enumerator
         * (xam_content_aggregate.cc, with GetContentMetadata); its plain one
         * writes XCONTENT_DATA, without it (xenumerator.cc WriteItems). A game
         * that passes the aggregate back to XamContentCreate has the save
         * opened in that XUID's folder when it isn't 0 (content_manager.cc
         * ResolvePackagePath), all at 27d11a2. */
        uint64_t xuid = 0;
        if (rc == SIGIL_OK && profile && sigil_read_be64(h + CON_PROFILE) &&
            sscanf(profile, "%16" SCNx64, &xuid) == 1 && strlen(profile) == 16) {
            sigil_write_be64(*out + CON_PROFILE, xuid);
        }
        return rc;
    }
    size_t size = form == SIGIL_HEADER_XCONTENT_CONTAINER ? CON_FILE_SIZE : AGG_SIZE;
    *out = (uint8_t *)malloc(size);
    if (!*out) return SIGIL_ERR_OOM;
    *out_len = size;
    header_fields f;
    read_fields(h, len, package, &f);
    if (form == SIGIL_HEADER_XCONTENT_CONTAINER) build_container(&f, *out);
    else build_aggregate(&f, *out);
    return SIGIL_OK;
}

bool sigil_x360_header_md5(const uint8_t *h, size_t len, const char *package, char md5[33]) {
    header_fields f;
    if (!read_fields(h, len, package, &f)) return false;
    uint8_t shared[AGG_SIZE];
    build_aggregate(&f, shared);
    /* The file name as the container keeps it, which stops at a terminator. */
    size_t file = strnlen((const char *)f.file, FILE_BYTES);
    memset(shared + AGG_FILE + file, 0, FILE_BYTES - file);
    sigil_md5_of(shared, sizeof(shared), md5);
    return true;
}

/* The XUID a header names: Canary's container keeps the signed-in profile's
 * in profile_id for a save it makes (xam_content.cc and
 * xcontent/xcontent_package.cc InitializePackageHeader at 27d11a2); Edge's
 * aggregate keeps one only when a game passed it (WriteContentHeaderFile at
 * 4ad875a), 0 otherwise. 0 and all ones name none. */
void sigil_x360_header_maker(const uint8_t *h, size_t len, char out[SIGIL_PROFILE_ID_MAX]) {
    out[0] = '\0';
    int form = sigil_x360_header_form(h, len);
    if (form == SIGIL_HEADER_NONE) return;
    uint64_t xuid = sigil_read_be64(h + (form == SIGIL_HEADER_XCONTENT_CONTAINER ? CON_PROFILE : AGG_XUID));
    if (xuid && xuid != UINT64_MAX) snprintf(out, SIGIL_PROFILE_ID_MAX, "%016" PRIX64, xuid);
}
