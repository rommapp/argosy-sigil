// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

/* Builds a ZArchive by hand around a synthetic default.xex and reads the title
 * id back through the .zar path. The member is deliberately pushed past the
 * first block so the test exercises the offset-record arithmetic rather than
 * only block zero, which is where the interesting failure lives: a wrong base
 * offset or a missing size+1 still reads block zero correctly. */

#define ZAR_MAGIC       0x169F52D6u
#define ZAR_VERSION_1   0x61BF3A01u
#define ZAR_FOOTER      144
#define ZAR_BLOCK       (64u * 1024u)
#define ZAR_PER_RECORD  16
#define ZAR_RECORD_SIZE (8 + 2 * ZAR_PER_RECORD)

#define XEX_EXEC_INFO   0x00040006u
#define SAMPLE_TITLE_ID 0x4D5307DCu   /* Crackdown, read from a real redump */
#define SAMPLE_HEX      "4D5307DC"

/* The member starts three blocks in, so reaching it requires summing the
 * preceding compressed sizes out of the offset record. */
#define MEMBER_BLOCK    3
#define TOTAL_BLOCKS    5

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

static void build_xex(uint8_t *p) {
    memcpy(p, "XEX2", 4);
    put_be32(p + 0x14, 1);            /* one optional header */
    put_be32(p + 0x18, XEX_EXEC_INFO);
    put_be32(p + 0x1C, 0x100);        /* exec info at +0x100 from XEX start */
    put_be32(p + 0x100 + 0x0C, SAMPLE_TITLE_ID);
}

int main(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/sigil_test.zar",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");

    /* --- uncompressed content space -------------------------------------- */
    size_t raw_len = (size_t)TOTAL_BLOCKS * ZAR_BLOCK;
    uint8_t *raw = calloc(1, raw_len);
    if (!raw) return 1;
    /* Fill earlier blocks with something that compresses differently per block
     * so the per-block sizes in the record genuinely differ. */
    for (size_t i = 0; i < (size_t)MEMBER_BLOCK * ZAR_BLOCK; i++) {
        raw[i] = (uint8_t)(i * 7 + (i / ZAR_BLOCK) * 31);
    }
    uint64_t member_off = (uint64_t)MEMBER_BLOCK * ZAR_BLOCK;
    build_xex(raw + member_off);
    uint64_t member_size = 0x200;

    FILE *fp = fopen(path, "wb");
    if (!fp) { free(raw); return 1; }

    /* --- compressed data section ----------------------------------------- */
    uint8_t *cbuf = malloc(ZSTD_compressBound(ZAR_BLOCK) + ZAR_BLOCK);
    uint16_t sizes[TOTAL_BLOCKS];
    if (!cbuf) { fclose(fp); free(raw); return 1; }

    for (int b = 0; b < TOTAL_BLOCKS; b++) {
        size_t got = ZSTD_compress(cbuf, ZSTD_compressBound(ZAR_BLOCK),
                                   raw + (size_t)b * ZAR_BLOCK, ZAR_BLOCK, 3);
        if (ZSTD_isError(got) || got >= ZAR_BLOCK) {
            /* Store raw when compression does not pay, exactly as the writer
             * does; the reader detects it by size == block size. */
            memcpy(cbuf, raw + (size_t)b * ZAR_BLOCK, ZAR_BLOCK);
            got = ZAR_BLOCK;
        }
        sizes[b] = (uint16_t)(got - 1);
        fwrite(cbuf, 1, got, fp);
    }
    uint64_t compressed_size = (uint64_t)ftell(fp);
    free(cbuf);

    /* --- offset records --------------------------------------------------- */
    uint64_t records_off = (uint64_t)ftell(fp);
    uint8_t rec[ZAR_RECORD_SIZE];
    memset(rec, 0, sizeof(rec));
    put_be64(rec, 0);                        /* first block starts at 0 */
    for (int b = 0; b < TOTAL_BLOCKS; b++) {
        rec[8 + b * 2]     = (uint8_t)(sizes[b] >> 8);
        rec[8 + b * 2 + 1] = (uint8_t)sizes[b];
    }
    fwrite(rec, 1, sizeof(rec), fp);
    uint64_t records_size = ZAR_RECORD_SIZE;

    /* --- name table ------------------------------------------------------- */
    uint64_t names_off = (uint64_t)ftell(fp);
    const char *member = "default.xex";
    uint8_t nlen = (uint8_t)strlen(member);
    fputc(nlen, fp);
    fwrite(member, 1, nlen, fp);
    uint64_t names_size = 1 + nlen;

    /* --- file tree: root directory with one file child -------------------- */
    uint64_t tree_off = (uint64_t)ftell(fp);
    uint8_t node[32];
    memset(node, 0, sizeof(node));
    put_be32(node + 0, 0x7FFFFFFFu & 0);     /* root: directory, name offset 0 */
    put_be32(node + 4, 1);                   /* children start at index 1 */
    put_be32(node + 8, 1);                   /* one child */
    put_be32(node + 16, 0x80000000u | 0);    /* child: file, name at offset 0 */
    put_be32(node + 20, (uint32_t)member_off);
    put_be32(node + 24, (uint32_t)member_size);
    put_be32(node + 28, 0);                  /* no high bits needed */
    fwrite(node, 1, sizeof(node), fp);
    uint64_t tree_size = sizeof(node);

    /* --- footer ----------------------------------------------------------- */
    uint8_t footer[ZAR_FOOTER];
    memset(footer, 0, sizeof(footer));
    put_be64(footer + 0x00, 0);              /* compressed data starts at 0 */
    put_be64(footer + 0x08, compressed_size);
    put_be64(footer + 0x10, records_off);
    put_be64(footer + 0x18, records_size);
    put_be64(footer + 0x20, names_off);
    put_be64(footer + 0x28, names_size);
    put_be64(footer + 0x30, tree_off);
    put_be64(footer + 0x38, tree_size);
    put_be32(footer + 0x88, ZAR_VERSION_1);
    put_be32(footer + 0x8C, ZAR_MAGIC);
    fwrite(footer, 1, sizeof(footer), fp);
    fclose(fp);
    free(raw);

    /* --- read it back ----------------------------------------------------- */
    sigil_result r;
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_AUTO, NULL, &r);
    remove(path);

    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL zar: rc=%d\n", rc);
        return 1;
    }
    if (r.platform != SIGIL_PLATFORM_XBOX360) {
        fprintf(stderr, "FAIL zar platform: got %d\n", (int)r.platform);
        return 1;
    }
    if (strcmp(r.title_id, SAMPLE_HEX) != 0 || strcmp(r.save_id, SAMPLE_HEX) != 0) {
        fprintf(stderr, "FAIL zar id: title_id='%s' save_id='%s'\n",
                r.title_id, r.save_id);
        return 1;
    }
    if (r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "FAIL zar: expected binary source\n");
        return 1;
    }
    printf("ok unit_zar (title_id=%s, member at block %d)\n", r.title_id, MEMBER_BLOCK);
    return 0;
}
