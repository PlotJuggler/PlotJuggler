// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_widgets/layers/trail_layer.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLoggingCategory>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent>
#include <algorithm>
#include <any>
#include <chrono>
#include <limits>
#include <utility>

#include "layer_xml_validation.h"
#include "pj_base/builtin/poses_in_frame.hpp"
#include "pj_base/time.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/object_topic_metadata.h"
#include "pj_scene3d_widgets/render_pass.h"
#include "pj_scene3d_widgets/resolve_object.h"
#include "pj_widgets/ColorPickerWidget.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SvgUtil.h"
using namespace Qt::StringLiterals;

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcTrail, "pj.scene3d.trail")

glm::vec3 toVec3(const QColor& color) {
  return glm::vec3(color.redF(), color.greenF(), color.blueF());
}

QString datasetSource(PJ::SessionManager& session, PJ::DatasetId dataset_id) {
  const PJ::DatasetInfo* info = session.dataEngine().getDataset(dataset_id);
  return info != nullptr ? QString::fromStdString(info->source_name) : QString();
}
}  // namespace

TrailLayer::TrailLayer(PJ::ObjectTopicId layer_id, TrailSource source, QObject* parent)
    : Scene3DLayer(parent), layer_id_(layer_id), source_(std::move(source)) {
  display_name_ = source_.kind == TrailSource::Kind::kTfFrame ? tr("Trail: %1").arg(source_.frame)
                                                              : tr("Trail: topic %1").arg(source_.topic.id);
}

TrailLayer::TrailLayer(PJ::ObjectTopicId layer_id, QObject* parent) : TrailLayer(layer_id, TrailSource{}, parent) {}

TrailLayer::~TrailLayer() = default;

PJ::SceneLayerInfo TrailLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = layer_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kNone,  // not a wire object; see class comment
      .display_name = display_name_,
      .family_name = u"Trail"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> TrailLayer::timeRange() const {
  if (stamps_.empty()) {
    // Inverted sentinel (RobotModelLayer convention): an empty trail must not
    // widen the playback timeline.
    return {PJ::Timepoint::max(), PJ::Timepoint::min()};
  }
  return {stamps_.front(), stamps_.back()};
}

QStringList TrailLayer::fallbackFrames() const {
  if (source_.kind == TrailSource::Kind::kTfFrame) {
    return {source_.frame};
  }
  return {};
}

QString TrailLayer::sourceFrame() const {
  // A trail poses nothing through TF at render time (points are pre-baked in
  // the fixed frame), so it never participates in the dock's orphan check.
  return {};
}

QString TrailLayer::statusWarning() const {
  return warning_;
}

std::size_t TrailLayer::pastCount(PJ::Timepoint time) const {
  const auto it = std::upper_bound(stamps_.begin(), stamps_.end(), time);
  return static_cast<std::size_t>(std::distance(stamps_.begin(), it));
}

uint64_t TrailLayer::renderKey(PJ::Timepoint time) const {
  // Decode-free by construction (a binary search over cached stamps). Between
  // two consecutive samples the key is constant -> the dock's repaint gate
  // coalesces scrubbing to zero repaints (the PR #259 contract).
  uint64_t key = build_revision_ * 0x9E3779B97F4A7C15ULL;
  key ^= static_cast<uint64_t>(style_revision_) << 48U;
  key ^= static_cast<uint64_t>(pastCount(time)) + 0x9E3779B9ULL + (key << 6U) + (key >> 2U);
  return key;
}

