// SPDX-License-Identifier: MPL-2.0
#include "threeds_fixture.h"
#include <stdbool.h>
#include <stdio.h>

/* NCSD keeps the first NCCH at partitions[0].offset * 0x200 (stock: 0x4000);
 * NCCH keeps program_id at +0x118 of the NCCH itself. */
#define NCSD_MAGIC_OFFSET      0x100
#define NCSD_PARTITION_TABLE   0x120
#define NCCH_PROGRAM_ID_OFFSET 0x118
#define NCSD_STOCK_PARTITION0  0x4000

/* Little-endian on disk; reversed it reads "0004000000033500". */
static const uint8_t PROGRAM_ID[8] = { 0x00, 0x35, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00 };
static const char    EXPECT_TID[]  = "0004000000033500";
static const char    EXPECT_SAVE[] = "00040000/00033500";

/* An id with hex letters in both halves; reversed it reads "0004000E0011C500".
 * title_id keeps them uppercase, save_id must not: azahar writes the on-disk
 * directories with "{:08x}" and case-sensitive storage keeps the two apart. */
static const uint8_t PROGRAM_ID_ALPHA[8] = { 0x00, 0xC5, 0x11, 0x00, 0x0E, 0x00, 0x04, 0x00 };
static const char    ALPHA_TID[]         = "0004000E0011C500";
static const char    ALPHA_SAVE[]        = "0004000e/0011c500";

static int failures = 0;


static uint8_t *make_ncch_with_id(size_t *len, bool with_magic, const uint8_t pid[8]) {
    size_t n = NCCH_PROGRAM_ID_OFFSET + 8 + 0x100;
    uint8_t *b = (uint8_t *)calloc(1, n);
    if (with_magic) memcpy(b + NCSD_MAGIC_OFFSET, "NCCH", 4);
    memcpy(b + NCCH_PROGRAM_ID_OFFSET, pid, 8);
    *len = n;
    return b;
}

static uint8_t *make_ncch(size_t *len, bool with_magic) {
    return make_ncch_with_id(len, with_magic, PROGRAM_ID);
}

/* `partition0` of 0 leaves the NCSD header out entirely, reproducing the
 * headerless stock-layout image the extractor must still read at 0x4118. */
static uint8_t *make_ncsd(size_t *len, uint32_t partition0) {
    uint64_t ncch_off = partition0 ? (uint64_t)partition0 * 0x200 : NCSD_STOCK_PARTITION0;
    size_t n = (size_t)ncch_off + NCCH_PROGRAM_ID_OFFSET + 8 + 0x100;
    uint8_t *b = (uint8_t *)calloc(1, n);
    if (partition0) {
        memcpy(b + NCSD_MAGIC_OFFSET, "NCSD", 4);
        fx_put_le32(b + NCSD_PARTITION_TABLE, partition0);
        memcpy(b + ncch_off + NCSD_MAGIC_OFFSET, "NCCH", 4);
    }
    memcpy(b + ncch_off + NCCH_PROGRAM_ID_OFFSET, PROGRAM_ID, 8);
    *len = n;
    return b;
}

static uint8_t *make_3dsx(size_t *len) {
    size_t n = 0x100;
    uint8_t *b = (uint8_t *)calloc(1, n);
    memcpy(b, "3DSX", 4);
    *len = n;
    return b;
}

static int run(const uint8_t *buf, size_t len, const char *name,
               const sigil_options *opts, sigil_result *out) {
    mem_ctx ctx = { buf, len };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    return sigil_extract_from_io(&io, name, SIGIL_PLATFORM_3DS, opts, out);
}

static void expect_ids(const char *label, const uint8_t *buf, size_t len, const char *name,
                       const char *want_tid, const char *want_save) {
    sigil_result r;
    int rc = run(buf, len, name, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL %s: rc=%d (%s)\n", label, rc, sigil_strerror(rc));
        failures++;
        return;
    }
    if (strcmp(r.title_id, want_tid) != 0) {
        fprintf(stderr, "FAIL %s: title_id='%s' (want '%s')\n", label, r.title_id, want_tid);
        failures++;
        return;
    }
    if (strcmp(r.save_id, want_save) != 0) {
        fprintf(stderr, "FAIL %s: save_id='%s' (want '%s')\n", label, r.save_id, want_save);
        failures++;
        return;
    }
    if (r.usage != SIGIL_USAGE_FOLDER_SPLIT || r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "FAIL %s: usage=%d source=%d\n", label, r.usage, r.source);
        failures++;
        return;
    }
    printf("ok  %s\n", label);
}

static void expect_title(const char *label, const uint8_t *buf, size_t len, const char *name) {
    expect_ids(label, buf, len, name, EXPECT_TID, EXPECT_SAVE);
}

static void expect_no_serial(const char *label, const uint8_t *buf, size_t len, const char *name) {
    sigil_options opts = { SIGIL_OPTIONS_V1, NULL, 0 };
    sigil_result r;
    int rc = run(buf, len, name, &opts, &r);
    if (rc == SIGIL_OK) {
        fprintf(stderr, "FAIL %s: expected no serial, got '%s'\n", label, r.title_id);
        failures++;
        return;
    }
    printf("ok  %s (%s)\n", label, sigil_strerror(rc));
}

