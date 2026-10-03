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
#define SIGIL_RESULT_V3   3u
#define SIGIL_SAVE_REQUEST_V1 1u
#define SIGIL_SAVE_UNIT_V1    1u
#define SIGIL_CARD_LISTING_V1 1u
#define SIGIL_SYNC_REQUEST_V1 1u
#define SIGIL_SYNC_RESULT_V1  1u
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
#define SIGIL_ERR_CONFLICT            -9   /* the saves on disk changed since the last sync */
#define SIGIL_ERR_EXISTS              -10  /* a save with that name is already there */
#define SIGIL_ERR_NO_SPACE            -11  /* the card or volume has too few free blocks or slots */
#define SIGIL_ERR_UNCOLLECTED         -12  /* a shared volume holds saves no collect has passed on yet */
#define SIGIL_ERR_DAMAGED             -13  /* a file the saves are in is damaged; the request's `repair` rebuilds it
                                                 when sigil can, and a card sigil can't read stays refused */
#define SIGIL_ERR_REGION              -14  /* a save belongs to another region than the game, which can't read it */
#define SIGIL_ERR_NO_TARGET           -15  /* the unit holds a volume the emulator's settings keep no file for;
                                                 `problem` names it */
#define SIGIL_ERR_AMBIGUOUS           -16  /* more than one file could be the card the emulator uses and the
                                                 options don't say which; `problem` names them, one per line */

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
    SIGIL_PLATFORM_DREAMCAST,
    SIGIL_PLATFORM_XBOX,
    /* Cartridge platforms whose saves key on the content file's stem rather
     * than an id inside the ROM. Extraction reads the cart header only for
     * SIGIL_FEATURE_* facts; title_id and save_id stay empty. */
    SIGIL_PLATFORM_GB,
    SIGIL_PLATFORM_GBC,
    SIGIL_PLATFORM_SNES
} sigil_platform;

/* Cart facts read from the ROM header that change what a save unit holds.
 * RTC: the cart carries a real-time clock, so a libretro core exposes
 * RETRO_MEMORY_RTC and the frontend persists it beside the SRAM (RetroArch
 * writes `<stem>.rtc`). GB: header byte 0x147 in {0x0F, 0x10, 0xFD, 0xFE}.
 * SNES: (ROMType << 8 | ROMSpeed) is 0x5535 (S-RTC) or 0xF93A (SPC7110 RTC). */
#define SIGIL_FEATURE_RTC  (1u << 0)

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
    /* SIGIL_FEATURE_* bits (struct_version >= SIGIL_RESULT_V3). */
    uint32_t       features;
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
SIGIL_API sigil_io *sigil_io_open_chd(const char *path);
SIGIL_API sigil_io *sigil_io_open_cso(const char *path);     /* .cso/.ciso v1 only */
SIGIL_API sigil_io *sigil_io_open_raw_cd(const char *path);
/* Presents the archive's largest member as a stream. `out_name` receives that
 * member's name so the caller can resolve the platform from it; pass NULL to
 * ignore. Seeking backwards restarts the decoder, so callers should read
 * ascending offsets where they can. */
SIGIL_API sigil_io *sigil_io_open_zip(const char *path, char *out_name, size_t name_cap);
/* Opens the first member whose path ends with `suffix`, matched
 * case-insensitively with `\` treated as `/`. Use when the wanted file sits
 * under a directory whose name varies per title, such as a Vita dump's
 * app/<TITLEID>/sce_sys/param.sfo. */
SIGIL_API sigil_io *sigil_io_open_zip_member(const char *path, const char *suffix);
/* Presents one file from inside a ZArchive (.zar / .wua) as a stream. Contents
 * are stored in fixed 64 KiB blocks with an offset record for every sixteen,
 * so unlike the zip backend this is true random access and a read costs one
 * block decompression regardless of how far into the archive it lands. */
SIGIL_API sigil_io *sigil_io_open_zar(const char *path, const char *member);
SIGIL_API void      sigil_io_close(sigil_io *io);

SIGIL_API int sigil_load_header_key_from_prod_keys(const char *path, uint8_t out[32]);

/* ---- Save units (README, "Save units") ---------------------------------------
 * Sigil never touches the filesystem here: the caller lists the root and the
 * subfolders `sigil_save_layout_subdirs` names, and opens members on request. */

typedef enum {
    SIGIL_SAVE_SHAPE_NONE = 0,   /* nothing present */
    SIGIL_SAVE_SHAPE_SINGLE,     /* raw file */
    SIGIL_SAVE_SHAPE_MULTI,      /* flat zip, members at root */
    SIGIL_SAVE_SHAPE_FOLDER      /* zip of the save_id folder(s) */
} sigil_save_shape;

