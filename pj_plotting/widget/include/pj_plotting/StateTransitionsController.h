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

/// Binds a StateTransitionsView to the runtime: owns one StateSeriesAdapter per row, maps raw-ns segments into the
/// playback-display frame (per-dataset `SessionManager::displayOffset`), and keeps the view current across streaming
/// ingest (coalesced to ~10 Hz), display-offset changes (an arithmetic re-map, no re-read), dataset replaces
/// (synchronous adapter clear), catalog removals and playback-range changes (the trailing open segment is pinned to the
/// range max so bands reach the right edge).
///
/// The view stays runtime-agnostic: this controller injects the droppable predicate (CatalogModel::isDiscreteKey:
/// string, integer and bool series, and floats holding only whole numbers), answers its seriesDropped /
/// rowRemoveRequested intents and re-feeds rows. The dock widget owns persistence: it reads currentSeries() and
/// replays keys through addSeries().
class StateTransitionsController : public QObject {
  Q_OBJECT
 public:
  StateTransitionsController(
      StateTransitionsView* view, SessionManager* session, CatalogModel* catalog, PlaybackEngine* playback,
      QObject* parent = nullptr);

  /// Adds a row for a catalog key. False for a duplicate, a non-discrete key (a float field) or a key the catalog
  /// cannot resolve.
  bool addSeries(const QString& catalog_key);
  /// Removes the row with this view row id; false if unknown.
  bool removeSeries(quint64 row_id);
  /// Moves the row at list position `from` to drop position `to`, interpreted against the pre-removal list (the 3D
  /// panel's drop-indicator arithmetic, detail::reorderIds). Reorders the strip, the persisted series order and the
  /// panel list. False on an invalid or no-op move.
  bool reorderSeries(int from, int to);

  /// One row as the curves side panel sees it: stable id, display name, eye-toggle state.
  struct SeriesEntry {
    quint64 row_id = 0;
    QString name;
    bool visible = true;
  };
  /// All rows (hidden included), in display order: the side-panel list.
  [[nodiscard]] std::vector<SeriesEntry> seriesEntries() const;
  /// Eye toggle: a hidden row keeps its data, demand and panel entry but is not rendered. False if the id is unknown.
  bool setSeriesVisible(quint64 row_id, bool visible);
  /// Removes every row (the panel's "Clear all curves").
  void removeAllSeries();

  /// The rows' catalog keys in display order (persistence).
  [[nodiscard]] QStringList currentSeriesKeys() const;
  /// The rows' full descriptors in display order: the stable dataset/topic/field identity the dock widget serializes.
  [[nodiscard]] std::vector<CurveDescriptor> currentSeries() const;
  /// The displayed (dataset, topic-name) set; the shell's TopicDemandController observes it (with seriesListChanged) to
  /// hold per-topic streaming references, as for plot curves.
  [[nodiscard]] QList<QPair<DatasetId, QString>> displayedTopics() const;

  [[nodiscard]] int rowCount() const noexcept {
    return static_cast<int>(rows_.size());
  }
  /// No coalesced refresh is still to come.
  [[nodiscard]] bool refreshIdleForTesting() const noexcept {
    return refresh_trigger_.idleForTesting();
  }

 signals:
  /// A row was added or removed (a drop, chip close, catalog removal or restore). The dock widget hooks it for undo
  /// snapshots and layout dirt; the shell for topic-demand refresh.
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
  /// Coalesced refresh: rebinds and rebuilds dirty adapters, re-pushes every row whose data or mapping changed and
  /// refreshes the view's display range.
  void refreshNow();
  /// Re-resolves a dirty row's payload from the catalog by key: an in-place dataset reload keeps keys and TopicIds but
  /// may change the column's layout or primitive type, so the cached adapter must follow. False when the key no longer
  /// names a discrete field (the caller drops the row).
  [[nodiscard]] bool rebindRowFromCatalog(RowBinding& binding);
  /// Maps one binding's raw segments into display-frame StateRow form.
  [[nodiscard]] StateRow displayRow(const RowBinding& binding) const;
  /// Pushes the full row set to the view (after an add, remove or restore).
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
