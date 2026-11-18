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
 * index of the save's size here, and the address size is how many bytes the
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

/* The DeSmuME type of a save of `len` bytes (mc.cpp searchFileSaveType), 0 when it has none. */
static uint32_t dsv_type(size_t len) {
    for (size_t i = 1; i < DSV_TYPE_COUNT; i++) {
        if (DSV_TYPES[i].size == len) return (uint32_t)i;
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

/* The bytes restore writes for `layout`: the raw save, and for DeSmuME what
 * DeSmuME writes when it takes in a raw save (mc.cpp, BackupDevice's
 * constructor): the save and its footer for a size it has a type for
 * (ensure), the save as it is for any other size. */
static int file_bytes(const char *layout, const uint8_t *raw, size_t len, uint8_t **out, size_t *out_len) {
    uint32_t type = footer_layout(layout) ? dsv_type(len) : 0;
    *out_len = len + (type ? DSV_FOOTER : 0);
    *out = (uint8_t *)malloc(*out_len ? *out_len : 1);
    if (!*out) return SIGIL_ERR_OOM;
    memcpy(*out, raw, len);
    if (!type) return SIGIL_OK;
    uint8_t *note = *out + len, *fields = note + DSV_NOTE_LEN;
    memcpy(note, DSV_NOTE, DSV_NOTE_LEN);
    sigil_write_le32(fields, (uint32_t)len);              /* bytes the save holds */
    sigil_write_le32(fields + 4, (uint32_t)len);          /* padSize: the save's length in the file */
    sigil_write_le32(fields + 8, type);
    sigil_write_le32(fields + 12, DSV_TYPES[type].addr_size);
    sigil_write_le32(fields + 16, (uint32_t)len);         /* the chip's size */
    sigil_write_le32(fields + 20, 0);                     /* version */
    memcpy(fields + DSV_FIELDS, DSV_COOKIE, DSV_COOKIE_LEN);
    return SIGIL_OK;
}

/* The game's save file the layout names, there or expected. */
typedef struct {
    char path[SIGIL_SAVE_PATH_MAX];
    bool present;
} nds_file;

static int file_of(const sigil_sync_request *req, nds_file *f) {
    memset(f, 0, sizeof(*f));
    sigil_save_unit *unit = NULL;
    int rc = sigil_save_resolve(&req->save, &unit);
    if (rc != SIGIL_OK) return rc;
    for (size_t i = 0; i < unit->member_count + unit->expected_count && !f->path[0]; i++) {
        bool present = i < unit->member_count;
        const sigil_save_member *m = present ? &unit->members[i] : &unit->expected[i - unit->member_count];
        if (m->role == SIGIL_SAVE_ROLE_RTC) continue;
        snprintf(f->path, sizeof(f->path), "%s", m->path);
        f->present = present;
    }
    sigil_save_unit_free(unit);
    return SIGIL_OK;
}

/* The raw save in the game's file, NULL when there's none. An empty file holds no save. */
static int read_local(const sigil_sync_request *req, const nds_file *f, uint8_t **raw, size_t *len) {
    *raw = NULL;
    *len = 0;
    if (!f->present) return SIGIL_OK;
    int rc = sigil_sync_read_file(req, f->path, NDS_MAX_SAVE + DSV_FOOTER, raw, len);
    if (rc == SIGIL_OK) *len = raw_len(*raw, *len);
    if (rc == SIGIL_OK && !*len) {
        free(*raw);
        *raw = NULL;
    }
    return rc == SIGIL_ERR_NOT_FOUND ? SIGIL_ERR_IO : rc;
}

static void artifact_of(const sigil_sync_request *req, sigil_sync_result *r) {
    char stem[SIGIL_SAVE_ENTRY_MAX];
    sigil_content_stem(req->save.content_path, stem, sizeof(stem));
    snprintf(r->artifact, sizeof(r->artifact), "%s.srm", stem);
}

int sigil_sync_collect_nds(sigil_sync_ctx *x, sigil_sync_result *r) {
    nds_file f;
    uint8_t *raw = NULL;
    size_t len = 0;
    int rc = file_of(x->req, &f);
    if (rc == SIGIL_OK) rc = read_local(x->req, &f, &raw, &len);
    if (rc == SIGIL_OK && raw) {
        sigil_md5_of(raw, len, r->identity_hash);
        memcpy(r->content_hash, r->identity_hash, sizeof(r->content_hash));
        r->data = raw;
        r->len = len;
        r->shape = SIGIL_SAVE_SHAPE_SINGLE;
        artifact_of(x->req, r);
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
    artifact_of(x->req, r);

    nds_file f;
    uint8_t *local = NULL;
    size_t local_len = 0;
    int rc = file_of(x->req, &f);
    if (rc == SIGIL_OK) rc = read_local(x->req, &f, &local, &local_len);
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
