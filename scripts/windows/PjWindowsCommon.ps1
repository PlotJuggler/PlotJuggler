# SPDX-License-Identifier: MPL-2.0
# Dot-sourced by build.ps1, run.ps1, test.ps1, install_qt6.ps1 and
# build_plugins.ps1: repo locations, the versions.env reader, the Qt kit path,
# the runtime DLL search path, the VS 2022 / Python / Conan environment and the
# native-command helpers. Dot-sourcing runs this in the caller's scope, so the
# $Pj* variables and the functions below become the caller's, and an `exit`
# here ends the calling script with that code.
#
# Windows PowerShell 5.1 syntax only (no &&/||, no ternary, no ??/?.) and
# ASCII only (5.1 reads a BOM-less file as ANSI). Callers set
# $ErrorActionPreference and Set-StrictMode themselves before dot-sourcing.

$PjRepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$PjBuildDir = Join-Path $PjRepoRoot 'build'
# The one configuration every Windows tree builds (windows-ci.yml): the Conan
# graph is resolved for it and every CMakeDeps generator expression is keyed on it.
$PjConfig = 'RelWithDebInfo'

# PlotJuggler's Artifactory Conan remote (anonymous read). It serves
# plotjuggler_sdk to PJ4 and to pj-official-plugins alike; ConanCenter supplies
# everything else.
$PjConanRemoteName = 'plotjuggler-conan'
$PjConanRemoteUrl = 'https://plotjuggler.jfrog.io/artifactory/api/conan/plotjuggler-conan'

function Write-Err([string]$Message) {
  [Console]::Error.WriteLine($Message)
}

# versions.env: KEY=VALUE per line, '#' comments, blank lines, LF or CRLF.
function Read-VersionsEnv([string]$Path) {
  $vars = @{}
  foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
    $t = $line.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { continue }
    $eq = $t.IndexOf('=')
    if ($eq -lt 1) { continue }
    $vars[$t.Substring(0, $eq).Trim()] = $t.Substring($eq + 1).Trim()
  }
  return $vars
}

$PjVersions = Read-VersionsEnv (Join-Path $PjRepoRoot 'versions.env')
foreach ($key in 'PJ_APP_VERSION', 'PJ_QT_VERSION') {
  if (-not $PjVersions[$key]) {
    Write-Err "versions.env: $key is missing"
    exit 1
  }
}
$PjQtVersion = $PjVersions['PJ_QT_VERSION']
# msvc2022_64 is the Qt kit token (gcc_64 on Linux), not a version: it stays
# literal, never derived from versions.env.
$PjQtDir = Join-Path $PjRepoRoot ".qt\$PjQtVersion\msvc2022_64"
# The kit counts as installed only when qmake.exe exists: a directory left by
# an interrupted download would otherwise pass (windows-ci.yml probes the same file).
$PjQmake = Join-Path $PjQtDir 'bin\qmake.exe'

# Display quoting only (dry run, command echo); real calls pass arrays.
function Format-ShellArg([string]$Arg) {
  if ($Arg -match '\s') { return '"' + $Arg + '"' }
  return $Arg
}

# Native commands never throw in 5.1, whatever $ErrorActionPreference says;
# $LASTEXITCODE is the only signal. Their stderr is deliberately not redirected
# at the PowerShell level (2>&1 would wrap every line into an ErrorRecord, and
# Conan logs almost everything to stderr).
function Invoke-Native([string]$Exe, [string[]]$Arguments = @()) {
  Write-Host ('> ' + $Exe + ' ' + (($Arguments | ForEach-Object { Format-ShellArg $_ }) -join ' '))
  & $Exe @Arguments
  if ($LASTEXITCODE -ne 0) {
    Write-Err "$Exe failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
  }
}

