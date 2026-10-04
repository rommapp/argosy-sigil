# Xbox 360

Status: in development
Extraction works, but sigil has no layout for Xenia or XenDroid, so it locates no saves.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `xbox360` | Xbox 360 | `.zar`, `.iso` (needs hint), extracted game folder or `.xex` | `4D5307DC` (4-byte XEX title_id, hex) | folder-exact | experimental |

See [Identification](../identification.md) for the fields.

### XEX title id

**Xbox 360 reads the XEX title id (experimental).** The 4-byte execution-info
title ID is returned as 8-char uppercase hex (`4D5307DC`), which is what
the console, Xenia and XenDroid all use, so `title_id` and `save_id` are
the same string. Optional header values are offsets from the XEX start
rather than from the file, so a XEX embedded in a disc image has its base
added back to each one.

Reachable four ways: a bare `default.xex`, an extracted game folder, a
disc image through the XDVDFS walker, or a `.zar`. GoD containers and
STFS packages are not implemented.

The XDVDFS walker is shared with the original Xbox; see [Xbox](xbox.md#xdvdfs).
A `.zar` is a ZArchive written by Xenia; see [Containers](../containers.md).

## Emulator research

Scope is game saves (battery RAM, EEPROM, flash, NVRAM, memory cards, emulated HDD content). Save states are excluded.
Sources were fetched 2026-09-26 from the default branch of each repo. Line numbers refer to that snapshot and will drift.
Items with no source are marked UNVERIFIED.

Real consoles store saves as STFS "CON" packages, content type `0x00000001` (SavedGame). Xenia instead stores each package **extracted as a host directory**, with the STFS header/metadata kept separately.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| Xenia Canary | `<content_root>/<XUID 16-hex>/<TitleID 8-hex>/00000001/<save file_name>/...` (extracted files). Metadata: `<content_root>/<XUID>/<TitleID>/Headers/00000001/<save file_name>.header` = raw `XContentContainerHeader` (display name, thumbnail etc.), padded to 4 KiB. `<content_root>` = `<storage_root>/content` by default. `storage_root` = the exe folder if `portable` (default true on Windows) or `portable.txt` exists, else `<user docs>/Xenia`. Overridable with `--content_root`. XUID is the signed-in profile's (randomly generated per profile; a default profile uses `B13EBABEBABEBABE`). Content with no user uses XUID `0000000000000000`, and marketplace content is forced to 0. Profile/GPD data: `<content_root>/<XUID>/FFFE07D1/00010000/<XUID>/` | Plain host directory tree plus a header blob | Per title, per profile. Old upstream layouts (`content/<TitleID>/<type>`) are migrated on startup: type `00000001` goes to the user XUID, others to the common XUID, and `Headers` is copied to both | Per-game extraction is trivial and lossless: one directory per save plus its `.header`. Neutral form = `{title_id, content_type=1, file_name, display_name, files[]}`. To inject into another profile, place it under the target XUID. Games that embed the XUID or profile ID inside their own files may reject it (UNVERIFIED per title). Converting to a console CON package requires STFS rebuild and console/profile re-signing with third-party tools (UNVERIFIED) | https://github.com/xenia-canary/xenia-canary (branch `canary_experimental`) `src/xenia/kernel/xam/content_manager.cc` L192-230 (`ResolvePackageRoot`/`ResolvePackagePath`), L45-68 (header write). `src/xenia/kernel/xam/xcontent/xcontent_package_directory.cc` L37-44 (`ComputeHeaderPath`), `xcontent_package.h` L247 (`kGameContentHeaderDirName = "Headers"`). `src/xenia/kernel/xam/profile_manager.cc` L453-512. `src/xenia/emulator.cc` L715-790 (migration). `src/xenia/app/xenia_main.cc` L89-99, L118-130, L482-516 |
| Xenia (upstream master) | `<content_root>/<TitleID>/00000001/<save file_name>/` (**no XUID level**). Thumbnail: `<package>/__thumbnail.png`. Per-profile game user content: `<content_root>/<TitleID>/profile/<user_name>/` | Host directory tree | Per title. Single implicit user | Per-game, lossless. Converting to Canary means moving it under `<XUID>/`, which Canary does automatically on startup | https://github.com/xenia-project/xenia `src/xenia/kernel/xam/content_manager.cc` L26, L60-79, L196-225, L240-254 (last upstream commit 2026-02-18) |

### Profiles

- **verified**: Xenia Canary keeps saves under `<content_root>/<XUID>/<TitleID>/00000001/<name>/`. Content with no user goes under XUID `0000000000000000`. On upgrade, Xenia moves saved games to the user and other content types to the common XUID. See the Xenia Canary row above.
- **lead**: some games write the profile's XUID inside their own save files and refuse a save from another profile. That can't be read from metadata; it would need per-title notes.

### Where saves land on Android

**Xbox 360 on XenDroid** (`xendroid.compose`).
`compose/content/<XUID>/<save_id>/00000001/<package>/`. Content is keyed
by profile first and title second, so `save_id` is the *second* component
and the 16-hex XUID directories above it have to be enumerated.
`00000001` is the saved-game content type, against `00000002` for DLC and
`000B0000` for title updates. Non-profile content lives under the machine
XUID `0000000000000000`, so a tree containing only that XUID holds no
saves. The layout is Xenia's, from `ResolvePackageRoot()`.

Other emulators' Android roots are in [Saves on Android](../saves-on-android.md).

## Open items

- GoD containers and STFS packages are not read for identification.
- No Xenia or XenDroid layout row. It would key folders per XUID the way the [profile rows](../save-units.md) key them per user.
- The extractor is experimental until it is validated against real-world samples.
