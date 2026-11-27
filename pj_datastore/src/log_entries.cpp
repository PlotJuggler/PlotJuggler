// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/log_entries.hpp"

#include <zstd.h>

#include <algorithm>
#include <cassert>
#include <cstring>
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

namespace {

/// Bytes per id that hold every id up to @p max_id.
uint8_t idWidth(uint32_t max_id) {
  return max_id <= UINT8_MAX ? 1 : max_id <= UINT16_MAX ? 2 : 4;
}

uint32_t readId(const std::vector<uint8_t>& packed, uint8_t width, size_t index) {
  uint32_t id = 0;
  std::memcpy(&id, packed.data() + index * width, width);  // little-endian hosts only, as the codecs assume
  return id;
}

}  // namespace

/// A chunk is open (plain rows) or sealed. Every column of a sealed chunk but the messages reads in O(1).
struct LogEntries::Chunk {
  struct Sealed {
    size_t rows = 0;
    bool level_constant = false;
    std::vector<uint8_t> levels;  ///< empty when level_constant: then `constant_level`
    uint8_t constant_level = 0;
    uint8_t source_width = 1;
    uint8_t category_width = 1;
    std::vector<uint8_t> sources;
    std::vector<uint8_t> categories;
    bool stamp_constant = false;
    int64_t constant_stamp = 0;
    /// One zstd frame: [uint32 message length] * rows, [int64 stamp offset] * rows unless stamp_constant, then the
    /// message bytes.
    std::vector<uint8_t> messages;
    size_t raw_message_bytes = 0;

    [[nodiscard]] size_t bytes() const {
      return sizeof(Sealed) + levels.capacity() + sources.capacity() + categories.capacity() + messages.capacity();
    }
  };

  std::vector<Row> rows;
  std::optional<Sealed> sealed;

  [[nodiscard]] size_t size() const {
    return sealed ? sealed->rows : rows.size();
  }
  [[nodiscard]] size_t bytes() const {
    if (sealed) {
      return sealed->bytes();
    }
    size_t total = 0;
    for (const Row& row : rows) {
      total += rowBytes(row);
    }
    return total;
  }
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
    start += chunks_[i].size();
  }
}

void LogEntries::appendRow(Row&& row) {
  if (chunks_.empty() || chunks_.back().sealed) {
    chunk_starts_.push_back(chunks_.empty() ? 0 : chunk_starts_.back() + chunks_.back().size());
    chunks_.emplace_back();
  }
  Chunk& last = chunks_.back();
  chunk_bytes_ += rowBytes(row);
  last.rows.push_back(std::move(row));
  if (last.rows.size() >= kChunkRows) {
    seal(last);
  }
}

void LogEntries::insertRow(size_t index, Row&& row) {
  const auto [chunk_index, offset] = locate(index);
  Chunk& chunk = chunks_[chunk_index];
  const bool was_sealed = chunk.sealed.has_value();
  unseal(chunk);
  chunk_bytes_ += rowBytes(row);
  chunk.rows.insert(chunk.rows.begin() + static_cast<std::ptrdiff_t>(offset), std::move(row));
  if (was_sealed) {
    seal(chunk);
  }
  for (size_t i = chunk_index + 1; i < chunk_starts_.size(); ++i) {
    ++chunk_starts_[i];
  }
}

