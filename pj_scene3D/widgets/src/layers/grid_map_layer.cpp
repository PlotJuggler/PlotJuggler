// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_widgets/layers/grid_map_layer.h"

#include <QDomElement>
#include <QFormLayout>
#include <QLoggingCategory>
#include <QPointer>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <any>
#include <cmath>
#include <glm/glm.hpp>
#include <limits>
#include <optional>
#include <utility>

#include "layer_xml_validation.h"
#include "pj_base/builtin/grid_map_codec.hpp"    // validateGridMap
#include "pj_base/time.hpp"                      // PJ::toRaw
#include "pj_scene3d_core/pointcloud_convert.h"  // findField
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/resolve_object.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/ToggleSwitch.h"
using namespace Qt::StringLiterals;

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcGridMap, "pj.scene3d.grid_map")

// The scalar field named `name`, or nullptr when absent or not single-element.
const PJ::sdk::PointField* scalarField(const std::vector<PJ::sdk::PointField>& fields, const std::string& name) {
  const PJ::sdk::PointField* field = name.empty() ? nullptr : findField(fields, name);
  return field != nullptr && isScalarGridMapField(*field) ? field : nullptr;
}

// The manual colormap range the pass can store: finite, float-representable,
// ordered. The setter normalizes to this; xmlLoadState rejects what violates it.
constexpr double kManualRangeLimit = std::numeric_limits<float>::max();

bool isValidManualRange(double lo, double hi) {
  return std::isfinite(lo) && std::isfinite(hi) && std::abs(lo) <= kManualRangeLimit &&
         std::abs(hi) <= kManualRangeLimit && lo <= hi;
}

bool sameField(const PJ::sdk::PointField& a, const PJ::sdk::PointField& b) {
  return a.name == b.name && a.offset == b.offset && a.datatype == b.datatype && a.count == b.count;
}
}  // namespace

GridMapLayer::GridMapLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : Scene3DLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)) {}

GridMapLayer::~GridMapLayer() = default;

PJ::SceneLayerInfo GridMapLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kGridMap,
      .display_name = display_name_,
      .family_name = u"GridMap"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> GridMapLayer::timeRange() const {
  const auto* store = ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr;
  return PJ::liveTopicTimeRange(store, topic_id_);
}

QStringList GridMapLayer::fallbackFrames() const {
  if (source_frame_.empty()) {
    return {};
  }
  return {QString::fromStdString(source_frame_)};
}

QString GridMapLayer::sourceFrame() const {
  return QString::fromStdString(source_frame_);
}

uint64_t GridMapLayer::renderKey(PJ::Timepoint time) const {
  // Sample identity by SequentialUID from latestEntryIdAt() (a binary search that
  // never resolves payload bytes) folded with the fixed<-source transform, the
  // same recipe as PointCloudLayer::renderKey.
  uint64_t key = 0x9e3779b97f4a7c15ULL;
  if (ctx_.session != nullptr) {
    PJ::ObjectStore& store = ctx_.session->objectStore();
    if (const auto entry_id = store.latestEntryIdAt(topic_id_, PJ::toRaw(time)); entry_id.has_value()) {
      key ^= entry_id->uid.value;
    } else {
      key ^= PJ::kNoSampleRenderKey;
    }
  }
  if (ctx_.tf_buffer != nullptr && !source_frame_.empty()) {
    if (const auto tf = ctx_.tf_buffer->tryLookupTransform(fixed_frame_.toStdString(), source_frame_, time); tf) {
      key = foldTransformIntoKey(key, glm::mat4(tf->matrix()));
    }
  }
  return key;
}

QDomElement GridMapLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(u"grid_map"_s);
  el.setAttribute(u"elevation_field"_s, QString::fromStdString(elevation_request_));
  el.setAttribute(u"color_field"_s, QString::fromStdString(color_request_));
  el.setAttribute(u"colormap"_s, static_cast<int>(colormap_));
  el.setAttribute(u"auto_range"_s, auto_range_ ? 1 : 0);
  el.setAttribute(u"range_lo"_s, manual_lo_);
  el.setAttribute(u"range_hi"_s, manual_hi_);
  el.setAttribute(u"lighting"_s, lighting_ ? 1 : 0);
  el.setAttribute(u"opacity"_s, opacity_);
  return el;
}

