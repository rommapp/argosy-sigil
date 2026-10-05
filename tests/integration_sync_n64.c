// SPDX-License-Identifier: MPL-2.0
/* N64 sync against saves from real games: the neutral unit holds the regions
 * each game uses, every emulator's files give the same unit, and a restore
 * writes each emulator's own files. */
#include "save_corpus.h"
#include "legacy_units.h"
#include "n64_save.h"

#define TEST_SKIP 77
#define MD5    "ABCDEF0123456789ABCDEF0123456789"
#define MD5_N64 "0123456789ABCDEF0123456789ABCDEF"

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static corpus_table g_manifest;

static uint8_t *sample(const char *id, size_t *len) {
    return corpus_sample(&g_manifest, "n64", id, NULL, len);
}

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    const char        *ids[1];
} game;

static void make_game(game *g, mem_root *root, const char *layout, const char *header, const char *code) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V4;
    g->result.platform = SIGIL_PLATFORM_N64;
    snprintf(g->result.title_id, sizeof(g->result.title_id), "%s", code);
    snprintf(g->result.n64_header, sizeof(g->result.n64_header), "%s", header);
    snprintf(g->result.n64_md5, sizeof(g->result.n64_md5), "%s", MD5);
    snprintf(g->result.n64_md5_n64, sizeof(g->result.n64_md5_n64), "%s", MD5_N64);
    g->ids[0] = code;
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = "n64";
    g->req.save.content_path = "Game (USA).z64";
    g->req.save.result = &g->result;
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
    g->req.save.open = root_open;
    g->req.save.open_ctx = root;
    g->req.game_ids = g->ids;
    g->req.game_id_count = 1;
    g->req.write = root_write;
    g->req.remove = root_remove;
    g->req.write_ctx = root;
}

static void refresh(game *g, mem_root *root) {
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
}

static sigil_sync_result *collect_from(mem_root *root, const char *layout, const char *header) {
    game g;
    make_game(&g, root, layout, header, "NXXE");
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK) {
        sigil_sync_result_free(r);
        return NULL;
    }
    return r;
}

/* The unit's member names, joined with ',' in unit order. */
static void member_names(const sigil_sync_result *r, char *out, size_t cap) {
    sigil_zip_member *m = NULL;
    size_t n = 0;
    out[0] = '\0';
    if (!r || !r->data || sigil_zip_read_mem(r->data, r->len, N64_BLOB_SIZE, &m, &n) != SIGIL_OK) return;
    for (size_t i = 0; i < n; i++) {
        size_t used = strlen(out);
        snprintf(out + used, cap - used, "%s%s", i ? "," : "", m[i].name);
    }
    sigil_zip_members_free(m, n);
}

static bool member_is(const sigil_sync_result *r, const char *name, const uint8_t *want, size_t len) {
    sigil_zip_member *m = NULL;
    size_t n = 0;
    bool same = false;
    if (!r || !r->data || sigil_zip_read_mem(r->data, r->len, N64_BLOB_SIZE, &m, &n) != SIGIL_OK) return false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(m[i].name, name) == 0) same = m[i].len == len && memcmp(m[i].data, want, len) == 0;
    }
    sigil_zip_members_free(m, n);
    return same;
}

typedef struct {
    const char *id;
    const char *members;
} region_case;

/* Each game's unit holds the regions it wrote, in bus order. */
static void check_regions(void) {
    static const region_case CASES[] = {
        { "sm64-retroarch", "eeprom" },
        { "banjo-retroarch", "eeprom" },
        { "cruisn-usa-mupen64plus-next", "eeprom,pak1" },
        { "1080-builtin", "sram" },
        { "majora-builtin", "" },   /* flash all 0xFF, paks formatted: no save */
        { "ssb-mupen64plus-next", "sram" },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        size_t len = 0;
        uint8_t *blob = sample(CASES[i].id, &len);
        if (!blob) continue;
        mem_root root = {0};
        root_put(&root, "Game (USA).srm", blob, len);
        sigil_sync_result *r = collect_from(&root, "mupen64plus_next", "GAME");
        char names[128];
        member_names(r, names, sizeof(names));
        if (strcmp(names, CASES[i].members) != 0) fail(CASES[i].id, names);
        if (!CASES[i].members[0]) {
            if (!r || r->data) fail(CASES[i].id, "a .srm holding no save gives a unit");
        } else if (!r || r->shape != SIGIL_SAVE_SHAPE_MULTI || strcmp(r->artifact, "Game (USA).zip") != 0) {
            fail(CASES[i].id, "shape or artifact");
        }
        if (strstr(CASES[i].members, "eeprom") && !member_is(r, "eeprom", blob, N64_EEPROM_4K)) {
            fail(CASES[i].id, "eeprom isn't the first 0x200 bytes as stored");
        }
        if (strcmp(CASES[i].members, "sram") == 0) {
            uint8_t *sram = (uint8_t *)malloc(N64_SRAM_SIZE);
            memcpy(sram, blob + 0x20800, N64_SRAM_SIZE);
            n64_swap32(sram, N64_SRAM_SIZE);
            if (!member_is(r, "sram", sram, N64_SRAM_SIZE)) fail(CASES[i].id, "sram isn't in bus order");
            free(sram);
        }
        sigil_sync_result_free(r);
        root_free(&root);
        free(blob);
    }
}