# The OS loader resolves Qt6*.dll, the FFmpeg DLLs and python3XY.dll through
# PATH. Conan package paths are hashed, so configure records them in
# <tree>\pj_ffmpeg_bin_dirs.txt and <tree>\pj_python_bin_dirs.txt (one dir per
# line, forward slashes), which CI's Test step prepends the same way.
# QT_PLUGIN_PATH is pinned to the kit so a stale value from another Qt install
# is never scanned first (a mismatched TLS backend there breaks HTTPS, hence
# the marketplace, exactly as run.sh guards against on Linux).
function Add-PjRuntimeDllDirs([string]$TreeDir) {
  $dirs = @((Join-Path $PjQtDir 'bin'))
  foreach ($listName in 'pj_ffmpeg_bin_dirs.txt', 'pj_python_bin_dirs.txt') {
    $listPath = Join-Path $TreeDir $listName
    if (-not (Test-Path -LiteralPath $listPath)) { continue }
    foreach ($line in [System.IO.File]::ReadAllLines($listPath)) {
      $d = $line.Trim()
      if ($d -ne '') { $dirs += $d.Replace('/', '\') }
    }
  }
  $env:PATH = ($dirs -join ';') + ';' + $env:PATH
  $env:QT_PLUGIN_PATH = Join-Path $PjQtDir 'plugins'
}

# The Microsoft Store 'python' stub on PATH breaks the meson-based Conan
# dependencies (dav1d) and hides pip/aqt: put the real interpreter (and its
# Scripts, where conan/ninja/aqt live) first, found through the py launcher.
function Repair-PjPythonPath {
  $python = Get-Command python.exe -ErrorAction SilentlyContinue
  if ($python -and $python.Source -notlike '*\Microsoft\WindowsApps\*') { return }
  if (-not (Get-Command py.exe -ErrorAction SilentlyContinue)) {
    Write-Err "'python' resolves to the Microsoft Store stub (or nothing) and the 'py' launcher is missing."
    Write-Err 'Install Python 3 from python.org (with the launcher), then: pip install "conan>=2,<3" ninja aqtinstall'
    exit 1
  }
  $real = & py.exe -3 -c 'import sys; print(sys.executable)'
  if ($LASTEXITCODE -ne 0 -or -not $real) {
    Write-Err "'py -3' found no Python 3 interpreter."
    exit 1
  }
  $pyDir = Split-Path -Parent ([string]$real).Trim()
  $env:PATH = $pyDir + ';' + (Join-Path $pyDir 'Scripts') + ';' + $env:PATH
  Write-Host "python: Store stub shadowed by $pyDir"
}

# VS 2022 (MSVC 14.4x, cl 19.4x) is the Windows toolchain: PJ4's CI and
# releases and the official plugins' CI all pin it (windows-ci.yml), so Conan
# package IDs and the plugin ABI line up; a VS 2026 (v18) environment is not a
# substitute.
function Test-PjMsvc2022Env {
  if (-not $env:VCToolsVersion) { return $false }
  if ($env:VCToolsVersion -notmatch '^14\.4\d') { return $false }
  if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') { return $false }
  return ($null -ne (Get-Command cl.exe -ErrorAction SilentlyContinue))
}

# Keeps an already active x64 14.4x environment; otherwise imports vcvars64.bat
# of the vswhere-located VS 2022 into this process. Exits 1 when there is none.
function Enable-PjMsvc2022Env {
  if (Test-PjMsvc2022Env) {
    Write-Host "MSVC $env:VCToolsVersion (x64) already active"
    return
  }
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path -LiteralPath $vswhere)) {
    Write-Err "vswhere.exe not found at $vswhere. Install Visual Studio 2022 with the 'Desktop development with C++' workload."
    exit 1
  }
  $cppWorkload = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
  $vsPath = & $vswhere -version '[17.0,18.0)' -products * -requires $cppWorkload -property installationPath | Select-Object -First 1
  if (-not $vsPath) {
    $latest = & $vswhere -latest -products * -requires $cppWorkload -property installationVersion | Select-Object -First 1
    if ($latest -and ([version]$latest).Major -ge 18) {
      Write-Err "Only Visual Studio $latest (v18, MSVC 14.5x) was found. PJ4 Windows builds pin VS 2022 / MSVC 14.4x:"
      Write-Err 'CI, releases and the official plugins are built with it so Conan package IDs and the plugin ABI'
      Write-Err 'stay compatible (see the runs-on comment in .github/workflows/windows-ci.yml). Install VS 2022 with the'
      Write-Err "'Desktop development with C++' workload alongside, or run from a VS 2022 x64 developer prompt."
    } else {
      Write-Err "Visual Studio 2022 with the 'Desktop development with C++' workload was not found (vswhere)."
    }
    exit 1
  }
  $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
  if (-not (Test-Path -LiteralPath $vcvars)) {
    Write-Err "vcvars64.bat not found under $vsPath (is the C++ workload installed?)"
    exit 1
  }
  Write-Host "Importing MSVC environment from $vcvars"
  # /s strips the outer quotes so the quoted path survives; the batch runs in
  # a child cmd and `set` dumps the environment it produced.
  $lines = & cmd.exe /s /c "`"$vcvars`" >nul 2>&1 && set"
  if ($LASTEXITCODE -ne 0 -or -not $lines) {
    Write-Err "vcvars64.bat failed (exit code $LASTEXITCODE)"
    exit 1
  }
  foreach ($line in $lines) {
    $eq = $line.IndexOf('=')
    if ($eq -lt 1) { continue }
    $value = $line.Substring($eq + 1)
    if ($value -eq '') { continue }
    Set-Item -LiteralPath ('Env:' + $line.Substring(0, $eq)) -Value $value
  }
  if (-not (Test-PjMsvc2022Env)) {
    Write-Err "vcvars64.bat ran, but no x64 MSVC 14.4x toolset is active (VCToolsVersion='$env:VCToolsVersion')."
    exit 1
  }
}

function Assert-PjBuildTools {
  foreach ($tool in 'conan.exe', 'cmake.exe', 'ninja.exe') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
      Write-Err "$tool is not on PATH. pip install `"conan>=2,<3`" `"ninja!=1.13.0`" (and CMake 3.25+ from cmake.org)."
      exit 1
    }
  }
  # Ninja 1.13.0 writes response files with bare LF line endings, which
  # link.exe misreads once a line passes 16,383 characters: it drops that
  # line's leading characters and fails with LNK1181 on a truncated object
  # path. Any target with a long library list hits it (ninja-build/ninja#2616);
  # 1.13.1 writes CRLF again and earlier releases never wrote LF.
  $ninja = (Get-Command ninja.exe).Source
  $ninjaVersion = ([string](& $ninja --version | Select-Object -First 1)).Trim()
  if ($ninjaVersion -match '^1\.13\.0(\D|$)') {
    Write-Err "ninja $ninjaVersion ($ninja) corrupts MSVC link response files: link.exe fails with LNK1181"
    Write-Err 'on a truncated object path once a link line passes 16,383 characters (ninja-build/ninja#2616).'
    Write-Err 'Upgrade it (1.13.1 and newer are fixed):  py -3 -m pip install --upgrade ninja'
    exit 1
  }
}

# Adds the PlotJuggler remote when missing and enables it; idempotent, like
# scripts/configure_conan_remote.sh.
function Register-PjConanRemote {
  $remotes = & conan.exe remote list
  if ($LASTEXITCODE -ne 0) {
    Write-Err "conan remote list failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
  }
  if (-not ($remotes | Where-Object { $_ -match "^${PjConanRemoteName}:" })) {
    Invoke-Native conan.exe @('remote', 'add', $PjConanRemoteName, $PjConanRemoteUrl)
  }
  Invoke-Native conan.exe @('remote', 'enable', $PjConanRemoteName)
}
