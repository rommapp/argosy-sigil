// SPDX-License-Identifier: MPL-2.0
/* The conversion the JNI binding uses between Java's UTF-16 strings and the
 * standard UTF-8 sigil reads and writes, in place of modified UTF-8. */
#include "utf16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails = 0;

static void expect(const char *label, bool ok) {
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", label);
        g_fails++;
    }
}

/* `utf8` decodes to `units` and, when `back` is set, encodes back to itself. */
static void round_trip(const char *label, const char *utf8, const uint16_t *units, size_t count, bool back) {
    uint16_t *out = (uint16_t *)malloc((sigil_utf16_capacity(utf8) + 1) * sizeof(uint16_t));
    size_t n = sigil_utf8_to_utf16(utf8, out);
    expect(label, n == count && memcmp(out, units, count * sizeof(uint16_t)) == 0);
    if (back) {
        char *again = (char *)malloc(sigil_utf8_capacity(n));
        sigil_utf16_to_utf8(out, n, again);
        expect(label, strcmp(again, utf8) == 0);
        free(again);
    }
    free(out);
}

int main(void) {
    const uint16_t ascii[] = { 'G', '.', 's', 'r', 'm' };
    round_trip("ascii", "G.srm", ascii, 5, true);

    const uint16_t two_three[] = { 0x00E9, 0x65E5 };
    round_trip("two- and three-byte characters", "\xC3\xA9\xE6\x97\xA5", two_three, 2, true);

    const uint16_t emoji[] = { 'a', 0xD83C, 0xDFAE, 'b' };
    round_trip("a character outside the BMP is one surrogate pair", "a\xF0\x9F\x8E\xAE" "b", emoji, 4, true);

    const uint16_t raw[] = { 'S', 0xFFFD, 'J' };
    round_trip("a byte that isn't UTF-8 becomes U+FFFD", "S\x82J", raw, 3, false);

    const uint16_t cut[] = { 0xFFFD, 0xFFFD, 'x' };
    round_trip("a sequence cut short", "\xE6\x97x", cut, 3, false);

    const uint16_t overlong[] = { 0xFFFD, 0xFFFD };
    round_trip("an overlong form", "\xC0\xAF", overlong, 2, false);

    const uint16_t cesu[] = { 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD };
    round_trip("modified UTF-8's surrogate bytes are not taken as a character", "\xED\xA0\xBC\xED\xBE\xAE", cesu, 6, false);

    const uint16_t lone[] = { 'a', 0xD800, 'b' };
    char out[16];
    sigil_utf16_to_utf8(lone, 3, out);
    expect("a lone surrogate becomes U+FFFD", strcmp(out, "a\xEF\xBF\xBD" "b") == 0);

    const uint16_t with_nul[] = { 'a', 0, 'b' };
    sigil_utf16_to_utf8(with_nul, 3, out);
    expect("U+0000 ends the string", strcmp(out, "a") == 0);

    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("unit_jni_utf16: ok\n");
    return 0;
}
