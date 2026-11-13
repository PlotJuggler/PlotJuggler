// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>
#include <functional>
#include <optional>
#include <utility>

#include "pj_scene3d_core/grid_map_view.h"  // FiniteRange
#include "pj_widgets/Colormap.h"

class QDomElement;
class QFormLayout;
class QObject;
class QWidget;

namespace pj::scene3d {

// The colormap side of a layer that colours by a data channel (GridMapLayer,
// MeshMapLayer): the colormap, auto or manual range, the last auto range, their
// layout attributes and their config rows. The layer keeps its public setters
// (they emit its signals) and delegates the state here; the channel selector
// stays with the layer, whose channel model differs.
class ChannelColoring {
 public:
  // The largest magnitude a manual range end may have: it is stored as a float
  // uniform.
  static constexpr double kManualRangeLimit = 3.4028234663852886e38;

  [[nodiscard]] PJ::Colormap colormap() const {
    return colormap_;
  }
  [[nodiscard]] bool autoRange() const {
    return auto_range_;
  }
  [[nodiscard]] std::pair<double, double> manualRange() const {
    return {manual_lo_, manual_hi_};
  }
  [[nodiscard]] const FiniteRange& lastAutoRange() const {
    return last_auto_range_;
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
  struct Persisted {
    PJ::Colormap colormap = PJ::Colormap::kTurbo;
    bool auto_range = true;
    double range_lo = 0.0;
    double range_hi = 1.0;
  };
  // Parses those attributes (defaults when absent) without applying them;
  // nullopt when one is malformed or out of range, so a layer can reject the
  // whole element before changing anything.
  [[nodiscard]] static std::optional<Persisted> parseXml(const QDomElement& element);

  // What the config rows call on a user edit: the layer's setters.
  struct Setters {
    std::function<void(PJ::Colormap)> colormap;
    std::function<void(bool)> auto_range;
    std::function<void(double, double)> manual_range;
  };
  // Adds the rows "Colors:", "Auto range:", "Range min:", "Range max:" to `form`,
  // initialised from this state. The scrubbers are named `<prefix>_range_min` /
  // `<prefix>_range_max` and the colormap combo `<prefix>_colormap` (stable
  // names: tests and stylesheets find them). Switching auto off seeds the
  // scrubbers from the last auto range; nudging one scrubber past the other drags
  // it along. `context` owns the connections.
  void addConfigRows(
      QFormLayout* form, QWidget* parent, QObject* context, const QString& prefix, const Setters& setters) const;

 private:
  PJ::Colormap colormap_ = PJ::Colormap::kTurbo;
  bool auto_range_ = true;
  double manual_lo_ = 0.0;
  double manual_hi_ = 1.0;
  FiniteRange last_auto_range_;
};

}  // namespace pj::scene3d
