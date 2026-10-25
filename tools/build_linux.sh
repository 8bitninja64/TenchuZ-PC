#!/bin/sh
# Builds Tenchu Z on Linux in one command (the counterpart of build.ps1): the generator
# (XenonRecomp in ../_Tools/XenonRecomp/build-linux), the game code translated from your disc
# (tenchu_game.so) and the runtime (TenchuZ) with its libraries (tools/setup_linux_deps.sh).
# Needs what tools/install_linux_deps.sh installs (Ubuntu/Debian; other distributions: the
# same tools and headers).
#   tools/build_linux.sh [disc image or default.xex] [build dir, default build/linux-clang]
# TENCHU_SKIP_RECOMP=1 builds the runtime only (no disc needed), as for a release.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
disc=${1:-"$root/Tenchu Z (USA, Europe) (En,Fr,De,Es,It).iso"}
bdir=${2:-"$root/build/linux-clang"}
cd "$root"

# 1. The generator: pinned XenonRecomp with the patch series, rebuilt when they change. It emits
#    wrappers, not weak aliases (XENON_RECOMP_USE_ALIAS, its default with Clang), so the
#    translated code is the same as the Windows (MSVC-built) generator's.
echo "# Generator"
python3 tools/setup_generator.py
tools=$(cd "$root/../_Tools/XenonRecomp" && pwd)
gen="$tools/build-linux"
stamp=$(cat tools/setup_generator.py tools/patches/*.patch | sha256sum | cut -c1-64)
if [ ! -x "$gen/XenonRecomp/XenonRecomp" ] || [ "$(cat "$gen/patches.stamp" 2>/dev/null)" != "$stamp" ]; then
  cmake -S "$tools" -B "$gen" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS=-UXENON_RECOMP_USE_ALIAS
  cmake --build "$gen" --target XenonRecomp
  echo "$stamp" > "$gen/patches.stamp"
fi

# 2. The game code: default.xex from the disc, translated into build/generated.
generated=/nonexistent
if [ -z "$TENCHU_SKIP_RECOMP" ]; then
  echo "# Translating default.xex"
  case $disc in
    *.xex) xex=$disc ;;
    *) if [ -f "$disc" ]; then
         python3 tools/extract_iso.py "$disc" build/xex --only default.xex
         xex=build/xex/default.xex
       elif [ -f game/default.xex ]; then
         xex=game/default.xex
       else
         echo "Disc image not found: $disc (pass the path of your Tenchu Z disc image)" >&2; exit 1
       fi ;;
  esac
  python3 tools/recompile.py --xex "$xex" --generator "$gen/XenonRecomp/XenonRecomp"
  generated="$root/build/generated"
fi

# 3. Libraries: vkd3d-shader, SDL3, DXVK-native, FFmpeg, Dear ImGui.
echo "# Libraries"
bash tools/setup_linux_deps.sh

# 4. The runtime (and the game module when the game code was translated).
echo "# Runtime"
cmake -S . -B "$bdir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ \
  -DTENCHU_GENERATED_DIR="$generated"
cmake --build "$bdir" --target TenchuZ
echo "built $bdir/TenchuZ"
