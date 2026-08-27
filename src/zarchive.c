// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

/* ZArchive (exzap/ZArchive) is one container serving two consoles. Cemu writes
 * it as `.wua` for Wii U and Xenia writes it as `.zar` for Xbox 360, both
 * vendoring the same library, so the metadata parsing lives here once and the
 * per-platform readers differ only in what they look for inside.
 *
 * Everything is big-endian. The footer sits at the very end of the file:
 *
 *   0x00 compressedData offset/size    0x10 offsetRecords offset/size
 *   0x20 names offset/size             0x30 fileTree offset/size
 *   0x40 metaDirectory offset/size     0x50 metaData offset/size
 *   0x60 integrityHash[32]             0x80 totalSize
 *   0x88 version                       0x8C magic
 */

int sigil_zar_read_footer(const sigil_io *io, sigil_zar_footer *out) {
    if (!io || !io->size) return SIGIL_ERR_INVALID_ARG;

    int64_t total = io->size(io->ctx);
    if (total < SIGIL_ZAR_FOOTER_SIZE) return SIGIL_ERR_NOT_FOUND;

    uint8_t buf[SIGIL_ZAR_FOOTER_SIZE];
    int rc = sigil_io_read_exact(io, (uint64_t)(total - SIGIL_ZAR_FOOTER_SIZE),
                                 buf, SIGIL_ZAR_FOOTER_SIZE);
    if (rc != SIGIL_OK) return rc;

    uint32_t version = sigil_read_be32(buf + 0x88);
    uint32_t magic   = sigil_read_be32(buf + 0x8C);
    if (magic != SIGIL_ZAR_MAGIC || version != SIGIL_ZAR_VERSION_1) {
        return SIGIL_ERR_NOT_FOUND;
    }

    out->compressed_off    = sigil_read_be64(buf + 0x00);
    out->compressed_size   = sigil_read_be64(buf + 0x08);
    out->offset_records_off  = sigil_read_be64(buf + 0x10);
    out->offset_records_size = sigil_read_be64(buf + 0x18);
    out->names_off         = sigil_read_be64(buf + 0x20);
    out->names_size        = sigil_read_be64(buf + 0x28);
    out->file_tree_off     = sigil_read_be64(buf + 0x30);
    out->file_tree_size    = sigil_read_be64(buf + 0x38);
    return SIGIL_OK;
}

/* Name table entries are a length byte followed by the characters. A set high
 * bit means the length is two bytes wide, which no name sigil looks for ever
 * uses, so those are skipped rather than decoded. */
size_t sigil_zar_read_name(const uint8_t *names, size_t names_len,
                           uint32_t offset, char *out, size_t out_size) {
    if (offset == 0x7FFFFFFFu || offset >= names_len) return 0;
    uint8_t first = names[offset];
    if (first & 0x80) return 0;

    size_t len = first & 0x7F;
    if (offset + 1 + len > names_len) return 0;
    if (len + 1 > out_size) return 0;
    memcpy(out, names + offset + 1, len);
    out[len] = '\0';
    return len;
}
