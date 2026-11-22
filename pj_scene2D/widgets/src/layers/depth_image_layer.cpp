// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/layers/depth_image_layer.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <algorithm>
#include <limits>
#include <memory>
#include <optional>

#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/XmlStateOptions.h"
#include "pj_scene2d_core/media_source.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SvgButton.h"
using namespace Qt::StringLiterals;

namespace PJ {

DepthImageLayer::DepthImageLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name, QObject* parent)
    : Scene2DLayer(topic_id, object_type, display_name, u"Depth"_s, parent) {}

QWidget* DepthImageLayer::createConfigWidget(QWidget* parent) {
  auto* widget = new QWidget(parent);
  auto* layout = new QFormLayout(widget);
  layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));

  auto* colormap_row = new QWidget(widget);
  auto* colormap_row_layout = new QHBoxLayout(colormap_row);
  colormap_row_layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  colormap_row_layout->setSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));

  auto* colormap = new QComboBox(colormap_row);
  colormap->addItem(tr("Turbo"), static_cast<int>(Colormap::kTurbo));
  colormap->addItem(tr("Viridis"), static_cast<int>(Colormap::kViridis));
  colormap->addItem(tr("Plasma"), static_cast<int>(Colormap::kPlasma));
  colormap->addItem(tr("Grayscale"), static_cast<int>(Colormap::kGrayscale));
  colormap->setCurrentIndex(colormap->findData(static_cast<int>(options_.colormap)));
  colormap_row_layout->addWidget(colormap, 1);

  auto* invert_btn = new PJ::SvgButton(u":/resources/svg/invert.svg"_s, PJ::SvgButton::Size::kDefault, colormap_row);
  invert_btn->setCheckable(true);
  invert_btn->setChecked(options_.invert);
  invert_btn->setExtent(24, 20);
  invert_btn->setToolTip(tr("Invert colormap"));
  colormap_row_layout->addWidget(invert_btn, 0);

  layout->addRow(tr("Colormap"), colormap_row);

  auto* near_spin = new PJ::DoubleScrubber(widget);
  near_spin->setRange(0.0, 100000.0);
  near_spin->setDecimals(3);
  near_spin->setSingleStep(0.1);
  near_spin->setSuffix(u" m"_s);
  near_spin->setValue(options_.near_m);
  layout->addRow(tr("Near"), near_spin);

  auto* far_spin = new PJ::DoubleScrubber(widget);
  far_spin->setRange(0.0, 100000.0);
  far_spin->setDecimals(3);
  far_spin->setSingleStep(0.1);
  far_spin->setSuffix(u" m"_s);
  far_spin->setValue(options_.far_m);
  layout->addRow(tr("Far"), far_spin);

  // One-shot fit to the current frame's depth percentiles; a continuous auto range flickered frame to frame.
  auto* autofit_btn = new QPushButton(tr("Auto-fit range"), widget);
  autofit_btn->setFocusPolicy(Qt::NoFocus);
  autofit_btn->setToolTip(tr("Set Near/Far from the current frame's depth distribution (2–98%)"));
  layout->addRow(autofit_btn);

  connect(colormap, &QComboBox::currentIndexChanged, this, [this, colormap](int) {
    setColormap(static_cast<Colormap>(colormap->currentData().toInt()));
  });
  connect(invert_btn, &PJ::SvgButton::toggled, this, [this](bool on) { setInvert(on); });
  connect(near_spin, &PJ::DoubleScrubber::valueChanged, this, [this, far_spin](double value) {
    setRange(static_cast<float>(value), static_cast<float>(far_spin->value()));
  });
  connect(far_spin, &PJ::DoubleScrubber::valueChanged, this, [this, near_spin](double value) {
    setRange(static_cast<float>(near_spin->value()), static_cast<float>(value));
  });
  connect(autofit_btn, &QPushButton::clicked, this, [this, near_spin, far_spin]() {
    if (depth_source_ == nullptr) {
      return;  // not attached / no decode yet
    }
    const auto range = depth_source_->autoRange();
    if (!range.has_value()) {
      return;  // no depth frame decoded yet, or no valid samples
    }
    {
      const QSignalBlocker block_near(near_spin);
      const QSignalBlocker block_far(far_spin);
      near_spin->setValue(range->min);
      far_spin->setValue(range->max);
    }
    setRange(range->min, range->max);  // applies to the source + repaints
  });

  return widget;
}

std::unique_ptr<MediaSource> DepthImageLayer::createMediaSource(const SceneLayerContext& ctx) {
  auto* session = ctx.session;
  auto* store = objectStore();
  if (session == nullptr || store == nullptr) {
    depth_source_ = nullptr;
    return nullptr;
  }

  const auto binding = session->parserBindingForObjectTopic(topicId());
  std::unique_ptr<DepthPipelineSource> source;
  if (binding.parser != nullptr) {
    source = std::make_unique<DepthPipelineSource>(store, topicId(), binding.parser, binding.mutex, binding.keepalive);
  } else {
    source = std::make_unique<DepthPipelineSource>(store, topicId());
  }
  depth_source_ = source.get();
  applyTo(*source);
  source->setFrameReadyCallback(makeQueuedRepaintCallback());
  return source;
}

