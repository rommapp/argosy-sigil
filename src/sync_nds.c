// SPDX-License-Identifier: MPL-2.0
/* Collect and restore for Nintendo DS carts. The save chip's bytes are the
 * same in every emulator: melonDS keeps them raw, and DeSmuME keeps them in a
 * .dsv followed by a 122-byte footer (mc.cpp). The unit is the raw bytes;
 * restore writes what the layout's emulator writes: DeSmuME's .dsv, or the
 * raw bytes. */
#include "sync_internal.h"

#define NDS_MAX_SAVE   (64u * 1024u * 1024u)
#define DSV_NOTE       "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:"
#define DSV_COOKIE     "|-DESMUME SAVE-|"
#define DSV_NOTE_LEN   (sizeof(DSV_NOTE) - 1)
#define DSV_COOKIE_LEN (sizeof(DSV_COOKIE) - 1)
#define DSV_FIELDS     24u
#define DSV_FOOTER     (DSV_NOTE_LEN + DSV_FIELDS + DSV_COOKIE_LEN)

/* DeSmuME's save types (mc.cpp save_types[]): a .dsv footer's type is the
 * index of the padded size here, and the address size is how many bytes the
 * game's commands address the chip with. 0 is autodetect. */
static const struct { uint32_t size; uint32_t addr_size; } DSV_TYPES[] = {
    { 0, 0 },
    { 512u, 1 },                      /* EEPROM 4 kbit */
    { 8u * 1024u, 2 },                /* EEPROM 64 kbit */
    { 64u * 1024u, 2 },               /* EEPROM 512 kbit */
    { 32u * 1024u, 2 },               /* FRAM 256 kbit */
    { 256u * 1024u, 3 },              /* FLASH 2 Mbit */
    { 512u * 1024u, 3 },
    { 1024u * 1024u, 3 },
    { 2048u * 1024u, 3 },
    { 4096u * 1024u, 3 },
    { 8192u * 1024u, 3 },
    { 16384u * 1024u, 3 },
    { 32768u * 1024u, 3 },
    { 65536u * 1024u, 3 },            /* FLASH 512 Mbit */
};
#define DSV_TYPE_COUNT (sizeof(DSV_TYPES) / sizeof(DSV_TYPES[0]))

/* The size DeSmuME pads a save to (mc.cpp pad_up_size over saveSizes[]): the
 * smallest of its sizes that holds `len`, or `len` past the largest. */
static uint32_t dsv_padded(uint32_t len) {
    uint32_t best = 0;
    for (size_t i = 1; i < DSV_TYPE_COUNT; i++) {
        uint32_t size = DSV_TYPES[i].size;
        if (size >= len && (!best || size < best)) best = size;
    }
    return best ? best : len;
}

static uint32_t dsv_type(uint32_t padded) {
    for (size_t i = 1; i < DSV_TYPE_COUNT; i++) {
        if (DSV_TYPES[i].size == padded) return (uint32_t)i;
    }
    return 0;
}

/* The layouts whose file carries DeSmuME's footer. Any other layout keeps the
 * raw save whatever its file is called: DraStic also writes .dsv. */
static bool footer_layout(const char *layout) {
    static const char *const LAYOUTS[] = { "desmume" };
    for (size_t i = 0; layout && i < sizeof(LAYOUTS) / sizeof(LAYOUTS[0]); i++) {
        if (strcmp(LAYOUTS[i], layout) == 0) return true;
    }
    return false;
}

/* The save's length in `data`: all of it, or before the footer on a DeSmuME
 * file. DeSmuME takes a footer by its cookie and version 0 alone (mc.cpp
 * readFooter); a .dsv without one, as DraStic writes, is raw. */
static size_t raw_len(const uint8_t *data, size_t len) {
    if (len < DSV_FOOTER) return len;
    const uint8_t *cookie = data + len - DSV_COOKIE_LEN;
    if (memcmp(cookie, DSV_COOKIE, DSV_COOKIE_LEN) != 0 || sigil_read_le32(cookie - 4) != 0) return len;
    return len - DSV_FOOTER;
}

/* The bytes restore writes for `layout`: the raw save, or for DeSmuME what
 * DeSmuME writes when it imports a raw save (mc.cpp import_raw, saveBuffer,
 * ensure): the save padded with 0xFF to its size for it, then the footer. A
 * size it has no type for (1 Mbit) is padded to the next one, 2 Mbit, as
 * DeSmuME pads it; its file read with no footer would count as empty. */
