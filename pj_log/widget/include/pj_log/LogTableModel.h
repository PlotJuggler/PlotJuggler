#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QAbstractTableModel>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "pj_log/LogStore.h"
#include "pj_widgets/TimeFormat.h"

namespace PJ::log_view {

/// What to do with rows later than the tracker.
enum class FutureRows { kGrey, kHide };

/// Table model over the filtered rows of a LogStore; the last row at or before the tracker is tinted. A Warn, Error or
/// Fatal level colours its Level cell only, so the other columns read in the normal text colour.
class LogTableModel : public QAbstractTableModel {
  Q_OBJECT

 public:
  enum Column { kTime = 0, kLevel, kSource, kCategory, kTopic, kMessage, kColumnCount };

  /// A filter over more rows than this runs in slices of this many rows, one per event-loop turn.
  static constexpr size_t kFilterSliceRows = 50'000;

  /// @p palette_source supplies the colours of greyed rows; @p topic_label names a wire.
  LogTableModel(
      const LogStore& store, std::function<QString(uint32_t wire)> topic_label, QWidget* palette_source,
      QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  /// Re-run the filter over the whole store: at once on a small store, else in slices, the previous result showing
  /// until the new one is complete. Emits filterApplied() when it is.
  void setFilter(const LogFilter& filter);
  /// Follow a store mutation, re-filtering only the rows it touched. The rows it adds are not revealed
  /// until the next setTracker(), which the caller makes right after.
  void applyStoreChange(const StoreChange& change);
  void setTracker(double tracker);
  void setFutureRows(FutureRows mode);
  /// How the Time column prints a row's display seconds (formatTimeLabel, with the date on a wall-clock instant).
  void setTimeLabelFormat(TimeLabelFormat format, int decimals);

  /// Filtered rows with time <= tracker.
  [[nodiscard]] int revealedRows() const {
    return static_cast<int>(revealed_);
  }
  /// Store index of a model row.
  [[nodiscard]] std::optional<size_t> storeIndex(int row) const;
  /// Model row showing store index @p index, or -1.
  [[nodiscard]] int rowOf(size_t index) const;
  /// The text of @p row's Time column.
  [[nodiscard]] QString timeText(const LogRow& row) const;

  static QString levelName(LogLevel level);

 signals:
  void filterApplied();

 private:
  /// Runs one slice of the pending filter; applies it when complete.
  void continueFilter();
  /// Follows a store change in the pending filter's partial result.
  void followPending(const StoreChange& change);
  int visibleCount() const;
  void updateRevealed();
  /// Repaints the background of row @p row_end - 1 (the tracker's row when row_end rows are revealed).
  void repaintRowBefore(size_t row_end);

  const LogStore& store_;
  std::function<QString(uint32_t)> topic_label_;
  QPointer<QWidget> palette_source_;
  LogFilter filter_;
  std::vector<uint32_t> filtered_;  ///< store indices that pass the filter, ascending
  size_t revealed_ = 0;             ///< prefix of filtered_ with time <= tracker_
  /// Store rows evicted while applyStoreChange() signals the removals: filtered_ still holds the indices
  /// from before the eviction until then.
  size_t pending_drop_ = 0;
  /// A filter being applied in slices: its rows so far cover store rows [0, pending_position_).
  std::optional<LogFilter> pending_filter_;
  std::vector<uint32_t> pending_;
  size_t pending_position_ = 0;
  QTimer slice_timer_;
  double tracker_;
  FutureRows future_rows_ = FutureRows::kGrey;
  TimeLabelFormat time_format_ = TimeLabelFormat::kSeconds;
  int time_decimals_ = 3;
};

}  // namespace PJ::log_view
