// SPDX-License-Identifier: MPL-2.0
#include "save_profiles.h"
#include "card_saturn.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#define PROFILE_LIST_MAX (64u * 1024u)
#define YUZU_HEADER      0x10u
#define YUZU_ENTRY       0xC8u
#define YUZU_NAME_AT     0x28u
#define YUZU_NAME_MAX    0x20u

/* ---- templates ------------------------------------------------------------------ */

typedef struct {
    char profile[SIGIL_PROFILE_ID_MAX];
    char save_id[SIGIL_SAVE_ENTRY_MAX];
} folder_vars;

static char *var_slot(folder_vars *v, const char *t, size_t n, size_t *cap) {
    if (n == 9 && strncmp(t, "{profile}", 9) == 0) {
        *cap = sizeof(v->profile);
        return v->profile;
    }
    if (n == 9 && strncmp(t, "{save_id}", 9) == 0) {
        *cap = sizeof(v->save_id);
        return v->save_id;
    }
    return NULL;
}

/* Walks `template_` and `path` a segment at a time, binding each variable to
 * the segment it stands for, until either runs out. `*t_end` and `*p_end` get
 * how far each got. False at the first segment that differs. */
static bool walk(const char *template_, const char *path, folder_vars *v, const char **t_end, const char **p_end) {
    memset(v, 0, sizeof(*v));
    const char *t = template_, *p = path;
    while (*t && *p) {
        size_t tn = strcspn(t, "/"), pn = strcspn(p, "/");
        if (pn == 0) return false;
        if (t[0] == '{') {
            size_t cap = 0;
            char *slot = var_slot(v, t, tn, &cap);
            if (!slot || pn >= cap) return false;
            memcpy(slot, p, pn);
            slot[pn] = '\0';
        } else if (tn != pn || strncmp(t, p, tn) != 0) {
            return false;
        }
        t += tn;
        p += pn;
        if (*t != '/' || *p != '/') break;
        t++;
        p++;
    }
    *t_end = t;
    *p_end = p;
    return true;
}

/* `path` lies in folder `template_`; `*rest` gets the part inside it. */
static bool in_folder(const char *template_, const char *path, folder_vars *v, const char **rest) {
    const char *t_end = NULL;
    return walk(template_, path, v, &t_end, rest) && !*t_end && **rest;
}

/* `path` is file `template_`. */
static bool is_file(const char *template_, const char *path, folder_vars *v) {
    const char *t_end = NULL, *p_end = NULL;
    return walk(template_, path, v, &t_end, &p_end) && !*t_end && !*p_end;
}

static bool expand(const char *template_, folder_vars *v, const char *rest, char *out, size_t cap) {
    size_t n = 0;
    for (const char *t = template_; *t;) {
        size_t tn = strcspn(t, "/");
        const char *value = t;
        size_t vn = tn;
        if (t[0] == '{') {
            size_t slot_cap = 0;
            value = var_slot(v, t, tn, &slot_cap);
            if (!value || !value[0]) return false;
            vn = strlen(value);
        }
        if (n + vn + 1 >= cap) return false;
        memcpy(out + n, value, vn);
        n += vn;
        t += tn;
        if (*t == '/') {
            out[n++] = '/';
            t++;
        }
    }
    return snprintf(out + n, cap - n, "%s", rest) < (int)(cap - n);
}

/* A file the emulator keeps in a save folder for itself. */
static bool ignored(const sigil_layout_profiles *row, const char *rest) {
    for (size_t i = 0; i < row->ignored_count; i++) {
        if (strcmp(row->ignored[i], rest) == 0) return true;
    }
    return false;
}

static bool save_id_fits(const sigil_profile_root *p, const char *name) {
    size_t n = strlen(p->save_id);
    if (!n) return false;
    return p->prefix ? strncmp(name, p->save_id, n) == 0 : strcmp(name, p->save_id) == 0;
}

/* ---- where the root sits ------------------------------------------------------- */

static bool to_base(const sigil_profile_root *p, const char *path, char out[SIGIL_SAVE_PATH_MAX]) {
    if (p->above[0]) {
        size_t n = strlen(p->above);
        if (strncmp(path, p->above, n) != 0) return false;
        path += n;
    }
    return snprintf(out, SIGIL_SAVE_PATH_MAX, "%s%s", p->below, path) < SIGIL_SAVE_PATH_MAX;
}

