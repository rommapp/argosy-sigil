// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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
        "sigil %s — extract platform-native title IDs from ROM files\n"
        "\n"
        "Usage: sigil [--platform=<slug>] [--prod-keys=<path>] <rom>\n"
        "\n"
        "Options:\n"
        "  --platform=<slug>   Force a platform (psp, psx, ps2, ps3, switch, 3ds,\n"
        "                      wii, wiiu, gamecube, psvita, xbox360, dreamcast).\n"
        "                      Default: auto-detect.\n"
        "  --prod-keys=<path>  Switch prod.keys file for NCA decryption.\n"
        "  --help              Show this message.\n",
        sigil_version());
}

int main(int argc, char **argv) {
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

    printf("platform=%s title_id=%s raw_serial=%s save_id=%s usage=%s source=%s experimental=%d content_type=%s title_version=%u\n",
           sigil_platform_to_slug(r.platform),
           r.title_id, r.raw_serial, r.save_id,
           usage_to_str(r.usage), source_to_str(r.source), r.experimental,
           content_type_to_str(r.switch_content_type), r.title_version);
    return 0;
}
