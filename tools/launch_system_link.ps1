# Player launcher for experimental System Link multiplayer; never changes system networking.
param([string]$LocalAddress, [string]$HostAddress)
$ErrorActionPreference = 'Stop'
try {
    $root = Split-Path -Parent $PSScriptRoot
    if (-not (Test-Path -LiteralPath "$root/TenchuZ.exe")) { throw 'Run Multiplayer.cmd from the extracted game folder.' }
    Write-Host 'Experimental multiplayer: use the same game version on every PC.'
    Write-Host 'Use your LAN address, or your private VPN address for internet play.'
    if (-not $LocalAddress) { $LocalAddress = Read-Host 'This PC IPv4 address' }
    if (-not $PSBoundParameters.ContainsKey('HostAddress')) { $HostAddress = Read-Host 'Host IPv4 address (leave blank to host)' }
    foreach ($address in @($LocalAddress, $HostAddress)) {
        if (-not $address -and $address -eq $HostAddress) { continue }
        $ip = $null
        if ($address -notmatch '^\d{1,3}(\.\d{1,3}){3}$' -or
            -not [Net.IPAddress]::TryParse($address, [ref]$ip) -or
            $ip.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
            $ip.GetAddressBytes()[0] -eq 0 -or $ip.GetAddressBytes()[0] -ge 224) {
            throw "Invalid unicast IPv4 address: $address"
        }
    }
    if (-not $LocalAddress) { throw 'This PC IPv4 address is required.' }
    $env:TENCHU_SYSTEM_LINK = '1'
    $env:TENCHU_NET_BIND = $LocalAddress
    $env:TENCHU_NET_JOIN = $HostAddress
    $env:TENCHU_NET_NAME = if ($HostAddress) { 'Guest' } else { 'Host' }
    $env:TENCHU_FRAME_RATE = '30'
    $env:TENCHU_NO_UPDATE_CHECK = '1'
    $env:TENCHU_TRACE = 'net,xam'
    Remove-Item Env:TENCHU_CONTROL -ErrorAction SilentlyContinue
    # The game's data folder: user\ beside the executable for portable installs, otherwise %APPDATA%.
    $data = if (Test-Path -LiteralPath "$root/portable.txt") { "$root/user" } else { "$env:APPDATA/TenchuZRecomp" }
    [IO.Directory]::CreateDirectory($data) | Out-Null
    Write-Host "Logs: $data"
    Start-Process -FilePath "$root/TenchuZ.exe" -WorkingDirectory $root -Wait `
        -RedirectStandardError "$data/multiplayer.log" -RedirectStandardOutput "$data/multiplayer-stdout.log"
} catch {
    Write-Host $_ -ForegroundColor Red
    exit 1
}