QDomElement TrailLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(u"trail"_s);
  el.setAttribute(u"source_kind"_s, source_.kind == TrailSource::Kind::kTfFrame ? u"tf_frame"_s : u"pose_topic"_s);
  if (source_.kind == TrailSource::Kind::kTfFrame) {
    el.setAttribute(u"frame"_s, source_.frame);
  }
  if (source_.kind == TrailSource::Kind::kPoseTopic && ctx_.session != nullptr) {
    // Persist the resolvable identity (dataset + topic name) so restore
    // re-binds the SAME topic across sessions where load order changed the
    // DatasetId — the RobotModelLayer topic-source idiom.
    const auto desc = ctx_.session->objectStore().descriptor(source_.topic);
    el.setAttribute(u"source_topic_name"_s, QString::fromStdString(desc.topic_name));
    el.setAttribute(u"source_dataset_id"_s, static_cast<uint>(desc.dataset_id));
    el.setAttribute(u"source_dataset_source"_s, datasetSource(*ctx_.session, desc.dataset_id));
    const QString path = ctx_.session->datasetSourcePath(desc.dataset_id);
    if (!path.isEmpty()) {
      el.setAttribute(u"source_dataset_path"_s, path);
    }
  }
  el.setAttribute(u"past_color"_s, past_color_.name(QColor::HexRgb));
  el.setAttribute(u"future_color"_s, future_color_.name(QColor::HexRgb));
  el.setAttribute(u"thickness"_s, static_cast<double>(thickness_px_));
  el.setAttribute(u"past_visible"_s, past_visible_ ? 1 : 0);
  el.setAttribute(u"future_visible"_s, future_visible_ ? 1 : 0);
  return el;
}

bool TrailLayer::xmlLoadState(const QDomElement& element) {
  return loadStyle(element);
}

TrailLayer::XmlLoadResult TrailLayer::xmlLoadStateResult(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "trail"_L1 || !detail::isLeafPayload(element)) {
    return XmlLoadResult::kInvalid;
  }
  TrailSource restored_source;
  const QString kind = element.attribute(u"source_kind"_s);
  if (kind == "tf_frame"_L1) {
    const QString frame = element.attribute(u"frame"_s);
    if (frame.isEmpty()) {
      return XmlLoadResult::kInvalid;
    }
    // Restored unconditionally: a frame absent from the current data behaves
    // like an orphan until it appears (follow-frame restore philosophy).
    restored_source = TrailSource::tfFrame(frame);
  } else if (kind == "pose_topic"_L1) {
    if (ctx_.session == nullptr) {
      return XmlLoadResult::kDeferred;
    }
    const QString topic_name = element.attribute(u"source_topic_name"_s);
    if (topic_name.isEmpty()) {
      return XmlLoadResult::kInvalid;
    }
    PJ::DatasetId saved_dataset_id = 0;
    if (element.hasAttribute(u"source_dataset_id"_s)) {
      bool id_ok = false;
      const qulonglong value = element.attribute(u"source_dataset_id"_s).toULongLong(&id_ok);
      if (!id_ok || value == 0 || value > std::numeric_limits<uint32_t>::max()) {
        return XmlLoadResult::kInvalid;
      }
      saved_dataset_id = static_cast<PJ::DatasetId>(value);
    }
    const QString source = element.attribute(u"source_dataset_source"_s);
    const QString path = element.attribute(u"source_dataset_path"_s);
    std::optional<PJ::ObjectTopicId> restored_topic;
    if (saved_dataset_id == 0 && source.isEmpty() && path.isEmpty()) {
      const UniqueObjectTopicResolution generic = resolveUniqueObjectTopic(
          ctx_.session->objectStore(), topic_name.toStdString(), PJ::sdk::BuiltinObjectType::kPosesInFrame);
      if (generic.ambiguous) {
        return XmlLoadResult::kInvalid;
      }
      restored_topic = generic.topic_id;
    } else {
      const PJ::DatasetIdentityResolution dataset =
          ctx_.session->resolveDatasetIdentity(saved_dataset_id, source, path);
      if (dataset.ambiguous) {
        return XmlLoadResult::kInvalid;
      }
      if (dataset.id.has_value()) {
        restored_topic = ctx_.session->objectStore().findTopic(*dataset.id, topic_name.toStdString());
      }
    }
    if (!restored_topic.has_value()) {
      return XmlLoadResult::kDeferred;
    }
    const PJ::ObjectTopicDescriptor& descriptor = ctx_.session->objectStore().descriptor(*restored_topic);
    const PJ::sdk::BuiltinObjectType live_type = builtinObjectTypeFor(descriptor);
    if (live_type != PJ::sdk::BuiltinObjectType::kNone && live_type != PJ::sdk::BuiltinObjectType::kPosesInFrame) {
      return XmlLoadResult::kInvalid;
    }
    restored_source = TrailSource::poseTopic(*restored_topic);
  } else {
    return XmlLoadResult::kInvalid;
  }
  if (!loadStyle(element)) {
    return XmlLoadResult::kInvalid;
  }
  source_ = restored_source;
  display_name_ = source_.kind == TrailSource::Kind::kTfFrame ? tr("Trail: %1").arg(source_.frame)
                                                              : tr("Trail: topic %1").arg(source_.topic.id);
  emit infoChanged();  // the Topics row was created before the source was known
  requestRebuild();    // no-op until a fixed frame is known; render() self-heals
  return XmlLoadResult::kRestored;
}

