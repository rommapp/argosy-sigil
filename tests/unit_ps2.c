// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include "test_fs.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SECTOR_SIZE 2048
#define RAW_SECTOR_SIZE 2352
#define MODE2_DATA_OFFSET 24
#define VOLUME_SECTORS 24
#define ROOT_LBA 20
#define CNF_LBA 21

static void write_both_endian_16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void write_both_endian_32(uint8_t *out, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) out[i] = (uint8_t)(value >> (8 * i));
    for (unsigned i = 0; i < 4; i++) out[4 + i] = (uint8_t)(value >> (8 * (3 - i)));
}

static uint8_t *logical_sector(uint8_t *image, uint32_t lba, size_t sector_size,
                               uint8_t mode) {
    size_t data_offset = sector_size == RAW_SECTOR_SIZE
                       ? (mode == 2 ? MODE2_DATA_OFFSET : 16)
                       : 0;
    return image + (size_t)lba * sector_size + data_offset;
}

static uint8_t directory_record(uint8_t *out, uint32_t lba, uint32_t length,
                                uint8_t flags, const uint8_t *name, uint8_t name_len) {
    uint8_t record_len = (uint8_t)(33 + name_len + (name_len % 2 == 0 ? 1 : 0));
    memset(out, 0, record_len);
    out[0] = record_len;
    write_both_endian_32(out + 2, lba);
    write_both_endian_32(out + 10, length);
    out[18] = 126;
    out[19] = 1;
    out[20] = 1;
    out[25] = flags;
    write_both_endian_16(out + 28, 1);
    out[32] = name_len;
    memcpy(out + 33, name, name_len);
    return record_len;
}

static int write_ps2_image(const char *path, size_t sector_size, uint8_t mode,
                           size_t max_bytes) {
    size_t image_len = (size_t)VOLUME_SECTORS * sector_size;
    if (max_bytes > 0 && max_bytes < image_len) image_len = max_bytes;
    uint8_t image[VOLUME_SECTORS * RAW_SECTOR_SIZE];
    memset(image, 0, sizeof(image));

    if (sector_size == RAW_SECTOR_SIZE) {
        for (uint32_t lba = 0; lba < VOLUME_SECTORS; lba++) {
            uint8_t *sector = image + (size_t)lba * RAW_SECTOR_SIZE;
            sector[0] = 0;
            memset(sector + 1, 0xFF, 10);
            sector[11] = 0;
            sector[15] = mode;
        }
    }

    uint8_t *pvd = logical_sector(image, 16, sector_size, mode);
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    uint8_t root_name = 0;
    directory_record(pvd + 156, ROOT_LBA, SECTOR_SIZE, 2, &root_name, 1);

    uint8_t *root = logical_sector(image, ROOT_LBA, sector_size, mode);
    uint8_t dot = 0;
    uint8_t dotdot = 1;
    size_t pos = directory_record(root, ROOT_LBA, SECTOR_SIZE, 2, &dot, 1);
    pos += directory_record(root + pos, ROOT_LBA, SECTOR_SIZE, 2, &dotdot, 1);
    static const uint8_t cnf_name[] = "SYSTEM.CNF;1";
    directory_record(root + pos, CNF_LBA, 64, 0, cnf_name, sizeof(cnf_name) - 1);

    static const char cnf[] = "BOOT2 = cdrom0:\\SLUS_212.34;1\r\n";
    memcpy(logical_sector(image, CNF_LBA, sector_size, mode), cnf, sizeof(cnf) - 1);

    FILE *fp = test_fopen(path, "wb");
    if (!fp) return -1;
    size_t written = fwrite(image, 1, image_len, fp);
    int close_rc = fclose(fp);
    return written == image_len && close_rc == 0 ? 0 : -1;
}

int main(void) {
    char temp_dir[1024];
    if (test_temp_dir(temp_dir, sizeof(temp_dir)) != 0) {
        fprintf(stderr, "could not create temporary folder\n");
        return 1;
    }

    const struct {
        const char *name;
        size_t sector_size;
        uint8_t mode;
    } cases[] = {
        { "cooked BIN", SECTOR_SIZE, 0 },
        { "raw MODE1 BIN", RAW_SECTOR_SIZE, 1 },
        { "raw MODE2 BIN", RAW_SECTOR_SIZE, 2 },
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char path[1200];
        snprintf(path, sizeof(path), "%s/ps2-%zu.bin", temp_dir, i);
        if (write_ps2_image(path, cases[i].sector_size, cases[i].mode, 0) != 0) {
            fprintf(stderr, "FAIL %s: could not write synthetic image\n", cases[i].name);
            failures++;
            continue;
        }

        sigil_result result;
        int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_PS2, NULL, &result);
        if (rc != SIGIL_OK) {
            fprintf(stderr, "FAIL %s extraction: %s (%d)\n",
                    cases[i].name, sigil_strerror(rc), rc);
            failures++;
            continue;
        }
        if (strcmp(result.title_id, "SLUS-21234") != 0
            || strcmp(result.raw_serial, "SLUS_212.34") != 0
            || strcmp(result.save_id, "BASLUS-21234") != 0
            || result.source != SIGIL_SOURCE_BINARY
            || result.usage != SIGIL_USAGE_FOLDER_PREFIX) {
            fprintf(stderr, "FAIL %s identity: title=%s raw=%s save=%s usage=%d\n",
                    cases[i].name, result.title_id, result.raw_serial, result.save_id,
                    (int)result.usage);
            failures++;
            continue;
        }
        printf("ok %s -> %s (save_id=%s)\n",
               cases[i].name, result.title_id, result.save_id);
    }

    char truncated_path[1200];
    snprintf(truncated_path, sizeof(truncated_path), "%s/ps2-truncated.bin", temp_dir);
    if (write_ps2_image(truncated_path, RAW_SECTOR_SIZE, 1,
                        21 * RAW_SECTOR_SIZE + 100) != 0) {
        fprintf(stderr, "FAIL truncated image: could not write synthetic image\n");
        failures++;
    } else {
        sigil_result result;
        int rc = sigil_extract_from_path(truncated_path, SIGIL_PLATFORM_PS2, NULL, &result);
        if (rc == SIGIL_OK) {
            fprintf(stderr, "FAIL truncated image was accepted as %s\n", result.title_id);
            failures++;
        } else {
            printf("ok truncated raw BIN rejected (%s)\n", sigil_strerror(rc));
        }
    }

    test_remove_tree(temp_dir);
    return failures ? 1 : 0;
}
