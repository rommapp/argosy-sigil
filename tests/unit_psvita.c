// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include "test_fs.h"
#include <stdio.h>
#include <string.h>

typedef struct { const char *key; const char *value; } sfo_entry;

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Builds a param.sfo holding `n` utf8 string entries. */
static size_t build_sfo(uint8_t *buf, size_t cap, const sfo_entry *e, size_t n) {
    size_t key_table = 20 + n * 16;
    size_t key_size = 0;
    for (size_t i = 0; i < n; i++) key_size += strlen(e[i].key) + 1;
    while (key_size % 4 != 0) key_size++;
    size_t data_table = key_table + key_size;
    size_t data_size = 0;
    for (size_t i = 0; i < n; i++) data_size += strlen(e[i].value) + 1;
    if (data_table + data_size > cap) return 0;

    memset(buf, 0, data_table + data_size);
    buf[1] = 'P'; buf[2] = 'S'; buf[3] = 'F';
    write_le32(buf + 4, 0x00000101);
    write_le32(buf + 8, (uint32_t)key_table);
    write_le32(buf + 12, (uint32_t)data_table);
    write_le32(buf + 16, (uint32_t)n);

    size_t key_off = 0, data_off = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t *x = buf + 20 + i * 16;
        size_t len = strlen(e[i].value) + 1;
        x[0] = (uint8_t)key_off; x[1] = (uint8_t)(key_off >> 8);
        x[2] = 0x04; x[3] = 0x02;
        write_le32(x + 4, (uint32_t)len);
        write_le32(x + 8, (uint32_t)len);
        write_le32(x + 12, (uint32_t)data_off);
        memcpy(buf + key_table + key_off, e[i].key, strlen(e[i].key) + 1);
        memcpy(buf + data_table + data_off, e[i].value, len);
        key_off += strlen(e[i].key) + 1;
        data_off += len;
    }
    return data_table + data_size;
}

typedef struct { const uint8_t *buf; size_t len; } mem_ctx;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_ctx *m = (mem_ctx *)ctx;
    if (off >= m->len) return 0;
    size_t avail = m->len - (size_t)off;
    size_t n = len < avail ? len : avail;
    memcpy(buf, m->buf + off, n);
    return (int)n;
}
static int64_t mem_size(void *ctx) { return (int64_t)((mem_ctx *)ctx)->len; }

static int expect(const sfo_entry *e, size_t n, const char *want_save, const char *label) {
    uint8_t blob[1024];
    size_t len = build_sfo(blob, sizeof(blob), e, n);
    if (len == 0) { fprintf(stderr, "FAIL %s: build_sfo\n", label); return 1; }
    mem_ctx ctx = { blob, len };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, "param.sfo", SIGIL_PLATFORM_PSVITA, NULL, &r);
    if (rc != SIGIL_OK) { fprintf(stderr, "FAIL %s: rc=%d\n", label, rc); return 1; }
    if (strcmp(r.title_id, "PCSE00123") != 0 || strcmp(r.raw_serial, "PCSE00123") != 0
        || strcmp(r.save_id, want_save) != 0 || r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "FAIL %s: title_id='%s' raw_serial='%s' save_id='%s' (want '%s') source=%d\n",
                label, r.title_id, r.raw_serial, r.save_id, want_save, (int)r.source);
        return 1;
    }
    return 0;
}

static int write_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = test_fopen(path, "wb");
    if (!f) return -1;
    size_t n = fwrite(data, 1, len, f);
    return fclose(f) == 0 && n == len ? 0 : -1;
}

/* An error code, or the result one extract call owes. */
typedef struct {
    int rc;
    sigil_platform platform;
    const char *title_id;
    const char *save_id;
    sigil_usage usage;
    sigil_source source;
} want_result;