bool TrailLayer::loadStyle(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "trail"_L1 || !detail::isLeafPayload(element)) {
    return false;
  }
  const QColor past(element.attribute(u"past_color"_s, u"#1f77b4"_s));
  const QColor future(element.attribute(u"future_color"_s, u"#9ecae1"_s));
  if (!past.isValid() || !future.isValid()) {
    return false;
  }
  float restored_thickness = 4.0F;
  bool restored_past_visible = true;
  bool restored_future_visible = true;
  if (!detail::parseFiniteFloat(element, "thickness", 4.0F, 1.0F, 8.0F, restored_thickness) ||
      !detail::parseZeroOne(element, "past_visible", true, restored_past_visible) ||
      !detail::parseZeroOne(element, "future_visible", true, restored_future_visible)) {
    return false;
  }
  setPastColor(past);
  setFutureColor(future);
  setThickness(restored_thickness);
  setPastVisible(restored_past_visible);
  setFutureVisible(restored_future_visible);
  return true;
}

bool TrailLayer::attach(const PJ::SceneLayerContext& ctx) {
  const auto& scene3d_ctx = static_cast<const Scene3DLayerContext&>(ctx);
  if (scene3d_ctx.session == nullptr) {
    qCWarning(lcTrail) << "attach: session is null";
    return false;
  }
  ctx_ = scene3d_ctx;
  if (ctx_.tf_buffer == nullptr) {
    // Tolerated: layout restore replays layers BEFORE the config topic that
    // binds TF, so a restored trail starts buffer-less and the dock pushes the
    // buffer via setTransformBuffer once the binding lands.
    warning_ = tr("waiting for TF");
  }
  if (source_.kind == TrailSource::Kind::kPoseTopic &&
      !scene3d_ctx.session->parserBindingForObjectTopic(source_.topic) &&
      !hasCanonical3DCodec(PJ::sdk::BuiltinObjectType::kPosesInFrame)) {
    qCWarning(lcTrail) << "attach: no parser and no canonical codec for pose topic" << source_.topic.id;
    return false;
  }
  stamps_.clear();
  points_.clear();
  pass_.setPoints({});
  ++build_revision_;
  // fixed_frame_ stays empty: the first render() adopts it from the
  // FrameContext (authoritative) and triggers the initial rebuild.
  return true;
}

void TrailLayer::detach() {
  ++build_generation_;  // orphan any in-flight rebuild result
  stamps_.clear();
  points_.clear();
  pass_.setPoints({});
  ++build_revision_;
  setWarning({});
}

void TrailLayer::setTransformBuffer(std::shared_ptr<TransformBuffer> tf_buffer) {
  if (ctx_.tf_buffer == tf_buffer) {
    return;
  }
  ctx_.tf_buffer = std::move(tf_buffer);
  ++build_generation_;  // orphan any in-flight build against the old buffer
  stamps_.clear();
  points_.clear();
  pass_.setPoints({});
  ++build_revision_;
  if (ctx_.tf_buffer == nullptr) {
    setWarning(tr("waiting for TF"));
  } else {
    setWarning({});  // definitive state comes from the next adoptPolyline
    startRebuild();  // no-op until the first render adopts the fixed frame
  }
  emit repaintRequested();
}

void TrailLayer::setFixedFrame(const QString& frame) {
  // render() adopts the authoritative fixed frame from the FrameContext and
  // rebuilds there; a repaint is all that is needed to kick that path.
  Q_UNUSED(frame);
  emit repaintRequested();
}

