// SPDX-License-Identifier: MPL-2.0
/* The GameCube kind. A save is a .gci: the 64-byte directory entry, then its
 * blocks. Units are the game's .gci files, named as Dolphin names them. */
#include "sync_internal.h"

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
    (void)like;
    *format = SIGIL_CARD_FORMAT_GAMECUBE_RAW;
    return gc_blank_sized(card, GC_MAX_CARD_SIZE, form == SIGIL_FORM_SHIFT_JIS);
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
    *save = sigil_sync_blob_new(gci, len);
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
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    int rc = sigil_gamecube_inject(c->image, c->size, s->data, s->len);
    if (rc != SIGIL_OK) return rc;
    uint32_t first = gc_first_block_of(c, s->data);
    return first == 0xFFFF ? SIGIL_ERR_IO : sigil_gamecube_bind_serial(c->image, c->size, first);
}

static uint32_t gc_cost(const void *card, const void *save) {
    (void)card;
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    return sigil_gamecube_cost(s->data, s->len);
}

static int gc_remove(void *card, const sigil_card_entry *entry) {
    gc_card *c = (gc_card *)card;
    return sigil_gamecube_delete(c->image, c->size, entry->first_block);
}

static int gc_image(const void *card, uint8_t **out, size_t *len) {
    const gc_card *c = (const gc_card *)card;
    return sigil_sync_copy_image(c->image, c->size, out, len);
}

/* The save as it reads off a fresh card after injecting and binding, its
 * time, copy count and first block left out: a save bound to one card's
 * serial and the same save bound to another's hash alike. */
static int gc_identity(const void *save, char out[33]) {
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
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
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    uint32_t first = gc_first_block_of(c, s->data);
    if (first == 0xFFFF) return SIGIL_ERR_NOT_FOUND;
    sigil_sync_blob placed = { (uint8_t *)malloc(s->len), s->len };
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
    const sigil_sync_blob *s = (const sigil_sync_blob *)save;
    char name[GC_FILENAME_LEN + 1];
    memcpy(name, s->data + GCI_FILENAME, GC_FILENAME_LEN);
    name[GC_FILENAME_LEN] = '\0';
    snprintf(out, SIGIL_CARD_NAME_MAX, "%02X%02X%02X%02X-%02X%02X-%s", s->data[0], s->data[1], s->data[2], s->data[3],
             s->data[GCI_MAKER], s->data[GCI_MAKER + 1], name);
}

static const char *gc_game_region(const sigil_sync_request *req) {
    return sigil_gc_region_folder(sigil_gc_region_letter(req->save.result));
}

static int gc_new_form(const sigil_sync_request *req, const char *path) {
    (void)path;
    const char *region = gc_game_region(req);
    return region && strcmp(region, "JAP") == 0 ? SIGIL_FORM_SHIFT_JIS : SIGIL_FORM_RAW;
}

/* A companion's save from another region than the game's: the game can't
 * read it, and Dolphin keeps it in another region's folder or card. */
static bool gc_foreign(const sigil_sync_request *req, const void *save) {
    const char *game = gc_game_region(req);
    const char *theirs = sigil_gc_region_folder((char)((const sigil_sync_blob *)save)->data[3]);
    return game && theirs && strcmp(game, theirs) != 0;
}

/* CP1252's 0x80-0x9F; 0 where the code page has no character. */
static const uint16_t CP1252_HIGH[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

/* `c` (0x80 and up) of a CP1252 name as UTF-8 at `out`; 0 when it has no
 * character. */
static size_t cp1252_utf8(unsigned char c, char *out) {
    uint16_t u = c < 0xA0 ? CP1252_HIGH[c - 0x80] : c;
    if (!u) return 0;
    if (u < 0x800) {
        out[0] = (char)(0xC0 | (u >> 6));
        out[1] = (char)(0x80 | (u & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (u >> 12));
    out[1] = (char)(0x80 | ((u >> 6) & 0x3F));
    out[2] = (char)(0x80 | (u & 0x3F));
    return 3;
}

/* The name Dolphin gives a .gci (GCMemcardUtils GenerateFilename and
 * NandPaths EscapeFileName): maker-gamecode-filename, decoded from CP1252,
 * with each "__" doubled into escapes and characters a file name can't hold
 * as __xx__. Dolphin decodes a Japanese save's name from Shift-JIS; sigil
 * escapes its bytes above 0x7F instead, which keeps the name UTF-8, and
 * Dolphin loads every .gci in the folder whatever its name. */
static void gc_file_name(const void *save, char *out, size_t cap) {
    const uint8_t *gci = ((const sigil_sync_blob *)save)->data;
    const char *region = sigil_gc_region_folder((char)gci[GCI_GAMECODE + 3]);
    bool shift_jis = region && strcmp(region, "JAP") == 0;
    char raw[GC_FILENAME_LEN + 16];
    size_t n = 0;
    raw[n++] = (char)gci[GCI_MAKER];
    raw[n++] = (char)gci[GCI_MAKER + 1];
    raw[n++] = '-';
    for (size_t i = 0; i < 4; i++) raw[n++] = (char)gci[GCI_GAMECODE + i];
    raw[n++] = '-';
    for (size_t i = 0; i < GC_FILENAME_LEN && gci[GCI_FILENAME + i]; i++) raw[n++] = (char)gci[GCI_FILENAME + i];

    size_t o = 0;
    for (size_t i = 0; i < n && o + 14 < cap; i++) {
        unsigned char c = (unsigned char)raw[i];
        size_t wide = c >= 0x80 && !shift_jis ? cp1252_utf8(c, out + o) : 0;
        if (wide) {
            o += wide;
        } else if (c == '_' && i + 1 < n && raw[i + 1] == '_') {
            o += (size_t)snprintf(out + o, cap - o, "__5f____5f__");
            i++;
        } else if (c <= 0x1F || c >= 0x7F || strchr("\"*/:<>?\\|", c)) {
            o += (size_t)snprintf(out + o, cap - o, "__%02x__", c);
        } else {
            out[o++] = (char)c;
        }
    }
    snprintf(out + o, cap - o, ".gci");
}

/* A .gci, or a GameShark .gcs or MaxDrive .sav of one save, as a .gci. */
static int gc_file_to_save(const uint8_t *data, size_t len, void **save) {
    uint8_t *gci = (uint8_t *)malloc(len ? len : 1);
    size_t gci_len = 0;
    int rc = gci ? sigil_gamecube_to_gci(data, len, gci, &gci_len) : SIGIL_ERR_OOM;
    if (rc != SIGIL_OK) { free(gci); return rc; }
    *save = sigil_sync_blob_new(gci, gci_len);
    return *save ? SIGIL_OK : SIGIL_ERR_OOM;
}

const sigil_sync_kind sigil_sync_gamecube_kind = {
    .platform = "gamecube", .has_ids = true, .main_device = SIGIL_DEVICE_NONE,
    .load = gc_load, .blank = gc_blank, .free_card = gc_free_card, .size = gc_size,
    .unit_size = sigil_sync_no_unit_size, .list = gc_list, .extract = gc_extract,
    .free_save = sigil_sync_blob_free, .inject = gc_inject, .cost = gc_cost, .remove = gc_remove, .verify = gc_verify,
    .image = gc_image, .identity = gc_identity,
    .save_key = gc_save_key, .new_form = gc_new_form, .save_files = true,
    .file_name = gc_file_name, .file_to_save = gc_file_to_save, .foreign = gc_foreign,
};