static bool to_root(const sigil_profile_root *p, const char *path, char out[SIGIL_SAVE_PATH_MAX]) {
    size_t n = strlen(p->below);
    if (strncmp(path, p->below, n) != 0) return false;
    return snprintf(out, SIGIL_SAVE_PATH_MAX, "%s%s", p->above, path + n) < SIGIL_SAVE_PATH_MAX;
}

/* The offset of the last segment of `path` named `top`. */
static bool find_top(const char *path, const char *top, size_t *at) {
    size_t n = strlen(top);
    bool found = false;
    for (size_t i = 0; path[i];) {
        size_t seg = strcspn(path + i, "/\\");
        if (seg == n && strncmp(path + i, top, n) == 0) {
            *at = i;
            found = true;
        }
        i += seg;
        if (path[i]) i++;
    }
    return found;
}

/* `path` from `at` on, '/' separated, each segment followed by '/'. */
static bool folder_from(const char *path, size_t at, char out[SIGIL_SAVE_PATH_MAX]) {
    size_t n = 0;
    for (const char *s = path + at; *s;) {
        size_t seg = strcspn(s, "/\\");
        if (seg && !(seg == 1 && s[0] == '.')) {
            if (n + seg + 2 > SIGIL_SAVE_PATH_MAX) return false;
            memcpy(out + n, s, seg);
            n += seg;
            out[n++] = '/';
        }
        s += seg;
        if (*s) s++;
    }
    out[n] = '\0';
    return true;
}

static const sigil_layout_profiles *profiles_row(const char *layout) {
    const sigil_layout *rows[8];
    size_t n = sigil_layout_rows(layout, rows, sizeof(rows) / sizeof(rows[0]));
    for (size_t i = 0; i < n; i++) {
        if (rows[i]->profiles) return rows[i]->profiles;
    }
    return NULL;
}

/* A folder name a device area spells out, where an account area has the
 * profile: Switch's all-zero user, Wii U's common. */
static bool device_folder_name(const sigil_layout_profiles *row, const char *name) {
    size_t n = strlen(name);
    for (size_t i = 0; i < row->area_count; i++) {
        if (row->areas[i].area != SIGIL_SAVE_AREA_DEVICE) continue;
        for (const char *t = row->areas[i].template_; *t;) {
            size_t tn = strcspn(t, "/");
            if (tn == n && strncmp(t, name, n) == 0) return true;
            t += tn;
            if (*t) t++;
        }
    }
    return false;
}

/* The profile whose folder `below` lies in, or "". */
static void implied_profile(const sigil_layout_profiles *row, const char *below, char out[SIGIL_PROFILE_ID_MAX]) {
    out[0] = '\0';
    for (size_t i = 0; i < row->area_count && below[0]; i++) {
        folder_vars v;
        const char *t_end = NULL, *p_end = NULL;
        if (row->areas[i].area != SIGIL_SAVE_AREA_ACCOUNT) continue;
        if (!walk(row->areas[i].template_, below, &v, &t_end, &p_end) || !v.profile[0]) continue;
        if (device_folder_name(row, v.profile)) continue;
        snprintf(out, SIGIL_PROFILE_ID_MAX, "%s", v.profile);
        return;
    }
}

const char *sigil_save_layout_top(const char *layout) {
    const sigil_layout_profiles *row = layout ? profiles_row(layout) : NULL;
    return row ? row->top : NULL;
}

int sigil_save_base(const char *layout, const char *path, char *base, size_t base_cap, char *profile,
                    size_t profile_cap) {
    if (!path || !base || !profile || !base_cap || !profile_cap) return SIGIL_ERR_INVALID_ARG;
    profile[0] = '\0';
    const sigil_layout_profiles *row = layout ? profiles_row(layout) : NULL;
    size_t at = 0, len = strlen(path);
    if (row && find_top(path, row->top, &at)) {
        char below[SIGIL_SAVE_PATH_MAX];
        char implied[SIGIL_PROFILE_ID_MAX];
        if (!folder_from(path, at, below)) return SIGIL_ERR_INVALID_ARG;
        implied_profile(row, below, implied);
        if (strlen(implied) >= profile_cap) return SIGIL_ERR_INVALID_ARG;
        snprintf(profile, profile_cap, "%s", implied);
        len = at;
    }
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\')) len--;
    if (len >= base_cap) return SIGIL_ERR_INVALID_ARG;
    memcpy(base, path, len);
    base[len] = '\0';
    return SIGIL_OK;
}

