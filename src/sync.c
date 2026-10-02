// SPDX-License-Identifier: MPL-2.0
#include "card_dreamcast.h"
#include "card_gamecube.h"
#include "card_ps1.h"
#include "card_ps2.h"
#include "card_segacd.h"
#include "save_layout.h"
#include <stdio.h>
#include <stdlib.h>

#define STATE_MAGIC     "sigil-state 1\n"
#define KEY_MAX         (4 * SIGIL_SAVE_PATH_MAX)
#define MAX_CARD_FILES  (8)
#define MAX_SHARED      (8)
#define MAX_UNIT_MEMBER (8u * 1024u * 1024u + 64u * 1024u)

/* ---- state ---------------------------------------------------------------------
 * Text, one fact per line, so a stored blob can be read when something goes
 * wrong. The caller treats it as opaque. Lines this version doesn't know are
 * kept. Fields are tab-separated, with '%', tab and newline escaped as %XX.
 *
 *   sigil-state 1
 *   synced   <game>  <identity>          the game's saves as last collected or restored
 *   restored <game>  <new> <old>         unmanaged: the last restore and what it replaced
 *   owner    <volume> <save name> <game> the game a save on a shared volume belongs to
 *   prepared <volume> <game>             managed: the volume was swapped in for this game
 *   held     <volume> <identity>         the saves with no known owner passed on in a holding unit
 *   seen     <volume> <identity>         every save on the volume at the last collect or restore
 */

typedef struct {
    char  **lines;
    size_t  count;
    size_t  cap;
} state_t;

static void state_free(state_t *s) {
    for (size_t i = 0; i < s->count; i++) free(s->lines[i]);
    free(s->lines);
    memset(s, 0, sizeof(*s));
}

static int state_append(state_t *s, const char *text, size_t len) {
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16;
        char **grown = (char **)realloc(s->lines, cap * sizeof(*grown));
        if (!grown) return SIGIL_ERR_OOM;
        s->lines = grown;
        s->cap = cap;
    }
    char *line = (char *)malloc(len + 1);
    if (!line) return SIGIL_ERR_OOM;
    memcpy(line, text, len);
    line[len] = '\0';
    s->lines[s->count++] = line;
    return SIGIL_OK;
}

static int state_parse(state_t *s, const uint8_t *blob, size_t len) {
    memset(s, 0, sizeof(*s));
    size_t magic = strlen(STATE_MAGIC);
    if (!blob || len < magic || memcmp(blob, STATE_MAGIC, magic) != 0) return SIGIL_OK;
    const char *p = (const char *)blob + magic, *end = (const char *)blob + len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t line = (size_t)((eol ? eol : end) - p);
        if (line > 0 && state_append(s, p, line) != SIGIL_OK) { state_free(s); return SIGIL_ERR_OOM; }
        p += line + 1;
    }
    return SIGIL_OK;
}

/* The rest of the first line starting with `prefix`, or NULL. */
static const char *state_get(const state_t *s, const char *prefix) {
    size_t n = strlen(prefix);
    for (size_t i = 0; i < s->count; i++) {
        if (strncmp(s->lines[i], prefix, n) == 0) return s->lines[i] + n;
    }
    return NULL;
}

/* Replaces every line starting with `prefix` by prefix + value; drops them when `value` is NULL. */
static int state_put(state_t *s, const char *prefix, const char *value) {
    size_t n = strlen(prefix), kept = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (strncmp(s->lines[i], prefix, n) == 0) free(s->lines[i]);
        else s->lines[kept++] = s->lines[i];
    }
    s->count = kept;
    if (!value) return SIGIL_OK;
    size_t v = strlen(value);
    char *line = (char *)malloc(n + v + 1);
    if (!line) return SIGIL_ERR_OOM;
    memcpy(line, prefix, n);
    memcpy(line + n, value, v + 1);
    int rc = state_append(s, line, n + v);
    free(line);
    return rc;
}

static int state_blob(const state_t *s, uint8_t **out, size_t *len) {
    size_t total = strlen(STATE_MAGIC);
    for (size_t i = 0; i < s->count; i++) total += strlen(s->lines[i]) + 1;
    char *text = (char *)malloc(total);
    if (!text) return SIGIL_ERR_OOM;
    size_t n = strlen(STATE_MAGIC);
    memcpy(text, STATE_MAGIC, n);
    for (size_t i = 0; i < s->count; i++) {
        size_t l = strlen(s->lines[i]);
        memcpy(text + n, s->lines[i], l);
        n += l;
        text[n++] = '\n';
    }
    *out = (uint8_t *)text;
    *len = n;
    return SIGIL_OK;
}

static void escape(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (; *in && n + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '%' || c == '\t' || c == '\n' || c == '\r') n += (size_t)snprintf(out + n, cap - n, "%%%02X", c);
        else out[n++] = (char)c;
    }
    out[n] = '\0';
}

/* ---- card formats ---------------------------------------------------------------
 * What sync needs from a card or volume format. `card` and `save` are the
 * format's own types; sync only moves them between these calls. */

typedef struct {
    const char *platform;     /* the slug the state keys this platform's games under */
    bool        has_ids;      /* saves carry their game's id; else ownership comes from volumes */
    int         main_device;  /* the volume a unit may carry alone, unzipped; SIGIL_DEVICE_NONE on cards */
    /* Reads a card or volume holding `device` (SIGIL_DEVICE_NONE on card platforms). */
    int    (*load)(const sigil_io *io, int device, void **card, int *format);
    /* An empty card or volume for `device`: the size and stored form of `like`
     * when given, else `size` collapsed bytes stored in `form`. */
    int    (*blank)(void **card, int *format, int device, size_t size, int form, const void *like);
    void   (*free_card)(void *card);
    size_t (*size)(const void *card);
    size_t (*unit_size)(int device, const void *source);
    int    (*list)(const void *card, int format, sigil_card_listing **out);
    int    (*extract)(const void *card, const sigil_card_entry *entry, void **save);
    void   (*free_save)(void *save);
    int    (*inject)(void *card, const void *save);
    int    (*remove)(void *card, const sigil_card_entry *entry);
    int    (*verify)(const void *card, const void *save);
    int    (*image)(const void *card, uint8_t **out, size_t *len);
    int    (*identity)(const void *save, char out[33]);
    bool   (*writable)(int format);
    /* Optional. The name a save is known by in units and the state, when the
     * card's name alone doesn't identify it (GameCube: game, maker, file). */
    void   (*save_key)(const void *save, char out[SIGIL_CARD_NAME_MAX]);
    /* Optional. The form a new card for this game is formatted in. */
    int    (*new_form)(const sigil_sync_request *req);
    /* The unit is the game's saves as files instead of a card: each travels
     * under file_name and reads back through file_to_save. */
    bool        save_files;
    void   (*file_name)(const void *save, char *out, size_t cap);
    int    (*file_to_save)(const uint8_t *data, size_t len, void **save);
    /* Optional. The save can't go beside this game's, as a companion. */
    bool   (*foreign)(const sigil_sync_request *req, const void *save);
    /* A shared card may be a PCSX2 folder card. */
    bool        folder_cards;
} card_kind;

/* A save as bytes: .mcs, .BUP, .scd. */
typedef struct {
    uint8_t *data;
    size_t   len;
} blob_save;

static void blob_free(void *save) {
    blob_save *s = (blob_save *)save;
    if (s) free(s->data);
    free(s);
}

static blob_save *blob_new(uint8_t *data, size_t len) {
    blob_save *s = (blob_save *)calloc(1, sizeof(*s));
    if (!s) { free(data); return NULL; }
    s->data = data;
    s->len = len;
    return s;
}

static size_t no_size(const void *card) { (void)card; return 0; }
static size_t no_unit_size(int device, const void *source) { (void)device; (void)source; return 0; }
static bool any_format(int format) { (void)format; return true; }

/* PS1 */

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
    *save = blob_new(mcs, len);
    return *save ? SIGIL_OK : SIGIL_ERR_OOM;
}

static int ps1_inject(void *card, const void *save) {
    const blob_save *s = (const blob_save *)save;
    return sigil_ps1_inject((uint8_t *)card, s->data, s->len);
}

static int ps1_remove(void *card, const sigil_card_entry *entry) {
    return sigil_ps1_delete((uint8_t *)card, entry->first_block);
}

static int ps1_verify(const void *card, const void *save) {
    const blob_save *s = (const blob_save *)save;
    return sigil_ps1_verify((const uint8_t *)card, s->data, s->len);
}

static int ps1_image(const void *card, uint8_t **out, size_t *len) {
    *out = (uint8_t *)malloc(PS1_CARD_SIZE);
    if (!*out) return SIGIL_ERR_OOM;
    memcpy(*out, card, PS1_CARD_SIZE);
    *len = PS1_CARD_SIZE;
    return SIGIL_OK;
}

/* Hashes the save as it goes on a card: sigil_ps1_inject rewrites the
 * directory frame's size field, so a card that stored it wrong and the unit
 * built from it hash alike. */
static int ps1_identity(const void *save, char out[33]) {
    const blob_save *s = (const blob_save *)save;
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

static const card_kind PS1_KIND = {
    "psx", true, SIGIL_DEVICE_NONE, ps1_load, ps1_blank, free, no_size, no_unit_size, ps1_list, ps1_extract, blob_free,
    ps1_inject, ps1_remove, ps1_verify, ps1_image, ps1_identity, ps1_writable,
};

/* PS2 */

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

static const card_kind PS2_KIND = {
    "ps2", true, SIGIL_DEVICE_NONE, ps2_load, ps2_blank, ps2_free_card, no_size, no_unit_size, ps2_list, ps2_extract,
    ps2_free_save, ps2_inject, ps2_remove, ps2_verify, ps2_image, ps2_identity, any_format, .folder_cards = true,
};

/* Saturn */

static void saturn_free_card(void *card) {
    sigil_saturn_volume_free((sigil_saturn_volume *)card);
    free(card);
}

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
    *save = blob_new(bup, len);
    return *save ? SIGIL_OK : SIGIL_ERR_OOM;
}

static int saturn_inject(void *card, const void *save) {
    const blob_save *s = (const blob_save *)save;
    return sigil_saturn_inject((sigil_saturn_volume *)card, s->data, s->len);
}

static int saturn_remove(void *card, const sigil_card_entry *entry) {
    return sigil_saturn_delete((sigil_saturn_volume *)card, entry->first_block);
}

static int saturn_verify(const void *card, const void *save) {
    const blob_save *s = (const blob_save *)save;
    return sigil_saturn_verify((const sigil_saturn_volume *)card, s->data, s->len);
}

static int saturn_image(const void *card, uint8_t **out, size_t *len) {
    return sigil_saturn_volume_write((const sigil_saturn_volume *)card, out, len);
}

static int saturn_identity(const void *save, char out[33]) {
    const blob_save *s = (const blob_save *)save;
    sigil_saturn_bup_md5(s->data, s->len, out);
    return SIGIL_OK;
}

static const card_kind SATURN_KIND = {
    "saturn", false, SIGIL_DEVICE_INTERNAL, saturn_load, saturn_blank, saturn_free_card, saturn_size, saturn_unit_size,
    saturn_list, saturn_extract, blob_free, saturn_inject, saturn_remove, saturn_verify, saturn_image,
    saturn_identity, any_format,
};

/* Sega CD */

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
    *save = blob_new(unit, len);
    return *save ? SIGIL_OK : SIGIL_ERR_OOM;
}

static int segacd_inject(void *card, const void *save) {
    const blob_save *s = (const blob_save *)save;
    return sigil_segacd_inject((sigil_segacd_volume *)card, s->data, s->len);
}

static int segacd_remove(void *card, const sigil_card_entry *entry) {
    return sigil_segacd_delete((sigil_segacd_volume *)card, entry->first_block);
}

static int segacd_verify(const void *card, const void *save) {
    const blob_save *s = (const blob_save *)save;
    return sigil_segacd_verify((const sigil_segacd_volume *)card, s->data, s->len);
}

static int segacd_image(const void *card, uint8_t **out, size_t *len) {
    return sigil_segacd_volume_write((const sigil_segacd_volume *)card, out, len);
}

static int segacd_identity(const void *save, char out[33]) {
    const blob_save *s = (const blob_save *)save;
    sigil_md5_of(s->data, s->len, out);
    return SIGIL_OK;
}

static const card_kind SEGACD_KIND = {
    "segacd", false, SIGIL_DEVICE_INTERNAL, segacd_load, segacd_blank, segacd_free_card, segacd_size, segacd_unit_size,
    segacd_list, segacd_extract, blob_free, segacd_inject, segacd_remove, segacd_verify, segacd_image,
    segacd_identity, any_format,
};

/* Dreamcast VMU */

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
    *save = blob_new(dci, len);
    return *save ? SIGIL_OK : SIGIL_ERR_OOM;
}

static int vmu_inject(void *card, const void *save) {
    const blob_save *s = (const blob_save *)save;
    return sigil_dreamcast_inject((uint8_t *)card, s->data, s->len);
}

static int vmu_remove(void *card, const sigil_card_entry *entry) {
    return sigil_dreamcast_delete((uint8_t *)card, entry->first_block);
}

static int vmu_verify(const void *card, const void *save) {
    const blob_save *s = (const blob_save *)save;
    return sigil_dreamcast_verify((const uint8_t *)card, s->data, s->len);
}

static int vmu_image(const void *card, uint8_t **out, size_t *len) {
    *out = (uint8_t *)malloc(VMU_CARD_SIZE);
    if (!*out) return SIGIL_ERR_OOM;
    memcpy(*out, card, VMU_CARD_SIZE);
    *len = VMU_CARD_SIZE;
    return SIGIL_OK;
}

/* The .dci with its directory entry's timestamp (0x10-0x17) left out, so a
 * save written again unchanged isn't a new save. */
