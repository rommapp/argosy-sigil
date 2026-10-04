# Super Nintendo

Status: located
Sigil reads the cart header and names the files each core keeps for a cart. Collect and restore don't cover these carts.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `snes` | Super Nintendo | `.sfc`, `.smc` | none; sets `features` | file-prefix | |

Game Boy and SNES carts carry no title id. Sigil validates the header
and reports what the cart holds in `features` (see
[Save units](../save-units.md)); `title_id` and `save_id` stay empty and
the emulator names the save after the content file.

Callers may pass `sfc` or `sfam` for `snes`.

### Features

`SIGIL_FEATURE_RTC` on `snes`: the internal header sits at `0x7FC0`
(LoROM), `0xFFC0` (HiROM) or `0x40FFC0` (ExHiROM), plus a 512-byte skew
when the file size leaves that remainder (copier header). The header's
checksum and complement pair has to xor to `0xFFFF`; bases are tried
deepest first because an ExHiROM image also carries plausible bytes at
the HiROM base. `(ROMType << 8) | ROMSpeed` from bytes `0x16` and `0x15`
equal to `0x5535` (S-RTC) or `0xF93A` (SPC7110 with RTC) sets the flag,
which is what snes9x `memmap.cpp` `InitROM` enables its clock chips on.

RetroArch writes `RETRO_MEMORY_SAVE_RAM` as `<stem>.srm` and
`RETRO_MEMORY_RTC` as `<stem>.rtc` (`save.c`,
`path_init_savefile_rtc`). Clock sizes: mGBA GB 48 bytes, gambatte 8,
snes9x 20.

## Save layouts

Rows from the [layout table](../save-units.md). The default row applies to any cart core with no row of its own.

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| default | `{stem}.srm` primary; `{stem}.rtc` rtc | | RetroArch `save.c` |
| `bsnes` | `{stem}.srm` primary; `{stem}.rtc` rtc; `{stem}.psr` sidecar | | bsnes-libretro `05439f9612` `target-libretro/program.cpp` L503-544, `sfc/cartridge/save.cpp` `saveEpsonRTC`, `saveSharpRTC`, `saveMCC` |

bsnes writes all three files itself. `{stem}.rtc` is the 16-byte clock of
an S-RTC or SPC7110 cart with an Epson RTC (Daikaijuu Monogatari II,
Tengai Makyou Zero); the cart header marks those carts, so their result
carries `SIGIL_FEATURE_RTC` and locate expects the `.rtc`. `{stem}.psr` is
the BS-X Satellaview cart's download RAM.

## Emulator research

Scope: battery/flash/EEPROM saves and RTC data, not save states. Researched 2026-09-26 against the default branch of each repo (shallow clones). Line numbers refer to those heads and will drift; the function names are the stable anchor.

`{stem}` = content filename without extension. "RA" = RetroArch frontend. RetroArch's own behaviour is in [RetroArch frontend behaviour](gb.md#retroarch-frontend-behaviour-applies-to-every-libretro-core-that-exposes-memory).

### Emulators

