// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

/* The arguments in UTF-8, as sigil takes paths; Windows hands main() the
 * ANSI code page, which can't spell most non-Latin file names. */
static char **utf8_argv(int *argc) {
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), argc);
    if (!wargv) return NULL;
    char **argv = (char **)calloc((size_t)*argc + 1, sizeof(char *));
    for (int i = 0; argv && i < *argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = n > 0 ? (char *)malloc((size_t)n) : NULL;
        if (!argv[i] || !WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL)) {
            argv = NULL;
        }
    }
    LocalFree(wargv);
    return argv;
}
#endif

static const char *usage_to_str(sigil_usage u) {
    switch (u) {
    case SIGIL_USAGE_FOLDER_EXACT:  return "folder-exact";
    case SIGIL_USAGE_FOLDER_PREFIX: return "folder-prefix";
    case SIGIL_USAGE_FILE_EXACT:    return "file-exact";
    case SIGIL_USAGE_FILE_PREFIX:   return "file-prefix";
    case SIGIL_USAGE_FOLDER_SPLIT:  return "folder-split";
    default:                         return "?";
    }
}

static const char *source_to_str(sigil_source s) {
    return s == SIGIL_SOURCE_BINARY ? "binary" : "filename";
}

static const char *content_type_to_str(int t) {
    switch (t) {
    case SIGIL_SWITCH_CONTENT_APPLICATION: return "application";
    case SIGIL_SWITCH_CONTENT_PATCH:       return "patch";
    case SIGIL_SWITCH_CONTENT_ADDON:       return "addon";
    default:                                return "unknown";
    }
}

static void print_usage(void) {
    fprintf(stderr,
        "sigil %s. Reads the platform-native title ID from a ROM file.\n"
        "\n"
        "Usage: sigil [--platform=<slug>] [--prod-keys=<path>] <rom>\n"
        "\n"
        "Options:\n"
        "  --platform=<slug>   Force a platform (psp, psx, ps2, ps3, switch, 3ds,\n"
        "                      wii, wiiu, gamecube, psvita, xbox, xbox360,\n"
        "                      dreamcast, gb, gbc, snes, n64).\n"
        "                      Default: auto-detect.\n"
        "  --prod-keys=<path>  Switch prod.keys file for NCA decryption.\n"
        "  --help              Show this message.\n",
        sigil_version());
}

int main(int argc, char **argv) {
#ifdef _WIN32
    argv = utf8_argv(&argc);
    if (!argv) {
        fprintf(stderr, "sigil: could not read the command line\n");
        return 2;
    }
    SetConsoleOutputCP(CP_UTF8);
#endif
    const char *path = NULL;
    sigil_platform hint = SIGIL_PLATFORM_AUTO;
    sigil_support sup = { .struct_version = SIGIL_SUPPORT_V1 };
    bool have_keys = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_usage();
            return 0;
        } else if (strncmp(a, "--platform=", 11) == 0) {
            hint = sigil_platform_from_slug(a + 11);
            if (hint == SIGIL_PLATFORM_AUTO && strcmp(a + 11, "auto") != 0) {
                fprintf(stderr, "sigil: unknown platform '%s'\n", a + 11);
                return 2;
            }
        } else if (strncmp(a, "--prod-keys=", 12) == 0) {
            sup.switch_prod_keys_path = a + 12;
            have_keys = true;
        } else if (a[0] == '-') {
            fprintf(stderr, "sigil: unknown option '%s'\n", a);
            return 2;
        } else {
            path = a;
        }
    }

    if (!path) {
        print_usage();
        return 2;
    }

    sigil_options opts = {
        .struct_version = SIGIL_OPTIONS_V1,
        .support = have_keys ? &sup : NULL,
        .flags = SIGIL_FLAG_FILENAME_FALLBACK
    };

    sigil_result r;
    int rc = sigil_extract_from_path(path, hint, &opts, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "sigil: %s (%s)\n", sigil_strerror(rc), path);
        return 1;
    }

    printf("platform=%s title_id=%s raw_serial=%s save_id=%s usage=%s source=%s experimental=%d content_type=%s title_version=%u features=%s\n",
           sigil_platform_to_slug(r.platform),
           r.title_id, r.raw_serial, r.save_id,
           usage_to_str(r.usage), source_to_str(r.source), r.experimental,
           content_type_to_str(r.switch_content_type), r.title_version,
           (r.features & SIGIL_FEATURE_RTC) ? "rtc" : "-");
    if (r.platform == SIGIL_PLATFORM_N64) {
        printf("n64_header=%s n64_md5=%s n64_md5_n64=%s\n", r.n64_header, r.n64_md5, r.n64_md5_n64);
    }
    return 0;
}