typedef enum {
    SIGIL_SAVE_ROLE_PRIMARY = 0, /* the member that names the unit */
    SIGIL_SAVE_ROLE_SIDECAR,     /* core-owned companion file */
    SIGIL_SAVE_ROLE_RTC          /* RETRO_MEMORY_RTC, expected only with SIGIL_FEATURE_RTC */
} sigil_save_role;

#define SIGIL_SAVE_PATH_MAX  512
#define SIGIL_SAVE_ENTRY_MAX 256

typedef struct {
    char path[SIGIL_SAVE_PATH_MAX];   /* relative to the save root, '/' separated */
    char entry[SIGIL_SAVE_ENTRY_MAX]; /* archive entry name */
    int  role;                        /* sigil_save_role */
    int  present;                     /* 1 when the listing held it */
} sigil_save_member;

typedef struct {
    const char *key;   /* core option key, e.g. "genesis_plus_gx_system_bram" */
    const char *value;
} sigil_save_option;

/* Returns a stream for one member of the save root, or NULL. */
typedef sigil_io *(*sigil_save_open_fn)(void *ctx, const char *relative_path);

typedef struct {
    uint32_t                  struct_version;   /* SIGIL_SAVE_REQUEST_V1 */
    const char               *layout;           /* core or emulator id; unknown ids use the libretro default */
    const char               *platform;         /* platform slug, may be NULL */
    const char               *content_path;     /* path the emulator loaded, verbatim; archive.zip#member.ext for a member */
    const sigil_result       *result;           /* may be NULL when the platform has no title id */
    uint32_t                  features;         /* SIGIL_FEATURE_* when result is NULL (persisted earlier) */
    const sigil_save_option  *options;
    size_t                    option_count;
    const char *const        *listing;          /* relative paths under the root */
    size_t                    listing_count;
    sigil_save_open_fn        open;             /* NULL resolves names only; see sigil_save_hash */
    void                     *open_ctx;
} sigil_save_request;

typedef struct {
    uint32_t           struct_version;      /* SIGIL_SAVE_UNIT_V1 */
    char               key[SIGIL_SAVE_ENTRY_MAX]; /* stem, or save_id for folder layouts */
    int                shape;               /* sigil_save_shape */
    sigil_save_member *members;             /* present, in archive order */
    size_t             member_count;
    sigil_save_member *expected;            /* absent: every applicable primary, and the rtc member when the cart has a clock */
    size_t             expected_count;
    char             (*unkeyed)[SIGIL_SAVE_PATH_MAX]; /* shared files seen in the root; never bundled */
    size_t             unkeyed_count;
    char               artifact[SIGIL_SAVE_ENTRY_MAX]; /* file name the unit travels under */
    char               content_hash[33];    /* RomM content_hash of the artifact; empty when not hashed */
    char               identity_hash[33];   /* content_hash over the non-rtc members */
} sigil_save_unit;

SIGIL_API int  sigil_save_resolve(const sigil_save_request *req, sigil_save_unit **out);
SIGIL_API void sigil_save_unit_free(sigil_save_unit *unit);

/* Fills content_hash and identity_hash of a resolved unit by opening its members. */
SIGIL_API int  sigil_save_hash(sigil_save_unit *unit, sigil_save_open_fn open, void *open_ctx);

/* Subfolders under the save root a layout writes into, so the caller knows
 * what to list. Returns the count written to `out` (at most `cap`). */
SIGIL_API size_t sigil_save_layout_subdirs(const char *layout, const char **out, size_t cap);

/* The base name RetroArch derives for save files (runloop_path_set_basename):
 * the loaded path's file name without its extension, taking the member name
 * for `archive.zip#member.ext`. Returns `out`. */
SIGIL_API const char *sigil_content_stem(const char *content_path, char *out, size_t cap);

/* ---- Memory cards --------------------------------------------------------- */

typedef enum {
    SIGIL_CARD_FORMAT_UNKNOWN = 0,
    SIGIL_CARD_FORMAT_PS1_RAW,        /* .mcr, .mcd, .srm, .bin: the bare 128 KiB card */
    SIGIL_CARD_FORMAT_PS1_GME,        /* DexDrive: 0xF40-byte header, then the card */
    SIGIL_CARD_FORMAT_PS1_VMP,        /* PSP and Vita: 0x80-byte signed header, then the card */
    SIGIL_CARD_FORMAT_PS2,            /* .ps2: PCSX2 file card, with or without ECC */
    SIGIL_CARD_FORMAT_GAMECUBE_RAW,   /* .raw, .gcp: a whole GameCube card image */
    SIGIL_CARD_FORMAT_DREAMCAST_VMU,  /* .bin, .vmu: a 128 KiB VMU flash image */
    SIGIL_CARD_FORMAT_SATURN_BACKUP,  /* .bkr, .bcr, .srm, backup.bin: Saturn backup RAM, internal or cart */
    SIGIL_CARD_FORMAT_SEGACD_BRAM     /* .brm, .srm: Sega CD backup RAM, internal or cart */
} sigil_card_format;

