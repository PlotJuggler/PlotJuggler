# SPDX-License-Identifier: MPL-2.0
# Windows counterpart of build.sh, reached through build.bat. Same options and
# PJ_* variables, same build\ tree; the toolchain and the Conan/CMake
# invocations mirror .github/workflows/windows-ci.yml (VS 2022 MSVC 14.4x,
# Ninja Multi-Config, RelWithDebInfo), so a local tree is interchangeable with
# what CI builds. Shared helpers live in PjWindowsCommon.ps1.
#
# Windows PowerShell 5.1 only, ASCII only. No param() block: `powershell -File`
# hands every argument to $args verbatim, so --opt=value and --opt VALUE both
# arrive intact.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'PjWindowsCommon.ps1')

function Show-Usage([bool]$ToStderr) {
  $text = @'
Usage: build.bat [OPTIONS]

Options:
  --minimal            App closure only: same as --no-tests --no-demos --target pj_app
  --no-tests           Build without the test suite (also turns off the widget
                       demos and the standalone marketplace app, which follow it)
  --no-demos           Skip the scene3D demos and benchmarks
  --no-widget-demos    Skip the pj_widgets demo apps
  --no-marketplace-app Skip the standalone marketplace executable
  --target NAME        CMake target to build (default: all; pj_app = app + plugin checker)
  --skip-conan-install Reuse the existing Conan toolchain in the build directory
  --sdk-local[=PATH]   Build plotjuggler_sdk from a local tree instead of the
                       Conan package (default PATH: ..\plotjuggler_sdk sibling).
                       Dev-only: not reproducible, refused in CI.
  --help               Show this help message

Linux-only, rejected here: --sanitize, --tsan, --debug-info, --no-compress-debug
(MSVC builds are always RelWithDebInfo with /Z7 embedded debug info).

Examples:
  build.bat                     full dev tree: tests + demos
  build.bat --minimal           what the packaging build compiles, on the host
  build.bat --no-tests --target pj_app

The same settings are accepted as PJ_BUILD_TESTS, PJ_BUILD_DEMOS,
PJ_BUILD_WIDGET_DEMOS, PJ_BUILD_MARKETPLACE_APP and PJ_BUILD_TARGET environment
variables; an option always wins over the variable. PJ_VERSION overrides the
app version and PJ_INSTALLATION the packaging channel (default: source).
PJ_DRY_RUN=1 prints the build directory, target, Conan and CMake command lines
without building.

Toolchain: Visual Studio 2022 (MSVC 14.4x) is located with vswhere and its
vcvars64.bat environment imported unless one is already active. Qt is expected
at .qt\<PJ_QT_VERSION>\msvc2022_64 (scripts\windows\install_qt6.ps1 installs it).
Like CI, every Conan install first seeds Conan's source download cache from
.github\conan\source-backup and records it in global.conf.
'@
  if ($ToStderr) { Write-Err $text } else { Write-Host $text }
}

function Exit-Usage([string]$Message) {
  Write-Err $Message
  Show-Usage $true
  exit 2
}

