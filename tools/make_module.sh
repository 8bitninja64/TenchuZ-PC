#!/bin/sh
# Builds only the game module (tenchu_game.so) from default.xex: translate it with XenonRecomp
# (tools/recompile.py), compile it (tools/compile_module.py). The Linux counterpart of
# make_module.ps1, run by the game's first start on the player's PC; the runtime (TenchuZ) is not
# rebuilt. Needs python3 (3.10+); the compiler is the Zig toolchain a release carries
# (tools/zig.tar.xz, unpacked into the work folder for the build and removed after: SteamOS and
# other systems without a compiler), else clang++. As in the NFL Blitz 2003 port.
#   tools/make_module.sh <default.xex> [out, default build/linux/tenchu_game.so] [work dir]
# TENCHU_CXX=<compiler> picks one (e.g. clang++, or "zig" for the bundled toolchain).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
xex=${1:?usage: make_module.sh <default.xex> [out] [work dir]}
out=${2:-"$root/build/linux/tenchu_game.so"}
work=${3:-"$root/build/module"}
case $xex in /*) ;; *) xex="$PWD/$xex" ;; esac
case $out in /*) ;; *) out="$PWD/$out" ;; esac
case $work in /*) ;; *) work="$PWD/$work" ;; esac
cd "$root"
command -v python3 >/dev/null || { echo "python3 is required (install your distribution's python3 package)" >&2; exit 1; }
mkdir -p "$work"
echo "Tenchu Z: compiling the game code from your disc. This is done once."

# The compiler: bundled Zig first (the same compiler on every system), then the system's clang++
# (the generated code needs Clang; g++ cannot build it).
zig_kit="$root/tools/zig.tar.xz"
pick=${TENCHU_CXX:-}
if [ -z "$pick" ]; then
  if [ -f "$zig_kit" ] || [ -x "$root/tools/zig/zig" ]; then pick=zig
  elif command -v clang++ >/dev/null; then pick=clang++
  else echo "no C++ compiler: this package's tools/zig.tar.xz is missing and clang++ is not installed" >&2; exit 1
  fi
fi
# The unpacked compiler and its cache go when the script ends, also when it is cancelled
# (signals become a normal exit so that every shell runs the EXIT trap).
zig_tmp=""
cleanup() {
  if [ -n "$zig_tmp" ]; then rm -rf "$zig_tmp" "$work/zig-cache"; fi
  rm -rf "$work/gen" "$work/obj"
  return 0
}
trap cleanup EXIT
trap 'exit 143' TERM INT HUP
if [ "$pick" = zig ]; then
  if [ -x "$root/tools/zig/zig" ]; then zig="$root/tools/zig/zig"
  else
    echo "# Unpacking the compiler"
    zig_tmp="$work/zig"
    rm -rf "$zig_tmp"; mkdir -p "$zig_tmp"
    python3 -I tools/compile_module.py unpack "$zig_kit" "$zig_tmp"
    zig="$zig_tmp/zig/zig"
  fi
  export ZIG_GLOBAL_CACHE_DIR="$work/zig-cache" ZIG_LOCAL_CACHE_DIR="$work/zig-cache"
  # By name on PATH: $CXX is split into words, and the user directory may have spaces in it.
  PATH="$(dirname "$zig"):$PATH"
  export CXX="zig c++ -target x86_64-linux-gnu"
else
  export CXX="$pick"
fi
echo "compiler: $CXX"

# Every build starts clean: units of another version must not be compiled in.
rm -rf "$work/gen" "$work/obj"
echo "# Translating the game code"
python3 -I tools/recompile.py --xex "$xex" --output "$work/gen" --strict
# simde: the release's third_party copy, or the generator's in a source checkout.
simde="$root/third_party/simde"
[ -f "$simde/x86/avx.h" ] || simde="$root/../_Tools/XenonRecomp/thirdparty/simde"
[ -f "$simde/x86/avx.h" ] || { echo "simde headers are missing (third_party/simde)" >&2; exit 1; }
python3 -I tools/compile_module.py --gen "$work/gen" --out "$out" --obj "$work/obj" \
  --include "$root/runtime/guest" --include "$simde" --extra "$root/runtime/guest/module_rt.cpp"
echo "built $out"
