// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/layers/depth_image_layer.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QPalette>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <algorithm>
#include <cmath>
#include <memory>

#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_core/media_source.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
// Persisted lowercase token (layout XML), distinct from the display name PJ::colormapName().
QString colormapToken(Colormap colormap) {
  switch (colormap) {
    case Colormap::kTurbo:
      return u"turbo"_s;
    case Colormap::kViridis:
      return u"viridis"_s;
    case Colormap::kPlasma:
      return u"plasma"_s;
    case Colormap::kGrayscale:
      return u"grayscale"_s;
  }
  return u"turbo"_s;
}

Colormap parseColormap(const QString& value) {
  if (value == "viridis"_L1) {
    return Colormap::kViridis;
  }
  if (value == "plasma"_L1) {
    return Colormap::kPlasma;
  }
  if (value == "grayscale"_L1) {
    return Colormap::kGrayscale;
  }
  return Colormap::kTurbo;  // also maps legacy "jet" layouts to the default.
}

QString qssColor(const QColor& color) {
  if (!color.isValid()) {
    return QStringLiteral("transparent");
  }
  return QStringLiteral("rgba(%1, %2, %3, %4)")
      .arg(color.red())
      .arg(color.green())
      .arg(color.blue())
      .arg(color.alpha());
}

PJ::theme::Theme widgetTheme(const QWidget* widget) {
  (void)widget;
  return PJ::theme::appTheme();
}

QString toggleButtonQss(PJ::theme::Theme active_theme) {
  // A button is the Secondary interaction fill per state — no border.
  return QStringLiteral(
             "QPushButton { border: none; border-radius: %1px; padding: %2px %3px; background-color: %4; }"
             "QPushButton:hover { background-color: %5; }"
             "QPushButton:checked { background-color: %6; }")
      .arg(PJ::theme::radius(PJ::theme::Radius::Input, active_theme))
      .arg(PJ::theme::space(PJ::theme::Space::Tight, active_theme))
      .arg(PJ::theme::space(PJ::theme::Space::Comfortable, active_theme))
      .arg(qssColor(PJ::theme::interaction(PJ::theme::Variant::Accent, PJ::theme::State::Nominal, active_theme)))
      .arg(qssColor(PJ::theme::interaction(PJ::theme::Variant::Accent, PJ::theme::State::Hovered, active_theme)))
      .arg(qssColor(PJ::theme::interaction(PJ::theme::Variant::Accent, PJ::theme::State::Checked, active_theme)));
}
}  // namespace

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
  colormap->setCurrentIndex(colormap->findData(static_cast<int>(colormap_)));
  colormap_row_layout->addWidget(colormap, 1);

  auto* invert_btn = new QPushButton(colormap_row);
  invert_btn->setCheckable(true);
  invert_btn->setChecked(invert_);
  invert_btn->setFocusPolicy(Qt::NoFocus);
  invert_btn->setStyleSheet(toggleButtonQss(widgetTheme(widget)));
  invert_btn->setIcon(QIcon(QStringLiteral(":/resources/svg/invert.svg")));
  invert_btn->setIconSize(QSize(20, 20));
  // Match the standard icon-button extent used across the app (icon 20 + padding 4).
  invert_btn->setFixedSize(24, 24);
  invert_btn->setToolTip(tr("Invert colormap"));
  colormap_row_layout->addWidget(invert_btn, 0);

  layout->addRow(tr("Colormap"), colormap_row);

  auto* near_spin = new PJ::DoubleScrubber(widget);
  near_spin->setRange(0.0, 100000.0);
  near_spin->setDecimals(3);
  near_spin->setSingleStep(0.1);
  near_spin->setSuffix(u" m"_s);
  near_spin->setValue(near_m_);
  layout->addRow(tr("Near"), near_spin);

  auto* far_spin = new PJ::DoubleScrubber(widget);
  far_spin->setRange(0.0, 100000.0);
  far_spin->setDecimals(3);
  far_spin->setSingleStep(0.1);
  far_spin->setSuffix(u" m"_s);
  far_spin->setValue(far_m_);
  layout->addRow(tr("Far"), far_spin);

  // On-demand range fit: snap near/far to the current frame's 2nd/98th depth
  // percentiles. Manual remains the default (continuous auto flickered per frame);
  // this is a one-shot the user triggers when the default range is unhelpful.
  auto* autofit_btn = new QPushButton(tr("Auto-fit range"), widget);
  autofit_btn->setFocusPolicy(Qt::NoFocus);
  autofit_btn->setToolTip(tr("Set Near/Far from the current frame's depth distribution (2–98%)"));
  layout->addRow(autofit_btn);

  connect(colormap, &QComboBox::currentIndexChanged, this, [this, colormap](int) {
    setColormap(static_cast<Colormap>(colormap->currentData().toInt()));
  });
  connect(invert_btn, &QPushButton::toggled, this, [this](bool on) { setInvert(on); });
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

  // Real depth (e.g. ROS sensor_msgs/Image) is decoded via the topic's parser,
  // exactly like ImageLayer; only canonical pj_image_v1 producers have no parser.
  const auto binding = session->parserBindingForObjectTopic(topicId());
  std::unique_ptr<DepthPipelineSource> source;
  if (binding.parser != nullptr) {
    source = std::make_unique<DepthPipelineSource>(store, topicId(), binding.parser, binding.mutex, binding.keepalive);
  } else {
    source = std::make_unique<DepthPipelineSource>(store, topicId());
  }
  depth_source_ = source.get();
  applyTo(*source);
  // The decode runs on a worker thread now; repaint when a frame is ready.
  source->setFrameReadyCallback(makeQueuedRepaintCallback());
  return source;
}

