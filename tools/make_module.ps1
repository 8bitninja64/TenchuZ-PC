# Builds only the game module (tenchu_game.dll) from default.xex: translate it with
# XenonRecomp (tools/recompile.py), compile with clang-cl (tools/compile_module.py). The
# game's first start runs this on the player's PC; the runtime (TenchuZ.exe) is not rebuilt.
# Needs Python 3.11+ and Visual Studio 2022 Build Tools with C++ and its Clang compiler.
#   powershell -ExecutionPolicy Bypass -File tools/make_module.ps1 -Xex <default.xex> [-Out <dll>] [-Work <dir>] [-PauseOnError]
param(
  [string]$Xex = "build/xex/default.xex",
  [string]$Out = "build/windows-clang/tenchu_game.dll",
  [string]$Work = "build/module",
  [switch]$PauseOnError
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not [IO.Path]::IsPathRooted($Xex)) { $Xex = Join-Path $root $Xex }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
if (-not [IO.Path]::IsPathRooted($Work)) { $Work = Join-Path $root $Work }
New-Item -ItemType Directory -Force $Work | Out-Null
$log = Join-Path $Work "build.log"
# Native programs' output reaches the transcript (build.log) only through Write-Host; their
# stderr must not become terminating errors.
function Invoke-Logged([scriptblock]$command) {
  $previous = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try { & $command 2>&1 | ForEach-Object { Write-Host "$_" } } finally { $ErrorActionPreference = $previous }
}
Start-Transcript -Path $log -Force | Out-Null
$failed = $false
$started = Get-Date
try {
  Write-Host "Tenchu Z: compiling the game code from your disc. This is done once."
  Write-Host ""
  if (-not (Get-Command clang-cl -ErrorAction SilentlyContinue) -or -not (Get-Command lld-link -ErrorAction SilentlyContinue)) {
    . "$PSScriptRoot\vsenv.ps1"
  }
  . "$PSScriptRoot\python.ps1"

  # Every unit is rewritten; units of another version must not be compiled in.
  $gen = Join-Path $Work "gen"
  if (Test-Path $gen) { Remove-Item -Recurse -Force $gen }
  Write-Host "# Translating the game code"
  Invoke-Logged { Invoke-Python tools/recompile.py --xex $Xex --output $gen --strict }
  if ($LASTEXITCODE) { throw "Translating the game code failed." }

  # simde: the release's third_party copy, or the generator's in a source checkout.
  $simde = @("$root\third_party\simde", "$root\..\_Tools\XenonRecomp\thirdparty\simde") |
    Where-Object { Test-Path "$_\x86\avx.h" } | Select-Object -First 1
  if (-not $simde) { throw "simde headers are missing (third_party\simde)." }
  $obj = Join-Path $Work "obj"
  Invoke-Logged { Invoke-Python tools/compile_module.py --gen $gen --out $Out --obj $obj `
    --include "$root\runtime\guest" --include $simde --extra "$root\runtime\guest\module_rt.cpp" }
  if ($LASTEXITCODE) { throw "Compiling the game code failed." }

  # The sources and objects are only needed again after an update.
  Remove-Item -Recurse -Force $gen, (Join-Path $Work "obj") -ErrorAction SilentlyContinue
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