/* The template's part before its first variable. */
static size_t literal_lead(const char *template_) {
    const char *brace = strchr(template_, '{');
    return brace ? (size_t)(brace - template_) : strlen(template_);
}

/* `path` from its first `top` segment on is a path of the layout. */
static bool layout_path(const sigil_layout_profiles *row, const char *path, size_t *at) {
    size_t n = strlen(row->top);
    for (size_t i = 0; path[i];) {
        size_t seg = strcspn(path + i, "/");
        if (seg == n && strncmp(path + i, row->top, n) == 0) {
            *at = i;
            const char *templates[SIGIL_PROFILES_MAX + 1];
            size_t count = 0;
            templates[count++] = row->list;
            for (size_t a = 0; a < row->area_count && count < SIGIL_PROFILES_MAX + 1; a++) {
                templates[count++] = row->areas[a].template_;
            }
            for (size_t k = 0; k < count; k++) {
                folder_vars v;
                const char *t_end = NULL, *p_end = NULL;
                if (walk(templates[k], path + i, &v, &t_end, &p_end) &&
                    (size_t)(t_end - templates[k]) >= literal_lead(templates[k])) {
                    return true;
                }
            }
            return false;
        }
        i += seg;
        if (path[i]) i++;
    }
    return false;
}

/* With no root path inside the base, the base is the root or a folder under
 * it the listing shows. */
static int find_above(sigil_profile_root *p, char problem[SIGIL_SAVE_PATH_MAX]) {
    const sigil_save_request *req = p->req;
    char seen[4][SIGIL_SAVE_PATH_MAX];
    size_t seen_count = 0;
    bool more = false;
    for (size_t i = 0; i < req->listing_count; i++) {
        size_t at = 0;
        if (!req->listing[i] || !layout_path(p->row, req->listing[i], &at)) continue;
        bool known = false;
        for (size_t s = 0; s < seen_count && !known; s++) {
            known = strlen(seen[s]) == at && strncmp(seen[s], req->listing[i], at) == 0;
        }
        if (known) continue;
        if (seen_count == 4 || at >= SIGIL_SAVE_PATH_MAX) {
            more = true;
            continue;
        }
        memcpy(seen[seen_count], req->listing[i], at);
        seen[seen_count][at] = '\0';
        seen_count++;
    }
    if (seen_count + (more ? 1 : 0) > 1) {
        size_t n = 0;
        problem[0] = '\0';
        for (size_t s = 0; s < seen_count && n < SIGIL_SAVE_PATH_MAX; s++) {
            int w = snprintf(problem + n, SIGIL_SAVE_PATH_MAX - n, "%s%s", s ? "\n" : "", seen[s][0] ? seen[s] : "./");
            if (w > 0) n += (size_t)w;
        }
        return SIGIL_ERR_AMBIGUOUS;
    }
    if (seen_count == 1) snprintf(p->above, sizeof(p->above), "%s", seen[0]);
    return SIGIL_OK;
}

/* The root lies inside one of the game's save folders: a collect there would
 * take part of the save as all of it. */
static bool inside_save_folder(const sigil_profile_root *p) {
    for (size_t i = 0; i < p->row->area_count && p->below[0]; i++) {
        folder_vars v;
        const char *rest = NULL;
        if (in_folder(p->row->areas[i].template_, p->below, &v, &rest) && save_id_fits(p, v.save_id)) return true;
    }
    return false;
}

/* ---- profiles ------------------------------------------------------------------ */

/* `*data` gets the listed file's bytes, or NULL when the root can't reach it,
 * it isn't listed or the request has no `open`. SIGIL_ERR_IO when it is
 * listed and won't open. */
static int read_root_file(const sigil_profile_root *p, const char *base_path, uint8_t **data, size_t *len) {
    char path[SIGIL_SAVE_PATH_MAX];
    *data = NULL;
    *len = 0;
    if (!p->req->open || !to_root(p, base_path, path) || !sigil_save_listed(p->req, path)) return SIGIL_OK;
    sigil_io *io = p->req->open(p->req->open_ctx, path);
    if (!io) return SIGIL_ERR_IO;
    int rc = sigil_bram_read_all(io, PROFILE_LIST_MAX, data, len);
    sigil_io_close(io);
    return rc == SIGIL_ERR_UNSUPPORTED_FORMAT ? SIGIL_OK : rc;
}

