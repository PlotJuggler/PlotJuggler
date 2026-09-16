# SPDX-License-Identifier: MPL-2.0
# Builds a pj-official-plugins checkout on Windows and stages its plugin DLLs
# into one folder for `run.bat --plugin-dir`. The plugin repository owns its
# build: this script only supplies the Windows environment its build.sh
# expects (VS 2022, the real Python, Git Bash, the PlotJuggler Conan remote)
# and runs its scripts/ensure_core.sh and build.sh in the order its
# ci-windows.yml does, so no Conan or CMake flag is duplicated here.
#
# Windows PowerShell 5.1 only, ASCII only. No param() block (see build.ps1).

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'PjWindowsCommon.ps1')

function Show-Usage([bool]$ToStderr) {
  $text = @'
Usage: powershell -ExecutionPolicy Bypass -File scripts\windows\build_plugins.ps1 [OPTIONS] [plugin_dir]

Builds pj-official-plugins from source with its own build.sh (every plugin, or
only plugin_dir, e.g. data_load_csv) and copies each *_plugin.dll into one
folder. Launch PJ4 against that folder with:  run.bat --plugin-dir <stage folder>

Options:
  --plugins-dir PATH   pj-official-plugins checkout (default: <PJ4>\pj-official-plugins)
  --stage-dir PATH     folder the plugin DLLs are copied to
                       (default: <plugins-dir>\build\pj4_plugins)
  --help               Show this help message

A full build refreshes the stage folder (its stale *_plugin.dll files are
removed first); a single-plugin build only replaces that plugin's DLLs.
BUILD_TYPE is honoured as by build.sh (default Release). PJ_DRY_RUN=1 prints
the resolved paths and commands without building.

Requires Visual Studio 2022 (imported automatically), Python 3 with conan and
ninja, CMake, Git for Windows (its bash runs build.sh; the WSL bash is never
used) and a default Conan profile with compiler=msvc and compiler.version=194,
which build.sh builds with. The first build compiles Arrow/Flight, gRPC,
protobuf and more from source and can take hours.
'@
  if ($ToStderr) { Write-Err $text } else { Write-Host $text }
}

function Exit-Usage([string]$Message) {
  Write-Err $Message
  Show-Usage $true
  exit 2
}

# .NET's notion of the current directory can differ from $PWD: root relative
# paths first, then let GetFullPath collapse '..' segments.
function Resolve-UserPath([string]$Path) {
  if (-not [System.IO.Path]::IsPathRooted($Path)) { $Path = Join-Path (Get-Location).Path $Path }
  return [System.IO.Path]::GetFullPath($Path)
}

# Git for Windows' bash, found next to git.exe (<Git>\cmd\git.exe or
# <Git>\mingw64\bin\git.exe -> <Git>\bin\bash.exe). Never a bare `bash`: on
# PATH that is usually C:\Windows\System32\bash.exe, which runs inside WSL and
# cannot drive the MSVC toolchain.
function Find-GitBash {
  $git = Get-Command git.exe -ErrorAction SilentlyContinue
  if (-not $git) { return $null }
  $dir = Split-Path -Parent $git.Source
  for ($n = 0; $n -lt 4 -and $dir; $n++) {
    $candidate = Join-Path $dir 'bin\bash.exe'
    if (Test-Path -LiteralPath $candidate) { return $candidate }
    $dir = Split-Path -Parent $dir
  }
  return $null
}

# [settings] key=value pairs of a Conan profile file.
function Read-ConanProfileSettings([string]$Path) {
  $settings = @{}
  $section = ''
  foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
    $t = $line.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { continue }
    if ($t.StartsWith('[')) { $section = $t; continue }
    if ($section -ne '[settings]') { continue }
    $eq = $t.IndexOf('=')
    if ($eq -lt 1) { continue }
    $settings[$t.Substring(0, $eq).Trim()] = $t.Substring($eq + 1).Trim()
  }
  return $settings
}

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
$PluginsDir = Join-Path $PjRepoRoot 'pj-official-plugins'
$StageDir = ''
$Plugin = ''

