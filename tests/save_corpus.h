// SPDX-License-Identifier: MPL-2.0
/*
 * Reader for the save sample manifests under tests/fixtures/saves. The
 * layout and column meanings live in tests/fixtures/saves/README.md.
 */
#ifndef SIGIL_TEST_SAVE_CORPUS_H
#define SIGIL_TEST_SAVE_CORPUS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SIGIL_SAVE_FIXTURES
#error "SIGIL_SAVE_FIXTURES must name the tests/fixtures/saves directory"
#endif

#define CORPUS_MAX_COLS 16

static const char *const CORPUS_MANIFEST_COLUMNS[] = {
    "id", "path", "size", "md5", "emulator", "emulator_version", "game",
    "content_id", "kind", "source", "verified_by", "notes", NULL
};

static const char *const CORPUS_ENTRIES_COLUMNS[] = {
    "id", "path", "entry", "owner_id", "blocks", "data_md5", "verified_by", NULL
};

typedef struct {
    char  *line;
    char  *cols[CORPUS_MAX_COLS];
    size_t ncols;
} corpus_row;

/** A parsed TSV file: the header names and every data row. */
typedef struct {
    corpus_row  header;
    corpus_row *rows;
    size_t      nrows;
} corpus_table;

static void corpus_split(corpus_row *row) {
    row->ncols = 0;
    char *p = row->line;
    while (row->ncols < CORPUS_MAX_COLS) {
        row->cols[row->ncols++] = p;
        char *tab = strchr(p, '\t');
        if (!tab) break;
        *tab = '\0';
        p = tab + 1;
    }
}

static char *corpus_read_line(FILE *f) {
    size_t cap = 256, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n') {
        if (len + 1 >= cap) {
            char *grown = (char *)realloc(buf, cap * 2);
            if (!grown) { free(buf); return NULL; }
            buf = grown;
            cap *= 2;
        }
        buf[len++] = (char)c;
    }
    if (c == EOF && len == 0) { free(buf); return NULL; }
    if (len > 0 && buf[len - 1] == '\r') len--;
    buf[len] = '\0';
    return buf;
}

/** Loads a TSV file. Returns 0 on success, -1 when the file can't be read. */
static int corpus_load(const char *path, corpus_table *out) {
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char *line = corpus_read_line(f);
    if (!line) { fclose(f); return -1; }
    out->header.line = line;
    corpus_split(&out->header);

    size_t cap = 0;
    while ((line = corpus_read_line(f)) != NULL) {
        if (line[0] == '\0') { free(line); continue; }
        if (out->nrows == cap) {
            size_t next = cap ? cap * 2 : 16;
            corpus_row *grown = (corpus_row *)realloc(out->rows, next * sizeof(*grown));
            if (!grown) { free(line); break; }
            out->rows = grown;
            cap = next;
        }
        corpus_row *row = &out->rows[out->nrows++];
        row->line = line;
        corpus_split(row);
    }
    fclose(f);
    return 0;
}

static void corpus_free(corpus_table *t) {
    free(t->header.line);
    for (size_t i = 0; i < t->nrows; i++) free(t->rows[i].line);
    free(t->rows);
    memset(t, 0, sizeof(*t));
}

/** Returns the column index for `name`, or -1 when the header lacks it. */
static int corpus_col(const corpus_table *t, const char *name) {
    for (size_t i = 0; i < t->header.ncols; i++) {
        if (strcmp(t->header.cols[i], name) == 0) return (int)i;
    }
    return -1;
}

/** Returns row `r`'s value for column `name`, or NULL when absent. */
static const char *corpus_get(const corpus_table *t, size_t r, const char *name) {
    int c = corpus_col(t, name);
    if (c < 0 || (size_t)c >= t->rows[r].ncols) return NULL;
    return t->rows[r].cols[c];
}

/** Writes `<fixtures>/<platform>/<name>` into `out`. */
static int corpus_platform_path(const char *platform, const char *name, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s/%s/%s", SIGIL_SAVE_FIXTURES, platform, name);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/** Writes `<fixtures>/<platform>/files/<id>/<path>` into `out`. */
static int corpus_sample_path(const char *platform, const char *id, const char *path,
                              char *out, size_t cap) {
    int n = snprintf(out, cap, "%s/%s/files/%s/%s", SIGIL_SAVE_FIXTURES, platform, id, path);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/** Reads a whole file into a malloc'd buffer. Returns NULL when it can't. */
static unsigned char *corpus_read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc(n > 0 ? (size_t)n : 1);
    if (!buf) { fclose(f); return NULL; }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len_out = (size_t)n;
    return buf;
}

/* Samples a check named that weren't there. Once a platform's samples are
 * present, every sample a check names must be: a test with any missing
 * sample fails instead of passing on the checks that did run. */
static int g_corpus_missing = 0;

/* The manifest row of sample `id`: its row with `path`, or its first row when `path` is NULL. */
static int corpus_row_of(const corpus_table *manifest, const char *id, const char *path) {
    for (size_t r = 0; r < manifest->nrows; r++) {
        if (strcmp(corpus_get(manifest, r, "id"), id) != 0) continue;
        if (path && strcmp(corpus_get(manifest, r, "path"), path) != 0) continue;
        return (int)r;
    }
    return -1;
}

static unsigned char *corpus_try_sample(const corpus_table *manifest, const char *platform, const char *id,
                                        const char *path, size_t *len) {
    int r = corpus_row_of(manifest, id, path);
    char full[1024];
    if (r < 0 || corpus_sample_path(platform, id, corpus_get(manifest, (size_t)r, "path"), full, sizeof(full)) != 0) {
        return NULL;
    }
    return corpus_read_file(full, len);
}

/** True when sample `id` is on disk; probes whether a platform's samples are present at all. */
static int corpus_present(const corpus_table *manifest, const char *platform, const char *id) {
    size_t len = 0;
    unsigned char *data = corpus_try_sample(manifest, platform, id, NULL, &len);
    free(data);
    return data != NULL;
}

/** The bytes of sample `id` (see corpus_row_of), or NULL after counting and reporting it missing. */
static unsigned char *corpus_sample(const corpus_table *manifest, const char *platform, const char *id,
                                    const char *path, size_t *len) {
    unsigned char *data = corpus_try_sample(manifest, platform, id, path, len);
    if (!data) {
        fprintf(stderr, "MISSING sample %s/%s%s%s\n", platform, id, path ? "/" : "", path ? path : "");
        g_corpus_missing++;
    }
    return data;
}

/** Counts and reports every file `manifest` lists for `platform` that isn't on disk. */
static int corpus_count_missing(const corpus_table *manifest, const char *platform) {
    int missing = 0;
    for (size_t r = 0; r < manifest->nrows; r++) {
        const char *id = corpus_get(manifest, r, "id");
        const char *path = corpus_get(manifest, r, "path");
        char full[1024];
        if (!id || !path || corpus_sample_path(platform, id, path, full, sizeof(full)) != 0) continue;
        FILE *f = fopen(full, "rb");
        if (f) { fclose(f); continue; }
        fprintf(stderr, "MISSING sample %s/%s/%s\n", platform, id, path);
        missing++;
    }
    return missing;
}

/** The exit status for `fails` failed checks: any missing sample fails the run too. */
static int corpus_exit(int fails) {
    return fails || g_corpus_missing ? 1 : 0;
}

#endif
