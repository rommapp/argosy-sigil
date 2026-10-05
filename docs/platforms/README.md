# Platforms

One page per system. Each says how sigil identifies a game, which emulator
layouts it knows and how each was verified, what sync does there, and
what isn't covered yet.

Status:

In each status the client says which emulator runs the game and passes its
save folder; sigil never searches the drive.

- **synced**: collect builds the upload and restore writes it back, taking one game's saves out of a shared memory card or backup RAM file where the emulator keeps one ([sync.md](../sync.md)).
- **located**: `sigil_save_resolve` names which files in the save folder are the game's ([save-units.md](../save-units.md)); the client uploads them as they are, and collect and restore don't cover the system yet.
- **in development**: sigil may read the game's id, but no layout names its save files yet.

## Systems

| System | Page | Slug | Reads the id | Status |
|---|---|---|---|---|
| PlayStation | [psx](psx.md) | `psx` | yes | synced |
| PlayStation 2 | [ps2](ps2.md) | `ps2` | yes | synced |
| PSP | [psp](psp.md) | `psp` | yes | synced |
| PS Vita | [psvita](psvita.md) | `psvita` | yes | synced |
| PlayStation 3 | [ps3](ps3.md) | `ps3` | yes | synced |
| Game Boy, Game Boy Color | [gb](gb.md) | `gb`, `gbc` | header facts | located |
| Game Boy Advance | [gba](gba.md) | | no | located |
| Super Nintendo | [snes](snes.md) | `snes` | header facts | located |
| NES, Famicom Disk System | [fds](fds.md) | `fds` (layout rows) | no | located |
| Nintendo DS | [nds](nds.md) | | no | located |
| Nintendo 3DS | [3ds](3ds.md) | `3ds` | yes | synced |
| Nintendo 64 | [n64](n64.md) | `n64` | yes | synced |
| Pokémon Mini | [pokemini](pokemini.md) | | no | located |
| GameCube | [gamecube](gamecube.md) | `gamecube` | yes | synced |
| Wii | [wii](wii.md) | `wii` | yes | in development |
| Wii U | [wiiu](wiiu.md) | `wiiu` | yes | synced |
| Switch | [switch](switch.md) | `switch` | yes | synced |
| Mega Drive, 32X, Master System, Game Gear | [genesis](genesis.md) | | no | located |
| Sega CD | [segacd](segacd.md) | `segacd` (layout rows) | no | synced |
| Saturn | [saturn](saturn.md) | `saturn` (layout rows) | no | synced |
| Dreamcast | [dreamcast](dreamcast.md) | `dreamcast` | yes | synced |
| Xbox | [xbox](xbox.md) | `xbox` | yes | in development |
| Xbox 360 | [xbox360](xbox360.md) | `xbox360` | yes | in development |
| Arcade (FBNeo, MAME 2003-Plus) | [arcade](arcade.md) | | no | located |
| Neo Geo | [neogeo](neogeo.md) | | no | located |
| Neo Geo Pocket | [ngp](ngp.md) | | no | located |
| Atari Lynx | [lynx](lynx.md) | | no | located |
| Atari Jaguar | [jaguar](jaguar.md) | | no | located |
| PC Engine, TurboGrafx-16 | [pce](pce.md) | | no | located |
| WonderSwan | [wonderswan](wonderswan.md) | | no | located |
| 3DO | [3do](3do.md) | | no | located |
| DOS | [dos](dos.md) | | no | located |
| CD-i | [cdi](cdi.md) | | no | located |

## Layouts

Every call that finds or moves saves takes a layout id: the `core`
argument in the bindings, `layout` in C. It names the emulator running
the game, because each emulator keeps its saves differently.

- For a libretro core, pass the core's library name without `_libretro`:
  `genesis_plus_gx` for `genesis_plus_gx_libretro.so`.
- For a standalone emulator, pass its id from the table, such as
  `dolphin_standalone`, `pcsx2_standalone` or `eden`.

