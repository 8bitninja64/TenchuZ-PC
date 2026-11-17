# Shared by the first-start build and the optional setup launcher. Dot-source this
# in the caller's scope so the selected Python and compiler environment survive.
$prerequisiteErrors = @()
$needsCompiler = $false
$needsPython = $false
$bundledCompiler = Join-Path $PSScriptRoot 'zig.zip'
$standaloneRelease = Test-Path -LiteralPath (Join-Path $PSScriptRoot 'bundled-tools.txt')
try {
  if (-not (Test-Path -LiteralPath $bundledCompiler)) {
    if ($standaloneRelease) { throw 'The bundled compiler is missing. Extract the full release ZIP again.' }
    . (Join-Path $PSScriptRoot 'vsenv.ps1')
  }
} catch {
  $needsCompiler = $true
  $prerequisiteErrors += "C++ compiler: $($_.Exception.Message)"
}
try { . (Join-Path $PSScriptRoot 'python.ps1') } catch {
  $needsPython = $true
  $prerequisiteErrors += "Python: $($_.Exception.Message)"
}
