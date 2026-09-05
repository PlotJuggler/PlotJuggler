// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_widgets/layers/poses_in_frame_layer.h"

#include <QColor>
#include <QDomElement>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLoggingCategory>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <any>
#include <glm/glm.hpp>
#include <optional>

#include "layer_xml_validation.h"
#include "pj_base/builtin/poses_in_frame.hpp"
#include "pj_base/time.hpp"  // PJ::toRaw
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/render_pass.h"     // ViewParams, FrameContext
#include "pj_scene3d_widgets/resolve_object.h"  // resolveObject, hasCanonical3DCodec
#include "pj_widgets/ColorPickerWidget.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/ToggleSwitch.h"
using namespace Qt::StringLiterals;

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcPoses, "pj.scene3d.poses_in_frame")
}  // namespace

PosesInFrameLayer::PosesInFrameLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : Scene3DLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)) {}

PosesInFrameLayer::~PosesInFrameLayer() = default;

PJ::SceneLayerInfo PosesInFrameLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kPosesInFrame,
      .display_name = display_name_,
      .family_name = u"PosesInFrame"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> PosesInFrameLayer::timeRange() const {
  const auto* store = ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr;
  return PJ::liveTopicTimeRange(store, topic_id_);
}

QStringList PosesInFrameLayer::fallbackFrames() const {
  if (source_frame_.empty()) {
    return {};
  }
  return {QString::fromStdString(source_frame_)};
}

QString PosesInFrameLayer::sourceFrame() const {
  return QString::fromStdString(source_frame_);
}

QString PosesInFrameLayer::statusWarning() const {
  return trail_ != nullptr ? trail_->statusWarning() : QString{};
}

QDomElement PosesInFrameLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(u"poses_in_frame"_s);
  el.setAttribute(u"gizmo_size"_s, static_cast<double>(gizmo_size_));
  el.setAttribute(u"gizmo_opacity"_s, static_cast<double>(gizmo_opacity_));
  el.setAttribute(u"x_arrow_only"_s, x_arrow_only_ ? 1 : 0);
  el.setAttribute(u"override_color"_s, override_color_enabled_ ? 1 : 0);
  el.setAttribute(u"override_color_value"_s, override_color_.name(QColor::HexRgb));
  // The owned trail persists as a nested payload: its presence IS the enabled
  // flag, and its style is written by the trail itself (one definition).
  if (trail_ != nullptr) {
    el.appendChild(trail_->xmlSaveState(doc));
  }
  return el;
}

bool PosesInFrameLayer::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "poses_in_frame"_L1 || !detail::isLeafPayload(element, "trail"_L1)) {
    return false;
  }
  float restored_size = 0.0f;
  float restored_opacity = 0.0f;
  bool restored_x_arrow_only = false;
  bool restored_override_enabled = false;
  if (!detail::parseFiniteFloat(element, "gizmo_size", 0.15f, 0.01f, 100.0f, restored_size) ||
      !detail::parseFiniteFloat(element, "gizmo_opacity", 1.0f, 0.0f, 1.0f, restored_opacity) ||
      !detail::parseZeroOne(element, "x_arrow_only", false, restored_x_arrow_only) ||
      !detail::parseZeroOne(element, "override_color", false, restored_override_enabled)) {
    return false;
  }
  QColor restored_color;
  const bool has_override_color = element.hasAttribute(u"override_color_value"_s);
  if (has_override_color) {
    restored_color = QColor(element.attribute(u"override_color_value"_s));
    if (!restored_color.isValid()) {
      return false;
    }
  }
  setGizmoSize(restored_size);
  setGizmoOpacity(restored_opacity);
  setXArrowOnly(restored_x_arrow_only);
  setOverrideColorEnabled(restored_override_enabled);
  if (has_override_color) {
    setOverrideColor(restored_color);
  }
  // A <trail> child means the trail was on; TrailLayer::xmlLoadState is
  // style-only by contract, which is exactly right here — the SOURCE is this
  // layer's own topic, never something the payload retargets.
  const QDomElement trail_el = element.firstChildElement(u"trail"_s);
  setTrailEnabled(!trail_el.isNull());
  if (trail_ != nullptr && !trail_->xmlLoadState(trail_el)) {
    return false;
  }
  return true;
}

