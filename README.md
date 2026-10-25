# Tenchu Z Recomp

A native Windows x64 port of **Tenchu Z (Xbox 360)** by static recompilation,
modeled on [The Darkness Recomp](https://github.com/portingpete/The-Darkness-Recomp).
The 360 executable is translated ahead of time to C++ with XenonRecomp and
linked against a native runtime: no emulation at runtime.

> **Bring your own game.** This repository contains no game code, assets, or
> binaries. Extract your own disc into `game/` (see below).

## Status

The game boots, plays its movies, music and sound, and runs the menus, the
hideout and missions with D3D11 rendering at the original 30 fps.

| Stage | State |
| --- | --- |
| Disc images (XDVDFS / XGD2) | read in place; `tools/extract_iso.py` extracts |
| XEX decode (AES + basic compression) | done: `tools/xex_image.py` |
| AOT translation | complete: 15,666 functions, 62 units |
| Kernel / XAM runtime | threads, sync, files, saves (XAM content), profile settings |
| Graphics | Xenos command processor + microcode-to-HLSL translator on D3D11 |
| Audio | XAudio2 output; XMA decoder hardware emulated with FFmpeg |
| Movies | the game's own WMV player, real time |
| Input | any controller via SDL3 (Xbox, PlayStation, Switch, 8BitDo...), keyboard and mouse (rebindable) |
| Modern features | 60 fps and higher, resolution scaling up to 4x (4K = scale 3), borderless fullscreen, HDR10, ambient occlusion, sharpening, MSAA up to 8x |

Not done yet: widescreen beyond 16:9, achievements.

## Supported game

Tenchu Z (USA, Europe) (En,Fr,De,Es,It), disc `default.xex`
SHA-256 `c727debeaad474dfcf8517202eebc388f9e51705ce3b01499486fa5ff47f9f06`.
The executable is built with XDK 2.0.4552 and imports `xboxkrnl.exe` and `xam.xex`.

## Playing on Windows

1. Download `TenchuZ-windows-x64.zip` from the [Releases](../../releases) page and unzip it
   anywhere you can write to (not Program Files).
2. Install [Python 3.11 or newer](https://www.python.org/downloads/) (tick *Add python.exe
   to PATH*) and [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/)
   with *Desktop development with C++* and *C++ Clang tools for Windows*. They are needed
   once: on its first start the game compiles itself from your disc.
3. Run `TenchuZ.exe` and pick your `Tenchu Z (USA, Europe)` disc image (`.iso`) when asked.
   Confirm the compile: a console window shows the progress (about 2 minutes on an
   8-core PC, longer with fewer cores), then the game starts. Later starts are instant.

The disc image is read in place (nothing is extracted) and remembered in `TenchuZ.ini`.
The compiled game code (`tenchu_game.dll`), saves and profile settings live in
`%APPDATA%\TenchuZRecomp`. The game looks for a newer release when it starts (one request
to GitHub; turn it off in F1 > Update): **F1 > Update** downloads it, checks its SHA-256
against GitHub's, installs it in place and restarts. Your settings, saves and the compiled
game code stay; the game code is recompiled automatically when the new release needs it.
Unzipping a newer release over the old one works too. The release contains no game code or
data; never share the `tenchu_game.dll` built from your disc.

Windows 10/11 64-bit with a Direct3D 11 graphics card. The Visual C++ runtime is included.

## Building from source

Requirements: Git, Python 3.11+ and Visual Studio 2022 (Build Tools are enough) with C++
and its Clang compiler. One command does everything: the generator (XenonRecomp, fetched
into `../_Tools` and patched), `default.xex` read from your disc and translated, FFmpeg
(prebuilt and hash-pinned, from this repository's `deps-ffmpeg` release), SDL3, Dear ImGui,
the game module and the runtime.

```bash
git clone https://github.com/8bitninja64/TenchuZ-PC.git
```

```bash
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Iso "path/to/Tenchu Z (USA, Europe) (En,Fr,De,Es,It).iso"
```

`-BuildFfmpeg` builds FFmpeg from its pinned source instead (`tools/build_xma_codec.py`,
which needs [MSYS2](https://www.msys2.org) with `pacman -S mingw-w64-x86_64-gcc make`, found
through `TENCHU_MSYS2`, `../_Tools/msys64` or `C:\msys64`).

Run `build/windows-clang/TenchuZ.exe` (pick the disc on the first start, or pass the
`.iso` or an extracted folder as the first argument). Later builds are incremental; pass
`-Configure` to reconfigure. `-SkipRecomp` builds the runtime alone (no disc needed).

```bash
powershell -ExecutionPolicy Bypass -File tools/package.ps1 -Zip
```

makes the release (`dist/TenchuZ-windows-x64.zip`, about 4 MB): the runtime, its DLLs and
the kit the first start compiles the game module with. It refuses to package anything
derived from the game. `tools/make_module.ps1 -Xex <default.xex>` builds only the module,
as a release's first start does.

The game code is a separate module, `tenchu_game.dll`, translated from your
`default.xex`; `TenchuZ.exe` contains none of it (`runtime/guest/module_abi.h`, modeled
on the NFL Blitz 2003 port).

## Playing

Settings live in `TenchuZ.ini` next to the executable, written with comments
on first run: the disc image, fullscreen, window size, vsync, `resolution_scale` (1 = 720p,
2 = 1440p, 3 = 4K), `frame_rate_limit`, mouse sensitivity and every keyboard
binding.

`frame_rate_limit` defaults to 60; 30 restores the console's pacing, and higher
values (120, 144, 240) or 0 (unlimited) go further. Game speed does not change
with the frame rate: the game measures real time. With vsync on and a refresh
rate that is a multiple of the limit (60 on a 60/120/240 Hz display), frames are
locked to the display. Missions currently reach about 75-80 fps unlimited on an
RTX 3070 laptop; menus and the hideout run far higher.

- **F1** (or **Back + Start** on a controller): settings menu, which pauses the game:
  display (frame rate limit, resolution, FPS counter, HDR), graphics (ambient occlusion,
  sharpening, anti-aliasing), audio volume, controls (controller and keyboard remapping,
  mouse), unlocking all missions in your save, cheats, and updates. A controller works it
  too: LB/RB switch sections, A selects, B goes back.
- **Cheats** (F1 > Cheats; all off at every start): infinite health, one-hit kills, invisible
  to enemies, silent ninja, infinite items, cure poison, unbreakable guard, super jump, fast
  run, low gravity, fast and long grapple, soft landing, free shopping, Grand Master rank,
  perfect stealth record; and actions: max money, unlock all skills, fill item stock, unlock
  all costumes.
- **Alt+Enter** or **F11**: toggle borderless fullscreen.
- Controller: any pad SDL3 recognizes, buttons by position as on an Xbox pad; the first
  connected pad is player 1. Layouts come from `gamecontrollerdb.txt` next to the
  executable; add your own lines to `gamecontrollerdb_user.txt`. Keyboard and mouse act as controller 1:
  WASD moves (Alt walks), click the window to capture the mouse for the camera,
  Escape releases it. Default buttons: Space = A, C = B, left mouse = X,
  right mouse = Y, Q/E = LB/RB, F/R = LT/RT, Enter or P = Start, Tab = Back,
  arrows or 1-4 = d-pad.
- Saves and profile settings are stored in `%APPDATA%\TenchuZRecomp`.

## Layout

| Path | Contents |
| --- | --- |
| `config/tenchuz.toml` | generator settings, ABI helper addresses, future mid-asm hooks |
| `config/tenchuz_switch_tables.toml` | XenonAnalyse output (regenerable) |
| `config/tenchuz_switch_tables_manual.toml` | jump tables bounded by hand |
| `runtime/guest/` | `ppc_context` template shared with the generated code |
| `runtime/kernel/` | xboxkrnl/XAM imports, memory, threads, files, live test control |
| `runtime/gpu/` | Xenos command processor, shader translator, D3D11 backend |
| `runtime/audio/` | XAudio render driver, XMA decoder contexts |
| `runtime/host/` | settings, keyboard and mouse |
| `app/` | window and entry point |
| `tools/patches/` | generator patch series (Darkness fixes + Tenchu Z additions) |
| `game/` | optional extracted disc for development (ignored by Git) |
| `tools/build.ps1`, `make_module.ps1`, `package.ps1` | one-command build, module build, release |

## License and credits

GPLv3 (see `COPYING`). The generator patches, the XEX decoder and the AOT
pipeline design come from The Darkness Recomp by portingpete (GPLv3), built on
hedge-dev's XenonRecomp (MIT); the release ships the patched generator with its
patches (`tools/patches`) and pinned upstream commit (`tools/setup_generator.py`).
The first-start module build follows the NFL Blitz 2003 port.
Audio and movie decoding use FFmpeg (LGPL 2.1+), built from pinned source with
a small opt-in patch to its XMA decoder; `tools/build_xma_codec.py` keeps the
patch, provenance and license files beside the DLLs.
