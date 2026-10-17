// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/layers/depth_cloud_layer.h"

#include <QBuffer>
#include <QComboBox>
#include <QFormLayout>
#include <QImage>
#include <QImageReader>
#include <QLoggingCategory>
#include <QVBoxLayout>
#include <QWidget>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include "layer_xml_validation.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/camera_info.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_base/time.hpp"  // PJ::fromRaw, PJ::toRaw
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/parse_locked.h"
#include "pj_scene3d_core/camera/camera.h"  // AABB, expandAABB
#include "pj_scene3d_core/pointcloud.h"     // PointVertex
#include "pj_scene3d_core/pointcloud_budget.h"
#include "pj_scene3d_core/tf/object_placement.h"
#include "pj_scene3d_widgets/object_topic_metadata.h"
#include "pj_scene3d_widgets/retained_upload.h"
#include "pj_widgets/Colormap.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcDepthCloudLayer, "pj.scene3d.layer.depthcloud")

using PJ::sdk::BuiltinObjectType;
using PJ::sdk::CameraInfo;
using PJ::sdk::Image;

// Reads only the header of an encoded image and checks its declared size
// against kSceneLimits, so an over-limit container is refused before inflating.
bool encodedImageFits(const uchar* data, std::size_t size, QString* error) {
  if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  QByteArray bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(data), static_cast<qsizetype>(size));
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::ReadOnly);
  const QSize dimensions = QImageReader(&buffer).size();
  std::uint64_t pixels = 0;
  if (dimensions.isValid() &&
      depthDimensionsFit(
          static_cast<std::uint32_t>(dimensions.width()), static_cast<std::uint32_t>(dimensions.height()), pixels)) {
    return true;
  }
  if (error != nullptr) {
    *error = DepthCloudLayer::tr("Decoded depth image exceeds the %1-pixel limit")
                 .arg(QString::number(kSceneLimits.max_points_per_cloud));
  }
  return false;
}

}  // namespace

bool isDepthEncoding(const std::string& encoding) {
  // Metric depth carriers only. mono16 is excluded on purpose: it is commonly a
  // grayscale camera, and offering every mono16 topic as a DepthCloud would be wrong.
  return encoding == "16UC1" || encoding == "32FC1" || encoding == "compressedDepth";
}

DepthCloudLayer::DepthCloudLayer(
    PJ::ObjectTopicId topic_id, QString display_name, BuiltinObjectType object_type, QObject* parent)
    : Scene3DLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)), object_type_(object_type) {}

DepthCloudLayer::~DepthCloudLayer() = default;

PJ::SceneLayerInfo DepthCloudLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = object_type_,
      .display_name = display_name_,
      .family_name = u"DepthCloud"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> DepthCloudLayer::timeRange() const {
  return PJ::liveTopicTimeRange(ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr, topic_id_);
}

QStringList DepthCloudLayer::fallbackFrames() const {
  QStringList out;
  if (!source_frame_.empty()) {
    out.append(QString::fromStdString(source_frame_));
  }
  return out;
}

QString DepthCloudLayer::sourceFrame() const {
  return QString::fromStdString(source_frame_);
}

bool DepthCloudLayer::attach(const PJ::SceneLayerContext& ctx) {
  const auto& scene3d_ctx = static_cast<const Scene3DLayerContext&>(ctx);
  if (scene3d_ctx.session == nullptr) {
    qCWarning(lcDepthCloudLayer) << "attach: session is null";
    return false;
  }
  ctx_ = scene3d_ctx;
  if (!ctx_.session->parserBindingForObjectTopic(topic_id_)) {
    qCWarning(lcDepthCloudLayer) << "attach: no parser for topic_id=" << topic_id_.id;
    return false;
  }
  // disconnect first: the reload path re-attaches without an intervening detach.
  disconnect(reload_connection_);
  reload_connection_ = connect(
      ctx_.session, &PJ::SessionManager::datasetAboutToBeReplaced, this, &DepthCloudLayer::onDatasetAboutToBeReplaced);
  PJ::ObjectStore& store = ctx_.session->objectStore();
  ts_first_.reset();
  if (store.entryCount(topic_id_) > 0) {
    ts_first_ = store.timeRange(topic_id_).first;
  }
  if (ts_first_.has_value()) {
    renderAt(*ts_first_);
  }
  return true;
}

