#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QCheckBox>
#include <QDomDocument>
#include <QDomElement>
#include <QHash>
#include <QList>
#include <QPoint>
#include <QPointer>
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

// The Datasets tree mirrors CatalogModel; the Custom Topics tree lists what the session derived (transform outputs
// and the outputs of persistent on-demand recipes).
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

  // Drives the dimmed "unsubscribed" rows: a topic of a per-topic-pause-capable dataset that nothing references.
  // Optional: without it no row is dimmed.
  void setTopicDemandTracker(TopicDemandTracker* tracker);

  // A double-click on a scalar placeholder leaf asks it for a field preview. Optional.
  void setTopicDemandController(TopicDemandController* controller);

  /// Where the tree's displayed marks come from (see CurveTreeView::setDisplayedKeys): the series of
  /// the registered widgets inside the widget `scope` returns (the shell: the selected dock), read
  /// from the TopicDemandController. `scope` is asked again on every refresh, so a selection that
  /// changes without a series change must call scheduleDisplayedKeysRefresh().
  void setDisplayedScope(std::function<const QWidget*()> scope);
  /// Re-marks the displayed rows on the next event-loop turn; a burst (a layout load) coalesces
  /// into one refresh. Also runs on every TopicDemandController::displayedSeriesChanged.
  void scheduleDisplayedKeysRefresh();

  // A dataset's file path, empty when it was not loaded from a file. Reload/Replace are offered only for a path;
  // without a resolver they stay disabled.
  using DatasetSourcePathResolver = std::function<QString(DatasetId)>;
  void setDatasetSourcePathResolver(DatasetSourcePathResolver resolver) {
    source_path_resolver_ = std::move(resolver);
  }

  void refreshValues(double tracker_time);

  /// Replaces the whole Custom Topics row set. The caller is the source of truth (MainWindow derives it from
  /// DataProcessorService and OnDemandService); a full replace keeps the panel right on paths that change the
  /// processor graph without announcing each row (layout restore, a plugin withdrawing its transform).
  ///
  /// A key with no catalog item is not shown, so a row never outlives its series. The rows are rebuilt only when a
  /// key, owner, name or pin changed; otherwise only rows whose glyph or tooltip differs are written, so the caller
  /// may re-send the set on every state or offset change.
  void setCustomTopics(const std::vector<CustomTopicRow>& rows);

  /// Sorted. Tests assert on this rather than on the tree, which the filter text also shapes.
  [[nodiscard]] QStringList customTopicNames() const;

  void removeCustomCurve(const QString& name);

  // <curve_list_state show_topics show_values datasets_filter custom_filter/>, a persisted layout element.
  [[nodiscard]] QDomElement saveListState(QDomDocument& doc) const;

  // Applies each attribute on its own; missing or unknown values are ignored. Never writes QSettings.
  void restoreListState(const QDomElement& element);

  /// Full-set replace from IngestProgressController.
  void setDatasetProgress(const QHash<quint64, CurveTreeView::DatasetProgress>& by_row_key);

  void setDatasetRowKey(const QString& dataset_tree_path, quint64 row_key);

  /// Replay progress of the on-demand series, keyed by the series' engine topic id, drawn on that series' rows of the
  /// Custom Topics tree (one per number column; the Datasets tree is untouched). Full-set replace, caller-driven
  /// (SeriesProgressController). A topic with no row yet is kept and drawn when its row appears.
  void setSeriesProgress(const QHash<quint64, CurveTreeView::DatasetProgress>& by_topic);

 signals:
  /// The user stopped a loading dataset from its row: the ✕ keeps what arrived (`keep_partial`), the bin discards it.
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
  // `covers_all`: the selection (an empty one means all) targets every known curve; MainWindow decides whether to
  // prompt.
  void trashRequested(QStringList names, bool covers_all);
  void clearAllCurvesRequested();
  // MainWindow confirms (one combined dialog) and removes.
  void removeDatasetsRequested(const QList<DatasetId>& dataset_ids);
  // At least two datasets; MainWindow confirms and merges.
  void mergeDatasetsRequested(const QList<DatasetId>& dataset_ids);
  // Run the datasets' marker rules now, whatever the batch mode and reload rate.
  void refreshDerivedRequested(const QList<DatasetId>& dataset_ids);
  void reloadDatasetRequested(DatasetId dataset_id);
  // Pick another file and replace this dataset's data with it, transactionally.
  void replaceDatasetRequested(DatasetId dataset_id);
  void datasetInfoRequested(DatasetId dataset_id);
  // Not-draggable feedback: why a pull started no drag, or which topics a multi-selection drag left out.
  void toastRequested(const QString& message);

 public slots:
  void onStylesheetChanged(QString theme);
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 protected:
  // While the Custom Topics filter has focus, its band's label and buttons hide so the input fills the row.
  bool eventFilter(QObject* watched, QEvent* event) override;

 private slots:
  void onFilterChanged(const QString& text);
  void onCustomFilterChanged(const QString& text);
  // Any of the plot / 2D / 3D toggles; all three off is allowed and shows the empty-filter message.
  void onTypeFilterToggled();
  void onShowValuesToggled(bool show);
  void onPreserveTopicNameToggled(bool checked);
  void onTrashClicked();
  // The dataset menu on a dataset node, the topic menu on any other row; emits intents, MainWindow acts.
  void onTreeContextMenu(const QPoint& pos);

 private:
  // With at most one row selected across both trees, on a per-topic-pause-capable dataset: the force-streaming
  // toggle (TopicDemandTracker::setTopicForced), the only way to accumulate a topic's history before it is displayed.
  void showTopicContextMenu(QTreeWidgetItem* clicked, const QPoint& pos);
  void onCatalogItemsAdded(const std::vector<CatalogItem>& items);
  void onCatalogItemsRemoved(const QStringList& keys);
  void onCatalogCleared();
  void onActiveTopicsChanged(DatasetId dataset_id, const std::vector<QString>& active_topics);
  // Without a rebuild. Skips the catalog walk while no dataset can pause topics; without a tracker it clears every
  // flag.
  void refreshUnsubscribedFlags();
  // Also after every rebuild: the marks live on tree nodes, which a rebuild recreates.
  void refreshForcedMarks();
  // Pushes the controller's displayed keys within displayed_scope_ to both trees.
  void refreshDisplayedKeys();
  // Requests the field preview and arms the tree's one-shot auto-expand at the topic. No-op without a controller.
  void onPlaceholderPeekRequested(const QString& catalog_key);
  void onDragPayloadKeysSkipped(const QStringList& catalog_keys);
  void applyIcons(QString theme);
  // From custom_rows_ ∩ the catalog, keeping expanded groups; a key with no catalog item is left out.
  void refillCustomView();
  // Each object row's glyph and whole tooltip (creator, state, a finding's pin), written in one pass.
  void applyCustomRowStates();
  // The selected rows themselves, not the keys beneath a selected group.
  [[nodiscard]] QStringList selectedCustomRowKeys() const;
  // Enables the pencil only for one row the editor owns and Delete only when no selected
  // row is read-only; words both tooltips after the selection.
  void updateCustomEditButton();
  // Not the Custom Topics editor's, a row with no owner included: no edit, no delete.
  [[nodiscard]] bool isReadOnlyCustomRow(const QString& catalog_key) const;
  [[nodiscard]] OwnerInfo ownerInfo(const QString& plugin_id) const;
  std::vector<QString> selectedCurveNamesForDrag() const;

  [[nodiscard]] bool valuesColumnActive() const;
  // The unthrottled body of refreshValues(), at last_tracker_time_.
  void fillValuesNow();
  // Empty for a folder that is not a topic.
  [[nodiscard]] QString rowTooltip(const QString& catalog_key, const QString& tree_path) const;

  Ui::CurveListPanel* ui_;
  CatalogModel* catalog_ = nullptr;
  TopicDemandTracker* tracker_ = nullptr;
  QPointer<TopicDemandController> controller_;
  std::function<const QWidget*()> displayed_scope_;
  bool displayed_refresh_scheduled_ = false;
  DatasetSourcePathResolver source_path_resolver_;
  CurveTreeView* tree_view_ = nullptr;
  CurveTreeView* custom_view_ = nullptr;
  // What setCustomTopics last received, by catalog key; excluded from the Datasets tree.
  QHash<QString, CustomTopicRow> custom_rows_;
  OwnerResolver owner_resolver_;
  QString editor_plugin_id_;  ///< see setEditorPluginId
  DisplayTimeResolver display_time_resolver_;
  struct CustomObjectRow {
    QString tree_path;
    DatasetId dataset_id = 0;
    QString base_tooltip;  ///< before the state's text and the pin are appended
  };
  QHash<QString, CustomObjectRow> custom_object_rows_;
  // Series row key (topic and column, kSeriesRowKeyColumnBits) -> its tree path.
  QHash<quint64, QString> series_row_paths_;
  QHash<quint64, CurveTreeView::DatasetProgress> series_progress_;
  void applySeriesProgress();
  // Menu items; members so applyIcons() can retint their icons.
  QPushButton* clear_all_button_ = nullptr;
  QPushButton* delete_custom_button_ = nullptr;
  QCheckBox* show_values_check_ = nullptr;
  QCheckBox* preserve_topic_name_check_ = nullptr;

  // Set during restoreListState: a layout must not change the user's QSettings defaults.
  bool applying_state_ = false;
  // Display-axis seconds; reused to fill the column at once when it is shown.
  double last_tracker_time_ = 0.0;
  // Leading and trailing edge: the first fill is immediate, then at most one per window, plus a final catch-up so
  // the value where playback stops is shown. value_refresh_pending_: a trailing fill is owed.
  QTimer* value_throttle_timer_ = nullptr;
  bool value_refresh_pending_ = false;
  ChromeMetrics chrome_metrics_;
};

}  // namespace PJ
