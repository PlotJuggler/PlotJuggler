#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Regenerate the Chinese UI font embedded in the browser build.

The browser has no system font fallback, so the WebAssembly build embeds a
subset of Noto Sans SC: every character the zh_CN and zh_TW catalogs use, the 3,755
common GB2312 level-1 hanzi (so Chinese series and file names render too),
and CJK punctuation. Rerun after a catalog refresh adds new characters:

  python3 scripts/make_cjk_font_subset.py /path/to/NotoSansSC-Regular.otf

The source font is NotoSansSC-Regular.otf from
https://github.com/notofonts/noto-cjk (Sans/SubsetOTF/SC). Needs fontTools.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import xml.etree.ElementTree as ET

from fontTools import subset

ROOT = Path(__file__).resolve().parents[1]
CATALOGS = [ROOT / f'pj_app/translations/plotjuggler4_{language}.ts' for language in ('zh_CN', 'zh_TW')]
OUTPUT = ROOT / 'resources/fonts/NotoSansSC/NotoSansSC-Subset.otf'


def gb2312_level1() -> str:
    """Rows 16-55 of GB2312 hold the 3,755 most common hanzi."""
    chars = []
    for row in range(0xB0, 0xD8):
        for cell in range(0xA1, 0xFF):
            try:
                chars.append(bytes((row, cell)).decode('gb2312'))
            except UnicodeDecodeError:
                pass
    return ''.join(chars)


def catalog_text() -> str:
    return ''.join(''.join(t.itertext()) for catalog in CATALOGS
                   for t in ET.parse(catalog).getroot().iter('translation'))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('source', type=Path, help='NotoSansSC-Regular.otf')
    args = parser.parse_args()
    punctuation = ''.join(map(chr, range(0x3000, 0x3040))) + ''.join(map(chr, range(0xFF00, 0xFFF0))) + '“”‘’…—·'
    text = set(catalog_text() + gb2312_level1() + punctuation + '中文简体繁體')
    # Latin comes from the bundled Noto Sans, first in the family list.
    text = ''.join(sorted(c for c in text if ord(c) > 0x7F))
    options = subset.Options()
    options.layout_features = ['*']
    options.name_IDs = ['*']
    options.notdef_outline = True
    font = subset.load_font(str(args.source), options)
    subsetter = subset.Subsetter(options)
    subsetter.populate(text=text)
    subsetter.subset(font)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    subset.save_font(font, str(OUTPUT), options)
    print(f'{OUTPUT.relative_to(ROOT)}: {len(text)} characters, {OUTPUT.stat().st_size} bytes')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
