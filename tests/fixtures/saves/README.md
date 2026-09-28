# Save samples

Real save files that the save tests check sigil against. The files themselves hold game data, so they are not committed. Each platform folder commits only its manifests, which record what every sample is and what the tests expect from it. A test whose sample is missing skips.

## Layout

```
tests/fixtures/saves/<platform>/
  manifest.tsv     committed: one row per sample file
  entries.tsv      committed: expected entries inside card and volume samples
  files/<id>/      not committed: the sample files
```

`<platform>` uses sigil's platform slugs (`psx`, `ps2`, `ngc`, `saturn`, `segacd`, `dc`, `gb`, ...). `<id>` is a short, stable name for one sample, such as `chrono-cross-srm`, unique within the platform.

[WANTED.md](WANTED.md) lists the cases no sample covers yet.

## Adding a sample

1. Copy the file or folder into `files/<id>/`, keeping the name the emulator wrote.
2. Add a row to `manifest.tsv`.
3. For a memory card, backup RAM volume or VMU, add one row per entry to `entries.tsv`.
4. Fill in `verified_by` with how the expected values were checked. A value sigil itself produced doesn't count as verified.

## manifest.tsv

Tab-separated, with a header row. One row per file; a folder sample has one row per file in it.

| Column | Meaning |
|---|---|
| `id` | sample id, the folder under `files/` |
| `path` | file path relative to `files/<id>/` |
| `size` | size in bytes |
| `md5` | MD5 of the file, lowercase hex |
| `emulator` | emulator or libretro core that wrote it, or `unknown` |
| `emulator_version` | version or commit when known, else `unknown` |
| `game` | game title as the release names it |
| `content_id` | the game's serial, game code or title id, or `unknown` |
| `kind` | `sram`, `rtc`, `card`, `volume`, `vmu`, `folder_file`, `gci`, `wrapped` (one save inside a tool's wrapper, such as `.gcs`, `.mcs`, `.psu` or `.BUP`), or `other` |
| `source` | where the file came from: `romm` for the maintainer's RomM store, a URL, or `capture` for a file saved by hand |
| `verified_by` | how the expected values were checked, for example `hex`, `spec-script`, `mymcplus`, `memcardrex` |
| `notes` | anything a test needs to know, or `-` |

## entries.tsv

Tab-separated, with a header row. One row per entry on a card, volume or VMU.

| Column | Meaning |
|---|---|
| `id` | sample id |
| `path` | the card file, relative to `files/<id>/` |
| `entry` | the entry's name as stored on the card |
| `owner_id` | the id the entry carries (product code, game code), or `-` when the format has none |
| `blocks` | blocks the entry uses |
| `data_md5` | MD5 of the entry's data, joined across its blocks in chain order, trimmed to its stated size when the format records one |
| `verified_by` | how the row was checked |
