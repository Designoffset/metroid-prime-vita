# Metroid Prime for PS Vita

A PS Vita port of [MetroidPrimePort](https://github.com/Odrannnn/MetroidPrimePort) (built on the PrimeDecomp decompilation, Aurora and MusyX). Rendering goes through a WebGPU-style layer (`mpvita/webgpu_vita`) implemented on [vitaGL](https://github.com/Rinnegatamante/vitaGL).

**No game data is included.** You need your own copy of Metroid Prime (GameCube, USA Rev 2) as an ISO.

## Status
Fully playable with Vita controls (twin-stick aiming, shoulder buttons for L/R). Performance is the remaining work: light rooms run 14–30 fps, heavy rooms are still CPU-bound in the GX command processor.

## What's here
| Path | Contents |
| --- | --- |
| `mpvita/webgpu_vita` | GX → vitaGL renderer: Cg shader generation, GPU vertex transforms, decoded-vertex/VBO cache, indirect texturing, GPU texture copies |
| `mpvita/vita` | Vita entry point, early boot, sampling profiler, linker script |
| `mpvita/compat` | Shims, `MP_ZONE` profiler zones |
| `patches/MetroidPrimePort.patch` | Changes to the game port and Aurora: resident geometry, compact matrix uniforms, model retention, Vita input defaults, tick cap, options-menu handheld model |
| `patches/*.patch` | Small fixes to abseil, tracy and zlib for the Vita |
| `vita/*.sh` | Toolchain, dependency and game build scripts |

## Building (Linux)
```bash
./setup.sh                # clone upstream sources at pinned commits and apply patches
cd vita && ./fetch.sh && ./build_tc.sh && ./build_deps.sh && ./build_libs.sh && ./build_game.sh
```
The output is `vita/b/game/MetroidPrime.vpk`.

## Installing
A ready-built VPK (build 49) and the data files are in [`download/`](download/) — see [download/README.md](download/README.md). To install a build you made yourself:

1. Install `MetroidPrime.vpk` with VitaShell.
2. Put your ISO in `ux0:data/MetroidPrime/rom/`.
3. Launch. Logs go to `ux0:data/MetroidPrime/error.log`.

The LiveArea images in this repo are placeholders.

## Credits
PrimeDecomp, MetroidPrimePort and Aurora contributors; Rinnegatamante (vitaGL, vitaShaRK, SceShaccCgExt); the VitaSDK team. Metroid Prime is © Nintendo; this project contains no Nintendo assets.