bool GridMapLayer::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "grid_map"_L1 || !detail::isLeafPayload(element)) {
    return false;
  }
  int colormap = 0;
  bool auto_range = true;
  double range_lo = 0.0;
  double range_hi = 1.0;
  bool lighting = true;
  double opacity = 1.0;
  if (!detail::parseBoundedInt(element, "colormap", 0, 0, PJ::kColormapCount - 1, colormap) ||
      !detail::parseZeroOne(element, "auto_range", true, auto_range) ||
      !detail::parseFiniteDouble(element, "range_lo", 0.0, -kManualRangeLimit, kManualRangeLimit, range_lo) ||
      !detail::parseFiniteDouble(element, "range_hi", 1.0, -kManualRangeLimit, kManualRangeLimit, range_hi) ||
      !isValidManualRange(range_lo, range_hi) || !detail::parseZeroOne(element, "lighting", true, lighting) ||
      !detail::parseFiniteDouble(element, "opacity", 1.0, 0.0, 1.0, opacity)) {
    return false;
  }
  // Requests are restored verbatim: a missing field falls back at render time and
  // the request is honoured again once a sample carrying it arrives.
  setElevationField(element.attribute(u"elevation_field"_s));
  setColorField(element.attribute(u"color_field"_s));
  setColormap(static_cast<PJ::Colormap>(colormap));
  setAutoRange(auto_range);
  setManualRange(range_lo, range_hi);
  setLighting(lighting);
  setOpacity(opacity);
  pushDisplayParamsToPass();
  return true;
}

bool GridMapLayer::attach(const PJ::SceneLayerContext& ctx) {
  const auto& scene3d_ctx = static_cast<const Scene3DLayerContext&>(ctx);
  if (scene3d_ctx.session == nullptr) {
    qCWarning(lcGridMap) << "attach: session is null";
    return false;
  }
  ctx_ = scene3d_ctx;
  // A parser (file / streaming sources) OR the canonical PJ.GridMap codec (a
  // toolbox that pushed serialized canonical blobs) can decode the topic.
  if (!ctx_.session->parserBindingForObjectTopic(topic_id_) &&
      !hasCanonical3DCodec(PJ::sdk::BuiltinObjectType::kGridMap)) {
    qCWarning(lcGridMap) << "attach: no parser and no canonical codec for topic" << topic_id_.id;
    return false;
  }
  resetStreamingState();
  pushDisplayParamsToPass();
  disconnect(reload_connection_);
  reload_connection_ = connect(
      ctx_.session, &PJ::SessionManager::datasetAboutToBeReplaced, this, &GridMapLayer::onDatasetAboutToBeReplaced);
  // Streaming-tolerant: the topic may have no sample yet; renderAt self-heals.
  if (!bootstrap()) {
    qCWarning(lcGridMap) << "attach: bootstrap deferred for grid-map topic" << topic_id_.id;
  }
  return true;
}

void GridMapLayer::detach() {
  disconnect(reload_connection_);
  reload_connection_ = {};
  pass_.clearGrid();
  resetStreamingState();
  ctx_ = {};
}

void GridMapLayer::resetStreamingState() {
  cached_grid_.reset();  // before the keepalive: its anchor may live in the plugin DSO
  cached_keepalive_.reset();
  cached_grid_uid_ = {};
  bounds_.reset();
  uploaded_uid_ = {};
  source_frame_.clear();  // re-announced by renderAt from the next decoded sample
  field_names_.clear();
  fields_memo_.clear();
  resolved_requests_.reset();
  resolved_elevation_.clear();
  resolved_color_.clear();
  resolve_warning_.clear();
  last_auto_range_ = {};
  setStatusWarning(QString());
}

void GridMapLayer::onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id) {
  if (ctx_.session == nullptr || ctx_.session->objectStore().descriptor(topic_id_).dataset_id != dataset_id) {
    return;
  }
  // The topic id survives the swap but every UID is minted fresh: a stale memo
  // must never serve the old bytes.
  pass_.clearGrid();
  resetStreamingState();
  tracker_dirty_ = true;
  emit repaintRequested();
}

bool GridMapLayer::bootstrap() {
  PJ::ObjectStore& store = ctx_.session->objectStore();
  auto first = store.at(topic_id_, 0);
  if (!first.has_value() || first->payload.bytes.empty()) {
    return false;
  }
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  auto obj = resolveObject(binding, PJ::sdk::BuiltinObjectType::kGridMap, first->timestamp, first->payload);
  if (!obj.has_value()) {
    qCWarning(lcGridMap) << "bootstrap: decode failed:" << QString::fromStdString(obj.error());
    return false;
  }
  const auto* grid = std::any_cast<PJ::sdk::GridMap>(&obj->object);
  if (grid == nullptr) {
    return false;
  }
  source_frame_ = grid->frame_id;
  resolveFields(*grid);  // pre-warm the field list + effective picks for the config UI
  if (!source_frame_.empty()) {
    emit sourceFrameChanged(QString::fromStdString(source_frame_));
    emit fallbackFramesChanged(fallbackFrames());
  }
  return true;
}

