// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_UTIL_H
#define SIGIL_UTIL_H

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline bool sigil_is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static inline bool sigil_is_dig(char c)   { return c >= '0' && c <= '9'; }
static inline bool sigil_is_hex(char c) {
    return sigil_is_dig(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static inline char sigil_to_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static inline char sigil_to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Copy `src` into `out` lowercased, truncated to out_size - 1 characters.
 * For path-shaped ids only: emulators name save directories with lowercase
 * hex, while title_id and raw_serial keep their canonical uppercase form. */
static inline void sigil_lower_copy(const char *src, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < out_size) {
            out[i] = sigil_to_lower(src[i]);
            i++;
        }
    }
    out[i] = '\0';
}

/* Split a 16-char hex id into the '/'-separated lowercase path an emulator
 * creates for it (0004000E0011C500 -> 0004000e/0011c500). `out` needs 18
 * bytes. Shared so the binary extractor and the filename scanner cannot drift
 * into emitting different paths for the same title. */
static inline void sigil_hex16_split_lower(const char *id16, char *out) {
    for (int i = 0; i < 8; i++) out[i] = sigil_to_lower(id16[i]);
    out[8] = '/';
    for (int i = 0; i < 8; i++) out[9 + i] = sigil_to_lower(id16[8 + i]);
    out[17] = '\0';
}

/* Hex-encode 8 bytes -> 16-char uppercase string + NUL.
 * If reverse is true, encodes src8[7..0]; otherwise src8[0..7]. */
static inline void sigil_hex_encode_8(const uint8_t *src8, char out[17], bool reverse) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) {
        uint8_t b = reverse ? src8[7 - i] : src8[i];
        out[i * 2]     = hex[(b >> 4) & 0xF];
        out[i * 2 + 1] = hex[b & 0xF];
    }
    out[16] = '\0';
}

/* Hex-encode 4 bytes -> 8-char uppercase string + NUL. */
static inline void sigil_hex_encode_4(const uint8_t *src4, char out[9]) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 4; i++) {
        out[i * 2]     = hex[(src4[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[src4[i] & 0xF];
    }
    out[8] = '\0';
}

/* Lowercase the extension of `name` into `out` (max 15 chars + NUL).
 * Empty `out` if no extension. */
static inline void sigil_lower_ext(const char *name, char out[16]) {
    out[0] = '\0';
    if (!name) return;
    const char *dot = strrchr(name, '.');
    if (!dot) return;
    dot++;
    size_t i = 0;
    while (dot[i] && i < 15) {
        out[i] = (char)tolower((unsigned char)dot[i]);
        i++;
    }
    out[i] = '\0';
}

#endif
