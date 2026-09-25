#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_plotting/StateSeriesAdapter.h"
#include "pj_widgets/CoalescingTrigger.h"
#include "pj_widgets/StateTransitionsView.h"

namespace PJ {

class CatalogModel;
class PlaybackEngine;
class SessionManager;

/// Binds a StateTransitionsView to the runtime: owns one StateSeriesAdapter per
/// row, maps raw-ns segments into the playback-display frame (per-dataset
/// `SessionManager::displayOffset`), and keeps the view current across
/// streaming ingest (coalesced ~10 Hz), display-offset changes (arithmetic-only
/// re-map, no re-read), dataset replaces (synchronous adapter clear), catalog
/// removals, and playback-range changes (the trailing open segment is pinned to
/// the range max so bands reach the right edge).
///
/// The view stays runtime-agnostic: this controller injects the droppable
/// predicate (CatalogModel::isDiscreteKey — string/integer/bool series, and
/// floats holding only whole numbers),
/// answers its seriesDropped / rowRemoveRequested intents, and re-feeds rows.
/// The dock widget owns persistence; it reads currentSeries() and replays keys
/// via addSeries().
class StateTransitionsController : public QObject {
  Q_OBJECT
 public:
  StateTransitionsController(
      StateTransitionsView* view, SessionManager* session, CatalogModel* catalog, PlaybackEngine* playback,
      QObject* parent = nullptr);

  /// Add a row for a catalog key. Rejects (returning false) duplicates,
  /// non-discrete keys (float fields), and keys the catalog cannot resolve.
  bool addSeries(const QString& catalog_key);
  /// Remove the row with this view row id. False if unknown.
  bool removeSeries(quint64 row_id);
  /// Move the row at list position `from` to drop position `to`, with `to`
  /// interpreted against the pre-removal list — the same drop-indicator
  /// arithmetic as the 3D panel's layer list (detail::reorderIds). Reorders
  /// the strip, the persisted series order, and the panel list. False on an
  /// invalid or no-op move.
  bool reorderSeries(int from, int to);

  /// One row as the curves side-panel sees it: stable id, display name, and
  /// the eye-toggle state.
  struct SeriesEntry {
    quint64 row_id = 0;
    QString name;
    bool visible = true;
  };
  /// All rows (hidden ones included), in display order — the side-panel list.
  [[nodiscard]] std::vector<SeriesEntry> seriesEntries() const;
  /// Eye toggle: a hidden row keeps its data/demand and its panel entry but is
  /// not rendered in the strip. False if the id is unknown.
  bool setSeriesVisible(quint64 row_id, bool visible);
  /// Remove every row (the panel's "Clear all curves").
  void removeAllSeries();

  /// Current rows' catalog keys, in display order (persistence).
  [[nodiscard]] QStringList currentSeriesKeys() const;
  /// Current rows' full descriptors, in display order — the stable
  /// dataset/topic/field identity the dock widget serializes.
  [[nodiscard]] std::vector<CurveDescriptor> currentSeries() const;
  /// The displayed (dataset, topic-name) set — the shell's TopicDemandController
  /// observes this (with seriesListChanged) to hold per-topic streaming
  /// references, exactly as it does for plot curves.
  [[nodiscard]] QList<QPair<DatasetId, QString>> displayedTopics() const;

  [[nodiscard]] int rowCount() const noexcept {
    return static_cast<int>(rows_.size());
  }

 signals:
  /// A row was added or removed (user drop/chip-close, catalog removal, or a
  /// restore). The dock widget hooks this for undo snapshots + layout dirt;
  /// the shell for topic-demand refresh.
  void seriesListChanged();

 private:
  struct RowBinding {
    quint64 row_id = 0;
    QString catalog_key;
    std::unique_ptr<StateSeriesAdapter> adapter;
    bool dirty = true;    // needs adapter rebuild on the next coalesced refresh
    bool visible = true;  // eye toggle: hidden rows keep data but are not rendered
  };

  void connectRuntime();
  /// Coalesced refresh: rebind + rebuild dirty adapters, re-push every row whose
  /// data or mapping changed, and refresh the view's display range.
  void refreshNow();
  /// Re-resolve a dirty row's payload from the catalog by key: an in-place
  /// dataset reload keeps keys/TopicIds stable but may change the column's
  /// field layout or primitive type, so the cached adapter must follow. False
  /// when the key no longer names a discrete field (caller drops the row).
  [[nodiscard]] bool rebindRowFromCatalog(RowBinding& binding);
  /// Map one binding's raw segments into display-frame StateRow form.
  [[nodiscard]] StateRow displayRow(const RowBinding& binding) const;
  /// Push the full row set to the view (structure changed: add/remove/restore).
  void pushAllRows();
  [[nodiscard]] qint64 displayRangeMaxNs() const;
  void markTopicsDirty(const QVector<TopicId>& ids);

  StateTransitionsView* view_ = nullptr;
  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  PlaybackEngine* playback_ = nullptr;

  std::vector<RowBinding> rows_;
  quint64 next_row_id_ = 1;
  bool range_dirty_ = true;  // re-push setDisplayRange on the next refresh
  CoalescingTrigger refresh_trigger_;
};

}  // namespace PJ
