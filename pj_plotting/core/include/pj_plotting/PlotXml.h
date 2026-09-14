#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomElement>
#include <QLatin1StringView>

namespace PJ::plot_xml {

/// Plot-range attribute that declares the coordinate system of its X edges.
inline constexpr QLatin1StringView kXBasisAttribute("x_basis");
/// Time-series range edges expressed in the saved absolute-time frame.
inline constexpr QLatin1StringView kXBasisAbsolute("absolute");
/// XY-plot range edges expressed directly as data values.
inline constexpr QLatin1StringView kXBasisValue("value");

/// Marks an unresolved <curve> as real workspace intent: the pending binder
/// fulfills it when its topic materializes, and no save/strip/prompt pass may
/// discard it. One predicate/writer pair so the keep-alive semantics cannot
/// drift between the layout passes, the binder, and the plot widget.
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

}  // namespace PJ::plot_xml
