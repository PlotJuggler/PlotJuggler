// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/wasm/occupancy_grid_layer_wasm.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLoggingCategory>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <any>
#include <cmath>
#include <limits>
#include <utility>

#include "layers/layer_xml_validation.h"
#include "pj_base/builtin/occupancy_grid_update.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/resolve_object.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/FrameworkTokens.h"

using namespace Qt::StringLiterals;

namespace pj::scene3d {
namespace {

Q_LOGGING_CATEGORY(lcWasmOccupancy, "pj.scene3d.wasm.occupancy_grid")

bool finitePose(const PJ::sdk::Pose& pose) {
  const auto& p = pose.position;
  const auto& q = pose.orientation;
  const double norm2 = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(q.x) && std::isfinite(q.y) &&
         std::isfinite(q.z) && std::isfinite(q.w) && std::isfinite(norm2) && norm2 > 1.0e-18;
}

}  // namespace

WasmOccupancyGridLayer::WasmOccupancyGridLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : PJ::ISceneLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)) {}

WasmOccupancyGridLayer::~WasmOccupancyGridLayer() = default;

PJ::SceneLayerInfo WasmOccupancyGridLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kOccupancyGrid,
      .display_name = display_name_,
      .family_name = u"OccupancyGrid"_s,
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> WasmOccupancyGridLayer::timeRange() const {
  const PJ::ObjectStore* store = session_ != nullptr ? &session_->objectStore() : nullptr;
  PJ::Range<PJ::Timepoint> range = PJ::liveTopicTimeRange(store, topic_id_);
  if (updates_topic_.has_value()) {
    const PJ::Range<PJ::Timepoint> updates = PJ::liveTopicTimeRange(store, *updates_topic_);
    range.min = std::min(range.min, updates.min);
    range.max = std::max(range.max, updates.max);
  }
  return range;
}

bool WasmOccupancyGridLayer::attach(const PJ::SceneLayerContext& context) {
  if (context.session == nullptr) {
    qCWarning(lcWasmOccupancy) << "attach: session is null";
    return false;
  }
  session_ = context.session;
  if (!session_->parserBindingForObjectTopic(topic_id_) &&
      !hasCanonical3DCodec(PJ::sdk::BuiltinObjectType::kOccupancyGrid)) {
    qCWarning(lcWasmOccupancy) << "attach: no parser or canonical codec for topic" << topic_id_.id;
    session_ = nullptr;
    return false;
  }
  const PJ::ObjectTopicDescriptor& descriptor = session_->objectStore().descriptor(topic_id_);
  updates_topic_ = session_->objectStore().findTopic(descriptor.dataset_id, descriptor.topic_name + "_updates");
  resetStreamingState();
  disconnect(reload_connection_);
  reload_connection_ = connect(
      session_, &PJ::SessionManager::datasetAboutToBeReplaced, this,
      &WasmOccupancyGridLayer::onDatasetAboutToBeReplaced);
  if (!bootstrap()) {
    qCDebug(lcWasmOccupancy) << "attach: first sample is not available yet for topic" << topic_id_.id;
  }
  const PJ::Range<PJ::Timepoint> range = timeRange();
  if (range.max >= range.min) {
    requested_time_ = range.min;
    decode_dirty_ = true;
  }
  return true;
}

void WasmOccupancyGridLayer::detach() {
  disconnect(reload_connection_);
  reload_connection_ = {};
  stageEmpty();
  resetStreamingState();
  session_ = nullptr;
  updates_topic_.reset();
  requested_time_.reset();
  decode_dirty_ = false;
  updateSourceFrame({});
  setDataWarning({});
  noteRenderSuccess();
}

void WasmOccupancyGridLayer::setTrackerTime(PJ::Timepoint time) {
  requested_time_ = time;
  decode_dirty_ = true;
  if (visible_) {
    emit repaintRequested();
  }
}

std::uint64_t WasmOccupancyGridLayer::renderKey(PJ::Timepoint time) const {
  if (session_ == nullptr) {
    return PJ::kNoSampleRenderKey;
  }
  const PJ::Timestamp stamp = PJ::toRaw(time);
  const auto base = session_->objectStore().latestAt(topic_id_, stamp);
  if (!base.has_value()) {
    return PJ::kNoSampleRenderKey;
  }
  std::uint64_t key = base->sequential_uid.value;
  if (updates_topic_.has_value()) {
    // Fingerprint the highest ARRIVAL UID in the consumed prefix, not merely
    // latestAt(stamp): a retroactive patch can have an older timestamp while
    // carrying the newest UID. That arrival must break the dock's repaint
    // coalescing gate so reconstructAt can run its late-update rewind.
    const std::uint64_t update_uid = session_->objectStore().maxUidAtOrBefore(*updates_topic_, stamp).value;
    key ^= update_uid + 0x9e3779b97f4a7c15ULL + (key << 6U) + (key >> 2U);
  }
  return key;
}

void WasmOccupancyGridLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  if (!visible_) {
    budget_rejected_cell_count_.reset();
    stageEmpty();
  } else if (requested_time_.has_value()) {
    reconstructor_.invalidate();
    base_cache_.reset();
    base_cache_uid_ = {};
    decode_dirty_ = true;
  }
  emit visibilityChanged(visible_);
  emit repaintRequested();
}

bool WasmOccupancyGridLayer::prepareForRender(std::uint64_t remaining_view_cells) {
  if (budget_rejected_cell_count_.has_value()) {
    if (*budget_rejected_cell_count_ > remaining_view_cells) {
      return false;
    }
    budget_rejected_cell_count_.reset();
    reconstructor_.invalidate();
    base_cache_.reset();
    base_cache_uid_ = {};
    decode_dirty_ = requested_time_.has_value();
  }
  if (!visible_ || !decode_dirty_ || !requested_time_.has_value()) {
    return false;
  }
  decode_dirty_ = false;
  const bool changed = reconstructAt(*requested_time_);
  const std::uint64_t cells = static_cast<std::uint64_t>(grid().width) * grid().height;
  if (!grid().empty() && cells > remaining_view_cells) {
    budget_rejected_cell_count_ = cells;
    setDataWarning(tr("Layer needs %1 occupancy cells; only %2 remain in the browser view budget")
                       .arg(QString::number(cells), QString::number(remaining_view_cells)));
    stageEmpty();
    reconstructor_.invalidate();
    return true;
  }
  return changed;
}

bool WasmOccupancyGridLayer::bootstrap() {
  if (session_ == nullptr) {
    return false;
  }
  const auto first = session_->objectStore().at(topic_id_, 0);
  if (!first.has_value() || first->payload.bytes.empty() ||
      !browserOccupancyPayloadFits(static_cast<std::uint64_t>(first->payload.bytes.size()))) {
    return false;
  }
  const auto binding = session_->parserBindingForObjectTopic(topic_id_);
  auto object = resolveObject(binding, PJ::sdk::BuiltinObjectType::kOccupancyGrid, first->timestamp, first->payload);
  if (!object.has_value()) {
    return false;
  }
  const auto* grid = object->object.get<PJ::sdk::OccupancyGrid>();
  if (grid == nullptr) {
    return false;
  }
  updateSourceFrame(grid->frame_id);
  return true;
}

