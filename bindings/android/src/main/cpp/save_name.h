// SPDX-License-Identifier: MPL-2.0
/* Save names on cards and volumes are raw bytes, and NewStringUTF aborts
 * under CheckJNI on bytes that aren't modified UTF-8. The JNI binding hands
 * Kotlin each name with every byte outside printable ASCII, and '%' itself,
 * written as %XX, and decodes names Kotlin passes back the same way. */
#ifndef SIGIL_JNI_SAVE_NAME_H
#define SIGIL_JNI_SAVE_NAME_H

#include "sigil.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void sigil_save_name_escape(const char *raw, char *out, size_t cap) {
    size_t n = 0;
    for (; *raw && n + 4 <= cap; raw++) {
        unsigned char c = (unsigned char)*raw;
        if (c >= 0x20 && c < 0x7F && c != '%') out[n++] = (char)c;
        else n += (size_t)snprintf(out + n, cap - n, "%%%02X", c);
    }
    out[n < cap ? n : cap - 1] = '\0';
}

/* Escapes each '\n'-separated line of `raw` as a name and keeps the line
 * breaks, so a list of names stays one per line. */
static void sigil_save_lines_escape(const char *raw, char *out, size_t cap) {
    size_t n = 0;
    out[0] = '\0';
    while (n + 1 < cap) {
        const char *end = strchr(raw, '\n');
        size_t len = end ? (size_t)(end - raw) : strlen(raw);
        char line[SIGIL_SAVE_PATH_MAX];
        snprintf(line, sizeof(line), "%.*s", (int)len, raw);
        sigil_save_name_escape(line, out + n, cap - n);
        n += strlen(out + n);
        if (!end || n + 2 > cap) break;
        out[n++] = '\n';
        out[n] = '\0';
        raw = end + 1;
    }
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* False when `in` holds a malformed escape or doesn't fit `out`. */
static bool sigil_save_name_unescape(const char *in, char *out, size_t cap) {
    size_t n = 0;
    while (*in) {
        if (n + 1 >= cap) return false;
        if (*in != '%') { out[n++] = *in++; continue; }
        int hi = hex_digit(in[1]), lo = hi < 0 ? -1 : hex_digit(in[2]);
        if (hi < 0 || lo < 0) return false;
        out[n++] = (char)(hi * 16 + lo);
        in += 3;
    }
    out[n] = '\0';
    return true;
}

#endif