void LogEntries::seal(Chunk& chunk) {
  if (chunk.sealed || chunk.rows.empty()) {
    return;
  }
  const std::vector<Row>& rows = chunk.rows;
  Chunk::Sealed sealed;
  sealed.rows = rows.size();

  sealed.constant_level = rows.front().level;
  sealed.level_constant = std::ranges::all_of(rows, [&](const Row& r) { return r.level == sealed.constant_level; });
  if (!sealed.level_constant) {
    sealed.levels.reserve(rows.size());
    for (const Row& r : rows) {
      sealed.levels.push_back(r.level);
    }
  }

  uint32_t max_source = 0;
  uint32_t max_category = 0;
  for (const Row& r : rows) {
    max_source = std::max(max_source, r.source);
    max_category = std::max(max_category, r.category);
  }
  sealed.source_width = idWidth(max_source);
  sealed.category_width = idWidth(max_category);
  sealed.sources.resize(rows.size() * sealed.source_width);
  sealed.categories.resize(rows.size() * sealed.category_width);
  for (size_t k = 0; k < rows.size(); ++k) {
    std::memcpy(sealed.sources.data() + k * sealed.source_width, &rows[k].source, sealed.source_width);
    std::memcpy(sealed.categories.data() + k * sealed.category_width, &rows[k].category, sealed.category_width);
  }

  sealed.constant_stamp = rows.front().stamp_offset;
  sealed.stamp_constant =
      std::ranges::all_of(rows, [&](const Row& r) { return r.stamp_offset == sealed.constant_stamp; });

  const size_t stamp_bytes = sealed.stamp_constant ? 0 : rows.size() * sizeof(int64_t);
  std::vector<uint8_t> raw(rows.size() * sizeof(uint32_t) + stamp_bytes);
  for (size_t k = 0; k < rows.size(); ++k) {
    const auto length = static_cast<uint32_t>(rows[k].message.size());
    std::memcpy(raw.data() + k * sizeof(uint32_t), &length, sizeof(uint32_t));
    if (!sealed.stamp_constant) {
      std::memcpy(
          raw.data() + rows.size() * sizeof(uint32_t) + k * sizeof(int64_t), &rows[k].stamp_offset, sizeof(int64_t));
    }
  }
  for (const Row& r : rows) {
    raw.insert(raw.end(), r.message.begin(), r.message.end());
  }
  sealed.raw_message_bytes = raw.size();
  sealed.messages.resize(ZSTD_compressBound(raw.size()));
  const size_t written = ZSTD_compress(sealed.messages.data(), sealed.messages.size(), raw.data(), raw.size(), 3);
  if (ZSTD_isError(written) != 0) {
    return;  // stays open: correct, only larger
  }
  sealed.messages.resize(written);
  sealed.messages.shrink_to_fit();

  chunk_bytes_ -= chunk.bytes();
  chunk.sealed = std::move(sealed);
  chunk.rows = {};
  chunk_bytes_ += chunk.bytes();
}

void LogEntries::unseal(Chunk& chunk) {
  if (!chunk.sealed) {
    return;
  }
  std::vector<Row> rows;
  rowsOf(chunk, rows);
  chunk_bytes_ -= chunk.bytes();
  chunk.sealed.reset();
  chunk.rows = std::move(rows);
  chunk_bytes_ += chunk.bytes();
}

