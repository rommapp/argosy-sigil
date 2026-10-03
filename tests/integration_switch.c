// SPDX-License-Identifier: MPL-2.0
#include "integration_helpers.h"

static const char *g_prod_keys = NULL;
static int g_keys_too_old = 0;

static int check(const char *path, const char *name) {
    sigil_support sup = {
        .struct_version = SIGIL_SUPPORT_V1,
        .switch_prod_keys_path = g_prod_keys,
    };
    sigil_options opts = {
        .struct_version = SIGIL_OPTIONS_V1,
        .support = &sup,
        .flags = SIGIL_FLAG_FILENAME_FALLBACK,
    };
    sigil_result r;
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_SWITCH, &opts, &r);
    if (rc == SIGIL_ERR_KEYS_INCOMPATIBLE) {
        /* The key file predates the dump's key generation: the environment,
         * not sigil. Counted apart and left out of the pass rate. */
        fprintf(stdout, "  keys %s: %s\n", name, sigil_strerror(rc));
        g_keys_too_old++;
        return 0;
    }
    if (rc != SIGIL_OK) {
        fprintf(stderr, "  FAIL %s: %s\n", name, sigil_strerror(rc));
        return -1;
    }
    /* Switch canonical: 16 hex chars starting with "01". */
    size_t L = strlen(r.title_id);
    if (L != 16 || r.title_id[0] != '0' || r.title_id[1] != '1') {
        fprintf(stderr, "  FAIL %s: title_id=%s\n", name, r.title_id);
        return -1;
    }
    if (r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "  FAIL %s: source=filename (want binary — wrong tweak math?)\n", name);
        return -1;
    }
    fprintf(stdout, "  ok  %s -> %s\n", name, r.title_id);
    return 0;
}

int main(void) {
    const char *rom_dir = get_rom_dir();
    if (!rom_dir) { fprintf(stderr, "SIGIL_ROM_DIR not set; skipping\n"); return TEST_SKIP; }

    /* Switch needs prod.keys for retail XCIs. Default to the bundled path
     * under /tmp/roms/bios/switch/prod.keys; allow override. */
    g_prod_keys = getenv("SIGIL_PROD_KEYS");
    if (!g_prod_keys) g_prod_keys = "/tmp/roms/bios/switch/prod.keys";

    /* Sanity check the keys file. */
    FILE *fp = fopen(g_prod_keys, "rb");
    if (!fp) {
        fprintf(stderr, "prod.keys missing at %s; skipping\n", g_prod_keys);
        return TEST_SKIP;
    }
    fclose(fp);

    char path[512];
    if (build_subdir(rom_dir, "switch", path) != 0) return 1;
    const char *exts[] = { "xci", "nsp", NULL };
    walk_stats st = walk_dir(path, check, exts);
    int read = st.passed - g_keys_too_old;
    fprintf(stdout, "Switch: processed=%d passed=%d failed=%d keys too old=%d\n",
            st.processed, read, st.failed, g_keys_too_old);
    if (st.processed == 0) { fprintf(stderr, "no Switch samples\n"); return TEST_SKIP; }
    int threshold = ((st.processed - g_keys_too_old) * 4) / 5;
    return read >= threshold ? 0 : 1;
}
