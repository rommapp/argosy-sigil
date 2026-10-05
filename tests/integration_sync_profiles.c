// SPDX-License-Identifier: MPL-2.0
/* Folder saves kept per user profile: the yuzu forks, Cemu, Vita3K and RPCS3,
 * against the saves and profile lists pulled from an AYN Odin 3. */
#include "save_corpus.h"
#include "mem_root.h"
#include "sigil_internal.h"
#include <stdbool.h>

#define TEST_SKIP    77
#define EDEN_USER    "125D2DBAEBDEB11000296E1E1ECBF401"
#define CITRON_USER  "735DA01FA7EAE565C8FAA61E710195E5"
#define ZERO_USER    "00000000000000000000000000000000"
#define STRAY_USER   "1000296E1E1ECBF40100000000000000"
#define SAVE_ROOT    "nand/user/save/0000000000000000/"
#define EDEN_SAVES   SAVE_ROOT EDEN_USER "/"
#define DEVICE_SAVES SAVE_ROOT ZERO_USER "/"
#define PROFILES_DAT "nand/system/save/8000000000000010/su/avators/profiles.dat"
#define ANDROID_EDEN "Android/data/dev.eden.eden_emulator/files/"
#define BOTW         "01007EF00011E000"
#define ACNH         "01006F8002326000"
#define MK8D         "0100152000022000"
#define SSBU         "01006A800016E000"
#define CEMU_SAVES   "mlc01/usr/save/00050000/"
#define CEMU_ACT     "mlc01/usr/save/system/act/"
#define WIIU_BOTW    "101c9400"
#define WIIU_LAND    "10102000"

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static void expect_rc(const char *where, int got, int want) {
    if (got == want) return;
    char what[96];
    snprintf(what, sizeof(what), "rc %d, want %d", got, want);
    fail(where, what);
}

static corpus_table g_switch, g_wiiu, g_vita;

/* Puts every file of sample `id` into `root` under `prefix`, a path starting
 * with `strip` losing it first. Returns how many it put. */
static size_t put_sample(mem_root *root, const corpus_table *t, const char *platform, const char *id,
                         const char *strip, const char *prefix) {
    size_t put = 0;
    for (size_t r = 0; r < t->nrows; r++) {
        if (strcmp(corpus_get(t, r, "id"), id) != 0) continue;
        const char *path = corpus_get(t, r, "path");
        if (strip && strncmp(path, strip, strlen(strip)) != 0) continue;
        size_t len = 0;
        uint8_t *data = corpus_sample(t, platform, id, path, &len);
        if (!data) continue;
        char full[SIGIL_SAVE_PATH_MAX];
        snprintf(full, sizeof(full), "%s%s", prefix, path + (strip ? strlen(strip) : 0));
        root_put(root, full, data, len);
        free(data);
        put++;
    }
    return put;
}

static void put_text(mem_root *root, const char *path, const char *text) {
    root_put(root, path, (const uint8_t *)text, strlen(text));
}

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    mem_root          *root;
} game;

static void make_game(game *g, mem_root *root, const char *layout, const char *platform, const char *save_id) {
    memset(g, 0, sizeof(*g));
    g->root = root;
    g->result.struct_version = SIGIL_RESULT_V3;
    g->result.platform = sigil_platform_from_slug(platform);
    snprintf(g->result.title_id, sizeof(g->result.title_id), "%s", save_id);
    snprintf(g->result.save_id, sizeof(g->result.save_id), "%s", save_id);
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = platform;
    g->req.save.content_path = "game.bin";
    g->req.save.result = &g->result;
    g->req.save.listing = root->listing;
    g->req.save.open = root_open;
    g->req.save.open_ctx = root;
    g->req.write = root_write;
    g->req.remove = root_remove;
    g->req.write_ctx = root;
}

static int collect(game *g, sigil_sync_result **out) {
    g->req.save.listing_count = g->root->count;
    *out = NULL;
    return sigil_collect(&g->req, out);
}

static int restore(game *g, const sigil_sync_result *unit, sigil_sync_result **out) {
    g->req.save.listing_count = g->root->count;
    *out = NULL;
    return sigil_restore(&g->req, unit->data, unit->len, out);
}

static void keep_state(game *g, const sigil_sync_result *r) {
    g->req.state = r->state;
    g->req.state_len = r->state_len;
}

/* The unit's member names, or 0 when it isn't a zip. */
static size_t unit_names(const sigil_sync_result *r, char (*names)[SIGIL_SAVE_ENTRY_MAX], size_t cap) {
    sigil_zip_member *members = NULL;
    size_t count = 0;
    if (!r || !r->data || sigil_zip_read_mem(r->data, r->len, 1u << 30, &members, &count) != SIGIL_OK) return 0;
    for (size_t i = 0; i < count && i < cap; i++) snprintf(names[i], SIGIL_SAVE_ENTRY_MAX, "%s", members[i].name);
    sigil_zip_members_free(members, count);
    return count < cap ? count : cap;
}

static bool unit_has(const sigil_sync_result *r, const char *name) {
    char names[160][SIGIL_SAVE_ENTRY_MAX];
    size_t n = unit_names(r, names, 160);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(names[i], name) == 0) return true;
    }
    return false;
}

/* Every member of the unit starts with `prefix` and none holds `banned`. */
static bool unit_all(const sigil_sync_result *r, const char *prefix, const char *banned, size_t want) {
    char names[160][SIGIL_SAVE_ENTRY_MAX];
    size_t n = unit_names(r, names, 160);
    if (n != want) return false;
    for (size_t i = 0; i < n; i++) {
        if (strncmp(names[i], prefix, strlen(prefix)) != 0) return false;
        if (banned && strstr(names[i], banned)) return false;
    }
    return true;
}

/* A zip of `count` name and text pairs. */
static sigil_sync_result *unit_of(const char *const *pairs, size_t count) {
    sigil_zip_member members[16];
    for (size_t i = 0; i < count; i++) {
        snprintf(members[i].name, sizeof(members[i].name), "%s", pairs[2 * i]);
        members[i].data = (uint8_t *)pairs[2 * i + 1];
        members[i].len = strlen(pairs[2 * i + 1]);
    }
    sigil_sync_result *r = (sigil_sync_result *)calloc(1, sizeof(*r));
    sigil_zip_store(members, count, &r->data, &r->len);
    return r;
}

static void unit_of_free(sigil_sync_result *r) {
    if (r) free(r->data);
    free(r);
}

static bool root_holds(mem_root *root, const char *path, const char *text) {
    mem_file *f = root_find(root, path);
    return f && f->len == strlen(text) && memcmp(f->data, text, f->len) == 0;
}

static bool same_file(mem_root *a, const char *path_a, mem_root *b, const char *path_b) {
    mem_file *x = root_find(a, path_a), *y = root_find(b, path_b);
    return x && y && x->len == y->len && memcmp(x->data, y->data, x->len) == 0;
}

/* The Eden profile list, the Eden user's Breath of the Wild save, the device
 * save of Animal Crossing and the stray folder Eden keeps, under `prefix`. */
static size_t eden_root(mem_root *root, const char *prefix) {
    char at[SIGIL_SAVE_PATH_MAX];
    snprintf(at, sizeof(at), "%s%s", prefix, "nand/system/save/8000000000000010/su/avators/");
    put_sample(root, &g_switch, "switch", "eden-profile", NULL, at);
    snprintf(at, sizeof(at), "%s%s", prefix, EDEN_SAVES);
    size_t botw = put_sample(root, &g_switch, "switch", "botw-eden", NULL, at);
    snprintf(at, sizeof(at), "%s%s", prefix, DEVICE_SAVES);
    put_sample(root, &g_switch, "switch", "acnh-eden-jksv-meta", NULL, at);
    snprintf(at, sizeof(at), "%s%s%s/%s/.yuzu_save_size", prefix, SAVE_ROOT, STRAY_USER, BOTW);
    root_put(root, at, (const uint8_t *)"0123456789abcdef", 16);
    snprintf(at, sizeof(at), "%s%s%s/%s/option.sav", prefix, SAVE_ROOT, STRAY_USER, BOTW);
    put_text(root, at, "a folder the profile list doesn't name");
    return botw;
}

