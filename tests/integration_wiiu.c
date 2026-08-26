// SPDX-License-Identifier: MPL-2.0
#include "integration_helpers.h"

/* Wii U integration test is uniquely self-validating: filenames already
 * carry the canonical 16-hex title id in brackets (e.g.
 * "Angry Birds Trilogy [0005000010138A00].wua"), and sigil reads the
 * SAME value from the WUA archive footer. The two MUST agree, or one of
 * them is wrong.
 *
 * Cemu names the save directory with "{:08x}", so save_id must be title_id
 * lowercased; an uppercase copy is a second directory on case-sensitive
 * storage and the saves split across the two. */

static int extract_bracket_id(const char *name, char out[17]) {
    size_t len = strlen(name);
    /* Find `[` and `]`. */
    const char *open = strchr(name, '[');
    if (!open) return -1;
    const char *close = strchr(open + 1, ']');
    if (!close) return -1;
    size_t span = (size_t)(close - open - 1);
    if (span != 16) return -1;
    for (size_t i = 0; i < 16; i++) {
        char c = open[1 + i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) return -1;
        out[i] = (c >= 'a' && c <= 'f') ? (char)(c - 32) : c;
    }
    out[16] = '\0';
    (void)len;
    return 0;
}

static int check(const char *path, const char *name) {
    sigil_result r;
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_WIIU, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "  FAIL %s: %s\n", name, sigil_strerror(rc));
        return -1;
    }
    if (r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "  FAIL %s: source=filename (binary parse failed)\n", name);
        return -1;
    }

    /* If the filename has a 16-hex bracket, cross-check it. */
    char bracket_id[17];
    if (extract_bracket_id(name, bracket_id) == 0) {
        if (strcmp(r.raw_serial, bracket_id) != 0) {
            fprintf(stderr, "  FAIL %s: raw=%s != bracket=%s\n",
                    name, r.raw_serial, bracket_id);
            return -1;
        }
    }
    if (strlen(r.save_id) != strlen(r.title_id)) {
        fprintf(stderr, "  FAIL %s: save_id=%s title_id=%s\n", name, r.save_id, r.title_id);
        return -1;
    }
    for (size_t i = 0; r.title_id[i]; i++) {
        char want = r.title_id[i];
        if (want >= 'A' && want <= 'Z') want = (char)(want + 32);
        if (r.save_id[i] != want) {
            fprintf(stderr, "  FAIL %s: save_id=%s title_id=%s\n",
                    name, r.save_id, r.title_id);
            return -1;
        }
    }
    fprintf(stdout, "  ok  %s -> %s (raw=%s save=%s)\n",
            name, r.title_id, r.raw_serial, r.save_id);
    return 0;
}

int main(void) {
    const char *rom_dir = get_rom_dir();
    if (!rom_dir) { fprintf(stderr, "SIGIL_ROM_DIR not set; skipping\n"); return TEST_SKIP; }
    char path[512];
    if (build_subdir(rom_dir, "wiiu", path) != 0) return 1;
    const char *exts[] = { "wua", NULL };
    walk_stats st = walk_dir(path, check, exts);
    fprintf(stdout, "Wii U: processed=%d passed=%d failed=%d\n",
            st.processed, st.passed, st.failed);
    if (st.processed == 0) { fprintf(stderr, "no Wii U samples\n"); return TEST_SKIP; }
    return st.failed ? 1 : 0;
}
