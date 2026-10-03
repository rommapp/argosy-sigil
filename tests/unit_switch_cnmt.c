// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include "aes.h"
#include <stdio.h>
#include <string.h>

/* Internal symbols exported by the static lib, not declared in sigil.h. */
void sigil_aes_xts_encrypt_nintendo(const uint8_t key[32], uint64_t start_sector,
                                    uint8_t *data, size_t len);
void sigil_aes_ctr_crypt(const uint8_t key[16], uint8_t ctr[16],
                         uint8_t *buf, size_t len);

#define NCA_HEADER_SIZE  0xC00u
#define NCA_SECTION_SIZE 0x400u
#define NCA_SIZE         (NCA_HEADER_SIZE + NCA_SECTION_SIZE)

/* Section 0 spans media units 6..8 (0xC00..0x1000), i.e. right after the
 * header, matching how the extractor derives sec_start from FsEntry[0]. */
#define SEC_START_MEDIA  6u
#define SEC_END_MEDIA    8u
#define SEC_START        (SEC_START_MEDIA * 0x200u)

#define SEC_PFS0_OFF     0x20u  /* inner PFS0 sits past a fake hash region */
#define CNMT_LEN         0x10u

/* Outer NSP (PFS0) layout wrapping the single Meta NCA. */
#define OUT_HDR    16u
#define OUT_ENTRY  24u
#define OUT_STRTBL 48u
#define OUT_DATA   (OUT_HDR + OUT_ENTRY + OUT_STRTBL)
#define OUT_SIZE   (OUT_DATA + NCA_SIZE)

static const uint8_t HEADER_KEY[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};
static const uint8_t KAEK[16] = {
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
};
static const uint8_t SECTION_KEY[16] = {
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
};

#define GENERATION   0x00000002u
#define SECURE_VALUE 0x00000003u

