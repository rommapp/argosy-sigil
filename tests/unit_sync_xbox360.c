// SPDX-License-Identifier: MPL-2.0
/* Xbox 360 saves on Xenia Canary, Xenia Edge and XenDroid: a game's save
 * folders and their headers travel as <title>/00000001/... and
 * <title>/Headers/00000001/..., the all-zero XUID's as device/<title>/...,
 * never with a profile's id, so a save made under one profile restores under
 * another, both ways between the emulators. The profile's own files (the
 * Account, the dashboard's and the game's GPD) stay where they are. */
#include "legacy_units.h"
#include "x360_headers.h"
#include <stdio.h>

#define TITLE      "4D530AA4"
#define OTHER      "45410950"
#define CABINET    "E030000043414231"
#define CANARY     "E03000004B4F4E41"
#define ZERO_XUID  "0000000000000000"
#define DASHBOARD  "/FFFE07D1/00010000/"
#define SAVE       "content/" CABINET "/" TITLE "/00000001/ForzaProfile/PlayerDatabase"
#define HEADER     "content/" CABINET "/" TITLE "/Headers/00000001/ForzaProfile.header"
#define DEVICE_HDR "content/" ZERO_XUID "/" TITLE "/Headers/00000001/marketplace.header"

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static void expect_rc(const char *where, int got, int want) {
    if (got == want) return;
    char what[64];
    snprintf(what, sizeof(what), "rc %d, want %d", got, want);
    fail(where, what);
}

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    mem_root          *root;
} game;

