// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
#include "sigil_internal.h"
#include "test_fs.h"
#include <stdbool.h>

#define TEST_SKIP 77

static const char *const KINDS[] = {
    "sram", "rtc", "card", "volume", "vmu", "folder_file", "gci", "wrapped", "other", NULL
};

static const char *const ENTRY_KINDS[] = { "card", "volume", "vmu", "gci", "wrapped", NULL };

static int g_fails = 0;
static int g_present = 0;
static int g_missing = 0;

static void fail(const char *platform, const char *file, size_t row, const char *what) {
    fprintf(stderr, "FAIL %s/%s row %zu: %s\n", platform, file, row + 1, what);
    g_fails++;
}

static bool in_list(const char *value, const char *const *list) {
    for (size_t i = 0; list[i]; i++) {
        if (strcmp(value, list[i]) == 0) return true;
    }
    return false;
}

static bool is_md5_hex(const char *s) {
    if (strlen(s) != 32) return false;
    for (size_t i = 0; i < 32; i++) {
        if (!isdigit((unsigned char)s[i]) && !(s[i] >= 'a' && s[i] <= 'f')) return false;
    }
    return true;
}

static bool is_count(const char *s) {
    if (!*s) return false;
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) return false;
    }
    return true;
}

static bool header_matches(const corpus_table *t, const char *const *expected) {
    size_t n = 0;
    while (expected[n]) n++;
    if (t->header.ncols != n) return false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(t->header.cols[i], expected[i]) != 0) return false;
    }
    return true;
}

static bool verified_independently(const char *verified_by) {
    return *verified_by && strcmp(verified_by, "-") != 0 && strstr(verified_by, "sigil") == NULL;
}

static void md5_file_hex(const unsigned char *buf, size_t len, char out[33]) {
    sigil_md5 m;
    uint8_t digest[16];
    sigil_md5_init(&m);
    sigil_md5_update(&m, buf, len);
    sigil_md5_final(&m, digest);
    sigil_md5_hex(digest, out);
}

static void check_manifest_row(const char *platform, const corpus_table *t, size_t r) {
    if (t->rows[r].ncols != t->header.ncols) {
        fail(platform, "manifest.tsv", r, "column count differs from the header");
        return;
    }
    const char *id = corpus_get(t, r, "id");
    const char *path = corpus_get(t, r, "path");
    const char *size = corpus_get(t, r, "size");
    const char *md5 = corpus_get(t, r, "md5");
    const char *kind = corpus_get(t, r, "kind");
    const char *verified_by = corpus_get(t, r, "verified_by");

    if (!*id || !*path) fail(platform, "manifest.tsv", r, "empty id or path");
    if (!is_count(size)) fail(platform, "manifest.tsv", r, "size is not a byte count");
    if (!is_md5_hex(md5)) fail(platform, "manifest.tsv", r, "md5 is not 32 lowercase hex digits");
    if (!in_list(kind, KINDS)) fail(platform, "manifest.tsv", r, "unknown kind");
    if (!verified_independently(verified_by)) {
        fail(platform, "manifest.tsv", r, "verified_by is empty or names sigil");
    }

    char full[1024];
    if (corpus_sample_path(platform, id, path, full, sizeof(full)) != 0) {
        fail(platform, "manifest.tsv", r, "sample path too long");
        return;
    }
    size_t len = 0;
    unsigned char *buf = corpus_read_file(full, &len);
    if (!buf) {
        g_missing++;
        return;
    }
    g_present++;
    if (is_count(size) && strtoull(size, NULL, 10) != (unsigned long long)len) {
        fail(platform, "manifest.tsv", r, "file size differs from the manifest");
    }
    char hex[33];
    md5_file_hex(buf, len, hex);
    if (is_md5_hex(md5) && strcmp(hex, md5) != 0) {
        fail(platform, "manifest.tsv", r, "file md5 differs from the manifest");
    }
    free(buf);
}

