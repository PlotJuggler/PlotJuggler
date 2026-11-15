#pragma once

#include <QString>

namespace PJ {

/// Registers the bundled PlotJuggler Sans faces (Regular, and a Bold that is a 600 — see
/// resources/fonts/PlotJugglerSans/README.md) found in `directory`, and returns the family name the fonts declare,
/// or an empty string when the Regular face cannot be registered. Warns about every face that fails to load.
QString registerBundledFont(const QString& directory);

}  // namespace PJ
