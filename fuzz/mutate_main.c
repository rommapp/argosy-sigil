// SPDX-License-Identifier: MPL-2.0
/*
 * Drives a libFuzzer target without libFuzzer: runs each file given on the
 * command line, then random mutations of them. For toolchains without the
 * libFuzzer runtime, such as Apple clang; build with the address and
 * undefined-behaviour sanitizers so a bad read aborts.
 *
 *   fuzz_card_mutate <iterations> <seed> file...
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static uint64_t g_state;

static uint32_t next_random(void) {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 7;
    g_state ^= g_state << 17;
    return (uint32_t)g_state;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (n > 0) ? (uint8_t *)malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = buf ? (size_t)n : 0;
    return buf;
}

/* Half the edits land in the first 64 KiB, where every card format keeps its
 * header, directory and allocation tables; the rest land anywhere. */
#define STRUCTURE_SPAN (64u * 1024u)

static void mutate(uint8_t *buf, size_t len) {
    uint32_t edits = 1 + next_random() % 16;
    for (uint32_t i = 0; i < edits; i++) {
        size_t span = (next_random() & 1) && len > STRUCTURE_SPAN ? STRUCTURE_SPAN : len;
        size_t at = next_random() % span;
        switch (next_random() % 3) {
        case 0: buf[at] ^= (uint8_t)(1u << (next_random() % 8)); break;
        case 1: buf[at] = (uint8_t)next_random(); break;
        default: buf[at] = (next_random() & 1) ? 0xFF : 0x00; break;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <iterations> <seed> file...\n", argv[0]);
        return 2;
    }
    long iterations = strtol(argv[1], NULL, 10);
    g_state = strtoull(argv[2], NULL, 10) | 1;

    int files = argc - 3;
    for (int i = 0; i < files; i++) {
        size_t len = 0;
        uint8_t *buf = read_file(argv[3 + i], &len);
        if (buf) LLVMFuzzerTestOneInput(buf, len);
        free(buf);
    }
    for (long it = 0; it < iterations; it++) {
        size_t len = 0;
        uint8_t *buf = read_file(argv[3 + (int)(next_random() % (uint32_t)files)], &len);
        if (!buf || len == 0) { free(buf); continue; }
        size_t cut = (next_random() % 8 == 0) ? next_random() % len : len;
        mutate(buf, cut ? cut : len);
        LLVMFuzzerTestOneInput(buf, cut ? cut : len);
        free(buf);
    }
    printf("%ld mutated inputs, no crash\n", iterations);
    return 0;
}