static int check(const char *label, int rc, const sigil_result *r, const want_result *w) {
    if (rc != w->rc) {
        fprintf(stderr, "FAIL %s: rc=%d (want %d)", label, rc, w->rc);
        if (rc == SIGIL_OK) {
            fprintf(stderr, " platform=%s title_id='%s'",
                    sigil_platform_to_slug(r->platform), r->title_id);
        }
        fprintf(stderr, "\n");
        return 1;
    }
    if (rc != SIGIL_OK) return 0;
    if (r->platform != w->platform || strcmp(r->title_id, w->title_id) != 0
        || strcmp(r->raw_serial, w->title_id) != 0 || strcmp(r->save_id, w->save_id) != 0
        || r->usage != w->usage || r->source != w->source) {
        fprintf(stderr, "FAIL %s: platform=%s title_id='%s' raw_serial='%s' save_id='%s' "
                        "usage=%d source=%d (want %s '%s' '%s' %d %d)\n",
                label, sigil_platform_to_slug(r->platform), r->title_id, r->raw_serial,
                r->save_id, (int)r->usage, (int)r->source,
                sigil_platform_to_slug(w->platform), w->title_id, w->save_id,
                (int)w->usage, (int)w->source);
        return 1;
    }
    return 0;
}

static int expect_path(const char *label, const char *path, sigil_platform hint,
                       const sigil_options *opts, const want_result *w) {
    sigil_result r;
    memset(&r, 0, sizeof(r));
    int rc = sigil_extract_from_path(path, hint, opts, &r);
    return check(label, rc, &r, w);
}

static int expect_io(const char *label, const char *path, const char *name,
                     sigil_platform hint, const sigil_options *opts, const want_result *w) {
    sigil_io *io = sigil_io_open_file(path);
    if (!io) { fprintf(stderr, "FAIL %s: open\n", label); return 1; }
    sigil_result r;
    memset(&r, 0, sizeof(r));
    int rc = sigil_extract_from_io(io, name, hint, opts, &r);
    sigil_io_close(io);
    return check(label, rc, &r, w);
}

/* Dumps sigil can't read a TITLE_ID from, so only their names carry an id:
 * a file of zeros and a folder with no param.sfo. With the filename fallback
 * on (opts == NULL), the name's id comes back with save_id set, the folder
 * psvita.md names. With it off, the name is never read. With no platform
 * named, a retail Vita id (PCS) comes back as Vita and any other id of the
 * shape as PSP. A dump whose param.sfo has a TITLE_ID is read the same with
 * the fallback off. */
