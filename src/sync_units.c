// SPDX-License-Identifier: MPL-2.0
/* The request, the saves sync moves, and units. A card platform's unit is
 * one per-game card. A volume platform's unit is the per-game internal
 * volume, or a zip of the game's volumes named by device. A kind whose saves
 * travel as files has the one file, or a zip of them, as its unit. */
#include "sync_internal.h"

/* ---- request ------------------------------------------------------------------ */

static bool in_ids(const char *const *ids, size_t count, const char *owner) {
    for (size_t i = 0; i < count; i++) {
        if (ids[i] && strcmp(ids[i], owner) == 0) return true;
    }
    return false;
}

bool sigil_sync_owned_by_game(const sigil_sync_request *req, const char *owner) {
    if (!owner[0]) return false;
    if (req->save.result && strcmp(req->save.result->title_id, owner) == 0) return true;
    return in_ids(req->game_ids, req->game_id_count, owner);
}

size_t sigil_sync_companion_of(const sigil_sync_request *req, const char *owner) {
    if (!owner[0]) return SIZE_MAX;
    for (size_t c = 0; c < req->companion_count; c++) {
        if (in_ids(req->companions[c].game_ids, req->companions[c].game_id_count, owner)) return c;
    }
    return SIZE_MAX;
}

size_t sigil_sync_companion_by_key(const sigil_sync_ctx *x, const char *key) {
    for (size_t c = 0; c < x->req->companion_count; c++) {
        if (strcmp(x->companion_keys[c], key) == 0) return c;
    }
    return SIZE_MAX;
}

bool sigil_sync_companion_restored(const sigil_sync_ctx *x, size_t c) {
    return c < x->req->companion_count && x->req->companions[c].unit;
}

bool sigil_sync_claimed(const sigil_sync_request *req, const char *name) {
    return in_ids(req->claimed, req->claimed_count, name);
}

bool sigil_sync_listed(const sigil_sync_request *req, const char *path) {
    return in_ids(req->save.listing, req->save.listing_count, path);
}

int sigil_sync_read_file(const sigil_sync_request *req, const char *path, size_t cap, uint8_t **out, size_t *len) {
    *out = NULL;
    *len = 0;
    sigil_io *io = req->save.open(req->save.open_ctx, path);
    if (!io) return SIGIL_ERR_NOT_FOUND;
    int rc = sigil_bram_read_all(io, cap, out, len);
    sigil_io_close(io);
    return rc;
}

bool sigil_sync_file_holds(const sigil_sync_request *req, const char *path, const uint8_t *data, size_t len) {
    uint8_t *have = NULL;
    size_t have_len = 0;
    bool same = sigil_sync_read_file(req, path, len + 1, &have, &have_len) == SIGIL_OK && have_len == len &&
                (len == 0 || memcmp(have, data, len) == 0);
    free(have);
    return same;
}

/* The key the state keeps a game's sync under: the lowest of its ids, so
 * every disc of a set shares one entry, or `fallback` when it has none. */
static void key_for(const sigil_sync_request *req, const sigil_sync_kind *kind, const char *first,
                    const char *const *ids, size_t count, const char *fallback, char out[SYNC_KEY_MAX]) {
    const char *id = first && first[0] ? first : NULL;
    for (size_t i = 0; i < count; i++) {
        if (ids[i] && ids[i][0] && (!id || strcmp(ids[i], id) < 0)) id = ids[i];
    }
    char raw[SYNC_KEY_MAX];
    snprintf(raw, sizeof(raw), "%s/%s/%s", kind->platform, req->save.layout ? req->save.layout : "", id ? id : fallback);
    sigil_sync_escape(raw, out, SYNC_KEY_MAX);
}

void sigil_sync_ctx_close(sigil_sync_ctx *x) {
    sigil_sync_state_free(&x->state);
    free(x->companion_keys);
    x->companion_keys = NULL;
}

int sigil_sync_ctx_open(sigil_sync_ctx *x, const sigil_sync_request *req, const sigil_sync_kind *kind) {
    memset(x, 0, sizeof(*x));
    x->req = req;
    x->kind = kind;
    char stem[SIGIL_SAVE_ENTRY_MAX];
    sigil_content_stem(req->save.content_path, stem, sizeof(stem));
    key_for(req, kind, req->save.result ? req->save.result->title_id : NULL, req->game_ids, req->game_id_count, stem,
            x->game);
    if (req->companion_count) {
        x->companion_keys = calloc(req->companion_count, SYNC_KEY_MAX);
        if (!x->companion_keys) return SIGIL_ERR_OOM;
        for (size_t c = 0; c < req->companion_count; c++) {
            const sigil_sync_companion *k = &req->companions[c];
            if (!k->game_id_count || !k->game_ids) return SIGIL_ERR_INVALID_ARG;
            key_for(req, kind, NULL, k->game_ids, k->game_id_count, "", x->companion_keys[c]);
        }
    }
    return sigil_sync_state_parse(&x->state, req->state, req->state_len);
}

