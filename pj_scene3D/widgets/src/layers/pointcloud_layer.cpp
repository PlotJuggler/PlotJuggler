// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/layers/pointcloud_layer.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLoggingCategory>
#include <QPalette>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/glm.hpp>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

#include "layer_xml_validation.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/compressed_point_cloud.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/time.hpp"  // PJ::fromRaw, PJ::toRaw
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/camera/camera.h"  // AABB, expandAABB
#include "pj_scene3d_core/pointcloud.h"
#include "pj_scene3d_core/pointcloud_codecs.h"
#include "pj_scene3d_core/pointcloud_convert.h"  // convertCanonical, ConvertedPointCloud
#include "pj_scene3d_core/tf/tf_buffer.h"        // source->fixed lookup for world-axis range
#include "pj_scene3d_widgets/resolve_object.h"   // resolveObject, hasCanonical3DCodec
#include "pj_widgets/CheckButton.h"
#include "pj_widgets/ColorPickerWidget.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/Style.h"  // PJ::Style::kInputHeight
#include "pj_widgets/SvgButton.h"
using namespace Qt::StringLiterals;

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcPointCloudLayer, "pj.scene3d.layer.pointcloud")

using PJ::sdk::BuiltinObjectType;
using PJ::sdk::CompressedPointCloud;
using PJ::sdk::PayloadView;
using PJ::sdk::PointCloud;

std::pair<float, float> clampScalarRange(float lo, float hi) {
  if (lo > hi) {
    return {0.0f, 1.0f};
  }
  if (hi - lo < 1e-6f) {
    hi = lo + 1.0f;
  }
  return {lo, hi};
}

std::pair<float, float> computeScalarRange(const std::vector<float>& scalar) {
  float lo = std::numeric_limits<float>::max();
  float hi = std::numeric_limits<float>::lowest();
  for (float v : scalar) {
    if (!std::isfinite(v)) {
      continue;
    }
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  return clampScalarRange(lo, hi);
}

// Fixed-frame spatial colour axis for a colour-field name: x->0, y->1, z->2, else
// -1 (a non-spatial scalar field like intensity, coloured by its raw value).
int spatialAxisIndex(const std::string& field) {
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

// The GPU AABB reducer indexes the cloud buffer as uint words, so the fast-path
// byte stride and the x offset (y/z follow contiguously at +4/+8) must both be
// 4-byte aligned for the GPU path; otherwise the layer keeps the CPU scan.
bool gpuAabbAligned(const AttribLayout& layout) {
  constexpr uint32_t kAlign = PointcloudAabbReducer::kRequiredAlignmentBytes;
  return layout.stride % kAlign == 0 && layout.xyz_offset % kAlign == 0;
}

// Exact equality of optional AABBs — suppresses repaints when an async GPU
// reduction returns the extent the layer already holds (a stable streaming cloud).
bool aabbEqual(const std::optional<AABB>& a, const std::optional<AABB>& b) {
  if (a.has_value() != b.has_value()) {
    return false;
  }
  return !a.has_value() || (a->min == b->min && a->max == b->max);
}

QString defaultColorField(const QStringList& available) {
  if (available.isEmpty()) {
    return QString();
  }
  return available.contains("intensity"_L1) ? u"intensity"_s : available.first();
}

// Reserved sentinel stored as the "RGB" combo item's data, distinguishing it from a
// "Field: X" item (whose data is the field name) and "Solid" (empty data). Safe because
// the colour channel names are excluded from the field list, so this never collides.
const QString kRgbComboToken = u"__rgb__"_s;

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

PointCloudLayer::PointCloudLayer(
    PJ::ObjectTopicId topic_id, QString display_name, BuiltinObjectType object_type, QObject* parent)
    : Scene3DLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)), object_type_(object_type) {
  // Push layer defaults to the pass at construction so the first paint
  // already reflects them (layer defaults are the design-spec values, not
  // the pass's "minimum visual change" defaults).
  cloud_pass_.setShape(shape_);
  cloud_pass_.setSizeMeters(size_meters_);
  cloud_pass_.setSizePixels(size_pixels_);
  cloud_pass_.setColorType(color_type_);
  cloud_pass_.setSolidColor(glm::vec3(solid_color_.redF(), solid_color_.greenF(), solid_color_.blueF()));
  cloud_pass_.setColormap(colormap_);
  cloud_pass_.setInvertLut(invert_lut_);
  pushOutsideRangeAlpha();
  // The pass owns the GPU AABB reducer and polls it inside render() (GL thread);
  // a completed reduction flows back here to refresh world_bounds_ + the camera.
  // cloud_pass_ is a member, so `this` outlives every callback invocation.
  cloud_pass_.setBoundsCallback([this](std::optional<AABB> box) { onGpuAabb(box); });
}

PointCloudLayer::~PointCloudLayer() = default;

PJ::SceneLayerInfo PointCloudLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = object_type_,
      .display_name = display_name_,
      .family_name = u"PointCloud"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> PointCloudLayer::timeRange() const {
  return PJ::liveTopicTimeRange(ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr, topic_id_);
}

QStringList PointCloudLayer::fallbackFrames() const {
  QStringList out;
  if (!source_frame_.empty()) {
    out.append(QString::fromStdString(source_frame_));
  }
  return out;
}

QString PointCloudLayer::sourceFrame() const {
  return QString::fromStdString(source_frame_);
}

QDomElement PointCloudLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(u"pointcloud"_s);
  auto shape_str = [&]() -> QString {
    switch (shape_) {
      case PointcloudRenderPass::Shape::kSphere:
        return u"sphere"_s;
      case PointcloudRenderPass::Shape::kPoint:
        return u"point"_s;
      case PointcloudRenderPass::Shape::kCube:
        return u"cube"_s;
    }
    return u"sphere"_s;
  }();
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
  el.setAttribute(u"shape"_s, shape_str);
  el.setAttribute(u"size_meters"_s, QString::number(static_cast<double>(size_meters_), 'g', 6));
  el.setAttribute(u"size_pixels"_s, QString::number(static_cast<double>(size_pixels_), 'g', 6));
  const QString color_type_str = !color_choice_explicit_                                  ? u"auto"_s
                                 : color_type_ == PointcloudRenderPass::ColorType::kSolid ? u"solid"_s
                                 : color_type_ == PointcloudRenderPass::ColorType::kRgb   ? u"rgb"_s
                                                                                          : u"field"_s;
  el.setAttribute(u"color_type"_s, color_type_str);
  el.setAttribute(u"color_choice_explicit"_s, color_choice_explicit_ ? u"true"_s : u"false"_s);
  el.setAttribute(u"color_field"_s, QString::fromStdString(color_field_));
  el.setAttribute(u"solid_color"_s, solid_color_.name(QColor::HexRgb));
  el.setAttribute(u"colormap"_s, colormap_str);
  el.setAttribute(u"auto_range"_s, auto_range_ ? u"true"_s : u"false"_s);
  el.setAttribute(u"invert_lut"_s, invert_lut_ ? u"true"_s : u"false"_s);
  el.setAttribute(u"outside_range_opacity"_s, QString::number(static_cast<double>(outside_range_opacity_), 'g', 6));
  el.setAttribute(u"outside_range_visible"_s, outside_range_visible_ ? u"true"_s : u"false"_s);
  el.setAttribute(u"range_min"_s, QString::number(static_cast<double>(manual_range_min_), 'g', 6));
  el.setAttribute(u"range_max"_s, QString::number(static_cast<double>(manual_range_max_), 'g', 6));
  return el;
}

