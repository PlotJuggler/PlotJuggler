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
#include <QtConcurrent/QtConcurrentMap>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <functional>
#include <glm/glm.hpp>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "layer_xml_validation.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/compressed_point_cloud.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/time.hpp"  // PJ::fromRaw, PJ::toRaw
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/camera/camera.h"  // AABB, expandAABB
#include "pj_scene3d_core/grid_map_view.h"  // finiteRange
#include "pj_scene3d_core/pointcloud.h"
#include "pj_scene3d_core/pointcloud_codecs.h"
#include "pj_scene3d_core/pointcloud_convert.h"  // planPointVertices, packPointVertices
#include "pj_scene3d_core/scene_limits.h"
#include "pj_scene3d_core/tf/object_placement.h"
#include "pj_scene3d_widgets/resolve_object.h"  // resolveObject, hasCanonical3DCodec
#include "pj_scene3d_widgets/retained_upload.h"
#include "pj_tf/tf_buffer.h"  // source->fixed lookup for world-axis range
#include "pj_widgets/CheckButton.h"
#include "pj_widgets/ColorPickerWidget.h"
#include "pj_widgets/Colormap.h"
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

// A sample arriving this soon after the previous one marks a stream: its layer
// keeps the uploaded array to rewrite in place. A slower stream re-allocates per
// sample, which costs little against its period.
constexpr std::chrono::milliseconds kStreamingWindow{1000};

std::pair<float, float> clampScalarRange(float lo, float hi) {
  if (lo > hi) {
    return {0.0f, 1.0f};
  }
  if (hi - lo < 1e-6f) {
    hi = lo + 1.0f;
  }
  return {lo, hi};
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

// Per-cloud admission against kSceneLimits; empty when `cloud` fits.
QString cloudAdmissionError(const PointCloud& cloud) {
  const uint64_t point_count = static_cast<uint64_t>(cloud.width) * cloud.height;
  if (point_count > kSceneLimits.max_points_per_cloud) {
    return PointCloudLayer::tr("Cloud has %1 points; the limit is %2")
        .arg(QString::number(point_count), QString::number(kSceneLimits.max_points_per_cloud));
  }
  if (static_cast<uint64_t>(cloud.data.size()) > kSceneLimits.max_point_wire_bytes_per_cloud) {
    return PointCloudLayer::tr("Cloud payload is %1 MiB; the limit is %2 MiB")
        .arg(QString::number(static_cast<double>(cloud.data.size()) / (1024.0 * 1024.0), 'f', 1))
        .arg(kSceneLimits.max_point_wire_bytes_per_cloud / (1024ULL * 1024ULL));
  }
  return {};
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

// Moves a freshly decoded cloud's own stamp onto the dataset clock, as TF ingest
// moves transform stamps: a time-shifted merge leaves payload stamps unshifted.
// Done once at decode (the record is still its single owner), so the async
// decode, the decode cache and every re-push carry it. An unstamped cloud stays 0.
void shiftStampToDatasetClock(PJ::sdk::BuiltinObject& object, PJ::Timestamp payload_stamp_shift) {
  PJ::Timestamp* stamp = nullptr;
  if (auto* cloud = object.get<PointCloud>(); cloud != nullptr) {
    stamp = &cloud->timestamp_ns;
  } else if (auto* compressed = object.get<CompressedPointCloud>(); compressed != nullptr) {
    stamp = &compressed->timestamp_ns;
  }
  if (stamp != nullptr && *stamp != 0) {
    *stamp += payload_stamp_shift;
  }
}

}  // namespace

void parallelForOnPool(std::size_t count, const std::function<void(std::size_t)>& body) {
  std::vector<std::size_t> chunks(count);
  std::iota(chunks.begin(), chunks.end(), std::size_t{0});
  QtConcurrent::blockingMap(chunks, [&body](std::size_t chunk) { body(chunk); });
}

PointCloudLayer::PointCloudLayer(
    PJ::ObjectTopicId topic_id, QString display_name, BuiltinObjectType object_type, QObject* parent)
    : Scene3DLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)), object_type_(object_type) {
  decode_worker_.setWaitingFilter(
      [this](const PendingDecode& next) { return next.id == wanted_ && next.id != failed_id_; });
  evict_timer_.setSingleShot(true);
  connect(&evict_timer_, &QTimer::timeout, this, [this]() {
    // Armed once, not per upload: re-armed for the rest of the window while uploads still come.
    if (const auto quiet = std::chrono::milliseconds(last_stream_upload_.elapsed()); quiet < kStreamingWindow) {
      evict_timer_.start(kStreamingWindow - quiet);
      return;
    }
    streaming_ = false;
    if (uploaded_revision_ == geometry_revision_) {
      evictVertices();
    }
  });
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
      case PointShape::kSphere:
        return u"sphere"_s;
      case PointShape::kPoint:
        return u"point"_s;
      case PointShape::kCube:
        return u"cube"_s;
    }
    return u"sphere"_s;
  }();
  el.setAttribute(u"shape"_s, shape_str);
  el.setAttribute(u"size_meters"_s, QString::number(static_cast<double>(size_meters_), 'g', 6));
  el.setAttribute(u"size_pixels"_s, QString::number(static_cast<double>(size_pixels_), 'g', 6));
  const QString color_type_str = !color_choice_explicit_                 ? u"auto"_s
                                 : color_type_ == PointColorType::kSolid ? u"solid"_s
                                 : color_type_ == PointColorType::kRgb   ? u"rgb"_s
                                                                         : u"field"_s;
  el.setAttribute(u"color_type"_s, color_type_str);
  el.setAttribute(u"color_choice_explicit"_s, color_choice_explicit_ ? u"true"_s : u"false"_s);
  el.setAttribute(u"color_field"_s, QString::fromStdString(color_field_));
  el.setAttribute(u"solid_color"_s, solid_color_.name(QColor::HexRgb));
  el.setAttribute(u"colormap"_s, PJ::colormapToken(colormap_));
  el.setAttribute(u"auto_range"_s, auto_range_ ? u"true"_s : u"false"_s);
  el.setAttribute(u"invert_lut"_s, invert_lut_ ? u"true"_s : u"false"_s);
  el.setAttribute(u"outside_range_opacity"_s, QString::number(static_cast<double>(outside_range_opacity_), 'g', 6));
  el.setAttribute(u"outside_range_visible"_s, outside_range_visible_ ? u"true"_s : u"false"_s);
  el.setAttribute(u"range_min"_s, QString::number(static_cast<double>(manual_range_min_), 'g', 6));
  el.setAttribute(u"range_max"_s, QString::number(static_cast<double>(manual_range_max_), 'g', 6));
  return el;
}

