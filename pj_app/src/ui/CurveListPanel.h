#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QCheckBox>
#include <QDomDocument>
#include <QDomElement>
#include <QHash>
#include <QList>
#include <QPoint>
#include <QSet>
#include <QStringList>
#include <QWidget>
#include <functional>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_widgets/ChromeMetrics.h"
// Not a forward declaration: the progress-forwarding signatures below name the
// nested CurveTreeView::DatasetProgress, which needs the complete type.
#include "pj_widgets/CurveTreeView.h"

class QAction;
class QPushButton;
class QTimer;
class QTreeWidgetItem;

namespace Ui {
class CurveListPanel;
}

namespace PJ {

class CatalogModel;
struct CatalogItem;
class CurveTreeView;
class TopicDemandController;
class TopicDemandTracker;

// Timeseries list + Custom Series section. Top tree mirrors CatalogModel;
// bottom tree is the user's custom/derived series.
class CurveListPanel : public QWidget {
  Q_OBJECT
 public:
  explicit CurveListPanel(QWidget* parent = nullptr);
  ~CurveListPanel() override;

  void setCatalog(CatalogModel* catalog);

  // Drives the "unsubscribed" (dimmed) row state: a topic that has data but is
  // not currently referenced on a per-topic-pause-capable dataset. Optional —
  // without it every row's unsubscribed flag stays false (today's behavior).
  void setTopicDemandTracker(TopicDemandTracker* tracker);

  // Routes a double-click on a scalar placeholder leaf to a bounded field
  // preview (and arms the one-shot auto-expand). Optional — without it a
  // double-click peek is a no-op.
  void setTopicDemandController(TopicDemandController* controller);

  // Resolves a dataset's tracked on-disk source path (empty when the dataset was
  // not loaded from a file, e.g. streaming/test data). Gates the per-dataset
  // Reload/Replace context-menu items to file-backed datasets. Optional —
  // without it both items stay disabled.
  using DatasetSourcePathResolver = std::function<QString(DatasetId)>;
  void setDatasetSourcePathResolver(DatasetSourcePathResolver resolver) {
    source_path_resolver_ = std::move(resolver);
  }

  void refreshValues(double tracker_time);

  /// Add a curve to the Custom Series panel. `catalog_key` is the drag key;
  /// `display_name` is what the user sees (the topic/alias name).
  void addCustomCurve(const QString& catalog_key, const QString& display_name);
  /// Remove a curve from the Custom Series panel by its catalog key.
  void removeCustomCurve(const QString& name);
  /// Remove a custom series by its DISPLAY name (the transform output name).
  /// Robust against catalog-key churn — mirrors PJ3's removeCurve(name) used when
  /// a source delete cascades to its derived series.
  void removeCustomCurveByName(const QString& display_name);

  // Builds <curve_list_state show_topics="..." show_values="..."
  // datasets_filter="..." custom_filter="..."/> — filter text plus
  // display-mode toggles.
  [[nodiscard]] QDomElement saveListState(QDomDocument& doc) const;

  // Applies <curve_list_state> attributes individually; missing or
  // mismatched values are silently ignored. Sets applying_state_
  // around the toggle calls so QSettings stays untouched.
  void restoreListState(const QDomElement& element);

  /// Forward progressive ingest progress state to the underlying tree view.
  /// Called by IngestProgressController whenever the progress set changes.
  void setDatasetProgress(const QHash<quint64, CurveTreeView::DatasetProgress>& by_row_key);

  /// Map a dataset's tree path to its opaque row key used in progress updates.
  /// Called by IngestProgressController when a dataset row is first created.
  void setDatasetRowKey(const QString& dataset_tree_path, quint64 row_key);

 signals:
  /// Emitted when the user stops a loading, cancellable dataset row from the
  /// row itself. `keep_partial` says which affordance was used: the ✕ keeps
  /// what arrived, the bin discards it. MainWindow routes this to
  /// SessionManager::requestCancel with that choice.
  void cancelRequested(quint64 row_key, bool keep_partial);

