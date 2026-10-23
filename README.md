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
| Disc extraction (XDVDFS / XGD2) | done: `tools/extract_iso.py` |
| XEX decode (AES + basic compression) | done: `tools/xex_image.py` |
| AOT translation | complete: 15,666 functions, 62 units |
| Kernel / XAM runtime | threads, sync, files, saves (XAM content), profile settings |
| Graphics | Xenos command processor + microcode-to-HLSL translator on D3D11 |
| Audio | XAudio2 output; XMA decoder hardware emulated with FFmpeg |
| Movies | the game's own WMV player, real time |
| Input | XInput controllers, keyboard and mouse (rebindable) |
| Modern features | resolution scaling up to 4x (4K = scale 3), borderless fullscreen |

Not done yet: widescreen beyond 16:9, frame rates above 30, hardware XMA looping
(no title use seen so far), achievements.

## Supported game

Tenchu Z (USA, Europe) (En,Fr,De,Es,It), disc `default.xex`
SHA-256 `c727debeaad474dfcf8517202eebc388f9e51705ce3b01499486fa5ff47f9f06`.
The executable is built with XDK 2.0.4552 and imports `xboxkrnl.exe` and `xam.xex`.

## Setup

Requirements: Python 3.11+, Git, CMake, Visual Studio 2022 (Build Tools is
enough) with the C++ and Clang (ClangCL) components.

1. Extract the disc image into `game/`:

   ```bash
   python tools/extract_iso.py "Tenchu Z (USA, Europe) (En,Fr,De,Es,It).iso" game
   ```

2. Fetch and patch the generator into the shared `../_Tools/XenonRecomp`, then build it:

   ```bash
   python tools/setup_generator.py
   ```

   ```bash
   cmake -S ../_Tools/XenonRecomp -B ../_Tools/XenonRecomp/build -G "Visual Studio 17 2022" -A x64
   ```

   ```bash
   cmake --build ../_Tools/XenonRecomp/build --config Release --target XenonRecomp XenonAnalyse
   ```

3. Translate the executable into `build/generated/`:

   ```bash
   python tools/recompile.py
   ```

4. Build FFmpeg (XMA audio and WMV decoders). This needs MSYS2 with
   `mingw-w64-x86_64-gcc` and `make` in `../_Tools/msys64`:

   ```bash
   python tools/build_xma_codec.py
   ```

5. Build the port:

   ```bash
   cmake -S . -B build/native -G "Visual Studio 17 2022" -A x64 -T ClangCL
   ```

   ```bash
   cmake --build build/native --config Release
   ```

6. Run `build/native/Release/TenchuZ.exe` (it finds `game/` in a parent
   directory, or pass the game directory as the first argument).

## Playing

Settings live in `TenchuZ.ini` next to the executable, written with comments
on first run: fullscreen, window size, vsync, `resolution_scale` (1 = 720p,
2 = 1440p, 3 = 4K), mouse sensitivity and every keyboard binding.

- **Alt+Enter** or **F11**: toggle borderless fullscreen.
- Controller: works as on the console. Keyboard and mouse act as controller 1:
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
| `game/` | your extracted disc (ignored by Git) |

## License and credits

GPLv3 (see `COPYING`). The generator patches, the XEX decoder and the AOT
pipeline design come from The Darkness Recomp by portingpete (GPLv3), built on
hedge-dev's XenonRecomp (GPLv3).