static size_t sample_count(const corpus_table *t, const char *id) {
    size_t n = 0;
    for (size_t r = 0; r < t->nrows; r++) n += strcmp(corpus_get(t, r, "id"), id) == 0;
    return n;
}

/* ---- yuzu forks ------------------------------------------------------------------ */

static void check_eden_collect(sigil_sync_result **botw_unit, sigil_sync_result **acnh_unit) {
    mem_root root = {0};
    size_t botw = eden_root(&root, "");
    game g;
    make_game(&g, &root, "eden", "switch", BOTW);
    sigil_sync_result *r = NULL;
    expect_rc("eden collect", collect(&g, &r), SIGIL_OK);
    if (!r || strcmp(r->profile, EDEN_USER) != 0) fail("eden collect", "profile is not Eden's");
    if (!r || r->profile_count != 1 || strcmp(r->profiles[0].name, "Eden") != 0) fail("eden collect", "profiles");
    if (!r || strcmp(r->artifact, BOTW ".zip") != 0 || r->shape != SIGIL_SAVE_SHAPE_FOLDER) {
        fail("eden collect", "artifact or shape");
    }
    if (!unit_all(r, BOTW "/", EDEN_USER, botw)) fail("eden collect", "members carry the profile or miss files");
    if (!unit_has(r, BOTW "/option.sav")) fail("eden collect", "option.sav missing");
    *botw_unit = r;

    make_game(&g, &root, "eden", "switch", ACNH);
    r = NULL;
    expect_rc("eden device collect", collect(&g, &r), SIGIL_OK);
    if (!unit_all(r, "device/" ACNH "/", NULL, sample_count(&g_switch, "acnh-eden-jksv-meta"))) {
        fail("eden device collect", "device save not under device/");
    }
    *acnh_unit = r;
    root_free(&root);
}

static void check_sidecar(void) {
    mem_root root = {0};
    put_sample(&root, &g_switch, "switch", "eden-profile", NULL, "nand/system/save/8000000000000010/su/avators/");
    size_t n = put_sample(&root, &g_switch, "switch", "ssbu-eden-sidecar", NULL, EDEN_SAVES);
    game g;
    make_game(&g, &root, "eden", "switch", SSBU);
    sigil_sync_result *r = NULL;
    expect_rc("sidecar collect", collect(&g, &r), SIGIL_OK);
    if (!unit_all(r, SSBU "/", NULL, n - 1) || unit_has(r, SSBU "/.yuzu_save_size")) {
        fail("sidecar collect", "size file travelled in the unit");
    }
    mem_root other = {0};
    put_sample(&other, &g_switch, "switch", "citron-profile", NULL, "nand/system/save/8000000000000010/su/avators/");
    root_put(&other, SAVE_ROOT CITRON_USER "/" SSBU "/.citron_save_size", (const uint8_t *)"citron-own-size", 15);
    game c;
    make_game(&c, &other, "citron", "switch", SSBU);
    c.req.overwrite_local = 1;
    sigil_sync_result *w = NULL;
    expect_rc("sidecar restore", restore(&c, r, &w), SIGIL_OK);
    if (!root_holds(&other, SAVE_ROOT CITRON_USER "/" SSBU "/.citron_save_size", "citron-own-size")) {
        fail("sidecar restore", "the target's own size file went");
    }
    if (root_find(&other, SAVE_ROOT CITRON_USER "/" SSBU "/.yuzu_save_size")) fail("sidecar restore", "size file written");
    sigil_sync_result_free(w);

    /* A unit that carries a size file, as one zipped by hand may: restore
     * leaves the emulator's own alone and reads back as what it wrote. */
    const char *const carried[] = { SSBU "/.yuzu_save_size", "the source's size", SSBU "/save.bin", "save" };
    sigil_sync_result *by_hand = unit_of(carried, 2);
    mem_root mine = {0};
    put_sample(&mine, &g_switch, "switch", "eden-profile", NULL, "nand/system/save/8000000000000010/su/avators/");
    put_text(&mine, EDEN_SAVES SSBU "/.yuzu_save_size", "own size");
    game e;
    make_game(&e, &mine, "eden", "switch", SSBU);
    w = NULL;
    expect_rc("carried size file", restore(&e, by_hand, &w), SIGIL_OK);
    if (!root_holds(&mine, EDEN_SAVES SSBU "/.yuzu_save_size", "own size")) fail("carried size file", "overwritten");
    if (mine.writes != 1) fail("carried size file", "write count");
    keep_state(&e, w);
    sigil_sync_result *back = NULL;
    expect_rc("carried size file collect", collect(&e, &back), SIGIL_OK);
    if (!back || !w || back->changed || strcmp(back->identity_hash, w->identity_hash) != 0) {
        fail("carried size file collect", "reads back changed");
    }
    sigil_sync_result_free(back);
    sigil_sync_result_free(w);
    unit_of_free(by_hand);
    root_free(&mine);
    sigil_sync_result_free(r);
    root_free(&other);
    root_free(&root);
}

static void check_cross_fork(const sigil_sync_result *botw) {
    mem_root root = {0};
    put_sample(&root, &g_switch, "switch", "citron-profile", NULL, "nand/system/save/8000000000000010/su/avators/");
    game g;
    make_game(&g, &root, "citron", "switch", BOTW);
    sigil_sync_result *r = NULL;
    expect_rc("cross fork restore", restore(&g, botw, &r), SIGIL_OK);
    if (!r || strcmp(r->profile, CITRON_USER) != 0) fail("cross fork restore", "profile is not citron's");
    if (!root_find(&root, SAVE_ROOT CITRON_USER "/" BOTW "/option.sav")) fail("cross fork restore", "option.sav");
    if (root.writes != (int)sample_count(&g_switch, "botw-eden")) fail("cross fork restore", "write count");
    if (r && strcmp(r->identity_hash, botw->identity_hash) != 0) fail("cross fork restore", "identity differs");
    keep_state(&g, r);
    sigil_sync_result *again = NULL;
    expect_rc("cross fork collect", collect(&g, &again), SIGIL_OK);
    if (!again || again->changed || strcmp(again->identity_hash, botw->identity_hash) != 0) {
        fail("cross fork collect", "the restored save reads back changed");
    }
    sigil_sync_result_free(again);
    sigil_sync_result_free(r);
    root_free(&root);
}

