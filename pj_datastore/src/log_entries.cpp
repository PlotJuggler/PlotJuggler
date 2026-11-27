// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/log_entries.hpp"

#include <algorithm>
#include <cassert>
#include <nlohmann/json.hpp>
#include <numeric>

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
  chunk_bytes_ += rowBytes(row);
  chunks_.back().rows.push_back(std::move(row));
}

void LogEntries::insertRow(size_t index, Row&& row) {
  const auto [chunk, offset] = locate(index);
  chunk_bytes_ += rowBytes(row);
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

// --- Mutators ---

size_t LogEntries::rowBytes(const Row& row) {
  return sizeof(Row) + row.message.size();
}

/// Walks a series' rows in order, decoding one chunk at a time.
struct LogEntries::Cursor {
  const LogEntries* series = nullptr;
  size_t index = 0;
  size_t chunk = 0;
  size_t offset = 0;
  std::vector<Row> rows;

  explicit Cursor(const LogEntries& s) : series(&s) {
    load();
  }
  [[nodiscard]] bool done() const {
    return index >= series->size();
  }
  [[nodiscard]] Timestamp timestamp() const {
    return series->timestamps_[index];
  }
  [[nodiscard]] SequentialUID uid() const {
    return series->uids_[index];
  }
  Row& row() {
    return rows[offset];
  }
  void next() {
    ++index;
    if (++offset >= rows.size()) {
      ++chunk;
      offset = 0;
      load();
    }
  }
  void load() {
    if (chunk < series->chunks_.size()) {
      series->rowsOf(series->chunks_[chunk], rows);
    } else {
      rows.clear();
    }
  }
};

void LogEntries::appendRaw(Timestamp timestamp, SequentialUID uid, Row&& row) {
  timestamps_.push_back(timestamp);
  uids_.push_back(uid);
  appendRow(std::move(row));
}

void LogEntries::remap(const LogEntries& from, Row& row, std::vector<uint32_t>& cache) {
  if (cache.size() < from.dictionary_.size()) {
    cache.resize(from.dictionary_.size(), UINT32_MAX);
  }
  for (uint32_t* id : {&row.source, &row.category}) {
    uint32_t& mapped = cache[*id];
    if (mapped == UINT32_MAX) {
      mapped = intern(from.dictionary_[*id]);
    }
    *id = mapped;
  }
}

std::vector<LogRecord> LogEntries::truncate(size_t keep) {
  std::vector<LogRecord> tail;
  if (keep >= size()) {
    return tail;
  }
  records(keep, size() - keep, [&tail](size_t, LogRecord&& record) { tail.push_back(std::move(record)); });
  Removal plan;
  plan.new_position.resize(size());
  for (size_t i = 0; i < size(); ++i) {
    plan.new_position[i] = i < keep ? static_cast<uint32_t>(i) : Removal::kRemoved;
  }
  plan.count = size() - keep;
  compactChunks(plan);
  timestamps_.resize(keep);
  uids_.resize(keep);
  std::erase_if(uid_order_, [keep](uint32_t position) { return position >= keep; });
  return tail;
}

void LogEntries::compactChunks(const Removal& plan) {
  size_t global = 0;
  std::vector<Chunk> kept_chunks;
  kept_chunks.reserve(chunks_.size());
  for (Chunk& chunk : chunks_) {
    std::vector<Row> rows;
    rowsOf(chunk, rows);
    const size_t first = global;
    global += rows.size();
    bool touched = false;
    for (size_t k = 0; k < rows.size(); ++k) {
      touched = touched || plan.new_position[first + k] == Removal::kRemoved;
    }
    if (!touched) {
      kept_chunks.push_back(std::move(chunk));
      continue;
    }
    std::vector<Row> survivors;
    for (size_t k = 0; k < rows.size(); ++k) {
      if (plan.new_position[first + k] == Removal::kRemoved) {
        chunk_bytes_ -= rowBytes(rows[k]);
      } else {
        survivors.push_back(std::move(rows[k]));
      }
    }
    if (!survivors.empty()) {
      chunk.rows = std::move(survivors);
      kept_chunks.push_back(std::move(chunk));
    }
  }
  chunks_ = std::move(kept_chunks);
  rebuildChunkStarts();
}

std::vector<SequentialUID> LogEntries::evictFront(size_t count) {
  assert(count <= size());
  if (count == 0) {
    return {};
  }
  std::vector<SequentialUID> removed(uids_.begin(), uids_.begin() + static_cast<std::ptrdiff_t>(count));
  Removal plan;
  plan.new_position.resize(size());
  for (size_t i = 0; i < size(); ++i) {
    plan.new_position[i] = i < count ? Removal::kRemoved : static_cast<uint32_t>(i - count);
  }
  plan.count = count;
  compactChunks(plan);
  timestamps_.erase(timestamps_.begin(), timestamps_.begin() + static_cast<std::ptrdiff_t>(count));
  uids_.erase(uids_.begin(), uids_.begin() + static_cast<std::ptrdiff_t>(count));
  size_t next = 0;
  for (const uint32_t position : uid_order_) {
    if (position >= count) {
      uid_order_[next++] = position - static_cast<uint32_t>(count);
    }
  }
  uid_order_.resize(next);
  evicted_ += count;
  changed(Change::kFollowable);
  return removed;
}

LogEntries::Removal LogEntries::planRemoval(const EntryPredicate& drop) const {
  Removal plan;
  plan.new_position.resize(size());
  uint32_t kept = 0;
  for (size_t i = 0; i < size(); ++i) {
    if (drop(timestamps_[i], uids_[i])) {
      plan.new_position[i] = Removal::kRemoved;
      ++plan.count;
    } else {
      plan.new_position[i] = kept++;
    }
  }
  return plan;
}

std::vector<SequentialUID> LogEntries::remove(const Removal& plan) {
  assert(plan.new_position.size() == size());
  if (plan.count == 0) {
    return {};
  }
  std::vector<SequentialUID> removed;
  removed.reserve(plan.count);
  compactChunks(plan);
  for (size_t i = 0; i < plan.new_position.size(); ++i) {
    const uint32_t to = plan.new_position[i];
    if (to == Removal::kRemoved) {
      removed.push_back(uids_[i]);
    } else {
      timestamps_[to] = timestamps_[i];
      uids_[to] = uids_[i];
    }
  }
  const size_t kept = plan.new_position.size() - plan.count;
  timestamps_.resize(kept);
  uids_.resize(kept);
  size_t next = 0;
  for (const uint32_t position : uid_order_) {
    if (plan.new_position[position] != Removal::kRemoved) {
      uid_order_[next++] = plan.new_position[position];
    }
  }
  uid_order_.resize(next);
  changed(Change::kRemoval);
  return removed;
}

std::pair<LogEntries::PushOrder, std::vector<SequentialUID>> LogEntries::removeAndPush(
    const Removal& plan, Timestamp timestamp, SequentialUID uid, const sdk::Log& log) {
  std::vector<SequentialUID> removed = remove(plan);
  return {push(timestamp, uid, log), std::move(removed)};
}

void LogEntries::clear() {
  timestamps_.clear();
  uids_.clear();
  uid_order_.clear();
  chunks_.clear();
  chunk_starts_.clear();
  chunk_bytes_ = 0;
  dictionary_.assign(1, std::string{});
  dictionary_ids_.clear();
  dictionary_ids_.emplace(std::string{}, 0);
  dictionary_bytes_ = 0;
  changed(Change::kRemoval);
}

bool LogEntries::shift(Timestamp delta) {
  if (delta == 0 || empty()) {
    return false;
  }
  // Own stamps are offsets from the store stamp, so they move along.
  for (Timestamp& timestamp : timestamps_) {
    timestamp = saturatingAdd(timestamp, delta);
  }
  changed(Change::kRemoval);
  return true;
}

void LogEntries::reuidAll() {
  for (SequentialUID& uid : uids_) {
    uid = SequentialUID::getNext();
  }
  uid_order_.resize(size());
  std::iota(uid_order_.begin(), uid_order_.end(), 0u);
  changed(Change::kRemoval);
}

void LogEntries::rebuildUidOrder() {
  uid_order_.resize(size());
  std::iota(uid_order_.begin(), uid_order_.end(), 0u);
  std::stable_sort(
      uid_order_.begin(), uid_order_.end(), [this](uint32_t a, uint32_t b) { return uids_[a] < uids_[b]; });
}

void LogEntries::appendReuidFrom(LogEntries& src) {
  if (!src.empty()) {
    // Only our rows stamped after the moved run's start interleave with it; the rest stay where they are.
    const auto merge_from = static_cast<size_t>(
        std::upper_bound(timestamps_.begin(), timestamps_.end(), src.frontTimestamp()) - timestamps_.begin());
    std::vector<LogRecord> tail = truncate(merge_from);
    std::vector<uint32_t> cache;
    Cursor moved(src);
    size_t t = 0;
    while (t < tail.size() || !moved.done()) {
      // Ours first on equal stamps, as std::inplace_merge keeps the first run first.
      if (moved.done() || (t < tail.size() && tail[t].timestamp <= moved.timestamp())) {
        Row row = toRow(tail[t].timestamp, tail[t].log);
        appendRaw(tail[t].timestamp, tail[t].uid, std::move(row));
        ++t;
      } else {
        Row row = std::move(moved.row());
        remap(src, row, cache);
        appendRaw(moved.timestamp(), SequentialUID::getNext(), std::move(row));
        moved.next();
      }
    }
  }
  rebuildUidOrder();
  changed(Change::kInsert);
  src = LogEntries{};
}

void LogEntries::adoptReuidFrom(LogEntries& src) {
  const uint64_t evicted = evicted_;
  *this = std::move(src);
  evicted_ = evicted;
  reuidAll();
  src = LogEntries{};
}

void LogEntries::mergeReuidFrom(const std::vector<LogEntries*>& sources) {
  LogEntries merged;
  std::vector<Cursor> cursors;
  cursors.reserve(sources.size() + 1);
  cursors.emplace_back(*this);
  for (LogEntries* source : sources) {
    cursors.emplace_back(*source);
  }
  std::vector<std::vector<uint32_t>> caches(cursors.size());
  for (;;) {
    size_t pick = cursors.size();
    for (size_t k = 0; k < cursors.size(); ++k) {
      // Strictly earlier wins, so equal stamps keep the listed order.
      if (!cursors[k].done() && (pick == cursors.size() || cursors[k].timestamp() < cursors[pick].timestamp())) {
        pick = k;
      }
    }
    if (pick == cursors.size()) {
      break;
    }
    Cursor& cursor = cursors[pick];
    Row row = std::move(cursor.row());
    merged.remap(*cursor.series, row, caches[pick]);
    merged.appendRaw(cursor.timestamp(), SequentialUID{}, std::move(row));
    cursor.next();
  }
  for (LogEntries* source : sources) {
    *source = LogEntries{};
  }
  adoptReuidFrom(merged);
}

}  // namespace PJ
