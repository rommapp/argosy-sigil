// SPDX-License-Identifier: MPL-2.0
/* The PS1 and PS2 card kinds. */
#include "sync_internal.h"

/* PS1: a save is an .mcs. */

static int ps1_load(const sigil_io *io, int device, void **card, int *format) {
    (void)device;
    uint8_t *image = (uint8_t *)malloc(PS1_CARD_SIZE);
    if (!image) return SIGIL_ERR_OOM;
    int rc = sigil_ps1_card_load(io, image, format);
    if (rc != SIGIL_OK) { free(image); return rc; }
    *card = image;
    return SIGIL_OK;
}

static int ps1_blank(void **card, int *format, int device, size_t size, int form, const void *like) {
    (void)device;
    (void)size;
    (void)form;
    (void)like;
    uint8_t *image = (uint8_t *)malloc(PS1_CARD_SIZE);
    if (!image) return SIGIL_ERR_OOM;
    sigil_ps1_format(image);
    *card = image;
    *format = SIGIL_CARD_FORMAT_PS1_RAW;
    return SIGIL_OK;
}

static int ps1_list(const void *card, int format, sigil_card_listing **out) {
    return sigil_ps1_card_list((const uint8_t *)card, format, out);
}

static int ps1_extract(const void *card, const sigil_card_entry *entry, void **save) {
    size_t len = sigil_ps1_mcs_size(entry->blocks);
    uint8_t *mcs = (uint8_t *)malloc(len);
    if (!mcs) return SIGIL_ERR_OOM;
    int rc = sigil_ps1_extract((const uint8_t *)card, entry->first_block, entry->blocks, mcs);
    if (rc != SIGIL_OK) { free(mcs); return rc; }
    *save = sigil_sync_blob_new(mcs, len);
    return *save ? SIGIL_OK : SIGIL_ERR_OOM;
}

static int ps1_inject(void *card, const void *save) {
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    return sigil_ps1_inject((uint8_t *)card, s->data, s->len);
}

static int ps1_remove(void *card, const sigil_card_entry *entry) {
    return sigil_ps1_delete((uint8_t *)card, entry->first_block);
}

static int ps1_verify(const void *card, const void *save) {
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    return sigil_ps1_verify((const uint8_t *)card, s->data, s->len);
}

static int ps1_image(const void *card, uint8_t **out, size_t *len) {
    return sigil_sync_copy_image((const uint8_t *)card, PS1_CARD_SIZE, out, len);
}

/* Hashes the save as it goes on a card: sigil_ps1_inject rewrites the
 * directory frame's size field, so a card that stored it wrong and the unit
 * built from it hash alike. */
static int ps1_identity(const void *save, char out[33]) {
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    uint8_t *card = (uint8_t *)malloc(PS1_CARD_SIZE);
    uint8_t *placed = (uint8_t *)malloc(s->len);
    int rc = card && placed ? SIGIL_OK : SIGIL_ERR_OOM;
    if (rc == SIGIL_OK) {
        sigil_ps1_format(card);
        rc = sigil_ps1_inject(card, s->data, s->len);
    }
    if (rc == SIGIL_OK) rc = sigil_ps1_extract(card, 1, (uint32_t)((s->len - PS1_FRAME_SIZE) / PS1_BLOCK_SIZE), placed);
    if (rc == SIGIL_OK) sigil_md5_of(placed, s->len, out);
    free(card);
    free(placed);
    return rc;
}

static bool ps1_writable(int format) { return format == SIGIL_CARD_FORMAT_PS1_RAW; }

const sigil_sync_kind sigil_sync_ps1_kind = {
    .platform = "psx", .has_ids = true, .main_device = SIGIL_DEVICE_NONE,
    .load = ps1_load, .blank = ps1_blank, .free_card = free, .size = sigil_sync_no_size,
    .unit_size = sigil_sync_no_unit_size, .list = ps1_list, .extract = ps1_extract,
    .free_save = sigil_sync_blob_free, .inject = ps1_inject, .remove = ps1_remove, .verify = ps1_verify,
    .image = ps1_image, .identity = ps1_identity, .writable = ps1_writable,
};

