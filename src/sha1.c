// SPDX-License-Identifier: MPL-2.0
/* SHA-1 as FIPS 180-4 section 6.1 specifies it, and HMAC over it as RFC 2104
 * does. */
#include "sigil_internal.h"

#define SHA1_BLOCK 64u

static inline uint32_t sha1_rotl(uint32_t x, uint32_t n) {
    return (x << n) | (x >> (32 - n));
}

static void sha1_block(uint32_t state[5], const uint8_t block[SHA1_BLOCK]) {
    uint32_t w[80];
    for (int t = 0; t < 16; t++) w[t] = sigil_read_be32(block + t * 4);
    for (int t = 16; t < 80; t++) w[t] = sha1_rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int t = 0; t < 80; t++) {
        uint32_t f, k;
        if (t < 20) {
            f = (b & c) ^ (~b & d);
            k = 0x5a827999;
        } else if (t < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (t < 60) {
            f = (b & c) ^ (b & d) ^ (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t temp = sha1_rotl(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = sha1_rotl(b, 30);
        b = a;
        a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void sigil_sha1_init(sigil_sha1 *s) {
    s->state[0] = 0x67452301;
    s->state[1] = 0xefcdab89;
    s->state[2] = 0x98badcfe;
    s->state[3] = 0x10325476;
    s->state[4] = 0xc3d2e1f0;
    s->length   = 0;
    s->buffered = 0;
}

void sigil_sha1_update(sigil_sha1 *s, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    s->length += len;
    if (s->buffered) {
        size_t take = SHA1_BLOCK - s->buffered;
        if (take > len) take = len;
        memcpy(s->buffer + s->buffered, p, take);
        s->buffered += take;
        p += take;
        len -= take;
        if (s->buffered < SHA1_BLOCK) return;
        sha1_block(s->state, s->buffer);
        s->buffered = 0;
    }
    while (len >= SHA1_BLOCK) {
        sha1_block(s->state, p);
        p += SHA1_BLOCK;
        len -= SHA1_BLOCK;
    }
    if (len) {
        memcpy(s->buffer, p, len);
        s->buffered = len;
    }
}

void sigil_sha1_final(sigil_sha1 *s, uint8_t digest[20]) {
    uint64_t bits = s->length * 8;
    s->buffer[s->buffered++] = 0x80;
    if (s->buffered > SHA1_BLOCK - 8) {
        memset(s->buffer + s->buffered, 0, SHA1_BLOCK - s->buffered);
        sha1_block(s->state, s->buffer);
        s->buffered = 0;
    }
    memset(s->buffer + s->buffered, 0, SHA1_BLOCK - 8 - s->buffered);
    for (int i = 0; i < 8; i++) s->buffer[SHA1_BLOCK - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha1_block(s->state, s->buffer);
    for (int i = 0; i < 5; i++) sigil_write_be32(digest + i * 4, s->state[i]);
}

void sigil_hmac_sha1(const uint8_t *key, size_t key_len, const void *data, size_t len, uint8_t mac[20]) {
    uint8_t block[SHA1_BLOCK] = { 0 };
    if (key_len > SHA1_BLOCK) {
        sigil_sha1 k;
        sigil_sha1_init(&k);
        sigil_sha1_update(&k, key, key_len);
        sigil_sha1_final(&k, block);
    } else {
        memcpy(block, key, key_len);
    }
    uint8_t pad[SHA1_BLOCK], inner[20];
    for (size_t i = 0; i < SHA1_BLOCK; i++) pad[i] = block[i] ^ 0x36;
    sigil_sha1 s;
    sigil_sha1_init(&s);
    sigil_sha1_update(&s, pad, SHA1_BLOCK);
    sigil_sha1_update(&s, data, len);
    sigil_sha1_final(&s, inner);
    for (size_t i = 0; i < SHA1_BLOCK; i++) pad[i] = block[i] ^ 0x5c;
    sigil_sha1_init(&s);
    sigil_sha1_update(&s, pad, SHA1_BLOCK);
    sigil_sha1_update(&s, inner, sizeof(inner));
    sigil_sha1_final(&s, mac);
}