/* One game's files laid out as `layout` keeps them, cut from a libretro .srm. */
static void put_split(mem_root *root, const char *layout, const uint8_t *blob, const char *header) {
    char dir[160], name[160], path[400];
    bool pj64 = strcmp(layout, "project64") == 0, fz = strcmp(layout, "m64plus_fz") == 0;
    if (pj64) snprintf(dir, sizeof(dir), "Save/%s-%s/", header, MD5_N64);
    else if (fz) snprintf(dir, sizeof(dir), "GameData/%s (U) %s/SramData/", header, "abcdef0123456789abcdef0123456789");
    else snprintf(dir, sizeof(dir), "%s", "");
    if (pj64 || fz) snprintf(name, sizeof(name), "%s", header);
    else snprintf(name, sizeof(name), "%s-%.8s", header, MD5);
    n64_save *s = (n64_save *)calloc(1, sizeof(*s));
    n64_take_blob(s, blob, N64_BLOB_SIZE);
    static const char *const EXT[] = { ".eep", NULL, NULL, NULL, NULL, ".sra", ".fla" };
    for (int r = 0; r < N64_REGION_COUNT; r++) {
        if (!s->present[r]) continue;
        bool pak = r >= N64_PAK1 && r <= N64_PAK4;
        if (pak && pj64) snprintf(path, sizeof(path), "%s%s_Cont_%d.mpk", dir, name, r - N64_PAK1 + 1);
        else if (pak) snprintf(path, sizeof(path), "%s%s.mpk", dir, name);
        else snprintf(path, sizeof(path), "%s%s%s", dir, name, EXT[r]);
        if (pak && !pj64) root_put(root, path, blob + 0x800, N64_PAKS * N64_PAK_SIZE);
        else if (pak) root_put(root, path, blob + 0x800 + (size_t)(r - N64_PAK1) * N64_PAK_SIZE, N64_PAK_SIZE);
        else if (r == N64_EEPROM) root_put(root, path, blob, N64_EEPROM_MAX);
        else root_put(root, path, blob + (r == N64_SRAM ? 0x20800 : 0x28800), r == N64_SRAM ? N64_SRAM_SIZE : N64_FLASH_SIZE);
    }
    free(s);
}

/* mupen64plus and Project64 files of a save give the same unit as the .srm. */
static void check_every_form(void) {
    static const char *const IDS[] = { "cruisn-usa-mupen64plus-next", "majora-builtin", "ssb-mupen64plus-next", "sm64-retroarch" };
    static const char *const LAYOUTS[] = { "mupen64plus_standalone", "m64plus_fz", "project64" };
    for (size_t i = 0; i < sizeof(IDS) / sizeof(IDS[0]); i++) {
        size_t len = 0;
        uint8_t *blob = sample(IDS[i], &len);
        if (!blob) continue;
        mem_root srm = {0};
        root_put(&srm, "Game (USA).srm", blob, len);
        sigil_sync_result *want = collect_from(&srm, "parallel_n64", "GAME");
        for (size_t l = 0; l < sizeof(LAYOUTS) / sizeof(LAYOUTS[0]); l++) {
            mem_root split = {0};
            put_split(&split, LAYOUTS[l], blob, "GAME");
            sigil_sync_result *got = collect_from(&split, LAYOUTS[l], "GAME");
            if (!want || !got || strcmp(want->identity_hash, got->identity_hash) != 0) {
                char what[96];
                snprintf(what, sizeof(what), "%s files give another unit than the .srm", LAYOUTS[l]);
                fail(IDS[i], what);
            }
            sigil_sync_result_free(got);
            root_free(&split);
        }
        sigil_sync_result_free(want);
        root_free(&srm);
        free(blob);
    }
}

