# PlotJuggler Sans (UI font)

PlotJuggler Sans is [Noto Sans](https://github.com/notofonts/latin-greek-cyrillic) (OFL-1.1, see `OFL.txt`) shipped as
two static faces:

| File | Registered as | Is |
|---|---|---|
| `PlotJugglerSans-Regular.ttf` | Regular (weight class 400) | Noto Sans Regular, wght 400 |
| `PlotJugglerSans-Bold.ttf` | Bold (weight class 700) | Noto Sans **SemiBold**, wght 600 |

Both faces keep every character (3,094 codepoints), glyph (4,515) and OpenType layout feature of the upstream font.

**Why static faces.** Qt 6.12 registers a variable application font as one Regular face and never moves its weight
axis. So with the variable font, a weight-600 request rendered as Regular, and a weight-700 request rendered as
Regular with synthesized emboldening.

**Why the Bold is 600.** DemiBold is the preferred emphasis. Because the only heavy face is a 600 registered as
Bold, every emphasis renders a real 600, never synthesized bold. That covers `setBold(true)`, `<b>`,
`font-weight: bold`, `font-weight: 600` and `QFont::DemiBold`. Lighter requests render as Regular. The splash asks
for Thin and Light from its own font list (Khula, then Roboto, then the platform default), not from this family.

**Why its own name.** Under the upstream name, a system-installed Noto Sans merged into the family on Linux desktops,
and its own Bold won. OFL-1.1 allows modified versions, and Noto Sans declares no Reserved Font Name, so the font can
be renamed. Renamed, it renders the same everywhere.

**Size.** `make_fonts.py` drops the glyph-name table, which Qt never reads, and its output is byte-reproducible.

## Regenerate

Use the upstream variable font: sha256 `bfb7bb691513f12e734dc346c03a03f784912432d7e3fa8e56efcf906fe86b3d`, first
committed as `resources/fonts/NotoSans/NotoSans-Variable.ttf` in 5071a798. With fontTools 4.61:

```bash
git show 5071a798:resources/fonts/NotoSans/NotoSans-Variable.ttf > /tmp/NotoSans-Variable.upstream.ttf
python3 resources/fonts/PlotJugglerSans/make_fonts.py /tmp/NotoSans-Variable.upstream.ttf
```