static void check_split(void) {
    mem_root root = {0};
    put_sample(&root, &g_switch, "switch", "eden-profile", NULL, "nand/system/save/8000000000000010/su/avators/");
    size_t user = put_sample(&root, &g_switch, "switch", "mk8d-eden-split", "user/", EDEN_SAVES);
    size_t device = put_sample(&root, &g_switch, "switch", "mk8d-eden-split", "device/", DEVICE_SAVES);
    game g;
    make_game(&g, &root, "eden", "switch", MK8D);
    sigil_sync_result *r = NULL;
    expect_rc("split collect", collect(&g, &r), SIGIL_OK);
    char names[16][SIGIL_SAVE_ENTRY_MAX];
    if (unit_names(r, names, 16) != user + device) fail("split collect", "member count");
    if (!unit_has(r, MK8D "/ghostlist.dat") || !unit_has(r, "device/" MK8D "/sg33.dat")) fail("split collect", "areas");

    mem_root other = {0};
    put_sample(&other, &g_switch, "switch", "citron-profile", NULL, "nand/system/save/8000000000000010/su/avators/");
    game c;
    make_game(&c, &other, "citron", "switch", MK8D);
    sigil_sync_result *w = NULL;
    expect_rc("split restore", restore(&c, r, &w), SIGIL_OK);
    if (!same_file(&root, DEVICE_SAVES MK8D "/sg33.dat", &other, DEVICE_SAVES MK8D "/sg33.dat")) {
        fail("split restore", "device save not in the device folder");
    }
    if (!same_file(&root, EDEN_SAVES MK8D "/userdata.dat", &other, SAVE_ROOT CITRON_USER "/" MK8D "/userdata.dat")) {
        fail("split restore", "account save not under the profile");
    }
    sigil_sync_result_free(w);

    /* A unit with only the account part leaves the device save alone. */
    mem_root account_only = {0};
    put_sample(&account_only, &g_switch, "switch", "eden-profile", NULL, "nand/system/save/8000000000000010/su/avators/");
    put_sample(&account_only, &g_switch, "switch", "mk8d-eden-split", "user/", EDEN_SAVES);
    put_text(&account_only, EDEN_SAVES MK8D "/userdata.dat", "played on another device");
    game a;
    make_game(&a, &account_only, "eden", "switch", MK8D);
    sigil_sync_result *part = NULL;
    expect_rc("account part collect", collect(&a, &part), SIGIL_OK);
    keep_state(&g, r);
    sigil_sync_result *back = NULL;
    expect_rc("account part restore", restore(&g, part, &back), SIGIL_OK);
    if (!root_holds(&root, EDEN_SAVES MK8D "/userdata.dat", "played on another device")) {
        fail("account part restore", "account part not written");
    }
    if (!root_find(&root, DEVICE_SAVES MK8D "/sg33.dat")) fail("account part restore", "device save removed");
    sigil_sync_result_free(back);
    sigil_sync_result_free(part);
    sigil_sync_result_free(r);
    root_free(&account_only);
    root_free(&other);
    root_free(&root);
}

static void check_root_too_high(const sigil_sync_result *botw) {
    mem_root root = {0};
    eden_root(&root, ANDROID_EDEN);
    game g;
    make_game(&g, &root, "eden", "switch", BOTW);
    sigil_sync_result *r = NULL;
    expect_rc("high collect", collect(&g, &r), SIGIL_OK);
    if (!r || strcmp(r->identity_hash, botw->identity_hash) != 0) fail("high collect", "not the save collected at base");
    sigil_sync_result_free(r);

    mem_root fresh = {0};
    put_sample(&fresh, &g_switch, "switch", "citron-profile", NULL,
               "Android/data/org.citron.emu/files/nand/system/save/8000000000000010/su/avators/");
    game c;
    make_game(&c, &fresh, "citron", "switch", BOTW);
    r = NULL;
    expect_rc("high restore", restore(&c, botw, &r), SIGIL_OK);
    if (!root_find(&fresh, "Android/data/org.citron.emu/files/" SAVE_ROOT CITRON_USER "/" BOTW "/option.sav")) {
        fail("high restore", "not written under the base the listing shows");
    }
    sigil_sync_result_free(r);

    put_text(&fresh, "Android/data/org.other.emu/files/" SAVE_ROOT CITRON_USER "/" BOTW "/x.sav", "x");
    r = NULL;
    expect_rc("two bases", collect(&c, &r), SIGIL_ERR_AMBIGUOUS);
    if (!r || !strstr(r->problem, "Android/data/org.citron.emu/files/") ||
        !strstr(r->problem, "Android/data/org.other.emu/files/")) {
        fail("two bases", "problem does not name both");
    }
    sigil_sync_result_free(r);
    root_free(&fresh);
    root_free(&root);
}

static void check_root_too_low(const sigil_sync_result *botw, const sigil_sync_result *acnh) {
    mem_root root = {0};
    put_sample(&root, &g_switch, "switch", "botw-eden", NULL, "");
    game g;
    make_game(&g, &root, "eden", "switch", BOTW);
    g.req.save.root_path = "/storage/emulated/0/" ANDROID_EDEN SAVE_ROOT EDEN_USER;
    sigil_sync_result *r = NULL;
    expect_rc("low collect", collect(&g, &r), SIGIL_OK);
    if (!r || strcmp(r->profile, EDEN_USER) != 0) fail("low collect", "profile not taken from the root path");
    if (!r || strcmp(r->identity_hash, botw->identity_hash) != 0) fail("low collect", "not the save collected at base");
    sigil_sync_result_free(r);

    r = NULL;
    expect_rc("low device restore", restore(&g, acnh, &r), SIGIL_ERR_NO_TARGET);
    if (!r || strncmp(r->problem, "device/" ACNH "/", strlen("device/" ACNH "/")) != 0) {
        fail("low device restore", "problem does not name the device member");
    }
    if (root.writes) fail("low device restore", "wrote");
    sigil_sync_result_free(r);

    game d;
    make_game(&d, &root, "eden", "switch", ACNH);
    d.req.save.root_path = g.req.save.root_path;
    r = NULL;
    expect_rc("low device collect", collect(&d, &r), SIGIL_OK);
    if (!r || r->data) fail("low device collect", "found a device save out of reach");
    sigil_sync_result_free(r);

    g.req.save.root_path = "C:\\Users\\me\\AppData\\Roaming\\eden\\nand\\user\\save\\0000000000000000\\" EDEN_USER "\\";
    r = NULL;
    expect_rc("windows root", collect(&g, &r), SIGIL_OK);
    if (!r || strcmp(r->identity_hash, botw->identity_hash) != 0) fail("windows root", "not the same save");
    sigil_sync_result_free(r);

    /* Above the profiles, with the profile list out of reach: the profile
     * folders holding files stand for the profiles. */
    mem_root mid = {0};
    put_sample(&mid, &g_switch, "switch", "botw-eden", NULL, EDEN_USER "/");
    root_put(&mid, STRAY_USER "/" BOTW "/.yuzu_save_size", (const uint8_t *)"0123456789abcdef", 16);
    game m;
    make_game(&m, &mid, "eden", "switch", BOTW);
    m.req.save.root_path = "/data/eden/nand/user/save/0000000000000000";
    r = NULL;
    expect_rc("mid collect", collect(&m, &r), SIGIL_OK);
    if (!r || strcmp(r->profile, EDEN_USER) != 0 || r->profile_count != 1) fail("mid collect", "folder profile");
    if (!r || strcmp(r->identity_hash, botw->identity_hash) != 0) fail("mid collect", "not the same save");
    sigil_sync_result_free(r);
    root_free(&mid);

    g.req.save.root_path = "/data/eden/nand/user/save/0000000000000000/" EDEN_USER "/" BOTW "/0";
    r = NULL;
    expect_rc("inside save folder", collect(&g, &r), SIGIL_ERR_INVALID_ARG);
    sigil_sync_result_free(r);
    root_free(&root);
}

/* Eden's profile list with the Citron profile added as a second user. */
static void two_profiles(mem_root *root) {
    size_t eden_len = 0, citron_len = 0;
    uint8_t *eden = corpus_sample(&g_switch, "switch", "eden-profile", "profiles.dat", &eden_len);
    uint8_t *citron = corpus_sample(&g_switch, "switch", "citron-profile", "profiles.dat", &citron_len);
    if (eden && citron && eden_len >= 0x10 + 2 * 0xC8 && citron_len >= 0x10 + 0xC8) {
        memcpy(eden + 0x10 + 0xC8, citron + 0x10, 0xC8);
        root_put(root, PROFILES_DAT, eden, eden_len);
    }
    free(eden);
    free(citron);
}