void DepthCloudLayer::detach() {
  disconnect(reload_connection_);
  reload_connection_ = {};
  ts_first_.reset();
  decoded_at_ns_.reset();
  world_bounds_.reset();
  intrinsics_cache_.reset();
  ctx_ = {};
  invalidateSample();
  budget_.reset();
  clearGeometry();
  setDataWarning({});
}

void DepthCloudLayer::setFixedFrame(const QString& frame) {
  if (fixed_frame_ == frame) {
    return;
  }
  fixed_frame_ = frame;
  updateRenderFrame();  // the QRhi view places the retained points; no re-projection
  emit repaintRequested();
}

void DepthCloudLayer::setTrackerTime(PJ::Timepoint time) {
  decoded_at_ns_ = time;
  tracker_dirty_ = true;
  if (visible_) {
    emit repaintRequested();
  }
}

void DepthCloudLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  if (!visible_) {
    // A hidden layer releases its retained points; re-showing projects once.
    // Its bounds stay: a hidden layer still counts in the camera's scene bounds.
    const std::optional<AABB> bounds = world_bounds_;
    invalidateSample();
    budget_.reset();
    clearGeometry();
    world_bounds_ = bounds;
  }
  emit visibilityChanged(visible);
  emit repaintRequested();
}

void DepthCloudLayer::setColormap(PJ::Colormap cm) {
  if (colormap_ == cm) {
    return;
  }
  colormap_ = cm;
  emit configurationChanged();
  emit repaintRequested();
}

void DepthCloudLayer::setPointSizePixels(float pixels) {
  if (point_size_px_ == pixels) {
    return;
  }
  point_size_px_ = pixels;
  emit configurationChanged();
  emit repaintRequested();
}

void DepthCloudLayer::setMinDepth(float metres) {
  if (min_depth_m_ == metres) {
    return;
  }
  min_depth_m_ = metres;
  invalidateSample();  // a running projection used the old range
  emit configurationChanged();
  refreshNow();
}

void DepthCloudLayer::setMaxDepth(float metres) {
  if (max_depth_m_ == metres) {
    return;
  }
  max_depth_m_ = metres;
  invalidateSample();  // a running projection used the old range
  emit configurationChanged();
  refreshNow();
}

QDomElement DepthCloudLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(u"depthcloud"_s);
  el.setAttribute(u"colormap"_s, PJ::colormapToken(colormap_));
  el.setAttribute(u"point_size_px"_s, QString::number(static_cast<double>(point_size_px_), 'g', 6));
  el.setAttribute(u"min_depth"_s, QString::number(static_cast<double>(min_depth_m_), 'g', 6));
  el.setAttribute(u"max_depth"_s, QString::number(static_cast<double>(max_depth_m_), 'g', 6));
  return el;
}

std::optional<DepthCloudLayer::Settings> DepthCloudLayer::parseSettings(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "depthcloud"_L1 || !detail::isLeafPayload(element)) {
    return std::nullopt;
  }
  Settings settings;
  const auto colormap = PJ::parseColormap(element.attribute(u"colormap"_s, PJ::colormapToken(PJ::Colormap::kTurbo)));
  if (!colormap.has_value() ||
      !detail::parseFiniteFloat(element, "point_size_px", 2.0f, 1.0f, 32.0f, settings.point_size_px) ||
      !detail::parseFiniteFloat(element, "min_depth", 0.0f, 0.0f, 1000.0f, settings.min_depth_m) ||
      !detail::parseFiniteFloat(element, "max_depth", 0.0f, 0.0f, 1000.0f, settings.max_depth_m) ||
      (settings.max_depth_m > 0.0f && settings.min_depth_m > settings.max_depth_m)) {
    return std::nullopt;
  }
  settings.colormap = *colormap;
  return settings;
}

