# Game Boy and Game Boy Color

Status: located
Sigil reads the cart header and names the files each core keeps for a cart. Collect and restore don't cover these carts.

## Identification

| Slug | Platform | Inputs | `title_id` example | `usage` | Status |
|---|---|---|---|---|---|
| `gb` | Game Boy | `.gb`, `.sgb` | none; sets `features` | file-prefix | |
| `gbc` | Game Boy Color | `.gbc` | none; sets `features` | file-prefix | |

Game Boy and SNES carts carry no title id. Sigil validates the header
and reports what the cart holds in `features` (see
[Save units](../save-units.md)); `title_id` and `save_id` stay empty and
the emulator names the save after the content file.

### Features

`SIGIL_FEATURE_RTC` on `gb` / `gbc`: header byte `0x147` is `0x0F`
(MBC3+TIMER+BATTERY), `0x10` (MBC3+TIMER+RAM+BATTERY), `0xFD` (TAMA5) or
`0xFE` (HuC3). The header checksum over `0x134..0x14C` has to match byte
`0x14D` first; the boot ROM refuses a cart that fails it, so a mismatch
means the bytes are not a Game Boy header. gambatte
(`cartridge_libretro.cpp` `hasRtc`), mGBA (`GB_MBC3_RTC`) and VBA-M
(`gbRTCPresent`) key their `RETRO_MEMORY_RTC` region on the same values.

RetroArch writes `RETRO_MEMORY_SAVE_RAM` as `<stem>.srm` and
`RETRO_MEMORY_RTC` as `<stem>.rtc` (`save.c`,
`path_init_savefile_rtc`). Clock sizes: mGBA GB 48 bytes, gambatte 8,
snes9x 20.

## Save layouts

Rows from the [layout table](../save-units.md). The default row applies to any cart core with no row of its own.

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| default | `{stem}.srm` primary; `{stem}.rtc` rtc | | RetroArch `save.c` |
| `vba_next`, `gpsp` | `{stem}.srm` primary | | no RTC region (`libretro.c` memory maps) |

## Emulator research

Scope: battery/flash/EEPROM saves and RTC data, not save states. Researched 2026-09-26 against the default branch of each repo (shallow clones). Line numbers refer to those heads and will drift; the function names are the stable anchor.

`{stem}` = content filename without extension. "RA" = RetroArch frontend.

### RetroArch frontend behaviour (applies to every libretro core that exposes memory)