void DepthImageLayer::onBeforeDetach() {
  depth_source_ = nullptr;
}

void DepthImageLayer::applyTo(DepthPipelineSource& source) const {
  source.setColormap(static_cast<uint8_t>(options_.colormap));  // core takes an opaque colormap id
  source.setInvert(options_.invert);
  source.setRange(options_.near_m, options_.far_m);
  source.setOpacity(options_.opacity);
}

void DepthImageLayer::saveOptions(QDomElement& element) const {
  element.setAttribute(u"colormap"_s, colormapToken(options_.colormap));
  element.setAttribute(u"invert"_s, options_.invert ? u"true"_s : u"false"_s);
  element.setAttribute(u"near_m"_s, QString::number(options_.near_m, 'g', 9));
  element.setAttribute(u"far_m"_s, QString::number(options_.far_m, 'g', 9));
  element.setAttribute(u"opacity"_s, QString::number(options_.opacity, 'g', 9));
}

std::optional<DepthImageLayer::Options> DepthImageLayer::parseOptions(
    const QDomElement& element, const Options& current) {
  Options parsed = current;

  // Tolerated: an absent colormap keeps the current one; an empty or unknown token (e.g. "jet") reads as turbo.
  const QString colormap_key = u"colormap"_s;
  QString colormap = colormapToken(element.hasAttribute(colormap_key) ? Colormap::kTurbo : current.colormap);
  (void)readXmlEnum(element, colormap_key, colormapTokens(), colormap, colormap, XmlBadValue::kTolerate);
  parsed.colormap = parseColormap(colormap).value_or(Colormap::kTurbo);

  constexpr float kLowest = std::numeric_limits<float>::lowest();
  constexpr float kMax = std::numeric_limits<float>::max();
  bool ok = readXmlBool(element, u"invert"_s, current.invert, parsed.invert);
  ok = readXmlFloat(element, u"near_m"_s, current.near_m, kLowest, kMax, parsed.near_m) && ok;
  ok = readXmlFloat(element, u"far_m"_s, current.far_m, kLowest, kMax, parsed.far_m) && ok;
  const QString opacity_key = u"opacity"_s;
  if (XmlOptionsRecorder::active()) {
    // Reported with its [0, 1] bounds ahead of the unbounded read below: a finite value outside them is clamped, not
    // refused.
    const XmlOption opacity{
        .attribute = opacity_key,
        .type = u"float"_s,
        .min = 0.0,
        .max = 1.0,
        .fallback = QString::number(current.opacity, 'g', 7)};
    XmlOptionsRecorder::noteOption(element, opacity);
    if (const QString problem =
            element.hasAttribute(opacity_key) ? xmlValueProblem(opacity, element.attribute(opacity_key)) : QString();
        !problem.isEmpty()) {
      XmlOptionsRecorder::noteRejection(element, opacity_key, problem);
    }
  }
  ok = readXmlFloat(element, opacity_key, current.opacity, kLowest, kMax, parsed.opacity) && ok;
  if (!ok) {
    return std::nullopt;
  }
  if (parsed.far_m < parsed.near_m) {
    // Tolerated: the two swap.
    XmlOptionsRecorder::noteRejection(element, u"far_m"_s, u"expected far_m >= near_m"_s);
    std::swap(parsed.near_m, parsed.far_m);
  }
  parsed.opacity = std::clamp(parsed.opacity, 0.0f, 1.0f);
  return parsed;
}

bool DepthImageLayer::loadOptions(const QDomElement& element) {
  const std::optional<Options> restored = parseOptions(element, options_);
  if (!restored.has_value()) {
    return false;
  }
  const bool changed = options_ != *restored;
  options_ = *restored;
  if (changed) {
    applyOptions();
  }
  return true;
}

void DepthImageLayer::applyOptions() {
  if (depth_source_ == nullptr) {
    return;
  }
  applyTo(*depth_source_);
  // The params travel on the frame: re-applying the tracker time re-emits it with them now.
  if (const auto ts = lastTrackerTimeNs(); ts.has_value()) {
    depth_source_->setTimestamp(*ts);
  }
  emit repaintRequested();
}

void DepthImageLayer::setColormap(Colormap colormap) {
  if (options_.colormap == colormap) {
    return;
  }
  options_.colormap = colormap;
  applyOptions();
  emit configurationChanged();
}

void DepthImageLayer::setInvert(bool invert) {
  if (options_.invert == invert) {
    return;
  }
  options_.invert = invert;
  applyOptions();
  emit configurationChanged();
}

void DepthImageLayer::setRange(float near_m, float far_m) {
  if (far_m < near_m) {
    std::swap(near_m, far_m);
  }
  if (options_.near_m == near_m && options_.far_m == far_m) {
    return;
  }
  options_.near_m = near_m;
  options_.far_m = far_m;
  applyOptions();
  emit configurationChanged();
}

}  // namespace PJ