bool GridMapLayer::resolveFields(const PJ::sdk::GridMap& grid) {
  if (std::ranges::equal(grid.fields, fields_memo_, sameField) &&
      resolved_requests_ == std::pair{elevation_request_, color_request_}) {
    setStatusWarning(resolve_warning_);  // a decode warning may have replaced it since
    return !resolved_color_.empty();
  }
  fields_memo_ = grid.fields;
  resolved_requests_ = std::pair{elevation_request_, color_request_};
  field_names_.clear();
  for (const auto& field : grid.fields) {
    if (isScalarGridMapField(field)) {
      field_names_.push_back(field.name);
    }
  }
  QStringList warnings;

  const PJ::sdk::PointField* elevation = nullptr;  // stays null for kGridMapElevationNone
  if (elevation_request_.empty()) {
    elevation = defaultElevationField(grid.fields);
  } else if (elevation_request_ != kGridMapElevationNone) {
    elevation = scalarField(grid.fields, elevation_request_);
    if (elevation == nullptr) {
      elevation = defaultElevationField(grid.fields);
      warnings << tr("Elevation field '%1' not found; using %2")
                      .arg(
                          QString::fromStdString(elevation_request_),
                          elevation != nullptr ? QString::fromStdString(elevation->name) : tr("a flat plane"));
    }
  }
  resolved_elevation_ = elevation != nullptr ? elevation->name : std::string();

  const PJ::sdk::PointField* color = scalarField(grid.fields, color_request_);
  if (color == nullptr && !color_request_.empty()) {
    warnings << tr("Color field '%1' not found").arg(QString::fromStdString(color_request_));
  }
  if (color == nullptr) {
    color = elevation;
  }
  if (color == nullptr && !field_names_.empty()) {
    color = findField(grid.fields, field_names_.front());
  }
  resolved_color_ = color != nullptr ? color->name : std::string();
  if (color == nullptr) {
    warnings << tr("Grid map has no scalar field to display");
  }
  resolve_warning_ = warnings.join(u"; "_s);
  setStatusWarning(resolve_warning_);
  emit fieldsChanged();
  return color != nullptr;
}

