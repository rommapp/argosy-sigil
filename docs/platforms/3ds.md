# Nintendo 3DS

Status: in development
Sigil reads the title id and `save_id` from a 3DS image. No layout row names a 3DS emulator's save files.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `3ds` | Nintendo 3DS | `.3ds`, `.cci`, `.cxi`, `.app`, `.z3ds`, `.zcci`, `.zcxi` | `0004000000123456` | folder-split | `.3dsx` / `.z3dsx` / `.elf` / `.axf` are homebrew and carry no title id |

Callers may pass `n3ds` for `3ds`.

For the 3DS,
`save_id` is itself a `/`-separated path (`00040000/00033500`, the
16-hex title id split into two 32-bit halves) and `usage` is
`folder-split` to flag that: the consumer creates the nested folders
and never has to know where to split. Both 3DS segments are lowercase
hex because that is the case Azahar writes them in, and storage that
distinguishes case would otherwise hold two of every directory;
`title_id` and `raw_serial` stay uppercase. Everything above it (a user
directory, `sdmc/Nintendo 3DS`, the `title/` root, per-install id
folders) is the emulator's prefix and the consumer's to supply,
because the same title differs per emulator.
When only a filename is available and it carries the low half of a 3DS
id alone (`[0011C500]`), no path can be formed and `save_id` stays
empty; the high half is `0004xxxx`, not a fixed `00040000`, so it is
never assumed.

| Value | Meaning | Example |
|---|---|---|
| `folder-split` | `save_id` is a `/`-separated nested path, not a flat name; consumer creates the intermediate folders | 3DS: `save_id` = `00040000/00033500` |

**3DS filters on `0004` for retail.** Program IDs not starting with `0004`
are filtered as non-retail (system titles, CIAs, etc.). Set
`SIGIL_FLAG_3DS_ALLOW_HOMEBREW` in `opts.flags` to disable the gate
for CIA/homebrew workflows.

**3DS container shapes.** NCSD images (`.3ds`, `.cci`) hold the
program id inside partition 0's NCCH; NCCH images (`.cxi`, `.app`)
hold it at +0x118 of the file itself. The `z`-prefixed extensions are
an Azahar Z3DS wrapper (a 0x20-byte header, then metadata, then a
seekable-zstd payload), and the wrapper's `underlying_magic` names the
inner container, so a mislabelled extension still resolves. `.3dsx`,
`.z3dsx`, `.elf` and `.axf` are homebrew: they carry no title id and
sigil reports none rather than inventing one. `.elf` / `.axf` are too
generic to sniff, so they need an explicit `3ds` hint.

See [Identification](../identification.md) for the fields and `usage` values.

## Emulator research

Scope: battery/flash/EEPROM saves and RTC data, not save states. Researched 2026-09-26 against the default branch of each repo (shallow clones). Line numbers refer to those heads and will drift; the function names are the stable anchor.

`{stem}` = content filename without extension. "RA" = RetroArch frontend. RetroArch's own behaviour is in [RetroArch frontend behaviour](gb.md#retroarch-frontend-behaviour-applies-to-every-libretro-core-that-exposes-memory).

### Emulators

| Platform | Emulator | Files and naming | Format | Scope | Options / versions that change files | Lossless conversion | Source |
|---|---|---|---|---|---|---|---|
| 3DS | Citra / Azahar / Lime3DS (standalone) | Per title, under `{user}/sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/`. SaveData: `title/{tid_high:08x}/{tid_low:08x}/data/00000001/` (a folder tree of the game's own files) plus the sibling `data/00000001.metadata` (16 B `ArchiveFormatInfo`: u32 total_size, u32 num_dirs, u32 num_files, u8 duplicate_data, padding). ExtData: `extdata/00000000/{id_low:08x}/` with a `metadata` file (path uses `{:08X}` in some code paths). Cartridge games use the same SD SaveData path. | **Host-filesystem container**: decrypted, unpacked files, not the DISA/DIFF image of real hardware | per-title folder (plus extdata folder, plus NAND sysdata for system titles) | none that change bytes | Azahar and Lime3DS share the Citra layout, so copying the folder tree is lossless. Real 3DS: Checkpoint/JKSM export the same unpacked file tree. The `.metadata` has to be synthesized (UNVERIFIED how a missing one is handled). | [archive_source_sd_savedata.cpp L21-34, L86-95](https://github.com/azahar-emu/azahar/blob/master/src/core/file_sys/archive_source_sd_savedata.cpp#L21-L34), [archive_extsavedata.cpp L189-203, L335](https://github.com/azahar-emu/azahar/blob/master/src/core/file_sys/archive_extsavedata.cpp#L189-L203), [archive_backend.h L111-118](https://github.com/azahar-emu/azahar/blob/master/src/core/file_sys/archive_backend.h#L111-L118), [fs/archive.h SYSTEM_ID/SDCARD_ID](https://github.com/azahar-emu/azahar/blob/master/src/core/hle/service/fs/archive.h) |
| 3DS | citra (libretro) / azahar (libretro) | Same tree, rooted at `{RA save dir}/Citra/` (libretro/citra) or `{RA save dir}/Azahar/` (azahar's `src/citra_libretro`), falling back to the RA system dir. No `.srm`. | Filesystem container | per-title folder | `citra_use_libretro_save_path` = "LibRetro Default" / "Citra Default" (azahar: `{prefix}_use_libretro_save_path`, prefix UNVERIFIED). The non-default value uses the emulator's normal user dir. | Folder copy between the libretro and standalone roots is lossless | [libretro/citra citra_libretro.cpp L446-466](https://github.com/libretro/citra/blob/master/src/citra_libretro/citra_libretro.cpp#L446-L466), [azahar core_settings.cpp L1072-1101](https://github.com/azahar-emu/azahar/blob/master/src/citra_libretro/core_settings.cpp#L1072-L1101) |

### Conversion notes

- **3DS** saves are folder trees, not files. A sync unit is `title/{high}/{low}/data/00000001/` + `00000001.metadata`, plus the extdata folders the title uses. The extdata ID is not derivable from the title ID in general (UNVERIFIED).

### User profiles

- **verified**: no user accounts. A title's save is `title/<high>/<low>/data/00000001/`, plus any extra data (extdata) folders it uses. See [conversion notes](#conversion-notes).
- **lead**: a title's extended header names the extdata id it may use; some extdata is shared between titles (a series sharing data, system extdata). That id is the flag for "this game keeps data outside its save". Reading it from a retail image needs the NCCH decrypted.

## Open items

- Check one 3DS title known to use shared extdata.
- How much key handling is in scope for reading the 3DS exheader?
