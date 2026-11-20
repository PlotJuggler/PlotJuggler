#!/usr/bin/env python3
# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0
"""Packs raw easter egg GIFs into resources/gif/<name>.gif.enc.

Each raw GIF (128x128, looping, on a white background, named <character>_<variant>.gif) has its background
flood-filled to transparent from the corners, is re-encoded on one shared 64-colour palette, and is encrypted with the
key in PJ_EASTER_EGG_KEY: the GitHub secret the release builds compile in. Needs ImageMagick. List each new file in
resources/memes.qrc.

usage: PJ_EASTER_EGG_KEY=<key> scripts/pack_easter_eggs.py RAW.gif [RAW.gif ...]
"""

import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

OUT_DIR = pathlib.Path(__file__).resolve().parent.parent / "resources" / "gif"
# Flood-fill tolerance: wide enough to drop the anti-aliased white fringe, narrow enough to keep cream fur.
FUZZ = "28%"


def cipher(data: bytes, key: bytes, name: str) -> bytes:
    """XOR with a SHA-256 counter keystream; must match easterEggCipher in VisualizationPlaceholderWidget.cpp."""
    salt = key + name.encode()
    out = bytearray(data)
    for offset in range(0, len(out), 32):
        pad = hashlib.sha256(salt + (offset // 32).to_bytes(8, "little")).digest()
        for i, byte in enumerate(pad[: len(out) - offset]):
            out[offset + i] ^= byte
    return bytes(out)


def make_transparent(raw: pathlib.Path, work: pathlib.Path) -> bytes:
    magick = shutil.which("magick") or "convert"
    frames = str(work / "f_%04d.png")
    corners = [f"color {x},{y} floodfill" for x in (0, 127) for y in (0, 127)]
    draw = [arg for corner in corners for arg in ("-draw", corner)]
    subprocess.run(
        [magick, str(raw), "-coalesce", "-alpha", "set", "-fuzz", FUZZ, "-fill", "none", *draw, frames], check=True)
    pngs = sorted(str(p) for p in work.glob("f_*.png"))
    palette = str(work / "palette.png")
    subprocess.run([magick, *pngs, "-append", "+dither", "-colors", "64", "-unique-colors", palette], check=True)
    out = work / "out.gif"
    # Background disposal clears each frame before the next: with the default (none), a pixel that turns transparent
    # keeps its old colour and the motion leaves a trail of stale pixels.
    subprocess.run(
        [magick, *pngs, "-set", "delay", "4", "-set", "dispose", "Background", "-loop", "0", "+dither", "-remap",
         palette, "-layers", "Optimize", str(out)],
        check=True)
    check_no_stale_pixels(magick, pngs, out, work)
    return out.read_bytes()


def check_no_stale_pixels(magick: str, pngs: list, gif: pathlib.Path, work: pathlib.Path) -> None:
    """Every decoded frame's transparency must match its source frame."""
    decoded = str(work / "d_%04d.png")
    subprocess.run([magick, str(gif), "-coalesce", decoded], check=True)
    frames = sorted(str(p) for p in work.glob("d_*.png"))
    if len(frames) != len(pngs):
        sys.exit(f"{gif}: {len(frames)} decoded frames, {len(pngs)} source frames")
    compare = [magick, "compare"] if magick.endswith("magick") else ["compare"]
    for source, frame in zip(pngs, frames):
        # Exit status 1 only means the images differ; the metric goes to stderr.
        result = subprocess.run(
            [*compare, "-metric", "AE", "-channel", "alpha", source, frame, "null:"], capture_output=True, text=True)
        if float(result.stderr.split()[0]) != 0:
            sys.exit(f"{frame}: {result.stderr.strip()} pixels differ in transparency from {source}")


def main() -> int:
    key = os.environ.get("PJ_EASTER_EGG_KEY", "").encode()
    if not key or len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 1
    for arg in sys.argv[1:]:
        raw = pathlib.Path(arg)
        with tempfile.TemporaryDirectory() as tmp:
            gif = make_transparent(raw, pathlib.Path(tmp))
        name = raw.stem + ".gif.enc"
        (OUT_DIR / name).write_bytes(cipher(gif, key, name))
        print(f"{raw.name} -> resources/gif/{name} ({len(gif)} bytes); qrc: <file>gif/{name}</file>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
