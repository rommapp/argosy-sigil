// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include "sigil_internal.h"
#include "test_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SUPERBLOCK  0x80u
#define TOC_HEADER  0x30u
#define PACKAGE     0x40u

typedef struct {
    uint64_t title_id;
    uint32_t version;
    uint8_t  kind;
    uint8_t  flags;
} package;

static int failures = 0;

static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) {
    put_le16(p, (uint16_t)v);
    put_le16(p + 2, (uint16_t)(v >> 16));
}
static void put_le64(uint8_t *p, uint64_t v) {
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static int write_nup_raw(const char *path, const package *pkgs, uint32_t count,
                         uint32_t superblock_flags, bool tamper_toc) {
    uint32_t toc_size = TOC_HEADER + count * PACKAGE;
    uint32_t file_size = SUPERBLOCK + toc_size;
    uint8_t *buf = calloc(1, file_size);
    if (!buf) return -1;

    memcpy(buf, "NUP\x01", 4);
    put_le16(buf + 0x04, SUPERBLOCK);
    put_le16(buf + 0x06, 1);
    put_le64(buf + 0x08, SUPERBLOCK);
    put_le64(buf + 0x10, toc_size);
    put_le64(buf + 0x38, file_size);
    put_le64(buf + 0x40, file_size);
    put_le32(buf + 0x48, count);
    put_le32(buf + 0x4C, superblock_flags);

    uint8_t *toc = buf + SUPERBLOCK;
    memcpy(toc, "TOC\x01", 4);
    put_le32(toc + 0x04, count);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *rec = toc + TOC_HEADER + i * PACKAGE;
        put_le64(rec, pkgs[i].title_id);
        put_le32(rec + 0x0C, 0xFFFFFFFFu);
        put_le32(rec + 0x10, 0xFFFFFFFFu);
        put_le32(rec + 0x14, pkgs[i].version);
        rec[0x18] = pkgs[i].kind;
        rec[0x19] = pkgs[i].flags;
    }

    sigil_sha256 sha;
    sigil_sha256_init(&sha);
    sigil_sha256_update(&sha, toc, toc_size);
    sigil_sha256_final(&sha, buf + 0x18);
    if (tamper_toc) toc[0x2F] ^= 0x01;

    FILE *fp = fopen(path, "wb");
    if (!fp) { free(buf); return -1; }
    size_t written = fwrite(buf, 1, file_size, fp);
    fclose(fp);
    free(buf);
    return written == file_size ? 0 : -1;
}

static int write_nup(const char *path, const package *pkgs, uint32_t count, uint32_t superblock_flags) {
    return write_nup_raw(path, pkgs, count, superblock_flags, false);
}

static void expect_sha256(const char *label, const char *input, const char *hex) {
    sigil_sha256 sha;
    sigil_sha256_init(&sha);
    sigil_sha256_update(&sha, input, strlen(input));
    uint8_t digest[32];
    sigil_sha256_final(&sha, digest);
    char got[65];
    for (int i = 0; i < 32; i++) snprintf(got + i * 2, 3, "%02x", digest[i]);
    if (strcmp(got, hex) != 0) {
        fprintf(stderr, "FAIL %s: sha256=%s\n", label, got);
        failures++;
    }
}

static void expect_tampered_toc_refused(void) {
    const package base[] = { { 0x040074A01BF12000ull, 0x10000, 0, 0 } };
    char path[512];
    snprintf(path, sizeof(path), "%s/sigil_unit_nup_tampered.nup", test_temp_root());
    if (write_nup_raw(path, base, 1, 0, true) != 0) {
        fprintf(stderr, "FAIL tampered toc: could not write fixture\n");
        failures++;
        return;
    }
    sigil_result r;
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_SWITCH, NULL, &r);
    test_remove_file(path);
    if (rc == SIGIL_OK) {
        fprintf(stderr, "FAIL tampered toc: read %s past a digest mismatch\n", r.title_id);
        failures++;
    }
}

