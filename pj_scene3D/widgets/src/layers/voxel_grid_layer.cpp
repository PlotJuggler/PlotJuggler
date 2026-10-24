// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_widgets/layers/voxel_grid_layer.h"

#include <QCheckBox>
#include <QDomElement>
#include <QFormLayout>
#include <QLoggingCategory>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <any>
#include <cmath>
#include <exception>
#include <glm/glm.hpp>
#include <limits>
#include <new>
#include <optional>

#include "layer_xml_validation.h"
#include "pj_base/builtin/voxel_grid.hpp"
#include "pj_base/time.hpp"  // PJ::fromRaw, PJ::toRaw
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/grid_map_view.h"       // finiteRange
#include "pj_scene3d_core/pointcloud_convert.h"  // findField (shared field lookup)
#include "pj_scene3d_core/pose_math.h"
#include "pj_scene3d_core/tf/object_placement.h"
#include "pj_scene3d_core/voxel_grid_budget.h"
#include "pj_scene3d_widgets/resolve_object.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcVoxel, "pj.scene3d.voxel_grid")

constexpr const char* kNoFieldSetting = "\x01";  // sentinel != any real "" / name

bool finitePositiveCellSize(const PJ::sdk::Vector3& size) {
  return std::isfinite(size.x) && std::isfinite(size.y) && std::isfinite(size.z) && size.x > 0.0 && size.y > 0.0 &&
         size.z > 0.0;
}

}  // namespace

VoxelGridLayer::VoxelGridLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : Scene3DLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)) {}

VoxelGridLayer::~VoxelGridLayer() = default;

PJ::SceneLayerInfo VoxelGridLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kVoxelGrid,
      .display_name = display_name_,
      .family_name = u"VoxelGrid"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> VoxelGridLayer::timeRange() const {
  const auto* store = ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr;
  return PJ::liveTopicTimeRange(store, topic_id_);
}

QStringList VoxelGridLayer::fallbackFrames() const {
  if (source_frame_.empty()) {
    return {};
  }
  return {QString::fromStdString(source_frame_)};
}

QString VoxelGridLayer::sourceFrame() const {
  return QString::fromStdString(source_frame_);
}

QDomElement VoxelGridLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(u"voxel_grid"_s);
  el.setAttribute(u"field"_s, QString::fromStdString(active_field_name_));
  el.setAttribute(u"draw_mode"_s, static_cast<int>(draw_mode_));
  el.setAttribute(u"threshold"_s, threshold_);
  el.setAttribute(u"auto_range"_s, auto_range_ ? 1 : 0);
  el.setAttribute(u"range_lo"_s, manual_lo_);
  el.setAttribute(u"range_hi"_s, manual_hi_);
  el.setAttribute(u"colormap"_s, static_cast<int>(colormap_));
  el.setAttribute(u"opacity"_s, opacity_);
  return el;
}

std::optional<VoxelGridLayer::ParsedSettings> VoxelGridLayer::parseSettings(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "voxel_grid"_L1 || !detail::isLeafPayload(element)) {
    return std::nullopt;
  }
  ParsedSettings settings;
  settings.field = element.attribute(u"field"_s).toStdString();
  constexpr double kLowest = std::numeric_limits<double>::lowest();
  constexpr double kMax = std::numeric_limits<double>::max();
  int mode = 1;
  int colormap = 0;
  if (!detail::parseBoundedInt(element, "draw_mode", 1, 0, 3, mode) ||
      !detail::parseFiniteDouble(element, "threshold", 0.0, kLowest, kMax, settings.threshold) ||
      !detail::parseZeroOne(element, "auto_range", true, settings.auto_range) ||
      !detail::parseFiniteDouble(element, "range_lo", 0.0, kLowest, kMax, settings.range_lo) ||
      !detail::parseFiniteDouble(element, "range_hi", 1.0, kLowest, kMax, settings.range_hi) ||
      settings.range_lo > settings.range_hi ||
      !detail::parseBoundedInt(element, "colormap", 0, 0, PJ::kColormapCount - 1, colormap) ||
      !detail::parseFiniteDouble(element, "opacity", 1.0, 0.0, 1.0, settings.opacity)) {
    return std::nullopt;
  }
  settings.draw_mode = static_cast<VoxelDrawMode>(mode);
  settings.colormap = static_cast<PJ::Colormap>(colormap);
  return settings;
}

bool VoxelGridLayer::validateXml(const QDomElement& element) {
  return parseSettings(element).has_value();
}

