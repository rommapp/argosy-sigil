// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_SAVE_LAYOUT_H
#define SIGIL_SAVE_LAYOUT_H

#include "sigil_internal.h"

/* A member template names one file a core writes for a game, relative to the
 * save root. Variables: {stem} {romset} {title_id} {save_id} {cart_size}
 * {nvram_version} {left_index} {right_index}. A template ending in '/' names a
 * folder whose whole subtree is the member.
 *
 * A template with `opt_key` applies only while that core option holds
 * `opt_value`; `opt_default` says whether an absent option counts as holding
 * it, which is how a core's own default is expressed without the caller
 * having to send every variable. */
typedef struct {
    const char *template_;
    int         role;        /* sigil_save_role */
    const char *opt_key;
    const char *opt_value;
    bool        opt_default;
} sigil_layout_member;

/* A file the core writes for every game at once. It is reported, never
 * bundled, because no game can claim it. */
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
