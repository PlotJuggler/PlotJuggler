// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

#include "pj_scene3d_core/finite_range.h"
#include "pj_widgets/Colormap.h"

class QDomElement;
class QFormLayout;
class QObject;
class QWidget;

namespace pj::scene3d {

// The colormap side of a layer that colours by a data channel (GridMapLayer,
// MeshMapLayer): the colormap, auto or manual range, the last auto range, their
// layout attributes and their config rows. The layer keeps its public setters
// (they emit its signals when a setter here reports a change); the channel
// selector stays with the layer, whose channel model differs.
class ChannelColoring {
 public:
  // The largest magnitude a manual range end may have: it is stored as a float
  // uniform.
  static constexpr double kManualRangeLimit = std::numeric_limits<float>::max();

  [[nodiscard]] PJ::Colormap colormap() const {
    return colormap_;
  }
  [[nodiscard]] bool autoRange() const {
    return auto_range_;
  }
  [[nodiscard]] std::pair<double, double> manualRange() const {
    return {manual_lo_, manual_hi_};
  }

  // Each returns true when the state changed. setManualRange ignores a
  // non-finite end, clamps to kManualRangeLimit and orders the pair.
  bool setColormap(PJ::Colormap colormap);
  bool setAutoRange(bool on);
  bool setManualRange(double lo, double hi);
  // The data's finite range, kept to seed the manual scrubbers when auto is
  // switched off. Not persisted.
  void noteAutoRange(const FiniteRange& range) {
    last_auto_range_ = range;
  }
  void resetAutoRange() {
    last_auto_range_ = {};
  }

  // The range the shader maps: the manual one, or `data` (lo == hi == 0 when the
  // data has no finite value, which the shaders paint as the LUT midpoint).
  [[nodiscard]] std::pair<float, float> effectiveRange(const FiniteRange& data) const;

  // Layout attributes `colormap`, `auto_range`, `range_lo`, `range_hi`.
  void saveXml(QDomElement& element) const;
  // Parses those attributes (defaults when absent) into a separate coloring,
  // without applying them; nullopt when one is malformed or out of range, so a
  // layer can reject the whole element before changing anything.
  [[nodiscard]] static std::optional<ChannelColoring> parseXml(const QDomElement& element);

  // Adds the rows "Colors:", "Auto range:", "Range min:", "Range max:" to `form`,
  // initialised from this state. The scrubbers are named `<prefix>_range_min` /
  // `<prefix>_range_max` and the colormap combo `<prefix>_colormap` (stable
  // names: tests and stylesheets find them). A user edit goes through this
  // object's setters and then calls `changed`, only when the state changed.
  // Switching auto off seeds the scrubbers from the last auto range; nudging one
  // scrubber past the other drags it along. `context` owns the connections and
  // must not outlive this object (the layer that holds both).
  void addConfigRows(
      QFormLayout* form, QWidget* parent, QObject* context, const QString& prefix, std::function<void()> changed);

 private:
  PJ::Colormap colormap_ = PJ::Colormap::kTurbo;
  bool auto_range_ = true;
  double manual_lo_ = 0.0;
  double manual_hi_ = 1.0;
  FiniteRange last_auto_range_;
};

}  // namespace pj::scene3d
