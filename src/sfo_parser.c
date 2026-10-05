// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

int sigil_sfo_find(const uint8_t *data, size_t len, const char *key, size_t *offset, size_t *size) {
    if (!data || !key || !offset || !size) return SIGIL_ERR_INVALID_ARG;
    if (len < 20) return SIGIL_ERR_NOT_FOUND;

    uint32_t magic            = sigil_read_le32(data + 0);
    uint32_t key_table_start  = sigil_read_le32(data + 8);
    uint32_t data_table_start = sigil_read_le32(data + 12);
    uint32_t entries          = sigil_read_le32(data + 16);

    if (magic != SIGIL_SFO_MAGIC) return SIGIL_ERR_NOT_FOUND;
    if (key_table_start >= len || data_table_start >= len) return SIGIL_ERR_NOT_FOUND;
    if ((size_t)entries * 16 + 20 > len) return SIGIL_ERR_NOT_FOUND;

    size_t key_len = strlen(key);

    for (uint32_t i = 0; i < entries; i++) {
        const uint8_t *e = data + 20 + (size_t)i * 16;
        uint16_t key_off  = (uint16_t)(e[0] | (e[1] << 8));
        uint32_t data_len = sigil_read_le32(e + 4);
        uint32_t data_off = sigil_read_le32(e + 12);

        size_t k_abs = (size_t)key_table_start + key_off;
        if (k_abs >= len) continue;
        size_t k_max = len - k_abs;
        if (k_max < key_len + 1) continue;
        if (data[k_abs + key_len] != 0) continue;
        if (memcmp(data + k_abs, key, key_len) != 0) continue;

        size_t d_abs = (size_t)data_table_start + data_off;
        if (d_abs >= len) return SIGIL_ERR_IO;
        *offset = d_abs;
        *size = data_len < len - d_abs ? data_len : len - d_abs;
        return SIGIL_OK;
    }
    return SIGIL_ERR_NOT_FOUND;
}

int sigil_sfo_get_string(const uint8_t *data, size_t len,
                         const char *key,
                         char *out, size_t out_cap) {
    if (!data || !key || !out || out_cap == 0) return SIGIL_ERR_INVALID_ARG;
    size_t at = 0, size = 0;
    int rc = sigil_sfo_find(data, len, key, &at, &size);
    if (rc != SIGIL_OK) return rc;
    size_t copy = size < out_cap - 1 ? size : out_cap - 1;

    size_t out_i = 0;
    for (size_t j = 0; j < copy; j++) {
        uint8_t c = data[at + j];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) break;
        out[out_i++] = (char)c;
    }
    out[out_i] = '\0';
    return out_i > 0 ? SIGIL_OK : SIGIL_ERR_NOT_FOUND;
}
