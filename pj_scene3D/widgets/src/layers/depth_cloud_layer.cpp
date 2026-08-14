// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/layers/depth_cloud_layer.h"

#include <QComboBox>
#include <QFormLayout>
#include <QImage>
#include <QLoggingCategory>
#include <QVBoxLayout>
#include <QWidget>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "layer_xml_validation.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/camera_info.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_base/time.hpp"  // PJ::fromRaw, PJ::toRaw
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/camera/camera.h"  // AABB, expandAABB
#include "pj_scene3d_core/pointcloud.h"     // DecodedPointCloud
#include "pj_scene3d_widgets/object_topic_metadata.h"
#include "pj_scene3d_widgets/parse_locked.h"
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

}  // namespace

bool isDepthEncoding(const std::string& encoding) {
  // Metric depth carriers only. mono16 is excluded on purpose: it is commonly a
  // grayscale camera, and offering every mono16 topic as a DepthCloud would be wrong.
  return encoding == "16UC1" || encoding == "32FC1" || encoding == "compressedDepth";
}

DepthCloudLayer::DepthCloudLayer(
    PJ::ObjectTopicId topic_id, QString display_name, BuiltinObjectType object_type, QObject* parent)
    : Scene3DLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)), object_type_(object_type) {
  cloud_pass_.setShape(PointcloudRenderPass::Shape::kPoint);
  cloud_pass_.setSizePixels(point_size_px_);
  cloud_pass_.setColorType(PointcloudRenderPass::ColorType::kField);
  cloud_pass_.setScalarAxis(-1);  // color by the uploaded depth scalar
  cloud_pass_.setColormap(colormap_);
}

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
  ts_first_.reset();
  decoded_at_ns_.reset();
  last_pushed_id_ = {};
  last_point_count_ = 0;
  world_bounds_.reset();
  intrinsics_cache_.reset();
  ctx_ = {};
  cloud_pass_.setActiveCloud(nullptr);
}

void DepthCloudLayer::setFixedFrame(const QString& frame) {
  if (fixed_frame_ == frame) {
    return;
  }
  fixed_frame_ = frame;
  if (source_frame_.empty()) {
    last_pushed_id_ = {};  // placed in the fixed frame -> re-stamp frame_id
    refreshNow();
  }
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
  cloud_pass_.setVisible(visible);
  emit visibilityChanged(visible);
  emit repaintRequested();
}

void DepthCloudLayer::initializeGL() {
  cloud_pass_.initializeGL();
}

void DepthCloudLayer::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (visible_ && tracker_dirty_) {
    tracker_dirty_ = false;
    renderAt(PJ::toRaw(frame_ctx.time));
  }
  cloud_pass_.render(view_params, frame_ctx);
}

void DepthCloudLayer::releaseGL() {
  cloud_pass_.releaseGL();
}

void DepthCloudLayer::setColormap(PointcloudRenderPass::Colormap cm) {
  if (colormap_ == cm) {
    return;
  }
  colormap_ = cm;
  cloud_pass_.setColormap(colormap_);
  emit configurationChanged();
  emit repaintRequested();
}

void DepthCloudLayer::setPointSizePixels(float pixels) {
  if (point_size_px_ == pixels) {
    return;
  }
  point_size_px_ = pixels;
  cloud_pass_.setSizePixels(point_size_px_);
  emit configurationChanged();
  emit repaintRequested();
}

void DepthCloudLayer::setMinDepth(float metres) {
  if (min_depth_m_ == metres) {
    return;
  }
  min_depth_m_ = metres;
  last_pushed_id_ = {};
  emit configurationChanged();
  refreshNow();
}

void DepthCloudLayer::setMaxDepth(float metres) {
  if (max_depth_m_ == metres) {
    return;
  }
  max_depth_m_ = metres;
  last_pushed_id_ = {};
  emit configurationChanged();
  refreshNow();
}

QDomElement DepthCloudLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(u"depthcloud"_s);
  auto colormap_str = [&]() -> QString {
    switch (colormap_) {
      case PointcloudRenderPass::Colormap::kTurbo:
        return u"turbo"_s;
      case PointcloudRenderPass::Colormap::kViridis:
        return u"viridis"_s;
      case PointcloudRenderPass::Colormap::kPlasma:
        return u"plasma"_s;
      case PointcloudRenderPass::Colormap::kGrayscale:
        return u"grayscale"_s;
    }
    return u"turbo"_s;
  }();
  el.setAttribute(u"colormap"_s, colormap_str);
  el.setAttribute(u"point_size_px"_s, QString::number(static_cast<double>(point_size_px_), 'g', 6));
  el.setAttribute(u"min_depth"_s, QString::number(static_cast<double>(min_depth_m_), 'g', 6));
  el.setAttribute(u"max_depth"_s, QString::number(static_cast<double>(max_depth_m_), 'g', 6));
  return el;
}

