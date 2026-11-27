#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The Qt-free data side of LogView: the merged row index and the row filter.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pj_log/LogRowSource.h"

namespace PJ {
namespace log_view {

/// One row of the merged index: where it sorts and which topic row it is.
struct RowRef {
  double time = 0.0;  ///< display-axis seconds, copied from the source so ordering never reads it
  uint32_t wire = 0;  ///< topic id; orders rows of equal time
  /// The row's absolute number in its source. ponytail: 32 bits, a topic past 2^32 rows in one session wraps.
  uint32_t row = 0;
};

/// A row read from its source. `present` is false when the source no longer has it (evicted, or replaced and not
/// re-indexed yet): every other field but `time` and `wire` is then empty.
struct LogRow {
  double time = 0.0;
  int64_t stamp_ns = 0;  ///< the entry's own stamp, shown as metadata only
  uint32_t wire = 0;
  uint32_t source = 0;    ///< id into LogStore::text()
  uint32_t category = 0;  ///< id into LogStore::text(); 0 = none
  LogLevel level = LogLevel::kUnknown;
  bool present = false;
  std::string message;  ///< terminal escapes stripped
};

/// What a mutation did to the row sequence, so a view can update without starting over.
///
/// In old coordinates: rows [0, dropped_front) were evicted, rows [dropped_front, old_tail) are unchanged,
/// rows [old_tail, old size) were removed. In new coordinates the unchanged rows are
/// [0, old_tail - dropped_front) (clamped at 0) and everything after them is new.
struct StoreChange {
  size_t dropped_front = 0;
  size_t old_tail = 0;
  bool reset = false;  ///< nothing is preserved; ignore the two fields above

  /// A change that touches nothing on a store of @p size rows.
  static StoreChange none(size_t size) {
    return StoreChange{0, size, false};
  }
  /// The change of applying @p next after this one, both expressed against the state before this one.
  void then(const StoreChange& next);
  /// New index of the row at @p old_index, when the change left it in place; nullopt when it was evicted,
  /// removed or rebuilt.
  [[nodiscard]] std::optional<size_t> kept(size_t old_index) const {
    if (reset || old_index < dropped_front || old_index >= old_tail) {
      return std::nullopt;
    }
    return old_index - dropped_front;
  }
};

/// Merged index of every topic's rows, ordered by (time, wire, row); the rows stay in their LogRowSource and are read
/// on demand (a source caches what it decodes). Each topic's index covers a contiguous range of its source's absolute
/// rows. GUI thread only.
class LogStore {
 public:
  LogStore();

  /// Id of @p text in the panel's string pool, adding it when new. Id 0 is the empty string.
  uint32_t intern(std::string_view text);
  [[nodiscard]] const std::string& text(uint32_t id) const {
    return strings_[id];
  }
  /// Whether a topic indexed a row with a source, a category.
  [[nodiscard]] bool hasSources() const;
  [[nodiscard]] bool hasCategories() const;

  /// Indexes @p source as topic @p wire: drops the rows it evicted and the rows numbered from @p from_row on (they
  /// changed), then adds its rows [from_row, to_row) (clamped to what it holds). @p source replaces the one held.
  StoreChange insert(uint32_t wire, std::shared_ptr<const LogRowSource> source, uint64_t from_row, uint64_t to_row);
  /// Drops the rows of @p wire numbered below @p evicted.
  StoreChange dropFront(uint32_t wire, uint64_t evicted);
  /// Drop every row of @p wire.
  StoreChange removeWire(uint32_t wire);
  /// Drop the rows of @p wire and index all of @p source in their place.
  StoreChange replaceWire(uint32_t wire, std::shared_ptr<const LogRowSource> source);

  [[nodiscard]] size_t size() const {
    return refs_.size();
  }
  [[nodiscard]] const RowRef& ref(size_t index) const {
    return refs_[index];
  }
  /// Reads row @p index from its source; without @p with_message the message stays empty.
  [[nodiscard]] LogRow row(size_t index, bool with_message = true) const;
  /// The source of @p wire, or null.
  [[nodiscard]] const LogRowSource* source(uint32_t wire) const;
  /// Absolute number past the last row of @p wire indexed (0 for an unknown wire).
  [[nodiscard]] uint64_t wireEnd(uint32_t wire) const {
    const auto found = wires_.find(wire);
    return found != wires_.end() ? found->second.end : 0;
  }
  /// Panel string id of @p wire's dictionary id @p id (0 when the topic never indexed it).
  [[nodiscard]] uint32_t panelId(uint32_t wire, uint32_t id) const;