$i = 0
while ($i -lt $args.Count) {
  $a = [string]$args[$i]
  if ($a -eq '--plugins-dir' -or $a.StartsWith('--plugins-dir=') -or $a -eq '--stage-dir' -or $a.StartsWith('--stage-dir=')) {
    $name = $a.Split('=')[0]
    if ($a -eq $name) {
      $i++
      $value = ''
      if ($i -lt $args.Count) { $value = [string]$args[$i] }
    } else {
      $value = $a.Substring($name.Length + 1)
    }
    if ($value -eq '' -or $value.StartsWith('-')) { Exit-Usage "$name needs a path" }
    if ($name -eq '--plugins-dir') { $PluginsDir = Resolve-UserPath $value } else { $StageDir = Resolve-UserPath $value }
  }
  elseif ($a -eq '-h' -or $a -eq '--help') {
    Show-Usage $false
    exit 0
  }
  elseif ($a.StartsWith('-')) { Exit-Usage "unknown argument: $a" }
  elseif ($Plugin -ne '') { Exit-Usage "only one plugin_dir may be given (got '$Plugin' and '$a')" }
  elseif ($a -match '[\\/:]') { Exit-Usage "plugin_dir is a directory name inside the checkout, not a path: $a" }
  else { $Plugin = $a }
  $i++
}

if ($StageDir -eq '') { $StageDir = Join-Path $PluginsDir 'build\pj4_plugins' }
$BuildType = 'Release'
if ($env:BUILD_TYPE) { $BuildType = $env:BUILD_TYPE }
# build.sh's output tree: build/all/<type> for the aggregate, build/<plugin>/<type> for one plugin.
$OutputDir = Join-Path $PluginsDir "build\all\$BuildType"
if ($Plugin -ne '') { $OutputDir = Join-Path $PluginsDir "build\$Plugin\$BuildType" }

$GitBash = Find-GitBash
$buildShArgs = @('build.sh')
if ($Plugin -ne '') { $buildShArgs += $Plugin }

if ($env:PJ_DRY_RUN -eq '1') {
  $target = 'all plugins'
  if ($Plugin -ne '') { $target = $Plugin }
  Write-Host "Plugins checkout: $PluginsDir"
  Write-Host "Plugins: $target"
  Write-Host "Build output: $OutputDir"
  Write-Host "Stage folder: $StageDir"
  Write-Host "> $GitBash scripts/ensure_core.sh"
  Write-Host ("> $GitBash " + ($buildShArgs -join ' '))
  exit 0
}

# ---------------------------------------------------------------------------
# Checkout
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath (Join-Path $PluginsDir 'build.sh'))) {
  Write-Err "No pj-official-plugins checkout at $PluginsDir (build.sh not found)."
  Write-Err "Clone it with: git clone https://github.com/PlotJuggler/pj-official-plugins.git `"$PluginsDir`""
  Write-Err 'or point at an existing one with --plugins-dir PATH.'
  exit 1
}
if ($Plugin -ne '' -and -not (Test-Path -LiteralPath (Join-Path $PluginsDir $Plugin) -PathType Container)) {
  Exit-Usage "plugin directory not found in the checkout: $Plugin"
}
if (-not $GitBash) {
  Write-Err 'Git for Windows was not found (no bin\bash.exe next to git.exe). Install it from https://git-scm.com;'
  Write-Err 'build.sh needs its bash (the WSL bash cannot drive the MSVC toolchain).'
  exit 1
}

# Report a stale checkout from the last fetch, without fetching or pulling:
# what gets built stays the user's decision. cmd owns the stderr redirection
# because PowerShell 5.1 would turn git's "no upstream" message into an error.
$behind = & cmd.exe /d /c "git -C `"$PluginsDir`" rev-list --count HEAD..@{upstream} 2>nul"
if ($LASTEXITCODE -eq 0 -and $behind -and [int]([string]$behind).Trim() -gt 0) {
  Write-Host "warning: $PluginsDir is $(([string]$behind).Trim()) commit(s) behind its upstream (as of the last fetch); 'git pull' to build the latest plugins."
}

# ---------------------------------------------------------------------------
# Toolchain, tools and Conan
# ---------------------------------------------------------------------------
Enable-PjMsvc2022Env
Repair-PjPythonPath
Assert-PjBuildTools

# build.sh passes no -s compiler.* settings, so the default profile decides the
# compiler. A profile detected on a machine that also has VS 2026 says 195,
# which builds binaries no VS 2022 host shares package IDs with.
$conanHome = ([string](& conan.exe config home | Select-Object -Last 1)).Trim()
if ($LASTEXITCODE -ne 0 -or $conanHome -eq '') {
  Write-Err 'conan config home failed'
  exit 1
}
$profilePath = Join-Path $conanHome 'profiles\default'
if (-not (Test-Path -LiteralPath $profilePath)) {
  Write-Err "No default Conan profile at $profilePath."
  Write-Err "Create one with 'conan profile detect', then set compiler.version=194 in its [settings]."
  exit 1
}
$profileSettings = Read-ConanProfileSettings $profilePath
if ($profileSettings['compiler'] -ne 'msvc' -or $profileSettings['compiler.version'] -ne '194') {
  Write-Err "The default Conan profile ($profilePath) has compiler=$($profileSettings['compiler']) compiler.version=$($profileSettings['compiler.version'])."
  Write-Err 'build.sh compiles with it; set compiler=msvc and compiler.version=194 (VS 2022) in its [settings] and rerun.'
  exit 1
}

Register-PjConanRemote

# ---------------------------------------------------------------------------
# Build, in ci-windows.yml order: the pinned SDK first (prebuilt from the
# remote when available, else built from its release tag), then the plugins.
# ---------------------------------------------------------------------------
# MSYS path conversion must stay on: build.sh hands POSIX paths such as
# -DCMAKE_TOOLCHAIN_FILE=/c/... to native cmake.exe and relies on Git Bash
# rewriting them into Windows paths.
$env:MSYS_NO_PATHCONV = $null
Set-Location -LiteralPath $PluginsDir
Invoke-Native $GitBash @('scripts/ensure_core.sh')
Invoke-Native $GitBash $buildShArgs

# ---------------------------------------------------------------------------
# Stage
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $OutputDir -PathType Container)) {
  Write-Err "build.sh succeeded but its output folder is missing: $OutputDir"
  exit 1
}
$built = @(Get-ChildItem -LiteralPath $OutputDir -Recurse -File -Filter '*_plugin.dll')
if ($built.Count -eq 0) {
  Write-Err "No *_plugin.dll was produced under $OutputDir"
  exit 1
}

