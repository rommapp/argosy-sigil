// SPDX-License-Identifier: MPL-2.0
/* PSP save PARAM.SFO: the file POPS keeps beside a PS1 classic's memory
 * cards, and the signature the PSP's save utility gives it. The utility
 * hashes the whole file with sceSdMac, AES-CMAC under one of KIRK's key
 * slots: slot 0x10 masked with a constant for the hash at 0x70, slot 3 for
 * the hash at 0x10. The hash at 0x20 goes through the console's fuse key,
 * which no one off the console has. POPS checks the hash at 0x70 alone; it
 * rewrites the file, the hash at 0x20 included, when the game saves. */
#include "sigil_internal.h"
#include "aes.h"
#include <stdlib.h>

#define PARAMS_LEN     128u
#define PARAMS_FLAG    0x41u /* 0x01: the hash at 0x20; 0x40: the newer mode's hash at 0x70 */
#define PARAMS_HASH_10 0x10u
#define PARAMS_HASH_70 0x70u

/* KIRK key slots 3 and 0x10, and the mask sceSdMacFinal applies in modes 5
 * and 6: public constants of the PSP's crypto engine. */
static const uint8_t KIRK_SLOT_03[16] = {
    0x98, 0x02, 0xC4, 0xE6, 0xEC, 0x9E, 0x9E, 0x2F, 0xFC, 0x63, 0x4C, 0xE4, 0x2F, 0xBB, 0x46, 0x68,
};
static const uint8_t KIRK_SLOT_10[16] = {
    0x32, 0x29, 0x5B, 0xD5, 0xEA, 0xF7, 0xA3, 0x42, 0x16, 0xC8, 0x8E, 0x48, 0xFF, 0x50, 0xD3, 0x71,
};
static const uint8_t MODE_5_MASK[16] = {
    0xCB, 0x15, 0xF4, 0x07, 0xF9, 0x6A, 0x52, 0x3C, 0x04, 0xB9, 0xB2, 0xEE, 0x5C, 0x53, 0xFA, 0x86,
};

static void double_block(uint8_t b[16]) {
    uint8_t carry = b[0] & 0x80;
    for (int i = 0; i < 15; i++) b[i] = (uint8_t)(b[i] << 1 | b[i + 1] >> 7);
    b[15] = (uint8_t)(b[15] << 1);
    if (carry) b[15] ^= 0x87;
}

/* AES-CMAC (RFC 4493) of `len` bytes of `data`. */
static void aes_cmac(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t mac[16]) {
    struct AES_ctx ctx;
    AES_init_ctx(&ctx, key);
    uint8_t k1[16] = { 0 };
    AES_ECB_encrypt(&ctx, k1);
    double_block(k1);
    uint8_t k2[16];
    memcpy(k2, k1, 16);
    double_block(k2);

    size_t blocks = len ? (len + 15) / 16 : 1;
    bool whole = len && len % 16 == 0;
    uint8_t x[16] = { 0 };
    for (size_t b = 0; b + 1 < blocks; b++) {
        for (int i = 0; i < 16; i++) x[i] ^= data[b * 16 + i];
        AES_ECB_encrypt(&ctx, x);
    }
    uint8_t last[16] = { 0 };
    size_t tail = len - (blocks - 1) * 16;
    memcpy(last, data + (blocks - 1) * 16, tail);
    if (!whole) last[tail] = 0x80;
    for (int i = 0; i < 16; i++) x[i] ^= last[i] ^ (whole ? k1[i] : k2[i]);
    AES_ECB_encrypt(&ctx, x);
    memcpy(mac, x, 16);
}

int sigil_psp_sfo_sign(uint8_t *sfo, size_t len) {
    size_t at = 0, size = 0;
    if (!sfo) return SIGIL_ERR_INVALID_ARG;
    int rc = sigil_sfo_find(sfo, len, "SAVEDATA_PARAMS", &at, &size);
    if (rc != SIGIL_OK) return rc;
    if (size < PARAMS_LEN) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    /* The save utility hashes the file zero-padded to a 16-byte boundary. */
    size_t padded = (len + 15) & ~(size_t)15;
    uint8_t *copy = (uint8_t *)calloc(1, padded ? padded : 16);
    if (!copy) return SIGIL_ERR_OOM;
    uint8_t *params = sfo + at;
    params[0] |= PARAMS_FLAG;
    memset(params + PARAMS_HASH_10, 0, 16);
    memset(params + PARAMS_HASH_70, 0, 16);
    memcpy(copy, sfo, len);
    uint8_t hash[16];
    aes_cmac(KIRK_SLOT_10, copy, padded, hash);
    for (int i = 0; i < 16; i++) params[PARAMS_HASH_70 + i] = hash[i] ^ MODE_5_MASK[i];
    memcpy(copy, sfo, len);
    aes_cmac(KIRK_SLOT_03, copy, padded, params + PARAMS_HASH_10);
    free(copy);
    return SIGIL_OK;
}

