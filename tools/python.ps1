# Finds Python 3.11 or newer (the py launcher, then every python.exe on PATH, including a
# Microsoft Store install) and defines Invoke-Python, which runs it in isolated, UTF-8 mode.
$script:pythonCommand = @()
$candidates = @()
$bundledPython = Join-Path $PSScriptRoot 'python/python.exe'
if (Test-Path -LiteralPath $bundledPython) { $candidates += ,@($bundledPython) }
elseif (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'bundled-tools.txt')) {
  throw 'The bundled Python runtime is missing. Extract the full release ZIP again.'
}
$launcher = Get-Command py.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if ($launcher) { $candidates += ,@($launcher.Source, "-3") }
foreach ($exe in @(Get-Command python.exe -CommandType Application -All -ErrorAction SilentlyContinue)) {
  $candidates += ,@($exe.Source)
}
foreach ($candidate in $candidates) {
  $arguments = @($candidate | Select-Object -Skip 1)
  # The Store's placeholder python.exe fails this probe (exit 9009), so it is skipped.
  # Windows PowerShell turns native stderr into a terminating error when the
  # caller uses Stop. A broken launcher must not prevent trying python.exe.
  $probePreference = $ErrorActionPreference
  $probeSucceeded = $false
  try {
    $ErrorActionPreference = 'Continue'
    & $candidate[0] @arguments -c "import sys; sys.exit(sys.version_info < (3, 11))" 2>$null | Out-Null
    $probeSucceeded = $LASTEXITCODE -eq 0
  } catch {
    # The executable may also have disappeared or be inaccessible.
  } finally {
    $ErrorActionPreference = $probePreference
  }
  if ($probeSucceeded) { $script:pythonCommand = $candidate; break }
  if ($candidate[0] -eq $bundledPython -and (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'bundled-tools.txt'))) {
    throw 'The bundled Python runtime could not start. Extract the full release ZIP again.'
  }
}
if (-not $script:pythonCommand.Count) {
  throw "Python 3.11 or newer is required (https://www.python.org/downloads/; tick 'Add python.exe to PATH')."
}

function Invoke-Python {
  $prefix = @($script:pythonCommand | Select-Object -Skip 1)
  & $script:pythonCommand[0] @prefix -I -X utf8 @args
}