static void wrapped(const char *label, const uint8_t *inner, size_t inner_len,
                    const char magic[4], const uint8_t *meta, size_t meta_len,
                    size_t frame_size, const char *name, bool expect_ok) {
    size_t len = 0;
    uint8_t *z = z3ds_build(inner, inner_len, magic, meta, meta_len, frame_size, &len);
    if (!z) {
        fprintf(stderr, "FAIL %s: fixture allocation failed\n", label);
        failures++;
        return;
    }
    if (expect_ok) expect_title(label, z, len, name);
    else           expect_no_serial(label, z, len, name);
    free(z);
}

int main(void) {
    size_t len;
    const uint8_t meta[] = { 1, 10, 4, 0, 'c', 'o', 'm', 'p', 'r', 'e', 's', 's', 'o', 'r',
                             't', 'e', 's', 't', 0 };

    uint8_t *ncsd_stock = make_ncsd(&len, 0);
    size_t   ncsd_stock_len = len;
    expect_title("ncsd stock layout (.3ds)", ncsd_stock, ncsd_stock_len, "game.3ds");

    uint8_t *ncsd_tab = make_ncsd(&len, 0x40);
    size_t   ncsd_tab_len = len;
    expect_title("ncsd partition table (.cci)", ncsd_tab, ncsd_tab_len, "game.cci");

    uint8_t *ncch = make_ncch(&len, true);
    size_t   ncch_len = len;
    expect_title("ncch magic (.cxi)", ncch, ncch_len, "game.cxi");
    expect_title("ncch magic (.app)", ncch, ncch_len, "00000000.app");

    uint8_t *ncch_alpha = make_ncch_with_id(&len, true, PROGRAM_ID_ALPHA);
    size_t   ncch_alpha_len = len;
    expect_ids("hex letters lowercase in save_id only", ncch_alpha, ncch_alpha_len,
               "game.cxi", ALPHA_TID, ALPHA_SAVE);
    {
        sigil_result r;
        int arc = run(ncch_alpha, ncch_alpha_len, "game.cxi", NULL, &r);
        if (arc != SIGIL_OK) {
            fprintf(stderr, "FAIL hex letters: rc=%d (%s)\n", arc, sigil_strerror(arc));
            failures++;
        } else if (strcmp(r.raw_serial, ALPHA_TID) != 0) {
            fprintf(stderr, "FAIL hex letters: raw_serial='%s' (want '%s')\n",
                    r.raw_serial, ALPHA_TID);
            failures++;
        } else {
            printf("ok  hex letters leave raw_serial uppercase\n");
        }
    }

    uint8_t *ncch_bare = make_ncch(&len, false);
    size_t   ncch_bare_len = len;
    expect_title("ncch by extension (.cxi)", ncch_bare, ncch_bare_len, "game.cxi");
    expect_no_serial("ncch bytes read as ncsd (.3ds)", ncch_bare, ncch_bare_len, "game.3ds");

    uint8_t *hb = make_3dsx(&len);
    size_t   hb_len = len;
    expect_no_serial("3dsx homebrew", hb, hb_len, "hello.3dsx");

    wrapped("z3ds wraps ncsd, no metadata", ncsd_stock, ncsd_stock_len, "NCSD",
            NULL, 0, 0, "game.z3ds", true);
    wrapped("z3ds wraps ncsd, metadata + multi-frame", ncsd_tab, ncsd_tab_len, "NCSD",
            meta, sizeof(meta), 0x1000, "game.zcci", true);
    wrapped("zcxi wraps ncch", ncch, ncch_len, "NCCH",
            meta, sizeof(meta), 0x800, "game.zcxi", true);
    wrapped("underlying magic beats extension", ncch, ncch_len, "NCCH",
            meta, sizeof(meta), 0, "mislabelled.z3ds", true);
    wrapped("z3dsx wraps homebrew", hb, hb_len, "3DSX",
            NULL, 0, 0, "hello.z3dsx", false);

    /* A wrapper header whose payload offset ignored metadata_size would start
     * decompression inside the metadata block and decode nothing. */
    size_t zlen = 0;
    uint8_t *z = z3ds_build(ncsd_stock, ncsd_stock_len, "NCSD", meta, sizeof(meta), 0, &zlen);
    if (z) {
        if (z[12] == 0 && z[13] == 0 && z[14] == 0 && z[15] == 0) {
            fprintf(stderr, "FAIL fixture: metadata_size not recorded\n");
            failures++;
        }
        z[0] = 'X';
        expect_no_serial("corrupt wrapper magic falls through to raw", z, zlen, "game.z3ds");
        free(z);
    }

    free(ncsd_stock);
    free(ncsd_tab);
    free(ncch);
    free(ncch_alpha);
    free(ncch_bare);
    free(hb);

    if (failures) {
        fprintf(stderr, "unit_3ds: %d failure(s)\n", failures);
        return 1;
    }
    printf("ok unit_3ds\n");
    return 0;
}
