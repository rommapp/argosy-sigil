// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_CARD_H
#define SIGIL_CARD_H

#include "sigil_internal.h"

/**
 * Lists one card format from `io`. Returns SIGIL_ERR_UNSUPPORTED_FORMAT when
 * the stream is not that format, so sigil_card_list can try the next reader.
 */
typedef int (*sigil_card_reader)(const sigil_io *io, sigil_card_listing **out);

/**
 * Allocates an empty listing with room for `capacity` entries. Card readers
 * fill it and hand it to the caller, who frees it with sigil_card_listing_free.
 */
sigil_card_listing *sigil_card_listing_new(int format, size_t capacity);

/**
 * Counts a save left out as corrupt. When the card still names it, `name`
 * and `owner_id` (either may be NULL) go in corrupt_entries with `first_block`.
 */
void sigil_card_listing_corrupt(sigil_card_listing *listing, const char *name, const char *owner_id,
                                uint32_t first_block);

/**
 * The product code a PS1 or PS2 save name carries, as disc identification
 * reports it: "BASLUSP01041..." and "BASLUS-01041..." both give
 * "SLUS-01041". Empty when the name has no region prefix and code.
 */
void sigil_card_sony_owner(const char *name, char out[SIGIL_CARD_OWNER_MAX]);

#endif
