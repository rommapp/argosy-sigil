// SPDX-License-Identifier: MPL-2.0
/* The PS1 and PS2 card kinds. */
#include "sync_internal.h"

/* PS1: a card is the file as it holds the card (raw, .gme or .vmp), and a
 * save is an .mcs. */

static int ps1_load(const sigil_io *io, int device, void **card, int *format) {
    (void)device;
    sigil_ps1_file *f = (sigil_ps1_file *)malloc(sizeof(*f));
    if (!f) return SIGIL_ERR_OOM;
    int rc = sigil_ps1_file_load(io, f);
    if (rc != SIGIL_OK) { free(f); return rc; }
    *card = f;
    *format = f->format;
    return SIGIL_OK;
}

static int ps1_blank(void **card, int *format, int device, size_t size, int form, const void *like) {
    (void)device;
    (void)size;
    (void)like;
    sigil_ps1_file *f = (sigil_ps1_file *)malloc(sizeof(*f));
    if (!f) return SIGIL_ERR_OOM;
    sigil_ps1_file_format(f, form == SIGIL_FORM_VMP ? SIGIL_CARD_FORMAT_PS1_VMP : SIGIL_CARD_FORMAT_PS1_RAW);
    *card = f;
    *format = f->format;
    return SIGIL_OK;
}

static int ps1_list(const void *card, int format, sigil_card_listing **out) {
    return sigil_ps1_card_list(((const sigil_ps1_file *)card)->image, format, out);
}

static int ps1_extract(const void *card, const sigil_card_entry *entry, void **save) {
    size_t len = sigil_ps1_mcs_size(entry->blocks);
    uint8_t *mcs = (uint8_t *)malloc(len);
    if (!mcs) return SIGIL_ERR_OOM;
    int rc = sigil_ps1_extract(((const sigil_ps1_file *)card)->image, entry->first_block, entry->blocks, mcs);
    if (rc != SIGIL_OK) { free(mcs); return rc; }
    *save = sigil_sync_blob_new(mcs, len);
    return *save ? SIGIL_OK : SIGIL_ERR_OOM;
}

static int ps1_inject(void *card, const void *save) {
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    return sigil_ps1_inject(((sigil_ps1_file *)card)->image, s->data, s->len);
}

static uint32_t ps1_cost(const void *card, const void *save) {
    (void)card;
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    return sigil_ps1_cost(s->data, s->len);
}

static int ps1_remove(void *card, const sigil_card_entry *entry) {
    return sigil_ps1_delete(((sigil_ps1_file *)card)->image, entry->first_block);
}

static int ps1_verify(const void *card, const void *save) {
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    return sigil_ps1_verify(((const sigil_ps1_file *)card)->image, s->data, s->len);
}

static int ps1_image(const void *card, uint8_t **out, size_t *len) {
    return sigil_ps1_file_write((const sigil_ps1_file *)card, out, len);
}

static int ps1_check(const void *card) {
    return sigil_ps1_file_check((const sigil_ps1_file *)card);
}

/* The PSP and Vita name a PS1 card SCEVMC0.VMP or SCEVMC1.VMP, and read it
 * only signed. */
static int ps1_new_form(const sigil_sync_request *req, const char *path) {
    (void)req;
    size_t len = strlen(path);
    return len >= 4 && strcmp(path + len - 4, ".VMP") == 0 ? SIGIL_FORM_VMP : SIGIL_FORM_RAW;
}

/* POPS won't read a .vmp whose folder has no PARAM.SFO, and writes one only
 * when it runs the game, so a first restore writes the file POPS would:
 * the folder's name, the content's name as its title, signed. */
static int ps1_beside(const sigil_sync_request *req, const char *path, const void *card) {
    if (((const sigil_ps1_file *)card)->format != SIGIL_CARD_FORMAT_PS1_VMP) return SIGIL_OK;
    const char *slash = strrchr(path, '/');
    if (!slash) return SIGIL_OK;
    char dir[SIGIL_SAVE_PATH_MAX], sfo_path[SIGIL_SAVE_PATH_MAX];
    snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
    snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", dir);
    if (sigil_sync_listed(req, sfo_path)) return SIGIL_OK;
    const char *name = strrchr(dir, '/');
    char title[256];
    sigil_content_stem(req->save.content_path, title, sizeof(title));
    uint8_t sfo[SIGIL_POPS_SFO_SIZE];
    int rc = sigil_pops_param_sfo(name ? name + 1 : dir, title, sfo);
    return rc == SIGIL_OK ? sigil_sync_put(req, sfo_path, sfo, sizeof(sfo)) : rc;
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

const sigil_sync_kind sigil_sync_ps1_kind = {
    .platform = "psx", .has_ids = true, .main_device = SIGIL_DEVICE_NONE,
    .load = ps1_load, .blank = ps1_blank, .free_card = free, .size = sigil_sync_no_size,
    .unit_size = sigil_sync_no_unit_size, .list = ps1_list, .extract = ps1_extract,
    .free_save = sigil_sync_blob_free, .inject = ps1_inject, .cost = ps1_cost, .remove = ps1_remove, .verify = ps1_verify,
    .image = ps1_image, .identity = ps1_identity, .check = ps1_check, .new_form = ps1_new_form,
    .raw_ext = ".mcr", .beside = ps1_beside,
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

static uint32_t ps2_cost(const void *card, const void *save) {
    return sigil_ps2_cost((const sigil_ps2_card *)card, (const sigil_ps2_save *)save);
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
    .inject = ps2_inject, .cost = ps2_cost, .remove = ps2_remove, .verify = ps2_verify, .image = ps2_image,
    .identity = ps2_identity, .raw_ext = ".ps2", .folder_cards = true,
};
