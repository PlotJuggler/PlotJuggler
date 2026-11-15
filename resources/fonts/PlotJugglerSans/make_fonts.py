#!/usr/bin/env python3
"""Builds PlotJugglerSans-Regular.ttf and PlotJugglerSans-Bold.ttf from the upstream Noto Sans variable font.

Usage: python3 make_fonts.py NotoSans-Variable.upstream.ttf   (fontTools 4.61; writes next to this script)

Qt registers a variable application font as one Regular face and never moves its weight axis, so weights must ship
as static faces. "Bold" is Noto Sans SemiBold (wght 600) registered as the family's Bold (weight class 700): every
setBold(true), <b>, font-weight:bold and QFont::DemiBold request then renders a real 600, never synthesized bold.
"""
import pathlib
import sys

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

FAMILY = "PlotJuggler Sans"
HERE = pathlib.Path(__file__).resolve().parent


def build(upstream, wght, style):
    font = instantiateVariableFont(TTFont(upstream), {"wdth": 100, "wght": wght}, static=True)
    bold = style == "Bold"
    font["OS/2"].usWeightClass = 700 if bold else 400
    # fsSelection: bit 0 italic, bit 5 bold, bit 6 regular. macStyle: bit 0 bold.
    font["OS/2"].fsSelection = (font["OS/2"].fsSelection & ~0b1100001) | (0b100000 if bold else 0b1000000)
    font["head"].macStyle = 1 if bold else 0
    name = font["name"]
    copyright_notice = name.getDebugName(0)
    name.names = []
    postscript = FAMILY.replace(" ", "") + "-" + style
    for name_id, value in {
        0: copyright_notice,
        1: FAMILY,
        2: style,
        3: postscript + ";derived from Noto Sans",
        4: f"{FAMILY} {style}",
        5: "Version 1.000",
        6: postscript,
    }.items():
        name.setName(value, name_id, 3, 1, 0x409)
    font.save(HERE / f"{postscript}.ttf")


build(sys.argv[1], 400, "Regular")
build(sys.argv[1], 600, "Bold")
