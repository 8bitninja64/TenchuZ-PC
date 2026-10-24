# Makes the release: the runtime (TenchuZ.exe and its DLLs) plus the kit the first start
# compiles the game module with (the generator, scripts, configuration, headers). It never
# contains game code or data: the player's own disc provides those.
#   powershell -ExecutionPolicy Bypass -File tools/package.ps1 [-BuildDir build/windows-clang] [-Out dist/TenchuZ] [-Zip]
param(
  [string]$BuildDir = "build/windows-clang",
  [string]$Out = "dist/TenchuZ",
  [switch]$Zip
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
if (-not (Test-Path "$BuildDir/TenchuZ.exe")) { throw "Build the runtime first: tools/build.ps1 (-SkipRecomp is enough)." }
$tools = (Resolve-Path "..\_Tools\XenonRecomp").Path
$generator = Join-Path $tools "build-tenchu\XenonRecomp\XenonRecomp.exe"
if (-not (Test-Path $generator)) { throw "The generator is missing: run tools/build.ps1 first." }

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
function Copy-To([string]$destination, [string[]]$sources) {
  New-Item -ItemType Directory -Force (Join-Path $Out $destination) | Out-Null
  foreach ($source in $sources) { Copy-Item -Force $source (Join-Path $Out $destination) }
}

# The runtime and the libraries it loads (the game module is built on the player's PC).
Copy-To "." (@("$BuildDir/TenchuZ.exe", "assets/gamecontrollerdb.txt", "README.md", "COPYING") +
  @(Get-ChildItem "$BuildDir/*.dll" | Where-Object { $_.Name -ne "tenchu_game.dll" } | ForEach-Object FullName))
# The Visual C++ runtime, app-local, for PCs without the redistributable.
. "$PSScriptRoot\vsenv.ps1"
$redist = Get-ChildItem "$env:VCToolsRedistDir\x64\Microsoft.VC*.CRT" -Directory | Select-Object -First 1
Copy-To "." @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll" | ForEach-Object { Join-Path $redist.FullName $_ })

# The kit: generator, scripts and export tables.
Copy-To "tools" @($generator, "tools/recompile.py", "tools/xex_image.py", "tools/compile_module.py",
  "tools/make_module.ps1", "tools/vsenv.ps1", "tools/python.ps1", "tools/setup_generator.py")
Copy-To "tools/xbox" @("$tools/XenonUtils/xbox/xboxkrnl_table.inc", "$tools/XenonUtils/xbox/xam_table.inc")
# The generator's corresponding source: the pinned upstream commit (tools/setup_generator.py) and these patches.
Copy-To "tools/patches" @(Get-ChildItem "tools/patches/*" | ForEach-Object FullName)
Copy-To "config" @("config/tenchuz.toml", "config/tenchuz_switch_tables.toml",
  "config/tenchuz_switch_tables_manual.toml", "config/overrides.txt")
Copy-To "runtime/guest" @("runtime/guest/ppc_context.template.h", "runtime/guest/ppc_config.h",
  "runtime/guest/module_abi.h", "runtime/guest/module_rt.cpp")
$simde = Join-Path $tools "thirdparty\simde"
Copy-To "third_party/simde" (@(Get-ChildItem "$simde/*.h" | ForEach-Object FullName) + @("$simde/COPYING"))
Copy-To "third_party/simde/x86" @(Get-ChildItem "$simde/x86/*.h" | ForEach-Object FullName)

# Licenses of the bundled components.
$licenses = "licenses"
Copy-To "$licenses/ffmpeg" @(Get-ChildItem "build/deps/ffmpeg/*" -File -Include "COPYING*", "LICENSE*", "PROVENANCE*" | ForEach-Object FullName)
# FFmpeg is modified (the XMA decoder option): the script holds the patch, the pinned source and the build.
Copy-To "$licenses/ffmpeg" @("tools/build_xma_codec.py")
Copy-To "$licenses/sdl3" @(Get-ChildItem "build/deps/SDL3/*" -File -Include "LICENSE*" | ForEach-Object FullName)
Copy-To "$licenses/imgui" @("build/deps/imgui/LICENSE.txt")
# XenonRecomp.exe: MIT, plus the components linked into it; the patch series (GPLv3, from The
# Darkness Recomp) and binutils' PowerPC disassembler (GPL-2.0-or-later) make the binary GPL.
# Its corresponding source: tools/setup_generator.py (pinned upstream commit) and tools/patches.
Copy-To "$licenses/xenonrecomp" @("$tools/LICENSE.md")
Copy-Item -Force "$tools/thirdparty/xxHash/LICENSE" "$Out/$licenses/xenonrecomp/xxHash-LICENSE"
Copy-Item -Force "$tools/thirdparty/tomlplusplus/LICENSE" "$Out/$licenses/xenonrecomp/tomlplusplus-LICENSE"
Copy-Item -Force "$tools/thirdparty/fmt/LICENSE" "$Out/$licenses/xenonrecomp/fmt-LICENSE"
Copy-Item -Force "$tools/thirdparty/libmspack/libmspack/COPYING.LIB" "$Out/$licenses/xenonrecomp/libmspack-COPYING.LIB"
Get-Content -LiteralPath "$tools/thirdparty/TinySHA1/TinySHA1.hpp" -TotalCount 30 |
  Set-Content -LiteralPath "$Out/$licenses/xenonrecomp/TinySHA1-NOTICE.txt" -Encoding ascii
Set-Content -LiteralPath "$Out/$licenses/xenonrecomp/README.txt" -Encoding ascii -Value @(
  "tools/XenonRecomp.exe is hedge-dev's XenonRecomp (MIT, LICENSE.md) at the commit pinned in",
  "tools/setup_generator.py with the patches in tools/patches (GPLv3, from The Darkness Recomp),",
  "built with MSVC. It links binutils' PowerPC disassembler (GPL-2.0-or-later; see COPYING at the",
  "top of this release), xxHash (BSD-2-Clause), toml++ (MIT), fmt (MIT), libmspack (LGPL-2.1)",
  "and TinySHA1 (ISC-style notice). Source: https://github.com/hedge-dev/XenonRecomp plus the",
  "patches; tools/setup_generator.py reproduces the tree.")
Copy-To "$licenses/simde" @("$simde/COPYING")

# Never ship anything derived from the game.
$forbidden = '^(tenchu_game\.(dll|lib|exp|pdb)|ppc_recomp\.\d+\.cpp|ppc_func_mapping\.cpp|ppc_module\.cpp|ppc_recomp_shared\.h|default\.xex|TenchuZ\.ini)$'
$bad = Get-ChildItem -Recurse -File $Out | Where-Object { $_.Name -match $forbidden -or $_.Extension -in ".iso", ".xex", ".obj", ".pdb" }
if ($bad) { throw "Refusing to package game-derived files: $($bad.FullName -join ', ')" }

$size = (Get-ChildItem -Recurse -File $Out | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("Packaged {0} ({1:N1} MB)" -f $Out, $size)
if ($Zip) {
  $archive = "dist/TenchuZ-windows-x64.zip"
  if (Test-Path $archive) { Remove-Item -Force $archive }
  Compress-Archive -Path "$Out\*" -DestinationPath $archive
  Write-Host ("Wrote {0} ({1:N1} MB)" -f $archive, ((Get-Item $archive).Length / 1MB))
}
