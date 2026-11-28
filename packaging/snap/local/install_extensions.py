#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Bundle checksum-verified marketplace archives for PJ4's per-user startup seed."""

import argparse
import calendar
import hashlib
import json
import os
import re
import shutil
import stat
import tempfile
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath


PLATFORMS = {"amd64": "linux-x86_64", "arm64": "linux-arm64"}
# The AppImage's set: keep in lockstep with BUNDLE_IDS in packaging/appimage/build_appimage.sh.
BUNDLE_IDS = (
    "csv-loader",
    "mcap-loader",
    "parquet-loader",
    "ulog-loader",
    "mp4-loader",
    "pointcloud-3d-loader",
    "dummy-streamer",
    "foxglove-bridge",
    "plotjuggler-bridge",
    "webrtc-client",
    "ros-parser",
    "protobuf-parser",
    "json-parser",
    "data-tamer-parser",
    "toolbox-quaternion",
    "toolbox-transform-editor",
    "toolbox-mosaico",
    "arrow-parser",
    "toolbox-fft",
    "toolbox-anomaly-detector",
    "toolbox-data-exporter",
    "influx-parser",
    "navmap-parser",
    "candump-loader",
    "blf-loader",
    "mf4-loader",
    "toolbox-assistant-agent",
    "ros2-topic-subscriber",
)


def version_key(version):
    """Order versions; a pre-release (1.0.0-dev.2) sorts below its release (1.0.0)."""
    release, _, pre = version.partition("-")
    numbers = tuple(int(part) for part in release.split("."))
    return numbers, not pre, tuple(int(part) if part.isdigit() else part for part in pre.split("."))


def incompatibility(metadata, app_version, sdk_version):
    """Return why this build cannot load the extension, or None when it can."""
    for key, available in (("min_plotjuggler_version", app_version), ("min_sdk_required", sdk_version)):
        minimum = metadata.get(key)
        if minimum and version_key(minimum) > version_key(available):
            return f"{metadata['id']} {metadata['version']} requires {key}={minimum}; build provides {available}"
    return None


def select_release(releases, platform, app_version, sdk_version):
    """Pick the newest release that ships the platform and that this build can load.

    The registry keeps older releases of an id for older hosts.
    """
    usable = [
        release for release in releases
        if platform in release.get("platforms", {}) and not incompatibility(release, app_version, sdk_version)
    ]
    return max(usable, key=lambda release: version_key(release["version"]), default=None)


def install_extension(extension, platform, destination, app_version, sdk_version):
    """Download and validate one archive before copying its complete payload."""
    extension_id = extension["id"]
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", extension_id):
        raise ValueError(f"Invalid extension id: {extension_id}")
    artifact = extension["platforms"][platform]
    checksum = artifact.get("checksum", "")
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", checksum):
        raise ValueError(f"Missing or invalid SHA256 for {extension_id}")
    url = artifact["url"]
    if not url.startswith("https://"):
        raise ValueError(f"Expected HTTPS archive for {extension_id}")
    print(f"Bundling {extension_id} {extension['version']} ({platform})", flush=True)
    with tempfile.TemporaryDirectory(prefix="pj-extension-") as temporary:
        work = Path(temporary)
        archive_path = work / "extension.zip"
        with urllib.request.urlopen(url, timeout=60) as response, archive_path.open("wb") as output:
            shutil.copyfileobj(response, output)
        with archive_path.open("rb") as archive_file:
            digest = hashlib.file_digest(archive_file, "sha256").hexdigest()
        if digest != checksum[7:]:
            raise ValueError(f"SHA256 mismatch for {extension_id}")
        unpacked = work / "unpacked"
        with zipfile.ZipFile(archive_path) as archive:
            for member in archive.infolist():
                path = PurePosixPath(member.filename)
                mode = member.external_attr >> 16
                if path.is_absolute() or ".." in path.parts or "\\" in member.filename or stat.S_ISLNK(mode):
                    raise ValueError(f"Unsafe archive entry: {member.filename}")
            archive.extractall(unpacked)
            for member in archive.infolist():
                if member.is_dir():
                    continue
                extracted = unpacked / member.filename
                mode = (member.external_attr >> 16) & 0o777
                if mode:
                    extracted.chmod(mode)
                # The app recognises an already-seeded bundled plugin by its library's size and
                # mtime: keep the archive's time, or every snap build looks like a new plugin.
                stamp = calendar.timegm(member.date_time)
                os.utime(extracted, (stamp, stamp))
        entries = list(unpacked.iterdir())
        payload = entries[0] if len(entries) == 1 and entries[0].is_dir() else unpacked
        manifest = json.loads((payload / "manifest.json").read_text())
        if manifest.get("id") != extension_id or manifest.get("version") != extension["version"]:
            raise ValueError(f"Manifest id/version mismatch for {extension_id}")
        reason = incompatibility(manifest, app_version, sdk_version)
        if reason:
            raise ValueError(reason)
        target = Path(destination) / extension_id
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(payload, target)


def main():
    """Resolve the curated set from the registry for the build architecture."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("registry", type=Path)
    parser.add_argument("architecture", choices=PLATFORMS)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--app-version", required=True)
    parser.add_argument("--sdk-version", required=True)
    args = parser.parse_args()
    registry = json.loads(args.registry.read_text())
    platform = PLATFORMS[args.architecture]
    for extension_id in BUNDLE_IDS:
        releases = [extension for extension in registry["extensions"] if extension["id"] == extension_id]
        release = select_release(releases, platform, args.app_version, args.sdk_version)
        if not release:
            raise ValueError(
                f"Registry has no {extension_id} for {platform} that PlotJuggler {args.app_version} "
                f"(SDK {args.sdk_version}) can load"
            )
        install_extension(release, platform, args.destination, args.app_version, args.sdk_version)


if __name__ == "__main__":
    main()