bool PosesInFrameLayer::attach(const PJ::SceneLayerContext& ctx) {
  const auto& scene3d_ctx = static_cast<const Scene3DLayerContext&>(ctx);
  if (scene3d_ctx.session == nullptr) {
    qCWarning(lcPoses) << "attach: session is null";
    return false;
  }
  ctx_ = scene3d_ctx;
  // Accept a parser-backed topic OR a parser-less canonical PosesInFrame topic
  // (a data-source/toolbox that pushed serialized canonical poses, decoded
  // host-side by resolveObject()).
  if (!scene3d_ctx.session->parserBindingForObjectTopic(topic_id_) &&
      !hasCanonical3DCodec(PJ::sdk::BuiltinObjectType::kPosesInFrame)) {
    qCWarning(lcPoses) << "attach: no parser and no canonical codec for poses topic" << topic_id_.id;
    return false;
  }
  resetReplayState();
  // Streaming-tolerant: a topic can be attached before its first sample lands
  // (layout restore at stream start, catalog drag). bootstrap() only pre-warms
  // source_frame_; renderAt() self-heals it, so a failure must NOT drop the layer
  // (SceneDockWidget::attach discards on false).
  if (!bootstrap()) {
    qCWarning(lcPoses) << "attach: bootstrap deferred for poses topic" << topic_id_.id
                       << "— render will start once a sample arrives";
  }
  syncTrailToCurrentState();
  return true;
}

void PosesInFrameLayer::detach() {
  resetReplayState();
  if (trail_ != nullptr) {
    trail_->detach();
  }
}

void PosesInFrameLayer::resetReplayState() {
  instances_.clear();
  pass_.setInstances({});
  staged_uid_ = {};
  staged_revision_ = -1;
  source_frame_.clear();
}

void PosesInFrameLayer::updateSourceFrame(const std::string& frame_id) {
  if (frame_id == source_frame_) {
    return;
  }
  source_frame_ = frame_id;
  emit sourceFrameChanged(QString::fromStdString(source_frame_));
  emit fallbackFramesChanged(fallbackFrames());
}

bool PosesInFrameLayer::bootstrap() {
  PJ::ObjectStore& store = ctx_.session->objectStore();
  auto first = store.at(topic_id_, 0);
  if (!first.has_value() || first->payload.bytes.empty()) {
    return false;
  }
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  auto obj = resolveObject(binding, PJ::sdk::BuiltinObjectType::kPosesInFrame, first->timestamp, first->payload);
  if (!obj.has_value()) {
    qCWarning(lcPoses) << "bootstrap: parseObject failed:" << QString::fromStdString(obj.error());
    return false;
  }
  const auto* msg = obj->object.get<PJ::sdk::PosesInFrame>();
  if (msg == nullptr) {
    return false;
  }
  updateSourceFrame(msg->frame_id);
  return true;
}

