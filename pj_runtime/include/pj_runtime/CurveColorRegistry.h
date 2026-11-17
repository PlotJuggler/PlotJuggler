#pragma once

#include <QString>
#include <map>
#include <optional>

namespace PJ {

// Session-scoped memory of the color assigned to each curve, so a curve keeps
// its color when dragged into another plot.
//
// Colors are stored as "#rrggbb" hex strings (the layout-XML form, QColor::name()), so this service stays free of
// Qt6::Gui and pj_runtime keeps its minimal Qt surface; the widget layer (pj_plotting) converts to/from QColor at the
// boundary. Keyed by the opaque catalog key (CurveDescriptor::name) that PlotWidget::addCurve receives. Lifetime is the
// session: clear() runs when the catalog is emptied (data cleared/replaced). Not thread-safe: GUI thread only.
class CurveColorRegistry {
 public:
  [[nodiscard]] std::optional<QString> color(const QString& curve_key) const;

  void setColor(const QString& curve_key, const QString& hex_color);

  // Hands out the next palette index (monotonic, shared across all plots) for a
  // brand-new curve that has no remembered color yet: one app-wide counter, so
  // palette rotation stays consistent between plots.
  [[nodiscard]] int nextPaletteIndex();

  void clear();

 private:
  std::map<QString, QString> colors_;
  int next_palette_index_ = 0;
};

}  // namespace PJ
