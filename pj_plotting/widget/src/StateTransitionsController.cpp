// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/StateTransitionsController.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/CurveDisplayName.h"  // curveDisplayName — the shared "topic/field" legend label
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_widgets/LayerListView.h"  // detail::reorderIds — the shared drop arithmetic
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
// Streaming ingest / range updates collapse to ~10 Hz of row refreshes — a full
// discrete-series re-scan per batch is cheap, but not per-sample cheap.
constexpr int kRefreshIntervalMs = 100;
}  // namespace

StateTransitionsController::StateTransitionsController(
    StateTransitionsView* view, SessionManager* session, CatalogModel* catalog, PlaybackEngine* playback,
    QObject* parent)
    : QObject(parent),
      view_(view),
      session_(session),
      catalog_(catalog),
      playback_(playback),
      refresh_trigger_(kRefreshIntervalMs, [this]() { refreshNow(); }) {
  view_->setDroppablePredicate(
      [catalog](const QString& key) { return catalog != nullptr && catalog->isDiscreteKey(key); });
  connect(view_, &StateTransitionsView::seriesDropped, this, [this](const QStringList& keys) {
    bool any_added = false;
    for (const QString& key : keys) {
      any_added = addSeries(key) || any_added;
    }
  });
  connectRuntime();
  refresh_trigger_.request();
}

void StateTransitionsController::connectRuntime() {
  if (session_ != nullptr) {
    connect(session_, &SessionManager::samplesIngested, this, [this](const QVector<TopicId>& ids, bool /*live*/) {
      markTopicsDirty(ids);
    });
    // SYNCHRONOUS by contract: adapters drop their cached data before the
    // dataset's storage is swapped out underneath them.
    connect(
        session_, &SessionManager::datasetAboutToBeReplaced, this,
        [this](DatasetId dataset_id) {
          for (RowBinding& binding : rows_) {
            if (binding.adapter->source().dataset_id == dataset_id) {
              binding.adapter->clear();
              binding.dirty = true;
            }
          }
          refresh_trigger_.request();
        },
        Qt::DirectConnection);
    // Per-dataset alignment change: pure re-map of cached raw segments.
    connect(session_, qOverload<DatasetId>(&SessionManager::displayOffsetChanged), this, [this](DatasetId dataset_id) {
      bool any = false;
      for (const RowBinding& binding : rows_) {
        if (binding.adapter->source().dataset_id == dataset_id) {
          view_->updateRow(displayRow(binding));
          any = true;
        }
      }
      Q_UNUSED(any);
    });
    // Global frame flip ("use time offset"): re-map everything (ruler labels
    // follow automatically — they always print the display-axis value).
    connect(session_, qOverload<>(&SessionManager::displayOffsetChanged), this, [this]() {
      range_dirty_ = true;
      for (const RowBinding& binding : rows_) {
        view_->updateRow(displayRow(binding));
      }
      refresh_trigger_.request();
    });
  }

  if (catalog_ != nullptr) {
    connect(catalog_, &CatalogModel::itemsRemoved, this, [this](const QStringList& keys) {
      const QSet<QString> removed(keys.begin(), keys.end());
      const std::size_t before = rows_.size();
      std::erase_if(rows_, [&removed](const RowBinding& binding) { return removed.contains(binding.catalog_key); });
      if (rows_.size() != before) {
        pushAllRows();
        emit seriesListChanged();
      }
    });
    // A surviving key whose item changed (e.g. a float field's first fractional
    // value makes it non-discrete) re-resolves on the next refresh. Keyed on the
    // catalog's own signal so it does not depend on the catalog's samplesIngested
    // slot having run before ours.
    connect(catalog_, &CatalogModel::itemsChanged, this, [this](const QStringList& keys) {
      const QSet<QString> changed(keys.begin(), keys.end());
      bool any_dirty = false;
      for (RowBinding& binding : rows_) {
        if (changed.contains(binding.catalog_key)) {
          binding.dirty = true;
          any_dirty = true;
        }
      }
      if (any_dirty) {
        refresh_trigger_.request();
      }
    });
    connect(catalog_, &CatalogModel::cleared, this, [this]() {
      if (!rows_.empty()) {
        rows_.clear();
        pushAllRows();
        emit seriesListChanged();
      }
    });
  }

  if (playback_ != nullptr) {
    connect(playback_, &PlaybackEngine::rangeChanged, this, [this](double /*min*/, double /*max*/) {
      // The trailing open segment is pinned to the range max, so a range change
      // re-maps rows too — coalesced with ingest refreshes.
      range_dirty_ = true;
      refresh_trigger_.request();
    });
  }
}

