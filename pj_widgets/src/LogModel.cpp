// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/LogModel.h"

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

/// Merge order: time, then wire, then arrival within the wire.
bool before(const LogRow& a, const LogRow& b) {
  if (a.time != b.time) {
    return a.time < b.time;
  }
  if (a.wire != b.wire) {
    return a.wire < b.wire;
  }
  return a.seq < b.seq;
}

char lowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Lower-cased exactly as containsCaseInsensitive() folds the haystack: std::tolower follows the C locale, which Qt
/// sets from the environment, and could fold UTF-8 bytes the haystack keeps.
std::string lowerNeedle(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), lowerAscii);
  return text;
}

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

// --- StoreChange ---

void StoreChange::then(const StoreChange& next) {
  if (reset || next.reset) {
    reset = true;
    return;
  }
  // `next` is expressed against the state after this change, where the unchanged rows are
  // [0, old_tail - dropped_front); its own unchanged range starts at its dropped_front.
  old_tail = std::min(old_tail, next.old_tail + dropped_front);
  dropped_front += next.dropped_front;
}

// --- LogStore ---

LogStore::LogStore(size_t capacity) : capacity_(capacity) {
  strings_.emplace_back();  // id 0 = ""
  is_category_.push_back(false);
}

uint32_t LogStore::intern(std::string_view text) {
  if (text.empty()) {
    return 0;
  }
  const auto found = string_ids_.find(text);
  if (found != string_ids_.end()) {
    return found->second;
  }
  const auto id = static_cast<uint32_t>(strings_.size());
  strings_.emplace_back(text);
  string_ids_.emplace(strings_.back(), id);
  is_category_.push_back(false);
  return id;
}

uint32_t LogStore::internCategory(std::string_view text) {
  const uint32_t id = intern(text);
  if (id != 0 && !is_category_[id]) {
    is_category_[id] = true;
    categories_.push_back(id);
  }
  return id;
}

StoreChange LogStore::insert(uint32_t wire, std::vector<LogRow> rows) {
  StoreChange change = StoreChange::none(rows_.size());
  if (rows.empty()) {
    return change;
  }
  auto& times = wire_times_[wire];
  for (auto& row : rows) {
    row.wire = wire;
    times.push_back(row.time);
  }
  const auto first = std::lower_bound(rows_.begin(), rows_.end(), rows.front(), before);
  const auto position = static_cast<size_t>(std::distance(rows_.begin(), first));
  change.old_tail = position;
  if (position == rows_.size()) {
    std::move(rows.begin(), rows.end(), std::back_inserter(rows_));
  } else {
    // Merge the new rows with the tail they interleave with; the head stays where it is.
    std::vector<LogRow> tail(std::make_move_iterator(first), std::make_move_iterator(rows_.end()));
    rows_.erase(first, rows_.end());
    std::merge(
        std::make_move_iterator(tail.begin()), std::make_move_iterator(tail.end()),
        std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()), std::back_inserter(rows_), before);
  }
  evictOverflow(change);
  return change;
}

StoreChange LogStore::removeWire(uint32_t wire) {
  StoreChange change = StoreChange::none(rows_.size());
  const auto times = wire_times_.find(wire);
  if (times == wire_times_.end()) {
    return change;
  }
  if (!times->second.empty()) {
    LogRow probe;
    probe.time = times->second.front();
    probe.wire = wire;
    const auto first = std::lower_bound(rows_.begin(), rows_.end(), probe, before);
    change.old_tail = static_cast<size_t>(std::distance(rows_.begin(), first));
    const auto kept = std::remove_if(first, rows_.end(), [wire](const LogRow& row) { return row.wire == wire; });
    rows_.erase(kept, rows_.end());
  }
  wire_times_.erase(times);
  return change;
}

StoreChange LogStore::replaceWire(uint32_t wire, std::vector<LogRow> rows) {
  StoreChange change = removeWire(wire);
  change.then(insert(wire, std::move(rows)));
  return change;
}

LogStore::RowKey LogStore::keyOf(size_t index) const {
  const LogRow& row = rows_[index];
  LogRow probe;
  probe.time = row.time;
  probe.wire = row.wire;
  const auto end = rows_.begin() + static_cast<std::ptrdiff_t>(index);
  const auto first = std::lower_bound(rows_.begin(), end, probe, before);
  return RowKey{row.time, row.wire, static_cast<size_t>(std::distance(first, end))};
}

std::optional<size_t> LogStore::indexOf(const RowKey& key) const {
  LogRow probe;
  probe.time = key.time;
  probe.wire = key.wire;
  const auto first = std::lower_bound(rows_.begin(), rows_.end(), probe, before);
  const size_t index = static_cast<size_t>(std::distance(rows_.begin(), first)) + key.rank;
  if (index >= rows_.size() || rows_[index].time != key.time || rows_[index].wire != key.wire) {
    return std::nullopt;
  }
  return index;
}