typedef struct {
    const char *key;
    uint16_t    fmt;      /* 0x0204 UTF-8 string, 0x0404 u32, 0x0004 bytes */
    uint32_t    max;
    const void *value;
    uint32_t    len;
} sfo_field;

int sigil_pops_param_sfo(const char *directory, const char *title, uint8_t out[SIGIL_POPS_SFO_SIZE]) {
    if (!directory || !title || !out) return SIGIL_ERR_INVALID_ARG;
    size_t dir_len = strlen(directory) + 1;
    if (dir_len > 64) return SIGIL_ERR_INVALID_ARG;
    /* The title slot holds 127 bytes and a NUL; a longer one is cut short,
     * back to the start of a UTF-8 character. */
    char shown[128];
    size_t title_len = strlen(title);
    if (title_len > sizeof(shown) - 1) {
        title_len = sizeof(shown) - 1;
        while (title_len && ((unsigned char)title[title_len] & 0xC0) == 0x80) title_len--;
    }
    memcpy(shown, title, title_len);
    shown[title_len++] = '\0';
    static const uint8_t parental[4] = { 1, 0, 0, 0 };
    /* POPS's own layout, keys in order and each value in a slot of its
     * maximum size (vita-pops-empty, chrono-trigger-vita-pops). */
    const sfo_field fields[] = {
        { "CATEGORY", 0x0204, 4, "MS", 3 },
        { "PARENTAL_LEVEL", 0x0404, 4, parental, 4 },
        { "SAVEDATA_DETAIL", 0x0204, 1024, "", 1 },
        { "SAVEDATA_DIRECTORY", 0x0204, 64, directory, (uint32_t)dir_len },
        { "SAVEDATA_FILE_LIST", 0x0004, 3168, NULL, 3168 },
        { "SAVEDATA_PARAMS", 0x0004, PARAMS_LEN, NULL, PARAMS_LEN },
        { "SAVEDATA_TITLE", 0x0204, 128, "", 1 },
        { "TITLE", 0x0204, 128, shown, (uint32_t)title_len },
    };
    const size_t count = sizeof(fields) / sizeof(fields[0]);
    memset(out, 0, SIGIL_POPS_SFO_SIZE);
    size_t keys = 0;
    for (size_t i = 0; i < count; i++) keys += strlen(fields[i].key) + 1;
    uint32_t key_table = 20 + (uint32_t)count * 16, data_table = key_table + (uint32_t)((keys + 3) & ~(size_t)3);
    memcpy(out, "\0PSF", 4);
    sigil_write_le32(out + 4, 0x101);
    sigil_write_le32(out + 8, key_table);
    sigil_write_le32(out + 12, data_table);
    sigil_write_le32(out + 16, (uint32_t)count);
    uint32_t key_at = 0, data_at = 0;
    for (size_t i = 0; i < count; i++) {
        uint8_t *e = out + 20 + i * 16;
        sigil_write_le16(e, (uint16_t)key_at);
        sigil_write_le16(e + 2, fields[i].fmt);
        sigil_write_le32(e + 4, fields[i].len);
        sigil_write_le32(e + 8, fields[i].max);
        sigil_write_le32(e + 12, data_at);
        memcpy(out + key_table + key_at, fields[i].key, strlen(fields[i].key));
        if (fields[i].value) memcpy(out + data_table + data_at, fields[i].value, fields[i].len);
        key_at += (uint32_t)strlen(fields[i].key) + 1;
        data_at += fields[i].max;
    }
    if (data_table + data_at != SIGIL_POPS_SFO_SIZE) return SIGIL_ERR_INVALID_ARG;
    return sigil_psp_sfo_sign(out, SIGIL_POPS_SFO_SIZE);
}
