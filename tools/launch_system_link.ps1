# Player launcher for the portable multiplayer beta; never changes system networking.
param([string]$LocalAddress, [string]$HostAddress)
$ErrorActionPreference = 'Stop'
try {
    $root = Split-Path -Parent $PSScriptRoot
    if (-not (Test-Path -LiteralPath "$root/portable.txt")) { throw 'Extract the beta into its own folder with portable.txt. See docs/multiplayer-beta.md.' }
    if (-not (Test-Path -LiteralPath "$root/TenchuZ.exe")) { throw 'Run this launcher from the extracted beta package.' }
    Write-Host 'Experimental multiplayer beta: use the same beta version on every PC.'
    Write-Host 'Use your LAN address, or your private VPN address for internet testing.'
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
    [IO.Directory]::CreateDirectory("$root/user") | Out-Null
    Start-Process -FilePath "$root/TenchuZ.exe" -WorkingDirectory $root -Wait `
        -RedirectStandardError "$root/user/multiplayer.log" -RedirectStandardOutput "$root/user/multiplayer-stdout.log"
} catch {
    Write-Host $_ -ForegroundColor Red
    exit 1
}