std::optional<PointCloudLayer::Settings> PointCloudLayer::parseSettings(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "pointcloud"_L1 || !detail::isLeafPayload(element)) {
    return std::nullopt;
  }
  Settings settings;
  const QString shape_str = element.attribute(u"shape"_s, u"sphere"_s);
  if (shape_str == "sphere"_L1) {
    settings.shape = PointShape::kSphere;
  } else if (shape_str == "point"_L1) {
    settings.shape = PointShape::kPoint;
  } else if (shape_str == "cube"_L1) {
    settings.shape = PointShape::kCube;
  } else {
    return std::nullopt;
  }
  if (!detail::parseFiniteFloat(element, "size_meters", 0.01f, 0.001f, 10.0f, settings.size_meters) ||
      !detail::parseFiniteFloat(element, "size_pixels", 2.0f, 1.0f, 32.0f, settings.size_pixels) ||
      !detail::parseFiniteFloat(element, "outside_range_opacity", 1.0f, 0.0f, 1.0f, settings.outside_range_opacity) ||
      !detail::parseFiniteFloat(
          element, "range_min", 0.0f, std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max(),
          settings.range_min) ||
      !detail::parseFiniteFloat(
          element, "range_max", 1.0f, std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max(),
          settings.range_max) ||
      settings.range_min > settings.range_max) {
    return std::nullopt;
  }
  const QString color_type_str = element.attribute(u"color_type"_s, u"field"_s);
  if (color_type_str != "auto"_L1 && color_type_str != "solid"_L1 && color_type_str != "rgb"_L1 &&
      color_type_str != "field"_L1) {
    return std::nullopt;
  }
  bool restored_explicit = color_type_str != "auto"_L1;
  if (!detail::parseTrueFalse(element, "color_choice_explicit", restored_explicit, restored_explicit) ||
      ((color_type_str == "auto"_L1) == restored_explicit)) {
    return std::nullopt;
  }
  settings.color_choice_explicit = restored_explicit;
  if (color_type_str == "solid"_L1) {
    settings.color_type = PointColorType::kSolid;
  } else if (color_type_str == "rgb"_L1) {
    settings.color_type = PointColorType::kRgb;
  }
  const auto colormap = PJ::parseColormap(element.attribute(u"colormap"_s, PJ::colormapToken(PJ::Colormap::kTurbo)));
  if (!colormap.has_value()) {
    return std::nullopt;
  }
  settings.colormap = *colormap;
  if (!detail::parseTrueFalse(element, "auto_range", true, settings.auto_range) ||
      !detail::parseTrueFalse(element, "invert_lut", false, settings.invert_lut) ||
      !detail::parseTrueFalse(element, "outside_range_visible", true, settings.outside_range_visible)) {
    return std::nullopt;
  }
  if (element.hasAttribute(u"solid_color"_s)) {
    const QColor color(element.attribute(u"solid_color"_s));
    if (!color.isValid()) {
      return std::nullopt;
    }
    settings.solid_color = color;
  }
  if (element.hasAttribute(u"color_field"_s)) {
    settings.color_field = element.attribute(u"color_field"_s);
  }
  return settings;
}

