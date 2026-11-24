# Tenchu Z Recomp

A native Windows and Linux x64 port of **Tenchu Z (Xbox 360)** by static recompilation,
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
| Graphics | Xenos command processor + microcode-to-HLSL translator on D3D11 (Linux: DXVK-native on Vulkan, vkd3d-shader) |
| Audio | XAudio2 output (Linux: SDL3); XMA decoder hardware emulated with FFmpeg |
| Movies | the game's own WMV player, real time |
| Input | any controller via SDL3 (Xbox, PlayStation, Switch, 8BitDo...), keyboard and mouse (rebindable) |
| Modern features | 60 fps and higher, resolution scaling up to 4x (4K = scale 3), borderless fullscreen, HDR10, ambient occlusion, sharpening, MSAA up to 8x |

**Multiplayer beta testers wanted:** [v0.4.0-beta.1](https://github.com/8bitninja64/TenchuZ-PC/releases/tag/v0.4.0-beta.1)
adds experimental System Link / online play through a private VPN. Please read
the [beta setup guide](docs/multiplayer-beta.md) and
[report problems or successful tests](https://github.com/8bitninja64/TenchuZ-PC/issues/new?template=multiplayer-beta.yml).
Real internet sessions and SteamOS hardware still need verification. The stable
release remains v0.3.5; this beta is a separate, opt-in download.

Not done yet: widescreen beyond 16:9, achievements.

## Supported game

Tenchu Z (USA, Europe) (En,Fr,De,Es,It), disc `default.xex`
SHA-256 `c727debeaad474dfcf8517202eebc388f9e51705ce3b01499486fa5ff47f9f06`.
The executable is built with XDK 2.0.4552 and imports `xboxkrnl.exe` and `xam.xex`.

## Playing on Windows

1. Download `TenchuZ-windows-x64.zip` from the [Releases](../../releases) page and unzip it
   anywhere you can write to (not Program Files).
2. Open **TenchuZ.exe** and choose your `Tenchu Z (USA, Europe)` disc image (`.iso`).
3. Confirm the one-time preparation and wait for the game to start. A console window
   shows progress; preparation takes a few minutes, depending on your PC. Later starts
   use the prepared game automatically.

**No Python, Visual Studio, Windows SDK, or other developer tools need to be installed.**
The Windows release includes everything needed to prepare your disc, works offline,
and does not need administrator access or changes to PATH. `Setup.cmd` also opens the game.
Temporary compiler files are removed after successful preparation. Keep the included
`tools` folder: updates that change the game code may need it again.

The disc image is read in place (nothing is extracted) and remembered in `TenchuZ.ini`.
The compiled game code (`tenchu_game.dll`), saves and profile settings live in
`%APPDATA%\TenchuZRecomp`. The game looks for a newer release when it starts (one request
to GitHub; turn it off in F1 > Update): **F1 > Update** downloads it, checks its SHA-256
against GitHub's, installs it in place and restarts. Your settings, saves and the compiled
game code stay; the game code is recompiled automatically when the new release needs it.
Unzipping a newer release over the old one works too. The release contains no game code or
data; never share the `tenchu_game.dll` built from your disc.

Windows 10/11 64-bit with a Direct3D 11 graphics card. The Visual C++ runtime is included.

Older releases may still ask for Python and Visual Studio. Replace them with a current
release ZIP, keeping your saves. Third-party launchers may show the old setup checklist;
use the release's `TenchuZ.exe` directly. Developer prerequisites are listed below under
building from source.

If the game crashes, include the runtime log with your report: `TenchuZ.log` in
`%APPDATA%\TenchuZRecomp` (or `user` beside the executable in portable mode).
Windows launches from Explorer save this log automatically; the previous run
is retained as `TenchuZ.previous.log`. Include the mission/training step, GPU and
graphics settings. First-start compilation errors have a separate `module_build\build.log`
in the same user folder. Logs may contain local file paths; review them before sharing.

## Playing on Linux and Steam Deck

1. Download `TenchuZ-linux-x64.tar.xz` from the [Releases](../../releases) page and unpack it
   anywhere you can write to (`tar xf TenchuZ-linux-x64.tar.xz`).
2. Run `TenchuZ/TenchuZ` and pick your disc image when asked (a file chooser through
   `zenity` or `kdialog`; or pass the image's path: `./TenchuZ "path/to/Tenchu Z.iso"`).
3. On the first start a window asks to compile the game code from your disc and shows the
   progress. Everything it needs comes with the game (including a compiler, so SteamOS works
   as it is) apart from `python3`, which SteamOS and desktop distributions include.

Needs x86-64 Linux with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Mint 21, Fedora,
Arch, SteamOS) and a Vulkan 1.3 driver. On a Steam Deck in Game Mode, add `TenchuZ` as a
non-Steam game (no Proton). The compiled game code (`tenchu_game.so`), saves and profile
settings live in `~/.local/share/TenchuZRecomp` (`$XDG_DATA_HOME`). Updates work as on
Windows (F1 > Update, with the system's `curl`). HDR output is Windows-only for now.

## Portable mode

Create an empty `portable.txt` beside `TenchuZ.exe` (Linux: `TenchuZ`) before starting
the game. The compiled module, saves, profile and caches then live in a `user` folder
beside the executable. `TenchuZ.ini` stays beside the executable as usual.

To keep an existing installation's compiled module and saves, **close the game** and
run `python tools/enable_portable.py` from the installation folder (Linux: `python3`).
Do this before creating `portable.txt`. The tool copies and verifies your current
account's data, preserves the originals, and refuses to overwrite existing portable
data. `--source <folder>` can import a backup instead. In a source checkout, also pass
`--directory build/windows-clang` to select the built installation.

Start the game once before moving it: portable mode remembers the selected disc as
a path relative to the executable where possible. Keep the ISO in the installation
folder or on the same external drive, and move both together, preserving their relative
locations. A disc on another drive retains its absolute path and may need selecting again.
Saved relative disc paths are always resolved against the executable folder.

Move the entire installation, including `user` and `portable.txt`, between compatible
PCs running the same operating system. Changing PCs does not require recompilation;
an update that changes the game module still can. Removing `portable.txt` switches back
to the account's original data; it does not copy portable saves back automatically.

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

### Linux

On Ubuntu/Debian, `sudo tools/install_linux_deps.sh` installs the compilers and headers
(other distributions: clang, CMake, Ninja, Meson, glslang, bison, flex, nasm and SDL3's
X11/Wayland/audio development packages). Then

```bash
tools/build_linux.sh "path/to/Tenchu Z (USA, Europe) (En,Fr,De,Es,It).iso"
```

builds the generator (`../_Tools/XenonRecomp/build-linux`), the libraries (vkd3d-shader,
SDL3, DXVK-native and FFmpeg from pinned sources, `tools/setup_linux_deps.sh`), the game
module and the runtime into `build/linux-clang`. `TENCHU_SKIP_RECOMP=1` builds the runtime
alone. Releases are built like CI does, in an Ubuntu 22.04 container
(`tools/build_linux_release.sh`), then packaged with `tools/package_linux.sh`, which adds a
trimmed [Zig](https://ziglang.org) as the first start's compiler. `tools/make_module.sh
<default.xex>` builds only the module.

The game code is a separate module, `tenchu_game.dll`, translated from your
`default.xex`; `TenchuZ.exe` contains none of it (`runtime/guest/module_abi.h`, modeled
on the NFL Blitz 2003 port).

## Playing

To change the game's text language, open **Esc > Game > Language**, choose English,
French, German, Spanish or Italian, and restart the game. You can also set
`language = fr` under `[game]` in `TenchuZ.ini` (codes: `en`, `fr`, `de`, `es`, `it`).
English remains the default. This uses the translations on your supported multilingual
disc; the recomp settings menu remains in English. No recompilation is needed.

Settings live in `TenchuZ.ini` next to the executable, written with comments
on first run: the disc image, fullscreen, window size, vsync, `resolution_scale` (1 = 720p,
1.5 = 1080p, 2 = 1440p, 3 = 4K), `frame_rate_limit`, mouse sensitivity and every keyboard
binding.

`frame_rate_limit` defaults to 60; 30 restores the console's pacing, and higher
values (120, 144, 240) or 0 (unlimited) go further. Game speed does not change
with the frame rate: the game measures real time. In-engine cutscenes run at
their original 30 fps to keep scripted events and cameras synchronized; your
selected limit resumes afterward. Water-light animations retain their original
speed at higher gameplay frame rates. With vsync on and a refresh
rate that is a multiple of the limit (60 on a 60/120/240 Hz display), frames are
locked to the display. Missions currently reach about 75-80 fps unlimited on an
RTX 3070 laptop; menus and the hideout run far higher.

- **Esc** (or **Back + Start** on a controller): settings menu ("F1 menu"), which pauses the game and its sound:
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
  Esc releases it (and opens the menu). Default buttons: Space = A, C = B, left mouse = X,
  right mouse = Y, Q/E = LB/RB, F/R = LT/RT, Enter or P = Start, Tab = Back,
  arrows or 1-4 = d-pad.
- Saves and profile settings are stored in `%APPDATA%\TenchuZRecomp` (Linux:
  `~/.local/share/TenchuZRecomp`).

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
| `runtime/platform/` | Linux: Win32 compatibility, SDL3 window messages, D3DCompile on vkd3d-shader; portable crypto |
| `app/` | window, entry point, disc choice and first start |
| `tools/patches/` | generator patch series (Darkness fixes + Tenchu Z additions) |
| `game/` | optional extracted disc for development (ignored by Git) |
| `tools/build.ps1`, `make_module.ps1`, `package.ps1` | one-command build, module build, release |
| `tools/build_linux.sh`, `make_module.sh`, `package_linux.sh` | the same on Linux |

## License and credits

GPLv3 (see `COPYING`). The generator patches, the XEX decoder and the AOT
pipeline design come from The Darkness Recomp by portingpete (GPLv3), built on
hedge-dev's XenonRecomp (MIT); the release ships the patched generator with its
patches (`tools/patches`) and pinned upstream commit (`tools/setup_generator.py`).
The first-start module build follows the NFL Blitz 2003 port.
Audio and movie decoding use FFmpeg (LGPL 2.1+), built from pinned source with
a small opt-in patch to its XMA decoder; `tools/build_xma_codec.py` keeps the
patch, provenance and license files beside the DLLs.
On Linux, Direct3D 11 runs on Vulkan through DXVK-native (zlib), shaders compile with
vkd3d-shader (LGPL 2.1+, with a small patch in `tools/patches`), and the window, input and
sound use SDL3 (zlib). The Linux release bundles a trimmed Zig toolchain (MIT, with its
bundled licenses) for the first-start compile.
