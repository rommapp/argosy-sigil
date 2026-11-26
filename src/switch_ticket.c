// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

/* A package's tickets, read as Eden reads them (at 56e628a:
 * src/core/file_sys/submission_package.cpp NSP::SetTicketKeys,
 * src/core/crypto/key_manager.h TicketData, src/core/crypto/key_manager.cpp
 * Ticket::Read and KeyManager::ParseTicketTitleKey): every file whose
 * extension is exactly "tik" holds a little-endian u32 signature type, the
 * signature and its padding, then the ticket data, with the issuer at 0x0 and
 * the rights id at 0x160 (also switchbrew "Ticket", oldid 12553). Eden drops
 * a ticket of a signature type it doesn't know, and one whose issuer or
 * rights id is all zero. Eden opens a game card's secure partition as an NSP
 * (card_image.cpp), so a ticket there reads the same way. On a card the game
 * and its DLC carry no rights id, and only an update bundled with the game
 * carries a ticket (nxdumptool dumper.c at e33408b, the comments at lines
 * 1095 and 1115): a card's content is its game, whichever ticket it holds.
 * nsz rewrites only the NCAs it compresses and copies every other file
 * unchanged (SolidCompressor.py processContainer at 148218f), so an NSZ or
 * XCZ keeps its tickets.
 *
 * Only the signature type, the issuer and the rights id are read: never the
 * title key block, the device id or the account id. */

#define TICKET_ISSUER_SIZE      0x40
#define TICKET_RIGHTS_ID_OFFSET 0x160
#define TICKET_RIGHTS_ID_SIZE   0x10

/* The signature and its padding after the type (key_manager.cpp
 * GetSignatureTypeDataSize and GetSignatureTypePaddingSize); 0 for a type
 * Eden doesn't read. */
static uint64_t ticket_signature_size(uint32_t type) {
    switch (type) {
    case 0x10000: /* RSA-4096, SHA-1 */
    case 0x10003: /* RSA-4096, SHA-256 */
        return 0x200 + 0x3C;
    case 0x10001: /* RSA-2048, SHA-1 */
    case 0x10004: /* RSA-2048, SHA-256 */
        return 0x100 + 0x3C;
    case 0x10002: /* ECDSA, SHA-1 */
    case 0x10005: /* ECDSA, SHA-256 */
        return 0x3C + 0x40;
    default:
        return 0;
    }
}

static bool name_is_ticket(const uint8_t *name, size_t name_len) {
    return name_len >= 4 && memcmp(name + name_len - 4, ".tik", 4) == 0;
}

/* The title id of the ticket at `off`, `size` bytes long. */
static int ticket_title_id(const sigil_io *io, uint64_t off, uint64_t size, char out[17]) {
    uint8_t type[4];
    if (size < sizeof type || sigil_io_read_exact(io, off, type, sizeof type) != SIGIL_OK) {
        return SIGIL_ERR_NOT_FOUND;
    }
    uint64_t signature = ticket_signature_size(sigil_read_le32(type));
    if (signature == 0) return SIGIL_ERR_NOT_FOUND;

    uint64_t data = sizeof type + signature;
    if (size < data + TICKET_RIGHTS_ID_OFFSET + TICKET_RIGHTS_ID_SIZE) return SIGIL_ERR_NOT_FOUND;

    uint8_t issuer[TICKET_ISSUER_SIZE];
    if (sigil_io_read_exact(io, off + data, issuer, sizeof issuer) != SIGIL_OK) {
        return SIGIL_ERR_NOT_FOUND;
    }
    bool named = false;
    for (size_t i = 0; i < sizeof issuer; i++) { if (issuer[i] != 0) { named = true; break; } }
    if (!named) return SIGIL_ERR_NOT_FOUND;

    uint8_t rights_id[TICKET_RIGHTS_ID_SIZE];
    if (sigil_io_read_exact(io, off + data + TICKET_RIGHTS_ID_OFFSET, rights_id,
                            sizeof rights_id) != SIGIL_OK) {
        return SIGIL_ERR_NOT_FOUND;
    }
    return sigil_switch_title_from_rights_id(rights_id, out);
}

int sigil_switch_tickets_title(const sigil_io *io, uint64_t data_start,
                               const uint8_t *entries, uint32_t file_count, size_t entry_size,
                               const uint8_t *string_table, uint32_t string_table_size,
                               bool game_card, sigil_switch_title *out) {
    bool have = false, own = false;
    char game[17], first[17];
    for (uint32_t i = 0; i < file_count; i++) {
        const uint8_t *e = entries + (size_t)i * entry_size;
        uint64_t file_offset = sigil_read_le64(e);
        uint64_t file_size   = sigil_read_le64(e + 8);
        uint32_t name_offset = sigil_read_le32(e + 16);
        if (name_offset >= string_table_size) continue;

        uint32_t name_end = name_offset;
        while (name_end < string_table_size && string_table[name_end] != 0) name_end++;
        if (!name_is_ticket(string_table + name_offset, name_end - name_offset)) continue;

        char id[17], app[17];
        if (ticket_title_id(io, data_start + file_offset, file_size, id) != SIGIL_OK) continue;
        sigil_switch_application_of_id(id, app);
        if (!have) {
            memcpy(game, app, sizeof game);
            memcpy(first, id, sizeof first);
            have = true;
        } else if (strcmp(app, game) != 0) {
            return SIGIL_ERR_NEEDS_KEY; /* two games: the CNMTs decide, with keys */
        }
        if (strcmp(id, app) == 0) own = true;
    }
    if (!have) return SIGIL_ERR_NEEDS_KEY;

    memset(out, 0, sizeof(*out));
    memcpy(out->title_id, own || game_card ? game : first, sizeof out->title_id);
    memcpy(out->application_id, game, sizeof out->application_id);
    return SIGIL_OK;
}