bool PointCloudLayer::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "pointcloud"_L1 || !detail::isLeafPayload(element)) {
    return false;
  }
  const QString shape_str = element.attribute(u"shape"_s, u"sphere"_s);
  PointcloudRenderPass::Shape restored_shape;
  if (shape_str == "sphere"_L1) {
    restored_shape = PointcloudRenderPass::Shape::kSphere;
  } else if (shape_str == "point"_L1) {
    restored_shape = PointcloudRenderPass::Shape::kPoint;
  } else if (shape_str == "cube"_L1) {
    restored_shape = PointcloudRenderPass::Shape::kCube;
  } else {
    return false;
  }
  float restored_size_meters = 0.0f;
  float restored_size_pixels = 0.0f;
  float restored_outside_opacity = 0.0f;
  float restored_range_min = 0.0f;
  float restored_range_max = 0.0f;
  if (!detail::parseFiniteFloat(element, "size_meters", 0.01f, 0.001f, 10.0f, restored_size_meters) ||
      !detail::parseFiniteFloat(element, "size_pixels", 2.0f, 1.0f, 32.0f, restored_size_pixels) ||
      !detail::parseFiniteFloat(element, "outside_range_opacity", 1.0f, 0.0f, 1.0f, restored_outside_opacity) ||
      !detail::parseFiniteFloat(
          element, "range_min", 0.0f, std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max(),
          restored_range_min) ||
      !detail::parseFiniteFloat(
          element, "range_max", 1.0f, std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max(),
          restored_range_max) ||
      restored_range_min > restored_range_max) {
    return false;
  }
  const QString color_type_str = element.attribute(u"color_type"_s, u"field"_s);
  if (color_type_str != "auto"_L1 && color_type_str != "solid"_L1 && color_type_str != "rgb"_L1 &&
      color_type_str != "field"_L1) {
    return false;
  }
  bool restored_explicit = color_type_str != "auto"_L1;
  if (!detail::parseTrueFalse(element, "color_choice_explicit", restored_explicit, restored_explicit) ||
      ((color_type_str == "auto"_L1) == restored_explicit)) {
    return false;
  }
  PointcloudRenderPass::ColorType restored_color_type = PointcloudRenderPass::ColorType::kField;
  if (color_type_str == "solid"_L1) {
    restored_color_type = PointcloudRenderPass::ColorType::kSolid;
  } else if (color_type_str == "rgb"_L1) {
    restored_color_type = PointcloudRenderPass::ColorType::kRgb;
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
  bool restored_auto_range = true;
  bool restored_invert_lut = false;
  bool restored_outside_visible = true;
  if (!detail::parseTrueFalse(element, "auto_range", true, restored_auto_range) ||
      !detail::parseTrueFalse(element, "invert_lut", false, restored_invert_lut) ||
      !detail::parseTrueFalse(element, "outside_range_visible", true, restored_outside_visible)) {
    return false;
  }
  QColor restored_solid_color;
  const bool has_solid_color = element.hasAttribute(u"solid_color"_s);
  if (has_solid_color) {
    restored_solid_color = QColor(element.attribute(u"solid_color"_s));
    if (!restored_solid_color.isValid()) {
      return false;
    }
  }
  setShape(restored_shape);
  setSizeMeters(restored_size_meters);
  setSizePixels(restored_size_pixels);
  setColorType(restored_color_type);
  color_choice_explicit_ = restored_explicit;
  // A latent ("auto") choice re-derives the colour-present RGB default the
  // smart default would have picked on the first decoded sample.
  if (!color_choice_explicit_ && has_color_) {
    color_type_ = PointcloudRenderPass::ColorType::kRgb;
    cloud_pass_.setColorType(color_type_);
  }
  if (element.hasAttribute(u"color_field"_s)) {
    setColorField(element.attribute(u"color_field"_s));
  }
  if (has_solid_color) {
    setSolidColor(restored_solid_color);
  }
  setColormap(restored_colormap);
  setInvertLut(restored_invert_lut);
  setOutsideRangeOpacity(restored_outside_opacity);
  setOutsideRangeVisible(restored_outside_visible);
  setManualRange(restored_range_min, restored_range_max);
  applyAutoRange(restored_auto_range, /*seed_manual_from_world=*/false);
  return true;
}

bool PointCloudLayer::attach(const PJ::SceneLayerContext& ctx) {
  const auto& scene3d_ctx = static_cast<const Scene3DLayerContext&>(ctx);
  if (scene3d_ctx.session == nullptr) {
    qCWarning(lcPointCloudLayer) << "attach: session is null";
    return false;
  }
  ctx_ = scene3d_ctx;
  // Accept the topic if it has EITHER a MessageParser (file/streaming sources)
  // OR a canonical codec for its object type (a data-source/toolbox that pushed
  // serialized canonical clouds, e.g. the Mosaico toolbox — resolveObject()
  // decodes those host-side). Reject only when neither path can decode it.
  if (!ctx_.session->parserBindingForObjectTopic(topic_id_) && !hasCanonical3DCodec(object_type_)) {
    qCWarning(lcPointCloudLayer) << "attach: no parser and no canonical codec for topic_id=" << topic_id_.id;
    return false;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  ts_first_.reset();  // re-derive: a detachless re-attach (reload path) may see an emptied store
  if (store.entryCount(topic_id_) > 0) {
    ts_first_ = store.timeRange(topic_id_).first;
  }
  // A reload swaps this topic's bytes under a stable ObjectTopicId, so every
  // cached sample identity must reset with them (see onDatasetAboutToBeReplaced).
  // disconnect first: the reload path re-attaches without an intervening detach.
  disconnect(reload_connection_);
  reload_connection_ = connect(
      ctx_.session, &PJ::SessionManager::datasetAboutToBeReplaced, this, &PointCloudLayer::onDatasetAboutToBeReplaced);
  if (!bootstrap()) {
    qCWarning(lcPointCloudLayer) << "attach: bootstrap failed for topic_id=" << topic_id_.id;
    // Continue anyway — render will silently skip until a sample arrives.
  }
  if (ts_first_.has_value()) {
    renderAt(*ts_first_);
  }
  return true;
}

void PointCloudLayer::detach() {
  disconnect(reload_connection_);
  reload_connection_ = {};
  drop_inflight_result_ = false;
  ts_first_.reset();
  decoded_at_ns_.reset();
  if (decode_watcher_ != nullptr) {
    decode_watcher_->disconnect(this);  // a late result must not touch a detached layer
    decode_watcher_->cancel();
    // Drop the watcher entirely: ensureDecodeWorker() only wires the finished signal
    // when it creates one, so a kept-but-disconnected watcher would leave a
    // re-attached layer decoding into the void.
    decode_watcher_->deleteLater();
    decode_watcher_ = nullptr;
  }
  pending_.reset();
  decoded_cache_.reset();
  decoded_cache_id_ = {};
  inflight_ = {};
  wanted_ = {};
  failed_id_ = {};
  last_pushed_id_ = {};
  last_pushed_color_field_.clear();
  ctx_ = {};
  cloud_pass_.setActiveCloud(nullptr);
  setWorldBounds(std::nullopt);
}

void PointCloudLayer::setFixedFrame(const QString& frame) {
  if (fixed_frame_ == frame) {
    return;
  }
  fixed_frame_ = frame;
  // Both the geometry transform and (for x/y/z colouring) the per-point colour + auto
  // colormap range are derived in the render pass from the live source->fixed model, so
  // a fixed-frame change is fully absorbed by re-running the shader — no re-decode or
  // CPU refit. A bare repaint suffices. (Non-spatial scalars are frame-independent.)
  emit repaintRequested();
}

void PointCloudLayer::setTrackerTime(PJ::Timepoint time) {
  decoded_at_ns_ = time;
  // Defer the heavy decode (parse + convertCanonical + GPU upload) to render():
  // a fast scrub fires many ticks but Qt coalesces the repaints into one paint,
  // so only the final cloud is decoded instead of every skipped frame x N topics.
  tracker_dirty_ = true;
  if (visible_) {
    emit repaintRequested();
  }
}

uint64_t PointCloudLayer::renderKey(PJ::Timepoint time) const {
  // Active cloud sample at `time` keyed by its SequentialUID via latestEntryIdAt(),
  // a pure binary search that never resolves a PayloadView — the per-tick gate must
  // not touch the cold-chunk decompression path (the decode happens once, later, in
  // renderAt() when we actually repaint). The UID, not the stamp: the store permits
  // duplicate timestamps, so a same-stamp replacement entry must still reopen the
  // dock's repaint gate (or renderAt's own UID memo would never get the chance to
  // see it); unlike a deque index it also survives retention eviction.
  uint64_t key = 0x9e3779b97f4a7c15ULL;
  if (ctx_.session != nullptr) {
    PJ::ObjectStore& store = ctx_.session->objectStore();
    if (const auto entry_id = store.latestEntryIdAt(topic_id_, PJ::toRaw(time)); entry_id.has_value()) {
      key ^= entry_id->uid.value;
    } else {
      key ^= PJ::kNoSampleRenderKey;  // no active sample at this time
    }
  }
  // Fixed←source transform (zero-order hold, so it is constant between TF samples —
  // that is what lets a 60 Hz playhead coalesce down to the TF update rate). Folding
  // the matrix also captures colour-by-axis recolouring, a pure function of it.
  if (ctx_.tf_buffer != nullptr && !source_frame_.empty()) {
    if (const auto tf = ctx_.tf_buffer->tryLookupTransform(fixed_frame_.toStdString(), source_frame_, time); tf) {
      const glm::mat4 m = glm::mat4(tf->matrix());
      for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
          uint32_t bits = 0;
          std::memcpy(&bits, &m[col][row], sizeof(bits));
          key = (key ^ bits) * 0x100000001b3ULL;
        }
      }
    }
  }
  return key;
}

void PointCloudLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  cloud_pass_.setVisible(visible);
  emit visibilityChanged(visible);
  // Catch-up on un-hide is the dock's job: SceneDockWidget::setLayerVisible
  // re-delivers the last tracker time (hidden layers receive no ticks), which
  // marks tracker_dirty_ so the next paint decodes at the playhead.
  emit repaintRequested();
}

void PointCloudLayer::initializeGL() {
  cloud_pass_.initializeGL();
}

void PointCloudLayer::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  // Drain a pending tracker move here (coalesced to one decode per painted frame).
  // renderAt's own SampleId guard makes this cheap when the active sample is
  // unchanged. frame_ctx.time and decoded_at_ns_ track the same playhead (the dock
  // paints at the tracker time); refreshNow() re-decodes from the latter on
  // color-field / range changes.
  if (visible_ && tracker_dirty_) {
    tracker_dirty_ = false;
    renderAt(PJ::toRaw(frame_ctx.time));
  }
  cloud_pass_.render(view_params, frame_ctx);
}

void PointCloudLayer::releaseGL() {
  cloud_pass_.releaseGL();
}

QWidget* PointCloudLayer::createConfigWidget(QWidget* parent) {
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

  // --- Shape ----------------------------------------------------------------
  auto* shape_combo = new PJ::ComboBox(container);
  shape_combo->addItem(tr("sphere"), static_cast<int>(PointcloudRenderPass::Shape::kSphere));
  shape_combo->addItem(tr("point"), static_cast<int>(PointcloudRenderPass::Shape::kPoint));
  shape_combo->addItem(tr("cube"), static_cast<int>(PointcloudRenderPass::Shape::kCube));
  shape_combo->setCurrentIndex(static_cast<int>(shape_));
  form->addRow(tr("Shape:"), shape_combo);

  // --- Point size (units depend on shape) ----------------------------------
  auto* size_spin = new PJ::DoubleScrubber(container);
  const auto apply_size_units = [size_spin, this]() {
    QSignalBlocker block(size_spin);
    if (shape_ == PointcloudRenderPass::Shape::kPoint) {
      size_spin->setSuffix(u" px"_s);
      size_spin->setDecimals(1);
      size_spin->setSingleStep(0.5);
      size_spin->setRange(1.0, 32.0);
      size_spin->setValue(static_cast<double>(size_pixels_));
    } else {
      size_spin->setSuffix(u" m"_s);
      size_spin->setDecimals(3);
      size_spin->setSingleStep(0.001);
      size_spin->setRange(0.001, 10.0);
      size_spin->setValue(static_cast<double>(size_meters_));
    }
  };
  apply_size_units();
  form->addRow(tr("Point size:"), size_spin);

  // --- Color type (Solid + per-field) ---------------------------------------
  auto* color_type_combo = new PJ::ComboBox(container);
  const auto rebuild_color_type_combo = [color_type_combo, this]() {
    QSignalBlocker block(color_type_combo);
    color_type_combo->clear();
    color_type_combo->addItem(tr("Solid"), QString{});
    // One "RGB" entry stands in for the cloud's colour channels (which are excluded from
    // available_color_fields_), so the user picks the literal per-point colour, not a
    // colormap over a single channel. Reserved sentinel in the item data (kRgbComboToken).
    if (has_color_) {
      color_type_combo->addItem(tr("RGB"), kRgbComboToken);
    }
    for (const QString& f : available_color_fields_) {
      color_type_combo->addItem(tr("Field: %1").arg(f), f);
    }
    if (color_type_ == PointcloudRenderPass::ColorType::kSolid) {
      color_type_combo->setCurrentIndex(0);
    } else if (color_type_ == PointcloudRenderPass::ColorType::kRgb) {
      const int idx = color_type_combo->findData(kRgbComboToken);
      color_type_combo->setCurrentIndex(idx >= 0 ? idx : 0);
    } else {
      const int idx = color_type_combo->findData(QString::fromStdString(color_field_));
      color_type_combo->setCurrentIndex(idx >= 0 ? idx : 0);
    }
  };
  rebuild_color_type_combo();
  form->addRow(tr("Color type:"), color_type_combo);

  // --- Color config swap section --------------------------------------------
  auto* color_stack = new QStackedWidget(container);
  form->addRow(tr("Color config:"), color_stack);

  // Page 0: Solid — the shared ColorPickerWidget swatch (opens a ColorPickerPopup
  // on click), consistent with every other layer's colour control.
  auto* solid_page = new QWidget(color_stack);
  auto* solid_layout = new QHBoxLayout(solid_page);
  solid_layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  solid_layout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  auto* color_button = new PJ::ColorPickerWidget(solid_page);
  color_button->setColor(solid_color_);
  solid_layout->addWidget(color_button);
  solid_layout->addStretch();
  color_stack->addWidget(solid_page);

  // Page 1: Gradient.
  auto* gradient_page = new QWidget(color_stack);
  auto* gradient_layout = new QFormLayout(gradient_page);
  gradient_layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  gradient_layout->setSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));

  // --- Colormap row: combo + invert toggle right-aligned ---
  auto* colormap_row = new QWidget(gradient_page);
  auto* colormap_row_layout = new QHBoxLayout(colormap_row);
  colormap_row_layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  colormap_row_layout->setSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  auto* colormap_combo = new PJ::ComboBox(colormap_row);
  colormap_combo->addItem(u"turbo"_s, static_cast<int>(PointcloudRenderPass::Colormap::kTurbo));
  colormap_combo->addItem(u"viridis"_s, static_cast<int>(PointcloudRenderPass::Colormap::kViridis));
  colormap_combo->addItem(u"plasma"_s, static_cast<int>(PointcloudRenderPass::Colormap::kPlasma));
  colormap_combo->addItem(u"grayscale"_s, static_cast<int>(PointcloudRenderPass::Colormap::kGrayscale));
  colormap_combo->setCurrentIndex(static_cast<int>(colormap_));
  colormap_row_layout->addWidget(colormap_combo, 1);
  auto* invert_btn = new QPushButton(colormap_row);
  invert_btn->setCheckable(true);
  invert_btn->setChecked(invert_lut_);
  invert_btn->setFocusPolicy(Qt::NoFocus);
  invert_btn->setStyleSheet(toggleButtonQss(widgetTheme(container)));
  invert_btn->setIcon(QIcon(QStringLiteral(":/resources/svg/invert.svg")));
  invert_btn->setIconSize(QSize(20, 20));
  // Match the standard icon-button extent used across the app
  // (icon_size + icon_padding = 24 — see CurveListPanel / TimelineWidget).
  invert_btn->setFixedSize(24, 24);
  invert_btn->setToolTip(tr("Invert colormap"));
  colormap_row_layout->addWidget(invert_btn, 0);
  // Label-less — the combo carries the colormap name itself.
  gradient_layout->addRow(colormap_row);

  color_stack->addWidget(gradient_page);

  // Page 2: RGB-direct — the per-point colour is used as-is, so there is nothing to
  // configure; a disabled hint stands in for the empty page.
  auto* rgb_page = new QWidget(color_stack);
  auto* rgb_layout = new QHBoxLayout(rgb_page);
  rgb_layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  auto* rgb_hint = new QLabel(tr("Per-point color"), rgb_page);
  rgb_hint->setEnabled(false);
  rgb_layout->addWidget(rgb_hint);
  rgb_layout->addStretch();
  color_stack->addWidget(rgb_page);

  // Initial stack page: Solid=0, gradient=1, RGB=2.
  color_stack->setCurrentIndex(
      color_type_ == PointcloudRenderPass::ColorType::kSolid ? 0
      : color_type_ == PointcloudRenderPass::ColorType::kRgb ? 2
                                                             : 1);

  // --- Range: row on the OUTER form (auto button as the field) ---
  auto* auto_btn = new PJ::CheckButton(tr("auto"), container);
  auto_btn->setChecked(auto_range_);
  auto_btn->setFocusPolicy(Qt::NoFocus);
  auto* auto_row = new QWidget(container);
  auto* auto_row_layout = new QHBoxLayout(auto_row);
  auto_row_layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  auto_row_layout->addWidget(auto_btn);
  auto_row_layout->addStretch();
  form->addRow(tr("Range:"), auto_row);

  // Range Min/Max also live on the OUTER form so their labels align with
  // Shape / Point size / Color type. Only visible in gradient mode with
  // auto-range OFF.
  auto* range_min_spin = new PJ::DoubleScrubber(container);
  range_min_spin->setObjectName(u"pointcloud_range_min"_s);
  range_min_spin->setDecimals(4);
  range_min_spin->setRange(-1e9, 1e9);
  range_min_spin->setValue(static_cast<double>(manual_range_min_));
  form->addRow(tr("Range Min:"), range_min_spin);

  auto* range_max_spin = new PJ::DoubleScrubber(container);
  range_max_spin->setObjectName(u"pointcloud_range_max"_s);
  range_max_spin->setDecimals(4);
  range_max_spin->setRange(-1e9, 1e9);
  range_max_spin->setValue(static_cast<double>(manual_range_max_));
  form->addRow(tr("Range Max:"), range_max_spin);

  // "Outside range" — points whose colour value leaves [Range Min, Range Max]
  // are de-emphasised via an opacity scrubber rather than clamped opaque, with an
  // eye toggle to hide them entirely (same visibility-eye glyph as elsewhere).
  // Field = [opacity scrubber | eye], mirroring the colormap row's [combo | icon]
  // layout. Shares the Range Min/Max visibility group (gradient + manual range).
  auto* outside_row = new QWidget(container);
  auto* outside_layout = new QHBoxLayout(outside_row);
  outside_layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  outside_layout->setSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  auto* outside_opacity_spin = new PJ::DoubleScrubber(outside_row);
  outside_opacity_spin->setObjectName(u"pointcloud_outside_range_opacity"_s);
  outside_opacity_spin->setDecimals(2);
  outside_opacity_spin->setSingleStep(0.05);
  outside_opacity_spin->setRange(0.0, 1.0);
  outside_opacity_spin->setValue(static_cast<double>(outside_range_opacity_));
  const auto eye_icon = [](bool visible) {
    return visible ? u":/resources/svg/visibility.svg"_s : u":/resources/svg/visibility_off.svg"_s;
  };
  auto* outside_eye = new PJ::SvgButton(eye_icon(outside_range_visible_), PJ::SvgButton::Size::kSmaller, outside_row);
  outside_eye->setObjectName(u"curveVisibilityToggle"_s);  // flat eye-toggle QSS
  outside_eye->setCheckable(true);
  outside_eye->setChecked(outside_range_visible_);
  outside_eye->setFocusPolicy(Qt::NoFocus);
  outside_eye->setToolTip(tr("Show/hide points outside the range"));
  outside_layout->addWidget(outside_opacity_spin, 1);
  outside_layout->addWidget(outside_eye, 0);
  form->addRow(tr("Outside range:"), outside_row);

  // Range step is 0.1 for spatial fields (x/y/z, always in metres) and 1.0
  // otherwise (intensity, reflectance, ring index, …). Recomputed whenever
  // the color field changes.
  const auto apply_range_step = [this, range_min_spin, range_max_spin]() {
    const bool is_spatial = spatialAxisIndex(color_field_) >= 0;
    const double step = is_spatial ? 0.1 : 1.0;
    range_min_spin->setSingleStep(step);
    range_max_spin->setSingleStep(step);
  };
  apply_range_step();

  // Visibility:
  //   Range:/auto row → shown in gradient mode; hidden in solid mode.
  //   Range Min/Max   → shown only in gradient AND auto-range OFF.
  const auto apply_range_visibility = [form, auto_row, range_min_spin, range_max_spin, outside_row,
                                       this](bool auto_on) {
    const bool gradient = color_type_ == PointcloudRenderPass::ColorType::kField;
    const bool show_spins = !auto_on && gradient;
    auto_row->setVisible(gradient);
    if (auto* lbl = form->labelForField(auto_row)) {
      lbl->setVisible(gradient);
    }
    range_min_spin->setVisible(show_spins);
    range_max_spin->setVisible(show_spins);
    if (auto* lbl = form->labelForField(range_min_spin)) {
      lbl->setVisible(show_spins);
    }
    if (auto* lbl = form->labelForField(range_max_spin)) {
      lbl->setVisible(show_spins);
    }
    outside_row->setVisible(show_spins);
    if (auto* lbl = form->labelForField(outside_row)) {
      lbl->setVisible(show_spins);
    }
  };
  apply_range_visibility(auto_range_);

  // ---- User → layer wires -------------------------------------------------

  QObject::connect(
      shape_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
      [this, shape_combo, apply_size_units](int) {
        const auto v = static_cast<PointcloudRenderPass::Shape>(shape_combo->currentData().toInt());
        setShape(v);
        apply_size_units();
      });

  QObject::connect(size_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) {
    if (shape_ == PointcloudRenderPass::Shape::kPoint) {
      setSizePixels(static_cast<float>(v));
    } else {
      setSizeMeters(static_cast<float>(v));
    }
  });

  QObject::connect(
      color_type_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
      [this, color_type_combo, color_stack, apply_range_step, apply_range_visibility](int) {
        const QString data = color_type_combo->currentData().toString();
        if (data.isEmpty()) {
          setColorType(PointcloudRenderPass::ColorType::kSolid);
          color_stack->setCurrentIndex(0);
        } else if (data == kRgbComboToken) {
          setColorType(PointcloudRenderPass::ColorType::kRgb);
          color_stack->setCurrentIndex(2);
        } else {
          setColorType(PointcloudRenderPass::ColorType::kField);
          setColorField(data);
          color_stack->setCurrentIndex(1);
          apply_range_step();
        }
        apply_range_visibility(auto_range_);
      });

  QObject::connect(color_button, &PJ::ColorPickerWidget::colorChanged, this, [this](QColor c) { setSolidColor(c); });

  QObject::connect(
      colormap_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, colormap_combo](int) {
        setColormap(static_cast<PointcloudRenderPass::Colormap>(colormap_combo->currentData().toInt()));
      });

  QObject::connect(auto_btn, &PJ::CheckButton::toggled, this, [this, apply_range_visibility](bool on) {
    setAutoRange(on);
    apply_range_visibility(on);
  });

  QObject::connect(invert_btn, &QPushButton::toggled, this, &PointCloudLayer::setInvertLut);

  QObject::connect(outside_opacity_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) {
    setOutsideRangeOpacity(static_cast<float>(v));
  });
  QObject::connect(outside_eye, &QToolButton::toggled, this, [this, outside_eye, eye_icon](bool visible) {
    outside_eye->setIconPath(eye_icon(visible));
    setOutsideRangeVisible(visible);
  });

  const auto push_manual_range = [this, range_min_spin, range_max_spin]() {
    setManualRange(static_cast<float>(range_min_spin->value()), static_cast<float>(range_max_spin->value()));
  };
  // Keep min <= max: nudging one scrubber past the other drags the other along
  // (raise min above max -> max follows up; drop max below min -> min follows
  // down). Block the sibling's signal so its programmatic update doesn't re-enter
  // the other handler, then push the now-consistent pair once.
  QObject::connect(
      range_min_spin, &PJ::DoubleScrubber::valueChanged, this,
      [range_min_spin, range_max_spin, push_manual_range](double v) {
        if (v > range_max_spin->value()) {
          QSignalBlocker block(range_max_spin);
          range_max_spin->setValue(v);
        }
        push_manual_range();
      });
  QObject::connect(
      range_max_spin, &PJ::DoubleScrubber::valueChanged, this,
      [range_min_spin, range_max_spin, push_manual_range](double v) {
        if (v < range_min_spin->value()) {
          QSignalBlocker block(range_min_spin);
          range_min_spin->setValue(v);
        }
        push_manual_range();
      });

  // ---- Layer → widget wires (out-of-band changes / auto-range refresh) ----

  QPointer<QComboBox> safe_type(color_type_combo);
  QObject::connect(this, &PointCloudLayer::colorFieldsChanged, container, [safe_type, rebuild_color_type_combo]() {
    if (!safe_type) {
      return;
    }
    rebuild_color_type_combo();
  });

  QPointer<PJ::DoubleScrubber> safe_min(range_min_spin);
  QPointer<PJ::DoubleScrubber> safe_max(range_max_spin);
  QObject::connect(this, &PointCloudLayer::autoRangeComputed, container, [safe_min, safe_max](float lo, float hi) {
    if (safe_min) {
      QSignalBlocker block(safe_min.data());
      safe_min->setValue(static_cast<double>(lo));
    }
    if (safe_max) {
      QSignalBlocker block(safe_max.data());
      safe_max->setValue(static_cast<double>(hi));
    }
  });

  // Combos and scrubbers get their uniform compact height from the QSS
  // (input_outer_height). The two custom toggle buttons (auto, invert) carry an
  // inline stylesheet that the input QSS rules can't reach, so pin them to the
  // same input height here so the whole panel lines up.
  int input_h = PJ::theme::metric(PJ::theme::Metric::InputOuterHeight, widgetTheme(container));
  if (input_h <= 0) {
    input_h = PJ::Style::kInputHeight;
  }
  invert_btn->setFixedSize(input_h, input_h);
  invert_btn->setIconSize(QSize(input_h - 6, input_h - 6));

  return container;
}

