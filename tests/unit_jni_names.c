// SPDX-License-Identifier: MPL-2.0
/* The escaping the JNI binding gives save names, so a name with any bytes
 * reaches Kotlin as valid modified UTF-8 and comes back as the same bytes. */
#include "save_name.h"
#include <stdio.h>
#include <string.h>

static int g_fails = 0;

static void expect(const char *label, bool ok) {
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", label);
        g_fails++;
    }
}

int main(void) {
    char escaped[3 * 64 + 1], back[64];

    sigil_save_name_escape("HYPERDUEL_0", escaped, sizeof(escaped));
    expect("printable ASCII stays as it is", strcmp(escaped, "HYPERDUEL_0") == 0);

    sigil_save_name_escape("HYPERDUEL\xB1" "0", escaped, sizeof(escaped));
    expect("a raw byte becomes %XX", strcmp(escaped, "HYPERDUEL%B10") == 0);

    sigil_save_name_escape("50%", escaped, sizeof(escaped));
    expect("a percent sign is escaped", strcmp(escaped, "50%25") == 0);

    char every[64];
    for (int start = 1; start < 256; start += 63) {
        size_t n = 0;
        for (int b = start; b < start + 63 && b < 256; b++) every[n++] = (char)b;
        every[n] = '\0';
        sigil_save_name_escape(every, escaped, sizeof(escaped));
        bool printable = true;
        for (size_t i = 0; escaped[i]; i++) printable = printable && escaped[i] >= 0x20 && escaped[i] < 0x7F;
        expect("the escaped form is printable ASCII", printable);
        expect("every byte round-trips", sigil_save_name_unescape(escaped, back, sizeof(back)) && strcmp(back, every) == 0);
    }

    expect("a malformed escape is refused", !sigil_save_name_unescape("BAD%G1", back, sizeof(back)));
    expect("a truncated escape is refused", !sigil_save_name_unescape("BAD%4", back, sizeof(back)));

    if (g_fails) return 1;
    printf("unit_jni_names: ok\n");
    return 0;
}