static sigil_sync_result *restore_into(mem_root *root, const char *layout, const sigil_sync_result *unit, int *rc) {
    game g;
    make_game(&g, root, layout, "GAME", "NXXE");
    g.req.overwrite_local = 1;
    sigil_sync_result *w = NULL;
    *rc = sigil_restore(&g.req, unit->data, unit->len, &w);
    return w;
}

/* A unit restores into each emulator's files and collects back the same. */
static void check_restore_forms(void) {
    size_t len = 0;
    uint8_t *blob = sample("cruisn-usa-mupen64plus-next", &len);
    if (!blob) return;
    mem_root srm = {0};
    root_put(&srm, "Game (USA).srm", blob, len);
    sigil_sync_result *unit = collect_from(&srm, "mupen64plus_next", "GAME");
    int rc = 0;

    mem_root empty = {0};
    sigil_sync_result *w = restore_into(&empty, "mupen64plus_next", unit, &rc);
    mem_file *f = root_find(&empty, "Game (USA).srm");
    if (rc != SIGIL_OK || !f || f->len != N64_BLOB_SIZE || memcmp(f->data, blob, 0x8800) != 0) {
        fail("restore .srm", "a new .srm doesn't hold the eeprom and pak 1 where they were");
    }
    sigil_sync_result *back = collect_from(&empty, "mupen64plus_next", "GAME");
    if (!back || !unit || strcmp(back->identity_hash, unit->identity_hash) != 0) fail("restore .srm", "collects back other saves");
    sigil_sync_result_free(back);
    sigil_sync_result_free(w);

    /* mupen64plus: the .eep there names the files restore adds. */
    mem_root mupen = {0};
    uint8_t blank[N64_EEPROM_MAX];
    memset(blank, 0xFF, sizeof(blank));
    root_put(&mupen, "Cruis'n USA (U) [!]-ABCDEF01.eep", blank, sizeof(blank));
    w = restore_into(&mupen, "mupen64plus_standalone", unit, &rc);
    mem_file *eep = root_find(&mupen, "Cruis'n USA (U) [!]-ABCDEF01.eep");
    mem_file *mpk = root_find(&mupen, "Cruis'n USA (U) [!]-ABCDEF01.mpk");
    uint8_t formatted[N64_PAK_SIZE];
    n64_pak_formatted(formatted);
    if (rc != SIGIL_OK || !eep || eep->len != N64_EEPROM_MAX || memcmp(eep->data, blob, N64_EEPROM_MAX) != 0 || !mpk ||
        mpk->len != N64_PAKS * N64_PAK_SIZE || memcmp(mpk->data, blob + 0x800, N64_PAK_SIZE) != 0 ||
        memcmp(mpk->data + N64_PAK_SIZE, formatted, N64_PAK_SIZE) != 0) {
        fail("restore mupen64plus", "the .eep and a four-pak .mpk beside it, the paks it lacks formatted");
    }
    sigil_sync_result_free(w);

    /* A .mpk already there: an unused pak keeps its own serial, a pak holding another save is formatted. */
    uint8_t *paks = (uint8_t *)malloc(N64_PAKS * N64_PAK_SIZE);
    for (size_t p = 0; p < N64_PAKS; p++) n64_pak_formatted(paks + p * N64_PAK_SIZE);
    paks[N64_PAK_SIZE + 0x20] = 0x9C;                      /* pak 2: unused, another serial */
    paks[2 * N64_PAK_SIZE + 0x10B] = 0x05;                 /* pak 3: a note of another save */
    paks[2 * N64_PAK_SIZE + 0x600] = 0x42;
    root_put(&mupen, "Cruis'n USA (U) [!]-ABCDEF01.mpk", paks, N64_PAKS * N64_PAK_SIZE);
    w = restore_into(&mupen, "mupen64plus_standalone", unit, &rc);
    mpk = root_find(&mupen, "Cruis'n USA (U) [!]-ABCDEF01.mpk");
    if (rc != SIGIL_OK || !mpk || mpk->data[N64_PAK_SIZE + 0x20] != 0x9C ||
        memcmp(mpk->data + 2 * N64_PAK_SIZE, formatted, N64_PAK_SIZE) != 0) {
        fail("restore mupen64plus over a .mpk", "unused pak kept, another save's pak formatted");
    }
    sigil_sync_result_free(w);
    free(paks);

    /* Project64: a .mpk per controller. */
    mem_root pj64 = {0};
    root_put(&pj64, "Save/GAME-" MD5_N64 "/GAME.eep", blank, 0);
    w = restore_into(&pj64, "project64", unit, &rc);
    mpk = root_find(&pj64, "Save/GAME-" MD5_N64 "/GAME_Cont_1.mpk");
    if (rc != SIGIL_OK || !mpk || mpk->len != N64_PAK_SIZE || memcmp(mpk->data, blob + 0x800, N64_PAK_SIZE) != 0 ||
        root_find(&pj64, "Save/GAME-" MD5_N64 "/GAME_Cont_2.mpk")) {
        fail("restore project64", "pak 1 as GAME_Cont_1.mpk, no others");
    }
    sigil_sync_result_free(w);

    /* With no file of the game there, mupen64plus's name can't be spelled. */
    mem_root none = {0};
    w = restore_into(&none, "mupen64plus_standalone", unit, &rc);
    if (rc != SIGIL_ERR_NO_TARGET || !w || strcmp(w->problem, "eeprom") != 0 || none.writes) {
        fail("restore mupen64plus, no files", "SIGIL_ERR_NO_TARGET naming eeprom, nothing written");
    }
    sigil_sync_result_free(w);

    root_free(&none);
    root_free(&pj64);
    root_free(&mupen);
    root_free(&empty);
    sigil_sync_result_free(unit);
    root_free(&srm);
    free(blob);
}

