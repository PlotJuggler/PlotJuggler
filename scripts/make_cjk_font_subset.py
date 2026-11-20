#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Regenerate the Chinese UI font embedded in the browser build.

The browser has no system font fallback, so the WebAssembly build embeds a
subset of Noto Sans SC: every character the zh_CN and zh_TW catalogs use, the 3,755
common GB2312 level-1 hanzi (so Chinese series and file names render too),
and CJK punctuation. Rerun after a catalog refresh adds new characters:

  python3 scripts/make_cjk_font_subset.py /path/to/NotoSansSC-Regular.otf \
      --symbols-source /path/to/NotoSansSymbols2-Regular.ttf

The source font is NotoSansSC-Regular.otf from
https://github.com/notofonts/noto-cjk (Sans/SubsetOTF/SC). Needs fontTools.
"""
from __future__ import annotations

import argparse
import unicodedata
from pathlib import Path
import xml.etree.ElementTree as ET

from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
CATALOGS = [ROOT / f'pj_app/translations/plotjuggler4_{language}.ts' for language in ('zh_CN', 'zh_TW')]
OUTPUT = ROOT / 'resources/fonts/NotoSansSC/NotoSansSC-Subset.otf'
SYMBOL_OUTPUT = ROOT / 'resources/fonts/NotoSansSymbols2/NotoSansSymbols2-Subset.ttf'
LATIN = ROOT / 'resources/fonts/PlotJugglerSans/PlotJugglerSans-Regular.ttf'


def cmap(path: Path) -> set[int]:
    with TTFont(path) as font:
        return set(font.getBestCmap())


def required_codepoints(text: str) -> set[int]:
    # Control/format characters affect layout, not glyph coverage. Keep all
    # printable catalog characters (including unassigned ones if actually used).
    return {ord(c) for c in text if unicodedata.category(c) not in ('Cc', 'Cf')}


def check_coverage(text: str, fonts: list[Path]) -> None:
    covered = set().union(*(cmap(path) for path in fonts))
    missing = required_codepoints(text) - covered
    if missing:
        raise ValueError('Missing catalog glyphs: ' + ', '.join(
            f'U+{cp:04X} {chr(cp)}' for cp in sorted(missing)))


def save_subset(source: Path, output: Path, codepoints: set[int]) -> None:
    options = subset.Options()
    options.ignore_missing_unicodes = False
    options.layout_features = ['*']
    options.name_IDs = ['*']
    options.notdef_outline = True
    font = subset.load_font(str(source), options)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=codepoints)
    subsetter.subset(font)
    output.parent.mkdir(parents=True, exist_ok=True)
    subset.save_font(font, str(output), options)
    print(f'{output.relative_to(ROOT)}: {len(cmap(output))} covered codepoints, '
          f'{output.stat().st_size} bytes')


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
    parser.add_argument('source', nargs='?', type=Path, help='NotoSansSC-Regular.otf')
    parser.add_argument('--symbols-source', type=Path, help='NotoSansSymbols2-Regular.ttf')
    parser.add_argument('--check', action='store_true', help='Check committed fonts against current catalogs')
    args = parser.parse_args()
    catalog = catalog_text()
    if args.check:
        check_coverage(catalog, [LATIN, OUTPUT, SYMBOL_OUTPUT])
        print('All printable catalog characters covered by bundled fonts')
        return 0
    if args.source is None or args.symbols_source is None:
        parser.error('generation requires source and --symbols-source')
    punctuation = ''.join(map(chr, range(0x3000, 0x3040))) + ''.join(map(chr, range(0xFF00, 0xFFF0))) + '“”‘’…—·'
    source_cmap = cmap(args.source)
    symbols_cmap = cmap(args.symbols_source)
    check_coverage(catalog, [LATIN, args.source, args.symbols_source])
    required = required_codepoints(catalog + gb2312_level1() + '中文简体繁體')
    # Optional ranges contain unassigned scalars and Hangul filler; they are not
    # catalog requirements. Never silently discard a required glyph.
    requested = required | {ord(c) for c in punctuation}
    save_subset(args.source, OUTPUT, {cp for cp in requested & source_cmap if cp > 0x7F})
    save_subset(args.symbols_source, SYMBOL_OUTPUT, required - cmap(LATIN) - source_cmap)
    check_coverage(catalog, [LATIN, OUTPUT, SYMBOL_OUTPUT])
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