New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

# A PlotJuggler started with --plugin-dir <stage folder> keeps every plugin DLL
# in it loaded, and Windows refuses to delete or overwrite a loaded DLL. Check
# every file this run would replace before touching any, so a refused
# replacement never leaves the folder half refreshed.
function Test-FileInUse([string]$Path) {
  try {
    $stream = [System.IO.File]::Open($Path, 'Open', 'ReadWrite', 'None')
    $stream.Close()
    return $false
  } catch {
    return $true
  }
}
$replaced = @()
if ($Plugin -eq '') {
  $replaced = @(Get-ChildItem -LiteralPath $StageDir -File -Filter '*_plugin.dll' | ForEach-Object { $_.FullName })
} else {
  $replaced = @($built | ForEach-Object { Join-Path $StageDir $_.Name } | Where-Object { Test-Path -LiteralPath $_ })
}
$busy = @($replaced | Where-Object { Test-FileInUse $_ } | ForEach-Object { Split-Path -Leaf $_ })
if ($busy.Count -gt 0) {
  Write-Err "Cannot refresh $StageDir`: $($busy.Count) plugin DLL(s) are in use or not writable ($(($busy | Select-Object -First 3) -join ', '))."
  $running = @(Get-Process plotjuggler4 -ErrorAction SilentlyContinue)
  if ($running.Count -gt 0) {
    Write-Err "Close PlotJuggler (running as pid $(($running | ForEach-Object { $_.Id }) -join ', ')) and rerun."
  } else {
    Write-Err 'Close whatever has them loaded and rerun.'
  }
  Write-Err 'Nothing in the folder was changed; the plugin build is up to date, so a rerun only repeats the staging.'
  exit 1
}

if ($Plugin -eq '') {
  # PJ4 loads every DLL it finds in the folder, so a plugin removed from the
  # checkout must not survive in it.
  foreach ($path in $replaced) { Remove-Item -LiteralPath $path -Force }
}
# One DLL per file name: if a build tree holds several copies, stage the newest.
foreach ($group in ($built | Group-Object -Property Name)) {
  $pick = $group.Group | Sort-Object -Property LastWriteTime -Descending | Select-Object -First 1
  if ($group.Count -gt 1) {
    Write-Host "note: $($group.Count) copies of $($group.Name) under $OutputDir; staging $($pick.FullName)"
  }
  Copy-Item -LiteralPath $pick.FullName -Destination $StageDir -Force
  Write-Host "  staged $($group.Name)"
}

$stagedCount = @(Get-ChildItem -LiteralPath $StageDir -File -Filter '*_plugin.dll').Count
Write-Host ''
Write-Host "Staged $stagedCount plugin DLL(s) in $StageDir"
Write-Host "Launch PJ4 with them:  run.bat --plugin-dir `"$StageDir`""
Write-Host '(or add the folder under Preferences -> plugin folders to load it on every launch)'
exit 0
