// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

/* An Xbox 360 title is identified by the 32-bit id in the XEX execution-info
 * header, rendered as 8 hex digits. Unlike the original Xbox there is no
 * publisher-letter form; the hex is what the console, Xenia and XenDroid all
 * use, so title_id and save_id are the same string.
 *
 * The XEX is reached from a bare default.xex, from a disc image through the
 * shared XDVDFS walker, or from a .zar whose member IO hands over the same
 * bytes. All three arrive here as an offset into some stream. */

#define XEX_MIN_HEADER          0x18
#define XEX_MAX_OPT_HEADERS     128
#define XEX_HEADER_EXEC_INFO    0x00040006u
#define XEX_TITLE_ID_OFFSET     0x0C

#define XBOX360_BOOT_FILE       "default.xex"

static bool xex_magic_ok(const uint8_t *p) {
    return (p[0] == 'X' && p[1] == 'E' && p[2] == 'X'
            && (p[3] == '2' || p[3] == '1'));
}

/* Reads the title id of the XEX beginning at `xex_off`. Optional header values
 * are offsets from the XEX start, not from the file, so a XEX embedded in a
 * disc image needs its base added back to every one of them. */
static int xex_read_title_id(const sigil_io *io, uint64_t xex_off, uint32_t *out_id) {
    uint8_t header[XEX_MIN_HEADER];
    if (sigil_io_read_exact(io, xex_off, header, sizeof(header)) != SIGIL_OK) {
        return SIGIL_ERR_IO;
    }
    if (!xex_magic_ok(header)) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    uint32_t header_count = sigil_read_be32(header + 0x14);
    if (header_count == 0 || header_count > XEX_MAX_OPT_HEADERS) {
        return SIGIL_ERR_NOT_FOUND;
    }

    for (uint32_t i = 0; i < header_count; i++) {
        uint8_t entry[8];
        uint64_t entry_off = xex_off + (uint64_t)XEX_MIN_HEADER + (uint64_t)i * 8;
        if (sigil_io_read_exact(io, entry_off, entry, sizeof(entry)) != SIGIL_OK) {
            return SIGIL_ERR_IO;
        }
        uint32_t key   = sigil_read_be32(entry);
        uint32_t value = sigil_read_be32(entry + 4);
        if (key != XEX_HEADER_EXEC_INFO) continue;

        uint8_t tid_bytes[4];
        uint64_t tid_off = xex_off + (uint64_t)value + XEX_TITLE_ID_OFFSET;
        if (sigil_io_read_exact(io, tid_off, tid_bytes, sizeof(tid_bytes)) != SIGIL_OK) {
            return SIGIL_ERR_IO;
        }
        *out_id = sigil_read_be32(tid_bytes);
        return SIGIL_OK;
    }
    return SIGIL_ERR_NOT_FOUND;
}

int sigil_extract_xbox360(const sigil_io *io, const char *filename_hint,
                          const sigil_options *opts, sigil_result *out) {
    (void)filename_hint;
    (void)opts;

    sigil_result_init(out);
    out->platform     = SIGIL_PLATFORM_XBOX360;
    out->usage        = SIGIL_USAGE_FOLDER_EXACT;
    out->experimental = 1;

    if (!io || !io->read || !io->size) return SIGIL_ERR_INVALID_ARG;

    /* Decide from the magic rather than from an error code, so a disc image
     * whose first sector is unreadable is not mistaken for a bare executable.
     * A .zar arrives already narrowed to its default.xex member and so takes
     * this same path. */
    uint64_t xex_off = 0;
    uint8_t magic[4];
    bool is_bare_xex = sigil_io_read_exact(io, 0, magic, sizeof(magic)) == SIGIL_OK
                       && xex_magic_ok(magic);

    if (!is_bare_xex) {
        uint32_t xex_size;
        int frc = sigil_xdvdfs_find_root_file(io, XBOX360_BOOT_FILE, &xex_off, &xex_size);
        if (frc != SIGIL_OK) return frc;
        if (xex_size < XEX_MIN_HEADER) return SIGIL_ERR_NOT_FOUND;
    }

    uint32_t title_id;
    int rc = xex_read_title_id(io, xex_off, &title_id);
    if (rc != SIGIL_OK) return rc;

    uint8_t be[4] = {
        (uint8_t)(title_id >> 24), (uint8_t)(title_id >> 16),
        (uint8_t)(title_id >> 8),  (uint8_t)title_id
    };
    sigil_hex_encode_4(be, out->title_id);
    memcpy(out->raw_serial, out->title_id, strlen(out->title_id) + 1);
    out->source = SIGIL_SOURCE_BINARY;
    return SIGIL_OK;
}
