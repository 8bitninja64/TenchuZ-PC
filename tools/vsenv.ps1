# Imports the Visual Studio x64 developer environment (2022 or newer, Build Tools are
# enough) into the current PowerShell session, with its Clang (clang-cl, lld-link), CMake
# and Ninja on PATH.
$vs = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
  $vs = & $vswhere -latest -version "[17.0,19.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
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
  throw "Visual Studio 2022 or its Build Tools (https://visualstudio.microsoft.com/visual-cpp-build-tools/) with 'Desktop development with C++' was not found."
}
if (-not (Test-Path "$vs\VC\Tools\Llvm\x64\bin\clang-cl.exe")) {
  throw "Visual Studio is missing its Clang compiler: in the Visual Studio Installer, modify $vs and add 'C++ Clang Compiler for Windows' (C++ Clang tools)."
}
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>nul && set" | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
$env:PATH = (Join-Path $vs "VC\Tools\Llvm\x64\bin") + ";" + (Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja") + ";" + (Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin") + ";" + $env:PATH