bool PointCloudLayer::validateXml(const QDomElement& element) {
  return parseSettings(element).has_value();
}

bool PointCloudLayer::xmlLoadState(const QDomElement& element) {
  const std::optional<Settings> settings = parseSettings(element);
  if (!settings.has_value()) {
    return false;
  }
  setShape(settings->shape);
  setSizeMeters(settings->size_meters);
  setSizePixels(settings->size_pixels);
  setColorType(settings->color_type);
  color_choice_explicit_ = settings->color_choice_explicit;
  // A latent ("auto") choice re-derives the colour-present RGB default the
  // smart default would have picked on the first decoded sample.
  if (!color_choice_explicit_ && has_color_) {
    color_type_ = PointColorType::kRgb;
  }
  if (settings->color_field.has_value()) {
    setColorField(*settings->color_field);
  }
  if (settings->solid_color.has_value()) {
    setSolidColor(*settings->solid_color);
  }
  setColormap(settings->colormap);
  setInvertLut(settings->invert_lut);
  setOutsideRangeOpacity(settings->outside_range_opacity);
  setOutsideRangeVisible(settings->outside_range_visible);
  setManualRange(settings->range_min, settings->range_max);
  applyAutoRange(settings->auto_range, /*seed_manual_from_world=*/false);
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
  ts_first_.reset();
  decoded_at_ns_.reset();
  invalidateSampleState();  // a late decode result must not touch a detached layer
  ctx_ = {};
  budget_.reset();
  clearGeometry();
  setDataWarning({});
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
  // Defer the heavy decode (parse + packing + GPU upload) to prepareForRender():
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
  uint64_t key =
      0x9e3779b97f4a7c15ULL ^
      PJ::latestEntryRenderKey(ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr, topic_id_, time);
  // Fixed←source transform (zero-order hold, so it is constant between TF samples —
  // that is what lets a 60 Hz playhead coalesce down to the TF update rate). Folding
  // the matrix also captures colour-by-axis recolouring, a pure function of it.
  return foldFrameTransformIntoKey(
      key, ctx_.tf_buffer.get(), fixed_frame_.toStdString(), source_frame_, geometry_stamp_, time, placementHold());
}

void PointCloudLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  if (!visible_) {
    // A hidden layer releases its retained cloud (the view drops its GPU buffer
    // too), keeping the per-view memory ceiling real; re-showing decodes once.
    // Its bounds stay: a hidden layer still counts in the camera's scene bounds.
    const std::optional<AABB> bounds = world_bounds_;
    invalidateSampleState();
    budget_.reset();
    clearGeometry();
    world_bounds_ = bounds;
  }
  emit visibilityChanged(visible);
  // Catch-up on un-hide is the dock's job: SceneDockWidget::setLayerVisible
  // re-delivers the last tracker time (hidden layers receive no ticks), which
  // marks tracker_dirty_ so the next paint decodes at the playhead.
  emit repaintRequested();
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
  shape_combo->addItem(tr("sphere"), static_cast<int>(PointShape::kSphere));
  shape_combo->addItem(tr("point"), static_cast<int>(PointShape::kPoint));
  shape_combo->addItem(tr("cube"), static_cast<int>(PointShape::kCube));
  shape_combo->setCurrentIndex(static_cast<int>(shape_));
  form->addRow(tr("Shape:"), shape_combo);

  // --- Point size (units depend on shape) ----------------------------------
  auto* size_spin = new PJ::DoubleScrubber(container);
  const auto apply_size_units = [size_spin, this]() {
    QSignalBlocker block(size_spin);
    if (shape_ == PointShape::kPoint) {
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
    if (color_type_ == PointColorType::kSolid) {
      color_type_combo->setCurrentIndex(0);
    } else if (color_type_ == PointColorType::kRgb) {
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
  for (int index = 0; index < PJ::kColormapCount; ++index) {
    colormap_combo->addItem(PJ::colormapToken(static_cast<PJ::Colormap>(index)), index);
  }
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
  color_stack->setCurrentIndex(color_type_ == PointColorType::kSolid ? 0 : color_type_ == PointColorType::kRgb ? 2 : 1);

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
    const bool gradient = color_type_ == PointColorType::kField;
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
        const auto v = static_cast<PointShape>(shape_combo->currentData().toInt());
        setShape(v);
        apply_size_units();
      });

  QObject::connect(size_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) {
    if (shape_ == PointShape::kPoint) {
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
          setColorType(PointColorType::kSolid);
          color_stack->setCurrentIndex(0);
        } else if (data == kRgbComboToken) {
          setColorType(PointColorType::kRgb);
          color_stack->setCurrentIndex(2);
        } else {
          setColorType(PointColorType::kField);
          setColorField(data);
          color_stack->setCurrentIndex(1);
          apply_range_step();
        }
        apply_range_visibility(auto_range_);
      });

  QObject::connect(color_button, &PJ::ColorPickerWidget::colorChanged, this, [this](QColor c) { setSolidColor(c); });

  QObject::connect(
      colormap_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
      [this, colormap_combo](int) { setColormap(static_cast<PJ::Colormap>(colormap_combo->currentData().toInt())); });

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
      range_min_spin, &PJ::DoubleScrubber::valueChanged, this, [range_max_spin, push_manual_range](double v) {
        if (v > range_max_spin->value()) {
          QSignalBlocker block(range_max_spin);
          range_max_spin->setValue(v);
        }
        push_manual_range();
      });
  QObject::connect(
      range_max_spin, &PJ::DoubleScrubber::valueChanged, this, [range_min_spin, push_manual_range](double v) {
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

void PointCloudLayer::setShape(PointShape shape) {
  if (shape_ == shape) {
    return;
  }
  shape_ = shape;
  emit configurationChanged();
  emit repaintRequested();
}

void PointCloudLayer::setSizeMeters(float meters) {
  if (size_meters_ == meters) {
    return;
  }
  size_meters_ = meters;
  emit configurationChanged();
  emit repaintRequested();
}

void PointCloudLayer::setSizePixels(float pixels) {
  if (size_pixels_ == pixels) {
    return;
  }
  size_pixels_ = pixels;
  emit configurationChanged();
  emit repaintRequested();
}

void PointCloudLayer::setColorType(PointColorType type) {
  const bool explicitness_changed = !color_choice_explicit_;
  color_choice_explicit_ = true;
  if (color_type_ == type) {
    if (explicitness_changed) {
      emit configurationChanged();
    }
    return;
  }
  // A change of what the vertices carry (per-point colour, the colormap scalar or
  // neither) rebuilds them; otherwise only a shader uniform flips and a repaint
  // suffices.
  const bool rgb_changed = (color_type_ == PointColorType::kRgb) != (type == PointColorType::kRgb);
  const bool stored_scalar = storesScalar();
  color_type_ = type;
  const bool repack = rgb_changed || storesScalar() != stored_scalar;
  emit configurationChanged();
  if (repack) {
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
  emit configurationChanged();
  emit repaintRequested();
}

void PointCloudLayer::setColormap(PJ::Colormap cm) {
  if (colormap_ == cm) {
    return;
  }
  colormap_ = cm;
  emit configurationChanged();
  emit repaintRequested();
}

void PointCloudLayer::setInvertLut(bool invert) {
  if (invert_lut_ == invert) {
    return;
  }
  invert_lut_ = invert;
  emit configurationChanged();
  emit repaintRequested();
}

void PointCloudLayer::setOutsideRangeOpacity(float opacity) {
  const float clamped = std::clamp(opacity, 0.0f, 1.0f);
  if (outside_range_opacity_ == clamped) {
    return;
  }
  outside_range_opacity_ = clamped;
  emit configurationChanged();
  emit repaintRequested();
}

void PointCloudLayer::setOutsideRangeVisible(bool visible) {
  if (outside_range_visible_ == visible) {
    return;
  }
  outside_range_visible_ = visible;
  emit configurationChanged();
  emit repaintRequested();
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
  // The view reports the range it actually coloured with (world-axis or scalar),
  // so seeding from it freezes exactly the colours on screen.
  if (seed_manual_from_world) {
    manual_range_min_ = last_rendered_scalar_range_.first;
    manual_range_max_ = last_rendered_scalar_range_.second;
  }
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
  shiftStampToDatasetClock(obj->object, first->payload_stamp_shift);
  // Compressed topic (Draco / Cloudini): the wrapper gives frame_id without a
  // decode, but the field list needs one. Decode the first sample asynchronously so
  // attach never blocks the UI — fields populate when the result lands.
  if (const auto* cpc = obj->object.get<CompressedPointCloud>()) {
    requestDecode(*cpc, SampleId{first->sequential_uid, first->timestamp});
    return true;
  }

  const auto* sdk_cloud = obj->object.get<PointCloud>();
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
    color_type_ = PointColorType::kRgb;
  } else if (color_type_ == PointColorType::kRgb && !has_color_) {
    // A restored/explicit RGB mode but this cloud has no colour: degrade to a scalar.
    color_type_ = PointColorType::kField;
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
  // The same placement the render pass applies.
  const auto tf = placeObject(
      *ctx_.tf_buffer, fixed_frame_.toStdString(), geometry_frame_, geometry_stamp_, *decoded_at_ns_, placementHold());
  if (!tf) {
    return std::nullopt;
  }
  return transformedAabbAxisRange(*world_bounds_, glm::mat4(tf->matrix()), axis);
}

void PointCloudLayer::pushCloud(const PointCloud& cloud, SampleId id, [[maybe_unused]] bool rederive) {
  updateSourceFrame(cloud.frame_id);
  if (available_color_fields_.isEmpty() && !cloud.fields.empty()) {
    // Self-heal after a failed bootstrap (topic attached before its first sample):
    // the first cloud that reaches the GPU also reveals the field set.
    populateColorFields(cloud);
  }
  // RGB-direct mode reads per-point colour from the cloud's colour field instead of a colormap
  // scalar. Fixed-frame (x/y/z) colouring derives colour from the GPU-transformed position
  // (scalarAxis()), so there is no raw scalar to decode for it.
  const bool rgb_mode = color_type_ == PointColorType::kRgb && has_color_;
  const int axis = rgb_mode ? -1 : spatialAxisIndex(color_field_);
  const std::string_view scalar_sv = (rgb_mode || axis >= 0) ? std::string_view{} : std::string_view{color_field_};
  const std::size_t point_count = static_cast<std::size_t>(cloud.width) * static_cast<std::size_t>(cloud.height);
  if (const QString rejection = cloudAdmissionError(cloud); !rejection.isEmpty()) {
    failSample(id, rejection);
    return;
  }
  const std::optional<PointVertexPlan> plan =
      planPointVertices(cloud, scalar_sv, /*extract_rgba=*/rgb_mode, storesScalar());
  // A cloud with no point is a valid empty sample (D25: an empty result, or a derived topic's tombstone): it
  // draws nothing and warns about nothing.
  if (!plan.has_value()) {
    failSample(id, point_count != 0 ? tr("Malformed point cloud was rejected") : QString());
    return;
  }
  PackedPointVertices packed;
  try {
    packIntoRetained(
        vertex_bytes_, static_cast<qsizetype>(plan->maxBytes()), [&](std::span<std::byte> out, bool reused) {
          packed =
              packPointVertices(cloud, *plan, out, parallelForOnPool, reused ? kMaxPackChunks : kMaxFreshPackChunks);
          return static_cast<qsizetype>(packed.count * pointVertexStride(packed.format));
        });
  } catch (const std::bad_alloc&) {
    failSample(id, tr("Not enough memory to convert this point cloud"));
    return;
  }
  if (packed.count == 0) {
    failSample(id, point_count != 0 ? tr("Point cloud contains no finite positions") : QString());
    return;
  }
  vertex_format_ = packed.format;
  point_count_ = packed.count;
  if (!rederive) {
    ++geometry_revision_;
    if (id != last_pushed_id_) {
      streaming_ = last_sample_push_.isValid() && last_sample_push_.elapsed() < kStreamingWindow.count();
      last_sample_push_.start();
    }
  }
  world_bounds_ = packed.bounds.valid ? std::optional<AABB>{packed.bounds} : std::nullopt;
  automatic_scalar_range_ = packed.scalar_range
                                ? clampScalarRange(packed.scalar_range->first, packed.scalar_range->second)
                                : std::pair<float, float>{0.0f, 1.0f};
  range_dirty_ = false;
  setDataWarning({});
  last_pushed_id_ = id;
  geometry_frame_ = cloud.frame_id;
  geometry_stamp_ = objectStamp(cloud.timestamp_ns, /*payload_stamp_shift=*/0);  // shifted at decode
  last_pushed_color_field_ = color_field_;
  last_pushed_rgb_ = rgb_mode;
  last_pushed_scalar_ = storesScalar();
  last_pushed_axis_ = axis;
  if (rederive) {
    return;  // the frame that asked for it is being prepared
  }
  emit repaintRequested();
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
      (color_type_ == PointColorType::kRgb) == last_pushed_rgb_ && storesScalar() == last_pushed_scalar_ &&
      !(auto_range_ && range_dirty_)) {
    updateSourceFrame(geometry_frame_);  // back from a pending sample in another frame
    return true;
  }
  // Compressed sample already decoded? Re-convert from cache so repaints /
  // color-field changes don't re-run the codec (or even the wrapper parse).
  if (decoded_cache_ && decoded_cache_id_ == id) {
    pushCloud(*decoded_cache_, id);
    return true;
  }
  // Known-undecodable sample; the memo holds until a reload swaps the bytes.
  if (id != failed_id_) {
    return false;
  }
  // Fail closed on a revisit too: never leave another sample's cloud on screen.
  clearGeometry();
  setDataWarning(failed_warning_);
  return true;
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
    // A seek before the shown cloud's stamp: nothing is there (a derived topic computed only later). At or after
    // it the shown entry was evicted under the cursor, and stays.
    if (last_pushed_id_ != SampleId{} && time_ns < last_pushed_id_.timestamp) {
      invalidateSampleState();
      clearGeometry();
      emit repaintRequested();
    }
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
    failSample(id, tr("Point cloud could not be decoded: %1").arg(QString::fromStdString(obj.error())));
    return;
  }
  shiftStampToDatasetClock(obj->object, resolved->payload_stamp_shift);

  // The mode is derived per-sample rather than latched at bootstrap, so a topic
  // whose first sample arrives only after attach (failed bootstrap) still renders.
  if (const auto* cpc = obj->object.get<CompressedPointCloud>()) {
    requestDecode(*cpc, id);  // off the UI thread; pushes when ready
    return;
  }

  const auto* sdk_cloud = obj->object.get<PointCloud>();
  if (sdk_cloud == nullptr) {
    failSample(id, tr("Point-cloud sample has an unexpected canonical type"));
    return;
  }
  pushCloud(*sdk_cloud, id);
}

void PointCloudLayer::requestDecode(const CompressedPointCloud& cloud, SampleId id) {
  updateSourceFrame(cloud.frame_id);
  wanted_ = id;  // a late decode of any other sample must not be painted
  // Admission before decompression: an over-limit blob never reaches the codec.
  if (static_cast<uint64_t>(cloud.data.size()) > kSceneLimits.max_point_wire_bytes_per_cloud) {
    failSample(
        id, tr("Compressed cloud payload is %1 MiB; the limit is %2 MiB")
                .arg(QString::number(static_cast<double>(cloud.data.size()) / (1024.0 * 1024.0), 'f', 1))
                .arg(kSceneLimits.max_point_wire_bytes_per_cloud / (1024ULL * 1024ULL)));
    return;
  }
  if (const PendingDecode* running = decode_worker_.running(); running != nullptr && running->id == id) {
    decode_worker_.cancelWaiting();  // the running decode is wanted again
    return;
  }
  // The copied wrapper's shared anchor keeps the compressed bytes alive on the
  // worker. This requires the anchored bytes to be IMMUTABLE, not merely alive:
  // every in-tree producer either deep-copies (canonical codec) or anchors a const
  // ObjectStore buffer, but a zero-copy parser reusing a scratch buffer would race.
  decode_worker_.request(PendingDecode{cloud, id});
}

PointCloudLayer::DecodeResult PointCloudLayer::decodeCompressed(const PendingDecode& request) {
  DecodeResult result;
  result.id = request.id;
  // Exception barrier: decodeCompressedPointCloud is barriered internally, but the
  // residue here (shared_ptr control block, error-string copy) can still throw
  // under memory pressure; module policy forbids worker exceptions reaching the
  // GUI thread.
  try {
#if defined(PJ_TARGET_WASM) && !defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
    // This browser build carries no codecs (the dock never offers the type).
    const PJ::Expected<PointCloud> decoded = PJ::unexpected("compressed point clouds unsupported");
#else
    // The limits reach the codec, which rejects an over-limit cloud before
    // allocating its decoded buffer.
    auto decoded = decodeCompressedPointCloud(
        request.cloud, PointCloudDecodeLimits{
                           .max_points = kSceneLimits.max_points_per_cloud,
                           .max_decoded_bytes = kSceneLimits.max_point_wire_bytes_per_cloud,
                       });
#endif
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
}

void PointCloudLayer::onDecodeFinished(DecodeResult result) {
  // Paint this result only if it's still the sample the tracker wants. If the user
  // scrubbed away while it decoded — even back onto a cached frame — it's stale: cache
  // it (cheap, useful on scrub-back) but don't paint it over the live frame. (A
  // result invalidated by a reload or detach never reaches here.)
  const bool is_current = result.id == wanted_;
  // Never cache an over-limit decompression: it would stay pinned in memory.
  const QString rejection = result.cloud ? cloudAdmissionError(*result.cloud) : QString{};
  if (!rejection.isEmpty()) {
    result.cloud.reset();
  }
  if (result.cloud) {
    decoded_cache_ = result.cloud;
    decoded_cache_id_ = result.id;
    if (available_color_fields_.isEmpty()) {
      populateColorFields(*result.cloud);  // first successful decode reveals the field set
    }
    if (is_current) {
      pushCloud(*result.cloud, result.id);
    }
    return;
  }
  // Memoized: renderAt won't re-request a sample that can never decode.
  failed_id_ = result.id;
  failed_warning_ =
      rejection.isEmpty() ? tr("Compressed point cloud could not be decoded: %1").arg(result.error) : rejection;
  if (is_current) {
    qCWarning(lcPointCloudLayer) << "compressed point cloud rejected:" << failed_warning_;
    failSample(result.id, failed_warning_);
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
  invalidateSampleState();
  budget_.reset();
  clearGeometry();
  setDataWarning({});
  // The signal precedes the swap and replaceDataset runs no event loop, so the
  // repaint scheduled here paints after the new bytes are in place.
  tracker_dirty_ = true;
  emit repaintRequested();
}

void PointCloudLayer::invalidateSampleState() {
  decoded_cache_.reset();
  decoded_cache_id_ = {};
  failed_id_ = {};
  failed_warning_.clear();
  last_pushed_id_ = {};
  last_pushed_color_field_.clear();
  wanted_ = {};
  decode_worker_.invalidate();  // queued/running decodes hold content nobody wants any more
}

std::uint64_t PointCloudLayer::budgetCandidate() const {
  // The sample refreshNow() would draw; no setting changes a cloud's point count.
  const PJ::ObjectStore* store = ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr;
  if (decoded_at_ns_.has_value()) {
    return PJ::latestEntryRenderKey(store, topic_id_, *decoded_at_ns_);
  }
  return ts_first_.has_value() ? PJ::latestEntryRenderKey(store, topic_id_, PJ::fromRaw(*ts_first_))
                               : PJ::kNoSampleRenderKey;
}

void PointCloudLayer::drainPendingUpdate() {
  // renderAt's own SampleId guard makes this cheap when the active sample is
  // unchanged; refreshNow() decodes at the latest tracker time.
  if (visible_ && tracker_dirty_) {
    tracker_dirty_ = false;
    refreshNow();
  }
}

bool PointCloudLayer::prepareForRender(std::uint64_t remaining_view_vertices) {
  const std::uint64_t revision_before = geometry_revision_;
  bool released = false;
  if (budget_.blocked(remaining_view_vertices, [this] { return budgetCandidate(); }, released)) {
    return false;
  }
  if (released) {
    tracker_dirty_ = true;
  }
  drainPendingUpdate();
  if (static_cast<std::uint64_t>(pointCount()) > remaining_view_vertices) {
    budget_.reject(pointCount(), last_pushed_id_.uid.value);
    setDataWarning(tr("Layer has %1 finite points; only %2 remain in this view's budget")
                       .arg(QString::number(pointCount()), QString::number(remaining_view_vertices)));
    clearGeometry();
    last_pushed_id_ = {};  // re-push once the budget frees up
  }
  return geometry_revision_ != revision_before;
}

void PointCloudLayer::onVerticesUploaded(std::uint64_t revision) {
  uploaded_revision_ = revision;
  if (revision != geometry_revision_ || vertex_bytes_.isEmpty()) {
    return;
  }
  if (streaming_) {
    last_stream_upload_.start();  // the timer fires once the stream is quiet
    if (!evict_timer_.isActive()) {
      evict_timer_.start(kStreamingWindow);
    }
  } else {
    evictVertices();
  }
}

void PointCloudLayer::evictVertices() {
  if (vertex_bytes_.isEmpty() || point_count_ == 0) {
    return;
  }
  // Only our reference: an upload batch still holding the array keeps it alive
  // until its frame is submitted.
  vertex_bytes_ = QByteArray();
}

bool PointCloudLayer::ensureVertexBytes() {
  if (!evicted()) {
    return true;
  }
  rederivePushedSample();
  return point_count_ > 0 && !evicted();
}

void PointCloudLayer::rederivePushedSample() {
  const SampleId id = last_pushed_id_;
  if (decoded_cache_ && decoded_cache_id_ == id) {
    pushCloud(*decoded_cache_, id, /*rederive=*/true);
    return;
  }
  std::optional<PJ::ResolvedObjectEntry> resolved;
  if (ctx_.session != nullptr) {
    resolved = ctx_.session->objectStore().at(topic_id_, id.uid);
  }
  if (!resolved.has_value() || resolved->payload.bytes.empty()) {
    // Retention dropped the sample since: the playhead's sample replaces it.
    clearGeometry();
    last_pushed_id_ = {};
    tracker_dirty_ = true;
    emit repaintRequested();
    return;
  }
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  auto obj = resolveObject(binding, object_type_, resolved->timestamp, resolved->payload);
  if (!obj.has_value()) {
    failSample(id, tr("Point cloud could not be decoded: %1").arg(QString::fromStdString(obj.error())));
    return;
  }
  shiftStampToDatasetClock(obj->object, resolved->payload_stamp_shift);
  if (const auto* cpc = obj->object.get<CompressedPointCloud>()) {
    // The decode cache holds another sample: decode off the UI thread, and the
    // cloud reappears when it lands (unless the tracker already wants another).
    clearGeometry();
    last_pushed_id_ = {};
    if (wanted_ == id) {
      requestDecode(*cpc, id);
    }
    return;
  }
  const auto* sdk_cloud = obj->object.get<PointCloud>();
  if (sdk_cloud == nullptr) {
    failSample(id, tr("Point-cloud sample has an unexpected canonical type"));
    return;
  }
  pushCloud(*sdk_cloud, id, /*rederive=*/true);
}

bool PointCloudLayer::storesScalar() const {
  return color_type_ == PointColorType::kField && spatialAxisIndex(color_field_) < 0;
}

int PointCloudLayer::scalarAxis() const {
  return color_type_ == PointColorType::kField ? spatialAxisIndex(color_field_) : -1;
}

std::pair<float, float> PointCloudLayer::scalarRange(const glm::mat4& fixed_from_source) const {
  if (!auto_range_) {
    return {manual_range_min_, manual_range_max_};
  }
  const int axis = scalarAxis();
  if (axis < 0) {
    return automatic_scalar_range_;
  }
  return world_bounds_ ? transformedAabbAxisRange(*world_bounds_, fixed_from_source, axis)
                       : std::pair<float, float>{0.0f, 1.0f};
}

void PointCloudLayer::noteRenderedScalarRange(float minimum, float maximum) {
  if (!auto_range_ || !std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum ||
      last_rendered_scalar_range_ == std::pair<float, float>{minimum, maximum}) {
    return;
  }
  last_rendered_scalar_range_ = {minimum, maximum};
  emit autoRangeComputed(minimum, maximum);
}

void PointCloudLayer::failSample(SampleId id, const QString& warning) {
  failed_id_ = id;
  failed_warning_ = warning;
  if (decoded_cache_id_ == id) {
    decoded_cache_.reset();
    decoded_cache_id_ = {};
  }
  last_pushed_id_ = {};
  last_pushed_color_field_.clear();
  clearGeometry();
  setDataWarning(warning);
  emit repaintRequested();
}

void PointCloudLayer::clearGeometry() {
  if (vertex_bytes_.isEmpty() && point_count_ == 0 && !world_bounds_.has_value()) {
    return;
  }
  vertex_bytes_ = QByteArray();  // clear() would keep the capacity pinned (~20 MiB per million points)
  point_count_ = 0;
  world_bounds_.reset();
  ++geometry_revision_;
}

}  // namespace pj::scene3d
