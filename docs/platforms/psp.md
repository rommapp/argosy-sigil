# PSP

Status: in development
sigil reads the title id from the disc image; no layout row locates PSP saves yet, so collect and restore don't cover them.

PS1 classics running on a PSP use the `vita_pops` layout, which is on the [PlayStation](psx.md) page.

## Identification

| Slug | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|
| `psp` | `.iso`, `.chd`, `.cso` / `.ciso` | `ULUS10064` | folder-prefix | `.cso` experimental |

**PSP saves share a folder prefix.** A single game produces multiple sibling
folders under `PSP/SAVEDATA/`, e.g. `ULUS10064DATA00`,
`ULUS10064SETTINGS`, `ULUS10064SAVE01`. Sigil emits the 9-char prefix
(`ULUS10064`); consumers MUST enumerate every folder under the parent
that starts with that prefix.

`folder-prefix` example: `ULUS10064DATA00`, `ULUS10064SETTINGS`.

`raw_serial` keeps the ID exactly as it appears in the binary, before any
normalization (`ULUS-10064`).

**PSP `.cso` / `.ciso` support is experimental.** v1 CSO with raw-deflate
blocks is decompressed transparently and fed to the standard PSP
extractor. v2 (LZ4) is not supported. Flagged `experimental=1` on the
result until validated against a real CSO sample.

## Save formats

PPSSPP, standalone and libretro, keeps each save as a folder under
`PSP/SAVEDATA/` named by the game: a disc id plus a suffix the game
picks. The folder's `PARAM.SFO` carries hashes of its files, so copy a
folder whole and never edit a file inside it. A game can read other
titles' folders for sequel carry-over. The same root also holds game-data
installs, which are not save progress. The libretro core uses the
frontend's save folder as the memory stick root.

## Open items

- No layout row covers PPSSPP, standalone or libretro.
- PPSSPP's default memory stick root per OS is unconfirmed.
- `.cso` v2 (LZ4) is not supported, and v1 has not been validated against a real CSO sample.
