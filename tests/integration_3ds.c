// SPDX-License-Identifier: MPL-2.0
#include "integration_helpers.h"
#include "threeds_fixture.h"

/* Enough of a real image to cover partition 0's NCCH header at 0x4000+0x118. */
#define SAMPLE_PREFIX_LEN 0x8000
#define NCSD_STOCK_PARTITION0 0x4000

static int run_mem(const uint8_t *buf, size_t len, const char *name, sigil_result *out) {
    mem_ctx ctx = { buf, len };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    return sigil_extract_from_io(&io, name, SIGIL_PLATFORM_3DS, NULL, out);
}

/* The wrapper and NCCH paths are re-derived from the same real bytes: a Z3DS
 * container built here around the image's own prefix, and the image's own
 * partition-0 NCCH lifted out as a bare .cxi. Both must resolve the id the
 * plain read produced. This proves sigil's parsers against real 3DS data; it
 * does NOT prove the Z3DS byte layout, which only an Azahar-written file can. */
static int check_derived(const char *path, const char *name, const char *want_tid) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "  FAIL %s: cannot reopen\n", name);
        return -1;
    }
    uint8_t *prefix = (uint8_t *)malloc(SAMPLE_PREFIX_LEN);
    if (!prefix) { fclose(f); return -1; }
    size_t got = fread(prefix, 1, SAMPLE_PREFIX_LEN, f);
    fclose(f);
    if (got < SAMPLE_PREFIX_LEN) {
        free(prefix);
        return 0;
    }

    int rc = 0;
    sigil_result r;

    size_t zlen = 0;
    uint8_t *z = z3ds_build(prefix, SAMPLE_PREFIX_LEN, "NCSD", NULL, 0, 0x1000, &zlen);
    if (!z) {
        rc = -1;
    } else {
        if (run_mem(z, zlen, "sample.z3ds", &r) != SIGIL_OK
            || strcmp(r.title_id, want_tid) != 0 || r.source != SIGIL_SOURCE_BINARY) {
            fprintf(stderr, "  FAIL %s: z3ds-wrapped id mismatch\n", name);
            rc = -1;
        }
        free(z);
    }

    if (run_mem(prefix + NCSD_STOCK_PARTITION0, SAMPLE_PREFIX_LEN - NCSD_STOCK_PARTITION0,
                "sample.cxi", &r) != SIGIL_OK
        || strcmp(r.title_id, want_tid) != 0 || r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "  FAIL %s: bare-NCCH id mismatch\n", name);
        rc = -1;
    }

    free(prefix);
    return rc;
}

static int check(const char *path, const char *name) {
    sigil_result r;
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_3DS, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "  FAIL %s: %s\n", name, sigil_strerror(rc));
        return -1;
    }
    /* 3DS retail: 16 hex chars starting with "0004". */
    if (strlen(r.title_id) != 16) {
        fprintf(stderr, "  FAIL %s: title_id=%s (want 16 hex)\n", name, r.title_id);
        return -1;
    }
    if (memcmp(r.title_id, "0004", 4) != 0) {
        fprintf(stderr, "  FAIL %s: title_id=%s (want 0004 prefix)\n", name, r.title_id);
        return -1;
    }
    if (r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "  FAIL %s: source=filename\n", name);
        return -1;
    }
    if (r.usage != SIGIL_USAGE_FOLDER_SPLIT) {
        fprintf(stderr, "  FAIL %s: usage=%d (want folder-split)\n", name, r.usage);
        return -1;
    }
    /* save_id is the on-disk split, not the flat id: <high 8>/<low 8>. */
    char want_save_id[18];
    snprintf(want_save_id, sizeof(want_save_id), "%.8s/%.8s", r.title_id, r.title_id + 8);
    if (strcmp(r.save_id, want_save_id) != 0) {
        fprintf(stderr, "  FAIL %s: save_id=%s (want %s)\n", name, r.save_id, want_save_id);
        return -1;
    }

    char ext[16];
    ext_of(name, ext);
    if (strcmp(ext, "3ds") == 0 || strcmp(ext, "cci") == 0) {
        if (check_derived(path, name, r.title_id) != 0) return -1;
    }

    fprintf(stdout, "  ok  %s -> %s\n", name, r.save_id);
    return 0;
}

int main(void) {
    const char *rom_dir = get_rom_dir();
    if (!rom_dir) { fprintf(stderr, "SIGIL_ROM_DIR not set; skipping\n"); return TEST_SKIP; }
    char path[512];
    if (build_subdir(rom_dir, "3ds", path) != 0) return 1;
    const char *exts[] = { "3ds", "cci", "z3ds", "zcci", "cxi", "zcxi", "app", NULL };
    walk_stats st = walk_dir(path, check, exts);
    fprintf(stdout, "3DS: processed=%d passed=%d failed=%d\n",
            st.processed, st.passed, st.failed);
    if (st.processed == 0) { fprintf(stderr, "no 3DS samples\n"); return TEST_SKIP; }
    int threshold = (st.processed * 4) / 5;
    return st.passed >= threshold ? 0 : 1;
}
