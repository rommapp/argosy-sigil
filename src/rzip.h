// SPDX-License-Identifier: MPL-2.0
/* RetroArch's RZIP, the form its save_file_compression writes saves in
 * (libretro-common streams/rzip_stream.c): "#RZIPv", a version byte (1:
 * deflate, 2: Zstandard), "#", the uncompressed chunk size as a
 * little-endian u32 and the total size as a u64, then chunks, each a u32
 * length and one zlib stream or Zstandard frame. RetroArch reads an
 * uncompressed file either way. */
#ifndef SIGIL_RZIP_H
#define SIGIL_RZIP_H

#include "sigil_internal.h"

#define SIGIL_RZIP_HEADER_SIZE 20u

/** `data` starts with an RZIP header sigil can read. */
bool sigil_rzip_is(const uint8_t *data, size_t len);

/**
 * The uncompressed bytes of the RZIP file `data`, at most `cap` of them;
 * `*out` is malloc'd. SIGIL_ERR_UNSUPPORTED_FORMAT when a chunk won't
 * decompress or the sizes don't add up.
 */
int sigil_rzip_decode(const uint8_t *data, size_t len, size_t cap, uint8_t **out, size_t *out_len);

/**
 * Opens a save file through `open`: an RZIP file comes back as a stream
 * over its uncompressed bytes, any other file as `open` gives it. NULL when
 * `open` gives none.
 */
sigil_io *sigil_save_open(sigil_save_open_fn open, void *ctx, const char *path);

#endif
