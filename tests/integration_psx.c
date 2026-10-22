// SPDX-License-Identifier: MPL-2.0
#include "integration_helpers.h"

static int check(const char *path, const char *name) {
    sigil_result r;
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_PSX, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "  FAIL %s: %s\n", name, sigil_strerror(rc));
        return -1;
    }
    /* PSX canonical: 10 chars LLLL-NNNNN. */
    size_t L = strlen(r.title_id);
    if (L != 10 || r.title_id[4] != '-') {
        fprintf(stderr, "  FAIL %s: title_id=%s (want LLLL-NNNNN)\n", name, r.title_id);
        return -1;
    }
    if (r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "  FAIL %s: source=filename\n", name);
        return -1;
    }
    fprintf(stdout, "  ok  %s -> %s (raw=%s)\n", name, r.title_id, r.raw_serial);
    return 0;
}

int main(void) {
    const char *rom_dir = get_rom_dir();
    if (!rom_dir) { fprintf(stderr, "SIGIL_ROM_DIR not set; skipping\n"); return TEST_SKIP; }
    char path[512];
    if (build_subdir(rom_dir, "psx", path) != 0) return 1;
    const char *exts[] = { "chd", "bin", "iso", "cue", NULL };
    walk_stats st = walk_dir(path, check, exts);
    fprintf(stdout, "PSX: processed=%d passed=%d failed=%d\n",
            st.processed, st.passed, st.failed);
    if (st.processed == 0) { fprintf(stderr, "no PSX samples\n"); return TEST_SKIP; }
    /* GameShark/Action Replay utility discs and a small number of demos lack
     * normal SLUS serials. Tolerate up to 20% missing rather than failing the
     * whole suite. Real regressions show up as much higher failure rates. */
    int threshold = (st.processed * 4) / 5;  /* 80% must pass */
    return st.passed >= threshold ? 0 : 1;
}
