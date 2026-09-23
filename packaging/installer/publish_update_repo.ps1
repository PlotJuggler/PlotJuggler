<#
.SYNOPSIS
  Publish a Qt Installer Framework update repository to Cloudflare R2.

.DESCRIPTION
  Uploads the repository that build_windows_installer.ps1 -RepoOutDir produced,
  so the maintenance tool installed on users' machines is offered the new
  version. Nothing is built or regenerated here.

  The installed maintenance tool reads Updates.xml and then fetches the
  component archives it names, so this uploads in the reverse of that order:
  archives first, Updates.xml last. A maintenance tool that polls mid-publish
  then sees an index older than the payload, never one promising an archive
  that has not landed. (The same rule governs packaging/deb/publish_apt.sh.)

  Credentials come from the environment, never the command line:

    R2_BUCKET               bucket holding the repository
    R2_ENDPOINT             https://<account-id>.r2.cloudflarestorage.com
    AWS_ACCESS_KEY_ID       R2 API token with object read/write on the bucket
    AWS_SECRET_ACCESS_KEY
    PJ_WINDOWS_UPDATE_URL   public URL the repository is served at — the one the
                            installer was built with. Its path is the key
                            prefix inside the bucket, so the repository always
                            lands exactly where installed maintenance tools look.

.PARAMETER RepoDir
  The repogen output directory to publish.

.PARAMETER Version
  Version expected to be advertised by the published Updates.xml.

.PARAMETER SkipVerify
  Do not re-fetch the published repository to check it.

.PARAMETER DryRun
  Print what would be uploaded and exit.
#>
param(
  [Parameter(Mandatory = $true)][string]$RepoDir,
  [Parameter(Mandatory = $true)][string]$Version,
  [switch]$SkipVerify,
  [switch]$DryRun
)

$ErrorActionPreference = "Stop"
function Info($m) { Write-Host "[publish-repo] $m" -ForegroundColor Cyan }
function Die($m)  { Write-Host "[publish-repo] ERROR: $m" -ForegroundColor Red; exit 1 }

if (-not (Test-Path $RepoDir)) { Die "repository directory not found: $RepoDir" }
$RepoDir = (Resolve-Path $RepoDir).Path
$updatesXml = Join-Path $RepoDir "Updates.xml"
if (-not (Test-Path $updatesXml)) { Die "$RepoDir has no Updates.xml — is it a repogen output directory?" }

if (-not $env:PJ_WINDOWS_UPDATE_URL) { Die "PJ_WINDOWS_UPDATE_URL is not set" }
$publicUrl = $env:PJ_WINDOWS_UPDATE_URL.TrimEnd('/')
$Prefix    = ([uri]$publicUrl).AbsolutePath.Trim('/')
if (-not $Prefix) { Die "PJ_WINDOWS_UPDATE_URL ($publicUrl) has no path to use as the bucket prefix" }

# Checked before anything is uploaded: publishing a repository that advertises
# the wrong version offers users either nothing or the wrong build.
if ((Get-Content -Raw $updatesXml) -notmatch [regex]::Escape("<Version>$Version</Version>")) {
  $found = (Select-String -Path $updatesXml -Pattern '<Version>.*?</Version>' -AllMatches).Matches.Value -join ", "
  Die "$updatesXml advertises [$found], not $Version."
}

Info "repository: $RepoDir"
Info "version   : $Version"
Info "prefix    : $Prefix"

if ($DryRun) {
  Info "--dry-run: would upload the following, Updates.xml last"
  Get-ChildItem $RepoDir -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($RepoDir.Length).TrimStart('\', '/').Replace('\', '/')
    Write-Host "  $Prefix/$rel"
  }
  exit 0
}

foreach ($name in @("R2_BUCKET", "R2_ENDPOINT", "AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY")) {
  if (-not (Get-Item "env:$name" -ErrorAction SilentlyContinue)) { Die "$name is not set" }
}
if (-not (Get-Command aws -ErrorAction SilentlyContinue)) { Die "the AWS CLI is required to reach R2's S3 API" }