  void createCustomSeriesRequested();
  void deleteCustomSeriesRequested(QString name);
  // Edit the selected custom series (pencil button): `name` is its display name.
  void editCustomSeriesRequested(QString name);
  // covers_all is true when the panel determined the selection (or its
  // empty-implies-all interpretation) targets every known curve. MainWindow
  // decides whether to prompt the user.
  void trashRequested(QStringList names, bool covers_all);
  // Emitted when the user picks "Clear All" from the datasets menu.
  void clearAllCurvesRequested();
  // The user chose "Remove" on a dataset selection. The panel resolved
  // the selected dataset nodes to ids; MainWindow confirms (one combined dialog)
  // and performs the removal.
  void removeDatasetsRequested(const QList<DatasetId>& dataset_ids);
  // The user chose "Merge" on a multi-dataset selection (≥2). MainWindow shows the
  // shared destructive-merge confirmation and performs the merge.
  void mergeDatasetsRequested(const QList<DatasetId>& dataset_ids);
  // The user chose "Reload" on a single file-backed dataset: reload it from its
  // recorded source (per-dataset variant of the global reload).
  void reloadDatasetRequested(DatasetId dataset_id);
  // The user chose "Replace" on a single file-backed dataset: pick a different
  // file and transactionally replace this dataset's data with it.
  void replaceDatasetRequested(DatasetId dataset_id);
  // Asks the shell to show a transient toast. Carries the panel's not-draggable
  // feedback: why a pull on an undisplayable topic started no drag, or which
  // topics a multi-selection drag left out.
  void toastRequested(const QString& message);

 public slots:
  void onStylesheetChanged(QString theme);
  // Rebinds Chrome metrics broadcast from MainWindow. Resizes header
  // bands to (icon_size + icon_padding) + 2 * layout_padding tall,
  // sizes the chrome buttons to (icon_size + icon_padding) square,
  // pushes layout_padding as contentsMargins on the header band
  // layouts, and uses layout_spacing for both the in-band spacing and
  // the per-row vertical padding of the Datasets / Custom Series tree
  // views (via per-instance QSS).
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 protected:
  // Hides the sibling label / action buttons in each header band when
  // its filter QLineEdit gets focus, so the input expands to fill the
  // row. Restores them on focus loss.
  bool eventFilter(QObject* watched, QEvent* event) override;

 private slots:
  void onFilterChanged(const QString& text);
  void onCustomFilterChanged(const QString& text);
  // Any of the three Datasets type-filter toggles (plot / 2D / 3D) changed.
  // Pushes the enabled kinds to the Datasets tree; all three off is allowed
  // and shows the tree's empty-filter message. Reads live button state, so
  // it is sender-agnostic.
  void onTypeFilterToggled();
  void onShowValuesToggled(bool show);
  void onPreserveTopicNameToggled(bool checked);
  void onTrashClicked();
  // Right-click on a dataset node → Merge (≥2 selected) / Reload / Replace
  // (exactly one file-backed dataset) / Remove dataset(s) menu, operating on the
  // selected top-level dataset nodes. Emits intents; MainWindow confirms +
  // performs.
  void onTreeContextMenu(const QPoint& pos);