void PointCloudLayer::setColorField(const QString& field) {
  const std::string new_field = field.toStdString();
  if (color_field_ == new_field) {
    return;
  }
  color_field_ = new_field;
  range_dirty_ = true;
  emit currentColorFieldChanged(field);
  emit configurationChanged();
  refreshNow();
}

void PointCloudLayer::setShape(PointcloudRenderPass::Shape shape) {
  if (shape_ == shape) {
    return;
  }
  shape_ = shape;
  cloud_pass_.setShape(shape_);
  emit configurationChanged();
  emit repaintRequested();
}

void PointCloudLayer::setSizeMeters(float meters) {
  if (size_meters_ == meters) {
    return;
  }
  size_meters_ = meters;
  cloud_pass_.setSizeMeters(size_meters_);
  emit configurationChanged();
  emit repaintRequested();
}

void PointCloudLayer::setSizePixels(float pixels) {
  if (size_pixels_ == pixels) {
    return;
  }
  size_pixels_ = pixels;
  cloud_pass_.setSizePixels(size_pixels_);
  emit configurationChanged();
  emit repaintRequested();
}

void PointCloudLayer::setColorType(PointcloudRenderPass::ColorType type) {
  const bool explicitness_changed = !color_choice_explicit_;
  color_choice_explicit_ = true;
  if (color_type_ == type) {
    if (explicitness_changed) {
      emit configurationChanged();
    }
    return;
  }
  // Toggling RGB-direct on/off changes what convertCanonical extracts (per-point colour
  // vs. a colormap scalar), so the GPU buffer must be rebuilt — force a re-decode. A
  // kField<->kSolid switch only flips a shader uniform, so a repaint suffices.
  const bool rgb_changed =
      (color_type_ == PointcloudRenderPass::ColorType::kRgb) != (type == PointcloudRenderPass::ColorType::kRgb);
  color_type_ = type;
  cloud_pass_.setColorType(color_type_);
  emit configurationChanged();
  if (rgb_changed) {
    range_dirty_ = true;  // re-fit the scalar auto-range when leaving RGB
    refreshNow();
  } else {
    emit repaintRequested();
  }
}

