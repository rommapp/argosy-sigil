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
#define SIGIL_FEATURE_RTC ...
#define SIGIL_SUPPORT_V1 ...
#define SIGIL_OPTIONS_V1 ...

#define SIGIL_OK ...
#define SIGIL_ERR_INVALID_ARG ...
#define SIGIL_ERR_IO ...
#define SIGIL_ERR_UNKNOWN_PLATFORM ...
#define SIGIL_ERR_UNSUPPORTED_FORMAT ...
#define SIGIL_ERR_NOT_FOUND ...
#define SIGIL_ERR_NEEDS_KEY ...
#define SIGIL_ERR_CRYPTO ...
#define SIGIL_ERR_OOM ...

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
