# package_windows.ps1
# Script to package PlotJuggler 4 for a Windows portable release.
# Usage: powershell -File scripts/package_windows.ps1 -BuildDir build -QtDir .qt/6.11.1/msvc2022_64 -OutputDir dist/PlotJuggler

param (
    [string]$BuildDir = "build",
    [string]$QtDir = ".qt/6.11.1/msvc2022_64",
    [string]$OutputDir = "dist/PlotJuggler"
)

$ErrorActionPreference = "Stop"

Write-Host "========================================="
Write-Host "Packaging PlotJuggler 4 for Windows"
Write-Host "Build Dir:  $BuildDir"
Write-Host "Qt Dir:     $QtDir"
Write-Host "Output Dir: $OutputDir"
Write-Host "========================================="

# Clean and create output directory
if (Test-Path $OutputDir) {
    Write-Host "Removing existing output directory: $OutputDir"
    Remove-Item -Recurse -Force $OutputDir
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

# Find and copy the main executable
# In multi-config builds, it might be under RelWithDebInfo
$ExePaths = @(
    "$BuildDir/pj_app/RelWithDebInfo/plotjuggler4.exe",
    "$BuildDir/pj_app/Release/plotjuggler4.exe",
    "$BuildDir/pj_app/plotjuggler4.exe",
    "$BuildDir/bin/plotjuggler4.exe"
)

$ExeSource = $null
foreach ($Path in $ExePaths) {
    if (Test-Path $Path) {
        $ExeSource = $Path
        break
    }
}

if ($null -eq $ExeSource) {
    Write-Error "Could not find plotjuggler4.exe in any of the expected paths: $ExePaths"
}

Write-Host "Found executable: $ExeSource"
Copy-Item -Path $ExeSource -Destination "$OutputDir/plotjuggler4.exe" -Force

# Run windeployqt
$WinDeployQt = "$QtDir/bin/windeployqt.exe"
if (-not (Test-Path $WinDeployQt)) {
    Write-Error "windeployqt.exe not found at $WinDeployQt"
}

Write-Host "Running windeployqt..."
# We pass --no-compiler-runtime because we bundle the runtime dependencies or rely on the host
& $WinDeployQt --no-compiler-runtime --dir $OutputDir "$OutputDir/plotjuggler4.exe"

# Copy FFmpeg DLLs from Conan directories
$FfmpegDirsFile = "$BuildDir/pj_ffmpeg_bin_dirs.txt"
if (Test-Path $FfmpegDirsFile) {
    Write-Host "Copying FFmpeg DLLs..."
    Get-Content $FfmpegDirsFile | ForEach-Object {
        $Dir = $_.Trim()
        if ($Dir -and (Test-Path $Dir)) {
            Write-Host "Scanning FFmpeg directory: $Dir"
            Get-ChildItem -Path $Dir -Filter "*.dll" | ForEach-Object {
                Write-Host "Copying DLL: $_.Name"
                Copy-Item -Path $_.FullName -Destination $OutputDir -Force -ErrorAction SilentlyContinue
            }
        }
    }
} else {
    Write-Warning "FFmpeg bin dirs file not found at $FfmpegDirsFile. Skipping FFmpeg copy."
}

# Copy Python DLLs from Conan directories
$PythonDirsFile = "$BuildDir/pj_python_bin_dirs.txt"
if (Test-Path $PythonDirsFile) {
    Write-Host "Copying Python DLLs..."
    Get-Content $PythonDirsFile | ForEach-Object {
        $Dir = $_.Trim()
        if ($Dir -and (Test-Path $Dir)) {
            Write-Host "Scanning Python directory: $Dir"
            Get-ChildItem -Path $Dir -Filter "*.dll" | ForEach-Object {
                Write-Host "Copying DLL: $_.Name"
                Copy-Item -Path $_.FullName -Destination $OutputDir -Force -ErrorAction SilentlyContinue
            }
        }
    }
} else {
    Write-Warning "Python bin dirs file not found at $PythonDirsFile. Skipping Python copy."
}

# Copy internal plugins (if any)
# In PJ4, plugins might be compiled as .dll files.
# Let's search for .dll files under build directory that are not part of conan/qt or thirdparty source
# Typically they will be in build/plugins/ or build/lib/
$PluginOutputDir = "$OutputDir/plugins"
New-Item -ItemType Directory -Force -Path $PluginOutputDir | Out-Null

$PluginPaths = @(
    "$BuildDir/plugins",
    "$BuildDir/lib"
)

foreach ($Dir in $PluginPaths) {
    if (Test-Path $Dir) {
        Write-Host "Scanning for PlotJuggler plugins in $Dir..."
        Get-ChildItem -Path $Dir -Filter "*.dll" -Recurse | ForEach-Object {
            Write-Host "Copying plugin DLL: $_.Name"
            Copy-Item -Path $_.FullName -Destination $PluginOutputDir -Force
        }
    }
}

# Clean up empty plugins directory if no plugins were copied
if ((Get-ChildItem $PluginOutputDir).Count -eq 0) {
    Remove-Item $PluginOutputDir
}

Write-Host "========================================="
Write-Host "Packaging complete: $OutputDir"
Write-Host "========================================="
