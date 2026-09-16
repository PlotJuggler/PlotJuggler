# SPDX-License-Identifier: MPL-2.0
# Windows counterpart of run.sh, reached through run.bat: launches the dev
# build with the pinned Qt and the Conan runtime DLLs resolvable and forwards
# every argument to the application. Windows PowerShell 5.1 syntax, ASCII only,
# no param() block (see build.ps1 for why).

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'PjWindowsCommon.ps1')

# Ninja Multi-Config puts each configuration's binaries in a subdirectory of
# the target's directory, as in CI's tree.
$Bin = Join-Path $PjBuildDir "pj_app\$PjConfig\plotjuggler4.exe"

# run.sh consumes --apitrace/--heaptrack itself and launches normally when the
# tool is missing; neither tool exists on Windows, so that is the only path.
$appArgs = @()
foreach ($a in $args) {
  if ($a -eq '--apitrace' -or $a -eq '--heaptrack') {
    Write-Err "run.bat: $a is not available on Windows; launching normally."
  } else {
    $appArgs += [string]$a
  }
}

if (-not (Test-Path -LiteralPath $Bin)) {
  Write-Err "$Bin not found; run build.bat first."
  exit 1
}
if (-not (Test-Path -LiteralPath (Join-Path $PjQtDir 'bin\Qt6Core.dll'))) {
  Write-Err "Qt kit not found at $PjQtDir (scripts\windows\install_qt6.ps1 installs it)."
  exit 1
}

Add-PjRuntimeDllDirs $PjBuildDir

# pj_app is a console-subsystem executable, so & waits for it and
# $LASTEXITCODE is its exit code (a WIN32_EXECUTABLE would need Start-Process -Wait).
& $Bin @appArgs
exit $LASTEXITCODE
