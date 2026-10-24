# Finds Python 3.11 or newer (the py launcher, then every python.exe on PATH, including a
# Microsoft Store install) and defines Invoke-Python, which runs it in isolated, UTF-8 mode.
$script:pythonCommand = @()
$candidates = @()
$launcher = Get-Command py.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if ($launcher) { $candidates += ,@($launcher.Source, "-3") }
foreach ($exe in @(Get-Command python.exe -CommandType Application -All -ErrorAction SilentlyContinue)) {
  $candidates += ,@($exe.Source)
}
foreach ($candidate in $candidates) {
  $arguments = @($candidate | Select-Object -Skip 1)
  # The Store's placeholder python.exe fails this probe (exit 9009), so it is skipped.
  & $candidate[0] @arguments -c "import sys; sys.exit(sys.version_info < (3, 11))" 2>$null | Out-Null
  if ($LASTEXITCODE -eq 0) { $script:pythonCommand = $candidate; break }
}
if (-not $script:pythonCommand.Count) {
  throw "Python 3.11 or newer is required (https://www.python.org/downloads/; tick 'Add python.exe to PATH')."
}

function Invoke-Python {
  $prefix = @($script:pythonCommand | Select-Object -Skip 1)
  & $script:pythonCommand[0] @prefix -I -X utf8 @args
}
