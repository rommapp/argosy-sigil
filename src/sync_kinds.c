// SPDX-License-Identifier: MPL-2.0
/* What the card kinds share, and the choice of kind for a request. */
#include "sync_internal.h"

sigil_sync_blob *sigil_sync_blob_new(uint8_t *data, size_t len) {
    sigil_sync_blob *s = (sigil_sync_blob *)calloc(1, sizeof(*s));
    if (!s) { free(data); return NULL; }
    s->data = data;
    s->len = len;
    return s;
}

void sigil_sync_blob_free(void *save) {
    sigil_sync_blob *s = (sigil_sync_blob *)save;
    if (s) free(s->data);
    free(s);
}

int sigil_sync_copy_image(const uint8_t *card, size_t size, uint8_t **out, size_t *len) {
    *out = (uint8_t *)malloc(size);
    if (!*out) return SIGIL_ERR_OOM;
    memcpy(*out, card, size);
    *len = size;
    return SIGIL_OK;
}

size_t sigil_sync_no_size(const void *card) {
    (void)card;
    return 0;
}

size_t sigil_sync_no_unit_size(int device, const void *source) {
    (void)device;
    (void)source;
    return 0;
}

const sigil_sync_kind *sigil_sync_kind_for(const sigil_sync_request *req) {
    const char *slug = req->save.platform ? sigil_layout_platform(req->save.platform) : NULL;
    if (slug) {
        if (strcmp(slug, "saturn") == 0) return &sigil_sync_saturn_kind;
        if (strcmp(slug, "segacd") == 0) return &sigil_sync_segacd_kind;
    }
    sigil_platform p = slug ? sigil_platform_from_slug(slug) : SIGIL_PLATFORM_AUTO;
    if (p == SIGIL_PLATFORM_AUTO && req->save.result) p = (sigil_platform)req->save.result->platform;
    if (p == SIGIL_PLATFORM_PSX) return &sigil_sync_ps1_kind;
    if (p == SIGIL_PLATFORM_PS2) return &sigil_sync_ps2_kind;
    if (p == SIGIL_PLATFORM_DREAMCAST) return &sigil_sync_vmu_kind;
    if (p == SIGIL_PLATFORM_GAMECUBE) return &sigil_sync_gamecube_kind;
    return NULL;
}
