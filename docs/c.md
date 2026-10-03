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
                                             under sigil_save_layout_subdirs(layout), four levels deep, as
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
`sigil_restore` puts a unit back and reads it back. PS1 and PS2 memory
cards, PCSX2 folder cards, GameCube cards and Dolphin's GCI folder, Saturn
and Sega CD backup RAM, and Dreamcast VMUs work today. On PS1 and PS2 the
unit is one per-game card: a raw PS1 card, or an 8 MB `.ps2` card with ECC.
Restore writes a PS1 card back in the form it found: a DexDrive `.gme`
keeps its header, with the frame copies following the directory and a
slot's comment kept only beside the save it was written for; a PSP or Vita
`.vmp` keeps its seed and is signed for its new contents. A new card named
`.VMP` (the `vita_pops` layout) is a signed `.vmp`. A `.vmp` whose
signature doesn't match its card, which the console refuses, is damaged.
A PCSX2 folder card gives the same unit as a file card: restore unpacks it
into the game's save folders and removes files of the game's folders the
unit lacks (through `remove`). It formats a new folder card, one with no
save folders, by writing its `_pcsx2_superblock`. A save folder whose
`_pcsx2_index` doesn't parse is damaged: collect and restore return
`SIGIL_ERR_DAMAGED` naming the index, and with `repair` read the folder
without it and write a fresh one. Restore refuses the same way to write onto
a card that holds saves behind an unusable superblock, and with `repair`
writes a new superblock; collect reads such a card's folders as they are.
A card or volume file sigil can't read as what its path holds (no card
magic, cut short, an internal volume where a cart goes) is damaged too, and so is a save folder of the game or a companion that sigil can't
pack (a subdirectory, a file name longer than a card entry holds), which
PCSX2 still shows. So is a card or volume holding a corrupt save that may be
the game's or a companion's (one carrying their id, one the owner rules give
them, or one whose name the card lost), since collect would read that save
as deleted. A managed restore also refuses a shared volume holding any
corrupt save, which the swap would drop; unmanaged keeps it in place.
`repair` changes none of these: sigil never writes over saves it can't
read. An empty file counts as no card. On
GameCube it is the game's saves as `.gci` files named as Dolphin names
them (`<maker>-<gamecode>-<file>.gci`, escaped): the one file, or a zip
of them named `<stem>.zip`. It is the same whether they came off a raw
card or a GCI folder, and restore puts them into either; F-Zero GX's save
is bound to the target card's serial on a raw card, as Dolphin binds it.
Into a GCI folder, restore refuses with `SIGIL_ERR_NO_SPACE` a save Dolphin
wouldn't load: Dolphin loads the running game's files first, then other
games' (a companion's too) in name order while each leaves a tenth of the
folder's blocks free, up to 112 saves, on a folder the size `MemoryCardSize`
sets. Files are found by a `.gci` extension in any case.
On Saturn and Sega CD it is the game's internal
volume (`backup.ram`) when it has internal saves alone, else a zip named
`<stem>.zip` holding `backup.ram` and `cart.ram` as present. On Dreamcast
it is the game's VMU A1 (`vmu_A1.bin`) when it has saves there alone, else
a zip of `vmu_A1.bin` to `vmu_D2.bin` as present. The member name, not the
size, says which device a volume is, since a 4 MiB Saturn volume can be
Yaba Sanshiro's internal memory or a 32 Mbit cart. Every
volume in a unit is raw, whatever form the emulator stores it in; restore
writes each file back in the emulator's form (gzip, byte expansion), and a
file the emulator hasn't created yet in the form and size its layout row
names. Every Saturn core but Yaba Sanshiro keeps 32 KiB of internal memory,
so a unit whose internal saves need more returns `SIGIL_ERR_NO_SPACE` there
with the blocks they lack.

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
    int conflict;                     /* restore wrote nothing because local saves changed. */
    uint8_t *state;                   /* Store it once every upload succeeded; pass it back next time. */
    size_t state_len;
    uint8_t *holding;                 /* collect, Saturn and Sega CD: zip of the saves on a shared volume
                                         with no known owner. NULL when there are none. */
    size_t holding_len;
    char (*unowned)[64];              /* The names of the saves in holding. */
    size_t unowned_count;
    int restore_again;                /* collect, unmanaged: the saves the last restore wrote were
                                         overwritten. Restore again instead of uploading. */
    sigil_sync_companion_result *companions;   /* collect: one per request companion, in request order. */
    size_t companion_count;
    char problem[512];                /* The save or file at fault; see the refusals below. */
    uint32_t blocks_short;            /* SIGIL_ERR_NO_SPACE: the blocks the save lacked; 0 when there were
                                         enough free blocks but no directory slot, or other saves hold
                                         the blocks a Dreamcast game file must start at. */
} sigil_sync_result;