void PointCloudLayer::setSolidColor(QColor color) {
  if (!color.isValid() || solid_color_.rgb() == color.rgb()) {
    return;
  }
  solid_color_ = color;
  cloud_pass_.setSolidColor(glm::vec3(solid_color_.redF(), solid_color_.greenF(), solid_color_.blueF()));
  emit configurationChanged();
  emit repaintRequested();
}

void PointCloudLayer::setColormap(PointcloudRenderPass::Colormap cm) {
  if (colormap_ == cm) {
    return;
  }
  colormap_ = cm;
  cloud_pass_.setColormap(colormap_);
  emit configurationChanged();
  emit repaintRequested();
}

void PointCloudLayer::setInvertLut(bool invert) {
  if (invert_lut_ == invert) {
    return;
  }
  invert_lut_ = invert;
  cloud_pass_.setInvertLut(invert_lut_);
  emit configurationChanged();
  emit repaintRequested();
}

void PointCloudLayer::setOutsideRangeOpacity(float opacity) {
  const float clamped = std::clamp(opacity, 0.0f, 1.0f);
  if (outside_range_opacity_ == clamped) {
    return;
  }
  outside_range_opacity_ = clamped;
  pushOutsideRangeAlpha();
  emit configurationChanged();
  emit repaintRequested();
}

void PointCloudLayer::setOutsideRangeVisible(bool visible) {
  if (outside_range_visible_ == visible) {
    return;
  }
  outside_range_visible_ = visible;
  pushOutsideRangeAlpha();
  emit configurationChanged();
  emit repaintRequested();
}

void PointCloudLayer::pushOutsideRangeAlpha() {
  cloud_pass_.setOutsideRangeAlpha(outside_range_visible_ ? outside_range_opacity_ : 0.0f);
}

void PointCloudLayer::setAutoRange(bool enable) {
  // Interactive entry point (config widget toggle): seed the manual range from
  // the world-axis range on screen when turning auto off.
  applyAutoRange(enable, /*seed_manual_from_world=*/true);
}

void PointCloudLayer::applyAutoRange(bool enable, bool seed_manual_from_world) {
  if (auto_range_ == enable) {
    return;
  }
  auto_range_ = enable;
  if (auto_range_) {
    // Force the next cloud swap to recompute, which will also re-emit
    // autoRangeComputed so the panel's hidden spinboxes refresh.
    range_dirty_ = true;
    emit configurationChanged();
    refreshNow();
    return;
  }
  // Auto off: pin the colormap to the manual range. An interactive toggle seeds
  // that manual range from the world-axis range currently on screen (so the
  // colours don't snap to a stale or sensor-local value); state restore passes
  // seed_manual_from_world=false because the saved range_min/range_max are
  // authoritative — world_bounds_ is already populated by attach(), so seeding
  // would overwrite them with the recomputed data range.
  const int axis = spatialAxisIndex(color_field_);
  if (axis >= 0) {
    if (seed_manual_from_world) {
      if (const auto frozen = currentWorldAxisRange(axis)) {
        manual_range_min_ = frozen->first;
        manual_range_max_ = frozen->second;
      }
    }
    cloud_pass_.setSpatialAutoBounds(std::nullopt);
  }
  // Pin the pass to the manual values the layer now holds so the colormap doesn't
  // snap to stale auto-computed bounds, and refresh the panel's spinboxes to match.
  cloud_pass_.setColormapRange(manual_range_min_, manual_range_max_);
  emit autoRangeComputed(manual_range_min_, manual_range_max_);
  emit configurationChanged();
  emit repaintRequested();
}