static void make_game(game *g, mem_root *root, const char *layout) {
    memset(g, 0, sizeof(*g));
    g->root = root;
    g->result.struct_version = SIGIL_RESULT_V4;
    g->result.platform = SIGIL_PLATFORM_XBOX360;
    snprintf(g->result.title_id, sizeof(g->result.title_id), "%s", TITLE);
    snprintf(g->result.save_id, sizeof(g->result.save_id), "%s", TITLE);
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = "xbox360";
    g->req.save.content_path = "Forza Horizon 2.iso";
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

static int restore(game *g, const uint8_t *unit, size_t len, sigil_sync_result **out) {
    g->req.save.listing_count = g->root->count;
    *out = NULL;
    return sigil_restore(&g->req, unit, len, out);
}

static void put_text(mem_root *root, const char *path, const char *text) {
    root_put(root, path, (const uint8_t *)text, strlen(text));
}

static bool holds(mem_root *root, const char *path, const char *text) {
    mem_file *f = root_find(root, path);
    return f && f->len == strlen(text) && memcmp(f->data, text, f->len) == 0;
}

static bool same_file(mem_root *a, const char *path_a, mem_root *b, const char *path_b) {
    mem_file *x = root_find(a, path_a), *y = root_find(b, path_b);
    return x && y && x->len == y->len && memcmp(x->data, y->data, x->len) == 0;
}

/* A path of `root` that holds `needle`. */
static bool any_path_has(mem_root *root, const char *needle) {
    for (size_t i = 0; i < root->count; i++) {
        if (strstr(root->files[i].path, needle)) return true;
    }
    return false;
}

/* The unit's member names, joined with ',' in name order. */
static void names_of(const sigil_sync_result *r, char *out, size_t cap) {
    sigil_zip_member *m = NULL;
    size_t n = 0;
    out[0] = '\0';
    if (!r || !r->data || sigil_zip_read_mem(r->data, r->len, 1u << 20, &m, &n) != SIGIL_OK) return;
    for (size_t i = 0; i < n; i++) {
        size_t used = strlen(out);
        snprintf(out + used, cap - used, "%s%s", i ? "," : "", m[i].name);
    }
    sigil_zip_members_free(m, n);
}

static void put_bytes(mem_root *root, const char *path, uint8_t *bytes, size_t len) {
    root_put(root, path, bytes, len);
    free(bytes);
}

/* A header Xenia Edge wrote for one of the game's saves. */
static void put_edge_header(mem_root *root, const char *path, const char *name, const char *file) {
    size_t len = 0;
    uint8_t *h = x360_edge_header(name, file, 0x4D530AA4u, &len);
    put_bytes(root, path, h, len);
}

/* A header Xenia Canary wrote for a save it made as `profile`. */
static void put_canary_header(mem_root *root, const char *path, const char *name, const char *file, uint64_t profile) {
    size_t len = 0;
    uint8_t *h = x360_canary_header(name, file, 0x4D530AA4u, profile, &len);
    put_bytes(root, path, h, len);
}

/* The file at `got` is what the emulator writes, in its form (Canary's when
 * `canary`), for the header at `from`. */
static bool converted(mem_root *a, const char *got, mem_root *b, const char *from, const char *package, bool canary) {
    mem_file *x = root_find(a, got), *y = root_find(b, from);
    return x && y && x360_converted(x->data, x->len, y->data, y->len, package, canary);
}

/* The profile folder Xenia makes for `xuid`: the Account and both GPDs. */
static void put_profile(mem_root *root, const char *xuid, bool played) {
    char path[SIGIL_SAVE_PATH_MAX];
    uint8_t account[404];
    memset(account, 0xA5, sizeof(account));
    snprintf(path, sizeof(path), "content/%s" DASHBOARD "%s/Account", xuid, xuid);
    root_put(root, path, account, sizeof(account));
    snprintf(path, sizeof(path), "content/%s" DASHBOARD "%s/FFFE07D1.gpd", xuid, xuid);
    put_text(root, path, "dashboard gpd");
    if (played) {
        snprintf(path, sizeof(path), "content/%s" DASHBOARD "%s/" TITLE ".gpd", xuid, xuid);
        put_text(root, path, "achievements");
    }
}

/* Xenia Edge's content folder as CabinetOS keeps it: one profile with the
 * game's save, a header under the all-zero XUID, add-on content that isn't a
 * save, and another game's save. */
static void edge_root(mem_root *root) {
    put_profile(root, CABINET, true);
    put_text(root, SAVE, "progress made in edge");
    put_text(root, "content/" CABINET "/" TITLE "/00000001/ForzaProfile/__thumbnail.png", "png");
    put_edge_header(root, HEADER, "MMagTech", "ForzaProfile");
    put_edge_header(root, DEVICE_HDR, "marketplace", "marketplace");
    put_text(root, "content/" ZERO_XUID "/" TITLE "/00000002/Car Pack/data.bin", "an add-on, not a save");
    put_text(root, "content/" CABINET "/" OTHER "/00000001/Other/save.dat", "another game");
    put_text(root, "content/" CABINET "/" OTHER "/Headers/00000001/Other.header", "another header");
}

/* The save travels as its folders and headers, with no profile id; the
 * profile's own files, add-on content and other games stay. */
static void check_collect(sigil_sync_result **unit) {
    mem_root root = {0};
    edge_root(&root);
    game g;
    make_game(&g, &root, "xenia_edge");
    sigil_sync_result *r = NULL;
    expect_rc("collect", collect(&g, &r), SIGIL_OK);
    if (!r || strcmp(r->profile, CABINET) != 0 || r->profile_count != 1 || strcmp(r->profiles[0].id, CABINET) != 0 ||
        r->profiles[0].name[0]) {
        fail("collect", "the one profile, listed by id with no name");
    }
    char names[1024];
    names_of(r, names, sizeof(names));
    if (strcmp(names, TITLE "/00000001/ForzaProfile/PlayerDatabase," TITLE "/00000001/ForzaProfile/__thumbnail.png,"
                      TITLE "/Headers/00000001/ForzaProfile.header,"
                      "device/" TITLE "/Headers/00000001/marketplace.header") != 0) {
        fail("collect", names);
    }
    if (!r || r->shape != SIGIL_SAVE_SHAPE_FOLDER || strcmp(r->artifact, TITLE ".zip") != 0 || !r->identity_hash[0]) {
        fail("collect", "shape, artifact or identity");
    }
    if (!r || r->struct_version != SIGIL_SYNC_RESULT_V2 || r->maker_profile[0]) {
        fail("collect", "Edge's headers hold XUID 0, so no maker");
    }
    *unit = r;
    root_free(&root);
}

/* Restored into another Xenia under the one profile it has: each file lands
 * under that profile's id, the device header under the all-zero XUID, each
 * header in Canary's form as its FillHeader makes it from Edge's, and the
 * target's own profile files are left as they were. */
static void check_restore_other_profile(const sigil_sync_result *unit, mem_root *canary) {
    put_profile(canary, CANARY, false);
    game g;
    make_game(&g, canary, "xenia_canary");
    sigil_sync_result *w = NULL;
    expect_rc("restore other profile", restore(&g, unit->data, unit->len, &w), SIGIL_OK);
    if (!w || strcmp(w->profile, CANARY) != 0) fail("restore other profile", "profile");
    mem_root edge = {0};
    edge_root(&edge);
    if (!same_file(canary, "content/" CANARY "/" TITLE "/00000001/ForzaProfile/PlayerDatabase", &edge, SAVE) ||
        !converted(canary, "content/" CANARY "/" TITLE "/Headers/00000001/ForzaProfile.header", &edge, HEADER,
                   "ForzaProfile", true) ||
        !converted(canary, DEVICE_HDR, &edge, DEVICE_HDR, "marketplace", true)) {
        fail("restore other profile", "files not under the target's profile, or headers not in Canary's form");
    }
    if (any_path_has(canary, CABINET)) fail("restore other profile", "the source's profile id reached the target");
    if (root_find(canary, "content/" CANARY DASHBOARD CANARY "/" TITLE ".gpd")) {
        fail("restore other profile", "a GPD travelled");
    }
    if (!holds(canary, "content/" CANARY DASHBOARD CANARY "/FFFE07D1.gpd", "dashboard gpd")) {
        fail("restore other profile", "the target's dashboard GPD changed");
    }
    sigil_sync_result_free(w);
    root_free(&edge);
}

/* Canary's progress, under a header Canary made itself as its profile, goes
 * back to Edge: Edge's save becomes Canary's and its header what Edge's
 * ContentDataFromHeader makes of Canary's, under Edge's profile. The header
 * names its maker, which collect and restore report; Edge's form keeps none.
 * Edge then collects the same identity, and its sync state says nothing
 * changed there since. */
static void check_back_to_edge(mem_root *canary) {
    const char *canary_header = "content/" CANARY "/" TITLE "/Headers/00000001/ForzaProfile.header";
    put_text(canary, "content/" CANARY "/" TITLE "/00000001/ForzaProfile/PlayerDatabase", "progress made in canary");
    put_canary_header(canary, canary_header, "MMagTech", "ForzaProfile", 0xE03000004B4F4E41ull);
    game c;
    make_game(&c, canary, "xenia_canary");
    sigil_sync_result *unit = NULL;
    expect_rc("canary collect", collect(&c, &unit), SIGIL_OK);
    if (!unit || strcmp(unit->maker_profile, CANARY) != 0) fail("canary collect", "maker");

    mem_root edge = {0};
    edge_root(&edge);
    game e;
    make_game(&e, &edge, "xenia_edge");
    sigil_sync_result *before = NULL;
    expect_rc("edge collect before", collect(&e, &before), SIGIL_OK);
    if (before) {
        e.req.state = before->state;
        e.req.state_len = before->state_len;
    }
    sigil_sync_result *w = NULL;
    expect_rc("canary to edge", restore(&e, unit ? unit->data : NULL, unit ? unit->len : 0, &w), SIGIL_OK);
    if (!w || strcmp(w->maker_profile, CANARY) != 0) fail("canary to edge", "restore reports the unit's maker");
    if (!holds(&edge, SAVE, "progress made in canary") ||
        !converted(&edge, HEADER, canary, canary_header, "ForzaProfile", false)) {
        fail("canary to edge", "Edge's save isn't Canary's, or its header isn't in Edge's form");
    }
    if (!holds(&edge, "content/" CABINET "/" OTHER "/00000001/Other/save.dat", "another game") ||
        !holds(&edge, "content/" CABINET DASHBOARD CABINET "/" TITLE ".gpd", "achievements")) {
        fail("canary to edge", "another game's save or the profile's GPD changed");
    }
    if (any_path_has(&edge, CANARY)) fail("canary to edge", "Canary's profile id reached Edge");
    sigil_sync_result *after = NULL;
    if (w) {
        e.req.state = w->state;
        e.req.state_len = w->state_len;
    }
    expect_rc("edge collect after", collect(&e, &after), SIGIL_OK);
    if (!after || !unit || strcmp(after->identity_hash, unit->identity_hash) != 0 || after->changed) {
        fail("canary to edge", "Edge doesn't collect what Canary sent, unchanged");
    }
    if (!after || after->maker_profile[0]) fail("canary to edge", "Edge's form keeps no maker");
    sigil_sync_result_free(after);
    sigil_sync_result_free(w);
    sigil_sync_result_free(before);
    sigil_sync_result_free(unit);
    root_free(&edge);
}

/* A unit from before sigil, the content folder zipped as it is: restored for
 * another profile it writes what sigil's own unit writes, and its profile
 * folder and gamer pictures stay behind. */
static void check_older_unit(const sigil_sync_result *unit) {
    mem_root edge = {0};
    edge_root(&edge);
    /* Each file of the Edge folder the zip holds, under its name in the zip:
     * the path below content/. The gamer picture isn't in the folder. */
    const char *paths[] = {
        SAVE, "content/" CABINET "/" TITLE "/00000001/ForzaProfile/__thumbnail.png", HEADER, DEVICE_HDR,
        "content/" CABINET DASHBOARD CABINET "/Account", "content/" CABINET DASHBOARD CABINET "/" TITLE ".gpd",
        "content/" ZERO_XUID "/FFFE07D1/00020000/58410b83/64_58410b830002000100010001.png",
    };
    const char *names[7];
    uint8_t *data[7];
    size_t lens[7];
    for (size_t i = 0; i < 7; i++) {
        mem_file *f = root_find(&edge, paths[i]);
        names[i] = paths[i] + strlen("content/");
        data[i] = f ? f->data : (uint8_t *)"picture";
        lens[i] = f ? f->len : strlen("picture");
    }
    size_t len = 0;
    uint8_t *zip = legacy_zip(names, data, lens, 7, &len);

    mem_root older = {0}, own = {0};
    put_profile(&older, CANARY, false);
    put_profile(&own, CANARY, false);
    game a, b;
    make_game(&a, &older, "xenia_canary");
    make_game(&b, &own, "xenia_canary");
    sigil_sync_result *w = NULL, *v = NULL;
    expect_rc("older unit", restore(&a, zip, len, &w), SIGIL_OK);
    expect_rc("own unit", restore(&b, unit->data, unit->len, &v), SIGIL_OK);
    if (!roots_same(&older, &own)) fail("older unit", "doesn't restore to what sigil's own unit restores to");
    if (!w || !v || strcmp(w->identity_hash, v->identity_hash) != 0) fail("older unit", "identity");
    if (any_path_has(&older, CABINET) || any_path_has(&older, "00020000")) {
        fail("older unit", "its profile folder or gamer picture was written");
    }
    sigil_sync_result_free(w);
    sigil_sync_result_free(v);
    free(zip);
    root_free(&older);
    root_free(&own);
    root_free(&edge);
}

/* A Xenia that never made a profile: an account save needs the client to
 * name one, and lands under that id. An id in a form no Xenia lists (Edge and
 * Canary's FindProfiles take [0-9A-F]{16} other than the all-zero XUID) names
 * a folder none reads, so it is refused before anything is written. */
static void check_no_profile(const sigil_sync_result *unit) {
    mem_root empty = {0};
    game g;
    make_game(&g, &empty, "xenia_edge");
    sigil_sync_result *w = NULL;
    expect_rc("restore with no profile", restore(&g, unit->data, unit->len, &w), SIGIL_ERR_NO_TARGET);
    if (!w || strncmp(w->problem, TITLE "/", strlen(TITLE "/")) != 0) fail("restore with no profile", "problem");
    if (empty.writes) fail("restore with no profile", "wrote before refusing");
    sigil_sync_result_free(w);

    const char *unread[] = { "e030000043414231", "E03000004341423", "E03000004341423G", "E0300000434142310",
                             "E03000004341423 ", ZERO_XUID };
    for (size_t i = 0; i < sizeof(unread) / sizeof(unread[0]); i++) {
        g.req.save.profile = unread[i];
        w = NULL;
        expect_rc(unread[i], restore(&g, unit->data, unit->len, &w), SIGIL_ERR_INVALID_ARG);
        sigil_sync_result_free(w);
    }
    if (empty.writes) fail("restore under an id no Xenia lists", "wrote before refusing");

    g.req.save.profile = CABINET;
    w = NULL;
    expect_rc("restore with profile", restore(&g, unit->data, unit->len, &w), SIGIL_OK);
    if (!holds(&empty, SAVE, "progress made in edge") || !root_find(&empty, HEADER) || !root_find(&empty, DEVICE_HDR)) {
        fail("restore with profile", "not under the named profile");
    }
    sigil_sync_result_free(w);
    root_free(&empty);
}

/* Two profiles, one holding the game and none named: collect asks which,
 * listing the ids alone; named in any case, it takes that one. */
static void check_two_profiles(void) {
    mem_root root = {0};
    edge_root(&root);
    put_profile(&root, CANARY, false);
    game g;
    make_game(&g, &root, "xenia_canary");
    sigil_sync_result *r = NULL;
    expect_rc("two profiles", collect(&g, &r), SIGIL_ERR_AMBIGUOUS);
    if (!r || strcmp(r->problem, CABINET "\n" CANARY) != 0 || r->profile_count != 2) {
        fail("two profiles", "problem lists both ids alone");
    }
    sigil_sync_result_free(r);
    g.req.save.profile = "e030000043414231";
    r = NULL;
    expect_rc("two profiles, one named", collect(&g, &r), SIGIL_OK);
    if (!r || strcmp(r->profile, CABINET) != 0) fail("two profiles, one named", "profile");
    sigil_sync_result_free(r);
    g.req.save.profile = "e03000000c0ffee1";
    r = NULL;
    expect_rc("two profiles, an unlisted lowercase id", collect(&g, &r), SIGIL_ERR_INVALID_ARG);
    sigil_sync_result_free(r);
    root_free(&root);
}

/* The profile list is the Account files under a profile's own folder, the
 * folder named as FindProfiles matches it: a folder naming two ids is no
 * profile, nor is the all-zero XUID, a lowercase or short folder name, or a
 * profile folder with no Account. */
static void check_profile_list(void) {
    mem_root root = {0};
    put_profile(&root, CABINET, true);
    put_profile(&root, CANARY, false);
    put_text(&root, "content/E030000011111111" DASHBOARD "E030000022222222/Account", "a stray copy");
    put_edge_header(&root, DEVICE_HDR, "marketplace", "marketplace");
    put_profile(&root, ZERO_XUID, false);
    put_profile(&root, "e03000000c0ffee1", false);
    put_profile(&root, "E03000000C0FFEE", false);
    put_text(&root, "content/E03000000C0FFEE2" DASHBOARD "E03000000C0FFEE2/FFFE07D1.gpd", "no account");
    sigil_save_request req;
    memset(&req, 0, sizeof(req));
    req.struct_version = SIGIL_SAVE_REQUEST_V1;
    req.layout = "xendroid";
    req.listing = root.listing;
    req.listing_count = root.count;
    req.open = root_open;
    req.open_ctx = &root;
    sigil_save_profile *profiles = NULL;
    size_t count = 0;
    expect_rc("profile list", sigil_save_profiles(&req, &profiles, &count), SIGIL_OK);
    if (count != 2 || strcmp(profiles[0].id, CABINET) != 0 || strcmp(profiles[1].id, CANARY) != 0 ||
        profiles[0].name[0] || profiles[1].name[0]) {
        fail("profile list", "the two profiles by id alone");
    }
    sigil_save_profiles_free(profiles);
    root_free(&root);
}

/* A member no folder of the game takes, or one leaving the root, is refused
 * before anything is written. */
static void check_foreign_units(void) {
    mem_root root = {0};
    put_profile(&root, CABINET, true);
    game g;
    make_game(&g, &root, "xendroid");
    g.req.overwrite_local = 1;
    const char *names[] = { OTHER "/00000001/Other/save.dat" };
    uint8_t *data[] = { (uint8_t *)"x" };
    size_t lens[] = { 1 }, len = 0;
    uint8_t *zip = legacy_zip(names, data, lens, 1, &len);
    sigil_sync_result *w = NULL;
    int writes = root.writes;
    expect_rc("another game's unit", restore(&g, zip, len, &w), SIGIL_ERR_NO_TARGET);
    if (!w || strcmp(w->problem, names[0]) != 0) fail("another game's unit", "problem");
    sigil_sync_result_free(w);
    free(zip);

    const char *escape[] = { TITLE "/00000001/Save/fine.dat", TITLE "/00000001/Save/../../../../../escape.dat" };
    uint8_t *two[] = { (uint8_t *)"x", (uint8_t *)"y" };
    size_t two_lens[] = { 1, 1 };
    zip = legacy_zip(escape, two, two_lens, 2, &len);
    w = NULL;
    expect_rc("escaping member", restore(&g, zip, len, &w), SIGIL_ERR_IO);
    if (root.writes != writes) fail("foreign units", "wrote before refusing");
    sigil_sync_result_free(w);
    free(zip);
    root_free(&root);
}

/* A Canary-made header restored to another Canary keeps every byte, the
 * maker's XUID and what only the container holds included; restored to
 * XenDroid, which reads Edge's form alone, it is in Edge's form. */
static void check_header_forms(void) {
    const char *from = "content/" CABINET "/" TITLE "/Headers/00000001/ForzaProfile.header";
    const char *to = "content/" CANARY "/" TITLE "/Headers/00000001/ForzaProfile.header";
    mem_root source = {0};
    put_profile(&source, CABINET, true);
    put_text(&source, SAVE, "progress");
    put_canary_header(&source, from, "MMagTech", "ForzaProfile", 0xE030000043414231ull);
    game g;
    make_game(&g, &source, "xenia_canary");
    sigil_sync_result *unit = NULL;
    expect_rc("header forms collect", collect(&g, &unit), SIGIL_OK);
    const char *layouts[] = { "xenia_canary", "xendroid" };
    for (size_t k = 0; k < 2 && unit; k++) {
        mem_root target = {0};
        put_profile(&target, CANARY, false);
        game t;
        make_game(&t, &target, layouts[k]);
        sigil_sync_result *w = NULL;
        expect_rc(layouts[k], restore(&t, unit->data, unit->len, &w), SIGIL_OK);
        bool ok = k == 0 ? same_file(&target, to, &source, from) : converted(&target, to, &source, from, "ForzaProfile", false);
        if (!ok) fail(layouts[k], k == 0 ? "Canary's header changed" : "XenDroid's header isn't in Edge's form");
        sigil_sync_result_free(w);
        root_free(&target);
    }
    sigil_sync_result_free(unit);
    root_free(&source);
}

/* A unit header in neither form is one no Xenia reads: restore builds no
 * form from it and refuses before writing. */
static void check_unreadable_header(void) {
    const char *names[] = { TITLE "/00000001/Save/save.dat", TITLE "/Headers/00000001/Save.header" };
    uint8_t short_header[100];
    memset(short_header, 0x11, sizeof(short_header));
    uint8_t *data[] = { (uint8_t *)"x", short_header };
    size_t lens[] = { 1, sizeof(short_header) }, len = 0;
    uint8_t *zip = legacy_zip(names, data, lens, 2, &len);
    const char *layouts[] = { "xenia_canary", "xenia_edge" };
    for (size_t k = 0; k < 2; k++) {
        mem_root root = {0};
        put_profile(&root, CABINET, false);
        game g;
        make_game(&g, &root, layouts[k]);
        int writes = root.writes;
        sigil_sync_result *w = NULL;
        expect_rc("unreadable header", restore(&g, zip, len, &w), SIGIL_ERR_UNSUPPORTED_FORMAT);
        if (root.writes != writes) fail("unreadable header", "wrote before refusing");
        sigil_sync_result_free(w);
        root_free(&root);
    }
    free(zip);
}

/* The maker is the one XUID the account saves' headers name: two that name
 * different ones give none, and a header naming none doesn't count. */
static void check_makers(void) {
    const char *second = "content/" CABINET "/" TITLE "/Headers/00000001/Second.header";
    for (int disagree = 0; disagree < 2; disagree++) {
        mem_root root = {0};
        put_profile(&root, CABINET, true);
        put_text(&root, SAVE, "progress");
        put_text(&root, "content/" CABINET "/" TITLE "/00000001/Second/data", "more");
        put_canary_header(&root, HEADER, "MMagTech", "ForzaProfile", 0xE030000043414231ull);
        if (disagree) put_canary_header(&root, second, "Second", "Second", 0xE03000000C0FFEE1ull);
        else put_edge_header(&root, second, "Second", "Second");
        game g;
        make_game(&g, &root, "xenia_canary");
        sigil_sync_result *r = NULL;
        expect_rc("makers", collect(&g, &r), SIGIL_OK);
        if (!r || strcmp(r->maker_profile, disagree ? "" : CABINET) != 0) fail("makers", disagree ? "two named" : "one named");
        sigil_sync_result_free(r);
        root_free(&root);
    }
}

static void expect_base(const char *path, const char *base, const char *profile) {
    char got[SIGIL_SAVE_PATH_MAX], got_profile[SIGIL_PROFILE_ID_MAX];
    int rc = sigil_save_base("xenia_canary", path, got, sizeof(got), got_profile, sizeof(got_profile));
    if (rc != SIGIL_OK || strcmp(got, base) != 0 || strcmp(got_profile, profile) != 0) {
        char what[SIGIL_SAVE_PATH_MAX + 64];
        snprintf(what, sizeof(what), "%s gave '%s' '%s'", path, got, got_profile);
        fail("save base", what);
    }
}

static void check_layouts(const sigil_sync_result *unit) {
    expect_base("/home/u/.local/share/Xenia/content/" CABINET "/" TITLE, "/home/u/.local/share/Xenia", CABINET);
    expect_base("/saves/x360/content/" ZERO_XUID "/" TITLE, "/saves/x360", "");
    expect_base("/saves/x360/content", "/saves/x360", "");
    if (strcmp(sigil_save_layout_top("xendroid"), "content") != 0) fail("layout top", "content");

    sigil_layout_info *l = NULL;
    size_t n = 0;
    if (sigil_layouts("xbox360", &l, &n) != SIGIL_OK) {
        fail("layouts", "xbox360");
        return;
    }
    const char *want[] = { "xenia_canary", "xenia_edge", "xendroid" };
    size_t found = 0, own = 0;
    for (size_t i = 0; i < n; i++) {
        own += strcmp(l[i].platform, "xbox360") == 0;
        for (size_t k = 0; k < 3; k++) {
            if (strcmp(l[i].id, want[k]) == 0 && strcmp(l[i].platform, "xbox360") == 0 && l[i].profiles &&
                !l[i].needs_existing) {
                found++;
            }
        }
    }
    if (strcmp(l[0].id, "libretro") != 0 || own != 3 || found != 3) fail("layouts", "the three Xenia rows, with profiles");
    sigil_layouts_free(l);
    if (sigil_layouts("ps3", &l, &n) == SIGIL_OK) {
        for (size_t i = 0; i < n; i++) {
            if (strncmp(l[i].id, "xen", 3) == 0) fail("layouts", "a Xenia row listed for ps3");
        }
        sigil_layouts_free(l);
    }

    /* "xenia" names upstream Xenia, whose tree has no XUID or headers: no row
     * takes it, so collect and restore refuse it rather than use another's. */
    mem_root root = {0};
    edge_root(&root);
    game g;
    make_game(&g, &root, "xenia");
    sigil_sync_result *r = NULL;
    expect_rc("xenia collect", collect(&g, &r), SIGIL_ERR_UNSUPPORTED_FORMAT);
    sigil_sync_result_free(r);
    r = NULL;
    int writes = root.writes;
    expect_rc("xenia restore", restore(&g, unit->data, unit->len, &r), SIGIL_ERR_UNSUPPORTED_FORMAT);
    if (root.writes != writes) fail("xenia restore", "wrote");
    sigil_sync_result_free(r);
    root_free(&root);
}

int main(void) {
    sigil_sync_result *unit = NULL;
    check_collect(&unit);
    if (unit) {
        mem_root canary = {0};
        check_restore_other_profile(unit, &canary);
        check_back_to_edge(&canary);
        root_free(&canary);
        check_no_profile(unit);
        check_older_unit(unit);
        check_layouts(unit);
    }
    check_two_profiles();
    check_profile_list();
    check_foreign_units();
    check_header_forms();
    check_unreadable_header();
    check_makers();
    sigil_sync_result_free(unit);
    if (g_fails) fprintf(stderr, "%d failure(s)\n", g_fails);
    return g_fails ? 1 : 0;
}