bool StateTransitionsController::addSeries(const QString& catalog_key) {
  if (catalog_ == nullptr || session_ == nullptr) {
    return false;
  }
  const bool duplicate = std::ranges::any_of(
      rows_, [&catalog_key](const RowBinding& binding) { return binding.catalog_key == catalog_key; });
  if (duplicate) {
    return false;
  }
  const std::optional<CatalogItem> item = catalog_->itemDescriptor(catalog_key);
  const ScalarFieldPayload* scalar = item.has_value() ? asScalarField(*item) : nullptr;
  // The kDiscrete descriptor doubles as the gate: only discrete fields
  // (string / integer / bool / whole-number float) form rows.
  std::optional<CurveDescriptor> descriptor = catalog_->curveDescriptor(catalog_key, SeriesCapability::kDiscrete);
  if (scalar == nullptr || !descriptor.has_value()) {
    return false;
  }

  RowBinding binding{
      .row_id = next_row_id_++,
      .catalog_key = catalog_key,
      .adapter = std::make_unique<StateSeriesAdapter>(session_, std::move(*descriptor), scalar->logical_type),
      .dirty = false,
  };
  binding.adapter->rebuild();
  rows_.push_back(std::move(binding));
  pushAllRows();
  emit seriesListChanged();
  return true;
}

bool StateTransitionsController::removeSeries(quint64 row_id) {
  const std::size_t before = rows_.size();
  std::erase_if(rows_, [row_id](const RowBinding& binding) { return binding.row_id == row_id; });
  if (rows_.size() == before) {
    return false;
  }
  pushAllRows();
  emit seriesListChanged();
  return true;
}

bool StateTransitionsController::reorderSeries(int from, int to) {
  std::vector<qint64> ids;
  ids.reserve(rows_.size());
  for (const RowBinding& binding : rows_) {
    ids.push_back(static_cast<qint64>(binding.row_id));
  }
  const std::vector<qint64> new_order = detail::reorderIds(ids, from, to);
  if (new_order == ids) {
    return false;
  }
  std::vector<RowBinding> reordered;
  reordered.reserve(rows_.size());
  for (const qint64 row_id : new_order) {
    auto match = std::ranges::find_if(
        rows_, [row_id](const RowBinding& binding) { return binding.row_id == static_cast<quint64>(row_id); });
    reordered.push_back(std::move(*match));
  }
  rows_ = std::move(reordered);
  pushAllRows();
  emit seriesListChanged();
  return true;
}

std::vector<StateTransitionsController::SeriesEntry> StateTransitionsController::seriesEntries() const {
  std::vector<SeriesEntry> entries;
  entries.reserve(rows_.size());
  for (const RowBinding& binding : rows_) {
    entries.push_back(
        {.row_id = binding.row_id,
         .name = curveDisplayName(binding.adapter->source()),  // same "topic/field" label as the strip + legend
         .visible = binding.visible});
  }
  return entries;
}

bool StateTransitionsController::setSeriesVisible(quint64 row_id, bool visible) {
  const auto binding =
      std::ranges::find_if(rows_, [row_id](const RowBinding& candidate) { return candidate.row_id == row_id; });
  if (binding == rows_.end() || binding->visible == visible) {
    return binding != rows_.end();
  }
  binding->visible = visible;
  pushAllRows();
  emit seriesListChanged();
  return true;
}

void StateTransitionsController::removeAllSeries() {
  if (rows_.empty()) {
    return;
  }
  rows_.clear();
  pushAllRows();
  emit seriesListChanged();
}

QStringList StateTransitionsController::currentSeriesKeys() const {
  QStringList keys;
  keys.reserve(static_cast<qsizetype>(rows_.size()));
  for (const RowBinding& binding : rows_) {
    keys.append(binding.catalog_key);
  }
  return keys;
}

std::vector<CurveDescriptor> StateTransitionsController::currentSeries() const {
  std::vector<CurveDescriptor> descriptors;
  descriptors.reserve(rows_.size());
  for (const RowBinding& binding : rows_) {
    descriptors.push_back(binding.adapter->source());
  }
  return descriptors;
}

QList<QPair<DatasetId, QString>> StateTransitionsController::displayedTopics() const {
  QList<QPair<DatasetId, QString>> topics;
  for (const RowBinding& binding : rows_) {
    const CurveDescriptor& source = binding.adapter->source();
    const QPair<DatasetId, QString> entry{source.dataset_id, source.topic_name};
    if (!topics.contains(entry)) {
      topics.append(entry);
    }
  }
  return topics;
}