bool VoxelGridLayer::xmlLoadState(const QDomElement& element) {
  const auto settings = parseSettings(element);
  if (!settings.has_value()) {
    return false;
  }
  // active_field_name_ is restored verbatim; if the named field is absent when a
  // grid arrives, resolveField() falls back to the default and adopts the saved
  // name later once a grid carrying it appears (late-arrival safe).
  setActiveField(QString::fromStdString(settings->field));
  uploaded_field_setting_ = kNoFieldSetting;  // force a re-pack on the next render
  setDrawMode(settings->draw_mode);
  setThreshold(settings->threshold);
  setAutoRange(settings->auto_range);
  setManualRange(settings->range_lo, settings->range_hi);
  setColormap(settings->colormap);
  setOpacity(settings->opacity);
  return true;
}

bool VoxelGridLayer::attach(const PJ::SceneLayerContext& ctx) {
  const auto& scene3d_ctx = static_cast<const Scene3DLayerContext&>(ctx);
  if (scene3d_ctx.session == nullptr) {
    qCWarning(lcVoxel) << "attach: session is null";
    return false;
  }
  // A parser-backed topic, or a parser-less canonical one (resolveObject()).
  if (!scene3d_ctx.session->parserBindingForObjectTopic(topic_id_) &&
      !hasCanonical3DCodec(PJ::sdk::BuiltinObjectType::kVoxelGrid)) {
    qCWarning(lcVoxel) << "attach: no parser or canonical codec for voxel-grid topic" << topic_id_.id;
    return false;
  }
  ctx_ = scene3d_ctx;
  resetStreamingState();
  disconnect(reload_connection_);
  reload_connection_ = connect(
      ctx_.session, &PJ::SessionManager::datasetAboutToBeReplaced, this, &VoxelGridLayer::onDatasetAboutToBeReplaced);
  // Streaming-tolerant attach (matches OccupancyGridLayer): the topic may have no
  // sample yet (layout restore at stream start, catalog drag). bootstrap() only
  // pre-warms source_frame_ + the default field; updateAt() self-heals later, so a
  // failure must NOT drop the layer.
  if (!bootstrap()) {
    qCWarning(lcVoxel) << "attach: bootstrap deferred for voxel-grid topic" << topic_id_.id
                       << "— render will start once a sample arrives";
  }
  return true;
}

void VoxelGridLayer::detach() {
  disconnect(reload_connection_);
  reload_connection_ = {};
  clearVolume();
  resetStreamingState();
  setDataWarning({});
  noteRenderSuccess();
  requested_time_.reset();
  tracker_dirty_ = false;
}

void VoxelGridLayer::resetStreamingState() {
  uploaded_uid_ = {};
  uploaded_field_setting_ = kNoFieldSetting;
  budget_.reset();
}

void VoxelGridLayer::onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id) {
  if (ctx_.session == nullptr || ctx_.session->objectStore().descriptor(topic_id_).dataset_id != dataset_id) {
    return;
  }
  clearVolume();
  resetStreamingState();
  tracker_dirty_ = true;
  setDataWarning({});
  noteRenderSuccess();
  emit repaintRequested();
}

bool VoxelGridLayer::bootstrap() {
  PJ::ObjectStore& store = ctx_.session->objectStore();
  auto first = store.at(topic_id_, 0);
  if (!first.has_value() || first->payload.bytes.empty() ||
      static_cast<std::uint64_t>(first->payload.bytes.size()) > kSceneLimits.max_voxel_wire_bytes) {
    return false;
  }
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  auto obj = resolveObject(binding, PJ::sdk::BuiltinObjectType::kVoxelGrid, first->timestamp, first->payload);
  if (!obj.has_value()) {
    qCWarning(lcVoxel) << "bootstrap: parseObject failed:" << QString::fromStdString(obj.error());
    return false;
  }
  const auto* grid = obj->object.get<PJ::sdk::VoxelGrid>();
  if (grid == nullptr) {
    return false;
  }
  updateSourceFrame(grid->frame_id);
  populateFields(*grid);
  resolveField(*grid);  // pre-warm resolved_field_name_ + value_kind_ for the config UI
  return true;
}

