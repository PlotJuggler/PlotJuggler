# SPDX-License-Identifier: MPL-2.0
<#
.SYNOPSIS
  Create an installation-free ZIP from the Windows installer's staged payload.
.PARAMETER StageDir
  The scratch tree produced by build_windows_installer.ps1 -StageDir.
.PARAMETER OutFile
  Destination ZIP path. Its contents extract directly to bin/, lib/ and licenses/.
#>
param(
  [Parameter(Mandatory = $true)][string]$StageDir,
  [Parameter(Mandatory = $true)][string]$OutFile
)

$ErrorActionPreference = "Stop"
$payload = Join-Path $StageDir "packages/io.plotjuggler.application/data"
if (-not (Test-Path -LiteralPath (Join-Path $payload "bin/PlotJuggler4.exe") -PathType Leaf)) {
  throw "No staged bin/PlotJuggler4.exe under $StageDir. Run build_windows_installer.ps1 first."
}
$payload = (Resolve-Path -LiteralPath $payload).ProviderPath

# IFW carries these in package metadata; the ZIP needs its own copies.
$licenses = Join-Path $payload "licenses"
New-Item -ItemType Directory -Force -Path $licenses | Out-Null
foreach ($name in @("license_mpl.txt", "license_lgpl.txt")) {
  Copy-Item -LiteralPath (Join-Path $PSScriptRoot "packages/io.plotjuggler.application/meta/$name") `
    -Destination $licenses -Force
}

$zip = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutFile)
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $zip) | Out-Null
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
# ZipFile includes hidden files and directories, unlike Compress-Archive.
[System.IO.Compression.ZipFile]::CreateFromDirectory(
  $payload, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $false)
Write-Host "[portable] DONE: $zip"
