# Xbox

Status: synced
Every game's saves live inside one hard-disk image the emulator keeps. Collect reads a game's save folders off the FATX partition inside it; restore writes them back and changes nothing else on the drive.

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
filesystem; see [Save layouts](#save-layouts).

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

## Save layouts

| Layout | Files (role, option) | Shared | Verified |
|---|---|---|---|
| `xemu` | | `{xemu_hdd}`: the hard-disk image `hdd_path` names in `xemu.toml` (`[sys.files]`). The option is required: xemu has no default for it and attaches no drive when it is empty, so without it the row names no drive and collect and restore return `SIGIL_ERR_INVALID_ARG`. The save root is the folder holding the image | emulator source (xemu at `v0.8.136`: `config_spec.yml`, `system/vl.c`); drive images xemu wrote, a save restored into another drive and loaded there |
| `x1box` | | `{x1box_hdd}`: the image X1 BOX (`com.izzy2lost.x1box`) copies into its files folder as `hdd.img`, qcow2 or raw; `hdd_path` names another file | emulator source (izzy2lost/xemu at `57bc6d6`: `SetupWizardActivity.kt`) |

Both rows match `xbox` only. `hdd_path` may be the setting as the emulator
writes it, a whole path: sigil takes the file's name. Which file is the
drive comes from the row; whether it is qcow2 or raw is read from its
first four bytes, as qemu and X1 BOX read it themselves, never from its
name (X1 BOX keeps either form as `hdd.img`). The image is a file
every game shares, so `sigil_save_resolve` reports it in `unkeyed` and
names no member.

xemu and X1 BOX run the console's own kernel on the image, so the saves
are where a console keeps them: `E:\UDATA\<save_id>\` holds a game's
saves, one folder per save beside `TitleMeta.xbx` and `TitleImage.xbx`,
and `E:\TDATA\<save_id>\` holds what the game keeps for itself. The
kernel names both folders with the title id in lowercase hex
(`4d530064`; on the `flatout-cabinetos` sample the game itself made
`TDATA/454d0009`), and X1 BOX's importer writes the lowercase name too.
sigil compares names without case, as FATX does.

X1 BOX's second core, hakuX, works on a folder of its own and X1 BOX
copies `UDATA` and `TDATA` between that folder and the image around each
run (`HakuxHddSync.kt`), so the image is the place to sync for either
core. With no image set, hakuX keeps its saves in the app's private
folder, which a client can't reach.

Other emulators' Android roots are in [Saves on Android](../saves-on-android.md).

## Sync

The unit is a zip of the game's two folders as they sit on E:,
`UDATA/4d530064/...` and `TDATA/4d530064/...`, the folder names spelled
as the kernel spells them and everything below as the drive has it.
Each file carries its last-write time from the drive as the zip entry's
time, since a game lists its saves by those times; no hash reads them. A
folder with no file in it isn't carried.

This is the tree X1 BOX's own save manager exports for a game and takes
back (`XboxSaveManager.kt` at `57bc6d6`: `exportZip` zips `UDATA/<id>/`
and `TDATA/<id>/` with each file's time, `importZip` writes them under
the lowercase id), so one of its exports restores as a unit does. The two
differ in what they do with a game's files the zip doesn't hold: X1 BOX
merges and keeps them, sigil's restore replaces the two folders.

Restore replaces the game's two folders with the unit's: what the drive
holds of them goes, the unit's files are written, each with its time, and
a folder gets the time of the newest file in it. `UDATA` and `TDATA` are
made when the drive has none, as on the image xemu publishes. No byte of
a save is converted, and an entry is written the way the console's kernel
writes one: the name padded with `0xFF`, a chain ended with `0xFFFFFFFF`,
the three time stamps alike (checked against entries the kernel wrote in
xemu). So a restored drive collects to the same unit, byte for byte. Every other
title's saves, the rest of E: and the other partitions stay as they are,
byte for byte. The image is then written back and read back: what was
written, then the game's saves on it. A request with `write_at` gets
only the parts of the image that changed, written into the file in place
([Writing in parts](#writing-in-parts)); any other gets the whole image
through `write` as one file. Nothing is written when the drive already
holds the unit's saves, or when anything before the first write fails.
`write` replaces the file whole and, when it fails, leaves the file as it
was (`sigil.h`), so a restore through `write` that fails leaves the drive
as it was. The one exception is a `write` that reports success but whose
file doesn't read back as written: restore fails, and the file holds what
`write` stored. A restore through `write_at` that stops part way leaves
the drive as [Writing in parts](#writing-in-parts) says.

Until a restore in parts ends, E: holds the unit's files beside the ones
they replace: the new files go only into clusters that were free when
restore opened the drive, and the old ones are freed last. Written back
whole, in one write, the new files may take the old ones' clusters.

Restore also reads a zip of `UDATA/` and `TDATA/` as a client took them
off a drive before it used sigil's units, in any case and with the saves
of other games beside the game's, which are left out.

Sync while the emulator is closed: it keeps the image open and writes to
it as the game saves.

| Code | When | `problem` |
|---|---|---|
| `SIGIL_ERR_INVALID_ARG` | either call on `xemu` with no `hdd_path`: xemu has no default drive, so sigil names none | |
| `SIGIL_ERR_NO_TARGET` | restore: the save root holds no drive image. sigil doesn't make one, since the console's other partitions are the user's; a server restoring for a client takes the client's image | the image's file name |
| `SIGIL_ERR_NO_TARGET` | restore: a file's name can't be a FATX name (over 42 characters, or a character FATX refuses) | the unit member |
| `SIGIL_ERR_UNSUPPORTED_FORMAT` | restore: two members go to one file, or one folder is spelled two ways. Either call: a qcow2 image using a feature sigil doesn't write (below) | |
| `SIGIL_ERR_DAMAGED` | the file isn't a drive image with a FATX partition E:, its qcow2 tables don't hold together, or a chain of the game's files on E: is broken | the image |
| `SIGIL_ERR_NO_SPACE` | restore: E: is too full; written in parts, the unit's files need room beside the ones they replace. `blocks_short` counts 16 KiB clusters | the file that didn't fit |
| `SIGIL_ERR_IO` | restore: what sigil staged doesn't read back as the unit, so nothing is written; `write` failed, which leaves the drive as it was; `write_at` failed, which leaves it as [Writing in parts](#writing-in-parts) says; or the file reported written doesn't read back as written (its bytes, or the game's saves on it), and holds what was stored | |
| `SIGIL_ERR_OOM` | restore without `write_at`: the image is over 2 GiB, which sigil won't hold in memory to write back as one file. A raw image is the whole 8 GB drive, so it restores only through `write_at` | |

Many games sign their saves; "non-roamable" titles also mix in the
console's key from `eeprom.bin`, so their saves validate only with the
same EEPROM.

## The drive image

A raw image is the drive byte for byte. A qcow2 image
([qemu's docs/interop/qcow2.txt](https://www.qemu.org/docs/master/interop/qcow2.html))
maps the drive's clusters through two levels of tables to clusters of
the file, each counted in a reference count table. sigil reads version 2
and 3 images with zlib compression and 16-bit reference counts, which is
every image xemu and `qemu-img` make by default. It refuses, rather than
guess at, an image with a backing file, encryption, internal snapshots,
an external data file, extended L2 entries, another compression type or
reference count width, dirty bitmaps or another autoclear feature bit, or
one qemu left marked dirty. A write would make an image's bitmaps stale,
and xemu makes none: its own drives hold no bitmaps extension and no
autoclear bits (every drive in `tests/fixtures/saves/xbox/manifest.tsv`).

A write to a cluster the image holds uncompressed goes in place. A
cluster that is compressed (every cluster of the image xemu publishes)
or was never written is replaced by a new cluster at the end of the
file, counted in the reference counts, and the references the old one
held are dropped, so the image stays what `qemu-img check` calls clean.

E: is at byte `0xABE80000` of a retail drive and `0x1312D6000` bytes
long, whatever the drive's size, so the smallest drive that holds it is
`0x1DD156000` bytes. Its superblock (`FATX`, a volume id, sectors per
cluster, the root directory's cluster) fills 4 KiB; the FAT follows, one
entry per cluster plus a reserved one, rounded up to 4 KiB, 32 bits wide
from `0xFFF0` clusters up. `0` is a free cluster and `0xFFFFFFFF` ends a
chain; a chain that runs into a reserved, bad or media marker
(`0xFFFFFFF0`, `0xFFFFFFF7`, `0xFFFFFFF8`) is damaged. A directory is a
chain of clusters of 64-byte entries: the name's length (`0xE5` deleted,
`0xFF` or `0x00` the end), attributes, a 42-byte name, the first cluster,
the size, and three time stamps packed as FAT packs them with the year
counted from 2000.

That layout is read from X1 BOX's `xemu_fatx_import.c` (izzy2lost/xemu
at `57bc6d6`) and libfatx (mborgerson/fatx `v0.0.8`), which agree, and
the qcow2 layout from xemu's own copy of the format (xemu at `v0.8.136`:
`docs/interop/qcow2.rst`, `block/qcow2.h`). Two things come from drives
the Xbox kernel wrote in xemu instead, because those sources differ from
it:

- The three stamps are creation, last write and last access, in that
  order; both sources call the first one "modified". On the
  `orta-flatout-xemu` sample a game rewrote the files of two save
  folders, and each folder's second stamp moved to the time of the write
  while its first and third stayed.
- The kernel pads a name with `0xFF` and gives a new entry three equal
  stamps (`orta-xemu`); X1 BOX pads with zeros.

The qcow2 and FATX code is a port of CabinetOS's MIT-licensed
`xboxhdd.cpp`; the notice is in
[licenses/cabinetos-MIT.txt](../../licenses/cabinetos-MIT.txt).

## Writing in parts

A request with `write_at` gets the parts of the image that change in this
order, each step in calls of its own, so the image is whole after any
call:

1. What the file gains past its end: new clusters, which nothing in the
   image refers to yet, each page as the step that makes the cluster
   leaves it.
2. The reference counts the new clusters gain.
3. E:'s new files and folders, in clusters that were free when restore
   opened the drive, with their FAT entries; then the L1 and L2 entries
   that map them.
4. `0xE5` on each of the game's old entries no new entry takes the slot
   of: another spelling of a folder, or a folder the unit doesn't have;
   then the L1 and L2 entries that map them.
5. The entries that put the new folders in `UDATA`, `TDATA` or the root,
   each in the slot of the entry it replaces when there is one, so the two
   never show together; the FAT entry that adds a cluster to a folder that
   was there; then the L1 and L2 entries that map them.
6. The FAT entries of the old folders' clusters, freed; then the L1 and
   L2 entries that map them.
7. The reference counts the replaced clusters lose.

The qcow2 half is qemu's own order (qemu at `v9.2.0`:
`block/qcow2-cluster.c` `perform_cow`, `block/qcow2-refcount.c`
`qcow2_alloc_bytes` and `update_refcount`), with each step stored before
the next where qemu flushes the file (`block/qcow2-cache.c`
`qcow2_cache_flush`). The FATX half is sigil's: no source gives one
(libfatx `v0.0.8` `fatx_unlink` frees a chain and then marks its entry
deleted), and in this one nothing on E: is pointed at before it is whole,
and nothing is freed while anything points at it. A page that two steps
change is written once for each, as that step leaves it.

A stop after any call leaves every reference in the image counted and every
chain on E: whole. What it can leave behind is clusters the image counts
that nothing refers to, which `qemu-img check` reports as leaked, and
clusters E:'s FAT marks used that no file holds. A folder the unit
replaces switches with the 64-byte entry written into its slot, a folder
the unit adds appears with its entry, and a folder the unit doesn't have
goes with a 1-byte `0xE5`; so each of the game's two folders is either as
it was or as the unit has it, and a stop between the two leaves one of
each. Restoring again finishes; neither it nor the console frees the space
a stop leaves.

That is what the tests check after every stop, clean or torn, of a restore
in parts into each kind of drive they build and into the sample drives.
`qemu-img check` (11.0.3) found no error on any stop of such restores into
the sample drives, the drives xemu ran games on and the forms `qemu-img`
writes, and xemu 0.8.136 loaded drives stopped just before and just after
a folder's switch, each showing that game's old or new save.

## Open items

- A file's creation and last-access times aren't carried: the unit holds the last-write time, and restore writes it into all three stamps, as the kernel does for a file it creates. A folder gets the time of its newest file in all three, where the kernel keeps a folder's creation time and moves only its last write.
- A folder with no file in it isn't carried (X1 BOX's export keeps one as a folder entry). The kernel makes a game's `UDATA` and `TDATA` folders again when the game starts.
- Restore replaces the game's two folders with the unit's. X1 BOX's own import merges instead and keeps files the zip doesn't hold.
- A drive image over 2 GiB, and any raw image, restores only for a request that gives `write_at`.
- A restore in parts that stops part way leaves the clusters it took counted in the image and marked used on E:. `qemu-img check -r leaks` frees the image's; nothing frees E:'s.
- The Kotlin binding reads a file whole through `SigilFileAccess.read`, so on Android a drive image is held in memory to collect or restore it: a raw 8 GB image is out of its reach until that binding can read part of a file.
- X1 BOX wasn't run: its row and its FATX layout come from its source (`SetupWizardActivity.kt`, `HakuxHddSync.kt`, `XboxSaveManager.kt`, `xemu_fatx_import.c` at `57bc6d6`), and its raw and qcow2 images are the forms tested with xemu.
- A game's saves under another title id (a title that reads an earlier one's saves) stay with that title: `game_ids` isn't read here.
- A drive with partitions past E: (F:, G:) works; saves on those partitions aren't read.
- The extractor is experimental until it is validated against real-world samples.
