// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/wasm/point_cloud_layer_wasm.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLoggingCategory>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
#include <QtConcurrentRun>
#endif
#include <algorithm>
#include <any>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

#include "layers/layer_xml_validation.h"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/pointcloud_convert.h"
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
#include "pj_scene3d_core/pointcloud_codecs.h"
#endif
#include "pj_scene3d_widgets/resolve_object.h"
#include "pj_widgets/CheckButton.h"
#include "pj_widgets/ColorPickerWidget.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"

using namespace Qt::StringLiterals;

namespace pj::scene3d {
namespace {

Q_LOGGING_CATEGORY(lcWasmPointCloud, "pj.scene3d.wasm.pointcloud")

std::pair<float, float> finiteRange(const std::vector<float>& values) {
  float minimum = std::numeric_limits<float>::max();
  float maximum = std::numeric_limits<float>::lowest();
  for (const float value : values) {
    if (std::isfinite(value)) {
      minimum = std::min(minimum, value);
      maximum = std::max(maximum, value);
    }
  }
  if (minimum > maximum) {
    return {0.0F, 1.0F};
  }
  if (maximum - minimum < 1.0e-6F) {
    maximum = minimum + 1.0F;
  }
  return {minimum, maximum};
}

int spatialAxis(std::string_view field) {
  if (field == "x") {
    return 0;
  }
  if (field == "y") {
    return 1;
  }
  if (field == "z") {
    return 2;
  }
  return -1;
}

QString defaultColorField(const QStringList& fields) {
  if (fields.isEmpty()) {
    return {};
  }
  return fields.contains(u"intensity"_s) ? u"intensity"_s : fields.front();
}

QString shapeName(WasmPointCloudLayer::Shape shape) {
  switch (shape) {
    case WasmPointCloudLayer::Shape::kSphere:
      return u"sphere"_s;
    case WasmPointCloudLayer::Shape::kPoint:
      return u"point"_s;
    case WasmPointCloudLayer::Shape::kCube:
      return u"cube"_s;
  }
  return u"sphere"_s;
}

}  // namespace

WasmPointCloudLayer::WasmPointCloudLayer(
    PJ::ObjectTopicId topic_id, QString display_name, PJ::sdk::BuiltinObjectType object_type, QObject* parent)
    : PJ::ISceneLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)), object_type_(object_type) {}

WasmPointCloudLayer::~WasmPointCloudLayer() = default;

PJ::SceneLayerInfo WasmPointCloudLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = object_type_,
      .display_name = display_name_,
      .family_name = u"PointCloud"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> WasmPointCloudLayer::timeRange() const {
  return PJ::liveTopicTimeRange(session_ != nullptr ? &session_->objectStore() : nullptr, topic_id_);
}

bool WasmPointCloudLayer::attach(const PJ::SceneLayerContext& context) {
  if (context.session == nullptr) {
    qCWarning(lcWasmPointCloud) << "attach: session is null";
    return false;
  }
  session_ = context.session;
  if (!session_->parserBindingForObjectTopic(topic_id_) && !hasCanonical3DCodec(object_type_)) {
    qCWarning(lcWasmPointCloud) << "attach: no parser or canonical codec for topic" << topic_id_.id;
    session_ = nullptr;
    return false;
  }
  disconnect(reload_connection_);
  reload_connection_ = connect(
      session_, &PJ::SessionManager::datasetAboutToBeReplaced, this, &WasmPointCloudLayer::onDatasetAboutToBeReplaced);
  if (!bootstrap()) {
    qCDebug(lcWasmPointCloud) << "attach: first sample is not available yet for topic" << topic_id_.id;
  }
  const PJ::Range<PJ::Timepoint> range = timeRange();
  if (range.max >= range.min) {
    requested_time_ = range.min;
    decode_dirty_ = true;
  }
  return true;
}

void WasmPointCloudLayer::detach() {
  disconnect(reload_connection_);
  reload_connection_ = {};
  session_ = nullptr;
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
  invalidateCompressedState();
#endif
  requested_time_.reset();
  budget_rejected_vertex_count_.reset();
  decode_dirty_ = false;
  active_sample_ = {};
  clearGeometry();
  updateSourceFrame({});
  available_color_fields_.clear();
  has_color_ = false;
  setWarning({});
}

void WasmPointCloudLayer::setTrackerTime(PJ::Timepoint time) {
  requested_time_ = time;
  decode_dirty_ = true;
  if (visible_) {
    emit repaintRequested();
  }
}

std::uint64_t WasmPointCloudLayer::renderKey(PJ::Timepoint time) const {
  if (session_ == nullptr) {
    return PJ::kNoSampleRenderKey;
  }
  PJ::ObjectStore& store = session_->objectStore();
  const auto index = store.indexAt(topic_id_, PJ::toRaw(time));
  if (!index.has_value()) {
    return PJ::kNoSampleRenderKey;
  }
  const auto stamps = store.entryTimestamps(topic_id_);
  return *index < stamps.size() ? static_cast<std::uint64_t>(stamps[*index]) : PJ::kNoSampleRenderKey;
}

void WasmPointCloudLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  if (!visible_) {
    // Hidden browser layers release their retained geometry as well as their
    // view-owned GPU buffer. Re-showing is seeded at the current playhead by
    // SceneDockWidget and decodes once, keeping the per-view memory ceiling real.
    active_sample_ = {};
    budget_rejected_vertex_count_.reset();
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
    invalidateCompressedState();
#endif
    clearGeometry();
  } else {
    decode_dirty_ = requested_time_.has_value();
  }
  emit visibilityChanged(visible_);
  emit repaintRequested();
}

bool WasmPointCloudLayer::prepareForRender(std::uint64_t remaining_view_vertices) {
  if (budget_rejected_vertex_count_.has_value()) {
    if (*budget_rejected_vertex_count_ > remaining_view_vertices) {
      return false;
    }
    budget_rejected_vertex_count_.reset();
    active_sample_ = {};
    decode_dirty_ = requested_time_.has_value();
  }
  if (static_cast<std::uint64_t>(vertices_.size()) > remaining_view_vertices) {
    budget_rejected_vertex_count_ = vertices_.size();
    setWarning(tr("Layer has %1 finite points; only %2 remain in the browser view budget")
                   .arg(QString::number(vertices_.size()), QString::number(remaining_view_vertices)));
    clearGeometry();
    return true;
  }
  if (!visible_ || !decode_dirty_ || !requested_time_.has_value()) {
    return false;
  }
  decode_dirty_ = false;
  const bool changed = decodeAt(*requested_time_);
  if (static_cast<std::uint64_t>(vertices_.size()) > remaining_view_vertices) {
    budget_rejected_vertex_count_ = vertices_.size();
    setWarning(tr("Layer has %1 finite points; only %2 remain in the browser view budget")
                   .arg(QString::number(vertices_.size()), QString::number(remaining_view_vertices)));
    clearGeometry();
    return true;
  }
  return changed;
}

bool WasmPointCloudLayer::bootstrap() {
  if (session_ == nullptr) {
    return false;
  }
  PJ::ObjectStore& store = session_->objectStore();
  const auto first = store.at(topic_id_, 0);
  if (!first.has_value() || first->payload.bytes.empty()) {
    return false;
  }
  const auto binding = session_->parserBindingForObjectTopic(topic_id_);
  auto object = resolveObject(binding, object_type_, first->timestamp, first->payload);
  if (!object.has_value()) {
    return false;
  }
  const auto* cloud = object->object.get<PJ::sdk::PointCloud>();
  if (cloud != nullptr) {
    updateSourceFrame(cloud->frame_id);
    populateColorFields(*cloud);
    return true;
  }
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
  if (const auto* compressed = object->object.get<PJ::sdk::CompressedPointCloud>()) {
    updateSourceFrame(compressed->frame_id);
    return true;
  }
#endif
  return false;
}

bool WasmPointCloudLayer::decodeAt(PJ::Timepoint time) {
  if (session_ == nullptr) {
    return false;
  }
  PJ::ObjectStore& store = session_->objectStore();
  const auto resolved = store.latestAt(topic_id_, PJ::toRaw(time));
  if (!resolved.has_value() || resolved->payload.bytes.empty()) {
    return false;
  }
  const SampleId sample{resolved->timestamp, resolved->payload.bytes.size()};
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
  wanted_ = sample;
#endif
  if (sample == active_sample_) {
    return false;
  }

#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
  if (decoded_cache_ && decoded_cache_id_ == sample) {
    const bool changed = pushCloud(*decoded_cache_, sample);
    if (!warning_reason_.isEmpty()) {
      failed_id_ = sample;
      failed_warning_ = warning_reason_;
      decoded_cache_.reset();
      decoded_cache_id_ = {};
    }
    return changed;
  }
  if (sample == failed_id_) {
    active_sample_ = sample;
    clearGeometry();
    setWarning(failed_warning_);
    return true;
  }
#endif

  const auto binding = session_->parserBindingForObjectTopic(topic_id_);
  auto object = resolveObject(binding, object_type_, resolved->timestamp, resolved->payload);
  if (!object.has_value()) {
    setWarning(tr("Point cloud could not be decoded: %1").arg(QString::fromStdString(object.error())));
    clearGeometry();
    active_sample_ = sample;
    return true;
  }

#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
  if (const auto* compressed = object->object.get<PJ::sdk::CompressedPointCloud>()) {
    updateSourceFrame(compressed->frame_id);
    if (!browserPointPayloadFits(static_cast<std::uint64_t>(compressed->data.size()))) {
      failed_warning_ =
          tr("Compressed cloud payload is %1 MiB; the browser limit is %2 MiB")
              .arg(QString::number(static_cast<double>(compressed->data.size()) / (1024.0 * 1024.0), 'f', 1))
              .arg(kMaxWireBytesPerCloud / (1024ULL * 1024ULL));
      setWarning(failed_warning_);
      clearGeometry();
      active_sample_ = sample;
      failed_id_ = sample;
      return true;
    }
    requestCompressedDecode(*compressed, sample);
    return false;
  }
#endif

  const auto* cloud = object->object.get<PJ::sdk::PointCloud>();
  if (cloud == nullptr) {
    setWarning(tr("Point-cloud sample has an unexpected canonical type"));
    clearGeometry();
    active_sample_ = sample;
    return true;
  }
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
  pending_.reset();
#endif
  return pushCloud(*cloud, sample);
}