/* ---- saves -------------------------------------------------------------------- */

void sigil_sync_saves_init(sigil_sync_saves *l, const sigil_sync_kind *kind) {
    memset(l, 0, sizeof(*l));
    l->kind = kind;
}

void sigil_sync_saves_free(sigil_sync_saves *l) {
    for (size_t i = 0; i < l->count; i++) l->kind->free_save(l->items[i].save);
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

bool sigil_sync_saves_has(const sigil_sync_saves *l, const char *name, int device) {
    for (size_t i = 0; i < l->count; i++) {
        if (l->items[i].device == device && strcmp(l->items[i].name, name) == 0) return true;
    }
    return false;
}

sigil_sync_save *sigil_sync_saves_push(sigil_sync_saves *l) {
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        sigil_sync_save *grown = (sigil_sync_save *)realloc(l->items, cap * sizeof(*grown));
        if (!grown) return NULL;
        l->items = grown;
        l->cap = cap;
    }
    sigil_sync_save *o = &l->items[l->count];
    memset(o, 0, sizeof(*o));
    return o;
}

void sigil_sync_set_companion(const sigil_sync_ctx *x, sigil_sync_save *o, size_t c) {
    o->owner = SYNC_OWN_COMPANION;
    o->companion = c;
    snprintf(o->other, sizeof(o->other), "%s", x->companion_keys[c]);
}

size_t sigil_sync_count_where(const sigil_sync_saves *l, int owner, size_t card) {
    size_t n = 0;
    for (size_t i = 0; i < l->count; i++) {
        if ((owner < 0 || l->items[i].owner == owner) && (card == SIZE_MAX || l->items[i].card == card)) n++;
    }
    return n;
}

/* The name each device's volume travels under in a unit. */
static const char *const DEVICE_NAMES[SIGIL_DEVICE_COUNT] = {
    "", "backup.ram", "cart.ram",
    "vmu_A1.bin", "vmu_A2.bin", "vmu_B1.bin", "vmu_B2.bin", "vmu_C1.bin", "vmu_C2.bin", "vmu_D1.bin", "vmu_D2.bin",
    "", "",
};

const char *sigil_sync_device_name(int device) {
    return device > SIGIL_DEVICE_NONE && device < SIGIL_DEVICE_COUNT ? DEVICE_NAMES[device] : "";
}

static int device_from_name(const char *name) {
    for (int d = SIGIL_DEVICE_INTERNAL; d < SIGIL_DEVICE_COUNT; d++) {
        if (DEVICE_NAMES[d][0] && strcmp(DEVICE_NAMES[d], name) == 0) return d;
    }
    return SIGIL_DEVICE_NONE;
}

/* Volume saves are named by device, as the same name can sit on the internal
 * volume and the cart. */
int sigil_sync_identity_where(const sigil_sync_saves *l, int owner, const char *other, int device, size_t card,
                              char out[33]) {
    sigil_named_md5 *parts = (sigil_named_md5 *)calloc(l->count ? l->count : 1, sizeof(*parts));
    if (!parts) return SIGIL_ERR_OOM;
    size_t n = 0;
    int rc = SIGIL_OK;
    for (size_t i = 0; i < l->count && rc == SIGIL_OK; i++) {
        const sigil_sync_save *o = &l->items[i];
        if (owner >= 0 && o->owner != owner) continue;
        if (other && strcmp(o->other, other) != 0) continue;
        if (device >= 0 && o->device != device) continue;
        if (card != SIZE_MAX && o->card != card) continue;
        if (o->device == SIGIL_DEVICE_NONE) snprintf(parts[n].name, sizeof(parts[n].name), "%s", o->name);
        else snprintf(parts[n].name, sizeof(parts[n].name), "%s/%s", sigil_sync_device_name(o->device), o->name);
        rc = l->kind->identity(o->save, parts[n].md5);
        n++;
    }
    if (rc == SIGIL_OK) sigil_named_hash(parts, n, out);
    free(parts);
    return rc;
}