static void check_two_profiles(const sigil_sync_result *botw, const sigil_sync_result *acnh) {
    mem_root root = {0};
    two_profiles(&root);
    put_sample(&root, &g_switch, "switch", "botw-eden", NULL, EDEN_SAVES);
    game g;
    make_game(&g, &root, "eden", "switch", BOTW);
    sigil_sync_result *r = NULL;
    expect_rc("two profiles collect", collect(&g, &r), SIGIL_ERR_AMBIGUOUS);
    if (!r || !strstr(r->problem, EDEN_USER " Eden") || !strstr(r->problem, CITRON_USER " citron")) {
        fail("two profiles collect", "problem does not list both profiles");
    }
    if (!r || r->profile_count != 2) fail("two profiles collect", "result does not list both");
    sigil_sync_result_free(r);

    g.req.save.profile = "125d2dbaebdeb11000296e1e1ecbf401";
    r = NULL;
    expect_rc("given profile", collect(&g, &r), SIGIL_OK);
    if (!r || strcmp(r->profile, EDEN_USER) != 0) fail("given profile", "not matched to the listed id");
    if (!r || strcmp(r->identity_hash, botw->identity_hash) != 0) fail("given profile", "not the same save");
    sigil_sync_result_free(r);

    game d;
    make_game(&d, &root, "eden", "switch", ACNH);
    r = NULL;
    expect_rc("two profiles device collect", collect(&d, &r), SIGIL_OK);
    sigil_sync_result_free(r);
    r = NULL;
    expect_rc("two profiles device restore", restore(&d, acnh, &r), SIGIL_OK);
    sigil_sync_result_free(r);

    game n;
    make_game(&n, &root, "eden", "switch", BOTW);
    n.req.overwrite_local = 1;
    int writes = root.writes;
    r = NULL;
    expect_rc("two profiles restore", restore(&n, botw, &r), SIGIL_ERR_AMBIGUOUS);
    if (root.writes != writes) fail("two profiles restore", "wrote");
    sigil_sync_result_free(r);

    n.req.save.profile = CITRON_USER;
    r = NULL;
    expect_rc("two profiles restore given", restore(&n, botw, &r), SIGIL_OK);
    if (!root_find(&root, SAVE_ROOT CITRON_USER "/" BOTW "/option.sav")) fail("two profiles restore given", "target");
    sigil_sync_result_free(r);

    n.req.save.profile = ZERO_USER;
    r = NULL;
    expect_rc("device folder as profile", restore(&n, botw, &r), SIGIL_ERR_INVALID_ARG);
    sigil_sync_result_free(r);

    sigil_save_unit *unit = NULL;
    g.req.save.profile = NULL;
    g.req.save.listing_count = root.count;
    expect_rc("two profiles resolve", sigil_save_resolve(&g.req.save, &unit), SIGIL_ERR_AMBIGUOUS);
    sigil_save_unit_free(unit);

    const char *const ids[] = { ACNH };
    sigil_sync_companion companion = { ids, 1, NULL, 0 };
    g.req.save.profile = EDEN_USER;
    g.req.companions = &companion;
    g.req.companion_count = 1;
    r = NULL;
    expect_rc("companion on a profile layout", collect(&g, &r), SIGIL_ERR_INVALID_ARG);
    sigil_sync_result_free(r);
    root_free(&root);
}

static void check_conflicts(void) {
    mem_root root = {0};
    eden_root(&root, "");
    game g;
    make_game(&g, &root, "eden", "switch", BOTW);
    sigil_sync_result *first = NULL;
    expect_rc("conflict collect", collect(&g, &first), SIGIL_OK);
    keep_state(&g, first);

    sigil_sync_result *r = NULL;
    expect_rc("same unit", restore(&g, first, &r), SIGIL_OK);
    if (root.writes) fail("same unit", "wrote");
    sigil_sync_result_free(r);

    put_text(&root, EDEN_SAVES BOTW "/option.sav", "played since");
    root.writes = 0;
    r = NULL;
    expect_rc("local change", restore(&g, first, &r), SIGIL_ERR_CONFLICT);
    if (!r || !r->conflict) fail("local change", "conflict not flagged");
    if (root.writes) fail("local change", "wrote");
    sigil_sync_result_free(r);

    put_text(&root, EDEN_SAVES BOTW "/extra.sav", "new file");
    sigil_sync_result *second = NULL;
    expect_rc("second collect", collect(&g, &second), SIGIL_OK);
    keep_state(&g, second);
    g.req.remove = NULL;
    root.writes = 0;
    r = NULL;
    expect_rc("no remove", restore(&g, first, &r), SIGIL_ERR_INVALID_ARG);
    if (root.writes) fail("no remove", "wrote");
    sigil_sync_result_free(r);

    g.req.remove = root_remove;
    r = NULL;
    expect_rc("restore older", restore(&g, first, &r), SIGIL_OK);
    if (root_find(&root, EDEN_SAVES BOTW "/extra.sav")) fail("restore older", "extra file kept");
    if (root_holds(&root, EDEN_SAVES BOTW "/option.sav", "played since")) fail("restore older", "option.sav kept");
    if (!root_find(&root, DEVICE_SAVES ACNH "/main.dat")) fail("restore older", "another game's device save went");
    if (root.writes != 1) fail("restore older", "rewrote files that held the unit's bytes");
    sigil_sync_result_free(r);
    sigil_sync_result_free(second);
    sigil_sync_result_free(first);
    root_free(&root);
}

static void mk8d_root(mem_root *root, bool citron) {
    two_profiles(root);
    put_sample(root, &g_switch, "switch", "mk8d-eden-split", "user/", EDEN_SAVES);
    put_sample(root, &g_switch, "switch", "mk8d-eden-split", "device/", DEVICE_SAVES);
    if (citron) {
        put_sample(root, &g_switch, "switch", "mk8d-eden-split", "user/", SAVE_ROOT CITRON_USER "/");
        put_text(root, SAVE_ROOT CITRON_USER "/" MK8D "/userdata.dat", "citron played");
    }
}

static void make_mk8d(game *g, mem_root *root, const char *profile) {
    make_game(g, root, "eden", "switch", MK8D);
    g->req.save.profile = profile;
}

/* The device area's state belongs to the device and the account area's to
 * the profile, so syncing one profile doesn't stand in for another. */
static void check_area_state(void) {
    mem_root root = {0}, newer = {0};
    mk8d_root(&root, false);
    mk8d_root(&newer, false);
    put_text(&newer, DEVICE_SAVES MK8D "/sg33.dat", "device moved on");
    game e, n, c;
    make_mk8d(&e, &root, EDEN_USER);
    make_mk8d(&n, &newer, EDEN_USER);
    make_mk8d(&c, &root, CITRON_USER);
    sigil_sync_result *first = NULL, *moved = NULL, *r = NULL;
    expect_rc("device state collect", collect(&e, &first), SIGIL_OK);
    expect_rc("device state newer", collect(&n, &moved), SIGIL_OK);
    keep_state(&c, first);
    expect_rc("device synced by another profile", restore(&c, moved, &r), SIGIL_OK);
    if (!root_holds(&root, DEVICE_SAVES MK8D "/sg33.dat", "device moved on")) fail("device state", "device save");
    sigil_sync_result_free(r);
    sigil_sync_result_free(moved);
    sigil_sync_result_free(first);

    mem_root both = {0};
    mk8d_root(&both, true);
    game ce, ee;
    make_mk8d(&ce, &both, CITRON_USER);
    make_mk8d(&ee, &both, EDEN_USER);
    sigil_sync_result *citron = NULL, *eden = NULL;
    expect_rc("account state citron", collect(&ce, &citron), SIGIL_OK);
    keep_state(&ee, citron);
    expect_rc("account state eden", collect(&ee, &eden), SIGIL_OK);
    keep_state(&ce, eden);
    r = NULL;
    expect_rc("account synced by its own profile", restore(&ce, eden, &r), SIGIL_OK);
    sigil_sync_result_free(r);
    sigil_sync_result_free(eden);
    sigil_sync_result_free(citron);
    root_free(&both);
    root_free(&newer);
    root_free(&root);
}

