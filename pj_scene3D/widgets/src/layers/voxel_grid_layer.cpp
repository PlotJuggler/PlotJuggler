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
#include <glm/glm.hpp>
#include <optional>

#include "layer_xml_validation.h"
#include "pj_base/builtin/voxel_grid.hpp"
#include "pj_base/time.hpp"  // PJ::fromRaw, PJ::toRaw
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/pointcloud_convert.h"  // findField (shared field lookup)
#include "pj_scene3d_widgets/parse_locked.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcVoxel, "pj.scene3d.voxel_grid")

glm::vec3 toGlmCellSize(const PJ::sdk::Vector3& v) {
  return glm::vec3(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z));
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

bool VoxelGridLayer::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "voxel_grid"_L1 || !detail::isLeafPayload(element)) {
    return false;
  }
  bool mode_ok = false;
  bool threshold_ok = false;
  bool auto_range_ok = false;
  bool range_lo_ok = false;
  bool range_hi_ok = false;
  bool colormap_ok = false;
  bool opacity_ok = false;
  const int mode = element.attribute(u"draw_mode"_s, u"1"_s).toInt(&mode_ok);
  const double threshold = element.attribute(u"threshold"_s, u"0"_s).toDouble(&threshold_ok);
  const int auto_range = element.attribute(u"auto_range"_s, u"1"_s).toInt(&auto_range_ok);
  const double range_lo = element.attribute(u"range_lo"_s, u"0"_s).toDouble(&range_lo_ok);
  const double range_hi = element.attribute(u"range_hi"_s, u"1"_s).toDouble(&range_hi_ok);
  const int colormap = element.attribute(u"colormap"_s, u"0"_s).toInt(&colormap_ok);
  const double opacity = element.attribute(u"opacity"_s, u"1"_s).toDouble(&opacity_ok);
  if (!mode_ok || mode < 0 || mode > 3 || !threshold_ok || !std::isfinite(threshold) || !auto_range_ok ||
      (auto_range != 0 && auto_range != 1) || !range_lo_ok || !std::isfinite(range_lo) || !range_hi_ok ||
      !std::isfinite(range_hi) || range_lo > range_hi || !colormap_ok || colormap < 0 ||
      colormap >= PJ::kColormapCount || !opacity_ok || !std::isfinite(opacity) || opacity < 0.0 || opacity > 1.0) {
    return false;
  }
  // active_field_name_ is restored verbatim; if the named field is absent when a
  // grid arrives, resolveField() falls back to the default and adopts the saved
  // name later once a grid carrying it appears (late-arrival safe).
  setActiveField(element.attribute(u"field"_s));
  uploaded_field_setting_ = "\x01";  // force a re-pack on the next render
  setDrawMode(static_cast<VoxelDrawMode>(mode));
  setThreshold(threshold);
  setAutoRange(auto_range != 0);
  setManualRange(range_lo, range_hi);
  setColormap(static_cast<PJ::Colormap>(colormap));
  setOpacity(opacity);
  pushDisplayParamsToPass();
  return true;
}

bool VoxelGridLayer::attach(const PJ::SceneLayerContext& ctx) {
  const auto& scene3d_ctx = static_cast<const Scene3DLayerContext&>(ctx);
  if (scene3d_ctx.session == nullptr) {
    qCWarning(lcVoxel) << "attach: session is null";
    return false;
  }
  ctx_ = scene3d_ctx;
  if (!scene3d_ctx.session->parserBindingForObjectTopic(topic_id_)) {
    qCWarning(lcVoxel) << "attach: no parser for voxel-grid topic" << topic_id_.id;
    return false;
  }
  resetStreamingState();
  pushDisplayParamsToPass();
  // Streaming-tolerant attach (matches OccupancyGridLayer): the topic may have no
  // sample yet (layout restore at stream start, catalog drag). bootstrap() only
  // pre-warms source_frame_ + the default field; renderAt() self-heals later, so a
  // failure must NOT drop the layer.
  if (!bootstrap()) {
    qCWarning(lcVoxel) << "attach: bootstrap deferred for voxel-grid topic" << topic_id_.id
                       << "— render will start once a sample arrives";
  }
  return true;
}

void VoxelGridLayer::detach() {
  pass_.clearGrid();
  resetStreamingState();
}

void VoxelGridLayer::resetStreamingState() {
  cached_grid_.reset();
  cached_grid_uid_ = {};
  uploaded_uid_ = {};
  uploaded_field_setting_ = "\x01";
}

