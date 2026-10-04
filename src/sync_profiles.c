// SPDX-License-Identifier: MPL-2.0
/* Collect and restore on layouts whose saves are folders kept per user
 * profile. The unit is a zip of the game's save folders named as the layout's
 * area rows say, never with a profile's id, so it restores under any profile.
 * The state keeps one hash per area: the account area's per profile, the
 * device area's for the whole device. */
#include "sync_internal.h"

#define PROFILE_MAX_MEMBER (512u * 1024u * 1024u)

static const int AREAS[] = { SIGIL_SAVE_AREA_ACCOUNT, SIGIL_SAVE_AREA_DEVICE };
#define AREA_COUNT (sizeof(AREAS) / sizeof(AREAS[0]))

typedef struct {
    sigil_profile_root root;
    char               area_keys[AREA_COUNT][SYNC_KEY_MAX];   /* the state key of each area, by AREAS */
} profile_sync;

/* Settles the root and profile, gives the result the profiles listed, and
 * keys the game's state by the profile. */
static int open_sync(sigil_sync_ctx *x, const sigil_layout_profiles *row, profile_sync *s, sigil_sync_result *r) {
    const sigil_result *game = x->req->save.result;
    if (!game || !game->save_id[0]) return SIGIL_ERR_INVALID_ARG;
    int rc = sigil_profile_root_open(&x->req->save, row, &s->root, r->problem);
    if (rc != SIGIL_OK) return rc;
    if (s->root.profile_count) {
        r->profiles = (sigil_save_profile *)calloc(s->root.profile_count, sizeof(*r->profiles));
        if (!r->profiles) return SIGIL_ERR_OOM;
        memcpy(r->profiles, s->root.profiles, s->root.profile_count * sizeof(*r->profiles));
        r->profile_count = s->root.profile_count;
    }
    snprintf(r->profile, sizeof(r->profile), "%s", s->root.profile);
    char device[SYNC_KEY_MAX];
    snprintf(device, sizeof(device), "%s", x->game);
    if (s->root.profile[0]) {
        char escaped[SYNC_KEY_MAX];
        sigil_sync_escape(s->root.profile, escaped, sizeof(escaped));
        size_t n = strlen(x->game);
        snprintf(x->game + n, sizeof(x->game) - n, "@%s", escaped);
    }
    snprintf(s->area_keys[0], SYNC_KEY_MAX, "%s#account", x->game);
    snprintf(s->area_keys[1], SYNC_KEY_MAX, "%s#device", device);
    return SIGIL_OK;
}

/* The hash over the parts of `area`, or of every part with an area when
 * `area` is negative; "" when there are none. A part restore skips has
 * SIGIL_SAVE_AREA_NONE. */
static void identity_of(const sigil_named_md5 *parts, const int *areas, size_t n, int area, char out[33]) {
    out[0] = '\0';
    sigil_named_md5 *picked = (sigil_named_md5 *)calloc(n ? n : 1, sizeof(*picked));
    size_t m = 0;
    for (size_t i = 0; picked && i < n; i++) {
        if (area < 0 ? areas[i] != SIGIL_SAVE_AREA_NONE : areas[i] == area) picked[m++] = parts[i];
    }
    if (m) sigil_named_hash(picked, m, out);
    free(picked);
}

/* RomM's content hash of a unit: every member, skipped or not. */
static void content_of(const sigil_named_md5 *parts, size_t n, char out[33]) {
    out[0] = '\0';
    sigil_named_md5 *all = (sigil_named_md5 *)calloc(n ? n : 1, sizeof(*all));
    if (!all) return;
    memcpy(all, parts, n * sizeof(*all));
    if (n) sigil_named_hash(all, n, out);
    free(all);
}

static void artifact_of(const profile_sync *s, sigil_sync_result *r) {
    snprintf(r->artifact, sizeof(r->artifact), "%s.zip", s->root.save_id);
}

/* ---- collect ------------------------------------------------------------------ */

