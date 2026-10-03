// SPDX-License-Identifier: MPL-2.0
#include "card_ps1.h"
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

static void broken(const char *what) {
    fprintf(stderr, "oracle: %s\n", what);
    abort();
}

/* The input read as a card file and written back in its own form must read
 * back as the same card, in the same form, a .vmp signed for it. */
static void check_write(const uint8_t *data, size_t size) {
    mem_ctx m = { data, size };
    sigil_io io = { mem_read, mem_size, NULL, &m };
    sigil_ps1_file *f = (sigil_ps1_file *)malloc(sizeof(*f));
    sigil_ps1_file *back = (sigil_ps1_file *)malloc(sizeof(*back));
    uint8_t *out = NULL;
    size_t len = 0;
    if (f && back && sigil_ps1_file_load(&io, f) == SIGIL_OK) {
        sigil_ps1_file_check(f);
        if (sigil_ps1_file_write(f, &out, &len) != SIGIL_OK) broken("a card that read didn't write");
        mem_ctx w = { out, len };
        sigil_io wio = { mem_read, mem_size, NULL, &w };
        if (sigil_ps1_file_load(&wio, back) != SIGIL_OK) broken("a written card didn't read back");
        if (back->format != f->format || memcmp(back->image, f->image, PS1_CARD_SIZE) != 0) {
            broken("a written card read back as another card");
        }
        if (sigil_ps1_file_check(back) != SIGIL_OK) broken("a written .vmp isn't signed for its card");
    }
    free(out);
    free(back);
    free(f);
}

/* Lists the input as a card, extracts every save it lists, writes it back in
 * its own form, and injects the input as a .mcs into an empty card. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    check_write(data, size);
    mem_ctx m = { data, size };
    sigil_io io = { mem_read, mem_size, NULL, &m };
    uint8_t *image = (uint8_t *)malloc(PS1_CARD_SIZE);
    if (!image) return 0;

    int format = 0;
    sigil_card_listing *listing = NULL;
    if (sigil_ps1_card_load(&io, image, &format) == SIGIL_OK &&
        sigil_ps1_card_list(image, format, &listing) == SIGIL_OK) {
        for (size_t i = 0; i < listing->entry_count; i++) {
            size_t len = sigil_ps1_mcs_size(listing->entries[i].blocks);
            uint8_t *mcs = (uint8_t *)malloc(len);
            if (mcs && sigil_ps1_extract(image, listing->entries[i].first_block,
                                         listing->entries[i].blocks, mcs) == SIGIL_OK) {
                sigil_ps1_verify(image, mcs, len);
            }
            free(mcs);
        }
    }
    sigil_card_listing_free(listing);

    sigil_ps1_format(image);
    if (sigil_ps1_inject(image, data, size) == SIGIL_OK) sigil_ps1_verify(image, data, size);
    free(image);
    return 0;
}