const PJ::sdk::PointField* VoxelGridLayer::resolveField(const PJ::sdk::VoxelGrid& grid) {
  if (grid.fields.empty()) {
    resolved_field_name_.clear();
    return nullptr;
  }
  const PJ::sdk::PointField* field = active_field_name_.empty() ? nullptr : findField(grid.fields, active_field_name_);
  if (field != nullptr) {
    value_kind_ = voxelFieldKind(*field);
  } else {
    const VoxelFieldSelection sel = chooseDefaultField(grid.fields);
    if (sel.index < 0) {
      resolved_field_name_.clear();
      return nullptr;
    }
    field = &grid.fields[static_cast<size_t>(sel.index)];
    value_kind_ = sel.kind;
  }
  resolved_field_name_ = field->name;
  return field;
}

void VoxelGridLayer::populateFields(const PJ::sdk::VoxelGrid& grid) {
  QStringList fields;
  fields.reserve(static_cast<qsizetype>(grid.fields.size()));
  for (const PJ::sdk::PointField& field : grid.fields) {
    fields.push_back(QString::fromStdString(field.name));
  }
  available_fields_ = std::move(fields);
}

void VoxelGridLayer::clearVolume() {
  // Swap, not clear(): clear() keeps the capacity, which a hidden, detached or
  // rejected layer would keep pinned (the browser heap is tight).
  std::vector<float>{}.swap(scalar_volume_);
  std::vector<std::uint8_t>{}.swap(rgba_volume_);
  columns_ = 0;
  rows_ = 0;
  slices_ = 0;
  source_bounds_ = {};
  ++texture_revision_;
}

void VoxelGridLayer::updateSourceFrame(const std::string& frame) {
  if (frame == source_frame_) {
    return;
  }
  source_frame_ = frame;
  emit sourceFrameChanged(QString::fromStdString(source_frame_));
  emit fallbackFramesChanged(fallbackFrames());
}

