#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The per-topic read seam of the Log panel: the panel indexes a topic's rows and reads them through a LogRowSource, so
// it never holds a copy of the log.

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

/// One log entry as a host hands it to LogView.
struct LogEntry {
  double time = 0.0;     ///< display-axis seconds: the Time column, the merge order and the tracker compare on it
  int64_t stamp_ns = 0;  ///< the entry's own stamp, shown in the detail pane only
  LogLevel level = LogLevel::kUnknown;
  std::string source;
  std::string category;  ///< empty: none
  std::string message;
};

/// One topic's rows, in ascending time. A row has a key that never changes while the row exists (its index does: a late
/// row moves the rows after it); a row added later has a key above every earlier one. Rows leave only from the front
/// (the oldest) unless the host says otherwise (a replace). Reads may come from any call of the panel on the GUI
/// thread.
class LogRowSource {
 public:
  using MessageSink = std::function<void(size_t index, std::string_view message)>;

  virtual ~LogRowSource() = default;

  [[nodiscard]] virtual size_t size() const = 0;
  /// Never 0 (the "no row yet" cursor).
  [[nodiscard]] virtual uint64_t key(size_t index) const = 0;
  /// Index of the row with @p key, nullopt when gone. @p hint is tried first (a sequential reader passes the index
  /// after the last one found).
  [[nodiscard]] virtual std::optional<size_t> find(uint64_t key, size_t hint) const = 0;
  /// Indices of the rows whose key is above @p after, ascending.
  [[nodiscard]] virtual std::vector<size_t> indicesAfter(uint64_t after) const = 0;
  /// Display-axis seconds.
  [[nodiscard]] virtual double time(size_t index) const = 0;
  [[nodiscard]] virtual LogLevel level(size_t index) const = 0;
  /// Ids into dictionaryText(); 0 is the empty string.
  [[nodiscard]] virtual uint32_t sourceId(size_t index) const = 0;
  [[nodiscard]] virtual uint32_t categoryId(size_t index) const = 0;
  [[nodiscard]] virtual int64_t ownStamp(size_t index) const = 0;
  [[nodiscard]] virtual std::string dictionaryText(uint32_t id) const = 0;
  /// @p sink gets each row of [first, first + count), in order; the view is valid during the call only.
  virtual void messages(size_t first, size_t count, const MessageSink& sink) const = 0;
  /// Runs @p body with every read of this source seeing one state (a host may hold a lock for it). @p body reads this
  /// source only: reading another one inside it may deadlock against the host's writers.
  virtual void batch(const std::function<void()>& body) const {
    body();
  }
};

/// A LogRowSource over rows it owns, for hosts that hand LogEntry lists (LogView::appendEntries) and for tests.
class VectorLogRowSource final : public LogRowSource {
 public:
  /// Adds @p entries (ascending time; non-finite times are skipped), each after this source's rows of equal time.
  void merge(std::vector<LogEntry> entries);
  /// Evicts the oldest @p count rows.
  void dropFront(size_t count);

  [[nodiscard]] size_t size() const override {
    return rows_.size();
  }
  [[nodiscard]] uint64_t key(size_t index) const override {
    return rows_[index].key;
  }
  // ponytail: a miss of the hint and indicesAfter() scan every row; fine for test-sized lists.
  [[nodiscard]] std::optional<size_t> find(uint64_t key, size_t hint) const override;
  [[nodiscard]] std::vector<size_t> indicesAfter(uint64_t after) const override;
  [[nodiscard]] double time(size_t index) const override {
    return rows_[index].time;
  }
  [[nodiscard]] LogLevel level(size_t index) const override {
    return rows_[index].level;
  }
  [[nodiscard]] uint32_t sourceId(size_t index) const override {
    return rows_[index].source;
  }
  [[nodiscard]] uint32_t categoryId(size_t index) const override {
    return rows_[index].category;
  }
  [[nodiscard]] int64_t ownStamp(size_t index) const override {
    return rows_[index].stamp_ns;
  }
  [[nodiscard]] std::string dictionaryText(uint32_t id) const override {
    return dictionary_[id];
  }
  void messages(size_t first, size_t count, const MessageSink& sink) const override;

 private:
  struct Row {
    double time = 0.0;
    uint64_t key = 0;
    int64_t stamp_ns = 0;
    LogLevel level = LogLevel::kUnknown;
    uint32_t source = 0;
    uint32_t category = 0;
    std::string message;
  };
  uint32_t intern(std::string text);

  std::deque<Row> rows_;
  uint64_t next_key_ = 1;
  std::vector<std::string> dictionary_{std::string()};
  std::unordered_map<std::string, uint32_t> dictionary_ids_;
};

}  // namespace PJ