void PointCloudLayer::setManualRange(float min_value, float max_value) {
  if (manual_range_min_ == min_value && manual_range_max_ == max_value) {
    return;
  }
  manual_range_min_ = min_value;
  manual_range_max_ = max_value;
  emit configurationChanged();
  if (!auto_range_) {
    cloud_pass_.setColormapRange(manual_range_min_, manual_range_max_);
    emit repaintRequested();
  }
}

bool PointCloudLayer::bootstrap() {
  PJ::ObjectStore& store = ctx_.session->objectStore();
  auto first = store.at(topic_id_, 0);
  if (!first.has_value() || first->payload.bytes.empty()) {
    return false;
  }
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  auto obj = resolveObject(binding, object_type_, first->timestamp, first->payload);
  if (!obj.has_value()) {
    qCWarning(lcPointCloudLayer) << "bootstrap parseObject failed:" << QString::fromStdString(obj.error());
    return false;
  }
  // Compressed topic (Draco / Cloudini): the wrapper gives frame_id without a
  // decode, but the field list needs one. Decode the first sample asynchronously so
  // attach never blocks the UI — fields populate when the result lands.
  if (const auto* cpc = std::any_cast<CompressedPointCloud>(&obj->object)) {
    requestDecode(*cpc, SampleId{first->sequential_uid, first->timestamp});
    return true;
  }

  const auto* sdk_cloud = std::any_cast<PointCloud>(&obj->object);
  if (sdk_cloud == nullptr) {
    return false;
  }
  updateSourceFrame(sdk_cloud->frame_id);
  populateColorFields(*sdk_cloud);
  return true;
}

void PointCloudLayer::populateColorFields(const PointCloud& cloud) {
  has_color_ = detectColorLayout(cloud).valid;

  // A cloud's colour channels describe one per-point colour, so they are offered as a
  // single "RGB" mode — never as individually colourable scalar fields. Exclude the
  // channel names (and the never-useful "timestamp") from the scalar field list.
  const auto is_color_channel = [](const std::string& name) {
    return name == "red" || name == "green" || name == "blue" || name == "alpha" || name == "rgb" || name == "rgba";
  };
  available_color_fields_.clear();
  for (const auto& f : cloud.fields) {
    if (f.name == "timestamp" || (has_color_ && is_color_channel(f.name))) {
      continue;
    }
    available_color_fields_.append(QString::fromStdString(f.name));
  }

  // Default colour mode on a FRESH attach (no explicit restore/user pick): a cloud
  // carrying colour defaults to RGB-direct (matches Foxglove Studio / rviz). Set the
  // field directly rather than via setColorType() — populate runs inside the decode
  // path and the pushCloud that follows reads color_type_, so no re-decode is needed
  // (and we avoid the refreshNow() reentrancy setColorType would trigger).
  if (!color_choice_explicit_ && has_color_) {
    color_type_ = PointcloudRenderPass::ColorType::kRgb;
    cloud_pass_.setColorType(color_type_);
  } else if (color_type_ == PointcloudRenderPass::ColorType::kRgb && !has_color_) {
    // A restored/explicit RGB mode but this cloud has no colour: degrade to a scalar.
    color_type_ = PointcloudRenderPass::ColorType::kField;
    cloud_pass_.setColorType(color_type_);
  }

  // Preserve an already-chosen field (e.g. one restored by xmlLoadState before the
  // async first decode landed) when it's still present; only fall back to the default
  // when the current selection is empty or no longer valid.
  if (color_field_.empty() || !available_color_fields_.contains(QString::fromStdString(color_field_))) {
    color_field_ = defaultColorField(available_color_fields_).toStdString();
  }
  emit colorFieldsChanged(available_color_fields_);
  if (!color_field_.empty()) {
    emit currentColorFieldChanged(QString::fromStdString(color_field_));
  }
}

void PointCloudLayer::updateSourceFrame(const std::string& frame_id) {
  // frame_id can change across samples (e.g. a recording bridging a config reload);
  // keep the dock's orphan check and the fallback-frames list current.
  if (frame_id == source_frame_) {
    return;
  }
  source_frame_ = frame_id;
  emit sourceFrameChanged(QString::fromStdString(source_frame_));
  emit fallbackFramesChanged(fallbackFrames());
}

std::optional<std::pair<float, float>> PointCloudLayer::currentWorldAxisRange(int axis) const {
  if (!world_bounds_ || ctx_.tf_buffer == nullptr || !decoded_at_ns_) {
    return std::nullopt;
  }
  // fixed<-source at the tracker time — the same transform the render pass applies.
  const auto tf = ctx_.tf_buffer->tryLookupTransform(fixed_frame_.toStdString(), source_frame_, *decoded_at_ns_);
  if (!tf) {
    return std::nullopt;
  }
  return transformedAabbAxisRange(*world_bounds_, glm::mat4(tf->matrix()), axis);
}

