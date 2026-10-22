// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include "test_fs.h"

#include <stdint.h>

#ifndef TEST_PATH_CAP
#define TEST_PATH_CAP 1024
#endif

#define SECTOR       2048
#define TRACK_FRAMES 20

static char g_dir[TEST_PATH_CAP];

static void path_in(char *out, const char *name) {
    snprintf(out, TEST_PATH_CAP, "%s/%s", g_dir, name);
}

static int write_file(const char *name, const void *data, size_t len) {
    char path[TEST_PATH_CAP];
    path_in(path, name);
    FILE *fp = test_fopen(path, "wb");
    if (!fp) return -1;
    size_t put = fwrite(data, 1, len, fp);
    fclose(fp);
    return put == len ? 0 : -1;
}

static int write_text(const char *name, const char *text) {
    return write_file(name, text, strlen(text));
}

static int write_dc_track(const char *name, const char *product) {
    static uint8_t buf[TRACK_FRAMES * SECTOR];
    memset(buf, 0, sizeof(buf));
    if (product) {
        memcpy(buf, "SEGA SEGAKATANA ", 16);
        memcpy(buf + 0x10, "SEGA ENTERPRISES", 16);
        memset(buf + 0x40, ' ', 10);
        memcpy(buf + 0x40, product, strlen(product));
    }
    return write_file(name, buf, sizeof(buf));
}

static int expect_id(const char *sheet, sigil_platform hint, const char *want, const char *label) {
    char path[TEST_PATH_CAP];
    path_in(path, sheet);
    sigil_result r;
    int rc = sigil_extract_from_path(path, hint, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL %s: %s\n", label, sigil_strerror(rc));
        return 1;
    }
    if (strcmp(r.title_id, want) != 0 || r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "FAIL %s: title_id='%s' source=%d (want %s, binary)\n",
                label, r.title_id, (int)r.source, want);
        return 1;
    }
    return 0;
}

static int expect_rc(const char *sheet, int want, const char *label) {
    char path[TEST_PATH_CAP];
    path_in(path, sheet);
    sigil_result r;
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_DREAMCAST, NULL, &r);
    if (rc != want) {
        fprintf(stderr, "FAIL %s: rc=%s (want %s)\n", label, sigil_strerror(rc), sigil_strerror(want));
        return 1;
    }
    return 0;
}

static int run(void) {
    if (write_dc_track("Game (Track 1).bin", "T-0001N") != 0
        || write_dc_track("Game (Track 2).bin", NULL) != 0
        || write_dc_track("Game (Track 3).bin", "T-8111N") != 0
        || write_dc_track("track01.bin", "T-0001N") != 0
        || write_dc_track("track03.bin", "T-8111N") != 0) {
        fprintf(stderr, "FAIL fixture write\n");
        return 1;
    }

    if (write_text("quoted.gdi",
                   "3\n"
                   "1 0 4 2352 \"Game (Track 1).bin\" 0\n"
                   "2 450 0 2352 \"Game (Track 2).bin\" 0\n"
                   "3 45000 4 2352 \"Game (Track 3).bin\" 0\n") != 0
        || write_text("bare.gdi",
                      "\xEF\xBB\xBF" "3\r\n"
                      "1 0 4 2352 track01.bin 0\r\n"
                      "2 450 0 2352 \"Game (Track 2).bin\" 0\r\n"
                      "3 45000 4 2352 track03.bin 0\r\n") != 0
        || write_text("redump.cue",
                      "REM SINGLE-DENSITY AREA\n"
                      "FILE \"Game (Track 1).bin\" BINARY\n"
                      "  TRACK 01 MODE1/2352\n"
                      "    INDEX 01 00:00:00\n"
                      "FILE \"Game (Track 2).bin\" BINARY\n"
                      "  TRACK 02 AUDIO\n"
                      "    INDEX 01 00:00:00\n"
                      "REM HIGH-DENSITY AREA\n"
                      "FILE \"Game (Track 3).bin\" BINARY\n"
                      "  TRACK 03 MODE1/2352\n"
                      "    INDEX 01 00:00:00\n") != 0
        || write_text("plain.cue",
                      "FILE \"Game (Track 2).bin\" BINARY\n"
                      "  TRACK 01 AUDIO\n"
                      "FILE \"Game (Track 3).bin\" BINARY\n"
                      "  TRACK 02 MODE1/2352\n") != 0
        || write_text("missing.gdi", "1\n1 45000 4 2352 gone.bin 0\n") != 0
        || write_text("audio.cue", "FILE \"Game (Track 2).bin\" BINARY\n  TRACK 01 AUDIO\n") != 0) {
        fprintf(stderr, "FAIL sheet write\n");
        return 1;
    }

    int failed = 0;
    failed += expect_id("quoted.gdi", SIGIL_PLATFORM_DREAMCAST, "T-8111N", "gdi high-density track");
    failed += expect_id("quoted.gdi", SIGIL_PLATFORM_AUTO, "T-8111N", "gdi sniffed");
    failed += expect_id("bare.gdi", SIGIL_PLATFORM_DREAMCAST, "T-8111N", "gdi bare names, crlf, bom");
    failed += expect_id("redump.cue", SIGIL_PLATFORM_DREAMCAST, "T-8111N", "cue high-density marker");
    failed += expect_id("plain.cue", SIGIL_PLATFORM_DREAMCAST, "T-8111N", "cue skips audio");
    failed += expect_rc("missing.gdi", SIGIL_ERR_IO, "gdi names a missing track");
    failed += expect_rc("audio.cue", SIGIL_ERR_NOT_FOUND, "cue with no data track");
    return failed ? 1 : 0;
}

int main(void) {
    if (test_temp_dir(g_dir, sizeof(g_dir)) != 0) {
        fprintf(stderr, "FAIL temp dir\n");
        return 1;
    }
    int rc = run();
    test_remove_tree(g_dir);
    if (rc == 0) printf("ok unit_disc_sheet\n");
    return rc;
}