bool VoxelGridLayer::updateAt(int64_t time_ns) {
  if (ctx_.session == nullptr) {
    return false;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  // Fast path, checked metadata-first (latestEntryIdAt — no payload resolve, so
  // an unchanged grid never pays a cold refetch of an evicted pool seed): the
  // same grid + same field setting is already packed. No parse, no pack, no
  // upload — the scrub/replay contract (zero per-voxel CPU work). Shared by the
  // gate and the post-resolve re-check (a racing insert can change the pick).
  const auto already_uploaded = [this](PJ::SequentialUID uid) {
    return uid == uploaded_uid_ && active_field_name_ == uploaded_field_setting_;
  };
  const auto entry_id = store.latestEntryIdAt(topic_id_, time_ns);
  if (entry_id.has_value() && already_uploaded(entry_id->uid)) {
    return false;
  }
  auto entry = entry_id.has_value() ? store.latestAt(topic_id_, time_ns) : std::nullopt;
  if (!entry.has_value() || entry->payload.bytes.empty()) {
    // No sample at/before this time (scrubbed before the first sample, or a gap).
    // Clear the volume AND invalidate the upload memo: otherwise scrubbing back onto
    // the same store entry would hit the fast-path above, skip the re-pack, and
    // leave the volume empty — the grid would silently never return.
    const bool changed = hasVolume() || !dataWarning().isEmpty();
    clearVolume();
    uploaded_uid_ = {};
    uploaded_field_setting_ = kNoFieldSetting;
    setDataWarning({});
    return changed;
  }
  if (already_uploaded(entry->sequential_uid)) {
    return false;
  }
  // Every exit below marks this sample packed, so a bad one is not retried per frame.
  uploaded_uid_ = entry->sequential_uid;
  uploaded_field_setting_ = active_field_name_;
  const auto fail = [this](QString warning) {
    clearVolume();
    setDataWarning(std::move(warning));
    return true;
  };

  if (static_cast<std::uint64_t>(entry->payload.bytes.size()) > kSceneLimits.max_voxel_wire_bytes) {
    return fail(tr("Voxel-grid payload is %1 MiB; the limit is %2 MiB")
                    .arg(QString::number(static_cast<double>(entry->payload.bytes.size()) / (1024.0 * 1024.0), 'f', 1))
                    .arg(kSceneLimits.max_voxel_wire_bytes / kMiB));
  }
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  auto obj = resolveObject(binding, PJ::sdk::BuiltinObjectType::kVoxelGrid, entry->timestamp, entry->payload);
  if (!obj.has_value()) {
    qCWarning(lcVoxel) << "updateAt: parseObject failed:" << QString::fromStdString(obj.error());
    return fail(tr("Voxel grid could not be decoded: %1").arg(QString::fromStdString(obj.error())));
  }
  const PJ::sdk::VoxelGrid* grid = obj->object.get<PJ::sdk::VoxelGrid>();  // valid while obj lives
  if (grid == nullptr) {
    return fail(tr("This layer accepts VoxelGrid samples only"));
  }

  updateSourceFrame(grid->frame_id);
  render_stamp_ = objectStamp(grid->timestamp_ns, entry->payload_stamp_shift);
  populateFields(*grid);

  // Untrusted wire dims: a corrupt grid can declare far more voxels than its
  // payload backs (see kMaxRenderableVoxels). Packing refuses it, so surface a
  // layer-row warning and skip the work rather than leave the user staring at an
  // empty view with no explanation.
  const uint64_t voxels = pj::scene3d::voxelCount(*grid);
  if (voxels > kMaxRenderableVoxels) {
    qCWarning(lcVoxel) << "updateAt: voxel count" << voxels << "exceeds render cap" << kMaxRenderableVoxels
                       << "— grid not displayed (corrupt/unsupported dimensions)";
    return fail(tr("Voxel grid too large to display: %1 voxels (corrupt or unsupported dimensions)").arg(voxels));
  }
  if (!voxelDimensionsFit(grid->column_count, grid->row_count, grid->slice_count)) {
    return fail(tr("Voxel grid is %1 x %2 x %3 (%4 voxels); the layer limit is %5")
                    .arg(
                        QString::number(grid->column_count), QString::number(grid->row_count),
                        QString::number(grid->slice_count), QString::number(voxels),
                        QString::number(kSceneLimits.max_voxels_per_layer)));
  }
  if (!finitePositiveCellSize(grid->cell_size) || !finitePose(grid->origin)) {
    return fail(tr("Voxel grid has an invalid cell size or origin pose"));
  }
  const PJ::sdk::PointField* field = resolveField(*grid);
  if (field == nullptr) {
    return fail(tr("Voxel grid has no displayable fields"));
  }

  std::vector<float> scalar;
  std::vector<std::uint8_t> rgba;
  try {
    if (value_kind_ == VoxelValueKind::kScalar) {
      scalar = packScalarField(*grid, *field);
    } else {
      rgba = packRgbaField(*grid, *field);
    }
  } catch (const std::bad_alloc&) {
    return fail(tr("Not enough memory to pack this voxel grid"));
  } catch (const std::exception& error) {
    return fail(tr("Voxel-grid packing failed: %1").arg(QString::fromUtf8(error.what())));
  }
  const bool complete = value_kind_ == VoxelValueKind::kScalar ? scalar.size() == voxels : rgba.size() == voxels * 4U;
  if (!complete) {
    return fail(tr("Voxel grid field layout could not be packed"));
  }

  scalar_volume_ = std::move(scalar);
  rgba_volume_ = std::move(rgba);
  origin_ = grid->origin;
  cell_size_ = glm::vec3(
      static_cast<float>(grid->cell_size.x), static_cast<float>(grid->cell_size.y),
      static_cast<float>(grid->cell_size.z));
  columns_ = grid->column_count;
  rows_ = grid->row_count;
  slices_ = grid->slice_count;
  source_bounds_ = voxelGridBounds(*grid);
  const FiniteRange range = value_kind_ == VoxelValueKind::kScalar ? finiteRange(scalar_volume_) : FiniteRange{};
  automatic_range_ = range.valid ? std::pair<float, float>{range.lo, range.hi} : std::pair<float, float>{0.0f, 1.0f};
  ++texture_revision_;
  setDataWarning({});
  return true;
}

void VoxelGridLayer::setFixedFrame(const QString& frame) {
  // The grid is frame-relative; the renderer places it per-frame.
  fixed_frame_ = frame.toStdString();
  emit repaintRequested();
}

void VoxelGridLayer::setTrackerTime(PJ::Timepoint time) {
  tracker_dirty_ = true;
  requested_time_ = time;
  emit repaintRequested();
}

void VoxelGridLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  // Release the packed volume while hidden; un-hiding re-packs. Its bounds
  // stay: a hidden layer still counts in the camera's scene bounds.
  if (!visible_) {
    budget_.reset();
    const AABB bounds = source_bounds_;
    clearVolume();
    source_bounds_ = bounds;
    uploaded_uid_ = {};
    uploaded_field_setting_ = kNoFieldSetting;
  } else if (requested_time_.has_value()) {
    tracker_dirty_ = true;
  }
  emit visibilityChanged(visible);
  emit repaintRequested();
}