/* Flash keeps its bytes through bus order and back; Argosy's raw .srm
 * uploads restore as the unit does. The corpus has no flash save, so this
 * writes one into a blank .srm. */
static void check_flash_and_raw(void) {
    size_t len = 0;
    uint8_t *blob = sample("majora-builtin", &len);
    if (!blob) return;
    for (size_t i = 0; i < 0x18000; i++) blob[0x28800 + i] = (uint8_t)(i * 7 + (i >> 8));
    mem_root srm = {0};
    root_put(&srm, "Game (USA).srm", blob, len);
    sigil_sync_result *unit = collect_from(&srm, "mupen64plus_next", "GAME");
    mem_root from_unit = {0}, from_raw = {0};
    uint8_t blank[N64_EEPROM_MAX];
    memset(blank, 0xFF, sizeof(blank));
    root_put(&from_unit, "GAME-ABCDEF01.eep", blank, sizeof(blank));
    root_put(&from_raw, "GAME-ABCDEF01.eep", blank, sizeof(blank));
    int rc = 0, rc_raw = 0;
    sigil_sync_result *a = restore_into(&from_unit, "mupen64plus_standalone", unit, &rc);
    sigil_sync_result raw = {0};
    raw.data = blob;
    raw.len = len;
    sigil_sync_result *b = restore_into(&from_raw, "mupen64plus_standalone", &raw, &rc_raw);
    mem_file *fla = root_find(&from_unit, "GAME-ABCDEF01.fla");
    if (rc != SIGIL_OK || !fla || fla->len != N64_FLASH_SIZE || memcmp(fla->data, blob + 0x28800, N64_FLASH_SIZE) != 0) {
        fail("flash", "the .fla isn't the flash as the .srm stored it");
    }
    if (root_find(&from_unit, "GAME-ABCDEF01.eep")) fail("flash", "an .eep the game doesn't use stays");
    if (rc_raw != SIGIL_OK || !roots_same(&from_raw, &from_unit)) fail("argosy .srm upload", "restores other files than the unit");
    if (!a || !b || strcmp(a->identity_hash, b->identity_hash) != 0) fail("argosy .srm upload", "identity differs");
    sigil_sync_result_free(a);
    sigil_sync_result_free(b);
    root_free(&from_raw);
    root_free(&from_unit);
    sigil_sync_result_free(unit);
    root_free(&srm);
    free(blob);
}

/* Removing a file needs `remove`; without it restore writes nothing. */
static void check_needs_remove(void) {
    size_t len = 0;
    uint8_t *blob = sample("ssb-mupen64plus-next", &len);
    if (!blob) return;
    mem_root srm = {0};
    root_put(&srm, "Game (USA).srm", blob, len);
    sigil_sync_result *unit = collect_from(&srm, "mupen64plus_next", "GAME");
    mem_root root = {0};
    uint8_t eep[N64_EEPROM_MAX];
    memset(eep, 0x22, sizeof(eep));
    root_put(&root, "GAME-ABCDEF01.eep", eep, sizeof(eep));
    game g;
    make_game(&g, &root, "mupen64plus_standalone", "GAME", "NXXE");
    g.req.overwrite_local = 1;
    g.req.remove = NULL;
    sigil_sync_result *w = NULL;
    if (sigil_restore(&g.req, unit->data, unit->len, &w) != SIGIL_ERR_INVALID_ARG || root.writes) {
        fail("needs remove", "restore without remove wrote, or didn't refuse");
    }
    sigil_sync_result_free(w);
    root_free(&root);
    sigil_sync_result_free(unit);
    root_free(&srm);
    free(blob);
}

