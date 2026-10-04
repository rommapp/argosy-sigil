# Mega Drive / Genesis, 32X, Master System and Game Gear

Status: located
sigil names a cartridge's save through the libretro default row, `{stem}.srm`, which covers genesis_plus_gx and picodrive; it has no extractor or sync kind for these systems.

## Identification

The platform table has no Genesis, 32X, Master System or Game Gear slug, and carts carry no title id
sigil reads. The emulator names the save after the content file (see
[Identification](../identification.md)).

## Save layouts

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| default | `{stem}.srm` primary; `{stem}.rtc` rtc | | RetroArch `save.c` |

There is no Genesis-specific row. The `genesis_plus_gx` row is limited to
`segacd`, so a cartridge loaded in genesis_plus_gx falls back to the
default row, as does picodrive (see [Save units](../save-units.md)).

## Emulator research

Research date 2026-09-26. Sources are shallow clones read locally unless marked otherwise.

Commits read:

- libretro/Genesis-Plus-GX `c2838c7`
- libretro/picodrive `ab02114`
- libretro/beetle-saturn-libretro `1382b85`
- libretro/yabause master `8926b0c` (yabause core), branch `kronos` `3791ffb2`, branch `yabasanshiro` `09ed8e5b`
- FCare/Kronos `d451a55` (same libretro save code as the libretro/yabause `kronos` branch, which is what the buildbot builds)
- flyinghead/flycast `869038f` (the buildbot builds the libretro core from this repo; libretro/flycast is a deprecated fork per its repo description)
- mednafen 1.32.1 source tarball (mednafen.github.io/releases)
- ares-emulator/ares `4cb8d92` (sparse)
- euan-forrester/save-file-converter `0a9786f` (format reference implementation)

The buildbot repo mapping comes from `libretro-super/recipes/linux/cores-linux-x64-generic`.

GH links below use `blob/master` plus the line numbers at the commits above.

### Per (platform, emulator) rows

Legend for Format:

- raw = memory dump with no header
- expanded = each data byte sits on an odd address with a filler byte (0xFF or 0x00) before it (2x size)
- container = a filesystem holding many games' saves

| Platform | Emulator | Files + naming | Format | Scope + switching options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|---|
| SMS / Game Gear | genesis_plus_gx (libretro) | `{stem}.srm` (frontend) | Raw `sram.sram` buffer, max 64 KiB. Size reported at save time = up to the last non-0xFF byte, so the length varies and trailing 0xFF is trimmed. Empty (all 0xFF) gives size 0 and no file. Initial fill 0xFF. | Per-game | Trivial. The neutral form is the raw bytes, right-padded with 0xFF to 64 KiB (or to cart RAM size) | `libretro/libretro.c:3750-3783`; `core/cart_hw/sms_cart.c:604-607` (always `sram.on=1`), `:1411,1438,1482` (mapper into `sram.sram`) |
| SMS / Game Gear | picodrive (libretro) | `{stem}.srm` | Raw, fixed 0x8000 (32 KiB, "2 banks of 16 KB") zero-filled. Reported only if any byte is nonzero | Per-game | Trivial. PicoDrive pads with 0x00 where GPGX pads with 0xFF. Converting means pad or trim; leading bytes match | `pico/cart.c:1390-1402`; `platform/libretro/libretro.c:1745-1762` |
| Genesis / MD | genesis_plus_gx (libretro) | `{stem}.srm` | Raw 64 KiB address image `sram.sram[addr & 0xffff]` (`sram.c:260-273`). 8-bit odd-byte SRAM is stored expanded (unused even bytes stay 0xFF). EEPROM games (I2C/SPI/93C) also live in `sram.sram`. Trailing 0xFF trimmed as above. Init 0xFF (0x00 for "Sonic 1 Remastered") | Per-game | Trivial. Neutral form is the 64 KiB address image, 0xFF padded | `core/cart_hw/sram.c:63-125, 260-273`; `libretro.c:3750-3783` |
| Genesis / MD | picodrive (libretro) | `{stem}.srm` | Raw `Pico.sv.data`. Size = `sv.end - sv.start + 1`, where the header start is aligned down to even and the end is forced odd, so odd-byte SRAM is expanded. EEPROM carts = 0x2000. calloc 0x00 fill. Reported only if nonzero | Per-game | Trivial. Byte layout matches GPGX from offset 0 when SRAM starts at 0x200000. Differences are pad value and length. UNVERIFIED for carts whose SRAM does not start at a 64 KiB boundary | `pico/cart.c:1300-1336`; `pico/memory.c:825-860` |
| Genesis / MD | ares | `save.ram` / `save.eeprom` in the game pak, persisted under ares's `Saves/` (`desktop-ui/emulator/emulator.cpp:24`: `{userData}/ares/Saves/{system}/`). The exact file name there is UNVERIFIED | Raw. Interleaving of word/upper/lower RAM (`Interface::save(wram, uram, lram)`) is UNVERIFIED | Per-game | Probably trivial. Layout UNVERIFIED | `ares/md/cartridge/board/standard.cpp:12-26` |
| Genesis / MD | Kega Fusion, BlastEm, standalone Mednafen MD | Mednafen MD: `sav/{stem}.srm`-style per `filesys.fname_sav` (`%f.%M%x`, default dir `sav`). Kega and BlastEm are UNVERIFIED | UNVERIFIED | Per-game | UNVERIFIED | mednafen.github.io/documentation (fname_sav) |
| 32X | picodrive (libretro) | `{stem}.srm` | Same cart SRAM path as MD (`Pico.sv`) | Per-game | Trivial | `platform/libretro/libretro.c:1745-1762` (32X support for this path is UNVERIFIED line-by-line) |
| 32X | ares | `save.ram` / `save.eeprom` (game pak) | Raw | Per-game | Trivial | `mia/medium/mega-32x.cpp:55-63` |
| 32X | genesis_plus_gx | Not supported (no 32X emulation) | n/a | n/a | n/a | n/a |

A real MD cartridge booted alongside a Sega CD disc (Mode 1) keeps its SRAM
in `.srm`; see [Sega CD](segacd.md#genesis_plus_gx-and-sega-cd-srm).

### Notes for save sync

- Per-game raw files, safe to sync as opaque blobs:
  - GPGX / PicoDrive MD, SMS, GG `.srm`
- GPGX MD `.srm` length varies because trailing 0xFF is trimmed. Hashing for change detection should normalize (pad to 64 KiB with 0xFF) or accept length drift.

## Open items

- No Genesis, 32X, Master System or Game Gear platform slug or extractor.
- The ares save file name, its RAM interleaving, and the Kega Fusion and BlastEm layouts are UNVERIFIED.
