// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/log_entries.hpp"

#include <algorithm>
#include <cassert>
#include <nlohmann/json.hpp>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_datastore/saturating_time.hpp"

namespace PJ {

bool isCanonicalLogMetadata(std::string_view metadata_json) {
  const auto metadata = nlohmann::json::parse(metadata_json, nullptr, /*allow_exceptions=*/false);
  if (!metadata.is_object()) {
    return false;
  }
  const auto type = metadata.find("builtin_object_type");
  if (type == metadata.end() || !type->is_string() || type->get<std::string>() != "kLog") {
    return false;
  }
  const auto encoding = metadata.find("payload_encoding");
  return encoding == metadata.end() || (encoding->is_string() && encoding->get<std::string>().empty());
}

struct LogEntries::Chunk {
  std::vector<Row> rows;
};

LogEntries::LogEntries() : dictionary_{std::string{}} {
  dictionary_ids_.emplace(std::string{}, 0);
  changed(Change::kRemoval);
}
LogEntries::LogEntries(LogEntries&&) noexcept = default;
LogEntries& LogEntries::operator=(LogEntries&&) noexcept = default;
LogEntries::~LogEntries() = default;

void LogEntries::changed(Change change) noexcept {
  identity_.content = nextContentRevision();
  if (change != Change::kFollowable) {
    identity_.rewrite = identity_.content;
  }
  if (change == Change::kRemoval) {
    identity_.removal = identity_.content;
  }
}

uint32_t LogEntries::intern(std::string_view text) {
  if (text.empty()) {
    return 0;
  }
  const auto it = dictionary_ids_.find(std::string(text));
  if (it != dictionary_ids_.end()) {
    return it->second;
  }
  const auto id = static_cast<uint32_t>(dictionary_.size());
  dictionary_.emplace_back(text);
  dictionary_ids_.emplace(std::string(text), id);
  dictionary_bytes_ += 2 * text.size() + 64;
  return id;
}

LogEntries::Row LogEntries::toRow(Timestamp timestamp, const sdk::Log& log) {
  Row row;
  row.level = static_cast<uint8_t>(log.level);
  row.source = intern(log.source);
  row.category = intern(log.category);
  row.stamp_offset = log.timestamp_ns == 0 ? kNoOwnStamp : saturatingSub(log.timestamp_ns, timestamp);
  row.message = log.message;
  return row;
}

sdk::Log LogEntries::toLog(Timestamp timestamp, const Row& row) const {
  sdk::Log log;
  log.timestamp_ns = row.stamp_offset == kNoOwnStamp ? 0 : saturatingAdd(timestamp, row.stamp_offset);
  log.level = static_cast<sdk::Log::Level>(row.level);
  log.source = dictionary_[row.source];
  log.category = dictionary_[row.category];
  log.message = row.message;
  return log;
}

// --- Chunks ---

std::pair<size_t, size_t> LogEntries::locate(size_t index) const {
  assert(index < size());
  const auto it = std::upper_bound(chunk_starts_.begin(), chunk_starts_.end(), index);
  const auto chunk = static_cast<size_t>(it - chunk_starts_.begin()) - 1;
  return {chunk, index - chunk_starts_[chunk]};
}

void LogEntries::rebuildChunkStarts() {
  chunk_starts_.resize(chunks_.size());
  size_t start = 0;
  for (size_t i = 0; i < chunks_.size(); ++i) {
    chunk_starts_[i] = start;
    start += chunks_[i].rows.size();
  }
}

void LogEntries::appendRow(Row&& row) {
  if (chunks_.empty() || chunks_.back().rows.size() >= kChunkRows) {
    chunk_starts_.push_back(chunks_.empty() ? 0 : chunk_starts_.back() + chunks_.back().rows.size());
    chunks_.emplace_back();
  }
  chunk_bytes_ += sizeof(Row) + row.message.size();
  chunks_.back().rows.push_back(std::move(row));
}

void LogEntries::insertRow(size_t index, Row&& row) {
  const auto [chunk, offset] = locate(index);
  chunk_bytes_ += sizeof(Row) + row.message.size();
  auto& rows = chunks_[chunk].rows;
  rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(offset), std::move(row));
  for (size_t i = chunk + 1; i < chunk_starts_.size(); ++i) {
    ++chunk_starts_[i];
  }
}

void LogEntries::rowsOf(const Chunk& chunk, std::vector<Row>& out) const {
  out = chunk.rows;
}

// --- Lookups ---

std::optional<size_t> LogEntries::indexAtOrBefore(Timestamp t) const {
  const auto it = std::upper_bound(timestamps_.begin(), timestamps_.end(), t);
  if (it == timestamps_.begin()) {
    return std::nullopt;
  }
  return static_cast<size_t>(it - timestamps_.begin()) - 1;
}

std::vector<LogEntries::TimeRangeEntry> LogEntries::rangeByTime(Timestamp lo, Timestamp hi) const {
  std::vector<TimeRangeEntry> out;
  if (hi <= lo) {
    return out;
  }
  const auto first = std::upper_bound(timestamps_.begin(), timestamps_.end(), lo);
  const auto last = std::upper_bound(timestamps_.begin(), timestamps_.end(), hi);
  out.reserve(static_cast<size_t>(last - first));
  for (auto it = first; it != last; ++it) {
    const auto idx = static_cast<size_t>(it - timestamps_.begin());
    out.push_back(TimeRangeEntry{uids_[idx], *it});
  }
  return out;
}

