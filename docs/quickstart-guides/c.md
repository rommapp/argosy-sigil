# C

`#include <sigil.h>`, link `libsigil` and its bundled decompression and
crypto libs ([building.md](../building.md)). Every call returns `SIGIL_OK` or a
negative `SIGIL_ERR_*`; `sigil_strerror(code)` names it. The caller sets
`struct_version` on every struct it passes. Paths are UTF-8 on every
system, Windows included ([building.md](../building.md#windows)).

## 1. Identify the game

Once, at import. Skip when the result is already stored.

```c
int sigil_extract_from_path(
    const char *path,             /* required. Rom file. */
    sigil_platform hint,          /* required. SIGIL_PLATFORM_PSP etc., or sigil_platform_from_slug(slug).
                                     SIGIL_PLATFORM_AUTO sniffs the extension; a bare .iso or .zip needs one. */
    const sigil_options *opts,    /* optional, NULL for defaults. */
    sigil_result *out             /* required. struct_version = SIGIL_RESULT_V4. */
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

A Switch XCI or NSP needs keys: without them extract returns
`SIGIL_ERR_NEEDS_KEY`, and with keys that don't open the content (a key file
older than the dump's key generation, or a wrong header key)
`SIGIL_ERR_KEYS_INCOMPATIBLE`.

```c
typedef struct {
    uint32_t struct_version;
    char title_id[32];            /* "" on gb, gbc, snes. A Switch update or DLC gives its game's id. */
    char raw_serial[32];          /* As found in the binary; a Switch update's or DLC's own id. */
    char save_id[32];             /* On-disk name the emulator keys the save by. "" on gb, gbc, snes. */
    sigil_platform platform;      /* sigil_platform_to_slug() for the slug. */
    sigil_source source;          /* SIGIL_SOURCE_BINARY, SIGIL_SOURCE_FILENAME. */
    sigil_usage usage;            /* SIGIL_USAGE_FOLDER_EXACT, _FOLDER_PREFIX, _FILE_EXACT, _FILE_PREFIX,
                                     _FOLDER_SPLIT. identification.md, "usage", says how to apply save_id for each. */
    int experimental;
    int switch_content_type;      /* SIGIL_SWITCH_CONTENT_UNKNOWN, _APPLICATION, _PATCH, _ADDON. */
    uint32_t title_version;       /* Switch only. */
    uint32_t features;            /* Bit set. SIGIL_FEATURE_RTC: cart has a clock. SIGIL_FEATURE_MBC2: MBC2 RAM. */
    char n64_header[24];          /* N64 only. The cart's name; "" when not plain ASCII. */
    char n64_md5[33];             /* N64 only. The ROM's MD5 in .z64 byte order, uppercase. */
    char n64_md5_n64[33];         /* N64 only. The same in .n64 byte order, as Project64 hashes it. */
} sigil_result;
```

For an N64 ROM, extract reads the whole file to fill the two MD5s; the
standalone N64 emulators name saves from them.

Store `title_id`, `save_id`, `raw_serial`, the platform slug, `features`
and, for N64, the three `n64_*` fields. Rebuild the result from them
later, or build one for a platform sigil cannot extract (Sega CD returns
`SIGIL_ERR_UNKNOWN_PLATFORM`), by filling a zeroed result:

```c
sigil_result game = { .struct_version = SIGIL_RESULT_V4, .features = stored_features };
strncpy(game.title_id, stored_title_id, sizeof(game.title_id) - 1);       /* or leave "" */
strncpy(game.save_id, stored_save_id, sizeof(game.save_id) - 1);          /* or leave "" */
strncpy(game.raw_serial, stored_raw_serial, sizeof(game.raw_serial) - 1); /* pcsx_rearmed's serial cards */
strncpy(game.n64_md5, stored_n64_md5, sizeof(game.n64_md5) - 1);          /* and the other n64_* fields */
```

## 2. Locate the saves

At sync time. No save is read.

```c
int sigil_save_resolve(
    const sigil_save_request *req,   /* required. */
    sigil_save_unit **out            /* required. Free with sigil_save_unit_free(). */
);                                   /* Hashes empty unless req->open is set. */

typedef struct {
    uint32_t struct_version;              /* SIGIL_SAVE_REQUEST_V1 */
    const char *layout;                   /* required. Layout id of the emulator running the game; see
                                             below. */
    const char *platform;                 /* optional. Slug from identification.md, or segacd, fds.
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
                                             under sigil_save_layout_subdirs(layout), twelve levels deep, as
                                             root-relative / paths. The root: the directory the emulator
                                             writes this game's save into; RetroArch: savefile_directory,
                                             plus the core-named subfolder when sort_savefiles_enable is on.
                                             Layouts with profiles: the emulator's base folder, see below. */
    size_t listing_count;
    sigil_save_open_fn open;              /* optional. Set with open_ctx to hash in this call (step 3). Layouts
                                             with profiles read the emulator's profile list through it. */
    void *open_ctx;
    const char *root_path;                /* optional. The root's own path, / or \ separated. Layouts with
                                             profiles read from it where the root sits. */
    const char *profile;                  /* optional. Layouts with profiles: the profile whose saves to take,
                                             by sigil_save_profile.id. */
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
    sigil_save_alternate *alternates;     /* Listed files other option values would take. */
    size_t alternate_count;
} sigil_save_unit;

typedef struct {
    char path[512];                       /* Root-relative. */
    int shared;                           /* 1 for a file every game shares. */
    sigil_save_option options[2];         /* The values that take it; send them to read it. */
    size_t option_count;
} sigil_save_alternate;

typedef struct {
    char path[512];      /* Root-relative. */
    char entry[256];     /* Archive entry name. */
    int role;            /* SIGIL_SAVE_ROLE_PRIMARY, _SIDECAR, _RTC. */
    int present;
    int area;            /* SIGIL_SAVE_AREA_ACCOUNT or _DEVICE on layouts with profiles, _NONE elsewhere. */
} sigil_save_member;
```

`layout` names the emulator, because each keeps its saves differently.
For a libretro core, pass the core's name without `_libretro`
(`genesis_plus_gx`, `mednafen_psx_hw`). For a standalone emulator, pass
its layout id (`dolphin_standalone`, `pcsx2_standalone`, `eden`).
[platforms/](../platforms/README.md#layouts) lists every id with its
emulator. An id with no row gets the libretro default (`<stem>.srm`, plus
`<stem>.rtc` when the cart has a clock), which fits an unlisted libretro
core but names nothing an unlisted standalone emulator writes.

If `alternate_count` is not 0, the root holds this game's saves under
other option values, such as Beetle Saturn's `.bkr` from a build older
than its save-method option. Ask the user, or call again with each
alternate's `options`; sigil never picks one itself.

The listing has to recurse into folders as well as files, twelve levels
below each of `sigil_save_layout_subdirs`, as the bindings' list helpers
do. Sigil tells a PCSX2 folder card from a file card by its contents:
`memcards/Mcd001.ps2` is a folder card when the listing holds paths below
it (`memcards/Mcd001.ps2/_pcsx2_superblock`), and a file card when it is
listed alone.

### Layouts with profiles

The Switch, Wii U, PS Vita and PS3 emulators keep a game's saves in folders
per user profile, and the Switch and Wii U also keep device saves every
profile shares. The PSP and 3DS emulators keep save folders the same way
with no profiles, so everything below applies to them except picking a
profile. [platforms/](../platforms/README.md#layouts) lists the layout ids
and [save-units.md](../save-units.md#profiles) the folders. The root is the emulator's base folder, the one holding
`nand/`, `bis/`, `mlc01/`, `ux0/`, `dev_hdd0/`, `PSP/` or `sdmc/`; a root above it works when the
listing reaches below, and a root inside it works when `root_path` says
where it is. `sigil_save_base` turns any path into the base and the
profile the path lies in, so a caller can list the base instead:

```c
int sigil_save_base(
    const char *layout,           /* required. */
    const char *path,             /* required. Any path around the emulator's folders. */
    char *base, size_t base_cap,  /* required. path cut above the top folder, or path itself. */
    char *profile, size_t profile_cap   /* required. The profile folder path lies in, or "". */
);                                /* SIGIL_ERR_INVALID_ARG when a buffer is too small. */

const char *sigil_save_layout_top(const char *layout);   /* "nand", "bis", "mlc01", "ux0", "dev_hdd0", "PSP", "sdmc", ... or NULL. */
```

```c
int sigil_save_profiles(
    const sigil_save_request *req,  /* required. layout, listing, root_path, and open for the yuzu forks;
                                       the game fields are not read. */
    sigil_save_profile **out,       /* required. Free with sigil_save_profiles_free. NULL when none. */
    size_t *count                   /* required. */
);                                  /* SIGIL_ERR_UNSUPPORTED_FORMAT on a layout without profiles. */
```

The profile is the request's, else the one the root lies in, else the only
one the emulator lists. Resolve returns `SIGIL_ERR_AMBIGUOUS` when two or
more are listed, none is picked and one holds the game's saves. If you
don't know which profile the user plays as, list them with
`sigil_save_profiles`, ask the user, and pass the answer as `profile`;
keep it per user so the question comes once. Without
`open`, a yuzu fork's list can't be read, so pass `profile` or a root inside
the profile's folder.

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
unit->content_hash    /* What RomM stores for the artifact and compares against. */
unit->identity_hash   /* The same over the saves alone, leaving out the clock file. */
```

Compare `content_hash` with RomM's. `identity_hash` is sigil's own: RomM
never sees it, and sync's `changed` is already built on it, so a clock
that ticked doesn't read as a new save. Most clients never read it.

## Upload and restore

`sigil_collect` and `sigil_restore` (see [Sync](#sync)) build and unpack
the artifact for you. One round trip, with `romm_*`, `stored_state`,
`store_state` and `ask_user` standing for your own server client,
storage and UI:

```c
sigil_sync_request req = { .struct_version = SIGIL_SYNC_REQUEST_V1 };
req.save = save_request;              /* as for sigil_save_resolve, with open set */
req.write = write_file;               /* your callbacks, rooted at the save root */
req.remove = remove_file;
req.write_ctx = root;
req.state = stored_state(game, &req.state_len);

/* After the game closes: collect, upload what changed, then keep the state. */
sigil_sync_result *r = NULL;
if (sigil_collect(&req, &r) == SIGIL_OK && r->changed && r->data) {
    int ok = romm_upload(game, r->artifact, r->data, r->len, r->content_hash) == 0;
    if (ok && r->holding) ok = romm_upload(game, "holding.zip", r->holding, r->holding_len, NULL) == 0;
    if (ok) store_state(game, r->state, r->state_len);   /* only once every upload succeeded */
}
sigil_sync_result_free(r);

/* Before the next launch: put the server's save back. */
size_t unit_len = 0;
uint8_t *unit = romm_download(game, &unit_len);
req.state = stored_state(game, &req.state_len);
r = NULL;
int rc = sigil_restore(&req, unit, unit_len, &r);
if (rc == SIGIL_ERR_CONFLICT) {
    /* The saves on disk changed since the last sync. Ask the user, then: */
    req.overwrite_local = 1;
} else if (rc == SIGIL_ERR_AMBIGUOUS) {
    /* More than one profile could take the saves. Ask which is theirs: */
    req.save.profile = ask_user(r->profiles, r->profile_count);   /* returns a copy of the id */
}
if (rc == SIGIL_ERR_CONFLICT || rc == SIGIL_ERR_AMBIGUOUS) {
    sigil_sync_result_free(r);
    r = NULL;
    rc = sigil_restore(&req, unit, unit_len, &r);
}
if (rc == SIGIL_OK) store_state(game, r->state, r->state_len);
sigil_sync_result_free(r);
```

Upload `data` under the name `artifact`; RomM computes the same
`content_hash`. Pass back the `state` the last call returned every time,
so sigil can tell a local change from its own last restore.

### Without collect and restore

`sigil_save_resolve` doesn't build an upload. It gives you `members`, the
files that make up the game's save, and you package them yourself:

1. Upload by `shape`. `SIGIL_SAVE_SHAPE_SINGLE`: send the one member's
   file as it is. `SIGIL_SAVE_SHAPE_MULTI`: zip the members yourself,
   each stored at the zip's root under its `entry`.
   `SIGIL_SAVE_SHAPE_FOLDER`: zip the `key` folder so entries read
   `<key>/<file>`. Name the upload `artifact`.
2. Compare with RomM by the `content_hash` from step 3; it matches what
   RomM computes for that upload.
3. To restore, unpack the artifact yourself. Single: write it to the
   member's `path`. Multi: write each zip entry to the `path` of the
   member with that `entry`. Folder: unzip into the key folder's parent.
   When the emulator hasn't created a primary yet, `expected` gives its
   `path`.

This path writes whole files, so it can't merge a game's saves into a
shared memory card or a profile folder the way `sigil_restore` does. Use
`sigil_collect` and `sigil_restore` wherever they cover the system. Hash
rules: [save-units.md](../save-units.md#hash).

## Memory cards

List the saves on a memory card or backup RAM volume: PS1 cards (the raw
card as `.mcr`, `.mcd` or `.srm`, DexDrive `.gme`, PSP or Vita `.vmp`),
PS2 `.ps2` file cards, GameCube raw cards, Dreamcast VMUs, and Saturn and
Sega CD backup RAM.

```c
int sigil_card_list(
    const sigil_io *io,             /* required. The card file. Its format is detected from the content. */
    sigil_card_listing **out        /* required. Free with sigil_card_listing_free. */
);                                  /* SIGIL_ERR_UNSUPPORTED_FORMAT when the stream is not a card sigil reads. */

typedef struct {
    uint32_t struct_version;
    int format;                     /* SIGIL_CARD_FORMAT_PS1_RAW, _PS1_GME, _PS1_VMP, _PS2, _GAMECUBE_RAW,
                                       _DREAMCAST_VMU, _SATURN_BACKUP, _SEGACD_BRAM. */
    uint32_t total_blocks;
    uint32_t free_blocks;           /* Blocks a new save can use. */
    uint32_t free_slots;            /* Directory slots a new save can use. */
    uint32_t corrupt_count;         /* Saves left out because their block chain is broken. */
    sigil_card_entry *entries;      /* Live saves, in directory order. */
    size_t entry_count;
    sigil_card_entry *corrupt_entries;  /* The left-out saves the card still names; blocks is 0. */
    size_t corrupt_entry_count;
} sigil_card_listing;

typedef struct {
    char name[64];                  /* As stored on the card, e.g. "BASLUSP01041USCHRO00". */
    char owner_id[16];              /* The game id the save carries, as disc identification reports it:
                                       PS1 and PS2 "SLUS-01041", GameCube "47465A45". "" when the
                                       format has none. */
    uint32_t blocks;                /* In the card's own block size. */
    uint32_t first_block;
} sigil_card_entry;
```

## Sync

`sigil_collect` gathers one game's saves into the unit that travels to RomM;
`sigil_restore` puts a unit back and reads it back. [sync.md](../sync.md) has
the rules every system shares (whose saves, managed and unmanaged, damaged
files, companions, refusals); each system's page under
[platforms/](../platforms/README.md) has what its unit holds.

```c
typedef struct {
    uint32_t struct_version;          /* SIGIL_SYNC_REQUEST_V1 */
    sigil_save_request save;          /* As for sigil_save_resolve. save.open is required. */
    const char *const *game_ids;      /* Every id the game's saves may carry: all discs of a set. */
    size_t game_id_count;
    int mode;                         /* SIGIL_SYNC_MANAGED or _UNMANAGED. */
    const uint8_t *state;             /* The blob the last call returned for this platform and emulator. */
    size_t state_len;
    int overwrite_local;              /* restore: the user chose to replace saves that changed locally. */
    sigil_save_write_fn write;        /* restore: writes one file of the save root, returns 0 on success.
                                         write and remove only ever get relative paths with no ".."
                                         segment; sigil fails the restore with SIGIL_ERR_IO before
                                         passing any other. */
    void *write_ctx;
    const char *const *claimed;       /* Saturn, Sega CD, Dreamcast: names from `unowned` the user gave
                                         this game. */
    size_t claimed_count;
    sigil_save_remove_fn remove;      /* restore: removes one file of the save root (called with write_ctx).
                                         Dolphin's GCI folder and PCSX2 folder cards need it to drop a
                                         save the unit lacks; restore refuses with SIGIL_ERR_INVALID_ARG,
                                         writing nothing, when it must remove a file and this is NULL.
                                         A path ending in '/' is a directory sigil emptied (a dropped
                                         PCSX2 save folder, which PCSX2 would still show): remove it. */
    const sigil_sync_companion *companions;   /* Games whose saves this game reads, in the order they go on. */
    size_t companion_count;
    int repair;                       /* Rebuild the damaged structures SIGIL_ERR_DAMAGED named instead of
                                         refusing. */
} sigil_sync_request;

typedef struct {
    const char *const *game_ids;      /* The companion's ids, as for the game's own game_ids. */
    size_t game_id_count;
    const uint8_t *unit;              /* restore: the companion's unit from RomM, or NULL to leave its saves
                                         as they are. */
    size_t unit_len;
} sigil_sync_companion;

int sigil_collect(const sigil_sync_request *req, sigil_sync_result **out);
int sigil_restore(const sigil_sync_request *req, const uint8_t *unit, size_t unit_len,
                  sigil_sync_result **out);
void sigil_sync_result_free(sigil_sync_result *result);

typedef struct {
    uint32_t struct_version;
    char artifact[256];               /* File name the unit travels under. */
    int shape;
    uint8_t *data;                    /* collect: the unit. NULL when the game has no saves. */
    size_t len;
    char content_hash[33];            /* RomM content_hash of the unit. */
    char identity_hash[33];           /* Over the saves themselves; placement and timestamps don't move it. */
    int changed;                      /* identity_hash differs from the last sync. */
    int conflict;                     /* restore wrote nothing because local saves changed; set exactly
                                         when it returns SIGIL_ERR_CONFLICT. The bindings raise the
                                         error and carry no such field. */
    uint8_t *state;                   /* Store it once every upload succeeded; pass it back next time. */
    size_t state_len;
    uint8_t *holding;                 /* collect, Saturn, Sega CD and Dreamcast: zip of the saves on a
                                         shared volume with no known owner. NULL when there are none. */
    size_t holding_len;
    char (*unowned)[64];              /* The names of the saves in holding. */
    size_t unowned_count;
    size_t unowned_changed;           /* How many names at the front of unowned are new or rewritten
                                         since the last collect (at the end of the struct). */
    int restore_again;                /* collect, unmanaged: the saves the last restore wrote were
                                         overwritten. Restore again instead of uploading. */
    sigil_sync_companion_result *companions;   /* collect: one per request companion, in request order. */
    size_t companion_count;
    char problem[512];                /* The save or file at fault; see the refusals below. */
    uint32_t blocks_short;            /* SIGIL_ERR_NO_SPACE: the blocks the save lacked; 0 when there were
                                         enough free blocks but no directory slot, or other saves hold
                                         the blocks a Dreamcast game file must start at. */
    sigil_save_profile *profiles;     /* Layouts with profiles: every profile the emulator lists. */
    size_t profile_count;
    char profile[64];                 /* The profile whose saves collect took or restore wrote. */
    sigil_save_alternate *alternates; /* Listed files other option values would take, as on the unit. */
    size_t alternate_count;
    int hardcore_marker;              /* restore: the unit ended in Argosy's legacy hardcore marker. */
} sigil_sync_result;

typedef struct {
    char id[64];                      /* As its save folder is named: Switch 125D2DBAEBDEB11000296E1E1ECBF401,
                                         Wii U 80000001, Vita3K 00, RPCS3 00000001. */
    char name[64];                    /* The nickname, UTF-8; "" when the emulator keeps none. */
} sigil_save_profile;

typedef struct {
    uint8_t *data;                    /* The companion's unit; NULL when none of its saves are there. */
    size_t len;
    char content_hash[33];
    char identity_hash[33];
    int changed;                      /* identity_hash differs from the companion's last sync. */
} sigil_sync_companion_result;
```

Restore refuses, writing nothing, with the codes in
[sync.md, Refusals](../sync.md#refusals), which also says what `problem`
names for each. Collect refuses with `SIGIL_ERR_DAMAGED`,
`SIGIL_ERR_AMBIGUOUS` and `SIGIL_ERR_IO` in the same way. With each of
these the call still sets `*out`; free it as usual.

## Memory cards

List the saves on a memory card or backup RAM volume: PS1 cards (the raw
card as `.mcr`, `.mcd` or `.srm`, DexDrive `.gme`, PSP or Vita `.vmp`),
PS2 `.ps2` file cards, GameCube raw cards, Dreamcast VMUs, and Saturn and
Sega CD backup RAM. Sync doesn't need it; it's for showing the user
what a card holds.

```c
int sigil_card_list(
    const sigil_io *io,             /* required. The card file. Its format is detected from the content. */
    sigil_card_listing **out        /* required. Free with sigil_card_listing_free. */
);                                  /* SIGIL_ERR_UNSUPPORTED_FORMAT when the stream is not a card sigil reads. */

typedef struct {
    uint32_t struct_version;
    int format;                     /* SIGIL_CARD_FORMAT_PS1_RAW, _PS1_GME, _PS1_VMP, _PS2, _GAMECUBE_RAW,
                                       _DREAMCAST_VMU, _SATURN_BACKUP, _SEGACD_BRAM. */
    uint32_t total_blocks;
    uint32_t free_blocks;           /* Blocks a new save can use. */
    uint32_t free_slots;            /* Directory slots a new save can use. */
    uint32_t corrupt_count;         /* Saves left out because their block chain is broken. */
    sigil_card_entry *entries;      /* Live saves, in directory order. */
    size_t entry_count;
    sigil_card_entry *corrupt_entries;  /* The left-out saves the card still names; blocks is 0. */
    size_t corrupt_entry_count;
} sigil_card_listing;

typedef struct {
    char name[64];                  /* As stored on the card, e.g. "BASLUSP01041USCHRO00". */
    char owner_id[16];              /* The game id the save carries, as disc identification reports it:
                                       PS1 and PS2 "SLUS-01041", GameCube "47465A45". "" when the
                                       format has none. */
    uint32_t blocks;                /* In the card's own block size. */
    uint32_t first_block;
} sigil_card_entry;
```

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
