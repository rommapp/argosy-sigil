// SPDX-License-Identifier: MPL-2.0
/* Card platforms: the game's card files, whether its own card or shared
 * ones, and putting a unit's saves back on them. */
#include "sync_internal.h"

void sigil_sync_cards_free(sigil_sync_cards *s) {
    for (size_t i = 0; i < s->count; i++) s->kind->free_card(s->files[i].card);
    s->count = 0;
}

static int load_card_file(const sigil_sync_request *req, const char *path, sigil_sync_cards *s) {
    if (s->count >= SYNC_MAX_CARD_FILES) return SIGIL_OK;
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->files[i].path, path) == 0) return SIGIL_OK;
    }
    sigil_io *io = req->save.open(req->save.open_ctx, path);
    if (!io) return SIGIL_OK;
    if (io->size && io->size(io->ctx) == 0) {
        sigil_io_close(io);
        return SIGIL_OK;
    }
    sigil_sync_card_file *f = &s->files[s->count];
    memset(f, 0, sizeof(*f));
    int rc = s->kind->load(io, SIGIL_DEVICE_NONE, &f->card, &f->format);
    sigil_io_close(io);
    if (rc == SIGIL_ERR_UNSUPPORTED_FORMAT) {
        snprintf(s->problem, sizeof(s->problem), "%s", path);
        return SIGIL_ERR_DAMAGED;
    }
    if (rc != SIGIL_OK) return rc;
    rc = s->kind->check ? s->kind->check(f->card) : SIGIL_OK;
    if (rc == SIGIL_ERR_DAMAGED && req->repair) {
        f->changed = true;
        rc = SIGIL_OK;
    }
    if (rc != SIGIL_OK) {
        s->kind->free_card(f->card);
        if (rc == SIGIL_ERR_DAMAGED) snprintf(s->problem, sizeof(s->problem), "%s", path);
        return rc;
    }
    snprintf(f->path, sizeof(f->path), "%s", path);
    f->primary = strcmp(path, s->primary_path) == 0;
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

/* SIGIL_ERR_AMBIGUOUS, naming the files one per line in `problem`, when more
 * than one alternative for a device is there: sigil can't tell which one the
 * emulator reads. */
static int check_alternatives(const sigil_sync_request *req, char (*shared)[SIGIL_SAVE_PATH_MAX], const int *devices,
                              size_t count, char problem[SIGIL_SAVE_PATH_MAX]) {
    for (size_t i = 0; i < count; i++) {
        if (devices[i] == SIGIL_DEVICE_NONE || !sigil_sync_listed(req, shared[i])) continue;
        size_t used = (size_t)snprintf(problem, SIGIL_SAVE_PATH_MAX, "%s", shared[i]);
        size_t present = 1;
        for (size_t j = i + 1; j < count; j++) {
            if (devices[j] != devices[i] || !sigil_sync_listed(req, shared[j])) continue;
            present++;
            if (used < SIGIL_SAVE_PATH_MAX) used += (size_t)snprintf(problem + used, SIGIL_SAVE_PATH_MAX - used, "\n%s", shared[j]);
        }
        if (present > 1) return SIGIL_ERR_AMBIGUOUS;
        problem[0] = '\0';
    }
    return SIGIL_OK;
}

/* The game's own card is the layout's primary member; a layout whose game
 * cards are all shared uses its first shared card present, or the first. */
int sigil_sync_gather_cards(const sigil_sync_ctx *x, sigil_sync_cards *s, char artifact[SIGIL_SAVE_ENTRY_MAX]) {
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
    char shared[SYNC_MAX_SHARED][SIGIL_SAVE_PATH_MAX];
    int devices[SYNC_MAX_SHARED];
    size_t shared_count = sigil_save_shared_paths(&req->save, shared, devices, SYNC_MAX_SHARED);
    rc = check_alternatives(req, shared, devices, shared_count, s->problem);
    if (rc != SIGIL_OK) {
        sigil_save_unit_free(unit);
        return rc;
    }
    bool folder[SYNC_MAX_SHARED] = { false };
    for (size_t i = 0; i < shared_count; i++) folder[i] = x->kind->folder_cards && sigil_sync_is_folder_card(req, shared[i]);
    if (!s->primary_path[0] && shared_count > 0) {
        size_t pick = 0;
        for (size_t i = 0; i < shared_count; i++) {
            if (folder[i] || sigil_sync_listed(req, shared[i])) { pick = i; break; }
        }
        snprintf(s->primary_path, SIGIL_SAVE_PATH_MAX, "%s", shared[pick]);
        artifact_from(req, shared[pick], artifact);
    }
    if (s->kind->raw_ext && s->kind->new_form && s->kind->new_form(req, s->primary_path) != SIGIL_FORM_RAW) {
        char stem[SIGIL_SAVE_ENTRY_MAX];
        sigil_content_stem(req->save.content_path, stem, sizeof(stem));
        snprintf(artifact, SIGIL_SAVE_ENTRY_MAX, "%s%s", stem, s->kind->raw_ext);
    }

    for (size_t i = 0; i < unit->member_count && rc == SIGIL_OK; i++) rc = load_card_file(req, unit->members[i].path, s);
    for (size_t i = 0; i < unit->unkeyed_count && rc == SIGIL_OK; i++) rc = load_card_file(req, unit->unkeyed[i], s);
    for (size_t i = 0; i < shared_count && rc == SIGIL_OK; i++) {
        if (folder[i]) rc = sigil_sync_load_folder_card(x, shared[i], s);
    }
    sigil_save_unit_free(unit);
    if (rc != SIGIL_OK) sigil_sync_cards_free(s);
    return rc;
}