int sigil_sync_identity_of(const sigil_sync_saves *l, char out[33]) {
    return sigil_sync_identity_where(l, SYNC_OWN_GAME, NULL, -1, SIZE_MAX, out);
}

int sigil_sync_identity_or_empty(const sigil_sync_saves *l, int owner, const char *key, char out[33]) {
    out[0] = '\0';
    bool any = false;
    for (size_t i = 0; i < l->count && !any; i++) {
        any = l->items[i].owner == owner && (!key || strcmp(l->items[i].other, key) == 0);
    }
    return any ? sigil_sync_identity_where(l, owner, key, -1, SIZE_MAX, out) : SIGIL_OK;
}

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

int sigil_sync_load_bytes(const sigil_sync_kind *kind, const uint8_t *data, size_t len, int device, void **card,
                          int *format) {
    mem_view view = { data, len };
    sigil_io io = { mem_read, mem_size, NULL, &view };
    return kind->load(&io, device, card, format);
}

/* On a card platform the ids say whose a save is; on a volume platform every
 * save is taken, as the game's or the companion's (the caller works out a
 * local volume's owners). A unit (index SIZE_MAX) holding a corrupt save is
 * refused. */
int sigil_sync_add_card_saves(const sigil_sync_ctx *x, const void *card, int format, int device, size_t index,
                              size_t who, sigil_sync_saves *out) {
    sigil_card_listing *listing = NULL;
    int rc = x->kind->list(card, format, &listing);
    if (rc != SIGIL_OK) return rc;
    if (listing->corrupt_count && index == SIZE_MAX) rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
    for (size_t i = 0; i < listing->entry_count && rc == SIGIL_OK; i++) {
        const sigil_card_entry *e = &listing->entries[i];
        size_t companion = SIZE_MAX;
        if (x->kind->has_ids) {
            bool game = sigil_sync_owned_by_game(x->req, e->owner_id);
            companion = game ? SIZE_MAX : sigil_sync_companion_of(x->req, e->owner_id);
            bool take = who == SYNC_WHO_GAME    ? game
                        : who == SYNC_WHO_LOCAL ? game || companion != SIZE_MAX
                                                : companion == who;
            if (!take) continue;
        } else if (who != SYNC_WHO_GAME && who != SYNC_WHO_LOCAL) {
            companion = who;
        }
        sigil_sync_save *o = sigil_sync_saves_push(out);
        if (!o) { rc = SIGIL_ERR_OOM; break; }
        if (x->kind->extract(card, e, &o->save) != SIGIL_OK) continue;
        if (x->kind->save_key) x->kind->save_key(o->save, o->name);
        else snprintf(o->name, sizeof(o->name), "%s", e->name);
        if (sigil_sync_saves_has(out, o->name, device)) {
            x->kind->free_save(o->save);
            continue;
        }
        o->device = device;
        o->card = index;
        o->owner = SYNC_OWN_GAME;
        o->companion = SIZE_MAX;
        if (companion != SIZE_MAX) sigil_sync_set_companion(x, o, companion);
        out->count++;
    }
    sigil_card_listing_free(listing);
    return rc;
}

/* ---- building units ------------------------------------------------------------ */

static int build_card_sized(const sigil_sync_saves *saves, int owner, int device, size_t size, uint8_t **out,
                            size_t *len) {
    const sigil_sync_kind *kind = saves->kind;
    void *card = NULL;
    int format = 0;
    int rc = kind->blank(&card, &format, device, size, SIGIL_FORM_RAW, NULL);
    if (rc != SIGIL_OK) return rc;
    for (size_t i = 0; i < saves->count && rc == SIGIL_OK; i++) {
        const sigil_sync_save *o = &saves->items[i];
        if ((owner >= 0 && o->owner != owner) || o->device != device) continue;
        rc = kind->inject(card, o->save);
    }
    if (rc == SIGIL_OK) rc = kind->image(card, out, len);
    kind->free_card(card);
    return rc;
}

/* One card holding `saves` (those with `owner`, or all when negative) on
 * `device`, sized for the unit. `source` is the card they came from: a
 * holding unit takes its size, and so does a game unit whose saves overflow
 * the platform's standard volume, such as a game's many saves on Yaba
 * Sanshiro's 4 MiB volume. */
