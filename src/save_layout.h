// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_SAVE_LAYOUT_H
#define SIGIL_SAVE_LAYOUT_H

#include "sigil_internal.h"

/* The device a volume file holds, for platforms whose saves live on volumes:
 * Saturn and Sega CD internal memory and their carts, and the Dreamcast VMU
 * in each controller port. */
typedef enum {
    SIGIL_DEVICE_NONE = 0,
    SIGIL_DEVICE_INTERNAL,
    SIGIL_DEVICE_CART,
    SIGIL_DEVICE_VMU_A1,
    SIGIL_DEVICE_VMU_A2,
    SIGIL_DEVICE_VMU_B1,
    SIGIL_DEVICE_VMU_B2,
    SIGIL_DEVICE_VMU_C1,
    SIGIL_DEVICE_VMU_C2,
    SIGIL_DEVICE_VMU_D1,
    SIGIL_DEVICE_VMU_D2,
    SIGIL_DEVICE_GC_FOLDER,   /* Dolphin's GCI folder: a directory with one .gci file per save */
    SIGIL_DEVICE_GC_CARD,     /* a raw GameCube memory card image */
    SIGIL_DEVICE_COUNT
} sigil_device;

/* How a core stores a volume file it creates, for sigil to write a new one
 * the same way. */
typedef enum {
    SIGIL_FORM_RAW = 0,
    SIGIL_FORM_EXPANDED_FF,  /* each byte in the odd half of a word, the even half 0xFF (Yabause) */
    SIGIL_FORM_SHIFT_JIS,    /* a GameCube card formatted for Japanese text */
    SIGIL_FORM_VMP           /* a PS1 card signed as the PSP and Vita keep one */
} sigil_volume_form;

/* Template variables and option semantics: README, "Save units". */
typedef struct {
    const char *template_;
    int         role;        /* sigil_save_role */
    const char *opt_key;
    const char *opt_value;
    bool        opt_default;
    int         device;      /* sigil_device */
    const char *opt2_key;    /* a second option that must hold as well, or NULL */
    const char *opt2_value;
    bool        opt2_default;
    int         form;        /* sigil_volume_form of a file the core creates */
    uint32_t    new_size;    /* collapsed bytes of a file the core creates; 0 for the format's default */
} sigil_layout_member;

/* One file for every game; reported, never bundled. */
typedef struct {
    const char *template_;
    const char *opt_key;
    const char *opt_value;
    bool        opt_default;
    int         device;      /* sigil_device */
    char        region;      /* 'U', 'E', 'J' when the core picks this file by the disc's region, else 0 */
    const char *opt2_key;    /* a second option that must hold as well, or NULL */
    const char *opt2_value;
    bool        opt2_default;
    int         form;        /* sigil_volume_form of a file the core creates */
    uint32_t    new_size;    /* collapsed bytes of a file the core creates; 0 for the format's default */
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
    const char                *region_option; /* option forcing the region shared files are picked by */
} sigil_layout;

/* The slug layout rows use for `slug`: "scd" and "mega_cd" give "segacd",
 * "ps1" gives "psx". Other slugs come back as given. */
const char *sigil_layout_platform(const char *slug);

/* The row for a layout id and platform, else the libretro default row. */
const sigil_layout *sigil_layout_find(const char *layout, const char *platform);

/* Every row that carries `layout`, for subfolder enumeration. */
size_t sigil_layout_rows(const char *layout, const sigil_layout **out, size_t cap);

#endif