#define SIGIL_CARD_NAME_MAX  64
#define SIGIL_CARD_OWNER_MAX 16

typedef struct {
    char     name[SIGIL_CARD_NAME_MAX];      /* the name stored on the card, e.g. "BASLUSP01041USCHRO00" */
    char     owner_id[SIGIL_CARD_OWNER_MAX]; /* the game id the save carries, as disc identification reports it
                                                (PS1 "SLUS-01041", GameCube "47465A45"); empty when the format has none */
    uint32_t blocks;                         /* blocks the save uses, in the card's own block size */
    uint32_t first_block;                    /* block the save starts at */
} sigil_card_entry;

typedef struct {
    uint32_t          struct_version;   /* SIGIL_CARD_LISTING_V1 */
    int               format;           /* sigil_card_format */
    uint32_t          total_blocks;     /* data blocks the card holds */
    uint32_t          free_blocks;      /* data blocks a new save can use */
    uint32_t          free_slots;       /* directory slots a new save can use */
    uint32_t          corrupt_count;    /* saves left out because their block chain is broken */
    sigil_card_entry *entries;          /* live saves, in directory order */
    size_t            entry_count;
    sigil_card_entry *corrupt_entries;  /* the left-out saves the card still names, with name, owner_id and
                                           first_block; blocks is 0. At most corrupt_count */
    size_t            corrupt_entry_count;
} sigil_card_listing;

/* Lists the saves on a memory card. The format is detected from the content.
 * SIGIL_ERR_UNSUPPORTED_FORMAT when the stream is not a card sigil reads. */
SIGIL_API int  sigil_card_list(const sigil_io *io, sigil_card_listing **out);
SIGIL_API void sigil_card_listing_free(sigil_card_listing *listing);

/* ---- Sync (docs/save-roadmap.md, "Client interface") -------------------------
 * collect gathers one game's saves off the emulator's files into the unit that
 * travels to RomM; restore puts a unit back. sigil keeps what it must remember
 * between calls in an opaque state blob the caller stores and passes back. */

/* Writes one file of the save root; returns 0 on success. */
typedef int (*sigil_save_write_fn)(void *ctx, const char *relative_path, const uint8_t *data, size_t len);

/* Removes one file of the save root; returns 0 on success. */
typedef int (*sigil_save_remove_fn)(void *ctx, const char *relative_path);

typedef enum {
    SIGIL_SYNC_MANAGED = 0,   /* the caller launches the game and calls collect after it closes */
    SIGIL_SYNC_UNMANAGED      /* the game runs outside the caller, which syncs on its own schedule */
} sigil_sync_mode;

/* Another game whose saves the game reads, as a sequel reads its prequel's:
 * its saves go on the game's card or volume beside the game's own. */
typedef struct {
    const char *const *game_ids;   /* the companion's ids, as for the game's own game_ids; at least one */
    size_t             game_id_count;
    const uint8_t     *unit;       /* restore: the companion's unit from RomM, or NULL to leave its saves as they are */
    size_t             unit_len;
} sigil_sync_companion;

/* What collect found of a companion's saves on the game's cards or volumes. */
typedef struct {
    uint8_t *data;                 /* the companion's unit; NULL when none of its saves are there */
    size_t   len;
    char     content_hash[33];
    char     identity_hash[33];
    int      changed;              /* 1 when identity_hash differs from the companion's last sync */
} sigil_sync_companion_result;

typedef struct {
    uint32_t              struct_version;   /* SIGIL_SYNC_REQUEST_V1 */
    sigil_save_request    save;             /* the emulator, content, options and save root; save.open is required */
    const char *const    *game_ids;         /* every id the game's saves may carry, as disc identification reports
                                               them: all discs of a set, and PCSX2's memcardFilters for PS2 */
    size_t                game_id_count;
    int                   mode;             /* sigil_sync_mode */
    const uint8_t        *state;            /* the blob the last call returned, or NULL */
    size_t                state_len;
    int                   overwrite_local;  /* restore: the user chose to replace saves that changed locally */
    sigil_save_write_fn   write;            /* restore: writes a file of the save root */
    void                 *write_ctx;
    const char *const    *claimed;          /* Saturn, Sega CD, Dreamcast: names of saves with no known owner that
                                               the user said belong to this game, as collect reported them in `unowned` */
    size_t                claimed_count;
    sigil_save_remove_fn  remove;           /* restore: removes a file of the save root, called with write_ctx.
                                               Needed where saves are files of their own (Dolphin's GCI folder,
                                               PCSX2 folder cards), so a save the unit lacks can go; restore refuses with
                                               SIGIL_ERR_INVALID_ARG, writing nothing, when it must remove one and
                                               this is NULL. A path ending in '/' names a directory sigil emptied
                                               (a dropped PCSX2 save folder): remove the directory */
    const sigil_sync_companion *companions; /* games whose saves this game reads, in the order they go on */
    size_t                companion_count;
    int                   repair;           /* 1 to rebuild the damaged structures that SIGIL_ERR_DAMAGED
                                               named, instead of refusing */
} sigil_sync_request;