  /// Identity of a row that survives a reload of its wire: row numbers restart on a reload.
  struct RowKey {
    double time = 0.0;
    uint32_t wire = 0;
    size_t rank = 0;  ///< among the rows of the wire with this time
  };
  [[nodiscard]] RowKey keyOf(size_t index) const;
  /// Index of the row with @p key, if the store holds it.
  [[nodiscard]] std::optional<size_t> indexOf(const RowKey& key) const;

 private:
  struct Wire {
    std::shared_ptr<const LogRowSource> source;
    /// Source dictionary id -> panel string id; kUnmapped where never seen.
    std::vector<uint32_t> ids;
    uint64_t begin = 0;  ///< the rows indexed are [begin, end)
    uint64_t end = 0;
    bool any_source = false;
    bool any_category = false;
  };
  /// Drops the rows of @p wire outside [evicted, cut), then merges @p fresh (the wire's rows from the new end, in
  /// order).
  StoreChange apply(uint32_t wire, Wire& state, uint64_t evicted, uint64_t cut, std::vector<RowRef> fresh);
  void mapIds(Wire& state, const LogRowSource& source, size_t index);

  std::deque<RowRef> refs_;
  std::map<uint32_t, Wire> wires_;

  std::vector<std::string> strings_;
  /// Hashes std::string and std::string_view alike, so a lookup does not build a std::string.
  struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view text) const noexcept {
      return std::hash<std::string_view>{}(text);
    }
  };
  std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>> string_ids_;
};

/// The spellings of a level: `key` in a saved layout, `label` on screen.
struct LevelName {
  LogLevel level;
  std::string_view key;
  std::string_view label;
};

/// Every level: the ones the user can toggle in toolbar order, then LogLevel::kUnknown, which has no toggle (it is
/// always shown) and no persisted key. The keys are a layout-file contract.
inline constexpr std::array<LevelName, 6> kLevelNames = {{
    {LogLevel::kDebug, "debug", "Debug"},
    {LogLevel::kInfo, "info", "Info"},
    {LogLevel::kWarning, "warn", "Warn"},
    {LogLevel::kError, "error", "Error"},
    {LogLevel::kFatal, "fatal", "Fatal"},
    {LogLevel::kUnknown, "", "Unknown"},
}};

/// Levels the user can toggle, in toolbar order: kLevelNames without kUnknown.
inline constexpr size_t kToggleLevelCount = kLevelNames.size() - 1;

struct LogFilter {
  /// Indexed by the LogLevel value; kUnknown is ignored (always shown).
  std::array<bool, 6> levels = {true, true, true, true, true, true};
  /// Case-insensitive (ASCII) substrings of the source, category and message; empty matches everything.
  std::string source;
  std::string category;
  std::string text;
  /// Wire ids whose rows are hidden.
  std::vector<uint32_t> hidden_wires;
};

/// A LogFilter resolved against one store: the source and category needles are evaluated once per panel string, and
/// the messages are read in batches, one topic at a time.
class CompiledFilter {
 public:
  CompiledFilter(const LogFilter& filter, const LogStore& store);

  /// Append to @p out the indices of the rows in [@p from, @p to) that pass.
  void collect(size_t from, size_t to, std::vector<uint32_t>& out) const {
    (void)collect(from, to, to - from, out);
  }
  /// As collect(), over at most @p budget_rows rows (at least one); returns the index it stopped at.
  size_t collect(size_t from, size_t to, size_t budget_rows, std::vector<uint32_t>& out) const;

 private:
  [[nodiscard]] bool idPasses(std::vector<char>& passes, uint32_t id, const std::string& needle) const;

  const LogStore& store_;
  std::array<bool, 6> levels_;
  std::string source_needle_;    ///< lower-cased
  std::string category_needle_;  ///< lower-cased
  std::string needle_;           ///< lower-cased
  /// By panel string id, filled on first use: -1 not evaluated yet.
  mutable std::vector<char> source_passes_;
  mutable std::vector<char> category_passes_;
  std::vector<uint32_t> hidden_wires_;
};

/// Whether @p haystack contains @p lower_needle, comparing ASCII letters case-insensitively.
bool containsCaseInsensitive(std::string_view haystack, std::string_view lower_needle);

/// Number of entries of @p filtered (store indices in ascending order) whose row time is <= @p tracker.
size_t revealedCount(const LogStore& store, const std::vector<uint32_t>& filtered, double tracker);

/// Removes terminal escape sequences (ANSI colours and the like) from @p text, in place: some ROS nodes colour their
/// messages and the codes travel inside the log text.
void stripTerminalEscapes(std::string& text);

}  // namespace log_view
}  // namespace PJ
