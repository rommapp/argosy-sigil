// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_TEST_FS_H
#define SIGIL_TEST_FS_H

/* Folder access for the tests on Windows and POSIX alike: list a folder,
 * make one, make a fresh temporary one, and remove a tree. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Called for each entry of a folder but . and ..; return false to stop. */
typedef bool (*test_dir_fn)(void *ctx, const char *name, bool is_dir);

#ifdef _WIN32
#include <windows.h>

/* 0, or -1 when `dir` can't be listed. */
static int test_dir_each(const char *dir, test_dir_fn fn, void *ctx) {
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        const char *name = fd.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (!fn(ctx, name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

static int test_make_dir(const char *path) {
    return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
}

static int test_temp_dir(char *out, size_t cap) {
    char base[MAX_PATH];
    if (!GetTempPathA(sizeof(base), base)) return -1;
    for (unsigned tries = 0; tries < 100; tries++) {
        snprintf(out, cap, "%ssigil-test-%lu-%u", base, (unsigned long)GetCurrentProcessId(), tries);
        if (CreateDirectoryA(out, NULL)) return 0;
    }
    return -1;
}

static int test_remove_file(const char *path) { return DeleteFileA(path) ? 0 : -1; }
static int test_remove_empty_dir(const char *path) { return RemoveDirectoryA(path) ? 0 : -1; }
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

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

static int test_temp_dir(char *out, size_t cap) {
    const char *base = getenv("TMPDIR");
    snprintf(out, cap, "%s/sigil-test-XXXXXX", base && *base ? base : "/tmp");
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
