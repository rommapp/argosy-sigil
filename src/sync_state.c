// SPDX-License-Identifier: MPL-2.0
/* The state blob: text, one fact per line, so a stored blob can be read when
 * something goes wrong. The caller treats it as opaque. Lines this version
 * doesn't know are kept. Fields are tab-separated, with '%', tab and newline
 * escaped as %XX.
 *
 *   sigil-state 1
 *   synced   <game>  <identity>          the game's saves as last collected or restored
 *   restored <game>  <new> <old>         unmanaged: the last restore and what it replaced
 *   owner    <volume> <save name> <game> the game a save on a shared volume belongs to
 *   prepared <volume> <game>             managed: the volume was swapped in for this game
 *   held     <volume> <identity>         the saves with no known owner passed on in a holding unit
 *   seen     <volume> <identity>         every save on the volume at the last collect or restore
 */
#include "sync_internal.h"

#define STATE_MAGIC "sigil-state 1\n"

void sigil_sync_state_free(sigil_sync_state *s) {
    for (size_t i = 0; i < s->count; i++) free(s->lines[i]);
    free(s->lines);
    memset(s, 0, sizeof(*s));
}

static int append(sigil_sync_state *s, const char *text, size_t len) {
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16;
        char **grown = (char **)realloc(s->lines, cap * sizeof(*grown));
        if (!grown) return SIGIL_ERR_OOM;
        s->lines = grown;
        s->cap = cap;
    }
    char *line = (char *)malloc(len + 1);
    if (!line) return SIGIL_ERR_OOM;
    memcpy(line, text, len);
    line[len] = '\0';
    s->lines[s->count++] = line;
    return SIGIL_OK;
}

int sigil_sync_state_parse(sigil_sync_state *s, const uint8_t *blob, size_t len) {
    memset(s, 0, sizeof(*s));
    size_t magic = strlen(STATE_MAGIC);
    if (!blob || len < magic || memcmp(blob, STATE_MAGIC, magic) != 0) return SIGIL_OK;
    const char *p = (const char *)blob + magic, *end = (const char *)blob + len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t line = (size_t)((eol ? eol : end) - p);
        if (line > 0 && append(s, p, line) != SIGIL_OK) { sigil_sync_state_free(s); return SIGIL_ERR_OOM; }
        p += line + 1;
    }
    return SIGIL_OK;
}

/* The rest of the first line starting with `prefix`, or NULL. */
static const char *get_prefixed(const sigil_sync_state *s, const char *prefix) {
    size_t n = strlen(prefix);
    for (size_t i = 0; i < s->count; i++) {
        if (strncmp(s->lines[i], prefix, n) == 0) return s->lines[i] + n;
    }
    return NULL;
}

/* Replaces every line starting with `prefix` by prefix + value; drops them when `value` is NULL. */
static int put_prefixed(sigil_sync_state *s, const char *prefix, const char *value) {
    size_t n = strlen(prefix), kept = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (strncmp(s->lines[i], prefix, n) == 0) free(s->lines[i]);
        else s->lines[kept++] = s->lines[i];
    }
    s->count = kept;
    if (!value) return SIGIL_OK;
    size_t v = strlen(value);
    char *line = (char *)malloc(n + v + 1);
    if (!line) return SIGIL_ERR_OOM;
    memcpy(line, prefix, n);
    memcpy(line + n, value, v + 1);
    int rc = append(s, line, n + v);
    free(line);
    return rc;
}

int sigil_sync_state_blob(const sigil_sync_state *s, uint8_t **out, size_t *len) {
    size_t total = strlen(STATE_MAGIC);
    for (size_t i = 0; i < s->count; i++) total += strlen(s->lines[i]) + 1;
    char *text = (char *)malloc(total);
    if (!text) return SIGIL_ERR_OOM;
    size_t n = strlen(STATE_MAGIC);
    memcpy(text, STATE_MAGIC, n);
    for (size_t i = 0; i < s->count; i++) {
        size_t l = strlen(s->lines[i]);
        memcpy(text + n, s->lines[i], l);
        n += l;
        text[n++] = '\n';
    }
    *out = (uint8_t *)text;
    *len = n;
    return SIGIL_OK;
}

void sigil_sync_escape(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (; *in && n + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '%' || c == '\t' || c == '\n' || c == '\r') n += (size_t)snprintf(out + n, cap - n, "%%%02X", c);
        else out[n++] = (char)c;
    }
    out[n] = '\0';
}

const char *sigil_sync_state_get(const sigil_sync_state *s, const char *tag, const char *key) {
    char prefix[3 * SYNC_KEY_MAX];
    snprintf(prefix, sizeof(prefix), "%s\t%s\t", tag, key);
    return get_prefixed(s, prefix);
}

int sigil_sync_state_put(sigil_sync_state *s, const char *tag, const char *key, const char *value) {
    char prefix[3 * SYNC_KEY_MAX];
    snprintf(prefix, sizeof(prefix), "%s\t%s\t", tag, key);
    return put_prefixed(s, prefix, value);
}

static void owner_prefix(const char *volume, const char *name, char out[3 * SYNC_KEY_MAX]) {
    char escaped[3 * SIGIL_CARD_NAME_MAX];
    sigil_sync_escape(name, escaped, sizeof(escaped));
    snprintf(out, 3 * SYNC_KEY_MAX, "owner\t%s\t%s\t", volume, escaped);
}

const char *sigil_sync_owner_get(const sigil_sync_state *s, const char *volume, const char *name) {
    char prefix[3 * SYNC_KEY_MAX];
    owner_prefix(volume, name, prefix);
    return get_prefixed(s, prefix);
}

int sigil_sync_owner_put(sigil_sync_state *s, const char *volume, const char *name, const char *game) {
    char prefix[3 * SYNC_KEY_MAX];
    owner_prefix(volume, name, prefix);
    return put_prefixed(s, prefix, game);
}
