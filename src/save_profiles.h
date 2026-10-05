// SPDX-License-Identifier: MPL-2.0
/* Layouts whose saves are folders kept per user profile: where the save root
 * sits in the emulator's folders, the profiles the emulator lists, and the
 * mapping between the game's files on disk and their names in a unit. */
#ifndef SIGIL_SAVE_PROFILES_H
#define SIGIL_SAVE_PROFILES_H

#include "save_layout.h"

#define SIGIL_PROFILES_MAX 16
#define SIGIL_PROFILE_INDEX_MAX 16

/* A save of the game the row's save index names: the folder's id, and the
 * profile whose account save it is ("" for the device save). */
typedef struct {
    uint64_t id;
    int      area;   /* sigil_save_area */
    char     profile[SIGIL_PROFILE_ID_MAX];
} sigil_profile_indexed;

typedef struct {
    const sigil_save_request    *req;
    const sigil_layout_profiles *row;
    char   above[SIGIL_SAVE_PATH_MAX];      /* the base's folder under the root when the root is above it, or "" */
    char   below[SIGIL_SAVE_PATH_MAX];      /* the root's folder under the base when the root is inside it, or "" */
    char   save_id[SIGIL_SAVE_ENTRY_MAX];   /* the game's save folder name, or the start of each one */
    bool   prefix;                          /* the row's: save_id starts each folder name */
    sigil_save_profile profiles[SIGIL_PROFILES_MAX];
    size_t profile_count;
    char   profile[SIGIL_PROFILE_ID_MAX];   /* the profile the request, the root path or the list settles on */
    bool   several;                         /* the list holds several profiles and nothing picks one */
    sigil_profile_indexed indexed[SIGIL_PROFILE_INDEX_MAX];   /* the game's saves in the row's index */
    size_t indexed_count;
} sigil_profile_root;

/* One file of the game's saves. */
typedef struct {
    int  area;                          /* sigil_save_area */
    char path[SIGIL_SAVE_PATH_MAX];     /* relative to the save root */
    char entry[SIGIL_SAVE_ENTRY_MAX];   /* its name in the unit */
} sigil_profile_file;

/**
 * Reads where the request's root sits and the profiles the emulator lists.
 * The game is the request's save id; with none, no folder is the game's.
 * SIGIL_ERR_AMBIGUOUS, the candidate base folders one per line in `problem`,
 * when the listing holds more than one; SIGIL_ERR_INVALID_ARG when the root
 * lies inside one of the game's save folders.
 */
int sigil_profile_root_open(const sigil_save_request *req, const sigil_layout_profiles *row, sigil_profile_root *p,
                            char problem[SIGIL_SAVE_PATH_MAX]);

/**
 * The game's files in the listing, in unit name order: every device area's,
 * and the account area's of `profile` (none when it is ""). The emulator's
 * own files are left out. `*out` is malloc'd.
 */
int sigil_profile_files(const sigil_profile_root *p, const char *profile, sigil_profile_file **out, size_t *count);

/** Nothing picks a profile and more than one is listed, one of them holding the game's saves. */
bool sigil_profile_undecided(const sigil_profile_root *p);

/**
 * Where unit member `entry` goes for the chosen profile: `f` gets its area,
 * its path relative to the save root, and the name collect gives it, which
 * differs from `entry` for an older unit's name. SIGIL_ERR_NOT_FOUND when no
 * folder of the layout takes it; SIGIL_ERR_NO_TARGET when it is an account
 * file and no profile is chosen, or the folder lies outside the root.
 * `*skip` is true, and the path empty, instead of the latter for a folder
 * the emulator writes again by itself, and for a file the emulator keeps for
 * itself: restore leaves both alone.
 */
int sigil_profile_place(const sigil_profile_root *p, const char *entry, sigil_profile_file *f, bool *skip);

/** The profiles, "id name" one per line, as SIGIL_ERR_AMBIGUOUS reports them. */
void sigil_profile_lines(const sigil_profile_root *p, char *out, size_t cap);

#endif
