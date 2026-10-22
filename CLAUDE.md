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

## Next steps
1. Runtime: start from Darkness `runtime/kernel` + `runtime/native` (kernel.cpp,
   guest_runner, dispatcher, filesystem, storage, xex_image), generalize the
   Darkness-specific pieces, stub networking/voice. 82 imports are missing there.
2. Get to the first VdSwap with a null renderer.
3. Graphics: identify XDK D3D functions (signatures/Ghidra), hook them, translate
   Xenos shader microcode (from the FXO effects) to HLSL.
