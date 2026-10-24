# Finds Python 3.11 or newer (the py launcher, or python.exe on PATH but not the Microsoft
# Store stub) and defines Invoke-Python, which runs it in isolated mode (-I).
$script:pythonCommand = @()
$launcher = Get-Command py.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if ($launcher) {
  $script:pythonCommand = @($launcher.Source, "-3")
} else {
  $exe = Get-Command python.exe -CommandType Application -All -ErrorAction SilentlyContinue |
    Where-Object { $_.Source -notlike "*\WindowsApps\*" } | Select-Object -First 1
  if ($exe) { $script:pythonCommand = @($exe.Source) }
}
if ($script:pythonCommand.Count) {
  $pythonArguments = @($script:pythonCommand | Select-Object -Skip 1)
  & $script:pythonCommand[0] @pythonArguments -c "import sys; sys.exit(sys.version_info < (3, 11))" 2>$null
  if ($LASTEXITCODE) { $script:pythonCommand = @() }
}
if (-not $script:pythonCommand.Count) {
  throw "Python 3.11 or newer is required (https://www.python.org/downloads/; tick 'Add python.exe to PATH')."
}

function Invoke-Python {
  $prefix = @($script:pythonCommand | Select-Object -Skip 1)
  & $script:pythonCommand[0] @prefix -I @args
}
