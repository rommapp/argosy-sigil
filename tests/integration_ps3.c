// SPDX-License-Identifier: MPL-2.0
#include "integration_helpers.h"

/* PSN packages in $SIGIL_ROM_DIR/ps3. A package's title id comes from its
 * header and metadata, so the first few KiB of a package are enough to run
 * this. */

static int check(const char *path, const char *name) {
    sigil_result r;
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_PS3, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "  FAIL %s: %s\n", name, sigil_strerror(rc));
        return -1;
    }
    size_t L = strlen(r.title_id);
    if (L != 9) {
        fprintf(stderr, "  FAIL %s: title_id len=%zu (want 9): %s\n", name, L, r.title_id);
        return -1;
    }
    if (strcmp(r.save_id, r.title_id) != 0) {
        fprintf(stderr, "  FAIL %s: save_id=%s differs from title_id=%s\n", name, r.save_id, r.title_id);
        return -1;
    }
    if (r.usage != SIGIL_USAGE_FOLDER_PREFIX) {
        fprintf(stderr, "  FAIL %s: usage=%d (want FOLDER_PREFIX)\n", name, r.usage);
        return -1;
    }
    if (r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "  FAIL %s: source=filename (want binary)\n", name);
        return -1;
    }
    fprintf(stdout, "  ok  %s -> %s\n", name, r.title_id);
    return 0;
}

int main(void) {
    const char *rom_dir = get_rom_dir();
    if (!rom_dir) {
        fprintf(stderr, "SIGIL_ROM_DIR not set; skipping\n");
        return TEST_SKIP;
    }
    char path[512];
    if (build_subdir(rom_dir, "ps3", path) != 0) return 1;

    const char *exts[] = { "pkg", NULL };
    walk_stats st = walk_dir(path, check, exts);
    fprintf(stdout, "PS3: processed=%d passed=%d failed=%d\n",
            st.processed, st.passed, st.failed);
    if (st.processed == 0) {
        fprintf(stderr, "no PS3 packages in %s; skipping\n", path);
        return TEST_SKIP;
    }
    return st.failed ? 1 : 0;
}
