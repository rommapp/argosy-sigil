// SPDX-License-Identifier: MPL-2.0
#include "n64_save.h"
#include <stdlib.h>

#define BLOB_EEPROM 0x0u
#define BLOB_PAKS   0x800u
#define BLOB_SRAM   0x20800u
#define BLOB_FLASH  0x28800u

/* A pak's index table: inodes 5 to 127 of page 1, big-endian, 0x0003 when free. */
#define PAK_INDEX_FIRST 0x10Au
#define PAK_INDEX_END   0x200u
#define PAK_INODE_FREE  0x0003u

static const char *const NAMES[N64_REGION_COUNT] = { "eeprom", "pak1", "pak2", "pak3", "pak4", "sram", "flash" };

const char *n64_region_name(int region) {
    return region >= 0 && region < N64_REGION_COUNT ? NAMES[region] : NULL;
}

int n64_region_of(const char *name) {
    for (int r = 0; r < N64_REGION_COUNT; r++) {
        if (strcmp(NAMES[r], name) == 0) return r;
    }
    return -1;
}

static size_t full_size(int region) {
    if (region == N64_EEPROM) return N64_EEPROM_MAX;
    if (region == N64_SRAM) return N64_SRAM_SIZE;
    if (region == N64_FLASH) return N64_FLASH_SIZE;
    return N64_PAK_SIZE;
}

uint8_t *n64_region_bytes(n64_save *s, int region, size_t *len) {
    switch (region) {
    case N64_EEPROM: *len = s->eeprom_len; return s->eeprom;
    case N64_SRAM: *len = N64_SRAM_SIZE; return s->sram;
    case N64_FLASH: *len = N64_FLASH_SIZE; return s->flash;
    default: *len = N64_PAK_SIZE; return s->pak[region - N64_PAK1];
    }
}

static bool swapped(int region) {
    return region == N64_SRAM || region == N64_FLASH;
}

void n64_swap32(uint8_t *data, size_t len) {
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint8_t a = data[i], b = data[i + 1];
        data[i] = data[i + 3];
        data[i + 1] = data[i + 2];
        data[i + 2] = b;
        data[i + 3] = a;
    }
}

void n64_pak_formatted(uint8_t out[N64_PAK_SIZE]) {
    memset(out, 0, N64_PAK_SIZE);
    static const uint8_t ID_TAIL[] = { 0x00, 0x01, 0x01, 0x00, 0x01, 0x01, 0xFE, 0xF1 };
    static const size_t ID_BLOCKS[] = { 0x20, 0x60, 0x80, 0xC0 };
    for (size_t i = 0; i < sizeof(ID_BLOCKS) / sizeof(ID_BLOCKS[0]); i++) {
        memcpy(out + ID_BLOCKS[i] + 0x18, ID_TAIL, sizeof(ID_TAIL));
    }
    for (size_t at = PAK_INDEX_FIRST; at < PAK_INDEX_END; at += 2) out[at + 1] = PAK_INODE_FREE;
    out[0x101] = 0x71;
    memcpy(out + 0x200, out + 0x100, 0x100);
}

static bool written(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (data[i] != 0x00 && data[i] != 0xFF) return true;
    }
    return false;
}

static bool pak_used(const uint8_t *pak) {
    for (size_t at = PAK_INDEX_FIRST; at < PAK_INDEX_END; at += 2) {
        if (sigil_read_be16(pak + at) != PAK_INODE_FREE) return written(pak, N64_PAK_SIZE);
    }
    return false;
}

void n64_take(n64_save *s, int region, const uint8_t *data, size_t len) {
    size_t full = full_size(region);
    uint8_t *buf = region == N64_EEPROM ? s->eeprom
                 : region == N64_SRAM   ? s->sram
                 : region == N64_FLASH  ? s->flash
                                        : s->pak[region - N64_PAK1];
    size_t n = len < full ? len : full;
    memcpy(buf, data, n);
    memset(buf + n, 0xFF, full - n);
    if (swapped(region)) n64_swap32(buf, full);
    if (region == N64_EEPROM) {
        s->eeprom_len = written(buf + N64_EEPROM_4K, N64_EEPROM_MAX - N64_EEPROM_4K) ? N64_EEPROM_MAX : N64_EEPROM_4K;
        s->present[region] = written(buf, s->eeprom_len);
    } else {
        s->present[region] = region >= N64_PAK1 && region <= N64_PAK4 ? pak_used(buf) : written(buf, full);
    }
}

bool n64_take_neutral(n64_save *s, int region, const uint8_t *data, size_t len) {
    if (region < 0 || region >= N64_REGION_COUNT) return false;
    if (region == N64_EEPROM ? len != N64_EEPROM_4K && len != N64_EEPROM_MAX : len != full_size(region)) return false;
    size_t room = 0;
    if (region == N64_EEPROM) s->eeprom_len = len;
    memcpy(n64_region_bytes(s, region, &room), data, len);
    s->present[region] = true;
    return true;
}

void n64_take_blob(n64_save *s, const uint8_t *blob, size_t len) {
    static const size_t AT[N64_REGION_COUNT] = {
        BLOB_EEPROM, BLOB_PAKS, BLOB_PAKS + N64_PAK_SIZE, BLOB_PAKS + 2 * N64_PAK_SIZE, BLOB_PAKS + 3 * N64_PAK_SIZE,
        BLOB_SRAM, BLOB_FLASH,
    };
    for (int r = 0; r < N64_REGION_COUNT; r++) {
        size_t have = len > AT[r] ? len - AT[r] : 0;
        n64_take(s, r, blob + (have ? AT[r] : 0), have < full_size(r) ? have : full_size(r));
    }
}

size_t n64_give(const n64_save *s, int region, uint8_t *out) {
    size_t full = full_size(region), len = 0;
    const uint8_t *src = n64_region_bytes((n64_save *)s, region, &len);
    memset(out, 0xFF, full);
    memcpy(out, src, len);
    if (swapped(region)) n64_swap32(out, full);
    return full;
}

int n64_give_blob(const n64_save *s, const uint8_t *existing, size_t existing_len, uint8_t out[N64_BLOB_SIZE]) {
    n64_save *old = NULL;
    if (existing && existing_len >= N64_BLOB_SIZE) {
        old = (n64_save *)calloc(1, sizeof(*old));
        if (!old) return SIGIL_ERR_OOM;
        n64_take_blob(old, existing, existing_len);
    }
    size_t at = 0;
    for (int r = 0; r < N64_REGION_COUNT; r++) {
        size_t full = full_size(r);
        if (s->present[r]) {
            n64_give(s, r, out + at);
        } else if (old && !old->present[r]) {
            memcpy(out + at, existing + at, full);
        } else if (r >= N64_PAK1 && r <= N64_PAK4) {
            n64_pak_formatted(out + at);
        } else {
            memset(out + at, 0xFF, full);
        }
        at += full;
    }
    free(old);
    return SIGIL_OK;
}
