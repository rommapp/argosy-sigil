# Game Boy Advance

Status: located
Sigil names the files each core keeps for a cart. It reads no GBA header, and collect and restore don't cover these carts.

## Save layouts

Rows from the [layout table](../save-units.md). The default row applies to any cart core with no row of its own.

| Layout | Files (role, option) | Shared | Source |
|---|---|---|---|
| default | `{stem}.srm` primary; `{stem}.rtc` rtc | | RetroArch `save.c` |
| `vba_next`, `gpsp` | `{stem}.srm` primary | | no RTC region (`libretro.c` memory maps) |

## Emulator research

Scope: battery/flash/EEPROM saves and RTC data, not save states. Researched 2026-09-26 against the default branch of each repo (shallow clones). Line numbers refer to those heads and will drift; the function names are the stable anchor.

`{stem}` = content filename without extension. "RA" = RetroArch frontend. RetroArch's own behaviour is in [RetroArch frontend behaviour](gb.md#retroarch-frontend-behaviour-applies-to-every-libretro-core-that-exposes-memory).

### Emulators

| Platform | Emulator | Files and naming | Format | Scope | Options / versions that change files | Lossless conversion | Source |
|---|---|---|---|---|---|---|---|
| GBA | mgba (libretro) | `{stem}.srm` | Raw, exact save-type size (512 / 8192 EEPROM, 32 KiB SRAM, 64 / 128 KiB flash). While the type is still autodetect, it exposes **128 KiB** (0xFF filled). **The RTC footer is not exported** (no `RETRO_MEMORY_RTC` for GBA). | per-game | `mgba_force_gbp`/save-type overrides change the size (UNVERIFIED key name) | Bytes are identical to standalone mGBA/VBA-M with the footer removed. To convert, trim to the save-type size and drop trailing 0xFF padding. | [libretro.c L1154-1166, L924-925](https://github.com/mgba-emu/mgba/blob/master/src/platform/libretro/libretro.c#L1154-L1166) |
| GBA | mGBA (standalone) | `{stem}.sav` | Raw plus, for RTC carts, a **16-byte footer** (7 BCD time bytes, 1 control byte, u64 LE unix latch time). This is the FlashGBX/GBxCart layout. It is placed at `size & ~0xFF`. | per-game | Added in 0.10.0 (2022-10-11, "Store RTC data in savegames"). The Qt Save Converter can export `.sav` without RTC (0.10.3). | Strip the last 16 B when `filesize & 0xFF == 16` to get raw. Other emulators ignore the RTC, so the RTC state is lost there. | [savedata.c `GBASavedataRTCWrite` L606-638](https://github.com/mgba-emu/mgba/blob/master/src/gba/savedata.c#L606-L638), [savedata.h L90-94](https://github.com/mgba-emu/mgba/blob/master/include/mgba/internal/gba/savedata.h#L90-L94), [CHANGES](https://github.com/mgba-emu/mgba/blob/master/CHANGES), [issue #2431](https://github.com/mgba-emu/mgba/issues/2431) |
| GBA | vba-m (libretro) | `{stem}.srm` | Raw: EEPROM `eepromSize` (512/8192), flash `g_flashSize`, SRAM 32 KiB. **No GBA RTC persistence.** | per-game | none | Byte-identical to VBA-M standalone and mGBA (raw part) | [libretro.cpp L332-384](https://github.com/visualboyadvance-m/visualboyadvance-m/blob/master/src/libretro/libretro.cpp#L332-L384) |
| GBA | VBA-M (standalone) | `{stem}.sav` | Raw, exact size. No RTC. | per-game | none | Lossless with mGBA raw | [gba.cpp `CPUWriteBatteryFile` L1786-1819](https://github.com/visualboyadvance-m/visualboyadvance-m/blob/master/src/core/gba/gba.cpp#L1786-L1819) |
| GBA | vba_next (libretro) | `{stem}.srm` | Buffer of 0x22000; the size is guessed by scanning for non-0xFF: 512, 0x2000, 0x10000, 0x20000. SRAM (32 KiB) games come out as 64 KiB. Before the first save, 0x22000 (139264 B). No RTC. | per-game | none | Truncate or pad with 0xFF to the real size. The content is the same bytes. | [libretro.c L33-108](https://github.com/libretro/vba-next/blob/master/libretro/libretro.c#L33-L108) |
| GBA | gpsp (libretro) | `{stem}.srm` | **Always 128 KiB** (`gamepak_backup`), with the real data at offset 0. The EEPROM bit order matches VBA/mGBA (MSB first, 8 bytes per block). RTC is not saved; `gpsp_rtc` / `gpsp_rtc_time_source` only choose the clock source. | per-game | none | Truncate to the real size. Pad with 0xFF when importing. | [libretro.c L1350-1361](https://github.com/libretro/gpsp/blob/master/libretro/libretro.c#L1350-L1361), [gba_memory.c L360, L560-595](https://github.com/libretro/gpsp/blob/master/gba_memory.c#L560-L595) |
| GBA | Mesen2 (standalone) | `Saves/{romname}.sav`, `Saves/{romname}.rtc` | `.sav` raw. `.rtc` = 10 state bytes (Y/M/D/DoW/H/M/S/status/intH/intM) + 1 pad + u64 **BE** seconds = 19 B. UNVERIFIED whether the values are BCD. | per-game | none | `.sav` lossless. `.rtc` converts to the mGBA 16 B footer by field mapping. | [GbaCart.cpp L164-176](https://github.com/SourMesen/Mesen2/blob/master/Core/GBA/Cart/GbaCart.cpp#L164-L176), [GbaRtc.cpp L238-289](https://github.com/SourMesen/Mesen2/blob/master/Core/GBA/Cart/GbaRtc.cpp#L238-L289) |
| GBA | Pizza Boy A (Android) | UNVERIFIED | UNVERIFIED (closed source). Whether it reads the mGBA/FlashGBX 16 B RTC footer is unknown. | per-game | - | UNVERIFIED | none found |

### RTC summary

| Emulator | GBA RTC |
|---|---|
| RetroArch generic | same |
| mgba (lr) | **not saved** |
| vba-m (lr) | **not saved** |
| gpsp / vba_next (lr) | **not saved** |
| SameBoy / mGBA / VBA-M / Gearboy (standalone) | mGBA: appended 16 B footer (0.10.0+); VBA-M: not saved |
| Mesen2 | separate `.rtc` 19 B (BE s) |

In the RetroArch generic row, "same" means `.rtc` = whatever `RETRO_MEMORY_RTC` the core exposes.

### What the stored clock means

Researched 2026-09-28. Pinned commits: gambatte-libretro `d9d6cd06`, SameBoy `213a12ce`, mgba `83846bb3`, visualboyadvance-m `4466886c`, vba-next `788192f2`, Mesen2 `b9fa69dd`, Gearboy `79fb5031`, tgbdual-libretro `0392c9c4`, bsnes `0c2fa0db`, gpsp `5819380c`.

- mGBA GBA: the 16-byte footer holds 7 BCD bytes (Y, M, D, DoW, H, M, S), a control byte and u64 LE `lastLatch`, the host time when the game last read the clock. On load it computes `lastLatch - mktime(time)` in local time and shows `now` minus that offset. A timezone change between save and load shifts the clock (inferred from `mktime`, UNVERIFIED). In 12-hour mode it stores `hour % 12` with no PM flag (`savedata.c` L644-712, `cart/gpio.c` L300-328).
- VBA-M, vba_next and gpSP GBA: none persist the GBA RTC. They read host time, or host time at boot plus emulated frames (`gbaRtc.cpp` L110-129, vba-next `src/memory.c` L788-797, gpsp `gba_memory.c` L1499-1528).

### Conversion notes

- **GBA** is raw everywhere. Canonical form = `(raw[save_type_size], rtc16 | null)`. Normalize the containers:
   - gpsp: 128 KiB.
   - vba_next: 64 KiB for SRAM, 0x22000 when never saved.
   - mgba-lr in autodetect: 128 KiB.
   - mGBA standalone: 16 B footer.

   The EEPROM bit layout is the same across mGBA, VBA-M and gpSP. None of the libretro GBA cores keep the RTC.

### Save compatibility across releases

- Mother 3: Japanese saves work with the translation only after the project's converter runs, because the new font layout garbles stored names. Saves from translation 1.0 to 1.2 carry over to 1.3 (https://mother3.fobby.net/blog/faqs/).

## Open items

- Pizza Boy A/C: closed source, format and RTC handling unknown.
- Whether the Mesen2 GBA `.rtc` values are BCD.