static int build_card(const sigil_sync_saves *saves, int owner, int device, const void *source, bool holding,
                      uint8_t **out, size_t *len) {
    const sigil_sync_kind *kind = saves->kind;
    size_t standard = kind->unit_size(device, source);
    size_t whole = source ? kind->size(source) : 0;
    if (holding && whole) return build_card_sized(saves, owner, device, whole, out, len);
    int rc = build_card_sized(saves, owner, device, standard, out, len);
    if (rc == SIGIL_ERR_NO_SPACE && whole > standard) rc = build_card_sized(saves, owner, device, whole, out, len);
    return rc;
}

/* A unit of one file: `data` itself, named `name`. Takes `data`. */
static void single_unit(uint8_t *data, size_t len, const char *name, uint8_t **out, size_t *out_len, int *shape,
                        char artifact[SIGIL_SAVE_ENTRY_MAX], char content[33]) {
    *out = data;
    *out_len = len;
    *shape = SIGIL_SAVE_SHAPE_SINGLE;
    snprintf(artifact, SIGIL_SAVE_ENTRY_MAX, "%s", name);
    sigil_md5_of(data, len, content);
}

/* A zip of `members` named `<stem>.zip`, its content hash over each member's
 * name and bytes. */
static int zip_unit(const sigil_zip_member *members, size_t n, const char *stem, uint8_t **out, size_t *len, int *shape,
                    char artifact[SIGIL_SAVE_ENTRY_MAX], char content[33]) {
    int rc = sigil_zip_store(members, n, out, len);
    sigil_named_md5 *parts = rc == SIGIL_OK ? (sigil_named_md5 *)calloc(n ? n : 1, sizeof(*parts)) : NULL;
    if (rc == SIGIL_OK && !parts) rc = SIGIL_ERR_OOM;
    if (rc != SIGIL_OK) return rc;
    for (size_t i = 0; i < n; i++) {
        snprintf(parts[i].name, sizeof(parts[i].name), "%s", members[i].name);
        sigil_md5_of(members[i].data, members[i].len, parts[i].md5);
    }
    sigil_named_hash(parts, n, content);
    free(parts);
    *shape = SIGIL_SAVE_SHAPE_MULTI;
    snprintf(artifact, SIGIL_SAVE_ENTRY_MAX, "%s.zip", stem);
    return SIGIL_OK;
}

/* The unit of a kind whose saves travel as files: the one file, or a zip of
 * them. Such kinds keep each save as a sigil_sync_blob. */
static int build_file_unit(const sigil_sync_saves *saves, int owner, const char *stem, uint8_t **out, size_t *len,
                           int *shape, char artifact[SIGIL_SAVE_ENTRY_MAX], char content[33]) {
    size_t n = sigil_sync_count_where(saves, owner, SIZE_MAX);
    sigil_zip_member *members = (sigil_zip_member *)calloc(n, sizeof(*members));
    if (!members) return SIGIL_ERR_OOM;
    size_t m = 0;
    for (size_t i = 0; i < saves->count; i++) {
        const sigil_sync_save *o = &saves->items[i];
        if (owner >= 0 && o->owner != owner) continue;
        const sigil_sync_blob *b = (const sigil_sync_blob *)o->save;
        saves->kind->file_name(o->save, members[m].name, sizeof(members[m].name));
        members[m].data = b->data;
        members[m].len = b->len;
        m++;
    }
    int rc = SIGIL_OK;
    if (n == 1) {
        uint8_t *copy = (uint8_t *)malloc(members[0].len);
        if (!copy) rc = SIGIL_ERR_OOM;
        else {
            memcpy(copy, members[0].data, members[0].len);
            single_unit(copy, members[0].len, members[0].name, out, len, shape, artifact, content);
        }
    } else {
        rc = zip_unit(members, n, stem, out, len, shape, artifact, content);
    }
    free(members);
    return rc;
}

/* A unit holding saves on the platform's main volume alone (internal memory,
 * VMU A1) is that one volume; with saves on another device it is a zip, so
 * the member name says which device a volume is whatever its size. A holding
 * unit is always a zip and keeps each source volume's size. */
