// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/wasm/depth_cloud_layer_wasm.h"

#include <QBuffer>
#include <QComboBox>
#include <QFormLayout>
#include <QImage>
#include <QImageReader>
#include <QLoggingCategory>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrentRun>
#include <algorithm>
#include <any>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <utility>

#include "layers/layer_xml_validation.h"
#include "pj_base/builtin/camera_info.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/object_topic_metadata.h"
#include "pj_scene3d_widgets/resolve_object.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"

using namespace Qt::StringLiterals;

namespace pj::scene3d {
namespace {

Q_LOGGING_CATEGORY(lcWasmDepthCloud, "pj.scene3d.wasm.depthcloud")

bool isPng(const PJ::sdk::Image& image) {
  const auto bytes = image.data;
  return bytes.size() >= 8U && bytes[0] == 0x89U && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G' &&
         bytes[4] == 0x0DU && bytes[5] == 0x0AU && bytes[6] == 0x1AU && bytes[7] == 0x0AU;
}

bool isJpeg(const PJ::sdk::Image& image) {
  const auto bytes = image.data;
  return bytes.size() >= 3U && bytes[0] == 0xFFU && bytes[1] == 0xD8U && bytes[2] == 0xFFU;
}

QImage readBoundedImage(const std::uint8_t* data, std::size_t size, QString& error) {
  if (data == nullptr || size == 0U || size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    error = QObject::tr("Depth image payload is invalid");
    return {};
  }
  QByteArray bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(data), static_cast<qsizetype>(size));
  QBuffer buffer(&bytes);
  if (!buffer.open(QIODevice::ReadOnly)) {
    error = QObject::tr("Depth image payload could not be opened");
    return {};
  }
  QImageReader reader(&buffer);
  const QSize dimensions = reader.size();
  std::uint64_t pixels = 0;
  if (!dimensions.isValid() ||
      !browserDepthDimensionsFit(
          static_cast<std::uint32_t>(dimensions.width()), static_cast<std::uint32_t>(dimensions.height()), pixels)) {
    error = QObject::tr("Decoded depth image exceeds the %1-pixel browser limit")
                .arg(QString::number(WasmDepthCloudLayer::kMaxPixelsPerImage));
    return {};
  }
  QImage decoded = reader.read();
  if (decoded.isNull()) {
    error = QObject::tr("Depth image decode failed: %1").arg(reader.errorString());
  }
  return decoded;
}

bool makeDepthView(
    const PJ::sdk::Image& image, std::vector<std::uint8_t>& scratch, PJ::sdk::DepthImage& view, QString& error) {
  if (image.encoding == "16UC1" || image.encoding == "32FC1") {
    std::uint64_t declared_pixels = 0;
    if (!browserDepthDimensionsFit(image.width, image.height, declared_pixels)) {
      error = QObject::tr("Depth image dimensions exceed the %1-pixel browser limit")
                  .arg(QString::number(WasmDepthCloudLayer::kMaxPixelsPerImage));
      return false;
    }
    view.width = image.width;
    view.height = image.height;
    view.encoding = image.encoding;
    if (isPng(image) || isJpeg(image)) {
      QImage wrapped = readBoundedImage(image.data.data(), image.data.size(), error);
      if (wrapped.isNull()) {
        return false;
      }
      std::size_t bytes_per_sample = 2U;
      if (wrapped.format() != QImage::Format_Grayscale16) {
        wrapped = wrapped.convertToFormat(QImage::Format_Grayscale8);
        bytes_per_sample = 1U;
      }
      const std::size_t row_bytes = static_cast<std::size_t>(wrapped.width()) * bytes_per_sample;
      scratch.resize(row_bytes * static_cast<std::size_t>(wrapped.height()));
      for (int row = 0; row < wrapped.height(); ++row) {
        std::memcpy(scratch.data() + static_cast<std::size_t>(row) * row_bytes, wrapped.constScanLine(row), row_bytes);
      }
      view.data = PJ::Span<const std::uint8_t>(scratch.data(), scratch.size());
      return true;
    }
    view.data = image.data;
    return true;
  }

  if (image.encoding != "compressedDepth") {
    error = QObject::tr("Not a depth image (encoding '%1')").arg(QString::fromStdString(image.encoding));
    return false;
  }

  // Some ROS bags contain the PNG stream starting at IHDR. Restore the standard
  // signature and IHDR length before the bounded QImageReader preflight.
  static constexpr std::uint8_t kPngPrefix[] = {0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU,
                                                0x1AU, 0x0AU, 0x00U, 0x00U, 0x00U, 0x0DU};
  const std::uint8_t* png_data = image.data.data();
  std::size_t png_size = image.data.size();
  std::vector<std::uint8_t> repaired;
  if (png_size >= 4U && png_data[0] == 'I' && png_data[1] == 'H' && png_data[2] == 'D' && png_data[3] == 'R') {
    repaired.reserve(sizeof(kPngPrefix) + png_size);
    repaired.insert(repaired.end(), std::begin(kPngPrefix), std::end(kPngPrefix));
    repaired.insert(repaired.end(), png_data, png_data + png_size);
    png_data = repaired.data();
    png_size = repaired.size();
  }
  QImage png = readBoundedImage(png_data, png_size, error);
  if (png.isNull()) {
    return false;
  }
  if (png.format() != QImage::Format_Grayscale16) {
    png = png.convertToFormat(QImage::Format_Grayscale16);
  }
  std::uint64_t decoded_pixels = 0;
  if (!browserDepthDimensionsFit(
          static_cast<std::uint32_t>(png.width()), static_cast<std::uint32_t>(png.height()), decoded_pixels)) {
    error = QObject::tr("Decoded depth image exceeds the %1-pixel browser limit")
                .arg(QString::number(WasmDepthCloudLayer::kMaxPixelsPerImage));
    return false;
  }
  const std::size_t row_bytes = static_cast<std::size_t>(png.width()) * 2U;
  scratch.resize(row_bytes * static_cast<std::size_t>(png.height()));
  for (int row = 0; row < png.height(); ++row) {
    std::memcpy(scratch.data() + static_cast<std::size_t>(row) * row_bytes, png.constScanLine(row), row_bytes);
  }
  view.width = static_cast<std::uint32_t>(png.width());
  view.height = static_cast<std::uint32_t>(png.height());
  view.encoding = "16UC1";
  view.data = PJ::Span<const std::uint8_t>(scratch.data(), scratch.size());
  return true;
}

}  // namespace