void LogEntries::rowsOf(const Chunk& chunk, std::vector<Row>& out) const {
  if (!chunk.sealed) {
    out = chunk.rows;
    return;
  }
  const Chunk::Sealed& sealed = *chunk.sealed;
  std::vector<uint8_t> raw(sealed.raw_message_bytes);
  const size_t read = ZSTD_decompress(raw.data(), raw.size(), sealed.messages.data(), sealed.messages.size());
  assert(ZSTD_isError(read) == 0 && read == raw.size());
  (void)read;
  out.resize(sealed.rows);
  const size_t stamps_at = sealed.rows * sizeof(uint32_t);
  size_t at = stamps_at + (sealed.stamp_constant ? 0 : sealed.rows * sizeof(int64_t));
  for (size_t k = 0; k < sealed.rows; ++k) {
    Row& row = out[k];
    row.level = sealed.level_constant ? sealed.constant_level : sealed.levels[k];
    row.source = readId(sealed.sources, sealed.source_width, k);
    row.category = readId(sealed.categories, sealed.category_width, k);
    row.stamp_offset = sealed.constant_stamp;
    if (!sealed.stamp_constant) {
      std::memcpy(&row.stamp_offset, raw.data() + stamps_at + k * sizeof(int64_t), sizeof(int64_t));
    }
    uint32_t length = 0;
    std::memcpy(&length, raw.data() + k * sizeof(uint32_t), sizeof(uint32_t));
    row.message.assign(reinterpret_cast<const char*>(raw.data() + at), length);
    at += length;
  }
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

size_t LogEntries::uidRank(SequentialUID uid, bool after) const {
  size_t lo = 0;
  size_t hi = size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const SequentialUID at = uids_[orderAt(mid)];
    if (after ? !(uid < at) : at < uid) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

void LogEntries::materializeUidOrder() {
  if (uid_order_identity_) {
    uid_order_.resize(size());
    std::iota(uid_order_.begin(), uid_order_.end(), 0u);
    uid_order_identity_ = false;
  }
}

std::optional<size_t> LogEntries::indexOfUid(SequentialUID uid) const {
  const size_t k = uidRank(uid, false);
  if (k == size() || uids_[orderAt(k)] != uid) {
    return std::nullopt;
  }
  return orderAt(k);
}

SequentialUID LogEntries::firstUid() const {
  return empty() ? SequentialUID{} : uids_[orderAt(0)];
}

SequentialUID LogEntries::nextUidAfter(SequentialUID after) const {
  const size_t k = uidRank(after, true);
  return k == size() ? SequentialUID{} : uids_[orderAt(k)];
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
  std::optional<Timestamp> oldest;
  for (size_t k = uidRank(after, true); k < size(); ++k) {
    const Timestamp stamp = timestamps_[orderAt(k)];
    oldest = std::min(oldest.value_or(stamp), stamp);
  }
  return oldest;
}

// --- Columns ---

sdk::Log::Level LogEntries::level(size_t index) const {
  const auto [chunk, offset] = locate(index);
  const Chunk& c = chunks_[chunk];
  if (!c.sealed) {
    return static_cast<sdk::Log::Level>(c.rows[offset].level);
  }
  return static_cast<sdk::Log::Level>(c.sealed->level_constant ? c.sealed->constant_level : c.sealed->levels[offset]);
}

uint32_t LogEntries::sourceId(size_t index) const {
  const auto [chunk, offset] = locate(index);
  const Chunk& c = chunks_[chunk];
  return c.sealed ? readId(c.sealed->sources, c.sealed->source_width, offset) : c.rows[offset].source;
}

uint32_t LogEntries::categoryId(size_t index) const {
  const auto [chunk, offset] = locate(index);
  const Chunk& c = chunks_[chunk];
  return c.sealed ? readId(c.sealed->categories, c.sealed->category_width, offset) : c.rows[offset].category;
}

Timestamp LogEntries::ownStamp(size_t index) const {
  const auto [chunk, offset] = locate(index);
  const Chunk& c = chunks_[chunk];
  int64_t stamp_offset = 0;
  if (!c.sealed) {
    stamp_offset = c.rows[offset].stamp_offset;
  } else if (c.sealed->stamp_constant) {
    stamp_offset = c.sealed->constant_stamp;
  } else {
    std::vector<Row> rows;  // the offsets are in the compressed frame
    rowsOf(c, rows);
    stamp_offset = rows[offset].stamp_offset;
  }
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
    if (!uid_order_identity_) {
      uid_order_.push_back(static_cast<uint32_t>(timestamps_.size() - 1));
    }
    appendRow(std::move(row));
    changed(Change::kFollowable);
    return PushOrder::kInOrderAppend;
  }
  materializeUidOrder();
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
  if (!uid_order_identity_) {
    std::erase_if(uid_order_, [keep](uint32_t position) { return position >= keep; });
  }
  return tail;
}

void LogEntries::compactChunks(const Removal& plan) {
  size_t global = 0;
  std::vector<Chunk> kept_chunks;
  kept_chunks.reserve(chunks_.size());
  for (Chunk& chunk : chunks_) {
    const size_t first = global;
    const size_t count = chunk.size();
    global += count;
    size_t removed = 0;
    for (size_t k = 0; k < count; ++k) {
      removed += plan.new_position[first + k] == Removal::kRemoved ? 1 : 0;
    }
    if (removed == 0) {
      kept_chunks.push_back(std::move(chunk));
      continue;
    }
    chunk_bytes_ -= chunk.bytes();
    if (removed == count) {
      continue;
    }
    const bool was_sealed = chunk.sealed.has_value();
    std::vector<Row> rows;
    rowsOf(chunk, rows);
    std::vector<Row> survivors;
    survivors.reserve(count - removed);
    for (size_t k = 0; k < count; ++k) {
      if (plan.new_position[first + k] != Removal::kRemoved) {
        survivors.push_back(std::move(rows[k]));
      }
    }
    chunk.sealed.reset();
    chunk.rows = std::move(survivors);
    chunk_bytes_ += chunk.bytes();
    if (was_sealed) {
      seal(chunk);
    }
    kept_chunks.push_back(std::move(chunk));
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
  if (!uid_order_identity_) {
    size_t next = 0;
    for (const uint32_t position : uid_order_) {
      if (position >= count) {
        uid_order_[next++] = position - static_cast<uint32_t>(count);
      }
    }
    uid_order_.resize(next);
  }
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
  if (!uid_order_identity_) {
    size_t next = 0;
    for (const uint32_t position : uid_order_) {
      if (plan.new_position[position] != Removal::kRemoved) {
        uid_order_[next++] = plan.new_position[position];
      }
    }
    uid_order_.resize(next);
  }
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
  uid_order_identity_ = true;
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
  uid_order_.clear();
  uid_order_identity_ = true;
  changed(Change::kRemoval);
}

void LogEntries::rebuildUidOrder() {
  uid_order_.clear();
  uid_order_identity_ = std::ranges::is_sorted(uids_);
  if (!uid_order_identity_) {
    uid_order_.resize(size());
    std::iota(uid_order_.begin(), uid_order_.end(), 0u);
    std::stable_sort(
        uid_order_.begin(), uid_order_.end(), [this](uint32_t a, uint32_t b) { return uids_[a] < uids_[b]; });
  }
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
