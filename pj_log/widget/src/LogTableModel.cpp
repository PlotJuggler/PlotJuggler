// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_log/LogTableModel.h"

#include <QColor>
#include <QFont>
#include <QPalette>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ::log_view {

namespace {

QString firstLine(const std::string& message) {
  size_t end = message.find('\n');
  if (end == std::string::npos) {
    end = message.size();
  }
  if (end > 0 && message[end - 1] == '\r') {
    --end;
  }
  return QString::fromUtf8(message.data(), static_cast<qsizetype>(end));
}

}  // namespace

LogTableModel::LogTableModel(
    const LogStore& store, std::function<QString(uint32_t wire)> topic_label, QWidget* palette_source, QObject* parent)
    : QAbstractTableModel(parent),
      store_(store),
      topic_label_(std::move(topic_label)),
      palette_source_(palette_source),
      tracker_(std::numeric_limits<double>::max()) {
  slice_timer_.setInterval(0);
  connect(&slice_timer_, &QTimer::timeout, this, &LogTableModel::continueFilter);
}

QString LogTableModel::levelName(LogLevel level) {
  const auto found = std::find_if(
      kLevelNames.begin(), kLevelNames.end(), [level](const LevelName& name) { return name.level == level; });
  const std::string_view label = (found != kLevelNames.end() ? *found : kLevelNames.back()).label;
  return QString::fromLatin1(label.data(), static_cast<qsizetype>(label.size()));
}

QString LogTableModel::timeText(const LogRow& row) const {
  return formatTimeLabel(row.time, time_format_, time_decimals_, /*with_date=*/true);
}

int LogTableModel::visibleCount() const {
  return static_cast<int>(future_rows_ == FutureRows::kGrey ? filtered_.size() : revealed_);
}

int LogTableModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : visibleCount();
}

int LogTableModel::columnCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : kColumnCount;
}

QVariant LogTableModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || static_cast<size_t>(index.row()) >= filtered_.size()) {
    return {};
  }
  const size_t old_index = filtered_[static_cast<size_t>(index.row())];
  if (old_index < pending_drop_ || old_index - pending_drop_ >= store_.size()) {
    return {};  // a row whose removal is being signalled
  }
  const bool wants_message = role == Qt::DisplayRole && index.column() == kMessage;
  const LogRow row = store_.row(old_index - pending_drop_, wants_message);
  if (!row.present) {
    // Gone from its topic, not re-indexed yet: its time only.
    return role == Qt::DisplayRole && index.column() == kTime ? QVariant(timeText(row)) : QVariant();
  }
  switch (role) {
    case Qt::DisplayRole:
      switch (index.column()) {
        case kTime:
          return timeText(row);
        case kLevel:
          return levelName(row.level);
        case kSource:
          return QString::fromStdString(store_.text(row.source));
        case kCategory:
          return QString::fromStdString(store_.text(row.category));
        case kTopic:
          return topic_label_ ? topic_label_(row.wire) : QString();
        case kMessage:
          return firstLine(row.message);
        default:
          return {};
      }
    case Qt::ForegroundRole:
      if (static_cast<size_t>(index.row()) >= revealed_) {
        return palette_source_ ? palette_source_->palette().color(QPalette::PlaceholderText) : QColor(Qt::gray);
      }
      if (index.column() != kLevel) {
        return {};
      }
      if (row.level == LogLevel::kWarning) {
        return theme::status(theme::Status::Warning, theme::appTheme());
      }
      if (row.level == LogLevel::kError || row.level == LogLevel::kFatal) {
        return theme::status(theme::Status::Error, theme::appTheme());
      }
      return {};
    case Qt::BackgroundRole:
      // The row the tracker is at: the last one at or before it, a lighter cousin of the selection.
      if (static_cast<size_t>(index.row()) + 1 == revealed_) {
        QColor color = theme::selectionFill(theme::Selection::Item, theme::appTheme());
        color.setAlphaF(0.35F);
        return color;
      }
      return {};
    case Qt::FontRole:
      if (index.column() == kLevel && row.level == LogLevel::kFatal && palette_source_) {
        QFont font = palette_source_->font();
        font.setWeight(QFont::DemiBold);
        return font;
      }
      return {};
    default:
      return {};
  }
}

