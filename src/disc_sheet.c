// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define SHEET_MAX_BYTES     (64 * 1024)
#define SHEET_NAME_CAP      512
#define GD_HIGH_DENSITY_LBA 45000u
#define GDI_DATA_TYPE       4

typedef struct {
    char first[SHEET_NAME_CAP];
    char high_density[SHEET_NAME_CAP];
} track_pick;

static const char *skip_space(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static bool read_token(const char **cursor, char *out, size_t cap) {
    const char *p = skip_space(*cursor);
    size_t n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (n + 1 >= cap) return false;
            out[n++] = *p++;
        }
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') {
            if (n + 1 >= cap) return false;
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
    *cursor = p;
    return n > 0;
}

static bool token_is(const char *token, const char *word) {
    while (*token && *word) {
        if (toupper((unsigned char)*token) != *word) return false;
        token++;
        word++;
    }
    return *token == '\0' && *word == '\0';
}

static bool token_starts(const char *token, const char *prefix) {
    while (*prefix) {
        if (toupper((unsigned char)*token) != *prefix) return false;
        token++;
        prefix++;
    }
    return true;
}

static void keep(char dst[SHEET_NAME_CAP], const char *name) {
    if (dst[0] != '\0') return;
    size_t len = strlen(name);
    if (len >= SHEET_NAME_CAP) return;
    memcpy(dst, name, len + 1);
}

static void parse_cue_line(const char *line, char *current_file, bool *high_density,
                           track_pick *pick) {
    char word[SHEET_NAME_CAP];
    const char *p = line;
    if (!read_token(&p, word, sizeof(word))) return;
    if (token_is(word, "FILE")) {
        if (!read_token(&p, current_file, SHEET_NAME_CAP)) current_file[0] = '\0';
        return;
    }
    if (token_is(word, "REM")) {
        if (read_token(&p, word, sizeof(word)) && token_is(word, "HIGH-DENSITY")) {
            *high_density = true;
        }
        return;
    }
    if (!token_is(word, "TRACK") || current_file[0] == '\0') return;
    if (!read_token(&p, word, sizeof(word)) || !read_token(&p, word, sizeof(word))) return;
    if (!token_starts(word, "MODE")) return;
    keep(pick->first, current_file);
    if (*high_density) keep(pick->high_density, current_file);
}

static void parse_gdi_line(const char *line, track_pick *pick) {
    char number[32], lba[32], type[32], size[32], name[SHEET_NAME_CAP];
    const char *p = line;
    if (!read_token(&p, number, sizeof(number)) || !read_token(&p, lba, sizeof(lba))
        || !read_token(&p, type, sizeof(type)) || !read_token(&p, size, sizeof(size))
        || !read_token(&p, name, sizeof(name))) {
        return;
    }
    if (strtol(type, NULL, 10) != GDI_DATA_TYPE) return;
    keep(pick->first, name);
    if (strtoul(lba, NULL, 10) >= GD_HIGH_DENSITY_LBA) keep(pick->high_density, name);
}

static char *read_sheet(const char *path) {
    FILE *fp = sigil_fopen(path, "rb");
    if (!fp) return NULL;
    char *text = (char *)malloc(SHEET_MAX_BYTES + 1);
    if (!text) { fclose(fp); return NULL; }
    size_t got = fread(text, 1, SHEET_MAX_BYTES, fp);
    fclose(fp);
    text[got] = '\0';
    return text;
}

static bool is_absolute(const char *name) {
#ifdef _WIN32
    if (name[0] == '\\' || (name[0] && name[1] == ':')) return true;
#endif
    return name[0] == '/';
}

static int join_beside(const char *sheet_path, const char *name, char *out, size_t cap) {
    size_t dir_len = 0;
    if (!is_absolute(name)) {
        const char *slash = strrchr(sheet_path, '/');
#ifdef _WIN32
        const char *bslash = strrchr(sheet_path, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
        if (slash) dir_len = (size_t)(slash - sheet_path) + 1;
    }
    size_t name_len = strlen(name);
    if (dir_len + name_len + 1 > cap) return SIGIL_ERR_INVALID_ARG;
    memcpy(out, sheet_path, dir_len);
    memcpy(out + dir_len, name, name_len + 1);
    return SIGIL_OK;
}

int sigil_sheet_data_track(const char *sheet_path, bool prefer_high_density,
                           char *out, size_t cap) {
    if (!sheet_path || !out || cap == 0) return SIGIL_ERR_INVALID_ARG;
    char ext[16];
    sigil_lower_ext(sheet_path, ext);
    bool gdi = strcmp(ext, "gdi") == 0;
    if (!gdi && strcmp(ext, "cue") != 0) return SIGIL_ERR_INVALID_ARG;

    char *text = read_sheet(sheet_path);
    if (!text) return SIGIL_ERR_IO;

    track_pick pick;
    memset(&pick, 0, sizeof(pick));
    char current_file[SHEET_NAME_CAP] = "";
    bool high_density = false;
    bool first_line = true;

    char *line = text;
    if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB
        && (unsigned char)line[2] == 0xBF) {
        line += 3;
    }
    while (line && *line) {
        char *end = strpbrk(line, "\r\n");
        char *next = NULL;
        if (end) {
            next = end + 1;
            *end = '\0';
        }
        if (*skip_space(line) != '\0') {
            if (gdi) {
                if (!first_line) parse_gdi_line(line, &pick);
                first_line = false;
            } else {
                parse_cue_line(line, current_file, &high_density, &pick);
            }
        }
        line = next;
    }
    free(text);

    const char *chosen = (prefer_high_density && pick.high_density[0]) ? pick.high_density
                                                                       : pick.first;
    if (chosen[0] == '\0') return SIGIL_ERR_NOT_FOUND;
    return join_beside(sheet_path, chosen, out, cap);
}