static void write_le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void write_le64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void hex_encode(const uint8_t *src, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hex[(src[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[src[i] & 0xF];
    }
    out[n * 2] = '\0';
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

static void build_section(uint8_t sec[NCA_SECTION_SIZE]) {
    memset(sec, 0xAA, NCA_SECTION_SIZE);

    uint8_t *pfs = sec + SEC_PFS0_OFF;
    memcpy(pfs, "PFS0", 4);
    write_le32(pfs + 4, 1);   /* file_count */
    write_le32(pfs + 8, 8);   /* string_table_size */
    write_le32(pfs + 12, 0);

    uint8_t *entry = pfs + 16;
    write_le64(entry, 0);          /* file_offset */
    write_le64(entry + 8, CNMT_LEN);
    write_le32(entry + 16, 0);     /* name_offset */
    write_le32(entry + 20, 0);

    uint8_t *strtbl = entry + 24;
    memset(strtbl, 0, 8);
    memcpy(strtbl, "a.cnmt", 6);

    uint8_t *cnmt = strtbl + 8;
    /* title_id 0x0100000000010800 little-endian */
    static const uint8_t title_id[8] = { 0x00, 0x08, 0x01, 0x00,
                                         0x00, 0x00, 0x00, 0x01 };
    memcpy(cnmt, title_id, 8);
    write_le32(cnmt + 8, 0x30000); /* version */
    cnmt[0x0C] = 0x81;             /* meta_type: Patch */
    cnmt[0x0D] = 0;
    cnmt[0x0E] = 0;
    cnmt[0x0F] = 0;

    uint8_t ctr[16] = {0};
    ctr[0] = (uint8_t)(SECURE_VALUE >> 24);
    ctr[1] = (uint8_t)(SECURE_VALUE >> 16);
    ctr[2] = (uint8_t)(SECURE_VALUE >> 8);
    ctr[3] = (uint8_t)SECURE_VALUE;
    ctr[4] = (uint8_t)(GENERATION >> 24);
    ctr[5] = (uint8_t)(GENERATION >> 16);
    ctr[6] = (uint8_t)(GENERATION >> 8);
    ctr[7] = (uint8_t)GENERATION;
    uint64_t block = SEC_START >> 4;
    for (int i = 0; i < 8; i++) ctr[15 - i] = (uint8_t)(block >> (8 * i));

    sigil_aes_ctr_crypt(SECTION_KEY, ctr, sec, NCA_SECTION_SIZE);
}

static void build_nca(uint8_t nca[NCA_SIZE]) {
    memset(nca, 0, NCA_SIZE);

    memcpy(nca + 0x200, "NCA3", 4);
    nca[0x205] = 1;   /* content_type: Meta */
    nca[0x206] = 0;   /* key_generation (old) */
    nca[0x207] = 0;   /* kaek_index: application */
    nca[0x220] = 0;   /* key_generation */

    write_le32(nca + 0x240, SEC_START_MEDIA);
    write_le32(nca + 0x244, SEC_END_MEDIA);

    /* Key area: slot 2 holds the AES-CTR body key, ECB-encrypted under KAEK. */
    uint8_t key_area[0x40] = {0};
    memcpy(key_area + 0x20, SECTION_KEY, 16);
    struct AES_ctx ecb;
    AES_init_ctx(&ecb, KAEK);
    for (int i = 0; i < 4; i++) AES_ECB_encrypt(&ecb, key_area + i * 0x10);
    memcpy(nca + 0x300, key_area, sizeof(key_area));

    uint8_t *fsh = nca + 0x400;
    fsh[0x02] = 1;  /* fs_type: PartitionFs */
    fsh[0x04] = 3;  /* encryption_type: AesCtr */
    write_le32(fsh + 0x140, GENERATION);
    write_le32(fsh + 0x144, SECURE_VALUE);

    build_section(nca + NCA_HEADER_SIZE);

    sigil_aes_xts_encrypt_nintendo(HEADER_KEY, 0, nca, NCA_HEADER_SIZE);
}

static void build_image(uint8_t buf[OUT_SIZE]) {
    memset(buf, 0, OUT_SIZE);
    memcpy(buf, "PFS0", 4);
    write_le32(buf + 4, 1);
    write_le32(buf + 8, OUT_STRTBL);

    uint8_t *entry = buf + OUT_HDR;
    write_le64(entry, 0);
    write_le64(entry + 8, NCA_SIZE);
    write_le32(entry + 16, 0);

    /* A retail NCA name: its 32-hex content id, whose last 16 characters
     * start with 01 (issue #9, Radiant Silvergun). A keyless guess read them
     * as a title id. */
    memcpy(buf + OUT_HDR + OUT_ENTRY, "db705e4d8570380301cd8d0cd59fbf16.nca", 37);

    build_nca(buf + OUT_DATA);
}

/* Extracts the image with prod.keys text `keys` (NULL for none). */
static int extract_with(const uint8_t *image, const char *keys, sigil_result *r) {
    sigil_support sup;
    memset(&sup, 0, sizeof(sup));
    sup.struct_version = SIGIL_SUPPORT_V1;
    sup.switch_prod_keys_text = keys;
    sup.switch_prod_keys_text_len = keys ? strlen(keys) : 0;

    sigil_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_version = SIGIL_OPTIONS_V1;
    opts.support = keys ? &sup : NULL;

    mem_ctx ctx = { image, OUT_SIZE };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    return sigil_extract_from_io(&io, "meta.nsp", SIGIL_PLATFORM_SWITCH, &opts, r);
}

int main(void) {
    uint8_t image[OUT_SIZE];
    build_image(image);

    char kaek_hex[33], hkey_hex[65], wrong_hex[65];
    hex_encode(KAEK, 16, kaek_hex);
    hex_encode(HEADER_KEY, 32, hkey_hex);
    uint8_t wrong[32];
    for (int i = 0; i < 32; i++) wrong[i] = (uint8_t)(HEADER_KEY[i] ^ 0x5A);
    hex_encode(wrong, 32, wrong_hex);

    char keys_text[256], no_kaek[256], wrong_header[256];
    snprintf(keys_text, sizeof(keys_text), "header_key = %s\nkey_area_key_application_00 = %s\n", hkey_hex, kaek_hex);
    snprintf(no_kaek, sizeof(no_kaek), "header_key = %s\n", hkey_hex);
    snprintf(wrong_header, sizeof(wrong_header), "header_key = %s\nkey_area_key_application_00 = %s\n", wrong_hex,
             kaek_hex);

    /* prod.keys is required: no title id is guessed without it, from the
     * NCA names or anywhere else. */
    sigil_result r;
    int rc = extract_with(image, NULL, &r);
    if (rc != SIGIL_ERR_NEEDS_KEY) {
        fprintf(stderr, "FAIL: no keys gave rc=%d title_id='%s', want NEEDS_KEY\n", rc, r.title_id);
        return 1;
    }
    /* Keys that can't open the content are a key file mismatch. */
    if ((rc = extract_with(image, no_kaek, &r)) != SIGIL_ERR_KEYS_INCOMPATIBLE) {
        fprintf(stderr, "FAIL: a key file without the generation's key gave rc=%d\n", rc);
        return 1;
    }
    if ((rc = extract_with(image, wrong_header, &r)) != SIGIL_ERR_KEYS_INCOMPATIBLE) {
        fprintf(stderr, "FAIL: a wrong header key gave rc=%d\n", rc);
        return 1;
    }

    rc = extract_with(image, keys_text, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL: rc=%d (%s)\n", rc, sigil_strerror(rc));
        return 1;
    }
    if (strcmp(r.title_id, "0100000000010800") != 0) {
        fprintf(stderr, "FAIL: title_id='%s'\n", r.title_id);
        return 1;
    }
    if (r.switch_content_type != SIGIL_SWITCH_CONTENT_PATCH) {
        fprintf(stderr, "FAIL: content_type=%d\n", r.switch_content_type);
        return 1;
    }
    if (r.title_version != 0x30000) {
        fprintf(stderr, "FAIL: title_version=%u\n", r.title_version);
        return 1;
    }
    if (r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "FAIL: source=%d\n", (int)r.source);
        return 1;
    }

    printf("ok unit_switch_cnmt\n");
    return 0;
}
