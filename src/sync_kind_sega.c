// SPDX-License-Identifier: MPL-2.0
/* The Saturn, Sega CD and Dreamcast VMU kinds: volumes whose saves carry no
 * game id. */
#include "sync_internal.h"

/* The stored form a core writes a new volume in. */
static sigil_bram_storage storage_for(int form) {
    sigil_bram_storage s;
    memset(&s, 0, sizeof(s));
    s.filler = -1;
    if (form == SIGIL_FORM_EXPANDED_FF) {
        s.expanded = true;
        s.filler = 0xFF;
    }
    return s;
}

/* Saturn: a save is a .BUP. */

static void saturn_free_card(void *card) {
    sigil_saturn_volume_free((sigil_saturn_volume *)card);
    free(card);
}

static int saturn_load(const sigil_io *io, int device, void **card, int *format) {
    sigil_saturn_volume *v = (sigil_saturn_volume *)calloc(1, sizeof(*v));
    if (!v) return SIGIL_ERR_OOM;
    int rc = device == SIGIL_DEVICE_CART       ? sigil_saturn_volume_load_cart(io, v)
             : device == SIGIL_DEVICE_INTERNAL ? sigil_saturn_volume_load_internal(io, v)
                                               : sigil_saturn_volume_load(io, v);
    if (rc != SIGIL_OK) { free(v); return rc; }
    *card = v;
    *format = SIGIL_CARD_FORMAT_SATURN_BACKUP;
    return SIGIL_OK;
}

static int saturn_blank(void **card, int *format, int device, size_t size, int form, const void *like) {
    const sigil_saturn_volume *l = (const sigil_saturn_volume *)like;
    sigil_bram_storage storage = l ? l->storage : storage_for(form);
    sigil_saturn_volume *v = (sigil_saturn_volume *)calloc(1, sizeof(*v));
    if (!v) return SIGIL_ERR_OOM;
    size_t bytes = l ? l->size : size;
    int rc = device == SIGIL_DEVICE_CART ? sigil_saturn_volume_format_cart(v, bytes, &storage)
                                         : sigil_saturn_volume_format(v, bytes, &storage);
    if (rc != SIGIL_OK) { free(v); return rc; }
    *card = v;
    *format = SIGIL_CARD_FORMAT_SATURN_BACKUP;
    return SIGIL_OK;
}

static size_t saturn_size(const void *card) { return ((const sigil_saturn_volume *)card)->size; }

static size_t saturn_unit_size(int device, const void *source) {
    if (device == SIGIL_DEVICE_INTERNAL) return SATURN_INTERNAL_SIZE;
    return source ? saturn_size(source) : SATURN_CART_SIZE;
}

static int saturn_list(const void *card, int format, sigil_card_listing **out) {
    (void)format;
    return sigil_saturn_list((const sigil_saturn_volume *)card, out);
}

static int saturn_extract(const void *card, const sigil_card_entry *entry, void **save) {
    uint8_t *bup = NULL;
    size_t len = 0;
    int rc = sigil_saturn_extract((const sigil_saturn_volume *)card, entry->first_block, &bup, &len);
    if (rc != SIGIL_OK) return rc;
    *save = sigil_sync_blob_new(bup, len);
    return *save ? SIGIL_OK : SIGIL_ERR_OOM;
}

static int saturn_inject(void *card, const void *save) {
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    return sigil_saturn_inject((sigil_saturn_volume *)card, s->data, s->len);
}

static int saturn_remove(void *card, const sigil_card_entry *entry) {
    return sigil_saturn_delete((sigil_saturn_volume *)card, entry->first_block);
}

static int saturn_verify(const void *card, const void *save) {
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    return sigil_saturn_verify((const sigil_saturn_volume *)card, s->data, s->len);
}

static int saturn_image(const void *card, uint8_t **out, size_t *len) {
    return sigil_saturn_volume_write((const sigil_saturn_volume *)card, out, len);
}

static int saturn_identity(const void *save, char out[33]) {
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    sigil_saturn_bup_md5(s->data, s->len, out);
    return SIGIL_OK;
}