static int vmu_identity(const void *save, char out[33]) {
    const blob_save *s = (const blob_save *)save;
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

static const card_kind VMU_KIND = {
    "dreamcast", false, SIGIL_DEVICE_VMU_A1, vmu_load, vmu_blank, free, vmu_size, vmu_unit_size,
    vmu_list, vmu_extract, blob_free, vmu_inject, vmu_remove, vmu_verify, vmu_image, vmu_identity, any_format,
};

/* GameCube. A save is a .gci: the 64-byte directory entry, then its blocks.
 * Units are the game's .gci files, named as Dolphin names them. */

#define GCI_GAMECODE 0x00u
#define GCI_MAKER    0x04u
#define GCI_FILENAME 0x08u
#define GCI_MTIME    0x28u
#define GCI_COPY     0x35u
#define GCI_FIRST    0x36u
#define GC_SMALL_CARD (512u * 1024u)

typedef struct {
    uint8_t *image;
    size_t   size;
} gc_card;

static void gc_free_card(void *card) {
    gc_card *c = (gc_card *)card;
    if (c) free(c->image);
    free(c);
}

static int gc_load(const sigil_io *io, int device, void **card, int *format) {
    (void)device;
    gc_card *c = (gc_card *)calloc(1, sizeof(*c));
    if (!c) return SIGIL_ERR_OOM;
    int rc = sigil_gamecube_card_load(io, &c->image, &c->size);
    if (rc != SIGIL_OK) { free(c); return rc; }
    *card = c;
    *format = SIGIL_CARD_FORMAT_GAMECUBE_RAW;
    return SIGIL_OK;
}

static int gc_blank_sized(void **card, size_t size, bool shift_jis) {
    gc_card *c = (gc_card *)calloc(1, sizeof(*c));
    if (!c) return SIGIL_ERR_OOM;
    c->size = size;
    c->image = (uint8_t *)malloc(size);
    int rc = c->image ? sigil_gamecube_format(c->image, size, shift_jis) : SIGIL_ERR_OOM;
    if (rc != SIGIL_OK) { gc_free_card(c); return rc; }
    *card = c;
    return SIGIL_OK;
}

/* A new card is Dolphin's default 2043-block card. */
static int gc_blank(void **card, int *format, int device, size_t size, int form, const void *like) {
    (void)device;
    (void)size;
    const gc_card *l = (const gc_card *)like;
    *format = SIGIL_CARD_FORMAT_GAMECUBE_RAW;
    return gc_blank_sized(card, l ? l->size : GC_MAX_CARD_SIZE, form == SIGIL_FORM_SHIFT_JIS);
}

static size_t gc_size(const void *card) { return ((const gc_card *)card)->size; }

static int gc_list(const void *card, int format, sigil_card_listing **out) {
    (void)format;
    const gc_card *c = (const gc_card *)card;
    return sigil_gamecube_card_list(c->image, c->size, out);
}

static int gc_extract(const void *card, const sigil_card_entry *entry, void **save) {
    const gc_card *c = (const gc_card *)card;
    size_t len = sigil_gamecube_gci_size(entry->blocks);
    uint8_t *gci = (uint8_t *)malloc(len);
    if (!gci) return SIGIL_ERR_OOM;
    int rc = sigil_gamecube_extract(c->image, c->size, entry->first_block, entry->blocks, gci);
    if (rc != SIGIL_OK) { free(gci); return rc; }
    *save = blob_new(gci, len);
    return *save ? SIGIL_OK : SIGIL_ERR_OOM;
}

/* The block a save with `gci`'s identity starts at on the card, or 0xFFFF. */
static uint32_t gc_first_block_of(const gc_card *c, const uint8_t *gci) {
    sigil_card_listing *l = NULL;
    uint32_t first = 0xFFFF;
    if (sigil_gamecube_card_list(c->image, c->size, &l) != SIGIL_OK) return first;
    char want[16];
    snprintf(want, sizeof(want), "%02X%02X%02X%02X", gci[0], gci[1], gci[2], gci[3]);
    char name[GC_FILENAME_LEN + 1];
    memcpy(name, gci + GCI_FILENAME, GC_FILENAME_LEN);
    name[GC_FILENAME_LEN] = '\0';
    for (size_t i = 0; i < l->entry_count && first == 0xFFFF; i++) {
        const sigil_card_entry *e = &l->entries[i];
        if (strcmp(e->owner_id, want) != 0 || strcmp(e->name, name) != 0) continue;
        uint8_t *placed = (uint8_t *)malloc(sigil_gamecube_gci_size(e->blocks));
        if (placed && sigil_gamecube_extract(c->image, c->size, e->first_block, e->blocks, placed) == SIGIL_OK &&
            memcmp(placed + GCI_MAKER, gci + GCI_MAKER, 2) == 0) {
            first = e->first_block;
        }
        free(placed);
    }
    sigil_card_listing_free(l);
    return first;
}

/* Injects the save and binds it to this card's serial, as Dolphin does on
 * import for F-Zero GX; other saves are left as they are. */
static int gc_inject(void *card, const void *save) {
    gc_card *c = (gc_card *)card;
    const blob_save *s = (const blob_save *)save;
    int rc = sigil_gamecube_inject(c->image, c->size, s->data, s->len);
    if (rc != SIGIL_OK) return rc;
    uint32_t first = gc_first_block_of(c, s->data);
    return first == 0xFFFF ? SIGIL_ERR_IO : sigil_gamecube_bind_serial(c->image, c->size, first);
}

static int gc_remove(void *card, const sigil_card_entry *entry) {
    gc_card *c = (gc_card *)card;
    return sigil_gamecube_delete(c->image, c->size, entry->first_block);
}

static int gc_image(const void *card, uint8_t **out, size_t *len) {
    const gc_card *c = (const gc_card *)card;
    *out = (uint8_t *)malloc(c->size);
    if (!*out) return SIGIL_ERR_OOM;
    memcpy(*out, c->image, c->size);
    *len = c->size;
    return SIGIL_OK;
}

/* The save as it reads off a fresh card after injecting and binding, its
 * time, copy count and first block left out: a save bound to one card's
 * serial and the same save bound to another's hash alike. */
static int gc_identity(const void *save, char out[33]) {
    const blob_save *s = (const blob_save *)save;
    void *scratch = NULL;
    int rc = gc_blank_sized(&scratch, GC_SMALL_CARD, false);
    if (rc == SIGIL_OK && gc_inject(scratch, save) == SIGIL_ERR_NO_SPACE) {
        gc_free_card(scratch);
        scratch = NULL;
        rc = gc_blank_sized(&scratch, GC_MAX_CARD_SIZE, false);
        if (rc == SIGIL_OK) rc = gc_inject(scratch, save);
    }
    gc_card *c = (gc_card *)scratch;
    uint8_t *placed = rc == SIGIL_OK ? (uint8_t *)malloc(s->len) : NULL;
    if (rc == SIGIL_OK && !placed) rc = SIGIL_ERR_OOM;
    if (rc == SIGIL_OK) {
        uint32_t first = gc_first_block_of(c, s->data);
        rc = sigil_gamecube_extract(c->image, c->size, first, (uint32_t)((s->len - GC_DENTRY_SIZE) / GC_BLOCK_SIZE), placed);
    }
    if (rc == SIGIL_OK) {
        memset(placed + GCI_MTIME, 0, 4);
        placed[GCI_COPY] = 0;
        memset(placed + GCI_FIRST, 0, 2);
        sigil_md5_of(placed, s->len, out);
    }
    free(placed);
    gc_free_card(scratch);
    return rc;
}

/* The card holds the save's identity, and its bytes match after binding. */
static int gc_verify(const void *card, const void *save) {
    const gc_card *c = (const gc_card *)card;
    const blob_save *s = (const blob_save *)save;
    uint32_t first = gc_first_block_of(c, s->data);
    if (first == 0xFFFF) return SIGIL_ERR_NOT_FOUND;
    blob_save placed = { (uint8_t *)malloc(s->len), s->len };
    if (!placed.data) return SIGIL_ERR_OOM;
    char want[33], got[33];
    int rc = sigil_gamecube_extract(c->image, c->size, first, (uint32_t)((s->len - GC_DENTRY_SIZE) / GC_BLOCK_SIZE),
                                    placed.data);
    if (rc == SIGIL_OK) rc = gc_identity(save, want);
    if (rc == SIGIL_OK) rc = gc_identity(&placed, got);
    free(placed.data);
    if (rc != SIGIL_OK) return rc;
    return strcmp(want, got) == 0 ? SIGIL_OK : SIGIL_ERR_NOT_FOUND;
}

/* Game code and maker code in hex, then the file name: the identity Dolphin
 * gives a save (GCMemcardUtils HasSameIdentity). */
static void gc_save_key(const void *save, char out[SIGIL_CARD_NAME_MAX]) {
    const blob_save *s = (const blob_save *)save;
    char name[GC_FILENAME_LEN + 1];
    memcpy(name, s->data + GCI_FILENAME, GC_FILENAME_LEN);
    name[GC_FILENAME_LEN] = '\0';
    snprintf(out, SIGIL_CARD_NAME_MAX, "%02X%02X%02X%02X-%02X%02X-%s", s->data[0], s->data[1], s->data[2], s->data[3],
             s->data[GCI_MAKER], s->data[GCI_MAKER + 1], name);
}

static const char *gc_game_region(const sigil_sync_request *req) {
    return sigil_gc_region_folder(sigil_gc_region_letter(req->save.result));
}

static int gc_new_form(const sigil_sync_request *req) {
    const char *region = gc_game_region(req);
    return region && strcmp(region, "JAP") == 0 ? SIGIL_FORM_SHIFT_JIS : SIGIL_FORM_RAW;
}

/* A companion's save from another region than the game's: the game can't
 * read it, and Dolphin keeps it in another region's folder or card. */
static bool gc_foreign(const sigil_sync_request *req, const void *save) {
    const char *game = gc_game_region(req);
    const char *theirs = sigil_gc_region_folder((char)((const blob_save *)save)->data[3]);
    return game && theirs && strcmp(game, theirs) != 0;
}

static void gc_unit_file_name(const void *save, char *out, size_t cap);

/* A .gci, or a GameShark .gcs or MaxDrive .sav of one save, as a .gci. */
static int gc_file_to_save(const uint8_t *data, size_t len, void **save) {
    uint8_t *gci = (uint8_t *)malloc(len ? len : 1);
    size_t gci_len = 0;
    int rc = gci ? sigil_gamecube_to_gci(data, len, gci, &gci_len) : SIGIL_ERR_OOM;
    if (rc != SIGIL_OK) { free(gci); return rc; }
    *save = blob_new(gci, gci_len);
    return *save ? SIGIL_OK : SIGIL_ERR_OOM;
}

static const card_kind GC_KIND = {
    .platform = "gamecube", .has_ids = true, .main_device = SIGIL_DEVICE_NONE,
    .load = gc_load, .blank = gc_blank, .free_card = gc_free_card, .size = gc_size, .unit_size = no_unit_size,
    .list = gc_list, .extract = gc_extract, .free_save = blob_free, .inject = gc_inject, .remove = gc_remove,
    .verify = gc_verify, .image = gc_image, .identity = gc_identity, .writable = any_format,
    .save_key = gc_save_key, .new_form = gc_new_form, .save_files = true,
    .file_name = gc_unit_file_name, .file_to_save = gc_file_to_save, .foreign = gc_foreign,
};

/* The name Dolphin gives a .gci (GCMemcardUtils GenerateFilename and
 * NandPaths EscapeFileName): maker-gamecode-filename, with a name of dots
 * alone spelled out, each "__" doubled into escapes, and characters a file
 * name can't hold as __xx__. */
static void gc_file_name(const uint8_t *gci, char *out, size_t cap) {
    char raw[GC_FILENAME_LEN + 16];
    size_t n = 0;
    raw[n++] = (char)gci[GCI_MAKER];
    raw[n++] = (char)gci[GCI_MAKER + 1];
    raw[n++] = '-';
    for (size_t i = 0; i < 4; i++) raw[n++] = (char)gci[GCI_GAMECODE + i];
    raw[n++] = '-';
    for (size_t i = 0; i < GC_FILENAME_LEN && gci[GCI_FILENAME + i]; i++) raw[n++] = (char)gci[GCI_FILENAME + i];
    raw[n] = '\0';

    size_t o = 0;
    bool all_dots = n > 0;
    for (size_t i = 0; i < n; i++) all_dots = all_dots && raw[i] == '.';
    for (size_t i = 0; i < n && o + 14 < cap; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (all_dots) {
            o += (size_t)snprintf(out + o, cap - o, "__2e__");
        } else if (c == '_' && i + 1 < n && raw[i + 1] == '_') {
            o += (size_t)snprintf(out + o, cap - o, "__5f____5f__");
            i++;
        } else if (c <= 0x1F || c == 0x7F || strchr("\"*/:<>?\\|", c)) {
            o += (size_t)snprintf(out + o, cap - o, "__%02x__", c);
        } else {
            out[o++] = (char)c;
        }
    }
    snprintf(out + o, cap - o, ".gci");
}

static void gc_unit_file_name(const void *save, char *out, size_t cap) {
    gc_file_name(((const blob_save *)save)->data, out, cap);
}

/* ---- request -------------------------------------------------------------- */

typedef struct {
    const sigil_sync_request *req;
    const card_kind          *kind;
    char                      game[KEY_MAX];   /* escaped state key of this game */
    char                    (*companion_keys)[KEY_MAX];  /* escaped state key of each request companion */
    state_t                   state;
} sync_ctx;

static const card_kind *kind_for(const sigil_sync_request *req) {
    const char *slug = req->save.platform ? sigil_layout_platform(req->save.platform) : NULL;
    if (slug) {
        if (strcmp(slug, "saturn") == 0) return &SATURN_KIND;
        if (strcmp(slug, "segacd") == 0) return &SEGACD_KIND;
    }
    sigil_platform p = slug ? sigil_platform_from_slug(slug) : SIGIL_PLATFORM_AUTO;
    if (p == SIGIL_PLATFORM_AUTO && req->save.result) p = (sigil_platform)req->save.result->platform;
    if (p == SIGIL_PLATFORM_PSX) return &PS1_KIND;
    if (p == SIGIL_PLATFORM_PS2) return &PS2_KIND;
    if (p == SIGIL_PLATFORM_DREAMCAST) return &VMU_KIND;
    if (p == SIGIL_PLATFORM_GAMECUBE) return &GC_KIND;
    return NULL;
}

static bool in_ids(const char *const *ids, size_t count, const char *owner) {
    for (size_t i = 0; i < count; i++) {
        if (ids[i] && strcmp(ids[i], owner) == 0) return true;
    }
    return false;
}

static bool owned_by_game(const sigil_sync_request *req, const char *owner) {
    if (!owner[0]) return false;
    if (req->save.result && strcmp(req->save.result->title_id, owner) == 0) return true;
    return in_ids(req->game_ids, req->game_id_count, owner);
}

/* The request companion whose ids include `owner`, or SIZE_MAX. */
static size_t companion_of(const sigil_sync_request *req, const char *owner) {
    if (!owner[0]) return SIZE_MAX;
    for (size_t c = 0; c < req->companion_count; c++) {
        if (in_ids(req->companions[c].game_ids, req->companions[c].game_id_count, owner)) return c;
    }
    return SIZE_MAX;
}

static bool claimed(const sigil_sync_request *req, const char *name) {
    for (size_t i = 0; i < req->claimed_count; i++) {
        if (req->claimed[i] && strcmp(req->claimed[i], name) == 0) return true;
    }
    return false;
}

/* The key the state keeps a game's sync under: the lowest of its ids, so
 * every disc of a set shares one entry, or `fallback` when it has none. */
static void key_for(const sigil_sync_request *req, const card_kind *kind, const char *first, const char *const *ids,
                    size_t count, const char *fallback, char out[KEY_MAX]) {
    const char *id = first && first[0] ? first : NULL;
    for (size_t i = 0; i < count; i++) {
        if (ids[i] && ids[i][0] && (!id || strcmp(ids[i], id) < 0)) id = ids[i];
    }
    char raw[KEY_MAX];
    snprintf(raw, sizeof(raw), "%s/%s/%s", kind->platform, req->save.layout ? req->save.layout : "", id ? id : fallback);
    escape(raw, out, KEY_MAX);
}

static void game_key(const sigil_sync_request *req, const card_kind *kind, char out[KEY_MAX]) {
    char stem[SIGIL_SAVE_ENTRY_MAX];
    sigil_content_stem(req->save.content_path, stem, sizeof(stem));
    key_for(req, kind, req->save.result ? req->save.result->title_id : NULL, req->game_ids, req->game_id_count, stem, out);
}

static void ctx_close(sync_ctx *x) {
    state_free(&x->state);
    free(x->companion_keys);
    x->companion_keys = NULL;
}

static int ctx_open(sync_ctx *x, const sigil_sync_request *req, const card_kind *kind) {
    memset(x, 0, sizeof(*x));
    x->req = req;
    x->kind = kind;
    game_key(req, kind, x->game);
    if (req->companion_count) {
        x->companion_keys = calloc(req->companion_count, KEY_MAX);
        if (!x->companion_keys) return SIGIL_ERR_OOM;
        for (size_t c = 0; c < req->companion_count; c++) {
            const sigil_sync_companion *k = &req->companions[c];
            if (!k->game_id_count || !k->game_ids) return SIGIL_ERR_INVALID_ARG;
            key_for(req, kind, NULL, k->game_ids, k->game_id_count, "", x->companion_keys[c]);
        }
    }
    return state_parse(&x->state, req->state, req->state_len);
}

/* The request companion whose state key is `key`, or SIZE_MAX. */
static size_t companion_by_key(const sync_ctx *x, const char *key) {
    for (size_t c = 0; c < x->req->companion_count; c++) {
        if (strcmp(x->companion_keys[c], key) == 0) return c;
    }
    return SIZE_MAX;
}

static int put2(state_t *s, const char *tag, const char *a, const char *value) {
    char prefix[3 * KEY_MAX];
    snprintf(prefix, sizeof(prefix), "%s\t%s\t", tag, a);
    return state_put(s, prefix, value);
}

static const char *get2(const state_t *s, const char *tag, const char *a) {
    char prefix[3 * KEY_MAX];
    snprintf(prefix, sizeof(prefix), "%s\t%s\t", tag, a);
    return state_get(s, prefix);
}

/* ---- saves ------------------------------------------------------------------------ */

enum { OWN_GAME, OWN_OTHER, OWN_NONE, OWN_COMPANION };

/* Whose saves a unit or card read takes: the game's, the game's and its
 * companions', or one companion's (its index). */
#define WHO_GAME  SIZE_MAX
#define WHO_LOCAL (SIZE_MAX - 1)

typedef struct {
    char   name[SIGIL_CARD_NAME_MAX];
    int    device;
    void  *save;
    size_t card;                /* index into the card or volume set it came from, or goes to */
    int    owner;
    size_t companion;           /* the request companion when owner is OWN_COMPANION */
    char   other[KEY_MAX];      /* the owning game's key when owner is OWN_OTHER or OWN_COMPANION */
    char   path[SIGIL_SAVE_PATH_MAX];  /* the file a save in a save folder came from */
} owned_save;

typedef struct {
    const card_kind *kind;
    owned_save      *items;
    size_t           count;
    size_t           cap;
} save_list;

static void save_list_init(save_list *l, const card_kind *kind) {
    memset(l, 0, sizeof(*l));
    l->kind = kind;
}

static void save_list_free(save_list *l) {
    for (size_t i = 0; i < l->count; i++) l->kind->free_save(l->items[i].save);
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

static bool save_list_has(const save_list *l, const char *name, int device) {
    for (size_t i = 0; i < l->count; i++) {
        if (l->items[i].device == device && strcmp(l->items[i].name, name) == 0) return true;
    }
    return false;
}

static owned_save *save_list_push(save_list *l) {
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        owned_save *grown = (owned_save *)realloc(l->items, cap * sizeof(*grown));
        if (!grown) return NULL;
        l->items = grown;
        l->cap = cap;
    }
    owned_save *o = &l->items[l->count];
    memset(o, 0, sizeof(*o));
    return o;
}

/* The name each device's volume travels under in a unit. */
static const char *const DEVICE_NAMES[SIGIL_DEVICE_COUNT] = {
    "", "backup.ram", "cart.ram",
    "vmu_A1.bin", "vmu_A2.bin", "vmu_B1.bin", "vmu_B2.bin", "vmu_C1.bin", "vmu_C2.bin", "vmu_D1.bin", "vmu_D2.bin",
    "", "",
};

static const char *device_name(int device) {
    return device > SIGIL_DEVICE_NONE && device < SIGIL_DEVICE_COUNT ? DEVICE_NAMES[device] : "";
}

static int device_from_name(const char *name) {
    for (int d = SIGIL_DEVICE_INTERNAL; d < SIGIL_DEVICE_COUNT; d++) {
        if (DEVICE_NAMES[d][0] && strcmp(DEVICE_NAMES[d], name) == 0) return d;
    }
    return SIGIL_DEVICE_NONE;
}

/* The unit's hash over each save's name and data, so timestamps and block
 * placement don't read as a new save. Only saves whose owner is `owner`
 * count, and with `other`, only those of that game; `device` limits it to
 * one device when not negative. Volume saves are named by device, as the
 * same name can sit on the internal volume and the cart. */
static int identity_where(const save_list *l, int owner, const char *other, int device, size_t card, char out[33]) {
    sigil_named_md5 *parts = (sigil_named_md5 *)calloc(l->count ? l->count : 1, sizeof(*parts));
    if (!parts) return SIGIL_ERR_OOM;
    size_t n = 0;
    int rc = SIGIL_OK;
    for (size_t i = 0; i < l->count && rc == SIGIL_OK; i++) {
        const owned_save *o = &l->items[i];
        if (owner >= 0 && o->owner != owner) continue;
        if (other && strcmp(o->other, other) != 0) continue;
        if (device >= 0 && o->device != device) continue;
        if (card != SIZE_MAX && o->card != card) continue;
        if (o->device == SIGIL_DEVICE_NONE) snprintf(parts[n].name, sizeof(parts[n].name), "%s", o->name);
        else snprintf(parts[n].name, sizeof(parts[n].name), "%s/%s", device_name(o->device), o->name);
        rc = l->kind->identity(o->save, parts[n].md5);
        n++;
    }
    if (rc == SIGIL_OK) sigil_named_hash(parts, n, out);
    free(parts);
    return rc;
}

/* The hash over the game's own saves, companions' left out. */
static int identity_of(const save_list *l, char out[33]) { return identity_where(l, OWN_GAME, NULL, -1, SIZE_MAX, out); }

/* A save belonging to companion `c`, which the save list records by key. */
static void set_companion(const sync_ctx *x, owned_save *o, size_t c) {
    o->owner = OWN_COMPANION;
    o->companion = c;
    snprintf(o->other, sizeof(o->other), "%s", x->companion_keys[c]);
}

static size_t count_where(const save_list *l, int owner, size_t card) {
    size_t n = 0;
    for (size_t i = 0; i < l->count; i++) {
        if ((owner < 0 || l->items[i].owner == owner) && (card == SIZE_MAX || l->items[i].card == card)) n++;
    }
    return n;
}

static sigil_sync_result *new_result(void) {
    sigil_sync_result *r = (sigil_sync_result *)calloc(1, sizeof(*r));
    if (r) r->struct_version = SIGIL_SYNC_RESULT_V1;
    return r;
}

static bool request_valid(const sigil_sync_request *req) {
    return req && req->struct_version == SIGIL_SYNC_REQUEST_V1 && req->save.open && req->save.content_path &&
           req->save.listing;
}

/* ---- units --------------------------------------------------------------------------
 * A card platform's unit is one per-game card. A volume platform's unit is
 * the per-game internal volume, the per-game cart volume, or a zip of both
 * named "backup.ram" and "cart.ram". */

typedef struct {
    const uint8_t *data;
    size_t         len;
} mem_view;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    const mem_view *m = (const mem_view *)ctx;
    if (off >= m->len) return 0;
    size_t n = m->len - (size_t)off < len ? m->len - (size_t)off : len;
    memcpy(buf, m->data + off, n);
    return (int)n;
}

