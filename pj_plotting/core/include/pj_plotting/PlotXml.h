#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QDomElement>
#include <QDomNodeList>
#include <QLatin1StringView>
#include <QString>
#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <tuple>
#include <type_traits>
#include <vector>

namespace PJ::plot_xml {

/// Plot-range attribute that declares the coordinate system of its X edges.
inline constexpr QLatin1StringView kXBasisAttribute("x_basis");
/// Time-series range edges expressed in the saved absolute-time frame.
inline constexpr QLatin1StringView kXBasisAbsolute("absolute");
/// XY-plot range edges expressed directly as data values.
inline constexpr QLatin1StringView kXBasisValue("value");

/// Marks an unresolved <curve> as workspace intent: the pending binder fulfills it when its topic materializes, and no
/// save, strip or prompt pass may discard it. One predicate/writer pair keeps the semantics identical across the
/// layout passes, the binder and the plot widget.
inline constexpr QLatin1StringView kPendingIntentAttribute("pending_intent");
inline constexpr QLatin1StringView kPendingIntentTrue("true");

[[nodiscard]] inline bool isPendingIntent(const QDomElement& curve) {
  return curve.attribute(kPendingIntentAttribute) == kPendingIntentTrue;
}

inline void markPendingIntent(QDomElement& curve) {
  curve.setAttribute(kPendingIntentAttribute, kPendingIntentTrue);
}

/// <marker_scope> attribute naming which of a dataset's marker sets the entry is
/// about. Its value is the persisted vocabulary below, not the `MarkerScope` enum
/// name, so the enum can be renamed without invalidating saved layouts.
inline constexpr QLatin1StringView kMarkerScopeAttribute("scope");
inline constexpr QLatin1StringView kMarkerScopeDataset("dataset");
inline constexpr QLatin1StringView kMarkerScopeAllDatasets("global");

/// The colours a plot gives new curves, in order.
[[nodiscard]] inline const std::array<QColor, 8>& curvePalette() {
  static const std::array<QColor, 8> colors = {
      QColor("#1f77b4"), QColor("#d62728"), QColor("#1ac938"), QColor("#ff7f0e"),
      QColor("#f14cc1"), QColor("#9467bd"), QColor("#17becf"), QColor("#bcbd22"),
  };
  return colors;
}

[[nodiscard]] inline QColor paletteColor(int index) {
  return curvePalette()[static_cast<std::size_t>(index) % curvePalette().size()];
}

/// The palette colour rule for a plot's next curve, deliberately in two halves (`worn` lists the colours the plot's
/// curves wear, one entry per curve; colours outside the palette take no slot):
///  - while some palette colour is worn by no curve, the next such colour in the caller's sequence (`next_index` is
///    drawn until it names one, so a session-wide counter skips the colours this plot holds);
///  - once every palette colour is worn, colours repeat: the least-worn one, the lowest index among ties, so the
///    repeats spread evenly instead of all landing on one colour. `next_index` is not drawn then.
[[nodiscard]] inline int freePaletteIndex(const std::function<int()>& next_index, const std::vector<QRgb>& worn) {
  constexpr std::size_t kSize = std::tuple_size_v<std::remove_reference_t<decltype(curvePalette())>>;
  std::array<int, kSize> uses{};
  for (const QRgb rgb : worn) {
    for (std::size_t slot = 0; slot < kSize; ++slot) {
      if (curvePalette()[slot].rgb() == rgb) {
        ++uses[slot];
        break;
      }
    }
  }
  const auto least_used = std::min_element(uses.begin(), uses.end());
  const int least_used_index = static_cast<int>(least_used - uses.begin());
  if (*least_used > 0) {
    return least_used_index;
  }
  for (std::size_t tried = 0; tried < kSize; ++tried) {
    const int index = next_index();
    if (uses[static_cast<std::size_t>(index) % kSize] == 0) {
      return index;
    }
  }
  return least_used_index;
}

/// Gives every <curve> of `plot` without a colour the one the plot would give it when it is added (PlotWidget::addCurve
/// with "per plot" colours): `remembered(curve)` when set, else freePaletteIndex's pick, drawing from the curve's
/// position. Every copy of a filled plot then shows each curve in the same colour.
inline void fillMissingCurveColors(
    QDomElement& plot, const std::function<std::optional<QColor>(const QDomElement& curve)>& remembered = {}) {
  std::vector<QRgb> worn;
  for (QDomElement curve = plot.firstChildElement(QStringLiteral("curve")); !curve.isNull();
       curve = curve.nextSiblingElement(QStringLiteral("curve"))) {
    if (const QColor color(curve.attribute(QStringLiteral("color"))); color.isValid()) {
      worn.push_back(color.rgb());
    }
  }
  int position = 0;
  for (QDomElement curve = plot.firstChildElement(QStringLiteral("curve")); !curve.isNull();
       curve = curve.nextSiblingElement(QStringLiteral("curve")), ++position) {
    if (QColor(curve.attribute(QStringLiteral("color"))).isValid()) {
      continue;
    }
    std::optional<QColor> color = remembered ? remembered(curve) : std::nullopt;
    if (!color.has_value()) {
      int next = position;
      color = paletteColor(freePaletteIndex([&next] { return next++; }, worn));
    }
    curve.setAttribute(QStringLiteral("color"), color->name());
    worn.push_back(color->rgb());
  }
}

/// Makes a saved <Tab> a copy that can restore beside its original: the copy is the user's (no plugin ownership), and
/// each id of the tab, its areas and its plots becomes `new_id(id)`, or is dropped (the restore mints a fresh one) when
/// `new_id` is empty.
inline void prepareTabCopy(QDomElement& tab, const std::function<QString(const QString& id)>& new_id = {}) {
  for (const QString& attribute :
       {QStringLiteral("history_exempt"), QStringLiteral("owner_plugin"), QStringLiteral("tab_id")}) {
    tab.removeAttribute(attribute);
  }
  std::vector<QDomElement> elements{tab};
  for (const QString& tag : {QStringLiteral("DockArea"), QStringLiteral("plot")}) {
    const QDomNodeList nodes = tab.elementsByTagName(tag);
    for (int index = 0; index < nodes.size(); ++index) {
      elements.push_back(nodes.at(index).toElement());
    }
  }
  for (QDomElement& element : elements) {
    const QString id = element.attribute(QStringLiteral("id"));
    if (new_id && !id.isEmpty()) {
      element.setAttribute(QStringLiteral("id"), new_id(id));
    } else {
      element.removeAttribute(QStringLiteral("id"));
    }
  }
}

}  // namespace PJ::plot_xml
