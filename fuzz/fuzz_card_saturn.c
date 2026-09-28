// SPDX-License-Identifier: MPL-2.0
#include "card_saturn.h"
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

/* Lists the input as a volume, extracts and verifies every save it lists,
 * deletes the first and writes the volume back; then injects the input as a
 * .BUP into an empty internal volume. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    mem_ctx m = { data, size };
    sigil_io io = { mem_read, mem_size, NULL, &m };

    sigil_saturn_volume vol;
    sigil_card_listing *listing = NULL;
    if (sigil_saturn_volume_load(&io, &vol) == SIGIL_OK) {
        if (sigil_saturn_list(&vol, &listing) == SIGIL_OK) {
            for (size_t i = 0; i < listing->entry_count; i++) {
                uint8_t *bup = NULL;
                size_t len = 0;
                if (sigil_saturn_extract(&vol, listing->entries[i].first_block, &bup, &len) == SIGIL_OK) {
                    sigil_saturn_verify(&vol, bup, len);
                }
                free(bup);
            }
            if (listing->entry_count > 0) sigil_saturn_delete(&vol, listing->entries[0].first_block);
        }
        uint8_t *out = NULL;
        size_t out_len = 0;
        sigil_saturn_volume_write(&vol, &out, &out_len);
        free(out);
        sigil_saturn_volume_free(&vol);
    }
    sigil_card_listing_free(listing);

    static const sigil_bram_storage raw = {0};
    if (sigil_saturn_volume_format(&vol, SATURN_INTERNAL_SIZE, &raw) == SIGIL_OK) {
        if (sigil_saturn_inject(&vol, data, size) == SIGIL_OK) sigil_saturn_verify(&vol, data, size);
        sigil_saturn_volume_free(&vol);
    }
    return 0;
}