WasmDepthCloudLayer::WasmDepthCloudLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : PJ::ISceneLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)) {
  decode_watcher_ = new QFutureWatcher<DecodeResult>(this);
  connect(decode_watcher_, &QFutureWatcher<DecodeResult>::finished, this, &WasmDepthCloudLayer::onDecodeFinished);
}

WasmDepthCloudLayer::~WasmDepthCloudLayer() = default;

bool WasmDepthCloudLayer::isDepthEncoding(const std::string& encoding) {
  return encoding == "16UC1" || encoding == "32FC1" || encoding == "compressedDepth";
}

PJ::SceneLayerInfo WasmDepthCloudLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kImage,
      .display_name = display_name_,
      .family_name = u"DepthCloud"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> WasmDepthCloudLayer::timeRange() const {
  return PJ::liveTopicTimeRange(session_ != nullptr ? &session_->objectStore() : nullptr, topic_id_);
}

bool WasmDepthCloudLayer::attach(const PJ::SceneLayerContext& context) {
  if (context.session == nullptr || !context.session->parserBindingForObjectTopic(topic_id_)) {
    qCWarning(lcWasmDepthCloud) << "attach: session/parser is unavailable for topic" << topic_id_.id;
    return false;
  }
  session_ = context.session;
  disconnect(reload_connection_);
  disconnect(samples_connection_);
  reload_connection_ = connect(
      session_, &PJ::SessionManager::datasetAboutToBeReplaced, this, &WasmDepthCloudLayer::onDatasetAboutToBeReplaced);
  samples_connection_ = connect(session_, &PJ::SessionManager::samplesIngested, this, [this](const auto&, bool) {
    if (visible_ && requested_time_.has_value() && !warning_reason_.isEmpty()) {
      decode_dirty_ = true;
      emit repaintRequested();
    }
  });
  bootstrap();
  const PJ::Range<PJ::Timepoint> range = timeRange();
  if (range.max >= range.min) {
    requested_time_ = range.min;
    decode_dirty_ = true;
  }
  return true;
}

