// SPDX-License-Identifier: MPL-2.0
#include "card_gamecube.h"
#include <stdlib.h>
#include <string.h>

#define SMALL_CARD_SIZE (64u * GC_BLOCK_SIZE)

typedef struct { const uint8_t *data; size_t len; } mem_ctx;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_ctx *m = (mem_ctx *)ctx;
    if (off >= m->len) return 0;
    size_t n = m->len - (size_t)off < len ? m->len - (size_t)off : len;
    memcpy(buf, m->data + off, n);
    return (int)n;
}

static int64_t mem_size(void *ctx) { return (int64_t)((mem_ctx *)ctx)->len; }

static void store_checksum(const uint8_t *data, size_t len, uint8_t *at) {
    uint16_t sum, inverse;
    sigil_gamecube_checksum(data, len, &sum, &inverse);
    at[0] = (uint8_t)(sum >> 8);
    at[1] = (uint8_t)sum;
    at[2] = (uint8_t)(inverse >> 8);
    at[3] = (uint8_t)inverse;
}

/* Makes every system block's checksum match its mutated contents, so the
 * mutations reach the directory and allocation table walks. */
static void reseal_system_blocks(uint8_t *image) {
    store_checksum(image, 0x1FC, image + 0x1FC);
    for (size_t copy = 0; copy < 2; copy++) {
        uint8_t *dir = image + (1 + copy) * GC_BLOCK_SIZE;
        store_checksum(dir, 0x1FFC, dir + 0x1FFC);
        uint8_t *bat = image + (3 + copy) * GC_BLOCK_SIZE;
        store_checksum(bat + 4, GC_BLOCK_SIZE - 4, bat);
    }
}

/* Extracts, verifies, binds, deletes and re-injects every save the card lists. */
static void exercise_card(uint8_t *image, size_t size) {
    sigil_card_listing *listing = NULL;
    if (sigil_gamecube_card_list(image, size, &listing) != SIGIL_OK) return;
    for (size_t i = 0; i < listing->entry_count; i++) {
        const sigil_card_entry *e = &listing->entries[i];
        size_t len = sigil_gamecube_gci_size(e->blocks);
        uint8_t *gci = (uint8_t *)malloc(len);
        if (gci && sigil_gamecube_extract(image, size, e->first_block, e->blocks, gci) == SIGIL_OK) {
            sigil_gamecube_verify(image, size, gci, len);
            sigil_gamecube_bind_serial(image, size, e->first_block);
            if (sigil_gamecube_delete(image, size, e->first_block) == SIGIL_OK) {
                sigil_gamecube_inject(image, size, gci, len);
            }
        }
        free(gci);
    }
    sigil_card_listing_free(listing);
}

/* Lists the input as a card as sigil_card_list would, then again with its
 * checksums repaired, and converts the input as a single save and injects it
 * into an empty card. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    mem_ctx m = { data, size };
    sigil_io io = { mem_read, mem_size, NULL, &m };

    uint8_t *image = NULL;
    size_t image_size = 0;
    if (sigil_gamecube_card_load(&io, &image, &image_size) == SIGIL_OK) exercise_card(image, image_size);
    free(image);

    if (size >= 5 * GC_BLOCK_SIZE && size <= GC_MAX_CARD_SIZE && size % GC_BLOCK_SIZE == 0) {
        uint8_t *resealed = (uint8_t *)malloc(size);
        if (resealed) {
            memcpy(resealed, data, size);
            reseal_system_blocks(resealed);
            exercise_card(resealed, size);
        }
        free(resealed);
    }

    uint8_t *gci = (uint8_t *)malloc(size ? size : 1);
    uint8_t *card = (uint8_t *)malloc(SMALL_CARD_SIZE);
    size_t gci_len = 0;
    if (gci && card && sigil_gamecube_to_gci(data, size, gci, &gci_len) == SIGIL_OK &&
        sigil_gamecube_format(card, SMALL_CARD_SIZE, false) == SIGIL_OK &&
        sigil_gamecube_inject(card, SMALL_CARD_SIZE, gci, gci_len) == SIGIL_OK) {
        sigil_gamecube_verify(card, SMALL_CARD_SIZE, gci, gci_len);
        exercise_card(card, SMALL_CARD_SIZE);
    }
    free(card);
    free(gci);
    return 0;
}
