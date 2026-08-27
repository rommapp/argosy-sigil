// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"
#include <stdlib.h>

/* A Vita title is identified by TITLE_ID in sce_sys/param.sfo, the same file
 * Vita3K reads to identify installed content (vita3k/interface.cpp). Dumps in
 * circulation carry it at app/<TITLEID>/sce_sys/param.sfo inside a zip, which
 * the archive member reader reaches directly, so the identifier comes from the
 * binary rather than from a bracket in the file name.
 *
 * The filename scanner stays as a fallback for dumps whose param.sfo cannot be
 * reached, but it is no longer the only path. */

#define VITA_SFO_MAX_BYTES (256u * 1024u)
#define VITA_SFO_MAGIC     0x46535000u  /* "\0PSF" little-endian */

int sigil_extract_psvita(const sigil_io *io, const char *filename_hint,
                         const sigil_options *opts, sigil_result *out) {
    (void)opts;

    if (io && io->read && io->size) {
        int64_t total = io->size(io->ctx);
        if (total > 0) {
            size_t want = (size_t)((uint64_t)total < VITA_SFO_MAX_BYTES
                                   ? (uint64_t)total : VITA_SFO_MAX_BYTES);
            uint8_t *buf = (uint8_t *)malloc(want);
            if (buf) {
                int got = io->read(io->ctx, 0, buf, want);
                char title_id[32] = {0};
                if (got > 0
                    && sigil_read_le32(buf) == VITA_SFO_MAGIC
                    && sigil_sfo_get_string(buf, (size_t)got, "TITLE_ID",
                                            title_id, sizeof(title_id)) == SIGIL_OK
                    && title_id[0] != '\0'
                    && strlen(title_id) < sizeof(out->title_id)) {
                    free(buf);
                    sigil_result_init(out);
                    out->platform = SIGIL_PLATFORM_PSVITA;
                    /* Saves land in ux0:user/00/savedata/<TITLEID>, one exact
                     * directory per title rather than a prefixed family. */
                    out->usage    = SIGIL_USAGE_FOLDER_EXACT;
                    size_t n = strlen(title_id);
                    memcpy(out->title_id,   title_id, n + 1);
                    memcpy(out->raw_serial, title_id, n + 1);
                    memcpy(out->save_id,    title_id, n + 1);
                    out->source = SIGIL_SOURCE_BINARY;
                    return SIGIL_OK;
                }
                free(buf);
            }
        }
    }

    return sigil_filename_fallback(filename_hint, SIGIL_PLATFORM_PSVITA, out);
}