void WasmDepthCloudLayer::detach() {
  disconnect(reload_connection_);
  disconnect(samples_connection_);
  reload_connection_ = {};
  samples_connection_ = {};
  session_ = nullptr;
  requested_time_.reset();
  intrinsics_cache_.reset();
  budget_rejected_vertex_count_.reset();
  invalidateDecodeState();
  clearGeometry();
  updateSourceFrame({});
  setWarning({});
}

void WasmDepthCloudLayer::setTrackerTime(PJ::Timepoint time) {
  requested_time_ = time;
  decode_dirty_ = true;
  if (visible_) {
    emit repaintRequested();
  }
}

std::uint64_t WasmDepthCloudLayer::renderKey(PJ::Timepoint time) const {
  if (session_ == nullptr) {
    return PJ::kNoSampleRenderKey;
  }
  const auto index = session_->objectStore().indexAt(topic_id_, PJ::toRaw(time));
  if (!index.has_value()) {
    return PJ::kNoSampleRenderKey;
  }
  const auto timestamps = session_->objectStore().entryTimestamps(topic_id_);
  return *index < timestamps.size() ? static_cast<std::uint64_t>(timestamps[*index]) : PJ::kNoSampleRenderKey;
}

void WasmDepthCloudLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  if (!visible_) {
    ++decode_generation_;
    pending_.reset();
    wanted_sample_ = {};
    active_sample_ = {};
    budget_rejected_vertex_count_.reset();
    clearGeometry();
  } else {
    decode_dirty_ = requested_time_.has_value();
  }
  emit visibilityChanged(visible_);
  emit repaintRequested();
}

void WasmDepthCloudLayer::setFixedFrame(const QString& frame) {
  const std::string next = frame.toStdString();
  if (fixed_frame_ == next) {
    return;
  }
  fixed_frame_ = next;
  updateRenderFrame();
}

bool WasmDepthCloudLayer::prepareForRender(std::uint64_t remaining_view_vertices) {
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
    setWarning(tr("DepthCloud has %1 points; only %2 remain in the browser view budget")
                   .arg(QString::number(vertices_.size()), QString::number(remaining_view_vertices)));
    active_sample_ = {};
    clearGeometry();
    return true;
  }
  if (!visible_ || !decode_dirty_ || !requested_time_.has_value()) {
    return false;
  }
  decode_dirty_ = false;
  return decodeAt(*requested_time_);
}

bool WasmDepthCloudLayer::bootstrap() {
  if (session_ == nullptr) {
    return false;
  }
  const auto first = session_->objectStore().at(topic_id_, 0);
  if (!first.has_value() || first->payload.bytes.empty()) {
    return false;
  }
  auto object = resolveObject(
      session_->parserBindingForObjectTopic(topic_id_), PJ::sdk::BuiltinObjectType::kImage, first->timestamp,
      first->payload);
  const auto* image = object.has_value() ? std::any_cast<PJ::sdk::Image>(&object->object) : nullptr;
  if (image == nullptr || !isDepthEncoding(image->encoding)) {
    return false;
  }
  updateSourceFrame(image->frame_id);
  return true;
}

