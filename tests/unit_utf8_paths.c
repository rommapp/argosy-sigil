// SPDX-License-Identifier: MPL-2.0
/* sigil takes UTF-8 paths on every system. Windows' ANSI file calls read them
 * in the local code page, so each opener has to reach the file by its UTF-8
 * name, here one with three- and two-byte characters. */
#include "sigil.h"
#include "test_fs.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* "テスト" [test] and "é". */
#define NON_ASCII_NAME "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88 \xc3\xa9"

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static bool put(const char *path, const void *data, size_t len) {
    FILE *f = test_fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

static void check_opener(const char *label, sigil_io *(*open)(const char *), const char *path, int64_t size) {
    sigil_io *io = open(path);
    if (!io) {
        fail(label, "did not open");
        return;
    }
    if (io->size(io->ctx) != size) fail(label, "wrong size");
    sigil_io_close(io);
}

int main(void) {
    char dir[1024];
    if (test_temp_dir(dir, sizeof(dir)) != 0) {
        fprintf(stderr, "no temporary folder\n");
        return 1;
    }

    char image[1024];
    snprintf(image, sizeof(image), "%s/%s.iso", dir, NON_ASCII_NAME);
    static uint8_t sectors[2048 * 4];
    if (!put(image, sectors, sizeof(sectors))) fail("image", "could not write");
    check_opener("sigil_io_open_file", sigil_io_open_file, image, (int64_t)sizeof(sectors));
    check_opener("sigil_io_open_raw_cd", sigil_io_open_raw_cd, image, (int64_t)sizeof(sectors));

    char keys[1024];
    snprintf(keys, sizeof(keys), "%s/%s.keys", dir, NON_ASCII_NAME);
    const char *text = "header_key = 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n";
    if (!put(keys, text, strlen(text))) fail("prod.keys", "could not write");
    uint8_t key[32];
    if (sigil_load_header_key_from_prod_keys(keys, key) != SIGIL_OK || key[0] != 0x00 || key[31] != 0x1f) {
        fail("sigil_load_header_key_from_prod_keys", "did not read the key");
    }

    test_remove_tree(dir);
    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("ok unit_utf8_paths\n");
    return 0;
}
