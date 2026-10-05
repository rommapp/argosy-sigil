# Nintendo Switch

Status: synced
sigil identifies Switch XCI and NSP dumps with the user's keys, and collects and restores the save folders the yuzu forks (Eden, Citron, Sudachi, yuzu) keep per profile and per device.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `switch` | Nintendo Switch | `.nsp`, `.xci` | `0100ABCD12345000` | folder-exact | |

| Value | Meaning | Example |
|---|---|---|
| `folder-exact` | One folder per game named exactly `save_id` | `Switch/saves/0100ABCD12345000/` |

See [Identification](../identification.md) for the result fields. The result also carries two Switch-only fields: `switch_content_type` (`SIGIL_SWITCH_CONTENT_UNKNOWN`, `_APPLICATION`, `_PATCH`, `_ADDON`) and `title_version`.

An update or a DLC reads as the game it belongs to. Its saves live in the
game's folder, so `title_id` and `save_id` are the game's id, the one the
CNMT's extended header names (`0100152000022000` for a Mario Kart 8 Deluxe
DLC). `raw_serial` keeps the content's own id (`0100152000023001`), and
`switch_content_type` and `title_version` describe the content itself. A
dump that holds the game beside its update or DLC reads as the game, its
version included. Read from a file name, an id ending in `800` is an update
of the game ending in `000`, and any other id not ending in `000` is a DLC of
the game `0x1000` below its block.

### Switch keys

Switch NCAs are encrypted; extracting the title ID from a retail XCI
or NSP requires the `header_key` from a `prod.keys` file. Pass it via
the support struct in any of three forms. Sigil resolves them in
this priority order:

```c
sigil_support sup = {
    .struct_version = SIGIL_SUPPORT_V1,

    /* (1) Highest priority: a raw 32-byte key, for callers that
     *     already loaded prod.keys themselves. */
    .switch_header_key = my_header_key_bytes,

    /* (2) An in-memory text blob, for sandboxed environments like
     *     Android SAF where direct file I/O is mediated. */
    .switch_prod_keys_text = prod_keys_blob,
    .switch_prod_keys_text_len = prod_keys_blob_len,

    /* (3) A path on disk; sigil opens and parses the file. */
    .switch_prod_keys_path = "/path/to/prod.keys",
};
sigil_options opts = { .struct_version = SIGIL_OPTIONS_V1, .support = &sup };
sigil_extract_from_path("game.xci", SIGIL_PLATFORM_SWITCH, &opts, &r);
```

Without a key, sigil reads nothing from an XCI or NSP and returns
`SIGIL_ERR_NEEDS_KEY`. It never guesses a title id from NCA file names,
which in a retail dump are content ids. When keys are given but don't
open the content, because the key file lacks the key for the dump's key
generation or its header key is wrong, it returns
`SIGIL_ERR_KEYS_INCOMPATIBLE`; a newer `prod.keys` fixes that. Either
error falls through to the file-name source when the caller sets
`SIGIL_FLAG_FILENAME_FALLBACK`, and the result then says
`source = filename`.

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `eden`, `citron`, `sudachi`, `yuzu` | folders per profile, see Profiles: `nand/user/save/0000000000000000/{profile}/{save_id}/` account, `.../00000000000000000000000000000000/{save_id}/` device | | emulator source; on device (Eden and Citron on Android) |
| `lemon` | as the yuzu forks; Lemon is an Eden fork | | emulator source |
| `skyline`, `strato` | `switch/nand/user/save/0000000000000000/00000000000000000000000000000001/{save_id}/` account, `.../00000000000000000000000000000000/{save_id}/` device; one fixed user, no profile list | | emulator source |
| `ryujinx`, `kenjinx` | `bis/user/save/<id>/0/`, where `bis/system/save/8000000000000000/0/imkvdb.arc` gives the game's account save of each user and its device save their `<id>`; users from `system/Profiles.json` | | emulator source |

Ryujinx names a save folder by the id its save index allocated, not by the
title. sigil reads the index to find the game's folders and takes only
each folder's committed copy (`0/`), leaving the working copy, the lock and
Kenji-NX's `TITLEID.txt` out. A unit uses the yuzu forks' names, so it
moves between Ryujinx and the yuzu forks unchanged. A profile id is the
user's `user_id` as `Profiles.json` writes it. Restore writes only into a
folder the index already holds: a game Ryujinx hasn't run has none, and
restore refuses with `SIGIL_ERR_NO_TARGET` naming the member. Run the game
once, then restore.

The layouts list `nand/user/save/0000000000000000` and
`nand/system/save/8000000000000010/su/avators`, which
`sigil_save_layout_subdirs()` names.

The yuzu forks keep a game's saves per user profile, and also keep saves every profile on the device shares. [Save units](../save-units.md#profiles) has the rules for picking a profile.

| Layout | Account | Device | Profile list |
|---|---|---|---|
| yuzu forks | `nand/user/save/0000000000000000/{profile}/{save_id}/` | the same under the all-zero user | `nand/system/save/8000000000000010/su/avators/profiles.dat`; the folder is the profile's UUID with its bytes reversed |

A profile id is named as its save folder is, such as `125D2DBAEBDEB11000296E1E1ECBF401`. The root is the emulator's base folder, the one holding `nand/`.

A
yuzu-fork unit holds `<save_id>/...` for the account save and
`device/<save_id>/...` for the device save. The size file each yuzu fork keeps in a save folder
(`.yuzu_save_size`, `.citron_save_size`, `.sudachi_save_size`,
`.suyu_save_size`) is never collected, written or removed.

A folder under the user save root that matches no profile in
`profiles.dat` is not a user's; Eden can leave such a folder holding only
its size file. Animal Crossing: New Horizons is a device save: the whole
island travels as one device save with no profile.

## Sync

On a layout
with profiles the unit is a zip of the game's save folders under the names
above, and `companions` is `SIGIL_ERR_INVALID_ARG`.

Without
`open`, a yuzu fork's list can't be read, so pass `profile` or a root inside
the profile's folder.

[Sync](../sync.md) has the rest of collect and restore.

## Open items

sigil has no layout for the yuzu forks' newer layout, and reads no NACP.

- Ryujinx: restoring a game it has never run would mean adding an entry to its save index and bumping `lastPublishedId`; until that is checked on a device, restore refuses. When `0/` is missing Ryujinx loads `1/`, which sigil doesn't read. Cache storage on the emulated SD card has its own index, which sigil doesn't read. The rows follow LibHac copies vendored in other projects, since the pinned Ryujinx.LibHac builds weren't reachable.
- Lemon moves to the newer layout (`user/save/account/<uuid>/<TITLEID>/0`) when that folder exists, and lets the user move its NAND folder; the `lemon` row knows the default yuzu tree only.
- Skyline's source is no longer published; its row follows the last version visible in Strato's history (May 2023).
- Whether Mario Kart 8 Deluxe keeps a real device save, or only a copy of account data in the device folder, is not shown. The game's control data (NACP) declares its device save size and would settle it; reading the NACP needs more of the user's keys than sigil uses today.