bool WasmDepthCloudLayer::decodeAt(PJ::Timepoint time) {
  if (session_ == nullptr) {
    return false;
  }
  const auto resolved = session_->objectStore().latestAt(topic_id_, PJ::toRaw(time));
  if (!resolved.has_value() || resolved->payload.bytes.empty()) {
    return false;
  }
  const SampleId sample{resolved->timestamp, resolved->payload.bytes.size()};
  wanted_sample_ = sample;
  if (sample == active_sample_) {
    return false;
  }
  auto object = resolveObject(
      session_->parserBindingForObjectTopic(topic_id_), PJ::sdk::BuiltinObjectType::kImage, resolved->timestamp,
      resolved->payload);
  if (!object.has_value()) {
    active_sample_ = sample;
    clearGeometry();
    setWarning(tr("Depth image could not be decoded: %1").arg(QString::fromStdString(object.error())));
    return true;
  }
  const auto* image = std::any_cast<PJ::sdk::Image>(&object->object);
  if (image == nullptr || !isDepthEncoding(image->encoding)) {
    active_sample_ = sample;
    clearGeometry();
    setWarning(tr("Not a depth image"));
    return true;
  }
  updateSourceFrame(image->frame_id);
  if (!browserPointPayloadFits(static_cast<std::uint64_t>(image->data.size()))) {
    active_sample_ = sample;
    clearGeometry();
    setWarning(tr("Depth image payload is %1 MiB; the browser limit is %2 MiB")
                   .arg(QString::number(static_cast<double>(image->data.size()) / (1024.0 * 1024.0), 'f', 1))
                   .arg(kMaxWireBytesPerImage / (1024ULL * 1024ULL)));
    return true;
  }
  if (image->encoding != "compressedDepth") {
    std::uint64_t pixels = 0;
    if (!browserDepthDimensionsFit(image->width, image->height, pixels)) {
      active_sample_ = sample;
      clearGeometry();
      setWarning(
          tr("Depth image dimensions exceed the %1-pixel browser limit").arg(QString::number(kMaxPixelsPerImage)));
      return true;
    }
  }
  const DepthIntrinsics intrinsics = resolveIntrinsics(image->frame_id, resolved->timestamp);
  if (!intrinsics.valid()) {
    clearGeometry();
    setWarning(tr("No CameraInfo intrinsics for frame '%1'").arg(QString::fromStdString(image->frame_id)));
    // Do not mark the sample active: a late/streamed CameraInfo may make this
    // exact depth sample drawable without changing its timestamp.
    return true;
  }
  queueDecode(
      DecodeRequest{
          .image = *image,
          .intrinsics = intrinsics,
          .sample = sample,
          .generation = decode_generation_,
          .min_depth_m = min_depth_m_,
          .max_depth_m = max_depth_m_,
      });
  return false;
}

DepthIntrinsics WasmDepthCloudLayer::resolveIntrinsics(const std::string& frame_id, std::int64_t time_ns) {
  if (session_ == nullptr) {
    return {};
  }
  PJ::ObjectStore& store = session_->objectStore();
  if (intrinsics_cache_.has_value() && intrinsics_cache_->frame_id == frame_id) {
    const auto resolved = store.latestAt(intrinsics_cache_->camera_topic, time_ns);
    if (resolved.has_value() &&
        SampleId{resolved->timestamp, resolved->payload.bytes.size()} == intrinsics_cache_->camera_sample) {
      return intrinsics_cache_->intrinsics;
    }
  }

  const PJ::DatasetId dataset = store.descriptor(topic_id_).dataset_id;
  std::optional<DepthIntrinsics> lone;
  PJ::ObjectTopicId lone_topic;
  SampleId lone_sample;
  int valid_count = 0;
  for (const PJ::ObjectTopicId candidate : store.listTopics()) {
    const PJ::ObjectTopicDescriptor descriptor = store.descriptor(candidate);
    if (descriptor.dataset_id != dataset ||
        builtinObjectTypeFor(descriptor) != PJ::sdk::BuiltinObjectType::kCameraInfo) {
      continue;
    }
    const auto resolved = store.latestAt(candidate, time_ns);
    if (!resolved.has_value()) {
      continue;
    }
    auto object = resolveObject(
        session_->parserBindingForObjectTopic(candidate), PJ::sdk::BuiltinObjectType::kCameraInfo, resolved->timestamp,
        resolved->payload);
    const auto* camera = object.has_value() ? std::any_cast<PJ::sdk::CameraInfo>(&object->object) : nullptr;
    if (camera == nullptr) {
      continue;
    }
    const DepthIntrinsics intrinsics = intrinsicsFromK(camera->K, camera->width, camera->height);
    if (!intrinsics.valid()) {
      continue;
    }
    const SampleId sample{resolved->timestamp, resolved->payload.bytes.size()};
    if (!frame_id.empty() && camera->frame_id == frame_id) {
      intrinsics_cache_ = IntrinsicsCache{frame_id, candidate, sample, intrinsics};
      return intrinsics;
    }
    ++valid_count;
    lone = intrinsics;
    lone_topic = candidate;
    lone_sample = sample;
  }
  if (valid_count == 1 && lone.has_value()) {
    intrinsics_cache_ = IntrinsicsCache{frame_id, lone_topic, lone_sample, *lone};
    return *lone;
  }
  intrinsics_cache_.reset();
  return {};
}