static int64_t mem_size(void *ctx) { return (int64_t)((const mem_view *)ctx)->len; }

static int load_bytes(const card_kind *kind, const uint8_t *data, size_t len, int device, void **card, int *format) {
    mem_view view = { data, len };
    sigil_io io = { mem_read, mem_size, NULL, &view };
    return kind->load(&io, device, card, format);
}

static int build_card_sized(const save_list *saves, int owner, int device, size_t size, uint8_t **out, size_t *len);

/* One card holding `saves` (those with `owner`, or all when negative) on
 * `device`, sized for the unit. `source` is the card they came from: a
 * holding unit takes its size, and so does a game unit whose saves overflow
 * the platform's standard volume, such as a game's many saves on Yaba
 * Sanshiro's 4 MiB volume. */
static int build_card(const save_list *saves, int owner, int device, const void *source, bool holding, uint8_t **out,
                      size_t *len) {
    const card_kind *kind = saves->kind;
    size_t standard = kind->unit_size(device, source);
    size_t whole = source ? kind->size(source) : 0;
    if (holding && whole) return build_card_sized(saves, owner, device, whole, out, len);
    int rc = build_card_sized(saves, owner, device, standard, out, len);
    if (rc == SIGIL_ERR_NO_SPACE && whole > standard) rc = build_card_sized(saves, owner, device, whole, out, len);
    return rc;
}

static int build_card_sized(const save_list *saves, int owner, int device, size_t size, uint8_t **out, size_t *len) {
    const card_kind *kind = saves->kind;
    void *card = NULL;
    int format = 0;
    int rc = kind->blank(&card, &format, device, size, SIGIL_FORM_RAW, NULL);
    if (rc != SIGIL_OK) return rc;
    for (size_t i = 0; i < saves->count && rc == SIGIL_OK; i++) {
        const owned_save *o = &saves->items[i];
        if ((owner >= 0 && o->owner != owner) || o->device != device) continue;
        rc = kind->inject(card, o->save);
    }
    if (rc == SIGIL_OK) rc = kind->image(card, out, len);
    kind->free_card(card);
    return rc;
}

typedef struct {
    const void *source[SIGIL_DEVICE_COUNT];   /* the card each device's saves came from, by sigil_device */
} unit_sources;

/* The unit of a kind whose saves travel as files: the one file, or a zip of
 * them. Such kinds keep each save as a blob_save. */
static int build_file_unit(const save_list *saves, int owner, const char *stem, uint8_t **out, size_t *len, int *shape,
                           char artifact[SIGIL_SAVE_ENTRY_MAX], char content[33]) {
    size_t n = count_where(saves, owner, SIZE_MAX);
    sigil_zip_member *members = (sigil_zip_member *)calloc(n, sizeof(*members));
    if (!members) return SIGIL_ERR_OOM;
    size_t m = 0;
    for (size_t i = 0; i < saves->count; i++) {
        const owned_save *o = &saves->items[i];
        if (owner >= 0 && o->owner != owner) continue;
        const blob_save *b = (const blob_save *)o->save;
        saves->kind->file_name(o->save, members[m].name, sizeof(members[m].name));
        members[m].data = b->data;
        members[m].len = b->len;
        m++;
    }
    int rc = SIGIL_OK;
    if (n == 1) {
        *out = (uint8_t *)malloc(members[0].len);
        if (!*out) rc = SIGIL_ERR_OOM;
        else {
            memcpy(*out, members[0].data, members[0].len);
            *len = members[0].len;
            *shape = SIGIL_SAVE_SHAPE_SINGLE;
            snprintf(artifact, SIGIL_SAVE_ENTRY_MAX, "%s", members[0].name);
            sigil_md5_of(*out, *len, content);
        }
    } else {
        rc = sigil_zip_store(members, n, out, len);
        sigil_named_md5 *parts = rc == SIGIL_OK ? (sigil_named_md5 *)calloc(n, sizeof(*parts)) : NULL;
        if (rc == SIGIL_OK && !parts) rc = SIGIL_ERR_OOM;
        for (size_t i = 0; parts && i < n; i++) {
            snprintf(parts[i].name, sizeof(parts[i].name), "%s", members[i].name);
            sigil_md5_of(members[i].data, members[i].len, parts[i].md5);
        }
        if (rc == SIGIL_OK) {
            sigil_named_hash(parts, n, content);
            *shape = SIGIL_SAVE_SHAPE_MULTI;
            snprintf(artifact, SIGIL_SAVE_ENTRY_MAX, "%s.zip", stem);
        }
        free(parts);
    }
    free(members);
    return rc;
}

/* The unit for `saves` with `owner`. A unit holding saves on the platform's
 * main volume alone (internal memory, VMU A1) is that one volume; with saves
 * on another device it is a zip, so the member name says which device a
 * volume is whatever its size. A holding unit is always a zip and keeps each
 * source volume's size. */
