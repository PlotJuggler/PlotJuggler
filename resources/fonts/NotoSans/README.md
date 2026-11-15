# Noto Sans (UI font)

`NotoSans-Variable.ttf` is the upstream Noto Sans variable font
([notofonts/latin-greek-cyrillic](https://github.com/notofonts/latin-greek-cyrillic), OFL-1.1, see `OFL.txt`) with
its **width axis pinned** at the default (wdth 100), because the app never sets a font stretch, and its **weight axis
capped at Bold** (wght 100–700), because nothing uses ExtraBold or Black. The app uses Thin and Light (splash
wordmark), Regular, DemiBold (the preferred emphasis) and occasionally Bold. Everything else is untouched: every
character (3,094 codepoints) and glyph (4,515), and every OpenType layout feature.

The font is compiled into every build through `resources/resources.qrc`, so its size is part of the browser
download: pinning the width axis and capping the weight axis take it from 919 KB to 491 KB Brotli. OFL-1.1 allows
modified versions, and this font declares no Reserved Font Name, so it keeps the name.

Regenerate from the upstream file (sha256 `bfb7bb691513f12e734dc346c03a03f784912432d7e3fa8e56efcf906fe86b3d`, as
first committed in 5071a798) with fontTools 4.61:

```bash
python3 -m fontTools.varLib.instancer NotoSans-Variable.upstream.ttf wdth=100 wght=100:700 -o NotoSans-Variable.ttf
```