void WasmDepthCloudLayer::queueDecode(DecodeRequest request) {
  wanted_sample_ = request.sample;
  if (inflight_.has_value()) {
    if (inflight_->sample == request.sample && inflight_->generation == request.generation) {
      pending_.reset();
    } else {
      pending_ = std::move(request);
    }
    return;
  }
  startDecode(std::move(request));
}

void WasmDepthCloudLayer::startDecode(DecodeRequest request) {
  inflight_ = request;
  ++decode_starts_;
  QThread* const main_thread = QThread::currentThread();
  decode_watcher_->setFuture(QtConcurrent::run([request = std::move(request), main_thread]() mutable {
    return decode(std::move(request), main_thread);
  }));
}

WasmDepthCloudLayer::DecodeResult WasmDepthCloudLayer::decode(DecodeRequest request, QThread* main_thread) {
  DecodeResult result{
      .sample = request.sample,
      .generation = request.generation,
      .vertices = {},
      .bounds = {},
      .scalar_range = {0.0F, 1.0F},
      .error = {},
      .ran_off_main_thread = QThread::currentThread() != main_thread,
  };
  try {
    std::vector<std::uint8_t> scratch;
    PJ::sdk::DepthImage depth;
    if (!makeDepthView(request.image, scratch, depth, result.error)) {
      return result;
    }
    BackprojectOptions options;
    options.min_depth_m = request.min_depth_m;
    options.max_depth_m = request.max_depth_m;
    std::vector<float> scalar;
    PJ::Range<float> scalar_range{0.0F, 1.0F};
    std::vector<glm::vec3> points =
        depthToPoints(depth, request.intrinsics, options, &scalar, &result.bounds, &scalar_range);
    if (points.empty()) {
      result.error = QObject::tr("Depth image contains no valid pixels in the selected range");
      return result;
    }
    result.vertices.reserve(points.size());
    for (std::size_t index = 0; index < points.size(); ++index) {
      result.vertices.push_back(
          WasmPointVertex{
              .x = points[index].x,
              .y = points[index].y,
              .z = points[index].z,
              .scalar = scalar[index],
              .rgba = 0xFFFFFFFFU,
          });
    }
    result.scalar_range = {scalar_range.min, scalar_range.max};
  } catch (const std::bad_alloc&) {
    result.vertices.clear();
    result.bounds = {};
    result.error = QObject::tr("Not enough browser memory to build this DepthCloud");
  } catch (const std::exception& error) {
    result.vertices.clear();
    result.bounds = {};
    result.error = QObject::tr("DepthCloud conversion failed: %1").arg(QString::fromUtf8(error.what()));
  }
  return result;
}

void WasmDepthCloudLayer::onDecodeFinished() {
  DecodeResult result = decode_watcher_->result();
  ++decode_completions_;
  if (result.ran_off_main_thread) {
    ++decode_off_main_completions_;
  }
  inflight_.reset();
  if (visible_ && result.generation == decode_generation_ && result.sample == wanted_sample_) {
    applyResult(std::move(result));
  }
  if (pending_.has_value()) {
    DecodeRequest next = std::move(*pending_);
    pending_.reset();
    if (visible_ && next.generation == decode_generation_ && next.sample == wanted_sample_) {
      startDecode(std::move(next));
    }
  }
}

