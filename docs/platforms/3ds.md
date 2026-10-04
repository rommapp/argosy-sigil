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

## Save formats

Citra, Azahar and Lime3DS keep a title's save as an unpacked folder
tree, `title/<high>/<low>/data/00000001/`, plus a small
`00000001.metadata` file beside it and any extra data (extdata) folders
the title uses. The libretro cores keep the same tree under the
frontend's save folder. Real-console tools (Checkpoint, JKSM) export the
same tree, so copying the folder moves a save without loss. The 3DS has
no user accounts.

## Open items

- Check one 3DS title known to use shared extdata.
- How much key handling is in scope for reading the 3DS exheader?