static void put_utf8(uint32_t c, char *out, size_t cap, size_t *n) {
    char buf[4];
    size_t k = 0;
    if (c < 0x80) {
        buf[k++] = (char)c;
    } else if (c < 0x800) {
        buf[k++] = (char)(0xC0 | (c >> 6));
        buf[k++] = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        buf[k++] = (char)(0xE0 | (c >> 12));
        buf[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
        buf[k++] = (char)(0x80 | (c & 0x3F));
    } else {
        buf[k++] = (char)(0xF0 | (c >> 18));
        buf[k++] = (char)(0x80 | ((c >> 12) & 0x3F));
        buf[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
        buf[k++] = (char)(0x80 | (c & 0x3F));
    }
    if (*n + k >= cap) return;
    memcpy(out + *n, buf, k);
    *n += k;
    out[*n] = '\0';
}

/* `n` less a UTF-8 sequence its last bytes start but don't finish, so a name
 * cut to fit ends on a whole character. */
static size_t utf8_whole(const char *s, size_t n) {
    size_t lead = n;
    while (lead > 0 && n - lead < 4 && ((unsigned char)s[lead - 1] & 0xC0) == 0x80) lead--;
    if (lead == 0) return n;
    unsigned char c = (unsigned char)s[lead - 1];
    size_t want = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    return n - (lead - 1) < want ? lead - 1 : n;
}

/* Cemu's MiiName: UTF-16BE code units written as hex, ending at a zero unit. */
static void cemu_name(const char *hex, size_t hex_len, char out[SIGIL_PROFILE_NAME_MAX]) {
    size_t n = 0;
    out[0] = '\0';
    uint32_t high = 0;
    for (size_t i = 0; i + 4 <= hex_len; i += 4) {
        unsigned unit = 0;
        char quad[5] = { hex[i], hex[i + 1], hex[i + 2], hex[i + 3], '\0' };
        if (sscanf(quad, "%4x", &unit) != 1 || unit == 0) break;
        if (unit >= 0xD800 && unit < 0xDC00) {
            high = unit;
            continue;
        }
        uint32_t c = unit;
        if (unit >= 0xDC00 && unit < 0xE000) {
            if (!high) continue;
            c = 0x10000 + ((high - 0xD800) << 10) + (unit - 0xDC00);
        }
        high = 0;
        put_utf8(c, out, SIGIL_PROFILE_NAME_MAX, &n);
    }
}

/* The value of `key` (ending in '=') on a line of `text`. */
static const char *line_value(const char *text, size_t len, const char *key, size_t *value_len) {
    size_t k = strlen(key);
    for (size_t i = 0; i + k <= len; i++) {
        if ((i == 0 || text[i - 1] == '\n') && strncmp(text + i, key, k) == 0) {
            size_t end = i + k;
            while (end < len && text[end] != '\n' && text[end] != '\r') end++;
            *value_len = end - i - k;
            return text + i + k;
        }
    }
    return NULL;
}

/* Vita3K's user.xml: the name attribute of <user>, with XML's five escapes. */
static void vita3k_name(const char *xml, size_t len, char out[SIGIL_PROFILE_NAME_MAX]) {
    static const struct { const char *code; char c; } ESCAPES[] = {
        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' },
    };
    out[0] = '\0';
    const char *end = xml + len;
    const char *user = NULL;
    for (const char *s = xml; s + 5 <= end && !user; s++) {
        if (strncmp(s, "<user", 5) == 0) user = s;
    }
    const char *name = NULL;
    for (const char *s = user; s && s + 7 <= end && *s != '>' && !name; s++) {
        if (strncmp(s, " name=\"", 7) == 0) name = s + 7;
    }
    size_t n = 0;
    for (const char *s = name; s && s < end && *s != '"' && n + 1 < SIGIL_PROFILE_NAME_MAX;) {
        char c = *s++;
        if (c == '&') {
            for (size_t e = 0; e < sizeof(ESCAPES) / sizeof(ESCAPES[0]); e++) {
                size_t el = strlen(ESCAPES[e].code) - 1;
                if ((size_t)(end - s) >= el && strncmp(s, ESCAPES[e].code + 1, el) == 0) {
                    c = ESCAPES[e].c;
                    s += el;
                    break;
                }
            }
        }
        out[n++] = c;
    }
    out[utf8_whole(out, n)] = '\0';
}

static void name_from(int format, const uint8_t *data, size_t len, char out[SIGIL_PROFILE_NAME_MAX]) {
    out[0] = '\0';
    if (!data) return;
    const char *text = (const char *)data;
    if (format == SIGIL_PROFILES_CEMU) {
        size_t value_len = 0;
        const char *hex = line_value(text, len, "MiiName=", &value_len);
        if (hex) cemu_name(hex, value_len, out);
    } else if (format == SIGIL_PROFILES_VITA3K) {
        vita3k_name(text, len, out);
    } else {
        size_t n = len < SIGIL_PROFILE_NAME_MAX - 1 ? len : SIGIL_PROFILE_NAME_MAX - 1;
        while (n && isspace((unsigned char)text[n - 1])) n--;
        memcpy(out, text, n);
        out[utf8_whole(out, n)] = '\0';
    }
}

static void add_profile(sigil_profile_root *p, const char *id, const char *name) {
    for (size_t i = 0; i < p->profile_count; i++) {
        if (strcmp(p->profiles[i].id, id) == 0) return;
    }
    if (p->profile_count == SIGIL_PROFILES_MAX) return;
    sigil_save_profile *out = &p->profiles[p->profile_count++];
    snprintf(out->id, sizeof(out->id), "%s", id);
    snprintf(out->name, sizeof(out->name), "%s", name);
}

/* With the list out of the root's reach, each folder an account area names
 * that holds files stands for a profile. */
static void folder_profiles(sigil_profile_root *p) {
    for (size_t i = 0; i < p->req->listing_count; i++) {
        char path[SIGIL_SAVE_PATH_MAX];
        if (!p->req->listing[i] || !to_base(p, p->req->listing[i], path)) continue;
        for (size_t a = 0; a < p->row->area_count; a++) {
            folder_vars v;
            const char *rest = NULL;
            if (p->row->areas[a].area != SIGIL_SAVE_AREA_ACCOUNT) continue;
            if (!in_folder(p->row->areas[a].template_, path, &v, &rest) || ignored(p->row, rest)) continue;
            if (!device_folder_name(p->row, v.profile)) add_profile(p, v.profile, "");
        }
    }
}

static bool list_reachable(const sigil_profile_root *p) {
    char lead[SIGIL_SAVE_PATH_MAX], path[SIGIL_SAVE_PATH_MAX];
    size_t n = literal_lead(p->row->list);
    if (n >= sizeof(lead)) return false;
    memcpy(lead, p->row->list, n);
    lead[n] = '\0';
    return to_root(p, lead, path);
}

static int yuzu_profiles(sigil_profile_root *p) {
    size_t len = 0;
    uint8_t *data = NULL;
    int rc = read_root_file(p, p->row->list, &data, &len);
    for (size_t off = YUZU_HEADER; data && off + YUZU_ENTRY <= len; off += YUZU_ENTRY) {
        const uint8_t *uuid = data + off;
        bool empty = true;
        for (size_t k = 0; k < 16 && empty; k++) empty = uuid[k] == 0;
        if (empty) continue;
        char id[33], name[YUZU_NAME_MAX + 1];
        for (size_t k = 0; k < 16; k++) snprintf(id + 2 * k, 3, "%02X", uuid[15 - k]);
        memcpy(name, data + off + YUZU_NAME_AT, YUZU_NAME_MAX);
        name[YUZU_NAME_MAX] = '\0';
        name[utf8_whole(name, strlen(name))] = '\0';
        add_profile(p, id, name);
    }
    free(data);
    return rc;
}

/* Emulators with one file per profile: each listed file the list template
 * names is a profile, named by its folder. */
static int file_profiles(sigil_profile_root *p) {
    int rc = SIGIL_OK;
    for (size_t i = 0; i < p->req->listing_count && rc == SIGIL_OK; i++) {
        char path[SIGIL_SAVE_PATH_MAX];
        folder_vars v;
        if (!p->req->listing[i] || !to_base(p, p->req->listing[i], path)) continue;
        if (!is_file(p->row->list, path, &v) || !v.profile[0]) continue;
        size_t len = 0;
        uint8_t *data = NULL;
        rc = read_root_file(p, path, &data, &len);
        char name[SIGIL_PROFILE_NAME_MAX];
        name_from(p->row->format, data, len, name);
        free(data);
        add_profile(p, v.profile, name);
    }
    return rc;
}

/* The listed profile `id` names, ignoring case, else `id` as given. */
static void pick(sigil_profile_root *p, const char *id) {
    for (size_t i = 0; i < p->profile_count; i++) {
        if (strlen(p->profiles[i].id) != strlen(id)) continue;
        bool same = true;
        for (size_t k = 0; id[k] && same; k++) {
            same = tolower((unsigned char)id[k]) == tolower((unsigned char)p->profiles[i].id[k]);
        }
        if (same) {
            snprintf(p->profile, sizeof(p->profile), "%s", p->profiles[i].id);
            return;
        }
    }
    snprintf(p->profile, sizeof(p->profile), "%s", id);
}

int sigil_profile_root_open(const sigil_save_request *req, const sigil_layout_profiles *row, sigil_profile_root *p,
                            char problem[SIGIL_SAVE_PATH_MAX]) {
    memset(p, 0, sizeof(*p));
    p->req = req;
    p->row = row;
    if (req->result) snprintf(p->save_id, sizeof(p->save_id), "%s", req->result->save_id);
    p->prefix = row->prefix;

    size_t at = 0;
    if (req->root_path && find_top(req->root_path, row->top, &at)) {
        if (!folder_from(req->root_path, at, p->below)) return SIGIL_ERR_INVALID_ARG;
        if (inside_save_folder(p)) return SIGIL_ERR_INVALID_ARG;
    } else {
        int rc = find_above(p, problem);
        if (rc != SIGIL_OK) return rc;
    }

    int rc = SIGIL_OK;
    if (!list_reachable(p)) folder_profiles(p);
    else if (row->format == SIGIL_PROFILES_YUZU) rc = yuzu_profiles(p);
    else rc = file_profiles(p);
    if (rc != SIGIL_OK) return rc;

    char implied[SIGIL_PROFILE_ID_MAX];
    implied_profile(row, p->below, implied);
    if (req->profile && req->profile[0]) pick(p, req->profile);
    else if (implied[0]) pick(p, implied);
    else if (p->profile_count == 1) pick(p, p->profiles[0].id);
    else p->several = p->profile_count > 1;
    return device_folder_name(row, p->profile) ? SIGIL_ERR_INVALID_ARG : SIGIL_OK;
}

/* ---- files ---------------------------------------------------------------------- */

/* The area folder `base_path` lies in for `profile`, with the file's name in the unit. */
static const sigil_layout_area *area_of(const sigil_profile_root *p, const char *profile, const char *base_path,
                                        char entry[SIGIL_SAVE_ENTRY_MAX]) {
    for (size_t a = 0; a < p->row->area_count; a++) {
        const sigil_layout_area *area = &p->row->areas[a];
        folder_vars v;
        const char *rest = NULL;
        if (area->area == SIGIL_SAVE_AREA_ACCOUNT && !profile[0]) continue;
        if (!in_folder(area->template_, base_path, &v, &rest) || !save_id_fits(p, v.save_id)) continue;
        if (area->area == SIGIL_SAVE_AREA_ACCOUNT && strcmp(v.profile, profile) != 0) continue;
        if (ignored(p->row, rest) || rest[strlen(rest) - 1] == '/') return NULL;
        return expand(area->entry, &v, rest, entry, SIGIL_SAVE_ENTRY_MAX) ? area : NULL;
    }
    return NULL;
}

static int by_entry(const void *a, const void *b) {
    return strcmp(((const sigil_profile_file *)a)->entry, ((const sigil_profile_file *)b)->entry);
}

int sigil_profile_files(const sigil_profile_root *p, const char *profile, sigil_profile_file **out, size_t *count) {
    *count = 0;
    *out = (sigil_profile_file *)calloc(p->req->listing_count ? p->req->listing_count : 1, sizeof(**out));
    if (!*out) return SIGIL_ERR_OOM;
    for (size_t i = 0; i < p->req->listing_count; i++) {
        char base_path[SIGIL_SAVE_PATH_MAX];
        sigil_profile_file *f = &(*out)[*count];
        if (!p->req->listing[i] || !to_base(p, p->req->listing[i], base_path)) continue;
        const sigil_layout_area *area = area_of(p, profile, base_path, f->entry);
        if (!area) continue;
        f->area = area->area;
        snprintf(f->path, sizeof(f->path), "%s", p->req->listing[i]);
        (*count)++;
    }
    qsort(*out, *count, sizeof(**out), by_entry);
    return SIGIL_OK;
}

static bool holds_game(const sigil_profile_root *p, const char *profile) {
    for (size_t i = 0; i < p->req->listing_count; i++) {
        char base_path[SIGIL_SAVE_PATH_MAX], entry[SIGIL_SAVE_ENTRY_MAX];
        if (!p->req->listing[i] || !to_base(p, p->req->listing[i], base_path)) continue;
        const sigil_layout_area *area = area_of(p, profile, base_path, entry);
        if (area && area->area == SIGIL_SAVE_AREA_ACCOUNT) return true;
    }
    return false;
}

bool sigil_profile_undecided(const sigil_profile_root *p) {
    for (size_t i = 0; i < p->profile_count && p->several; i++) {
        if (holds_game(p, p->profiles[i].id)) return true;
    }
    return false;
}

int sigil_profile_place(const sigil_profile_root *p, const char *entry, sigil_profile_file *f, bool *skip) {
    *skip = false;
    memset(f, 0, sizeof(*f));
    for (int legacy = 0; legacy < 2; legacy++) {
        for (size_t a = 0; a < p->row->area_count; a++) {
            const sigil_layout_area *row_area = &p->row->areas[a];
            const char *name = legacy ? row_area->legacy : row_area->entry;
            folder_vars v;
            const char *rest = NULL;
            if (!name || !in_folder(name, entry, &v, &rest) || !save_id_fits(p, v.save_id)) continue;
            f->area = row_area->area;
            if (!expand(row_area->entry, &v, rest, f->entry, sizeof(f->entry))) return SIGIL_ERR_NOT_FOUND;
            if (ignored(p->row, rest)) {
                *skip = true;
                return SIGIL_OK;
            }
            if (row_area->area == SIGIL_SAVE_AREA_ACCOUNT) {
                if (!p->profile[0]) return SIGIL_ERR_NO_TARGET;
                snprintf(v.profile, sizeof(v.profile), "%s", p->profile);
            }
            char base_path[SIGIL_SAVE_PATH_MAX];
            if (!expand(row_area->template_, &v, rest, base_path, sizeof(base_path))) return SIGIL_ERR_NOT_FOUND;
            if (to_root(p, base_path, f->path)) return SIGIL_OK;
            if (!row_area->rebuilt) return SIGIL_ERR_NO_TARGET;
            f->path[0] = '\0';
            *skip = true;
            return SIGIL_OK;
        }
    }
    return SIGIL_ERR_NOT_FOUND;
}

int sigil_save_profiles(const sigil_save_request *req, sigil_save_profile **out, size_t *count) {
    if (!req || !out || !count || req->struct_version != SIGIL_SAVE_REQUEST_V1 || !req->listing) {
        return SIGIL_ERR_INVALID_ARG;
    }
    *out = NULL;
    *count = 0;
    const sigil_layout_profiles *row = req->layout ? profiles_row(req->layout) : NULL;
    if (!row) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    sigil_profile_root p;
    char problem[SIGIL_SAVE_PATH_MAX];
    int rc = sigil_profile_root_open(req, row, &p, problem);
    if (rc != SIGIL_OK) return rc;
    if (!p.profile_count) return SIGIL_OK;
    *out = (sigil_save_profile *)calloc(p.profile_count, sizeof(**out));
    if (!*out) return SIGIL_ERR_OOM;
    memcpy(*out, p.profiles, p.profile_count * sizeof(**out));
    *count = p.profile_count;
    return SIGIL_OK;
}

void sigil_save_profiles_free(sigil_save_profile *profiles) {
    free(profiles);
}

void sigil_profile_lines(const sigil_profile_root *p, char *out, size_t cap) {
    size_t n = 0;
    out[0] = '\0';
    for (size_t i = 0; i < p->profile_count && n < cap; i++) {
        int w = snprintf(out + n, cap - n, "%s%s %s", i ? "\n" : "", p->profiles[i].id, p->profiles[i].name);
        if (w > 0) n += (size_t)w;
    }
}