int sigil_sync_gather_card_saves(const sigil_sync_ctx *x, sigil_sync_cards *s, sigil_sync_saves *out) {
    sigil_sync_saves_init(out, s->kind);
    int rc = SIGIL_OK;
    for (size_t c = 0; c < s->count && rc == SIGIL_OK; c++) {
        rc = sigil_sync_add_card_saves(x, s->files[c].card, s->files[c].format, SIGIL_DEVICE_NONE, c, SYNC_WHO_LOCAL,
                                       out);
        if (rc == SIGIL_ERR_DAMAGED) snprintf(s->problem, sizeof(s->problem), "%s", s->files[c].path);
    }
    if (rc != SIGIL_OK) sigil_sync_saves_free(out);
    return rc;
}

/* Removes from card `c` every save of the game and of each companion the
 * restore carries a unit for, and records on which card each name sat so a
 * newer copy goes back to the same card. */
static int clear_game(const sigil_sync_ctx *x, sigil_sync_cards *s, size_t c, sigil_sync_saves *placements) {
    sigil_sync_card_file *f = &s->files[c];
    sigil_card_listing *listing = NULL;
    int rc = s->kind->list(f->card, f->format, &listing);
    if (rc != SIGIL_OK) return rc;
    for (size_t i = 0; i < listing->entry_count && rc == SIGIL_OK; i++) {
        const sigil_card_entry *e = &listing->entries[i];
        if (!sigil_sync_owned_by_game(x->req, e->owner_id) &&
            !sigil_sync_companion_restored(x, sigil_sync_companion_of(x->req, e->owner_id))) {
            continue;
        }
        rc = s->kind->remove(f->card, e);
        f->changed = true;
        for (size_t k = 0; k < placements->count; k++) {
            if (strcmp(placements->items[k].name, e->name) == 0) placements->items[k].card = c;
        }
    }
    sigil_card_listing_free(listing);
    return rc;
}

/* The game's own card, made when it doesn't exist yet. */
static size_t primary_card(const sigil_sync_ctx *x, sigil_sync_cards *s, int *rc) {
    for (size_t c = 0; c < s->count; c++) {
        if (s->files[c].primary) return c;
    }
    if (!s->primary_path[0] || s->count >= SYNC_MAX_CARD_FILES) { *rc = SIGIL_ERR_INVALID_ARG; return 0; }
    sigil_sync_card_file *f = &s->files[s->count];
    memset(f, 0, sizeof(*f));
    int form = s->kind->new_form ? s->kind->new_form(x->req, s->primary_path) : SIGIL_FORM_RAW;
    *rc = s->kind->blank(&f->card, &f->format, SIGIL_DEVICE_NONE, 0, form, NULL);
    if (*rc != SIGIL_OK) return 0;
    f->primary = true;
    f->changed = true;
    snprintf(f->path, sizeof(f->path), "%s", s->primary_path);
    return s->count++;
}

/* Each save goes back on the card it sat on, a new one on the game's own
 * card. Nothing is written until every card took its saves and every folder
 * card's removals can run. */
int sigil_sync_place_on_cards(const sigil_sync_ctx *x, sigil_sync_cards *cards, sigil_sync_saves *incoming,
                              sigil_sync_result *r) {
    int rc = SIGIL_OK;
    for (size_t i = 0; i < incoming->count; i++) incoming->items[i].card = SIZE_MAX;
    for (size_t c = 0; c < cards->count && rc == SIGIL_OK; c++) rc = clear_game(x, cards, c, incoming);
    size_t primary = rc == SIGIL_OK ? primary_card(x, cards, &rc) : 0;
    for (size_t i = 0; i < incoming->count && rc == SIGIL_OK; i++) {
        sigil_sync_save *o = &incoming->items[i];
        if (o->card == SIZE_MAX) o->card = primary;
        sigil_sync_card_file *f = &cards->files[o->card];
        rc = cards->kind->inject(f->card, o->save);
        if (rc == SIGIL_ERR_NO_SPACE) sigil_sync_note_overflow(x, f->card, f->format, o, r);
        f->changed = true;
    }
    for (size_t c = 0; c < cards->count && rc == SIGIL_OK; c++) {
        size_t removes = 0;
        if (cards->files[c].changed && cards->files[c].folder) {
            rc = sigil_sync_check_folder_card(x, &cards->files[c], &removes, r);
        }
        if (rc == SIGIL_OK && removes && !x->req->remove) rc = SIGIL_ERR_INVALID_ARG;
    }
    for (size_t c = 0; c < cards->count && rc == SIGIL_OK; c++) {
        const sigil_sync_card_file *f = &cards->files[c];
        if (!f->changed) continue;
        rc = f->folder ? sigil_sync_write_folder_card(x, f)
                       : sigil_sync_write_and_verify(x, f->path, SIGIL_DEVICE_NONE, f->card, incoming, c);
    }
    return rc;
}