bool VoxelGridLayer::bootstrap() {
  PJ::ObjectStore& store = ctx_.session->objectStore();
  auto first = store.at(topic_id_, 0);
  if (!first.has_value() || first->payload.bytes.empty()) {
    return false;
  }
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  if (!binding) {
    return false;
  }
  auto obj = parseLocked(binding, first->timestamp, first->payload);
  if (!obj.has_value()) {
    qCWarning(lcVoxel) << "bootstrap: parseObject failed:" << QString::fromStdString(obj.error());
    return false;
  }
  const auto* grid = std::any_cast<PJ::sdk::VoxelGrid>(&obj->object);
  if (grid == nullptr) {
    return false;
  }
  source_frame_ = grid->frame_id;
  resolveField(*grid);  // pre-warm resolved_field_name_ + value_kind_ for the config UI
  if (!source_frame_.empty()) {
    emit sourceFrameChanged(QString::fromStdString(source_frame_));
    emit fallbackFramesChanged(fallbackFrames());
  }
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

void VoxelGridLayer::renderAt(int64_t time_ns) {
  if (ctx_.session == nullptr) {
    return;
  }
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  if (!binding) {
    return;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  // Fast path, checked metadata-first (latestEntryIdAt — no payload resolve, so
  // an unchanged grid never pays a cold refetch of an evicted pool seed): the
  // same grid + same field setting is already on the GPU. No parse, no pack, no
  // upload — the scrub/replay contract (zero per-voxel CPU work). Shared by the
  // gate and the post-resolve re-check (a racing insert can change the pick).
  const auto already_uploaded = [this](PJ::SequentialUID uid) {
    return uid == uploaded_uid_ && active_field_name_ == uploaded_field_setting_;
  };
  const auto entry_id = store.latestEntryIdAt(topic_id_, time_ns);
  if (entry_id.has_value() && already_uploaded(entry_id->uid)) {
    return;
  }
  auto entry = entry_id.has_value() ? store.latestAt(topic_id_, time_ns) : std::nullopt;
  if (!entry.has_value() || entry->payload.bytes.empty()) {
    // No sample at/before this time (scrubbed before the first sample, or a gap).
    // Clear the pass AND invalidate the upload memo: otherwise scrubbing back onto
    // the same store entry would hit the fast-path above, skip the re-upload, and
    // leave the (now-cleared) pass empty — the grid would silently never return.
    pass_.clearGrid();
    uploaded_uid_ = {};
    uploaded_field_setting_ = "\x01";
    return;
  }
  if (already_uploaded(entry->sequential_uid)) {
    return;
  }

  // Reuse the parse memo when only the field changed; otherwise decode + cache.
  const PJ::sdk::VoxelGrid* grid = nullptr;
  if (cached_grid_.has_value() && entry->sequential_uid == cached_grid_uid_) {
    grid = &*cached_grid_;
  } else {
    auto obj = parseLocked(binding, entry->timestamp, entry->payload);
    if (!obj.has_value()) {
      qCWarning(lcVoxel) << "renderAt: parseObject failed:" << QString::fromStdString(obj.error());
      return;
    }
    const auto* parsed = std::any_cast<PJ::sdk::VoxelGrid>(&obj->object);
    if (parsed == nullptr) {
      return;
    }
    cached_grid_ = *parsed;  // copy carries the anchor → bytes stay alive past the call
    cached_grid_uid_ = entry->sequential_uid;
    grid = &*cached_grid_;
  }

  if (grid->frame_id != source_frame_) {
    source_frame_ = grid->frame_id;
    emit sourceFrameChanged(QString::fromStdString(source_frame_));
    emit fallbackFramesChanged(fallbackFrames());
  }

  // Untrusted wire dims: a corrupt grid can declare far more voxels than its
  // payload backs (see kMaxRenderableVoxels). Packing refuses it and the pass
  // would drop it silently, so surface a layer-row warning and skip the work
  // rather than leave the user staring at an empty view with no explanation.
  const uint64_t voxels = voxelCount(*grid);
  if (voxels > kMaxRenderableVoxels) {
    qCWarning(lcVoxel) << "renderAt: voxel count" << voxels << "exceeds render cap" << kMaxRenderableVoxels
                       << "— grid not displayed (corrupt/unsupported dimensions)";
    setStatusWarning(tr("Voxel grid too large to display: %1 voxels (corrupt or unsupported dimensions)").arg(voxels));
    pass_.clearGrid();
    uploaded_uid_ = entry->sequential_uid;
    uploaded_field_setting_ = active_field_name_;
    return;
  }
  setStatusWarning(QString());  // a renderable-sized grid arrived → clear any notice

  const PJ::sdk::PointField* field = resolveField(*grid);
  if (field == nullptr) {
    pass_.clearGrid();
    uploaded_uid_ = entry->sequential_uid;
    uploaded_field_setting_ = active_field_name_;
    return;
  }

  VoxelGridUpload upload;
  upload.frame_id = grid->frame_id;
  upload.origin = grid->origin;
  upload.cell_size = toGlmCellSize(grid->cell_size);
  upload.column_count = grid->column_count;
  upload.row_count = grid->row_count;
  upload.slice_count = grid->slice_count;
  upload.kind = value_kind_;
  if (value_kind_ == VoxelValueKind::kScalar) {
    upload.scalar = packScalarField(*grid, *field);
  } else {
    upload.rgba = packRgbaField(*grid, *field);
  }
  pass_.setGrid(std::move(upload));

  uploaded_uid_ = entry->sequential_uid;
  uploaded_field_setting_ = active_field_name_;
}

void VoxelGridLayer::setFixedFrame(const QString& frame) {
  // The grid is frame-relative; the pass places it per-frame via FrameContext.
  Q_UNUSED(frame);
  emit repaintRequested();
}

void VoxelGridLayer::setStatusWarning(const QString& reason) {
  if (status_warning_ == reason) {
    return;
  }
  status_warning_ = reason;
  emit statusWarningChanged();  // dock re-combines this into the layer-row warning
}

void VoxelGridLayer::setTrackerTime(PJ::Timepoint time) {
  Q_UNUSED(time);  // render() reads frame_ctx.time via the tracker_dirty_ path
  tracker_dirty_ = true;
  emit repaintRequested();
}

void VoxelGridLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  pass_.setVisible(visible);
  emit visibilityChanged(visible);
  emit repaintRequested();
}

void VoxelGridLayer::initializeGL() {
  pass_.initializeGL();
}

void VoxelGridLayer::releaseGL() {
  pass_.releaseGL();
}

void VoxelGridLayer::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!visible_) {
    return;
  }
  if (tracker_dirty_) {
    tracker_dirty_ = false;
    renderAt(PJ::toRaw(frame_ctx.time));
  }
  pass_.render(view_params, frame_ctx);
}

