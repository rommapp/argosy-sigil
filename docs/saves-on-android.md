# When saves reach disk on Android

Whether a save file on disk is current after the user presses HOME, and
what happens if Android then kills the process. Checked against each
emulator's source (2026-09-28).

## RetroArch

- HOME flushes nothing. On focus loss RetroArch pauses `retro_run` and
  keeps the core loaded.
- A low-memory kill is SIGKILL, so nothing runs.
- The `QUITFOCUS` intent extra exits without saving.
- SRAM reaches disk at core deinit, through the `SAVE_FILES` network
  command (off by default), or from the autosave thread. No environment
  call tells a core it is paused.
- `autosave_interval` defaults to 10 seconds on Android. The autosave
  thread covers only `SAVE_RAM` and `RTC`, writes only changed buffers,
  keeps running while the core is paused, and truncates before writing,
  so a kill mid-write can leave a short file.
- Unconfirmed: the Android 12+ cached-app freezer may stop the autosave
  thread once the app has sat in the background.

## Cores and standalone emulators

| Emulator | Files | When they reach disk | Current after HOME? |
|---|---|---|---|
| picodrive Sega CD | `.srm` (BRAM as `SAVE_RAM`) | RetroArch autosave | After about 10 s |
| Beetle Saturn, libretro method | `.srm` | RetroArch autosave | After about 10 s |
| Beetle Saturn, mednafen method | `.bkr` | After 180 dirty frames of `retro_run` | Only if the game ran about 3 s after saving |
| Beetle Saturn, both methods | `.bcr` | Same 180-frame rule | Same |
| Beetle Saturn, both methods | `.smpc` | Unload only | No |
| genesis_plus_gx | `scd_*.brm`, cart `.brm` | Unload only | No |
| Kronos | backup RAM | Unload, or the HLE BIOS flush | No |
| Yaba Sanshiro libretro and standalone | `backup.bin` | Memory-mapped `MAP_SHARED`, so guest writes land in the page cache at once. The standalone app also flushes on pause | Yes; unconfirmed on a FUSE `/sdcard` path |
| flycast libretro and standalone | VMU `.bin` | `fseek` plus `fwrite` per block with no `fflush`, so the last write sits in the stdio buffer until the next write or close | No |
| pcsx_rearmed | card 1 in libretro mode | RetroArch autosave | After about 10 s |
| pcsx_rearmed | `.mcd` file cards | Open, write, close on every write | Yes |
| Beetle PSX | `.mcr` | About 2 s of emulated time after the last write | Only if the game ran on after saving |
| DuckStation | `.mcd` | 5 s of emulated time after the last write; Android pause behaviour unconfirmed (closed source) | Only if the game ran on after saving |
| Dolphin, GCI folder | `.gci` | Host thread, 1 s after writes stop | After 1 s |
| Dolphin, raw card | `.raw` | Host thread, every 15 s | After 15 s |
| PPSSPP | `SAVEDATA/` | Written at save time | Yes |

## Consequences for a sync client

- HOME does not end a session. A sync client reads only after it sees the content close or the process die, and a kill leaves each file as it last reached disk.
- For unload-only files (genesis_plus_gx BRAM, Kronos, Beetle Saturn `.smpc`, flycast VMUs) the last in-game save is lost if Android kills a backgrounded RetroArch.
- Launching new content while earlier content is still loaded makes RetroArch unload the old core first, and that unload writes the old core's save files. A client that swaps a per-game volume in before launch has to be sure the previous content has closed, or the old core's unload overwrites the swapped-in volume.