int sigil_sync_collect_profiles(sigil_sync_ctx *x, const sigil_layout_profiles *row, sigil_sync_result *r) {
    profile_sync s;
    int rc = open_sync(x, row, &s, r);
    if (rc == SIGIL_OK && sigil_profile_undecided(&s.root)) {
        sigil_profile_lines(&s.root, r->problem, sizeof(r->problem));
        rc = SIGIL_ERR_AMBIGUOUS;
    }
    sigil_profile_file *files = NULL;
    size_t n = 0;
    if (rc == SIGIL_OK) rc = sigil_profile_files(&s.root, s.root.profile, &files, &n);
    if (rc != SIGIL_OK) return rc;
    sigil_zip_member *members = (sigil_zip_member *)calloc(n ? n : 1, sizeof(*members));
    sigil_named_md5 *parts = (sigil_named_md5 *)calloc(n ? n : 1, sizeof(*parts));
    int *areas = (int *)calloc(n ? n : 1, sizeof(*areas));
    if (!members || !parts || !areas) rc = SIGIL_ERR_OOM;
    for (size_t i = 0; i < n && rc == SIGIL_OK; i++) {
        snprintf(members[i].name, sizeof(members[i].name), "%s", files[i].entry);
        rc = sigil_sync_read_file(x->req, files[i].path, PROFILE_MAX_MEMBER, &members[i].data, &members[i].len);
        if (rc == SIGIL_ERR_NOT_FOUND) rc = SIGIL_ERR_IO;
        if (rc != SIGIL_OK) break;
        snprintf(parts[i].name, sizeof(parts[i].name), "%s", files[i].entry);
        sigil_md5_of(members[i].data, members[i].len, parts[i].md5);
        areas[i] = files[i].area;
    }
    if (rc == SIGIL_OK && n) {
        rc = sigil_zip_store(members, n, &r->data, &r->len);
        identity_of(parts, areas, n, -1, r->identity_hash);
        memcpy(r->content_hash, r->identity_hash, sizeof(r->content_hash));
        r->shape = SIGIL_SAVE_SHAPE_FOLDER;
        artifact_of(&s, r);
    }
    for (size_t a = 0; a < AREA_COUNT && rc == SIGIL_OK; a++) {
        char hash[33];
        identity_of(parts, areas, n, AREAS[a], hash);
        rc = sigil_sync_state_put(&x->state, "synced", s.area_keys[a], hash[0] ? hash : NULL);
    }
    if (members) sigil_zip_members_free(members, n);
    free(parts);
    free(areas);
    free(files);
    return rc;
}

/* ---- restore ------------------------------------------------------------------ */

typedef struct {
    sigil_zip_member *members;
    size_t            count;
    char            (*paths)[SIGIL_SAVE_PATH_MAX];   /* where each member goes; "" for one restore skips */
    int              *areas;
    sigil_named_md5  *parts;                         /* under the name collect gives each member */
    char              content[33];                   /* RomM's hash of the unit as it came */
    bool              carried[AREA_COUNT];           /* the unit holds files of the area, by AREAS */
} incoming_unit;

static void incoming_free(incoming_unit *u) {
    if (u->members) sigil_zip_members_free(u->members, u->count);
    free(u->paths);
    free(u->areas);
    free(u->parts);
}

static size_t area_index(int area) {
    for (size_t a = 0; a < AREA_COUNT; a++) {
        if (AREAS[a] == area) return a;
    }
    return 0;
}

/* Two members of `u` before `i` go to the same file. */
static bool placed_twice(const incoming_unit *u, size_t i) {
    for (size_t k = 0; k < i; k++) {
        if (u->paths[k][0] && strcmp(u->paths[k], u->paths[i]) == 0) return true;
    }
    return false;
}

/* Reads the unit and places each member, before anything is written. A
 * member no folder of the game takes, or one with no folder the root
 * reaches, is SIGIL_ERR_NO_TARGET; an account member with several profiles
 * and none picked is SIGIL_ERR_AMBIGUOUS; one whose path leaves the root is
 * SIGIL_ERR_IO; two members for one file are SIGIL_ERR_UNSUPPORTED_FORMAT. */