bool VoxelGridLayer::drainPendingUpdate() {
  if (!visible_ || !tracker_dirty_ || !requested_time_.has_value()) {
    return false;
  }
  tracker_dirty_ = false;
  return updateAt(PJ::toRaw(*requested_time_));
}

std::uint64_t VoxelGridLayer::renderKey(PJ::Timepoint time) const {
  const std::uint64_t key =
      PJ::latestEntryRenderKey(ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr, topic_id_, time);
  return foldFrameTransformIntoKey(
      key, ctx_.tf_buffer.get(), fixed_frame_, source_frame_, render_stamp_, time, placementHold());
}

bool VoxelGridLayer::prepareForRender(std::uint64_t remaining_view_voxels) {
  // The budget_ candidate: the sample at the playhead (settings changes reset the latch).
  const auto candidate = [this] {
    return PJ::latestEntryRenderKey(
        ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr, topic_id_,
        requested_time_.value_or(PJ::Timepoint{}));
  };
  bool released = false;
  if (budget_.blocked(remaining_view_voxels, candidate, released)) {
    return false;
  }
  if (released) {
    uploaded_uid_ = {};
    uploaded_field_setting_ = kNoFieldSetting;
    tracker_dirty_ = true;
  }
  const bool changed = drainPendingUpdate();
  if (!hasVolume() || voxelCount() <= remaining_view_voxels) {
    return changed;
  }
  budget_.reject(voxelCount(), candidate());
  setDataWarning(tr("Layer needs %1 voxels; only %2 remain in this view's budget")
                     .arg(QString::number(voxelCount()), QString::number(remaining_view_voxels)));
  clearVolume();
  uploaded_uid_ = {};
  uploaded_field_setting_ = kNoFieldSetting;
  return true;
}

void VoxelGridLayer::rejectVolumeForRender(QString warning) {
  noteRenderFailure(std::move(warning));
  clearVolume();
}

void VoxelGridLayer::setActiveField(const QString& field_name) {
  const std::string next = field_name.toStdString();
  if (active_field_name_ == next) {
    return;
  }
  active_field_name_ = next;
  tracker_dirty_ = true;  // force updateAt → re-pack with the new field
  budget_.reset();
  emit configurationChanged();
  emit repaintRequested();
}

void VoxelGridLayer::setDrawMode(VoxelDrawMode mode) {
  if (draw_mode_ == mode) {
    return;
  }
  draw_mode_ = mode;
  emit configurationChanged();
  emit repaintRequested();
}

void VoxelGridLayer::setThreshold(double threshold) {
  if (threshold_ == threshold) {
    return;
  }
  threshold_ = threshold;
  emit configurationChanged();
  emit repaintRequested();
}

void VoxelGridLayer::setAutoRange(bool on) {
  if (auto_range_ == on) {
    return;
  }
  auto_range_ = on;
  emit configurationChanged();
  emit repaintRequested();
}

void VoxelGridLayer::setManualRange(double lo, double hi) {
  if (manual_lo_ == lo && manual_hi_ == hi) {
    return;
  }
  manual_lo_ = lo;
  manual_hi_ = hi;
  emit configurationChanged();
  emit repaintRequested();
}

void VoxelGridLayer::setColormap(PJ::Colormap colormap) {
  if (colormap_ == colormap) {
    return;
  }
  colormap_ = colormap;
  emit configurationChanged();
  emit repaintRequested();
}

void VoxelGridLayer::setOpacity(double opacity) {
  const double clamped = std::clamp(opacity, 0.0, 1.0);
  if (opacity_ == clamped) {
    return;
  }
  opacity_ = clamped;
  emit configurationChanged();
  emit repaintRequested();
}

