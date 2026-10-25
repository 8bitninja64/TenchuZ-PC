# Tenchu Z Recomp: working notes

Xbox 360 static recomp (XenonRecomp) to a native Windows port, following The
Darkness Recomp (reference clone: `../_Tools/DarknessRecomp_ref`, GPLv3, so this
project is GPLv3 too and may reuse its code with credit).

## Rules
- Never commit game data or generated code (`game/`, `build/`).
- Shared tools live in `../_Tools` (XenonRecomp checkout, Ghidra), not in this repo.
- Generator changes go in a new patch appended to `PATCHES` in
  `tools/setup_generator.py`; keep `../_Tools/XenonRecomp` reproducible from it.
  Its local commit `c5f3f16` holds the Darkness patches; the Tenchu patch is the
  working-tree diff on top (`git diff > tools/patches/xenonrecomp-tenchuz.patch`).
- Files written from Python on Windows: use `newline="\n"` or `write_bytes`;
  CRLF breaks the patches.

## Executable facts (disc default.xex)
- XDK 2.0.4552, encrypted + basic compression, load 0x82000000, entry 0x82410E68.
- `.text` 0x82270000-0x82809DDC, seven `.embsec_` code sections 0x82809E00-0x82829DB8,
  `.data` 0x82830000 (12 MB), `.pdata` 0x8224DE00.
- Stock XDK D3D9 static library and D3DX9 effects (`DATA/*.FXO`; a D3DX effect
  compiler is linked in). Engine prefix "K2X". XACT banks `DATA/*.xsb`, `xten.xgs`.
  Movies are `.wmv`. XNet/sockets imports exist for online play.
- Inline switch 0x825EFD30 (r19, 21 cases) is bounded by hand.

- setjmp 0x82657B00 / longjmp 0x826578C0 (libjpeg/libpng error handling) are
  translated to host setjmp/longjmp by the generator.

## Runtime layout (runtime/)
Original code following Darkness's architecture (not copied files).
- `kernel/memory.*`: 4 GiB placeholder reservation; 0xA0/0xC0/0xE0000000 are views
  of one 512 MiB backing (E biased by 4 KiB). Virtual 0x40-0x70M, stacks
  0x70-0x7F0M, PCR pages 0x7FF00000+, kernel bump heap 0x80000000-0x81000000.
- `kernel/kernel.cpp`: handles, dispatcher (all waitable state in guest dispatcher
  headers; one global mutex + condvar, 20 ms recheck slices), guest threads (PCR +0
  TLS ptr, +0x100 KTHREAD, +0x10C cpu; KTHREAD +0x5C/+0x60 stack, +0x68 TLS,
  +0x14C id), APCs on alertable waits, `callGuest`, bring-up.
- `kernel/imports_*.cpp`: Ke/Ex/Mm, Rtl + printf, Nt file I/O (device mounts in
  imports_io.cpp), XAM (profile settings persist under %APPDATA%\TenchuZRecomp),
  offline NetDll.
- `gpu/null_gpu.cpp`: Xenos register file behind PPCMmioRead32/Write32 (template
  hook), command processor thread for ring + indirect buffers, vblank (60 Hz in 30 fps
  mode, 1 kHz otherwise).
- `audio/audio_driver.cpp`: XAudio render client -> XAudio2 (6 planar BE float channels, 256
  samples, 48 kHz). `audio/xma.cpp`: XMA decoder contexts (APU registers 0x7FEA0000).
- `kernel/control.cpp`: TENCHU_CONTROL live command file for automated testing.
- `host/gamepad.cpp`: SDL3 gamepads (tools/fetch_sdl.py, assets/gamecontrollerdb.txt). With
  TENCHU_CONTROL active, controller 1 comes from the control file and SDL is not polled.
