// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/wasm/voxel_grid_layer_wasm.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLoggingCategory>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <any>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <utility>

#include "layers/layer_xml_validation.h"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/pointcloud_convert.h"
#include "pj_scene3d_widgets/resolve_object.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"

using namespace Qt::StringLiterals;

namespace pj::scene3d {
namespace {

Q_LOGGING_CATEGORY(lcWasmVoxel, "pj.scene3d.wasm.voxel")

bool finitePose(const PJ::sdk::Pose& pose) {
  const auto& position = pose.position;
  const auto& orientation = pose.orientation;
  const double norm_squared = orientation.x * orientation.x + orientation.y * orientation.y +
                              orientation.z * orientation.z + orientation.w * orientation.w;
  return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
         std::isfinite(orientation.x) && std::isfinite(orientation.y) && std::isfinite(orientation.z) &&
         std::isfinite(orientation.w) && std::isfinite(norm_squared) && norm_squared > 1.0e-18;
}

bool finitePositiveCellSize(const PJ::sdk::Vector3& size) {
  return std::isfinite(size.x) && std::isfinite(size.y) && std::isfinite(size.z) && size.x > 0.0 && size.y > 0.0 &&
         size.z > 0.0;
}

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
  return {minimum, maximum};
}

bool parseFiniteSetting(const QDomElement& element, const char* attribute, float fallback, float& output) {
  bool ok = true;
  const double parsed = element.hasAttribute(QLatin1String(attribute))
                            ? element.attribute(QLatin1String(attribute)).toDouble(&ok)
                            : static_cast<double>(fallback);
  if (!ok || !std::isfinite(parsed) || parsed < -std::numeric_limits<float>::max() ||
      parsed > std::numeric_limits<float>::max()) {
    return false;
  }
  output = static_cast<float>(parsed);
  return true;
}

}  // namespace

WasmVoxelGridLayer::WasmVoxelGridLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : PJ::ISceneLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)) {}

WasmVoxelGridLayer::~WasmVoxelGridLayer() = default;

PJ::SceneLayerInfo WasmVoxelGridLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kVoxelGrid,
      .display_name = display_name_,
      .family_name = u"VoxelGrid"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> WasmVoxelGridLayer::timeRange() const {
  return PJ::liveTopicTimeRange(session_ != nullptr ? &session_->objectStore() : nullptr, topic_id_);
}

bool WasmVoxelGridLayer::attach(const PJ::SceneLayerContext& context) {
  if (context.session == nullptr) {
    qCWarning(lcWasmVoxel) << "attach: session is null";
    return false;
  }
  session_ = context.session;
  if (!session_->parserBindingForObjectTopic(topic_id_) &&
      !hasCanonical3DCodec(PJ::sdk::BuiltinObjectType::kVoxelGrid)) {
    qCWarning(lcWasmVoxel) << "attach: no parser or canonical codec for topic" << topic_id_.id;
    session_ = nullptr;
    return false;
  }
  resetStreamingState();
  disconnect(reload_connection_);
  reload_connection_ = connect(
      session_, &PJ::SessionManager::datasetAboutToBeReplaced, this, &WasmVoxelGridLayer::onDatasetAboutToBeReplaced);
  if (!bootstrap()) {
    qCDebug(lcWasmVoxel) << "attach: first sample is not available yet for topic" << topic_id_.id;
  }
  const PJ::Range<PJ::Timepoint> range = timeRange();
  if (range.max >= range.min) {
    requested_time_ = range.min;
    decode_dirty_ = true;
  }
  return true;
}

void WasmVoxelGridLayer::detach() {
  disconnect(reload_connection_);
  reload_connection_ = {};
  clearVolume();
  resetStreamingState();
  session_ = nullptr;
  requested_time_.reset();
  decode_dirty_ = false;
  updateSourceFrame({});
  available_fields_.clear();
  resolved_field_name_.clear();
  setDataWarning({});
  noteRenderSuccess();
}

void WasmVoxelGridLayer::setTrackerTime(PJ::Timepoint time) {
  requested_time_ = time;
  decode_dirty_ = true;
  if (visible_) {
    emit repaintRequested();
  }
}