static int read_unit(const profile_sync *s, const uint8_t *unit, size_t len, incoming_unit *u, sigil_sync_result *r) {
    memset(u, 0, sizeof(*u));
    int rc = sigil_zip_read_mem(unit, len, PROFILE_MAX_MEMBER, &u->members, &u->count);
    if (rc != SIGIL_OK) return rc;
    u->paths = calloc(u->count ? u->count : 1, SIGIL_SAVE_PATH_MAX);
    u->areas = (int *)calloc(u->count ? u->count : 1, sizeof(*u->areas));
    u->parts = (sigil_named_md5 *)calloc(u->count ? u->count : 1, sizeof(*u->parts));
    if (!u->paths || !u->areas || !u->parts) return SIGIL_ERR_OOM;
    for (size_t i = 0; i < u->count; i++) {
        snprintf(u->parts[i].name, sizeof(u->parts[i].name), "%s", u->members[i].name);
        sigil_md5_of(u->members[i].data, u->members[i].len, u->parts[i].md5);
    }
    content_of(u->parts, u->count, u->content);
    for (size_t i = 0; i < u->count; i++) {
        sigil_profile_file f;
        bool skip = false;
        rc = sigil_profile_place(&s->root, u->members[i].name, &f, &skip);
        if (rc == SIGIL_ERR_NO_TARGET && f.area == SIGIL_SAVE_AREA_ACCOUNT && s->root.several) {
            sigil_profile_lines(&s->root, r->problem, sizeof(r->problem));
            return SIGIL_ERR_AMBIGUOUS;
        }
        if (rc != SIGIL_OK) {
            snprintf(r->problem, sizeof(r->problem), "%s", u->members[i].name);
            return SIGIL_ERR_NO_TARGET;
        }
        snprintf(u->paths[i], SIGIL_SAVE_PATH_MAX, "%s", f.path);
        snprintf(u->parts[i].name, sizeof(u->parts[i].name), "%s", f.entry);
        u->areas[i] = skip ? SIGIL_SAVE_AREA_NONE : f.area;
        if (!skip) u->carried[area_index(f.area)] = true;
        if (u->paths[i][0] && !sigil_sync_path_inside(u->paths[i])) return SIGIL_ERR_IO;
        if (u->paths[i][0] && placed_twice(u, i)) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    }
    return SIGIL_OK;
}

typedef struct {
    sigil_profile_file *files;
    size_t              count;
    sigil_named_md5    *parts;
    int                *areas;
} local_files;

static void local_free(local_files *l) {
    free(l->files);
    free(l->parts);
    free(l->areas);
}

static int read_local(const sigil_sync_ctx *x, const profile_sync *s, local_files *l) {
    memset(l, 0, sizeof(*l));
    int rc = sigil_profile_files(&s->root, s->root.profile, &l->files, &l->count);
    if (rc != SIGIL_OK) return rc;
    l->parts = (sigil_named_md5 *)calloc(l->count ? l->count : 1, sizeof(*l->parts));
    l->areas = (int *)calloc(l->count ? l->count : 1, sizeof(*l->areas));
    if (!l->parts || !l->areas) return SIGIL_ERR_OOM;
    for (size_t i = 0; i < l->count; i++) {
        uint8_t *data = NULL;
        size_t len = 0;
        rc = sigil_sync_read_file(x->req, l->files[i].path, PROFILE_MAX_MEMBER, &data, &len);
        if (rc != SIGIL_OK) return rc == SIGIL_ERR_NOT_FOUND ? SIGIL_ERR_IO : rc;
        snprintf(l->parts[i].name, sizeof(l->parts[i].name), "%s", l->files[i].entry);
        sigil_md5_of(data, len, l->parts[i].md5);
        free(data);
        l->areas[i] = l->files[i].area;
    }
    return SIGIL_OK;
}

/* The conflict rule per area the unit carries; `*already` is true when each
 * of them already holds what the unit brings. */