static int build_unit(const save_list *saves, int owner, const unit_sources *src, bool holding, const char *stem,
                      uint8_t **out, size_t *len, int *shape, char artifact[SIGIL_SAVE_ENTRY_MAX], char content[33]) {
    *out = NULL;
    *len = 0;
    if (count_where(saves, owner, SIZE_MAX) == 0) return SIGIL_OK;
    if (saves->kind->save_files) return build_file_unit(saves, owner, stem, out, len, shape, artifact, content);
    if (saves->kind->has_ids) {
        int rc = build_card(saves, owner, SIGIL_DEVICE_NONE, NULL, false, out, len);
        if (rc != SIGIL_OK) return rc;
        *shape = SIGIL_SAVE_SHAPE_SINGLE;
        sigil_md5_of(*out, *len, content);
        return SIGIL_OK;
    }
    sigil_zip_member members[SIGIL_DEVICE_COUNT];
    int member_device[SIGIL_DEVICE_COUNT];
    size_t n = 0;
    int rc = SIGIL_OK;
    for (int device = SIGIL_DEVICE_INTERNAL; device < SIGIL_DEVICE_COUNT && rc == SIGIL_OK; device++) {
        bool any = false;
        for (size_t i = 0; i < saves->count && !any; i++) {
            any = saves->items[i].device == device && (owner < 0 || saves->items[i].owner == owner);
        }
        if (!any) continue;
        snprintf(members[n].name, sizeof(members[n].name), "%s", device_name(device));
        member_device[n] = device;
        rc = build_card(saves, owner, device, src->source[device], holding, &members[n].data, &members[n].len);
        if (rc == SIGIL_OK) n++;
    }
    if (rc == SIGIL_OK && n == 1 && !holding && member_device[0] == saves->kind->main_device) {
        *out = members[0].data;
        *len = members[0].len;
        *shape = SIGIL_SAVE_SHAPE_SINGLE;
        snprintf(artifact, SIGIL_SAVE_ENTRY_MAX, "%s", members[0].name);
        sigil_md5_of(*out, *len, content);
        return SIGIL_OK;
    }
    if (rc == SIGIL_OK) rc = sigil_zip_store(members, n, out, len);
    if (rc == SIGIL_OK) {
        sigil_named_md5 parts[SIGIL_DEVICE_COUNT];
        for (size_t i = 0; i < n; i++) {
            snprintf(parts[i].name, sizeof(parts[i].name), "%s", members[i].name);
            sigil_md5_of(members[i].data, members[i].len, parts[i].md5);
        }
        sigil_named_hash(parts, n, content);
        *shape = SIGIL_SAVE_SHAPE_MULTI;
        snprintf(artifact, SIGIL_SAVE_ENTRY_MAX, "%s.zip", stem);
    }
    for (size_t i = 0; i < n; i++) free(members[i].data);
    return rc;
}

/* Adds the saves on `card` that `who` takes to `out`. On a card platform the
 * ids say whose a save is; on a volume platform every save is taken, as the
 * game's or the companion's (the caller works out a local volume's owners). */
static int add_card_saves(const sync_ctx *x, const void *card, int format, int device, size_t index, size_t who,
                          save_list *out) {
    sigil_card_listing *listing = NULL;
    int rc = x->kind->list(card, format, &listing);
    if (rc != SIGIL_OK) return rc;
    if (listing->corrupt_count && index == SIZE_MAX) rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
    for (size_t i = 0; i < listing->entry_count && rc == SIGIL_OK; i++) {
        const sigil_card_entry *e = &listing->entries[i];
        size_t companion = SIZE_MAX;
        if (x->kind->has_ids) {
            bool game = owned_by_game(x->req, e->owner_id);
            companion = game ? SIZE_MAX : companion_of(x->req, e->owner_id);
            bool take = who == WHO_GAME    ? game
                        : who == WHO_LOCAL ? game || companion != SIZE_MAX
                                           : companion == who;
            if (!take) continue;
        } else if (who != WHO_GAME && who != WHO_LOCAL) {
            companion = who;
        }
        owned_save *o = save_list_push(out);
        if (!o) { rc = SIGIL_ERR_OOM; break; }
        if (x->kind->extract(card, e, &o->save) != SIGIL_OK) continue;
        if (x->kind->save_key) x->kind->save_key(o->save, o->name);
        else snprintf(o->name, sizeof(o->name), "%s", e->name);
        if (save_list_has(out, o->name, device)) {
            x->kind->free_save(o->save);
            continue;
        }
        o->device = device;
        o->card = index;
        o->owner = OWN_GAME;
        o->companion = SIZE_MAX;
        if (companion != SIZE_MAX) set_companion(x, o, companion);
        out->count++;
    }
    sigil_card_listing_free(listing);
    return rc;
}

/* A unit of save files: each goes onto a scratch card, which lists the
 * game's saves as any card does. */
static int file_unit_saves(const sync_ctx *x, const uint8_t *unit, size_t len, size_t who, save_list *out) {
    sigil_zip_member *members = NULL;
    size_t count = 0;
    bool zip = len >= 4 && sigil_read_le32(unit) == 0x04034b50u;
    int rc = zip ? sigil_zip_read_mem(unit, len, MAX_UNIT_MEMBER, &members, &count) : SIGIL_OK;
    void *card = NULL;
    int format = 0;
    if (rc == SIGIL_OK) rc = x->kind->blank(&card, &format, SIGIL_DEVICE_NONE, 0, SIGIL_FORM_RAW, NULL);
    size_t files = zip ? count : 1;
    for (size_t i = 0; i < files && rc == SIGIL_OK; i++) {
        void *save = NULL;
        rc = x->kind->file_to_save(zip ? members[i].data : unit, zip ? members[i].len : len, &save);
        if (rc == SIGIL_OK) {
            rc = x->kind->inject(card, save);
            x->kind->free_save(save);
        }
    }
    if (rc == SIGIL_OK) rc = add_card_saves(x, card, format, SIGIL_DEVICE_NONE, SIZE_MAX, who, out);
    if (card) x->kind->free_card(card);
    sigil_zip_members_free(members, count);
    return rc == SIGIL_ERR_EXISTS ? SIGIL_ERR_UNSUPPORTED_FORMAT : rc;
}

/* Adds the saves of the unit that `who` takes to `out`, every volume's
 * device noted. Saves of other games in a card platform's unit are left
 * out, since the unit speaks only for its own game. `sizes` receives each
 * device's volume size. SIGIL_ERR_NOT_FOUND when the unit adds none. */
static int unit_saves(const sync_ctx *x, const uint8_t *unit, size_t len, size_t who, save_list *out,
                      size_t sizes[SIGIL_DEVICE_COUNT]) {
    size_t before = out->count;
    if (x->kind->save_files) {
        int rc = file_unit_saves(x, unit, len, who, out);
        return rc == SIGIL_OK && out->count == before ? SIGIL_ERR_NOT_FOUND : rc;
    }
    sigil_zip_member *members = NULL;
    size_t count = 0;
    bool zip = !x->kind->has_ids && len >= 4 && sigil_read_le32(unit) == 0x04034b50u;
    int rc = SIGIL_OK;
    if (zip) rc = sigil_zip_read_mem(unit, len, MAX_UNIT_MEMBER, &members, &count);
    size_t volumes = zip ? count : 1;
    for (size_t v = 0; v < volumes && rc == SIGIL_OK; v++) {
        const uint8_t *data = zip ? members[v].data : unit;
        size_t data_len = zip ? members[v].len : len;
        int device = SIGIL_DEVICE_NONE;
        if (!x->kind->has_ids) {
            device = zip ? device_from_name(members[v].name) : x->kind->main_device;
            if (device == SIGIL_DEVICE_NONE) { rc = SIGIL_ERR_UNSUPPORTED_FORMAT; break; }
        }
        void *card = NULL;
        int format = 0;
        rc = load_bytes(x->kind, data, data_len, device, &card, &format);
        if (rc != SIGIL_OK) break;
        if (device != SIGIL_DEVICE_NONE && !sizes[device]) sizes[device] = x->kind->size(card);
        rc = add_card_saves(x, card, format, device, SIZE_MAX, who, out);
        x->kind->free_card(card);
    }
    sigil_zip_members_free(members, count);
    return rc == SIGIL_OK && out->count == before ? SIGIL_ERR_NOT_FOUND : rc;
}

/* The game's unit, then each companion's unit given, into one list. */
static int request_saves(const sync_ctx *x, const uint8_t *unit, size_t len, save_list *out,
                         size_t sizes[SIGIL_DEVICE_COUNT]) {
    save_list_init(out, x->kind);
    memset(sizes, 0, SIGIL_DEVICE_COUNT * sizeof(size_t));
    int rc = unit_saves(x, unit, len, WHO_GAME, out, sizes);
    for (size_t c = 0; c < x->req->companion_count && rc == SIGIL_OK; c++) {
        const sigil_sync_companion *k = &x->req->companions[c];
        if (k->unit) rc = unit_saves(x, k->unit, k->unit_len, c, out, sizes);
    }
    for (size_t i = 0; i < out->count && rc == SIGIL_OK && x->kind->foreign; i++) {
        const owned_save *o = &out->items[i];
        if (o->owner == OWN_COMPANION && x->kind->foreign(x->req, o->save)) rc = SIGIL_ERR_INVALID_ARG;
    }
    if (rc != SIGIL_OK) save_list_free(out);
    return rc;
}

/* ---- card platforms: the game's card files --------------------------------------- */

typedef struct {
    char  path[SIGIL_SAVE_PATH_MAX];
    void *card;
    int   format;
    bool  primary;
    bool  changed;
    bool  folder;   /* a PCSX2 folder card: `path` is its directory */
} card_file;

typedef struct {
    const card_kind *kind;
    card_file        files[MAX_CARD_FILES];
    size_t           count;
    char             primary_path[SIGIL_SAVE_PATH_MAX];  /* where the game's own card goes */
} card_set;

static void card_set_free(card_set *s) {
    for (size_t i = 0; i < s->count; i++) s->kind->free_card(s->files[i].card);
    s->count = 0;
}

static int load_card_file(const sigil_sync_request *req, const char *path, card_set *s) {
    if (s->count >= MAX_CARD_FILES) return SIGIL_OK;
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->files[i].path, path) == 0) return SIGIL_OK;
    }
    sigil_io *io = req->save.open(req->save.open_ctx, path);
    if (!io) return SIGIL_OK;
    card_file *f = &s->files[s->count];
    memset(f, 0, sizeof(*f));
    int rc = s->kind->load(io, SIGIL_DEVICE_NONE, &f->card, &f->format);
    sigil_io_close(io);
    if (rc == SIGIL_ERR_UNSUPPORTED_FORMAT) return SIGIL_OK;
    if (rc != SIGIL_OK) return rc;
    snprintf(f->path, sizeof(f->path), "%s", path);
    f->primary = strcmp(path, s->primary_path) == 0;
    s->count++;
    return SIGIL_OK;
}

/* ---- PCSX2 folder cards ----------------------------------------------------------- */

/* A PCSX2 folder card is a directory named as the card file would be, holding
 * _pcsx2_superblock and one directory per save (MemoryCardFolder.cpp). sigil
 * loads it as a card holding the folders the game sees, and writes back only
 * the folders of the game and of each companion the restore carries. */

#define FOLDER_CARD_FILE_MAX (8u * 1024u * 1024u)

typedef struct {
    char (*paths)[SIGIL_SAVE_PATH_MAX];
    size_t count;
} path_list;

/* The listing holds files under `dir`/: it is a folder card. */
static bool is_folder_card(const sigil_sync_request *req, const char *dir) {
    size_t n = strlen(dir);
    for (size_t i = 0; i < req->save.listing_count; i++) {
        const char *p = req->save.listing[i];
        if (p && strncmp(p, dir, n) == 0 && p[n] == '/') return true;
    }
    return false;
}

/* The save folder `path` sits in under `dir`/, into `name`; false for a file
 * of the card itself, such as _pcsx2_superblock. */
static bool folder_card_save_of(const char *dir, const char *path, char name[SIGIL_CARD_NAME_MAX]) {
    size_t n = strlen(dir);
    if (strncmp(path, dir, n) != 0 || path[n] != '/') return false;
    const char *start = path + n + 1;
    const char *slash = strchr(start, '/');
    if (!slash || (size_t)(slash - start) >= SIGIL_CARD_NAME_MAX || slash == start) return false;
    snprintf(name, SIGIL_CARD_NAME_MAX, "%.*s", (int)(slash - start), start);
    return true;
}

/* PCSX2 shows a game the folders its filter names and the system folders
 * every game reads (MemoryCardFolder.cpp, the DATA-SYSTEM and BWNETCNF
 * filter); of those, the card needs the game's, its companions' and the
 * system's. */
static bool folder_card_shows(const sigil_sync_request *req, const char *name) {
    if (strstr(name, "DATA-SYSTEM") || strstr(name, "BWNETCNF")) return true;
    char owner[SIGIL_CARD_OWNER_MAX];
    sigil_card_sony_owner(name, owner);
    return owner[0] && (owned_by_game(req, owner) || companion_of(req, owner) != SIZE_MAX);
}

/* Reads the files of save folder `name` on folder card `dir` and packs them. */
static int read_save_folder(const sigil_sync_request *req, const char *dir, const char *name, sigil_ps2_save *out) {
    char prefix[SIGIL_SAVE_PATH_MAX];
    snprintf(prefix, sizeof(prefix), "%s/%s/", dir, name);
    size_t plen = strlen(prefix);
    sigil_ps2_folder_file *files = calloc(req->save.listing_count + 1, sizeof(*files));
    if (!files) return SIGIL_ERR_OOM;
    size_t n = 0;
    int rc = SIGIL_OK;
    for (size_t i = 0; i < req->save.listing_count && rc == SIGIL_OK; i++) {
        const char *p = req->save.listing[i];
        if (!p || strncmp(p, prefix, plen) != 0 || strlen(p + plen) >= PS2_FOLDER_PATH_MAX) continue;
        sigil_io *io = req->save.open(req->save.open_ctx, p);
        if (!io) continue;
        rc = sigil_bram_read_all(io, FOLDER_CARD_FILE_MAX, &files[n].data, &files[n].len);
        sigil_io_close(io);
        if (rc == SIGIL_OK) snprintf(files[n++].path, PS2_FOLDER_PATH_MAX, "%s", p + plen);
    }
    if (rc == SIGIL_OK) rc = n ? sigil_ps2_pack(name, files, n, out) : SIGIL_ERR_NOT_FOUND;
    sigil_ps2_folder_files_free(files, n);
    return rc;
}

static int compare_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* The save folders under `dir` the game sees, one name each, sorted. */
static size_t folder_card_saves(const sigil_sync_request *req, const char *dir, char (**out)[SIGIL_CARD_NAME_MAX]) {
    *out = calloc(req->save.listing_count + 1, SIGIL_CARD_NAME_MAX);
    if (!*out) return 0;
    size_t n = 0;
    for (size_t i = 0; i < req->save.listing_count; i++) {
        char name[SIGIL_CARD_NAME_MAX];
        if (!req->save.listing[i] || !folder_card_save_of(dir, req->save.listing[i], name)) continue;
        bool seen = false;
        for (size_t k = 0; k < n && !seen; k++) seen = strcmp((*out)[k], name) == 0;
        if (!seen && folder_card_shows(req, name)) snprintf((*out)[n++], SIGIL_CARD_NAME_MAX, "%s", name);
    }
    qsort(*out, n, SIGIL_CARD_NAME_MAX, compare_names);
    return n;
}