An id with no row here gets the libretro default row (`{stem}.srm`, plus
`{stem}.rtc` when the cart has a clock), documented on
[gb](gb.md#save-layouts). That fits a libretro core with no row of its
own. It does not fit a standalone emulator missing from the table: the
default row names a `.srm` the emulator never writes, so locate finds
nothing and restore writes a file the emulator doesn't read.

`libretro` names the default row on purpose, for every call: a client
can keep a core on it after the core gets a row of its own, until the
client is ready for the new row's units. No row ever takes that id.

Every layout id `src/save_layout.c` defines, and the page that documents
it. A layout id without a row here, or missing from its page's Save layouts
section, fails `bindings/python/test_docs.py`.

| Layout | Page | Emulator | Kind |
|---|---|---|---|
| `vba_next` | [gba](gba.md) | VBA Next | libretro core |
| `gpsp` | [gba](gba.md) | gpSP | libretro core |
| `bsnes` | [snes](snes.md) | bsnes | libretro core |
| `genesis_plus_gx` | [segacd](segacd.md) | Genesis Plus GX, Sega CD games | libretro core |
| `mednafen_psx_hw` | [psx](psx.md) | Beetle PSX HW | libretro core |
| `mednafen_psx` | [psx](psx.md) | Beetle PSX | libretro core |
| `pcsx_rearmed` | [psx](psx.md) | PCSX ReARMed | libretro core |
| `vita_pops` | [psx](psx.md) | PS1 Classics on a PSP or PS Vita (POPS, Adrenaline) | console |
| `swanstation` | [psx](psx.md) | SwanStation | libretro core |
| `duckstation` | [psx](psx.md) | DuckStation | standalone |
| `armsx1` | [psx](psx.md) | ARMSX1 | standalone |
| `pcsx2` | [ps2](ps2.md) | LRPS2 | libretro core |
| `pcsx2_standalone` | [ps2](ps2.md) | PCSX2, AetherSX2, NetherSX2, ARMSX2 | standalone |
| `mednafen_saturn` | [saturn](saturn.md) | Beetle Saturn | libretro core |
| `kronos` | [saturn](saturn.md) | Kronos | libretro core |
| `dolphin` | [gamecube](gamecube.md) | Dolphin | libretro core |
| `dolphin_standalone` | [gamecube](gamecube.md) | Dolphin | standalone |
| `flycast` | [dreamcast](dreamcast.md) | Flycast | libretro core |
| `flycast_standalone` | [dreamcast](dreamcast.md) | Flycast | standalone |
| `yabause` | [saturn](saturn.md) | Yabause | libretro core |
| `yabasanshiro` | [saturn](saturn.md) | Yaba Sanshiro | libretro core |
| `mednafen_ngp` | [ngp](ngp.md) | Beetle NeoPop | libretro core |
| `opera` | [3do](3do.md) | Opera | libretro core |
| `pokemini` | [pokemini](pokemini.md) | PokeMini | libretro core |
| `handy` | [lynx](lynx.md) | Handy | libretro core |
| `melonds` | [nds](nds.md) | melonDS (the legacy core) | libretro core |
| `fbneo` | [arcade](arcade.md) | FBNeo | libretro core |
| `mame2003_plus` | [arcade](arcade.md) | MAME 2003-Plus | libretro core |
| `dosbox_pure` | [dos](dos.md) | DOSBox Pure | libretro core |
| `same_cdi` | [cdi](cdi.md) | SAME CDi | libretro core |
| `nestopia` | [fds](fds.md) | Nestopia, FDS disks | libretro core |
| `mupen64plus_next` | [n64](n64.md) | Mupen64Plus-Next | libretro core |
| `parallel_n64` | [n64](n64.md) | ParaLLEl N64 | libretro core |
| `mupen64plus_standalone` | [n64](n64.md) | mupen64plus, RMG, simple64 | standalone |
| `m64plus_fz` | [n64](n64.md) | M64Plus FZ | standalone |
| `project64` | [n64](n64.md) | Project64 | standalone |
| `eden` | [switch](switch.md) | Eden | standalone |
| `citron` | [switch](switch.md) | Citron | standalone |
| `sudachi` | [switch](switch.md) | Sudachi | standalone |
| `yuzu` | [switch](switch.md) | yuzu | standalone |
| `lemon` | [switch](switch.md) | Lemon | standalone |
| `skyline` | [switch](switch.md) | Skyline | standalone |
| `strato` | [switch](switch.md) | Strato | standalone |
| `ryujinx` | [switch](switch.md) | Ryujinx, Ryubing | standalone |
| `kenjinx` | [switch](switch.md) | Kenji-NX | standalone |
| `cemu` | [wiiu](wiiu.md) | Cemu | standalone |
| `azahar` | [3ds](3ds.md) | Azahar | standalone |
| `citra` | [3ds](3ds.md) | Citra, and its libretro core | standalone |
| `lime3ds` | [3ds](3ds.md) | Lime3DS | standalone |
| `vita3k` | [psvita](psvita.md) | Vita3K | standalone |
| `rpcs3` | [ps3](ps3.md) | RPCS3 | standalone |
| `aps3e` | [ps3](ps3.md) | aPS3e | standalone |
| `armsx3` | [ps3](ps3.md) | ARMSX3 | standalone |
| `ppsspp` | [psp](psp.md) | PPSSPP | libretro core |
| `ppsspp_standalone` | [psp](psp.md) | PPSSPP | standalone |
| `psp_console` | [psp](psp.md) | PSP games on a PSP, or on a PS Vita (Adrenaline, the PSP emulator) | console |