std::optional<size_t> LogEntries::indexOfUid(SequentialUID uid) const {
  const auto it = std::lower_bound(
      uid_order_.begin(), uid_order_.end(), uid, [this](uint32_t pos, SequentialUID u) { return uids_[pos] < u; });
  if (it == uid_order_.end() || uids_[*it] != uid) {
    return std::nullopt;
  }
  return *it;
}

SequentialUID LogEntries::firstUid() const {
  return uid_order_.empty() ? SequentialUID{} : uids_[uid_order_.front()];
}

SequentialUID LogEntries::nextUidAfter(SequentialUID after) const {
  const auto it = std::upper_bound(
      uid_order_.begin(), uid_order_.end(), after, [this](SequentialUID u, uint32_t pos) { return u < uids_[pos]; });
  return it == uid_order_.end() ? SequentialUID{} : uids_[*it];
}

SequentialUID LogEntries::maxUidAtOrBefore(Timestamp t) const {
  const auto end =
      static_cast<size_t>(std::upper_bound(timestamps_.begin(), timestamps_.end(), t) - timestamps_.begin());
  SequentialUID max_uid;
  for (size_t i = 0; i < end; ++i) {
    max_uid = std::max(max_uid, uids_[i]);
  }
  return max_uid;
}

std::optional<Timestamp> LogEntries::oldestTimestampAfterUid(SequentialUID after) const {
  auto it = std::upper_bound(
      uid_order_.begin(), uid_order_.end(), after, [this](SequentialUID u, uint32_t pos) { return u < uids_[pos]; });
  std::optional<Timestamp> oldest;
  for (; it != uid_order_.end(); ++it) {
    oldest = std::min(oldest.value_or(timestamps_[*it]), timestamps_[*it]);
  }
  return oldest;
}

// --- Columns ---

sdk::Log::Level LogEntries::level(size_t index) const {
  const auto [chunk, offset] = locate(index);
  return static_cast<sdk::Log::Level>(chunks_[chunk].rows[offset].level);
}

uint32_t LogEntries::sourceId(size_t index) const {
  const auto [chunk, offset] = locate(index);
  return chunks_[chunk].rows[offset].source;
}

uint32_t LogEntries::categoryId(size_t index) const {
  const auto [chunk, offset] = locate(index);
  return chunks_[chunk].rows[offset].category;
}

Timestamp LogEntries::ownStamp(size_t index) const {
  const auto [chunk, offset] = locate(index);
  const int64_t stamp_offset = chunks_[chunk].rows[offset].stamp_offset;
  return stamp_offset == kNoOwnStamp ? 0 : saturatingAdd(timestamps_[index], stamp_offset);
}

void LogEntries::messages(size_t first, size_t count, const MessageSink& sink) const {
  records(first, count, [&sink](size_t index, LogRecord&& record) { sink(index, record.log.message); });
}

void LogEntries::records(size_t first, size_t count, const std::function<void(size_t, LogRecord&&)>& sink) const {
  const size_t end = std::min(size(), first + count);
  std::vector<Row> rows;
  size_t index = first;
  while (index < end) {
    const auto [chunk, offset] = locate(index);
    rowsOf(chunks_[chunk], rows);
    for (size_t k = offset; k < rows.size() && index < end; ++k, ++index) {
      sink(index, LogRecord{timestamps_[index], uids_[index], toLog(timestamps_[index], rows[k])});
    }
  }
}

LogRecord LogEntries::recordAt(size_t index) const {
  LogRecord out;
  records(index, 1, [&out](size_t, LogRecord&& record) { out = std::move(record); });
  return out;
}

std::vector<uint8_t> LogEntries::canonicalBytesAt(size_t index) const {
  auto bytes = serializeBuiltinObject(sdk::BuiltinObject(recordAt(index).log));
  assert(bytes.has_value());
  return bytes ? std::move(*bytes) : std::vector<uint8_t>{};
}

size_t LogEntries::memoryBytes() const noexcept {
  return timestamps_.capacity() * sizeof(Timestamp) + uids_.capacity() * sizeof(SequentialUID) +
         uid_order_.capacity() * sizeof(uint32_t) + chunks_.capacity() * (sizeof(Chunk) + sizeof(size_t)) +
         chunk_bytes_ + dictionary_bytes_;
}

// --- Mutation ---

LogEntries::PushOrder LogEntries::push(Timestamp timestamp, SequentialUID uid, const sdk::Log& log) {
  assert(size() < UINT32_MAX);
  Row row = toRow(timestamp, log);
  if (timestamps_.empty() || timestamp >= timestamps_.back()) {
    timestamps_.push_back(timestamp);
    uids_.push_back(uid);
    uid_order_.push_back(static_cast<uint32_t>(timestamps_.size() - 1));
    appendRow(std::move(row));
    changed(Change::kFollowable);
    return PushOrder::kInOrderAppend;
  }
  const auto pos = std::upper_bound(timestamps_.begin(), timestamps_.end(), timestamp);
  const auto idx = static_cast<size_t>(pos - timestamps_.begin());
  insertRow(idx, std::move(row));
  timestamps_.insert(pos, timestamp);
  uids_.insert(uids_.begin() + static_cast<std::ptrdiff_t>(idx), uid);
  for (auto& p : uid_order_) {
    if (p >= idx) {
      ++p;
    }
  }
  uid_order_.push_back(static_cast<uint32_t>(idx));
  changed(Change::kInsert);
  return PushOrder::kOutOfOrderInsert;
}

}  // namespace PJ
