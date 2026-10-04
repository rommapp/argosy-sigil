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

## Save formats

Xenia keeps each save as an extracted folder,
`content/<XUID>/<save_id>/00000001/<save name>/`, with its metadata in a
`.header` file under `Headers/`. The XUID is the signed-in profile's.
XenDroid uses the same tree under its own `compose/content/`. Copying
the folder and its header moves a save between Xenia installs; moving
one to a console needs the STFS package rebuilt and re-signed.

## Open items

- GoD containers and STFS packages are not read for identification.
- No Xenia or XenDroid layout row. It would key folders per XUID the way the [profile rows](../save-units.md) key them per user.
- The extractor is experimental until it is validated against real-world samples.