QWidget* VoxelGridLayer::createConfigWidget(QWidget* parent) {
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

  // Field selector — populated from the last decoded grid's fields.
  auto* field_combo = new PJ::ComboBox(container);
  for (const QString& name : available_fields_) {
    field_combo->addItem(name, name);
  }
  const int field_idx = field_combo->findData(QString::fromStdString(resolved_field_name_));
  if (field_idx >= 0) {
    field_combo->setCurrentIndex(field_idx);
  }
  field_combo->setEnabled(field_combo->count() > 0);
  form->addRow(tr("Field:"), field_combo);
  QObject::connect(field_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, field_combo](int) {
    setActiveField(field_combo->currentData().toString());
  });

  auto* colormap_combo = new PJ::ComboBox(container);
  for (int idx = 0; idx < PJ::kColormapCount; ++idx) {
    colormap_combo->addItem(tr(PJ::colormapName(static_cast<PJ::Colormap>(idx))), idx);
  }
  colormap_combo->setCurrentIndex(static_cast<int>(colormap_));
  form->addRow(tr("Colors:"), colormap_combo);
  QObject::connect(colormap_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
    setColormap(idx >= 0 && idx < PJ::kColormapCount ? static_cast<PJ::Colormap>(idx) : PJ::Colormap::kTurbo);
  });

  auto* mode_combo = new PJ::ComboBox(container);
  mode_combo->addItem(tr("All cells"), static_cast<int>(VoxelDrawMode::kAll));
  mode_combo->addItem(tr("Non-zero"), static_cast<int>(VoxelDrawMode::kNonZero));
  mode_combo->addItem(tr("Threshold"), static_cast<int>(VoxelDrawMode::kThreshold));
  mode_combo->addItem(tr("In range"), static_cast<int>(VoxelDrawMode::kRange));
  mode_combo->setCurrentIndex(static_cast<int>(draw_mode_));
  form->addRow(tr("Draw:"), mode_combo);
  QObject::connect(mode_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
    setDrawMode(idx >= 0 && idx <= 3 ? static_cast<VoxelDrawMode>(idx) : VoxelDrawMode::kNonZero);
  });

  auto* threshold_spin = new PJ::DoubleScrubber(container);
  threshold_spin->setRange(-1.0e9, 1.0e9);
  threshold_spin->setDecimals(3);
  threshold_spin->setValue(threshold_);
  form->addRow(tr("Threshold:"), threshold_spin);
  QObject::connect(threshold_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) { setThreshold(v); });

  auto* auto_check = new QCheckBox(tr("Auto colormap range"), container);
  auto_check->setChecked(auto_range_);
  form->addRow(QString(), auto_check);

  auto* range_lo_spin = new PJ::DoubleScrubber(container);
  range_lo_spin->setRange(-1.0e9, 1.0e9);
  range_lo_spin->setDecimals(3);
  range_lo_spin->setValue(manual_lo_);
  form->addRow(tr("Range min:"), range_lo_spin);

  auto* range_hi_spin = new PJ::DoubleScrubber(container);
  range_hi_spin->setRange(-1.0e9, 1.0e9);
  range_hi_spin->setDecimals(3);
  range_hi_spin->setValue(manual_hi_);
  form->addRow(tr("Range max:"), range_hi_spin);

  // The Range min/max fields serve TWO roles: the manual colormap range (when auto
  // is off) AND the kRange draw-predicate window (always, regardless of auto). So
  // they stay editable whenever auto-range is off OR the draw mode is "In range" —
  // otherwise picking "In range" with the default auto-range on would pin the
  // predicate to a stale [0,1] with no way to set it (the inert range-cull bug).
  auto sync_range_enabled = [this, range_lo_spin, range_hi_spin]() {
    const bool enabled = !auto_range_ || draw_mode_ == VoxelDrawMode::kRange;
    range_lo_spin->setEnabled(enabled);
    range_hi_spin->setEnabled(enabled);
  };
  sync_range_enabled();

  QObject::connect(auto_check, &QCheckBox::toggled, this, [this, sync_range_enabled](bool on) {
    setAutoRange(on);
    sync_range_enabled();
  });
  // Second connection on the draw-mode combo (the first, above, calls setDrawMode):
  // Qt fires slots in connection order, so draw_mode_ is updated before this reads it.
  QObject::connect(mode_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [sync_range_enabled](int) {
    sync_range_enabled();
  });
  QObject::connect(range_lo_spin, &PJ::DoubleScrubber::valueChanged, this, [this, range_hi_spin](double v) {
    setManualRange(v, range_hi_spin->value());
  });
  QObject::connect(range_hi_spin, &PJ::DoubleScrubber::valueChanged, this, [this, range_lo_spin](double v) {
    setManualRange(range_lo_spin->value(), v);
  });

  auto* opacity_spin = new PJ::DoubleScrubber(container);
  opacity_spin->setRange(0.0, 1.0);
  opacity_spin->setDecimals(2);
  opacity_spin->setSingleStep(0.05);
  opacity_spin->setValue(opacity_);
  form->addRow(tr("Opacity:"), opacity_spin);
  QObject::connect(opacity_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) { setOpacity(v); });

  return container;
}

}  // namespace pj::scene3d
