"""Exercise setup decisions without downloading or installing software (Windows only)."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
WRAPPER = r'''
$ErrorActionPreference = 'Stop'
$fixture = $PSScriptRoot
function Get-ItemProperty { [pscustomobject]@{ KitsRoot10 = "$fixture/sdk" } }
if ($env:TEST_CASE -eq 'existing-sdk') {
  foreach ($file in 'Include/10.0.26100.0/um/Windows.h', 'Include/10.0.26100.0/ucrt/stdio.h',
                    'Lib/10.0.26100.0/um/x64/kernel32.lib', 'Lib/10.0.26100.0/ucrt/x64/ucrt.lib') {
    $path = Join-Path "$fixture/sdk" $file
    New-Item -ItemType Directory -Path (Split-Path $path) -Force | Out-Null
    Set-Content -LiteralPath $path ''
  }
}
if ($env:TEST_CASE -eq 'existing') {
  ${env:ProgramFiles(x86)} = $fixture
  $installerDir = Join-Path $fixture 'Microsoft Visual Studio/Installer'
  New-Item -ItemType Directory -Path $installerDir -Force | Out-Null
  Add-Type -TypeDefinition 'public class Probe { public static void Main() { System.Console.WriteLine(@"C:\Mock Visual Studio"); } }' -OutputAssembly "$installerDir/vswhere.exe" -OutputType ConsoleApplication
}
function Read-Host { if ($env:TEST_CASE -eq 'decline') { 'n' } else { 'y' } }
function Get-Command { [pscustomobject]@{ Source = 'mock-winget' } }
function winget.exe {
  Add-Content "$fixture/actions.txt" "python $args"
  if ($env:TEST_CASE -eq 'python-failure') { $global:LASTEXITCODE = 1; return }
  Set-Content "$fixture/python-ready" ''
  $global:LASTEXITCODE = 0
}
function Test-Path {
  param([string]$LiteralPath)
  if ($LiteralPath -like '*vswhere.exe' -and $env:TEST_CASE -ne 'existing') { return $false }
  Microsoft.PowerShell.Management\Test-Path -LiteralPath $LiteralPath
}
function Invoke-WebRequest {
  param($Uri, $OutFile, [switch]$UseBasicParsing)
  Add-Content "$fixture/actions.txt" "download $Uri"
  Set-Content -LiteralPath $OutFile 'mock installer'
}
function Get-AuthenticodeSignature {
  param($LiteralPath)
  $status = if ($env:TEST_CASE -eq 'bad-signature') { 'NotSigned' } else { 'Valid' }
  [pscustomobject]@{ Status = $status; SignerCertificate = [pscustomobject]@{ Subject = 'CN=Microsoft Corporation, O=Microsoft Corporation, C=US' } }
}
function Start-Process {
  param($FilePath, $ArgumentList, $Verb, $WindowStyle, [switch]$Wait, [switch]$PassThru)
  Add-Content "$fixture/actions.txt" "compiler $ArgumentList"
  if ($env:TEST_CASE -eq 'reboot') { return [pscustomobject]@{ ExitCode = 3010 } }
  if ($env:TEST_CASE -eq 'cancel') { return [pscustomobject]@{ ExitCode = 1602 } }
  Set-Content "$fixture/compiler-ready" ''
  [pscustomobject]@{ ExitCode = 0 }
}
& "$fixture/setup_windows.ps1" -CheckOnly:($env:TEST_CASE -eq 'check')
exit $LASTEXITCODE
'''


@unittest.skipUnless(os.name == 'nt', 'Windows PowerShell setup')
class SetupTests(unittest.TestCase):
    def run_setup(self, case, ready=False):
        with tempfile.TemporaryDirectory(prefix='tenchu-setup-') as tmp:
            folder = Path(tmp)
            for name in ('setup_windows.ps1', 'prerequisites.ps1'):
                shutil.copy2(ROOT / 'tools' / name, folder / name)
            for script, marker in [('vsenv.ps1', 'compiler'), ('python.ps1', 'python')]:
                (folder / script).write_text(
                    f"if (-not (Test-Path -LiteralPath \"$PSScriptRoot/{marker}-ready\")) {{ throw '{marker} missing' }}")
                if ready:
                    (folder / f'{marker}-ready').touch()
            (folder / 'wrapper.ps1').write_text(WRAPPER)
            result = subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                     '-File', str(folder / 'wrapper.ps1')],
                                    env={**os.environ, 'TEST_CASE': case}, capture_output=True, text=True)
            actions = folder / 'actions.txt'
            return result.returncode, result.stdout + result.stderr, actions.read_text() if actions.exists() else ''

    def test_already_ready_does_not_install(self):
        code, output, actions = self.run_setup('ready', ready=True)
        self.assertEqual(code, 0, output)
        self.assertEqual(actions, '')

    def test_check_reports_both_without_installing(self):
        code, output, actions = self.run_setup('check')
        self.assertEqual(code, 1, output)
        self.assertIn('compiler missing', output)
        self.assertIn('python missing', output)
        self.assertEqual(actions, '')

    def test_decline_does_not_install(self):
        code, output, actions = self.run_setup('decline')
        self.assertEqual(code, 1, output)
        self.assertEqual(actions, '')

    def test_install_selects_all_components_and_rechecks(self):
        code, output, actions = self.run_setup('install')
        self.assertEqual(code, 0, output)
        self.assertIn('Setup is ready', output)
        for item in ('Python.Python.3.13', 'VC.Tools.x86.x64', 'VC.Llvm.Clang',
                     'Windows11SDK.22621', '--wait', '--norestart'):
            self.assertIn(item, actions)
        self.assertNotIn('--includeRecommended', actions)

    def test_python_failure_stops(self):
        code, output, actions = self.run_setup('python-failure')
        self.assertEqual(code, 1, output)
        self.assertNotIn('download', actions)

    def test_existing_sdk_is_reused(self):
        code, output, actions = self.run_setup('existing-sdk')
        self.assertEqual(code, 0, output)
        self.assertIn('VC.Llvm.Clang', actions)
        self.assertNotIn('Windows11SDK', actions)

    def test_existing_visual_studio_is_modified_without_download(self):
        code, output, actions = self.run_setup('existing')
        self.assertEqual(code, 0, output)
        self.assertIn('modify --installPath "C:\\Mock Visual Studio"', actions)
        self.assertNotIn('--wait', actions)
        self.assertNotIn('download', actions)

    def test_untrusted_download_never_runs(self):
        code, output, actions = self.run_setup('bad-signature')
        self.assertEqual(code, 1, output)
        self.assertIn('signature verification', output)
        self.assertNotIn('compiler ', actions)

    def test_reboot_is_not_reported_as_ready(self):
        code, output, _ = self.run_setup('reboot')
        self.assertEqual(code, 3010, output)
        self.assertNotIn('Setup is ready', output)

    def test_installer_cancel_is_not_success(self):
        code, output, _ = self.run_setup('cancel')
        self.assertEqual(code, 1, output)
        self.assertNotIn('Setup is ready', output)


if __name__ == '__main__':
    unittest.main()