/* ---- Cemu ------------------------------------------------------------------------ */

static void cemu_root(mem_root *root) {
    put_sample(root, &g_wiiu, "wiiu", "cemu-account", "act/", CEMU_ACT);
    put_sample(root, &g_wiiu, "wiiu", "botw-cemu", NULL, CEMU_SAVES);
    put_sample(root, &g_wiiu, "wiiu", "nintendo-land-cemu", NULL, CEMU_SAVES);
}

static void check_cemu(void) {
    mem_root root = {0};
    cemu_root(&root);
    game g;
    make_game(&g, &root, "cemu", "wiiu", WIIU_BOTW);
    sigil_sync_result *botw = NULL;
    expect_rc("cemu collect", collect(&g, &botw), SIGIL_OK);
    if (!botw || strcmp(botw->profile, "80000001") != 0 || botw->profile_count != 1 ||
        strcmp(botw->profiles[0].name, "default") != 0) {
        fail("cemu collect", "account or Mii name");
    }
    if (!unit_has(botw, WIIU_BOTW "/meta/meta.xml") || !unit_has(botw, WIIU_BOTW "/user/account/option.sav") ||
        !unit_all(botw, WIIU_BOTW "/", "80000001", sample_count(&g_wiiu, "botw-cemu"))) {
        fail("cemu collect", "unit names");
    }

    game l;
    make_game(&l, &root, "cemu", "wiiu", WIIU_LAND);
    sigil_sync_result *land = NULL;
    expect_rc("cemu common collect", collect(&l, &land), SIGIL_OK);
    if (!unit_has(land, WIIU_LAND "/user/common/save.dat")) fail("cemu common collect", "common save");

    /* An older unit names the account folder by its id: it restores as the
     * account part, under whatever account the target has. */
    mem_root target = {0};
    put_text(&target, CEMU_ACT "80000002/account.dat", "PersistentId=80000002\nMiiName=004d0069006b0075\n");
    sigil_zip_member members[16];
    size_t count = 0;
    for (size_t r = 0; r < g_wiiu.nrows && count < 16; r++) {
        if (strcmp(corpus_get(&g_wiiu, r, "id"), "botw-cemu") != 0) continue;
        const char *path = corpus_get(&g_wiiu, r, "path");
        snprintf(members[count].name, sizeof(members[count].name), "%s", path);
        members[count].data = corpus_sample(&g_wiiu, "wiiu", "botw-cemu", path, &members[count].len);
        count++;
    }
    sigil_sync_result old = {0};
    sigil_zip_store(members, count, &old.data, &old.len);
    for (size_t i = 0; i < count; i++) free(members[i].data);
    game t;
    make_game(&t, &target, "cemu", "wiiu", WIIU_BOTW);
    sigil_sync_result *r = NULL;
    expect_rc("old unit restore", restore(&t, &old, &r), SIGIL_OK);
    if (!r || strcmp(r->profile, "80000002") != 0 || r->profile_count != 1 || strcmp(r->profiles[0].name, "Miku") != 0) {
        fail("old unit restore", "target account");
    }
    if (!same_file(&root, CEMU_SAVES WIIU_BOTW "/user/80000001/option.sav", &target,
                   CEMU_SAVES WIIU_BOTW "/user/80000002/option.sav")) {
        fail("old unit restore", "account save not under the target account");
    }
    if (r && botw && strcmp(r->identity_hash, botw->identity_hash) != 0) fail("old unit restore", "identity");
    keep_state(&t, r);
    sigil_sync_result *back = NULL;
    expect_rc("old unit collect", collect(&t, &back), SIGIL_OK);
    if (!back || back->changed) fail("old unit collect", "the restored save reads back changed");
    sigil_sync_result_free(back);
    sigil_sync_result_free(r);
    free(old.data);

    /* A root at the account folder reaches the account part only; Cemu
     * writes meta/ again itself, so it is skipped rather than refused. */
    mem_root low = {0};
    game a;
    make_game(&a, &low, "cemu", "wiiu", WIIU_BOTW);
    a.req.save.root_path = "/sdcard/Android/data/info.cemu.cemu/files/mlc01/usr/save/00050000/" WIIU_BOTW "/user/80000001";
    r = NULL;
    expect_rc("cemu account root", restore(&a, botw, &r), SIGIL_OK);
    if (!root_find(&low, "option.sav") || root_find(&low, "meta/meta.xml")) fail("cemu account root", "placement");
    sigil_sync_result_free(r);

    mem_root low_land = {0};
    game b;
    make_game(&b, &low_land, "cemu", "wiiu", WIIU_LAND);
    b.req.save.root_path = "/x/mlc01/usr/save/00050000/" WIIU_LAND "/user/80000001/";
    r = NULL;
    expect_rc("cemu common out of reach", restore(&b, land, &r), SIGIL_ERR_NO_TARGET);
    if (!r || strcmp(r->problem, WIIU_LAND "/user/common/save.dat") != 0) fail("cemu common out of reach", "problem");
    sigil_sync_result_free(r);

    sigil_sync_result_free(land);
    sigil_sync_result_free(botw);
    root_free(&low_land);
    root_free(&low);
    root_free(&target);
    root_free(&root);
}

/* ---- Vita3K and RPCS3 ------------------------------------------------------------- */

static void check_vita3k(void) {
    mem_root root = {0};
    put_text(&root, "ux0/user/00/user.xml", "<?xml version=\"1.0\"?>\n<user id=\"00\" name=\"Kat &amp; Co\" avatar=\"default\">\n</user>\n");
    size_t n = put_sample(&root, &g_vita, "psvita", "isaac-rebirth-vita3k", NULL, "ux0/user/00/savedata/");
    put_text(&root, "ux0/user/00/savedata/PCSE00001/other.bin", "another game");
    put_text(&root, "ux0/user/00/savedata/PCSB00676-OTHER/other.bin", "a folder the id only starts");
    game g;
    make_game(&g, &root, "vita3k", "vita", "PCSB00676");
    sigil_sync_result *r = NULL;
    expect_rc("vita3k collect", collect(&g, &r), SIGIL_OK);
    if (!r || strcmp(r->profile, "00") != 0 || r->profile_count != 1 || strcmp(r->profiles[0].name, "Kat & Co") != 0) {
        fail("vita3k collect", "user");
    }
    if (!unit_all(r, "PCSB00676/", NULL, n) || !unit_has(r, "PCSB00676/SlotParam_0.bin")) fail("vita3k collect", "unit");
    sigil_sync_result_free(r);
    root_free(&root);
}

