# Xbox

Status: in development
Extraction works, but sigil locates no saves, because they live inside a FATX hard-disk image rather than on the host filesystem.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `xbox` | Xbox | `.xiso`, `.xiso.iso`, `.iso` (needs hint), extracted game folder or `.xbe` | `TT-027` (XBE certificate title id) | folder-exact | experimental |

The original Xbox diverges for the opposite reason:
there the id is a 32-bit number and the two fields are two renderings
of it. `save_id` is the raw hex the console names its directory after
(`E:\UDATA\4D530064`), while `title_id` is the serial everything else
prints, two publisher letters and the low half in decimal
(`title_id=MS-100`, `save_id=4D530064`).

See [Identification](../identification.md) for the fields.

### XBE certificate

**Xbox reads the XBE certificate (experimental).** The certificate in the XBE
holds a 32-bit title id, and two forms of it matter. The console names
its save directory after the raw hex (`E:\UDATA\4D530064`), while the
serial every tool prints is two publisher letters, a hyphen, and the low
half in decimal (`MS-100`). So `title_id` is the formatted serial and
`save_id` is the hex, diverging the way PS2 and 3DS do. Ids whose prefix
bytes are not `A-Z` (the dashboard, XDK samples) fall back to plain
8-digit hex in both fields, which is what Cxbx-Reloaded's
`FormatTitleId()` does. That directory lives inside a FATX disk image
rather than on the host filesystem, so `save_id` identifies a save here
without locating one; see
[Where saves land on Android](#where-saves-land-on-android).

The certificate is addressed by the virtual address the image loads at,
so its file offset is that address minus the image base. Pass a bare
`default.xbe`, an extracted game folder, or a disc image.

### XDVDFS

**Xbox and Xbox 360 share one filesystem, XDVDFS.** Both consoles use the same
filesystem, so one walker serves `default.xbe` and `default.xex` alike.
What differs is where the game partition starts, and that is probed
rather than assumed: 0 for a trimmed image, then the XGD3, XGD2 and XGD1
bases in ascending order. Ascending matters because an archive-backed IO
seeks forward cheaply and rewinds expensively. Both copies of the
`MICROSOFT*XBOX*MEDIA` magic are checked, the one at the start of the
volume descriptor and the one at `+0x7EC`, so a stray copy of that string
inside game data cannot pass as a partition header.

Directory entries form a binary search tree rather than a flat list, so
the ISO9660 helpers do not carry over. Three details bite: subtree
offsets are in 4-byte units, the absent-child sentinel is `0` or `0xff`
despite the field being 16 bits wide, and names are WINDOWS-1252 compared
case-insensitively. `.xiso` and the compound `.xiso.iso` resolve without
a hint; a bare `.iso` is ambiguous and needs one.

A disc image in a `.zip` is read in place; see [Containers](../containers.md).

## Emulator research

Scope is game saves (battery RAM, EEPROM, flash, NVRAM, memory cards, emulated HDD content). Save states are excluded.
Sources were fetched 2026-09-26 from the default branch of each repo. Line numbers refer to that snapshot and will drift.
Items with no source are marked UNVERIFIED.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| xemu | One HDD image for all games, `xbox_hdd.qcow2` (path in xemu settings, `sys.files.hdd_path`). The prebuilt image is 8 GB qcow2. Saves live on partition E: in `E:\UDATA\<TitleID 8-hex>\` (`TitleMeta.xbx` = `TitleName=...`, `TitleImage.xbx` 128x128 texture) and `E:\UDATA\<TitleID>\<save folder>\` (`SaveMeta.xbx` = `Name=...`, optional `SaveImage.xbx` 64x64, then game files). Some titles also keep data in `E:\TDATA\<TitleID>\`. The per-console `eeprom.bin` sits alongside and holds the HDKey | qcow2 wrapping a raw Xbox disk. The E: FATX partition starts at byte 0xABE80000, size 0x131F00000 (retail layout). X/Y/Z cache partitions at 0x80000/0x2EE80000/0x5DC80000, C: at 0x8CA80000 | One shared container for every game. Optional 8 MiB XMU (memory unit) images, FATX, attached through the monitor (`drive_add`/`device_add usb-storage`) | Feasible and lossless at the file level: extract `E:\UDATA\<TitleID>\` (and `TDATA\<TitleID>\`) as a directory tree. That tree is the neutral form, and it is what real-console tools and FTP use. Reading/writing needs a qcow2 layer (`qemu-img convert` or `qemu-nbd`) plus FATX (mborgerson/fatx: libfatx, fatxfs FUSE, pyfatx `python -m pyfatx -x`). The image must not be open in xemu while it is written. Signatures: many games sign saves with a per-title key. "Non-roamable" titles also mix in the console's HDKey from EEPROM, so their saves only validate with the same `eeprom.bin`. Moving between xemu installs with different EEPROMs needs re-signing or the same EEPROM | https://xboxdevwiki.net/Hard_Drive (partition table). https://xboxdevwiki.net/Xbox_Savegame_System (UDATA/TDATA, meta files). https://xemu.app/docs/required-files/ (8G qcow2 `xbox_hdd.qcow2`). https://xemu.app/docs/xmus/. https://github.com/mborgerson/fatx (`README.md`, `pyfatx/README.md`, `fatxfs/README.md` qcow notes). https://consolemods.org/wiki/Xbox:Games_with_Non-Roamable_(EEPROM-Locked)_Saves. https://github.com/feudalnate/Original-Xbox-Gamesave-Resigners/blob/master/XSavSig.md. The `sys.files.hdd_path` key name is UNVERIFIED |

### Where saves land on Android

**Xbox on X1 BOX** (`com.izzy2lost.x1box`). No host path exists. Saves are
written to `E:\UDATA\<save_id>\` inside a FATX filesystem within the
`.qcow2` or `.img` hard-disk image, and the user supplies that image
through the setup wizard rather than the app placing it anywhere fixed.
Reaching a save means reading FATX out of that image; X1 BOX's own FATX
code only imports a dashboard and exports nothing. Treat `save_id` on
this platform as an identifier for matching, not as a locator. Desktop
xemu has the same property for the same reason.

Other emulators' Android roots are in [Saves on Android](../saves-on-android.md).

### Notes for save sync

- Shared containers (one file, many games) are the geargrafx MB128, the fbneo `shared.memcard`, the mame2003-plus `MEMCARD.NNN`, MAME's `.neo` card and `nvram/neocd/saveram`, opera shared NVRAM, same_cdi shared / MAME `cdimono1` NVRAM, the xemu HDD image, and the Jaguar Memory Track on hardware. Of these, lossless per-game split/inject is well-defined for the PCE BRAM (HUBM entries), the Neo Geo cards/NeoCD (NGH-tagged directory) and the xemu HDD (FATX `UDATA\<TitleID>`). It is defined but untooled for 3DO (Opera linked-mem FS), and unknown for CD-i NVR, MB128 and Memory Track (sync those whole).

## Open items

- sigil has no qcow2 or FATX reader, so it cannot collect or restore a save from the hard-disk image.
- The extractor is experimental until it is validated against real-world samples.
