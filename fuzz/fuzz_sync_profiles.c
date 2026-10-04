// SPDX-License-Identifier: MPL-2.0
/* Restores units into the folders of emulators that keep saves per user
 * profile. The input is the unit's members, one per line as "name<TAB>data",
 * so mutations reach member placement rather than the zip reader, which
 * fuzz_sync covers. */
#include "sigil_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOT_FILES 256

typedef struct { const uint8_t *data; size_t len; } mem_ctx;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_ctx *m = (mem_ctx *)ctx;
    if (off >= m->len) return 0;
    size_t n = m->len - (size_t)off < len ? m->len - (size_t)off : len;
    memcpy(buf, m->data + off, n);
    return (int)n;
}
static int64_t mem_size(void *ctx) { return (int64_t)((mem_ctx *)ctx)->len; }
static void mem_close(void *ctx) { free(ctx); }

typedef struct {
    char    *paths[ROOT_FILES];
    uint8_t *data[ROOT_FILES];
    size_t   lens[ROOT_FILES];
    size_t   count;
    const char *listing[ROOT_FILES];
    const char *game_folder;   /* every write lands under this, or under device_folder */
    const char *device_folder;
} root;

static void broken(const char *what, const char *path) {
    fprintf(stderr, "oracle: %s %s\n", what, path ? path : "");
    abort();
}

static int find(const root *r, const char *path) {
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(r->paths[i], path) == 0) return (int)i;
    }
    return -1;
}

static int put(root *r, const char *path, const uint8_t *data, size_t len) {
    int at = find(r, path);
    if (at < 0) {
        if (r->count == ROOT_FILES) return -1;
        at = (int)r->count++;
        r->paths[at] = strdup(path);
        r->data[at] = NULL;
    }
    free(r->data[at]);
    r->data[at] = (uint8_t *)malloc(len ? len : 1);
    if (len) memcpy(r->data[at], data, len);
    r->lens[at] = len;
    for (size_t i = 0; i < r->count; i++) r->listing[i] = r->paths[i];
    return 0;
}

static void root_free(root *r) {
    for (size_t i = 0; i < r->count; i++) {
        free(r->paths[i]);
        free(r->data[i]);
    }
    r->count = 0;
}

static sigil_io *root_open(void *ctx, const char *path) {
    root *r = (root *)ctx;
    int at = find(r, path);
    if (at < 0) return NULL;
    mem_ctx *m = (mem_ctx *)malloc(sizeof(*m));
    sigil_io *io = (sigil_io *)malloc(sizeof(*io));
    if (!m || !io) { free(m); free(io); return NULL; }
    m->data = r->data[at];
    m->len = r->lens[at];
    io->read = mem_read;
    io->size = mem_size;
    io->close = mem_close;
    io->ctx = m;
    return io;
}

/* `path` lies in `folder` and has no ".." segment. */
static bool under(const char *path, const char *folder) {
    if (!folder || strncmp(path, folder, strlen(folder)) != 0) return false;
    for (const char *s = path; *s;) {
        size_t n = strcspn(s, "/\\");
        if (n == 2 && s[0] == '.' && s[1] == '.') return false;
        s += n;
        if (*s) s++;
    }
    return true;
}

static int root_write(void *ctx, const char *path, const uint8_t *data, size_t len) {
    root *r = (root *)ctx;
    if (!under(path, r->game_folder) && !under(path, r->device_folder)) broken("write outside the game", path);
    return put(r, path, data, len);
}

static int root_remove(void *ctx, const char *path) {
    root *r = (root *)ctx;
    if (!under(path, r->game_folder) && !under(path, r->device_folder)) broken("remove outside the game", path);
    int at = find(r, path);
    if (at < 0) return -1;
    free(r->paths[at]);
    free(r->data[at]);
    r->count--;
    r->paths[at] = r->paths[r->count];
    r->data[at] = r->data[r->count];
    r->lens[at] = r->lens[r->count];
    for (size_t i = 0; i < r->count; i++) r->listing[i] = r->paths[i];
    return 0;
}

