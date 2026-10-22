// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include "test_fs.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define SECTOR        2048u
#define SECTORS       20u
#define PVD_LBA       16u
#define ROOT_LBA      18u
#define CNF_LBA       19u
#define HEADER_SIZE   24u
#define RAW_FLAG      0x80000000u
#define EXPECTED_ID   "SLUS-20035"

typedef size_t (*block_encoder)(const uint8_t *src, uint8_t *dst, size_t cap);

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

static void dir_record(uint8_t *p, uint32_t lba, uint32_t len, const char *name) {
    size_t name_len = strlen(name);
    p[0] = (uint8_t)(33 + name_len + ((33 + name_len) & 1));
    put_le32(p + 2, lba);
    put_le32(p + 10, len);
    p[32] = (uint8_t)name_len;
    memcpy(p + 33, name, name_len);
}

static uint8_t *build_ps2_iso(void) {
    uint8_t *iso = calloc(SECTORS, SECTOR);
    if (!iso) return NULL;
    const char *cnf = "BOOT2 = cdrom0:\\SLUS_200.35;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";
    uint8_t *pvd = iso + PVD_LBA * SECTOR;
    pvd[0] = 0x01;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 0x01;
    dir_record(pvd + 156, ROOT_LBA, SECTOR, "\0");
    dir_record(iso + ROOT_LBA * SECTOR, CNF_LBA, (uint32_t)strlen(cnf), "SYSTEM.CNF;1");
    memcpy(iso + CNF_LBA * SECTOR, cnf, strlen(cnf));
    return iso;
}

static size_t lz4_put_length(uint8_t *dst, size_t len) {
    size_t n = 0;
    for (len -= 15; len >= 255; len -= 255) dst[n++] = 255;
    dst[n++] = (uint8_t)len;
    return n;
}

static size_t lz4_sequence(uint8_t *dst, const uint8_t *lits, size_t lit_len, size_t match_len) {
    size_t n = 1;
    uint8_t token = (uint8_t)((lit_len >= 15 ? 15 : lit_len) << 4);
    if (lit_len >= 15) n += lz4_put_length(dst + n, lit_len);
    memcpy(dst + n, lits, lit_len);
    n += lit_len;
    if (match_len > 0) {
        size_t code = match_len - 4;
        token |= (uint8_t)(code >= 15 ? 15 : code);
        put_le16(dst + n, 1);
        n += 2;
        if (code >= 15) n += lz4_put_length(dst + n, code);
    }
    dst[0] = token;
    return n;
}

static size_t encode_lz4_runs(const uint8_t *src, uint8_t *dst, size_t cap) {
    (void)cap;
    size_t out = 0, lit_start = 0, i = 0;
    while (i + 12 < SECTOR) {
        size_t run = 0;
        while (i + 1 + run < SECTOR - 5 && src[i + 1 + run] == src[i]) run++;
        if (run >= 4) {
            out += lz4_sequence(dst + out, src + lit_start, i + 1 - lit_start, run);
            i += 1 + run;
            lit_start = i;
        } else {
            i++;
        }
    }
    out += lz4_sequence(dst + out, src + lit_start, SECTOR - lit_start, 0);
    return out;
}