bool WasmOccupancyGridLayer::reconstructAt(PJ::Timepoint time) {
  if (session_ == nullptr) {
    return false;
  }
  PJ::ObjectStore& store = session_->objectStore();
  const PJ::Timestamp time_ns = PJ::toRaw(time);
  const auto base_binding = session_->parserBindingForObjectTopic(topic_id_);
  const auto updates_binding = updates_topic_.has_value() ? session_->parserBindingForObjectTopic(*updates_topic_)
                                                          : PJ::SessionManager::ParserBinding{};

  PJ::SequentialUID cursor_candidate = updates_cursor_;
  if (updates_topic_.has_value()) {
    const PJ::SequentialUID high_at_time = store.maxUidAtOrBefore(*updates_topic_, time_ns);
    cursor_candidate = std::max(cursor_candidate, high_at_time);
    if (last_consumed_time_.has_value() && high_at_time > updates_cursor_ &&
        store.maxUidAtOrBefore(*updates_topic_, *last_consumed_time_) > updates_cursor_) {
      const auto retroactive =
          store.rangeByTime(*updates_topic_, std::numeric_limits<PJ::Timestamp>::min(), *last_consumed_time_);
      for (const auto& reference : retroactive) {
        if (reference.uid > updates_cursor_) {
          reconstructor_.invalidateAfter(reference.timestamp);
          last_consumed_time_.reset();
          break;
        }
      }
    }
  }

  QString rejection;
  QString update_warning;
  using BaseSample = OccupancyGridReconstructor::BaseSample;
  auto base_at = [this, &store, &base_binding, &rejection](PJ::Timestamp stamp) -> std::optional<BaseSample> {
    const auto entry = store.latestAt(topic_id_, stamp);
    if (!entry.has_value() || entry->payload.bytes.empty()) {
      return std::nullopt;
    }
    if (base_cache_.has_value() && entry->sequential_uid == base_cache_uid_) {
      return BaseSample{*base_cache_, base_cache_uid_.value};
    }
    if (!browserOccupancyPayloadFits(static_cast<std::uint64_t>(entry->payload.bytes.size()))) {
      rejection = tr("Occupancy-grid payload is %1 MiB; the browser limit is %2 MiB")
                      .arg(
                          QString::number(static_cast<double>(entry->payload.bytes.size()) / (1024.0 * 1024.0), 'f', 1),
                          QString::number(kMaxWireBytesPerSample / (1024ULL * 1024ULL)));
      return std::nullopt;
    }
    auto object =
        resolveObject(base_binding, PJ::sdk::BuiltinObjectType::kOccupancyGrid, entry->timestamp, entry->payload);
    if (!object.has_value()) {
      rejection = tr("Occupancy grid could not be decoded: %1").arg(QString::fromStdString(object.error()));
      return std::nullopt;
    }
    const auto* decoded = object->object.get<PJ::sdk::OccupancyGrid>();
    if (decoded == nullptr) {
      rejection = tr("This browser layer accepts OccupancyGrid base samples only");
      return std::nullopt;
    }
    if (!browserOccupancyDimensionsFit(decoded->width, decoded->height)) {
      rejection = tr("Occupancy grid is %1 x %2; the browser limit is %3 cells and %4 per dimension")
                      .arg(
                          QString::number(decoded->width), QString::number(decoded->height),
                          QString::number(kMaxCellsPerLayer), QString::number(kBrowserMaxOccupancyTextureDimension));
      return std::nullopt;
    }
    const double width_metres = decoded->resolution * static_cast<double>(decoded->width);
    const double height_metres = decoded->resolution * static_cast<double>(decoded->height);
    if (!std::isfinite(decoded->resolution) || decoded->resolution <= 0.0 || !std::isfinite(width_metres) ||
        !std::isfinite(height_metres) || !finitePose(decoded->origin)) {
      rejection = tr("Occupancy grid has an invalid resolution or origin pose");
      return std::nullopt;
    }
    base_cache_uid_ = entry->sequential_uid;
    base_cache_ = *decoded;
    return BaseSample{*base_cache_, base_cache_uid_.value};
  };

  std::uint64_t update_window_bytes = 0;
  std::optional<PJ::Timestamp> skipped_update_timestamp;
  const auto note_skipped_update = [&update_warning, &skipped_update_timestamp](
                                       PJ::Timestamp timestamp, QString warning) {
    if (!skipped_update_timestamp.has_value() || timestamp < *skipped_update_timestamp) {
      skipped_update_timestamp = timestamp;
      update_warning = std::move(warning);
    }
  };
  auto updates_in = [this, &store, &updates_binding, &rejection, &update_window_bytes, &note_skipped_update](
                        PJ::Timestamp lower, PJ::Timestamp upper) {
    std::vector<PJ::sdk::OccupancyGridUpdate> output;
    if (!updates_topic_.has_value()) {
      return output;
    }
    const PJ::ObjectTopicId id = *updates_topic_;
    const auto window = store.rangeByTime(id, lower, upper);
    output.reserve(window.size());
    for (const auto& reference : window) {
      const auto entry = store.at(id, reference.uid);
      if (entry.has_value() && entry->fetch_failed) {
        // Permanent loss (re-read failed); surface via the skip channel, don't retry.
        note_skipped_update(entry->timestamp, tr("Occupancy update lost: source re-read failed"));
        continue;
      }
      if (!entry.has_value() || entry->payload.bytes.empty()) {
        continue;
      }
      if (!browserOccupancyPayloadFits(static_cast<std::uint64_t>(entry->payload.bytes.size()))) {
        rejection = tr("Occupancy update payload exceeds the %1 MiB browser sample limit")
                        .arg(kMaxWireBytesPerSample / (1024ULL * 1024ULL));
        return std::vector<PJ::sdk::OccupancyGridUpdate>{};
      }
      auto object = resolveObject(
          updates_binding, PJ::sdk::BuiltinObjectType::kOccupancyGridUpdate, entry->timestamp, entry->payload);
      if (!object.has_value()) {
        note_skipped_update(
            entry->timestamp, tr("Occupancy update could not be decoded and was skipped: %1")
                                  .arg(QString::fromStdString(object.error())));
        continue;
      }
      const auto* update = object->object.get<PJ::sdk::OccupancyGridUpdate>();
      if (update == nullptr) {
        note_skipped_update(entry->timestamp, tr("A paired update sample was not OccupancyGridUpdate and was skipped"));
        continue;
      }
      const std::uint64_t declared = static_cast<std::uint64_t>(update->width) * update->height;
      if (declared > update->data.size()) {
        note_skipped_update(
            entry->timestamp, tr("Occupancy update data was shorter than its declared rectangle and was skipped"));
        continue;
      }
      if (!browserOccupancyUpdateWindowFits(declared) || declared > kMaxUpdateWindowBytes - update_window_bytes) {
        rejection = tr("Occupancy updates in this seek window exceed the %1 MiB browser limit")
                        .arg(kMaxUpdateWindowBytes / (1024ULL * 1024ULL));
        return std::vector<PJ::sdk::OccupancyGridUpdate>{};
      }
      update_window_bytes += declared;
      output.push_back(*update);
    }
    return output;
  };

  const GridUpdate update = reconstructor_.reconstructAt(time_ns, base_at, updates_in);
  updates_cursor_ = cursor_candidate;
  last_consumed_time_ = last_consumed_time_.has_value() ? std::max(*last_consumed_time_, time_ns) : time_ns;
  if (!rejection.isEmpty()) {
    reconstructor_.invalidate();
    stageEmpty();
    setDataWarning(std::move(rejection));
    return true;
  }
  // Repeated compositor requests at the same tracker time have an empty
  // incremental window and cannot rediscover a skipped patch. Retain the soft
  // warning until the tracker moves before that patch or a later base epoch
  // supersedes it; a later forward replay will rediscover it if relevant again.
  if (skipped_update_warning_timestamp_.has_value() &&
      (time_ns < *skipped_update_warning_timestamp_ ||
       (!update.grid.empty() && update.grid.base_timestamp_ns > *skipped_update_warning_timestamp_))) {
    skipped_update_warning_timestamp_.reset();
    skipped_update_warning_.clear();
  }
  if (skipped_update_timestamp.has_value()) {
    skipped_update_warning_timestamp_ = skipped_update_timestamp;
    skipped_update_warning_ = std::move(update_warning);
  }
  if (update.grid.empty()) {
    stageEmpty();
    setDataWarning(skipped_update_warning_);
    return true;
  }
  updateSourceFrame(update.grid.frame_id);
  stageGrid(update);
  setDataWarning(skipped_update_warning_);
  return true;
}

