#!/bin/bash
# Builds the Linux runtime's dependencies into build/deps-linux (pinned, checksummed):
#   vkd3d 1.19 (vkd3d-shader: HLSL -> DXBC for the translated shaders; with
#     tools/patches/vkd3d-clipdistance.patch, which lets HLSL write SV_ClipDistance),
#   DXVK 3.1.1 native (Direct3D 11 / DXGI on Vulkan, SDL3 windows),
#   SDL3 3.4.18 (window, audio, input; the version the Windows build uses),
#   FFmpeg (tools/build_xma_codec.py) and Dear ImGui (tools/fetch_imgui.py).
# Needs clang or gcc, cmake, ninja, meson, glslang, pkg-config, perl with JSON, bison, flex,
# nasm, the Vulkan and X11/Wayland development headers (Ubuntu/Debian: tools/install_linux_deps.sh).
# Sources and build trees go to $TENCHU_DEPS_WORK (default ~/tenchu-deps: building on a
# Windows drive under WSL is slow); TENCHU_DEPS_DIR replaces build/deps-linux (the release
# build, tools/build_linux_release.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="${TENCHU_DEPS_DIR:-$ROOT/build/deps-linux}"
WORK="${TENCHU_DEPS_WORK:-$HOME/tenchu-deps}"
export TENCHU_DEPS_DIR="$DEPS" TENCHU_DEPS_WORK="$WORK"
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
  # Its build needs Vulkan 1.3.228+ and matching SPIR-V headers (Ubuntu 22.04 has 1.3.204):
  # pinned copies, used at build time only.
  fetch https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.3.290.tar.gz \
    Vulkan-Headers-1.3.290.tar.gz f38a653bf93cab7a2a229a53d2d53b1cba9a2819e4c0a7de13c54085bde9bcf5
  fetch https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.3.290.0.tar.gz \
    SPIRV-Headers-1.3.290.0.tar.gz 1b9ff8a33e07814671dee61fe246c67ccbcfc9be6581f229e251784499700e24
  rm -rf "$WORK/Vulkan-Headers-1.3.290" "$WORK/SPIRV-Headers-vulkan-sdk-1.3.290.0"
  tar xf "$WORK/src/Vulkan-Headers-1.3.290.tar.gz" -C "$WORK"
  tar xf "$WORK/src/SPIRV-Headers-1.3.290.0.tar.gz" -C "$WORK"
  rm -rf "$WORK/vkd3d-1.19" && tar xf "$WORK/src/vkd3d-1.19.tar.xz" -C "$WORK"
  (cd "$WORK/vkd3d-1.19" && patch -p1 < "$ROOT/tools/patches/vkd3d-clipdistance.patch" &&
   ./configure --prefix="$DEPS/vkd3d" --libdir="$DEPS/vkd3d/lib" --disable-tests --disable-demos \
     CPPFLAGS="-I$WORK/Vulkan-Headers-1.3.290/include -I$WORK/SPIRV-Headers-vulkan-sdk-1.3.290.0/include" >/dev/null &&
   make -j"$JOBS" >/dev/null && make install >/dev/null)
  cp "$WORK/vkd3d-1.19/COPYING" "$DEPS/vkd3d/COPYING"
fi

# ---- SDL3 --------------------------------------------------------------------------------
if [ "$(cat "$DEPS/SDL3/VERSION" 2>/dev/null)" != 3.4.18 ]; then
  fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz SDL3-3.4.18.tar.gz \
    9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
  rm -rf "$WORK/SDL3-3.4.18" "$WORK/SDL3-build" "$DEPS/SDL3" && tar xf "$WORK/src/SDL3-3.4.18.tar.gz" -C "$WORK"
  cmake -S "$WORK/SDL3-3.4.18" -B "$WORK/SDL3-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEPS/SDL3" -DCMAKE_INSTALL_LIBDIR=lib -DSDL_TESTS=OFF -DSDL_STATIC=OFF >/dev/null
  cmake --build "$WORK/SDL3-build" >/dev/null && cmake --install "$WORK/SDL3-build" >/dev/null
  cp "$WORK/SDL3-3.4.18/LICENSE.txt" "$DEPS/SDL3/LICENSE.txt"
  echo 3.4.18 > "$DEPS/SDL3/VERSION"
fi

# ---- DXVK native -------------------------------------------------------------------------
if [ ! -f "$DEPS/dxvk/lib/libdxvk_d3d11.so.0" ]; then
  if [ ! -d "$WORK/dxvk-src" ]; then
    git clone -q --depth 1 --branch v3.1.1 --recurse-submodules --shallow-submodules \
      https://github.com/doitsujin/dxvk.git "$WORK/dxvk-src"
  fi
  [ "$(git -C "$WORK/dxvk-src" rev-parse HEAD)" = b1a1c99ab52b687cf950d62c88bc2fa316b41663 ] ||
    { echo "unexpected DXVK commit" >&2; exit 1; }
  # Its SDL3 window system integration builds against the SDL3 above.
  (cd "$WORK/dxvk-src" && rm -rf build-native &&
   PKG_CONFIG_PATH="$DEPS/SDL3/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
   meson setup build-native --buildtype=release --prefix="$DEPS/dxvk" --libdir=lib \
     -Dnative_sdl3=enabled -Dnative_sdl2=disabled -Dnative_glfw=disabled >/dev/null &&
   ninja -C build-native >/dev/null && ninja -C build-native install >/dev/null)
  cp "$WORK/dxvk-src/LICENSE" "$DEPS/dxvk/LICENSE"
fi

# ---- FFmpeg and Dear ImGui ---------------------------------------------------------------
[ -f "$DEPS/ffmpeg/include/libavcodec/avcodec.h" ] || [ -n "${TENCHU_SKIP_FFMPEG:-}" ] || python3 "$ROOT/tools/build_xma_codec.py"
[ -f "$ROOT/build/deps/imgui/imgui.h" ] || python3 "$ROOT/tools/fetch_imgui.py"
echo "Linux dependencies ready in $DEPS"
