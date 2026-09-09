# C

`#include <sigil.h>`, link `libsigil` and its bundled decompression and
crypto libs (README, "Building"). Every call returns `SIGIL_OK` or a
negative `SIGIL_ERR_*`; `sigil_strerror(code)` names it. The caller sets
`struct_version` on every struct it passes.

## 1. Identify the game

Once, at import. Skip when the result is already stored.

```c
int sigil_extract_from_path(
    const char *path,             /* required. Rom file. */
    sigil_platform hint,          /* required. SIGIL_PLATFORM_PSP etc., or sigil_platform_from_slug(slug).
                                     SIGIL_PLATFORM_AUTO sniffs the extension; a bare .iso or .zip needs one. */
    const sigil_options *opts,    /* optional, NULL for defaults. */
    sigil_result *out             /* required. struct_version = SIGIL_RESULT_V3. */
);                                /* SIGIL_ERR_* when nothing identified the file. */

typedef struct {
    uint32_t struct_version;      /* SIGIL_OPTIONS_V1 */
    const sigil_support *support; /* optional. Switch key material, below. */
    uint32_t flags;               /* optional. SIGIL_FLAG_FILENAME_FALLBACK (on when opts is NULL): scan the
                                     file name when the binary gives nothing, source reports which happened.
                                     SIGIL_FLAG_3DS_ALLOW_HOMEBREW. */
} sigil_options;

typedef struct {
    uint32_t struct_version;              /* SIGIL_SUPPORT_V1 */
    const uint8_t *switch_header_key;     /* optional. 32 bytes. Wins over both below. */
    const char *switch_prod_keys_path;    /* optional. prod.keys file. */
    const char *switch_prod_keys_text;    /* optional. prod.keys contents. Wins over the path. */
    size_t switch_prod_keys_text_len;
} sigil_support;
```

```c
typedef struct {
    uint32_t struct_version;
    char title_id[32];            /* "" on gb, gbc, snes. */
    char raw_serial[32];          /* As found in the binary. */
    char save_id[32];             /* On-disk name the emulator keys the save by. "" on gb, gbc, snes. */
    sigil_platform platform;      /* sigil_platform_to_slug() for the slug. */
    sigil_source source;          /* SIGIL_SOURCE_BINARY, SIGIL_SOURCE_FILENAME. */
    sigil_usage usage;            /* SIGIL_USAGE_FOLDER_EXACT, _FOLDER_PREFIX, _FILE_EXACT, _FILE_PREFIX,
                                     _FOLDER_SPLIT. README "usage" says how to apply save_id for each. */
    int experimental;
    int switch_content_type;      /* SIGIL_SWITCH_CONTENT_UNKNOWN, _APPLICATION, _PATCH, _ADDON. */
    uint32_t title_version;       /* Switch only. */
    uint32_t features;            /* Bit set. SIGIL_FEATURE_RTC: cart has a clock. */
} sigil_result;
```

Store `title_id`, `save_id`, the platform slug, `features`. Rebuild
later, or build for a platform sigil cannot extract (Sega CD returns
`SIGIL_ERR_UNKNOWN_PLATFORM`), by filling a zeroed result:

```c
sigil_result game = { .struct_version = SIGIL_RESULT_V3, .features = stored_features };
strncpy(game.title_id, stored_title_id, sizeof(game.title_id) - 1);   /* or leave "" */
strncpy(game.save_id, stored_save_id, sizeof(game.save_id) - 1);      /* or leave "" */
```

## 2. Locate the saves

At sync time. No file is read.

```c
int sigil_save_resolve(
    const sigil_save_request *req,   /* required. */
    sigil_save_unit **out            /* required. Free with sigil_save_unit_free(). */
);                                   /* Hashes empty unless req->open is set. */

typedef struct {
    uint32_t struct_version;              /* SIGIL_SAVE_REQUEST_V1 */
    const char *layout;                   /* required. Libretro core name without _libretro: genesis_plus_gx,
                                             mednafen_psx_hw, mame2003_plus. A core without a README layout
                                             row gets the default row (<stem>.srm, plus <stem>.rtc when the
                                             cart has a clock). */
    const char *platform;                 /* optional. Slug from the README platform table, or segacd, fds.
                                             Rows limited to one platform match only when it is given. */
    const char *content_path;             /* required. The path you handed the emulator, verbatim: rom,
                                             .m3u, .cue, .chd, or archive.zip#member.ext when a member was
                                             loaded. Only the file name part is used. */
    const sigil_result *result;           /* optional. Step 1, for the ids and features. */
    uint32_t features;                    /* optional. Stored features when result is NULL. */
    const sigil_save_option *options;     /* optional. Core option key to value, the strings the core defines
                                             and RetroArch writes to <core>.opt, never display labels:
                                             {"genesis_plus_gx_system_bram", "per game"}. Pass all of them;
                                             only the keys the row names are read. */
    size_t option_count;
    const char *const *listing;           /* required. Every file directly in the save root plus every file
                                             under sigil_save_layout_subdirs(layout), three levels deep, as
                                             root-relative / paths. The root: the directory the emulator
                                             writes this game's save into; RetroArch: savefile_directory,
                                             plus the core-named subfolder when sort_savefiles_enable is on. */
    size_t listing_count;
    sigil_save_open_fn open;              /* optional. Set with open_ctx to hash in this call (step 3). */
    void *open_ctx;
} sigil_save_request;
```

