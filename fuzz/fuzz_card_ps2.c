// SPDX-License-Identifier: MPL-2.0
#include "card_ps2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const uint8_t *data; size_t len; } mem_ctx;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_ctx *m = (mem_ctx *)ctx;
    if (off >= m->len) return 0;
    size_t n = m->len - (size_t)off < len ? m->len - (size_t)off : len;
    memcpy(buf, m->data + off, n);
    return (int)n;
}

static int64_t mem_size(void *ctx) { return (int64_t)((mem_ctx *)ctx)->len; }

/* Packs a folder whose _pcsx2_index is the input, then unpacks the result. */
static void fuzz_folder(const uint8_t *data, size_t size) {
    static const uint8_t payload[3] = { 1, 2, 3 };
    sigil_ps2_folder_file files[3];
    memset(files, 0, sizeof(files));
    snprintf(files[0].path, sizeof(files[0].path), "_pcsx2_index");
    files[0].data = (uint8_t *)data;
    files[0].len = size;
    snprintf(files[1].path, sizeof(files[1].path), "icon.sys");
    files[1].data = (uint8_t *)payload;
    files[1].len = sizeof(payload);
    snprintf(files[2].path, sizeof(files[2].path), "_pcsx2_meta/icon.sys");
    files[2].data = (uint8_t *)data;
    files[2].len = size;

    sigil_ps2_save save;
    if (sigil_ps2_pack("BASLUS-00000", files, 3, &save) != SIGIL_OK) return;
    sigil_ps2_folder_file *out = NULL;
    size_t n = 0;
    if (sigil_ps2_unpack(&save, &out, &n) == SIGIL_OK) sigil_ps2_folder_files_free(out, n);
    sigil_ps2_save_free(&save);
}

/* Loads the input as a .ps2 card, lists it, and moves every save it lists
 * onto the same card again after deleting it. Small inputs go through the
 * folder packer as an index instead. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 65536) {
        fuzz_folder(data, size);
        return 0;
    }
    mem_ctx m = { data, size };
    sigil_io io = { mem_read, mem_size, NULL, &m };
    sigil_ps2_card card;
    if (sigil_ps2_card_load(&io, &card) != SIGIL_OK) return 0;

    sigil_card_listing *listing = NULL;
    if (sigil_ps2_card_list(&card, &listing) == SIGIL_OK) {
        for (size_t i = 0; i < listing->entry_count; i++) {
            sigil_ps2_save save;
            if (sigil_ps2_extract(&card, listing->entries[i].first_block, &save) != SIGIL_OK) continue;
            sigil_ps2_verify(&card, &save);
            if (sigil_ps2_delete(&card, listing->entries[i].first_block) == SIGIL_OK) {
                sigil_ps2_inject(&card, &save);
            }
            sigil_ps2_save_free(&save);
        }
    }
    sigil_card_listing_free(listing);

    uint8_t *out = (uint8_t *)malloc(sigil_ps2_card_file_size(&card));
    if (out) sigil_ps2_card_write(&card, out);
    free(out);
    sigil_ps2_card_free(&card);
    return 0;
}