/* PS2: a save is a save folder lifted off the card. */

/* The root directory time a card sigil builds carries: 2000-01-01 00:00:00. */
static const uint8_t PS2_BUILT_TOD[PS2_TOD_SIZE] = { 0, 0, 0, 0, 1, 1, 0xD0, 0x07 };

static void ps2_free_card(void *card) {
    sigil_ps2_card_free((sigil_ps2_card *)card);
    free(card);
}

static int ps2_load(const sigil_io *io, int device, void **card, int *format) {
    (void)device;
    sigil_ps2_card *c = (sigil_ps2_card *)calloc(1, sizeof(*c));
    if (!c) return SIGIL_ERR_OOM;
    int rc = sigil_ps2_card_load(io, c);
    if (rc != SIGIL_OK) { free(c); return rc; }
    *card = c;
    *format = SIGIL_CARD_FORMAT_PS2;
    return SIGIL_OK;
}

static int ps2_blank(void **card, int *format, int device, size_t size, int form, const void *like) {
    (void)device;
    (void)size;
    (void)form;
    (void)like;
    sigil_ps2_card *c = (sigil_ps2_card *)calloc(1, sizeof(*c));
    if (!c) return SIGIL_ERR_OOM;
    int rc = sigil_ps2_card_format(c, PS2_BUILT_TOD);
    if (rc != SIGIL_OK) { ps2_free_card(c); return rc; }
    *card = c;
    *format = SIGIL_CARD_FORMAT_PS2;
    return SIGIL_OK;
}

static int ps2_list(const void *card, int format, sigil_card_listing **out) {
    (void)format;
    return sigil_ps2_card_list((const sigil_ps2_card *)card, out);
}

static void ps2_free_save(void *save) {
    sigil_ps2_save_free((sigil_ps2_save *)save);
    free(save);
}

static int ps2_extract(const void *card, const sigil_card_entry *entry, void **save) {
    sigil_ps2_save *s = (sigil_ps2_save *)calloc(1, sizeof(*s));
    if (!s) return SIGIL_ERR_OOM;
    int rc = sigil_ps2_extract((const sigil_ps2_card *)card, entry->first_block, s);
    if (rc != SIGIL_OK) { free(s); return rc; }
    *save = s;
    return SIGIL_OK;
}

static int ps2_inject(void *card, const void *save) {
    return sigil_ps2_inject((sigil_ps2_card *)card, (const sigil_ps2_save *)save);
}

static int ps2_remove(void *card, const sigil_card_entry *entry) {
    return sigil_ps2_delete((sigil_ps2_card *)card, entry->first_block);
}

static int ps2_verify(const void *card, const void *save) {
    return sigil_ps2_verify((const sigil_ps2_card *)card, (const sigil_ps2_save *)save);
}

static int ps2_image(const void *card, uint8_t **out, size_t *len) {
    const sigil_ps2_card *c = (const sigil_ps2_card *)card;
    *len = sigil_ps2_card_file_size(c);
    *out = (uint8_t *)malloc(*len);
    if (!*out) return SIGIL_ERR_OOM;
    int rc = sigil_ps2_card_write(c, *out);
    if (rc != SIGIL_OK) { free(*out); *out = NULL; }
    return rc;
}

static int ps2_identity(const void *save, char out[33]) {
    sigil_ps2_save_md5((const sigil_ps2_save *)save, out);
    return SIGIL_OK;
}

const sigil_sync_kind sigil_sync_ps2_kind = {
    .platform = "ps2", .has_ids = true, .main_device = SIGIL_DEVICE_NONE,
    .load = ps2_load, .blank = ps2_blank, .free_card = ps2_free_card, .size = sigil_sync_no_size,
    .unit_size = sigil_sync_no_unit_size, .list = ps2_list, .extract = ps2_extract, .free_save = ps2_free_save,
    .inject = ps2_inject, .remove = ps2_remove, .verify = ps2_verify, .image = ps2_image,
    .identity = ps2_identity, .writable = sigil_sync_any_format, .folder_cards = true,
};