QVariant LogTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
    return {};
  }
  switch (section) {
    case kTime:
      return tr("Time");
    case kLevel:
      return tr("Level");
    case kSource:
      return tr("Source");
    case kCategory:
      return tr("Category");
    case kTopic:
      return tr("Topic");
    case kMessage:
      return tr("Message");
    default:
      return {};
  }
}

void LogTableModel::setFilter(const LogFilter& filter) {
  if (store_.size() > kFilterSliceRows) {
    pending_filter_ = filter;
    pending_.clear();
    pending_position_ = 0;
    slice_timer_.start();
    return;
  }
  slice_timer_.stop();
  pending_filter_.reset();
  beginResetModel();
  filter_ = filter;
  filtered_.clear();
  CompiledFilter(filter_, store_).collect(0, store_.size(), filtered_);
  revealed_ = revealedCount(store_, filtered_, tracker_);
  endResetModel();
  emit filterApplied();
}

void LogTableModel::continueFilter() {
  if (!pending_filter_) {
    slice_timer_.stop();
    return;
  }
  pending_position_ =
      CompiledFilter(*pending_filter_, store_).collect(pending_position_, store_.size(), kFilterSliceRows, pending_);
  if (pending_position_ < store_.size()) {
    return;
  }
  slice_timer_.stop();
  beginResetModel();
  filter_ = *std::exchange(pending_filter_, std::nullopt);
  filtered_ = std::move(pending_);
  pending_.clear();
  revealed_ = revealedCount(store_, filtered_, tracker_);
  endResetModel();
  emit filterApplied();
}

void LogTableModel::followPending(const StoreChange& change) {
  if (!pending_filter_) {
    return;
  }
  if (change.reset) {
    pending_.clear();
    pending_position_ = 0;
    return;
  }
  // Like filtered_: rows from old_tail on are re-scanned, evicted ones leave, the rest shift.
  std::erase_if(
      pending_, [&change](uint32_t index) { return index >= change.old_tail || index < change.dropped_front; });
  for (auto& index : pending_) {
    index -= static_cast<uint32_t>(change.dropped_front);
  }
  const size_t kept_end = change.old_tail > change.dropped_front ? change.old_tail - change.dropped_front : 0;
  const size_t scanned = pending_position_ > change.dropped_front ? pending_position_ - change.dropped_front : 0;
  pending_position_ = std::min(scanned, kept_end);
}

void LogTableModel::applyStoreChange(const StoreChange& change) {
  followPending(change);
  if (change.reset) {
    beginResetModel();
    filtered_.clear();
    revealed_ = 0;
    endResetModel();
    setFilter(pending_filter_ ? *pending_filter_ : filter_);
    return;
  }
  // The store has already changed. Removal signals go first; until the indices are shifted, data() reads
  // the kept rows through pending_drop_, so they show the same entries throughout the signals.
  pending_drop_ = change.dropped_front;

  // 1. Rows removed or replaced at the tail.
  const auto tail = std::lower_bound(filtered_.begin(), filtered_.end(), change.old_tail);
  const auto kept = static_cast<size_t>(std::distance(filtered_.begin(), tail));
  if (kept < filtered_.size()) {
    const int visible = visibleCount();
    const bool notify = static_cast<int>(kept) < visible;
    if (notify) {
      beginRemoveRows(QModelIndex(), static_cast<int>(kept), visible - 1);
    }
    filtered_.resize(kept);
    const bool moved = revealed_ > kept;
    revealed_ = std::min(revealed_, kept);
    if (notify) {
      endRemoveRows();
    }
    if (moved) {
      repaintRowBefore(revealed_);  // the tracker's row was removed: tint the new last one
    }
  }

  // 2. Rows evicted at the front.
  if (change.dropped_front > 0) {
    const auto front = std::lower_bound(filtered_.begin(), filtered_.end(), change.dropped_front);
    const auto gone = static_cast<size_t>(std::distance(filtered_.begin(), front));
    const size_t visible_gone = future_rows_ == FutureRows::kGrey ? gone : std::min(gone, revealed_);
    if (visible_gone > 0) {
      beginRemoveRows(QModelIndex(), 0, static_cast<int>(visible_gone) - 1);
    }
    filtered_.erase(filtered_.begin(), front);
    revealed_ -= std::min(gone, revealed_);
    for (auto& index : filtered_) {
      index -= static_cast<uint32_t>(change.dropped_front);
    }
    pending_drop_ = 0;
    if (visible_gone > 0) {
      endRemoveRows();
    }
  }

  // 3. New rows, not revealed yet: setTracker() settles the revealed prefix.
  const size_t start = change.old_tail > change.dropped_front ? change.old_tail - change.dropped_front : 0;
  if (start >= store_.size()) {
    return;
  }
  std::vector<uint32_t> added;
  // While a sliced filter runs, new rows follow it: the old filter would show them until it completes.
  CompiledFilter(pending_filter_ ? *pending_filter_ : filter_, store_).collect(start, store_.size(), added);
  if (added.empty()) {
    return;
  }
  const bool notify = future_rows_ == FutureRows::kGrey;
  if (notify) {
    const int first = static_cast<int>(filtered_.size());
    beginInsertRows(QModelIndex(), first, first + static_cast<int>(added.size()) - 1);
  }
  filtered_.insert(filtered_.end(), added.begin(), added.end());
  if (notify) {
    endInsertRows();
  }
}