bool WasmPointCloudLayer::pushCloud(const PJ::sdk::PointCloud& cloud, SampleId sample) {
  updateSourceFrame(cloud.frame_id);
  populateColorFields(cloud);
  const std::uint64_t point_count = static_cast<std::uint64_t>(cloud.width) * cloud.height;
  if (!browserPointCountFits(point_count)) {
    setWarning(tr("Cloud has %1 points; the browser limit is %2")
                   .arg(QString::number(point_count), QString::number(kMaxPointsPerCloud)));
    clearGeometry();
    active_sample_ = sample;
    return true;
  }
  if (!browserPointPayloadFits(static_cast<std::uint64_t>(cloud.data.size()))) {
    setWarning(tr("Cloud payload is %1 MiB; the browser limit is %2 MiB")
                   .arg(QString::number(static_cast<double>(cloud.data.size()) / (1024.0 * 1024.0), 'f', 1))
                   .arg(kMaxWireBytesPerCloud / (1024ULL * 1024ULL)));
    clearGeometry();
    active_sample_ = sample;
    return true;
  }

  const bool rgb_mode = color_type_ == ColorType::kRgb && has_color_;
  const int axis = rgb_mode ? -1 : scalarAxis();
  const std::string_view scalar_field =
      (rgb_mode || color_type_ == ColorType::kSolid || axis >= 0) ? std::string_view{} : std::string_view{color_field_};
  ConvertedPointCloud converted;
  try {
    converted = convertCanonical(cloud, scalar_field, /*extract_rgba=*/rgb_mode);
  } catch (const std::bad_alloc&) {
    setWarning(tr("Not enough browser memory to convert this point cloud"));
    clearGeometry();
    active_sample_ = sample;
    return true;
  } catch (const std::exception& error) {
    setWarning(tr("Point-cloud conversion failed: %1").arg(QString::fromUtf8(error.what())));
    clearGeometry();
    active_sample_ = sample;
    return true;
  }

  if (point_count != 0 && converted.cloud.positions.empty()) {
    setWarning(tr("Malformed point cloud was rejected"));
    clearGeometry();
    active_sample_ = sample;
    return true;
  }

  std::vector<WasmPointVertex> next_vertices;
  next_vertices.reserve(converted.cloud.positions.size());
  const bool has_scalar = converted.cloud.scalar.size() == converted.cloud.positions.size();
  const bool has_rgba = converted.cloud.rgba.size() == converted.cloud.positions.size();
  for (std::size_t index = 0; index < converted.cloud.positions.size(); ++index) {
    const glm::vec3& position = converted.cloud.positions[index];
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
      continue;
    }
    next_vertices.push_back(
        WasmPointVertex{
            .x = position.x,
            .y = position.y,
            .z = position.z,
            .scalar = has_scalar ? converted.cloud.scalar[index] : 0.0F,
            .rgba = has_rgba ? converted.cloud.rgba[index] : 0xFFFFFFFFU,
        });
  }
  automatic_scalar_range_ = has_scalar ? finiteRange(converted.cloud.scalar) : std::pair<float, float>{0.0F, 1.0F};
  if (next_vertices.empty()) {
    active_sample_ = sample;
    clearGeometry();
    setWarning(tr("Point cloud contains no finite positions"));
    return true;
  }
  vertices_ = std::move(next_vertices);
  source_bounds_ = converted.bounds;
  active_sample_ = sample;
  ++geometry_revision_;
  setWarning({});
  return true;
}

#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
void WasmPointCloudLayer::ensureDecodeWorker() {
  if (decode_watcher_ != nullptr) {
    return;
  }
  decode_watcher_ = new QFutureWatcher<DecodeResult>(this);
  connect(
      decode_watcher_, &QFutureWatcher<DecodeResult>::finished, this, &WasmPointCloudLayer::onCompressedDecodeFinished);
}

void WasmPointCloudLayer::requestCompressedDecode(const PJ::sdk::CompressedPointCloud& cloud, SampleId sample) {
  updateSourceFrame(cloud.frame_id);
  wanted_ = sample;
  ensureDecodeWorker();
  if (inflight_ == sample && inflight_generation_ == decode_generation_) {
    pending_.reset();
    return;
  }
  // A future may already be complete while its finished signal is still queued.
  // `inflight_`, cleared only by that signal, closes the setFuture() race and
  // collapses arbitrary scrubbing to the newest one pending sample.
  if (inflight_ != SampleId{}) {
    pending_ = PendingDecode{cloud, sample, decode_generation_};
    return;
  }
  startCompressedDecode(cloud, sample, decode_generation_);
}