typedef struct {
    uint32_t  struct_version;                  /* SIGIL_SYNC_RESULT_V1 */
    char      artifact[SIGIL_SAVE_ENTRY_MAX];  /* the file name the unit travels under */
    int       shape;                           /* sigil_save_shape */
    uint8_t  *data;                            /* collect: the unit to upload; NULL when the game has no saves */
    size_t    len;
    char      content_hash[33];                /* RomM content_hash of the unit */
    char      identity_hash[33];               /* hash over the saves themselves, unmoved by timestamps and placement */
    int       changed;                         /* 1 when identity_hash differs from the last sync */
    int       conflict;                        /* restore: 1 when it wrote nothing because local saves changed */
    uint8_t  *state;                           /* store it once every upload succeeded; pass it back next time */
    size_t    state_len;
    uint8_t  *holding;                         /* collect, Saturn and Sega CD: a zip of the saves on a shared volume
                                                  with no known owner, "backup.ram" and "cart.ram"; NULL when none.
                                                  Keep it wherever the user can claim them from */
    size_t    holding_len;
    char    (*unowned)[SIGIL_CARD_NAME_MAX];   /* the names of the saves in `holding`, for the user to claim */
    size_t    unowned_count;
    int       restore_again;                   /* collect, unmanaged: 1 when the saves the last restore wrote were
                                                  overwritten, as a core does when it unloads after the restore */
    sigil_sync_companion_result *companions;   /* collect: one per request companion, in request order */
    size_t    companion_count;
    char      problem[SIGIL_SAVE_PATH_MAX];    /* with SIGIL_ERR_NO_SPACE or _REGION, the save at fault; with
                                                  SIGIL_ERR_DAMAGED, the damaged file; with
                                                  SIGIL_ERR_NO_TARGET, the unit member with no file; with
                                                  SIGIL_ERR_AMBIGUOUS, the candidate files, one per line */
    uint32_t  blocks_short;                    /* SIGIL_ERR_NO_SPACE: blocks the save lacked, in the card's block
                                                  size; 0 when a directory slot was missing instead */
} sigil_sync_result;

/* Gathers the game's saves into one unit. Returns SIGIL_ERR_DAMAGED when a structure holding
 * them is damaged and req->repair is 0; *out then names the file in `problem`. */
SIGIL_API int  sigil_collect(const sigil_sync_request *req, sigil_sync_result **out);
/* Puts the unit `unit` back into the save root through req->write, then reads it back to verify.
 * Each of these refusals writes nothing:
 *   SIGIL_ERR_CONFLICT     the saves on disk changed since the last sync and req->overwrite_local is 0.
 *   SIGIL_ERR_UNCOLLECTED  a shared Saturn or Sega CD volume holds saves of other games, or with no
 *                          known owner, that no collect has passed on yet: call collect for the game
 *                          that ran last, then restore again.
 *   SIGIL_ERR_NO_SPACE     the saves don't fit; `problem` names the save, `blocks_short` its shortfall.
 *   SIGIL_ERR_REGION       a GameCube companion's save belongs to another region than the game;
 *                          `problem` names it.
 *   SIGIL_ERR_DAMAGED      a file the saves go in is damaged and req->repair is 0, or it isn't a
 *                          card sigil can read at all; `problem` names the file.
 *   SIGIL_ERR_NO_TARGET    the unit holds a volume the emulator's settings keep no file for (a cart,
 *                          a VMU port); `problem` names the unit member.
 *   SIGIL_ERR_AMBIGUOUS    more than one file could be the emulator's card (Dolphin raw cards of two
 *                          sizes) and the options don't say which; `problem` names them, one per line.
 *                          Collect refuses the same way.
 * With these, *out is set as well; free it as usual. */
SIGIL_API int  sigil_restore(const sigil_sync_request *req, const uint8_t *unit, size_t unit_len,
                             sigil_sync_result **out);
SIGIL_API void sigil_sync_result_free(sigil_sync_result *result);

SIGIL_API const char *sigil_strerror(int code);
SIGIL_API const char *sigil_version(void);

#ifdef __cplusplus
}
#endif

#endif