/* Loads folder card `dir` as a card holding the save folders the game sees.
 * A folder whose index doesn't parse is left off, as an unreadable card is. */
static int load_folder_card(const sync_ctx *x, const char *dir, card_set *s) {
    if (s->count >= MAX_CARD_FILES) return SIGIL_OK;
    card_file *f = &s->files[s->count];
    memset(f, 0, sizeof(*f));
    int rc = x->kind->blank(&f->card, &f->format, SIGIL_DEVICE_NONE, 0, SIGIL_FORM_RAW, NULL);
    if (rc != SIGIL_OK) return rc;
    char (*names)[SIGIL_CARD_NAME_MAX] = NULL;
    size_t n = folder_card_saves(x->req, dir, &names);
    if (!names) rc = SIGIL_ERR_OOM;
    for (size_t i = 0; i < n && rc == SIGIL_OK; i++) {
        sigil_ps2_save save;
        int read = read_save_folder(x->req, dir, names[i], &save);
        if (read == SIGIL_ERR_UNSUPPORTED_FORMAT || read == SIGIL_ERR_NOT_FOUND) continue;
        rc = read;
        if (rc == SIGIL_OK) {
            rc = sigil_ps2_inject((sigil_ps2_card *)f->card, &save);
            sigil_ps2_save_free(&save);
        }
    }
    free(names);
    if (rc != SIGIL_OK) { x->kind->free_card(f->card); return rc; }
    snprintf(f->path, sizeof(f->path), "%s", dir);
    f->folder = true;
    f->primary = strcmp(dir, s->primary_path) == 0;
    s->count++;
    return SIGIL_OK;
}

/* Sets the artifact to the content stem with the extension of `path`. */
static void artifact_from(const sigil_sync_request *req, const char *path, char artifact[SIGIL_SAVE_ENTRY_MAX]) {
    char stem[SIGIL_SAVE_ENTRY_MAX];
    sigil_content_stem(req->save.content_path, stem, sizeof(stem));
    const char *base = strrchr(path, '/');
    const char *ext = strrchr(base ? base : path, '.');
    snprintf(artifact, SIGIL_SAVE_ENTRY_MAX, "%s%s", stem, ext ? ext : "");
}

/* The game's own card files and the shared ones beside them, its own first.
 * The game's own card is the layout's primary member; a layout whose game
 * cards are all shared uses its first shared card. */
static int gather_cards(const sync_ctx *x, card_set *s, char artifact[SIGIL_SAVE_ENTRY_MAX]) {
    const sigil_sync_request *req = x->req;
    memset(s, 0, sizeof(*s));
    s->kind = x->kind;
    artifact[0] = '\0';
    sigil_save_request resolve = req->save;
    resolve.open = NULL;
    sigil_save_unit *unit = NULL;
    int rc = sigil_save_resolve(&resolve, &unit);
    if (rc != SIGIL_OK) return rc;

    for (size_t i = 0; i < unit->member_count && !s->primary_path[0]; i++) {
        if (unit->members[i].role != SIGIL_SAVE_ROLE_PRIMARY) continue;
        snprintf(s->primary_path, SIGIL_SAVE_PATH_MAX, "%s", unit->members[i].path);
        snprintf(artifact, SIGIL_SAVE_ENTRY_MAX, "%s", unit->members[i].entry);
    }
    for (size_t i = 0; i < unit->expected_count && !s->primary_path[0]; i++) {
        if (unit->expected[i].role != SIGIL_SAVE_ROLE_PRIMARY) continue;
        snprintf(s->primary_path, SIGIL_SAVE_PATH_MAX, "%s", unit->expected[i].path);
        snprintf(artifact, SIGIL_SAVE_ENTRY_MAX, "%s", unit->expected[i].entry);
    }
    char shared[MAX_SHARED][SIGIL_SAVE_PATH_MAX];
    size_t shared_count = sigil_save_shared_paths(&req->save, shared, MAX_SHARED);
    bool folder[MAX_SHARED] = { false };
    for (size_t i = 0; i < shared_count; i++) folder[i] = x->kind->folder_cards && is_folder_card(req, shared[i]);
    if (!s->primary_path[0] && shared_count > 0) {
        size_t pick = 0;
        for (size_t i = 0; i < shared_count; i++) {
            bool listed = folder[i];
            for (size_t k = 0; k < req->save.listing_count && !listed; k++) {
                listed = req->save.listing[k] && strcmp(req->save.listing[k], shared[i]) == 0;
            }
            if (listed) { pick = i; break; }
        }
        snprintf(s->primary_path, SIGIL_SAVE_PATH_MAX, "%s", shared[pick]);
        artifact_from(req, shared[pick], artifact);
    }

    for (size_t i = 0; i < unit->member_count && rc == SIGIL_OK; i++) rc = load_card_file(req, unit->members[i].path, s);
    for (size_t i = 0; i < unit->unkeyed_count && rc == SIGIL_OK; i++) rc = load_card_file(req, unit->unkeyed[i], s);
    for (size_t i = 0; i < shared_count && rc == SIGIL_OK; i++) {
        if (folder[i]) rc = load_folder_card(x, shared[i], s);
    }
    sigil_save_unit_free(unit);
    if (rc != SIGIL_OK) card_set_free(s);
    return rc;
}

/* The game's and its companions' saves across its cards, one per name, the
 * first card holding a name winning. */
static int gather_saves(const sync_ctx *x, const card_set *s, save_list *out) {
    save_list_init(out, s->kind);
    int rc = SIGIL_OK;
    for (size_t c = 0; c < s->count && rc == SIGIL_OK; c++) {
        rc = add_card_saves(x, s->files[c].card, s->files[c].format, SIGIL_DEVICE_NONE, c, WHO_LOCAL, out);
    }
    if (rc != SIGIL_OK) save_list_free(out);
    return rc;
}

/* The restore replaces this companion's saves: the request carries its unit. */
static bool companion_restored(const sync_ctx *x, size_t c) {
    return c < x->req->companion_count && x->req->companions[c].unit;
}

/* Removes from card `c` every save of the game and of each companion the
 * restore carries a unit for, and records on which card each name sat so a
 * newer copy goes back to the same card. */
static int clear_game(const sync_ctx *x, card_set *s, size_t c, save_list *placements) {
    card_file *f = &s->files[c];
    sigil_card_listing *listing = NULL;
    int rc = s->kind->list(f->card, f->format, &listing);
    if (rc != SIGIL_OK) return rc;
    for (size_t i = 0; i < listing->entry_count && rc == SIGIL_OK; i++) {
        const sigil_card_entry *e = &listing->entries[i];
        if (!owned_by_game(x->req, e->owner_id) && !companion_restored(x, companion_of(x->req, e->owner_id))) continue;
        rc = s->kind->remove(f->card, e);
        f->changed = true;
        for (size_t k = 0; k < placements->count; k++) {
            if (strcmp(placements->items[k].name, e->name) == 0) placements->items[k].card = c;
        }
    }
    sigil_card_listing_free(listing);
    return rc;
}

static size_t primary_card(const sync_ctx *x, card_set *s, int *rc) {
    for (size_t c = 0; c < s->count; c++) {
        if (s->files[c].primary) return c;
    }
    if (!s->primary_path[0] || s->count >= MAX_CARD_FILES) { *rc = SIGIL_ERR_INVALID_ARG; return 0; }
    card_file *f = &s->files[s->count];
    memset(f, 0, sizeof(*f));
    int form = s->kind->new_form ? s->kind->new_form(x->req) : SIGIL_FORM_RAW;
    *rc = s->kind->blank(&f->card, &f->format, SIGIL_DEVICE_NONE, 0, form, NULL);
    if (*rc != SIGIL_OK) return 0;
    f->primary = true;
    f->changed = true;
    snprintf(f->path, sizeof(f->path), "%s", s->primary_path);
    return s->count++;
}

/* Writes each changed file, then reads it back and checks every save placed on it. */
static int write_and_verify(const sync_ctx *x, const char *path, int device, const void *card, const save_list *placed,
                            size_t index) {
    const sigil_sync_request *req = x->req;
    uint8_t *image = NULL;
    size_t len = 0;
    int rc = x->kind->image(card, &image, &len);
    if (rc == SIGIL_OK && req->write(req->write_ctx, path, image, len) != 0) rc = SIGIL_ERR_IO;
    free(image);
    if (rc != SIGIL_OK) return rc;

    sigil_io *io = req->save.open(req->save.open_ctx, path);
    if (!io) return SIGIL_ERR_IO;
    void *back = NULL;
    int format = 0;
    rc = x->kind->load(io, device, &back, &format);
    sigil_io_close(io);
    for (size_t i = 0; i < placed->count && rc == SIGIL_OK; i++) {
        if (placed->items[i].card != index) continue;
        if (x->kind->verify(back, placed->items[i].save) != SIGIL_OK) rc = SIGIL_ERR_IO;
    }
    if (back) x->kind->free_card(back);
    return rc == SIGIL_ERR_UNSUPPORTED_FORMAT ? SIGIL_ERR_IO : rc;
}

/* Names the save that didn't fit on `card` and the blocks it lacked: the
 * blocks it takes on an empty card like this one, less the card's free
 * blocks; 0 when the card has the blocks and lacks a directory slot. */
static void note_overflow(const sync_ctx *x, const void *card, int format, int device, const owned_save *o,
                          sigil_sync_result *r) {
    snprintf(r->overflow, sizeof(r->overflow), "%s", o->name);
    r->overflow_blocks = 0;
    sigil_card_listing *have = NULL, *need = NULL;
    void *empty = NULL;
    int empty_format = 0;
    if (x->kind->list(card, format, &have) == SIGIL_OK &&
        x->kind->blank(&empty, &empty_format, device, x->kind->size(card), SIGIL_FORM_RAW, card) == SIGIL_OK &&
        x->kind->inject(empty, o->save) == SIGIL_OK && x->kind->list(empty, empty_format, &need) == SIGIL_OK &&
        need->entry_count == 1 && need->entries[0].blocks > have->free_blocks) {
        r->overflow_blocks = need->entries[0].blocks - have->free_blocks;
    }
    sigil_card_listing_free(have);
    sigil_card_listing_free(need);
    if (empty) x->kind->free_card(empty);
}

/* The restore rewrites save folder `name` on a folder card: it is the game's,
 * or a companion's the restore carries a unit for. */
static bool folder_rewritten(const sync_ctx *x, const char *name) {
    char owner[SIGIL_CARD_OWNER_MAX];
    sigil_card_sony_owner(name, owner);
    return owner[0] && (owned_by_game(x->req, owner) || companion_restored(x, companion_of(x->req, owner)));
}

/* `path` holds exactly `len` bytes of `data`. */
static bool file_holds(const sigil_sync_request *req, const char *path, const uint8_t *data, size_t len) {
    sigil_io *io = req->save.open(req->save.open_ctx, path);
    if (!io) return false;
    uint8_t *have = NULL;
    size_t have_len = 0;
    int rc = sigil_bram_read_all(io, len + 1, &have, &have_len);
    sigil_io_close(io);
    bool same = rc == SIGIL_OK && have_len == len && (len == 0 || memcmp(have, data, len) == 0);
    free(have);
    return same;
}

static bool file_listed_in(const sigil_ps2_folder_file *files, size_t n, const char *rel) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(files[i].path, rel) == 0) return true;
    }
    return false;
}

/* Adds to `doomed` the files under `prefix` the unpacked folder `files`
 * lacks, or all of them when `files` is NULL. */
static void doom_folder_files(const sync_ctx *x, const char *prefix, const sigil_ps2_folder_file *files, size_t n,
                              path_list *doomed) {
    size_t plen = strlen(prefix);
    for (size_t i = 0; i < x->req->save.listing_count; i++) {
        const char *p = x->req->save.listing[i];
        if (!p || strncmp(p, prefix, plen) != 0 || (files && file_listed_in(files, n, p + plen))) continue;
        snprintf(doomed->paths[doomed->count++], SIGIL_SAVE_PATH_MAX, "%s", p);
    }
}

/* Writes save folder `save` under `prefix` as PCSX2 keeps it, file by file
 * where the bytes differ, and reads each back; with `apply` false it writes
 * nothing. Its files the save lacks go in `doomed`. */
static int write_save_folder(const sync_ctx *x, const char *prefix, const sigil_ps2_save *save, bool apply,
                             path_list *doomed) {
    sigil_ps2_folder_file *files = NULL;
    size_t n = 0;
    int rc = sigil_ps2_unpack(save, &files, &n);
    if (rc != SIGIL_OK) return rc;
    sigil_ps2_save back;
    char name[33], want[33], got[33];
    snprintf(name, sizeof(name), "%.32s", (const char *)save->entry + 0x40);
    rc = sigil_ps2_pack(name, files, n, &back);
    if (rc == SIGIL_OK) {
        sigil_ps2_save_md5(save, want);
        sigil_ps2_save_md5(&back, got);
        sigil_ps2_save_free(&back);
        if (strcmp(want, got) != 0) rc = SIGIL_ERR_IO;
    }
    for (size_t i = 0; i < n && rc == SIGIL_OK && apply; i++) {
        char path[SIGIL_SAVE_PATH_MAX];
        snprintf(path, sizeof(path), "%s%s", prefix, files[i].path);
        if (file_holds(x->req, path, files[i].data, files[i].len)) continue;
        if (x->req->write(x->req->write_ctx, path, files[i].data, files[i].len) != 0 ||
            !file_holds(x->req, path, files[i].data, files[i].len)) {
            rc = SIGIL_ERR_IO;
        }
    }
    if (rc == SIGIL_OK) doom_folder_files(x, prefix, files, n, doomed);
    sigil_ps2_folder_files_free(files, n);
    return rc;
}

/* Brings folder card `f` on disk in line with its card for the folders the
 * restore rewrites, and removes their files the card lacks. Without `apply`
 * it only counts the files that would go. */