void WasmPointCloudLayer::startCompressedDecode(
    const PJ::sdk::CompressedPointCloud& cloud, SampleId sample, std::uint64_t generation) {
  inflight_ = sample;
  inflight_generation_ = generation;
  ++decode_starts_;
  // The canonical wrapper owns an immutable anchor to the compressed bytes. The
  // worker captures it by value and touches no QObject/layer state, so a hidden,
  // detached, reloaded, or destroyed layer never requires a main-thread wait.
  PJ::sdk::CompressedPointCloud snapshot = cloud;
  QThread* const main_thread = QThread::currentThread();
  decode_watcher_->setFuture(
      QtConcurrent::run([snapshot = std::move(snapshot), sample, generation, main_thread]() -> DecodeResult {
        DecodeResult result{
            .id = sample,
            .generation = generation,
            .cloud = {},
            .error = {},
            .ran_off_main_thread = QThread::currentThread() != main_thread,
        };
        // Keep every exception inside the worker. QFutureWatcher::result() must
        // never rethrow on the browser UI thread.
        try {
          auto decoded = decodeCompressedPointCloud(
              snapshot, PointCloudDecodeLimits{
                            .max_points = WasmPointCloudLayer::kMaxPointsPerCloud,
                            .max_decoded_bytes = WasmPointCloudLayer::kMaxWireBytesPerCloud,
                        });
          if (decoded.has_value()) {
            result.cloud = std::make_shared<PJ::sdk::PointCloud>(std::move(decoded.value()));
          } else {
            result.error = QString::fromStdString(decoded.error());
          }
        } catch (const std::bad_alloc&) {
          result.error = u"out of memory finalizing decoded point cloud"_s;
        } catch (const std::exception& error) {
          result.error = QString::fromUtf8(error.what());
        } catch (...) {
          result.error = u"unknown exception decoding point cloud"_s;
        }
        return result;
      }));
}

void WasmPointCloudLayer::onCompressedDecodeFinished() {
  const DecodeResult result = decode_watcher_->result();
  ++decode_completions_;
  decode_off_main_completions_ += result.ran_off_main_thread ? 1 : 0;
  inflight_ = {};
  inflight_generation_ = 0;
  const bool generation_is_current = result.generation == decode_generation_;
  const bool sample_is_current = generation_is_current && result.id == wanted_ && visible_ && session_ != nullptr;

  if (generation_is_current && result.cloud) {
    const std::uint64_t point_count = static_cast<std::uint64_t>(result.cloud->width) * result.cloud->height;
    const bool decoded_budget_fits = browserPointCountFits(point_count) &&
                                     browserPointPayloadFits(static_cast<std::uint64_t>(result.cloud->data.size()));
    if (decoded_budget_fits) {
      decoded_cache_ = result.cloud;
      decoded_cache_id_ = result.id;
    } else {
      // Do not let an over-budget decompression remain pinned in the WASM heap.
      failed_id_ = result.id;
      failed_warning_ =
          !browserPointCountFits(point_count)
              ? tr("Cloud has %1 points; the browser limit is %2")
                    .arg(QString::number(point_count), QString::number(kMaxPointsPerCloud))
              : tr("Cloud payload is %1 MiB; the browser limit is %2 MiB")
                    .arg(QString::number(static_cast<double>(result.cloud->data.size()) / (1024.0 * 1024.0), 'f', 1))
                    .arg(kMaxWireBytesPerCloud / (1024ULL * 1024ULL));
    }
    if (sample_is_current) {
      if (decoded_budget_fits) {
        pushCloud(*result.cloud, result.id);
        if (!warning_reason_.isEmpty()) {
          failed_id_ = result.id;
          failed_warning_ = warning_reason_;
          decoded_cache_.reset();
          decoded_cache_id_ = {};
        }
      } else {
        active_sample_ = result.id;
        clearGeometry();
        setWarning(failed_warning_);
      }
      emit repaintRequested();
    }
  } else if (generation_is_current && !result.cloud) {
    failed_id_ = result.id;
    failed_warning_ = tr("Compressed point cloud could not be decoded: %1").arg(result.error);
    if (sample_is_current) {
      active_sample_ = result.id;
      clearGeometry();
      setWarning(failed_warning_);
      emit repaintRequested();
    }
  }

  if (pending_.has_value()) {
    PendingDecode next = std::move(*pending_);
    pending_.reset();
    if (next.generation == decode_generation_ && next.id == wanted_ && next.id != failed_id_ && visible_ &&
        session_ != nullptr) {
      startCompressedDecode(next.cloud, next.id, next.generation);
    }
  }
}

void WasmPointCloudLayer::invalidateCompressedState() {
  ++decode_generation_;
  if (decode_generation_ == 0) {
    decode_generation_ = 1;
  }
  pending_.reset();
  decoded_cache_.reset();
  decoded_cache_id_ = {};
  wanted_ = {};
  failed_id_ = {};
  failed_warning_.clear();
}
#endif

void WasmPointCloudLayer::populateColorFields(const PJ::sdk::PointCloud& cloud) {
  const bool next_has_color = detectColorLayout(cloud).valid;
  QStringList next_fields;
  const auto is_color = [](std::string_view name) {
    return name == "red" || name == "green" || name == "blue" || name == "alpha" || name == "rgb" || name == "rgba";
  };
  for (const auto& field : cloud.fields) {
    if (field.name != "timestamp" && !(next_has_color && is_color(field.name))) {
      next_fields.push_back(QString::fromStdString(field.name));
    }
  }
  has_color_ = next_has_color;
  if (!color_choice_explicit_ && has_color_) {
    color_type_ = ColorType::kRgb;
  } else if (color_type_ == ColorType::kRgb && !has_color_) {
    color_type_ = ColorType::kField;
  }
  if (color_field_.empty() || !next_fields.contains(QString::fromStdString(color_field_))) {
    color_field_ = defaultColorField(next_fields).toStdString();
  }
  if (next_fields != available_color_fields_) {
    available_color_fields_ = next_fields;
    emit colorFieldsChanged(available_color_fields_);
  }
}