static int check_areas(const sigil_sync_ctx *x, const profile_sync *s, const incoming_unit *u, const local_files *l,
                       sigil_sync_result *r, bool *already) {
    bool blocked = false;
    *already = true;
    for (size_t a = 0; a < AREA_COUNT; a++) {
        if (!u->carried[a]) continue;
        char mine[33], theirs[33];
        identity_of(l->parts, l->areas, l->count, AREAS[a], mine);
        identity_of(u->parts, u->areas, u->count, AREAS[a], theirs);
        bool same = false;
        const char *last = sigil_sync_state_get(&x->state, "synced", s->area_keys[a]);
        blocked = sigil_sync_blocks_restore(x, mine, theirs, last, &same) || blocked;
        *already = *already && same;
    }
    if (!blocked) return SIGIL_OK;
    r->conflict = 1;
    return SIGIL_ERR_CONFLICT;
}

static bool placed(const incoming_unit *u, const char *path) {
    for (size_t i = 0; i < u->count; i++) {
        if (u->paths[i][0] && strcmp(u->paths[i], path) == 0) return true;
    }
    return false;
}

/* A local file of an area the unit carries that the unit lacks. */
static bool goes(const incoming_unit *u, const local_files *l, size_t i) {
    return u->carried[area_index(l->areas[i])] && !placed(u, l->files[i].path);
}

static bool holds(const local_files *l, const char *path, const char *md5) {
    for (size_t i = 0; i < l->count; i++) {
        if (strcmp(l->files[i].path, path) == 0) return strcmp(l->parts[i].md5, md5) == 0;
    }
    return false;
}

static int write_areas(const sigil_sync_ctx *x, const incoming_unit *u, const local_files *l) {
    bool removing = false;
    for (size_t i = 0; i < l->count && !removing; i++) removing = goes(u, l, i);
    if (removing && !x->req->remove) return SIGIL_ERR_INVALID_ARG;
    int rc = SIGIL_OK;
    for (size_t i = 0; i < u->count && rc == SIGIL_OK; i++) {
        if (!u->paths[i][0] || holds(l, u->paths[i], u->parts[i].md5)) continue;
        rc = sigil_sync_put(x->req, u->paths[i], u->members[i].data, u->members[i].len);
    }
    for (size_t i = 0; i < l->count && rc == SIGIL_OK; i++) {
        if (goes(u, l, i)) rc = sigil_sync_drop(x->req, l->files[i].path);
    }
    return rc;
}

int sigil_sync_restore_profiles(sigil_sync_ctx *x, const sigil_layout_profiles *row, const uint8_t *unit, size_t len,
                                sigil_sync_result *r, char local_identity[33]) {
    profile_sync s;
    incoming_unit u;
    local_files l;
    memset(&u, 0, sizeof(u));
    memset(&l, 0, sizeof(l));
    int rc = open_sync(x, row, &s, r);
    if (rc == SIGIL_OK) rc = read_unit(&s, unit, len, &u, r);
    if (rc == SIGIL_OK) rc = read_local(x, &s, &l);
    bool already = false;
    if (rc == SIGIL_OK) {
        r->problem[0] = '\0';
        identity_of(u.parts, u.areas, u.count, -1, r->identity_hash);
        memcpy(r->content_hash, u.content, sizeof(r->content_hash));
        identity_of(l.parts, l.areas, l.count, -1, local_identity);
        r->shape = SIGIL_SAVE_SHAPE_FOLDER;
        artifact_of(&s, r);
        rc = check_areas(x, &s, &u, &l, r, &already);
    }
    if (rc == SIGIL_OK && !already) rc = write_areas(x, &u, &l);
    for (size_t a = 0; a < AREA_COUNT && rc == SIGIL_OK; a++) {
        if (!u.carried[a]) continue;
        char hash[33];
        identity_of(u.parts, u.areas, u.count, AREAS[a], hash);
        rc = sigil_sync_state_put(&x->state, "synced", s.area_keys[a], hash[0] ? hash : NULL);
    }
    incoming_free(&u);
    local_free(&l);
    return rc;
}