void GridMapLayer::renderAt(int64_t time_ns) {
  if (ctx_.session == nullptr) {
    return;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  // Metadata-first gate: the same sample is already staged with the current
  // selectors (the setters forget the memo), so nothing is resolved, decoded,
  // packed or uploaded (the scrub path).
  const auto entry_id = store.latestEntryIdAt(topic_id_, time_ns);
  if (entry_id.has_value() && entry_id->uid == uploaded_uid_) {
    return;
  }
  auto entry = entry_id.has_value() ? store.latestAt(topic_id_, time_ns) : std::nullopt;
  if (!entry.has_value() || entry->payload.bytes.empty()) {
    // Before the first sample (or in a gap): clear AND forget the upload memo, so a
    // back-scrub onto the same entry re-stages instead of hitting the fast path.
    pass_.clearGrid();
    uploaded_uid_ = {};
    setBounds(std::nullopt);
    setStatusWarning(QString());  // the resolve memo keeps its own warning for the back-scrub
    return;
  }
  if (entry->sequential_uid == uploaded_uid_) {
    return;  // a racing insert changed the pick between the two queries
  }
  const auto remember_upload = [this, uid = entry->sequential_uid]() { uploaded_uid_ = uid; };
  // A refused sample is remembered like a staged one so it is not retried every tick.
  const auto bail = [&](const QString& why) {
    setStatusWarning(why);
    pass_.clearGrid();
    setBounds(std::nullopt);
    remember_upload();
  };

  if (!cached_grid_.has_value() || entry->sequential_uid != cached_grid_uid_) {
    const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
    auto obj = resolveObject(binding, PJ::sdk::BuiltinObjectType::kGridMap, entry->timestamp, entry->payload);
    if (!obj.has_value()) {
      qCWarning(lcGridMap) << "renderAt: decode failed:" << QString::fromStdString(obj.error());
      bail(tr("Grid map decode failed: %1").arg(QString::fromStdString(obj.error())));
      return;
    }
    auto* parsed = std::any_cast<PJ::sdk::GridMap>(&obj->object);
    if (parsed == nullptr) {
      bail(tr("Grid map decode returned an object of another type"));
      return;
    }
    cached_grid_ = std::move(*parsed);      // carries the anchor: bytes stay alive past the call
    cached_keepalive_ = binding.keepalive;  // pins the DSO that owns that anchor (null on the canonical route)
    cached_grid_uid_ = entry->sequential_uid;
  }
  const PJ::sdk::GridMap& grid = *cached_grid_;

  if (grid.frame_id != source_frame_) {
    source_frame_ = grid.frame_id;
    emit sourceFrameChanged(QString::fromStdString(source_frame_));
    emit fallbackFramesChanged(fallbackFrames());
  }

  // Wire dims are untrusted until validated: the host splice path does not yet
  // validate attached bytes (SDK follow-up), so every consumer checks here.
  if (const auto valid = PJ::validateGridMap(grid); !valid) {
    bail(tr("Invalid grid map: %1").arg(QString::fromStdString(valid.error())));
    return;
  }
  if (const uint64_t cells = gridMapCellCount(grid); cells > kMaxRenderableGridMapCells) {
    bail(tr("Grid map too large to display: %1 cells").arg(cells));
    return;
  }
  if (!resolveFields(grid)) {
    bail(status_warning_);  // resolveFields already set the warning
    return;
  }

  GridMapUpload upload;
  upload.frame_id = grid.frame_id;
  upload.origin = grid.origin;
  upload.cell_size = glm::vec2(static_cast<float>(grid.cell_size.x), static_cast<float>(grid.cell_size.y));
  upload.column_count = grid.column_count;
  upload.row_count = grid.row_count;
  FiniteRange elevation_range;
  if (!resolved_elevation_.empty()) {
    upload.elevation = packGridMapField(grid, *findField(grid.fields, resolved_elevation_), &elevation_range);
  }
  upload.color_is_elevation = !resolved_elevation_.empty() && resolved_color_ == resolved_elevation_;
  if (upload.color_is_elevation) {
    last_auto_range_ = elevation_range;
  } else {
    upload.color = packGridMapField(grid, *findField(grid.fields, resolved_color_), &last_auto_range_);
  }
  upload.color_range = last_auto_range_;
  setBounds(gridMapBounds(grid, resolved_elevation_.empty() ? std::nullopt : std::optional{elevation_range}));
  pass_.setGrid(std::move(upload));
  ++pack_count_;
  remember_upload();
}

void GridMapLayer::setFixedFrame(const QString& frame) {
  if (fixed_frame_ == frame) {
    return;
  }
  fixed_frame_ = frame;  // placement is resolved per frame via FrameContext
  emit repaintRequested();
}

void GridMapLayer::setStatusWarning(const QString& reason) {
  if (status_warning_ == reason) {
    return;
  }
  status_warning_ = reason;
  emit statusWarningChanged();
}

void GridMapLayer::setBounds(std::optional<AABB> bounds) {
  if (bounds.has_value() && !bounds->valid) {
    bounds.reset();
  }
  if (bounds == bounds_) {
    return;
  }
  bounds_ = bounds;
  emit repaintRequested();  // re-fits the camera via Scene3DDockWidget::updateSceneBounds
}

void GridMapLayer::setTrackerTime(PJ::Timepoint time) {
  Q_UNUSED(time);  // render() reads frame_ctx.time via the tracker_dirty_ path
  tracker_dirty_ = true;
  emit repaintRequested();
}

void GridMapLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  pass_.setVisible(visible);
  emit visibilityChanged(visible);
  emit repaintRequested();
}

void GridMapLayer::initializeGL() {
  pass_.initializeGL();
}

void GridMapLayer::releaseGL() {
  pass_.releaseGL();
}

void GridMapLayer::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!visible_) {
    return;
  }
  if (tracker_dirty_) {
    tracker_dirty_ = false;
    renderAt(PJ::toRaw(frame_ctx.time));
  }
  pass_.render(view_params, frame_ctx);
  if (const auto& why = pass_.rejection(); why.has_value()) {
    setStatusWarning(*why);  // a GL-limit verdict, known only once the upload was attempted
    setBounds(std::nullopt);
  }
}

