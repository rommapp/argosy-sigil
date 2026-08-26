// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_H
#define SIGIL_H

#include <stddef.h>
#include <stdint.h>

/* SIGIL_EXPORTS is defined when building the shared library itself;
 * consumers of the DLL define SIGIL_SHARED (CMake propagates it). Static
 * builds need neither and the macro is empty. */
#ifndef SIGIL_API
#if defined(_WIN32)
#if defined(SIGIL_EXPORTS)
#define SIGIL_API __declspec(dllexport)
#elif defined(SIGIL_SHARED)
#define SIGIL_API __declspec(dllimport)
#else
#define SIGIL_API
#endif
#elif defined(SIGIL_EXPORTS)
#define SIGIL_API __attribute__((visibility("default")))
#else
#define SIGIL_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SIGIL_RESULT_V1   1u
#define SIGIL_RESULT_V2   2u
#define SIGIL_SUPPORT_V1  1u
#define SIGIL_OPTIONS_V1  1u

#define SIGIL_OK                       0
#define SIGIL_ERR_INVALID_ARG         -1
#define SIGIL_ERR_IO                  -2
#define SIGIL_ERR_UNKNOWN_PLATFORM    -3
#define SIGIL_ERR_UNSUPPORTED_FORMAT  -4
#define SIGIL_ERR_NOT_FOUND           -5
#define SIGIL_ERR_NEEDS_KEY           -6
#define SIGIL_ERR_CRYPTO              -7
#define SIGIL_ERR_OOM                 -8

/* SIGIL_FLAG_FILENAME_FALLBACK: when the binary parser fails, scan the
 * filename for community naming patterns ([ULUS10064] etc.). On by default
 * when sigil_options is NULL.
 *
 * SIGIL_FLAG_3DS_ALLOW_HOMEBREW: disable the "0004" retail-only filter for
 * 3DS extractions, accepting CIA / homebrew title IDs. Off by default. */
#define SIGIL_FLAG_FILENAME_FALLBACK   (1u << 0)
#define SIGIL_FLAG_3DS_ALLOW_HOMEBREW  (1u << 1)

typedef enum {
    SIGIL_PLATFORM_AUTO = 0,
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
    SIGIL_PLATFORM_DREAMCAST
} sigil_platform;

typedef enum {
    SIGIL_SOURCE_BINARY = 0,
    SIGIL_SOURCE_FILENAME = 1
} sigil_source;

/* EXACT vs PREFIX is load-bearing — PREFIX platforms have multiple save
 * artifacts per game that consumers must enumerate-and-bundle. SPLIT means
 * save_id is already a '/'-separated nested path rather than a flat folder
 * name (3DS: 00040000/00033500), so the consumer creates the intermediate
 * directories. */
typedef enum {
    SIGIL_USAGE_FOLDER_EXACT = 0,
    SIGIL_USAGE_FOLDER_PREFIX = 1,
    SIGIL_USAGE_FILE_EXACT = 2,
    SIGIL_USAGE_FILE_PREFIX = 3,
    SIGIL_USAGE_FOLDER_SPLIT = 4
} sigil_usage;

/* Switch CNMT content-meta type. Set only for Switch when the content-meta
 * NCA was parsed; UNKNOWN for all other platforms and keyless fallbacks. */
enum sigil_switch_content_type {
    SIGIL_SWITCH_CONTENT_UNKNOWN = 0,
    SIGIL_SWITCH_CONTENT_APPLICATION,
    SIGIL_SWITCH_CONTENT_PATCH,
    SIGIL_SWITCH_CONTENT_ADDON
};

typedef struct {
    uint32_t       struct_version;
    char           title_id[32];
    char           raw_serial[32];
    /* Literal on-disk save folder/file name (e.g. PS2 BASLUS-217311). Empty when unknown.
     * With usage FOLDER_SPLIT it is a '/'-separated relative path (3DS: 00040000/00033500),
     * and it is left empty rather than falling back to title_id when the path cannot be
     * resolved, because a flat id is not a valid location on those platforms. */
    char           save_id[32];
    sigil_platform platform;
    sigil_source   source;
    sigil_usage    usage;
    /* 1 if the extractor is unverified against real-world samples; 0 otherwise. */
    int            experimental;
    /* Switch only, from CNMT (struct_version >= SIGIL_RESULT_V2). Values from
     * enum sigil_switch_content_type; 0/UNKNOWN when no CNMT was parsed. */
    int            switch_content_type;
    /* Switch only, per-content version from CNMT; 0 when unavailable. */
    uint32_t       title_version;
} sigil_result;

typedef struct sigil_io sigil_io;

struct sigil_io {
    /* Returns bytes read (may be < len near EOF) or negative on error. */
    int     (*read)(void *ctx, uint64_t off, void *buf, size_t len);
    /* Total stream size, or -1 if unknown. */
    int64_t (*size)(void *ctx);
    /* Optional teardown; may be NULL. */
    void    (*close)(void *ctx);
    void     *ctx;
};

/* Per-platform external resources. All fields optional; missing context
 * degrades gracefully (encrypted Switch falls back to filename source,
 * etc.) rather than failing. */
typedef struct {
    uint32_t       struct_version;

    /* Switch: provide ONE of these for AES-XTS NCA decryption. Resolution
     * priority is raw key > text blob > path. */
    const uint8_t *switch_header_key;       /* 32 bytes, or NULL */
    const char    *switch_prod_keys_path;
    const char    *switch_prod_keys_text;
    size_t         switch_prod_keys_text_len;
} sigil_support;

typedef struct {
    uint32_t              struct_version;
    const sigil_support  *support;       /* may be NULL */
    uint32_t              flags;         /* SIGIL_FLAG_* */
} sigil_options;

/* Extract from a path. `hint=SIGIL_PLATFORM_AUTO` sniffs from the file
 * extension. `opts=NULL` uses defaults (filename fallback ON, no support
 * context, retail-only 3DS). */
SIGIL_API int sigil_extract_from_path(const char *path,
                                      sigil_platform hint,
                                      const sigil_options *opts,
                                      sigil_result *out);

/* Extract through a caller-supplied I/O abstraction. `filename_hint` is
 * used for extension sniffing and the filename fallback path; may be NULL. */
SIGIL_API int sigil_extract_from_io(const sigil_io *io,
                                    const char *filename_hint,
                                    sigil_platform hint,
                                    const sigil_options *opts,
                                    sigil_result *out);

/* Returns SIGIL_PLATFORM_AUTO for unknown / NULL slugs. */
SIGIL_API sigil_platform sigil_platform_from_slug(const char *slug);
/* Returns "auto" for invalid values. Pointer is to a static string. */
SIGIL_API const char *sigil_platform_to_slug(sigil_platform p);

SIGIL_API sigil_io *sigil_io_open_file(const char *path);
SIGIL_API sigil_io *sigil_io_open_chd(const char *path);     /* requires SIGIL_WITH_CHD */
SIGIL_API sigil_io *sigil_io_open_cso(const char *path);     /* requires SIGIL_WITH_CSO; .cso/.ciso v1 only */
SIGIL_API sigil_io *sigil_io_open_raw_cd(const char *path);
SIGIL_API void      sigil_io_close(sigil_io *io);

SIGIL_API int sigil_load_header_key_from_prod_keys(const char *path, uint8_t out[32]);

SIGIL_API const char *sigil_strerror(int code);
SIGIL_API const char *sigil_version(void);

#ifdef __cplusplus
}
#endif

#endif
