// SPDX-License-Identifier: MPL-2.0
/* SHA-256 as FIPS 180-4 section 6.2 specifies it. */
#include "sigil_internal.h"

#define SHA256_BLOCK 64u

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(uint32_t state[8], const uint8_t block[SHA256_BLOCK]) {
    uint32_t w[64];
    for (int t = 0; t < 16; t++) w[t] = sigil_read_be32(block + t * 4);
    for (int t = 16; t < 64; t++) {
        uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int t = 0; t < 64; t++) {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[t] + w[t];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void sigil_sha256_init(sigil_sha256 *s) {
    s->state[0] = 0x6a09e667;
    s->state[1] = 0xbb67ae85;
    s->state[2] = 0x3c6ef372;
    s->state[3] = 0xa54ff53a;
    s->state[4] = 0x510e527f;
    s->state[5] = 0x9b05688c;
    s->state[6] = 0x1f83d9ab;
    s->state[7] = 0x5be0cd19;
    s->length   = 0;
    s->buffered = 0;
}

void sigil_sha256_update(sigil_sha256 *s, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    s->length += len;
    if (s->buffered) {
        size_t take = SHA256_BLOCK - s->buffered;
        if (take > len) take = len;
        memcpy(s->buffer + s->buffered, p, take);
        s->buffered += take;
        p += take;
        len -= take;
        if (s->buffered < SHA256_BLOCK) return;
        sha256_block(s->state, s->buffer);
        s->buffered = 0;
    }
    while (len >= SHA256_BLOCK) {
        sha256_block(s->state, p);
        p += SHA256_BLOCK;
        len -= SHA256_BLOCK;
    }
    if (len) {
        memcpy(s->buffer, p, len);
        s->buffered = len;
    }
}

void sigil_sha256_final(sigil_sha256 *s, uint8_t digest[32]) {
    uint64_t bits = s->length * 8;
    s->buffer[s->buffered++] = 0x80;
    if (s->buffered > SHA256_BLOCK - 8) {
        memset(s->buffer + s->buffered, 0, SHA256_BLOCK - s->buffered);
        sha256_block(s->state, s->buffer);
        s->buffered = 0;
    }
    memset(s->buffer + s->buffered, 0, SHA256_BLOCK - 8 - s->buffered);
    sigil_write_be64(s->buffer + SHA256_BLOCK - 8, bits);
    sha256_block(s->state, s->buffer);
    for (int i = 0; i < 8; i++) sigil_write_be32(digest + i * 4, s->state[i]);
}