/* A save changed since the last sync stops the restore until the user agrees. */
static void check_conflict(void) {
    size_t len = 0, older_len = 0;
    uint8_t *blob = sample("ssb-mupen64plus-next", &len);
    uint8_t *older = sample("ssb-mupen64plus-next-older", &older_len);
    if (!blob || !older) {
        free(blob);
        free(older);
        return;
    }
    mem_root root = {0};
    root_put(&root, "Game (USA).srm", older, older_len);
    game g;
    make_game(&g, &root, "mupen64plus_next", "GAME", "NXXE");
    sigil_sync_result *first = NULL, *w = NULL;
    sigil_collect(&g.req, &first);
    mem_root other = {0};
    root_put(&other, "Game (USA).srm", blob, len);
    sigil_sync_result *newer = collect_from(&other, "mupen64plus_next", "GAME");

    g.req.state = first->state;
    g.req.state_len = first->state_len;
    uint8_t *changed = (uint8_t *)malloc(older_len);
    memcpy(changed, older, older_len);
    changed[0x20800 + 0x40] ^= 0x5A;
    root_put(&root, "Game (USA).srm", changed, older_len);   /* played on since the last sync */
    refresh(&g, &root);
    int writes = root.writes;
    if (sigil_restore(&g.req, newer->data, newer->len, &w) != SIGIL_ERR_CONFLICT || !w || !w->conflict ||
        root.writes != writes) {
        fail("conflict", "a local change was overwritten without asking");
    }
    sigil_sync_result_free(w);
    w = NULL;
    g.req.overwrite_local = 1;
    if (sigil_restore(&g.req, newer->data, newer->len, &w) != SIGIL_OK) fail("conflict", "overwrite_local didn't restore");
    sigil_sync_result_free(w);
    free(changed);
    sigil_sync_result_free(newer);
    sigil_sync_result_free(first);
    root_free(&other);
    root_free(&root);
    free(blob);
    free(older);
}

/* parallel_n64 keeps a loaded 64DD disk after the .srm's 0x48800 bytes: restore leaves it. */
static void check_disk_tail(void) {
    size_t len = 0;
    uint8_t *blob = sample("sm64-retroarch", &len);
    if (!blob) return;
    mem_root src = {0};
    root_put(&src, "Game (USA).srm", blob, len);
    sigil_sync_result *unit = collect_from(&src, "parallel_n64", "GAME");
    uint8_t *with_disk = (uint8_t *)malloc(N64_BLOB_SIZE + 0x100);
    memset(with_disk, 0xFF, N64_BLOB_SIZE);
    memset(with_disk + N64_BLOB_SIZE, 0x3C, 0x100);
    mem_root root = {0};
    root_put(&root, "Game (USA).srm", with_disk, N64_BLOB_SIZE + 0x100);
    int rc = 0;
    sigil_sync_result *w = restore_into(&root, "parallel_n64", unit, &rc);
    mem_file *f = root_find(&root, "Game (USA).srm");
    if (rc != SIGIL_OK || !f || f->len != N64_BLOB_SIZE + 0x100 || f->data[N64_BLOB_SIZE] != 0x3C ||
        memcmp(f->data, blob, N64_EEPROM_4K) != 0) {
        fail("64DD disk", "the bytes after the .srm's save regions changed");
    }
    sigil_sync_result_free(w);
    root_free(&root);
    free(with_disk);
    sigil_sync_result_free(unit);
    root_free(&src);
    free(blob);
}

int main(void) {
    char path[1024];
    if (corpus_platform_path("n64", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_manifest) != 0) {
        fprintf(stderr, "SKIP: no n64 manifest\n");
        return TEST_SKIP;
    }
    if (!corpus_present(&g_manifest, "n64", "cruisn-usa-mupen64plus-next")) {
        fprintf(stderr, "SKIP: n64 samples missing\n");
        corpus_free(&g_manifest);
        return TEST_SKIP;
    }
    check_regions();
    check_every_form();
    check_restore_forms();
    check_flash_and_raw();
    check_needs_remove();
    check_conflict();
    check_disk_tail();
    corpus_free(&g_manifest);
    printf("n64 sync: %d failures\n", g_fails);
    return corpus_exit(g_fails);
}
