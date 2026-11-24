# Generator patches

Applied in order to hedge-dev/XenonRecomp commit
`c5bfd90d87f2ed0db8cff5c19ea3aff0e161e527` by `tools/setup_generator.py`.

1. `xenonrecomp-darkness.patch` and
2. `xenonrecomp-corrections.patch`: copied unchanged from The Darkness Recomp
   (portingpete/The-Darkness-Recomp @ `98f0bcd`, GPLv3). Additional PowerPC/VMX
   translations, reachable-instruction and inline switch recovery, explicit
   semantic diagnostics, translation-unit counts, the guest timebase hook, and
   correctness fixes. See that repository's `tools/patches/README.md`.
3. `xenonrecomp-tenchuz.patch`: instructions Tenchu Z needs that the series
   lacked:
   - `vpkuwus` / `vpkuwus128` (58 sites, mostly in the `.embsec_` sections):
     unsigned word to halfword pack with unsigned saturation.
   - `vslo` / `vslo128` (2 sites): vector shift left by octet.
   - `vcmpbfp` / `vcmpbfp128` and their record forms (2 sites): bounds compare.
     Upstream emitted a debug trap here.

4. `xenonrecomp-system-link-half.patch`: FLOAT16_2 unpack (`vupkd3d128`,
   immediate 12), first reached while receiving another player's state. The
   previous generator emitted a debug trap. Handles source/destination aliasing,
   signed zeros, subnormals, infinities and NaNs; unsupported unpack formats now
   fail the strict generation gate. `tools/test_generated_half.py` checks the
   emitted instruction against Python's half conversion for all 65,536 inputs.

The VSCR saturation bit is not modelled, matching the existing pack instructions.