void LogTableModel::setTracker(double tracker) {
  tracker_ = std::isnan(tracker) ? std::numeric_limits<double>::max() : tracker;
  updateRevealed();
}

void LogTableModel::updateRevealed() {
  const size_t next = revealedCount(store_, filtered_, tracker_);
  if (next == revealed_) {
    return;
  }
  const size_t low = std::min(next, revealed_);
  const size_t high = std::max(next, revealed_);
  // The tracker's row moves between rows low - 1 and high - 1: the changed range covers high - 1, and
  // low - 1 (the old row going forward, the new one going back) needs its own repaint.
  if (future_rows_ == FutureRows::kGrey) {
    revealed_ = next;
    emit dataChanged(
        index(static_cast<int>(low > 0 ? low - 1 : 0), 0), index(static_cast<int>(high) - 1, kColumnCount - 1),
        {Qt::ForegroundRole, Qt::BackgroundRole});
    return;
  }
  if (next > revealed_) {
    beginInsertRows(QModelIndex(), static_cast<int>(low), static_cast<int>(high) - 1);
    revealed_ = next;
    endInsertRows();
  } else {
    beginRemoveRows(QModelIndex(), static_cast<int>(low), static_cast<int>(high) - 1);
    revealed_ = next;
    endRemoveRows();
  }
  repaintRowBefore(low);
}

void LogTableModel::repaintRowBefore(size_t row_end) {
  const auto row = static_cast<int>(row_end) - 1;
  if (row >= 0 && row < visibleCount()) {
    emit dataChanged(index(row, 0), index(row, kColumnCount - 1), {Qt::BackgroundRole});
  }
}

void LogTableModel::setFutureRows(FutureRows mode) {
  if (mode == future_rows_) {
    return;
  }
  beginResetModel();
  future_rows_ = mode;
  endResetModel();
}

void LogTableModel::setTimeLabelFormat(TimeLabelFormat format, int decimals) {
  if (format == time_format_ && decimals == time_decimals_) {
    return;
  }
  time_format_ = format;
  time_decimals_ = decimals;
  if (rowCount() > 0) {
    emit dataChanged(index(0, kTime), index(rowCount() - 1, kTime), {Qt::DisplayRole});
  }
}

std::optional<size_t> LogTableModel::storeIndex(int row) const {
  if (row < 0 || row >= visibleCount()) {
    return std::nullopt;
  }
  return filtered_[static_cast<size_t>(row)];
}

int LogTableModel::rowOf(size_t store_index) const {
  const auto found = std::lower_bound(filtered_.begin(), filtered_.end(), store_index);
  if (found == filtered_.end() || *found != store_index) {
    return -1;
  }
  const auto row = static_cast<int>(std::distance(filtered_.begin(), found));
  return row < visibleCount() ? row : -1;
}

}  // namespace PJ::log_view
