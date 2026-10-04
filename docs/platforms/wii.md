# Wii

Status: in development
sigil identifies Wii discs and WADs, but no layout row covers Wii saves yet, so collect and restore don't handle them.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `wii` | Wii | `.iso`, `.rvz`, `.wbfs`, `.wad` | `525A5445` (hex of ASCII gameId); `.wad`: `00010001574B5445` (full 16-hex title id) | folder-exact; `.wad`: folder-split | |

See [Identification](../identification.md) for the result fields.

Wii and Wii U diverge the same way and for the same reason: Dolphin
writes `Wii/title/00010000/525a4445` and Cemu writes
`mlc01/usr/save/00050000/1010ec00`, both with `{:08x}`, so `save_id` is
the lowercase form of `title_id` on those two platforms
(`title_id=525A4445`, `save_id=525a4445`).

**Wii and GameCube title ids are the hex of ASCII.** The disc header
carries a 4-character ASCII gameId (`RZTE`, `GZLE`). `title_id` is the
hex encoding of those bytes (`52535445`, `475A4C45`) on both consoles,
and `raw_serial` preserves the ASCII form. Where they land on disk
differs: Dolphin's Wii NAND directory is the hex, lowercased, while a
GameCube `.gci` file name carries the ASCII characters, so `save_id`
follows the platform and only Wii's tracks `title_id`. The header
starts at 0 in an `.iso` and at 0x58 in an `.rvz`,
behind the RVZ container header; a console magic backs it (Wii
`5D1C9EA3` at +0x18, GameCube `C2339F3D` at +0x1C). That magic also
names the console when nobody else does, since `.iso`, `.rvz` and
`.wbfs` carry either one and the extension cannot tell them apart. A
platform passed in wins over the magic, so a caller that has already
classified the file keeps its answer; with no platform and no magic
there is nothing left to read, and the extraction fails rather than
guessing from the name.

**In a Wii `.wbfs` the disc header moves; it does not disappear.** A WBFS
file wraps a real disc header behind its own container header; the
wrapped header starts at the first hd sector, whose size the container
records as a shift at offset 8. Sigil reads the id there only when a
console magic backs it (Wii `5D1C9EA3` at +0x18, GameCube `C2339F3D`
at +0x1C). That check is not decoration: `WBFS` is four uppercase ASCII
bytes, so without it the container magic itself passes as a game id and
every wbfs dump collapses to the same bogus `57424653`.

**Wii `.wad` files hold WiiWare and Virtual Console channels.** A WAD is the
NAND install package: a 0x20-byte header, then the certificate chain,
ticket, TMD, data and footer, each 64-byte aligned in that order. The
header holds a size of 0x20 at offset 0, the type at 4 (`Is` 0x4973
installable, `ib` 0x6962 boot2, `Bk` 0x426B backup), the certificate
chain size at 8, the ticket size at 0x10 and the TMD size at 0x14. The
8-byte title id sits at ticket offset 0x1DC, and at TMD offset 0x18C
for a ticketless package; sigil reads the ticket first and falls back
to the TMD. The upper four bytes are the category (`00010001` WiiWare
and Virtual Console, `00010004` channels that ship with a disc game),
the lower four are the ASCII game code, and a package whose lower half
is not `[A-Z0-9]` (IOS, system titles) has no save and returns
`SIGIL_ERR_NOT_FOUND`. `title_id` is the full 16 hex, `raw_serial` the
ASCII code, and `save_id` is `<category>/<code>` in lowercase with
`usage` folder-split, because Dolphin keeps the save at
`Wii/title/<category>/<code>/data` and the category is not fixed. Disc
saves keep their `Wii/title/00010000/<code>` form, so a consumer's Wii
root is `Wii/title/00010000` for discs and `Wii/title` for WADs.

## Emulator research

Scope: in-game saves only (no save states). Sources pinned to the default branch as fetched 2026-09-26. Line numbers are for that snapshot; re-pin to a commit SHA before citing in save sync. Anything without a source is marked UNVERIFIED.

Abbreviations: `DOL` = https://github.com/dolphin-emu/dolphin/blob/master, `CEMU` = https://github.com/cemu-project/Cemu/blob/main, `EDEN` = https://git.eden-emu.dev/eden-emu/eden/src/branch/master, `CIT` = https://github.com/citron-neo/emulator/blob/main, `LIBHAC` = LibHac as vendored in https://github.com/hyjinx-emu/Hyjinx/tree/master/src/Hyjinx.LibHac (the canonical LibHac host, git.ryujinx.app/ryubing/libhac, refused connections from here; the GitHub original Thealexbarney/LibHac is gone).