void TrailLayer::setTrackerTime(PJ::Timepoint time) {
  // The split position is derived from frame_ctx.time at render, so the tick
  // itself only drives the live-append probe.
  Q_UNUSED(time);
  maybeAppendLiveSamples();
  emit repaintRequested();  // the renderKey gate coalesces between samples
}

void TrailLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  emit visibilityChanged(visible);
  emit repaintRequested();
}

void TrailLayer::initializeGL() {
  pass_.initializeGL();
}

void TrailLayer::releaseGL() {
  pass_.releaseGL();
}

void TrailLayer::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!visible_) {
    return;
  }
  // Fixed-frame self-heal: attach can precede the dock's fixed-frame fan-out,
  // and the render context is authoritative. Adopt + rebuild; draw next paint.
  if (fixed_frame_ != frame_ctx.fixed_frame) {
    fixed_frame_ = frame_ctx.fixed_frame;
    requestRebuild();
    return;
  }
  pass_.render(
      view_params, frame_ctx.render_origin, pastCount(frame_ctx.time), toVec3(past_color_), toVec3(future_color_),
      thickness_px_, past_visible_, future_visible_);
}

void TrailLayer::setPastColor(QColor color) {
  if (!color.isValid() || past_color_.rgb() == color.rgb()) {
    return;
  }
  past_color_ = color;
  ++style_revision_;
  emit configurationChanged();
  emit repaintRequested();
}

void TrailLayer::setFutureColor(QColor color) {
  if (!color.isValid() || future_color_.rgb() == color.rgb()) {
    return;
  }
  future_color_ = color;
  ++style_revision_;
  emit configurationChanged();
  emit repaintRequested();
}

void TrailLayer::setThickness(float px) {
  const float clamped = std::clamp(px, 1.0F, 8.0F);
  if (thickness_px_ == clamped) {
    return;
  }
  thickness_px_ = clamped;
  ++style_revision_;
  emit configurationChanged();
  emit repaintRequested();
}

void TrailLayer::setPastVisible(bool visible) {
  if (past_visible_ == visible) {
    return;
  }
  past_visible_ = visible;
  ++style_revision_;
  emit configurationChanged();
  emit repaintRequested();
}

void TrailLayer::setFutureVisible(bool visible) {
  if (future_visible_ == visible) {
    return;
  }
  future_visible_ = visible;
  ++style_revision_;
  emit configurationChanged();
  emit repaintRequested();
}

void TrailLayer::setWarning(const QString& warning) {
  if (warning_ == warning) {
    return;
  }
  warning_ = warning;
  emit statusWarningChanged();
}

void TrailLayer::requestRebuild() {
  ++build_generation_;
  startRebuild();
}

void TrailLayer::startRebuild(bool synchronous) {
  if (ctx_.tf_buffer == nullptr || fixed_frame_.empty()) {
    return;
  }
  if (source_.kind == TrailSource::Kind::kPoseTopic) {
    // GUI-thread only: ObjectStore reads + parseLocked decodes are per-use
    // GUI-thread contracts. Bounded: stamps decimate to kMaxTrailPoints BEFORE
    // any decode.
    adoptPolyline(buildPoseTrailNow());
    return;
  }
  if (synchronous) {
    adoptPolyline(buildTfFrameTrail(
        *ctx_.tf_buffer, fixed_frame_, source_.frame.toStdString(), PJ::Timepoint::min(), PJ::Timepoint::max(),
        kMaxTrailPoints));
    return;
  }
  if (inflight_) {
    return;  // onRebuildFinished sees generation != inflight_generation_ and relaunches
  }
  if (watcher_ == nullptr) {
    watcher_ = new QFutureWatcher<TrailPolyline>(this);
    connect(watcher_, &QFutureWatcher<TrailPolyline>::finished, this, &TrailLayer::onRebuildFinished);
  }
  inflight_ = true;
  inflight_generation_ = build_generation_;
  // The shared_ptr copy keeps the buffer alive off-thread; TransformBuffer is
  // shared_mutex-protected, so concurrent GUI-thread writes are safe.
  const std::shared_ptr<TransformBuffer> tf = ctx_.tf_buffer;
  const std::string fixed = fixed_frame_;
  const std::string frame = source_.frame.toStdString();
  watcher_->setFuture(QtConcurrent::run([tf, fixed, frame]() -> TrailPolyline {
    return buildTfFrameTrail(*tf, fixed, frame, PJ::Timepoint::min(), PJ::Timepoint::max(), kMaxTrailPoints);
  }));
}