void PointCloudLayer::pushCloud(const PointCloud& cloud, SampleId id) {
  updateSourceFrame(cloud.frame_id);
  if (available_color_fields_.isEmpty() && !cloud.fields.empty()) {
    // Self-heal after a failed bootstrap (topic attached before its first sample):
    // the first cloud that reaches the GPU also reveals the field set.
    populateColorFields(cloud);
  }
  // RGB-direct mode reads per-point colour from the cloud's colour field instead of a colormap
  // scalar. Fixed-frame (x/y/z) colouring derives colour from the GPU-transformed position
  // (see PointcloudRenderPass::setScalarAxis), so there is no raw scalar to decode for it.
  const bool rgb_mode = color_type_ == PointcloudRenderPass::ColorType::kRgb && has_color_;
  const bool colormap = color_type_ == PointcloudRenderPass::ColorType::kField;
  const int axis = rgb_mode ? -1 : spatialAxisIndex(color_field_);
  const std::string_view scalar_sv = (rgb_mode || axis >= 0) ? std::string_view{} : std::string_view{color_field_};
  const std::size_t point_count = static_cast<std::size_t>(cloud.width) * static_cast<std::size_t>(cloud.height);
  // RGB-direct takes the zero-copy fast path too when the cloud has a packed (contiguous) rgba
  // field: the wire colour bytes upload verbatim (want_rgba), no CPU extraction. Separate
  // red/green/blue channels (non-contiguous) fall back to convertCanonical's CPU rgba packer.
  std::optional<AttribLayout> layout =
      rgb_mode ? checkFastPath(cloud, std::string_view{}, /*want_rgba=*/true) : checkFastPath(cloud, scalar_sv);

  // Whether world_bounds_ ends up describing THIS cloud (see the publish at the end).
  bool bounds_describe_this_cloud = true;
  if (layout.has_value()) {
    cloud_pass_.setScalarAxis(axis);
    // The per-point SCALAR min/max (a kField auto-range colormap's range) is needed only for a
    // non-spatial field whose auto-range is dirty; solid colour, RGB-direct, a disabled auto-range,
    // a spatial axis, and a non-dirty field all need no scalar reduction (sf == nullptr).
    const bool want_scalar_range = colormap && auto_range_ && axis < 0 && range_dirty_;
    const PJ::sdk::PointField* sf =
        (want_scalar_range && !color_field_.empty()) ? findField(cloud.fields, color_field_) : nullptr;

    // world_bounds_ feeds the camera scene-fit / adaptive near-far EVERY frame
    // (Scene3DDockWidget::updateSceneBounds), so the geometry AABB must refresh on every new
    // sample. On the bounds-only case (sf == nullptr) with a GPU-aligned layout, that reduction
    // moves onto the GPU: the FIRST eligible sample still CPU-scans (to seed world_bounds_ AND
    // probe compute support), then the async GPU reduction maintains it via onGpuAabb() and the CPU
    // scan leaves the hot path. A CPU scalar pass (sf != nullptr), a misaligned layout, an unseeded
    // world_bounds_, or a compute-less context all keep the CPU scan.
    const bool gpu_unavailable = cloud_pass_.gpuAabbProbed() && !cloud_pass_.gpuAabbAvailable();
    const bool gpu_candidate =
        sf == nullptr && gpuAabbAligned(*layout) && world_bounds_.has_value() && !gpu_unavailable;
    cloud_pass_.setGpuAabbEnabled(gpu_candidate);
    const bool gpu_authoritative = gpu_candidate && cloud_pass_.gpuAabbAvailable();
    // On the GPU-authoritative path world_bounds_ still describes the PREVIOUS sample
    // until the async reduction lands, so this push must not hand it to the frustum
    // cull; the pass adopts the reduction's own result instead.
    bounds_describe_this_cloud = !gpu_authoritative;

    if (!gpu_authoritative) {
      const BoundsScanResult scan = scanBoundsAndScalarRange(cloud, *layout, sf);
      setWorldBounds(scan.bounds.valid ? std::optional<AABB>{scan.bounds} : std::nullopt);
      if (axis >= 0) {
        cloud_pass_.setSpatialAutoBounds(auto_range_ ? world_bounds_ : std::optional<AABB>{});
        if (!auto_range_) {
          cloud_pass_.setColormapRange(manual_range_min_, manual_range_max_);
        }
        range_dirty_ = false;
      } else {
        cloud_pass_.setSpatialAutoBounds(std::nullopt);
        if (sf != nullptr) {
          // Match the fallback's computeScalarRange exactly, including its all-NaN behaviour:
          // a field with no finite value yields {0,1} (computeScalarRange's lo>hi clamp branch)
          // and still sets the range + emits + clears dirty.
          const auto [lo, hi] = scan.scalar_range
                                    ? clampScalarRange(scan.scalar_range->first, scan.scalar_range->second)
                                    : std::pair<float, float>{0.0f, 1.0f};
          cloud_pass_.setColormapRange(lo, hi);
          range_dirty_ = false;
          emit autoRangeComputed(lo, hi);
        }
      }
    } else {
      // GPU authoritative: skip the CPU scan (world_bounds_ is maintained async by onGpuAabb()),
      // but still mirror the colormap-range wiring the CPU branch would do. It reads the last-known
      // world_bounds_; for a spatial AUTO axis onGpuAabb() refreshes spatial_auto_bounds_ when the
      // extent changes, so the range follows the GPU result a frame later.
      if (axis >= 0) {
        cloud_pass_.setSpatialAutoBounds(auto_range_ ? world_bounds_ : std::optional<AABB>{});
        if (!auto_range_) {
          cloud_pass_.setColormapRange(manual_range_min_, manual_range_max_);
        }
        range_dirty_ = false;
      } else {
        cloud_pass_.setSpatialAutoBounds(std::nullopt);
      }
    }

    FastCloudData fc;
    fc.wire = cloud;
    fc.point_count = point_count;
    fc.layout = *layout;
    cloud_pass_.setActiveFastCloud(std::move(fc));
  } else {
    ConvertedPointCloud converted = convertCanonical(cloud, scalar_sv, /*extract_rgba=*/rgb_mode);
    DecodedPointCloud& decoded = converted.cloud;

    // convertCanonical accumulates the finite-point AABB in its single decode pass, so the
    // source-frame bounds come back for free (TF is applied per-render in the shader, so the
    // decoded positions stay in the cloud's own frame).
    setWorldBounds(converted.bounds.valid ? std::optional<AABB>{converted.bounds} : std::nullopt);

    cloud_pass_.setScalarAxis(axis);
    if (axis >= 0) {
      // The pass derives both the per-point colour AND (when auto) the colormap range
      // from these source bounds transformed by the live source->fixed model, so a
      // fixed-frame or TF change needs no re-decode. Manual range pins explicit
      // world-axis bounds; the shader still colours by the fixed-frame coordinate.
      cloud_pass_.setSpatialAutoBounds(auto_range_ ? world_bounds_ : std::optional<AABB>{});
      if (!auto_range_) {
        cloud_pass_.setColormapRange(manual_range_min_, manual_range_max_);
      }
      range_dirty_ = false;
    } else {
      // Non-spatial field: colour by the raw per-point scalar in the cloud's own frame.
      // Recompute the auto-range only when actually dirty (new field / re-enabled auto).
      cloud_pass_.setSpatialAutoBounds(std::nullopt);
      if (colormap && !decoded.scalar.empty() && auto_range_ && range_dirty_) {
        const auto [lo, hi] = computeScalarRange(decoded.scalar);
        cloud_pass_.setColormapRange(lo, hi);
        range_dirty_ = false;
        emit autoRangeComputed(lo, hi);
      }
    }

    cloud_pass_.setActiveCloud(std::make_shared<DecodedPointCloud>(std::move(decoded)));
  }
  // Re-publish the extent AFTER the cloud swap: setActiveCloud/setActiveFastCloud clear
  // the pass's frustum-cull box on purpose (bounds from the previous cloud must never
  // cull the new one), so the value this push computed has to be handed over again.
  if (bounds_describe_this_cloud) {
    cloud_pass_.setGeometryBounds(world_bounds_);
  }
  last_pushed_id_ = id;
  last_pushed_color_field_ = color_field_;
  last_pushed_rgb_ = rgb_mode;
  last_pushed_axis_ = axis;
  emit repaintRequested();
}

void PointCloudLayer::setWorldBounds(std::optional<AABB> bounds) {
  world_bounds_ = bounds;
  // The pass frustum-culls the whole draw against this, so it has to track every
  // update — including the resets that mean "extent unknown, draw it regardless".
  cloud_pass_.setGeometryBounds(std::move(bounds));
}

void PointCloudLayer::onGpuAabb(std::optional<AABB> box) {
  // The async GPU reduction completed (called from the pass's render(), GL thread). An invalid
  // box means the cloud had no finite points -> no bounds, same as the CPU scan.
  const std::optional<AABB> new_bounds = (box && box->valid) ? box : std::nullopt;
  if (aabbEqual(new_bounds, world_bounds_)) {
    return;  // unchanged extent (stable streaming cloud) — no spurious repaint
  }
  setWorldBounds(new_bounds);
  if (last_pushed_axis_ >= 0 && auto_range_) {
    // For spatial-axis auto colouring the pass derives the colormap range from these source
    // bounds each frame, so the GPU result must refresh them too (the CPU path did this inline).
    cloud_pass_.setSpatialAutoBounds(world_bounds_);
  }
  emit repaintRequested();  // re-fits the camera via Scene3DDockWidget::updateSceneBounds
}

bool PointCloudLayer::servedFromMemos(SampleId id) {
  // This sample is now the one the tracker wants — record it on EVERY outcome
  // (skip, cache push, failure memo, and the resolve path that follows), so a
  // stale in-flight compressed decode of another sample is classified stale in
  // onDecodeFinished instead of overwriting a newer cloud on a mixed-mode topic.
  wanted_ = id;
  // The tracker ticks at ~60 Hz but a topic publishes far slower, so the common case
  // is "same sample, same color field" — skip the whole parse/convert/upload then.
  // range_dirty_ only matters when auto-range will actually recompute in pushCloud.
  if (id == last_pushed_id_ && color_field_ == last_pushed_color_field_ &&
      (color_type_ == PointcloudRenderPass::ColorType::kRgb) == last_pushed_rgb_ && !(auto_range_ && range_dirty_)) {
    return true;
  }
  // Compressed sample already decoded? Re-convert from cache so repaints /
  // color-field changes don't re-run the codec (or even the wrapper parse).
  if (decoded_cache_ && decoded_cache_id_ == id) {
    pushCloud(*decoded_cache_, id);
    return true;
  }
  // Known-undecodable sample; the memo holds until a reload swaps the bytes.
  return id == failed_id_;
}