void DepthImageLayer::onBeforeDetach() {
  depth_source_ = nullptr;
}

void DepthImageLayer::applyTo(DepthPipelineSource& source) const {
  source.setColormap(static_cast<uint8_t>(colormap_));  // core takes an opaque colormap id
  source.setInvert(invert_);
  source.setRange(near_m_, far_m_);  // manual default; Auto-fit updates near_m_/far_m_, then re-applies here
  source.setOpacity(opacity_);
}

void DepthImageLayer::saveOptions(QDomElement& element) const {
  element.setAttribute(u"colormap"_s, colormapToken(colormap_));
  element.setAttribute(u"invert"_s, invert_ ? u"true"_s : u"false"_s);
  element.setAttribute(u"near_m"_s, QString::number(near_m_, 'g', 9));
  element.setAttribute(u"far_m"_s, QString::number(far_m_, 'g', 9));
  element.setAttribute(u"opacity"_s, QString::number(opacity_, 'g', 9));
}

bool DepthImageLayer::loadOptions(const QDomElement& element) {
  const Colormap restored_colormap = parseColormap(element.attribute(u"colormap"_s, colormapToken(colormap_)));
  const QString invert_text = element.attribute(u"invert"_s, invert_ ? u"true"_s : u"false"_s);
  if (invert_text != u"true"_s && invert_text != u"false"_s) {
    return false;
  }
  const bool restored_invert = invert_text == u"true"_s;

  const auto read_float = [&element](const QString& name, float fallback, float& output) {
    if (!element.hasAttribute(name)) {
      output = fallback;
      return true;
    }
    bool ok = false;
    output = element.attribute(name).toFloat(&ok);
    return ok && std::isfinite(output);
  };
  float restored_near = near_m_;
  float restored_far = far_m_;
  float restored_opacity = opacity_;
  if (!read_float(u"near_m"_s, near_m_, restored_near) || !read_float(u"far_m"_s, far_m_, restored_far) ||
      !read_float(u"opacity"_s, opacity_, restored_opacity)) {
    return false;
  }
  if (restored_far < restored_near) {
    std::swap(restored_near, restored_far);
  }
  restored_opacity = std::clamp(restored_opacity, 0.0f, 1.0f);

  const bool changed = colormap_ != restored_colormap || invert_ != restored_invert || near_m_ != restored_near ||
                       far_m_ != restored_far || opacity_ != restored_opacity;
  colormap_ = restored_colormap;
  invert_ = restored_invert;
  near_m_ = restored_near;
  far_m_ = restored_far;
  opacity_ = restored_opacity;
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
  // Colormap/range/invert are GPU uniforms carried on the frame's DepthColorParams
  // (not baked into the pixels). The setters above invalidate the worker so it
  // re-emits a frame with the new params; re-apply the current tracker time so that
  // re-emit happens now instead of on the next tick.
  if (const auto ts = lastTrackerTimeNs(); ts.has_value()) {
    depth_source_->setTimestamp(*ts);
  }
  emit repaintRequested();
}

void DepthImageLayer::setColormap(Colormap colormap) {
  if (colormap_ == colormap) {
    return;
  }
  colormap_ = colormap;
  applyOptions();
  emit configurationChanged();
}

void DepthImageLayer::setInvert(bool invert) {
  if (invert_ == invert) {
    return;
  }
  invert_ = invert;
  applyOptions();
  emit configurationChanged();
}

void DepthImageLayer::setRange(float near_m, float far_m) {
  if (far_m < near_m) {
    std::swap(near_m, far_m);
  }
  if (near_m_ == near_m && far_m_ == far_m) {
    return;
  }
  near_m_ = near_m;
  far_m_ = far_m;
  applyOptions();
  emit configurationChanged();
}

}  // namespace PJ