- `host/overlay.cpp`: F1 menu (Dear ImGui 1.91.9b, tools/fetch_imgui.py; Win32 + DX11
  backends), modeled on Blitz2003's overlay.cpp. Drawn by the renderer before Present; the
  game pauses in VdSwap while open and the CP re-presents the last frame (presentAgain).
  Back+Start is filtered in XamInputGetState (filterGamePad) so the game never sees it.
  Settings it changes are written back into TenchuZ.ini in place (saveSettings).
  host::g_mutex is recursive: handleMessage re-enters through ReleaseCapture (combo clicks
  crashed with "resource deadlock would occur"). Panes are NavFlattened (the pad reaches
  Resume/Quit); focusFirstOption() goes right before a page's first focusable widget.
  Controller remapping ([controller] in TenchuZ.ini, gamepad.cpp remap()): the menu reads
  the physical buttons (gamepadRawState), the game the mapped ones.
- `host/update_check.cpp`: F1 > Update, ported from Blitz2003's updater (GitHub latest
  release, SHA-256 digest check, tar.exe unpack, *.old-update backups removed at the next
  start, restart). Version = TENCHU_VERSION (CMake cache, from the env; the release
  workflow sets the tag). Test against a fake API: TENCHU_UPDATE_API=http://127.0.0.1:PORT
  plus TENCHU_UPDATE_AUTOINSTALL=1 (installs and restarts without clicks). Runs with
  TENCHU_CONTROL skip the startup check unless TENCHU_UPDATE_API is set.
- Game module boundary (runtime/guest/module_abi.h): the translated code is tenchu_game.dll,
  TenchuZ.exe has none. Runtime functions the module calls register by name: TZ_IMPORT(Name)
  for imports, TZ_OVERRIDE(addr) for replaced game functions (also list the address in
  config/overrides.txt; __imp__sub_X calls the original), TZ_MIDASM_HOOK1/2 after mid-asm
  hooks. recompile.py emits build/generated/ppc_module.cpp (forwarders, originals, data
  imports, descriptor); module_loader.cpp binds it and rejects ABI/layout/name mismatches.
  Bump kModuleAbiVersion when PPCContext, ppc_config.h or the ABI structs change;
  runtime/guest/ppc_config.h must equal the generator's copy (recompile.py checks).
- app/boot.cpp: disc choice (argv[1] file or folder, TENCHU_DISC, [game] disc, a game/
  folder up the tree, file picker), default.xex SHA-256, module search (TENCHU_MODULE,
  exe dir, %APPDATA% built + kit stamp) and the first-start build (make_module.ps1).
  Disc images are mounted read-only in place (kernel/xdvdfs.cpp, imports_io.cpp).

## Running
One-command build: `powershell -ExecutionPolicy Bypass -File tools/build.ps1 [-Iso <iso>]`
(generator in ../_Tools/XenonRecomp/build-tenchu with MSVC + Ninja, recompile, deps,
Ninja + clang-cl into build/windows-clang). The older VS build in build/native also works
(`cmake -S . -B build/native -G "Visual Studio 17 2022" -A x64 -T ClangCL`; tenchu_game
builds when build/generated has sources). Run `<build>/TenchuZ.exe game` (folder) or
`TenchuZ.exe <iso>`. Env: `TENCHU_TRACE=thread,file,io,xam,mem,mmio,dbg,swap` (or `all`),
`TENCHU_EXIT_AFTER_FRAMES=N`, `TENCHU_MODULE=<dll>`, `TENCHU_DISC=<iso>`, `TENCHU_AUTO_BUILD=1`
(skip the first-start prompt). Release: tools/package.ps1 -Zip; the first start of a release
runs tools/make_module.ps1 (recompile.py --strict + compile_module.py, about 110 s here).
Patches are byte-exact (.gitattributes): Git for Windows' autocrlf broke them in clones.
GitHub: github.com/8bitninja64/TenchuZ-PC (public). The build workflow builds and packages
the runtime on every push; a `v*` tag publishes TenchuZ-windows-x64.zip as a release. FFmpeg
for builds is the `deps-ffmpeg-n8.1.2-1` pre-release asset (ffmpeg workflow, run by hand),
pinned by SHA-256 in tools/fetch_ffmpeg.py: when build_xma_codec.py changes, run the
workflow with a new tag and update URL and SHA256 there.

