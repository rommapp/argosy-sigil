// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

#include "aes.h"

/* Nintendo XTS variant: tweak input has bytes 0-7 zero, bytes 8-15 holding
 * the sector number in big-endian. Standard P1619 puts the sector LE in
 * bytes 0-7 with bytes 8-15 zero. Test vectors against generic XTS pass
 * at sector 0 (zero is zero either way) but fail at any sector >= 1.
 *
 * The GF(2^128) `mul_alpha` below IS standard: shift left by 1 with byte
 * 0 the LSB and byte 15 the MSB, XOR 0x87 into byte 0 if the high bit was
 * set. Only the tweak input is non-standard. */

static inline void mul_alpha(uint8_t t[16]) {
    uint8_t carry = (t[15] >> 7) & 1;
    for (int i = 15; i >= 1; i--) {
        t[i] = (uint8_t)((t[i] << 1) | (t[i - 1] >> 7));
    }
    t[0] = (uint8_t)(t[0] << 1);
    if (carry) t[0] ^= 0x87;
}

void sigil_aes_xts_decrypt_nintendo(const uint8_t key[32],
                                     uint64_t start_sector,
                                     uint8_t *data, size_t len) {
    struct AES_ctx data_ctx;
    struct AES_ctx tweak_ctx;
    AES_init_ctx(&data_ctx,  key);
    AES_init_ctx(&tweak_ctx, key + 16);

    const size_t SECTOR = 0x200;
    size_t full_sectors = len / SECTOR;

    for (size_t s = 0; s < full_sectors; s++) {
        uint8_t tweak[16] = {0};
        uint64_t n = start_sector + s;
        for (int i = 15; i >= 8; i--) {
            tweak[i] = (uint8_t)(n & 0xFF);
            n >>= 8;
        }
        AES_ECB_encrypt(&tweak_ctx, tweak);

        uint8_t *sector_base = data + s * SECTOR;
        for (size_t off = 0; off + 16 <= SECTOR; off += 16) {
            uint8_t *block = sector_base + off;
            for (int i = 0; i < 16; i++) block[i] ^= tweak[i];
            AES_ECB_decrypt(&data_ctx, block);
            for (int i = 0; i < 16; i++) block[i] ^= tweak[i];
            mul_alpha(tweak);
        }
    }

    /* Trailing partial sector path. NCA headers are exact 0xC00 = 6 full
     * sectors so this is rarely exercised, but the original Kotlin handles
     * it for robustness. */
    size_t remaining_off = full_sectors * SECTOR;
    if (remaining_off < len) {
        uint8_t tweak[16] = {0};
        uint64_t n = start_sector + full_sectors;
        for (int i = 15; i >= 8; i--) {
            tweak[i] = (uint8_t)(n & 0xFF);
            n >>= 8;
        }
        AES_ECB_encrypt(&tweak_ctx, tweak);

        size_t off = remaining_off;
        while (off + 16 <= len) {
            uint8_t *block = data + off;
            for (int i = 0; i < 16; i++) block[i] ^= tweak[i];
            AES_ECB_decrypt(&data_ctx, block);
            for (int i = 0; i < 16; i++) block[i] ^= tweak[i];
            mul_alpha(tweak);
            off += 16;
        }
    }
}

void sigil_aes_xts_encrypt_nintendo(const uint8_t key[32],
                                     uint64_t start_sector,
                                     uint8_t *data, size_t len) {
    struct AES_ctx data_ctx;
    struct AES_ctx tweak_ctx;
    AES_init_ctx(&data_ctx,  key);
    AES_init_ctx(&tweak_ctx, key + 16);

    const size_t SECTOR = 0x200;
    size_t sectors = (len + SECTOR - 1) / SECTOR;

    for (size_t s = 0; s < sectors; s++) {
        uint8_t tweak[16] = {0};
        uint64_t n = start_sector + s;
        for (int i = 15; i >= 8; i--) {
            tweak[i] = (uint8_t)(n & 0xFF);
            n >>= 8;
        }
        AES_ECB_encrypt(&tweak_ctx, tweak);

        uint8_t *sector_base = data + s * SECTOR;
        size_t sector_len = (s * SECTOR + SECTOR <= len) ? SECTOR : len - s * SECTOR;
        for (size_t off = 0; off + 16 <= sector_len; off += 16) {
            uint8_t *block = sector_base + off;
            for (int i = 0; i < 16; i++) block[i] ^= tweak[i];
            AES_ECB_encrypt(&data_ctx, block);
            for (int i = 0; i < 16; i++) block[i] ^= tweak[i];
            mul_alpha(tweak);
        }
    }
}