void WasmPointCloudLayer::updateSourceFrame(const std::string& frame) {
  if (source_frame_ == frame) {
    return;
  }
  source_frame_ = frame;
  emit sourceFrameChanged(QString::fromStdString(source_frame_));
}

void WasmPointCloudLayer::setWarning(QString warning) {
  if (warning_reason_ == warning) {
    return;
  }
  warning_reason_ = std::move(warning);
  emit warningChanged(!warning_reason_.isEmpty(), warning_reason_);
}

void WasmPointCloudLayer::clearGeometry() {
  const bool had_geometry = !vertices_.empty() || source_bounds_.valid;
  // clear() retains capacity, which would make a hidden or budget-rejected
  // million-point layer continue pinning ~20 MiB in the WASM heap.
  std::vector<WasmPointVertex>{}.swap(vertices_);
  source_bounds_ = {};
  if (had_geometry) {
    ++geometry_revision_;
  }
}

int WasmPointCloudLayer::scalarAxis() const {
  return color_type_ == ColorType::kField ? spatialAxis(color_field_) : -1;
}

std::pair<float, float> WasmPointCloudLayer::scalarRange(const glm::mat4& fixed_from_source) const {
  if (!auto_range_) {
    return {manual_range_min_, manual_range_max_};
  }
  const int axis = scalarAxis();
  if (axis >= 0) {
    return transformedAabbAxisRange(source_bounds_, fixed_from_source, axis);
  }
  return automatic_scalar_range_;
}

void WasmPointCloudLayer::noteRenderedScalarRange(float minimum, float maximum) {
  if (!auto_range_ || !std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum ||
      last_rendered_scalar_range_ == std::pair<float, float>{minimum, maximum}) {
    return;
  }
  last_rendered_scalar_range_ = {minimum, maximum};
  emit autoRangeComputed(minimum, maximum);
}

void WasmPointCloudLayer::requestDecode() {
  active_sample_ = {};
  decode_dirty_ = requested_time_.has_value();
  emit repaintRequested();
}

