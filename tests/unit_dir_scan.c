// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include "test_fs.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* A param.sfo holding one utf8 entry, TITLE_ID. */
static size_t build_sfo(uint8_t *buf, const char *title_id) {
    const char *key = "TITLE_ID";
    size_t key_table = 20 + 16;
    size_t key_size = strlen(key) + 1;
    while (key_size % 4 != 0) key_size++;
    size_t data_table = key_table + key_size;
    size_t len = strlen(title_id) + 1;
    memset(buf, 0, data_table + len);
    buf[1] = 'P'; buf[2] = 'S'; buf[3] = 'F';
    write_le32(buf + 4, 0x00000101);
    write_le32(buf + 8, (uint32_t)key_table);
    write_le32(buf + 12, (uint32_t)data_table);
    write_le32(buf + 16, 1);
    uint8_t *x = buf + 20;
    x[2] = 0x04; x[3] = 0x02;
    write_le32(x + 4, (uint32_t)len);
    write_le32(x + 8, (uint32_t)len);
    memcpy(buf + key_table, key, strlen(key) + 1);
    memcpy(buf + data_table, title_id, len);
    return data_table + len;
}

static void make_dirs(const char *root, const char *rel) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    for (char *p = path + strlen(root) + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        test_make_dir(path);
        *p = '/';
    }
    test_make_dir(path);
}

static void put_sfo(const char *root, const char *dir, const char *name, const char *title_id) {
    make_dirs(root, dir);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/%s", root, dir, name);
    uint8_t buf[256];
    size_t n = build_sfo(buf, title_id);
    FILE *f = fopen(path, "wb");
    if (!f) { fail(path, "could not write"); return; }
    fwrite(buf, 1, n, f);
    fclose(f);
}

/* Each folder holds the title's own SFO and deeper ones that describe
 * something else (a save, an update); the shallowest names the title. Decoys
 * sit in folders that sort before the title's own and in several branches,
 * so a depth-first walk meets one first. Two more sit as deep as the title's
 * own, under names that sort after it: the smaller path wins a tie. A folder
 * at the top carries the file's name and is not the file. */
static void check(const char *label, sigil_platform platform, const char *name, const char *own_dir,
                  const char *own_id) {
    char root[1024];
    if (test_temp_dir(root, sizeof(root)) != 0) { fail(label, "no temporary folder"); return; }
    const char *decoys[] = { "a/savedata/AAAA00001", "b/savedata/BBBB00002", "0/x", "PS3_GAME/USRDIR/TROPDIR/a",
                             "zz", "zy" };
    for (size_t i = 0; i < sizeof(decoys) / sizeof(decoys[0]); i++) put_sfo(root, decoys[i], name, "ZZZZ99999");
    make_dirs(root, name);
    put_sfo(root, own_dir, name, own_id);

    sigil_result r;
    int rc = sigil_extract_from_path(root, platform, NULL, &r);
    if (rc != SIGIL_OK || strcmp(r.title_id, own_id) != 0 || r.source != SIGIL_SOURCE_BINARY) {
        char msg[128];
        snprintf(msg, sizeof(msg), "rc=%d title_id='%s' (want '%s')", rc, rc == SIGIL_OK ? r.title_id : "", own_id);
        fail(label, msg);
    }
    test_remove_tree(root);
}

int main(void) {
    check("vita folder", SIGIL_PLATFORM_PSVITA, "param.sfo", "sce_sys", "PCSE00123");
    check("ps3 folder", SIGIL_PLATFORM_PS3, "PARAM.SFO", "PS3_GAME", "BLUS30443");
    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("unit_dir_scan: ok\n");
    return 0;
}
