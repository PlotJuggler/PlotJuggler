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
#include <optional>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_runtime/NodeScheduler.h"
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

// Timeseries list + Custom Topics section. Top tree mirrors CatalogModel;
// bottom tree lists what the session derived (transform outputs and the object
// topics of persistent on-demand recipes).
class CurveListPanel : public QWidget {
  Q_OBJECT
 public:
  explicit CurveListPanel(QWidget* parent = nullptr);
  ~CurveListPanel() override;

  void setCatalog(CatalogModel* catalog);

  /// The plugin id of the Custom Topics editor (the toolbox whose manifest declares
  /// `custom_topics_editor`): the one owner whose rows the edit (pencil) and delete
  /// actions accept, and whose launch the "+" stands for. Empty (the default) when no
  /// loaded toolbox declares it: "+" is hidden and every owned row is read-only.
  void setEditorPluginId(const QString& plugin_id);

  /// Who created a Custom Topics row, as the panel shows it: the display `name`
  /// (tooltips) and the short `badge` painted after another plugin's rows.
  struct OwnerInfo {
    QString name;
    QString badge;
  };
  /// Resolves an owner's plugin id to its display strings. Optional: without it, or
  /// for an empty answer, both fall back to the plugin id.
  using OwnerResolver = std::function<OwnerInfo(const QString& plugin_id)>;
  void setOwnerResolver(OwnerResolver resolver) {
    owner_resolver_ = std::move(resolver);
  }

  /// Maps a raw dataset instant (ns) to the display axis the timeline shows, or nullopt
  /// for an unknown dataset. Drives the "Pinned at <t> s" tooltip line; without it, or on
  /// nullopt, the line is left out rather than shown in raw time.
  using DisplayTimeResolver = std::function<std::optional<double>(DatasetId, Timestamp raw_ns)>;
  void setDisplayTimeResolver(DisplayTimeResolver resolver) {
    display_time_resolver_ = std::move(resolver);
  }

  /// One Custom Topics row, as the caller derives it from the recipes.
  struct CustomTopicRow {
    QString key = {};    ///< catalog key of the derived topic or transform output
    QString owner = {};  ///< plugin id of the recipe's creator; empty for a recipe no plugin owns
    /// The flat label of an on-demand row: an object output's is the recipe id, or `recipe/output`, a series
    /// column's is `recipe/output`. Empty labels an object row with its topic name; a scalar row without one is a
    /// transform output, which keeps its topic path.
    QString name = {};
    std::optional<Timestamp> pinned_t = {};  ///< a finding's pin, raw dataset time
    /// The on-demand node's state, painted as the row glyph; nullopt for a transform output.
    std::optional<NodeScheduler::NodeState> state = {};

    bool operator==(const CustomTopicRow&) const = default;
  };

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

  /// Replace the whole set of rows routed to Custom Topics.
  ///
  /// The panel holds no independent source of truth for which series are
  /// derived, so the caller is authoritative: MainWindow derives the set from
  /// DataProcessorService and OnDemandService. Replacing wholesale — rather than adding and removing
  /// row by row — is what keeps the panel correct on the paths that change the
  /// processor graph without announcing each row (layout restore, a plugin
  /// withdrawing its transform through pj.data_processors.v1).
  ///
  /// A key with no catalog item is simply not shown, so a row can never outlive
  /// the series behind it. The rows are rebuilt only when a key, an owner, a name or a
  /// pin changed; otherwise the glyphs and tooltips are re-composed (the pin's display
  /// time may have moved) and only a row whose result differs is written, so the caller
  /// may re-send the set on every state or offset change.
  void setCustomTopics(const std::vector<CustomTopicRow>& rows);

  /// Topic names currently listed in Custom Topics, sorted. The row set is not
  /// otherwise observable from outside; assert on this rather than on the tree's
  /// contents, which the section's filter text also shapes.
  [[nodiscard]] QStringList customTopicNames() const;

  /// Remove a row from the Custom Topics panel by its catalog key.
  void removeCustomCurve(const QString& name);

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

  /// Replay progress of the on-demand series, keyed by the series' engine topic id, drawn on that series' rows of the
  /// Custom Topics tree (one per number column; the Datasets tree is untouched). Full-set replace, caller-driven
  /// (SeriesProgressController). A topic with no row yet is kept and drawn when its row appears.
  void setSeriesProgress(const QHash<quint64, CurveTreeView::DatasetProgress>& by_topic);

 signals:
  /// Emitted when the user stops a loading, cancellable dataset row from the
  /// row itself. `keep_partial` says which affordance was used: the ✕ keeps
  /// what arrived, the bin discards it. MainWindow routes this to
  /// SessionManager::requestCancel with that choice.
  void cancelRequested(quint64 row_key, bool keep_partial);
  /// The user stopped a series replay from its Custom Topics row (either affordance: a series stop always keeps the
  /// rows computed so far). `topic` is the series' engine topic.
  void seriesStopRequested(quint64 topic);

