# Save compatibility across releases

Research date 2026-09-28. The question is when one release's save works on another release of the same game: a regional release, a revision, a fan translation or hack, or a disc release whose region changes the save's id.

## Findings

Regional cartridge releases often share the save layout but not the text encoding, so a copied save loads with garbled or corrupting names.

- Pokemon Gen 1: the Japanese and English releases use different character sets. Moving a save between them corrupts it, and Japanese text fields shift the data around (https://bulbapedia.bulbagarden.net/wiki/Pok%C3%A9mon_Red_and_Blue_Versions, https://projectpokemon.org/home/forums/topic/59258-english-save-on-a-japanese-copy/).
- Ocarina of Time: every version saves 0x8000 bytes with a `ZELDAZ` magic and a checksum. NTSC and PAL encode the player name with different character sets (https://wiki.cloudmodding.com/oot/Save_Format).
- Super Mario 64: one EEPROM format, with a language field at 0x12 on PAL (https://bryc.github.io/sm64eep/docs).

A matching size and checksum don't make two releases' saves compatible.

Revisions usually keep the layout.

- Ocarina of Time NTSC 1.0, 1.1 and 1.2 differ in RAM addresses, not in the save layout, so their saves are likely interchangeable (source above; UNVERIFIED on hardware).
- Pokemon Ruby and Sapphire 1.0 against 1.1, and the FF7 and MGS PS1 revisions: UNVERIFIED.
- On disc platforms a revision normally keeps its serial, so id-based ownership already treats revisions as one game.

Fan translations and hacks vary.

- Mother 3: Japanese saves work with the translation only after the project's converter runs, because the new font layout garbles stored names. Saves from translation 1.0 to 1.2 carry over to 1.3 (https://mother3.fobby.net/blog/faqs/).
- The FF3us SRAM Expansion hack grows SRAM from 8 KiB to 32 KiB with a new per-slot layout (https://www.romhacking.net/hacks/4008/; the site returned 403, so this rests on a search snippet).
- Seiken Densetsu 3 stores a 2-byte sum of the save log. A patch keeps saves working only if it leaves the layout alone (https://gamefaqs.gamespot.com/snes/588648-seiken-densetsu-3/faqs/9788).
- FF5 RPGe and Tales of Phantasia DeJap against the Japanese originals: UNVERIFIED.
- A patched ROM with its own file name already gets its own save, because cartridge saves follow the content stem.

Disc releases look their saves up by id, so a save from another region is invisible to the game.

- PS1: SLES and SLUS saves don't show up across regions. Rewriting the product code in the directory frame works for many games, not all (https://janstechblog.blogspot.com/2014/01/howto-changing-region-on-ps1-savegames.html).
- GameCube: the fourth character of the game code (E, P, J) is in the save header, and Dolphin keeps separate `USA`, `EUR` and `JAP` folders ([gamecube](platforms/gamecube.md#emulator-research)). Rewriting the code works for some games only. Fire Emblem: Path of Radiance EUR to USA fails (https://www.gc-forever.com/forums/viewtopic.php?t=3471, https://gbatemp.net/threads/change-save-region-for-fire-emblem-path-of-radiance-from-ntsc-to-pal.575449/).
- Saturn: saves are named per region, for example Resident Evil `BIOUDATA_` and Hang-On GP `_01` against `_02` ([saturn](platforms/saturn.md#emulator-research), "Finding the names a disc writes").
- PS2: 71 of the 421 `memcardFilters` entries leave out their own serial, mostly bundles, demos and region variants ([ps2](platforms/ps2.md#emulator-research), "PCSX2 folder memory cards"). That GPL-3 list is the only large source that says saves cross ids.
- Wii and Switch title ids per region, and whether regions share saves: UNVERIFIED.

What breaks compatibility:

- A different save size, which the SNES header (byte 0xFFD8) and GB header (byte 0x149) state.
- A layout change from a hack, or from Japanese text fields.
- A different text encoding in stored names.
- An extra field, such as a language byte.
- A lookup by id on disc platforms.

No source was found for a checksum seeded with the region.

## Existing groupings

- No-Intro parent/clone groups regions, revisions and betas under one parent for picking one release per game (https://datomatic.no-intro.org/stuff/help_pc_dat.txt). It says nothing about saves, and its betas and multi-language PAL releases break the assumption. License UNVERIFIED.
- libretro-database is CC-BY-SA-4.0 (https://github.com/libretro/libretro-database), which can't ship inside MPL sigil, and it has no clone links.
- RetroAchievements groups regional versions and labelled patches under one game (https://docs.retroachievements.org/guidelines/content/working-with-the-right-rom.html). It checks memory addresses, not save layout. Reuse license UNVERIFIED.
- PCSX2 `memcardFilters` is GPL-3.
- emu-atlas records emulator layouts, not ROM families.