# R2 is single-region; its S3 API wants a region and ignores which one.
if (-not $env:AWS_DEFAULT_REGION) { $env:AWS_DEFAULT_REGION = "auto" }
# Newer AWS CLI releases attach a CRC32 checksum trailer to every upload by
# default, which R2 rejects with an opaque 400.
if (-not $env:AWS_REQUEST_CHECKSUM_CALCULATION) { $env:AWS_REQUEST_CHECKSUM_CALCULATION = "when_required" }
if (-not $env:AWS_RESPONSE_CHECKSUM_VALIDATION) { $env:AWS_RESPONSE_CHECKSUM_VALIDATION = "when_required" }

$dest = "s3://$env:R2_BUCKET/$Prefix"

# Component archives carry their version in the file name, so they never change
# once written; Updates.xml does, and a stale copy at the edge is exactly what
# makes a maintenance tool miss a release that is already published.
$immutable = "public, max-age=31536000, immutable"
$nocache   = "public, max-age=60, must-revalidate"

# A version is published once. Archive names carry the version and go up
# immutable, so an edge that already served one keeps serving it for a year:
# replacing it (a re-run release rebuilds with different bytes) would pair the
# new checksum with the old archive and fail every update's hash verification.
# Re-publishing identical bytes, e.g. a retried job, is harmless. Comparing the
# small .sha1 files repogen writes beside each archive is enough.
$existing = @(& aws s3 ls "$dest/" --recursive --endpoint-url $env:R2_ENDPOINT 2>$null) |
  ForEach-Object { ($_.Trim() -split '\s+', 4)[3] }
foreach ($sha in Get-ChildItem $RepoDir -Recurse -Filter *.sha1) {
  $key = "$Prefix/" + $sha.FullName.Substring($RepoDir.Length).TrimStart('\', '/').Replace('\', '/')
  if ($existing -notcontains $key) { continue }
  $remote = (& aws s3 cp "s3://$env:R2_BUCKET/$key" - --endpoint-url $env:R2_ENDPOINT) -join ''
  if ($LASTEXITCODE -ne 0) { Die "could not read s3://$env:R2_BUCKET/$key (exit $LASTEXITCODE)." }
  if ($remote.Trim() -ne (Get-Content -Raw $sha.FullName).Trim()) {
    Die "$key is already published with different content; archives are immutable at the edge, so publish a new version instead."
  }
}

Info "uploading component archives..."
& aws s3 sync $RepoDir $dest --endpoint-url $env:R2_ENDPOINT `
    --exclude "Updates.xml" --cache-control $immutable --no-progress
if ($LASTEXITCODE -ne 0) { Die "uploading the component archives failed (exit $LASTEXITCODE)." }

Info "uploading Updates.xml..."
& aws s3 cp $updatesXml "$dest/Updates.xml" --endpoint-url $env:R2_ENDPOINT `
    --content-type "text/xml" --cache-control $nocache --no-progress
if ($LASTEXITCODE -ne 0) { Die "uploading Updates.xml failed (exit $LASTEXITCODE)." }

Info "published: $dest"

if ($SkipVerify) { Info "--skip-verify: not re-fetching the published repository."; exit 0 }

# Verified through the PUBLIC url and unauthenticated, because that is how a
# maintenance tool reaches it: a bucket that only answers to the CI credentials
# is broken for every user while every upload above succeeded.
Info "verifying $publicUrl as a maintenance tool would fetch it..."

$published = $null
foreach ($attempt in 1..10) {
  try {
    $published = (Invoke-WebRequest -Uri "$publicUrl/Updates.xml" -UseBasicParsing -TimeoutSec 30).Content
    break
  } catch {
    if ($attempt -eq 10) { Die "could not fetch $publicUrl/Updates.xml — check the bucket is public and served at that URL." }
    Start-Sleep -Seconds 6
  }
}
if ($published -notmatch [regex]::Escape("<Version>$Version</Version>")) {
  Die "the published Updates.xml does not advertise $Version."
}

Info "DONE: maintenance tools will be offered $Version."