bool DepthCloudLayer::validateXml(const QDomElement& element) {
  return parseSettings(element).has_value();
}

bool DepthCloudLayer::xmlLoadState(const QDomElement& element) {
  const std::optional<Settings> settings = parseSettings(element);
  if (!settings.has_value()) {
    return false;
  }
  setColormap(settings->colormap);
  setPointSizePixels(settings->point_size_px);
  setMinDepth(settings->min_depth_m);
  setMaxDepth(settings->max_depth_m);
  return true;
}

QWidget* DepthCloudLayer::createConfigWidget(QWidget* parent) {
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
  // Match the Grid / Transforms section grids in Scene3DConfigPanel: comfortable
  // label↔field gap, snug row pitch — so the panel keeps one consistent rhythm.
  form->setHorizontalSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  form->setVerticalSpacing(PJ::theme::space(PJ::theme::Space::Snug));
  outer->addLayout(form);

  auto* colormap_combo = new PJ::ComboBox(container);
  for (int index = 0; index < PJ::kColormapCount; ++index) {
    colormap_combo->addItem(PJ::colormapToken(static_cast<PJ::Colormap>(index)), index);
  }
  colormap_combo->setCurrentIndex(static_cast<int>(colormap_));
  form->addRow(tr("Colormap:"), colormap_combo);

  auto* size_spin = new PJ::DoubleScrubber(container);
  size_spin->setSuffix(u" px"_s);
  size_spin->setDecimals(1);
  size_spin->setSingleStep(0.5);
  size_spin->setRange(1.0, 32.0);
  size_spin->setValue(static_cast<double>(point_size_px_));
  form->addRow(tr("Point size:"), size_spin);

  auto* min_spin = new PJ::DoubleScrubber(container);
  min_spin->setSuffix(u" m"_s);
  min_spin->setDecimals(2);
  min_spin->setSingleStep(0.1);
  min_spin->setRange(0.0, 1000.0);
  min_spin->setValue(static_cast<double>(min_depth_m_));
  form->addRow(tr("Min depth:"), min_spin);

  auto* max_spin = new PJ::DoubleScrubber(container);
  max_spin->setSuffix(u" m"_s);
  max_spin->setDecimals(2);
  max_spin->setSingleStep(0.1);
  max_spin->setRange(0.0, 1000.0);  // 0 = no far clip
  max_spin->setValue(static_cast<double>(max_depth_m_));
  form->addRow(tr("Max depth:"), max_spin);

  QObject::connect(
      colormap_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
      [this, colormap_combo](int) { setColormap(static_cast<PJ::Colormap>(colormap_combo->currentData().toInt())); });
  QObject::connect(size_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) {
    setPointSizePixels(static_cast<float>(v));
  });
  QObject::connect(
      min_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) { setMinDepth(static_cast<float>(v)); });
  QObject::connect(
      max_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) { setMaxDepth(static_cast<float>(v)); });

  return container;
}