static void check_rpcs3(void) {
    mem_root root = {0};
    put_text(&root, "dev_hdd0/home/00000001/localusername", "User\n");
    put_text(&root, "dev_hdd0/home/00000001/savedata/BLUS30443-AUTOSAVE/PARAM.SFO", "sfo");
    put_text(&root, "dev_hdd0/home/00000001/savedata/BLUS30443-AUTOSAVE/DATA.BIN", "data");
    put_text(&root, "dev_hdd0/home/00000001/savedata/BLUS30443-SYS/SYS.BIN", "sys");
    put_text(&root, "dev_hdd0/home/00000001/savedata/BLUS99999/OTHER.BIN", "other");
    game g;
    make_game(&g, &root, "rpcs3", "ps3", "BLUS30443");
    sigil_sync_result *r = NULL;
    expect_rc("rpcs3 collect", collect(&g, &r), SIGIL_OK);
    if (!r || strcmp(r->profiles[0].name, "User") != 0) fail("rpcs3 collect", "user name");
    if (!unit_all(r, "BLUS30443-", NULL, 3)) fail("rpcs3 collect", "save folders by prefix");

    mem_root target = {0};
    put_text(&target, "dev_hdd0/home/00000002/localusername", "Second");
    game t;
    make_game(&t, &target, "rpcs3", "ps3", "BLUS30443");
    sigil_sync_result *w = NULL;
    expect_rc("rpcs3 restore", restore(&t, r, &w), SIGIL_OK);
    if (!root_holds(&target, "dev_hdd0/home/00000002/savedata/BLUS30443-SYS/SYS.BIN", "sys")) fail("rpcs3 restore", "");
    sigil_sync_result_free(w);

    const char *const foreign[] = { "BLUS99999/OTHER.BIN", "other" };
    sigil_sync_result *bad = unit_of(foreign, 1);
    w = NULL;
    expect_rc("rpcs3 foreign unit", restore(&t, bad, &w), SIGIL_ERR_NO_TARGET);
    if (!w || strcmp(w->problem, "BLUS99999/OTHER.BIN") != 0) fail("rpcs3 foreign unit", "problem");
    sigil_sync_result_free(w);
    unit_of_free(bad);

    const char *const escape[] = { "BLUS30443-A/fine.bin", "fine", "BLUS30443-X/../../../../escape.bin", "x" };
    bad = unit_of(escape, 2);
    t.req.overwrite_local = 1;
    int writes = target.writes;
    w = NULL;
    expect_rc("rpcs3 escaping member", restore(&t, bad, &w), SIGIL_ERR_IO);
    if (target.writes != writes) fail("rpcs3 escaping member", "wrote before refusing");
    sigil_sync_result_free(w);
    unit_of_free(bad);

    const char *const twice[] = { "BLUS30443-A/x.bin", "one", "BLUS30443-A/x.bin", "two" };
    bad = unit_of(twice, 2);
    w = NULL;
    expect_rc("rpcs3 member twice", restore(&t, bad, &w), SIGIL_ERR_UNSUPPORTED_FORMAT);
    if (target.writes != writes) fail("rpcs3 member twice", "wrote");
    sigil_sync_result_free(w);
    unit_of_free(bad);

    sigil_sync_result_free(r);
    root_free(&target);
    root_free(&root);
}

/* ---- PSP -------------------------------------------------------------------------- */

/* A PARAM.SFO holding one empty string entry per key. */
static size_t build_sfo(uint8_t *buf, size_t cap, const char *const *keys, size_t count) {
    size_t key_table = 20 + count * 16, key_len = 0;
    for (size_t i = 0; i < count; i++) key_len += strlen(keys[i]) + 1;
    size_t data_table = key_table + ((key_len + 3) & ~(size_t)3);
    size_t len = data_table + count * 4;
    if (len > cap) return 0;
    memset(buf, 0, len);
    memcpy(buf, "\0PSF", 4);
    sigil_write_le32(buf + 4, 0x00000101);
    sigil_write_le32(buf + 8, (uint32_t)key_table);
    sigil_write_le32(buf + 12, (uint32_t)data_table);
    sigil_write_le32(buf + 16, (uint32_t)count);
    size_t key_off = 0;
    for (size_t i = 0; i < count; i++) {
        uint8_t *e = buf + 20 + i * 16;
        e[0] = (uint8_t)key_off;
        e[1] = (uint8_t)(key_off >> 8);
        e[2] = 0x04;
        e[3] = 0x02;
        sigil_write_le32(e + 4, 1);
        sigil_write_le32(e + 8, 4);
        sigil_write_le32(e + 12, (uint32_t)(i * 4));
        memcpy(buf + key_table + key_off, keys[i], strlen(keys[i]) + 1);
        key_off += strlen(keys[i]) + 1;
    }
    return len;
}

static void put_sfo(mem_root *root, const char *path, const char *const *keys, size_t count) {
    uint8_t sfo[512];
    size_t len = build_sfo(sfo, sizeof(sfo), keys, count);
    if (len) root_put(root, path, sfo, len);
}

/* PPSSPP's savedata utility writes both keys into every save's PARAM.SFO;
 * its game-data installer writes neither (PSPGamedataInstallDialog.cpp). A
 * folder whose PARAM.SFO is missing or isn't an SFO stays with the saves. */
static void check_psp_game_data(void) {
    static const char *const save_keys[] = { "CATEGORY", "SAVEDATA_FILE_LIST", "SAVEDATA_PARAMS", "TITLE" };
    static const char *const one_key[] = { "CATEGORY", "SAVEDATA_PARAMS" };
    static const char *const install_keys[] = { "CATEGORY", "SAVEDATA_DIRECTORY", "TITLE" };
    mem_root root = {0};
    put_sfo(&root, "PSP/SAVEDATA/ULUS10064DATA00/PARAM.SFO", save_keys, 4);
    put_text(&root, "PSP/SAVEDATA/ULUS10064DATA00/DATA.BIN", "save");
    put_sfo(&root, "PSP/SAVEDATA/ULUS10064SYS/PARAM.SFO", one_key, 2);
    put_sfo(&root, "PSP/SAVEDATA/ULUS10064INSTALL/PARAM.SFO", install_keys, 3);
    put_text(&root, "PSP/SAVEDATA/ULUS10064INSTALL/DISC.DAT", "installed disc data");
    put_text(&root, "PSP/SAVEDATA/ULUS10064NOSFO/DATA.BIN", "no sfo");
    put_text(&root, "PSP/SAVEDATA/ULUS10064TEXT/PARAM.SFO", "SAVEDATA_PARAMS is only text here");
    game g;
    make_game(&g, &root, "ppsspp", "psp", "ULUS10064");
    sigil_sync_result *r = NULL;
    expect_rc("psp game data collect", collect(&g, &r), SIGIL_OK);
    if (!unit_all(r, "ULUS10064", "INSTALL", 5) || !unit_has(r, "ULUS10064NOSFO/DATA.BIN") ||
        !unit_has(r, "ULUS10064TEXT/PARAM.SFO") || !unit_has(r, "ULUS10064SYS/PARAM.SFO")) {
        fail("psp game data collect", "the install folder travels, or a save folder doesn't");
    }
    keep_state(&g, r);

    const char *const unit[] = { "ULUS10064DATA00/DATA.BIN", "played" };
    sigil_sync_result *only_data = unit_of(unit, 1);
    sigil_sync_result *w = NULL;
    g.req.overwrite_local = 1;
    expect_rc("psp game data restore", restore(&g, only_data, &w), SIGIL_OK);
    if (!root_holds(&root, "PSP/SAVEDATA/ULUS10064INSTALL/DISC.DAT", "installed disc data")) {
        fail("psp game data restore", "restore removed the install folder");
    }
    if (root_holds(&root, "PSP/SAVEDATA/ULUS10064NOSFO/DATA.BIN", "no sfo")) {
        fail("psp game data restore", "a save folder the unit lacks stayed");
    }
    sigil_sync_result_free(w);
    unit_of_free(only_data);

    sigil_save_unit *located = NULL;
    g.req.save.listing_count = root.count;
    expect_rc("psp game data locate", sigil_save_resolve(&g.req.save, &located), SIGIL_OK);
    for (size_t i = 0; located && i < located->member_count; i++) {
        if (strstr(located->members[i].path, "INSTALL")) fail("psp game data locate", "the install folder is a member");
    }
    sigil_save_unit_free(located);
    sigil_sync_result_free(r);
    root_free(&root);
}

/* A memory stick: PPSSPP's, a PSP's ms0:/ or Adrenaline's ux0:pspemu/. */
static void psp_stick(mem_root *root, const char *at) {
    char path[SIGIL_SAVE_PATH_MAX];
    const char *const files[][2] = {
        { "PSP/SAVEDATA/ULUS10064DATA00/PARAM.SFO", "sfo" },
        { "PSP/SAVEDATA/ULUS10064DATA00/DATA.BIN", "data" },
        { "PSP/SAVEDATA/ULUS10064SETTINGS/PARAM.SFO", "settings" },
        { "PSP/SAVEDATA/ULUS10041DATA00/PARAM.SFO", "another game" },
        { "PSP/SAVEDATA/SLUS01040/SCEVMC0.VMP", "a PS1 classic's card" },
        { "PSP/GAME/ULUS10064/EBOOT.PBP", "the game" },
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s%s", at, files[i][0]);
        put_text(root, path, files[i][1]);
    }
}