  void createCustomTopicRequested();
  // Delete the selected Custom Topics rows, by catalog key (several may be one recipe's).
  // Emitted only for rows the panel lets the user delete; the handler checks the owner again.
  void deleteCustomTopicsRequested(QStringList keys);
  // Edit the selected custom topic (pencil button): `key` is its catalog key.
  void editCustomTopicRequested(QString key);
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
  // The user chose "Refresh derived": run the selected datasets' marker rules now,
  // whatever the batch mode and reload rate.
  void refreshDerivedRequested(const QList<DatasetId>& dataset_ids);
  // The user chose "Reload" on a single file-backed dataset: reload it from its
  // recorded source (per-dataset variant of the global reload).
  void reloadDatasetRequested(DatasetId dataset_id);
  // The user chose "Replace" on a single file-backed dataset: pick a different
  // file and transactionally replace this dataset's data with it.
  void replaceDatasetRequested(DatasetId dataset_id);
  // The user chose "Info" on a single dataset: MainWindow shows the dataset-info
  // dialog (name, backing file, provenance from its SourceRecord if any).
  void datasetInfoRequested(DatasetId dataset_id);
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
  // the per-row vertical padding of the Datasets / Custom Topics tree
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
  // Right-click on a topic/field/placeholder row with at most one selected row
  // across both trees, in a per-topic-pause-capable dataset: one toggle —
  // "Force topic streaming" / "Stop forced streaming" —
  // driving TopicDemandTracker::setTopicForced. The only way to accumulate a
  // topic's history BEFORE it is first displayed.
  void showTopicContextMenu(QTreeWidgetItem* clicked, const QPoint& pos);
  void onCatalogItemsAdded(const std::vector<CatalogItem>& items);
  void onCatalogItemsRemoved(const QStringList& keys);
  void onCatalogCleared();
  void onActiveTopicsChanged(DatasetId dataset_id, const std::vector<QString>& active_topics);
  // Recomputes every catalog item's unsubscribed state from scratch and pushes
  // it to tree_view_ — the live-update path for onActiveTopicsChanged (no
  // rebuild). Walks the catalog in place, and not at all while no dataset can
  // pause topics. No-op without a catalog; without a tracker it clears every flag.
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
  // Repaints the Custom Topics view from custom_rows_ ∩ the catalog, preserving
  // expanded groups. Keys with no catalog item are dropped from the view by
  // omission, so this doubles as the cleanup for a row left without one.
  void refillCustomView();
  // Composes each object row's glyph and whole tooltip (the creator line, the state's
  // text and a finding's pin) and hands them to the custom view in one pass.
  void applyCustomRowStates();
  // Catalog keys of the rows selected in the Custom Topics view (the rows themselves,
  // not the keys beneath a selected group).
  [[nodiscard]] QStringList selectedCustomRowKeys() const;
  // Enables the pencil only for one row the editor owns and Delete only when no selected
  // row is read-only; words both tooltips after the selection.
  void updateCustomEditButton();
  // Whether the row of `catalog_key` is not the Custom Topics editor's, a row with no owner
  // included (use-only: no edit, no delete).
  [[nodiscard]] bool isReadOnlyCustomRow(const QString& catalog_key) const;
  [[nodiscard]] OwnerInfo ownerInfo(const QString& plugin_id) const;
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
  // What setCustomTopics last received, by catalog key. The keys are excluded from the
  // main tree so a derived topic never appears in both.
  QHash<QString, CustomTopicRow> custom_rows_;
  OwnerResolver owner_resolver_;
  QString editor_plugin_id_;  ///< see setEditorPluginId
  DisplayTimeResolver display_time_resolver_;
  // An object row of the custom view: the tree path its glyph is keyed by, its dataset
  // and its tooltip before the state's text and the pin are appended.
  struct CustomObjectRow {
    QString tree_path;
    DatasetId dataset_id = 0;
    QString base_tooltip;
  };
  QHash<QString, CustomObjectRow> custom_object_rows_;
  // Series rows of the custom view: the row key the view reports stop clicks against
  // (series topic << 16 | column) and the tree path it decorates. `series_progress_` is
  // what setSeriesProgress last received, applied to those rows.
  QHash<quint64, QString> series_row_paths_;
  QHash<quint64, CurveTreeView::DatasetProgress> series_progress_;
  // Pushes `series_progress_` onto the rows in `series_row_paths_` (registered by refillCustomView).
  void applySeriesProgress();
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
