// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

static const uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

static const uint8_t MD5_S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static inline uint32_t md5_rotl(uint32_t x, uint32_t c) {
    return (x << c) | (x >> (32 - c));
}

static void md5_block(uint32_t state[4], const uint8_t block[64]) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++) m[i] = sigil_read_le32(block + i * 4);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + md5_rotl(a + f + MD5_K[i] + m[g], MD5_S[i]);
        a = tmp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void sigil_md5_init(sigil_md5 *m) {
    m->state[0] = 0x67452301;
    m->state[1] = 0xefcdab89;
    m->state[2] = 0x98badcfe;
    m->state[3] = 0x10325476;
    m->length   = 0;
    m->buffered = 0;
}

void sigil_md5_update(sigil_md5 *m, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    m->length += len;
    if (m->buffered) {
        size_t take = 64 - m->buffered;
        if (take > len) take = len;
        memcpy(m->buffer + m->buffered, p, take);
        m->buffered += take;
        p += take;
        len -= take;
        if (m->buffered < 64) return;
        md5_block(m->state, m->buffer);
        m->buffered = 0;
    }
    while (len >= 64) {
        md5_block(m->state, p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(m->buffer, p, len);
        m->buffered = len;
    }
}

void sigil_md5_final(sigil_md5 *m, uint8_t digest[16]) {
    uint64_t bits = m->length * 8;
    uint8_t pad = 0x80;
    sigil_md5_update(m, &pad, 1);
    m->length -= 1;
    uint8_t zero = 0;
    while (m->buffered != 56) {
        sigil_md5_update(m, &zero, 1);
        m->length -= 1;
    }
    uint8_t len_le[8];
    for (int i = 0; i < 8; i++) len_le[i] = (uint8_t)(bits >> (8 * i));
    sigil_md5_update(m, len_le, 8);
    for (int i = 0; i < 4; i++) {
        digest[i * 4]     = (uint8_t)(m->state[i]);
        digest[i * 4 + 1] = (uint8_t)(m->state[i] >> 8);
        digest[i * 4 + 2] = (uint8_t)(m->state[i] >> 16);
        digest[i * 4 + 3] = (uint8_t)(m->state[i] >> 24);
    }
}

void sigil_md5_hex(const uint8_t digest[16], char out[33]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2]     = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    out[32] = '\0';
}
