#!/bin/sh
# Builds the Linux release runtime the way CI does, for the widest range of distributions: run it
# in an Ubuntu 22.04 (glibc 2.35) container (the NFL Blitz 2003 port's recipe). The generator,
# vkd3d-shader, SDL3, DXVK-native and FFmpeg are built from pinned sources into the build
# directory; the runtime needs nothing from the player's system besides glibc, libstdc++ and the
# Vulkan loader. No game code: the module is compiled from the player's disc on the first start.
#   podman run --rm -v <repo>:/src -v <build dir>:/build ubuntu:22.04 sh /src/tools/build_linux_release.sh /src /build
#   tools/build_linux_release.sh [source dir] [build dir]   (TENCHU_SKIP_DEPS=1: skip the apt part)
# Then: tools/package_linux.sh <build dir>.
set -e
src=${1:-$(cd "$(dirname "$0")/.." && pwd)}
bdir=${2:-"$src/build/linux-release"}
[ -n "$TENCHU_SKIP_DEPS" ] || sh "$src/tools/install_linux_deps.sh"
cd "$src"
git config --global --add safe.directory '*'   # a mounted tree owned by another user

# The generator (wrappers, not weak aliases: the same output as the Windows build's).
python3 tools/setup_generator.py --destination "$bdir/xenonrecomp-src"
cmake -S "$bdir/xenonrecomp-src" -B "$bdir/xenonrecomp" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS=-UXENON_RECOMP_USE_ALIAS
cmake --build "$bdir/xenonrecomp" --target XenonRecomp

# Libraries, then the runtime.
TENCHU_DEPS_DIR="$bdir/deps" TENCHU_DEPS_WORK="$bdir/deps-work" bash tools/setup_linux_deps.sh
cmake -S "$src" -B "$bdir/runtime" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ \
  -DTENCHU_GENERATED_DIR=/nonexistent -DTENCHU_DEPS="$bdir/deps" \
  -DTENCHU_SIMDE_DIR="$bdir/xenonrecomp-src/thirdparty/simde"
cmake --build "$bdir/runtime" --target TenchuZ
echo "built $bdir/runtime/TenchuZ"
