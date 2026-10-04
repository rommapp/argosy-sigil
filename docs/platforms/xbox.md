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
8-digit hex in both fields, as Cxbx-Reloaded formats them. That
directory lives inside a FATX disk image rather than on the host
filesystem, so `save_id` identifies a save here without locating one;
see [Where saves land](#where-saves-land).

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

## Where saves land

xemu, on desktop, and X1 BOX (`com.izzy2lost.x1box`), on Android, keep
every game's saves in one hard-disk image (`.qcow2` or `.img`). A save is
the folder `E:\UDATA\<save_id>\`, sometimes with `E:\TDATA\<save_id>\`,
inside the FATX partition E: of that image. The user supplies the image,
so it has no fixed path, and no save is reachable without reading FATX.
Treat `save_id` on this platform as an identifier for matching, not as a
locator.

Copying the `UDATA` folder moves a save between installs. Many games sign
their saves; "non-roamable" titles also mix in the console's key from
`eeprom.bin`, so their saves validate only with the same EEPROM.

Other emulators' Android roots are in [Saves on Android](../saves-on-android.md).

## Open items

- sigil has no qcow2 or FATX reader, so it cannot collect or restore a save from the hard-disk image.
- The extractor is experimental until it is validated against real-world samples.
