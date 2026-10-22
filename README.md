# Tenchu Z Recomp

A native Windows x64 port of **Tenchu Z (Xbox 360)** by static recompilation,
modeled on [The Darkness Recomp](https://github.com/portingpete/The-Darkness-Recomp).
The 360 executable is translated ahead of time to C++ with XenonRecomp and
linked against a native runtime: no emulation at runtime.

> **Bring your own game.** This repository contains no game code, assets, or
> binaries. Extract your own disc into `game/` (see below).

## Status

| Stage | State |
| --- | --- |
| Disc extraction (XDVDFS / XGD2) | done: `tools/extract_iso.py` |
| XEX decode (AES + basic compression) | done: `tools/xex_image.py` |
| ABI helper discovery | done: `tools/find_abi_helpers.py` |
| Jump tables | 792 from XenonAnalyse + 1 hand-bounded inline table |
| AOT translation | **complete**: 15,666 functions, 62 units, 0 diagnostics |
| Kernel / XAM runtime | not started (Darkness covers 123 of 205 imports) |
| Graphics (XDK D3D9 + D3DX effects → D3D11/12) | not started |
| Audio (XACT banks, XMA) | not started |
| Movies (`DATA/MOVIE/*.wmv`) | not started (Media Foundation decodes WMV) |
| Input, settings, modern features | not started |

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

## Layout

| Path | Contents |
| --- | --- |
| `config/tenchuz.toml` | generator settings, ABI helper addresses, future mid-asm hooks |
| `config/tenchuz_switch_tables.toml` | XenonAnalyse output (regenerable) |
| `config/tenchuz_switch_tables_manual.toml` | jump tables bounded by hand |
| `runtime/guest/` | `ppc_context` template shared with the generated code |
| `tools/patches/` | generator patch series (Darkness fixes + Tenchu Z additions) |
| `game/` | your extracted disc (ignored by Git) |

## License and credits

GPLv3 (see `COPYING`). The generator patches, the XEX decoder and the AOT
pipeline design come from The Darkness Recomp by portingpete (GPLv3), built on
hedge-dev's XenonRecomp (GPLv3).