void LogStore::evictOverflow(StoreChange& change) {
  while (rows_.size() > capacity_) {
    auto times = wire_times_.find(rows_.front().wire);
    if (times != wire_times_.end() && !times->second.empty()) {
      times->second.pop_front();
    }
    rows_.pop_front();
    ++change.dropped_front;
  }
}

// --- Filter ---

bool containsCaseInsensitive(const std::string& haystack, const std::string& lower_needle) {
  if (lower_needle.empty()) {
    return true;
  }
  const auto found = std::search(
      haystack.begin(), haystack.end(), lower_needle.begin(), lower_needle.end(),
      [](char a, char b) { return lowerAscii(a) == b; });
  return found != haystack.end();
}

CompiledFilter::CompiledFilter(const LogFilter& filter, const LogStore& store)
    : store_(store),
      levels_(filter.levels),
      source_needle_(lowerNeedle(filter.source)),
      category_needle_(lowerNeedle(filter.category)),
      needle_(lowerNeedle(filter.text)),
      hidden_wires_(filter.hidden_wires) {
  levels_[static_cast<size_t>(LogLevel::kUnknown)] = true;
  std::sort(hidden_wires_.begin(), hidden_wires_.end());
}

bool CompiledFilter::accepts(const LogRow& row) const {
  const auto level = static_cast<size_t>(row.level);
  if (level < levels_.size() && !levels_[level]) {
    return false;
  }
  if (!containsCaseInsensitive(store_.text(row.source), source_needle_) ||
      !containsCaseInsensitive(store_.text(row.category), category_needle_)) {
    return false;
  }
  if (!hidden_wires_.empty() && std::binary_search(hidden_wires_.begin(), hidden_wires_.end(), row.wire)) {
    return false;
  }
  return containsCaseInsensitive(row.message, needle_);
}

void CompiledFilter::collect(size_t from, size_t to, std::vector<uint32_t>& out) const {
  for (size_t i = from; i < to; ++i) {
    if (accepts(store_.row(i))) {
      out.push_back(static_cast<uint32_t>(i));
    }
  }
}

size_t revealedCount(const LogStore& store, const std::vector<uint32_t>& filtered, double tracker) {
  const auto end = std::upper_bound(filtered.begin(), filtered.end(), tracker, [&store](double t, uint32_t index) {
    return t < store.row(index).time;
  });
  return static_cast<size_t>(std::distance(filtered.begin(), end));
}

void stripTerminalEscapes(std::string& text) {
  constexpr char kEsc = '\x1b';
  size_t in = text.find(kEsc);
  if (in == std::string::npos) {
    return;
  }
  size_t out = in;
  const size_t size = text.size();
  while (in < size) {
    if (text[in] != kEsc) {
      text[out++] = text[in++];
      continue;
    }
    ++in;
    if (in >= size) {
      break;
    }
    if (text[in] == '[') {
      // CSI: parameter and intermediate bytes, then one final byte in 0x40..0x7E.
      ++in;
      while (in < size &&
             (static_cast<unsigned char>(text[in]) < 0x40 || static_cast<unsigned char>(text[in]) > 0x7E)) {
        ++in;
      }
      ++in;
    } else if (text[in] == ']') {
      // OSC: up to BEL or ESC backslash.
      ++in;
      while (in < size && text[in] != '\a' && !(text[in] == kEsc && in + 1 < size && text[in + 1] == '\\')) {
        ++in;
      }
      in += (in < size && text[in] == kEsc) ? 2 : 1;
    } else {
      // Any other: intermediate bytes in 0x20..0x2F, then one final byte (ESC ( B, ESC 7, ...).
      while (in < size && text[in] >= 0x20 && text[in] <= 0x2F) {
        ++in;
      }
      ++in;
    }
  }
  text.resize(out);
}

// --- LogTableModel ---

LogTableModel::LogTableModel(
    const LogStore& store, std::function<QString(uint32_t wire)> topic_label, QWidget* palette_source, QObject* parent)
    : QAbstractTableModel(parent),
      store_(store),
      topic_label_(std::move(topic_label)),
      palette_source_(palette_source),
      tracker_(std::numeric_limits<double>::max()) {}

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
  const LogRow& row = store_.row(old_index - pending_drop_);
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
      if (row.level == LogLevel::kFatal && palette_source_) {
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
  beginResetModel();
  filter_ = filter;
  filtered_.clear();
  CompiledFilter(filter_, store_).collect(0, store_.size(), filtered_);
  revealed_ = revealedCount(store_, filtered_, tracker_);
  endResetModel();
}

void LogTableModel::applyStoreChange(const StoreChange& change) {
  if (change.reset) {
    setFilter(filter_);
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
  CompiledFilter(filter_, store_).collect(start, store_.size(), added);
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
