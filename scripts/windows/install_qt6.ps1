# SPDX-License-Identifier: MPL-2.0
# Installs the Qt kit pinned in versions.env into .\.qt via aqtinstall: the
# Windows counterpart of scripts/install_qt6.sh (win64_msvc2022_64 instead of
# linux_gcc_64) and of the "Install Qt via aqt" / "Verify Qt cache content"
# steps in .github/workflows/windows-ci.yml. build.ps1 points here when the
# kit is missing and never installs it itself. Windows PowerShell 5.1, ASCII.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'PjWindowsCommon.ps1')

# Released aqt (<=3.3.0) cannot install the pinned Qt on Windows: Qt moved the
# Windows repo to per-arch folders and released aqt still requests the old
# nested path (miurahr/aqtinstall#959 / #1007), failing with "Failed to locate
# XML data for Qt version". The fix (PR #1000) is merged but unreleased, so
# the pin is that merge commit. The ONLY copy of the pin in the scripts; keep
# it identical to the "Install Qt via aqt" step of windows-ci.yml.
# TODO: revert to 'aqtinstall>=3.3.1' once it ships.
$AqtPin = 'git+https://github.com/miurahr/aqtinstall.git@8c3695d4a4e1ceabf6a74dc6c79681656dc6b74b'
$QtOutputDir = Join-Path $PjRepoRoot '.qt'

if (Test-Path -LiteralPath $PjQmake) {
  Write-Host "Qt $PjQtVersion already installed at $PjQtDir"
  Write-Host "set CMAKE_PREFIX_PATH=$PjQtDir"
  exit 0
}

Repair-PjPythonPath

# aqt is invoked as `python -m aqt` (and pip as `python -m pip`) so both
# resolve to the interpreter Repair-PjPythonPath put first, never to a stray
# Scripts dir. The installed aqt is adequate when it can resolve the kit's
# metadata: list-qt --arch performs exactly the Updates.xml lookup that fails
# on a too-old aqt, without downloading anything but that index.
function Test-AqtResolvesKit {
  $archs = & python.exe -m aqt list-qt windows desktop --arch $PjQtVersion
  if ($LASTEXITCODE -ne 0) { return $false }
  return (($archs -join ' ') -match '(^|\s)win64_msvc2022_64(\s|$)')
}

if (-not (Test-AqtResolvesKit)) {
  Write-Host 'Installing aqtinstall (pinned commit, see the comment above)...'
  Invoke-Native python.exe @('-m', 'pip', 'install', $AqtPin)
  if (-not (Test-AqtResolvesKit)) {
    Write-Err "aqt still cannot resolve the Qt $PjQtVersion win64_msvc2022_64 metadata; check network access to the Qt mirrors."
    exit 1
  }
}

# A kit directory without qmake.exe is a partial download; start clean, as CI
# does. (.qt itself is never removed, so a junctioned .qt stays intact.)
if (Test-Path -LiteralPath $PjQtDir) {
  Write-Host "Removing partial kit at $PjQtDir"
  Remove-Item -LiteralPath $PjQtDir -Recurse -Force
}

Write-Host "Installing Qt $PjQtVersion (win64_msvc2022_64) via aqtinstall into $QtOutputDir ..."
# No add-on Qt modules needed (PJ4 uses only desktop-default modules; charts
# is replaced by the vendored Qwt, websockets is unused).
# Retry with backoff: aqt intermittently picks a mirror that is missing the
# metadata checksum ("Failed to download checksum ... Failed to locate XML
# data for Qt version"). It is transient; a retry usually lands on a healthy
# mirror. Same 5 attempts / 15 s * attempt as install_qt6.sh and CI.
$attempt = 0
while ($true) {
  & python.exe -m aqt install-qt windows desktop $PjQtVersion win64_msvc2022_64 --outputdir $QtOutputDir
  if ($LASTEXITCODE -eq 0) { break }
  $attempt++
  if ($attempt -ge 5) {
    Write-Err "aqt failed after $attempt attempts"
    exit 1
  }
  $delay = $attempt * 15
  Write-Host "aqt attempt $attempt failed (transient mirror?); retrying in ${delay}s..."
  Start-Sleep -Seconds $delay
}

if (-not (Test-Path -LiteralPath $PjQmake)) {
  Write-Err "aqt reported success but $PjQmake is missing"
  exit 1
}
Write-Host ''
Write-Host "Qt $PjQtVersion installed at $PjQtDir"
Write-Host ''
Write-Host 'build.bat finds it there. To use it directly, run:'
Write-Host "  set CMAKE_PREFIX_PATH=$PjQtDir"
exit 0