const sigil_sync_kind sigil_sync_saturn_kind = {
    .platform = "saturn", .has_ids = false, .main_device = SIGIL_DEVICE_INTERNAL,
    .load = saturn_load, .blank = saturn_blank, .free_card = saturn_free_card, .size = saturn_size,
    .unit_size = saturn_unit_size, .list = saturn_list, .extract = saturn_extract,
    .free_save = sigil_sync_blob_free, .inject = saturn_inject, .remove = saturn_remove,
    .verify = saturn_verify, .image = saturn_image, .identity = saturn_identity,
    .writable = sigil_sync_any_format,
};

/* Sega CD: a save is sigil's unit of its stored ECC-encoded blocks. */

static void segacd_free_card(void *card) {
    sigil_segacd_volume_free((sigil_segacd_volume *)card);
    free(card);
}

/* Internal backup RAM is always 8 KiB; anything larger is a RAM cart. */
static int segacd_load(const sigil_io *io, int device, void **card, int *format) {
    sigil_segacd_volume *v = (sigil_segacd_volume *)calloc(1, sizeof(*v));
    if (!v) return SIGIL_ERR_OOM;
    int rc = sigil_segacd_volume_load(io, v);
    if (rc == SIGIL_OK && device != SIGIL_DEVICE_NONE &&
        (v->size == SEGACD_INTERNAL_SIZE) != (device == SIGIL_DEVICE_INTERNAL)) {
        sigil_segacd_volume_free(v);
        rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
    if (rc != SIGIL_OK) { free(v); return rc; }
    *card = v;
    *format = SIGIL_CARD_FORMAT_SEGACD_BRAM;
    return SIGIL_OK;
}

static int segacd_blank(void **card, int *format, int device, size_t size, int form, const void *like) {
    (void)device;
    const sigil_segacd_volume *l = (const sigil_segacd_volume *)like;
    sigil_bram_storage storage = l ? l->storage : storage_for(form);
    sigil_segacd_volume *v = (sigil_segacd_volume *)calloc(1, sizeof(*v));
    if (!v) return SIGIL_ERR_OOM;
    int rc = sigil_segacd_volume_format(v, l ? l->size : size, &storage);
    if (rc != SIGIL_OK) { free(v); return rc; }
    *card = v;
    *format = SIGIL_CARD_FORMAT_SEGACD_BRAM;
    return SIGIL_OK;
}

static size_t segacd_size(const void *card) { return ((const sigil_segacd_volume *)card)->size; }

static size_t segacd_unit_size(int device, const void *source) {
    if (device == SIGIL_DEVICE_INTERNAL) return SEGACD_INTERNAL_SIZE;
    return source ? segacd_size(source) : SEGACD_MAX_CART_SIZE;
}

static int segacd_list(const void *card, int format, sigil_card_listing **out) {
    (void)format;
    return sigil_segacd_list((const sigil_segacd_volume *)card, out);
}

static int segacd_extract(const void *card, const sigil_card_entry *entry, void **save) {
    size_t len = sigil_segacd_unit_size(entry->blocks);
    uint8_t *unit = (uint8_t *)malloc(len);
    if (!unit) return SIGIL_ERR_OOM;
    int rc = sigil_segacd_extract((const sigil_segacd_volume *)card, entry->first_block, entry->blocks, unit);
    if (rc != SIGIL_OK) { free(unit); return rc; }
    *save = sigil_sync_blob_new(unit, len);
    return *save ? SIGIL_OK : SIGIL_ERR_OOM;
}

static int segacd_inject(void *card, const void *save) {
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    return sigil_segacd_inject((sigil_segacd_volume *)card, s->data, s->len);
}

static int segacd_remove(void *card, const sigil_card_entry *entry) {
    return sigil_segacd_delete((sigil_segacd_volume *)card, entry->first_block);
}

static int segacd_verify(const void *card, const void *save) {
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    return sigil_segacd_verify((const sigil_segacd_volume *)card, s->data, s->len);
}

static int segacd_image(const void *card, uint8_t **out, size_t *len) {
    return sigil_segacd_volume_write((const sigil_segacd_volume *)card, out, len);
}

static int segacd_identity(const void *save, char out[33]) {
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    sigil_md5_of(s->data, s->len, out);
    return SIGIL_OK;
}

const sigil_sync_kind sigil_sync_segacd_kind = {
    .platform = "segacd", .has_ids = false, .main_device = SIGIL_DEVICE_INTERNAL,
    .load = segacd_load, .blank = segacd_blank, .free_card = segacd_free_card, .size = segacd_size,
    .unit_size = segacd_unit_size, .list = segacd_list, .extract = segacd_extract,
    .free_save = sigil_sync_blob_free, .inject = segacd_inject, .remove = segacd_remove,
    .verify = segacd_verify, .image = segacd_image, .identity = segacd_identity,
    .writable = sigil_sync_any_format,
};

/* Dreamcast VMU: a save is a .dci. */

static int vmu_load(const sigil_io *io, int device, void **card, int *format) {
    (void)device;
    uint8_t *image = (uint8_t *)malloc(VMU_CARD_SIZE);
    if (!image) return SIGIL_ERR_OOM;
    int rc = sigil_dreamcast_card_load(io, image);
    if (rc != SIGIL_OK) { free(image); return rc; }
    *card = image;
    *format = SIGIL_CARD_FORMAT_DREAMCAST_VMU;
    return SIGIL_OK;
}

static int vmu_blank(void **card, int *format, int device, size_t size, int form, const void *like) {
    (void)device;
    (void)size;
    (void)form;
    (void)like;
    uint8_t *image = (uint8_t *)malloc(VMU_CARD_SIZE);
    if (!image) return SIGIL_ERR_OOM;
    sigil_dreamcast_format(image);
    *card = image;
    *format = SIGIL_CARD_FORMAT_DREAMCAST_VMU;
    return SIGIL_OK;
}

static size_t vmu_size(const void *card) {
    (void)card;
    return VMU_CARD_SIZE;
}

static size_t vmu_unit_size(int device, const void *source) {
    (void)device;
    (void)source;
    return VMU_CARD_SIZE;
}

static int vmu_list(const void *card, int format, sigil_card_listing **out) {
    (void)format;
    return sigil_dreamcast_card_list((const uint8_t *)card, out);
}

static int vmu_extract(const void *card, const sigil_card_entry *entry, void **save) {
    size_t len = sigil_dreamcast_dci_size(entry->blocks);
    uint8_t *dci = (uint8_t *)malloc(len);
    if (!dci) return SIGIL_ERR_OOM;
    int rc = sigil_dreamcast_extract((const uint8_t *)card, entry->first_block, entry->blocks, dci);
    if (rc != SIGIL_OK) { free(dci); return rc; }
    *save = sigil_sync_blob_new(dci, len);
    return *save ? SIGIL_OK : SIGIL_ERR_OOM;
}

static int vmu_inject(void *card, const void *save) {
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    return sigil_dreamcast_inject((uint8_t *)card, s->data, s->len);
}

static int vmu_remove(void *card, const sigil_card_entry *entry) {
    return sigil_dreamcast_delete((uint8_t *)card, entry->first_block);
}

static int vmu_verify(const void *card, const void *save) {
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    return sigil_dreamcast_verify((const uint8_t *)card, s->data, s->len);
}

static int vmu_image(const void *card, uint8_t **out, size_t *len) {
    return sigil_sync_copy_image((const uint8_t *)card, VMU_CARD_SIZE, out, len);
}

/* The .dci with its directory entry's timestamp (0x10-0x17) left out, so a
 * save written again unchanged isn't a new save. */
static int vmu_identity(const void *save, char out[33]) {
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    sigil_md5 m;
    sigil_md5_init(&m);
    if (s->len >= VMU_DIR_ENTRY_SIZE) {
        sigil_md5_update(&m, s->data, 0x10);
        sigil_md5_update(&m, s->data + 0x18, s->len - 0x18);
    }
    uint8_t digest[16];
    sigil_md5_final(&m, digest);
    sigil_md5_hex(digest, out);
    return SIGIL_OK;
}

const sigil_sync_kind sigil_sync_vmu_kind = {
    .platform = "dreamcast", .has_ids = false, .main_device = SIGIL_DEVICE_VMU_A1,
    .load = vmu_load, .blank = vmu_blank, .free_card = free, .size = vmu_size, .unit_size = vmu_unit_size,
    .list = vmu_list, .extract = vmu_extract, .free_save = sigil_sync_blob_free, .inject = vmu_inject,
    .remove = vmu_remove, .verify = vmu_verify, .image = vmu_image, .identity = vmu_identity,
    .writable = sigil_sync_any_format,
};