void PosesInFrameLayer::renderAt(int64_t time_ns) {
  if (ctx_.session == nullptr) {
    return;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  // Scrub coalescing, checked metadata-first (latestEntryIdAt — no payload
  // resolve, so an unchanged entry never pays a cold refetch of an evicted pool
  // seed): the same store entry under the same style is already staged. Shared by
  // the gate and the post-resolve re-check (a racing insert can change the pick).
  const auto already_staged = [this](PJ::SequentialUID uid) {
    return uid == staged_uid_ && style_revision_ == staged_revision_;
  };
  const auto entry_id = store.latestEntryIdAt(topic_id_, time_ns);
  if (entry_id.has_value() && already_staged(entry_id->uid)) {
    return;
  }
  auto entry = entry_id.has_value() ? store.latestAt(topic_id_, time_ns) : std::nullopt;
  if (!entry.has_value() || entry->payload.bytes.empty()) {
    // No sample at/before this time -> show nothing.
    if (!instances_.empty()) {
      instances_.clear();
      pass_.setInstances({});
    }
    staged_uid_ = {};
    return;
  }
  if (already_staged(entry->sequential_uid)) {
    return;
  }
  // Per-use binding fetch (never cached) — a reload re-registers the parser slot.
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  auto obj = resolveObject(binding, PJ::sdk::BuiltinObjectType::kPosesInFrame, entry->timestamp, entry->payload);
  if (!obj.has_value()) {
    return;
  }
  const auto* msg = obj->object.get<PJ::sdk::PosesInFrame>();
  if (msg == nullptr) {
    return;
  }
  updateSourceFrame(msg->frame_id);
  instances_ = buildPoseTriadInstances(
      *msg, PoseTriadStyle{
                .axis_length = gizmo_size_,
                .opacity = gizmo_opacity_,
                .x_arrow_only = x_arrow_only_,
                .override_color = override_color_enabled_,
                .color = glm::vec3(override_color_.redF(), override_color_.greenF(), override_color_.blueF()),
            });
  pass_.setInstances(instances_);
  staged_uid_ = entry->sequential_uid;
  staged_revision_ = style_revision_;
}

void PosesInFrameLayer::setFixedFrame(const QString& frame) {
  // The poses are frame-relative; render() re-resolves the fixed frame each paint
  // via FrameContext, so a change only needs a repaint. The trail is the
  // exception — it rebuilds its polyline against the fixed frame.
  fixed_frame_ = frame;
  if (trail_ != nullptr) {
    trail_->setFixedFrame(frame);
  }
  emit repaintRequested();
}

void PosesInFrameLayer::setTrackerTime(PJ::Timepoint time) {
  tracker_dirty_ = true;  // render() reads frame_ctx.time via the tracker_dirty_ path
  if (trail_ != nullptr) {
    trail_->setTrackerTime(time);  // moves the past/future colour split
  }
  emit repaintRequested();
}

void PosesInFrameLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  if (trail_ != nullptr) {
    trail_->setVisible(visible);  // the trail is part of this layer, not a peer
  }
  emit visibilityChanged(visible);
  // Catch-up on un-hide is the dock's job (it re-delivers the last tracker time).
  emit repaintRequested();
}

void PosesInFrameLayer::initializeGL() {
  pass_.initializeGL();
  // The view calls this every paintGL before render(), so a trail enabled later
  // (outside any GL context) initializes on the first paint that sees it.
  if (trail_ != nullptr) {
    trail_->initializeGL();
  }
}

void PosesInFrameLayer::releaseGL() {
  pass_.releaseGL();
  if (trail_ != nullptr) {
    trail_->releaseGL();
  }
}

void PosesInFrameLayer::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!visible_) {
    return;
  }
  // Ahead of every early-out below: the trail carries its own geometry and
  // resolves its own frame, so it must still draw on ticks where this instant
  // has no decoded pose set (before the first sample, or an empty array).
  if (trail_ != nullptr) {
    trail_->render(view_params, frame_ctx);
  }
  // Drain a pending tracker move or style edit (one expansion per painted frame).
  if (tracker_dirty_) {
    tracker_dirty_ = false;
    renderAt(PJ::toRaw(frame_ctx.time));
  }
  if (source_frame_.empty() || instances_.empty()) {
    return;  // nothing decoded yet, or an empty pose set
  }
  const auto transform = frame_ctx.lookup(source_frame_);
  if (!transform.has_value()) {
    return;  // orphan: source frame can't resolve into the fixed frame -> draw nothing
  }
  pass_.render(view_params, glm::mat4(transform->matrix()));
}