std::optional<PJ::sdk::DepthImage> DepthCloudLayer::toDepthView(
    const Image& image, std::vector<uint8_t>& scratch, QString* error) {
  PJ::sdk::DepthImage view;
  if (image.encoding == "16UC1" || image.encoding == "32FC1") {
    view.width = image.width;
    view.height = image.height;
    view.encoding = image.encoding;
    // Some transports (Mosaico, serialization_format=image) losslessly PNG/JPEG-wrap
    // the raw depth buffer as an 8-bit grayscale image of width=stride. Recover the
    // flat bytes via QImage before aliasing — otherwise the compressed container is
    // read as raw depth and the cloud is empty/garbage (mirrors the 2D depth path's
    // recoverContainerRawSamples). A non-container payload aliases the raw bytes.
    const uchar* d = image.data.data();
    const size_t n = image.data.size();
    const bool is_png = n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G' && d[4] == 0x0D &&
                        d[5] == 0x0A && d[6] == 0x1A && d[7] == 0x0A;
    const bool is_jpeg = n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF;
    if ((is_png || is_jpeg) && !encodedImageFits(d, n, error)) {
      return std::nullopt;
    }
    QImage wrapped;
    if ((is_png || is_jpeg) && wrapped.loadFromData(d, static_cast<int>(n)) && wrapped.width() > 0 &&
        wrapped.height() > 0) {
      const int h = wrapped.height();
      const size_t wz = static_cast<size_t>(wrapped.width());
      // Mono16 already IS the flat 2-byte-per-sample buffer; anything else is
      // collapsed to one luma byte per sample (input is gray-expanded, so luma
      // == the original byte). Pack scanline-by-scanline (QImage rows are padded).
      size_t bytes_per_sample = 2U;
      if (wrapped.format() != QImage::Format_Grayscale16) {
        wrapped = wrapped.convertToFormat(QImage::Format_Grayscale8);
        bytes_per_sample = 1U;
      }
      const size_t row_bytes = wz * bytes_per_sample;
      scratch.resize(row_bytes * static_cast<size_t>(h));
      for (int y = 0; y < h; ++y) {
        std::memcpy(scratch.data() + static_cast<size_t>(y) * row_bytes, wrapped.constScanLine(y), row_bytes);
      }
      view.data = PJ::Span<const uint8_t>(scratch.data(), scratch.size());
      return view;
    }
    view.data = image.data;  // zero-copy view; the image's anchor keeps it alive
    return view;
  }
  if (image.encoding == "compressedDepth") {
    // PNG-decode (the 12-byte ROS header was already stripped by the parser, which
    // also set compressed_depth_min/max). RealSense depth is 16UC1 (millimetres),
    // so we read 16-bit grayscale directly. 32FC1 inverse-quantized compressedDepth
    // is not yet handled (TODO: use compressed_depth_min/max + the quant params).
    //
    // Some streams (e.g. RealSense bags) carry a BARE PNG that begins at the IHDR
    // chunk type, missing the 8-byte signature + the IHDR length field; restore them
    // so QImage accepts it (mirrors the 2D depth path's toDepthImage). Without this,
    // QImage rejects the payload and the layer reports "Not a depth image".
    static constexpr uchar kPngPrefix[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D};
    const uchar* png_data = image.data.data();
    int png_size = static_cast<int>(image.data.size());
    std::vector<uchar> repaired;
    if (image.data.size() >= 4 && png_data[0] == 'I' && png_data[1] == 'H' && png_data[2] == 'D' &&
        png_data[3] == 'R') {
      repaired.reserve(sizeof(kPngPrefix) + image.data.size());
      repaired.insert(repaired.end(), kPngPrefix, kPngPrefix + sizeof(kPngPrefix));
      repaired.insert(repaired.end(), png_data, png_data + image.data.size());
      png_data = repaired.data();
      png_size = static_cast<int>(repaired.size());
    }
    if (!encodedImageFits(png_data, static_cast<std::size_t>(png_size), error)) {
      return std::nullopt;
    }
    QImage png;
    if (!png.loadFromData(png_data, png_size)) {
      qCWarning(lcDepthCloudLayer) << "compressedDepth: PNG decode failed";
      if (error != nullptr) {
        *error = tr("compressedDepth PNG decode failed");
      }
      return std::nullopt;
    }
    if (png.format() != QImage::Format_Grayscale16) {
      png = png.convertToFormat(QImage::Format_Grayscale16);
    }
    const int w = png.width();
    const int h = png.height();
    if (w <= 0 || h <= 0) {
      if (error != nullptr) {
        *error = tr("compressedDepth image is empty");
      }
      return std::nullopt;
    }
    scratch.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 2U);
    for (int y = 0; y < h; ++y) {
      std::memcpy(scratch.data() + static_cast<size_t>(y) * w * 2U, png.constScanLine(y), static_cast<size_t>(w) * 2U);
    }
    view.width = static_cast<uint32_t>(w);
    view.height = static_cast<uint32_t>(h);
    view.encoding = "16UC1";  // decoded 16-bit depth in millimetres
    view.data = PJ::Span<const uint8_t>(scratch.data(), scratch.size());
    return view;
  }
  if (error != nullptr) {
    *error = tr("Not a depth image (encoding '%1')").arg(QString::fromStdString(image.encoding));
  }
  return std::nullopt;  // non-depth encoding
}

