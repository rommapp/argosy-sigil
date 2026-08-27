// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"
#include <stdio.h>

/* An original Xbox game is identified by the 32-bit title id in the XBE
 * certificate. Two forms of that id matter and they are not the same string.
 * The console names its save directory after the raw hex (E:\UDATA\4D530064),
 * while the serial printed on the disc and shown by every tool is the two
 * publisher letters plus the low half in decimal (MS-100). So title_id and
 * save_id diverge here the way they do on PS2 and 3DS.
 *
 * The XBE is reached either directly, when the caller hands over a bare
 * default.xbe, or by walking the disc's XDVDFS filesystem. */

#define XBE_MAGIC             "XBEH"
#define XBE_MAGIC_LEN         4
#define XBE_HEADER_MIN        0x11C
#define XBE_BASE_ADDR_OFF     0x104
#define XBE_CERT_ADDR_OFF     0x118
#define XBE_CERT_HEADER       0x0C
#define XBE_CERT_TITLE_ID_OFF 0x08
/* Retail certificates run a few hundred bytes. The bound exists so a wild
 * pointer is rejected before it becomes a read offset. */
#define XBE_CERT_MAX_SIZE     0x1000

#define XBOX_BOOT_FILE        "default.xbe"

/* Cxbx-Reloaded's FormatTitleId (src/common/xbe/XbePrinter.cpp). Retail ids
 * carry two printable publisher letters; the dashboard and XDK samples do not
 * and fall back to plain hex, so the check is on the prefix bytes rather than
 * on any list of known publishers. */
static void xbe_format_title_id(uint32_t title_id, char out[32]) {
    char hi = (char)((title_id >> 24) & 0xFF);
    char lo = (char)((title_id >> 16) & 0xFF);

    if (sigil_is_upper(hi) && sigil_is_upper(lo)) {
        snprintf(out, 32, "%c%c-%03u", hi, lo, (unsigned)(title_id & 0xFFFF));
    } else {
        snprintf(out, 32, "%08X", (unsigned)title_id);
    }
}

/* Reads the certificate of the XBE beginning at `xbe_off`. The certificate is
 * addressed by the virtual address the image would load at, so the file offset
 * is the difference between it and the image base. */
static int xbe_read_title_id(const sigil_io *io, uint64_t xbe_off, uint32_t *out_id) {
    uint8_t header[XBE_HEADER_MIN];
    if (sigil_io_read_exact(io, xbe_off, header, sizeof(header)) != SIGIL_OK) {
        return SIGIL_ERR_IO;
    }
    if (memcmp(header, XBE_MAGIC, XBE_MAGIC_LEN) != 0) {
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }

    uint32_t base_addr = sigil_read_le32(header + XBE_BASE_ADDR_OFF);
    uint32_t cert_addr = sigil_read_le32(header + XBE_CERT_ADDR_OFF);
    if (cert_addr < base_addr) return SIGIL_ERR_NOT_FOUND;

    uint64_t cert_off = xbe_off + (uint64_t)(cert_addr - base_addr);
    uint8_t cert[XBE_CERT_HEADER];
    if (sigil_io_read_exact(io, cert_off, cert, sizeof(cert)) != SIGIL_OK) {
        return SIGIL_ERR_IO;
    }

    uint32_t cert_size = sigil_read_le32(cert);
    if (cert_size < XBE_CERT_HEADER || cert_size > XBE_CERT_MAX_SIZE) {
        return SIGIL_ERR_NOT_FOUND;
    }

    *out_id = sigil_read_le32(cert + XBE_CERT_TITLE_ID_OFF);
    return SIGIL_OK;
}

int sigil_extract_xbox(const sigil_io *io, const char *filename_hint,
                       const sigil_options *opts, sigil_result *out) {
    (void)filename_hint;
    (void)opts;

    sigil_result_init(out);
    out->platform     = SIGIL_PLATFORM_XBOX;
    out->usage        = SIGIL_USAGE_FOLDER_EXACT;
    /* The id parse is confirmed against real redumps, but no emulator has been
     * observed creating the save directory this names, which is what the flag
     * tracks. */
    out->experimental = 1;

    if (!io || !io->read) return SIGIL_ERR_INVALID_ARG;

    /* A bare default.xbe is its own entry point. Decide which it is from the
     * magic alone: dispatching on an error code instead would send a disc
     * image whose first sector is unreadable down the executable path. */
    uint64_t xbe_off = 0;
    uint8_t magic[XBE_MAGIC_LEN];
    bool is_bare_xbe = sigil_io_read_exact(io, 0, magic, sizeof(magic)) == SIGIL_OK
                       && memcmp(magic, XBE_MAGIC, XBE_MAGIC_LEN) == 0;

    if (!is_bare_xbe) {
        uint32_t xbe_size;
        int frc = sigil_xdvdfs_find_root_file(io, XBOX_BOOT_FILE, &xbe_off, &xbe_size);
        if (frc != SIGIL_OK) return frc;
        if (xbe_size < XBE_HEADER_MIN) return SIGIL_ERR_NOT_FOUND;
    }

    uint32_t title_id;
    int rc = xbe_read_title_id(io, xbe_off, &title_id);
    if (rc != SIGIL_OK) return rc;

    xbe_format_title_id(title_id, out->title_id);
    /* The save directory is named after the hex id, never the MS-100 form,
     * so raw_serial and save_id carry the hex and only title_id is formatted. */
    uint8_t be[4] = {
        (uint8_t)(title_id >> 24), (uint8_t)(title_id >> 16),
        (uint8_t)(title_id >> 8),  (uint8_t)title_id
    };
    sigil_hex_encode_4(be, out->raw_serial);
    memcpy(out->save_id, out->raw_serial, strlen(out->raw_serial) + 1);

    out->source = SIGIL_SOURCE_BINARY;
    return SIGIL_OK;
}
