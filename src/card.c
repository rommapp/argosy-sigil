// SPDX-License-Identifier: MPL-2.0
#include "card.h"
#include "card_dreamcast.h"
#include "card_gamecube.h"
#include "card_ps1.h"
#include "card_ps2.h"
#include "card_saturn.h"
#include "card_segacd.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

/* Each reader checks its own magic or layout. The ones with a fixed magic go
 * first; the Dreamcast VMU, which is recognised by its layout alone, goes last. */
static const sigil_card_reader READERS[] = {
    sigil_ps1_card_list_io,
    sigil_ps2_card_list_io,
    sigil_gamecube_card_list_io,
    sigil_saturn_card_list_io,
    sigil_segacd_card_list_io,
    sigil_dreamcast_card_list_io,
};

sigil_card_listing *sigil_card_listing_new(int format, size_t capacity) {
    sigil_card_listing *listing = (sigil_card_listing *)calloc(1, sizeof(*listing));
    if (!listing) return NULL;
    if (capacity > 0) {
        listing->entries = (sigil_card_entry *)calloc(capacity, sizeof(sigil_card_entry));
        listing->corrupt_entries = (sigil_card_entry *)calloc(capacity, sizeof(sigil_card_entry));
        if (!listing->entries || !listing->corrupt_entries) {
            sigil_card_listing_free(listing);
            return NULL;
        }
    }
    listing->struct_version = SIGIL_CARD_LISTING_V1;
    listing->format = format;
    return listing;
}

void sigil_card_listing_corrupt(sigil_card_listing *listing, const char *name, const char *owner_id,
                                uint32_t first_block) {
    listing->corrupt_count++;
    if (!name || !listing->corrupt_entries) return;
    sigil_card_entry *e = &listing->corrupt_entries[listing->corrupt_entry_count++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->owner_id, sizeof(e->owner_id), "%s", owner_id ? owner_id : "");
    e->first_block = first_block;
}

void sigil_card_sony_owner(const char *name, char out[SIGIL_CARD_OWNER_MAX]) {
    out[0] = '\0';
    if (strlen(name) < 12 || name[0] != 'B') return;
    if (name[1] != 'A' && name[1] != 'E' && name[1] != 'I') return;
    const char *code = name + 2;
    for (int i = 0; i < 4; i++) {
        if (!isupper((unsigned char)code[i])) return;
    }
    for (int i = 5; i < 10; i++) {
        if (!isdigit((unsigned char)code[i])) return;
    }
    memcpy(out, code, 10);
    out[4] = '-';
    out[10] = '\0';
}

int sigil_card_list(const sigil_io *io, sigil_card_listing **out) {
    if (!io || !io->read || !out) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    for (size_t i = 0; i < sizeof(READERS) / sizeof(READERS[0]); i++) {
        int rc = READERS[i](io, out);
        if (rc != SIGIL_ERR_UNSUPPORTED_FORMAT) return rc;
    }
    return SIGIL_ERR_UNSUPPORTED_FORMAT;
}

void sigil_card_listing_free(sigil_card_listing *listing) {
    if (!listing) return;
    free(listing->entries);
    free(listing->corrupt_entries);
    free(listing);
}
