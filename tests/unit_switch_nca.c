// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Internal, exported by the static lib but not declared in sigil.h. */
void sigil_aes_xts_encrypt_nintendo(const uint8_t key[32], uint64_t start_sector,
                                    uint8_t *data, size_t len);

#define NCA_HEADER_SIZE   0xC00u
#define STRING_TABLE_SIZE 16u
#define DATA_START        (16u + 24u + STRING_TABLE_SIZE)
#define IMAGE_SIZE        (DATA_START + NCA_HEADER_SIZE)

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void write_le64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
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

/* Minimal single-file NSP: PFS0 header, one entry, "game.nca", then a
 * plaintext NCA header carrying title 0100000000010000. */
static void build_image(uint8_t *buf, const char *magic) {
    memset(buf, 0, IMAGE_SIZE);
    memcpy(buf, "PFS0", 4);
    write_le32(buf + 4, 1);                 /* file_count */
    write_le32(buf + 8, STRING_TABLE_SIZE);

    uint8_t *entry = buf + 16;
    write_le64(entry, 0);                   /* file_offset */
    write_le64(entry + 8, NCA_HEADER_SIZE); /* file_size */
    write_le32(entry + 16, 0);              /* name_offset */

    memcpy(buf + 16 + 24, "game.nca", 9);

    uint8_t *nca = buf + DATA_START;
    memcpy(nca + 0x200, magic, 4);
    /* program_id 0x0100000000010000, stored little-endian */
    static const uint8_t program_id[8] = { 0x00, 0x00, 0x01, 0x00,
                                           0x00, 0x00, 0x00, 0x01 };
    memcpy(nca + 0x210, program_id, 8);
}

static int run_case(const char *label, const uint8_t *buf,
                    const sigil_options *opts, int want_rc) {
    mem_ctx ctx = { buf, IMAGE_SIZE };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, "game.nsp", SIGIL_PLATFORM_SWITCH,
                                   opts, &r);
    if (rc != want_rc) {
        fprintf(stderr, "FAIL %s: rc=%d want %d\n", label, rc, want_rc);
        return 1;
    }
    if (want_rc != SIGIL_OK) return 0;
    if (strcmp(r.title_id, "0100000000010000") != 0) {
        fprintf(stderr, "FAIL %s: title_id '%s'\n", label, r.title_id);
        return 1;
    }
    if (r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "FAIL %s: source=%d\n", label, (int)r.source);
        return 1;
    }
    return 0;
}

int main(void) {
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;

    sigil_support sup;
    memset(&sup, 0, sizeof(sup));
    sup.struct_version = SIGIL_SUPPORT_V1;
    sup.switch_header_key = key;

    sigil_options no_key;
    memset(&no_key, 0, sizeof(no_key));
    no_key.struct_version = SIGIL_OPTIONS_V1;

    sigil_options with_key = no_key;
    with_key.support = &sup;

    uint8_t plain[IMAGE_SIZE];
    build_image(plain, "NCA3");

    /* prod.keys is required, even for a plaintext header. */
    if (run_case("plaintext no key", plain, &no_key, SIGIL_ERR_NEEDS_KEY)) return 1;

    /* A supplied key must not scramble an already-plaintext header. */
    if (run_case("plaintext with key", plain, &with_key, SIGIL_OK)) return 1;

    uint8_t enc[IMAGE_SIZE];
    memcpy(enc, plain, IMAGE_SIZE);
    sigil_aes_xts_encrypt_nintendo(key, 0, enc + DATA_START, NCA_HEADER_SIZE);

    if (run_case("encrypted with key", enc, &with_key, SIGIL_OK)) return 1;
    if (run_case("encrypted no key", enc, &no_key, SIGIL_ERR_NEEDS_KEY)) return 1;

    uint8_t nca2[IMAGE_SIZE];
    build_image(nca2, "NCA2");
    if (run_case("NCA2 plaintext", nca2, &with_key, SIGIL_OK)) return 1;
    if (run_case("NCA2 plaintext no key", nca2, &no_key, SIGIL_ERR_NEEDS_KEY)) return 1;

    /* An encrypted header the key given doesn't open is a key mismatch. */
    uint8_t other[32];
    for (int i = 0; i < 32; i++) other[i] = (uint8_t)(key[i] ^ 0xA5);
    sigil_support wrong_sup = sup;
    wrong_sup.switch_header_key = other;
    sigil_options wrong_key = no_key;
    wrong_key.support = &wrong_sup;
    if (run_case("encrypted wrong key", enc, &wrong_key, SIGIL_ERR_KEYS_INCOMPATIBLE)) return 1;

    printf("ok unit_switch_nca\n");
    return 0;
}
