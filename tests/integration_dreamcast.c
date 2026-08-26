// SPDX-License-Identifier: MPL-2.0
#include "integration_helpers.h"

static int check(const char *path, const char *name) {
    sigil_result r;
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_DREAMCAST, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "  FAIL %s: %s\n", name, sigil_strerror(rc));
        return -1;
    }
    if (r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "  FAIL %s: source=%d (want binary)\n", name, r.source);
        return -1;
    }
    size_t len = strlen(r.title_id);
    if (len == 0 || len > 10) {
        fprintf(stderr, "  FAIL %s: title_id='%s'\n", name, r.title_id);
        return -1;
    }
    if (r.usage != SIGIL_USAGE_FILE_PREFIX) {
        fprintf(stderr, "  FAIL %s: usage=%d (want FILE_PREFIX)\n", name, r.usage);
        return -1;
    }
    fprintf(stdout, "  ok  %s -> %s\n", name, r.title_id);
    return 0;
}

int main(void) {
    const char *rom_dir = get_rom_dir();
    if (!rom_dir) { fprintf(stderr, "SIGIL_ROM_DIR not set; skipping\n"); return TEST_SKIP; }
    char path[512];
    if (build_subdir(rom_dir, "dc", path) != 0) return 1;
    /* A .gdi is a text index naming its track files, not a disc image, so the
     * binary extractor cannot read one; point the test at whole-disc forms. */
    const char *exts[] = { "chd", "iso", NULL };
    walk_stats st = walk_dir(path, check, exts);
    fprintf(stdout, "Dreamcast: processed=%d passed=%d failed=%d\n",
            st.processed, st.passed, st.failed);
    if (st.processed == 0) { fprintf(stderr, "no Dreamcast samples\n"); return TEST_SKIP; }
    return st.failed ? 1 : 0;
}