/* The members the input names, as a zip; NULL when it names none. */
static uint8_t *unit_of(const uint8_t *data, size_t size, size_t *len) {
    sigil_zip_member members[64];
    size_t n = 0;
    for (size_t at = 0; at < size && n < 64;) {
        const uint8_t *line = data + at;
        const uint8_t *end = memchr(line, '\n', size - at);
        size_t line_len = end ? (size_t)(end - line) : size - at;
        const uint8_t *tab = memchr(line, '\t', line_len);
        size_t name_len = tab ? (size_t)(tab - line) : line_len;
        if (name_len && name_len < sizeof(members[n].name)) {
            memcpy(members[n].name, line, name_len);
            members[n].name[name_len] = '\0';
            members[n].data = (uint8_t *)(tab ? tab + 1 : line + line_len);
            members[n].len = tab ? line_len - name_len - 1 : 0;
            n++;
        }
        at += line_len + 1;
    }
    uint8_t *zip = NULL;
    if (!n || sigil_zip_store(members, n, &zip, len) != SIGIL_OK) return NULL;
    return zip;
}

/* Restores `unit` into `r`; a restore that succeeds must collect back to the
 * saves it wrote. */
static void round_trip(root *r, const char *layout, const char *platform, const char *save_id, const uint8_t *unit,
                       size_t unit_len) {
    sigil_result result;
    memset(&result, 0, sizeof(result));
    result.struct_version = SIGIL_RESULT_V3;
    result.platform = sigil_platform_from_slug(platform);
    snprintf(result.title_id, sizeof(result.title_id), "%s", save_id);
    snprintf(result.save_id, sizeof(result.save_id), "%s", save_id);

    sigil_sync_request req;
    memset(&req, 0, sizeof(req));
    req.struct_version = SIGIL_SYNC_REQUEST_V1;
    req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    req.save.layout = layout;
    req.save.platform = platform;
    req.save.content_path = "game.bin";
    req.save.result = &result;
    req.save.listing = r->listing;
    req.save.listing_count = r->count;
    req.save.open = root_open;
    req.save.open_ctx = r;
    req.write = root_write;
    req.remove = root_remove;
    req.write_ctx = r;
    req.overwrite_local = 1;

    sigil_sync_result *restored = NULL;
    int rc = sigil_restore(&req, unit, unit_len, &restored);
    if (rc == SIGIL_OK) {
        req.save.listing_count = r->count;
        req.state = restored->state;
        req.state_len = restored->state_len;
        sigil_sync_result *collected = NULL;
        if (sigil_collect(&req, &collected) != SIGIL_OK) broken("collect after restore failed", layout);
        if (strcmp(collected->identity_hash, restored->identity_hash) != 0 || collected->changed) {
            broken("restored saves collect back changed", layout);
        }
        sigil_sync_result_free(collected);
        sigil_save_unit *resolved = NULL;
        if (sigil_save_resolve(&req.save, &resolved) != SIGIL_OK) broken("resolve after restore failed", layout);
        sigil_save_unit_free(resolved);
    }
    sigil_sync_result_free(restored);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t unit_len = 0;
    uint8_t *unit = unit_of(data, size, &unit_len);
    if (!unit) return 0;

    root eden;
    memset(&eden, 0, sizeof(eden));
    uint8_t profiles[0x10 + 8 * 0xC8];
    memset(profiles, 0, sizeof(profiles));
    for (size_t k = 0; k < 16; k++) profiles[0x10 + k] = (uint8_t)(0x11 * (k + 1));
    memcpy(profiles + 0x10 + 0x28, "Fuzz", 4);
    put(&eden, "nand/system/save/8000000000000010/su/avators/profiles.dat", profiles, sizeof(profiles));
    eden.game_folder = "nand/user/save/0000000000000000/10FFEEDDCCBBAA998877665544332211/01007EF00011E000/";
    eden.device_folder = "nand/user/save/0000000000000000/00000000000000000000000000000000/01007EF00011E000/";
    round_trip(&eden, "eden", "switch", "01007EF00011E000", unit, unit_len);
    root_free(&eden);

    root cemu;
    memset(&cemu, 0, sizeof(cemu));
    static const char account[] = "PersistentId=80000001\nMiiName=00460075007a007a\n";
    put(&cemu, "mlc01/usr/save/system/act/80000001/account.dat", (const uint8_t *)account, sizeof(account) - 1);
    cemu.game_folder = "mlc01/usr/save/00050000/101c9400/";
    round_trip(&cemu, "cemu", "wiiu", "101c9400", unit, unit_len);
    root_free(&cemu);

    free(unit);
    return 0;
}