static int test_ids_from_names(void) {
    char dir[512];
    if (test_temp_dir(dir, sizeof(dir)) != 0) {
        fprintf(stderr, "FAIL names: temp dir\n");
        return 1;
    }

    static const uint8_t zeros[2048];
    const sigil_options off = { SIGIL_OPTIONS_V1, NULL, 0 };
    const want_result vita = { SIGIL_OK, SIGIL_PLATFORM_PSVITA, "PCSE00123", "PCSE00123",
                               SIGIL_USAGE_FOLDER_EXACT, SIGIL_SOURCE_FILENAME };
    const want_result vita_g = { SIGIL_OK, SIGIL_PLATFORM_PSVITA, "PCSG00456", "PCSG00456",
                                 SIGIL_USAGE_FOLDER_EXACT, SIGIL_SOURCE_FILENAME };
    const want_result psp = { SIGIL_OK, SIGIL_PLATFORM_PSP, "ULUS10001", "ULUS10001",
                              SIGIL_USAGE_FOLDER_PREFIX, SIGIL_SOURCE_FILENAME };
    const want_result sfo = { SIGIL_OK, SIGIL_PLATFORM_PSVITA, "PCSE00123", "PCSB00456",
                              SIGIL_USAGE_FOLDER_EXACT, SIGIL_SOURCE_BINARY };
    const want_result not_found = { .rc = SIGIL_ERR_NOT_FOUND };
    const want_result unknown = { .rc = SIGIL_ERR_UNKNOWN_PLATFORM };

    char vita_bin[600], vita_g_bin[600], psp_bin[600], bare[600], bare_eboot[700];
    char sfo_dir[600], sfo_sys[700], sfo_path[800];
    snprintf(vita_bin, sizeof(vita_bin), "%s/Game [PCSE00123].bin", dir);
    snprintf(vita_g_bin, sizeof(vita_g_bin), "%s/Game (PCSG00456).bin", dir);
    snprintf(psp_bin, sizeof(psp_bin), "%s/Game [ULUS10001].bin", dir);
    snprintf(bare, sizeof(bare), "%s/Game [PCSE00123]", dir);
    snprintf(bare_eboot, sizeof(bare_eboot), "%s/eboot.bin", bare);
    snprintf(sfo_dir, sizeof(sfo_dir), "%s/Other [PCSE99999]", dir);
    snprintf(sfo_sys, sizeof(sfo_sys), "%s/sce_sys", sfo_dir);
    snprintf(sfo_path, sizeof(sfo_path), "%s/param.sfo", sfo_sys);

    const sfo_entry shared[] = {
        { "INSTALL_DIR_SAVEDATA", "PCSB00456" }, { "TITLE_ID", "PCSE00123" },
    };
    uint8_t blob[1024];
    size_t blob_len = build_sfo(blob, sizeof(blob), shared, 2);

    int fails = 0;
    if (write_file(vita_bin, zeros, sizeof(zeros)) != 0
        || write_file(vita_g_bin, zeros, sizeof(zeros)) != 0
        || write_file(psp_bin, zeros, sizeof(zeros)) != 0
        || test_make_dir(bare) != 0 || write_file(bare_eboot, zeros, sizeof(zeros)) != 0
        || test_make_dir(sfo_dir) != 0 || test_make_dir(sfo_sys) != 0
        || blob_len == 0 || write_file(sfo_path, blob, blob_len) != 0) {
        fprintf(stderr, "FAIL names: could not write fixtures\n");
        fails++;
    } else {
        fails += expect_path("file, vita named", vita_bin, SIGIL_PLATFORM_PSVITA, NULL, &vita);
        fails += expect_path("folder, vita named", bare, SIGIL_PLATFORM_PSVITA, NULL, &vita);
        fails += expect_io("io, vita named", vita_bin, "Game [PCSE00123].bin",
                           SIGIL_PLATFORM_PSVITA, NULL, &vita);
        fails += expect_path("file, nothing named", vita_bin, SIGIL_PLATFORM_AUTO, NULL, &vita);
        fails += expect_path("file, nothing named, PCSG", vita_g_bin, SIGIL_PLATFORM_AUTO,
                             NULL, &vita_g);
        fails += expect_path("psp file, nothing named", psp_bin, SIGIL_PLATFORM_AUTO, NULL, &psp);

        fails += expect_path("file, vita named, fallback off", vita_bin,
                             SIGIL_PLATFORM_PSVITA, &off, &not_found);
        fails += expect_path("folder, vita named, fallback off", bare,
                             SIGIL_PLATFORM_PSVITA, &off, &not_found);
        fails += expect_io("io, vita named, fallback off", vita_bin, "Game [PCSE00123].bin",
                           SIGIL_PLATFORM_PSVITA, &off, &not_found);
        fails += expect_path("file, nothing named, fallback off", vita_bin,
                             SIGIL_PLATFORM_AUTO, &off, &unknown);

        fails += expect_path("param.sfo folder, fallback off", sfo_dir,
                             SIGIL_PLATFORM_PSVITA, &off, &sfo);
        fails += expect_path("param.sfo, fallback off", sfo_path,
                             SIGIL_PLATFORM_PSVITA, &off, &sfo);
    }
    test_remove_tree(dir);
    return fails;
}

int main(void) {
    int fails = 0;

    const sfo_entry own[] = { { "TITLE", "Game" }, { "TITLE_ID", "PCSE00123" } };
    fails += expect(own, 2, "PCSE00123", "no INSTALL_DIR_SAVEDATA");

    const sfo_entry shared[] = {
        { "INSTALL_DIR_SAVEDATA", "PCSB00456" }, { "TITLE", "Game" }, { "TITLE_ID", "PCSE00123" },
    };
    fails += expect(shared, 3, "PCSB00456", "INSTALL_DIR_SAVEDATA names another title");

    const sfo_entry empty[] = {
        { "INSTALL_DIR_SAVEDATA", "" }, { "TITLE", "Game" }, { "TITLE_ID", "PCSE00123" },
    };
    fails += expect(empty, 3, "PCSE00123", "empty INSTALL_DIR_SAVEDATA");

    const sfo_entry oversized[] = {
        { "INSTALL_DIR_SAVEDATA", "PCSB00456PCSB00456PCSB00456PCSB00456" },
        { "TITLE_ID", "PCSE00123" },
    };
    fails += expect(oversized, 2, "PCSE00123", "INSTALL_DIR_SAVEDATA too long for save_id");

    fails += test_ids_from_names();

    if (fails == 0) fprintf(stdout, "unit_psvita: ok\n");
    return fails == 0 ? 0 : 1;
}
