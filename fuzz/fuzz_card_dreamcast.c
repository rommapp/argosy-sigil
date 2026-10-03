// SPDX-License-Identifier: MPL-2.0
#include "card_dreamcast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VMI_LEN 108u

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

static void inject_and_verify(uint8_t *image, const uint8_t *dci, size_t len) {
    if (sigil_dreamcast_inject(image, dci, len) == SIGIL_OK && sigil_dreamcast_verify(image, dci, len) != SIGIL_OK) {
        broken("an injected save doesn't verify");
    }
}

/* Lists the input as a VMU, extracts and verifies every file it lists, then
 * reinjects the first file after deleting it. Separately injects the input
 * into an empty VMU as a .dci, and as a .vmi followed by its .vms. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    mem_ctx m = { data, size };
    sigil_io io = { mem_read, mem_size, NULL, &m };
    uint8_t *image = (uint8_t *)malloc(VMU_CARD_SIZE);
    if (!image) return 0;

    sigil_card_listing *listing = NULL;
    if (sigil_dreamcast_card_load(&io, image) == SIGIL_OK &&
        sigil_dreamcast_card_list(image, &listing) == SIGIL_OK) {
        uint8_t *first = NULL;
        size_t first_len = 0;
        for (size_t i = 0; i < listing->entry_count; i++) {
            const sigil_card_entry *e = &listing->entries[i];
            size_t len = sigil_dreamcast_dci_size(e->blocks);
            uint8_t *dci = (uint8_t *)malloc(len);
            if (dci && sigil_dreamcast_extract(image, e->first_block, e->blocks, dci) == SIGIL_OK) {
                if (sigil_dreamcast_verify(image, dci, len) != SIGIL_OK) broken("a listed save doesn't verify");
                if (!first) { first = dci; first_len = len; dci = NULL; }
            }
            free(dci);
        }
        if (first && sigil_dreamcast_delete(image, listing->entries[0].first_block) == SIGIL_OK) {
            if (sigil_dreamcast_inject(image, first, first_len) != SIGIL_OK) broken("a deleted save didn't go back");
            if (sigil_dreamcast_verify(image, first, first_len) != SIGIL_OK) broken("a reinjected save doesn't verify");
        }
        free(first);
    }
    sigil_card_listing_free(listing);

    sigil_dreamcast_format(image);
    inject_and_verify(image, data, size);

    if (size > VMI_LEN) {
        size_t vms_len = size - VMI_LEN;
        size_t len = sigil_dreamcast_dci_size((uint32_t)(vms_len / VMU_BLOCK_SIZE));
        uint8_t *dci = (uint8_t *)malloc(len);
        if (dci && sigil_dreamcast_dci_from_vms(data + VMI_LEN, vms_len, data, VMI_LEN, dci) == SIGIL_OK) {
            sigil_dreamcast_format(image);
            inject_and_verify(image, dci, len);
        }
        free(dci);
    }
    free(image);
    return 0;
}
