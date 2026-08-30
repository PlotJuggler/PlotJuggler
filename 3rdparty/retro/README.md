# Bundled retro runtime (aggregated, not linked)

This directory holds an **independent, separately-licensed** program that
PlotJuggler launches as a child process. PlotJuggler links **none** of it.

## Contents (added at package time, not kept in git)

- `pj-raster-helper` — GPLv2 render helper built from the doomgeneric git
  submodule (`3rdparty/doomgeneric`). It runs as a separate process and renders
  into PlotJuggler through shared-memory IPC.
- `base.wad` — id Software's freely-redistributable **shareware** game data
  (`DOOM1.WAD`), renamed. Redistribution of the unmodified shareware file is
  permitted by its license.

## License artifacts (present in this directory; shipped with the bundle)

- `COPYING` — the full GPLv2 text governing `pj-raster-helper` and the
  doomgeneric engine it links.
- `SOURCE-OFFER.txt` — the written offer of the complete corresponding source
  for the `pj-raster-helper` binary (GPLv2 section 3): `raster_helper/` in this
  repo plus the doomgeneric submodule pinned to a specific upstream commit.
- `SHAREWARE-LICENSE.txt` — id Software's shareware distribution license
  covering `base.wad` (DOOM1.WAD), including id's confirmation that the
  unmodified shareware data is freely redistributable.

Two paths place these next to the helper, and both are conditional on the
helper being built at all: `cmake --install` (root `CMakeLists.txt`,
`if(TARGET pj-raster-helper)`), and `appimage/build_appimage.sh --retro-wad`,
which stages helper + data + licenses into the AppDir — and thereby into both
the AppImage and the `.deb`. They MUST remain present and legible in the
shipped bundle. Only the *trigger* is hidden — the licenses are not.