static int sync_folder_card(const sync_ctx *x, const card_file *f, bool apply, size_t *removes) {
    *removes = 0;
    path_list doomed = { calloc(x->req->save.listing_count + 1, SIGIL_SAVE_PATH_MAX), 0 };
    sigil_card_listing *listing = NULL;
    int rc = doomed.paths ? x->kind->list(f->card, f->format, &listing) : SIGIL_ERR_OOM;
    for (size_t i = 0; rc == SIGIL_OK && i < listing->entry_count; i++) {
        const sigil_card_entry *e = &listing->entries[i];
        if (!folder_rewritten(x, e->name)) continue;
        void *save = NULL;
        rc = x->kind->extract(f->card, e, &save);
        char prefix[SIGIL_SAVE_PATH_MAX];
        snprintf(prefix, sizeof(prefix), "%s/%s/", f->path, e->name);
        if (rc == SIGIL_OK) rc = write_save_folder(x, prefix, (const sigil_ps2_save *)save, apply, &doomed);
        if (save) x->kind->free_save(save);
    }
    for (size_t i = 0; rc == SIGIL_OK && i < x->req->save.listing_count; i++) {
        char name[SIGIL_CARD_NAME_MAX];
        const char *p = x->req->save.listing[i];
        if (!p || !folder_card_save_of(f->path, p, name) || !folder_rewritten(x, name)) continue;
        bool kept = false;
        for (size_t k = 0; k < listing->entry_count && !kept; k++) kept = strcmp(listing->entries[k].name, name) == 0;
        if (!kept) snprintf(doomed.paths[doomed.count++], SIGIL_SAVE_PATH_MAX, "%s", p);
    }
    sigil_card_listing_free(listing);
    *removes = doomed.count;
    for (size_t i = 0; rc == SIGIL_OK && apply && i < doomed.count; i++) {
        if (x->req->remove(x->req->write_ctx, doomed.paths[i]) != 0) rc = SIGIL_ERR_IO;
    }
    free(doomed.paths);
    return rc;
}

/* Writes a full formatted _pcsx2_superblock to folder card `dir` when the one
 * there is missing or PCSX2 wouldn't read it as formatted; PCSX2 hides every
 * save on such a card. */