int sigil_sync_build_unit(const sigil_sync_saves *saves, int owner, const sigil_sync_sources *src, bool holding,
                          const char *stem, uint8_t **out, size_t *len, int *shape,
                          char artifact[SIGIL_SAVE_ENTRY_MAX], char content[33]) {
    *out = NULL;
    *len = 0;
    if (sigil_sync_count_where(saves, owner, SIZE_MAX) == 0) return SIGIL_OK;
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
        snprintf(members[n].name, sizeof(members[n].name), "%s", sigil_sync_device_name(device));
        member_device[n] = device;
        rc = build_card(saves, owner, device, src->source[device], holding, &members[n].data, &members[n].len);
        if (rc == SIGIL_OK) n++;
    }
    if (rc == SIGIL_OK && n == 1 && !holding && member_device[0] == saves->kind->main_device) {
        single_unit(members[0].data, members[0].len, members[0].name, out, len, shape, artifact, content);
        return SIGIL_OK;
    }
    if (rc == SIGIL_OK) rc = zip_unit(members, n, stem, out, len, shape, artifact, content);
    for (size_t i = 0; i < n; i++) free(members[i].data);
    return rc;
}

/* ---- reading units ------------------------------------------------------------- */

/* A unit of save files: each goes onto a scratch card, which lists the
 * game's saves as any card does. */
static int file_unit_saves(const sigil_sync_ctx *x, const uint8_t *unit, size_t len, size_t who,
                           sigil_sync_saves *out) {
    sigil_zip_member *members = NULL;
    size_t count = 0;
    bool zip = len >= 4 && sigil_read_le32(unit) == 0x04034b50u;
    int rc = zip ? sigil_zip_read_mem(unit, len, SYNC_MAX_UNIT_MEMBER, &members, &count) : SIGIL_OK;
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
    if (rc == SIGIL_OK) rc = sigil_sync_add_card_saves(x, card, format, SIGIL_DEVICE_NONE, SIZE_MAX, who, out);
    if (card) x->kind->free_card(card);
    sigil_zip_members_free(members, count);
    return rc == SIGIL_ERR_EXISTS ? SIGIL_ERR_UNSUPPORTED_FORMAT : rc;
}

/* Adds the saves of the unit that `who` takes to `out`, every volume's
 * device noted. Saves of other games in a card platform's unit are left
 * out, since the unit speaks only for its own game. `sizes` receives each
 * device's volume size. SIGIL_ERR_NOT_FOUND when the unit adds none. */
static int unit_saves(const sigil_sync_ctx *x, const uint8_t *unit, size_t len, size_t who, sigil_sync_saves *out,
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
    if (zip) rc = sigil_zip_read_mem(unit, len, SYNC_MAX_UNIT_MEMBER, &members, &count);
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
        rc = sigil_sync_load_bytes(x->kind, data, data_len, device, &card, &format);
        if (rc != SIGIL_OK) break;
        if (device != SIGIL_DEVICE_NONE && !sizes[device]) sizes[device] = x->kind->size(card);
        rc = sigil_sync_add_card_saves(x, card, format, device, SIZE_MAX, who, out);
        x->kind->free_card(card);
    }
    sigil_zip_members_free(members, count);
    return rc == SIGIL_OK && out->count == before ? SIGIL_ERR_NOT_FOUND : rc;
}

int sigil_sync_request_saves(const sigil_sync_ctx *x, const uint8_t *unit, size_t len, sigil_sync_saves *out,
                             size_t sizes[SIGIL_DEVICE_COUNT]) {
    sigil_sync_saves_init(out, x->kind);
    memset(sizes, 0, SIGIL_DEVICE_COUNT * sizeof(size_t));
    int rc = unit_saves(x, unit, len, SYNC_WHO_GAME, out, sizes);
    for (size_t c = 0; c < x->req->companion_count && rc == SIGIL_OK; c++) {
        const sigil_sync_companion *k = &x->req->companions[c];
        if (k->unit) rc = unit_saves(x, k->unit, k->unit_len, c, out, sizes);
    }
    for (size_t i = 0; i < out->count && rc == SIGIL_OK && x->kind->foreign; i++) {
        const sigil_sync_save *o = &out->items[i];
        if (o->owner == SYNC_OWN_COMPANION && x->kind->foreign(x->req, o->save)) rc = SIGIL_ERR_INVALID_ARG;
    }
    if (rc != SIGIL_OK) sigil_sync_saves_free(out);
    return rc;
}

/* ---- placing ------------------------------------------------------------------- */

/* The blocks it lacked are the blocks it takes on an empty card like this
 * one, less the card's free blocks; 0 when the card has the blocks and lacks
 * a directory slot. */
void sigil_sync_note_overflow(const sigil_sync_ctx *x, const void *card, int format, int device,
                              const sigil_sync_save *o, sigil_sync_result *r) {
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

int sigil_sync_write_and_verify(const sigil_sync_ctx *x, const char *path, int device, const void *card,
                                const sigil_sync_saves *placed, size_t index) {
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
