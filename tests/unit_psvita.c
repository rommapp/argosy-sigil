// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
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

    if (fails == 0) fprintf(stdout, "unit_psvita: ok\n");
    return fails == 0 ? 0 : 1;
}
