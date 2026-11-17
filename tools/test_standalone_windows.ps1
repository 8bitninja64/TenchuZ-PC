# Exercise the packaged tools with no installed Python/compiler/SDK on PATH.
# The generated test DLL is original test code, not game code.
param([string]$Package = 'dist/TenchuZ')
$ErrorActionPreference = 'Stop'
$packageRoot = (Resolve-Path -LiteralPath $Package).Path
$root = Split-Path -Parent $PSScriptRoot
$scratch = [IO.Path]::GetFullPath((Join-Path $root ('build/standalone smoke [test]-' + [guid]::NewGuid().ToString('N'))))
[IO.Directory]::CreateDirectory($scratch) | Out-Null
$oldPath = $env:PATH
$oldInclude = $env:INCLUDE
$oldLib = $env:LIB
$oldGlobalCache = $env:ZIG_GLOBAL_CACHE_DIR
$oldLocalCache = $env:ZIG_LOCAL_CACHE_DIR
try {
  $env:PATH = "$env:SystemRoot/System32;$env:SystemRoot/System32/WindowsPowerShell/v1.0"
  $env:INCLUDE = ''
  $env:LIB = ''
  . (Join-Path $packageRoot 'tools/prerequisites.ps1')
  if ($prerequisiteErrors.Count) { throw ($prerequisiteErrors -join "`n") }
  if ($script:pythonCommand[0] -ne (Join-Path $packageRoot 'tools/python/python.exe')) {
    throw 'Setup selected system Python.'
  }
  Invoke-Python -m zipfile -e (Join-Path $packageRoot 'tools/zig.zip') "$scratch/compiler"
  if ($LASTEXITCODE) { throw 'Compiler extraction failed.' }
  $zig = Join-Path $scratch 'compiler/zig-x86_64-windows-0.17.1/zig.exe'
  $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $scratch 'cache'
  $env:ZIG_LOCAL_CACHE_DIR = Join-Path $scratch 'local-cache'
  $gen = Join-Path $scratch 'gen'
  [IO.Directory]::CreateDirectory($gen) | Out-Null
  [IO.File]::WriteAllText("$gen/sources.txt", "probe.cpp`n")
  [IO.File]::WriteAllText("$gen/probe.cpp", @'
#include <string>
extern "C" __declspec(dllexport) int answer() {
    volatile int size = 42;
    return static_cast<int>(std::string(size, 'x').size());
}
'@)
  $dll = Join-Path $scratch 'output/probe.dll'
  Invoke-Python (Join-Path $packageRoot 'tools/compile_module.py') --gen $gen --obj "$scratch/obj" --out $dll --zig $zig
  if ($LASTEXITCODE) { throw 'Standalone compile failed.' }
  Invoke-Python -c 'import ctypes, sys; assert ctypes.CDLL(sys.argv[1]).answer() == 42' $dll
  if ($LASTEXITCODE) { throw 'Standalone DLL could not run.' }
  Write-Host 'Standalone setup passed without system Python, Visual Studio or SDK.'
} finally {
  $env:PATH = $oldPath
  $env:INCLUDE = $oldInclude
  $env:LIB = $oldLib
  $env:ZIG_GLOBAL_CACHE_DIR = $oldGlobalCache
  $env:ZIG_LOCAL_CACHE_DIR = $oldLocalCache
  # Only the uniquely named test directory below this project's build is removed.
  if (-not $scratch.StartsWith((Join-Path $root 'build') + [IO.Path]::DirectorySeparatorChar,
      [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected test directory.' }
  Remove-Item -LiteralPath $scratch -Recurse -Force
}
