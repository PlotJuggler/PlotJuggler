// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_log/LogRowSource.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace PJ {

uint32_t VectorLogRowSource::intern(std::string text) {
  if (text.empty()) {
    return 0;
  }
  const auto [it, added] = dictionary_ids_.try_emplace(text, static_cast<uint32_t>(dictionary_.size()));
  if (added) {
    dictionary_.push_back(std::move(text));
  }
  return it->second;
}

uint64_t VectorLogRowSource::merge(std::vector<LogEntry> entries) {
  std::erase_if(entries, [](const LogEntry& entry) { return !std::isfinite(entry.time); });
  const uint64_t end = evicted_ + rows_.size();
  if (entries.empty()) {
    return end;
  }
  std::vector<Row> fresh;
  fresh.reserve(entries.size());
  for (LogEntry& entry : entries) {
    fresh.push_back(
        Row{
            .time = entry.time,
            .stamp_ns = entry.stamp_ns,
            .level = entry.level,
            .source = intern(std::move(entry.source)),
            .category = intern(std::move(entry.category)),
            .message = std::move(entry.message),
        });
  }
  // The new rows go after the held rows of equal time, so only the rows later than the first new one move.
  const auto first = std::upper_bound(
      rows_.begin(), rows_.end(), fresh.front().time, [](double time, const Row& row) { return time < row.time; });
  const auto position = static_cast<size_t>(std::distance(rows_.begin(), first));
  std::vector<Row> tail(std::make_move_iterator(first), std::make_move_iterator(rows_.end()));
  rows_.erase(first, rows_.end());
  std::merge(
      std::make_move_iterator(tail.begin()), std::make_move_iterator(tail.end()),
      std::make_move_iterator(fresh.begin()), std::make_move_iterator(fresh.end()), std::back_inserter(rows_),
      [](const Row& a, const Row& b) { return a.time < b.time; });
  return evicted_ + position;
}

void VectorLogRowSource::dropFront(size_t count) {
  count = std::min(count, rows_.size());
  rows_.erase(rows_.begin(), rows_.begin() + static_cast<std::ptrdiff_t>(count));
  evicted_ += count;
}

void VectorLogRowSource::messages(size_t first, size_t count, const MessageSink& sink) const {
  for (size_t index = first; index < first + count && index < rows_.size(); ++index) {
    sink(index, rows_[index].message);
  }
}

}  // namespace PJ