std::uint64_t WasmVoxelGridLayer::renderKey(PJ::Timepoint time) const {
  if (session_ == nullptr) {
    return PJ::kNoSampleRenderKey;
  }
  PJ::ObjectStore& store = session_->objectStore();
  const auto index = store.indexAt(topic_id_, PJ::toRaw(time));
  if (!index.has_value()) {
    return PJ::kNoSampleRenderKey;
  }
  const auto stamps = store.entryTimestamps(topic_id_);
  if (*index >= stamps.size()) {
    return PJ::kNoSampleRenderKey;
  }
  return static_cast<std::uint64_t>(stamps[*index]) ^ (static_cast<std::uint64_t>(*index) + 0x9e3779b97f4a7c15ULL);
}

void WasmVoxelGridLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  if (!visible_) {
    budget_rejected_voxel_count_.reset();
    clearVolume();
    staged_uid_ = {};
    staged_field_setting_ = "\x01";
  } else if (requested_time_.has_value()) {
    decode_dirty_ = true;
  }
  emit visibilityChanged(visible_);
  emit repaintRequested();
}

bool WasmVoxelGridLayer::prepareForRender(std::uint64_t remaining_view_voxels) {
  if (budget_rejected_voxel_count_.has_value()) {
    if (*budget_rejected_voxel_count_ > remaining_view_voxels) {
      return false;
    }
    budget_rejected_voxel_count_.reset();
    staged_uid_ = {};
    staged_field_setting_ = "\x01";
    decode_dirty_ = requested_time_.has_value();
  }
  if (hasVolume() && voxelCount() > remaining_view_voxels) {
    budget_rejected_voxel_count_ = voxelCount();
    setDataWarning(tr("Layer needs %1 voxels; only %2 remain in the browser view budget")
                       .arg(QString::number(voxelCount()), QString::number(remaining_view_voxels)));
    clearVolume();
    staged_uid_ = {};
    staged_field_setting_ = "\x01";
    return true;
  }
  if (!visible_ || !decode_dirty_ || !requested_time_.has_value()) {
    return false;
  }
  decode_dirty_ = false;
  const bool changed = decodeAt(*requested_time_);
  if (hasVolume() && voxelCount() > remaining_view_voxels) {
    budget_rejected_voxel_count_ = voxelCount();
    setDataWarning(tr("Layer needs %1 voxels; only %2 remain in the browser view budget")
                       .arg(QString::number(voxelCount()), QString::number(remaining_view_voxels)));
    clearVolume();
    staged_uid_ = {};
    staged_field_setting_ = "\x01";
    return true;
  }
  return changed;
}

bool WasmVoxelGridLayer::bootstrap() {
  if (session_ == nullptr) {
    return false;
  }
  const auto first = session_->objectStore().at(topic_id_, 0);
  if (!first.has_value() || first->payload.bytes.empty() ||
      !browserVoxelPayloadFits(static_cast<std::uint64_t>(first->payload.bytes.size()))) {
    return false;
  }
  const auto binding = session_->parserBindingForObjectTopic(topic_id_);
  auto object = resolveObject(binding, PJ::sdk::BuiltinObjectType::kVoxelGrid, first->timestamp, first->payload);
  if (!object.has_value()) {
    return false;
  }
  const auto* grid = object->object.get<PJ::sdk::VoxelGrid>();
  if (grid == nullptr) {
    return false;
  }
  updateSourceFrame(grid->frame_id);
  populateFields(*grid);
  resolveField(*grid);
  return true;
}

