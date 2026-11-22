// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include "test_fs.h"
#include <stdio.h>
#include <string.h>

typedef struct { const uint8_t *buf; size_t len; } mem_ctx;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_ctx *m = (mem_ctx *)ctx;
    if (off >= m->len) return 0;
    size_t avail = m->len - (size_t)off;
    size_t n = len < avail ? len : avail;
    memcpy(buf, m->buf + off, n);
    return (int)n;
}
static int64_t mem_size(void *ctx) { return (int64_t)((mem_ctx *)ctx)->len; }

/* A file of zeros that no reader takes, so the filename decides. */
static int extract_zeros_opts(const char *filename, sigil_platform hint,
                              const sigil_options *opts, sigil_result *r) {
    static uint8_t buf[2048];
    memset(buf, 0, sizeof(buf));
    mem_ctx ctx = { buf, sizeof(buf) };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    memset(r, 0, sizeof(*r));
    return sigil_extract_from_io(&io, filename, hint, opts, r);
}

static int extract_zeros(const char *filename, sigil_platform hint, sigil_result *r) {
    return extract_zeros_opts(filename, hint, NULL, r);
}

static int expect(const char *filename, sigil_platform hint,
                  const char *want_title, const char *want_save,
                  sigil_usage want_usage, const char *label) {
    sigil_result r;
    int rc = extract_zeros(filename, hint, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL %s: rc=%d\n", label, rc);
        return 1;
    }
    if (r.source != SIGIL_SOURCE_FILENAME) {
        fprintf(stderr, "FAIL %s: source=%d\n", label, (int)r.source);
        return 1;
    }
    if (strcmp(r.title_id, want_title) != 0 || strcmp(r.save_id, want_save) != 0) {
        fprintf(stderr, "FAIL %s: title_id='%s' save_id='%s'\n",
                label, r.title_id, r.save_id);
        return 1;
    }
    if (r.usage != want_usage) {
        fprintf(stderr, "FAIL %s: usage=%d (want %d)\n", label, (int)r.usage, (int)want_usage);
        return 1;
    }
    if (hint != SIGIL_PLATFORM_AUTO && r.platform != hint) {
        fprintf(stderr, "FAIL %s: platform=%d (want %d)\n", label, (int)r.platform, (int)hint);
        return 1;
    }
    printf("ok %s -> %s\n", label, r.title_id);
    return 0;
}

/* A named platform reads only its own pattern: a name carrying another
 * console's serial gives no id, not that console's. */
static int expect_no_id(const char *filename, sigil_platform hint, sigil_platform want_platform,
                        const char *label) {
    sigil_result r;
    int rc = extract_zeros(filename, hint, &r);
    if (rc == SIGIL_OK || r.title_id[0] != '\0' || r.save_id[0] != '\0'
        || r.platform != want_platform) {
        fprintf(stderr, "FAIL %s: rc=%d platform=%d title_id='%s' source=%d, want no id and platform %d\n",
                label, rc, (int)r.platform, r.title_id, (int)r.source, (int)want_platform);
        return 1;
    }
    printf("ok %s: no id (%s)\n", label, sigil_strerror(rc));
    return 0;
}

/* Nothing names a platform: the path's extension is a bare .bin, so every
 * pattern is tried and the one that matches names the console. */
static int expect_guess(const char *dir, const char *name, sigil_platform want_platform,
                        const char *want_title, const char *want_save,
                        sigil_usage want_usage, const char *label) {
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = test_fopen(path, "wb");
    static const uint8_t zeros[2048];
    if (!f || fwrite(zeros, 1, sizeof(zeros), f) != sizeof(zeros)) {
        if (f) fclose(f);
        fprintf(stderr, "FAIL %s: can't write %s\n", label, path);
        return 1;
    }
    fclose(f);
    sigil_result r;
    memset(&r, 0, sizeof(r));
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_AUTO, NULL, &r);
    if (rc != SIGIL_OK || r.platform != want_platform || strcmp(r.title_id, want_title) != 0
        || strcmp(r.save_id, want_save) != 0 || r.usage != want_usage
        || r.source != SIGIL_SOURCE_FILENAME) {
        fprintf(stderr, "FAIL %s: rc=%d platform=%d title_id='%s' save_id='%s' usage=%d source=%d\n",
                label, rc, (int)r.platform, r.title_id, r.save_id, (int)r.usage, (int)r.source);
        return 1;
    }
    printf("ok %s -> %s\n", label, r.title_id);
    return 0;
}