Driving the game (preferred over timed scripts): `TENCHU_CONTROL=<file>` executes lines
appended at runtime (press/hold/release/stick/wait/shot/traceframe/dumpframe/quit, see
runtime/kernel/control.h). `python tools/drive.py <file> "press a" "wait 2000" --shot out`
appends commands and waits for the PNG. Path to the hideout from boot (with
`TENCHU_SKIP_MOVIES=1`, otherwise the logo and opening movies play first):
wait 14000, start, wait 3000, a (load save), wait 3000, down, a (Continue), wait 4000,
a (Solo Mission), ~16 s load. Forward-slash paths for `shot`.
Debug aids: `TENCHU_PS_OVERRIDE=<hash>=<hlsl>` replaces a pixel shader's color (e.g. r[6],
input.t[5]); `TENCHU_TRACE_FRAME`/traceframe logs every draw; dumpframe writes resolved
textures to build/resolved; `TENCHU_PROF=start,seconds` with trace `prof`;
`TENCHU_AUDIO_DUMP=x.wav` + tools/wav_levels.py.

## Status (2026-10-23)
- Title, menus, save/load, hideout and Mission 1 render on D3D11 at 30 fps; movies play
  through the title's own WMV player in real time; music and sound effects play.
- Fixed along the way (see git log): XAPI heap needs MEM_LARGE_PAGES 64 KiB commit
  rounding; eieio stores are ordinary memory unless in MMIO ranges; scratch
  registers are 0x1DC/0x1DD; INTERRUPT dispatches per CPU in its mask with the PCR
  processor number set; predicated type-3 packets (header bit 0) depend on bin
  mask/select; EVENT_WRITE_EXT (0x5A) must report screen extents or predicated tiling
  drops draws; rectangle lists do not always start at the right-angle corner;
  scalar ops with write mask 0 still update ps; threads keep their ExCreateThread
  affinity (flags bits 24-29) as PCR processor number (audio workers barrier on it).
- Audio: XAudio2 render driver works; 99% of waves are XMA (runtime/audio/xma.cpp
  emulates the decoder contexts with FFmpeg's xma2 in raw-frame mode, loops at packet
  granularity).
- Modern features: TenchuZ.ini (runtime/host/settings.cpp), borderless fullscreen
  (Alt+Enter/F11), keyboard+mouse merged into controller 1, resolution_scale 1-4,
  anisotropic filtering, host-generated mips, discrete GPU preferred.
- Frame rate: the title presents with interval 2 (device+13248) and times its logic with
  QueryPerformanceCounter (sub_824104E8), so unlocking is safe. frame_rate_limit != 30
  forces interval 1, runs the emulated vblank at 1 kHz, and paces with Present's sync
  interval (when refresh % limit == 0) or a timer in the CP (limitFrameRate).
- Performance: two threads bound the frame rate, the command processor and the title's
  frame thread (the one calling VdSwap). TENCHU_TRACE=hostprof + the control file's
  `profile` command samples both (the runtime has /Z7 symbols); gputime's "cp idle" and
  "mem waits" show which one waits on the other. Sweep scripts: build/live/sweep.sh
  (TENCHU_FRAME_RATE=240 measures headroom).
- Guest memory mirror: physical memory has a 512 MB GPU copy (xenos_backend mirrorRange),
  uploaded per 4 KiB page. Memory::watchPages write-protects uploaded pages in all three
  views; the first guest write faults, a vectored handler unprotects the page and clears
  it for every watch client (mirror, textures). OS writes into guest memory do not fault:
  call Memory::prepareHostWrite first (NtReadFile does). Vertex streams and list-primitive
  indices are read from the mirror in place (direct draws); pages rewritten every frame
  turn hot and use the per-draw ring copy. Textures are revalidated only after a write.