static int ensure_superblock(const sync_ctx *x, const char *dir) {
    char path[SIGIL_SAVE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/_pcsx2_superblock", dir);
    sigil_io *io = x->req->save.open(x->req->save.open_ctx, path);
    uint8_t *have = NULL;
    size_t have_len = 0;
    if (io) {
        sigil_bram_read_all(io, PS2_FOLDER_SUPERBLOCK_SIZE + 1, &have, &have_len);
        sigil_io_close(io);
    }
    bool usable = have && sigil_ps2_folder_superblock_usable(have, have_len);
    free(have);
    if (usable) return SIGIL_OK;
    uint8_t sb[PS2_FOLDER_SUPERBLOCK_SIZE];
    sigil_ps2_folder_superblock(sb);
    if (x->req->write(x->req->write_ctx, path, sb, sizeof(sb)) != 0 || !file_holds(x->req, path, sb, sizeof(sb))) {
        return SIGIL_ERR_IO;
    }
    return SIGIL_OK;
}

static int place_on_cards(const sync_ctx *x, card_set *cards, save_list *incoming, sigil_sync_result *r) {
    int rc = SIGIL_OK;
    for (size_t i = 0; i < incoming->count; i++) incoming->items[i].card = SIZE_MAX;
    for (size_t c = 0; c < cards->count && rc == SIGIL_OK; c++) rc = clear_game(x, cards, c, incoming);
    size_t primary = rc == SIGIL_OK ? primary_card(x, cards, &rc) : 0;
    for (size_t c = 0; c < cards->count && rc == SIGIL_OK; c++) {
        if (cards->files[c].changed && !cards->kind->writable(cards->files[c].format)) rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
    for (size_t i = 0; i < incoming->count && rc == SIGIL_OK; i++) {
        owned_save *o = &incoming->items[i];
        if (o->card == SIZE_MAX) o->card = primary;
        card_file *f = &cards->files[o->card];
        rc = cards->kind->inject(f->card, o->save);
        if (rc == SIGIL_ERR_NO_SPACE) note_overflow(x, f->card, f->format, SIGIL_DEVICE_NONE, o, r);
        f->changed = true;
    }
    for (size_t c = 0; c < cards->count && rc == SIGIL_OK; c++) {
        size_t removes = 0;
        if (cards->files[c].changed && cards->files[c].folder) rc = sync_folder_card(x, &cards->files[c], false, &removes);
        if (rc == SIGIL_OK && removes && !x->req->remove) rc = SIGIL_ERR_INVALID_ARG;
    }
    for (size_t c = 0; c < cards->count && rc == SIGIL_OK; c++) {
        size_t removes = 0;
        if (!cards->files[c].changed) continue;
        if (!cards->files[c].folder) {
            rc = write_and_verify(x, cards->files[c].path, SIGIL_DEVICE_NONE, cards->files[c].card, incoming, c);
            continue;
        }
        rc = ensure_superblock(x, cards->files[c].path);
        if (rc == SIGIL_OK) rc = sync_folder_card(x, &cards->files[c], true, &removes);
    }
    return rc;
}

/* ---- volume platforms: the game's volumes ----------------------------------------- */

typedef struct {
    sigil_volume_target target;
    char                key[KEY_MAX];   /* escaped state key of the volume */
    void               *card;           /* NULL when the file doesn't exist yet */
    int                 format;
} volume_file;

typedef struct {
    const card_kind *kind;
    volume_file      files[SIGIL_VOLUME_TARGETS_MAX];
    size_t           count;
} volume_set;

static void volume_set_free(volume_set *s) {
    for (size_t i = 0; i < s->count; i++) {
        if (s->files[i].card) s->kind->free_card(s->files[i].card);
    }
    s->count = 0;
}

static int gather_volumes(const sync_ctx *x, volume_set *s) {
    memset(s, 0, sizeof(*s));
    s->kind = x->kind;
    sigil_volume_target targets[SIGIL_VOLUME_TARGETS_MAX];
    size_t count = 0;
    int rc = sigil_save_volume_targets(&x->req->save, targets, &count);
    for (size_t i = 0; i < count && rc == SIGIL_OK; i++) {
        volume_file *f = &s->files[s->count++];
        f->target = targets[i];
        char raw[KEY_MAX];
        snprintf(raw, sizeof(raw), "%s/%s/%s", x->kind->platform, x->req->save.layout ? x->req->save.layout : "",
                 f->target.path);
        escape(raw, f->key, sizeof(f->key));
        sigil_io *io = x->req->save.open(x->req->save.open_ctx, f->target.path);
        if (!io) continue;
        int64_t size = io->size ? io->size(io->ctx) : -1;
        if (size != 0) rc = x->kind->load(io, f->target.device, &f->card, &f->format);
        sigil_io_close(io);
    }
    if (rc != SIGIL_OK) volume_set_free(s);
    return rc;
}

/* The save-name table gives the game this save by one of its ids. */
static bool in_name_table(const sync_ctx *x, const char *name) {
    const char *ids[1 + 64];
    size_t n = 0;
    if (x->req->save.result && x->req->save.result->title_id[0]) ids[n++] = x->req->save.result->title_id;
    for (size_t i = 0; i < x->req->game_id_count && n < sizeof(ids) / sizeof(ids[0]); i++) ids[n++] = x->req->game_ids[i];
    return n > 0 && sigil_save_names_match(sigil_save_name_table, sigil_save_name_table_count, x->kind->platform, name,
                                           ids, n);
}

/* Who a save on volume file `f` belongs to: the user's claim, then the owner
 * the state learned (on a per-game file only a companion's counts), then
 * the per-game file's game, then in managed mode the game a shared volume
 * was swapped in for, then the save-name table. */
static int volume_owner(const sync_ctx *x, const volume_file *f, const char *name, char other[KEY_MAX],
                        size_t *companion) {
    other[0] = '\0';
    *companion = SIZE_MAX;
    if (claimed(x->req, name)) return OWN_GAME;
    char escaped[3 * SIGIL_CARD_NAME_MAX], prefix[3 * KEY_MAX];
    escape(name, escaped, sizeof(escaped));
    snprintf(prefix, sizeof(prefix), "owner\t%s\t%s\t", f->key, escaped);
    const char *learned = state_get(&x->state, prefix);
    if (learned) {
        if (strcmp(learned, x->game) == 0) return OWN_GAME;
        size_t c = companion_by_key(x, learned);
        if (c != SIZE_MAX || !f->target.per_game) {
            snprintf(other, KEY_MAX, "%s", learned);
            *companion = c;
            return c != SIZE_MAX ? OWN_COMPANION : OWN_OTHER;
        }
    }
    if (f->target.per_game) return OWN_GAME;
    const char *prepared = get2(&x->state, "prepared", f->key);
    if (x->req->mode == SIGIL_SYNC_MANAGED && prepared && strcmp(prepared, x->game) == 0) return OWN_GAME;
    return in_name_table(x, name) ? OWN_GAME : OWN_NONE;
}

static int gather_volume_saves(const sync_ctx *x, const volume_set *s, save_list *out) {
    save_list_init(out, s->kind);
    int rc = SIGIL_OK;
    for (size_t v = 0; v < s->count && rc == SIGIL_OK; v++) {
        const volume_file *f = &s->files[v];
        if (!f->card) continue;
        size_t before = out->count;
        rc = add_card_saves(x, f->card, f->format, f->target.device, v, WHO_LOCAL, out);
        for (size_t i = before; i < out->count; i++) {
            owned_save *o = &out->items[i];
            o->owner = volume_owner(x, f, o->name, o->other, &o->companion);
        }
    }
    if (rc != SIGIL_OK) save_list_free(out);
    return rc;
}

/* The state learns who owns each save of the game on a shared volume, and
 * each companion's save on any volume, so collect can tell them apart. */
static int learn_owners(sync_ctx *x, const volume_set *s, const save_list *saves) {
    int rc = SIGIL_OK;
    for (size_t i = 0; i < saves->count && rc == SIGIL_OK; i++) {
        const owned_save *o = &saves->items[i];
        if (o->card == SIZE_MAX) continue;
        bool game = o->owner == OWN_GAME && !s->files[o->card].target.per_game;
        if (!game && o->owner != OWN_COMPANION) continue;
        char escaped[3 * SIGIL_CARD_NAME_MAX], prefix[3 * KEY_MAX];
        escape(o->name, escaped, sizeof(escaped));
        snprintf(prefix, sizeof(prefix), "owner\t%s\t%s\t", s->files[o->card].key, escaped);
        rc = state_put(&x->state, prefix, game ? x->game : o->other);
    }
    return rc;
}

/* Records, per shared volume, every save on it and the ones with no owner. */
static int note_volumes(sync_ctx *x, const volume_set *s, const save_list *saves) {
    int rc = SIGIL_OK;
    for (size_t v = 0; v < s->count && rc == SIGIL_OK; v++) {
        if (s->files[v].target.per_game) continue;
        char all[33], none[33];
        rc = identity_where(saves, -1, NULL, -1, v, all);
        if (rc == SIGIL_OK) rc = identity_where(saves, OWN_NONE, NULL, -1, v, none);
        if (rc == SIGIL_OK) rc = put2(&x->state, "seen", s->files[v].key, all);
        if (rc == SIGIL_OK) rc = put2(&x->state, "held", s->files[v].key, count_where(saves, OWN_NONE, v) ? none : NULL);
    }
    return rc;
}

static int finish_collect(sync_ctx *x, const save_list *saves, const unit_sources *src, sigil_sync_result *r);

static int collect_volumes(sync_ctx *x, sigil_sync_result *r) {
    volume_set vols;
    int rc = gather_volumes(x, &vols);
    if (rc != SIGIL_OK) return rc;
    save_list saves;
    rc = gather_volume_saves(x, &vols, &saves);
    if (rc != SIGIL_OK) { volume_set_free(&vols); return rc; }

    unit_sources src;
    memset(&src, 0, sizeof(src));
    for (size_t v = 0; v < vols.count; v++) src.source[vols.files[v].target.device] = vols.files[v].card;
    char holding_artifact[SIGIL_SAVE_ENTRY_MAX], holding_hash[33];
    int holding_shape = 0;
    rc = finish_collect(x, &saves, &src, r);
    if (rc == SIGIL_OK) {
        rc = build_unit(&saves, OWN_NONE, &src, true, "holding", &r->holding, &r->holding_len, &holding_shape,
                        holding_artifact, holding_hash);
    }
    size_t unowned = count_where(&saves, OWN_NONE, SIZE_MAX);
    if (rc == SIGIL_OK && unowned) {
        r->unowned = calloc(unowned, SIGIL_CARD_NAME_MAX);
        if (!r->unowned) rc = SIGIL_ERR_OOM;
        for (size_t i = 0; i < saves.count && rc == SIGIL_OK; i++) {
            if (saves.items[i].owner == OWN_NONE) snprintf(r->unowned[r->unowned_count++], SIGIL_CARD_NAME_MAX, "%s", saves.items[i].name);
        }
    }
    if (rc == SIGIL_OK) rc = learn_owners(x, &vols, &saves);
    if (rc == SIGIL_OK) rc = note_volumes(x, &vols, &saves);
    save_list_free(&saves);
    volume_set_free(&vols);
    return rc;
}

/* Managed: a shared volume can be swapped only when every save on it that
 * isn't the game's has been passed on: another game's saves match what that
 * game last synced, and saves with no owner match the last holding unit. */
static int check_swappable(const sync_ctx *x, const volume_set *s, const save_list *local) {
    for (size_t v = 0; v < s->count; v++) {
        if (s->files[v].target.per_game || count_where(local, OWN_NONE, v) == 0) continue;
        char none[33];
        int rc = identity_where(local, OWN_NONE, NULL, -1, v, none);
        if (rc != SIGIL_OK) return rc;
        const char *held = get2(&x->state, "held", s->files[v].key);
        if (!held || strcmp(held, none) != 0) return SIGIL_ERR_UNCOLLECTED;
    }
    for (size_t i = 0; i < local->count; i++) {
        if (local->items[i].owner != OWN_OTHER) continue;
        char theirs[33];
        int rc = identity_where(local, OWN_OTHER, local->items[i].other, -1, SIZE_MAX, theirs);
        if (rc != SIGIL_OK) return rc;
        const char *synced = get2(&x->state, "synced", local->items[i].other);
        if (!synced || strcmp(synced, theirs) != 0) return SIGIL_ERR_UNCOLLECTED;
    }
    return SIGIL_OK;
}

/* Unmanaged: a shared volume takes an inject only when nothing on it changed
 * since the last collect saw it. */
static int check_unchanged(const sync_ctx *x, const volume_set *s, const save_list *local) {
    for (size_t v = 0; v < s->count; v++) {
        if (s->files[v].target.per_game || count_where(local, -1, v) == 0) continue;
        char all[33];
        int rc = identity_where(local, -1, NULL, -1, v, all);
        if (rc != SIGIL_OK) return rc;
        const char *seen = get2(&x->state, "seen", s->files[v].key);
        if (!seen || strcmp(seen, all) != 0) return SIGIL_ERR_UNCOLLECTED;
    }
    return SIGIL_OK;
}

/* Removes the game's saves, and those of each companion the restore carries
 * a unit for, from `card`, listing again after each removal, since a Sega
 * CD delete moves the saves after it. */
static int remove_game_saves(const sync_ctx *x, const volume_file *f, void *card) {
    for (;;) {
        sigil_card_listing *listing = NULL;
        int rc = x->kind->list(card, f->format, &listing);
        if (rc != SIGIL_OK) return rc;
        const sigil_card_entry *found = NULL;
        char other[KEY_MAX];
        for (size_t i = 0; i < listing->entry_count && !found; i++) {
            size_t c = SIZE_MAX;
            int owner = volume_owner(x, f, listing->entries[i].name, other, &c);
            if (owner == OWN_GAME || (owner == OWN_COMPANION && companion_restored(x, c))) found = &listing->entries[i];
        }
        if (found) rc = x->kind->remove(card, found);
        sigil_card_listing_free(listing);
        if (rc != SIGIL_OK || !found) return rc;
    }
}

/* The hash over every save on `card`, as the `seen` record keeps it. */
static int card_identity(const sync_ctx *x, const void *card, int format, int device, char out[33]) {
    save_list all;
    save_list_init(&all, x->kind);
    int rc = add_card_saves(x, card, format, device, 0, WHO_GAME, &all);
    if (rc == SIGIL_OK) rc = identity_where(&all, -1, NULL, -1, SIZE_MAX, out);
    save_list_free(&all);
    return rc;
}

static int place_on_volumes(sync_ctx *x, volume_set *s, const save_list *local, save_list *incoming,
                            const size_t sizes[SIGIL_DEVICE_COUNT], bool already_there, sigil_sync_result *r,
                            char seen[SIGIL_VOLUME_TARGETS_MAX][33]) {
    bool managed = x->req->mode == SIGIL_SYNC_MANAGED;
    int rc = SIGIL_OK;
    for (size_t i = 0; i < incoming->count && rc == SIGIL_OK; i++) {
        incoming->items[i].card = SIZE_MAX;
        for (size_t v = 0; v < s->count; v++) {
            if (s->files[v].target.device == incoming->items[i].device) incoming->items[i].card = v;
        }
        if (incoming->items[i].card == SIZE_MAX) rc = SIGIL_ERR_INVALID_ARG;
    }
    void *next[SIGIL_VOLUME_TARGETS_MAX] = { NULL };
    bool write[SIGIL_VOLUME_TARGETS_MAX] = { false };
    for (size_t v = 0; v < s->count && rc == SIGIL_OK; v++) {
        volume_file *f = &s->files[v];
        bool others = count_where(local, OWN_OTHER, v) + count_where(local, OWN_NONE, v) > 0;
        bool receives = false;
        for (size_t i = 0; i < incoming->count; i++) receives = receives || incoming->items[i].card == v;
        if (managed) write[v] = already_there ? others : (f->card || receives);
        else write[v] = !already_there && (receives || count_where(local, OWN_GAME, v) > 0);
        if (!write[v]) continue;
        int format = 0;
        if (managed || !f->card) {
            int device = f->target.device;
            size_t size = f->target.new_size ? f->target.new_size
                          : sizes[device]    ? sizes[device]
                                             : x->kind->unit_size(device, NULL);
            rc = x->kind->blank(&next[v], &format, device, size, f->target.form, f->card);
        } else {
            uint8_t *image = NULL;
            size_t len = 0;
            rc = x->kind->image(f->card, &image, &len);
            if (rc == SIGIL_OK) rc = load_bytes(x->kind, image, len, f->target.device, &next[v], &format);
            free(image);
            if (rc == SIGIL_OK) rc = remove_game_saves(x, f, next[v]);
        }
        if (!f->card) f->format = format;
        for (size_t i = 0; i < incoming->count && rc == SIGIL_OK; i++) {
            if (incoming->items[i].card != v) continue;
            rc = x->kind->inject(next[v], incoming->items[i].save);
            if (rc == SIGIL_ERR_NO_SPACE) note_overflow(x, next[v], format, f->target.device, &incoming->items[i], r);
        }
        for (size_t i = 0; managed && i < local->count && rc == SIGIL_OK; i++) {
            const owned_save *o = &local->items[i];
            if (o->card != v || o->owner != OWN_COMPANION || companion_restored(x, o->companion)) continue;
            rc = x->kind->inject(next[v], o->save);
            if (rc == SIGIL_ERR_NO_SPACE) note_overflow(x, next[v], format, f->target.device, o, r);
        }
    }
    for (size_t v = 0; v < s->count && rc == SIGIL_OK; v++) {
        if (write[v]) rc = write_and_verify(x, s->files[v].target.path, s->files[v].target.device, next[v], incoming, v);
    }
    for (size_t v = 0; v < s->count && rc == SIGIL_OK; v++) {
        const void *final = write[v] ? next[v] : s->files[v].card;
        if (final) rc = card_identity(x, final, s->files[v].format, s->files[v].target.device, seen[v]);
        else sigil_named_hash(NULL, 0, seen[v]);
    }
    for (size_t v = 0; v < SIGIL_VOLUME_TARGETS_MAX; v++) {
        if (next[v]) x->kind->free_card(next[v]);
    }
    return rc;
}

/* After a restore, the state learns the owners of the saves it placed and
 * what each shared volume now holds. */
static int note_restored(sync_ctx *x, const volume_set *s, const save_list *incoming,
                         char seen[SIGIL_VOLUME_TARGETS_MAX][33]) {
    bool managed = x->req->mode == SIGIL_SYNC_MANAGED;
    int rc = learn_owners(x, s, incoming);
    for (size_t v = 0; v < s->count && rc == SIGIL_OK; v++) {
        if (s->files[v].target.per_game) continue;
        if (managed) rc = put2(&x->state, "prepared", s->files[v].key, x->game);
        if (rc == SIGIL_OK) rc = put2(&x->state, "seen", s->files[v].key, seen[v]);
        if (rc == SIGIL_OK && managed) rc = put2(&x->state, "held", s->files[v].key, NULL);
    }
    return rc;
}

/* ---- save folders: Dolphin's GCI folder ----------------------------------------- */

/* The save folder the layout row gives the game; "" when it keeps a card. */
static int save_folder_of(const sync_ctx *x, char folder[SIGIL_SAVE_PATH_MAX]) {
    folder[0] = '\0';
    sigil_volume_target targets[SIGIL_VOLUME_TARGETS_MAX];
    size_t count = 0;
    int rc = sigil_save_volume_targets(&x->req->save, targets, &count);
    for (size_t i = 0; i < count && rc == SIGIL_OK; i++) {
        if (targets[i].device == SIGIL_DEVICE_GC_FOLDER) snprintf(folder, SIGIL_SAVE_PATH_MAX, "%s", targets[i].path);
    }
    return rc;
}

static int compare_paths(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Files directly in `folder` with the .gci extension Dolphin loads, sorted. */
static size_t folder_files(const sigil_sync_request *req, const char *folder, const char ***out) {
    size_t prefix = strlen(folder), n = 0;
    *out = (const char **)calloc(req->save.listing_count + 1, sizeof(char *));
    if (!*out) return 0;
    for (size_t i = 0; i < req->save.listing_count; i++) {
        const char *p = req->save.listing[i];
        if (!p || strncmp(p, folder, prefix) != 0 || strchr(p + prefix, '/')) continue;
        size_t len = strlen(p);
        if (len <= prefix + 4 || strcmp(p + len - 4, ".gci") != 0) continue;
        (*out)[n++] = p;
    }
    qsort(*out, n, sizeof(char *), compare_paths);
    return n;
}

/* The game's saves in `folder`, one per identity. A second file with an
 * identity already seen, which Dolphin refuses at load, goes in `stale`. */
static int folder_saves(const sync_ctx *x, const char *folder, save_list *out, path_list *stale) {
    save_list_init(out, x->kind);
    const char **files = NULL;
    size_t n = folder_files(x->req, folder, &files);
    stale->paths = calloc(n + 1, SIGIL_SAVE_PATH_MAX);
    stale->count = 0;
    int rc = files && stale->paths ? SIGIL_OK : SIGIL_ERR_OOM;
    for (size_t i = 0; i < n && rc == SIGIL_OK; i++) {
        sigil_io *io = x->req->save.open(x->req->save.open_ctx, files[i]);
        if (!io) continue;
        uint8_t *data = NULL;
        size_t len = 0;
        int read = sigil_bram_read_all(io, GC_MAX_CARD_SIZE + GC_DENTRY_SIZE, &data, &len);
        sigil_io_close(io);
        void *save = NULL;
        if (read == SIGIL_OK && x->kind->file_to_save(data, len, &save) == SIGIL_OK) {
            const blob_save *b = (const blob_save *)save;
            char owner[SIGIL_CARD_OWNER_MAX];
            snprintf(owner, sizeof(owner), "%02X%02X%02X%02X", b->data[0], b->data[1], b->data[2], b->data[3]);
            char key[SIGIL_CARD_NAME_MAX];
            x->kind->save_key(save, key);
            bool game = owned_by_game(x->req, owner);
            size_t companion = game ? SIZE_MAX : companion_of(x->req, owner);
            if (!game && companion == SIZE_MAX) {
                x->kind->free_save(save);
            } else if (save_list_has(out, key, SIGIL_DEVICE_NONE)) {
                snprintf(stale->paths[stale->count++], SIGIL_SAVE_PATH_MAX, "%s", files[i]);
                x->kind->free_save(save);
            } else {
                owned_save *o = save_list_push(out);
                if (!o) { x->kind->free_save(save); rc = SIGIL_ERR_OOM; }
                else {
                    snprintf(o->name, sizeof(o->name), "%s", key);
                    snprintf(o->path, sizeof(o->path), "%s", files[i]);
                    o->save = save;
                    o->card = SIZE_MAX;
                    o->owner = OWN_GAME;
                    o->companion = SIZE_MAX;
                    if (companion != SIZE_MAX) set_companion(x, o, companion);
                    out->count++;
                }
            }
        }
        free(data);
    }
    free(files);
    if (rc != SIGIL_OK) {
        save_list_free(out);
        free(stale->paths);
        stale->paths = NULL;
    }
    return rc;
}

static bool listed(const sigil_sync_request *req, const char *path) {
    for (size_t i = 0; i < req->save.listing_count; i++) {
        if (req->save.listing[i] && strcmp(req->save.listing[i], path) == 0) return true;
    }
    return false;
}

/* Where an incoming save goes: over the game's file holding its identity,
 * else Dolphin's name for it, with a digit before .gci when another game's
 * file already has that name, as Dolphin numbers one. */
static void folder_target(const sync_ctx *x, const char *folder, const save_list *local, const owned_save *o,
                          char out[SIGIL_SAVE_PATH_MAX]) {
    for (size_t i = 0; i < local->count; i++) {
        if (strcmp(local->items[i].name, o->name) == 0) {
            snprintf(out, SIGIL_SAVE_PATH_MAX, "%s", local->items[i].path);
            return;
        }
    }
    char name[SIGIL_SAVE_PATH_MAX];
    x->kind->file_name(o->save, name, sizeof(name));
    snprintf(out, SIGIL_SAVE_PATH_MAX, "%s%s", folder, name);
    size_t base = strlen(out) - 4;
    for (int digit = 0; digit < 10 && listed(x->req, out); digit++) {
        snprintf(out + base, SIGIL_SAVE_PATH_MAX - base, "%d.gci", digit);
    }
}

/* The restore replaces this local file: it is the game's, or a companion's
 * the request carries a unit for, and the incoming saves lack it. */
static bool folder_file_goes(const sync_ctx *x, const owned_save *o, const save_list *incoming) {
    bool replaced = o->owner == OWN_GAME || (o->owner == OWN_COMPANION && companion_restored(x, o->companion));
    return replaced && !save_list_has(incoming, o->name, SIGIL_DEVICE_NONE);
}

static int place_in_folder(const sync_ctx *x, const char *folder, const save_list *local, const path_list *stale,
                           const save_list *incoming) {
    const sigil_sync_request *req = x->req;
    size_t removes = stale->count;
    for (size_t i = 0; i < local->count; i++) removes += folder_file_goes(x, &local->items[i], incoming);
    if (removes && !req->remove) return SIGIL_ERR_INVALID_ARG;

    int rc = SIGIL_OK;
    for (size_t i = 0; i < incoming->count && rc == SIGIL_OK; i++) {
        const blob_save *b = (const blob_save *)incoming->items[i].save;
        char path[SIGIL_SAVE_PATH_MAX];
        folder_target(x, folder, local, &incoming->items[i], path);
        if (req->write(req->write_ctx, path, b->data, b->len) != 0) { rc = SIGIL_ERR_IO; break; }
        sigil_io *io = req->save.open(req->save.open_ctx, path);
        uint8_t *back = NULL;
        size_t len = 0;
        rc = io ? sigil_bram_read_all(io, b->len + 1, &back, &len) : SIGIL_ERR_IO;
        if (io) sigil_io_close(io);
        if (rc == SIGIL_OK && (len != b->len || memcmp(back, b->data, len) != 0)) rc = SIGIL_ERR_IO;
        free(back);
    }
    for (size_t i = 0; i < local->count && rc == SIGIL_OK; i++) {
        if (!folder_file_goes(x, &local->items[i], incoming)) continue;
        if (req->remove(req->write_ctx, local->items[i].path) != 0) rc = SIGIL_ERR_IO;
    }
    for (size_t i = 0; i < stale->count && rc == SIGIL_OK; i++) {
        if (req->remove(req->write_ctx, stale->paths[i]) != 0) rc = SIGIL_ERR_IO;
    }
    return rc;
}

/* ---- collect ------------------------------------------------------------------- */

/* A view of `saves` holding companion `c`'s alone, relabelled as the game's
 * so the unit builders take them. It shares the saves; free only `items`. */
static int companion_view(const save_list *saves, size_t c, save_list *view) {
    save_list_init(view, saves->kind);
    view->items = (owned_save *)calloc(saves->count ? saves->count : 1, sizeof(owned_save));
    if (!view->items) return SIGIL_ERR_OOM;
    for (size_t i = 0; i < saves->count; i++) {
        const owned_save *o = &saves->items[i];
        if (o->owner != OWN_COMPANION || o->companion != c) continue;
        view->items[view->count] = *o;
        view->items[view->count].owner = OWN_GAME;
        view->count++;
    }
    view->cap = saves->count;
    return SIGIL_OK;
}

/* Each companion's saves found with the game's, as the companion's unit, and
 * the state's record of what each companion last synced. */
static int collect_companions(sync_ctx *x, const save_list *saves, const unit_sources *src, sigil_sync_result *r) {
    size_t n = x->req->companion_count;
    if (!n) return SIGIL_OK;
    r->companions = (sigil_sync_companion_result *)calloc(n, sizeof(*r->companions));
    if (!r->companions) return SIGIL_ERR_OOM;
    r->companion_count = n;
    int rc = SIGIL_OK;
    for (size_t c = 0; c < n && rc == SIGIL_OK; c++) {
        sigil_sync_companion_result *out = &r->companions[c];
        save_list view;
        rc = companion_view(saves, c, &view);
        char artifact[SIGIL_SAVE_ENTRY_MAX];
        int shape = 0;
        if (rc == SIGIL_OK) rc = build_unit(&view, OWN_GAME, src, false, "companion", &out->data, &out->len, &shape, artifact,
                                            out->content_hash);
        if (rc == SIGIL_OK && out->data) rc = identity_of(&view, out->identity_hash);
        free(view.items);
        const char *last = get2(&x->state, "synced", x->companion_keys[c]);
        out->changed = strcmp(last ? last : "", out->identity_hash) != 0;
        if (rc == SIGIL_OK && out->identity_hash[0]) rc = put2(&x->state, "synced", x->companion_keys[c], out->identity_hash);
    }
    return rc;
}

/* The unit and its hashes from the game's saves, and the companions' units.
 * A card unit keeps the artifact name the card file gave it; other units
 * are named for their volume or file, or the content. */
static int finish_collect(sync_ctx *x, const save_list *saves, const unit_sources *src, sigil_sync_result *r) {
    char stem[SIGIL_SAVE_ENTRY_MAX], unused[SIGIL_SAVE_ENTRY_MAX];
    sigil_content_stem(x->req->save.content_path, stem, sizeof(stem));
    char *artifact = x->kind->has_ids && !x->kind->save_files ? unused : r->artifact;
    int rc = build_unit(saves, OWN_GAME, src, false, stem, &r->data, &r->len, &r->shape, artifact, r->content_hash);
    if (rc == SIGIL_OK && r->data) rc = identity_of(saves, r->identity_hash);
    if (rc == SIGIL_OK) rc = collect_companions(x, saves, src, r);
    return rc;
}

static int collect_cards(sync_ctx *x, sigil_sync_result *r) {
    card_set cards;
    int rc = gather_cards(x, &cards, r->artifact);
    if (rc != SIGIL_OK) return rc;
    save_list saves;
    rc = gather_saves(x, &cards, &saves);
    card_set_free(&cards);
    if (rc != SIGIL_OK) return rc;
    rc = finish_collect(x, &saves, NULL, r);
    save_list_free(&saves);
    return rc;
}

/* Sets `changed` and the new state. After an unmanaged restore, a collect
 * that finds the saves the restore replaced asks for the restore again. */
static int finish(sync_ctx *x, sigil_sync_result *r, bool collecting) {
    const char *last = get2(&x->state, "synced", x->game);
    r->changed = strcmp(last ? last : "", r->identity_hash) != 0;
    int rc = SIGIL_OK;
    const char *restored = collecting ? get2(&x->state, "restored", x->game) : NULL;
    if (restored) {
        const char *tab = strchr(restored, '\t');
        size_t new_len = tab ? (size_t)(tab - restored) : strlen(restored);
        const char *old = tab ? tab + 1 : "";
        bool survived = new_len == strlen(r->identity_hash) && strncmp(restored, r->identity_hash, new_len) == 0;
        if (!survived && strcmp(old, r->identity_hash) == 0) {
            r->restore_again = 1;
            r->changed = 0;
        } else {
            rc = put2(&x->state, "restored", x->game, NULL);
        }
    }
    if (rc == SIGIL_OK && !r->restore_again) {
        rc = put2(&x->state, "synced", x->game, r->identity_hash[0] ? r->identity_hash : NULL);
    }
    if (rc == SIGIL_OK) rc = state_blob(&x->state, &r->state, &r->state_len);
    return rc;
}

static int collect_folder(sync_ctx *x, const char *folder, sigil_sync_result *r) {
    save_list saves;
    path_list stale;
    int rc = folder_saves(x, folder, &saves, &stale);
    if (rc != SIGIL_OK) return rc;
    rc = finish_collect(x, &saves, NULL, r);
    save_list_free(&saves);
    free(stale.paths);
    return rc;
}

static int collect_any(sync_ctx *x, sigil_sync_result *r) {
    if (!x->kind->has_ids) return collect_volumes(x, r);
    if (x->kind->save_files) {
        char folder[SIGIL_SAVE_PATH_MAX];
        int rc = save_folder_of(x, folder);
        if (rc != SIGIL_OK) return rc;
        if (folder[0]) return collect_folder(x, folder, r);
    }
    return collect_cards(x, r);
}

int sigil_collect(const sigil_sync_request *req, sigil_sync_result **out) {
    if (!out) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    if (!request_valid(req)) return SIGIL_ERR_INVALID_ARG;
    const card_kind *kind = kind_for(req);
    if (!kind) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    sync_ctx x;
    int rc = ctx_open(&x, req, kind);
    sigil_sync_result *r = rc == SIGIL_OK ? new_result() : NULL;
    if (rc == SIGIL_OK && !r) rc = SIGIL_ERR_OOM;
    if (rc == SIGIL_OK) rc = collect_any(&x, r);
    if (rc == SIGIL_OK) rc = finish(&x, r, true);
    ctx_close(&x);
    if (rc != SIGIL_OK) { sigil_sync_result_free(r); return rc; }
    *out = r;
    return SIGIL_OK;
}

/* ---- restore ------------------------------------------------------------------- */

/* The conflict rule: saves on disk that changed since the last sync stop a
 * restore unless the user chose to overwrite them. */
static int check_conflict(const sync_ctx *x, const char *local_identity, const char *incoming, sigil_sync_result *r,
                          bool *already_there) {
    const char *last = get2(&x->state, "synced", x->game);
    *already_there = strcmp(local_identity, incoming) == 0;
    bool local_changed = local_identity[0] && strcmp(local_identity, last ? last : "") != 0;
    if (!*already_there && local_changed && !x->req->overwrite_local) {
        r->conflict = 1;
        return SIGIL_ERR_CONFLICT;
    }
    return SIGIL_OK;
}

/* The identity of the saves `owner` (and with `key`, that companion) has in
 * `l`, or "" when it has none there. */
static int identity_or_empty(const save_list *l, int owner, const char *key, char out[33]) {
    out[0] = '\0';
    bool any = false;
    for (size_t i = 0; i < l->count && !any; i++) {
        any = l->items[i].owner == owner && (!key || strcmp(l->items[i].other, key) == 0);
    }
    return any ? identity_where(l, owner, key, -1, SIZE_MAX, out) : SIGIL_OK;
}

/* The conflict rule for the game and for each companion the restore carries
 * a unit for. `already_there` is true when all of them already hold what
 * the restore brings. */
static int check_restore(const sync_ctx *x, const save_list *local, const save_list *incoming, sigil_sync_result *r,
                         char local_identity[33], bool *already_there) {
    int rc = identity_or_empty(local, OWN_GAME, NULL, local_identity);
    if (rc == SIGIL_OK) rc = check_conflict(x, local_identity, r->identity_hash, r, already_there);
    for (size_t c = 0; c < x->req->companion_count && rc == SIGIL_OK; c++) {
        if (!companion_restored(x, c)) continue;
        const char *key = x->companion_keys[c];
        char mine[33], theirs[33];
        rc = identity_or_empty(local, OWN_COMPANION, key, mine);
        if (rc == SIGIL_OK) rc = identity_or_empty(incoming, OWN_COMPANION, key, theirs);
        if (rc != SIGIL_OK) break;
        const char *last = get2(&x->state, "synced", key);
        bool same = strcmp(mine, theirs) == 0;
        bool changed = mine[0] && strcmp(mine, last ? last : "") != 0;
        if (!same && changed && !x->req->overwrite_local) {
            r->conflict = 1;
            rc = SIGIL_ERR_CONFLICT;
        }
        *already_there = *already_there && same;
    }
    return rc;
}

/* The state records what each companion now holds, as the restore put it. */
static int note_companions(sync_ctx *x, const save_list *incoming) {
    int rc = SIGIL_OK;
    for (size_t c = 0; c < x->req->companion_count && rc == SIGIL_OK; c++) {
        if (!companion_restored(x, c)) continue;
        char theirs[33];
        rc = identity_or_empty(incoming, OWN_COMPANION, x->companion_keys[c], theirs);
        if (rc == SIGIL_OK) rc = put2(&x->state, "synced", x->companion_keys[c], theirs[0] ? theirs : NULL);
    }
    return rc;
}

static int restore_cards(sync_ctx *x, save_list *incoming, sigil_sync_result *r, char local_identity[33]) {
    card_set cards;
    int rc = gather_cards(x, &cards, r->artifact);
    if (rc != SIGIL_OK) return rc;
    save_list local;
    rc = gather_saves(x, &cards, &local);
    bool already_there = false;
    if (rc == SIGIL_OK) rc = check_restore(x, &local, incoming, r, local_identity, &already_there);
    save_list_free(&local);
    if (rc == SIGIL_OK && !already_there) rc = place_on_cards(x, &cards, incoming, r);
    card_set_free(&cards);
    return rc;
}

static int restore_folder(sync_ctx *x, const char *folder, save_list *incoming, sigil_sync_result *r,
                          char local_identity[33]) {
    save_list local;
    path_list stale;
    int rc = folder_saves(x, folder, &local, &stale);
    if (rc != SIGIL_OK) return rc;
    bool already_there = false;
    rc = check_restore(x, &local, incoming, r, local_identity, &already_there);
    if (rc == SIGIL_OK && !already_there) rc = place_in_folder(x, folder, &local, &stale, incoming);
    save_list_free(&local);
    free(stale.paths);
    return rc;
}

static int restore_with_ids(sync_ctx *x, save_list *incoming, sigil_sync_result *r, char local_identity[33]) {
    if (x->kind->save_files) {
        char folder[SIGIL_SAVE_PATH_MAX];
        int rc = save_folder_of(x, folder);
        if (rc != SIGIL_OK) return rc;
        if (folder[0]) return restore_folder(x, folder, incoming, r, local_identity);
    }
    return restore_cards(x, incoming, r, local_identity);
}

static int restore_volumes(sync_ctx *x, save_list *incoming, const size_t sizes[SIGIL_DEVICE_COUNT], sigil_sync_result *r,
                           char local_identity[33]) {
    volume_set vols;
    int rc = gather_volumes(x, &vols);
    if (rc != SIGIL_OK) return rc;
    save_list local;
    rc = gather_volume_saves(x, &vols, &local);
    if (rc != SIGIL_OK) { volume_set_free(&vols); return rc; }

    bool already_there = false;
    char seen[SIGIL_VOLUME_TARGETS_MAX][33];
    rc = check_restore(x, &local, incoming, r, local_identity, &already_there);
    if (rc == SIGIL_OK) rc = x->req->mode == SIGIL_SYNC_MANAGED ? check_swappable(x, &vols, &local)
                                                                 : check_unchanged(x, &vols, &local);
    if (rc == SIGIL_OK) rc = place_on_volumes(x, &vols, &local, incoming, sizes, already_there, r, seen);
    if (rc == SIGIL_OK) rc = note_restored(x, &vols, incoming, seen);
    save_list_free(&local);
    volume_set_free(&vols);
    return rc;
}

int sigil_restore(const sigil_sync_request *req, const uint8_t *unit, size_t unit_len, sigil_sync_result **out) {
    if (!out) return SIGIL_ERR_INVALID_ARG;
    *out = NULL;
    if (!request_valid(req) || !req->write || !unit) return SIGIL_ERR_INVALID_ARG;
    const card_kind *kind = kind_for(req);
    if (!kind) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    sync_ctx x;
    int rc = ctx_open(&x, req, kind);
    sigil_sync_result *r = rc == SIGIL_OK ? new_result() : NULL;
    if (rc == SIGIL_OK && !r) rc = SIGIL_ERR_OOM;

    save_list incoming;
    save_list_init(&incoming, kind);
    size_t sizes[SIGIL_DEVICE_COUNT];
    char local_identity[33] = "";
    if (rc == SIGIL_OK) rc = request_saves(&x, unit, unit_len, &incoming, sizes);
    if (rc == SIGIL_OK) rc = identity_of(&incoming, r->identity_hash);
    if (rc == SIGIL_OK) {
        sigil_md5_of(unit, unit_len, r->content_hash);
        bool zip = (!kind->has_ids || kind->save_files) && sigil_read_le32(unit) == 0x04034b50u;
        r->shape = zip ? SIGIL_SAVE_SHAPE_MULTI : SIGIL_SAVE_SHAPE_SINGLE;
        rc = kind->has_ids ? restore_with_ids(&x, &incoming, r, local_identity)
                           : restore_volumes(&x, &incoming, sizes, r, local_identity);
        if (!kind->has_ids || kind->save_files) {
            char stem[SIGIL_SAVE_ENTRY_MAX];
            sigil_content_stem(req->save.content_path, stem, sizeof(stem));
            if (zip) snprintf(r->artifact, sizeof(r->artifact), "%s.zip", stem);
            else if (kind->save_files) kind->file_name(incoming.items[0].save, r->artifact, sizeof(r->artifact));
            else snprintf(r->artifact, sizeof(r->artifact), "%s", device_name(incoming.items[0].device));
        }
    }
    if (rc == SIGIL_OK) rc = note_companions(&x, &incoming);
    save_list_free(&incoming);
    if (rc == SIGIL_OK && req->mode == SIGIL_SYNC_UNMANAGED) {
        char both[80];
        snprintf(both, sizeof(both), "%s\t%s", r->identity_hash, local_identity);
        rc = put2(&x.state, "restored", x.game, both);
    }
    if (rc == SIGIL_OK) rc = finish(&x, r, false);
    if (rc == SIGIL_OK) r->changed = 0;
    ctx_close(&x);
    if (rc == SIGIL_ERR_CONFLICT || rc == SIGIL_ERR_NO_SPACE) {
        *out = r;
        return rc;
    }
    if (rc != SIGIL_OK) { sigil_sync_result_free(r); return rc; }
    *out = r;
    return SIGIL_OK;
}

void sigil_sync_result_free(sigil_sync_result *result) {
    if (!result) return;
    free(result->data);
    free(result->state);
    free(result->holding);
    free(result->unowned);
    for (size_t c = 0; result->companions && c < result->companion_count; c++) free(result->companions[c].data);
    free(result->companions);
    free(result);
}