static void expect(const char *label, const package *pkgs, uint32_t count,
                   const char *title_id, const char *raw_serial, int content_type, uint32_t version) {
    char path[512];
    snprintf(path, sizeof(path), "%s/sigil_unit_nup.nup", test_temp_root());
    if (write_nup(path, pkgs, count, 0) != 0) {
        fprintf(stderr, "FAIL %s: could not write fixture\n", label);
        failures++;
        return;
    }
    sigil_result r;
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_AUTO, NULL, &r);
    test_remove_file(path);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL %s: %s\n", label, sigil_strerror(rc));
        failures++;
        return;
    }
    if (strcmp(r.title_id, title_id) != 0 || strcmp(r.raw_serial, raw_serial) != 0
        || r.switch_content_type != content_type || r.title_version != version
        || r.source != SIGIL_SOURCE_BINARY || r.platform != SIGIL_PLATFORM_SWITCH) {
        fprintf(stderr, "FAIL %s: title_id=%s raw_serial=%s type=%d version=%u source=%d platform=%d\n",
                label, r.title_id, r.raw_serial, r.switch_content_type, r.title_version,
                (int)r.source, (int)r.platform);
        failures++;
    }
}

static void expect_refused(const char *label, const package *pkgs, uint32_t count, uint32_t flags) {
    char path[512];
    snprintf(path, sizeof(path), "%s/sigil_unit_nup_bad.nup", test_temp_root());
    if (write_nup(path, pkgs, count, flags) != 0) {
        fprintf(stderr, "FAIL %s: could not write fixture\n", label);
        failures++;
        return;
    }
    sigil_result r;
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_SWITCH, NULL, &r);
    test_remove_file(path);
    if (rc == SIGIL_OK) {
        fprintf(stderr, "FAIL %s: read %s from a container it should refuse\n", label, r.title_id);
        failures++;
    }
}

int main(void) {
    expect_sha256("sha256 abc", "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expect_sha256("sha256 two blocks", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    expect_tampered_toc_refused();

    const package base_and_update[] = {
        { 0x040074A01BF12000ull, 0x10000, 0, 0 },
        { 0x040074A01BF12000ull, 0x20000, 1, 0 },
    };
    expect("base beside its update", base_and_update, 2,
           "040074A01BF12000", "040074A01BF12000", SIGIL_SWITCH_CONTENT_APPLICATION, 0x10000);

    const package update_first[] = {
        { 0x040074A01BF12000ull, 0x20000, 1, 0 },
        { 0x040074A01BF12000ull, 0x10000, 0, 0 },
    };
    expect("base listed after its update", update_first, 2,
           "040074A01BF12000", "040074A01BF12000", SIGIL_SWITCH_CONTENT_APPLICATION, 0x10000);

    const package standalone_update[] = {
        { 0x0400C3F00006E000ull, 0xE0000, 1, 1 },
    };
    expect("standalone update", standalone_update, 1,
           "0400C3F00006E000", "0400C3F00006E000", SIGIL_SWITCH_CONTENT_PATCH, 0xE0000);

    const package empty_base[] = {
        { 0, 0, 0, 0 },
        { 0x0400A3A01EE4C000ull, 0x50000, 1, 0 },
    };
    expect("base record without a title id", empty_base, 2,
           "0400A3A01EE4C000", "0400A3A01EE4C000", SIGIL_SWITCH_CONTENT_PATCH, 0x50000);

    const package addon_only[] = {
        { 0x0400A3A01EE4D001ull, 0x10000, 2, 0 },
    };
    expect("add-on alone", addon_only, 1,
           "0400A3A01EE4C000", "0400A3A01EE4D001", SIGIL_SWITCH_CONTENT_ADDON, 0x10000);

    const package reserved_kind[] = {
        { 0x040074A01BF12000ull, 0x10000, 5, 0 },
    };
    expect_refused("reserved package kind", reserved_kind, 1, 0);
    expect_refused("unknown superblock flags", base_and_update, 2, 1);

    const package no_ids[] = {
        { 0, 0, 0, 0 },
    };
    expect_refused("no package with a title id", no_ids, 1, 0);

    if (failures) return 1;
    printf("unit_nup: ok\n");
    return 0;
}