void WasmPointCloudLayer::setShape(Shape shape) {
  if (shape_ == shape) {
    return;
  }
  shape_ = shape;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmPointCloudLayer::setSizeMeters(float metres) {
  metres = std::clamp(metres, 0.001F, 10.0F);
  if (size_meters_ == metres) {
    return;
  }
  size_meters_ = metres;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmPointCloudLayer::setSizePixels(float pixels) {
  pixels = std::clamp(pixels, 1.0F, 32.0F);
  if (size_pixels_ == pixels) {
    return;
  }
  size_pixels_ = pixels;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmPointCloudLayer::setColorType(ColorType type) {
  const bool explicitness_changed = !color_choice_explicit_;
  color_choice_explicit_ = true;
  if (color_type_ == type) {
    if (explicitness_changed) {
      emit configurationChanged();
    }
    return;
  }
  color_type_ = type;
  emit configurationChanged();
  requestDecode();
}

void WasmPointCloudLayer::setColorField(const QString& field) {
  const std::string next = field.toStdString();
  if (color_field_ == next) {
    return;
  }
  color_field_ = next;
  emit configurationChanged();
  requestDecode();
}

void WasmPointCloudLayer::setSolidColor(QColor color) {
  if (!color.isValid() || solid_color_.rgb() == color.rgb()) {
    return;
  }
  solid_color_ = color;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmPointCloudLayer::setColormap(PJ::Colormap colormap) {
  if (colormap_ == colormap) {
    return;
  }
  colormap_ = colormap;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmPointCloudLayer::setInvertLut(bool invert) {
  if (invert_lut_ == invert) {
    return;
  }
  invert_lut_ = invert;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmPointCloudLayer::setAutoRange(bool automatic) {
  if (auto_range_ == automatic) {
    return;
  }
  if (!automatic) {
    manual_range_min_ = last_rendered_scalar_range_.first;
    manual_range_max_ = last_rendered_scalar_range_.second;
    emit autoRangeComputed(manual_range_min_, manual_range_max_);
  }
  auto_range_ = automatic;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmPointCloudLayer::setManualRange(float minimum, float maximum) {
  if (minimum > maximum || (manual_range_min_ == minimum && manual_range_max_ == maximum)) {
    return;
  }
  manual_range_min_ = minimum;
  manual_range_max_ = maximum;
  emit configurationChanged();
  if (!auto_range_) {
    emit repaintRequested();
  }
}

void WasmPointCloudLayer::setOutsideRangeOpacity(float opacity) {
  opacity = std::clamp(opacity, 0.0F, 1.0F);
  if (outside_range_opacity_ == opacity) {
    return;
  }
  outside_range_opacity_ = opacity;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmPointCloudLayer::setOutsideRangeVisible(bool visible) {
  if (outside_range_visible_ == visible) {
    return;
  }
  outside_range_visible_ = visible;
  emit configurationChanged();
  emit repaintRequested();
}

QWidget* WasmPointCloudLayer::createConfigWidget(QWidget* parent) {
  auto* container = new QWidget(parent);
  auto* form = new QFormLayout(container);
  form->setContentsMargins(0, 0, 0, 0);

  auto* shape = new PJ::ComboBox(container);
  shape->setObjectName(u"wasmPointCloudShape"_s);
  shape->addItem(tr("sphere"), static_cast<int>(Shape::kSphere));
  shape->addItem(tr("point"), static_cast<int>(Shape::kPoint));
  shape->addItem(tr("cube"), static_cast<int>(Shape::kCube));
  shape->setCurrentIndex(static_cast<int>(shape_));
  form->addRow(tr("Shape:"), shape);

  auto* size = new PJ::DoubleScrubber(container);
  size->setObjectName(u"wasmPointCloudSize"_s);
  const auto refresh_size = [this, size]() {
    QSignalBlocker blocker(size);
    if (shape_ == Shape::kPoint) {
      size->setRange(1.0, 32.0);
      size->setDecimals(1);
      size->setSuffix(u" px"_s);
      size->setValue(size_pixels_);
    } else {
      size->setRange(0.001, 10.0);
      size->setDecimals(3);
      size->setSuffix(u" m"_s);
      size->setValue(size_meters_);
    }
  };
  refresh_size();
  form->addRow(tr("Point size:"), size);

  auto* color_type = new PJ::ComboBox(container);
  color_type->setObjectName(u"wasmPointCloudColorType"_s);
  const auto rebuild_color_types = [this, color_type]() {
    QSignalBlocker blocker(color_type);
    color_type->clear();
    color_type->addItem(tr("Solid"), u"solid"_s);
    if (has_color_) {
      color_type->addItem(tr("RGB"), u"rgb"_s);
    }
    for (const QString& field : available_color_fields_) {
      color_type->addItem(tr("Field: %1").arg(field), u"field:"_s + field);
    }
    const QString selected = color_type_ == ColorType::kSolid ? u"solid"_s
                             : color_type_ == ColorType::kRgb ? u"rgb"_s
                                                              : u"field:"_s + QString::fromStdString(color_field_);
    const int index = color_type->findData(selected);
    color_type->setCurrentIndex(index >= 0 ? index : 0);
  };
  rebuild_color_types();
  form->addRow(tr("Color type:"), color_type);

  auto* solid_color = new PJ::ColorPickerWidget(container);
  solid_color->setObjectName(u"wasmPointCloudSolidColor"_s);
  solid_color->setColor(solid_color_);
  form->addRow(tr("Solid color:"), solid_color);

  auto* colormap = new PJ::ComboBox(container);
  colormap->setObjectName(u"wasmPointCloudColormap"_s);
  for (const PJ::Colormap value :
       {PJ::Colormap::kTurbo, PJ::Colormap::kViridis, PJ::Colormap::kPlasma, PJ::Colormap::kGrayscale}) {
    colormap->addItem(colormapToken(value), static_cast<int>(value));
  }
  colormap->setCurrentIndex(static_cast<int>(colormap_));
  form->addRow(tr("Colormap:"), colormap);

  auto* invert = new QCheckBox(tr("Invert"), container);
  invert->setObjectName(u"wasmPointCloudInvert"_s);
  invert->setChecked(invert_lut_);
  form->addRow(QString{}, invert);

  auto* automatic = new PJ::CheckButton(tr("auto"), container);
  automatic->setObjectName(u"wasmPointCloudAutoRange"_s);
  automatic->setChecked(auto_range_);
  form->addRow(tr("Range:"), automatic);

  auto* range_min = new PJ::DoubleScrubber(container);
  auto* range_max = new PJ::DoubleScrubber(container);
  range_min->setObjectName(u"wasmPointCloudRangeMin"_s);
  range_max->setObjectName(u"wasmPointCloudRangeMax"_s);
  for (PJ::DoubleScrubber* scrubber : {range_min, range_max}) {
    scrubber->setRange(-1.0e9, 1.0e9);
    scrubber->setDecimals(4);
  }
  range_min->setValue(manual_range_min_);
  range_max->setValue(manual_range_max_);
  form->addRow(tr("Range Min:"), range_min);
  form->addRow(tr("Range Max:"), range_max);

  auto* outside = new PJ::DoubleScrubber(container);
  outside->setObjectName(u"wasmPointCloudOutsideOpacity"_s);
  outside->setRange(0.0, 1.0);
  outside->setDecimals(2);
  outside->setValue(outside_range_opacity_);
  form->addRow(tr("Outside opacity:"), outside);

  auto* outside_visible = new QCheckBox(tr("Show outside range"), container);
  outside_visible->setObjectName(u"wasmPointCloudOutsideVisible"_s);
  outside_visible->setChecked(outside_range_visible_);
  form->addRow(QString{}, outside_visible);

  // Hide whole rows (field + its label) via setRowVisible — a bare
  // setVisible on the field orphans the QFormLayout-owned label, which the
  // desktop layer avoids by hiding labelForField too.
  const auto refresh_visibility = [this, form, solid_color, colormap, invert, automatic, range_min, range_max, outside,
                                   outside_visible]() {
    const bool field = color_type_ == ColorType::kField;
    form->setRowVisible(solid_color, color_type_ == ColorType::kSolid);
    form->setRowVisible(colormap, field);
    form->setRowVisible(invert, field);
    form->setRowVisible(automatic, field);
    form->setRowVisible(range_min, field && !auto_range_);
    form->setRowVisible(range_max, field && !auto_range_);
    form->setRowVisible(outside, field && !auto_range_);
    form->setRowVisible(outside_visible, field && !auto_range_);
  };
  refresh_visibility();

  connect(shape, &QComboBox::currentIndexChanged, this, [this, shape, refresh_size](int) {
    setShape(static_cast<Shape>(shape->currentData().toInt()));
    refresh_size();
  });
  connect(size, &PJ::DoubleScrubber::valueChanged, this, [this](double value) {
    if (shape_ == Shape::kPoint) {
      setSizePixels(static_cast<float>(value));
    } else {
      setSizeMeters(static_cast<float>(value));
    }
  });
  connect(color_type, &QComboBox::currentIndexChanged, this, [this, color_type, refresh_visibility](int) {
    const QString value = color_type->currentData().toString();
    if (value == "solid"_L1) {
      setColorType(ColorType::kSolid);
    } else if (value == "rgb"_L1) {
      setColorType(ColorType::kRgb);
    } else if (value.startsWith(u"field:"_s)) {
      setColorType(ColorType::kField);
      setColorField(value.sliced(6));
    }
    refresh_visibility();
  });
  connect(solid_color, &PJ::ColorPickerWidget::colorChanged, this, &WasmPointCloudLayer::setSolidColor);
  connect(colormap, &QComboBox::currentIndexChanged, this, [this, colormap](int) {
    setColormap(static_cast<PJ::Colormap>(colormap->currentData().toInt()));
  });
  connect(invert, &QCheckBox::toggled, this, &WasmPointCloudLayer::setInvertLut);
  connect(automatic, &PJ::CheckButton::toggled, this, [this, refresh_visibility](bool checked) {
    setAutoRange(checked);
    refresh_visibility();
  });
  const auto push_range = [this, range_min, range_max]() {
    setManualRange(static_cast<float>(range_min->value()), static_cast<float>(range_max->value()));
  };
  connect(range_min, &PJ::DoubleScrubber::valueChanged, this, [range_max, push_range](double value) {
    if (value > range_max->value()) {
      QSignalBlocker blocker(range_max);
      range_max->setValue(value);
    }
    push_range();
  });
  connect(range_max, &PJ::DoubleScrubber::valueChanged, this, [range_min, push_range](double value) {
    if (value < range_min->value()) {
      QSignalBlocker blocker(range_min);
      range_min->setValue(value);
    }
    push_range();
  });
  connect(outside, &PJ::DoubleScrubber::valueChanged, this, [this](double value) {
    setOutsideRangeOpacity(static_cast<float>(value));
  });
  connect(outside_visible, &QCheckBox::toggled, this, &WasmPointCloudLayer::setOutsideRangeVisible);
  connect(
      this, &WasmPointCloudLayer::colorFieldsChanged, container, [rebuild_color_types]() { rebuild_color_types(); });
  connect(
      this, &WasmPointCloudLayer::autoRangeComputed, container, [range_min, range_max](float minimum, float maximum) {
        if (!range_min->isVisible()) {
          QSignalBlocker min_blocker(range_min);
          QSignalBlocker max_blocker(range_max);
          range_min->setValue(minimum);
          range_max->setValue(maximum);
        }
      });
  return container;
}

QDomElement WasmPointCloudLayer::xmlSaveState(QDomDocument& document) const {
  QDomElement element = document.createElement(u"pointcloud"_s);
  element.setAttribute(u"shape"_s, shapeName(shape_));
  element.setAttribute(u"size_meters"_s, QString::number(size_meters_, 'g', 6));
  element.setAttribute(u"size_pixels"_s, QString::number(size_pixels_, 'g', 6));
  const QString color_type = !color_choice_explicit_            ? u"auto"_s
                             : color_type_ == ColorType::kSolid ? u"solid"_s
                             : color_type_ == ColorType::kRgb   ? u"rgb"_s
                                                                : u"field"_s;
  element.setAttribute(u"color_type"_s, color_type);
  element.setAttribute(u"color_choice_explicit"_s, color_choice_explicit_ ? u"true"_s : u"false"_s);
  element.setAttribute(u"color_field"_s, QString::fromStdString(color_field_));
  element.setAttribute(u"solid_color"_s, solid_color_.name(QColor::HexRgb));
  element.setAttribute(u"colormap"_s, colormapToken(colormap_));
  element.setAttribute(u"auto_range"_s, auto_range_ ? u"true"_s : u"false"_s);
  element.setAttribute(u"invert_lut"_s, invert_lut_ ? u"true"_s : u"false"_s);
  element.setAttribute(u"outside_range_opacity"_s, QString::number(outside_range_opacity_, 'g', 6));
  element.setAttribute(u"outside_range_visible"_s, outside_range_visible_ ? u"true"_s : u"false"_s);
  element.setAttribute(u"range_min"_s, QString::number(manual_range_min_, 'g', 6));
  element.setAttribute(u"range_max"_s, QString::number(manual_range_max_, 'g', 6));
  return element;
}

std::optional<WasmPointCloudLayer::ParsedSettings> WasmPointCloudLayer::parseSettings(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "pointcloud"_L1 || !detail::isLeafPayload(element)) {
    return std::nullopt;
  }
  ParsedSettings settings;
  const QString shape = element.attribute(u"shape"_s, u"sphere"_s);
  if (shape == "sphere"_L1) {
    settings.shape = Shape::kSphere;
  } else if (shape == "point"_L1) {
    settings.shape = Shape::kPoint;
  } else if (shape == "cube"_L1) {
    settings.shape = Shape::kCube;
  } else {
    return std::nullopt;
  }
  if (!detail::parseFiniteFloat(element, "size_meters", 0.01F, 0.001F, 10.0F, settings.size_meters) ||
      !detail::parseFiniteFloat(element, "size_pixels", 2.0F, 1.0F, 32.0F, settings.size_pixels) ||
      !detail::parseFiniteFloat(element, "outside_range_opacity", 1.0F, 0.0F, 1.0F, settings.outside_range_opacity) ||
      !detail::parseFiniteFloat(
          element, "range_min", 0.0F, std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max(),
          settings.range_min) ||
      !detail::parseFiniteFloat(
          element, "range_max", 1.0F, std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max(),
          settings.range_max) ||
      settings.range_min > settings.range_max) {
    return std::nullopt;
  }

  const QString color_type = element.attribute(u"color_type"_s, u"field"_s);
  if (color_type == "auto"_L1) {
    settings.color_type = ColorType::kField;
    settings.color_choice_explicit = false;
  } else if (color_type == "solid"_L1) {
    settings.color_type = ColorType::kSolid;
    settings.color_choice_explicit = true;
  } else if (color_type == "rgb"_L1) {
    settings.color_type = ColorType::kRgb;
    settings.color_choice_explicit = true;
  } else if (color_type == "field"_L1) {
    settings.color_type = ColorType::kField;
    settings.color_choice_explicit = true;
  } else {
    return std::nullopt;
  }
  bool explicit_choice = settings.color_choice_explicit;
  if (!detail::parseTrueFalse(element, "color_choice_explicit", settings.color_choice_explicit, explicit_choice) ||
      explicit_choice != settings.color_choice_explicit) {
    return std::nullopt;
  }
  settings.color_field = element.attribute(u"color_field"_s).toStdString();
  if (element.hasAttribute(u"solid_color"_s)) {
    settings.solid_color = QColor(element.attribute(u"solid_color"_s));
    if (!settings.solid_color.isValid()) {
      return std::nullopt;
    }
  }
  const auto colormap = pj::scene3d::parseColormap(element.attribute(u"colormap"_s, u"turbo"_s));
  if (!colormap.has_value()) {
    return std::nullopt;
  }
  settings.colormap = *colormap;
  if (!detail::parseTrueFalse(element, "auto_range", true, settings.auto_range) ||
      !detail::parseTrueFalse(element, "invert_lut", false, settings.invert_lut) ||
      !detail::parseTrueFalse(element, "outside_range_visible", true, settings.outside_range_visible)) {
    return std::nullopt;
  }
  return settings;
}

bool WasmPointCloudLayer::validateXml(const QDomElement& element) {
  return parseSettings(element).has_value();
}

bool WasmPointCloudLayer::xmlLoadState(const QDomElement& element) {
  const auto settings = parseSettings(element);
  if (!settings.has_value()) {
    return false;
  }
  shape_ = settings->shape;
  size_meters_ = settings->size_meters;
  size_pixels_ = settings->size_pixels;
  color_type_ = settings->color_type;
  color_choice_explicit_ = settings->color_choice_explicit;
  color_field_ = settings->color_field;
  solid_color_ = settings->solid_color;
  colormap_ = settings->colormap;
  auto_range_ = settings->auto_range;
  invert_lut_ = settings->invert_lut;
  outside_range_opacity_ = settings->outside_range_opacity;
  outside_range_visible_ = settings->outside_range_visible;
  manual_range_min_ = settings->range_min;
  manual_range_max_ = settings->range_max;
  if (!color_choice_explicit_ && has_color_) {
    color_type_ = ColorType::kRgb;
  } else if (color_type_ == ColorType::kRgb && !has_color_) {
    color_type_ = ColorType::kField;
  }
  emit configurationChanged();
  requestDecode();
  return true;
}

void WasmPointCloudLayer::onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id) {
  if (session_ == nullptr || session_->objectStore().descriptor(topic_id_).dataset_id != dataset_id) {
    return;
  }
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
  // The topic id and (timestamp,size) sample key survive replaceDataset, so
  // every async result/cache from the old generation must become ineligible.
  invalidateCompressedState();
#endif
  active_sample_ = {};
  budget_rejected_vertex_count_.reset();
  decode_dirty_ = requested_time_.has_value();
  clearGeometry();
  setWarning({});
  emit repaintRequested();
}

}  // namespace pj::scene3d
