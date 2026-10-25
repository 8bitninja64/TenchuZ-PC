#!/bin/sh
# Installs what the Linux build needs on Ubuntu/Debian (22.04 and newer): compilers, CMake,
# Ninja, Meson, glslang (DXVK's shaders), the vkd3d build tools, and the development headers
# SDL3 needs for X11, Wayland, PulseAudio/PipeWire/ALSA and controllers. On Ubuntu 22.04 it adds
# clang 18 from apt.llvm.org (key fingerprint checked) with g++-12's C++ library, and pip's CMake
# and Meson (jammy's are too old), as the NFL Blitz 2003 port's release build does.
#   sudo tools/install_linux_deps.sh
set -e
export DEBIAN_FRONTEND=noninteractive
SUDO=
[ "$(id -u)" = 0 ] || SUDO=sudo
. /etc/os-release
$SUDO apt-get update -qq
$SUDO apt-get install -y -qq --no-install-recommends \
  ca-certificates curl git gnupg xz-utils file python3 python3-pip pkg-config ninja-build make patch \
  autoconf automake libtool bison flex libjson-perl nasm glslang-tools spirv-headers libvulkan-dev \
  libasound2-dev libpulse-dev libpipewire-0.3-dev \
  libx11-dev libx11-xcb-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
  libxinerama-dev libxss-dev libxtst-dev libxkbcommon-dev \
  libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev \
  libwayland-dev wayland-protocols libdecor-0-dev libdbus-1-dev libudev-dev libusb-1.0-0-dev >/dev/null
if [ "$VERSION_CODENAME" = jammy ]; then
  fpr=6084F3CF814B57C1CF12EFD515CF4D18AF4F7421
  curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor > /tmp/llvm.gpg
  gpg --show-keys --with-colons /tmp/llvm.gpg | grep -q "^fpr:*$fpr:" ||
    { echo "apt.llvm.org key is not $fpr" >&2; exit 1; }
  $SUDO install -m 644 /tmp/llvm.gpg /usr/share/keyrings/llvm.gpg
  echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/jammy/ llvm-toolchain-jammy-18 main" |
    $SUDO tee /etc/apt/sources.list.d/llvm-18.list >/dev/null
  $SUDO apt-get update -qq
  $SUDO apt-get install -y -qq --no-install-recommends clang-18 lld-18 g++-12 >/dev/null
  $SUDO update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 100
  $SUDO update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 100
  $SUDO python3 -m pip install -q --no-cache-dir 'cmake==3.31.6' 'meson==1.5.2'
else
  $SUDO apt-get install -y -qq --no-install-recommends clang lld cmake meson >/dev/null
fi
echo "Linux build dependencies installed"
