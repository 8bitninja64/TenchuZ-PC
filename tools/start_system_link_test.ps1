# Isolated developer test copies; never writes to the supplied save directory.
param(
    [string]$Runtime = 'build/windows-clang',
    [string]$SaveDirectory,
    [switch]$Visible,
    [switch]$ControllerInput,
    [string]$Trace = 'net,xam',
    [ValidateSet('both','all','host','guest','guest2','guest3')][string]$Side = 'both',
    [string]$HostAddress = '127.0.0.1',
    [string]$GuestAddress = '127.0.0.2',
    [string]$Guest2Address = '127.0.0.3',
    [string]$Guest3Address = '127.0.0.4'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$runtimePath = (Resolve-Path -LiteralPath (Join-Path $root $Runtime)).Path
$testRoot = Join-Path $root 'build/multiplayer'
$module = Join-Path $runtimePath 'tenchu_game.dll'
if (-not (Test-Path -LiteralPath $module)) { throw 'Build the game module first.' }
$names = @('TENCHU_SYSTEM_LINK','TENCHU_NET_BIND','TENCHU_NET_JOIN','TENCHU_NET_NAME',
    'TENCHU_MODULE','TENCHU_CONTROL','TENCHU_TRACE','TENCHU_FRAME_RATE','TENCHU_SKIP_MOVIES','TENCHU_NO_UPDATE_CHECK')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
try {
    $sides = if ($Side -eq 'all') { @('host','guest','guest2','guest3') } elseif ($Side -eq 'both') { @('host','guest') } else { @($Side) }
    # Check all requested copies before starting any of them.
    foreach ($testSide in $sides) {
        $exe = Join-Path $testRoot "$testSide/TenchuZ.exe"
        if (Get-Process TenchuZ -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) {
            throw "Test instance is already running: $exe"
        }
    }
    foreach ($testSide in $sides) {
        $folder = Join-Path $testRoot $testSide
        [IO.Directory]::CreateDirectory($folder) | Out-Null
        $exe = Join-Path $folder 'TenchuZ.exe'
        if (Get-Process TenchuZ -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) {
            throw "Test instance is already running: $folder"
        }
        Copy-Item -LiteralPath (Join-Path $runtimePath 'TenchuZ.exe') -Destination $exe
        Get-ChildItem -LiteralPath $runtimePath -Filter '*.dll' | Where-Object { $_.Name -ne 'tenchu_game.dll' } |
            Copy-Item -Destination $folder
        [IO.File]::WriteAllText((Join-Path $folder 'portable.txt'), '')
        if ($SaveDirectory -and -not (Test-Path -LiteralPath (Join-Path $folder 'user'))) {
            Copy-Item -LiteralPath (Resolve-Path -LiteralPath $SaveDirectory).Path -Destination (Join-Path $folder 'user') -Recurse
        }
        [IO.File]::WriteAllText((Join-Path $folder 'TenchuZ.ini'), @"
[game]
update_check=false
language=en
[display]
fullscreen=false
window_width=960
window_height=540
resolution_scale=1
frame_rate_limit=30
vsync=false
hdr=false
[graphics]
msaa=1
ambient_occlusion=false
"@)
        $control = Join-Path $folder 'control.txt'
        [IO.File]::WriteAllText($control, '')
        $env:TENCHU_SYSTEM_LINK='1'
        $env:TENCHU_NET_BIND=switch ($testSide) { host { $HostAddress } guest { $GuestAddress } guest2 { $Guest2Address } guest3 { $Guest3Address } }
        $env:TENCHU_NET_JOIN=if ($testSide -ne 'host') { $HostAddress } else { '' }
        $env:TENCHU_NET_NAME=$testSide
        $env:TENCHU_MODULE=$module
        $env:TENCHU_CONTROL=if ($ControllerInput) { '' } else { $control }
        $env:TENCHU_TRACE=$Trace
        $env:TENCHU_FRAME_RATE='30'
        $env:TENCHU_SKIP_MOVIES='1'
        $env:TENCHU_NO_UPDATE_CHECK='1'
        $style=if ($Visible) { 'Normal' } else { 'Hidden' }
        $process = Start-Process -FilePath $exe -ArgumentList ('"' + (Join-Path $root 'game') + '"') -WorkingDirectory $folder -WindowStyle $style -PassThru -RedirectStandardError (Join-Path $folder 'run.log') -RedirectStandardOutput (Join-Path $folder 'stdout.log')
        [pscustomobject]@{ Side=$testSide; PID=$process.Id; Control=$control; Log=(Join-Path $folder 'run.log') }
    }
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
}