std::optional<AABB> VoxelGridLayer::worldBounds() const {
  if (!cached_grid_.has_value()) {
    return std::nullopt;
  }
  const AABB box = voxelGridBounds(*cached_grid_);
  if (!box.valid) {
    return std::nullopt;
  }
  return box;
}

void VoxelGridLayer::pushDisplayParamsToPass() {
  pass_.setDrawMode(draw_mode_);
  pass_.setThreshold(static_cast<float>(threshold_));
  pass_.setAutoRange(auto_range_);
  pass_.setManualRange(static_cast<float>(manual_lo_), static_cast<float>(manual_hi_));
  pass_.setColormap(colormap_);
  pass_.setOpacity(static_cast<float>(opacity_));
  pass_.setVisible(visible_);
}

void VoxelGridLayer::setActiveField(const QString& field_name) {
  const std::string next = field_name.toStdString();
  if (active_field_name_ == next) {
    return;
  }
  active_field_name_ = next;
  tracker_dirty_ = true;  // force renderAt → re-pack with the new field
  emit configurationChanged();
  emit repaintRequested();
}

void VoxelGridLayer::setDrawMode(VoxelDrawMode mode) {
  if (draw_mode_ == mode) {
    return;
  }
  draw_mode_ = mode;
  pass_.setDrawMode(mode);
  emit configurationChanged();
  emit repaintRequested();
}

void VoxelGridLayer::setThreshold(double threshold) {
  if (threshold_ == threshold) {
    return;
  }
  threshold_ = threshold;
  pass_.setThreshold(static_cast<float>(threshold));
  emit configurationChanged();
  emit repaintRequested();
}

void VoxelGridLayer::setAutoRange(bool on) {
  if (auto_range_ == on) {
    return;
  }
  auto_range_ = on;
  pass_.setAutoRange(on);
  emit configurationChanged();
  emit repaintRequested();
}

void VoxelGridLayer::setManualRange(double lo, double hi) {
  if (manual_lo_ == lo && manual_hi_ == hi) {
    return;
  }
  manual_lo_ = lo;
  manual_hi_ = hi;
  pass_.setManualRange(static_cast<float>(lo), static_cast<float>(hi));
  emit configurationChanged();
  emit repaintRequested();
}

void VoxelGridLayer::setColormap(PJ::Colormap colormap) {
  if (colormap_ == colormap) {
    return;
  }
  colormap_ = colormap;
  pass_.setColormap(colormap);
  emit configurationChanged();
  emit repaintRequested();
}

void VoxelGridLayer::setOpacity(double opacity) {
  const double clamped = std::clamp(opacity, 0.0, 1.0);
  if (opacity_ == clamped) {
    return;
  }
  opacity_ = clamped;
  pass_.setOpacity(static_cast<float>(opacity_));
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
  if (cached_grid_.has_value()) {
    for (const PJ::sdk::PointField& f : cached_grid_->fields) {
      field_combo->addItem(QString::fromStdString(f.name), QString::fromStdString(f.name));
    }
    const int idx = field_combo->findData(QString::fromStdString(resolved_field_name_));
    if (idx >= 0) {
      field_combo->setCurrentIndex(idx);
    }
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
