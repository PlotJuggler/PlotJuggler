#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Annotate QA screenshots and publish them for a PR body.

  shots.py annotate IN.png OUT.png [--rect X,Y,W,H]... [--arrow X1,Y1,X2,Y2]... [--text X,Y,LABEL]...
                                   [--crop X,Y,W,H] [--color red]
      draws on a copy: rectangles around the problem, an arrow pointing at it (head at X2,Y2), a label
      on a filled box. Coordinates are pixels of IN.png; --crop applies last, so draw in IN's coordinates.
  shots.py publish FOLDER "Caption=path.png"...
      commits the images to the `qa-screenshots` orphan branch under FOLDER/ (one folder per fix branch),
      pushes it, and prints Markdown that embeds them through raw.githubusercontent.com.

`gh` cannot upload images to a PR, hence the orphan branch. Needs Pillow for `annotate`.
"""

import argparse
import math
import os
import subprocess
import sys
from pathlib import Path

REPO = "PlotJuggler/PlotJuggler"
BRANCH = "qa-screenshots"


def ints(text, count):
    values = [int(round(float(v))) for v in text.split(",")]
    if len(values) != count:
        raise argparse.ArgumentTypeError(f"expected {count} comma-separated numbers, got {text!r}")
    return values


def annotate(args):
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        sys.exit("annotate needs Pillow: pip install pillow")
    image = Image.open(args.input).convert("RGB")
    draw = ImageDraw.Draw(image)
    width = max(3, image.width // 400)
    font = ImageFont.load_default(size=max(16, image.width // 60))
    for x, y, w, h in (ints(r, 4) for r in args.rect):
        draw.rectangle([x, y, x + w, y + h], outline=args.color, width=width)
    for x1, y1, x2, y2 in (ints(a, 4) for a in args.arrow):
        draw.line([x1, y1, x2, y2], fill=args.color, width=width)
        # Head: a triangle at (x2, y2) pointing along the shaft.
        angle = math.atan2(y2 - y1, x2 - x1)
        size = width * 5
        left = (x2 - size * math.cos(angle - 0.45), y2 - size * math.sin(angle - 0.45))
        right = (x2 - size * math.cos(angle + 0.45), y2 - size * math.sin(angle + 0.45))
        draw.polygon([(x2, y2), left, right], fill=args.color)
    for spec in args.text:
        x, y, label = spec.split(",", 2)
        x, y = int(float(x)), int(float(y))
        box = draw.textbbox((x, y), label, font=font)
        pad = width * 2
        draw.rectangle([box[0] - pad, box[1] - pad, box[2] + pad, box[3] + pad], fill=args.color)
        draw.text((x, y), label, fill="white", font=font)
    if args.crop:
        x, y, w, h = ints(args.crop, 4)
        image = image.crop((x, y, x + w, y + h))
    image.save(args.output)
    print(args.output)


def git(worktree, *argv, capture=False):
    return subprocess.run(["git", "-C", str(worktree), *argv], check=True, text=True,
                          capture_output=capture).stdout


def screenshots_worktree():
    common = Path(subprocess.run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                 check=True, text=True, capture_output=True).stdout.strip())
    worktree = common.parent / ".worktrees" / BRANCH
    if not worktree.exists():
        subprocess.run(["git", "fetch", "-q", "origin", f"{BRANCH}:{BRANCH}"], check=False)
        subprocess.run(["git", "worktree", "add", "-q", str(worktree), BRANCH], check=True)
    return worktree


def publish(args):
    worktree = screenshots_worktree()
    git(worktree, "pull", "-q", "--rebase", "origin", BRANCH)
    target = worktree / args.folder
    target.mkdir(parents=True, exist_ok=True)
    markdown = []
    for item in args.images:
        caption, _, source = item.rpartition("=")
        if not caption:
            sys.exit(f"expected Caption=path.png, got {item!r}")
        name = Path(source).name
        (target / name).write_bytes(Path(source).read_bytes())
        git(worktree, "add", f"{args.folder}/{name}")
        url = f"https://raw.githubusercontent.com/{REPO}/{BRANCH}/{args.folder}/{name}"
        markdown.append(f"**{caption}**\n\n![{caption}]({url})\n")
    if git(worktree, "status", "--porcelain", capture=True).strip():
        subprocess.run(["git", "-C", str(worktree), "commit", "-q", "-m", f"screenshots for {args.folder}"],
                       check=True, env={**os.environ, "PRE_COMMIT_ALLOW_NO_CONFIG": "1"})
        git(worktree, "push", "-q", "origin", BRANCH)
    print("\n".join(markdown))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    a = sub.add_parser("annotate")
    a.add_argument("input")
    a.add_argument("output")
    a.add_argument("--rect", action="append", default=[])
    a.add_argument("--arrow", action="append", default=[])
    a.add_argument("--text", action="append", default=[])
    a.add_argument("--crop")
    a.add_argument("--color", default="#e0201b")
    a.set_defaults(func=annotate)
    p = sub.add_parser("publish")
    p.add_argument("folder")
    p.add_argument("images", nargs="+")
    p.set_defaults(func=publish)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
