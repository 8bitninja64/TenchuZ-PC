# Builds Tenchu Z from your disc image in one command: the generator (XenonRecomp), the
# game code translated from the disc (tenchu_game.dll) and the runtime (TenchuZ.exe).
# Needs Git, Python 3.11+ and Visual Studio 2022 (or its Build Tools) with C++ and Clang.
#   powershell -ExecutionPolicy Bypass -File tools/build.ps1 [-Iso <iso>] [-Configure] [-SkipRecomp] [-Target TenchuZ] [-BuildDir dir]
# -SkipRecomp builds the runtime only (no disc needed), as for a release.
param(
  [string]$Iso = "Tenchu Z (USA, Europe) (En,Fr,De,Es,It).iso",
  [switch]$Configure,
  [switch]$SkipRecomp,
  [string]$Target = "TenchuZ",
  [string]$BuildDir = "build/windows-clang"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. "$PSScriptRoot\vsenv.ps1"
. "$PSScriptRoot\python.ps1"
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw "Git is required (https://git-scm.com)." }
function Assert-Exit([string]$step) { if ($LASTEXITCODE) { throw "$step failed (exit code $LASTEXITCODE)." } }

# 1. The generator: pinned XenonRecomp with the patch series in ../_Tools/XenonRecomp,
#    built with MSVC (a Clang-built generator emits weak aliases instead of wrappers).
Write-Host "# Generator"
Invoke-Python tools/setup_generator.py; Assert-Exit "Generator setup"
$tools = (Resolve-Path "..\_Tools\XenonRecomp").Path
$generatorBuild = Join-Path $tools "build-tenchu"
$generator = Join-Path $generatorBuild "XenonRecomp\XenonRecomp.exe"
$inputs = @(Get-Item tools/setup_generator.py) + @(Get-ChildItem tools/patches/*.patch | Sort-Object Name)
$stamp = ($inputs | ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash }) -join ""
$stampFile = Join-Path $generatorBuild "patches.stamp"
if (-not (Test-Path $generator) -or -not (Test-Path $stampFile) -or (Get-Content $stampFile -Raw).Trim() -ne $stamp) {
  cmake -S $tools -B $generatorBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
  Assert-Exit "Configuring the generator"
  cmake --build $generatorBuild --target XenonRecomp; Assert-Exit "Building the generator"
  Set-Content -Path $stampFile -Value $stamp
}

# 2. The game code: default.xex from the disc, translated into build/generated.
if (-not $SkipRecomp) {
  Write-Host "# Translating default.xex"
  if (Test-Path $Iso) {
    Invoke-Python tools/extract_iso.py $Iso build/xex --only default.xex; Assert-Exit "Reading the disc image"
    $xex = "build/xex/default.xex"
  } elseif (Test-Path "game/default.xex") {
    $xex = "game/default.xex"
  } else {
    throw "Disc image not found: $Iso (pass -Iso <path to your Tenchu Z disc image>)."
  }
  Invoke-Python tools/recompile.py --xex $xex --generator $generator; Assert-Exit "Translating the game code"
}

# 3. Libraries: FFmpeg (XMA audio and WMV movies; built with MSYS2), SDL3, Dear ImGui.
if (-not (Test-Path "build/deps/ffmpeg/include/libavcodec/avcodec.h")) {
  Write-Host "# FFmpeg"
  Invoke-Python tools/build_xma_codec.py; Assert-Exit "Building FFmpeg"
}
if (-not (Test-Path "build/deps/SDL3/include/SDL3/SDL.h")) { Invoke-Python tools/fetch_sdl.py; Assert-Exit "Fetching SDL3" }
if (-not (Test-Path "build/deps/imgui/imgui.h")) { Invoke-Python tools/fetch_imgui.py; Assert-Exit "Fetching Dear ImGui" }

# 4. Configure and build.
Write-Host "# Building"
if ($Configure -or -not (Test-Path "$BuildDir/build.ninja")) {
  cmake -S . -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link
  Assert-Exit "Configuring"
}
cmake --build $BuildDir --target $Target; Assert-Exit "Building"
Write-Host ""
Write-Host "Built $BuildDir\TenchuZ.exe. Run it and pick your disc image on the first start."
exit 0
