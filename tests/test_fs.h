// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_TEST_FS_H
#define SIGIL_TEST_FS_H

/* Folder access for the tests on Windows and POSIX alike: open a file, list a
 * folder, make one, make a fresh temporary one, and remove a tree. Paths are
 * UTF-8 everywhere, as sigil takes them. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Called for each entry of a folder but . and ..; return false to stop. */
typedef bool (*test_dir_fn)(void *ctx, const char *name, bool is_dir);

#ifdef _WIN32
#include <windows.h>

#define TEST_PATH_CAP 1024

static bool test_wide(const char *utf8, wchar_t *out) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, TEST_PATH_CAP) > 0;
}

static bool test_utf8(const wchar_t *wide, char *out, size_t cap) {
    return WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)cap, NULL, NULL) > 0;
}

static FILE *test_fopen(const char *path, const char *mode) {
    wchar_t wpath[TEST_PATH_CAP], wmode[16];
    if (!test_wide(path, wpath) || !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 16)) return NULL;
    return _wfopen(wpath, wmode);
}

/* 0, or -1 when `dir` can't be listed. */
static int test_dir_each(const char *dir, test_dir_fn fn, void *ctx) {
    char pattern[TEST_PATH_CAP];
    wchar_t wpattern[TEST_PATH_CAP];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    if (!test_wide(pattern, wpattern)) return -1;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        char name[TEST_PATH_CAP];
        if (!test_utf8(fd.cFileName, name, sizeof(name))) continue;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (!fn(ctx, name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

static int test_make_dir(const char *path) {
    wchar_t wpath[TEST_PATH_CAP];
    if (!test_wide(path, wpath)) return -1;
    return CreateDirectoryW(wpath, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
}

/* The system's folder for temporary files, with no trailing separator. */
static const char *test_temp_root(void) {
    static char base[TEST_PATH_CAP];
    wchar_t wbase[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, wbase);
    if (n == 0 || n > MAX_PATH) return ".";
    if (wbase[n - 1] == L'\\' || wbase[n - 1] == L'/') wbase[n - 1] = L'\0';
    return test_utf8(wbase, base, sizeof(base)) ? base : ".";
}

static int test_temp_dir(char *out, size_t cap) {
    for (unsigned tries = 0; tries < 100; tries++) {
        wchar_t wout[TEST_PATH_CAP];
        snprintf(out, cap, "%s\\sigil-test-%lu-%u", test_temp_root(), (unsigned long)GetCurrentProcessId(), tries);
        if (test_wide(out, wout) && CreateDirectoryW(wout, NULL)) return 0;
    }
    return -1;
}

static int test_remove_file(const char *path) {
    wchar_t wpath[TEST_PATH_CAP];
    return test_wide(path, wpath) && DeleteFileW(wpath) ? 0 : -1;
}

static int test_remove_empty_dir(const char *path) {
    wchar_t wpath[TEST_PATH_CAP];
    return test_wide(path, wpath) && RemoveDirectoryW(wpath) ? 0 : -1;
}
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static FILE *test_fopen(const char *path, const char *mode) { return fopen(path, mode); }

static int test_dir_each(const char *dir, test_dir_fn fn, void *ctx) {
    DIR *dp = opendir(dir);
    if (!dp) return -1;
    struct dirent *e;
    while ((e = readdir(dp)) != NULL) {
        const char *name = e->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (!fn(ctx, name, S_ISDIR(st.st_mode))) break;
    }
    closedir(dp);
    return 0;
}

static int test_make_dir(const char *path) {
    struct stat st;
    return mkdir(path, 0755) == 0 || (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
}

/* The system's folder for temporary files. */
static const char *test_temp_root(void) {
    const char *base = getenv("TMPDIR");
    return base && *base ? base : "/tmp";
}

static int test_temp_dir(char *out, size_t cap) {
    snprintf(out, cap, "%s/sigil-test-XXXXXX", test_temp_root());
    return mkdtemp(out) ? 0 : -1;
}

static int test_remove_file(const char *path) { return unlink(path); }
static int test_remove_empty_dir(const char *path) { return rmdir(path); }
#endif

typedef struct {
    char (*names)[260];
    bool *dirs;
    size_t count;
    size_t cap;
} test_entries;

static bool test_gather_entry(void *ctx, const char *name, bool is_dir) {
    test_entries *e = (test_entries *)ctx;
    if (e->count == e->cap) {
        size_t cap = e->cap ? e->cap * 2 : 16;
        char (*names)[260] = (char (*)[260])realloc(e->names, cap * sizeof(*names));
        bool *dirs = (bool *)realloc(e->dirs, cap * sizeof(*dirs));
        if (names) e->names = names;
        if (dirs) e->dirs = dirs;
        if (!names || !dirs) return false;
        e->cap = cap;
    }
    snprintf(e->names[e->count], sizeof(e->names[0]), "%s", name);
    e->dirs[e->count++] = is_dir;
    return true;
}

/* Removes `path` and everything under it; the folder's names are read before
 * any is removed, so no listing runs over entries going away under it. */
static void test_remove_tree(const char *path) {
    test_entries e = { NULL, NULL, 0, 0 };
    test_dir_each(path, test_gather_entry, &e);
    for (size_t i = 0; i < e.count; i++) {
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", path, e.names[i]);
        if (e.dirs[i]) test_remove_tree(full);
        else test_remove_file(full);
    }
    free(e.names);
    free(e.dirs);
    test_remove_empty_dir(path);
}

#endif
