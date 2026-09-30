#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Compares two directory trees of PNG dumps image by image.

    scripts/perf/pxcmp.py BEFORE AFTER [--ignore GLOB]...

Prints, per image, DIFF with the number of differing pixels (exact RGBA) and the
largest channel delta, SIZE for a changed size, NEW / MISSING for images present
on one side only, then a one-line total. Exits 1 when any image differs, changes
size or is missing. A differing image whose path matches an --ignore glob (an
image that shows live values, such as the perf HUD's timings) prints as NOISE
and does not count. Needs numpy and Pillow.
"""
import argparse
import fnmatch
import os
import sys

import numpy as np
from PIL import Image


def pngs(root):
    return {os.path.relpath(os.path.join(r, f), root) for r, _, fs in os.walk(root) for f in fs if f.endswith('.png')}


def main():
    parser = argparse.ArgumentParser(usage=__doc__)
    parser.add_argument('before')
    parser.add_argument('after')
    parser.add_argument('--ignore', action='append', default=[])
    args = parser.parse_args()
    before, after = args.before, args.after
    pb, pa = pngs(before), pngs(after)
    diff = 0
    for rel in sorted(pb | pa):
        if rel not in pa:
            print(f'MISSING {rel}')
            continue
        if rel not in pb:
            print(f'NEW {rel}')
            continue
        ib = np.asarray(Image.open(os.path.join(before, rel)).convert('RGBA'), dtype=np.int16)
        ia = np.asarray(Image.open(os.path.join(after, rel)).convert('RGBA'), dtype=np.int16)
        if ib.shape != ia.shape:
            print(f'SIZE {rel} {ib.shape} {ia.shape}')
            diff += 1
            continue
        d = np.abs(ib - ia)
        n = int((d.max(axis=2) > 0).sum())
        if n and any(fnmatch.fnmatch(rel, glob) for glob in args.ignore):
            print(f'NOISE {rel} pixels={n} max={int(d.max())}')
        elif n:
            print(f'DIFF {rel} pixels={n} max={int(d.max())}')
            diff += 1
    missing = len(pb - pa)
    print(f'{len(pb & pa)} compared, {diff} differ, {len(pa - pb)} new, {missing} missing')
    return 1 if diff or missing else 0


if __name__ == '__main__':
    sys.exit(main())