static const char *manifest_kind_for(const corpus_table *manifest, const char *id, const char *path) {
    for (size_t r = 0; r < manifest->nrows; r++) {
        const char *mid = corpus_get(manifest, r, "id");
        const char *mpath = corpus_get(manifest, r, "path");
        if (mid && mpath && strcmp(mid, id) == 0 && strcmp(mpath, path) == 0) {
            return corpus_get(manifest, r, "kind");
        }
    }
    return NULL;
}

static void check_entries(const char *platform, const corpus_table *manifest) {
    char path[1024];
    if (corpus_platform_path(platform, "entries.tsv", path, sizeof(path)) != 0) return;
    corpus_table entries;
    if (corpus_load(path, &entries) != 0) return;

    if (!header_matches(&entries, CORPUS_ENTRIES_COLUMNS)) {
        fail(platform, "entries.tsv", 0, "header differs from tests/fixtures/saves/README.md");
        corpus_free(&entries);
        return;
    }
    for (size_t r = 0; r < entries.nrows; r++) {
        if (entries.rows[r].ncols != entries.header.ncols) {
            fail(platform, "entries.tsv", r, "column count differs from the header");
            continue;
        }
        const char *kind = manifest_kind_for(manifest, corpus_get(&entries, r, "id"),
                                             corpus_get(&entries, r, "path"));
        if (!kind) fail(platform, "entries.tsv", r, "id and path match no manifest row");
        else if (!in_list(kind, ENTRY_KINDS)) {
            fail(platform, "entries.tsv", r, "manifest kind has no entries");
        }
        if (!*corpus_get(&entries, r, "entry")) fail(platform, "entries.tsv", r, "empty entry name");
        if (!is_count(corpus_get(&entries, r, "blocks"))) {
            fail(platform, "entries.tsv", r, "blocks is not a count");
        }
        if (!is_md5_hex(corpus_get(&entries, r, "data_md5"))) {
            fail(platform, "entries.tsv", r, "data_md5 is not 32 lowercase hex digits");
        }
        if (!verified_independently(corpus_get(&entries, r, "verified_by"))) {
            fail(platform, "entries.tsv", r, "verified_by is empty or names sigil");
        }
    }
    corpus_free(&entries);
}

static void check_platform(const char *platform) {
    char path[1024];
    if (corpus_platform_path(platform, "manifest.tsv", path, sizeof(path)) != 0) return;
    corpus_table manifest;
    if (corpus_load(path, &manifest) != 0) return;

    if (!header_matches(&manifest, CORPUS_MANIFEST_COLUMNS)) {
        fail(platform, "manifest.tsv", 0, "header differs from tests/fixtures/saves/README.md");
        corpus_free(&manifest);
        return;
    }
    for (size_t r = 0; r < manifest.nrows; r++) {
        check_manifest_row(platform, &manifest, r);
        if (manifest.rows[r].ncols != manifest.header.ncols) continue;
        for (size_t earlier = 0; earlier < r; earlier++) {
            if (manifest.rows[earlier].ncols != manifest.header.ncols) continue;
            if (strcmp(corpus_get(&manifest, r, "id"), corpus_get(&manifest, earlier, "id")) == 0 &&
                strcmp(corpus_get(&manifest, r, "path"), corpus_get(&manifest, earlier, "path")) == 0) {
                fail(platform, "manifest.tsv", r, "duplicate id and path");
            }
        }
    }
    check_entries(platform, &manifest);
    corpus_free(&manifest);
}

static bool check_platform_entry(void *ctx, const char *name, bool is_dir) {
    (void)ctx;
    if (name[0] != '.' && is_dir) check_platform(name);
    return true;
}

int main(void) {
    if (test_dir_each(SIGIL_SAVE_FIXTURES, check_platform_entry, NULL) != 0) {
        fprintf(stderr, "SKIP: no %s\n", SIGIL_SAVE_FIXTURES);
        return TEST_SKIP;
    }

    printf("save corpus: %d present, %d missing, %d failures\n", g_present, g_missing, g_fails);
    if (g_fails) return 1;
    return g_present ? 0 : TEST_SKIP;
}
