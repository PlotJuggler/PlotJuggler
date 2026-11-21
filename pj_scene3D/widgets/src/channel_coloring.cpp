// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_widgets/channel_coloring.h"

#include <QComboBox>
#include <QDomElement>
#include <QFormLayout>
#include <QObject>
#include <QSignalBlocker>
#include <QWidget>
#include <algorithm>
#include <cmath>

#include "layers/layer_xml_validation.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/ToggleSwitch.h"
using namespace Qt::StringLiterals;

namespace pj::scene3d {

bool ChannelColoring::setColormap(PJ::Colormap colormap) {
  if (colormap_ == colormap) {
    return false;
  }
  colormap_ = colormap;
  return true;
}

bool ChannelColoring::setAutoRange(bool on) {
  if (auto_range_ == on) {
    return false;
  }
  auto_range_ = on;
  return true;
}

bool ChannelColoring::setManualRange(double lo, double hi) {
  if (!std::isfinite(lo) || !std::isfinite(hi)) {
    return false;
  }
  lo = std::clamp(lo, -kManualRangeLimit, kManualRangeLimit);
  hi = std::clamp(hi, -kManualRangeLimit, kManualRangeLimit);
  if (lo > hi) {
    std::swap(lo, hi);
  }
  if (manual_lo_ == lo && manual_hi_ == hi) {
    return false;
  }
  manual_lo_ = lo;
  manual_hi_ = hi;
  return true;
}

std::pair<float, float> ChannelColoring::effectiveRange(const FiniteRange& data) const {
  if (!auto_range_) {
    return {static_cast<float>(manual_lo_), static_cast<float>(manual_hi_)};
  }
  return data.valid ? std::pair{data.lo, data.hi} : std::pair{0.0F, 0.0F};
}

void ChannelColoring::saveXml(QDomElement& element) const {
  element.setAttribute(u"colormap"_s, static_cast<int>(colormap_));
  element.setAttribute(u"auto_range"_s, auto_range_ ? 1 : 0);
  element.setAttribute(u"range_lo"_s, manual_lo_);
  element.setAttribute(u"range_hi"_s, manual_hi_);
}

std::optional<ChannelColoring> ChannelColoring::parseXml(const QDomElement& element) {
  int colormap = 0;
  ChannelColoring out;
  if (!detail::parseBoundedInt(element, "colormap", 0, 0, PJ::kColormapCount - 1, colormap) ||
      !detail::parseZeroOne(element, "auto_range", true, out.auto_range_) ||
      !detail::parseFiniteDouble(element, "range_lo", 0.0, -kManualRangeLimit, kManualRangeLimit, out.manual_lo_) ||
      !detail::parseFiniteDouble(element, "range_hi", 1.0, -kManualRangeLimit, kManualRangeLimit, out.manual_hi_) ||
      out.manual_lo_ > out.manual_hi_) {
    return std::nullopt;
  }
  out.colormap_ = static_cast<PJ::Colormap>(colormap);
  return out;
}

void ChannelColoring::addConfigRows(
    QFormLayout* form, QWidget* parent, QObject* context, const QString& prefix, std::function<void()> changed) {
  auto* colormap_combo = new PJ::ComboBox(parent);
  colormap_combo->setObjectName(prefix + u"_colormap"_s);
  for (int idx = 0; idx < PJ::kColormapCount; ++idx) {
    colormap_combo->addItem(PJ::colormapDisplayName(static_cast<PJ::Colormap>(idx)), idx);
  }
  colormap_combo->setCurrentIndex(static_cast<int>(colormap_));
  form->addRow(QObject::tr("Colors:"), colormap_combo);
  QObject::connect(
      colormap_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), context, [this, changed](int idx) {
        if (setColormap(idx >= 0 && idx < PJ::kColormapCount ? static_cast<PJ::Colormap>(idx) : PJ::Colormap::kTurbo)) {
          changed();
        }
      });

  auto* auto_toggle = new PJ::ToggleSwitch(parent);
  auto_toggle->setChecked(auto_range_, /*animate=*/false);
  form->addRow(QObject::tr("Auto range:"), auto_toggle);

  const auto add_scrubber = [this, form, parent](const QString& label, const QString& object_name, double value) {
    auto* spin = new PJ::DoubleScrubber(parent);
    spin->setObjectName(object_name);
    spin->setRange(-1.0e9, 1.0e9);
    spin->setDecimals(3);
    spin->setValue(value);
    spin->setEnabled(!auto_range_);
    form->addRow(label, spin);
    return spin;
  };
  auto* range_lo_spin = add_scrubber(QObject::tr("Range min:"), prefix + u"_range_min"_s, manual_lo_);
  auto* range_hi_spin = add_scrubber(QObject::tr("Range max:"), prefix + u"_range_max"_s, manual_hi_);

  QObject::connect(
      auto_toggle, &PJ::ToggleSwitch::toggled, context, [this, changed, range_lo_spin, range_hi_spin](bool on) {
        // Switching to manual starts from the range the user was just looking at.
        bool edited = false;
        if (!on && last_auto_range_.valid) {
          const QSignalBlocker block_lo(range_lo_spin);
          const QSignalBlocker block_hi(range_hi_spin);
          range_lo_spin->setValue(last_auto_range_.lo);
          range_hi_spin->setValue(last_auto_range_.hi);
          edited = setManualRange(last_auto_range_.lo, last_auto_range_.hi);
        }
        edited = setAutoRange(on) || edited;
        range_lo_spin->setEnabled(!on);
        range_hi_spin->setEnabled(!on);
        if (edited) {
          changed();
        }
      });
  // Keep min <= max: nudging one scrubber past the other drags the sibling along
  // (its signal blocked so it does not re-enter here), then pushes the pair once.
  QObject::connect(
      range_lo_spin, &PJ::DoubleScrubber::valueChanged, context,
      [this, changed, range_lo_spin, range_hi_spin](double v) {
        if (v > range_hi_spin->value()) {
          const QSignalBlocker block(range_hi_spin);
          range_hi_spin->setValue(v);
        }
        if (setManualRange(range_lo_spin->value(), range_hi_spin->value())) {
          changed();
        }
      });
  QObject::connect(
      range_hi_spin, &PJ::DoubleScrubber::valueChanged, context,
      [this, changed, range_lo_spin, range_hi_spin](double v) {
        if (v < range_lo_spin->value()) {
          const QSignalBlocker block(range_lo_spin);
          range_lo_spin->setValue(v);
        }
        if (setManualRange(range_lo_spin->value(), range_hi_spin->value())) {
          changed();
        }
      });
}

}  // namespace pj::scene3d
