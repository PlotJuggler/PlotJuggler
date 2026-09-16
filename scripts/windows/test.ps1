# SPDX-License-Identifier: MPL-2.0
# Windows counterpart of test.sh, reached through test.bat: runs the ctest
# suite headless with the runtime DLL search path set up (the Test step of
# windows-ci.yml). Every argument that is not --test-dir/--sanitize goes to
# ctest. Windows PowerShell 5.1 syntax, ASCII only, no param() block.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'PjWindowsCommon.ps1')

$TestDir = ''
$ctestArgs = @()
$i = 0
while ($i -lt $args.Count) {
  $a = [string]$args[$i]
  if ($a -eq '--sanitize' -or $a.StartsWith('--sanitize=')) {
    # test.sh selects a sanitizer build tree and runtime options here; only the
    # `none` lane exists on Windows.
    $lane = ''
    if ($a -eq '--sanitize') {
      $i++
      if ($i -lt $args.Count) { $lane = [string]$args[$i] }
    } else {
      $lane = $a.Substring('--sanitize='.Length)
    }
    if ($lane -ne 'none') {
      Write-Err "--sanitize $lane is Linux-only: sanitizer lanes do not apply to MSVC builds (docs/SANITIZERS.md)."
      exit 2
    }
  }
  elseif ($a -eq '--test-dir' -or $a.StartsWith('--test-dir=')) {
    # Last caller-supplied directory wins, as in test.sh.
    if ($a -eq '--test-dir') {
      $i++
      $TestDir = ''
      if ($i -lt $args.Count) { $TestDir = [string]$args[$i] }
    } else {
      $TestDir = $a.Substring('--test-dir='.Length)
    }
    if ($TestDir -eq '' -or $TestDir.StartsWith('-')) {
      Write-Err '--test-dir needs a directory'
      exit 2
    }
  }
  else { $ctestArgs += $a }
  $i++
}

if ($env:PJ_SANITIZE -and $env:PJ_SANITIZE -ne 'none') {
  Write-Err "PJ_SANITIZE=$($env:PJ_SANITIZE) is Linux-only: sanitizer lanes do not apply to MSVC builds."
  exit 2
}

if ($TestDir -eq '') {
  $TestDir = $PjBuildDir
} elseif (-not [System.IO.Path]::IsPathRooted($TestDir)) {
  $TestDir = [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $TestDir))
}

# Many widget tests create a QApplication without forcing a platform plugin;
# offscreen keeps their windows off the desktop (test.sh does the same).
$env:QT_QPA_PLATFORM = 'offscreen'
# The bin-dir files live in the tree being tested.
Add-PjRuntimeDllDirs $TestDir

$ctestFull = @('--test-dir', $TestDir, '--build-config', $PjConfig, '--output-on-failure', '--no-tests=error', '--timeout', '120') + $ctestArgs
Write-Host ('> ctest ' + (($ctestFull | ForEach-Object { Format-ShellArg $_ }) -join ' '))
& ctest.exe @ctestFull
exit $LASTEXITCODE
