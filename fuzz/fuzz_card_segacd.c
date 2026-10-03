// SPDX-License-Identifier: MPL-2.0
#include "card_segacd.h"
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

/* A listed save verifies; the first, deleted, goes back in and verifies. */
static void exercise(sigil_segacd_volume *vol) {
    sigil_card_listing *listing = NULL;
    if (sigil_segacd_list(vol, &listing) == SIGIL_OK) {
        for (size_t i = 0; i < listing->entry_count; i++) {
            size_t len = sigil_segacd_unit_size(listing->entries[i].blocks);
            uint8_t *unit = (uint8_t *)malloc(len);
            if (unit && sigil_segacd_extract(vol, listing->entries[i].first_block,
                                             listing->entries[i].blocks, unit) == SIGIL_OK) {
                if (sigil_segacd_verify(vol, unit, len) != SIGIL_OK) broken("a listed save doesn't verify");
                if (i == 0 && sigil_segacd_delete(vol, listing->entries[i].first_block) == SIGIL_OK) {
                    if (sigil_segacd_inject(vol, unit, len) != SIGIL_OK) broken("a deleted save didn't go back");
                    if (sigil_segacd_verify(vol, unit, len) != SIGIL_OK) broken("a reinjected save doesn't verify");
                }
            }
            free(unit);
        }
    }
    sigil_card_listing_free(listing);
    uint8_t *out = NULL;
    size_t out_len = 0;
    sigil_segacd_volume_write(vol, &out, &out_len);
    free(out);
}

/* Lists the input as a volume and as a combined file's cart part, extracts
 * and verifies every save, deletes the first and injects it again; injects the input as a unit
 * into an empty internal volume; and decodes its first block. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    mem_ctx m = { data, size };
    sigil_io io = { mem_read, mem_size, NULL, &m };

    sigil_segacd_volume vol;
    if (sigil_segacd_volume_load(&io, &vol) == SIGIL_OK) {
        exercise(&vol);
        sigil_segacd_volume_free(&vol);
    }
    if (sigil_segacd_volume_load_cart(&io, &vol) == SIGIL_OK) {
        exercise(&vol);
        sigil_segacd_volume_free(&vol);
    }

    static const sigil_bram_storage raw = {0};
    if (sigil_segacd_volume_format(&vol, SEGACD_INTERNAL_SIZE, &raw) == SIGIL_OK) {
        if (sigil_segacd_inject(&vol, data, size) == SIGIL_OK) {
            if (sigil_segacd_verify(&vol, data, size) != SIGIL_OK) broken("an injected save doesn't verify");
            exercise(&vol);
        }
        sigil_segacd_volume_free(&vol);
    }

    if (size >= SEGACD_BLOCK_SIZE) {
        uint8_t payload[SEGACD_PAYLOAD_SIZE];
        sigil_segacd_decode_block(data, payload);
    }
    return 0;
}