static int file_bytes(const char *layout, const uint8_t *raw, size_t len, uint8_t **out, size_t *out_len) {
    bool dsv = footer_layout(layout);
    uint32_t padded = dsv ? dsv_padded((uint32_t)len) : (uint32_t)len;
    *out_len = (size_t)padded + (dsv ? DSV_FOOTER : 0);
    *out = (uint8_t *)malloc(*out_len ? *out_len : 1);
    if (!*out) return SIGIL_ERR_OOM;
    memcpy(*out, raw, len);
    if (!dsv) return SIGIL_OK;
    memset(*out + len, 0xFF, padded - len);
    uint8_t *note = *out + padded, *fields = note + DSV_NOTE_LEN;
    uint32_t type = dsv_type(padded);
    memcpy(note, DSV_NOTE, DSV_NOTE_LEN);
    sigil_write_le32(fields, (uint32_t)len);              /* bytes the save holds */
    sigil_write_le32(fields + 4, padded);                 /* the size it's padded to */
    sigil_write_le32(fields + 8, type);
    sigil_write_le32(fields + 12, DSV_TYPES[type].addr_size);
    sigil_write_le32(fields + 16, padded);                /* the chip's size */
    sigil_write_le32(fields + 20, 0);                     /* version */
    memcpy(fields + DSV_FIELDS, DSV_COOKIE, DSV_COOKIE_LEN);
    return SIGIL_OK;
}

/* The raw save in the game's file, NULL when there's none: a file of
 * DeSmuME's footer alone holds no save, as an empty one doesn't. */
static int read_raw(const sigil_sync_request *req, const sigil_sync_cart_file *f, uint8_t **raw, size_t *len) {
    int rc = sigil_sync_cart_read(req, f, NDS_MAX_SAVE + DSV_FOOTER, raw, len);
    if (*raw) *len = raw_len(*raw, *len);
    if (*raw && !*len) {
        free(*raw);
        *raw = NULL;
    }
    return rc;
}

int sigil_sync_collect_nds(sigil_sync_ctx *x, sigil_sync_result *r) {
    sigil_sync_cart_file f;
    uint8_t *raw = NULL;
    size_t len = 0;
    int rc = sigil_sync_cart_file_of(x->req, &f);
    if (rc == SIGIL_OK) rc = read_raw(x->req, &f, &raw, &len);
    if (rc == SIGIL_OK && raw) {
        sigil_md5_of(raw, len, r->identity_hash);
        memcpy(r->content_hash, r->identity_hash, sizeof(r->content_hash));
        r->data = raw;
        r->len = len;
        r->shape = SIGIL_SAVE_SHAPE_SINGLE;
        sigil_sync_cart_artifact(x->req, ".srm", r);
        raw = NULL;
    }
    free(raw);
    return rc;
}

int sigil_sync_restore_nds(sigil_sync_ctx *x, const uint8_t *unit, size_t len, size_t received_len,
                           sigil_sync_result *r, char local_identity[33]) {
    size_t save_len = raw_len(unit, len);
    if (!save_len) return SIGIL_ERR_NOT_FOUND;
    if (save_len > NDS_MAX_SAVE) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    sigil_md5_of(unit, save_len, r->identity_hash);
    sigil_md5_of(unit, received_len, r->content_hash);
    r->shape = SIGIL_SAVE_SHAPE_SINGLE;
    sigil_sync_cart_artifact(x->req, ".srm", r);

    sigil_sync_cart_file f;
    uint8_t *local = NULL;
    size_t local_len = 0;
    int rc = sigil_sync_cart_file_of(x->req, &f);
    if (rc == SIGIL_OK) rc = read_raw(x->req, &f, &local, &local_len);
    if (rc == SIGIL_OK && local) sigil_md5_of(local, local_len, local_identity);
    free(local);
    bool already = false;
    if (rc == SIGIL_OK && sigil_sync_blocks_restore(x, local_identity, r->identity_hash,
                                                    sigil_sync_state_get(&x->state, "synced", x->game), &already)) {
        r->conflict = 1;
        rc = SIGIL_ERR_CONFLICT;
    }
    if (rc == SIGIL_OK && !f.path[0]) {
        snprintf(r->problem, sizeof(r->problem), "%s", r->artifact);
        rc = SIGIL_ERR_NO_TARGET;
    }
    /* The same save in another form (a .dsv without its footer) is written
     * again, so the file is the one the emulator reads. */
    uint8_t *file = NULL;
    size_t file_len = 0;
    if (rc == SIGIL_OK) rc = file_bytes(x->req->save.layout, unit, save_len, &file, &file_len);
    if (rc == SIGIL_OK && !sigil_sync_file_holds(x->req, f.path, file, file_len)) {
        rc = sigil_sync_put(x->req, f.path, file, file_len);
    }
    free(file);
    return rc;
}