function ConvertTo-CMakePath([string]$Path) {
  return $Path.Replace('\', '/')
}

# ---------------------------------------------------------------------------
# Options. They set the same variables the environment can provide, so an
# option always wins and everything below reads one source. The last
# repetition of a valid option wins, as in build.sh.
# ---------------------------------------------------------------------------
$SkipTest = $false
$SkipConanInstall = $false
$SdkLocalDir = ''
$PJ_BUILD_TESTS = $env:PJ_BUILD_TESTS
$PJ_BUILD_DEMOS = $env:PJ_BUILD_DEMOS
$PJ_BUILD_WIDGET_DEMOS = $env:PJ_BUILD_WIDGET_DEMOS
$PJ_BUILD_MARKETPLACE_APP = $env:PJ_BUILD_MARKETPLACE_APP
$PJ_BUILD_TARGET = $env:PJ_BUILD_TARGET

$i = 0
while ($i -lt $args.Count) {
  $a = [string]$args[$i]
  if ($a -eq '--sanitize' -or $a.StartsWith('--sanitize=') -or $a -eq '--tsan' -or
      $a -eq '--debug-info' -or $a.StartsWith('--debug-info=') -or $a -eq '--no-compress-debug') {
    Exit-Usage "$a is Linux-only: sanitizer lanes and the DWARF debug-info policy do not apply to MSVC builds (docs/SANITIZERS.md, cmake/PjDebugInfo.cmake)."
  }
  elseif ($a -eq '--no-tests' -or $a -eq '--skip-test') { $SkipTest = $true }
  elseif ($a -eq '--no-demos') { $PJ_BUILD_DEMOS = 'OFF' }
  elseif ($a -eq '--no-widget-demos') { $PJ_BUILD_WIDGET_DEMOS = 'OFF' }
  elseif ($a -eq '--no-marketplace-app') { $PJ_BUILD_MARKETPLACE_APP = 'OFF' }
  elseif ($a -eq '--target' -or $a.StartsWith('--target=')) {
    if ($a -eq '--target') {
      $i++
      $value = ''
      if ($i -lt $args.Count) { $value = [string]$args[$i] }
    } else {
      $value = $a.Substring('--target='.Length)
    }
    if ($value -eq '' -or $value.StartsWith('-')) { Exit-Usage '--target needs a target name' }
    $PJ_BUILD_TARGET = $value
  }
  elseif ($a -eq '--minimal') {
    $SkipTest = $true
    $PJ_BUILD_DEMOS = 'OFF'
    $PJ_BUILD_TARGET = 'pj_app'
  }
  elseif ($a -eq '--skip-conan-install') { $SkipConanInstall = $true }
  elseif ($a -eq '--sdk-local' -or $a.StartsWith('--sdk-local=')) {
    if ($a -eq '--sdk-local') {
      $value = Join-Path $PjRepoRoot '..\plotjuggler_sdk'
      if (-not (Test-Path -LiteralPath $value -PathType Container)) {
        $value = Join-Path $env:USERPROFILE 'ws_plotjuggler\plotjuggler_sdk'
      }
    } else {
      $value = $a.Substring('--sdk-local='.Length)
      if ($value -eq '') { Exit-Usage '--sdk-local= needs a path' }
    }
    # .NET's notion of the current directory can differ from $PWD: root
    # relative paths here, then let GetFullPath collapse the '..' segments.
    if (-not [System.IO.Path]::IsPathRooted($value)) { $value = Join-Path (Get-Location).Path $value }
    $SdkLocalDir = [System.IO.Path]::GetFullPath($value)
  }
  elseif ($a -eq '-h' -or $a -eq '--help') {
    Show-Usage $false
    exit 0
  }
  else { Exit-Usage "unknown argument: $a" }
  $i++
}

# The environment can carry a lane too (CI, wrappers); anything but none is
# Linux-only and must not silently build an uninstrumented tree.
if ($env:PJ_SANITIZE -and $env:PJ_SANITIZE -ne 'none') {
  Write-Err "PJ_SANITIZE=$($env:PJ_SANITIZE) is Linux-only: sanitizer lanes do not apply to MSVC builds."
  exit 2
}

if ($SdkLocalDir -ne '') {
  # Existence, not content: the root CMakeLists refuses PJ_SDK_LOCAL_DIR under
  # if(DEFINED ENV{CI}), so a defined CI must fail here, early, too.
  if (Test-Path Env:CI) {
    Write-Err '--sdk-local is a local development mode; refusing to run in CI'
    exit 2
  }
  if (-not (Test-Path -LiteralPath (Join-Path $SdkLocalDir 'pj_base\CMakeLists.txt'))) {
    Write-Err "--sdk-local: no plotjuggler_sdk tree at $SdkLocalDir"
    exit 2
  }
  Write-Host '=================================================================='
  Write-Host " plotjuggler_sdk from LOCAL TREE: $SdkLocalDir"
  Write-Host ' NOT reproducible - do not use for release artifacts'
  Write-Host '=================================================================='
}

# ---------------------------------------------------------------------------
# Build selection. Every selection is passed explicitly on every configure:
# CMake's option() keeps the cached value, so an unset -D would let a tree
# configured once with tests ON keep its widget demos and marketplace app
# after --no-tests, and a --minimal tree keep tests OFF after a plain build.
# ---------------------------------------------------------------------------
if ($SkipTest) {
  $PJ_BUILD_TESTS = 'OFF'
  # Documented as following --no-tests; an explicit --no-widget-demos or
  # --no-marketplace-app can only turn them off, never back on.
  $PJ_BUILD_WIDGET_DEMOS = 'OFF'
  $PJ_BUILD_MARKETPLACE_APP = 'OFF'
}
if (-not $PJ_BUILD_TESTS) { $PJ_BUILD_TESTS = 'ON' }
if (-not $PJ_BUILD_DEMOS) { $PJ_BUILD_DEMOS = 'ON' }
if (-not $PJ_BUILD_WIDGET_DEMOS) { $PJ_BUILD_WIDGET_DEMOS = $PJ_BUILD_TESTS }
if (-not $PJ_BUILD_MARKETPLACE_APP) { $PJ_BUILD_MARKETPLACE_APP = $PJ_BUILD_TESTS }
if (-not $PJ_BUILD_TARGET) { $PJ_BUILD_TARGET = 'all' }

$PjVersion = $PjVersions['PJ_APP_VERSION']
if ($env:PJ_VERSION) { $PjVersion = $env:PJ_VERSION }
$PjInstallation = 'source'
if ($env:PJ_INSTALLATION) { $PjInstallation = $env:PJ_INSTALLATION }

$Toolchain = Join-Path $PjBuildDir 'conan_toolchain.cmake'

# ---------------------------------------------------------------------------
# Conan install command line (windows-ci.yml "Conan install").
# Every setting `conan profile detect` emits for MSVC is pinned here, with
# compiler.version forced to 194: detect reports the newest VS on the box (195
# once VS 2026 is installed, even inside a VS 2022 prompt), a user's profile
# could carry anything, and the package IDs must match CI's VS 2022 graph.
# compiler.runtime_type is pinned too: detect leaves it unset and Conan's
# profile plugin derives Release from a non-Debug build_type (what CI gets),
# but a user profile could set it to Debug.
# cpython: its recipe has no static MSVC build (>=3.10) and its MSVC build has
# no RelWithDebInfo configuration, only Debug/Release; Release is
# CRT-compatible with the RelWithDebInfo graph (both /MD).
# generator: the toolchain must be stamped for Ninja Multi-Config. Conan's
# default picks Visual Studio and adds CMAKE_GENERATOR_PLATFORM=x64, which the
# Ninja configure below rejects.
# ---------------------------------------------------------------------------
$conanArgs = @('install', $PjRepoRoot, "--output-folder=$PjBuildDir", '--build=missing', '-r', $PjConanRemoteName, '-r', 'conancenter')
# Committed lockfile pins every recipe revision so local and CI builds resolve
# the exact graph JFrog holds binaries for; --lockfile-partial keeps
# platform-only additions resolvable.
$lockfile = Join-Path $PjRepoRoot 'conan.lock'
if (Test-Path -LiteralPath $lockfile) { $conanArgs += "--lockfile=$lockfile", '--lockfile-partial' }
$conanArgs += @(
  '-s', 'arch=x86_64',
  '-s', 'os=Windows',
  '-s', 'compiler=msvc',
  '-s', 'compiler.version=194',
  '-s', 'compiler.runtime=dynamic',
  '-s', 'compiler.runtime_type=Release',
  '-s', 'compiler.cppstd=20',
  '-s', "build_type=$PjConfig",
  '-o', 'cpython/*:shared=True',
  '-s', 'cpython/*:build_type=Release',
  '-c', 'tools.cmake.cmaketoolchain:generator=Ninja Multi-Config'
)

# ---------------------------------------------------------------------------
# CMake configure command line (windows-ci.yml "Build"). No CMAKE_BUILD_TYPE:
# Ninja Multi-Config selects the configuration at build time (--config).
# CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded forces /Z7 over /Zi: /Zi writes
# a shared PDB through mspdbsrv.exe, which parallel cl.exe workers race on
# (fatal C1041) and sccache cannot cache. CMake only honours the variable
# under policy CMP0141=NEW (CMake 3.25+); the root cmake_minimum_required
# stays 3.22 for Ubuntu 22.04, so the opt-in lives here, as in CI.
# ---------------------------------------------------------------------------
$cmakeArgs = @(
  '-S', (ConvertTo-CMakePath $PjRepoRoot),
  '-B', (ConvertTo-CMakePath $PjBuildDir),
  '-G', 'Ninja Multi-Config',
  "-DCMAKE_TOOLCHAIN_FILE=$(ConvertTo-CMakePath $Toolchain)",
  "-DCMAKE_PREFIX_PATH=$(ConvertTo-CMakePath $PjQtDir)",
  '-DCMAKE_POLICY_DEFAULT_CMP0141=NEW',
  '-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded',
  "-DPJ_SDK_LOCAL_DIR=$(ConvertTo-CMakePath $SdkLocalDir)"
)
# sccache is the Windows analogue of build.sh's ccache pickup (CI wires it the
# same way); /Z7 above is what makes its compile cache effective.
if (Get-Command sccache.exe -ErrorAction SilentlyContinue) {
  $cmakeArgs += '-DCMAKE_C_COMPILER_LAUNCHER=sccache', '-DCMAKE_CXX_COMPILER_LAUNCHER=sccache'
}
$cmakeArgs += @(
  "-DPJ_BUILD_TESTS=$PJ_BUILD_TESTS",
  "-DPJ_BUILD_DEMOS=$PJ_BUILD_DEMOS",
  "-DPJ_BUILD_WIDGET_DEMOS=$PJ_BUILD_WIDGET_DEMOS",
  "-DPJ_BUILD_MARKETPLACE_APP=$PJ_BUILD_MARKETPLACE_APP"
)
if ($env:PJ_BUILD_RASTER_HELPER) { $cmakeArgs += "-DPJ_BUILD_RASTER_HELPER=$($env:PJ_BUILD_RASTER_HELPER)" }
# What build.sh passes on its `none` lane, named explicitly so a stale cache
# entry can never re-enable instrumentation.
$cmakeArgs += @(
  '-DPJ_ENABLE_SANITIZERS=OFF',
  '-DPJ_ENABLE_TSAN=OFF',
  '-DPJ_SDK_FORCE_SOURCE=OFF',
  '-DPJ_SANITIZE_CONTAINERS=OFF',
  "-DPJ_VERSION=$PjVersion",
  "-DPJ_INSTALLATION=$PjInstallation"
)

# PJ_BUILD_TARGET: packaging builds set it to pj_app so only the shipped
# closure (app + pj-plugin-check) is linked; `all` is the dev default.
$buildArgs = @('--build', $PjBuildDir, '--config', $PjConfig, '--target', $PJ_BUILD_TARGET, '--parallel')

# Share the actual command lines with the preview, before requiring the
# toolchain or Qt, creating build artifacts, or contacting Conan remotes.
if ($env:PJ_DRY_RUN -eq '1') {
  Write-Host "Build directory: $PjBuildDir"
  Write-Host "Build target: $PJ_BUILD_TARGET"
  Write-Host ('conan ' + (($conanArgs | ForEach-Object { Format-ShellArg $_ }) -join ' '))
  Write-Host ('cmake ' + (($cmakeArgs | ForEach-Object { Format-ShellArg $_ }) -join ' '))
  Write-Host ('cmake ' + (($buildArgs | ForEach-Object { Format-ShellArg $_ }) -join ' '))
  exit 0
}

# ---------------------------------------------------------------------------
# Qt: never installed from here; scripts\windows\install_qt6.ps1 does that,
# as scripts/install_qt6.sh does for build.sh.
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $PjQmake)) {
  Write-Err "Qt $PjQtVersion (msvc2022_64) not found at $PjQtDir."
  Write-Err 'Install it with: scripts\windows\install_qt6.ps1'
  exit 1
}

