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
| Game Boy, Game Boy Color | [gb](gb.md) | `gb`, `gbc` | header facts | synced |
| Game Boy Advance | [gba](gba.md) | | no | located |
| Super Nintendo | [snes](snes.md) | `snes` | header facts | located |
| NES, Famicom Disk System | [fds](fds.md) | `fds` (layout rows) | no | located |
| Nintendo DS | [nds](nds.md) | | no | located |
| Nintendo 3DS | [3ds](3ds.md) | `3ds` | yes | synced |
| Nintendo 64 | [n64](n64.md) | `n64` | yes | synced |
| Pokémon Mini | [pokemini](pokemini.md) | | no | located |
| GameCube | [gamecube](gamecube.md) | `gamecube` | yes | synced |
| Wii | [wii](wii.md) | `wii` | yes | synced |
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
| `mgba_standalone` | [gb](gb.md) | mGBA | standalone |
| `vbam_standalone` | [gb](gb.md) | VBA-M | standalone |
| `sameboy_standalone` | [gb](gb.md) | SameBoy | standalone |
| `gearboy_standalone` | [gb](gb.md) | Gearboy | standalone |
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

## Restore targets

What `restore` writes for each layout, relative to the save root: under
the default options first, then under the options that change it. Use it
to tell what a request for a layout produces before asking for it, as a
server that restores for a client without sigil does.

The last column says what restore needs to build the target from nothing:

- **yes**: restore into an empty folder gives the file the emulator reads.
- **with `profile`**: the same, once the request names the user profile
  whose account save to write.
