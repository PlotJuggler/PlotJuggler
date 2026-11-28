#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_log/LogTableModel.h"
#include "pj_widgets/TimeFormat.h"

class QToolButton;

namespace Ui {
class LogView;
}

namespace PJ {

/// What the user set in the panel; a host persists it.
struct LogViewState {
  /// Indexed like log_view::kLevelNames (the toggles, in toolbar order).
  std::array<bool, log_view::kToggleLevelCount> levels = {true, true, true, true, true};
  QString source;
  QString category;
  QString text;
  /// Labels of the topics the user hid in the Topics menu.
  QStringList hidden_topics;
  /// Entries later than the tracker are shown greyed (true) or hidden.
  bool show_future = false;

  bool operator==(const LogViewState&) const = default;
};

/// A log list over several topics merged by time: level toggles, source/category/message filters, a detail pane with
/// Copy, the last entry at or before the tracker tinted, later entries greyed or hidden. Runtime-agnostic: the host
/// hands each topic, keyed by an id of its choosing, as a LogRowSource the view indexes and reads on demand (or as
/// LogEntry lists the view keeps), sets the tracker and the time format, and answers seekRequested().
///
/// Data calls: an entry list is in ascending time; appendEntries() merges it with what the topic holds, earlier entries
/// included, after the topic's rows of equal time. Topic ids order rows of equal time.
///
/// The entry the user selects stays selected while it is shown. Not shown (filtered out, after the tracker with later
/// rows hidden, absent while a reload refills its topic), nothing is selected in its place; it is selected again once
/// it shows.
///
/// While playing (setPlaying) the list follows the tracker and ignores scrolling; paused, a tracker move brings its row
/// into view, except the move a double click on a row asked for.
class LogView : public QWidget {
  Q_OBJECT

 public:
  explicit LogView(QWidget* parent = nullptr);
  ~LogView() override;

  /// Names (or renames) topic @p topic; a topic shows in the Topic column and the Topics menu once named.
  void setTopicLabel(uint32_t topic, const QString& label);
  void appendEntries(uint32_t topic, std::vector<LogEntry> entries);
  void replaceEntries(uint32_t topic, std::vector<LogEntry> entries);
  /// Shows every row of @p source as topic @p topic, in place of what the topic showed.
  void setTopicRows(uint32_t topic, std::shared_ptr<const LogRowSource> source);
  /// Follows @p source as topic @p topic: drops the rows it evicted and indexes the rows added since
  /// (LogStore::update).
  void updateTopicRows(uint32_t topic, std::shared_ptr<const LogRowSource> source);
  /// Drops the topic and its rows.
  void removeTopic(uint32_t topic);

  /// Display-axis seconds of the playback cursor; NaN reveals every row.
  void setTrackerTime(double time);
  void setPlaying(bool playing);
  /// How the Time column prints a row's seconds: formatTimeLabel with the date on a wall-clock instant, as the
  /// playback readout prints the tracker.
  void setTimeLabelFormat(TimeLabelFormat format, int decimals = 3);

  [[nodiscard]] LogViewState state() const;
  /// Applies @p state without emitting stateEdited().
  void applyState(const LogViewState& state);

  [[nodiscard]] const log_view::LogStore& store() const {
    return store_;
  }
  [[nodiscard]] log_view::LogTableModel* model() const {
    return model_;
  }

 signals:
  /// A user gesture changed state(): a toggle, a filter edit, the Topics menu.
  void stateEdited();
  /// A double click on a row while paused: seek the playback to @p time (display-axis seconds).
  void seekRequested(double time);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void showEvent(QShowEvent* event) override;

 private:
  /// Applies @p mutate to the store and follows its change, keeping the selection.
  void changeStore(const std::function<log_view::StoreChange()>& mutate);
  /// Follows a store change in the model, the tracker and the selection; @p selected was taken before it.
  void followChange(const log_view::StoreChange& change, std::optional<size_t> selected);
  [[nodiscard]] log_view::LogFilter currentFilter() const;
  void applyFilters();
  /// After the model applied a filter: the selection, the scroll position and the detail pane follow.
  void onFilterApplied();
  void refreshTopics();
  void refreshOptionalColumns();
  [[nodiscard]] std::array<QToolButton*, log_view::kToggleLevelCount> levelToggles() const;
  void updateDetail();
  void selectStoreRow(std::optional<size_t> store_index);
  [[nodiscard]] std::optional<size_t> selectedStoreRow() const;
  /// The tracker's row (the last one revealed) is in the viewport.
  [[nodiscard]] bool trackerRowInView() const;
  /// A selection change: the user's unless the view is moving it (adjusting_).
  void onSelectionChanged();
  /// Selects chosen_ if it is shown, else nothing. Call with adjusting_ set.
  void showChosen();
  void scrollToTracker(bool to_bottom);
  void onRowDoubleClicked(const QModelIndex& index);
  [[nodiscard]] int fitWidth(const QString& text, int column) const;
  bool fitTimeColumn();
  void autoSizeColumns();
  void setShowFuture(bool show);

  std::unique_ptr<Ui::LogView> ui_;
  log_view::LogStore store_;
  log_view::LogTableModel* model_ = nullptr;
  std::map<uint32_t, QString> topics_;
  /// The rows of the topics handed as LogEntry lists.
  std::map<uint32_t, std::shared_ptr<VectorLogRowSource>> entry_sources_;
  /// The entry the user selected, kept while it is not shown.
  std::optional<log_view::LogStore::RowKey> chosen_;
  /// Set while the view changes the model or the selection: Qt moves the selection off removed rows, and that move is
  /// not the user's choice.
  bool adjusting_ = false;
  QStringList hidden_topics_;
  QTimer text_timer_;

  /// The store row the detail pane shows (nullopt: none); valid while detail_valid_.
  bool detail_valid_ = false;
  std::optional<size_t> detail_row_;
  QString detail_text_;

  double tracker_;
  bool playing_ = false;
  bool shown_rows_ = false;  ///< the list was positioned on the tracker once since it had rows
  bool columns_fitted_ = false;
  bool splitter_sized_ = false;
  std::optional<double> pending_seek_;
};

}  // namespace PJ