std::optional<AABB> GridMapLayer::worldBounds() const {
  if (!bounds_.has_value() || !bounds_->valid) {
    return std::nullopt;
  }
  return bounds_;
}

void GridMapLayer::pushDisplayParamsToPass() {
  pass_.setAutoRange(auto_range_);
  pass_.setManualRange(static_cast<float>(manual_lo_), static_cast<float>(manual_hi_));
  pass_.setColormap(colormap_);
  pass_.setLighting(lighting_);
  pass_.setOpacity(static_cast<float>(opacity_));
  pass_.setVisible(visible_);
}

void GridMapLayer::setElevationField(const QString& field_name) {
  const std::string next = field_name.toStdString();
  if (elevation_request_ == next) {
    return;
  }
  elevation_request_ = next;
  uploaded_uid_ = {};  // re-pack with the new selection on the next render
  tracker_dirty_ = true;
  emit configurationChanged();
  emit repaintRequested();
}

void GridMapLayer::setColorField(const QString& field_name) {
  const std::string next = field_name.toStdString();
  if (color_request_ == next) {
    return;
  }
  color_request_ = next;
  uploaded_uid_ = {};
  tracker_dirty_ = true;
  emit configurationChanged();
  emit repaintRequested();
}

void GridMapLayer::setColormap(PJ::Colormap colormap) {
  if (colormap_ == colormap) {
    return;
  }
  colormap_ = colormap;
  pass_.setColormap(colormap);
  emit configurationChanged();
  emit repaintRequested();
}

void GridMapLayer::setAutoRange(bool on) {
  if (auto_range_ == on) {
    return;
  }
  auto_range_ = on;
  pass_.setAutoRange(on);
  emit configurationChanged();
  emit repaintRequested();
}

void GridMapLayer::setManualRange(double lo, double hi) {
  if (!std::isfinite(lo) || !std::isfinite(hi)) {
    return;
  }
  lo = std::clamp(lo, -kManualRangeLimit, kManualRangeLimit);
  hi = std::clamp(hi, -kManualRangeLimit, kManualRangeLimit);
  if (lo > hi) {
    std::swap(lo, hi);
  }
  if (manual_lo_ == lo && manual_hi_ == hi) {
    return;
  }
  manual_lo_ = lo;
  manual_hi_ = hi;
  pass_.setManualRange(static_cast<float>(lo), static_cast<float>(hi));
  emit configurationChanged();
  emit repaintRequested();
}

void GridMapLayer::setLighting(bool on) {
  if (lighting_ == on) {
    return;
  }
  lighting_ = on;
  pass_.setLighting(on);
  emit configurationChanged();
  emit repaintRequested();
}

void GridMapLayer::setOpacity(double opacity) {
  const double clamped = std::clamp(opacity, 0.0, 1.0);
  if (opacity_ == clamped) {
    return;
  }
  opacity_ = clamped;
  pass_.setOpacity(static_cast<float>(opacity_));
  emit configurationChanged();
  emit repaintRequested();
}