# ---------------------------------------------------------------------------
# Toolchain and tools: VS 2022 (see Test-PjMsvc2022Env for why that one), the
# real Python, conan/cmake/ninja on PATH.
# ---------------------------------------------------------------------------
Enable-PjMsvc2022Env
Repair-PjPythonPath
Assert-PjBuildTools

# ---------------------------------------------------------------------------
# Conan. plotjuggler_sdk is served only by PlotJuggler's Artifactory remote
# (anonymous read); ConanCenter supplies everything else. The remote list is
# explicit so unrelated developer remotes can never shadow the stock recipes.
# ---------------------------------------------------------------------------
Register-PjConanRemote

# Mirrors windows-ci.yml "Seed Conan source download cache". Some third-party
# source tarballs live on hosts that refuse datacenter IPs (zlib.net answers
# HTTP 415; minizip, a hard requirement of assimp, downloads zlib's tarball
# from there and has no mirror in its recipe). Those tarballs are versioned in
# .github/conan/source-backup, addressed by sha256, and copied into Conan's
# source download cache so a source() fetch is served locally; Conan still
# verifies the recipe-pinned sha256 on every use, and anything not seeded falls
# through to the recipe's own URLs. The cache is per user and outside every
# checkout: content-addressed files are safe to share between worktrees.
# core.* confs are accepted only in global.conf (Conan rejects them in
# profiles and on the -c command line), so that file is rewritten in place:
# any previous download_cache line is dropped before the current one is
# appended, which keeps repeated runs from accumulating entries.
function Initialize-PjConanSourceCache([string]$ConanHome) {
  $backup = Join-Path $PjRepoRoot '.github\conan\source-backup'
  $cacheDir = Join-Path $env:LOCALAPPDATA 'PlotJuggler4\conan-source-cache'
  $storeDir = Join-Path $cacheDir 's'
  New-Item -ItemType Directory -Force -Path $storeDir | Out-Null
  Get-ChildItem -LiteralPath $backup -File | Where-Object { $_.Name -ne 'README.md' } | Copy-Item -Destination $storeDir -Force
  $conf = Join-Path $ConanHome 'global.conf'
  $existing = @()
  if (Test-Path -LiteralPath $conf) { $existing = @([System.IO.File]::ReadAllLines($conf)) }
  # Only the download_cache line is ours to rewrite. A developer who has already
  # pointed Conan at their own source mirror keeps it: this is a shared, per-user
  # global.conf, not a CI-owned one.
  $hasUrls = @($existing | Where-Object { $_ -match '^core\.sources:download_urls=' }).Count -gt 0
  $lines = @($existing | Where-Object { $_ -notmatch '^core\.sources:download_cache=' })
  $entry = 'core.sources:download_cache=' + $cacheDir.Replace('\', '/')
  $lines += $entry
  Write-Host "Editing $conf (Conan source download cache, seeded from $backup):"
  Write-Host "  $entry"
  if (-not $hasUrls) {
    # Anything not seeded retries through ConanCenter's sha256-addressed mirror
    # when the recipe's own host refuses the request. 'origin' stays first: Conan
    # aborts the download if a backup entry answers 401/403, only 404 falls through.
    $mirror = 'core.sources:download_urls=["origin", "https://c3i.jfrog.io/artifactory/conan-center-backup-sources/"]'
    $lines += $mirror
    Write-Host "  $mirror"
  }
  [System.IO.File]::WriteAllLines($conf, [string[]]$lines)
}

if (-not $SkipConanInstall) {
  $conanHome = ([string](& conan.exe config home | Select-Object -Last 1)).Trim()
  if ($LASTEXITCODE -ne 0 -or $conanHome -eq '') {
    Write-Err 'conan config home failed'
    exit 1
  }
  # A default profile is created only when none exists: Conan needs one to
  # resolve anything, but every setting that matters is pinned on the command
  # line, so what it detects is not relied on.
  if (-not (Test-Path -LiteralPath (Join-Path $conanHome 'profiles\default'))) {
    Write-Host 'No default Conan profile yet; detecting one'
    Invoke-Native conan.exe @('profile', 'detect')
  }
  Initialize-PjConanSourceCache $conanHome
  Invoke-Native conan.exe $conanArgs
} elseif (-not (Test-Path -LiteralPath $Toolchain)) {
  Write-Err "Missing $Toolchain; run Conan install first (build.bat without --skip-conan-install)."
  exit 1
}

# ---------------------------------------------------------------------------
# CMake configure + build
# ---------------------------------------------------------------------------
Invoke-Native cmake.exe $cmakeArgs

# Surface the compile DB at the repo root so clangd/editors resolve includes
# without extra config. Ninja Multi-Config writes it at configure time with one
# entry per source per configuration (Debug first). Copied, not linked:
# symlinks need admin rights or Developer Mode. The root path is gitignored.
$compileDb = Join-Path $PjBuildDir 'compile_commands.json'
if (Test-Path -LiteralPath $compileDb) {
  Copy-Item -LiteralPath $compileDb -Destination (Join-Path $PjRepoRoot 'compile_commands.json') -Force
} else {
  Write-Host "note: $compileDb was not generated; the root compile_commands.json is left untouched"
}

Invoke-Native cmake.exe $buildArgs
exit 0
