// SPDX-License-Identifier: MPL-2.0
/* SHA-1 against the FIPS 180-4 examples and HMAC-SHA1 against RFC 2202. */
#include "sigil_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails = 0;

static void hex(const uint8_t digest[20], char out[41]) {
    for (int i = 0; i < 20; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
}

static void expect(const char *what, const uint8_t digest[20], const char *want) {
    char got[41];
    hex(digest, got);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s: %s want %s\n", what, got, want);
        g_fails++;
    }
}

static void test_sha1(void) {
    static const struct { const char *what; const char *data; const char *want; } CASES[] = {
        { "empty", "", "da39a3ee5e6b4b0d3255bfef95601890afd80709" },
        { "abc", "abc", "a9993e364706816aba3e25717850c26c9cd0d89d" },
        { "two blocks", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1" },
        { "55 bytes, padding fits", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          "c1c8bbdc22796e28c0e15163d20899b65621d65a" },
        { "56 bytes, padding spills", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          "c2db330f6083854c99d4b5bfb6e8f29f201be699" },
        { "64 bytes, one block", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          "0098ba824b5c16427bd7a1122a5a442a25ec644d" },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        uint8_t digest[20];
        sigil_sha1 s;
        sigil_sha1_init(&s);
        sigil_sha1_update(&s, CASES[i].data, strlen(CASES[i].data));
        sigil_sha1_final(&s, digest);
        expect(CASES[i].what, digest, CASES[i].want);
    }

    /* A million 'a's, fed in uneven pieces so the buffering is exercised. */
    uint8_t *million = (uint8_t *)malloc(1000000);
    memset(million, 'a', 1000000);
    sigil_sha1 s;
    sigil_sha1_init(&s);
    size_t at = 0, piece = 1;
    while (at < 1000000) {
        size_t n = piece < 1000000 - at ? piece : 1000000 - at;
        sigil_sha1_update(&s, million + at, n);
        at += n;
        piece = piece * 3 % 997 + 1;
    }
    uint8_t digest[20];
    sigil_sha1_final(&s, digest);
    expect("a million a", digest, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    free(million);
}

static void test_hmac(void) {
    uint8_t k1[20], k3[20], k4[25], k5[20], k6[80], d3[50], d4[50];
    memset(k1, 0x0b, sizeof(k1));
    memset(k3, 0xaa, sizeof(k3));
    for (int i = 0; i < 25; i++) k4[i] = (uint8_t)(i + 1);
    memset(k5, 0x0c, sizeof(k5));
    memset(k6, 0xaa, sizeof(k6));
    memset(d3, 0xdd, sizeof(d3));
    memset(d4, 0xcd, sizeof(d4));
    const struct { const char *what; const uint8_t *key; size_t key_len; const void *data; size_t len; const char *want; } CASES[] = {
        { "rfc2202 1", k1, 20, "Hi There", 8, "b617318655057264e28bc0b6fb378c8ef146be00" },
        { "rfc2202 2", (const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28,
          "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79" },
        { "rfc2202 3", k3, 20, d3, 50, "125d7342b9ac11cd91a39af48aa17b4f63f175d3" },
        { "rfc2202 4", k4, 25, d4, 50, "4c9007f4026250c6bc8414f9bf50c86c2d7235da" },
        { "rfc2202 5", k5, 20, "Test With Truncation", 20, "4c1a03424b55e07fe7f27be1d58bb9324a9a5a04" },
        { "rfc2202 6", k6, 80, "Test Using Larger Than Block-Size Key - Hash Key First", 54,
          "aa4ae5e15272d00e95705637ce8a3b55ed402112" },
        { "rfc2202 7", k6, 80, "Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data", 73,
          "e8e99d0f45237d786d6bbaa7965c7808bbff1a91" },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        uint8_t mac[20];
        sigil_hmac_sha1(CASES[i].key, CASES[i].key_len, CASES[i].data, CASES[i].len, mac);
        expect(CASES[i].what, mac, CASES[i].want);
    }
}

int main(void) {
    test_sha1();
    test_hmac();
    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("unit_sha1: ok\n");
    return 0;
}
