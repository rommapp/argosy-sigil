# Neo Geo (MVS / AES / CD)

Status: located
sigil has no Neo Geo row of its own. The `fbneo` and `mame2003_plus` rows on [Arcade](arcade.md) name the files those cores keep for a Neo Geo romset, and nothing splits a memory card by game.

## Save formats

FBNeo keeps a cart's backup RAM as `fbneo/<driver>.fs` and its memory
card as `fbneo/<driver>.memcard`, or `fbneo/shared.memcard` in shared
mode. mame2003-plus keeps an 8 KiB NVRAM per game, so its saves don't
carry over to FBNeo or MAME. The MAME-format card is the same bytes in
FBNeo, mame2003-plus and MAME.

## Open items

- FBNeo keeps a Neo Geo CD game's save as `fbneo/ngcd_<name>.fs`, where `<name>` is FBNeo's short name for the disc, looked up from the game id the disc carries. The file name doesn't come from the content's name, and sigil reads no Neo Geo CD id, so the `fbneo` row names `fbneo/{romset}.fs`, which never exists for a CD. Closing this needs Neo Geo CD identification (the game id from the disc) and a table from that id to FBNeo's names.
