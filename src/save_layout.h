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

/* Template variables and option semantics: docs/save-units.md, "Layout rows". */
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

/* How an emulator lists its user profiles. */
typedef enum {
    SIGIL_PROFILES_YUZU = 1,   /* profiles.dat: 0x10-byte header, then 0xC8-byte entries, UUID at +0 and
                                  nickname at +0x28; the save folder is the UUID's bytes reversed, in hex */
    SIGIL_PROFILES_CEMU,       /* act/<id>/account.dat: PersistentId= and MiiName= (UTF-16BE in hex) lines */
    SIGIL_PROFILES_VITA3K,     /* user/<id>/user.xml: the name attribute of <user> */
    SIGIL_PROFILES_RPCS3,      /* home/<id>/localusername: the name alone */
    SIGIL_PROFILES_RYUJINX     /* system/Profiles.json: "user_id" (32 hex) and "name" per profile */
} sigil_profiles_format;

/* One folder of a game's saves on a layout with profiles. Templates are
 * relative to the emulator's base folder and end in '/'; {profile} and
 * {save_id} each stand for one whole path segment. */
typedef struct {
    const char *template_;   /* the folder on disk */
    const char *entry;       /* the same folder in the unit */
    int         area;        /* sigil_save_area */
    bool        rebuilt;     /* the emulator writes it again by itself: restore skips it when it is out of
                                the root's reach instead of refusing */
    const char *legacy;      /* an older unit's name for the folder, {profile} standing for any id, or NULL */
    bool        optional;    /* a unit may leave the folder out: restore then keeps what is there (3DS extdata) */
} sigil_layout_area;

/* A layout whose saves are folders, kept per user profile when it has an
 * account area. */
typedef struct {
    const char              *top;           /* the folder of the emulator's base every template starts in */
    int                      format;        /* sigil_profiles_format; 0 with no profiles */
    const char              *list;          /* the profile list file, or the template naming one file per
                                               profile, relative to the base; NULL with no profiles */
    const sigil_layout_area *areas;
    size_t                   area_count;
    const char *const       *ignored;       /* files the emulator keeps in a save folder for itself: never
                                               collected, never removed */
    size_t                   ignored_count;
    bool                     prefix;        /* {save_id} takes every folder whose name starts with it */
    bool                     savedata_only; /* a folder whose PARAM.SFO holds neither SAVEDATA_PARAMS nor
                                               SAVEDATA_FILE_LIST is installed game data, not a save */
    const char              *fixed_profile; /* the one profile an emulator with no list always uses, or NULL */
    const char              *index;         /* the save index (Ryujinx's imkvdb.arc), relative to the base: save
                                               folders are named by the id it gives each save, and it says
                                               whose each one is; NULL when folders are named by the title */
    size_t                   save_id_segments;   /* the path segments {save_id} spans (3DS: 2); 0 means 1 */
    const char              *category;      /* Wii: the title category {category} stands for when the save
                                               id is the code alone ("00010000", a disc); a save id of
                                               <category>/<code> (a WAD) gives its own. NULL elsewhere */
    const char *const       *dropped;       /* folders, as a unit names them, that an older unit carries and
                                               restore leaves out: Wii's installed content/ */
    size_t                   dropped_count;
} sigil_layout_profiles;

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
    const sigil_layout_profiles *profiles;    /* saves are folders per user profile; NULL otherwise */
} sigil_layout;

/* The slug layout rows use for `slug`: "scd" and "mega_cd" give "segacd",
 * "ps1" gives "psx". Other slugs come back as given. */
const char *sigil_layout_platform(const char *slug);

/* The row for a layout id and platform, else the libretro default row. */
const sigil_layout *sigil_layout_find(const char *layout, const char *platform);

/* Every row that carries `layout`, for subfolder enumeration. */
size_t sigil_layout_rows(const char *layout, const sigil_layout **out, size_t cap);

#endif