void WasmOccupancyGridLayer::resetStreamingState() {
  base_cache_.reset();
  base_cache_uid_ = {};
  updates_cursor_ = {};
  last_consumed_time_.reset();
  skipped_update_warning_timestamp_.reset();
  skipped_update_warning_.clear();
  budget_rejected_cell_count_.reset();
  reconstructor_.invalidate();
  dirty_rects_.clear();
}

void WasmOccupancyGridLayer::stageGrid(const GridUpdate& update) {
  const bool incremental = update.kind == GridUpdate::Kind::kIncremental;
  if (incremental && update.dirty.empty() && grid_staged_) {
    return;
  }
  full_upload_pending_ = !incremental || !grid_staged_;
  dirty_rects_.assign(update.dirty.begin(), update.dirty.end());
  grid_staged_ = true;
  ++texture_revision_;
}

void WasmOccupancyGridLayer::stageEmpty() {
  dirty_rects_.clear();
  full_upload_pending_ = true;
  if (grid_staged_) {
    grid_staged_ = false;
    ++texture_revision_;
  }
}

void WasmOccupancyGridLayer::updateSourceFrame(const std::string& frame) {
  if (source_frame_ == frame) {
    return;
  }
  source_frame_ = frame;
  emit sourceFrameChanged(QString::fromStdString(source_frame_));
}

