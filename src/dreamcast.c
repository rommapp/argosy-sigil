// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

/* Dreamcast discs boot from an IP.BIN header at the start of the data track:
 * the hardware id "SEGA SEGAKATANA " at 0x00, then a 10-byte ASCII product
 * number at 0x40 padded with trailing spaces. Flycast names per-game VMU
 * files after that product number, so it is the save identity. */

#define DC_MAGIC         "SEGA SEGAKATANA"
#define DC_MAGIC_LEN     15
#define DC_PRODUCT_OFF   0x40
#define DC_PRODUCT_LEN   10
#define DC_SECTOR        SIGIL_ISO_SECTOR_SIZE
#define DC_CHUNK_SECTORS 8

/* A GD-ROM carries its data track third and a CHD packs tracks contiguously
 * from frame 0, so IP.BIN lands after tracks 1-2 rather than at the physical
 * GD-area LBA 45000. Tracks 1-2 live in the single-density area, which spans
 * the first four minutes of the disc (4 * 60 * 75 = 18000 frames), so no
 * conformant dump can push IP.BIN beyond that; the remainder is slack for the
 * padding frames a CHD inserts between tracks. The bound also caps a miss at
 * ~40 MB of sequential reads, and a miss only happens on a file the caller
 * already declared to be a Dreamcast disc. */
#define DC_SCAN_MAX_SECTORS 20000

static bool dc_is_space(uint8_t c) {
    return c == ' ' || (c >= 0x09 && c <= 0x0D);
}

static bool dc_is_printable(uint8_t c) {
    return c >= 0x20 && c <= 0x7E;
}

static size_t dc_read_upto(const sigil_io *io, uint64_t off, uint8_t *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        int got = io->read(io->ctx, off + total, buf + total, len - total);
        if (got <= 0) break;
        total += (size_t)got;
    }
    return total;
}

/* Flycast trims trailing whitespace and only then truncates at the first NUL,
 * because some discs leave garbage after the terminator (core/emulator.cpp).
 * That order decides the name the emulator writes, so it is reproduced rather
 * than normalised. */
static int dc_product_number(const uint8_t *sector, char out[32]) {
    size_t len = DC_PRODUCT_LEN;
    while (len > 0 && dc_is_space(sector[DC_PRODUCT_OFF + len - 1])) len--;
    for (size_t i = 0; i < len; i++) {
        if (sector[DC_PRODUCT_OFF + i] == '\0') {
            len = i;
            break;
        }
    }
    if (len == 0) return SIGIL_ERR_NOT_FOUND;

    bool has_alnum = false;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = sector[DC_PRODUCT_OFF + i];
        if (!dc_is_printable(c)) return SIGIL_ERR_NOT_FOUND;
        if (sigil_is_upper((char)c) || sigil_is_dig((char)c)) has_alnum = true;
    }
    if (!has_alnum) return SIGIL_ERR_NOT_FOUND;

    memcpy(out, sector + DC_PRODUCT_OFF, len);
    out[len] = '\0';
    return SIGIL_OK;
}

/* Copies the sector holding IP.BIN into `out_sector`. The first candidate is
 * offset 0, which covers a bare data track (.gdi track03, an unwrapped ISO);
 * beyond that the magic is searched on sector boundaries, because in a CHD the
 * data track starts wherever tracks 1-2 happened to end. Short reads end the
 * scan: a stream that stops mid-sector has nothing further to match. */
static int dc_find_ip_bin(const sigil_io *io, uint8_t out_sector[DC_SECTOR]) {
    uint8_t chunk[DC_CHUNK_SECTORS * DC_SECTOR];
    for (uint32_t base = 0; base < DC_SCAN_MAX_SECTORS; base += DC_CHUNK_SECTORS) {
        size_t got = dc_read_upto(io, (uint64_t)base * DC_SECTOR, chunk, sizeof(chunk));
        size_t whole = got / DC_SECTOR;
        for (size_t i = 0; i < whole; i++) {
            const uint8_t *s = chunk + i * DC_SECTOR;
            if (memcmp(s, DC_MAGIC, DC_MAGIC_LEN) == 0) {
                memcpy(out_sector, s, DC_SECTOR);
                return SIGIL_OK;
            }
        }
        if (whole < DC_CHUNK_SECTORS) break;
    }
    return SIGIL_ERR_NOT_FOUND;
}

int sigil_extract_dreamcast(const sigil_io *io, const char *filename_hint,
                            const sigil_options *opts, sigil_result *out) {
    (void)filename_hint;
    (void)opts;

    sigil_result_init(out);
    out->platform     = SIGIL_PLATFORM_DREAMCAST;
    out->usage        = SIGIL_USAGE_FILE_PREFIX;
    out->experimental = 1;

    if (!io || !io->read) return SIGIL_ERR_INVALID_ARG;

    uint8_t sector[DC_SECTOR];
    int rc = dc_find_ip_bin(io, sector);
    if (rc != SIGIL_OK) return rc;

    rc = dc_product_number(sector, out->title_id);
    if (rc != SIGIL_OK) return rc;

    memcpy(out->raw_serial, out->title_id, strlen(out->title_id) + 1);
    out->source = SIGIL_SOURCE_BINARY;
    return SIGIL_OK;
}
