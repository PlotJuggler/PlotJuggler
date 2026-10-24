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
#include <cmath>
#include <exception>
#include <glm/glm.hpp>
#include <new>
#include <optional>

#include "layer_xml_validation.h"
#include "pj_base/builtin/poses_in_frame.hpp"
#include "pj_base/time.hpp"  // PJ::toRaw
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/scene_limits.h"
#include "pj_scene3d_core/tf/object_placement.h"
#include "pj_scene3d_widgets/resolve_object.h"  // resolveObject, hasCanonical3DCodec
#include "pj_tf/transform.h"
#include "pj_widgets/ColorPickerWidget.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/ToggleSwitch.h"
using namespace Qt::StringLiterals;

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcPoses, "pj.scene3d.poses_in_frame")

bool finiteInstance(const PoseTriadInstance& instance) {
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      if (!std::isfinite(instance.model[column][row])) {
        return false;
      }
    }
    if (!std::isfinite(instance.color[column])) {
      return false;
    }
  }
  return true;
}

void expandInstanceBounds(AABB& bounds, const PoseTriadInstance& instance) {
  const glm::vec3 origin = glm::vec3(instance.model * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
  const glm::vec3 tip = glm::vec3(instance.model * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
  // The shared unit arrow's widest section is a 0.10-radius cone. Uniform
  // instance scale makes this conservative in source axes for every rotation.
  const glm::vec3 padding(0.10f * glm::length(glm::vec3(instance.model[0])));
  expandAABB(bounds, origin - padding);
  expandAABB(bounds, origin + padding);
  expandAABB(bounds, tip - padding);
  expandAABB(bounds, tip + padding);
}
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
  const QString own = Scene3DLayer::statusWarning();
  return own.isEmpty() && trail_ != nullptr ? trail_->statusWarning() : own;
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

std::optional<PosesInFrameLayer::ParsedSettings> PosesInFrameLayer::parseSettings(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "poses_in_frame"_L1 || !detail::isLeafPayload(element, "trail"_L1)) {
    return std::nullopt;
  }
  ParsedSettings settings;
  if (!detail::parseFiniteFloat(element, "gizmo_size", 0.15f, 0.01f, 100.0f, settings.gizmo_size) ||
      !detail::parseFiniteFloat(element, "gizmo_opacity", 1.0f, 0.0f, 1.0f, settings.gizmo_opacity) ||
      !detail::parseZeroOne(element, "x_arrow_only", false, settings.x_arrow_only) ||
      !detail::parseZeroOne(element, "override_color", false, settings.override_color_enabled)) {
    return std::nullopt;
  }
  if (element.hasAttribute(u"override_color_value"_s)) {
    const QColor color(element.attribute(u"override_color_value"_s));
    if (!color.isValid()) {
      return std::nullopt;
    }
    settings.override_color = color;
  }
  return settings;
}

bool PosesInFrameLayer::validateXml(const QDomElement& element) {
  return parseSettings(element).has_value();
}

bool PosesInFrameLayer::xmlLoadState(const QDomElement& element) {
  const auto settings = parseSettings(element);
  if (!settings.has_value()) {
    return false;
  }
  setGizmoSize(settings->gizmo_size);
  setGizmoOpacity(settings->gizmo_opacity);
  setXArrowOnly(settings->x_arrow_only);
  setOverrideColorEnabled(settings->override_color_enabled);
  if (settings->override_color.has_value()) {
    setOverrideColor(*settings->override_color);
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
  // Accept a parser-backed topic OR a parser-less canonical PosesInFrame topic
  // (a data-source/toolbox that pushed serialized canonical poses, decoded
  // host-side by resolveObject()).
  if (!scene3d_ctx.session->parserBindingForObjectTopic(topic_id_) &&
      !hasCanonical3DCodec(PJ::sdk::BuiltinObjectType::kPosesInFrame)) {
    qCWarning(lcPoses) << "attach: no parser and no canonical codec for poses topic" << topic_id_.id;
    return false;
  }
  ctx_ = scene3d_ctx;
  resetReplayState();
  disconnect(reload_connection_);
  reload_connection_ = connect(
      ctx_.session, &PJ::SessionManager::datasetAboutToBeReplaced, this,
      &PosesInFrameLayer::onDatasetAboutToBeReplaced);
  // Streaming-tolerant: a topic can be attached before its first sample lands
  // (layout restore at stream start, catalog drag). bootstrap() only pre-warms
  // source_frame_; updateAt() self-heals it, so a failure must NOT drop the layer
  // (SceneDockWidget::attach discards on false).
  if (!bootstrap()) {
    qCWarning(lcPoses) << "attach: bootstrap deferred for poses topic" << topic_id_.id
                       << "— render will start once a sample arrives";
  }
  syncTrailToCurrentState();
  return true;
}

void PosesInFrameLayer::detach() {
  disconnect(reload_connection_);
  reload_connection_ = {};
  resetReplayState();
  setDataWarning({});
  requested_time_.reset();
  tracker_dirty_ = false;
  if (trail_ != nullptr) {
    trail_->detach();
  }
}

void PosesInFrameLayer::resetReplayState() {
  clearGeometry();
  staged_uid_ = {};
  staged_revision_ = -1;
  source_frame_.clear();
  budget_.reset();
}

void PosesInFrameLayer::clearGeometry() {
  const bool had_geometry = !instances_.empty() || source_bounds_.valid;
  std::vector<PoseTriadInstance>{}.swap(instances_);
  source_bounds_ = {};
  if (had_geometry) {
    ++geometry_revision_;
  }
}

void PosesInFrameLayer::markDirty() {
  tracker_dirty_ = true;
  budget_.reset();
}

void PosesInFrameLayer::onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id) {
  if (ctx_.session == nullptr || ctx_.session->objectStore().descriptor(topic_id_).dataset_id != dataset_id) {
    return;
  }
  clearGeometry();
  staged_uid_ = {};
  staged_revision_ = -1;
  markDirty();
  setDataWarning({});
  emit repaintRequested();
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
  if (!first.has_value() || first->payload.bytes.empty() ||
      static_cast<std::uint64_t>(first->payload.bytes.size()) > kSceneLimits.max_pose_wire_bytes_per_layer) {
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

bool PosesInFrameLayer::updateAt(int64_t time_ns) {
  if (ctx_.session == nullptr) {
    return false;
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
    return false;
  }
  auto entry = entry_id.has_value() ? store.latestAt(topic_id_, time_ns) : std::nullopt;
  if (!entry.has_value() || entry->payload.bytes.empty()) {
    // No sample at/before this time -> show nothing.
    const bool changed = !instances_.empty() || !dataWarning().isEmpty();
    clearGeometry();
    staged_uid_ = {};
    setDataWarning({});
    return changed;
  }
  if (already_staged(entry->sequential_uid)) {
    return false;
  }
  // Every exit below stages this sample, so a bad one is not retried per frame.
  staged_uid_ = entry->sequential_uid;
  staged_revision_ = style_revision_;
  const auto fail = [this](QString warning) {
    clearGeometry();
    setDataWarning(std::move(warning));
    return true;
  };
  if (static_cast<std::uint64_t>(entry->payload.bytes.size()) > kSceneLimits.max_pose_wire_bytes_per_layer) {
    return fail(tr("Pose payload is %1 MiB; the limit is %2 MiB")
                    .arg(QString::number(static_cast<double>(entry->payload.bytes.size()) / (1024.0 * 1024.0), 'f', 1))
                    .arg(kSceneLimits.max_pose_wire_bytes_per_layer / kMiB));
  }
  // Per-use binding fetch (never cached) — a reload re-registers the parser slot.
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  auto obj = resolveObject(binding, PJ::sdk::BuiltinObjectType::kPosesInFrame, entry->timestamp, entry->payload);
  if (!obj.has_value()) {
    return fail(tr("Pose array could not be decoded: %1").arg(QString::fromStdString(obj.error())));
  }
  const auto* msg = obj->object.get<PJ::sdk::PosesInFrame>();
  if (msg == nullptr) {
    return fail(tr("This layer accepts PosesInFrame samples only"));
  }
  updateSourceFrame(msg->frame_id);
  render_stamp_ = objectStamp(msg->timestamp_ns, entry->payload_stamp_shift);
  if (static_cast<std::uint64_t>(msg->poses.size()) > kSceneLimits.max_poses_per_layer) {
    return fail(tr("Pose array has %1 poses; the limit is %2")
                    .arg(QString::number(msg->poses.size()), QString::number(kSceneLimits.max_poses_per_layer)));
  }
  std::vector<PoseTriadInstance> expanded;
  try {
    expanded = buildPoseTriadInstances(
        *msg, PoseTriadStyle{
                  .axis_length = gizmo_size_,
                  .opacity = gizmo_opacity_,
                  .x_arrow_only = x_arrow_only_,
                  .override_color = override_color_enabled_,
                  .color = glm::vec3(override_color_.redF(), override_color_.greenF(), override_color_.blueF()),
              });
  } catch (const std::bad_alloc&) {
    return fail(tr("Not enough memory to expand this pose array"));
  } catch (const std::exception& error) {
    return fail(tr("Pose-array expansion failed: %1").arg(QString::fromUtf8(error.what())));
  }
  AABB bounds;
  const auto finite_end = std::remove_if(expanded.begin(), expanded.end(), [&bounds](const PoseTriadInstance& inst) {
    if (!finiteInstance(inst)) {
      return true;
    }
    expandInstanceBounds(bounds, inst);
    return false;
  });
  const bool had_nonfinite = finite_end != expanded.end();
  expanded.erase(finite_end, expanded.end());
  instances_ = std::move(expanded);
  source_bounds_ = bounds;
  ++geometry_revision_;
  if (!msg->poses.empty() && instances_.empty()) {
    setDataWarning(tr("Pose array contains no finite poses"));
  } else if (had_nonfinite) {
    setDataWarning(tr("Non-finite poses were skipped"));
  } else {
    setDataWarning({});
  }
  return true;
}

void PosesInFrameLayer::setFixedFrame(const QString& frame) {
  // The poses are frame-relative; the renderer re-resolves the fixed frame each
  // paint, so a change only needs a repaint. The trail is the exception — it
  // rebuilds its polyline against the fixed frame.
  fixed_frame_ = frame;
  if (trail_ != nullptr) {
    trail_->setFixedFrame(frame);
  }
  emit repaintRequested();
}

void PosesInFrameLayer::setTrackerTime(PJ::Timepoint time) {
  requested_time_ = time;
  tracker_dirty_ = true;  // drained once per painted frame (see tracker_dirty_)
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
  if (!visible_) {
    // Release the retained arms while hidden; un-hiding re-decodes.
    clearGeometry();
    staged_uid_ = {};
    staged_revision_ = -1;
    budget_.reset();
  }
  if (trail_ != nullptr) {
    trail_->setVisible(visible);  // the trail is part of this layer, not a peer
  }
  emit visibilityChanged(visible);
  // Catch-up on un-hide is the dock's job (it re-delivers the last tracker time).
  emit repaintRequested();
}

bool PosesInFrameLayer::drainPendingUpdate() {
  if (!visible_ || !tracker_dirty_ || !requested_time_.has_value()) {
    return false;
  }
  tracker_dirty_ = false;
  return updateAt(PJ::toRaw(*requested_time_));
}

std::uint64_t PosesInFrameLayer::renderKey(PJ::Timepoint time) const {
  if (trail_ != nullptr) {
    return Scene3DLayer::renderKey(time);
  }
  const std::uint64_t key =
      PJ::latestEntryRenderKey(ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr, topic_id_, time);
  return foldFrameTransformIntoKey(
      key, ctx_.tf_buffer.get(), fixed_frame_.toStdString(), source_frame_, render_stamp_, time, placementHold());
}

bool PosesInFrameLayer::prepareForRender(std::uint64_t remaining_view_arms) {
  // The budget_ candidate: the sample at the playhead (settings changes reset the latch).
  const auto candidate = [this] {
    return PJ::latestEntryRenderKey(
        ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr, topic_id_,
        requested_time_.value_or(PJ::Timepoint{}));
  };
  bool released = false;
  if (budget_.blocked(remaining_view_arms, candidate, released)) {
    return false;
  }
  if (released) {
    staged_uid_ = {};
    staged_revision_ = -1;
    tracker_dirty_ = true;
  }
  const bool changed = drainPendingUpdate();
  if (static_cast<std::uint64_t>(instances_.size()) <= remaining_view_arms) {
    return changed;
  }
  budget_.reject(instances_.size(), candidate());
  setDataWarning(tr("Layer needs %1 pose arrows; only %2 remain in this view's budget")
                     .arg(QString::number(instances_.size()), QString::number(remaining_view_arms)));
  clearGeometry();
  return true;
}

void PosesInFrameLayer::setGizmoSize(float meters) {
  const float clamped = std::clamp(meters, 0.01f, 100.0f);
  if (gizmo_size_ == clamped) {
    return;
  }
  gizmo_size_ = clamped;
  ++style_revision_;
  markDirty();
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
  markDirty();
  emit configurationChanged();
  emit repaintRequested();
}

void PosesInFrameLayer::setXArrowOnly(bool x_arrow_only) {
  if (x_arrow_only_ == x_arrow_only) {
    return;
  }
  x_arrow_only_ = x_arrow_only;
  ++style_revision_;
  markDirty();
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
  markDirty();
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
  markDirty();
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