void TrailLayer::onRebuildFinished() {
  inflight_ = false;
  // result() would rethrow a worker exception into this slot; buildTfFrameTrail
  // does allocation-only work, and nothing cancels the watcher (it dies with
  // the layer), so both QtConcurrent failure modes are structurally absent.
  TrailPolyline result = watcher_->result();
  if (inflight_generation_ == build_generation_) {
    adoptPolyline(std::move(result));
  } else {
    startRebuild();  // stale: parameters changed while running; go again
  }
}

void TrailLayer::adoptPolyline(TrailPolyline polyline) {
  stamps_.clear();
  points_.clear();
  stamps_.reserve(polyline.size());
  points_.reserve(polyline.size());
  for (const TrailPoint& point : polyline) {
    stamps_.push_back(point.t);
    points_.push_back(point.pos);
  }
  pass_.setPoints(points_);
  ++build_revision_;
  if (ctx_.tf_buffer != nullptr) {
    last_seen_tf_revision_ = ctx_.tf_buffer->revision();
  }
  if (stamps_.size() >= 2) {
    setWarning({});
  } else if (source_.kind == TrailSource::Kind::kTfFrame && !fixed_frame_.empty() && ctx_.tf_buffer != nullptr) {
    const bool connected = ctx_.tf_buffer->areConnected(fixed_frame_, source_.frame.toStdString());
    setWarning(
        connected
            ? tr("no motion samples for '%1'").arg(source_.frame)
            : tr("'%1' is not connected to fixed frame '%2'").arg(source_.frame, QString::fromStdString(fixed_frame_)));
  } else if (source_.kind == TrailSource::Kind::kPoseTopic) {
    setWarning(
        last_pose_decode_failures_ > 0
            ? tr("pose data could not be decoded (%1 message(s) failed)").arg(last_pose_decode_failures_)
            : tr("waiting for pose samples"));
  }
  emit repaintRequested();
}

void TrailLayer::appendTail(TrailPolyline tail) {
  if (tail.empty()) {
    return;
  }
  for (const TrailPoint& point : tail) {
    stamps_.push_back(point.t);
    points_.push_back(point.pos);
  }
  if (stamps_.size() > kMaxTrailPoints) {
    // Ring semantics at the cap: a long-running live stream stays bounded.
    const auto drop = static_cast<std::ptrdiff_t>(stamps_.size() - kMaxTrailPoints);
    stamps_.erase(stamps_.begin(), stamps_.begin() + drop);
    points_.erase(points_.begin(), points_.begin() + drop);
  }
  pass_.setPoints(points_);
  ++build_revision_;
  setWarning({});
  emit repaintRequested();
}

