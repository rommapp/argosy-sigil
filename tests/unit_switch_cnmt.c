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
#define CNMT_LEN         0x30u  /* header, then an 0x10-byte extended header */

/* Outer NSP (PFS0) layout wrapping up to two Meta NCAs. */
#define MAX_NCAS   2u
#define OUT_HDR    16u
#define OUT_ENTRY  24u
#define OUT_STRTBL 96u
#define OUT_DATA   (OUT_HDR + OUT_ENTRY * MAX_NCAS + OUT_STRTBL)
#define OUT_SIZE   (OUT_DATA + NCA_SIZE * MAX_NCAS)

#define META_APPLICATION 0x80u
#define META_PATCH       0x81u
#define META_ADDON       0x82u

#define BASE_ID  0x0100000000010000ull
#define PATCH_ID 0x0100000000010800ull
#define ADDON_ID 0x0100000000011001ull

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

/* What one Meta NCA's CNMT says. `application_id` 0 leaves the extended
 * header out, as a CNMT whose extended header size is 0. */
typedef struct {
    uint8_t  meta_type;
    uint64_t id;
    uint32_t version;
    uint64_t application_id;
} meta;

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static void write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
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

static void build_section(uint8_t sec[NCA_SECTION_SIZE], const meta *m) {
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
    memset(cnmt, 0, CNMT_LEN);
    write_le64(cnmt, m->id);
    write_le32(cnmt + 0x08, m->version);
    cnmt[0x0C] = m->meta_type;
    if (m->application_id) {
        write_le16(cnmt + 0x0E, 0x10);   /* extended header size */
        write_le64(cnmt + 0x20, m->application_id);
    }

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

static void build_nca(uint8_t nca[NCA_SIZE], const meta *m) {
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

    build_section(nca + NCA_HEADER_SIZE, m);

    sigil_aes_xts_encrypt_nintendo(HEADER_KEY, 0, nca, NCA_HEADER_SIZE);
}

/* An NSP holding one Meta NCA per entry of `metas`, in that order. */
static void build_image(uint8_t buf[OUT_SIZE], const meta *metas, uint32_t count) {
    static const char *const NAMES[MAX_NCAS] = {
        /* A retail NCA name: its 32-hex content id, whose last 16 characters
         * start with 01 (issue #9, Radiant Silvergun). A keyless guess read
         * them as a title id. */
        "db705e4d8570380301cd8d0cd59fbf16.nca",
        "5c2b1a9e0f3d47e6a8b0c1d2e3f40516.nca",
    };
    memset(buf, 0, OUT_SIZE);
    memcpy(buf, "PFS0", 4);
    write_le32(buf + 4, count);
    write_le32(buf + 8, OUT_STRTBL);
    uint32_t name_at = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *entry = buf + OUT_HDR + i * OUT_ENTRY;
        write_le64(entry, (uint64_t)i * NCA_SIZE);
        write_le64(entry + 8, NCA_SIZE);
        write_le32(entry + 16, name_at);
        memcpy(buf + OUT_HDR + count * OUT_ENTRY + name_at, NAMES[i], strlen(NAMES[i]) + 1);
        name_at += (uint32_t)strlen(NAMES[i]) + 1;
    }
    /* The string table sits after `count` entries; data after the table. */
    size_t data = OUT_HDR + count * OUT_ENTRY + OUT_STRTBL;
    for (uint32_t i = 0; i < count; i++) build_nca(buf + data + (size_t)i * NCA_SIZE, &metas[i]);
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

static char g_keys[256];

/* An NSP of `metas` reads as the game `title_id`, with the content's own id,
 * type and version kept beside it. */
static void expect(const char *where, const meta *metas, uint32_t count, const char *title_id, const char *raw_serial,
                   int content_type, uint32_t version) {
    static uint8_t image[OUT_SIZE];
    build_image(image, metas, count);
    sigil_result r;
    memset(&r, 0, sizeof(r));
    int rc = extract_with(image, g_keys, &r);
    char what[160];
    if (rc != SIGIL_OK) {
        snprintf(what, sizeof(what), "rc=%d (%s)", rc, sigil_strerror(rc));
        fail(where, what);
        return;
    }
    if (strcmp(r.title_id, title_id) != 0 || strcmp(r.save_id, title_id) != 0) {
        snprintf(what, sizeof(what), "title_id '%s' save_id '%s', want %s", r.title_id, r.save_id, title_id);
        fail(where, what);
    }
    if (strcmp(r.raw_serial, raw_serial) != 0) {
        snprintf(what, sizeof(what), "raw_serial '%s', want %s", r.raw_serial, raw_serial);
        fail(where, what);
    }
    if (r.switch_content_type != content_type) fail(where, "content type");
    if (r.title_version != version) fail(where, "version");
    if (r.source != SIGIL_SOURCE_BINARY) fail(where, "source");
}

int main(void) {
    char kaek_hex[33], hkey_hex[65], wrong_hex[65];
    hex_encode(KAEK, 16, kaek_hex);
    hex_encode(HEADER_KEY, 32, hkey_hex);
    uint8_t wrong[32];
    for (int i = 0; i < 32; i++) wrong[i] = (uint8_t)(HEADER_KEY[i] ^ 0x5A);
    hex_encode(wrong, 32, wrong_hex);

    char no_kaek[256], wrong_header[256];
    snprintf(g_keys, sizeof(g_keys), "header_key = %s\nkey_area_key_application_00 = %s\n", hkey_hex, kaek_hex);
    snprintf(no_kaek, sizeof(no_kaek), "header_key = %s\n", hkey_hex);
    snprintf(wrong_header, sizeof(wrong_header), "header_key = %s\nkey_area_key_application_00 = %s\n", wrong_hex,
             kaek_hex);

    const meta patch = { META_PATCH, PATCH_ID, 0x30000, BASE_ID };
    static uint8_t image[OUT_SIZE];
    build_image(image, &patch, 1);

    /* prod.keys is required: no title id is guessed without it, from the
     * NCA names or anywhere else. */
    sigil_result r;
    int rc = extract_with(image, NULL, &r);
    if (rc != SIGIL_ERR_NEEDS_KEY) fail("no keys", "want NEEDS_KEY");
    /* Keys that can't open the content are a key file mismatch. */
    if (extract_with(image, no_kaek, &r) != SIGIL_ERR_KEYS_INCOMPATIBLE) fail("no kaek", "want KEYS_INCOMPATIBLE");
    if (extract_with(image, wrong_header, &r) != SIGIL_ERR_KEYS_INCOMPATIBLE) {
        fail("wrong header key", "want KEYS_INCOMPATIBLE");
    }

    /* Saves are kept under the base game's id, so an update or a DLC reads as
     * that game; its own id stays in raw_serial. */
    expect("patch", &patch, 1, "0100000000010000", "0100000000010800", SIGIL_SWITCH_CONTENT_PATCH, 0x30000);
    const meta bare_patch = { META_PATCH, PATCH_ID, 0x30000, 0 };
    expect("patch without extended header", &bare_patch, 1, "0100000000010000", "0100000000010800",
           SIGIL_SWITCH_CONTENT_PATCH, 0x30000);
    const meta addon = { META_ADDON, ADDON_ID, 0x10000, BASE_ID };
    expect("addon", &addon, 1, "0100000000010000", "0100000000011001", SIGIL_SWITCH_CONTENT_ADDON, 0x10000);
    const meta bare_addon = { META_ADDON, ADDON_ID, 0x10000, 0 };
    expect("addon without extended header", &bare_addon, 1, "0100000000010000", "0100000000011001",
           SIGIL_SWITCH_CONTENT_ADDON, 0x10000);
    /* The CNMT's own word wins over the id arithmetic. */
    const meta elsewhere = { META_ADDON, ADDON_ID, 0x10000, 0x0100000000050000ull };
    expect("addon naming its game", &elsewhere, 1, "0100000000050000", "0100000000011001", SIGIL_SWITCH_CONTENT_ADDON,
           0x10000);
    const meta app = { META_APPLICATION, BASE_ID, 0, BASE_ID | 0x800 };
    expect("application", &app, 1, "0100000000010000", "0100000000010000", SIGIL_SWITCH_CONTENT_APPLICATION, 0);

    /* A dump holding the game and its update reads as the game, whichever
     * Meta NCA comes first. */
    const meta merged[2] = { patch, app };
    expect("patch before application", merged, 2, "0100000000010000", "0100000000010000",
           SIGIL_SWITCH_CONTENT_APPLICATION, 0);

    if (g_fails) return 1;
    printf("ok unit_switch_cnmt\n");
    return 0;
}
