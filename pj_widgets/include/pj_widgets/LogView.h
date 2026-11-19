#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_widgets/LogModel.h"
#include "pj_widgets/TimeFormat.h"

namespace Ui {
class LogView;
}

namespace PJ {

/// One log entry as a host hands it to LogView.
struct LogEntry {
  double time = 0.0;     ///< display-axis seconds: the Time column, the merge order and the tracker compare on it
  int64_t stamp_ns = 0;  ///< the entry's own stamp, shown in the detail pane only
  LogLevel level = LogLevel::kUnknown;
  std::string source;
  std::string category;  ///< empty: none
  std::string message;
};

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
  bool show_future = true;

  bool operator==(const LogViewState&) const = default;
};

/// A log list over several topics merged by time: level toggles, source/category/message filters, a detail pane with
/// Copy, the last entry at or before the tracker tinted, later entries greyed or hidden. Runtime-agnostic: the host
/// decodes its data into LogEntry rows keyed by a topic id of its choosing, sets the tracker and the time format, and
/// answers seekRequested().
///
/// Data calls: an entry list is in ascending time and, for appendEntries(), not earlier than what the topic holds
/// (anything else is a replaceEntries()). Topic ids order rows of equal time. The view keeps at most
/// log_view::LogStore::kDefaultCapacity rows, dropping the oldest. A selected row keeps its selection across every data
/// call that leaves it in the store.
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
  /// Converts @p entries into rows of @p topic, interning their strings.
  std::vector<log_view::LogRow> toRows(uint32_t topic, std::vector<LogEntry> entries);
  /// Follows a store change in the model, the tracker and the selection; @p selected_key was taken before it.
  void followChange(
      const log_view::StoreChange& change, std::optional<size_t> selected,
      std::optional<log_view::LogStore::RowKey> selected_key);
  [[nodiscard]] log_view::LogFilter currentFilter() const;
  void applyFilters();
  void refreshTopics();
  void refreshCategories();
  void updateDetail();
  void selectStoreRow(std::optional<size_t> store_index);
  [[nodiscard]] std::optional<size_t> selectedStoreRow() const;
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
  std::map<uint32_t, uint64_t> next_seq_;
  QStringList hidden_topics_;
  QTimer text_timer_;
  std::vector<uint32_t> applied_hidden_wires_;
  size_t categories_listed_ = 0;

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