- Vertex data must never be reused from an earlier copy without that check: the title
  rewrites dynamic buffers (same base/size) between draws (one-frame black tiles and
  stretched polygons). Ring copies take only the draw's index range (vertexStreams).
- Generator: CR/CTR/XER/reservation are function locals (config/tenchuz.toml). Register
  and non-volatile locals stay off: ~90 analyzer-split functions read r11/r12 (and
  non-volatiles) live from the code that jumps into them, and the game hangs on save
  load with them on. /fp:precise measured no gain over /fp:strict: keep strict.
- Guest loads/stores (runtime/guest/ppc_context.template.h) are plain host accesses with
  no checks. MMIO (0x7FC00000-0x7FCFFFFF, 0x7FE00000-0x7FEFFFFF) stays unmapped: plain
  accesses fault into kernel/mmio_fault.cpp, which decodes the x86 mov/movzx and calls
  PPCMmioRead32/Write32 (~5 per frame). Never commit memory in those ranges. Null
  (first 64 KiB) is committed zero memory. eieio-marked PPC_MM_* keep explicit routing.
- Emulated vblank is 1 kHz for unlocked rates (60 Hz in 30 fps mode): Present parks the
  GPU until the next vblank, so a coarse tick quantizes frame times.
- Command processor hot paths: type-0 register runs without side-effect registers are
  bulk-copied (writeRegisterRun); shader constants upload straight from the register
  file when constantsWritten is set; the backend's BoundState skips unchanged D3D calls
  (anything else that binds state must call forgetBoundState, e.g. resolve, swap, the
  texture cache's mip generation via pipelineChanges). TENCHU_TRACE=cpstats counts packets.
- Native replacements of hot guest functions live in runtime/kernel/native_overrides.cpp
  (TENCHU_TRACE=verifynative runs the original too and logs differences).
- Never block the title's threads on host devices: gamepad.cpp publishes snapshots; SDL
  calls (enumeration, rumble) ran under the title's lock and caused 30-250 ms hitches.
- The upload ring must not wrap inside a draw: wrapping maps with DISCARD (renames the
  buffer), so streams copied before the wrap are gone for that draw. Draws reserve their
  total upload first (reserveUploads). This was the remaining one-tile glitch source.
- Shaders compile on worker threads (draws skip until ready) and are cached on disk in
  %APPDATA%\TenchuZRecomp\shadercache keyed by translated HLSL + flags; skinned vertex
  shaders take ~5 s each in FXC, which used to freeze scene loads for up to a minute.
  TENCHU_SYNC_SHADERS=1 compiles inline (deterministic captures).
- Only player 1 is signed in: all controllers merge into user 0 (a pad seen as player 2
  made the game refuse to load/save). Test the real input path with build/live/keys.py
  (posted keys, no control file); TENCHU_CONTROL_USER picks the control file's player.
- Jump tables: tools/audit_switch_tables.py checks every generated switch register against
  the disassembly; run it after regenerating with new switch tables.
- Flicker hunting: control-file `burst <n> <dir>` saves consecutive frames;
  `python tools/flicker.py <dir>` reports one-frame glitches (region ratio > 1.5 is real;
  the per-block "spike" list also catches ordinary motion). Navigation by timing alone is
  fragile: keep waits generous (build/live/nav_hideout.txt uses 17/6/6/6/18 s).
- Hybrid laptop (Intel iGPU + RTX 3070, 240 Hz): verify rendering on NVIDIA. GPU
  differences found so far: interpolated alpha just below 1.0 (alpha test now
  quantized), 0*inf in normalizations (XMul helpers).
- Save (content/00000001/TENCHU_Z/tenchu.bin, 5216 bytes, no checksum): chapters of mission
  records at +0x500, per-difficulty status bits 0|1 = cleared; tools/save_unlock.py marks
  missions cleared so Mission Select offers all 50. Unset XAM title settings must read as
  source 1 (default), or the game copies zeros (dead buttons, volume 0).
- XMA (runtime/audio/xma.cpp): streams start at the title's input read offset (not packet
  0), a failed packet moves on (retrying it reset the position: endless failures, lag, a
  freeze through the context mutex), packet header byte 3 skips other streams' packets
  (Xenia's rule), passes are bounded and silent loops end. FFmpeg's log is quiet unless
  TENCHU_TRACE=ffmpeg. The rustling bushes in Mission 1 exercise all of this.
- Frame time: 0x82B57824 is milliseconds per frame (sub_822929D0). Effect code (water
  ripples, type 85) converts it to 30 Hz ticks with int(ms * 0.03 + 0.5), 0 above ~60 fps,
  so effect lives never counted down. midasm_hook entries in config/tenchuz.toml replace
  those values with carried whole ticks/ms (native_overrides.cpp). New frame-rate bugs:
  look for fctiwz of values derived from 0x82B57824. Debug aids: TENCHU_SKIP_VS,
  TENCHU_FIND_WRITER=<vs hash> / TENCHU_PROBE_RANGE=<pages> (guest writers of memory),
  TENCHU_PROF_LINES=<function> (hot source lines; the generated code has /Z7 symbols), control
  commands findf (search guest memory for floats) and watchw (hardware write watchpoint;
  its fn is the last function entered, often a save helper: read the backtrace/lr).
- Frame delta: sub_8228E5C8 stores the QPC delta in seconds at 0x82B577BC (read through
  sub_8228E5B8 by everything that moves). Measured on the title's thread it swung 7-10 ms at
  120 fps (player shook against the camera: "motion blur" while running); the override
  smooths it (display period while the limit holds, else an average; the rest carried).
  TENCHU_TRACE=rawdelta disables it; TENCHU_TRACE=playerpos logs ms and position per frame.
- User clip planes: PA_CL_CLIP_CNTL bits 0-5 + PA_CL_UCP_n (0x2388) become SV_ClipDistance
  in translated vertex shaders (cbuffer b2). The water reflection pass clips at the water
  plane with plane 0; without it submerged legs showed upside down. TENCHU_TRACE=noucp.
- Shader debugging: TENCHU_DUMP_SHADERS=<dir> writes every translated shader's HLSL;
  traceframe also logs resolves in order ([draw] resolve ... -> dest).
- Wave water (sub_823C30E8, type 1 Gerstner waves, table 0x833AF5E0): the phase advanced
  a constant per frame; WavePhaseStepMidAsmHook scales it by the frame time.
- Shader exports: with export_data set, the scalar op also writes the vector destination;
  a component in both write masks is 1.0, in neither (scalar_dest_rel set) 0.0 (Xenia's
  rule). Writing the scalar to scalar_dest left water's projected w unset: no reflection.
- Front end: the game mode task (scheduler list head 0x82B5A65C, next +40, callbacks
  +24/+28) runs title 822A5CB8/822A2370, load prompt 822A2CC0, main menu 822A55B8/
  822A24B8, hideout 822A3920. Their animations count frames: overrides note front-end
  frames and host::activeFrameRateLimit() holds presents to 30 meanwhile
  (TENCHU_FRONTEND_UNCAPPED=1 disables). Control command 'list' walks such lists.
- Grapple (works; LT is the Aid/guard button, not the hook): RS click aims, LB fires. The
  verdict sub_82363860 needs the look direction >14.5 deg up, a 1400-unit ray hit
  (823DC650) without attribute bits 0x44, and the hit within 1100 of bone 24; it stores the
  hook point at player+4416/+5200 (player = *0x82B58A58). Lit reticle tint 0xFFFFFFFF, dim
  0xFFA0A0A0. States: 34 throw, 35 zip, 15 fall (hook points mid-wall end in a fall).
- Cheats (runtime/kernel/cheats.cpp, F1 > Cheats; control-file "cheat <id> on|off"): characters
  keep life at +0x118C (max +0x1188, guard gauge +0x1190, poison +0x11CC.. +0x11D8, type +0xA0
  with bit 0x1000 = player); every hit goes through the damage routine sub_822D8C20 (r3 target,
  f1 damage, r5 attacker; items carry the owner id at +0xCC). AI list u32[64] 0x82B57930, count
  0x82B58E6C; AI state +0x1420 (3 patrol, 6 noise, 7 spotted, 10 combat), sight check
  sub_82309910, think sub_822F2988 (contact timer +0x1458), noise sub_8228F3C0. MGR =
  *0x82B57814 (game data): profile MGR+0xA0 (money +0x1E8 from MGR, skill masks MGR+0x440/0x444,
  costume masks MGR+0x1F0/0x230, mission stats MGR+0x400), inventory MGR+0x470 (entries
  {id 0x10000+i, stock, flags} at +8, carried ids +536 / counts +708). Jump table 0x829D4998
  (32-byte rows, height at +0x1C), gravity 0x8200E600, grapple zip 0x820242D4 and hit distance
  0x820242BC. Player states: 0 idle, 11/12 jump, 15 fall, 34/35 grapple, 36 swimming. Jumping is
  disabled in the hideout. Verified in game: health (combat at 140/140 vs 140 -> 0 without),
  actions, jump (rise 232 -> 402), run (top speed x1.75), data patches. Mission 1 test bot:
  scratchpad bot.py style (peek positions, steer the stick from observed motion).
- Resolved textures are allocated padded (resolve widths round up): fetches scale UVs by
  fetch size / host size (g_textureScale), or blur chains drift (lantern bloom offset).
- Saved profile settings once had all volumes at 0 from button-mashing tests: if the
  game is silent, check Options > Sound Settings before debugging audio.
- Graphics extras ([graphics], F1 > Graphics, gpu/d3d11/post_effects.*): the scene composite
  ps 7582CB38B8D839B4 (DOF + bloom before the HUD; scene on fetch 2, depth resolve on fetch 3
  as 20e4-as-unorm, reversed Z) reads a processed copy: GTAO, CAS, experimental indirect
  light. The camera is voted from depth-writing draws' vertex constants (c0-c3 rows, c3-c6
  columns; validated 16:9 perspective). TENCHU_TRACE=postfx logs it and skipped composites
  (and other composite-like shaders), aoview shows the occlusion. Stable bloom: at scale > 1
  the bloom shrink ps 723D9B291D342106 reads a guest-size FP16 box average of the scene.
  MSAA override: host sample count of the 2x targets (keys and guest sizes stay 2x). The
  present scales with Catmull-Rom (up) or a box filter (down). Offline tests of the passes
  are easy: post_effects.cpp only needs host::settings, logf and traceEnabled.