void WasmDepthCloudLayer::applyResult(DecodeResult result) {
  active_sample_ = result.sample;
  if (!result.error.isEmpty()) {
    clearGeometry();
    setWarning(std::move(result.error));
  } else {
    vertices_ = std::move(result.vertices);
    source_bounds_ = result.bounds;
    scalar_range_ = result.scalar_range;
    ++geometry_revision_;
    setWarning({});
  }
  emit repaintRequested();
}

void WasmDepthCloudLayer::invalidateDecodeState() {
  ++decode_generation_;
  pending_.reset();
  wanted_sample_ = {};
  active_sample_ = {};
  decode_dirty_ = requested_time_.has_value();
}

void WasmDepthCloudLayer::updateSourceFrame(const std::string& frame) {
  if (source_frame_ == frame) {
    return;
  }
  source_frame_ = frame;
  updateRenderFrame();
}

void WasmDepthCloudLayer::updateRenderFrame() {
  const std::string effective = source_frame_.empty() ? fixed_frame_ : source_frame_;
  if (render_frame_ == effective) {
    return;
  }
  render_frame_ = effective;
  emit sourceFrameChanged(QString::fromStdString(render_frame_));
}

void WasmDepthCloudLayer::setWarning(QString warning) {
  if (warning_reason_ == warning) {
    return;
  }
  warning_reason_ = std::move(warning);
  emit warningChanged(!warning_reason_.isEmpty(), warning_reason_);
}

void WasmDepthCloudLayer::clearGeometry() {
  if (vertices_.empty() && !source_bounds_.valid) {
    return;
  }
  vertices_.clear();
  vertices_.shrink_to_fit();
  source_bounds_ = {};
  scalar_range_ = {0.0F, 1.0F};
  ++geometry_revision_;
}

void WasmDepthCloudLayer::requestDecode() {
  invalidateDecodeState();
  if (visible_ && requested_time_.has_value()) {
    emit repaintRequested();
  }
}

