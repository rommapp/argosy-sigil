# PC Engine / TurboGrafx-16 / SuperGrafx / PC Engine CD

Status: located
sigil has no row of its own for these cores. Their `<stem>.srm` resolves through the default libretro row, and nothing reads the BRAM or syncs it.

## Save formats

Every PCE core keeps one 2 KiB backup RAM per content in `{stem}.srm`
(Populous keeps a 32 KiB on-card RAM instead). A real console shares one
BRAM across every CD game. The BRAM is a "HUBM" file cabinet of
self-delimiting entries, so a dump splits into per-game entries and
merges back without loss. Mednafen standalone writes the same bytes as
the libretro cores, gzipped at times. geargrafx can also keep a 128 KiB
Memory Base 128 that every game shares, `geargrafx_mb128.sav`.

## Open items

- No row names geargrafx's shared `geargrafx_mb128.sav`.
- FBNeo keeps a PC Engine CD game's save as `fbneo/pcecd_<name>.fs`. `<name>` is FBNeo's short name from its disc database, or for a disc it doesn't know, the file name lowercased, cut at the first `-`, `(` or `[`, with everything but letters and digits dropped, at most 32 characters. The `fbneo` row names `fbneo/{romset}.fs`, which never matches. Closing this needs the same disc-id-to-name table as the Neo Geo CD.