QWidget* GridMapLayer::createConfigWidget(QWidget* parent) {
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

  // Selectors list the last decoded grid's scalar fields; the current item is the
  // EFFECTIVE pick, so a fallen-back request shows what is actually drawn. They
  // rebuild (silently: not a user edit) whenever a decoded sample changes either.
  const auto add_field_combo = [this, container, form](
                                   const QString& label, const QString& object_name, const std::string* current,
                                   void (GridMapLayer::*setter)(const QString&), bool with_none) {
    auto* combo = new PJ::ComboBox(container);
    combo->setObjectName(object_name);
    const auto rebuild = [this, combo, current, with_none]() {
      const QSignalBlocker block(combo);
      combo->clear();
      if (with_none) {
        combo->addItem(tr("None"), QString::fromUtf8(kGridMapElevationNone.data(), kGridMapElevationNone.size()));
      }
      for (const std::string& name : field_names_) {
        combo->addItem(QString::fromStdString(name), QString::fromStdString(name));
      }
      combo->setCurrentIndex(std::max(0, combo->findData(QString::fromStdString(*current))));
      combo->setEnabled(combo->count() > 0);
    };
    rebuild();
    form->addRow(label, combo);
    QObject::connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, combo, setter](int) {
      (this->*setter)(combo->currentData().toString());
    });
    QPointer<PJ::ComboBox> safe_combo(combo);
    QObject::connect(this, &GridMapLayer::fieldsChanged, container, [safe_combo, rebuild]() {
      if (safe_combo) {
        rebuild();
      }
    });
  };
  add_field_combo(
      tr("Elevation:"), u"grid_map_elevation_field"_s, &resolved_elevation_, &GridMapLayer::setElevationField,
      /*with_none=*/true);
  add_field_combo(
      tr("Color by:"), u"grid_map_color_field"_s, &resolved_color_, &GridMapLayer::setColorField,
      /*with_none=*/false);

  auto* colormap_combo = new PJ::ComboBox(container);
  for (int idx = 0; idx < PJ::kColormapCount; ++idx) {
    colormap_combo->addItem(tr(PJ::colormapName(static_cast<PJ::Colormap>(idx))), idx);
  }
  colormap_combo->setCurrentIndex(static_cast<int>(colormap_));
  form->addRow(tr("Colors:"), colormap_combo);
  QObject::connect(colormap_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
    setColormap(idx >= 0 && idx < PJ::kColormapCount ? static_cast<PJ::Colormap>(idx) : PJ::Colormap::kTurbo);
  });

  buildRangeRows(form, container);

  auto* lighting_toggle = new PJ::ToggleSwitch(container);
  lighting_toggle->setChecked(lighting_, /*animate=*/false);
  form->addRow(tr("Lighting:"), lighting_toggle);
  QObject::connect(lighting_toggle, &PJ::ToggleSwitch::toggled, this, [this](bool on) { setLighting(on); });

  auto* opacity_spin = new PJ::DoubleScrubber(container);
  opacity_spin->setRange(0.0, 1.0);
  opacity_spin->setDecimals(2);
  opacity_spin->setSingleStep(0.05);
  opacity_spin->setValue(opacity_);
  form->addRow(tr("Opacity:"), opacity_spin);
  QObject::connect(opacity_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) { setOpacity(v); });

  return container;
}

void GridMapLayer::buildRangeRows(QFormLayout* form, QWidget* parent) {
  auto* auto_toggle = new PJ::ToggleSwitch(parent);
  auto_toggle->setChecked(auto_range_, /*animate=*/false);
  form->addRow(tr("Auto range:"), auto_toggle);

  const auto add_scrubber = [this, form, parent](const QString& label, const QString& object_name, double value) {
    auto* spin = new PJ::DoubleScrubber(parent);
    spin->setObjectName(object_name);
    spin->setRange(-1.0e9, 1.0e9);
    spin->setDecimals(3);
    spin->setValue(value);
    spin->setEnabled(!auto_range_);
    form->addRow(label, spin);
    return spin;
  };
  auto* range_lo_spin = add_scrubber(tr("Range min:"), u"grid_map_range_min"_s, manual_lo_);
  auto* range_hi_spin = add_scrubber(tr("Range max:"), u"grid_map_range_max"_s, manual_hi_);

  QObject::connect(auto_toggle, &PJ::ToggleSwitch::toggled, this, [this, range_lo_spin, range_hi_spin](bool on) {
    // Switching to manual starts from the range the user was just looking at.
    if (!on && last_auto_range_.valid) {
      const QSignalBlocker block_lo(range_lo_spin);
      const QSignalBlocker block_hi(range_hi_spin);
      range_lo_spin->setValue(last_auto_range_.lo);
      range_hi_spin->setValue(last_auto_range_.hi);
      setManualRange(last_auto_range_.lo, last_auto_range_.hi);
    }
    setAutoRange(on);
    range_lo_spin->setEnabled(!on);
    range_hi_spin->setEnabled(!on);
  });
  // Keep min <= max: nudging one scrubber past the other drags the sibling along
  // (its signal blocked so it does not re-enter here), then pushes the pair once.
  QObject::connect(
      range_lo_spin, &PJ::DoubleScrubber::valueChanged, this, [this, range_lo_spin, range_hi_spin](double v) {
        if (v > range_hi_spin->value()) {
          const QSignalBlocker block(range_hi_spin);
          range_hi_spin->setValue(v);
        }
        setManualRange(range_lo_spin->value(), range_hi_spin->value());
      });
  QObject::connect(
      range_hi_spin, &PJ::DoubleScrubber::valueChanged, this, [this, range_lo_spin, range_hi_spin](double v) {
        if (v < range_lo_spin->value()) {
          const QSignalBlocker block(range_lo_spin);
          range_lo_spin->setValue(v);
        }
        setManualRange(range_lo_spin->value(), range_hi_spin->value());
      });
}

}  // namespace pj::scene3d