| Platform | Emulator | Files and naming | Format | Scope | Options / versions that change files | Lossless conversion | Source |
|---|---|---|---|---|---|---|---|
| SNES | snes9x (libretro) | `{stem}.srm`, `{stem}.rtc` | `.srm` raw SRAM, `(1<<(SRAMSize+3))*128` bytes, capped at 128 KiB. `.rtc` = 20 bytes (`RTCData.reg[20]`), only for S-RTC or SPC7110 RTC. | per-game | none | `.srm` is byte-identical to snes9x standalone, bsnes, Mesen2 and Mesen-S. `.rtc` is snes9x-private (20 B vs bsnes 16 B vs Mesen2 24 B). Field mapping is possible but UNVERIFIED. | [libretro.cpp L2329-2370](https://github.com/libretro/snes9x/blob/master/libretro/libretro.cpp#L2329-L2370), [srtc.h L192-195](https://github.com/libretro/snes9x/blob/master/srtc.h#L192) |
| SNES | snes9x (standalone) | `{stem}.srm` and `{stem}.rtc` in the SRAM dir. Sufami Turbo slot B: `{slotBname}.srm`. | Same as the libretro core: raw, `.rtc` 20 bytes | per-game | SRAM dir setting | Same as the libretro core | [memmap.cpp `SaveSRAM` L1937-1985, `SaveSRTC` L1839-1854](https://github.com/libretro/snes9x/blob/master/memmap.cpp#L1839-L1985) (the libretro fork shares the core) |
| SNES | bsnes (libretro) | Core-written in the RA save dir: `{stem}.srm`, `{stem}.rtc`. The core exposes no memory to RA (`retro_get_memory_data` returns NULL). | `.srm` raw. `.rtc` 16 bytes: 8 bytes of packed BCD nibbles plus a u64 LE unix timestamp (S-RTC and Epson RTC-4513 alike). | per-game | none | `.srm` is the same bytes as snes9x. RetroArch-level sync sees no RA-managed `.srm` for this core (the core writes the file itself). | [target-libretro/program.cpp L503-530](https://github.com/libretro/bsnes-libretro/blob/master/bsnes/target-libretro/program.cpp#L503-L530), [libretro.cpp L1039-1047](https://github.com/libretro/bsnes-libretro/blob/master/bsnes/target-libretro/libretro.cpp#L1039-L1047), [sharprtc/memory.cpp `save` L56-66](https://github.com/bsnes-emu/bsnes/blob/master/bsnes/sfc/coprocessor/sharprtc/memory.cpp#L56) |
| SNES | bsnes (standalone v115+) | `{stem}.srm` (also coprocessor data RAM: ARM6, HG51BS169, uPD7725, uPD96050), `{stem}.rtc`, `{stem}.psr` (Satellaview download RAM). Saves sit next to the ROM unless `Paths/Saves` is set. | Raw, `.rtc` 16 B as above | per-game | `settings.path.saves` | Same as the libretro core | [target-bsnes/program/game-rom.cpp L94-119](https://github.com/bsnes-emu/bsnes/blob/master/bsnes/target-bsnes/program/game-rom.cpp#L94-L119), [paths.cpp L1-17](https://github.com/bsnes-emu/bsnes/blob/master/bsnes/target-bsnes/program/paths.cpp) |
| SNES | mesen-s (libretro) | `{stem}.srm` via RA. SPC7110 RTC: the core writes `{romname}.rtc` itself. | `.srm` raw. `.rtc` = 16 regs + u64 **big-endian** seconds = 24 B. | per-game | none | `.srm` is the same bytes as snes9x | [Libretro/libretro.cpp L766-788](https://github.com/libretro/Mesen-S/blob/master/Libretro/libretro.cpp#L766-L788), [Core/Rtc4513.cpp L23-50](https://github.com/libretro/Mesen-S/blob/master/Core/Rtc4513.cpp#L23-L50) |
| SNES | Mesen2 (standalone) | `Saves/{romname}.srm`, `.rtc` (SPC7110 only), `.bs` (BS-X pack). GSU, SA-1 (iRAM when there is no BW-RAM) and NEC DSP RAM also go to `.srm`. | Raw, `.rtc` 24 B (BE time) | per-game | none | `.srm` is the same bytes as snes9x. UNVERIFIED: no S-RTC support was found in the source. | [BaseCartridge.cpp L385-400](https://github.com/SourMesen/Mesen2/blob/master/Core/SNES/BaseCartridge.cpp#L385-L400), [Rtc4513.cpp L22-51](https://github.com/SourMesen/Mesen2/blob/master/Core/SNES/Coprocessors/SPC7110/Rtc4513.cpp#L22-L51) |

### RTC summary

| Emulator | SNES S-RTC / SPC7110 |
|---|---|
| RetroArch generic | same |
| Mesen2 | SPC7110 only: `.rtc` 24 B (BE s) |
| snes9x (lr + standalone) | `.rtc` 20 B |
| bsnes (lr + standalone) | `.rtc` 16 B (LE time) |
| mesen-s (lr) | SPC7110 `.rtc` 24 B (core-written) |

In the RetroArch generic row, "same" means `.rtc` = whatever `RETRO_MEMORY_RTC` the core exposes.

### Conversion notes

- **SNES** `.srm` is byte-identical across snes9x, bsnes, Mesen2 and Mesen-S. The `.rtc` formats are all mutually incompatible (20 / 16 / 24 B). The only affected games are the S-RTC title (Daikaijuu Monogatari II) and SPC7110 with RTC (Tengai Makyou Zero).

### Save compatibility across releases

- The FF3us SRAM Expansion hack grows SRAM from 8 KiB to 32 KiB with a new per-slot layout (https://www.romhacking.net/hacks/4008/; the site returned 403, so this rests on a search snippet).
- Seiken Densetsu 3 stores a 2-byte sum of the save log. A patch keeps saves working only if it leaves the layout alone (https://gamefaqs.gamespot.com/snes/588648-seiken-densetsu-3/faqs/9788).
- FF5 RPGe and Tales of Phantasia DeJap against the Japanese originals: UNVERIFIED.

## Open items

- The conversion mapping between snes9x (20 B), bsnes (16 B) and Mesen (24 B) `.rtc`.
