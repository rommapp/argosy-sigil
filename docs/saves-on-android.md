# When saves reach disk on Android

Research date 2026-09-28. The question is whether a save file on disk is current after the user presses HOME, and what happens if Android then kills the process.

Pinned commits:

- RetroArch `5c981d3f5c667020bd7affa3d8d534d8096757db` (RA)
- Genesis-Plus-GX `c2838c7dc4236fc2fe94e5dbd08b41486067918e` (GPGX)
- beetle-saturn-libretro `65f05fa66f83e65e33be83aa433d883b4fd9509a` (BSS)
- libretro/yabause yabasanshiro branch `09ed8e5b2e97e7a848ea2514545c34c7b809e399` (YBS)
- devmiyax/yabause `7d28bd54aa3188e90e4126ff9f7c27b945380488` (YBA)
- FCare/Kronos `d451a55253e2e75bcef704ec8ade2085d298212c` (KRO)
- picodrive `1890c2932234c9d30f4cd3851d02228baae8f09e` (PICO)
- flycast `ea087b9140ff5a3b1809e090da0f8d644ee2db95` (FLY)
- pcsx_rearmed `ff81ed17a15241d2f3730cdd7585b38e172532ca` (PCSX)
- beetle-psx-libretro `d50a9d0da3a9cea8417529cdfbb547eef8cbba15` (BPSX)
- dolphin `bb3558a70ecb767ca59c23bbe06fb6bcb78cd3bd` (DOL)
- ppsspp `9fc4eb3e4d0cc161c7a4a003b3098b0ce5387dc0` (PPS)
- duckstation `d78fd23db934cabc38d66e9a3dd8106f27a00772` (DUCK)

## RetroArch

- HOME flushes nothing. `onPause` and `onStop` only record the activity state (RA `frontend/drivers/platform_unix.c` L323-333, `input/drivers/android_input.c` L419-427). On focus loss RetroArch pauses `retro_run` and keeps the core loaded (`android_input.c` L472-478).
- A low-memory kill is SIGKILL, so nothing runs. `onDestroy` only joins the thread (`platform_unix.c` L261-280).
- The `QUITFOCUS` intent extra calls `System.exit(0)` in `onStop` without saving (`RetroActivityFuture.java` L130-135).
- SRAM reaches disk at core deinit (`retroarch.c` L8198-8205), through the `SAVE_FILES` network command (off by default), or from the autosave thread. No environment call tells a core it is paused.
- `autosave_interval` defaults to 10 seconds on Android (`config.def.h` L1314-1319). The autosave thread covers only `SAVE_RAM` and `RTC`, writes only changed buffers, keeps running while the core is paused, and truncates before writing, so a kill mid-write can leave a short file (`save.c` L93-148, L535-542).
- UNVERIFIED: the Android 12+ cached-app freezer may stop the autosave thread once the app has sat in the background.

## Cores and standalone emulators

| Emulator | Files | When they reach disk | Current after HOME? |
|---|---|---|---|
| picodrive Sega CD | `.srm` (BRAM as `SAVE_RAM`) | RetroArch autosave | After about 10 s |
| Beetle Saturn, libretro method | `.srm` | RetroArch autosave | After about 10 s |
| Beetle Saturn, mednafen method | `.bkr` | After 180 dirty frames of `retro_run` (BSS `libretro.c` L1593-1650) | Only if the game ran about 3 s after saving |
| Beetle Saturn, both methods | `.bcr` | Same 180-frame rule | Same |
| Beetle Saturn, both methods | `.smpc` | Unload only (`mednafen/ss/ss.c` L2136-2140) | No |
| genesis_plus_gx | `scd_*.brm`, cart `.brm` | Unload only (GPGX `libretro/libretro.c` L1140, L3714-3730) | No |
| Kronos | backup RAM | Unload, or the HLE BIOS flush (KRO `memory.c` L1329-1339, `bios.c` L1045) | No |
| Yaba Sanshiro libretro and standalone | `backup.bin` | Memory-mapped `MAP_SHARED`, so guest writes land in the page cache at once (YBS `memory.c` L157). The standalone app also flushes on pause (YBA `yui.cpp` L2360-2362) | Yes. UNVERIFIED on a FUSE `/sdcard` path |
| flycast libretro and standalone | VMU `.bin` | `fseek` plus `fwrite` per block with no `fflush`, so the last write sits in the stdio buffer until the next write or close (FLY `maple_devs.cpp` L700-710) | No |
| pcsx_rearmed | card 1 in libretro mode | RetroArch autosave | After about 10 s |
| pcsx_rearmed | `.mcd` file cards | Open, write, close on every write (PCSX `sio.c` L406-423) | Yes |
| Beetle PSX | `.mcr` | About 2 s of emulated time after the last write (BPSX `libretro.c` L6677-6705) | Only if the game ran on after saving |
| DuckStation | `.mcd` | 5 s of emulated time after the last write (DUCK `memory_card.cpp` L397-404). Android pause behaviour UNVERIFIED (closed source) | Only if the game ran on after saving |
| Dolphin, GCI folder | `.gci` | Host thread, 1 s after writes stop (DOL `GCMemcardDirectory.cpp` L261-295) | After 1 s |
| Dolphin, raw card | `.raw` | Host thread, every 15 s (DOL `GCMemcardRaw.cpp` L94-112) | After 15 s |
| PPSSPP | `SAVEDATA/` | Written at save time (PPS `SavedataParam.cpp` L90-95) | Yes |

## Consequences for a sync client

- HOME does not end a session. A sync client reads only after it sees the content close or the process die, and a kill leaves each file as it last reached disk.
- For unload-only files (genesis_plus_gx BRAM, Kronos, Beetle Saturn `.smpc`, flycast VMUs) the last in-game save is lost if Android kills a backgrounded RetroArch.
- Launching new content while earlier content is still loaded makes RetroArch unload the old core first, and that unload writes the old core's save files. A client that swaps a per-game volume in before launch has to be sure the previous content has closed, or the old core's unload overwrites the swapped-in volume.
