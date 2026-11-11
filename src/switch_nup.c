// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

#include <stdio.h>
#include <string.h>

/* NUP v1 (https://github.com/a55uka/nup SPEC.md, 1.0-draft): a plaintext
 * container for Switch 1 and Switch 2 dumps. Each package record in the TOC
 * carries its title id verbatim, so no key material is needed. */

#define NUP_SUPERBLOCK_SIZE  0x80
#define NUP_TOC_HEADER_SIZE  0x30
#define NUP_PACKAGE_SIZE     0x40
#define NUP_MAX_PACKAGES     1024
#define NUP_DIGEST_CHUNK     65536

enum {
    NUP_KIND_BASE   = 0,
    NUP_KIND_UPDATE = 1,
    NUP_KIND_ADDON  = 2,
    NUP_KIND_LIMIT  = 5
};

bool sigil_nup_is_container(const uint8_t magic[4]) {
    return memcmp(magic, "NUP\x01", 4) == 0;
}

static int content_type_of(uint8_t kind) {
    switch (kind) {
    case NUP_KIND_BASE:   return SIGIL_SWITCH_CONTENT_APPLICATION;
    case NUP_KIND_UPDATE: return SIGIL_SWITCH_CONTENT_PATCH;
    case NUP_KIND_ADDON:  return SIGIL_SWITCH_CONTENT_ADDON;
    default:              return SIGIL_SWITCH_CONTENT_UNKNOWN;
    }
}

static int toc_digest_matches(const sigil_io *io, uint64_t toc_offset, uint64_t toc_size,
                              const uint8_t expected[32]) {
    sigil_sha256 sha;
    sigil_sha256_init(&sha);
    uint8_t chunk[NUP_DIGEST_CHUNK];
    for (uint64_t done = 0; done < toc_size;) {
        size_t take = (size_t)(toc_size - done < sizeof chunk ? toc_size - done : sizeof chunk);
        int rc = sigil_io_read_exact(io, toc_offset + done, chunk, take);
        if (rc != SIGIL_OK) return rc;
        sigil_sha256_update(&sha, chunk, take);
        done += take;
    }
    uint8_t digest[32];
    sigil_sha256_final(&sha, digest);
    return memcmp(digest, expected, sizeof digest) == 0 ? SIGIL_OK : SIGIL_ERR_UNSUPPORTED_FORMAT;
}

int sigil_nup_extract_title(const sigil_io *io, sigil_switch_title *out) {
    uint8_t sb[NUP_SUPERBLOCK_SIZE];
    int rc = sigil_io_read_exact(io, 0, sb, sizeof sb);
    if (rc != SIGIL_OK) return rc;
    if (!sigil_nup_is_container(sb)) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    uint16_t header_size   = sigil_read_le16(sb + 0x04);
    uint64_t toc_offset    = sigil_read_le64(sb + 0x08);
    uint64_t toc_size      = sigil_read_le64(sb + 0x10);
    uint64_t file_size     = sigil_read_le64(sb + 0x40);
    uint32_t package_count = sigil_read_le32(sb + 0x48);
    uint32_t flags         = sigil_read_le32(sb + 0x4C);

    if (header_size < NUP_SUPERBLOCK_SIZE || toc_offset != header_size || flags != 0)
        return SIGIL_ERR_UNSUPPORTED_FORMAT;
    if (package_count == 0 || package_count > NUP_MAX_PACKAGES) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    int64_t actual = io->size ? io->size(io->ctx) : -1;
    if (actual >= 0 && (uint64_t)actual != file_size) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    uint64_t records_end = NUP_TOC_HEADER_SIZE + (uint64_t)package_count * NUP_PACKAGE_SIZE;
    if (toc_size < records_end || toc_offset + toc_size > file_size) return SIGIL_ERR_UNSUPPORTED_FORMAT;

    rc = toc_digest_matches(io, toc_offset, toc_size, sb + 0x18);
    if (rc != SIGIL_OK) return rc;

    uint8_t toc[NUP_TOC_HEADER_SIZE];
    rc = sigil_io_read_exact(io, toc_offset, toc, sizeof toc);
    if (rc != SIGIL_OK) return rc;
    if (memcmp(toc, "TOC\x01", 4) != 0 || sigil_read_le32(toc + 0x04) != package_count)
        return SIGIL_ERR_UNSUPPORTED_FORMAT;

    sigil_switch_title best;
    bool have = false;
    for (uint32_t i = 0; i < package_count; i++) {
        uint8_t rec[NUP_PACKAGE_SIZE];
        rc = sigil_io_read_exact(io, toc_offset + NUP_TOC_HEADER_SIZE + (uint64_t)i * NUP_PACKAGE_SIZE,
                                 rec, sizeof rec);
        if (rc != SIGIL_OK) return rc;

        uint8_t kind = rec[0x18];
        if (kind >= NUP_KIND_LIMIT) return SIGIL_ERR_UNSUPPORTED_FORMAT;
        uint64_t title = sigil_read_le64(rec);
        int type = content_type_of(kind);
        if (title == 0 || type == SIGIL_SWITCH_CONTENT_UNKNOWN) continue;

        sigil_switch_title t;
        memset(&t, 0, sizeof t);
        snprintf(t.title_id, sizeof t.title_id, "%016llX", (unsigned long long)title);
        sigil_switch_application_of_id(t.title_id, t.application_id);
        t.content_type = type;
        t.version = sigil_read_le32(rec + 0x14);
        if (sigil_switch_title_keep(&best, &have, &t)) break;
    }
    if (!have) return SIGIL_ERR_NOT_FOUND;
    *out = best;
    return SIGIL_OK;
}