void PosesInFrameLayer::setGizmoSize(float meters) {
  const float clamped = std::clamp(meters, 0.01f, 100.0f);
  if (gizmo_size_ == clamped) {
    return;
  }
  gizmo_size_ = clamped;
  ++style_revision_;
  tracker_dirty_ = true;  // re-expand the current sample on the next paint
  emit configurationChanged();
  emit repaintRequested();
}

void PosesInFrameLayer::setGizmoOpacity(float opacity) {
  const float clamped = std::clamp(opacity, 0.0f, 1.0f);
  if (gizmo_opacity_ == clamped) {
    return;
  }
  gizmo_opacity_ = clamped;
  ++style_revision_;
  tracker_dirty_ = true;
  emit configurationChanged();
  emit repaintRequested();
}

void PosesInFrameLayer::setXArrowOnly(bool x_arrow_only) {
  if (x_arrow_only_ == x_arrow_only) {
    return;
  }
  x_arrow_only_ = x_arrow_only;
  ++style_revision_;
  tracker_dirty_ = true;  // re-expand the current sample (triad <-> single arm)
  emit configurationChanged();
  emit repaintRequested();
}

void PosesInFrameLayer::setTrailEnabled(bool enabled) {
  if (enabled == (trail_ != nullptr)) {
    return;
  }
  if (!enabled) {
    trail_.reset();
    emit configurationChanged();
    emit statusWarningChanged();  // the trail's warning, if any, goes with it
    emit repaintRequested();
    return;
  }
  trail_ = std::make_unique<TrailLayer>(topic_id_, TrailSource::poseTopic(topic_id_));
  // Nothing else would ever repaint, persist, or surface a warning on the
  // trail's behalf: it is not a dock layer, so it has no row of its own and the
  // dock's wireScene3DLayer never sees it.
  connect(trail_.get(), &TrailLayer::repaintRequested, this, &PosesInFrameLayer::repaintRequested);
  connect(trail_.get(), &TrailLayer::configurationChanged, this, &PosesInFrameLayer::configurationChanged);
  connect(trail_.get(), &TrailLayer::statusWarningChanged, this, &PosesInFrameLayer::statusWarningChanged);
  syncTrailToCurrentState();
  emit configurationChanged();
  emit repaintRequested();
}

void PosesInFrameLayer::syncTrailToCurrentState() {
  if (trail_ == nullptr || ctx_.session == nullptr) {
    return;  // not attached yet — attach() calls this again once ctx_ is live
  }
  if (!trail_->attach(ctx_)) {
    qCWarning(lcPoses) << "trail attach failed for poses topic" << topic_id_.id;
    trail_.reset();
    return;
  }
  if (!fixed_frame_.isEmpty()) {
    trail_->setFixedFrame(fixed_frame_);
  }
  trail_->setVisible(visible_);
}

void PosesInFrameLayer::setOverrideColorEnabled(bool enabled) {
  if (override_color_enabled_ == enabled) {
    return;
  }
  override_color_enabled_ = enabled;
  ++style_revision_;
  tracker_dirty_ = true;  // re-expand: per-axis RGB <-> shared override color
  emit configurationChanged();
  emit repaintRequested();
}

void PosesInFrameLayer::setOverrideColor(QColor color) {
  if (!color.isValid() || override_color_.rgb() == color.rgb()) {
    return;
  }
  override_color_ = color;
  ++style_revision_;
  // Only changes what is drawn while the override is enabled, but bump
  // unconditionally so the next paint reflects it immediately when it is.
  tracker_dirty_ = true;
  emit configurationChanged();
  emit repaintRequested();
}