void StateTransitionsController::refreshNow() {
  if (range_dirty_ && playback_ != nullptr) {
    view_->setDisplayRange(toAxisDouble(playback_->rangeMin()), toAxisDouble(playback_->rangeMax()));
  }
  // A dirty row whose key no longer names a discrete field (an in-place reload
  // retyped it to a float, say) leaves the strip — same policy as a vanished
  // topic. Survivors have already been rebound to the fresh layout/type.
  const std::size_t before = rows_.size();
  std::erase_if(rows_, [this](RowBinding& binding) { return binding.dirty && !rebindRowFromCatalog(binding); });
  if (rows_.size() != before) {
    pushAllRows();
    emit seriesListChanged();
  }
  for (RowBinding& binding : rows_) {
    const bool remap_only = range_dirty_ && !binding.dirty;
    if (binding.dirty) {
      binding.adapter->rebuild();
      binding.dirty = false;
    } else if (!remap_only) {
      continue;
    }
    if (binding.visible) {  // a hidden row keeps its data current but has no view row
      view_->updateRow(displayRow(binding));
    }
  }
  range_dirty_ = false;
}

bool StateTransitionsController::rebindRowFromCatalog(RowBinding& binding) {
  if (catalog_ == nullptr || session_ == nullptr) {
    return true;  // nothing to rebind against; keep the row as-is
  }
  const std::optional<CatalogItem> item = catalog_->itemDescriptor(binding.catalog_key);
  const ScalarFieldPayload* scalar = item.has_value() ? asScalarField(*item) : nullptr;
  std::optional<CurveDescriptor> descriptor =
      catalog_->curveDescriptor(binding.catalog_key, SeriesCapability::kDiscrete);
  if (scalar == nullptr || !descriptor.has_value()) {
    return false;  // vanished or no longer discrete
  }
  // Recreate the adapter only on a real identity/type change: an unchanged
  // adapter keeps its previous segments through a failed re-query (stale beats
  // blank mid-stream).
  const CurveDescriptor& current = binding.adapter->source();
  const bool changed = current.topic_id != descriptor->topic_id || current.column_index != descriptor->column_index ||
                       current.field_path != descriptor->field_path ||
                       binding.adapter->logicalType() != scalar->logical_type;
  if (changed) {
    binding.adapter = std::make_unique<StateSeriesAdapter>(session_, std::move(*descriptor), scalar->logical_type);
  }
  return true;
}

StateRow StateTransitionsController::displayRow(const RowBinding& binding) const {
  const CurveDescriptor& source = binding.adapter->source();
  const qint64 offset_ns = session_ != nullptr ? session_->displayOffset(source.dataset_id).value.count() : 0;
  const qint64 range_max_ns = displayRangeMaxNs();

  StateRow row;
  row.id = binding.row_id;
  row.name = curveDisplayName(source);  // full "topic/field" path, as the plot legend shows it
  row.segments.reserve(binding.adapter->segments().size());
  for (const RawStateSegment& raw : binding.adapter->segments()) {
    const qint64 start = raw.t_start_raw_ns - offset_ns;
    const qint64 end =
        std::max(start, raw.t_end_raw_ns == RawStateSegment::kOpenEnd ? range_max_ns : raw.t_end_raw_ns - offset_ns);
    row.segments.push_back({.t_start_ns = start, .t_end_ns = end, .value = raw.value});
  }
  return row;
}

void StateTransitionsController::pushAllRows() {
  std::vector<StateRow> display_rows;
  display_rows.reserve(rows_.size());
  for (const RowBinding& binding : rows_) {
    if (binding.visible) {
      display_rows.push_back(displayRow(binding));
    }
  }
  view_->setRows(display_rows);
}

qint64 StateTransitionsController::displayRangeMaxNs() const {
  if (playback_ == nullptr) {
    return 0;
  }
  return static_cast<qint64>(std::llround(toAxisDouble(playback_->rangeMax()) * kNanosecondsPerSecond));
}

void StateTransitionsController::markTopicsDirty(const QVector<TopicId>& ids) {
  bool any = false;
  for (RowBinding& binding : rows_) {
    if (ids.contains(binding.adapter->source().topic_id)) {
      binding.dirty = true;
      any = true;
    }
  }
  if (any) {
    refresh_trigger_.request();
  }
}

}  // namespace PJ