bool DepthCloudLayer::intrinsicsCurrentAt(int64_t time_ns) const {
  if (!intrinsics_cache_.has_value() || ctx_.session == nullptr) {
    return true;  // nothing memoized -> nothing can be stale
  }
  const auto entry_id = ctx_.session->objectStore().latestEntryIdAt(intrinsics_cache_->camera_topic, time_ns);
  return entry_id.has_value() && *entry_id == intrinsics_cache_->camera_sample;
}

DepthIntrinsics DepthCloudLayer::resolveIntrinsics(const std::string& frame_id, int64_t time_ns) {
  if (ctx_.session == nullptr) {
    return {};
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();

  // Fast path: intrinsics for this frame_id are memoized and the CameraInfo that
  // supplied them is unchanged at the playhead (same sample identity, verified
  // metadata-only — no parse and no payload resolve). Calibration is latched/
  // constant, so this skips re-scanning every topic and re-parsing every
  // CameraInfo on each depth frame.
  if (intrinsics_cache_.has_value() && intrinsics_cache_->frame_id == frame_id && intrinsicsCurrentAt(time_ns)) {
    return intrinsics_cache_->intr;
  }

  const PJ::DatasetId dataset_id = store.descriptor(topic_id_).dataset_id;

  // frame_id is the authoritative join key between a depth image and its
  // CameraInfo (matching Foxglove's frame_id rule / Rerun's camera hierarchy) —
  // never the topic name. An exact frame_id match wins outright. Otherwise we may
  // fall back to a lone CameraInfo, but ONLY when the choice is unambiguous (a
  // single camera, or a frame-less image); with multiple cameras and no match we
  // refuse rather than risk pairing the wrong camera's intrinsics.
  std::optional<DepthIntrinsics> lone;  // the sole usable CameraInfo, if exactly one
  PJ::ObjectTopicId lone_topic;         // its topic + sample identity, to memoize the fallback
  SampleId lone_sample;
  int valid_count = 0;
  for (const PJ::ObjectTopicId id : store.listTopics()) {
    const PJ::ObjectTopicDescriptor desc = store.descriptor(id);
    if (desc.dataset_id != dataset_id || builtinObjectTypeFor(desc) != BuiltinObjectType::kCameraInfo) {
      continue;
    }
    // latestAt() yields the CameraInfo sample at-or-before the playhead, which
    // also covers the common latched/once-published calibration.
    auto resolved = store.latestAt(id, time_ns);
    if (!resolved.has_value()) {
      continue;
    }
    const auto binding = ctx_.session->parserBindingForObjectTopic(id);
    if (!binding) {
      continue;
    }
    auto obj = parseLocked(binding, resolved->timestamp, resolved->payload);
    if (!obj.has_value()) {
      continue;
    }
    const auto* ci = obj->object.get<CameraInfo>();
    if (ci == nullptr) {
      continue;
    }
    const DepthIntrinsics intr = intrinsicsFromK(ci->K, ci->width, ci->height);
    if (!intr.valid()) {
      continue;
    }
    const SampleId sample{resolved->sequential_uid, resolved->timestamp};
    if (!frame_id.empty() && ci->frame_id == frame_id) {
      intrinsics_cache_ = IntrinsicsCache{frame_id, id, sample, intr};  // exact match -> memoize
      return intr;
    }
    ++valid_count;
    lone = intr;  // remember in case it turns out to be the only one
    lone_topic = id;
    lone_sample = sample;
  }

  // No exact frame_id match. Use a lone CameraInfo only when unambiguous.
  if (valid_count == 1 && lone.has_value()) {
    if (!frame_id.empty()) {
      qCDebug(lcDepthCloudLayer) << "no CameraInfo matches frame" << QString::fromStdString(frame_id)
                                 << "; using the only CameraInfo present in the dataset";
    }
    intrinsics_cache_ = IntrinsicsCache{frame_id, lone_topic, lone_sample, *lone};  // lone fallback -> memoize
    return *lone;
  }
  if (valid_count > 1) {
    qCWarning(lcDepthCloudLayer) << "no CameraInfo matches frame" << QString::fromStdString(frame_id) << "and"
                                 << valid_count << "cameras are present — refusing to guess intrinsics";
  }
  intrinsics_cache_.reset();  // refuse / none available -> never serve a stale memo
  return {};                  // invalid -> renderAt() surfaces the user-facing warning
}

void DepthCloudLayer::renderAt(int64_t time_ns) {
  if (ctx_.session == nullptr) {
    return;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  // Sample-memo predicate shared by the metadata-first gate and the post-resolve
  // re-check: the depth sample under the calibration at the playhead is already
  // drawn or (QRhi) being projected. The calibration half keeps a CameraInfo-only
  // update (new K, unchanged depth image) from being swallowed by the depth memo.
  // A hit becomes what the playhead wants: a projection queued for another
  // sample is cancelled, and one running for another sample is dropped when it
  // lands, so a scrub back never ends on the sample it left.
  const auto already_current = [this, time_ns](const SampleId& id) {
    if (!intrinsicsCurrentAt(time_ns)) {
      return false;
    }
    const ProjectionKey key{id, memoCamera()};
    const BackprojectRequest* running = backproject_worker_.running();
    if (key != pushed_ && (running == nullptr || running->key != key)) {
      return false;
    }
    wanted_ = key;
    backproject_worker_.cancelWaiting();
    return true;
  };
  // Metadata-first gate: identify the entry latestAt() would resolve and skip
  // before touching payload bytes — resolving first would re-fetch an evicted
  // pool seed only to discard it when nothing changed.
  const auto entry_id = store.latestEntryIdAt(topic_id_, time_ns);
  if (!entry_id.has_value() || already_current(*entry_id)) {
    return;
  }
  auto resolved = store.latestAt(topic_id_, time_ns);
  if (!resolved.has_value() || resolved->payload.bytes.empty()) {
    return;
  }
  const SampleId id{resolved->sequential_uid, resolved->timestamp};
  if (already_current(id)) {
    return;  // a racing insert changed the pick back onto the already-current state
  }
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  if (!binding) {
    return;
  }
  auto obj = parseLocked(binding, resolved->timestamp, resolved->payload);
  if (!obj.has_value()) {
    qCWarning(lcDepthCloudLayer) << "renderAt parseObject failed:" << QString::fromStdString(obj.error());
    return;
  }
  const auto* image = obj->object.get<Image>();
  if (image == nullptr) {
    return;
  }
  updateSourceFrame(image->frame_id);
  const auto fail = [this](const QString& warning) {
    clearGeometry();
    setDataWarning(warning);
    emit repaintRequested();
  };
  // Cheap admission before any decode or allocation. A rejected depth sample is
  // memoized as pushed (it renders nothing until it changes); a non-depth image
  // is not, since a later encoding change must be picked up.
  if (const QString rejection = admissionError(*image); !rejection.isEmpty()) {
    invalidateSample();  // nothing queued earlier may paint over the warning
    if (isDepthEncoding(image->encoding)) {
      pushed_ = {id, memoCamera()};
    }
    fail(rejection);
    return;
  }
  const DepthIntrinsics intr = resolveIntrinsics(image->frame_id, time_ns);
  if (!intr.valid()) {
    // Not memoized: a late CameraInfo can make this very sample drawable.
    invalidateSample();
    fail(tr("No CameraInfo intrinsics for frame '%1'").arg(QString::fromStdString(image->frame_id)));
    return;
  }
  const ProjectionKey key{id, memoCamera()};
  // Latest wins: while one runs, only the newest request waits behind it.
  wanted_ = key;
  backproject_worker_.request(
      BackprojectRequest{
          .image = *image,
          .intrinsics = intr,
          .key = key,
          .min_depth_m = min_depth_m_,
          .max_depth_m = max_depth_m_,
          .stamp = objectStamp(image->timestamp_ns, resolved->payload_stamp_shift),
      });
}

void DepthCloudLayer::refreshNow() {
  if (decoded_at_ns_.has_value()) {
    renderAt(PJ::toRaw(*decoded_at_ns_));
  } else if (ts_first_.has_value()) {
    renderAt(*ts_first_);
  }
}

void DepthCloudLayer::updateSourceFrame(const std::string& frame_id) {
  if (frame_id == source_frame_) {
    return;
  }
  source_frame_ = frame_id;
  updateRenderFrame();
  emit sourceFrameChanged(QString::fromStdString(source_frame_));
  emit fallbackFramesChanged(fallbackFrames());
}

std::uint64_t DepthCloudLayer::budgetKey(std::uint64_t sample_uid) const {
  // The depth range decides which pixels become points.
  const std::uint64_t range =
      std::bit_cast<std::uint32_t>(min_depth_m_) | (std::uint64_t{std::bit_cast<std::uint32_t>(max_depth_m_)} << 32U);
  return (sample_uid * 0x9e3779b97f4a7c15ULL) ^ range;
}

std::uint64_t DepthCloudLayer::budgetCandidate() const {
  const PJ::ObjectStore* store = ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr;
  if (decoded_at_ns_.has_value()) {
    return budgetKey(PJ::latestEntryRenderKey(store, topic_id_, *decoded_at_ns_));
  }
  return budgetKey(
      ts_first_.has_value() ? PJ::latestEntryRenderKey(store, topic_id_, PJ::fromRaw(*ts_first_))
                            : PJ::kNoSampleRenderKey);
}

void DepthCloudLayer::drainPendingUpdate() {
  if (visible_ && tracker_dirty_) {
    tracker_dirty_ = false;
    refreshNow();
  }
}

bool DepthCloudLayer::prepareForRender(std::uint64_t remaining_view_vertices) {
  const std::uint64_t revision_before = geometry_revision_;
  bool released = false;
  if (budget_.blocked(remaining_view_vertices, [this] { return budgetCandidate(); }, released)) {
    return false;
  }
  if (released) {
    tracker_dirty_ = true;
  }
  // A projection queued here lands asynchronously and is checked on a later frame.
  drainPendingUpdate();
  if (static_cast<std::uint64_t>(pointCount()) > remaining_view_vertices) {
    budget_.reject(pointCount(), budgetKey(pushed_.sample.uid.value));
    setDataWarning(tr("DepthCloud has %1 points; only %2 remain in this view's budget")
                       .arg(QString::number(pointCount()), QString::number(remaining_view_vertices)));
    clearGeometry();
    pushed_ = {};  // re-project once the budget frees up
  }
  return geometry_revision_ != revision_before;
}

QString DepthCloudLayer::admissionError(const Image& image) {
  if (!isDepthEncoding(image.encoding)) {
    return tr("Not a depth image (encoding '%1')").arg(QString::fromStdString(image.encoding));
  }
  if (static_cast<std::uint64_t>(image.data.size()) > kSceneLimits.max_point_wire_bytes_per_cloud) {
    return tr("Depth image payload is %1 MiB; the limit is %2 MiB")
        .arg(QString::number(static_cast<double>(image.data.size()) / (1024.0 * 1024.0), 'f', 1))
        .arg(kSceneLimits.max_point_wire_bytes_per_cloud / (1024ULL * 1024ULL));
  }
  // compressedDepth declares its size in the PNG header, checked by toDepthView.
  if (std::uint64_t pixels = 0;
      image.encoding != "compressedDepth" && !depthDimensionsFit(image.width, image.height, pixels)) {
    return tr("Depth image dimensions exceed the %1-pixel limit")
        .arg(QString::number(kSceneLimits.max_points_per_cloud));
  }
  return {};
}

DepthCloudLayer::BackprojectResult DepthCloudLayer::backproject(const BackprojectRequest& request) {
  BackprojectResult result;
  result.key = request.key;
  result.stamp = request.stamp;
  // Keep every exception inside the worker: it must never reach the UI thread.
  try {
    std::vector<uint8_t> scratch;
    const auto depth = toDepthView(request.image, scratch, &result.error);
    if (!depth.has_value()) {
      return result;
    }
    BackprojectOptions opts;
    opts.min_depth_m = request.min_depth_m;
    opts.max_depth_m = request.max_depth_m;
    std::vector<float> scalar;
    PJ::Range<float> scalar_range{0.0f, 1.0f};
    const std::vector<glm::vec3> points =
        depthToPoints(*depth, request.intrinsics, opts, &scalar, &result.bounds, &scalar_range);
    if (points.empty()) {
      result.error = tr("Depth image contains no valid pixels in the selected range");
      return result;
    }
    const auto used = static_cast<qsizetype>(points.size() * sizeof(PointVertex));
    packIntoRetained(result.vertices, used, [&](std::span<std::byte> out, bool /*reused*/) {
      for (std::size_t index = 0; index < points.size(); ++index) {
        const PointVertex vertex{
            .x = points[index].x,
            .y = points[index].y,
            .z = points[index].z,
            .attribute = std::bit_cast<std::uint32_t>(scalar[index]),
        };
        std::memcpy(out.data() + index * sizeof(vertex), &vertex, sizeof(vertex));
      }
      return used;
    });
    result.point_count = points.size();
    result.scalar_range = {scalar_range.min, scalar_range.max};
  } catch (const std::bad_alloc&) {
    result.vertices = {};
    result.error = tr("Not enough memory to build this DepthCloud");
  } catch (const std::exception& error) {
    result.vertices = {};
    result.error = tr("DepthCloud conversion failed: %1").arg(QString::fromUtf8(error.what()));
  }
  return result;
}

void DepthCloudLayer::onBackprojectFinished(BackprojectResult result) {
  // A hide, a depth-range edit or a dataset swap invalidated older work; a
  // projection the tracker moved away from is obsolete.
  if (result.key != wanted_) {
    return;
  }
  pushed_ = result.key;
  if (!result.error.isEmpty()) {
    clearGeometry();
    setDataWarning(result.error);
  } else {
    vertex_bytes_ = std::move(result.vertices);
    point_count_ = result.point_count;
    world_bounds_ = result.bounds.valid ? std::optional<AABB>{result.bounds} : std::nullopt;
    scalar_range_ = result.scalar_range;
    render_stamp_ = result.stamp;
    ++geometry_revision_;
    setDataWarning({});
  }
  emit repaintRequested();
}

void DepthCloudLayer::invalidateSample() {
  backproject_worker_.invalidate();
  pushed_ = {};
  wanted_ = {};
}

void DepthCloudLayer::onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id) {
  if (ctx_.session == nullptr || ctx_.session->objectStore().descriptor(topic_id_).dataset_id != dataset_id) {
    return;
  }
  intrinsics_cache_.reset();
  invalidateSample();
  budget_.reset();
  clearGeometry();
  setDataWarning({});
  // The signal precedes the swap and replaceDataset runs no event loop, so the
  // repaint scheduled here drains against the replacement bytes.
  tracker_dirty_ = true;
  emit repaintRequested();
}

void DepthCloudLayer::clearGeometry() {
  if (vertex_bytes_.isEmpty() && !world_bounds_.has_value()) {
    return;
  }
  vertex_bytes_ = QByteArray();
  point_count_ = 0;
  world_bounds_.reset();
  scalar_range_ = {0.0f, 1.0f};
  ++geometry_revision_;
}

void DepthCloudLayer::updateRenderFrame() {
  render_frame_ = source_frame_.empty() ? fixed_frame_.toStdString() : source_frame_;
}

}  // namespace pj::scene3d