typedef struct {
    uint8_t *data;                    /* The companion's unit; NULL when none of its saves are there. */
    size_t len;
    char content_hash[33];
    char identity_hash[33];
    int changed;                      /* identity_hash differs from the companion's last sync. */
} sigil_sync_companion_result;
```

A game that reads an earlier title's save, as a sequel reads its prequel's,
lists that title in `companions`. Restore puts each companion's saves on the
game's card, volume or GCI folder beside the game's own. A companion without
a unit keeps the saves it already has there. Collect leaves companion saves
out of the game's unit and hash, and returns each companion's saves as its
own unit, with `changed` against that companion's last sync.

Restore refuses, writing nothing, with:

| Code | When | `problem` |
|---|---|---|
| `SIGIL_ERR_CONFLICT` | the saves on disk changed since the last sync and `overwrite_local` is 0 | |
| `SIGIL_ERR_UNCOLLECTED` | a shared volume holds saves no collect has passed on yet | |
| `SIGIL_ERR_NO_SPACE` | the saves don't fit; `blocks_short` says by how much | the save |
| `SIGIL_ERR_REGION` | a GameCube companion's save is from another Dolphin region than the game | the save |
| `SIGIL_ERR_DAMAGED` | a file the saves go in is damaged and `repair` is 0, it isn't a card sigil can read, or it holds a corrupt save of the game or a companion (above) | the file |
| `SIGIL_ERR_AMBIGUOUS` | more than one file could be the emulator's card and the options don't say which: Dolphin raw cards of two sizes with no `MemoryCardSize`. Collect refuses the same way | the files, one per line |
| `SIGIL_ERR_NO_TARGET` | the unit holds a volume the emulator's settings keep no file for: a `cart.ram` for a core with no cart, a VMU port flycast doesn't keep per game | the unit member |
| `SIGIL_ERR_EXISTS` | Dolphin's GCI folder holds other games' files under the name Dolphin gives a new save and each of its ten `0`-inserted forms, so Dolphin would write over one | the save |

Collect refuses with `SIGIL_ERR_DAMAGED` and `SIGIL_ERR_AMBIGUOUS` in the same way. With each of
these the call still sets `*out`; free it as usual. sigil reports and the
client decides: re-run with `overwrite_local` or `repair` once the user
agreed, or leave the saves as they are.

A game's saves are the ones carrying one of its ids, on its own card and on
the shared cards beside it. Restore puts each save back on the card it was
on, or on the game's own card when it is new; it never touches another
game's save, and refuses with `SIGIL_ERR_NO_SPACE` before writing when the
saves don't fit. A unit holding none of the game's saves is refused with
`SIGIL_ERR_NOT_FOUND`. Saves of other games inside a unit are ignored.

Saturn, Sega CD and Dreamcast saves carry no game id. Every save on a
per-game volume (Beetle Saturn's `<stem>.srm` and `.bcr`, genesis_plus_gx
with `system_bram` = `per game`, flycast's per-game VMUs) is the game's. On
a shared volume (genesis_plus_gx's default `scd_U.brm`, Beetle's shared
volumes, Yaba Sanshiro's `backup.bin`, flycast's `vmu_save_A1.bin`) a save
belongs to the game the user claimed it for, else to the game the state
learned it belongs to, else, in managed mode, to the game the volume was
last swapped in for, else to the game the save-name table gives it by one
of the ids in `title_id` or `game_ids` (`src/save_names.c`; Saturn and
Sega CD product codes as the disc header spells them, Dreamcast product
numbers). The rest come back in `holding`, which the client
keeps where the user can claim them; `holding` and the unit both go up
before the state is stored.

In managed mode, restore swaps each shared volume for one holding only the
game's saves, keeping the file's form (gzip, byte expansion). It refuses
with `SIGIL_ERR_UNCOLLECTED` while the volume holds a save that isn't in
the last holding unit or doesn't match its game's last collect: call
collect for the game that ran last, upload, then restore again. In
unmanaged mode, restore never swaps. It replaces only the game's saves,
and only when the volume is as the last collect saw it; a collect that
then finds the old saves back sets `restore_again`.

genesis_plus_gx picks `scd_E`, `scd_U` or `scd_J` by the disc's region.
sigil takes the region from `genesis_plus_gx_region_detect` when it is
forced, else from the content file name's first region tag, such as
`(USA)`, else from the only one of the three files present. Otherwise
collect and restore return `SIGIL_ERR_NOT_FOUND`.

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
