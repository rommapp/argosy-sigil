// SPDX-License-Identifier: MPL-2.0
#include "save_profiles.h"
#include "card_saturn.h"
#include <ctype.h>
#include <inttypes.h>
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
    char extdata_id[16];
} folder_vars;

static char *var_slot(folder_vars *v, const char *t, size_t n, size_t *cap) {
    if (n == 12 && strncmp(t, "{extdata_id}", 12) == 0) {
        *cap = sizeof(v->extdata_id);
        return v->extdata_id;
    }
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

/* The length of the first `segments` segments of `p`, '/'s between them included. */
static size_t segments_len(const char *p, size_t segments) {
    size_t n = strcspn(p, "/");
    for (size_t s = 1; s < segments && p[n] == '/' && p[n + 1]; s++) n += 1 + strcspn(p + n + 1, "/");
    return n;
}

/* Walks `template_` and `path` a segment at a time, binding each variable to
 * the segment it stands for ({save_id} to the row's number of them), until
 * either runs out. `*t_end` and `*p_end` get how far each got. False at the
 * first segment that differs. */
static bool walk(const sigil_layout_profiles *row, const char *template_, const char *path, folder_vars *v,
                 const char **t_end, const char **p_end) {
    memset(v, 0, sizeof(*v));
    size_t id_segments = row->save_id_segments ? row->save_id_segments : 1;
    const char *t = template_, *p = path;
    while (*t && *p) {
        size_t tn = strcspn(t, "/"), pn = strcspn(p, "/");
        if (pn == 0) return false;
        if (tn == 9 && strncmp(t, "{save_id}", 9) == 0) pn = segments_len(p, id_segments);
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
static bool in_folder(const sigil_layout_profiles *row, const char *template_, const char *path, folder_vars *v,
                      const char **rest) {
    const char *t_end = NULL;
    return walk(row, template_, path, v, &t_end, rest) && !*t_end && **rest;
}

/* `path` is file `template_`. */
static bool is_file(const sigil_layout_profiles *row, const char *template_, const char *path, folder_vars *v) {
    const char *t_end = NULL, *p_end = NULL;
    return walk(row, template_, path, v, &t_end, &p_end) && !*t_end && !*p_end;
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

/* A 3DS title's extra data folder: the low half of its id shifted right 8
 * bits, as 8 lowercase hex digits (00113200 gives 00001132); "" when the
 * save id has no hex low half. */
static void extdata_id_of(const char *save_id, char out[16]) {
    const char *low = strrchr(save_id, '/');
    low = low ? low + 1 : save_id;
    char *end = NULL;
    unsigned long value = strtoul(low, &end, 16);
    if (!*low || *end) {
        out[0] = '\0';
        return;
    }
    snprintf(out, 16, "%08lx", value >> 8);
}

/* The folder a walk bound is the game's: by its save id, or by the extdata id
 * the save id gives. A name holding neither, an older unit's, stands for the
 * game. Fills in whichever `v` lacks, so a template with either expands. */
static bool game_fits(const sigil_profile_root *p, folder_vars *v) {
    char extdata[16];
    extdata_id_of(p->save_id, extdata);
    if (!p->save_id[0]) return false;
    if (v->save_id[0] && !save_id_fits(p, v->save_id)) return false;
    if (!v->save_id[0] && v->extdata_id[0] && strcmp(v->extdata_id, extdata) != 0) return false;
    if (!v->save_id[0]) snprintf(v->save_id, sizeof(v->save_id), "%s", p->save_id);
    if (!v->extdata_id[0]) snprintf(v->extdata_id, sizeof(v->extdata_id), "%s", extdata);
    return true;
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
        if (!walk(row, row->areas[i].template_, below, &v, &t_end, &p_end) || !v.profile[0]) continue;
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
            if (row->list) templates[count++] = row->list;
            for (size_t a = 0; a < row->area_count && count < SIGIL_PROFILES_MAX + 1; a++) {
                templates[count++] = row->areas[a].template_;
            }
            for (size_t k = 0; k < count; k++) {
                folder_vars v;
                const char *t_end = NULL, *p_end = NULL;
                if (walk(row, templates[k], path + i, &v, &t_end, &p_end) &&
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
        if (in_folder(p->row, p->row->areas[i].template_, p->below, &v, &rest) && game_fits(p, &v)) return true;
    }
    return false;
}

/* ---- profiles ------------------------------------------------------------------ */

/* `*data` gets the listed file's bytes, or NULL when the root can't reach it,
 * it isn't listed, it is over `cap` bytes or the request has no `open`.
 * SIGIL_ERR_IO when it is listed and won't open. */
static int read_root_file(const sigil_profile_root *p, const char *base_path, size_t cap, uint8_t **data,
                          size_t *len) {
    char path[SIGIL_SAVE_PATH_MAX];
    *data = NULL;
    *len = 0;
    if (!p->req->open || !to_root(p, base_path, path) || !sigil_save_listed(p->req, path)) return SIGIL_OK;
    sigil_io *io = p->req->open(p->req->open_ctx, path);
    if (!io) return SIGIL_ERR_IO;
    int rc = sigil_bram_read_all(io, cap, data, len);
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
            if (!in_folder(p->row, p->row->areas[a].template_, path, &v, &rest) || ignored(p->row, rest)) continue;
            if (!device_folder_name(p->row, v.profile)) add_profile(p, v.profile, "");
        }
    }
}

static bool list_reachable(const sigil_profile_root *p) {
    char lead[SIGIL_SAVE_PATH_MAX], path[SIGIL_SAVE_PATH_MAX];
    if (!p->row->list) return false;
    size_t n = literal_lead(p->row->list);
    if (n >= sizeof(lead)) return false;
    memcpy(lead, p->row->list, n);
    lead[n] = '\0';
    return to_root(p, lead, path);
}

static int yuzu_profiles(sigil_profile_root *p) {
    size_t len = 0;
    uint8_t *data = NULL;
    int rc = read_root_file(p, p->row->list, PROFILE_LIST_MAX, &data, &len);
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

/* The JSON string starting at the quote `s` points to, unescaped into `out`;
 * the byte after its closing quote, or NULL when it doesn't close. */
static const char *json_string(const char *s, const char *end, char *out, size_t cap) {
    size_t n = 0;
    out[0] = '\0';
    if (s >= end || *s++ != '"') return NULL;
    uint32_t high = 0;
    while (s < end && *s != '"') {
        if (*s != '\\') {
            if (n + 1 < cap) out[n++] = *s;
            s++;
            continue;
        }
        if (++s >= end) return NULL;
        char e = *s++;
        uint32_t c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : (uint8_t)e;
        if (e == 'u') {
            unsigned unit = 0;
            char quad[5] = { 0 };
            if (end - s < 4) return NULL;
            memcpy(quad, s, 4);
            s += 4;
            if (sscanf(quad, "%4x", &unit) != 1) return NULL;
            if (unit >= 0xD800 && unit < 0xDC00) {
                high = unit;
                continue;
            }
            c = unit >= 0xDC00 && unit < 0xE000 && high ? 0x10000 + ((high - 0xD800) << 10) + (unit - 0xDC00) : unit;
        }
        high = 0;
        out[n] = '\0';
        put_utf8(c, out, cap, &n);
    }
    if (s >= end) return NULL;
    out[utf8_whole(out, n)] = '\0';
    return s + 1;
}

/* The string value of the first `"key":` in [s, end), into `out`. */
static bool json_value(const char *s, const char *end, const char *key, char *out, size_t cap) {
    char quoted[32];
    int k = snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    for (; s + k <= end; s++) {
        if (strncmp(s, quoted, (size_t)k) != 0) continue;
        const char *v = s + k;
        while (v < end && (isspace((unsigned char)*v) || *v == ':')) v++;
        return json_string(v, end, out, cap) != NULL;
    }
    return false;
}

/* Ryujinx's Profiles.json: each "user_id" and the "name" that follows it. */
static int ryujinx_profiles(sigil_profile_root *p) {
    size_t len = 0;
    uint8_t *data = NULL;
    int rc = read_root_file(p, p->row->list, PROFILE_LIST_MAX, &data, &len);
    char *text = data ? (char *)malloc(len + 1) : NULL;
    if (text) {
        memcpy(text, data, len);
        text[len] = '\0';
    }
    const char *end = text ? text + len : NULL;
    for (const char *s = text; text && s < end;) {
        const char *at = strstr(s, "\"user_id\"");
        if (!at || at >= end) break;
        const char *next = strstr(at + 1, "\"user_id\"");
        const char *object_end = next && next < end ? next : end;
        char id[SIGIL_PROFILE_ID_MAX], name[SIGIL_PROFILE_NAME_MAX];
        if (json_value(at, object_end, "user_id", id, sizeof(id))) {
            if (!json_value(at, object_end, "name", name, sizeof(name))) name[0] = '\0';
            add_profile(p, id, name);
        }
        s = object_end;
    }
    free(text);
    free(data);
    return rc;
}

/* ---- the save index ------------------------------------------------------------ */

#define KVDB_MAGIC   0x564B4D49u   /* "IMKV" */
#define KVDB_ENTRY   0x4E454D49u   /* "IMEN" */
#define KVDB_HEADER  0xCu
#define KVDB_KEY     0x40u
#define KVDB_VALUE   0x40u
#define INDEX_MAX    (16u * 1024u * 1024u)
#define INDEX_ACCOUNT 1
#define INDEX_DEVICE  3
#define INDEX_SPACE_USER 1
#define INDEX_DELETED 3

/* One entry of a KeyValueArchive: "IMEN", the key's and the value's sizes,
 * then both. */
typedef struct {
    const uint8_t *start, *key, *value;
    uint32_t       key_len, value_len;
    size_t         size;   /* the whole entry's bytes */
} kvdb_entry;

/* The archive's entry at `*at`, moving `*at` past it; false at the end of
 * the archive or at an entry that runs past it. */
static bool kvdb_next(const uint8_t *data, size_t len, size_t *at, kvdb_entry *e) {
    if (*at + KVDB_HEADER > len || sigil_read_le32(data + *at) != KVDB_ENTRY) return false;
    e->key_len = sigil_read_le32(data + *at + 4);
    e->value_len = sigil_read_le32(data + *at + 8);
    if (e->key_len > len || e->value_len > len || *at + KVDB_HEADER + e->key_len + e->value_len > len) return false;
    e->start = data + *at;
    e->key = e->start + KVDB_HEADER;
    e->value = e->key + e->key_len;
    e->size = KVDB_HEADER + e->key_len + e->value_len;
    *at += e->size;
    return true;
}

static bool kvdb_valid(const uint8_t *data, size_t len) {
    return data && len >= KVDB_HEADER && sigil_read_le32(data) == KVDB_MAGIC;
}

/* Ryujinx's imkvdb.arc (LibHac KeyValueArchive of SaveDataAttribute keys and
 * SaveDataIndexerValue values): the game's account and device saves in the
 * user space, with each one's folder id and, for an account save, its user
 * as Profiles.json writes it. */
static int load_index(sigil_profile_root *p) {
    size_t len = 0;
    uint8_t *data = NULL;
    int rc = read_root_file(p, p->row->index, INDEX_MAX, &data, &len);
    uint64_t title = 0;
    bool have_title = sscanf(p->save_id, "%16" SCNx64, &title) == 1;
    if (!kvdb_valid(data, len) || !have_title) {
        free(data);
        return rc;
    }
    uint32_t count = sigil_read_le32(data + 8);
    size_t at = KVDB_HEADER;
    kvdb_entry entry;
    for (uint32_t i = 0; i < count && kvdb_next(data, len, &at, &entry); i++) {
        const uint8_t *key = entry.key, *value = entry.value;
        if (entry.key_len < KVDB_KEY || entry.value_len < 0x1A || sigil_read_le64(key) != title) continue;
        int type = key[0x20];
        if ((type != INDEX_ACCOUNT && type != INDEX_DEVICE) || value[0x18] != INDEX_SPACE_USER ||
            value[0x19] == INDEX_DELETED || p->indexed_count == SIGIL_PROFILE_INDEX_MAX) {
            continue;
        }
        sigil_profile_indexed *e = &p->indexed[p->indexed_count++];
        e->id = sigil_read_le64(value);
        e->area = type == INDEX_ACCOUNT ? SIGIL_SAVE_AREA_ACCOUNT : SIGIL_SAVE_AREA_DEVICE;
        e->profile[0] = '\0';
        if (type == INDEX_ACCOUNT) {
            snprintf(e->profile, sizeof(e->profile), "%016" PRIx64 "%016" PRIx64, sigil_read_le64(key + 0x08),
                     sigil_read_le64(key + 0x10));
        }
    }
    free(data);
    return SIGIL_OK;
}

/* The folder `path` (relative to the base) names when it is
 * <saves>/<16 hex>/0/<file>: its id, and the file inside. */
static bool indexed_path(const char *path, uint64_t *id, const char **rest) {
    static const char SAVES[] = "bis/user/save/";
    size_t n = sizeof(SAVES) - 1;
    if (strncmp(path, SAVES, n) != 0 || strlen(path) < n + 19) return false;
    for (size_t i = 0; i < 16; i++) {
        if (!isxdigit((unsigned char)path[n + i])) return false;
    }
    if (strncmp(path + n + 16, "/0/", 3) != 0 || !path[n + 19] || path[strlen(path) - 1] == '/') return false;
    if (sscanf(path + n, "%16" SCNx64, id) != 1) return false;
    *rest = path + n + 19;
    return true;
}

static const sigil_profile_indexed *indexed_save(const sigil_profile_root *p, int area, const char *profile) {
    for (size_t i = 0; i < p->indexed_count; i++) {
        const sigil_profile_indexed *e = &p->indexed[i];
        if (e->area == area && (area == SIGIL_SAVE_AREA_DEVICE || strcasecmp(e->profile, profile) == 0)) return e;
    }
    return NULL;
}

/* LibHac's SaveDataAttribute order, which the index keeps its entries in:
 * program id, type, user (high, then low), static id, index, rank. */
static int key_order(const uint8_t *a, const uint8_t *b) {
    static const struct { uint8_t at, size; } FIELDS[] = {
        { 0x00, 8 }, { 0x20, 1 }, { 0x08, 8 }, { 0x10, 8 }, { 0x18, 8 }, { 0x22, 2 }, { 0x21, 1 },
    };
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
        const uint8_t *x = a + FIELDS[i].at, *y = b + FIELDS[i].at;
        uint64_t u = FIELDS[i].size == 8 ? sigil_read_le64(x) : FIELDS[i].size == 2 ? sigil_read_le16(x) : *x;
        uint64_t v = FIELDS[i].size == 8 ? sigil_read_le64(y) : FIELDS[i].size == 2 ? sigil_read_le16(y) : *y;
        if (u != v) return u < v ? -1 : 1;
    }
    return 0;
}

/* The highest save folder id the counter, the index or a listed folder under
 * bis/user/save/ holds. The emulator wipes the folder of an id it publishes
 * again, so a new id goes past all three. */
static uint64_t highest_id(const sigil_profile_root *p, const uint8_t *index, size_t len, const uint8_t *counter,
                           size_t counter_len) {
    static const char SAVES[] = "bis/user/save/";
    uint64_t top = counter_len == 8 ? sigil_read_le64(counter) : 0;
    size_t at = KVDB_HEADER;
    kvdb_entry e;
    while (kvdb_next(index, len, &at, &e)) {
        if (e.value_len >= 8 && sigil_read_le64(e.value) > top) top = sigil_read_le64(e.value);
    }
    for (size_t i = 0; i < p->req->listing_count; i++) {
        char path[SIGIL_SAVE_PATH_MAX];
        uint64_t id = 0;
        size_t n = sizeof(SAVES) - 1;
        if (!p->req->listing[i] || !to_base(p, p->req->listing[i], path) || strncmp(path, SAVES, n) != 0) continue;
        bool hex = strlen(path) > n + 16 && path[n + 16] == '/';
        for (size_t k = 0; k < 16 && hex; k++) hex = isxdigit((unsigned char)path[n + k]) != 0;
        if (hex && sscanf(path + n, "%16" SCNx64, &id) == 1 && id > top) top = id;
    }
    return top;
}

/* A new entry's key and value: `type` for program `title`, under `profile`
 * (32 hex, "" for none), with folder `id` in the user space. */
static bool make_entry(uint64_t title, int type, const char *profile, uint64_t id, uint8_t key[KVDB_KEY],
                       uint8_t value[KVDB_VALUE]) {
    memset(key, 0, KVDB_KEY);
    memset(value, 0, KVDB_VALUE);
    sigil_write_le64(key, title);
    if (profile[0]) {
        uint64_t high = 0, low = 0;
        if (strlen(profile) != 32 || sscanf(profile, "%16" SCNx64, &high) != 1 ||
            sscanf(profile + 16, "%16" SCNx64, &low) != 1) {
            return false;
        }
        sigil_write_le64(key + 0x08, high);
        sigil_write_le64(key + 0x10, low);
    }
    key[0x20] = (uint8_t)type;
    sigil_write_le64(value, id);
    value[0x18] = INDEX_SPACE_USER;
    return true;
}

static bool append(uint8_t *out, size_t cap, size_t *n, const void *data, size_t len) {
    if (*n + len > cap) return false;
    memcpy(out + *n, data, len);
    *n += len;
    return true;
}

static bool append_entry(uint8_t *out, size_t cap, size_t *n, const uint8_t *key, const uint8_t *value) {
    uint8_t head[KVDB_HEADER];
    sigil_write_le32(head, KVDB_ENTRY);
    sigil_write_le32(head + 4, KVDB_KEY);
    sigil_write_le32(head + 8, KVDB_VALUE);
    return append(out, cap, n, head, sizeof(head)) && append(out, cap, n, key, KVDB_KEY) &&
           append(out, cap, n, value, KVDB_VALUE);
}

/* `index` with `count` new entries, keys ascending, put in key order among
 * its own; NULL when an entry runs past the archive or a new key is taken. */
static uint8_t *index_with(const uint8_t *index, size_t len, uint8_t (*keys)[KVDB_KEY],
                           uint8_t (*values)[KVDB_VALUE], size_t count, size_t *out_len) {
    uint32_t have = sigil_read_le32(index + 8);
    size_t cap = len + count * (KVDB_HEADER + KVDB_KEY + KVDB_VALUE), n = 0, at = KVDB_HEADER, next = 0;
    uint8_t *out = (uint8_t *)malloc(cap);
    bool ok = out && append(out, cap, &n, index, 8);
    uint8_t total[4];
    sigil_write_le32(total, have + (uint32_t)count);
    ok = ok && append(out, cap, &n, total, 4);
    kvdb_entry e;
    for (uint32_t i = 0; ok && i < have; i++) {
        ok = kvdb_next(index, len, &at, &e) && e.key_len >= KVDB_KEY;
        for (size_t k = 0; ok && k < count; k++) ok = key_order(keys[k], e.key) != 0;
        while (ok && next < count && key_order(keys[next], e.key) < 0) {
            ok = append_entry(out, cap, &n, keys[next], values[next]);
            next++;
        }
        ok = ok && append(out, cap, &n, e.start, e.size);
    }
    for (; ok && next < count; next++) ok = append_entry(out, cap, &n, keys[next], values[next]);
    if (!ok) {
        free(out);
        return NULL;
    }
    *out_len = n;
    return out;
}

int sigil_profile_publish(sigil_profile_root *p, bool account, bool device, sigil_profile_published *out) {
    memset(out, 0, sizeof(*out));
    account = account && p->profile[0] && !indexed_save(p, SIGIL_SAVE_AREA_ACCOUNT, p->profile);
    device = device && !indexed_save(p, SIGIL_SAVE_AREA_DEVICE, "");
    if (!p->row->index || (!account && !device)) return SIGIL_OK;

    char counter_base[SIGIL_SAVE_PATH_MAX];
    const char *slash = strrchr(p->row->index, '/');
    int w = snprintf(counter_base, sizeof(counter_base), "%.*slastPublishedId",
                     slash ? (int)(slash - p->row->index + 1) : 0, p->row->index);
    uint64_t title = 0;
    if (w < 0 || w >= (int)sizeof(counter_base) || sscanf(p->save_id, "%16" SCNx64, &title) != 1 ||
        !to_root(p, p->row->index, out->index_path) || !to_root(p, counter_base, out->counter_path) ||
        p->indexed_count + 2 > SIGIL_PROFILE_INDEX_MAX) {
        return SIGIL_ERR_NO_TARGET;
    }

    uint8_t *index = NULL, *counter = NULL;
    size_t len = 0, counter_len = 0;
    int rc = read_root_file(p, p->row->index, INDEX_MAX, &index, &len);
    if (rc == SIGIL_OK) rc = read_root_file(p, counter_base, 8, &counter, &counter_len);
    if (rc == SIGIL_OK && !kvdb_valid(index, len)) rc = SIGIL_ERR_NO_TARGET;

    uint8_t keys[2][KVDB_KEY], values[2][KVDB_VALUE];
    size_t count = 0;
    uint64_t id = rc == SIGIL_OK ? highest_id(p, index, len, counter, counter_len) : 0;
    if (rc == SIGIL_OK && account) {
        if (!make_entry(title, INDEX_ACCOUNT, p->profile, ++id, keys[count], values[count])) rc = SIGIL_ERR_NO_TARGET;
        count++;
    }
    if (rc == SIGIL_OK && device) {
        make_entry(title, INDEX_DEVICE, "", ++id, keys[count], values[count]);
        count++;
    }
    if (rc == SIGIL_OK) {
        out->index = index_with(index, len, keys, values, count, &out->index_len);
        if (!out->index) rc = SIGIL_ERR_NO_TARGET;
    }
    free(index);
    free(counter);
    if (rc != SIGIL_OK) {
        sigil_profile_published_free(out);
        return rc;
    }
    sigil_write_le64(out->counter, id);
    for (size_t i = 0; i < count; i++) {
        sigil_profile_indexed *e = &p->indexed[p->indexed_count++];
        e->id = sigil_read_le64(values[i]);
        e->area = keys[i][0x20] == INDEX_ACCOUNT ? SIGIL_SAVE_AREA_ACCOUNT : SIGIL_SAVE_AREA_DEVICE;
        snprintf(e->profile, sizeof(e->profile), "%s", e->area == SIGIL_SAVE_AREA_ACCOUNT ? p->profile : "");
    }
    return SIGIL_OK;
}

void sigil_profile_published_free(sigil_profile_published *out) {
    free(out->index);
    memset(out, 0, sizeof(*out));
}

/* Emulators with one file per profile: each listed file the list template
 * names is a profile, named by its folder. */
static int file_profiles(sigil_profile_root *p) {
    int rc = SIGIL_OK;
    for (size_t i = 0; i < p->req->listing_count && rc == SIGIL_OK; i++) {
        char path[SIGIL_SAVE_PATH_MAX];
        folder_vars v;
        if (!p->req->listing[i] || !to_base(p, p->req->listing[i], path)) continue;
        if (!is_file(p->row, p->row->list, path, &v) || !v.profile[0]) continue;
        size_t len = 0;
        uint8_t *data = NULL;
        rc = read_root_file(p, path, PROFILE_LIST_MAX, &data, &len);
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
    if (row->fixed_profile) add_profile(p, row->fixed_profile, "");
    else if (!list_reachable(p)) folder_profiles(p);
    else if (row->format == SIGIL_PROFILES_YUZU) rc = yuzu_profiles(p);
    else if (row->format == SIGIL_PROFILES_RYUJINX) rc = ryujinx_profiles(p);
    else rc = file_profiles(p);
    if (rc == SIGIL_OK && row->index) rc = load_index(p);
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

/* `folder` (relative to the base, ending in '/') holds installed game data:
 * its PARAM.SFO reads as an SFO and carries neither key a save's does. A
 * folder without one, or whose one won't open or parse, stays a save. */
static bool game_data_install(const sigil_profile_root *p, const char *folder) {
    char sfo_path[SIGIL_SAVE_PATH_MAX];
    if (snprintf(sfo_path, sizeof(sfo_path), "%sPARAM.SFO", folder) >= (int)sizeof(sfo_path)) return false;
    uint8_t *sfo = NULL;
    size_t len = 0;
    if (read_root_file(p, sfo_path, PROFILE_LIST_MAX, &sfo, &len) != SIGIL_OK || !sfo) {
        free(sfo);
        return false;
    }
    size_t off = 0, size = 0;
    bool install = len >= 4 && sigil_read_le32(sfo) == SIGIL_SFO_MAGIC &&
                   sigil_sfo_find(sfo, len, "SAVEDATA_PARAMS", &off, &size) != SIGIL_OK &&
                   sigil_sfo_find(sfo, len, "SAVEDATA_FILE_LIST", &off, &size) != SIGIL_OK;
    free(sfo);
    return install;
}

static const sigil_layout_area INDEXED_ACCOUNT = { NULL, NULL, SIGIL_SAVE_AREA_ACCOUNT, false, NULL };
static const sigil_layout_area INDEXED_DEVICE = { NULL, NULL, SIGIL_SAVE_AREA_DEVICE, false, NULL };

/* On a row with a save index: a file of a folder the index gives the game,
 * named in the unit as the yuzu forks name it, <title>/... for the account
 * save of `profile` and device/<title>/... for the device save. Only the
 * folder's committed copy (0/) is the save. */
static const sigil_layout_area *indexed_area_of(const sigil_profile_root *p, const char *profile,
                                                const char *base_path, char entry[SIGIL_SAVE_ENTRY_MAX]) {
    uint64_t id = 0;
    const char *rest = NULL;
    if (!indexed_path(base_path, &id, &rest)) return NULL;
    for (size_t i = 0; i < p->indexed_count; i++) {
        const sigil_profile_indexed *e = &p->indexed[i];
        if (e->id != id) continue;
        bool device = e->area == SIGIL_SAVE_AREA_DEVICE;
        if (!device && (!profile[0] || strcasecmp(e->profile, profile) != 0)) return NULL;
        int n = snprintf(entry, SIGIL_SAVE_ENTRY_MAX, "%s%s/%s", device ? "device/" : "", p->save_id, rest);
        if (n < 0 || n >= SIGIL_SAVE_ENTRY_MAX) return NULL;
        return device ? &INDEXED_DEVICE : &INDEXED_ACCOUNT;
    }
    return NULL;
}

/* Where unit member `entry` goes on a row with a save index: the folder the
 * index gives the game's account save of the chosen profile, or its device
 * save. A save the index has no folder for, after sigil_profile_publish, is
 * SIGIL_ERR_NO_TARGET. */
static int indexed_place(const sigil_profile_root *p, const char *entry, sigil_profile_file *f) {
    size_t n = strlen(p->save_id);
    bool device = strncmp(entry, "device/", 7) == 0;
    const char *in = device ? entry + 7 : entry;
    if (!n || strncmp(in, p->save_id, n) != 0 || in[n] != '/' || !in[n + 1]) return SIGIL_ERR_NOT_FOUND;
    f->area = device ? SIGIL_SAVE_AREA_DEVICE : SIGIL_SAVE_AREA_ACCOUNT;
    snprintf(f->entry, sizeof(f->entry), "%s", entry);
    if (!device && !p->profile[0]) return SIGIL_ERR_NO_TARGET;
    const sigil_profile_indexed *e = indexed_save(p, f->area, p->profile);
    if (!e) return SIGIL_ERR_NO_TARGET;
    char base_path[SIGIL_SAVE_PATH_MAX];
    int w = snprintf(base_path, sizeof(base_path), "bis/user/save/%016" PRIx64 "/0/%s", e->id, in + n + 1);
    if (w < 0 || w >= (int)sizeof(base_path) || !to_root(p, base_path, f->path)) return SIGIL_ERR_NO_TARGET;
    return SIGIL_OK;
}

/* The area folder `base_path` lies in for `profile`, with the file's name in the unit. */
static const sigil_layout_area *area_of(const sigil_profile_root *p, const char *profile, const char *base_path,
                                        char entry[SIGIL_SAVE_ENTRY_MAX]) {
    if (p->row->index) return indexed_area_of(p, profile, base_path, entry);
    for (size_t a = 0; a < p->row->area_count; a++) {
        const sigil_layout_area *area = &p->row->areas[a];
        folder_vars v;
        const char *rest = NULL;
        if (area->area == SIGIL_SAVE_AREA_ACCOUNT && !profile[0]) continue;
        if (!in_folder(p->row, area->template_, base_path, &v, &rest) || !game_fits(p, &v)) continue;
        if (area->area == SIGIL_SAVE_AREA_ACCOUNT && strcmp(v.profile, profile) != 0) continue;
        if (ignored(p->row, rest) || rest[strlen(rest) - 1] == '/') return NULL;
        char folder[SIGIL_SAVE_PATH_MAX];
        if (p->row->savedata_only && expand(area->template_, &v, "", folder, sizeof(folder)) &&
            game_data_install(p, folder)) {
            return NULL;
        }
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
        f->folder = p->row->index ? NULL : area;
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
    if (p->row->index) return indexed_place(p, entry, f);
    for (int legacy = 0; legacy < 2; legacy++) {
        for (size_t a = 0; a < p->row->area_count; a++) {
            const sigil_layout_area *row_area = &p->row->areas[a];
            const char *name = legacy ? row_area->legacy : row_area->entry;
            folder_vars v;
            const char *rest = NULL;
            if (!name || !in_folder(p->row, name, entry, &v, &rest) || !game_fits(p, &v)) continue;
            f->area = row_area->area;
            f->folder = row_area;
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
