// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include <stdio.h>
#include <string.h>

typedef struct { const uint8_t *buf; size_t len; } mem_ctx;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_ctx *m = (mem_ctx *)ctx;
    if (off >= m->len) return 0;
    size_t avail = m->len - (size_t)off;
    size_t n = len < avail ? len : avail;
    memcpy(buf, m->buf + off, n);
    return (int)n;
}
static int64_t mem_size(void *ctx) { return (int64_t)((mem_ctx *)ctx)->len; }

static int expect(const char *filename, sigil_platform hint,
                  const char *want_title, const char *want_save,
                  sigil_usage want_usage, const char *label) {
    uint8_t buf[2048];
    memset(buf, 0, sizeof(buf));
    mem_ctx ctx = { buf, sizeof(buf) };
    sigil_io io = { mem_read, mem_size, NULL, &ctx };
    sigil_result r;
    int rc = sigil_extract_from_io(&io, filename, hint, NULL, &r);
    if (rc != SIGIL_OK) {
        fprintf(stderr, "FAIL %s: rc=%d\n", label, rc);
        return 1;
    }
    if (r.source != SIGIL_SOURCE_FILENAME) {
        fprintf(stderr, "FAIL %s: source=%d\n", label, (int)r.source);
        return 1;
    }
    if (strcmp(r.title_id, want_title) != 0 || strcmp(r.save_id, want_save) != 0) {
        fprintf(stderr, "FAIL %s: title_id='%s' save_id='%s'\n",
                label, r.title_id, r.save_id);
        return 1;
    }
    if (r.usage != want_usage) {
        fprintf(stderr, "FAIL %s: usage=%d (want %d)\n", label, (int)r.usage, (int)want_usage);
        return 1;
    }
    return 0;
}

/* A serial read off the filename addresses the same on-disk location as one
 * read off the disc, so the fallback owes the same save_id and usage the binary
 * extractor emits. */
int main(void) {
    if (expect("Ace Combat 04 [SLUS-20152].iso", SIGIL_PLATFORM_PS2,
               "SLUS-20152", "BASLUS-20152", SIGIL_USAGE_FOLDER_PREFIX, "ps2 ntsc-u")) {
        return 1;
    }
    if (expect("Ico [SLES-50760].iso", SIGIL_PLATFORM_PS2,
               "SLES-50760", "BESLES-50760", SIGIL_USAGE_FOLDER_PREFIX, "ps2 pal")) {
        return 1;
    }
    printf("ok unit_filename\n");
    return 0;
}
