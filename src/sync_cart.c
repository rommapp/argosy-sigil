// SPDX-License-Identifier: MPL-2.0
/* The save file a cartridge converter reads and writes: the file the layout
 * row names for the game, its bytes, the name the unit travels under, and
 * the members of a unit that travels as a zip. */
#include "sync_internal.h"

int sigil_sync_cart_file_of(const sigil_sync_request *req, sigil_sync_cart_file *f) {
    memset(f, 0, sizeof(*f));
    sigil_save_unit *unit = NULL;
    int rc = sigil_save_resolve(&req->save, &unit);
    if (rc != SIGIL_OK) return rc;
    for (size_t i = 0; i < unit->member_count + unit->expected_count && !f->path[0]; i++) {
        bool present = i < unit->member_count;
        const sigil_save_member *m = present ? &unit->members[i] : &unit->expected[i - unit->member_count];
        if (m->role != SIGIL_SAVE_ROLE_PRIMARY) continue;
        snprintf(f->path, sizeof(f->path), "%s", m->path);
        f->present = present;
    }
    sigil_save_unit_free(unit);
    return SIGIL_OK;
}

int sigil_sync_cart_read(const sigil_sync_request *req, const sigil_sync_cart_file *f, size_t cap, uint8_t **data,
                         size_t *len) {
    *data = NULL;
    *len = 0;
    if (!f->present) return SIGIL_OK;
    int rc = sigil_sync_read_file(req, f->path, cap, data, len);
    if (rc != SIGIL_OK || !*len) {
        free(*data);
        *data = NULL;
        *len = 0;
    }
    return rc == SIGIL_ERR_NOT_FOUND ? SIGIL_ERR_IO : rc;
}

void sigil_sync_cart_artifact(const sigil_sync_request *req, const char *ext, sigil_sync_result *r) {
    char stem[SIGIL_SAVE_ENTRY_MAX];
    sigil_content_stem(req->save.content_path, stem, sizeof(stem));
    snprintf(r->artifact, sizeof(r->artifact), "%s%s", stem, ext);
}

int sigil_sync_cart_read_zip(const uint8_t *unit, size_t len, size_t max_member, sigil_sync_cart_take_fn take,
                             void *ctx, sigil_sync_result *r) {
    sigil_zip_member *members = NULL;
    size_t count = 0;
    int rc = sigil_zip_read_mem(unit, len, max_member, &members, &count);
    sigil_named_md5 *parts = rc == SIGIL_OK ? (sigil_named_md5 *)calloc(count ? count : 1, sizeof(*parts)) : NULL;
    if (rc == SIGIL_OK && !parts) rc = SIGIL_ERR_OOM;
    for (size_t i = 0; i < count && rc == SIGIL_OK; i++) {
        const sigil_zip_member *m = &members[i];
        snprintf(parts[i].name, sizeof(parts[i].name), "%s", m->name);
        sigil_md5_of(m->data, m->len, parts[i].md5);
        rc = take(ctx, m);
        if (rc != SIGIL_OK) snprintf(r->problem, sizeof(r->problem), "%s", m->name);
    }
    if (rc == SIGIL_OK && count) sigil_named_hash(parts, count, r->content_hash);
    free(parts);
    if (members) sigil_zip_members_free(members, count);
    return rc;
}
