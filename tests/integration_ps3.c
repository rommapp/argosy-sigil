// SPDX-License-Identifier: MPL-2.0
/* PSN packages recorded in tests/fixtures/saves/ps3/manifest.tsv. Each
 * sample is the first 4096 bytes of a package: its title id comes from the
 * header and the metadata, which end inside them. A row's content_id is the
 * title id the package installs under, read from its header by a script and
 * checked against another source, and the result must carry it as title_id
 * and save_id. Each row's check runs when its sample is present, and the
 * test skips when none ran. */
#include "save_corpus.h"
#include "sigil.h"

#define TEST_SKIP 77

static int check(const char *id, const char *path, const char *want) {
    sigil_options opts = { .struct_version = SIGIL_OPTIONS_V1 };
    sigil_result r;
    memset(&r, 0, sizeof(r));
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_PS3, &opts, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL %s: %s\n", id, sigil_strerror(rc));
        return 1;
    }
    if (strcmp(r.title_id, want) != 0 || strcmp(r.save_id, want) != 0) {
        fprintf(stderr, "FAIL %s: title_id=%s save_id=%s (want %s)\n", id, r.title_id, r.save_id, want);
        return 1;
    }
    if (r.platform != SIGIL_PLATFORM_PS3 || r.usage != SIGIL_USAGE_FOLDER_PREFIX
        || r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "FAIL %s: platform=%d usage=%d source=%d (want ps3, folder-prefix, binary)\n",
                id, (int)r.platform, (int)r.usage, (int)r.source);
        return 1;
    }
    printf("ok %s -> %s\n", id, r.title_id);
    return 0;
}

int main(void) {
    char path[1024];
    corpus_table t;
    if (corpus_platform_path("ps3", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &t) != 0) {
        fprintf(stderr, "SKIP: no ps3 manifest\n");
        return TEST_SKIP;
    }
    int ran = 0, fails = 0;
    for (size_t r = 0; r < t.nrows; r++) {
        const char *id = corpus_get(&t, r, "id");
        char sample[1024];
        if (!corpus_present(&t, "ps3", id)
            || corpus_sample_path("ps3", id, corpus_get(&t, r, "path"), sample, sizeof(sample)) != 0) {
            continue;
        }
        ran++;
        fails += check(id, sample, corpus_get(&t, r, "content_id"));
    }
    corpus_free(&t);
    if (!ran) {
        fprintf(stderr, "SKIP: no ps3 samples present\n");
        return TEST_SKIP;
    }
    printf("ps3 packages: %d checks ran, %d failures\n", ran, fails);
    return corpus_exit(fails);
}
