#!/bin/sh
# Assembles the Linux release (dist/TenchuZ + dist/TenchuZ-linux-x64.tar.xz) WITHOUT any game
# code: the runtime and its libraries plus the kit the first start compiles the game module with
# (the generator, scripts, configuration, headers and a compiler). The counterpart of package.ps1.
#   tools/package_linux.sh [build dir] [out, default dist/TenchuZ]
# The build dir is tools/build_linux_release.sh's (runtime/, xenonrecomp/, xenonrecomp-src/,
# deps/) or tools/build_linux.sh's (default build/linux-clang, with the generator in
# ../_Tools/XenonRecomp/build-linux and the libraries in build/deps-linux).
# TENCHU_MAX_GLIBC=2.35 (the release) refuses a runtime that needs a newer glibc. The compiler is
# Zig 0.17.0 (ZIG_DIR, default ~/zig/zig-x86_64-linux-0.17.0), as in the NFL Blitz 2003 port.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
bdir=${1:-"$root/build/linux-clang"}
out=${2:-"$root/dist/TenchuZ"}
cd "$root"
if [ -f "$bdir/runtime/TenchuZ" ]; then
  bin="$bdir/runtime" generator="$bdir/xenonrecomp/XenonRecomp/XenonRecomp" xenon="$bdir/xenonrecomp-src" deps="$bdir/deps"
else
  bin="$bdir" generator="$root/../_Tools/XenonRecomp/build-linux/XenonRecomp/XenonRecomp"
  xenon="$root/../_Tools/XenonRecomp" deps="$root/build/deps-linux"
fi
[ -f "$bin/TenchuZ" ] || { echo "build the runtime first (tools/build_linux.sh or tools/build_linux_release.sh)" >&2; exit 1; }
[ -x "$generator" ] || { echo "the generator is missing: $generator" >&2; exit 1; }
rm -rf "$out" "$out-linux-x64.tar.xz"   # a failed run must not leave an older archive behind
mkdir -p "$out"
put() { mkdir -p "$(dirname "$out/$2")"; cp -L "$1" "$out/$2"; }

# The runtime and the libraries it loads (by soname, next to it: its rpath is $ORIGIN).
put "$bin/TenchuZ" TenchuZ
strip "$out/TenchuZ"
for lib in $(objdump -p "$out/TenchuZ" | awk '/NEEDED/ {print $2}'); do
  case $lib in
    libc.so*|libm.so*|libstdc++.so*|libgcc_s.so*|libdl.so*|libpthread.so*|ld-linux*) ;;
    *) put "$bin/$lib" "$lib"; strip --strip-unneeded "$out/$lib" ;;
  esac
