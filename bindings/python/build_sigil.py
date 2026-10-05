# SPDX-License-Identifier: MPL-2.0
"""cffi API-mode builder for the sigil C library.

Compiles the sigil._sigil extension against the static libs in
../../build-python. Run via `make build` (or directly after the cmake step).
"""

import os

from cffi import FFI

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
INCLUDE_DIR = os.path.join(ROOT, "include")
LIB_DIR = os.environ.get("SIGIL_LIB_DIR", os.path.join(ROOT, "build-python"))

ffibuilder = FFI()

ffibuilder.cdef(
    """
#define SIGIL_RESULT_V1 ...
#define SIGIL_RESULT_V2 ...
#define SIGIL_RESULT_V3 ...
#define SIGIL_RESULT_V4 ...
#define SIGIL_FEATURE_RTC ...
#define SIGIL_FEATURE_MBC2 ...
#define SIGIL_SUPPORT_V1 ...
#define SIGIL_OPTIONS_V1 ...
#define SIGIL_SAVE_REQUEST_V1 ...
#define SIGIL_SAVE_UNIT_V1 ...
#define SIGIL_SAVE_PATH_MAX ...
#define SIGIL_SAVE_ENTRY_MAX ...

#define SIGIL_OK ...
#define SIGIL_ERR_INVALID_ARG ...
#define SIGIL_ERR_IO ...
#define SIGIL_ERR_UNKNOWN_PLATFORM ...
#define SIGIL_ERR_UNSUPPORTED_FORMAT ...
#define SIGIL_ERR_NOT_FOUND ...
#define SIGIL_ERR_NEEDS_KEY ...
#define SIGIL_ERR_CRYPTO ...
#define SIGIL_ERR_OOM ...
#define SIGIL_ERR_CONFLICT ...
#define SIGIL_ERR_EXISTS ...
#define SIGIL_ERR_NO_SPACE ...
#define SIGIL_ERR_UNCOLLECTED ...
#define SIGIL_ERR_DAMAGED ...
#define SIGIL_ERR_REGION ...
#define SIGIL_ERR_NO_TARGET ...
#define SIGIL_ERR_AMBIGUOUS ...
#define SIGIL_ERR_KEYS_INCOMPATIBLE ...

#define SIGIL_FLAG_FILENAME_FALLBACK ...
#define SIGIL_FLAG_3DS_ALLOW_HOMEBREW ...

typedef enum {
    SIGIL_PLATFORM_AUTO,
    SIGIL_PLATFORM_PSP,
    SIGIL_PLATFORM_PSX,
    SIGIL_PLATFORM_PS2,
    SIGIL_PLATFORM_PSVITA,
    SIGIL_PLATFORM_SWITCH,
    SIGIL_PLATFORM_3DS,
    SIGIL_PLATFORM_WII,
    SIGIL_PLATFORM_WIIU,
    SIGIL_PLATFORM_GAMECUBE,
    SIGIL_PLATFORM_PS3,
    SIGIL_PLATFORM_XBOX360,
    SIGIL_PLATFORM_DREAMCAST,
    SIGIL_PLATFORM_XBOX,
    SIGIL_PLATFORM_GB,
    SIGIL_PLATFORM_GBC,
    SIGIL_PLATFORM_SNES,
    SIGIL_PLATFORM_N64,
    ...
} sigil_platform;

typedef enum {
    SIGIL_SOURCE_BINARY,
    SIGIL_SOURCE_FILENAME,
    ...
} sigil_source;

typedef enum {
    SIGIL_USAGE_FOLDER_EXACT,
    SIGIL_USAGE_FOLDER_PREFIX,
    SIGIL_USAGE_FILE_EXACT,
    SIGIL_USAGE_FILE_PREFIX,
    SIGIL_USAGE_FOLDER_SPLIT,
    ...
} sigil_usage;

enum sigil_switch_content_type {
    SIGIL_SWITCH_CONTENT_UNKNOWN,
    SIGIL_SWITCH_CONTENT_APPLICATION,
    SIGIL_SWITCH_CONTENT_PATCH,
    SIGIL_SWITCH_CONTENT_ADDON,
    ...
};

typedef struct {
    uint32_t       struct_version;
    char           title_id[32];
    char           raw_serial[32];
    char           save_id[32];
    sigil_platform platform;
    sigil_source   source;
    sigil_usage    usage;
    int            experimental;
    int            switch_content_type;
    uint32_t       title_version;
    uint32_t       features;
    char           n64_header[24];
    char           n64_md5[33];
    char           n64_md5_n64[33];
} sigil_result;

typedef struct {
    uint32_t       struct_version;
    const uint8_t *switch_header_key;
    const char    *switch_prod_keys_path;
    const char    *switch_prod_keys_text;
    size_t         switch_prod_keys_text_len;
} sigil_support;

typedef struct {
    uint32_t              struct_version;
    const sigil_support  *support;
    uint32_t              flags;
} sigil_options;

int sigil_extract_from_path(const char *path,
                            sigil_platform hint,
                            const sigil_options *opts,
                            sigil_result *out);

sigil_platform sigil_platform_from_slug(const char *slug);
const char *sigil_platform_to_slug(sigil_platform p);

int sigil_load_header_key_from_prod_keys(const char *path, uint8_t *out);

typedef struct sigil_io sigil_io;
struct sigil_io {
    int     (*read)(void *ctx, uint64_t off, void *buf, size_t len);
    int64_t (*size)(void *ctx);
    void    (*close)(void *ctx);
    void     *ctx;
};

sigil_io *sigil_io_open_file(const char *path);
void      sigil_io_close(sigil_io *io);

typedef enum {
    SIGIL_SAVE_SHAPE_NONE,
    SIGIL_SAVE_SHAPE_SINGLE,
    SIGIL_SAVE_SHAPE_MULTI,
    SIGIL_SAVE_SHAPE_FOLDER,
    ...
} sigil_save_shape;

typedef enum {
    SIGIL_SAVE_ROLE_PRIMARY,
    SIGIL_SAVE_ROLE_SIDECAR,
    SIGIL_SAVE_ROLE_RTC,
    ...
} sigil_save_role;

typedef enum {
    SIGIL_SAVE_AREA_NONE,
    SIGIL_SAVE_AREA_ACCOUNT,
    SIGIL_SAVE_AREA_DEVICE,
    ...
} sigil_save_area;

#define SIGIL_PROFILE_ID_MAX ...
#define SIGIL_PROFILE_NAME_MAX ...

typedef struct {
    char path[...];
    char entry[...];
    int  role;
    int  present;
    int  area;
} sigil_save_member;

typedef struct {
    char id[...];
    char name[...];
} sigil_save_profile;

typedef struct {
    const char *key;
    const char *value;
} sigil_save_option;

typedef struct {
    char              path[512];
    int               shared;
    sigil_save_option options[2];
    size_t            option_count;
} sigil_save_alternate;

typedef sigil_io *(*sigil_save_open_fn)(void *ctx, const char *relative_path);

typedef struct {
    uint32_t                  struct_version;
    const char               *layout;
    const char               *platform;
    const char               *content_path;
    const sigil_result       *result;
    uint32_t                  features;
    const sigil_save_option  *options;
    size_t                    option_count;
    const char *const        *listing;
    size_t                    listing_count;
    sigil_save_open_fn        open;
    void                     *open_ctx;
    const char               *root_path;
    const char               *profile;
} sigil_save_request;

typedef struct {
    uint32_t           struct_version;
    char               key[...];
    int                shape;
    sigil_save_member *members;
    size_t             member_count;
    sigil_save_member *expected;
    size_t             expected_count;
    char             (*unkeyed)[512];
    size_t             unkeyed_count;
    char               artifact[...];
    char               content_hash[33];
    char               identity_hash[33];
    sigil_save_alternate *alternates;
    size_t             alternate_count;
} sigil_save_unit;

int  sigil_save_resolve(const sigil_save_request *req, sigil_save_unit **out);
void sigil_save_unit_free(sigil_save_unit *unit);
int  sigil_save_hash(sigil_save_unit *unit, sigil_save_open_fn open, void *open_ctx);
size_t sigil_save_layout_subdirs(const char *layout, const char **out, size_t cap);
int    sigil_save_base(const char *layout, const char *path, char *base, size_t base_cap, char *profile,
                       size_t profile_cap);
const char *sigil_save_layout_top(const char *layout);
int    sigil_save_profiles(const sigil_save_request *req, sigil_save_profile **out, size_t *count);
void   sigil_save_profiles_free(sigil_save_profile *profiles);
const char *sigil_content_stem(const char *content_path, char *out, size_t cap);

#define SIGIL_CARD_LISTING_V1 ...

typedef enum {
    SIGIL_CARD_FORMAT_UNKNOWN,
    SIGIL_CARD_FORMAT_PS1_RAW,
    SIGIL_CARD_FORMAT_PS1_GME,
    SIGIL_CARD_FORMAT_PS1_VMP,
    SIGIL_CARD_FORMAT_PS2,
    SIGIL_CARD_FORMAT_GAMECUBE_RAW,
    SIGIL_CARD_FORMAT_DREAMCAST_VMU,
    SIGIL_CARD_FORMAT_SATURN_BACKUP,
    SIGIL_CARD_FORMAT_SEGACD_BRAM,
    ...
} sigil_card_format;

typedef struct {
    char     name[...];
    char     owner_id[...];
    uint32_t blocks;
    uint32_t first_block;
} sigil_card_entry;

typedef struct {
    uint32_t          struct_version;
    int               format;
    uint32_t          total_blocks;
    uint32_t          free_blocks;
    uint32_t          free_slots;
    uint32_t          corrupt_count;
    sigil_card_entry *entries;
    size_t            entry_count;
    sigil_card_entry *corrupt_entries;
    size_t            corrupt_entry_count;
} sigil_card_listing;

int  sigil_card_list(const sigil_io *io, sigil_card_listing **out);
void sigil_card_listing_free(sigil_card_listing *listing);

#define SIGIL_SYNC_REQUEST_V1 ...
#define SIGIL_SYNC_RESULT_V1 ...

typedef int (*sigil_save_write_fn)(void *ctx, const char *relative_path, const uint8_t *data, size_t len);
typedef int (*sigil_save_remove_fn)(void *ctx, const char *relative_path);

typedef enum {
    SIGIL_SYNC_MANAGED,
    SIGIL_SYNC_UNMANAGED,
    ...
} sigil_sync_mode;

typedef struct {
    const char *const *game_ids;
    size_t             game_id_count;
    const uint8_t     *unit;
    size_t             unit_len;
} sigil_sync_companion;

typedef struct {
    uint8_t *data;
    size_t   len;
    char     content_hash[33];
    char     identity_hash[33];
    int      changed;
} sigil_sync_companion_result;

typedef struct {
    uint32_t              struct_version;
    sigil_save_request    save;
    const char *const    *game_ids;
    size_t                game_id_count;
    int                   mode;
    const uint8_t        *state;
    size_t                state_len;
    int                   overwrite_local;
    sigil_save_write_fn   write;
    void                 *write_ctx;
    const char *const    *claimed;
    size_t                claimed_count;
    sigil_save_remove_fn  remove;
    const sigil_sync_companion *companions;
    size_t                companion_count;
    int                   repair;
} sigil_sync_request;

typedef struct {
    uint32_t  struct_version;
    char      artifact[...];
    int       shape;
    uint8_t  *data;
    size_t    len;
    char      content_hash[33];
    char      identity_hash[33];
    int       changed;
    int       conflict;
    uint8_t  *state;
    size_t    state_len;
    uint8_t  *holding;
    size_t    holding_len;
    char    (*unowned)[64];
    size_t    unowned_count;
    int       restore_again;
    sigil_sync_companion_result *companions;
    size_t    companion_count;
    char      problem[...];
    uint32_t  blocks_short;
    sigil_save_profile *profiles;
    size_t    profile_count;
    char      profile[...];
    sigil_save_alternate *alternates;
    size_t    alternate_count;
    int       hardcore_marker;
    size_t    unowned_changed;
} sigil_sync_result;

int  sigil_collect(const sigil_sync_request *req, sigil_sync_result **out);
int  sigil_restore(const sigil_sync_request *req, const uint8_t *unit, size_t unit_len, sigil_sync_result **out);
void sigil_sync_result_free(sigil_sync_result *result);

const char *sigil_strerror(int code);
const char *sigil_version(void);
"""
)

ffibuilder.set_source(
    "sigil._sigil",
    '#include "sigil.h"',
    include_dirs=[INCLUDE_DIR],
    library_dirs=[LIB_DIR],
    # Link order matters: sigil first, then its decompression/crypto deps.
    libraries=[
        "sigil",
        "sigil_chdr",
        "sigil_zstd",
        "sigil_zlib",
        "sigil_lzma",
        "sigil_aes",
    ],
)

if __name__ == "__main__":
    ffibuilder.compile(tmpdir=HERE, verbose=True)
