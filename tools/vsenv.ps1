# Imports the Visual Studio x64 developer environment (2022 or newer, Build Tools are
# enough) into the current PowerShell session, with its Clang (clang-cl, lld-link), CMake
# and Ninja on PATH.
$vs = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
  # Prefer an instance with Clang; any instance with the C++ tools otherwise (reported below).
  $vs = & $vswhere -latest -version "[17.0,19.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
  if (-not $vs) {
    $vs = & $vswhere -latest -version "[17.0,19.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  }
}
if (-not $vs) {
  foreach ($version in "18", "2022") {
    foreach ($edition in "BuildTools", "Community", "Professional", "Enterprise") {
      foreach ($base in "${env:ProgramFiles(x86)}", "$env:ProgramFiles") {
        $p = "$base\Microsoft Visual Studio\$version\$edition"
        if (Test-Path "$p\VC\Auxiliary\Build\vcvars64.bat") { $vs = $p; break }
      }
      if ($vs) { break }
    }
    if ($vs) { break }
  }
}
if (-not $vs) {
  throw "Visual Studio Build Tools with MSVC x64/x86 tools, C++ Clang Compiler for Windows and a Windows 10/11 SDK was not found. Run Setup.cmd to install them together."
}
if (-not (Test-Path "$vs\VC\Tools\Llvm\x64\bin\clang-cl.exe")) {
  throw "Visual Studio is missing its Clang compiler. Run Setup.cmd, or modify $vs and add 'C++ Clang Compiler for Windows' and a Windows 10/11 SDK."
}
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>nul && set" | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
if ($LASTEXITCODE -ne 0) { throw "Visual Studio could not initialize its C++ environment. Run Setup.cmd to repair the required components." }
$missing = @()
if (-not $env:VCToolsInstallDir -or -not (Test-Path -LiteralPath "$env:VCToolsInstallDir\include\vector")) {
  $missing += 'MSVC C++ headers'
}
if (-not $env:WindowsSdkDir -or -not $env:WindowsSDKVersion -or
    -not (Test-Path -LiteralPath "$env:WindowsSdkDir\Include\$env:WindowsSDKVersion\um\Windows.h") -or
    -not (Test-Path -LiteralPath "$env:WindowsSdkDir\Lib\$env:WindowsSDKVersion\um\x64\kernel32.lib") -or
    -not (Test-Path -LiteralPath "$env:WindowsSdkDir\Lib\$env:WindowsSDKVersion\ucrt\x64\ucrt.lib")) {
  $missing += 'Windows SDK headers and x64 libraries'
}
if (-not (Test-Path -LiteralPath "$vs\VC\Tools\Llvm\x64\bin\lld-link.exe")) { $missing += 'Clang linker (lld-link)' }
if ($missing.Count) { throw ("Missing: " + ($missing -join ', ') + '. Run Setup.cmd to install the required components.') }
$env:PATH = (Join-Path $vs "VC\Tools\Llvm\x64\bin") + ";" + (Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja") + ";" + (Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin") + ";" + $env:PATH