void PointCloudLayer::renderAt(int64_t time_ns) {
  if (ctx_.session == nullptr) {
    return;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  // Metadata-first gate: identify the entry latestAt() would resolve (a pure
  // binary search) and consult the sample memos BEFORE touching payload bytes.
  // Resolving first would re-fetch an evicted pool seed (file read + whole-chunk
  // decompress on the GUI thread) only to discard the bytes on an identity match.
  const auto entry_id = store.latestEntryIdAt(topic_id_, time_ns);
  if (!entry_id.has_value()) {
    return;
  }
  if (servedFromMemos(*entry_id)) {
    return;
  }
  auto resolved = store.latestAt(topic_id_, time_ns);
  if (!resolved.has_value() || resolved->payload.bytes.empty()) {
    return;
  }
  const SampleId id{resolved->sequential_uid, resolved->timestamp};
  // A streaming insert between the two store queries can change the pick —
  // re-consult the memos for the entry actually resolved.
  if (id.uid != entry_id->uid && servedFromMemos(id)) {
    return;
  }
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  auto obj = resolveObject(binding, object_type_, resolved->timestamp, resolved->payload);
  if (!obj.has_value()) {
    qCWarning(lcPointCloudLayer) << "renderAt parseObject failed:" << QString::fromStdString(obj.error());
    return;
  }

  // The mode is derived per-sample rather than latched at bootstrap, so a topic
  // whose first sample arrives only after attach (failed bootstrap) still renders.
  if (const auto* cpc = std::any_cast<CompressedPointCloud>(&obj->object)) {
    requestDecode(*cpc, id);  // off the UI thread; pushes when ready
    return;
  }

  const auto* sdk_cloud = std::any_cast<PointCloud>(&obj->object);
  if (sdk_cloud == nullptr) {
    return;
  }
  pushCloud(*sdk_cloud, id);
}

void PointCloudLayer::ensureDecodeWorker() {
  if (decode_watcher_ == nullptr) {
    decode_watcher_ = new QFutureWatcher<DecodeResult>(this);
    connect(decode_watcher_, &QFutureWatcher<DecodeResult>::finished, this, &PointCloudLayer::onDecodeFinished);
  }
}

void PointCloudLayer::requestDecode(const CompressedPointCloud& cloud, SampleId id) {
  updateSourceFrame(cloud.frame_id);
  wanted_ = id;  // a late decode of any other sample must not be painted
  ensureDecodeWorker();
  if (inflight_ == id) {
    pending_.reset();  // the running decode is wanted again; drop any superseded sample
    return;
  }
  // Gate on inflight_, NOT QFutureWatcher::isRunning(): a future can be finished with
  // its finished() event still queued, and setFuture() in that window would silently
  // drop the event (and the decoded result with it). inflight_ is reset only once
  // onDecodeFinished() actually ran, so the pending queue catches that window too.
  if (inflight_ != SampleId{}) {
    pending_ = PendingDecode{cloud, id};  // the shared anchor keeps the blob alive
    return;
  }
  startDecode(cloud, id);
}

void PointCloudLayer::startDecode(const CompressedPointCloud& cloud, SampleId id) {
  inflight_ = id;
  ++start_decode_count_;
  // Capture the wrapper by value — its BufferAnchor keeps the compressed bytes alive on
  // the worker. This requires the anchored bytes to be IMMUTABLE, not merely alive:
  // every in-tree producer either deep-copies (canonical codec) or anchors a const
  // ObjectStore buffer, but a zero-copy parser reusing a scratch buffer would race the
  // worker. The task touches no layer state (decodeCompressedPointCloud is a pure core
  // function), so it stays safe even if the layer is destroyed mid-decode.
  CompressedPointCloud snapshot = cloud;
  decode_watcher_->setFuture(QtConcurrent::run([snapshot = std::move(snapshot), id]() -> DecodeResult {
    DecodeResult result;
    result.id = id;
    // Exception barrier: decodeCompressedPointCloud is barriered internally, but
    // the residue here (shared_ptr control block, error-string copy) can still
    // throw under memory pressure. An exception stored in the future would be
    // rethrown by decode_watcher_->result() ON THE GUI THREAD and terminate the
    // app — module policy forbids worker exceptions reaching the GUI thread.
    try {
      auto decoded = decodeCompressedPointCloud(snapshot);
      if (decoded.has_value()) {
        result.cloud = std::make_shared<PointCloud>(std::move(decoded.value()));
      } else {
        result.error = QString::fromStdString(decoded.error());
      }
    } catch (const std::bad_alloc&) {
      result.cloud.reset();
      result.error = u"out of memory finalizing decoded point cloud"_s;  // no-alloc literal
    } catch (const std::exception& ex) {
      result.cloud.reset();
      result.error = QString::fromUtf8(ex.what());
    } catch (...) {
      result.cloud.reset();
      result.error = u"unknown exception decoding point cloud"_s;
    }
    return result;
  }));
}

void PointCloudLayer::onDecodeFinished() {
  const DecodeResult result = decode_watcher_->result();
  inflight_ = {};
  // A dataset reload while this sample decoded means the result holds pre-reload
  // content under a UID that no longer names a live entry: neither the cache nor
  // the failure memo may keep it (wanted_ was reset too, so it can never paint).
  // Still fall through to the pending drain below.
  const bool drop_result = drop_inflight_result_;
  drop_inflight_result_ = false;
  // Render this result only if it's still the sample the tracker wants. If the user
  // scrubbed away while it decoded — even back onto a cached frame — it's stale: cache
  // it (cheap, useful on scrub-back) but don't paint it over the live frame.
  const bool is_current = result.id == wanted_;

  if (drop_result) {
    // Discarded pre-reload result: no cache, no failed_id_, no paint.
  } else if (result.cloud) {
    decoded_cache_ = result.cloud;
    decoded_cache_id_ = result.id;
    if (available_color_fields_.isEmpty()) {
      populateColorFields(*result.cloud);  // first successful decode reveals the field set
    }
    if (is_current) {
      pushCloud(*result.cloud, result.id);
    }
  } else {
    failed_id_ = result.id;  // memoize: renderAt won't re-request a sample that can never decode
    if (is_current) {
      qCWarning(lcPointCloudLayer) << "compressed point cloud decode failed:" << result.error;
      // Match the raw path's malformed-cloud behavior (empty convertCanonical):
      // clear the view rather than leaving the previous sample's points painted
      // at the wrong tracker time.
      cloud_pass_.setActiveCloud(std::make_shared<DecodedPointCloud>());
      setWorldBounds(std::nullopt);
      last_pushed_id_ = {};
      last_pushed_color_field_.clear();
      emit repaintRequested();
    }
  }

  // Drain the latest-wins queue — but only if the queued sample is still the one
  // the tracker wants; decoding a stale one would evict the wanted cache entry.
  if (pending_.has_value()) {
    const PendingDecode next = std::move(*pending_);
    pending_.reset();
    if (next.id == wanted_ && next.id != failed_id_) {
      startDecode(next.cloud, next.id);
    }
  }
}

void PointCloudLayer::refreshNow() {
  // Decode at the latest delivered tracker time, else at the first sample's
  // stamp (captured in attach). Presence is explicit — 0 ns is a valid stamp,
  // so a sim-time dataset starting at t=0 still refreshes.
  if (decoded_at_ns_.has_value()) {
    renderAt(PJ::toRaw(*decoded_at_ns_));
  } else if (ts_first_.has_value()) {
    renderAt(*ts_first_);
  }
}

void PointCloudLayer::onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id) {
  if (ctx_.session == nullptr || ctx_.session->objectStore().descriptor(topic_id_).dataset_id != dataset_id) {
    return;
  }
  // replaceDataset keeps the ObjectTopicId stable while swapping the bytes. The
  // new entries carry fresh SequentialUIDs, so a stale id can never alias them —
  // but the pre-reload caches must still drop their content (memory held by the
  // decode cache, an in-flight decode that must not be painted or memoized) and
  // wanted_ must reset so only post-reload requests are honored.
  decoded_cache_.reset();
  decoded_cache_id_ = {};
  failed_id_ = {};
  last_pushed_id_ = {};
  last_pushed_color_field_.clear();
  pending_.reset();
  wanted_ = {};
  if (inflight_ != SampleId{}) {
    drop_inflight_result_ = true;  // the in-flight worker is decoding pre-reload bytes
  }
  // The signal precedes the swap and replaceDataset runs no event loop, so the
  // repaint scheduled here paints after the new bytes are in place.
  tracker_dirty_ = true;
  emit repaintRequested();
}

}  // namespace pj::scene3d