 private:
  // Right-click on a topic/field/placeholder row of a per-topic-pause-capable
  // dataset: one toggle — "Force topic streaming" / "Stop forced streaming" —
  // driving TopicDemandTracker::setTopicForced. The only way to accumulate a
  // topic's history BEFORE it is first displayed.
  void showTopicContextMenu(QTreeWidgetItem* clicked, const QPoint& pos);
  void onCatalogItemsAdded(const std::vector<CatalogItem>& items);
  void onCatalogItemsRemoved(const QStringList& keys);
  void onCatalogCleared();
  void onActiveTopicsChanged(DatasetId dataset_id, const std::vector<QString>& active_topics);
  // Recomputes every catalog item's unsubscribed state from scratch and pushes
  // it to tree_view_ — the live-update path for onActiveTopicsChanged (no
  // rebuild). No-op without both a catalog and a tracker.
  void refreshUnsubscribedFlags();
  // Pushes the tracker's forced-topic sets (every dataset) to the tree as
  // accent-painted topic marks. Runs on forcedTopicsChanged and after any
  // rebuild (the marks live on tree nodes, which rebuilds recreate).
  void refreshForcedMarks();
  // Handles CurveTreeView::placeholderPeekRequested: resolves the catalog key,
  // asks the controller for a bounded field preview, and arms the tree's
  // one-shot auto-expand at the topic's path. No-op without a controller.
  void onPlaceholderPeekRequested(const QString& catalog_key);
  // Handles CurveTreeView::dragPayloadKeysSkipped: resolves the skipped keys to
  // topic names via the catalog and emits a toastRequested explaining that they
  // were left out of the drag.
  void onDragPayloadKeysSkipped(const QStringList& catalog_keys);
  void applyIcons(QString theme);
  std::vector<QString> selectedCurveNamesForDrag() const;

  // True when there is a catalog and at least one tree's Value column is shown —
  // i.e. there is anything to fill.
  [[nodiscard]] bool valuesColumnActive() const;
  // Reads each visible scalar leaf at last_tracker_time_ and writes the formatted
  // value. The unthrottled body behind refreshValues().
  void fillValuesNow();
  // Hover text for a Datasets-tree row (see CurveTreeView::setTooltipProvider):
  // field path / type / sample count for a scalar leaf, type / message count for
  // a topic, topic count for a dataset. Empty for folders that are not a topic.
  [[nodiscard]] QString rowTooltip(const QString& catalog_key, const QString& tree_path) const;

  Ui::CurveListPanel* ui_;
  CatalogModel* catalog_ = nullptr;
  TopicDemandTracker* tracker_ = nullptr;
  TopicDemandController* controller_ = nullptr;
  DatasetSourcePathResolver source_path_resolver_;
  CurveTreeView* tree_view_ = nullptr;
  CurveTreeView* custom_view_ = nullptr;
  // Catalog keys routed to the Custom Series panel (plugin-created transforms).
  // Excluded from the main tree so a custom series never appears in both.
  QSet<QString> custom_keys_;
  // Display name -> catalog key, so a custom series can be removed by name even if
  // its catalog key has churned (used by the source-delete cascade).
  QHash<QString, QString> custom_name_to_key_;
  // QPushButtons hosted inside QWidgetAction items in the section
  // dropdown menus. Kept as members so applyIcons() can retint their
  // leading icons on theme switch.
  QPushButton* clear_all_button_ = nullptr;
  QPushButton* delete_custom_button_ = nullptr;
  // Show Values and Preserve Topic Name checkboxes live inside the
  // datasets popup menu. Held as members so restoreListState can flip
  // them without rummaging through the menu's children.
  QCheckBox* show_values_check_ = nullptr;
  QCheckBox* preserve_topic_name_check_ = nullptr;

  // Set to true around restoreListState so the toggle slots suppress their
  // QSettings writes. Layout-driven changes mutate the UI but must not mutate
  // the global per-user defaults.
  bool applying_state_ = false;
  // Last tracker time pushed via refreshValues(), in display-axis seconds. Reused
  // when a non-tracker event (Show Values toggle) needs to re-fill the column at
  // the current cursor instead of waiting for the next playback tick.
  double last_tracker_time_ = 0.0;
  // Caps the value-column refresh to ~10 Hz: playback emits tracker updates up to
  // ~60 Hz, but the column only needs to track the eye. Leading + trailing edge
  // (immediate first fill, then at most one per window, plus a final catch-up so
  // the value where playback stops is shown). value_refresh_pending_ marks that a
  // tracker update arrived mid-window and a trailing fill is owed.
  QTimer* value_throttle_timer_ = nullptr;
  bool value_refresh_pending_ = false;
  // Chrome metrics from MainWindow::chromeMetricsChanged.
  ChromeMetrics chrome_metrics_;
};

}  // namespace PJ