void WasmOccupancyGridLayer::setColorScheme(ColorScheme scheme) {
  if (color_scheme_ == scheme) {
    return;
  }
  color_scheme_ = scheme;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmOccupancyGridLayer::setOpacity(float opacity) {
  opacity = std::clamp(opacity, 0.0F, 1.0F);
  if (opacity_ == opacity) {
    return;
  }
  opacity_ = opacity;
  emit configurationChanged();
  emit repaintRequested();
}

void WasmOccupancyGridLayer::setDataWarning(QString warning) {
  if (data_warning_ == warning) {
    return;
  }
  data_warning_ = std::move(warning);
  updateWarning();
}

void WasmOccupancyGridLayer::noteRenderFailure(QString warning) {
  if (render_warning_ == warning) {
    return;
  }
  render_warning_ = std::move(warning);
  updateWarning();
}

void WasmOccupancyGridLayer::noteRenderSuccess() {
  if (render_warning_.isEmpty()) {
    return;
  }
  render_warning_.clear();
  updateWarning();
}

void WasmOccupancyGridLayer::updateWarning() {
  const QString combined = !data_warning_.isEmpty() ? data_warning_ : render_warning_;
  if (warning_reason_ == combined) {
    return;
  }
  warning_reason_ = combined;
  emit warningChanged(!warning_reason_.isEmpty(), warning_reason_);
}

QWidget* WasmOccupancyGridLayer::createConfigWidget(QWidget* parent) {
  auto* container = new QWidget(parent);
  auto* outer = new QVBoxLayout(container);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(PJ::theme::space(PJ::theme::Space::Snug));
  auto* form = new QFormLayout();
  form->setContentsMargins(0, 0, 0, 0);
  form->setHorizontalSpacing(PJ::theme::space(PJ::theme::Space::Comfortable));
  form->setVerticalSpacing(PJ::theme::space(PJ::theme::Space::Snug));
  outer->addLayout(form);

  auto* scheme = new PJ::ComboBox(container);
  scheme->setObjectName(u"wasmOccupancyColorScheme"_s);
  scheme->addItem(tr("Map (grayscale)"), static_cast<int>(ColorScheme::kMap));
  scheme->addItem(tr("Costmap"), static_cast<int>(ColorScheme::kCostmap));
  scheme->setCurrentIndex(color_scheme_ == ColorScheme::kCostmap ? 1 : 0);
  form->addRow(tr("Colors:"), scheme);
  connect(scheme, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
    setColorScheme(index == 1 ? ColorScheme::kCostmap : ColorScheme::kMap);
  });

  auto* opacity = new PJ::DoubleScrubber(container);
  opacity->setObjectName(u"wasmOccupancyOpacity"_s);
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

QDomElement WasmOccupancyGridLayer::xmlSaveState(QDomDocument& document) const {
  QDomElement element = document.createElement(u"occupancy_grid"_s);
  element.setAttribute(u"color_scheme"_s, color_scheme_ == ColorScheme::kCostmap ? u"costmap"_s : u"map"_s);
  element.setAttribute(u"opacity"_s, static_cast<double>(opacity_));
  return element;
}

std::optional<WasmOccupancyGridLayer::ParsedSettings> WasmOccupancyGridLayer::parseSettings(
    const QDomElement& element) {
  if (element.isNull() || element.tagName() != "occupancy_grid"_L1 || !detail::isLeafPayload(element)) {
    return std::nullopt;
  }
  ParsedSettings settings;
  const QString scheme = element.attribute(u"color_scheme"_s, u"map"_s);
  if (scheme == "map"_L1) {
    settings.color_scheme = ColorScheme::kMap;
  } else if (scheme == "costmap"_L1) {
    settings.color_scheme = ColorScheme::kCostmap;
  } else {
    return std::nullopt;
  }
  if (!detail::parseFiniteFloat(element, "opacity", 0.7F, 0.0F, 1.0F, settings.opacity)) {
    return std::nullopt;
  }
  return settings;
}

bool WasmOccupancyGridLayer::validateXml(const QDomElement& element) {
  return parseSettings(element).has_value();
}

bool WasmOccupancyGridLayer::xmlLoadState(const QDomElement& element) {
  const auto settings = parseSettings(element);
  if (!settings.has_value()) {
    return false;
  }
  setColorScheme(settings->color_scheme);
  setOpacity(settings->opacity);
  return true;
}

void WasmOccupancyGridLayer::onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id) {
  if (session_ == nullptr || session_->objectStore().descriptor(topic_id_).dataset_id != dataset_id) {
    return;
  }
  stageEmpty();
  resetStreamingState();
  decode_dirty_ = requested_time_.has_value();
  setDataWarning({});
  noteRenderSuccess();
  emit repaintRequested();
}

}  // namespace pj::scene3d
