# Regression checks that need no disc or copyrighted game data.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
. "$PSScriptRoot/vsenv.ps1"
. "$PSScriptRoot/python.ps1"
$scratch = Join-Path $root ('build/regressions-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($scratch) | Out-Null
function Check-Exit([string]$step) {
  if ($LASTEXITCODE -ne 0) { throw "$step failed (exit $LASTEXITCODE)." }
}
foreach ($test in 'frame_delta', 'animation_timing', 'portable_paths', 'language', 'release_layout', 'resolve_watch', 'network') {
  $sources = @("tools/test_$test.cpp")
  $includes = @('/Iruntime')
  if ($test -in @('language', 'animation_timing')) { $sources += 'runtime/host/settings.cpp' }
  if ($test -in @('resolve_watch', 'network')) {
    $sources += 'runtime/kernel/memory.cpp', 'runtime/kernel/fault.cpp'
    $includes += "/I$root/../_Tools/XenonRecomp-tenchu/thirdparty/simde"
  }
  if ($test -eq 'network') {
    $sources += 'runtime/kernel/imports_net.cpp', 'runtime/kernel/host_registry.cpp', 'ws2_32.lib'
    $includes += '/DWIN32_LEAN_AND_MEAN', '/D_CRT_SECURE_NO_WARNINGS'
  }
  & clang-cl /nologo /EHsc /std:c++20 /DNOMINMAX @includes @sources "/Fe$scratch/test_$test.exe" "/Fo$scratch/"
  Check-Exit "Compile $test"
}
& "$scratch/test_frame_delta.exe"
Check-Exit 'Frame timing'
& "$scratch/test_animation_timing.exe"
Check-Exit 'Animation timing'
& "$scratch/test_resolve_watch.exe"
Check-Exit 'Resolve write watching'
& "$scratch/test_network.exe"
Check-Exit 'Guest networking (requires localhost UDP/TCP)'
& "$scratch/test_network.exe" offline
Check-Exit 'Default offline behavior'
& "$scratch/test_portable_paths.exe" "$scratch/paths"
Check-Exit 'Portable paths'
& "$scratch/test_release_layout.exe" "$scratch/release-layout"
Check-Exit 'Release layout'
Invoke-Python tools/test_build_version.py
Check-Exit 'Cached build version'
foreach ($case in @(@('en', 1), @('FR', 4), @('French', 4), @('de', 3), @('es', 5), @('it', 6), @('invalid', 1))) {
  & "$scratch/test_language.exe" @case
  Check-Exit "Language $($case[0])"
}
Invoke-Python -m unittest discover -s tools -p test_portable.py
Check-Exit 'Portable migration'
Invoke-Python tools/test_setup_windows.py
Check-Exit 'Setup decisions'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/test_python.ps1"
Check-Exit 'Python detection'
Write-Host 'All regression checks passed.'
