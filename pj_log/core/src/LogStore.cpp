// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_log/LogStore.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

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

}  // namespace PJ::log_view