bool DepthCloudLayer::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "depthcloud"_L1) {
    return false;
  }
  if (!detail::isLeafPayload(element)) {
    return false;
  }
  const QString colormap_text = element.attribute(u"colormap"_s, u"turbo"_s);
  PointcloudRenderPass::Colormap restored_colormap;
  if (colormap_text == "turbo"_L1) {
    restored_colormap = PointcloudRenderPass::Colormap::kTurbo;
  } else if (colormap_text == "viridis"_L1) {
    restored_colormap = PointcloudRenderPass::Colormap::kViridis;
  } else if (colormap_text == "plasma"_L1) {
    restored_colormap = PointcloudRenderPass::Colormap::kPlasma;
  } else if (colormap_text == "grayscale"_L1) {
    restored_colormap = PointcloudRenderPass::Colormap::kGrayscale;
  } else {
    return false;
  }
  float restored_point_size = 0.0f;
  float restored_min_depth = 0.0f;
  float restored_max_depth = 0.0f;
  if (!detail::parseFiniteFloat(element, "point_size_px", 2.0f, 1.0f, 32.0f, restored_point_size) ||
      !detail::parseFiniteFloat(element, "min_depth", 0.0f, 0.0f, 1000.0f, restored_min_depth) ||
      !detail::parseFiniteFloat(element, "max_depth", 0.0f, 0.0f, 1000.0f, restored_max_depth) ||
      (restored_max_depth > 0.0f && restored_min_depth > restored_max_depth)) {
    return false;
  }
  setColormap(restored_colormap);
  setPointSizePixels(restored_point_size);
  setMinDepth(restored_min_depth);
  setMaxDepth(restored_max_depth);
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
  colormap_combo->addItem(u"turbo"_s, static_cast<int>(PointcloudRenderPass::Colormap::kTurbo));
  colormap_combo->addItem(u"viridis"_s, static_cast<int>(PointcloudRenderPass::Colormap::kViridis));
  colormap_combo->addItem(u"plasma"_s, static_cast<int>(PointcloudRenderPass::Colormap::kPlasma));
  colormap_combo->addItem(u"grayscale"_s, static_cast<int>(PointcloudRenderPass::Colormap::kGrayscale));
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
      colormap_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, colormap_combo](int) {
        setColormap(static_cast<PointcloudRenderPass::Colormap>(colormap_combo->currentData().toInt()));
      });
  QObject::connect(size_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) {
    setPointSizePixels(static_cast<float>(v));
  });
  QObject::connect(
      min_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) { setMinDepth(static_cast<float>(v)); });
  QObject::connect(
      max_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) { setMaxDepth(static_cast<float>(v)); });

  return container;
}

std::optional<PJ::sdk::DepthImage> DepthCloudLayer::toDepthView(const Image& image, std::vector<uint8_t>& scratch) {
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
    QImage png;
    if (!png.loadFromData(png_data, png_size)) {
      qCWarning(lcDepthCloudLayer) << "compressedDepth: PNG decode failed";
      return std::nullopt;
    }
    if (png.format() != QImage::Format_Grayscale16) {
      png = png.convertToFormat(QImage::Format_Grayscale16);
    }
    const int w = png.width();
    const int h = png.height();
    if (w <= 0 || h <= 0) {
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
    const auto* ci = std::any_cast<CameraInfo>(&obj->object);
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
  // re-check: the depth sample is already on the GPU AND its memoized intrinsics
  // still match the CameraInfo sample at the playhead. The second condition keeps
  // a CameraInfo-only update (new calibration, unchanged depth image) from being
  // swallowed by the depth memo — the same points must re-project under the new K.
  const auto already_current = [this, time_ns](const SampleId& id) {
    return id == last_pushed_id_ && intrinsicsCurrentAt(time_ns);
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
  const auto* image = std::any_cast<Image>(&obj->object);
  if (image == nullptr) {
    return;
  }
  updateSourceFrame(image->frame_id);

  std::vector<uint8_t> scratch;  // keeps decoded compressedDepth bytes alive through depthToPoints
  const auto depth_view = toDepthView(*image, scratch);
  if (!depth_view.has_value()) {
    emit warningChanged(true, tr("Not a depth image (encoding '%1')").arg(QString::fromStdString(image->encoding)));
    return;
  }

  const DepthIntrinsics intr = resolveIntrinsics(image->frame_id, time_ns);
  if (!intr.valid()) {
    qCWarning(lcDepthCloudLayer) << "renderAt: no CameraInfo intrinsics for frame"
                                 << QString::fromStdString(image->frame_id);
    emit warningChanged(
        true, tr("No CameraInfo intrinsics for frame '%1'").arg(QString::fromStdString(image->frame_id)));
    return;
  }

  BackprojectOptions opts;
  opts.min_depth_m = min_depth_m_;
  opts.max_depth_m = max_depth_m_;
  std::vector<float> scalar;
  AABB bounds;
  PJ::Range<float> scalar_range{0.0f, 1.0f};
  // One pass yields positions, depth scalars, the world AABB, and the colormap
  // range together — no separate AABB or min/max sweep over the ~1 point/pixel.
  std::vector<glm::vec3> points = depthToPoints(*depth_view, intr, opts, &scalar, &bounds, &scalar_range);

  DecodedPointCloud decoded;
  // No frame on the image -> place in the fixed frame so the geometry is visible.
  decoded.frame_id = image->frame_id.empty() ? fixed_frame_.toStdString() : image->frame_id;
  decoded.scalar_field_name = "depth";

  world_bounds_ = bounds.valid ? std::optional<AABB>{bounds} : std::nullopt;
  cloud_pass_.setColormapRange(scalar_range.min, scalar_range.max);

  last_point_count_ = points.size();
  decoded.positions = std::move(points);
  decoded.scalar = std::move(scalar);
  cloud_pass_.setActiveCloud(std::make_shared<DecodedPointCloud>(std::move(decoded)));

  last_pushed_id_ = id;
  emit warningChanged(false, QString());
  emit repaintRequested();
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
  emit sourceFrameChanged(QString::fromStdString(source_frame_));
  emit fallbackFramesChanged(fallbackFrames());
}

}  // namespace pj::scene3d
