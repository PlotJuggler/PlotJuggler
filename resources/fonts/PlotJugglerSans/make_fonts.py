#!/usr/bin/env python3
"""Builds PlotJugglerSans-Regular.ttf and PlotJugglerSans-Bold.ttf from the upstream Noto Sans variable font.

Usage: python3 make_fonts.py NotoSans-Variable.upstream.ttf   (fontTools 4.61; writes next to this script)

Why two static faces, and why the Bold is Noto Sans SemiBold: see README.md. The output is byte-reproducible.
"""
import pathlib
import sys

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

FAMILY = "PlotJuggler Sans"
HERE = pathlib.Path(__file__).resolve().parent


def build(upstream, wght, style):
    font = instantiateVariableFont(TTFont(upstream, recalcTimestamp=False), {"wdth": 100, "wght": wght}, static=True)
    font.recalcTimestamp = False
    del font["STAT"]  # describes the variable font's axes; a static face has none
    font["post"].formatType = 3.0  # no glyph names: Qt never reads them (about 11 KB Brotli per face)
    bold = style == "Bold"
    font["OS/2"].usWeightClass = 700 if bold else 400
    # fsSelection: bit 0 italic, bit 5 bold, bit 6 regular. macStyle: bit 0 bold.
    font["OS/2"].fsSelection = (font["OS/2"].fsSelection & ~0b1100001) | (0b100000 if bold else 0b1000000)
    font["head"].macStyle = 1 if bold else 0
    name = font["name"]
    # Keep upstream's copyright, trademark, designer and licence records; replace the naming ones, and drop the
    # typographic family/style, variations prefix and axis/instance names a renamed static face must not carry.
    name.names = [r for r in name.names if r.platformID == 3 and r.nameID in (0, 5, 7, 8, 9, 11, 12, 13, 14)]
    postscript = FAMILY.replace(" ", "") + "-" + style
    for name_id, value in {
        1: FAMILY,
        2: style,
        3: postscript + ";derived from Noto Sans",
        4: f"{FAMILY} {style}",
        6: postscript,
    }.items():
        name.setName(value, name_id, 3, 1, 0x409)
    font.save(HERE / f"{postscript}.ttf")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    build(sys.argv[1], 400, "Regular")
    build(sys.argv[1], 600, "Bold")


if __name__ == "__main__":
    main()