void WasmDepthCloudLayer::setColormap(PJ::Colormap colormap) {
  if (colormap_ == colormap) {
    return;
  }
  colormap_ = colormap;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmDepthCloudLayer::setPointSizePixels(float pixels) {
  if (point_size_px_ == pixels) {
    return;
  }
  point_size_px_ = pixels;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmDepthCloudLayer::setMinDepth(float metres) {
  if (min_depth_m_ == metres) {
    return;
  }
  min_depth_m_ = metres;
  emit configurationChanged();
  requestDecode();
}

void WasmDepthCloudLayer::setMaxDepth(float metres) {
  if (max_depth_m_ == metres) {
    return;
  }
  max_depth_m_ = metres;
  emit configurationChanged();
  requestDecode();
}

std::optional<WasmDepthCloudLayer::ParsedSettings> WasmDepthCloudLayer::parseSettings(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "depthcloud"_L1 || !detail::isLeafPayload(element)) {
    return std::nullopt;
  }
  ParsedSettings settings;
  const auto colormap = parseColormap(element.attribute(u"colormap"_s, u"turbo"_s));
  if (!colormap.has_value() ||
      !detail::parseFiniteFloat(element, "point_size_px", 2.0F, 1.0F, 32.0F, settings.point_size_px) ||
      !detail::parseFiniteFloat(element, "min_depth", 0.0F, 0.0F, 1000.0F, settings.min_depth_m) ||
      !detail::parseFiniteFloat(element, "max_depth", 0.0F, 0.0F, 1000.0F, settings.max_depth_m) ||
      (settings.max_depth_m > 0.0F && settings.min_depth_m > settings.max_depth_m)) {
    return std::nullopt;
  }
  settings.colormap = *colormap;
  return settings;
}

bool WasmDepthCloudLayer::validateXml(const QDomElement& element) {
  return parseSettings(element).has_value();
}

QDomElement WasmDepthCloudLayer::xmlSaveState(QDomDocument& document) const {
  QDomElement element = document.createElement(u"depthcloud"_s);
  element.setAttribute(u"colormap"_s, colormapToken(colormap_));
  element.setAttribute(u"point_size_px"_s, QString::number(static_cast<double>(point_size_px_), 'g', 6));
  element.setAttribute(u"min_depth"_s, QString::number(static_cast<double>(min_depth_m_), 'g', 6));
  element.setAttribute(u"max_depth"_s, QString::number(static_cast<double>(max_depth_m_), 'g', 6));
  return element;
}

bool WasmDepthCloudLayer::xmlLoadState(const QDomElement& element) {
  const auto settings = parseSettings(element);
  if (!settings.has_value()) {
    return false;
  }
  setColormap(settings->colormap);
  setPointSizePixels(settings->point_size_px);
  setMinDepth(settings->min_depth_m);
  setMaxDepth(settings->max_depth_m);
  return true;
}

QWidget* WasmDepthCloudLayer::createConfigWidget(QWidget* parent) {
  auto* container = new QWidget(parent);
  auto* outer = new QVBoxLayout(container);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(PJ::theme::space(PJ::theme::Space::Snug));
  auto* form = new QFormLayout();
  form->setContentsMargins(0, 0, 0, 0);
  form->setHorizontalSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  form->setVerticalSpacing(PJ::theme::space(PJ::theme::Space::Snug));
  outer->addLayout(form);

  auto* colormap = new PJ::ComboBox(container);
  colormap->setObjectName(u"wasmDepthCloudColormap"_s);
  for (const PJ::Colormap value :
       {PJ::Colormap::kTurbo, PJ::Colormap::kViridis, PJ::Colormap::kPlasma, PJ::Colormap::kGrayscale}) {
    colormap->addItem(colormapToken(value), static_cast<int>(value));
  }
  colormap->setCurrentIndex(static_cast<int>(colormap_));
  form->addRow(tr("Colormap:"), colormap);

  auto* size = new PJ::DoubleScrubber(container);
  size->setObjectName(u"wasmDepthCloudPointSize"_s);
  size->setSuffix(u" px"_s);
  size->setDecimals(1);
  size->setSingleStep(0.5);
  size->setRange(1.0, 32.0);
  size->setValue(point_size_px_);
  form->addRow(tr("Point size:"), size);

  auto* minimum = new PJ::DoubleScrubber(container);
  minimum->setObjectName(u"wasmDepthCloudMinDepth"_s);
  minimum->setSuffix(u" m"_s);
  minimum->setDecimals(2);
  minimum->setSingleStep(0.1);
  minimum->setRange(0.0, 1000.0);
  minimum->setValue(min_depth_m_);
  form->addRow(tr("Min depth:"), minimum);

  auto* maximum = new PJ::DoubleScrubber(container);
  maximum->setObjectName(u"wasmDepthCloudMaxDepth"_s);
  maximum->setSuffix(u" m"_s);
  maximum->setDecimals(2);
  maximum->setSingleStep(0.1);
  maximum->setRange(0.0, 1000.0);
  maximum->setValue(max_depth_m_);
  form->addRow(tr("Max depth:"), maximum);

  connect(colormap, &QComboBox::currentIndexChanged, this, [this, colormap](int) {
    setColormap(static_cast<PJ::Colormap>(colormap->currentData().toInt()));
  });
  connect(size, &PJ::DoubleScrubber::valueChanged, this, [this](double value) {
    setPointSizePixels(static_cast<float>(value));
  });
  connect(minimum, &PJ::DoubleScrubber::valueChanged, this, [this](double value) {
    setMinDepth(static_cast<float>(value));
  });
  connect(maximum, &PJ::DoubleScrubber::valueChanged, this, [this](double value) {
    setMaxDepth(static_cast<float>(value));
  });
  return container;
}

void WasmDepthCloudLayer::onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id) {
  if (session_ == nullptr || session_->objectStore().descriptor(topic_id_).dataset_id != dataset_id) {
    return;
  }
  intrinsics_cache_.reset();
  requested_time_.reset();
  budget_rejected_vertex_count_.reset();
  invalidateDecodeState();
  clearGeometry();
  updateSourceFrame({});
  setWarning({});
}

}  // namespace pj::scene3d