QWidget* PosesInFrameLayer::createConfigWidget(QWidget* parent) {
  auto* container = new QWidget(parent);
  auto* outer = new QVBoxLayout(container);
  outer->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  outer->setSpacing(PJ::theme::space(PJ::theme::Space::Snug));
  auto* form = new QFormLayout();
  form->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  form->setHorizontalSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  form->setVerticalSpacing(PJ::theme::space(PJ::theme::Space::Snug));
  outer->addLayout(form);

  auto* size_spin = new PJ::DoubleScrubber(container);
  size_spin->setRange(0.01, 5.0);
  size_spin->setDecimals(3);
  size_spin->setSingleStep(0.01);
  size_spin->setValue(static_cast<double>(gizmo_size_));
  form->addRow(tr("Arrow length (m):"), size_spin);
  QObject::connect(
      size_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) { setGizmoSize(static_cast<float>(v)); });

  auto* opacity_spin = new PJ::DoubleScrubber(container);
  opacity_spin->setRange(0.0, 1.0);
  opacity_spin->setDecimals(2);
  opacity_spin->setSingleStep(0.05);
  opacity_spin->setValue(static_cast<double>(gizmo_opacity_));
  form->addRow(tr("Opacity:"), opacity_spin);
  QObject::connect(opacity_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) {
    setGizmoOpacity(static_cast<float>(v));
  });

  // "X Arrow only" geometry toggle: a single X arm per pose instead of the triad.
  auto* x_only_toggle = new PJ::ToggleSwitch(container);
  x_only_toggle->setChecked(x_arrow_only_, /*animate=*/false);
  form->addRow(tr("X Arrow only:"), x_only_toggle);
  QObject::connect(x_only_toggle, &PJ::ToggleSwitch::toggled, this, [this](bool on) { setXArrowOnly(on); });

  // Override color: the sliding switch + swatch sit on one row. Recolors the
  // single X arm OR the whole triad; picking a color auto-enables the override.
  auto* override_row = new QWidget(container);
  auto* override_layout = new QHBoxLayout(override_row);
  override_layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  override_layout->setSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  auto* override_toggle = new PJ::ToggleSwitch(override_row);
  override_toggle->setChecked(override_color_enabled_, /*animate=*/false);
  auto* swatch = new PJ::ColorPickerWidget(override_row);
  swatch->setColor(override_color_);
  override_layout->addWidget(override_toggle);
  override_layout->addWidget(swatch);
  override_layout->addStretch();
  form->addRow(tr("Override color:"), override_row);

  QObject::connect(override_toggle, &PJ::ToggleSwitch::toggled, this, [this](bool on) { setOverrideColorEnabled(on); });
  QObject::connect(swatch, &PJ::ColorPickerWidget::colorChanged, this, [this, override_toggle](QColor c) {
    setOverrideColor(c);
    if (!override_toggle->isChecked()) {
      override_toggle->setChecked(true);  // picking implies enable (visual slide)
    }
    setOverrideColorEnabled(true);  // apply now; the toggled-driven call lags the slide animation
  });

  // Trail: opt-in toggle, and when on the owned trail's own style rows are
  // appended to THIS form — same label column as the rows above, and the single
  // definition of those controls (TrailLayer::appendStyleRows).
  auto* trail_toggle = new PJ::ToggleSwitch(container);
  trail_toggle->setChecked(trail_ != nullptr, /*animate=*/false);
  trail_toggle->setToolTip(tr("Trace this topic's pose across the whole time range"));
  form->addRow(tr("Trail:"), trail_toggle);
  const int trail_rows_begin = form->rowCount();
  const auto refresh_trail_rows = [this, form, container, trail_rows_begin]() {
    while (form->rowCount() > trail_rows_begin) {
      form->removeRow(form->rowCount() - 1);
    }
    if (trail_ != nullptr) {
      trail_->appendStyleRows(form, container);
    }
  };
  refresh_trail_rows();
  QObject::connect(trail_toggle, &PJ::ToggleSwitch::toggled, this, [this, refresh_trail_rows](bool on) {
    setTrailEnabled(on);
    refresh_trail_rows();
  });

  return container;
}

}  // namespace pj::scene3d