/* A serial read off the filename addresses the same on-disk location as one
 * read off the disc, so the fallback owes the same save_id and usage the binary
 * extractor emits. */
int main(void) {
    int failures = 0;
    failures += expect("Ace Combat 04 [SLUS-20152].iso", SIGIL_PLATFORM_PS2,
                       "SLUS-20152", "BASLUS-20152", SIGIL_USAGE_FOLDER_PREFIX, "ps2 ntsc-u");
    failures += expect("Ico [SLES-50760].iso", SIGIL_PLATFORM_PS2,
                       "SLES-50760", "BESLES-50760", SIGIL_USAGE_FOLDER_PREFIX, "ps2 pal");
    /* A Switch update or DLC named by its own id saves under its game's:
     * an update's id is the game's with 0x800 set, a DLC's the game's plus
     * 0x1000 and an index. */
    failures += expect("Mario Kart 8 Deluxe [0100152000023001][v65536][DLC].nsp", SIGIL_PLATFORM_SWITCH,
                       "0100152000022000", "0100152000022000", SIGIL_USAGE_FOLDER_EXACT, "switch dlc");
    failures += expect("Mario Kart 8 Deluxe [0100152000022800][v1179648][UPD].nsp", SIGIL_PLATFORM_SWITCH,
                       "0100152000022000", "0100152000022000", SIGIL_USAGE_FOLDER_EXACT, "switch update");
    failures += expect("Mario Kart 8 Deluxe [0100152000022000][v0].nsp", SIGIL_PLATFORM_AUTO,
                       "0100152000022000", "0100152000022000", SIGIL_USAGE_FOLDER_EXACT, "switch base");
    failures += expect("Mario Kart 8 Deluxe [0100152000023001].nsp", SIGIL_PLATFORM_AUTO,
                       "0100152000022000", "0100152000022000", SIGIL_USAGE_FOLDER_EXACT, "switch dlc without a hint");

    /* Every other platform with a naming pattern reads its own. */
    failures += expect("Patapon [ULUS10335].iso", SIGIL_PLATFORM_PSP,
                       "ULUS10335", "ULUS10335", SIGIL_USAGE_FOLDER_PREFIX, "psp");
    failures += expect("Gravity Rush [PCSE00123].zip", SIGIL_PLATFORM_PSVITA,
                       "PCSE00123", "PCSE00123", SIGIL_USAGE_FOLDER_EXACT, "psvita");
    failures += expect("Spyro the Dragon [SCUS-94228].bin", SIGIL_PLATFORM_PSX,
                       "SCUS-94228", "SCUS-94228", SIGIL_USAGE_FILE_PREFIX, "psx");
    failures += expect("Game [0004000000030800].3ds", SIGIL_PLATFORM_3DS,
                       "0004000000030800", "00040000/00030800", SIGIL_USAGE_FOLDER_SPLIT, "3ds");
    failures += expect("Mario Kart 8 [000500001010EC00].wua", SIGIL_PLATFORM_WIIU,
                       "1010EC00", "00050000/1010ec00", SIGIL_USAGE_FOLDER_SPLIT, "wiiu");
    failures += expect("Mario Kart Wii [RMCE].iso", SIGIL_PLATFORM_WII,
                       "524D4345", "524d4345", SIGIL_USAGE_FOLDER_EXACT, "wii");
    failures += expect("Wind Waker [GZLE].iso", SIGIL_PLATFORM_GAMECUBE,
                       "475A4C45", "GZLE", SIGIL_USAGE_FILE_PREFIX, "gamecube");
    /* A PS3 title id has the PSP id's shape, and its saves are the folders
     * starting with it, as ps3.c reports for the same game. */
    failures += expect("Game [BLUS31426].pkg", SIGIL_PLATFORM_PS3,
                       "BLUS31426", "BLUS31426", SIGIL_USAGE_FOLDER_PREFIX, "ps3");
    failures += expect_no_id("Game (SLUS-20675).iso", SIGIL_PLATFORM_PS3, SIGIL_PLATFORM_PS3, "ps3 named, a ps2 serial");
    {
        /* With the fallback off, the PS3 reader's own failure stands. */
        sigil_options off = { .struct_version = SIGIL_OPTIONS_V1 };
        sigil_result r;
        int rc = extract_zeros_opts("Game [BLUS31426].pkg", SIGIL_PLATFORM_PS3, &off, &r);
        if (rc == SIGIL_OK || r.title_id[0] != '\0') {
            fprintf(stderr, "FAIL ps3, fallback off: rc=%d title_id='%s'\n", rc, r.title_id);
            failures++;
        } else {
            printf("ok ps3, fallback off: no id (%s)\n", sigil_strerror(rc));
        }
    }

    /* A platform named outright, with no naming pattern of its own, gets no
     * id from a name that carries another console's serial. */
    failures += expect_no_id("Game (SLUS-20675).gb", SIGIL_PLATFORM_GB, SIGIL_PLATFORM_GB, "gb named, a ps2 serial");
    failures += expect_no_id("Game (SLUS-20675).gbc", SIGIL_PLATFORM_GBC, SIGIL_PLATFORM_GBC, "gbc named, a ps2 serial");
    failures += expect_no_id("Game (SLUS-20675).gba", SIGIL_PLATFORM_GBA, SIGIL_PLATFORM_GBA, "gba named, a ps2 serial");
    failures += expect_no_id("Game [0100ABCD12345000].z64", SIGIL_PLATFORM_N64, SIGIL_PLATFORM_N64, "n64 named, a switch id");
    failures += expect_no_id("Game [ULUS10064].sfc", SIGIL_PLATFORM_SNES, SIGIL_PLATFORM_SNES, "snes named, a psp id");
    failures += expect_no_id("Game [0100ABCD12345000].xbe", SIGIL_PLATFORM_XBOX, SIGIL_PLATFORM_XBOX, "xbox named, a switch id");
    failures += expect_no_id("Game [ULUS10064].xex", SIGIL_PLATFORM_XBOX360, SIGIL_PLATFORM_XBOX360, "xbox360 named, a psp id");
    failures += expect_no_id("Game [SLUS-20675].gdi", SIGIL_PLATFORM_DREAMCAST, SIGIL_PLATFORM_DREAMCAST, "dreamcast named, a ps2 serial");
    /* The same when the extension, not the caller, names the platform. */
    failures += expect_no_id("Game (SLUS-20675).gba", SIGIL_PLATFORM_AUTO, SIGIL_PLATFORM_GBA, "gba by extension, a ps2 serial");

    /* With nothing naming a platform, the patterns still decide it. */
    char dir[512];
    if (test_temp_dir(dir, sizeof(dir)) != 0) {
        fprintf(stderr, "FAIL no temp folder\n");
        failures++;
    } else {
        failures += expect_guess(dir, "Patapon [ULUS10335].bin", SIGIL_PLATFORM_PSP, "ULUS10335",
                                 "ULUS10335", SIGIL_USAGE_FOLDER_PREFIX, "auto, a psp id");
        /* An id starting with PCS is a retail Vita game, the one PSP-shaped
         * id the guesser reads as Vita. */
        failures += expect_guess(dir, "Gravity Rush [PCSE00123].bin", SIGIL_PLATFORM_PSVITA, "PCSE00123",
                                 "PCSE00123", SIGIL_USAGE_FOLDER_EXACT, "auto, a vita id");
        failures += expect_guess(dir, "Ico (SLES-50760).bin", SIGIL_PLATFORM_PS2, "SLES-50760",
                                 "BESLES-50760", SIGIL_USAGE_FOLDER_PREFIX, "auto, a ps2 serial");
        failures += expect_guess(dir, "Game [0100ABCD12345000].bin", SIGIL_PLATFORM_SWITCH, "0100ABCD12345000",
                                 "0100ABCD12345000", SIGIL_USAGE_FOLDER_EXACT, "auto, a switch id");
        failures += expect_guess(dir, "Game [000500001010EC00].bin", SIGIL_PLATFORM_WIIU, "1010EC00",
                                 "00050000/1010ec00", SIGIL_USAGE_FOLDER_SPLIT, "auto, a wiiu id");
        test_remove_tree(dir);
    }

    if (failures) {
        fprintf(stderr, "unit_filename: %d failed\n", failures);
        return 1;
    }
    printf("ok unit_filename\n");
    return 0;
}
