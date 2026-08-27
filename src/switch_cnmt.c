// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

#include "aes.h"
#include <stdio.h>
#include <stdlib.h>

/* Meta NCA section 0 is a PartitionFs wrapped in HierarchicalSha256. Rather
 * than parse the hash-layer offsets we decrypt from the section start and
 * scan the plaintext for the inner PFS0, then read its single .cnmt entry.
 * Content-meta header (packaged): title_id u64 @0x0, version u32 @0x8,
 * meta_type u8 @0xC. */

#define NCA_KEY_AREA_OFFSET   0x300
#define NCA_FS_ENTRY0_OFFSET  0x240
#define NCA_FS_HEADER0_OFFSET 0x400
#define NCA_MEDIA_UNIT        0x200

#define FS_HEADER_TYPE_OFFSET  0x02
#define FS_HEADER_ENC_OFFSET   0x04
#define FS_HEADER_GEN_OFFSET   0x140
#define FS_HEADER_SECURE_OFFSET 0x144

#define NCA_ENC_TYPE_AES_CTR   3

#define CNMT_META_APPLICATION  0x80
#define CNMT_META_PATCH        0x81
#define CNMT_META_ADDON        0x82

/* Cap the section read; content-meta sections are a few KB and the PFS0
 * sits within the first hash-layer region regardless of total section size. */
#define CNMT_MAX_SECTION (16u * 1024 * 1024)

void sigil_aes_ctr_crypt(const uint8_t key[16], uint8_t ctr[16],
                         uint8_t *buf, size_t len) {
    struct AES_ctx c;
    AES_init_ctx_iv(&c, key, ctr);
    AES_CTR_xcrypt_buffer(&c, buf, len);
}

static int load_kaek(const sigil_support *sup, unsigned gen_index,
                     uint8_t out[16]) {
    char name[48];
    snprintf(name, sizeof(name), "key_area_key_application_%02x", gen_index);
    if (sup->switch_prod_keys_text && sup->switch_prod_keys_text_len > 0) {
        return sigil_decode_key16_from_text(sup->switch_prod_keys_text,
                                            sup->switch_prod_keys_text_len,
                                            name, out);
    }
    if (sup->switch_prod_keys_path) {
        return sigil_load_key16_from_prod_keys(sup->switch_prod_keys_path,
                                               name, out);
    }
    return SIGIL_ERR_NEEDS_KEY;
}

static int content_type_from_meta(uint8_t meta_type) {
    switch (meta_type) {
    case CNMT_META_APPLICATION: return SIGIL_SWITCH_CONTENT_APPLICATION;
    case CNMT_META_PATCH:       return SIGIL_SWITCH_CONTENT_PATCH;
    case CNMT_META_ADDON:       return SIGIL_SWITCH_CONTENT_ADDON;
    default:                    return SIGIL_SWITCH_CONTENT_UNKNOWN;
    }
}

/* Read the content-meta header from a .cnmt entry inside an in-memory PFS0. */
static int parse_inner_pfs0(const uint8_t *buf, size_t len,
                            sigil_switch_title *out) {
    sigil_pfs0_layout lay;
    if (sigil_pfs0_parse_header(buf, &lay) != SIGIL_OK) return SIGIL_ERR_NOT_FOUND;
    if (lay.data_off > len) return SIGIL_ERR_NOT_FOUND;

    const uint8_t *entries = buf + lay.entries_off;
    const uint8_t *strings = buf + lay.string_table_off;

    for (uint32_t i = 0; i < lay.file_count; i++) {
        const uint8_t *e = entries + (size_t)i * 24;
        uint64_t file_offset = sigil_read_le64(e);
        uint64_t file_size   = sigil_read_le64(e + 8);
        uint32_t name_offset = sigil_read_le32(e + 16);
        if (name_offset >= lay.string_table_size) continue;

        uint32_t name_end = name_offset;
        while (name_end < lay.string_table_size && strings[name_end] != 0) name_end++;
        size_t name_len = name_end - name_offset;
        const char *name = (const char *)strings + name_offset;
        if (name_len < 5) continue;
        if (memcmp(name + name_len - 5, ".cnmt", 5) != 0) continue;

        uint64_t abs = lay.data_off + file_offset;
        if (abs + 0x10 > len || file_size < 0x10) continue;

        const uint8_t *cnmt = buf + abs;
        sigil_hex_encode_8(cnmt, out->title_id, true);
        out->version = sigil_read_le32(cnmt + 0x08);
        out->content_type = content_type_from_meta(cnmt[0x0C]);
        out->from_cnmt = true;
        return SIGIL_OK;
    }
    return SIGIL_ERR_NOT_FOUND;
}

