# Atari Jaguar (+ Jaguar CD)

Status: located
sigil has no row of its own for Virtual Jaguar. Its `<stem>.srm` resolves through the default libretro row, and nothing splits the Memory Track or syncs it.

## Emulator research

Scope is game saves (battery RAM, EEPROM, flash, NVRAM, memory cards, emulated HDD content). Save states are excluded.
Sources were fetched 2026-09-26 from the default branch of each repo. Line numbers refer to that snapshot and will drift.
Items with no source are marked UNVERIFIED.

Conventions used below:

- `<savedir>` is the libretro frontend save directory. RetroArch can also insert `<core name>/` or `<content dir name>/` under it (sort_savefiles_by_* settings). That layer belongs to the frontend, not the core.
- `<base>` is the content filename without extension (for multi-disc, the `.m3u`/`.cue`/`.chd` basename that was loaded).
- `.srm` means the core exposes a buffer via `retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)` and the frontend writes it verbatim as `<savedir>/<base>.srm`. The core never sees a filename. A `.srm` is always raw bytes of the exposed buffer, with no header.

Carts use a 128-byte 93C46 EEPROM (64 x 16-bit words). Jaguar CD games save to the Memory Track cartridge (128 KiB flash), which all CD games share on hardware.

| Emulator | Files and naming | Format | Scope and options | Per-game extraction / neutral form | Source |
|---|---|---|---|---|---|
| libretro virtualjaguar (current master) | `<savedir>/<base>.srm` | Cart: 128 B (64 BE u16 words). Memory Track cart content (CRC32 0xFDF37F47): 131072 B raw `mtMem`. CD content: 128 B cart EEPROM + 128 B CD EEPROM + 131072 B Memory Track = 131328 B. The EEPROMs are a prefix, so the layout stays compatible with cart-only saves. No-content boot reports 0 | Per content (the Memory Track becomes per-disc, unlike hardware) | Cart EEPROM is per-game and raw. The Memory Track is an AT29C010 flash with a directory of per-title saves, so it is a container. Its internal layout is UNVERIFIED, so treat it as opaque. Neutral form: split the `.srm` at offsets 0/128/256 | https://github.com/libretro/virtualjaguar-libretro `libretro.c` L236-262 (layout comment and sizes), L6630-6674 (`retro_get_memory_*`). The repo is under very active development (commits 2026-09); earlier releases exposed only the 128 B cart EEPROM (UNVERIFIED which release changed it) |

### Notes for save sync

- Shared containers (one file, many games) are the geargrafx MB128, the fbneo `shared.memcard`, the mame2003-plus `MEMCARD.NNN`, MAME's `.neo` card and `nvram/neocd/saveram`, opera shared NVRAM, same_cdi shared / MAME `cdimono1` NVRAM, the xemu HDD image, and the Jaguar Memory Track on hardware. Of these, lossless per-game split/inject is well-defined for the PCE BRAM (HUBM entries), the Neo Geo cards/NeoCD (NGH-tagged directory) and the xemu HDD (FATX `UDATA\<TitleID>`). It is defined but untooled for 3DO (Opera linked-mem FS), and unknown for CD-i NVR, MB128 and Memory Track (sync those whole).
- Emulators that make a hardware-shared store per-game: every PCE core (BRAM per content), fbneo NeoCD, virtualjaguar Memory Track (inside the per-disc `.srm`), opera (default per game), and same_cdi (default per game). Importing a real-hardware dump into any of them means either duplicating the whole container per game or splitting it by the formats above.
