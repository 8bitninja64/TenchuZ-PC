# No installs with -CheckOnly. Otherwise ask once, then let the official installers
# install missing prerequisites. Existing working tools are left alone.
param([switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
try {
  . (Join-Path $PSScriptRoot 'prerequisites.ps1')
  if (-not $prerequisiteErrors.Count) {
    Write-Host 'Setup is ready. Run TenchuZ.exe and choose your disc image.'
    exit 0
  }
  Write-Host 'Tenchu Z needs these tools to compile your own disc on first start:'
  foreach ($problem in $prerequisiteErrors) { Write-Host " - $problem" }
  if ($CheckOnly) { exit 1 }
  Write-Host ''
  Write-Host 'Setup will install missing Python and/or Microsoft C++ tools, Clang and Windows SDK.'
  Write-Host 'Downloads and several GB of disk space may be needed. Windows may ask for administrator access.'
  Write-Host 'Existing Visual Studio installations will be extended with the required components.'
  if ((Read-Host 'Install the missing requirements now? [y/N]') -notmatch '^(y|yes)$') { exit 1 }

  if ($needsPython) {
    if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) {
      throw 'Windows App Installer (winget) is unavailable. Install Python 3.11+ from https://www.python.org/downloads/ with Add python.exe to PATH, then run Setup.cmd again.'
    }
    & winget.exe install --id Python.Python.3.13 --exact --source winget --scope user --silent --accept-source-agreements --accept-package-agreements --disable-interactivity
    if ($LASTEXITCODE -ne 0) { throw "Python installation did not finish (exit $LASTEXITCODE). Run Setup.cmd again after resolving the installer error above." }
    # A running process does not inherit the PATH written by an installer.
    $env:PATH = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
  }
  if ($needsCompiler) {
    $installerDir = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
    $vswhere = Join-Path $installerDir 'vswhere.exe'
    $installation = $null
    if (Test-Path -LiteralPath $vswhere) {
      $installation = & $vswhere -latest -version '[17.0,19.0)' -products * -property installationPath
    }
    $components = '--add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.Llvm.Clang'
    # An SDK installed with another VS edition is usable too. Avoid downloading
    # a second multi-GB SDK merely because this installation is missing Clang.
    $sdkRoot = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
    if (-not $sdkRoot) { $sdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10" }
    $sdkReady = $false
    foreach ($version in @(Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Include') -Directory -ErrorAction SilentlyContinue)) {
      if ($version.Name -notmatch '^10\.0\.\d+\.\d+$') { continue }
      $sdkFiles = @("Include/$($version.Name)/um/Windows.h", "Include/$($version.Name)/ucrt/stdio.h",
                    "Lib/$($version.Name)/um/x64/kernel32.lib", "Lib/$($version.Name)/ucrt/x64/ucrt.lib")
      $missingSdkFiles = @($sdkFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $sdkRoot $_)) })
      if (-not $missingSdkFiles.Count) { $sdkReady = $true; break }
    }
    if (-not $sdkReady) { $components += ' --add Microsoft.VisualStudio.Component.Windows11SDK.22621' }
    $download = $null
    try {
      if ($installation) {
        $installer = Join-Path $installerDir 'setup.exe'
        $arguments = "modify --installPath `"$installation`" --passive --norestart $components"
      } else {
        $download = Join-Path ([IO.Path]::GetTempPath()) ('tenchu-vs-' + [guid]::NewGuid().ToString('N') + '.exe')
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Write-Host 'Downloading Microsoft Build Tools...'
        Invoke-WebRequest 'https://aka.ms/vs/17/release/vs_BuildTools.exe' -OutFile $download -UseBasicParsing
        $signature = Get-AuthenticodeSignature -LiteralPath $download
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation(,|$)') {
          throw 'The downloaded Microsoft installer did not pass signature verification.'
        }
        $installer = $download
        $arguments = "--wait --passive --norestart $components"
      }
      $process = Start-Process -FilePath $installer -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -Wait -PassThru
      if ($process.ExitCode -eq 3010) {
        Write-Host 'The installer requires a Windows restart. Restart, then run Setup.cmd again.'
        exit 3010
      }
      if ($process.ExitCode -ne 0) { throw "Microsoft installer exited with $($process.ExitCode). Close other installer windows and run Setup.cmd again." }
    } finally {
      if ($download -and (Test-Path -LiteralPath $download)) { Remove-Item -LiteralPath $download -Force }
    }
  }
  . (Join-Path $PSScriptRoot 'prerequisites.ps1')
  if ($prerequisiteErrors.Count) { throw ($prerequisiteErrors -join "`n") }
  Write-Host 'Setup is ready. Run TenchuZ.exe and choose your disc image.'
} catch {
  Write-Host "Setup could not finish: $($_.Exception.Message)" -ForegroundColor Red
  exit 1
}