- HDR: ResizeBuffers needs the back buffer unbound (the F1 menu re-presents). The HUD mask
  is a copy of the composite's target taken right after the composite (the HUD follows in
  the same target); the present's blit writes "still matches" into alpha
  (TENCHU_TRACE=hudmask screenshots show it). Nits 0 = the display's peak / Windows' SDR
  white (polled on its own thread).

## D3D facts (device at *VdGlobalDevice = 0x40001600)
- Present = sub_8241BDE0 (park GPU on SCRATCH1 + vsync callback 8241BD50) then
  sub_8241BF50. Interrupt handler 82425D88. Kick-off thread 82431D28 runs recorded
  command lists (interpreter 824318A8) handed over through interrupts.
- Kick (ring IB submission) sub_824262B8; resolve-like sub_8241CC58.
- Fence/status block *(dev+10768) = physical 0xB000 (+0 fence, +4 consumed CB ptr).
- FXO files are a custom effect format (magic A3D70141), no XDK shader containers.

## Next steps
1. Gameplay check of missions (combat, stealth kills, cutscenes in DATA/CUTSCENE).
2. Modern features as in Darkness: keyboard/mouse, settings file, fullscreen/borderless
   with Alt+Enter, resolution scaling (render targets are host textures already) and
   widescreen/ultrawide, frame pacing.
3. XMA loop fields (loop_start/end/count) are not implemented; check that looping
   sounds and music keep playing.
