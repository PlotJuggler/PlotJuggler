#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The Qt-free data side of LogView: the merged row store and the row filter.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace PJ {

/// Severity of a log entry. Values and order match the SDK's canonical `sdk::Log::Level`, so a host converts with a
/// cast.
enum class LogLevel : std::uint8_t { kUnknown = 0, kDebug = 1, kInfo = 2, kWarning = 3, kError = 4, kFatal = 5 };

namespace log_view {

/// One log entry. Strings that repeat (source, category) are interned in the store.
struct LogRow {
  double time = 0.0;      ///< display-axis seconds of the sample
  int64_t stamp_ns = 0;   ///< the entry's own stamp, shown as metadata only
  uint64_t seq = 0;       ///< arrival order within the wire
  uint32_t wire = 0;      ///< topic id; orders rows of equal time
  uint32_t source = 0;    ///< interned
  uint32_t category = 0;  ///< interned; 0 = none
  LogLevel level = LogLevel::kUnknown;
  std::string message;
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

/// Merged log of every topic, ordered by (time, wire, seq). Evicts the oldest rows beyond its capacity.
class LogStore {
 public:
  static constexpr size_t kDefaultCapacity = 1'000'000;

  explicit LogStore(size_t capacity = kDefaultCapacity);

  /// Id of @p text in the string pool, adding it when new. Id 0 is the empty string.
  uint32_t intern(std::string_view text);
  [[nodiscard]] const std::string& text(uint32_t id) const {
    return strings_[id];
  }
  /// Whether a row the store holds has a source, a category.
  [[nodiscard]] bool hasSources() const {
    return rows_with_source_ > 0;
  }
  [[nodiscard]] bool hasCategories() const {
    return rows_with_category_ > 0;
  }

  /// Add the rows of @p wire, in ascending (time, seq) order; they merge with the rows already held, earlier ones
  /// included. @p seq must keep growing per wire: it orders the wire's rows of equal time.
  StoreChange insert(uint32_t wire, std::vector<LogRow> rows);
  /// Drop every row of @p wire.
  StoreChange removeWire(uint32_t wire);
  /// Drop the rows of @p wire and insert @p rows in their place.
  StoreChange replaceWire(uint32_t wire, std::vector<LogRow> rows);

  [[nodiscard]] size_t size() const {
    return rows_.size();
  }
  [[nodiscard]] const LogRow& row(size_t index) const {
    return rows_[index];
  }

  /// Identity of a row that survives a reload of its wire: arrival numbers (seq) restart on a reload.
  struct RowKey {
    double time = 0.0;
    uint32_t wire = 0;
    size_t rank = 0;  ///< among the rows of the wire with this time
  };
  [[nodiscard]] RowKey keyOf(size_t index) const;
  /// Index of the row with @p key, if the store holds it.
  [[nodiscard]] std::optional<size_t> indexOf(const RowKey& key) const;

 private:
  void evictOverflow(StoreChange& change);
  /// Counts @p row in (or, with @p sign -1, out of) the rows with a source and a category.
  void tally(const LogRow& row, int sign);

  size_t capacity_;
  std::deque<LogRow> rows_;

  std::vector<std::string> strings_;
  /// Hashes std::string and std::string_view alike, so a lookup does not build a std::string.
  struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view text) const noexcept {
      return std::hash<std::string_view>{}(text);
    }
  };
  std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>> string_ids_;
  size_t rows_with_source_ = 0;
  size_t rows_with_category_ = 0;
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

/// A LogFilter resolved against one store, cheap to evaluate per row.
class CompiledFilter {
 public:
  CompiledFilter(const LogFilter& filter, const LogStore& store);

  [[nodiscard]] bool accepts(const LogRow& row) const;

  /// Append to @p out the indices of the rows in [@p from, @p to) that pass.
  void collect(size_t from, size_t to, std::vector<uint32_t>& out) const;

 private:
  const LogStore& store_;
  std::array<bool, 6> levels_;
  std::string source_needle_;    ///< lower-cased
  std::string category_needle_;  ///< lower-cased
  std::string needle_;           ///< lower-cased
  std::vector<uint32_t> hidden_wires_;
};

/// Whether @p haystack contains @p lower_needle, comparing ASCII letters case-insensitively.
bool containsCaseInsensitive(const std::string& haystack, const std::string& lower_needle);

/// Number of entries of @p filtered (store indices in ascending order) whose row time is <= @p tracker.
size_t revealedCount(const LogStore& store, const std::vector<uint32_t>& filtered, double tracker);

/// Removes terminal escape sequences (ANSI colours and the like) from @p text, in place: some ROS nodes colour their
/// messages and the codes travel inside the log text.
void stripTerminalEscapes(std::string& text);

}  // namespace log_view
}  // namespace PJ
