// SPDX-License-Identifier: MPL-2.0
/* Java strings cross JNI as UTF-16. GetStringUTFChars and NewStringUTF use
 * modified UTF-8 instead, which spells a character outside the BMP as two
 * three-byte surrogates and aborts under CheckJNI on the four-byte form
 * sigil reads from file names and profile files. The binding converts
 * between UTF-16 and standard UTF-8 itself. */
#ifndef SIGIL_JNI_UTF16_H
#define SIGIL_JNI_UTF16_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIGIL_REPLACEMENT_CHAR 0xFFFDu

/* Code units `in` (standard UTF-8, NUL-terminated) needs: never more than its
 * byte count. */
static size_t sigil_utf16_capacity(const char *in) {
    size_t n = 0;
    while (in[n]) n++;
    return n;
}

/* Decodes `in` into `out`, which holds at least sigil_utf16_capacity(in)
 * units; a byte that doesn't start a whole, shortest-form sequence becomes
 * U+FFFD. Returns the units written. */
static size_t sigil_utf8_to_utf16(const char *in, uint16_t *out) {
    const unsigned char *s = (const unsigned char *)in;
    size_t n = 0;
    while (*s) {
        unsigned c = *s;
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        size_t k = 1;
        while (len > 1 && k < len && (s[k] & 0xC0) == 0x80) cp = (cp << 6) | (s[k++] & 0x3F);
        static const uint32_t SHORTEST[] = { 0, 0, 0x80, 0x800, 0x10000 };
        bool whole = len != 0 && k == len && cp >= SHORTEST[len] && cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
        if (!whole) {
            out[n++] = SIGIL_REPLACEMENT_CHAR;
            s++;
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 | (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        } else {
            out[n++] = (uint16_t)cp;
        }
        s += len;
    }
    return n;
}

/* Bytes the UTF-8 form of `len` units needs, with its NUL. */
static size_t sigil_utf8_capacity(size_t len) {
    return 3 * len + 1;
}

/* Encodes `len` units into `out`, which holds sigil_utf8_capacity(len)
 * bytes; a lone surrogate becomes U+FFFD, and a U+0000 ends the string, as
 * no C string can carry one. */
static void sigil_utf16_to_utf8(const uint16_t *in, size_t len, char *out) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        uint32_t cp = in[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[++i] - 0xDC00);
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = SIGIL_REPLACEMENT_CHAR;
        }
        if (cp < 0x80) {
            out[n++] = (char)cp;
        } else if (cp < 0x800) {
            out[n++] = (char)(0xC0 | (cp >> 6));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[n++] = (char)(0xE0 | (cp >> 12));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[n++] = (char)(0xF0 | (cp >> 18));
            out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[n] = '\0';
}

#endif