- **send the container**: the target is a card or volume every game
  shares. Restore into an empty folder makes a new one holding only this
  game's saves, which would replace the user's. Restore into the user's
  own container instead, and only this game's saves change
  ([sync](../sync.md#whose-saves)).
- **one of the game's files**: the emulator names its files from a
  database sigil doesn't have, so restore takes the name from a file of
  the game already there.
- **located only**: collect and restore don't cover the layout yet. The
  stored file is what the emulator reads.

Option keys are the core option or setting the emulator uses; `=` gives a
value, and the default is the first listed.

| Layout | Writes by default | Options that change it | From nothing |
|---|---|---|---|
| `libretro`, and any core with no row | `{stem}.srm`; `{stem}.rtc` on a clock cart (GB, GBC, N64) | | yes |
| `vba_next`, `gpsp` | `{stem}.srm` (GBA) | | located only |
| `mgba_standalone`, `vbam_standalone`, `sameboy_standalone`, `gearboy_standalone` | `{stem}.sav`: the RAM, the clock appended on a clock cart | | yes |
| `bsnes` | `{stem}.srm`, `{stem}.rtc`, `{stem}.psr` | | located only |
| `genesis_plus_gx` | Sega CD: `scd_U.brm`, `scd_E.brm` or `scd_J.brm` by the disc's region, and `{cart_size}_cart.brm` | `genesis_plus_gx_system_bram=per game`: `{stem}.brm`; `genesis_plus_gx_cart_bram=per game`: `{stem}_{cart_size}_cart.brm`; `genesis_plus_gx_region_detect` picks the region file | send the container; yes per game |
| `mednafen_psx_hw`, `mednafen_psx` | `{stem}.srm` | `*_use_mednafen_memcard0_method=mednafen`: `{stem}.{left_index}.mcr`, and `{stem}.{right_index}.mcr` with `*_enable_memcard1=enabled`; `*_shared_memory_cards=enabled`: `mednafen_psx_libretro_shared.{index}.mcr` (shared). `*` is `beetle_psx_hw` or `beetle_psx` | yes; send the container when shared |
| `pcsx_rearmed` | slot 1 `{stem}.srm`; slot 2 `pcsx-card2.mcd` (shared) | `pcsx_rearmed_memcard1=serial`: `{pcsx_serial}_1.mcd`, `=shared`: `pcsx-card1.mcd`; `pcsx_rearmed_memcard2=serial`: `{pcsx_serial}_2.mcd` | yes for slot 1; send the container when shared |
| `vita_pops` | `PSP/SAVEDATA/{disc_id}/SCEVMC0.VMP`, `SCEVMC1.VMP` | | yes |
| `swanstation` | `{stem}.srm` | `swanstation_MemoryCards_Card1Type=PerGame`: `{title_id}_1.mcd`, `=PerGameTitle`: `{stem}_1.mcd`, `=Shared`: `duckstation_shared_card_1.mcd`; `Card2Type` the same for slot 2 | yes; send the container when shared |
| `duckstation` | `memcards/{stem}_1.mcd` (`Card1Type=PerGameTitle`) | `Card1Type=PerGame`: `memcards/{title_id}_1.mcd`, `=PerGameFileTitle`: `memcards/{stem}_1.mcd`, `=Shared`: `memcards/shared_card_1.mcd`; `Card2Type` the same for slot 2 | yes for `PerGame` and `PerGameFileTitle`. The default names the card after DuckStation's game database title, and `{stem}` comes from the request's content path: send a content path named after the card (`Final Fantasy VII.cue` for `Final Fantasy VII_1.mcd`). Send the container when shared |
| `armsx1` | `slot1.mcd`, `slot2.mcd` (shared) | | send the container |
| `pcsx2` | `Mcd001.ps2`, `Mcd002.ps2` (shared) | `pcsx2_shared_memory_cards=disabled`: `{stem}.ps2` | send the container; yes per game |
| `pcsx2_standalone` | `memcards/Mcd001.ps2`, `memcards/Mcd002.ps2` (shared, a file or folder card) | `Slot1_Filename`, `Slot2_Filename` name each slot's card | send the container |
| `mednafen_saturn` | `{stem}.srm` | `beetle_saturn_save_method=mednafen`: `{stem}.bkr`, `{stem}.bcr`, `{stem}.smpc`; `beetle_saturn_shared_int=enabled`: `mednafen_saturn_libretro_shared.bkr`; `beetle_saturn_shared_ext=enabled`: `mednafen_saturn_libretro_shared.bcr` (shared) | yes; send the container when shared |
| `kronos` | `kronos/saturn/{stem}.ram`, `kronos/saturn/{stem}-ext512K.ram` | `kronos_addon_cartridge` sizes the cart file (`-ext1M`, `-ext2M`, `-ext4M`); `kronos_use_beetle_saves=enabled`: `{stem}.bkr`, `{stem}.bcr` | yes |
| `yabause` | `{stem}.srm` | | yes |
| `yabasanshiro` | `yabasanshiro/backup.bin` (shared) | | send the container |
| `dolphin`, `dolphin_standalone` (Wii) | `User/Wii/title/{category}/{save_id}/data/` (`dolphin_standalone`: `Wii/title/...`); `{category}` is `00010000` for a disc, a WAD's own otherwise | | yes |
| `dolphin`, `dolphin_standalone` (GameCube) | the game's `.gci` files in `User/GC/{gc_region}/Card A/` (`dolphin_standalone`: `GC/{gc_region}/Card A/`) | `SlotA=1`: `User/GC/MemoryCardA.{gc_region}.raw` (shared), `MemoryCardSize` naming smaller cards (`.59.raw` to `.1019.raw`) | yes; send the container for a raw card |
| `flycast` | `vmu_save_A1.bin` and the other ports, shared, in the system folder's `dc/` | `reicast_per_content_vmus=VMU A1`: `{dc_vmu_id}.A1.bin`; `=All VMUs`: every port as `{dc_vmu_id}.{port}.bin` | send the container; yes per game |
| `flycast_standalone` | `{dc_vmu_id}_vmu_save_A1.bin`; other ports `vmu_save_{port}.bin` (shared) | `PerGameVmu=no`: `vmu_save_A1.bin` (shared) | yes for A1; send the container when shared |
| `mednafen_ngp`, `opera`, `pokemini`, `handy`, `melonds`, `fbneo`, `mame2003_plus`, `dosbox_pure`, `same_cdi`, `nestopia` | see each platform's page | | located only |
| `mupen64plus_next`, `parallel_n64` | `{stem}.srm` | | yes |
| `mupen64plus_standalone` | `*-{n64_md5_8}.eep`, `.sra`, `.fla`, `.mpk` | | one of the game's files |
| `m64plus_fz` | `GameData/*{n64_md5_lower}/SramData/*.eep`, `.sra`, `.fla`, `.mpk` | | one of the game's files |
| `project64` | `Save/*-{n64_md5_n64}/*.eep`, `.sra`, `.fla`, `*_Cont_<n>.mpk` | `Unique Game Dir=0`: `Save/{n64_header}.eep` and the rest | one of the game's files; yes with `Unique Game Dir=0` |
| `eden`, `citron`, `sudachi`, `yuzu`, `lemon` | `nand/user/save/0000000000000000/{profile}/{save_id}/`; device saves under the all-zero user | | with `profile` (a device save needs none) |
| `skyline`, `strato` | `switch/nand/user/save/0000000000000000/<fixed user>/{save_id}/`; device saves under the all-zero user | | yes |
| `ryujinx`, `kenjinx` | `bis/user/save/<index id>/0/`, and new entries in `bis/system/save/8000000000000000/0/imkvdb.arc` and `lastPublishedId` for a game never run | | send the emulator's save index and `system/Profiles.json`; restore returns them updated |
| `cemu` | `mlc01/usr/save/00050000/{save_id}/meta/`, `user/common/`, `user/{profile}/` | | with `profile` (a unit without an account save needs none) |
| `azahar`, `citra`, `lime3ds` | `sdmc/Nintendo 3DS/<32 zeros>/<32 zeros>/title/{save_id}/data/`, and the title's `extdata/00000000/{extdata_id}/` | | yes |
| `vita3k` | `ux0/user/{profile}/savedata/{save_id}/` | | with `profile` |
| `rpcs3`, `aps3e`, `armsx3` | `dev_hdd0/home/{profile}/savedata/{save_id}*/` | | with `profile` |
| `ppsspp`, `ppsspp_standalone`, `psp_console` | `PSP/SAVEDATA/{save_id}*/` | | yes |