static int scan_for_cnmt(const uint8_t *buf, size_t len,
                         sigil_switch_title *out) {
    if (len < 16) return SIGIL_ERR_NOT_FOUND;
    for (size_t i = 0; i + 16 <= len; i++) {
        if (memcmp(buf + i, "PFS0", 4) != 0) continue;
        if (parse_inner_pfs0(buf + i, len - i, out) == SIGIL_OK) return SIGIL_OK;
    }
    return SIGIL_ERR_NOT_FOUND;
}

int sigil_cnmt_from_meta_nca(const sigil_io *io, uint64_t nca_offset,
                             const uint8_t decrypted_header[SIGIL_NCA_HEADER_SIZE],
                             const sigil_support *sup,
                             sigil_switch_title *out) {
    if (!io || !decrypted_header || !sup || !out) return SIGIL_ERR_INVALID_ARG;

    /* key_generation lives in two fields for backwards compatibility; the
     * kaek slot is one below the generation. */
    uint8_t key_generation = decrypted_header[0x206];
    if (decrypted_header[0x220] > key_generation) key_generation = decrypted_header[0x220];
    unsigned kaek_gen_index = (key_generation == 0) ? 0 : (unsigned)key_generation - 1;

    uint8_t kaek[16];
    int rc = load_kaek(sup, kaek_gen_index, kaek);
    if (rc != SIGIL_OK) return rc;

    /* Decrypt the key area; slot 2 is the AES-CTR body key. */
    uint8_t key_area[0x40];
    memcpy(key_area, decrypted_header + NCA_KEY_AREA_OFFSET, sizeof(key_area));
    struct AES_ctx ecb;
    AES_init_ctx(&ecb, kaek);
    for (int i = 0; i < 4; i++) AES_ECB_decrypt(&ecb, key_area + i * 0x10);
    const uint8_t *section_key = key_area + 0x20;

    uint64_t sec_start = (uint64_t)sigil_read_le32(decrypted_header + NCA_FS_ENTRY0_OFFSET)
                         * NCA_MEDIA_UNIT;
    uint64_t sec_end   = (uint64_t)sigil_read_le32(decrypted_header + NCA_FS_ENTRY0_OFFSET + 4)
                         * NCA_MEDIA_UNIT;
    if (sec_end <= sec_start) return SIGIL_ERR_NOT_FOUND;

    const uint8_t *fsh = decrypted_header + NCA_FS_HEADER0_OFFSET;
    uint8_t enc_type = fsh[FS_HEADER_ENC_OFFSET];
    uint32_t generation   = sigil_read_le32(fsh + FS_HEADER_GEN_OFFSET);
    uint32_t secure_value = sigil_read_le32(fsh + FS_HEADER_SECURE_OFFSET);

    uint64_t sec_size = sec_end - sec_start;
    if (sec_size > CNMT_MAX_SECTION) sec_size = CNMT_MAX_SECTION;

    uint8_t *buf = (uint8_t *)malloc((size_t)sec_size);
    if (!buf) return SIGIL_ERR_OOM;
    rc = sigil_io_read_exact(io, nca_offset + sec_start, buf, (size_t)sec_size);
    if (rc != SIGIL_OK) { free(buf); return rc; }

    if (enc_type == NCA_ENC_TYPE_AES_CTR) {
        /* Upper 8 counter bytes: secure_value then generation, both big-endian
         * (hactool section_ctr order); lower 8: byte offset >> 4, big-endian. */
        uint8_t ctr[16];
        ctr[0] = (uint8_t)(secure_value >> 24);
        ctr[1] = (uint8_t)(secure_value >> 16);
        ctr[2] = (uint8_t)(secure_value >> 8);
        ctr[3] = (uint8_t)secure_value;
        ctr[4] = (uint8_t)(generation >> 24);
        ctr[5] = (uint8_t)(generation >> 16);
        ctr[6] = (uint8_t)(generation >> 8);
        ctr[7] = (uint8_t)generation;
        /* Counter offset is relative to the NCA start, not the container. */
        uint64_t block = sec_start >> 4;
        for (int i = 0; i < 8; i++) ctr[15 - i] = (uint8_t)(block >> (8 * i));

        sigil_aes_ctr_crypt(section_key, ctr, buf, (size_t)sec_size);
    }

    rc = scan_for_cnmt(buf, (size_t)sec_size, out);
    free(buf);
    return rc;
}