done
if [ -n "$TENCHU_MAX_GLIBC" ]; then
  need=$(for f in "$out/TenchuZ" "$out"/*.so*; do objdump -T "$f"; done | grep -o 'GLIBC_[0-9][0-9.]*' |
         sed 's/GLIBC_//' | sort -uV | tail -1)
  newest=$(printf '%s\n%s\n' "$need" "$TENCHU_MAX_GLIBC" | sort -V | tail -1)
  [ "$newest" = "$TENCHU_MAX_GLIBC" ] || { echo "the runtime needs glibc $need, more than $TENCHU_MAX_GLIBC" >&2; exit 1; }
  echo "glibc needed: $need (ceiling $TENCHU_MAX_GLIBC)"
fi
put assets/gamecontrollerdb.txt gamecontrollerdb.txt
put README.md README.md
put COPYING COPYING

# The kit: generator, scripts, export tables, configuration, module headers, simde.
put "$generator" tools/XenonRecomp
strip "$out/tools/XenonRecomp"
for f in tools/recompile.py tools/xex_image.py tools/compile_module.py tools/make_module.sh tools/setup_generator.py \
         config/tenchuz.toml config/tenchuz_switch_tables.toml config/tenchuz_switch_tables_manual.toml \
         config/overrides.txt runtime/guest/ppc_context.template.h runtime/guest/ppc_config.h \
         runtime/guest/module_abi.h runtime/guest/module_rt.cpp; do
  put "$f" "$f"
done
for f in tools/patches/*; do put "$f" "$f"; done
put "$xenon/XenonUtils/xbox/xboxkrnl_table.inc" tools/xbox/xboxkrnl_table.inc
put "$xenon/XenonUtils/xbox/xam_table.inc" tools/xbox/xam_table.inc
simde="$xenon/thirdparty/simde"
for f in "$simde"/*.h; do put "$f" "third_party/simde/$(basename "$f")"; done
for f in "$simde"/x86/*.h; do put "$f" "third_party/simde/x86/$(basename "$f")"; done
put "$simde/COPYING" third_party/simde/COPYING
chmod +x "$out/TenchuZ" "$out/tools/make_module.sh" "$out/tools/XenonRecomp"

# The compiler for the first-start build: the unpacked official Zig release, so players need no
# compiler (SteamOS has none). Only what an x86_64-linux-gnu build uses is kept; every license
# text it ships stays. Packed as tools/zig.tar.xz, unpacked by make_module.sh.
zig_dir=${ZIG_DIR:-"$HOME/zig/zig-x86_64-linux-0.17.0"}
[ -x "$zig_dir/zig" ] || { echo "Zig not found at $zig_dir (set ZIG_DIR)" >&2; exit 1; }
ztmp=$(mktemp -d)
mkdir -p "$ztmp/zig/lib/libc/include"
(cd "$zig_dir" && tar cf - --exclude=./doc --exclude=./lib/libtsan --exclude=./lib/libc/include \
   --exclude=./lib/libc/musl --exclude=./lib/libc/mingw --exclude=./lib/libc/wasi --exclude=./lib/libc/darwin \
   --exclude=./lib/libc/freebsd --exclude=./lib/libc/netbsd --exclude=./lib/libc/openbsd .) | tar xf - -C "$ztmp/zig"
for d in generic-glibc any-linux-any x86-linux-any x86-linux-gnu; do
  cp -r "$zig_dir/lib/libc/include/$d" "$ztmp/zig/lib/libc/include/"
done
(cd "$zig_dir" && find . -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' \)) | while read -r f; do
  [ -e "$ztmp/zig/$f" ] || { mkdir -p "$ztmp/zig/$(dirname "$f")"; cp "$zig_dir/$f" "$ztmp/zig/$f"; }
done
tar -C "$ztmp" -cf - zig | xz -9 -T0 > "$out/tools/zig.tar.xz"
rm -rf "$ztmp"

# Licenses of the bundled components.
put "$zig_dir/LICENSE" licenses/zig.txt
put "$zig_dir/lib/libc/glibc/LICENSES" licenses/glibc-LICENSES.txt
put third_party_licenses/LGPL-2.1.txt licenses/LGPL-2.1.txt
put third_party_licenses/GPL-2.0.txt licenses/GPL-2.0.txt
put third_party_licenses/Linux-syscall-note.txt licenses/Linux-syscall-note.txt
cat >> "$out/licenses/zig.txt" <<'EOF'

tools/zig.tar.xz holds files of the unmodified Zig 0.17.0 release (https://ziglang.org), limited to
what x86_64 Linux builds use. Besides Zig's own code (above) it carries:
- LLVM's libc++, libc++abi and libunwind (Apache-2.0 WITH LLVM-exception; their LICENSE.TXT files
  are inside the archive, in zig/lib/libcxx, libcxxabi and libunwind),
- glibc headers and ABI lists (LGPL-2.1-or-later: licenses/LGPL-2.1.txt; other permission notices:
  licenses/glibc-LICENSES.txt),
- Linux kernel UAPI headers (GPL-2.0 WITH Linux-syscall-note: licenses/GPL-2.0.txt,
  licenses/Linux-syscall-note.txt).
EOF
for f in "$deps"/ffmpeg/COPYING* "$deps"/ffmpeg/LICENSE* "$deps"/ffmpeg/PROVENANCE*; do put "$f" "licenses/ffmpeg/$(basename "$f")"; done
# FFmpeg is modified (the XMA decoder option): the script holds the patch, the pinned source and the build.
put tools/build_xma_codec.py licenses/ffmpeg/build_xma_codec.py
put "$deps/SDL3/LICENSE.txt" licenses/sdl3/LICENSE.txt
put "$deps/dxvk/LICENSE" licenses/dxvk/LICENSE
# vkd3d-shader (LGPL-2.1-or-later), with the SV_ClipDistance patch in tools/patches; its
# source: the pinned release in tools/setup_linux_deps.sh plus that patch.
put "$deps/vkd3d/COPYING" licenses/vkd3d/COPYING
put tools/setup_linux_deps.sh licenses/vkd3d/setup_linux_deps.sh
put build/deps/imgui/LICENSE.txt licenses/imgui/LICENSE.txt
put "$xenon/LICENSE.md" licenses/xenonrecomp/LICENSE.md
put "$xenon/thirdparty/xxHash/LICENSE" licenses/xenonrecomp/xxHash-LICENSE
put "$xenon/thirdparty/tomlplusplus/LICENSE" licenses/xenonrecomp/tomlplusplus-LICENSE
put "$xenon/thirdparty/fmt/LICENSE" licenses/xenonrecomp/fmt-LICENSE
put "$xenon/thirdparty/libmspack/libmspack/COPYING.LIB" licenses/xenonrecomp/libmspack-COPYING.LIB
head -30 "$xenon/thirdparty/TinySHA1/TinySHA1.hpp" > "$out/licenses/xenonrecomp/TinySHA1-NOTICE.txt"
cat > "$out/licenses/xenonrecomp/README.txt" <<'EOF'
tools/XenonRecomp is hedge-dev's XenonRecomp (MIT, LICENSE.md) at the commit pinned in
tools/setup_generator.py with the patches in tools/patches (GPLv3, from The Darkness Recomp),
built with Clang. It links binutils' PowerPC disassembler (GPL-2.0-or-later; see COPYING at the
top of this release), xxHash (BSD-2-Clause), toml++ (MIT), fmt (MIT), libmspack (LGPL-2.1)
and TinySHA1 (ISC-style notice). Source: https://github.com/hedge-dev/XenonRecomp plus the
patches; tools/setup_generator.py reproduces the tree.
EOF
put "$simde/COPYING" licenses/simde/COPYING

# Never ship anything derived from the game.
bad=$(find "$out" \( -name 'tenchu_game.*' -o -name 'ppc_recomp.*.cpp' -o -name 'ppc_func_mapping.cpp' \
      -o -name 'ppc_module.cpp' -o -name 'ppc_recomp_shared.h' -o -name '*.xex' -o -name '*.iso' -o -name 'TenchuZ.ini' \))
[ -z "$bad" ] || { echo "refusing to package game-derived files: $bad" >&2; exit 1; }
echo "packaged $out ($(du -sh "$out" | cut -f1))"
tar -C "$(dirname "$out")" -cJf "$out-linux-x64.tar.xz" "$(basename "$out")"
echo "archived $out-linux-x64.tar.xz ($(du -h "$out-linux-x64.tar.xz" | cut -f1))"
