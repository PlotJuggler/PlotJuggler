#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_base/builtin/log.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/ordered_entries.hpp"
#include "pj_datastore/sequential_uid.hpp"

namespace PJ {

/// True for an object topic whose entries are canonical `sdk::Log` bytes: `builtin_object_type` is "kLog" and no
/// `payload_encoding` names a wire format. Such a topic is stored as a LogEntries series.
[[nodiscard]] bool isCanonicalLogMetadata(std::string_view metadata_json);

/// One decoded row: the store stamp, its UID and the log.
struct LogRecord {
  Timestamp timestamp = 0;
  SequentialUID uid;
  sdk::Log log;
};

/// The storage of a kLog series: rows decoded once into columns, held compactly. Shares OrderedEntries' contract for
/// ordering (ascending store stamp, equal stamps in arrival order), UID cursors and ContentIdentity revisions, so the
/// ObjectStore serves both alike.
///
/// Layout: the stamps, UIDs and UID order are whole-series vectors (`timestamps()` must stay a stable vector for
/// EntryTimestampsView). The rest lives in chunks of up to about kChunkRows rows: the last chunk is open (plain rows),
/// the others are sealed (narrowed source/category ids, constant-or-raw level and own-stamp columns, the messages as
/// one zstd frame). A change inside a sealed chunk unseals and reseals that chunk only. No locking: the caller holds
/// the series lock.
class LogEntries {
 public:
  using ContentIdentity = OrderedEntries::ContentIdentity;
  using PushOrder = OrderedEntries::PushOrder;
  using Removal = OrderedEntries::Removal;
  using TimeRangeEntry = OrderedEntries::TimeRangeEntry;
  using MessageSink = std::function<void(size_t index, std::string_view message)>;

  static constexpr size_t kChunkRows = 4096;

  LogEntries();
  LogEntries(LogEntries&&) noexcept;
  LogEntries& operator=(LogEntries&&) noexcept;
  LogEntries(const LogEntries&) = delete;
  LogEntries& operator=(const LogEntries&) = delete;
  ~LogEntries();

  [[nodiscard]] ContentIdentity contentIdentity() const noexcept {
    return identity_;
  }
  [[nodiscard]] bool empty() const noexcept {
    return timestamps_.empty();
  }
  [[nodiscard]] size_t size() const noexcept {
    return timestamps_.size();
  }
  /// Rows ever evicted from the front: the absolute number of the row at index i is evicted() + i. Never decreases.
  [[nodiscard]] uint64_t evicted() const noexcept {
    return evicted_;
  }

  [[nodiscard]] Timestamp frontTimestamp() const {
    return timestamps_.front();
  }
  [[nodiscard]] Timestamp backTimestamp() const {
    return timestamps_.back();
  }
  [[nodiscard]] const std::vector<Timestamp>& timestamps() const noexcept {
    return timestamps_;
  }
  [[nodiscard]] SequentialUID uidAt(size_t index) const {
    return uids_[index];
  }

  // --- Lookups, as OrderedEntries' ---

  [[nodiscard]] std::optional<size_t> indexAtOrBefore(Timestamp t) const;
  [[nodiscard]] std::vector<TimeRangeEntry> rangeByTime(Timestamp lo, Timestamp hi) const;
  [[nodiscard]] std::optional<size_t> indexOfUid(SequentialUID uid) const;
  [[nodiscard]] SequentialUID firstUid() const;
  [[nodiscard]] SequentialUID nextUidAfter(SequentialUID after) const;
  [[nodiscard]] SequentialUID maxUidAtOrBefore(Timestamp t) const;
  [[nodiscard]] std::optional<Timestamp> oldestTimestampAfterUid(SequentialUID after) const;

  // --- Columns ---

  [[nodiscard]] sdk::Log::Level level(size_t index) const;
  /// Ids into dictionaryText(); 0 is the empty string.
  [[nodiscard]] uint32_t sourceId(size_t index) const;
  [[nodiscard]] uint32_t categoryId(size_t index) const;
  [[nodiscard]] std::string_view dictionaryText(uint32_t id) const {
    return dictionary_[id];
  }
  [[nodiscard]] size_t dictionarySize() const noexcept {
    return dictionary_.size();
  }
  /// The log's own stamp on the store clock (it moves with shift()); 0 when the log had none.
  [[nodiscard]] Timestamp ownStamp(size_t index) const;
  /// Calls @p sink for each row in [first, first + count), in order; a sealed chunk is decompressed once per call. The
  /// view is valid only during the call.
  void messages(size_t first, size_t count, const MessageSink& sink) const;

  /// Decompresses the row's chunk: for many rows, use records().
  [[nodiscard]] LogRecord recordAt(size_t index) const;
  void records(size_t first, size_t count, const std::function<void(size_t, LogRecord&&)>& sink) const;
  /// serializeBuiltinObject() of the row's log.
  [[nodiscard]] std::vector<uint8_t> canonicalBytesAt(size_t index) const;

  /// Bytes held: columns, chunks and the dictionary.
  [[nodiscard]] size_t memoryBytes() const noexcept;

  // --- Mutation ---

  /// @p uid is the series maximum (the store mints it right before), as for OrderedEntries::push().
  PushOrder push(Timestamp timestamp, SequentialUID uid, const sdk::Log& log);

 private:
  struct Row {
    uint8_t level = 0;
    uint32_t source = 0;
    uint32_t category = 0;
    /// Own stamp minus store stamp, or kNoOwnStamp.
    int64_t stamp_offset = 0;
    std::string message;
  };
  struct Chunk;
  /// "No own stamp": an own stamp exactly this far from the store stamp reads as none.
  static constexpr int64_t kNoOwnStamp = std::numeric_limits<int64_t>::min();

  enum class Change { kFollowable, kInsert, kRemoval };
  void changed(Change change) noexcept;

  uint32_t intern(std::string_view text);
  [[nodiscard]] Row toRow(Timestamp timestamp, const sdk::Log& log);
  [[nodiscard]] sdk::Log toLog(Timestamp timestamp, const Row& row) const;
  /// (chunk, offset in it) of row @p index.
  [[nodiscard]] std::pair<size_t, size_t> locate(size_t index) const;
  void appendRow(Row&& row);
  void insertRow(size_t index, Row&& row);
  void rebuildChunkStarts();
  void seal(Chunk& chunk);
  void unseal(Chunk& chunk);
  void rowsOf(const Chunk& chunk, std::vector<Row>& out) const;

  std::vector<Timestamp> timestamps_;
  std::vector<SequentialUID> uids_;
  std::vector<uint32_t> uid_order_;
  std::vector<Chunk> chunks_;
  std::vector<size_t> chunk_starts_;
  /// Shared by sources and categories; id 0 is "".
  std::vector<std::string> dictionary_;
  std::unordered_map<std::string, uint32_t> dictionary_ids_;
  size_t dictionary_bytes_ = 0;
  size_t chunk_bytes_ = 0;
  uint64_t evicted_ = 0;
  ContentIdentity identity_;
};

}  // namespace PJ
