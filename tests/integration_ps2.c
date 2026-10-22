// SPDX-License-Identifier: MPL-2.0
#include "integration_helpers.h"

/* save_id is the folder-prefix stem: B<region letter> + full title_id, no
 * per-artifact suffix. */
static int save_id_matches_title(const char *save_id, const char *title_id) {
    if (save_id[0] != 'B' || save_id[1] < 'A' || save_id[1] > 'Z') return 0;
    return strcmp(save_id + 2, title_id) == 0;
}

static int check(const char *path, const char *name) {
    sigil_result r;
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_PS2, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "  FAIL %s: %s\n", name, sigil_strerror(rc));
        return -1;
    }
    size_t L = strlen(r.title_id);
    if (L != 10 || r.title_id[4] != '-') {
        fprintf(stderr, "  FAIL %s: title_id=%s\n", name, r.title_id);
        return -1;
    }
    if (r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "  FAIL %s: source=filename\n", name);
        return -1;
    }
    if (!save_id_matches_title(r.save_id, r.title_id)) {
        fprintf(stderr, "  FAIL %s: save_id=%s does not match title_id=%s\n",
                name, r.save_id, r.title_id);
        return -1;
    }
    fprintf(stdout, "  ok  %s -> %s (raw=%s, save_id=%s)\n",
            name, r.title_id, r.raw_serial, r.save_id);
    return 0;
}

int main(void) {
    const char *rom_dir = get_rom_dir();
    if (!rom_dir) { fprintf(stderr, "SIGIL_ROM_DIR not set; skipping\n"); return TEST_SKIP; }
    char path[512];
    if (build_subdir(rom_dir, "ps2", path) != 0) return 1;
    const char *exts[] = { "iso", "chd", "cso", "zso", NULL };
    walk_stats st = walk_dir(path, check, exts);
    fprintf(stdout, "PS2: processed=%d passed=%d failed=%d\n",
            st.processed, st.passed, st.failed);
    if (st.processed == 0) { fprintf(stderr, "no PS2 samples\n"); return TEST_SKIP; }
    return st.failed ? 1 : 0;
}
