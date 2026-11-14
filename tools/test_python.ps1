# Run with Windows PowerShell 5.1 (the game's first-start host):
# powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/test_python.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$testDir = Join-Path $root ('build/python-probe-test-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testDir) | Out-Null
$script:failedProbe = Join-Path $testDir 'broken-launcher.cmd'
[IO.File]::WriteAllText($script:failedProbe, "@echo No installed Python found! 1>&2`r`n@exit /b 1`r`n")
$script:workingPython = Microsoft.PowerShell.Core\Get-Command python.exe -CommandType Application -ErrorAction Stop |
    Select-Object -First 1 -ExpandProperty Source
$script:allBroken = $false

# Control discovery while executing real native processes: stderr from the first
# candidate must not prevent fallback to a working Python under Stop.
function Get-Command {
    [CmdletBinding()]
    param([string]$Name, [string]$CommandType, [switch]$All)
    if ($Name -eq 'py.exe' -or $script:allBroken) {
        [pscustomobject]@{ Source = $script:failedProbe }
    } else {
        [pscustomobject]@{ Source = $script:workingPython }
    }
}

. (Join-Path $PSScriptRoot 'python.ps1')
if ($ErrorActionPreference -ne 'Stop') { throw 'Probe changed the caller error preference.' }
if ($script:pythonCommand[0] -ne $script:workingPython) { throw 'Working Python was not selected.' }
Invoke-Python -c 'import sys; sys.exit(sys.version_info < (3, 11))'
if ($LASTEXITCODE -ne 0) { throw 'Selected Python failed.' }

$script:allBroken = $true
$reportedMissing = $false
try {
    . (Join-Path $PSScriptRoot 'python.ps1')
} catch {
    $reportedMissing = $_.Exception.Message -like 'Python 3.11 or newer is required*'
}
if (-not $reportedMissing) { throw 'Missing Python did not produce the requirements message.' }
if ($ErrorActionPreference -ne 'Stop') { throw 'Failed probes changed the caller error preference.' }
Write-Output 'Python discovery regression tests passed.'