```c
typedef struct {
    uint32_t struct_version;
    char key[256];                        /* Stem, or save_id for folder layouts. */
    int shape;                            /* SIGIL_SAVE_SHAPE_SINGLE, _MULTI, _FOLDER. _NONE when nothing
                                             is there. */
    sigil_save_member *members;           /* Files present. */
    size_t member_count;
    sigil_save_member *expected;          /* Files the core should have written but has not: every applicable
                                             primary, plus the rtc file when the cart has a clock. */
    size_t expected_count;
    char (*unkeyed)[512];                 /* Root files shared by every game. Never bundle them. */
    size_t unkeyed_count;
    char artifact[256];                   /* File name the upload travels under. */
    char content_hash[33];                /* "" until step 3. */
    char identity_hash[33];               /* "" until step 3. */
} sigil_save_unit;

typedef struct {
    char path[512];      /* Root-relative. */
    char entry[256];     /* Archive entry name. */
    int role;            /* SIGIL_SAVE_ROLE_PRIMARY, _SIDECAR, _RTC. */
    int present;
} sigil_save_member;
```

## 3. Hash the saves

When you need to compare with the server.

```c
int sigil_save_hash(
    sigil_save_unit *unit,        /* required. Step 2. Filled in place. */
    sigil_save_open_fn open,      /* required. Returns a sigil_io for one root-relative path, or NULL.
                                     sigil closes what it opens. */
    void *open_ctx                /* Passed to open. */
);                                /* SIGIL_ERR_IO when a member cannot be opened. */

typedef sigil_io *(*sigil_save_open_fn)(void *ctx, const char *relative_path);

static sigil_io *open_member(void *ctx, const char *relative_path) {
    char path[SIGIL_SAVE_PATH_MAX * 2];
    snprintf(path, sizeof(path), "%s/%s", (const char *)ctx, relative_path);
    return sigil_io_open_file(path);
}
```

```c
unit->content_hash    /* What the RomM server computes for the artifact. */
unit->identity_hash   /* The same over the non-rtc members. Different content_hash, same
                         identity_hash: a clock tick, not a new save. */
```

## Upload and restore

Upload by `shape`. `SINGLE` sends the member as is. `MULTI` zips the
members flat, each under its `entry`. `FOLDER` zips the `key` folder so
entries read `<key>/<file>`. Name the upload `artifact`. Hash rules and
the layout table: README, "Save units".

Restore by `path`. Unzip a `MULTI` artifact so every entry lands at its
member's `path` under the root. Unzip a `FOLDER` artifact from the
root's parent of the key folder. `expected` says where a primary goes
when the emulator has not created one yet.

## Helpers

```c
const char *sigil_content_stem(const char *content_path, char *out, size_t cap);   /* Stem the save is named after. */
size_t sigil_save_layout_subdirs(const char *layout, const char **out, size_t cap); /* Subfolders the core writes into. */
sigil_platform sigil_platform_from_slug(const char *slug);   /* SIGIL_PLATFORM_AUTO when unknown. */
const char *sigil_platform_to_slug(sigil_platform p);
int sigil_load_header_key_from_prod_keys(const char *path, uint8_t out[32]);
int sigil_extract_from_io(const sigil_io *io, const char *filename_hint, sigil_platform hint,
                          const sigil_options *opts, sigil_result *out);   /* Step 1 over your own stream. */
sigil_io *sigil_io_open_file(const char *path);   /* Also _chd, _cso, _raw_cd, _zip, _zip_member, _zar. */
void sigil_io_close(sigil_io *io);
const char *sigil_strerror(int code);
const char *sigil_version(void);
```