| Platform | Neutral per-game unit | Identity key |
| --- | --- | --- |
| Wii | Directory tree of `title/<tid-hi>/<tid-lo>/data/` (banner.bin + game files) | 64-bit title ID |

### Row 5. Wii / Dolphin (standalone, Android, libretro)

- Files and path: `<WiiRoot>/title/<tid-hi 8 hex>/<tid-lo 8 hex>/data/` where hex is lowercase, e.g. `title/00010000/524d4745/data/` for RMGE. `tid-hi` is `00010000` for disc games, `00010001` for channels/WiiWare/VC, `00010004` for disc-based game channels (hi values are standard Wii title types, UNVERIFIED in Dolphin source beyond the format string). Source: `DOL/Source/Core/Common/NandPaths.cpp` `GetTitlePath` L63-66, `GetTitleDataPath` L69-72.
- `<WiiRoot>` = `<User>/Wii/` (`FileUtil.cpp` L840 `D_WIIROOT_IDX = D_USER_IDX + "Wii/"`), overridable by `[General] NANDRootPath` (`MainSettings.cpp` L335). Same `<User>` resolution as GC rows; libretro core uses its own `<User>`.
- NAND is a HOST FOLDER, not an image. Dolphin's `HostFileSystem` maps each NAND file to a host file. Per-file ownership/permissions (uid, gid, modes, attribute) live in `<WiiRoot>/fst.bin` (0x20-byte `SerializedFstEntry` per node). Files that exist on disk without an `fst.bin` entry get a default RW/RW/RW entry, so injecting plain files works. Source: `DOL/Source/Core/Core/IOS/FS/HostBackend/FS.cpp` L56-82, L157, L234-270.
- NAND filenames containing `" * / : < > ? \ |`, 0x7F, control chars, or `__` are escaped on the host as `__xx__`. A client copying the tree must copy host names verbatim (do not unescape). Source: `NandPaths.cpp` `EscapeFileName` L119-145.
- Format: arbitrary game files plus `banner.bin` (the save banner/icon the System Menu shows). Some games put files in `data/nocopy/` (not copyable on real hardware; Dolphin's `NandStorage` handles the nocopy flag, `WiiSave.cpp` L124). Games that also write to SD (`WiiSD.raw` / SD sync folder) or `shared2/` are out of this model (UNVERIFIED list of such games).
- Scope: PER-GAME directory inside a shared NAND tree. No option switches it.
- Extraction/injection: lossless per game by copying the `data/` tree while the game is not running. Dolphin's own export (`Tools > Export Wii Save` / `ExportAll`) writes the console-format `private/wii/title/<ID4>/data.bin`: AES-128-CBC with the Wii SD key, a BK header with Dolphin's fixed NG ID `0x0403AC68`, per-file headers carrying permissions/attributes, and an ECDSA-signed cert chain from Dolphin's emulated console. Import reads the same format into the NAND. Source: `DOL/Source/Core/Core/HW/WiiSave.cpp` L47-51, `DataBinStorage` L270-475, `Import` L544-565, `Export` L568-575, `ExportAll` L583-593.
  - `data.bin` from a real Wii or another Dolphin imports fine (signatures are not verified on import, UNVERIFIED; the code path decrypts and copies).
- Neutral per-game form: the raw `data/` directory tree (host-escaped names) is simplest and lossless for Dolphin-to-Dolphin. `data.bin` is the interchange form for real hardware (SaveGame Manager GX, Wii System Menu) and is itself lossless apart from nocopy files.
- Profile/user path components: none. Client needs `<WiiRoot>` and the 64-bit title ID (from the disc header/ID4 -> `00010000` + ASCII hex of the 4-char ID).

The Dolphin `<User>` resolution the row refers to is in [GameCube row 1](gamecube.md#row-1-gamecube--dolphin-standalone-desktop-raw-card-mode). RetroArch's own `.srm` is not used for GC/Wii saves (UNVERIFIED; the core's `retro_get_memory_data` for SAVE_RAM was not checked).

### Per-user saves

- **verified**: no per-user saves. Wii saves are per title in the NAND. Nothing to split.

### Notes for save sync

- Identity fields a client must compute locally: GC 4-char gamecode (from disc ID) plus the per-file DEntry identity; Wii 64-bit title ID; Wii U title ID + Cemu persistent ID; Switch application ID + profile UUID (byte-reversed for yuzu legacy layout, raw for "future" layout), and for Ryujinx the save data ID from `imkvdb.arc`.

## Open items

- sigil has no layout row for Dolphin's Wii NAND, so `sigil_save_resolve`, collect and restore don't cover Wii saves.
- Which games write to SD or `shared2/`, and whether Dolphin verifies `data.bin` signatures on import, are UNVERIFIED (row 5).