static size_t encode_deflate(const uint8_t *src, uint8_t *dst, size_t cap) {
    z_stream strm = {0};
    if (deflateInit2(&strm, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return 0;
    strm.next_in = (Bytef *)src;
    strm.avail_in = SECTOR;
    strm.next_out = dst;
    strm.avail_out = (uInt)cap;
    int rc = deflate(&strm, Z_FINISH);
    size_t got = strm.total_out;
    deflateEnd(&strm);
    return rc == Z_STREAM_END ? got : 0;
}

static size_t lz4_first_offset_at(const uint8_t *block) {
    size_t pos = 1, lit = block[0] >> 4;
    if (lit == 15) {
        uint8_t b;
        do { b = block[pos++]; lit += b; } while (b == 255);
    }
    return pos + lit;
}

static int write_container(const char *path, const char *magic, const uint8_t *iso,
                           block_encoder encode, uint32_t corrupt_block) {
    FILE *fp = test_fopen(path, "wb");
    if (!fp) return -1;
    uint8_t hdr[HEADER_SIZE] = {0};
    memcpy(hdr, magic, 4);
    put_le32(hdr + 4, HEADER_SIZE);
    put_le64(hdr + 8, (uint64_t)SECTORS * SECTOR);
    put_le32(hdr + 16, SECTOR);
    hdr[20] = 1;
    fwrite(hdr, 1, sizeof(hdr), fp);

    uint8_t index[(SECTORS + 1) * 4];
    long index_at = ftell(fp);
    fwrite(index, 1, sizeof(index), fp);

    uint8_t block[SECTOR * 2];
    for (uint32_t b = 0; b < SECTORS; b++) {
        uint32_t at = (uint32_t)ftell(fp);
        const uint8_t *src = iso + (size_t)b * SECTOR;
        size_t len;
        if (b == PVD_LBA) {
            memcpy(block, src, SECTOR);
            len = SECTOR;
            at |= RAW_FLAG;
        } else {
            len = encode(src, block, sizeof(block));
            if (len == 0) { fclose(fp); return -1; }
            if (b == corrupt_block) put_le16(block + lz4_first_offset_at(block), 0xFFFF);
        }
        put_le32(index + b * 4, at);
        fwrite(block, 1, len, fp);
    }
    put_le32(index + SECTORS * 4, (uint32_t)ftell(fp));
    fseek(fp, index_at, SEEK_SET);
    fwrite(index, 1, sizeof(index), fp);
    fclose(fp);
    return 0;
}

static void expect_id(const char *label, const char *path, sigil_platform hint) {
    sigil_result r;
    int rc = sigil_extract_from_path(path, hint, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL %s: %s\n", label, sigil_strerror(rc));
        failures++;
        return;
    }
    if (strcmp(r.title_id, EXPECTED_ID) != 0 || r.source != SIGIL_SOURCE_BINARY
        || r.platform != SIGIL_PLATFORM_PS2) {
        fprintf(stderr, "FAIL %s: title_id=%s source=%d platform=%d\n",
                label, r.title_id, (int)r.source, (int)r.platform);
        failures++;
    }
}

static void expect_none(const char *label, const char *path, sigil_platform hint) {
    sigil_result r;
    int rc = sigil_extract_from_path(path, hint, NULL, &r);
    if (rc == SIGIL_OK) {
        fprintf(stderr, "FAIL %s: read %s from a damaged block\n", label, r.title_id);
        failures++;
    }
}

int main(void) {
    uint8_t *iso = build_ps2_iso();
    if (!iso) return 1;

    char cso[512], zso[512], bad_zso[512];
    snprintf(cso, sizeof(cso), "%s/sigil_unit_ps2.cso", test_temp_root());
    snprintf(zso, sizeof(zso), "%s/sigil_unit_ps2.zso", test_temp_root());
    snprintf(bad_zso, sizeof(bad_zso), "%s/sigil_unit_ps2_bad.zso", test_temp_root());

    if (write_container(cso, "CISO", iso, encode_deflate, UINT32_MAX) != 0
        || write_container(zso, "ZISO", iso, encode_lz4_runs, UINT32_MAX) != 0
        || write_container(bad_zso, "ZISO", iso, encode_lz4_runs, ROOT_LBA) != 0) {
        fprintf(stderr, "could not write fixtures\n");
        free(iso);
        return 1;
    }

    expect_id("ps2 cso", cso, SIGIL_PLATFORM_PS2);
    expect_id("ps2 zso", zso, SIGIL_PLATFORM_PS2);
    expect_id("zso sniffed", zso, SIGIL_PLATFORM_AUTO);
    expect_none("zso match before its output", bad_zso, SIGIL_PLATFORM_PS2);

    test_remove_file(cso);
    test_remove_file(zso);
    test_remove_file(bad_zso);
    free(iso);
    if (failures) return 1;
    printf("unit_cso: ok\n");
    return 0;
}
