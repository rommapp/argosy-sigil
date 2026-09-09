// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_SAVE_LAYOUT_H
#define SIGIL_SAVE_LAYOUT_H

#include "sigil_internal.h"

/* Template variables and option semantics: README, "Save units". */
typedef struct {
    const char *template_;
    int         role;        /* sigil_save_role */
    const char *opt_key;
    const char *opt_value;
    bool        opt_default;
} sigil_layout_member;

/* One file for every game; reported, never bundled. */
typedef struct {
    const char *template_;
    const char *opt_key;
    const char *opt_value;
    bool        opt_default;
} sigil_layout_shared;

typedef struct {
    const char                *layout;    /* core or emulator id */
    const char                *platform;  /* slug this row is limited to, or NULL */
    const sigil_layout_member *members;
    size_t                     member_count;
    const sigil_layout_shared *shared;
    size_t                     shared_count;
    const char *const         *subdirs;
    size_t                     subdir_count;
} sigil_layout;

/* The row for a layout id and platform, else the libretro default row. */
const sigil_layout *sigil_layout_find(const char *layout, const char *platform);

/* Every row that carries `layout`, for subfolder enumeration. */
size_t sigil_layout_rows(const char *layout, const sigil_layout **out, size_t cap);

#endif
