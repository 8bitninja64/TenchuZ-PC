# Builds only the game module (tenchu_game.dll) from default.xex: translate it with
# XenonRecomp (tools/recompile.py), compile with tools/compile_module.py. The
# game's first start runs this on the player's PC; the runtime (TenchuZ.exe) is not rebuilt.
# Releases carry Python and Zig; source checkouts can use Python and Visual Studio.
#   powershell -ExecutionPolicy Bypass -File tools/make_module.ps1 -Xex <default.xex> [-Out <dll>] [-Work <dir>] [-PauseOnError]
param(
  [string]$Xex = "build/xex/default.xex",
  [string]$Out = "build/windows-clang/tenchu_game.dll",
  [string]$Work = "build/module",
  [switch]$PauseOnError
)
$ErrorActionPreference = "Stop"
# Paths are literal throughout: [ and ] in a folder name are wildcards to PowerShell.
$root = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($Xex)) { $Xex = Join-Path $root $Xex }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
if (-not [IO.Path]::IsPathRooted($Work)) { $Work = Join-Path $root $Work }
[IO.Directory]::CreateDirectory($Work) | Out-Null
$log = Join-Path $Work "build.log"
# Native programs' output reaches the transcript (build.log) only through Write-Host; their
# stderr must not become terminating errors.
function Invoke-Logged([scriptblock]$command) {
  $previous = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try { & $command 2>&1 | ForEach-Object { Write-Host "$_" } } finally { $ErrorActionPreference = $previous }
}
Start-Transcript -LiteralPath $log -Force | Out-Null
$failed = $false
$started = Get-Date
try {
  Write-Host "Tenchu Z: compiling the game code from your disc. This is done once."
  Write-Host ""
  Set-Location -LiteralPath $root
  # Release tools take priority over developer tools installed on the machine.
  . (Join-Path $PSScriptRoot "prerequisites.ps1")
  if ($prerequisiteErrors.Count) {
    if ($standaloneRelease) { throw ($prerequisiteErrors -join "`n") }
    throw (($prerequisiteErrors -join "`n") + "`nRun Setup.cmd in the game folder to install the missing requirements, then start the game again.")
  }
  $compilerArgs = @()
  $compilerWork = Join-Path $Work 'compiler'
  if (Test-Path -LiteralPath $bundledCompiler) {
    Write-Host '# Preparing the included compiler'
    Invoke-Logged { Invoke-Python -m zipfile -e $bundledCompiler $compilerWork }
    if ($LASTEXITCODE) { throw 'Could not unpack the included compiler. Extract the release ZIP again.' }
    $zig = Join-Path $compilerWork 'zig-x86_64-windows-0.17.1/zig.exe'
    if (-not (Test-Path -LiteralPath $zig)) { throw 'The included compiler is incomplete.' }
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $compilerWork 'cache'
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $compilerWork 'local-cache'
    $compilerArgs = @('--zig', $zig)
  }

  # Every unit is rewritten; units of another version must not be compiled in.
  $gen = Join-Path $Work "gen"
  if (Test-Path -LiteralPath $gen) { Remove-Item -LiteralPath $gen -Recurse -Force }
  Write-Host "# Translating the game code"
  Invoke-Logged { Invoke-Python tools/recompile.py --xex $Xex --output $gen --strict }
  if ($LASTEXITCODE) { throw "Translating the game code failed." }

  # simde: the release's third_party copy, or the generator's in a source checkout.
  $simde = @("$root\third_party\simde", "$root\..\_Tools\XenonRecomp\thirdparty\simde") |
    Where-Object { Test-Path -LiteralPath (Join-Path $_ "x86\avx.h") } | Select-Object -First 1
  if (-not $simde) { throw "simde headers are missing (third_party\simde)." }
  $obj = Join-Path $Work "obj"
  Invoke-Logged { Invoke-Python tools/compile_module.py --gen $gen --out $Out --obj $obj `
    --include "$root\runtime\guest" --include $simde --extra "$root\runtime\guest\module_rt.cpp" @compilerArgs }
  if ($LASTEXITCODE) { throw "Compiling the game code failed." }

  # The sources and objects are only needed again after an update.
  Remove-Item -LiteralPath $gen, $obj -Recurse -Force -ErrorAction SilentlyContinue
  if (Test-Path -LiteralPath $compilerWork) {
    if ([IO.Path]::GetFullPath($compilerWork) -ne (Join-Path ([IO.Path]::GetFullPath($Work)) 'compiler')) {
      throw 'Unexpected compiler work directory.'
    }
    Remove-Item -LiteralPath $compilerWork -Recurse -Force
  }
  Write-Host ("# Done in {0:N0} s: {1}" -f ((Get-Date) - $started).TotalSeconds, $Out)
} catch {
  Write-Host ""
  Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
  Write-Host "The log is in $log"
  $failed = $true
} finally {
  Stop-Transcript | Out-Null
}
if ($failed) {
  if ($PauseOnError) { Read-Host "Press Enter to close" | Out-Null }
  exit 1
}
exit 0