static void check_psp(void) {
    mem_root root = {0};
    psp_stick(&root, "");
    game g;
    make_game(&g, &root, "ppsspp", "psp", "ULUS10064");
    sigil_sync_result *r = NULL;
    expect_rc("psp collect", collect(&g, &r), SIGIL_OK);
    if (!r || r->profile[0] || r->profile_count) fail("psp collect", "a profile where PSP keeps none");
    if (!unit_all(r, "ULUS10064", NULL, 3) || !unit_has(r, "ULUS10064SETTINGS/PARAM.SFO")) {
        fail("psp collect", "every folder starting with the game's id, and only those");
    }
    if (!r || strcmp(r->artifact, "ULUS10064.zip") != 0) fail("psp collect", "artifact");
    keep_state(&g, r);

    mem_root fresh = {0};
    game t;
    make_game(&t, &fresh, "ppsspp_standalone", "psp", "ULUS10064");
    sigil_sync_result *w = NULL;
    expect_rc("psp restore to an empty stick", restore(&t, r, &w), SIGIL_OK);
    if (!root_holds(&fresh, "PSP/SAVEDATA/ULUS10064DATA00/DATA.BIN", "data")) fail("psp restore to an empty stick", "");
    sigil_sync_result_free(w);

    mem_root vita = {0};
    psp_stick(&vita, "pspemu/");
    game v;
    make_game(&v, &vita, "psp_console", "psp", "ULUS10064");
    put_text(&vita, "pspemu/PSP/SAVEDATA/ULUS10064SETTINGS/PARAM.SFO", "changed on the Vita");
    v.req.overwrite_local = 1;
    w = NULL;
    expect_rc("psp restore under pspemu", restore(&v, r, &w), SIGIL_OK);
    if (!root_holds(&vita, "pspemu/PSP/SAVEDATA/ULUS10064SETTINGS/PARAM.SFO", "settings") ||
        !root_holds(&vita, "pspemu/PSP/SAVEDATA/ULUS10041DATA00/PARAM.SFO", "another game") ||
        !root_holds(&vita, "pspemu/PSP/SAVEDATA/SLUS01040/SCEVMC0.VMP", "a PS1 classic's card")) {
        fail("psp restore under pspemu", "the game's folders written, the others kept");
    }
    sigil_sync_result_free(w);

    put_text(&root, "PSP/SAVEDATA/ULUS10064DATA00/DATA.BIN", "played since");
    w = NULL;
    expect_rc("psp conflict", restore(&g, r, &w), SIGIL_ERR_CONFLICT);
    sigil_sync_result_free(w);

    sigil_save_request req = g.req.save;
    req.listing_count = root.count;
    sigil_save_profile *profiles = NULL;
    size_t count = 1;
    expect_rc("psp profiles", sigil_save_profiles(&req, &profiles, &count), SIGIL_OK);
    if (profiles || count) fail("psp profiles", "a profile where PSP keeps none");

    sigil_sync_result_free(r);
    root_free(&vita);
    root_free(&fresh);
    root_free(&root);
}

/* ---- names and the base ----------------------------------------------------------- */

static void expect_base(const char *layout, const char *path, const char *base, const char *profile) {
    char got[SIGIL_SAVE_PATH_MAX], got_profile[SIGIL_PROFILE_ID_MAX];
    int rc = sigil_save_base(layout, path, got, sizeof(got), got_profile, sizeof(got_profile));
    if (rc != SIGIL_OK || strcmp(got, base) != 0 || strcmp(got_profile, profile) != 0) {
        char what[1200];
        snprintf(what, sizeof(what), "%s gave rc %d base '%s' profile '%s'", path, rc, got, got_profile);
        fail("save base", what);
    }
}

static void check_save_base(void) {
    expect_base("eden", "/sdcard/" ANDROID_EDEN SAVE_ROOT EDEN_USER "/",
                "/sdcard/Android/data/dev.eden.eden_emulator/files", EDEN_USER);
    expect_base("eden", "/home/u/.local/share/eden/nand/user/save/0000000000000000/" EDEN_USER "/" BOTW,
                "/home/u/.local/share/eden", EDEN_USER);
    expect_base("eden", "/home/u/.local/share/eden/nand/user/save/0000000000000000/" ZERO_USER,
                "/home/u/.local/share/eden", "");
    expect_base("eden", "/home/u/.local/share/eden/", "/home/u/.local/share/eden", "");
    expect_base("citron", "C:\\Emu\\citron\\nand\\user\\save\\0000000000000000\\" CITRON_USER, "C:\\Emu\\citron",
                CITRON_USER);
    expect_base("cemu", "/x/mlc01/usr/save/00050000/" WIIU_BOTW "/user/80000001", "/x", "80000001");
    expect_base("cemu", "/x/mlc01/usr/save/00050000/" WIIU_BOTW "/user/common", "/x", "");
    expect_base("vita3k", "/v/ux0/user/00/savedata/PCSB00676", "/v", "00");
    expect_base("rpcs3", "/r/dev_hdd0/home/00000001", "/r", "00000001");
    expect_base("ppsspp_standalone", "/storage/emulated/0/PSP/SAVEDATA", "/storage/emulated/0", "");
    expect_base("psp_console", "ux0:pspemu/PSP/SAVEDATA/ULUS10064DATA00", "ux0:pspemu", "");
    expect_base("mednafen_psx_hw", "/saves/psx/", "/saves/psx", "");
    char small[4], profile[SIGIL_PROFILE_ID_MAX];
    expect_rc("save base small", sigil_save_base("eden", "/a/long/path", small, sizeof(small), profile, sizeof(profile)),
              SIGIL_ERR_INVALID_ARG);
}

/* A listed save file or profile list the client can't open is an I/O error:
 * without the list, a collect would find no profile and report no saves. */
static void check_unopenable_files(void) {
    for (int which = 0; which < 2; which++) {
        mem_root root = {0};
        eden_root(&root, "");
        snprintf(root.unreadable, sizeof(root.unreadable), "%s",
                 which ? PROFILES_DAT : EDEN_SAVES BOTW "/option.sav");
        game g;
        make_game(&g, &root, "eden", "switch", BOTW);
        sigil_sync_result *r = NULL;
        expect_rc(which ? "unopenable profile list" : "unopenable save file", collect(&g, &r), SIGIL_ERR_IO);
        sigil_sync_result_free(r);
        root_free(&root);
    }
}

/* The profiles a client shows the user, with no game in the request. */
/* A name longer than a profile name holds ends on a whole character: 31
 * two-byte characters fill 62 bytes, and the three-byte one after them
 * doesn't fit in the one byte left; nor does a two-byte one after 62 ASCII
 * bytes. */