- RA writes `RETRO_MEMORY_SAVE_RAM` to `{stem}.srm` and `RETRO_MEMORY_RTC` to `{stem}.rtc` (path inferred from the .srm path). The bytes are exactly what the core exposes; RA adds no header.
  Source: [runloop.c `runloop_path_init_savefile_internal` L4786-4813](https://github.com/libretro/RetroArch/blob/master/runloop.c#L4786-L4813), [save.c `path_init_savefile_rtc` L768-781](https://github.com/libretro/RetroArch/blob/master/save.c#L768-L781), [save.c `content_save_ram_file` L619](https://github.com/libretro/RetroArch/blob/master/save.c#L619).
- Optional compression. `save_file_compression` (default `false`, [config.def.h `DEFAULT_SAVE_FILE_COMPRESSION` L1651](https://github.com/libretro/RetroArch/blob/master/config.def.h#L1651)) wraps the file in RZIP. The magic is `#RZIPv` + version byte (1 = deflate chunks, 2 = zstd chunks, selected by `save_compression_codec`), with a 20-byte header and 4-byte chunk headers. RA reads uncompressed files regardless ([rzip_stream.c L39-68, L216-226](https://github.com/libretro/RetroArch/blob/master/libretro-common/streams/rzip_stream.c#L216-L226)). A sync server must detect `#RZIPv` and inflate before comparing bytes.
- Directory options change the path but not the bytes: `sort_savefiles_enable` (current master default `true`, which gives `saves/{core name}/`), `sort_savefiles_by_content_enable` (default `false`), `savefiles_in_content_dir` ([configuration.c L1999-2005](https://github.com/libretro/RetroArch/blob/master/configuration.c#L1999-L2005), [config.def.h L1064-1066](https://github.com/libretro/RetroArch/blob/master/config.def.h#L1064-L1066)). UNVERIFIED: when the sort default flipped to true.
- Some cores bypass the memory API and write their own files through `RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY` (bsnes, DeSmuME, old melonDS, Nestopia FDS, Mesen FDS, citra/azahar). RA's `.srm` compression setting does not apply to those files.

### Emulators

| Platform | Emulator | Files and naming | Format | Scope | Options / versions that change files | Lossless conversion | Source |
|---|---|---|---|---|---|---|---|
| GB/GBC | gambatte (libretro) | `{stem}.srm`, `{stem}.rtc` | `.srm` raw cart RAM (0 bytes if the header has no battery; Sachen excluded). `.rtc` = **8 bytes**, a host-endian u64 `baseTime` (unix epoch the counter is relative to). No registers, halt time not exported. HuC3 uses its own baseTime. | per-game | none | `.srm` is identical to every other emulator's raw RAM. `.rtc` to VBA 48-byte: synthesize registers from `now - baseTime`. This is functionally lossless, but the halt flag and latched registers are lost. | [libretro.cpp L2877-2903](https://github.com/libretro/gambatte-libretro/blob/master/libgambatte/libretro/libretro.cpp#L2877-L2903), [cartridge_libretro.cpp L43-95](https://github.com/libretro/gambatte-libretro/blob/master/libgambatte/src/mem/cartridge_libretro.cpp#L43-L95), [rtc.h L40-82](https://github.com/libretro/gambatte-libretro/blob/master/libgambatte/src/mem/rtc.h#L40) |
| GB/GBC | sameboy (libretro) | `{stem}.srm`, `{stem}.rtc` | `.srm` raw. `.rtc` = SameBoy's internal `rtc` section struct: `GB_rtc_time_t rtc_real, rtc_latched` (5 B each), `uint64 last_rtc_second`, `uint32 rtc_cycles`, `uint8 tpp1_mr4`, with compiler padding. Host-endian, about 32 B (UNVERIFIED exact size). Exposed for any battery cart, even without an RTC. | per-game | none | The fields map one-to-one onto the VBA 48-byte footer, so conversion is lossless for real/latched/timestamp. The struct is version-internal, so treat it as SameBoy-only. | [libretro.c L1691-1781](https://github.com/LIJI32/SameBoy/blob/master/libretro/libretro.c#L1691-L1781), [gb.h L554-559](https://github.com/LIJI32/SameBoy/blob/master/Core/gb.h#L554-L559) |
| GB/GBC | SameBoy (standalone) | `{stem}.sav` | Raw RAM plus an appended RTC footer. MBC3: **48 B VBA64/BGB footer** (5 x u32 real regs, 5 x u32 latched regs, u64 LE unix time). HuC3: 17 B packed `GB_huc3_rtc_time_t`. TPP1: 20 B (`magic`, `version`, `mr4`, `u64 last_rtc_second`, 4 regs). Reads 44 B (vba32) and legacy SameBoy footers too. | per-game | none | MBC3 footer is byte-compatible with mGBA, VBA-M and BGB (all 48 B). HuC3 is incompatible with mGBA (136 B), so it needs a field conversion. | [gb.c L775-812, L826-958, L1008-1060](https://github.com/LIJI32/SameBoy/blob/master/Core/gb.c#L775-L812) |
| GB/GBC | gearboy (libretro) | `{stem}.srm`, `{stem}.rtc` | `.srm` raw. `.rtc` = 48-byte `RTC_Registers` (10 x s32 regs, s32 LastTime, s32 padding), host-endian, raw internal values (Days not masked, Control not merged). | per-game | none | Close to the VBA layout, but LastTime is 32-bit and the regs are not normalized, so it is not byte-identical to the VBA-M/mGBA `.rtc`. Normalize the fields to convert. | [platforms/libretro/libretro.cpp L1373-1409](https://github.com/drhelius/Gearboy/blob/master/platforms/libretro/libretro.cpp#L1373-L1409), [MBC3MemoryRule.h L26-40](https://github.com/drhelius/Gearboy/blob/master/src/MBC3MemoryRule.h#L26-L40) |
| GB/GBC | Gearboy (standalone) | `{stem}.sav` | Raw plus a 48-byte MBC3 footer in VBA layout. LastTime is written as s32 followed by 4 zero bytes, which equals a u64 LE until 2038. Loader accepts +44 or +48. | per-game | none | Byte-compatible with the VBA-M/mGBA/SameBoy MBC3 footer | [MBC3MemoryRule.cpp `SaveRam` L590-611, `LoadRam` L614-640](https://github.com/drhelius/Gearboy/blob/master/src/MBC3MemoryRule.cpp#L590-L640) |
| GB/GBC | mgba (libretro) | `{stem}.srm`, `{stem}.rtc` (MBC3 only) | `.srm` raw `sramSize`. `.rtc` = 48 B `GBMBCRTCSaveBuffer` (10 x u32 LE + u64 LE unix time), the same bytes as the standalone footer. HuC3/TAMA5 RTC not exposed. | per-game | none | `.srm` + `.rtc` concatenated = the standalone mGBA / VBA-M / SameBoy `.sav`. Lossless. | [libretro.c L1127-1185](https://github.com/mgba-emu/mgba/blob/master/src/platform/libretro/libretro.c#L1127-L1185), [mbc.h L34-59](https://github.com/mgba-emu/mgba/blob/master/include/mgba/internal/gb/mbc.h#L34-L59) |
| GB/GBC | mGBA (standalone) | `{stem}.sav` | Raw plus an appended suffix. MBC3: 48 B (as above). HuC3: 136 B (`regs[0x80]` + u64). TAMA5: 40 B. | per-game | none | MBC3 is lossless to the SameBoy/VBA-M/Gearboy footer. HuC3/TAMA5 are mGBA-specific. The Qt "Save Converter" can strip RTC (0.10.3+). | [mbc.c `GBMBCRTCWrite` L667-691](https://github.com/mgba-emu/mgba/blob/master/src/gb/mbc.c#L667-L691), [huc-3.c L211](https://github.com/mgba-emu/mgba/blob/master/src/gb/mbc/huc-3.c#L211), [tama5.c L432](https://github.com/mgba-emu/mgba/blob/master/src/gb/mbc/tama5.c#L432) |
| GB/GBC | vba-m (libretro) | `{stem}.srm`, `{stem}.rtc` | `.srm` raw. `.rtc`: MBC3 48 B (`sizeof(int)*10 + u64`, starting at `mapperSeconds`). TAMA5 64 B. HuC3 only 8 B (`mapperLastTime`). | per-game | none | MBC3 `.srm`+`.rtc` = the standalone VBA-M `.sav`. HuC3 libretro RTC is a subset of the standalone 24 B, so it is lossy. | [src/libretro/libretro.cpp L179-215, L349-410](https://github.com/visualboyadvance-m/visualboyadvance-m/blob/master/src/libretro/libretro.cpp#L179-L215), [gbMemory.h L208-212](https://github.com/visualboyadvance-m/visualboyadvance-m/blob/master/src/core/gb/gbMemory.h#L208-L212) |
| GB/GBC | VBA-M (standalone) | `{stem}.sav` (battery dir) | Raw RAM plus a footer. MBC3 +48 (a 44-byte legacy footer is also accepted, via the `-4` tolerance). TAMA5 adds `kTama5RamSize` RAM then +64. HuC3 +24. | per-game | none | MBC3 is lossless with mGBA/SameBoy/Gearboy | [gb.cpp L452-480](https://github.com/visualboyadvance-m/visualboyadvance-m/blob/master/src/core/gb/gb.cpp#L452-L480) |
| GB/GBC | Mesen2 (standalone) | `Saves/{romname}.srm`, `Saves/{romname}.rtc` | `.srm` raw. MBC3 `.rtc` = 5 regs + u64 **big-endian milliseconds** = 13 B. | per-game | none | `.srm` lossless. `.rtc` converts to VBA 48 B by field mapping (ms/1000). Latched regs are not stored. | [Gameboy.cpp L165-172](https://github.com/SourMesen/Mesen2/blob/master/Core/Gameboy/Gameboy.cpp#L165-L172), [GbMbc3Rtc.h L38-70](https://github.com/SourMesen/Mesen2/blob/master/Core/Gameboy/Carts/GbMbc3Rtc.h#L38-L70) |
| GB/GBC | Pizza Boy C (Android) | UNVERIFIED: `.sav` next to the ROM or in the app save dir | UNVERIFIED (closed source) | per-game | - | UNVERIFIED | none found |

### RTC summary

| Emulator | GB MBC3 RTC |
|---|---|
| RetroArch generic | `.rtc` = whatever `RETRO_MEMORY_RTC` the core exposes |
| gambatte (lr) | `.rtc` 8 B, u64 baseTime |
| sameboy (lr) | `.rtc` internal struct, about 32 B |
| gearboy (lr) | `.rtc` 48 B, raw regs + s32 time |
| mgba (lr) | `.rtc` 48 B, VBA/BGB layout |
| vba-m (lr) | `.rtc` 48 B, VBA layout |
| SameBoy / mGBA / VBA-M / Gearboy (standalone) | appended 48 B footer |
| Mesen2 | separate `.rtc` 13 B (BE ms) |

Canonical MBC3 layout (48 B, little-endian): `u32 sec, min, hour, day_lo, day_hi_ctrl; u32 latched x5; u64 unix_time`. mGBA, VBA-M, SameBoy and Gearboy standalone all emit it, and the mGBA and VBA-M libretro cores emit it as a separate `.rtc`. The bytes match, but the emulators pair the timestamp with different registers (2.1), so the same 48 bytes can mean different clocks.

### What the stored clock means

Researched 2026-09-28. Pinned commits: gambatte-libretro `d9d6cd06`, SameBoy `213a12ce`, mgba `83846bb3`, visualboyadvance-m `4466886c`, vba-next `788192f2`, Mesen2 `b9fa69dd`, Gearboy `79fb5031`, tgbdual-libretro `0392c9c4`, bsnes `0c2fa0db`, gpsp `5819380c`.

DH holds day bit 8 (bit 0), halt (bit 6) and carry (bit 7).

- gambatte (libretro): the 8-byte `.rtc` is a host-endian u64 `baseTime`, an absolute anchor. The counter is `now - baseTime`. No registers, halt, carry or halt time are stored, so a halted clock comes back running. HuC3 also stores only a baseTime. A host clock earlier than `baseTime` wraps the unsigned subtraction and sets carry (`cartridge_libretro.cpp` L66-92, `rtc.h` L40-48, `rtc.cpp` L41-64; runtime behaviour UNVERIFIED).
- SameBoy standalone (and bsnes SGB, which vendors SameBoy): MBC3 writes the 48-byte footer with current and latched regs and `time(NULL)` at save. On load it runs the clock forward to now unless halted. A stamp later than host time, or before 1997, resets the anchor to now and sets carry (`Core/gb.c` L775-812, L876-889, L1008-1066; `timing.c` L308-381).
- SameBoy (libretro): the `.rtc` is the 32-byte `rtc` section, host-endian. Its layout is real regs (5 B), latched regs (5 B), 6 B padding, u64 `last_rtc_second`, u32 cycles, u8 `tpp1_mr4` and 3 B padding. In sync mode (the default) `last_rtc_second` is host time at the last tick, and a stamp later than host time freezes the clock until the host passes it. In accurate mode it is an emulated counter and the clock stops while the game is off. The libretro core doesn't persist the HuC3 clock (`gb.h` L498, L554-559; `timing.c` L383-409; `libretro_core_options.inc` L659-671).
- mGBA GB: 10 u32 LE plus u64 LE `unixTime`. The current slot holds regs computed at save. The latched slot holds regs as of the last latch, and `unixTime` is that latch time. On read it loads only the latched slot and `unixTime`, so mGBA's pair is latched regs at the last latch. It ignores halt, and a host clock earlier than the stamp runs the clock backward (`src/gb/mbc.c` L648-691, `src/gb/mbc/mbc.c` L14-62).
- VBA-M GB: 48 bytes from `mapperSeconds`, host-endian, and it also reads 44. `mapperLastTime` is the last latch time, which is also when the current regs were last updated, so current regs plus the stamp are a consistent pair. The current days field can hold up to 511 unmasked. A host clock earlier than the stamp re-anchors to now (`gbMemory.h` L22-43, `gb.cpp` L455-478, `gbMemory.cpp` L250-303, L359-368).
- Mesen2 GB: 13 bytes, 5 current regs plus u64 big-endian milliseconds at save. No latched regs. On load it runs the clock forward even when halted. A host clock earlier than the stamp changes nothing (`GbMbc3Rtc.h` L38-71).
- Gearboy: standalone writes 10 s32 plus an s32 `LastTime` at save plus 4 bytes of padding. That matches the 48-byte form on little-endian hosts until 2038. The libretro core exposes raw `m_RTC`, with Days unmasked and day bit 8 missing from Control. Halted clocks don't advance. A host clock earlier than the stamp re-anchors to now (`MBC3MemoryRule.cpp` L540-611, L664-677, L738-741).
- TGB Dual (libretro): 4 bytes, a host-endian u32 of emulated seconds. No wall time, so the clock stops while the game is off. It saves neither the game-set offset nor halt or carry (`libretro.cpp` L500-502, `dmy_renderer.cpp` L185-196, L266-310).
- higan: not checked.

Stamp pairings in the 48-byte form:

- SameBoy and Gearboy: current regs at save time.
- VBA-M: current regs at last latch time.
- mGBA: latched regs at last latch time.

Moving an mGBA save to VBA-M, SameBoy or Gearboy counts the interval between the last latch and the save twice unless the converter takes mGBA's latched slot. Moving a SameBoy save to mGBA loses that interval unless the converter sets latched equal to current.

### Conversion notes

- **GB/GBC SRAM** is raw in every emulator. Normalize to `(sram, rtc48 | null)`. The `.sav`-with-footer form = `sram || rtc48`, and `filesize - sram_size` tells you the footer kind (0, 44, 48, and mapper-specific sizes for HuC3/TAMA5/TPP1). `sram_size` comes from ROM header byte 0x149 (MBC2 = 512 B). From gambatte `.rtc` (8 B) or Mesen2 `.rtc` (13 B) you can only synthesize a 48 B footer; the latched regs and halt state are lost.

### Save compatibility across releases

- Pokemon Gen 1: the Japanese and English releases use different character sets. Moving a save between them corrupts it, and Japanese text fields shift the data around (https://bulbapedia.bulbagarden.net/wiki/Pok%C3%A9mon_Red_and_Blue_Versions, https://projectpokemon.org/home/forums/topic/59258-english-save-on-a-japanese-copy/).

## Open items

- Pizza Boy A/C: closed source, format and RTC handling unknown.
- When RetroArch's `sort_savefiles_enable` default became `true`.