bool WasmVoxelGridLayer::decodeAt(PJ::Timepoint time) {
  if (session_ == nullptr) {
    return false;
  }
  PJ::ObjectStore& store = session_->objectStore();
  const auto entry = store.latestAt(topic_id_, PJ::toRaw(time));
  if (!entry.has_value() || entry->payload.bytes.empty()) {
    const bool changed = hasVolume();
    clearVolume();
    staged_uid_ = {};
    staged_field_setting_ = "\x01";
    setDataWarning({});
    return changed;
  }
  if (entry->sequential_uid == staged_uid_ && active_field_name_ == staged_field_setting_) {
    return false;
  }
  if (!browserVoxelPayloadFits(static_cast<std::uint64_t>(entry->payload.bytes.size()))) {
    setDataWarning(
        tr("Voxel-grid payload is %1 MiB; the browser limit is %2 MiB")
            .arg(QString::number(static_cast<double>(entry->payload.bytes.size()) / (1024.0 * 1024.0), 'f', 1))
            .arg(kMaxWireBytesPerSample / (1024ULL * 1024ULL)));
    clearVolume();
    staged_uid_ = entry->sequential_uid;
    staged_field_setting_ = active_field_name_;
    return true;
  }

  const auto binding = session_->parserBindingForObjectTopic(topic_id_);
  auto object = resolveObject(binding, PJ::sdk::BuiltinObjectType::kVoxelGrid, entry->timestamp, entry->payload);
  if (!object.has_value()) {
    setDataWarning(tr("Voxel grid could not be decoded: %1").arg(QString::fromStdString(object.error())));
    clearVolume();
    staged_uid_ = entry->sequential_uid;
    staged_field_setting_ = active_field_name_;
    return true;
  }
  const auto* grid = object->object.get<PJ::sdk::VoxelGrid>();
  if (grid == nullptr) {
    setDataWarning(tr("This browser layer accepts VoxelGrid samples only"));
    clearVolume();
    staged_uid_ = entry->sequential_uid;
    staged_field_setting_ = active_field_name_;
    return true;
  }

  updateSourceFrame(grid->frame_id);
  populateFields(*grid);
  if (!browserVoxelDimensionsFit(grid->column_count, grid->row_count, grid->slice_count)) {
    const std::uint64_t declared = pj::scene3d::voxelCount(*grid);
    setDataWarning(tr("Voxel grid is %1 x %2 x %3 (%4 voxels); the browser layer limit is %5")
                       .arg(
                           QString::number(grid->column_count), QString::number(grid->row_count),
                           QString::number(grid->slice_count), QString::number(declared),
                           QString::number(kMaxVoxelsPerLayer)));
    clearVolume();
    staged_uid_ = entry->sequential_uid;
    staged_field_setting_ = active_field_name_;
    return true;
  }
  if (!finitePositiveCellSize(grid->cell_size) || !finitePose(grid->origin)) {
    setDataWarning(tr("Voxel grid has an invalid cell size or origin pose"));
    clearVolume();
    staged_uid_ = entry->sequential_uid;
    staged_field_setting_ = active_field_name_;
    return true;
  }
  const PJ::sdk::PointField* field = resolveField(*grid);
  if (field == nullptr) {
    setDataWarning(tr("Voxel grid has no displayable fields"));
    clearVolume();
    staged_uid_ = entry->sequential_uid;
    staged_field_setting_ = active_field_name_;
    return true;
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
    setDataWarning(tr("Not enough browser memory to pack this voxel grid"));
    clearVolume();
    staged_uid_ = entry->sequential_uid;
    staged_field_setting_ = active_field_name_;
    return true;
  } catch (const std::exception& error) {
    setDataWarning(tr("Voxel-grid packing failed: %1").arg(QString::fromUtf8(error.what())));
    clearVolume();
    staged_uid_ = entry->sequential_uid;
    staged_field_setting_ = active_field_name_;
    return true;
  }
  const std::uint64_t count = pj::scene3d::voxelCount(*grid);
  const bool complete = value_kind_ == VoxelValueKind::kScalar ? scalar.size() == count : rgba.size() == count * 4U;
  if (!complete) {
    setDataWarning(tr("Voxel grid field layout could not be packed"));
    clearVolume();
    staged_uid_ = entry->sequential_uid;
    staged_field_setting_ = active_field_name_;
    return true;
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
  automatic_range_ =
      value_kind_ == VoxelValueKind::kScalar ? finiteRange(scalar_volume_) : std::pair<float, float>{0.0F, 1.0F};
  ++texture_revision_;
  staged_uid_ = entry->sequential_uid;
  staged_field_setting_ = active_field_name_;
  setDataWarning({});
  return true;
}

const PJ::sdk::PointField* WasmVoxelGridLayer::resolveField(const PJ::sdk::VoxelGrid& grid) {
  if (grid.fields.empty()) {
    resolved_field_name_.clear();
    return nullptr;
  }
  const PJ::sdk::PointField* field = active_field_name_.empty() ? nullptr : findField(grid.fields, active_field_name_);
  if (field != nullptr) {
    value_kind_ = voxelFieldKind(*field);
  } else {
    const VoxelFieldSelection selection = chooseDefaultField(grid.fields);
    if (selection.index < 0) {
      resolved_field_name_.clear();
      return nullptr;
    }
    field = &grid.fields[static_cast<std::size_t>(selection.index)];
    value_kind_ = selection.kind;
  }
  resolved_field_name_ = field->name;
  return field;
}

void WasmVoxelGridLayer::populateFields(const PJ::sdk::VoxelGrid& grid) {
  QStringList fields;
  fields.reserve(static_cast<qsizetype>(grid.fields.size()));
  for (const PJ::sdk::PointField& field : grid.fields) {
    fields.push_back(QString::fromStdString(field.name));
  }
  available_fields_ = std::move(fields);
}

void WasmVoxelGridLayer::clearVolume() {
  // clear() retains capacity, which would let a hidden, detached, or rejected
  // four-million-voxel layer keep roughly 16 MiB pinned in the WASM heap. The
  // decoded object is deliberately scoped to decodeAt(), so this vector is the
  // only retained CPU copy owned by the layer after packing.
  std::vector<float>{}.swap(scalar_volume_);
  std::vector<std::uint8_t>{}.swap(rgba_volume_);
  columns_ = 0;
  rows_ = 0;
  slices_ = 0;
  source_bounds_ = {};
  ++texture_revision_;
}

void WasmVoxelGridLayer::resetStreamingState() {
  staged_uid_ = {};
  staged_field_setting_ = "\x01";
  budget_rejected_voxel_count_.reset();
}

void WasmVoxelGridLayer::updateSourceFrame(const std::string& frame) {
  if (source_frame_ == frame) {
    return;
  }
  source_frame_ = frame;
  emit sourceFrameChanged(QString::fromStdString(source_frame_));
}

void WasmVoxelGridLayer::setActiveField(const QString& field) {
  const std::string next = field.toStdString();
  if (active_field_name_ == next) {
    return;
  }
  active_field_name_ = next;
  staged_uid_ = {};
  staged_field_setting_ = "\x01";
  decode_dirty_ = requested_time_.has_value();
  emit configurationChanged();
  emit repaintRequested();
}

void WasmVoxelGridLayer::setDrawMode(VoxelDrawMode mode) {
  if (draw_mode_ == mode) {
    return;
  }
  draw_mode_ = mode;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmVoxelGridLayer::setThreshold(float threshold) {
  if (threshold_ == threshold) {
    return;
  }
  threshold_ = threshold;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmVoxelGridLayer::setAutoRange(bool automatic) {
  if (auto_range_ == automatic) {
    return;
  }
  auto_range_ = automatic;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmVoxelGridLayer::setManualRange(float minimum, float maximum) {
  if (manual_range_lo_ == minimum && manual_range_hi_ == maximum) {
    return;
  }
  manual_range_lo_ = minimum;
  manual_range_hi_ = maximum;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmVoxelGridLayer::setColormap(PJ::Colormap colormap) {
  if (colormap_ == colormap) {
    return;
  }
  colormap_ = colormap;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmVoxelGridLayer::setOpacity(float opacity) {
  opacity = std::clamp(opacity, 0.0F, 1.0F);
  if (opacity_ == opacity) {
    return;
  }
  opacity_ = opacity;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmVoxelGridLayer::setDataWarning(QString warning) {
  if (data_warning_ == warning) {
    return;
  }
  data_warning_ = std::move(warning);
  updateWarning();
}

void WasmVoxelGridLayer::noteRenderFailure(QString warning) {
  if (render_warning_ == warning) {
    return;
  }
  render_warning_ = std::move(warning);
  updateWarning();
}

void WasmVoxelGridLayer::rejectVolumeForRender(QString warning) {
  noteRenderFailure(std::move(warning));
  clearVolume();
}

void WasmVoxelGridLayer::noteRenderSuccess() {
  if (render_warning_.isEmpty()) {
    return;
  }
  render_warning_.clear();
  updateWarning();
}

void WasmVoxelGridLayer::updateWarning() {
  const QString combined = !data_warning_.isEmpty() ? data_warning_ : render_warning_;
  if (warning_reason_ == combined) {
    return;
  }
  warning_reason_ = combined;
  emit warningChanged(!warning_reason_.isEmpty(), warning_reason_);
}

QWidget* WasmVoxelGridLayer::createConfigWidget(QWidget* parent) {
  auto* container = new QWidget(parent);
  auto* outer = new QVBoxLayout(container);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(PJ::theme::space(PJ::theme::Space::Snug));
  auto* form = new QFormLayout();
  form->setContentsMargins(0, 0, 0, 0);
  form->setHorizontalSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  form->setVerticalSpacing(PJ::theme::space(PJ::theme::Space::Snug));
  outer->addLayout(form);

  auto* field = new PJ::ComboBox(container);
  field->setObjectName(u"wasmVoxelField"_s);
  for (const QString& name : available_fields_) {
    field->addItem(name, name);
  }
  const int field_index = field->findData(QString::fromStdString(resolved_field_name_));
  if (field_index >= 0) {
    field->setCurrentIndex(field_index);
  }
  field->setEnabled(field->count() > 0);
  form->addRow(tr("Field:"), field);
  connect(field, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, field](int) {
    setActiveField(field->currentData().toString());
  });

  auto* colormap = new PJ::ComboBox(container);
  colormap->setObjectName(u"wasmVoxelColormap"_s);
  colormap->addItem(tr("Turbo"), static_cast<int>(PJ::Colormap::kTurbo));
  colormap->addItem(tr("Viridis"), static_cast<int>(PJ::Colormap::kViridis));
  colormap->addItem(tr("Plasma"), static_cast<int>(PJ::Colormap::kPlasma));
  colormap->addItem(tr("Grayscale"), static_cast<int>(PJ::Colormap::kGrayscale));
  colormap->setCurrentIndex(static_cast<int>(colormap_));
  form->addRow(tr("Colors:"), colormap);
  connect(colormap, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
    setColormap(index >= 0 && index < PJ::kColormapCount ? static_cast<PJ::Colormap>(index) : PJ::Colormap::kTurbo);
  });

  auto* mode = new PJ::ComboBox(container);
  mode->setObjectName(u"wasmVoxelDrawMode"_s);
  mode->addItem(tr("All cells"), static_cast<int>(VoxelDrawMode::kAll));
  mode->addItem(tr("Non-zero"), static_cast<int>(VoxelDrawMode::kNonZero));
  mode->addItem(tr("Threshold"), static_cast<int>(VoxelDrawMode::kThreshold));
  mode->addItem(tr("In range"), static_cast<int>(VoxelDrawMode::kRange));
  mode->setCurrentIndex(static_cast<int>(draw_mode_));
  form->addRow(tr("Draw:"), mode);
  connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
    setDrawMode(index >= 0 && index <= 3 ? static_cast<VoxelDrawMode>(index) : VoxelDrawMode::kNonZero);
  });

  auto* threshold = new PJ::DoubleScrubber(container);
  threshold->setObjectName(u"wasmVoxelThreshold"_s);
  threshold->setRange(-1.0e9, 1.0e9);
  threshold->setDecimals(3);
  threshold->setValue(threshold_);
  form->addRow(tr("Threshold:"), threshold);
  connect(threshold, &PJ::DoubleScrubber::valueChanged, this, [this](double value) {
    setThreshold(static_cast<float>(value));
  });

  auto* automatic = new QCheckBox(tr("Auto colormap range"), container);
  automatic->setObjectName(u"wasmVoxelAutoRange"_s);
  automatic->setChecked(auto_range_);
  form->addRow(QString(), automatic);

  auto* range_minimum = new PJ::DoubleScrubber(container);
  range_minimum->setObjectName(u"wasmVoxelRangeMin"_s);
  range_minimum->setRange(-1.0e9, 1.0e9);
  range_minimum->setDecimals(3);
  range_minimum->setValue(manual_range_lo_);
  form->addRow(tr("Range min:"), range_minimum);

  auto* range_maximum = new PJ::DoubleScrubber(container);
  range_maximum->setObjectName(u"wasmVoxelRangeMax"_s);
  range_maximum->setRange(-1.0e9, 1.0e9);
  range_maximum->setDecimals(3);
  range_maximum->setValue(manual_range_hi_);
  form->addRow(tr("Range max:"), range_maximum);

  const auto sync_range_enabled = [this, range_minimum, range_maximum]() {
    const bool enabled = !auto_range_ || draw_mode_ == VoxelDrawMode::kRange;
    range_minimum->setEnabled(enabled);
    range_maximum->setEnabled(enabled);
  };
  sync_range_enabled();
  connect(automatic, &QCheckBox::toggled, this, [this, sync_range_enabled](bool checked) {
    setAutoRange(checked);
    sync_range_enabled();
  });
  connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [sync_range_enabled](int) {
    sync_range_enabled();
  });
  connect(range_minimum, &PJ::DoubleScrubber::valueChanged, this, [this, range_maximum](double value) {
    setManualRange(static_cast<float>(value), static_cast<float>(range_maximum->value()));
  });
  connect(range_maximum, &PJ::DoubleScrubber::valueChanged, this, [this, range_minimum](double value) {
    setManualRange(static_cast<float>(range_minimum->value()), static_cast<float>(value));
  });

  auto* opacity = new PJ::DoubleScrubber(container);
  opacity->setObjectName(u"wasmVoxelOpacity"_s);
  opacity->setRange(0.0, 1.0);
  opacity->setDecimals(2);
  opacity->setSingleStep(0.05);
  opacity->setValue(opacity_);
  form->addRow(tr("Opacity:"), opacity);
  connect(opacity, &PJ::DoubleScrubber::valueChanged, this, [this](double value) {
    setOpacity(static_cast<float>(value));
  });
  return container;
}

QDomElement WasmVoxelGridLayer::xmlSaveState(QDomDocument& document) const {
  QDomElement element = document.createElement(u"voxel_grid"_s);
  element.setAttribute(u"field"_s, QString::fromStdString(active_field_name_));
  element.setAttribute(u"draw_mode"_s, static_cast<int>(draw_mode_));
  element.setAttribute(u"threshold"_s, static_cast<double>(threshold_));
  element.setAttribute(u"auto_range"_s, auto_range_ ? 1 : 0);
  element.setAttribute(u"range_lo"_s, static_cast<double>(manual_range_lo_));
  element.setAttribute(u"range_hi"_s, static_cast<double>(manual_range_hi_));
  element.setAttribute(u"colormap"_s, static_cast<int>(colormap_));
  element.setAttribute(u"opacity"_s, static_cast<double>(opacity_));
  return element;
}

std::optional<WasmVoxelGridLayer::ParsedSettings> WasmVoxelGridLayer::parseSettings(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "voxel_grid"_L1 || !detail::isLeafPayload(element)) {
    return std::nullopt;
  }
  ParsedSettings settings;
  settings.field = element.attribute(u"field"_s).toStdString();
  bool draw_ok = false;
  const int draw_mode = element.attribute(u"draw_mode"_s, u"1"_s).toInt(&draw_ok);
  bool auto_ok = false;
  const int auto_range = element.attribute(u"auto_range"_s, u"1"_s).toInt(&auto_ok);
  bool colormap_ok = false;
  const int colormap = element.attribute(u"colormap"_s, u"0"_s).toInt(&colormap_ok);
  if (!draw_ok || draw_mode < 0 || draw_mode > 3 || !auto_ok || (auto_range != 0 && auto_range != 1) || !colormap_ok ||
      colormap < 0 || colormap >= PJ::kColormapCount ||
      !parseFiniteSetting(element, "threshold", 0.0F, settings.threshold) ||
      !parseFiniteSetting(element, "range_lo", 0.0F, settings.range_lo) ||
      !parseFiniteSetting(element, "range_hi", 1.0F, settings.range_hi) || settings.range_lo > settings.range_hi ||
      !detail::parseFiniteFloat(element, "opacity", 1.0F, 0.0F, 1.0F, settings.opacity)) {
    return std::nullopt;
  }
  settings.draw_mode = static_cast<VoxelDrawMode>(draw_mode);
  settings.auto_range = auto_range != 0;
  settings.colormap = static_cast<PJ::Colormap>(colormap);
  return settings;
}

bool WasmVoxelGridLayer::validateXml(const QDomElement& element) {
  return parseSettings(element).has_value();
}

bool WasmVoxelGridLayer::xmlLoadState(const QDomElement& element) {
  const auto settings = parseSettings(element);
  if (!settings.has_value()) {
    return false;
  }
  setActiveField(QString::fromStdString(settings->field));
  setDrawMode(settings->draw_mode);
  setThreshold(settings->threshold);
  setAutoRange(settings->auto_range);
  setManualRange(settings->range_lo, settings->range_hi);
  setColormap(settings->colormap);
  setOpacity(settings->opacity);
  return true;
}

void WasmVoxelGridLayer::onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id) {
  if (session_ == nullptr || session_->objectStore().descriptor(topic_id_).dataset_id != dataset_id) {
    return;
  }
  clearVolume();
  resetStreamingState();
  decode_dirty_ = requested_time_.has_value();
  updateSourceFrame({});
  available_fields_.clear();
  resolved_field_name_.clear();
  setDataWarning({});
  noteRenderSuccess();
  emit repaintRequested();
}

}  // namespace pj::scene3d
