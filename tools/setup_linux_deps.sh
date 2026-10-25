#!/bin/bash
# Builds the Linux runtime's dependencies into build/deps-linux (pinned, checksummed):
#   vkd3d 1.19 (vkd3d-shader: HLSL -> DXBC for the translated shaders; with
#     tools/patches/vkd3d-clipdistance.patch, which lets HLSL write SV_ClipDistance),
#   DXVK 3.1.1 native (Direct3D 11 / DXGI on Vulkan, SDL3 windows),
#   SDL3 3.2.10 (window, audio, input; the version the Windows build uses),
#   FFmpeg (tools/build_xma_codec.py) and Dear ImGui (tools/fetch_imgui.py).
# Needs clang or gcc, cmake, ninja, meson, glslang, pkg-config, perl with JSON, bison, flex,
# nasm, the Vulkan and X11/Wayland development headers (Ubuntu: see tools/install_linux_deps.sh).
# Sources and build trees go to $TENCHU_LINUX_WORK (default ~/tenchu-deps: building on a
# Windows drive under WSL is slow).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/build/deps-linux"
WORK="${TENCHU_LINUX_WORK:-$HOME/tenchu-deps}"
JOBS="$(nproc)"
mkdir -p "$DEPS" "$WORK/src"

fetch() {  # url file sha256
  local file="$WORK/src/$2"
  [ -f "$file" ] || curl -fsSL "$1" -o "$file.part" && { [ -f "$file" ] || mv "$file.part" "$file"; }
  echo "$3  $file" | sha256sum -c --quiet - || { echo "checksum mismatch: $file" >&2; exit 1; }
}

# ---- vkd3d -------------------------------------------------------------------------------
if [ ! -f "$DEPS/vkd3d/lib/libvkd3d-shader.so.1" ]; then
  fetch https://dl.winehq.org/vkd3d/source/vkd3d-1.19.tar.xz vkd3d-1.19.tar.xz \
    034613605baab8ba84674f8d272cf22b5e86bc6bc03fc5728ef9bce07308baa6
  rm -rf "$WORK/vkd3d-1.19" && tar xf "$WORK/src/vkd3d-1.19.tar.xz" -C "$WORK"
  (cd "$WORK/vkd3d-1.19" && patch -p1 < "$ROOT/tools/patches/vkd3d-clipdistance.patch" &&
   ./configure --prefix="$DEPS/vkd3d" --libdir="$DEPS/vkd3d/lib" --disable-tests --disable-demos >/dev/null &&
   make -j"$JOBS" >/dev/null && make install >/dev/null)
  cp "$WORK/vkd3d-1.19/COPYING" "$DEPS/vkd3d/COPYING"
fi

# ---- DXVK native -------------------------------------------------------------------------
if [ ! -f "$DEPS/dxvk/lib/libdxvk_d3d11.so.0" ]; then
  if [ ! -d "$WORK/dxvk-src" ]; then
    git clone -q --depth 1 --branch v3.1.1 --recurse-submodules --shallow-submodules \
      https://github.com/doitsujin/dxvk.git "$WORK/dxvk-src"
  fi
  [ "$(git -C "$WORK/dxvk-src" rev-parse HEAD)" = b1a1c99ab52b687cf950d62c88bc2fa316b41663 ] ||
    { echo "unexpected DXVK commit" >&2; exit 1; }
  (cd "$WORK/dxvk-src" && rm -rf build-native &&
   meson setup build-native --buildtype=release --prefix="$DEPS/dxvk" --libdir=lib \
     -Dnative_sdl3=enabled -Dnative_sdl2=disabled -Dnative_glfw=disabled >/dev/null &&
   ninja -C build-native >/dev/null && ninja -C build-native install >/dev/null)
  cp "$WORK/dxvk-src/LICENSE" "$DEPS/dxvk/LICENSE"
fi

# ---- SDL3 --------------------------------------------------------------------------------
if [ ! -f "$DEPS/SDL3/lib/libSDL3.so.0" ]; then
  fetch https://github.com/libsdl-org/SDL/releases/download/release-3.2.10/SDL3-3.2.10.tar.gz SDL3-3.2.10.tar.gz \
    f87be7b4dec66db4098e9c167b2aa34e2ca10aeb5443bdde95ae03185ed513e0
  rm -rf "$WORK/SDL3-3.2.10" && tar xf "$WORK/src/SDL3-3.2.10.tar.gz" -C "$WORK"
  cmake -S "$WORK/SDL3-3.2.10" -B "$WORK/SDL3-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEPS/SDL3" -DCMAKE_INSTALL_LIBDIR=lib -DSDL_TESTS=OFF -DSDL_STATIC=OFF >/dev/null
  cmake --build "$WORK/SDL3-build" >/dev/null && cmake --install "$WORK/SDL3-build" >/dev/null
  cp "$WORK/SDL3-3.2.10/LICENSE.txt" "$DEPS/SDL3/LICENSE.txt"
fi

# ---- FFmpeg and Dear ImGui ---------------------------------------------------------------
[ -f "$DEPS/ffmpeg/include/libavcodec/avcodec.h" ] || [ -n "${TENCHU_SKIP_FFMPEG:-}" ] || python3 "$ROOT/tools/build_xma_codec.py"
[ -f "$ROOT/build/deps/imgui/imgui.h" ] || python3 "$ROOT/tools/fetch_imgui.py"
echo "Linux dependencies ready in $DEPS"