TrailPolyline TrailLayer::buildPoseTrailNow() {
  if (ctx_.session == nullptr || ctx_.tf_buffer == nullptr) {
    return {};
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  pose_seen_entry_count_ = store.entryCount(source_.topic);  // the rebuild consumes everything current
  last_pose_decode_failures_ = 0;
  const auto [range_lo, range_hi] = store.timeRange(source_.topic);
  // rangeByTime's window is (lo, hi]; step lo back one tick so the first entry
  // (timestamp == range_lo) is included.
  const auto entries = store.rangeByTime(source_.topic, range_lo - 1, range_hi);
  const std::vector<std::size_t> kept = decimateEvenly(entries.size(), kMaxTrailPoints);
  std::vector<PoseTrailSample> samples;
  samples.reserve(kept.size());
  // Per-use binding fetch (never cached) — a reload re-registers the parser slot.
  const auto binding = ctx_.session->parserBindingForObjectTopic(source_.topic);
  for (const std::size_t index : kept) {
    if (auto sample = decodePoseSample(binding, entries[index].uid, last_pose_decode_failures_)) {
      samples.push_back(std::move(*sample));
    }
  }
  if (last_pose_decode_failures_ > 0) {
    qCWarning(lcTrail) << "pose trail rebuild: failed to decode" << last_pose_decode_failures_ << "of" << kept.size()
                       << "messages on topic" << source_.topic.id;
  }
  return buildPoseTopicTrail(*ctx_.tf_buffer, fixed_frame_, samples);
}

std::optional<PoseTrailSample> TrailLayer::decodePoseSample(
    const PJ::SessionManager::ParserBinding& binding, PJ::SequentialUID uid, std::size_t& decode_failures) const {
  const auto entry = ctx_.session->objectStore().at(source_.topic, uid);
  if (entry.has_value() && entry->fetch_failed) {
    ++decode_failures;  // lazy re-read failed: fold into the row's aggregate, don't retry
    return std::nullopt;
  }
  if (!entry.has_value() || entry->payload.bytes.empty()) {
    return std::nullopt;
  }
  auto object = resolveObject(binding, PJ::sdk::BuiltinObjectType::kPosesInFrame, entry->timestamp, entry->payload);
  if (!object.has_value()) {
    // Aggregate, not per-entry: a schema regression fails ALL of them and a
    // 50k-line log helps nobody. Callers log the count once and adoptPolyline
    // surfaces it on the row.
    ++decode_failures;
    return std::nullopt;
  }
  const auto* message = object->object.get<PJ::sdk::PosesInFrame>();
  if (message == nullptr || message->poses.empty()) {
    return std::nullopt;
  }
  // v1 contract: the FIRST pose only (one trail point per message).
  const auto& pose = message->poses.front();
  return PoseTrailSample{
      PJ::fromRaw(entry->timestamp), glm::dvec3(pose.position.x, pose.position.y, pose.position.z), message->frame_id};
}

void TrailLayer::maybeAppendLiveSamples() {
  if (inflight_ || ctx_.tf_buffer == nullptr || fixed_frame_.empty()) {
    return;  // a full rebuild is running; it lands on the live edge itself
  }
  if (source_.kind == TrailSource::Kind::kTfFrame) {
    const uint64_t tf_revision = ctx_.tf_buffer->revision();
    if (tf_revision == last_seen_tf_revision_) {
      return;
    }
    // An empty trail has no anchor stamp to append after — but new TF data
    // means it may be buildable NOW. Revive with a full rebuild instead of
    // leaving the "waiting"/"no motion samples" warning up forever. The gate
    // advances first so a still-empty result cannot retrigger every tick.
    last_seen_tf_revision_ = tf_revision;
    if (stamps_.empty()) {
      requestRebuild();
      return;
    }
    const PJ::Timepoint after = stamps_.back() + std::chrono::nanoseconds(1);
    appendTail(buildTfFrameTrail(
        *ctx_.tf_buffer, fixed_frame_, source_.frame.toStdString(), after, PJ::Timepoint::max(), kMaxTrailPoints));
    return;
  }
  if (ctx_.session == nullptr) {
    return;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  const std::size_t entry_count = store.entryCount(source_.topic);
  if (entry_count == pose_seen_entry_count_) {
    return;
  }
  if (stamps_.empty()) {
    // Same revival rule as the TF branch (the rebuild advances the cursor).
    requestRebuild();
    return;
  }
  pose_seen_entry_count_ = entry_count;
  const PJ::Timepoint after = stamps_.back() + std::chrono::nanoseconds(1);
  const auto entries = store.rangeByTime(source_.topic, PJ::toRaw(after) - 1, std::numeric_limits<int64_t>::max());
  std::vector<PoseTrailSample> samples;
  samples.reserve(entries.size());
  const auto binding = ctx_.session->parserBindingForObjectTopic(source_.topic);
  std::size_t decode_failures = 0;
  for (const auto& range_entry : entries) {
    if (auto sample = decodePoseSample(binding, range_entry.uid, decode_failures)) {
      samples.push_back(std::move(*sample));
    }
  }
  if (decode_failures > 0) {
    qCWarning(lcTrail) << "pose trail live append: failed to decode" << decode_failures << "of" << entries.size()
                       << "new messages on topic" << source_.topic.id;
  }
  appendTail(buildPoseTopicTrail(*ctx_.tf_buffer, fixed_frame_, samples));
}

QWidget* TrailLayer::createConfigWidget(QWidget* parent) {
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

  const QString source_text =
      source_.kind == TrailSource::Kind::kTfFrame ? source_.frame : tr("topic %1").arg(source_.topic.id);
  auto* source_label = new QLabel(source_text, container);
  form->addRow(tr("Tracks:"), source_label);

  appendStyleRows(form, container);

  return container;
}

void TrailLayer::appendStyleRows(QFormLayout* form, QWidget* parent) {
  auto* thickness_spin = new PJ::DoubleScrubber(parent);
  thickness_spin->setRange(1.0, 8.0);
  thickness_spin->setDecimals(0);
  thickness_spin->setSingleStep(1.0);
  thickness_spin->setValue(static_cast<double>(thickness_px_));
  form->addRow(tr("Thickness (px):"), thickness_spin);
  QObject::connect(thickness_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) {
    setThickness(static_cast<float>(v));
  });

  // Color rows carry an eye toggle (the LayerListView row pattern) that hides
  // that half of the ribbon independently. Resources are registered
  // process-wide by pj_app.
  const QString icon_theme = PJ::theme::appTheme() == PJ::theme::Theme::Light ? u"light"_s : u"dark"_s;
  const auto make_eye = [icon_theme](QWidget* eye_parent, bool visible_now) {
    auto* eye = new QToolButton(eye_parent);
    // The shared objectName picks up the QSS rule that keeps eye toggles flat
    // in every state (no checked/hover wash) — the glyph is the indicator,
    // matching every other eye in the app.
    eye->setObjectName(u"curveVisibilityToggle"_s);
    eye->setIconSize(QSize(20, 20));
    eye->setAutoRaise(true);
    eye->setCheckable(true);
    eye->setChecked(visible_now);
    eye->setFocusPolicy(Qt::NoFocus);
    eye->setIcon(
        PJ::loadSvg(
            visible_now ? u":/resources/svg/visibility.svg"_s : u":/resources/svg/visibility_off.svg"_s, icon_theme));
    return eye;
  };
  const auto refresh_eye = [icon_theme](QToolButton* eye, bool on) {
    eye->setIcon(
        PJ::loadSvg(on ? u":/resources/svg/visibility.svg"_s : u":/resources/svg/visibility_off.svg"_s, icon_theme));
  };

  // The past/future rows are twins: swatch + eye, differing only in the bound
  // half. Member-pointer setters keep the lambda captures tiny.
  const auto add_color_row = [this, parent, form, make_eye, refresh_eye](
                                 const QString& label, const QColor& color, bool visible_now, const QString& tip,
                                 void (TrailLayer::*set_color)(QColor), void (TrailLayer::*set_visible)(bool)) {
    auto* row = new QWidget(parent);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(
        PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
        PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
    row_layout->setSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
    auto* swatch = new PJ::ColorPickerWidget(row);
    swatch->setColor(color);
    auto* eye = make_eye(row, visible_now);
    eye->setToolTip(tip);
    row_layout->addWidget(swatch);
    row_layout->addWidget(eye);
    row_layout->addStretch();
    form->addRow(label, row);
    QObject::connect(
        swatch, &PJ::ColorPickerWidget::colorChanged, this, [this, set_color](QColor c) { (this->*set_color)(c); });
    QObject::connect(eye, &QToolButton::toggled, this, [this, eye, refresh_eye, set_visible](bool on) {
      (this->*set_visible)(on);
      refresh_eye(eye, on);
    });
  };
  add_color_row(
      tr("Past color:"), past_color_, past_visible_, tr("Show/hide the path before the current time"),
      &TrailLayer::setPastColor, &TrailLayer::setPastVisible);
  add_color_row(
      tr("Future color:"), future_color_, future_visible_, tr("Show/hide the path after the current time"),
      &TrailLayer::setFutureColor, &TrailLayer::setFutureVisible);
}

}  // namespace pj::scene3d
