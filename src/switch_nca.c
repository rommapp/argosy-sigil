// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

#define NCA_MAGIC_OFFSET      0x200
#define NCA_PROGRAM_ID_OFFSET 0x210
#define NCA_RIGHTS_ID_OFFSET  0x230

int sigil_resolve_header_key(const sigil_support *sup, uint8_t out[32]) {
    if (!sup) return SIGIL_ERR_NEEDS_KEY;

    if (sup->switch_header_key) {
        memcpy(out, sup->switch_header_key, 32);
        return SIGIL_OK;
    }
    if (sup->switch_prod_keys_text && sup->switch_prod_keys_text_len > 0) {
        return sigil_decode_header_key_from_text(
            sup->switch_prod_keys_text, sup->switch_prod_keys_text_len, out);
    }
    if (sup->switch_prod_keys_path) {
        return sigil_load_header_key_from_prod_keys(sup->switch_prod_keys_path, out);
    }
    return SIGIL_ERR_NEEDS_KEY;
}

int sigil_nca_extract_title_id(const uint8_t *decrypted_header, char out_title_id[17]) {
    if (memcmp(decrypted_header + NCA_MAGIC_OFFSET, "NCA3", 4) != 0 &&
        memcmp(decrypted_header + NCA_MAGIC_OFFSET, "NCA2", 4) != 0) {
        return SIGIL_ERR_NOT_FOUND;
    }

    /* program_id at 0x210: 8 bytes LE, byte-reverse to hex with "01" gate. */
    char candidate[17];
    sigil_hex_encode_8(decrypted_header + NCA_PROGRAM_ID_OFFSET, candidate, true);
    if (candidate[0] == '0' && candidate[1] == '1') {
        memcpy(out_title_id, candidate, 17);
        return SIGIL_OK;
    }

    /* rights_id at 0x230: first 8 bytes BE, no reverse. */
    const uint8_t *rid = decrypted_header + NCA_RIGHTS_ID_OFFSET;
    bool nonzero = false;
    for (int i = 0; i < 16; i++) { if (rid[i] != 0) { nonzero = true; break; } }
    if (nonzero) {
        sigil_hex_encode_8(rid, candidate, false);
        if (candidate[0] == '0' && candidate[1] == '1') {
            memcpy(out_title_id, candidate, 17);
            return SIGIL_OK;
        }
    }

    return SIGIL_ERR_NOT_FOUND;
}

/* Plaintext-first: decrypted dumps already carry a valid NCA magic, and
 * XTS-decrypting a plaintext header would scramble it. Only decrypt (a
 * copy, the caller's buffer stays raw) when the plaintext probe fails. */
int sigil_nca_title_from_raw_header(const uint8_t *raw_header,
                                    const uint8_t *header_key_or_null,
                                    char out_title_id[17]) {
    if (sigil_nca_extract_title_id(raw_header, out_title_id) == SIGIL_OK) {
        return SIGIL_OK;
    }
    if (!header_key_or_null) return SIGIL_ERR_NEEDS_KEY;

    uint8_t header[SIGIL_NCA_HEADER_SIZE];
    memcpy(header, raw_header, SIGIL_NCA_HEADER_SIZE);
    sigil_aes_xts_decrypt_nintendo(header_key_or_null, 0, header,
                                   SIGIL_NCA_HEADER_SIZE);
    return sigil_nca_extract_title_id(header, out_title_id);
}

static bool nca_has_magic(const uint8_t *header) {
    return memcmp(header + NCA_MAGIC_OFFSET, "NCA3", 4) == 0 ||
           memcmp(header + NCA_MAGIC_OFFSET, "NCA2", 4) == 0;
}

int sigil_nca_decrypt_header(const uint8_t *raw_header,
                             const uint8_t *header_key_or_null,
                             uint8_t out[SIGIL_NCA_HEADER_SIZE]) {
    memcpy(out, raw_header, SIGIL_NCA_HEADER_SIZE);
    if (nca_has_magic(out)) return SIGIL_OK; /* plaintext dump */
    if (!header_key_or_null) return SIGIL_ERR_NEEDS_KEY;
    sigil_aes_xts_decrypt_nintendo(header_key_or_null, 0, out,
                                   SIGIL_NCA_HEADER_SIZE);
    return nca_has_magic(out) ? SIGIL_OK : SIGIL_ERR_NOT_FOUND;
}