static void check_long_profile_names(void) {
    char long_name[128] = "";
    for (int i = 0; i < 31; i++) strcat(long_name, "\xC3\xA9");
    char want[128];
    snprintf(want, sizeof(want), "%s", long_name);
    strcat(long_name, "\xE6\x97\xA5" "tail");

    char ascii_name[128] = "";
    for (int i = 0; i < 62; i++) strcat(ascii_name, "a");
    char ascii_want[128];
    snprintf(ascii_want, sizeof(ascii_want), "%s", ascii_name);
    strcat(ascii_name, "\xC3\xA9");

    struct { const char *layout; const char *file; const char *before; const char *after; const char *name;
             const char *want; } rows[] = {
        { "rpcs3", "dev_hdd0/home/00000001/localusername", "", "\n", long_name, want },
        { "rpcs3", "dev_hdd0/home/00000001/localusername", "", "\n", ascii_name, ascii_want },
        { "vita3k", "ux0/user/00/user.xml", "<?xml version=\"1.0\"?>\n<user id=\"00\" name=\"", "\" avatar=\"default\">\n</user>\n",
          long_name, want },
    };
    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        char text[512];
        snprintf(text, sizeof(text), "%s%s%s", rows[r].before, rows[r].name, rows[r].after);
        mem_root root = {0};
        put_text(&root, rows[r].file, text);
        sigil_save_request req;
        memset(&req, 0, sizeof(req));
        req.struct_version = SIGIL_SAVE_REQUEST_V1;
        req.layout = rows[r].layout;
        req.listing = root.listing;
        req.listing_count = root.count;
        req.open = root_open;
        req.open_ctx = &root;
        sigil_save_profile *profiles = NULL;
        size_t count = 0;
        expect_rc(rows[r].layout, sigil_save_profiles(&req, &profiles, &count), SIGIL_OK);
        if (count != 1 || strcmp(profiles[0].name, rows[r].want) != 0) fail(rows[r].layout, "a long name ends mid-character");
        sigil_save_profiles_free(profiles);
        root_free(&root);
    }

    /* yuzu's 32-byte nickname field, filled to the end by a character cut short. */
    size_t len = 0;
    uint8_t *dat = corpus_sample(&g_switch, "switch", "eden-profile", "profiles.dat", &len);
    if (!dat || len < 0x10 + 0xC8) { free(dat); return; }
    uint8_t *field = dat + 0x10 + 0x28;
    char short_want[64] = "";
    for (int i = 0; i < 15; i++) {
        memcpy(field + 2 * i, "\xC3\xA9", 2);
        strcat(short_want, "\xC3\xA9");
    }
    memcpy(field + 30, "\xE6\x97", 2);
    mem_root root = {0};
    root_put(&root, PROFILES_DAT, dat, len);
    free(dat);
    sigil_save_request req;
    memset(&req, 0, sizeof(req));
    req.struct_version = SIGIL_SAVE_REQUEST_V1;
    req.layout = "eden";
    req.listing = root.listing;
    req.listing_count = root.count;
    req.open = root_open;
    req.open_ctx = &root;
    sigil_save_profile *profiles = NULL;
    size_t count = 0;
    expect_rc("yuzu name", sigil_save_profiles(&req, &profiles, &count), SIGIL_OK);
    if (count != 1 || strcmp(profiles[0].name, short_want) != 0) fail("yuzu name", "a cut nickname ends mid-character");
    sigil_save_profiles_free(profiles);
    root_free(&root);
}

static void check_list_profiles(void) {
    mem_root root = {0};
    two_profiles(&root);
    sigil_save_request req;
    memset(&req, 0, sizeof(req));
    req.struct_version = SIGIL_SAVE_REQUEST_V1;
    req.layout = "eden";
    req.listing = root.listing;
    req.listing_count = root.count;
    req.open = root_open;
    req.open_ctx = &root;
    sigil_save_profile *profiles = NULL;
    size_t count = 0;
    expect_rc("list profiles", sigil_save_profiles(&req, &profiles, &count), SIGIL_OK);
    if (count != 2 || strcmp(profiles[0].id, EDEN_USER) != 0 || strcmp(profiles[0].name, "Eden") != 0 ||
        strcmp(profiles[1].id, CITRON_USER) != 0 || strcmp(profiles[1].name, "citron") != 0) {
        fail("list profiles", "not both profiles with their names");
    }
    sigil_save_profiles_free(profiles);

    mem_root cemu = {0};
    put_sample(&cemu, &g_wiiu, "wiiu", "cemu-account", "act/", "files/" CEMU_ACT);
    req.layout = "cemu";
    req.listing = cemu.listing;
    req.listing_count = cemu.count;
    req.open_ctx = &cemu;
    profiles = NULL;
    expect_rc("list profiles below the root", sigil_save_profiles(&req, &profiles, &count), SIGIL_OK);
    if (count != 1 || strcmp(profiles[0].id, "80000001") != 0 || strcmp(profiles[0].name, "default") != 0) {
        fail("list profiles below the root", "Cemu account");
    }
    sigil_save_profiles_free(profiles);

    req.layout = "mednafen_psx_hw";
    profiles = NULL;
    expect_rc("list profiles without profiles", sigil_save_profiles(&req, &profiles, &count),
              SIGIL_ERR_UNSUPPORTED_FORMAT);
    if (profiles || count) fail("list profiles without profiles", "output set");
    root_free(&cemu);
    root_free(&root);
}

static void check_resolve(void) {
    mem_root root = {0};
    eden_root(&root, "");
    put_sample(&root, &g_switch, "switch", "mk8d-eden-split", "user/", EDEN_SAVES);
    put_sample(&root, &g_switch, "switch", "mk8d-eden-split", "device/", DEVICE_SAVES);
    game g;
    make_game(&g, &root, "eden", "switch", MK8D);
    g.req.save.listing_count = root.count;
    sigil_save_unit *unit = NULL;
    expect_rc("resolve", sigil_save_resolve(&g.req.save, &unit), SIGIL_OK);
    size_t account = 0, device = 0;
    for (size_t i = 0; unit && i < unit->member_count; i++) {
        const sigil_save_member *m = &unit->members[i];
        if (m->area == SIGIL_SAVE_AREA_ACCOUNT && strncmp(m->path, EDEN_SAVES MK8D "/", strlen(EDEN_SAVES MK8D "/")) == 0 &&
            strncmp(m->entry, MK8D "/", strlen(MK8D "/")) == 0) {
            account++;
        }
        if (m->area == SIGIL_SAVE_AREA_DEVICE && strcmp(m->entry, "device/" MK8D "/sg33.dat") == 0) device++;
    }
    if (account != 3 || device != 1) fail("resolve", "members and areas");
    if (!unit || unit->shape != SIGIL_SAVE_SHAPE_FOLDER || strcmp(unit->artifact, MK8D ".zip") != 0 ||
        !unit->content_hash[0]) {
        fail("resolve", "shape, artifact or hash");
    }
    sigil_save_unit_free(unit);
    root_free(&root);
}

int main(void) {
    char path[1024];
    if (corpus_platform_path("switch", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_switch) != 0 ||
        corpus_platform_path("wiiu", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_wiiu) != 0 ||
        corpus_platform_path("psvita", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_vita) != 0) {
        fprintf(stderr, "SKIP: manifests missing\n");
        return TEST_SKIP;
    }
    if (!corpus_present(&g_switch, "switch", "eden-profile") || !corpus_present(&g_wiiu, "wiiu", "cemu-account")) {
        fprintf(stderr, "SKIP: profile samples missing\n");
        return TEST_SKIP;
    }

    sigil_sync_result *botw = NULL, *acnh = NULL;
    check_eden_collect(&botw, &acnh);
    if (botw && acnh) {
        check_cross_fork(botw);
        check_root_too_high(botw);
        check_root_too_low(botw, acnh);
        check_two_profiles(botw, acnh);
    }
    check_sidecar();
    check_split();
    check_conflicts();
    check_area_state();
    check_cemu();
    check_vita3k();
    check_rpcs3();
    check_psp();
    check_psp_game_data();
    check_save_base();
    check_resolve();
    check_list_profiles();
    check_long_profile_names();
    check_unopenable_files();
    sigil_sync_result_free(botw);
    sigil_sync_result_free(acnh);

    corpus_free(&g_switch);
    corpus_free(&g_wiiu);
    corpus_free(&g_vita);
    if (g_fails) fprintf(stderr, "%d failure(s)\n", g_fails);
    return corpus_exit(g_fails);
}
