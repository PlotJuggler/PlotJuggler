// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// LogStore, LogFilter and LogTableModel. The model runs over a small store so the eviction of the oldest rows is
// reachable; QAbstractItemModelTester checks every row signal against rowCount() and data(), its complaints arriving
// as warnings.

#include <gtest/gtest.h>

#include <QAbstractItemModelTester>
#include <string>
#include <vector>

#include "pj_widgets/LogModel.h"

namespace {

using PJ::LogLevel;
using PJ::TimeLabelFormat;
using PJ::log_view::CompiledFilter;
using PJ::log_view::FutureRows;
using PJ::log_view::LogFilter;
using PJ::log_view::LogRow;
using PJ::log_view::LogStore;
using PJ::log_view::LogTableModel;
using PJ::log_view::StoreChange;

LogRow row(double time, uint64_t seq, std::string message = {}, LogLevel level = LogLevel::kInfo) {
  LogRow out;
  out.time = time;
  out.seq = seq;
  out.level = level;
  out.message = std::move(message);
  return out;
}

std::vector<double> times(const LogStore& store) {
  std::vector<double> out;
  for (size_t i = 0; i < store.size(); ++i) {
    out.push_back(store.row(i).time);
  }
  return out;
}

size_t wireRows(const LogStore& store, uint32_t wire) {
  size_t count = 0;
  for (size_t i = 0; i < store.size(); ++i) {
    count += store.row(i).wire == wire ? 1 : 0;
  }
  return count;
}

// ---------------------------------------------------------------------------------------------- store

TEST(LogStore, EqualTimesOrderByWireThenArrival) {
  LogStore store;
  store.insert(1, {row(1.0, 0, "w1 a"), row(2.0, 1, "w1 b"), row(2.0, 2, "w1 c")});
  store.insert(0, {row(2.0, 0, "w0 a"), row(3.0, 1, "w0 b")});
  ASSERT_EQ(store.size(), 5u);
  const std::vector<std::string> expected = {"w1 a", "w0 a", "w1 b", "w1 c", "w0 b"};
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(store.row(i).message, expected[i]) << i;
  }
}

TEST(LogStore, RevealCountAtTheEdges) {
  LogStore store;
  std::vector<uint32_t> all;
  EXPECT_EQ(PJ::log_view::revealedCount(store, all, 10.0), 0u);
  store.insert(0, {row(1.0, 0), row(2.0, 1), row(2.0, 2), row(3.0, 3)});
  all = {0, 1, 2, 3};
  EXPECT_EQ(PJ::log_view::revealedCount(store, all, 0.5), 0u);
  EXPECT_EQ(PJ::log_view::revealedCount(store, all, 1.0), 1u);
  EXPECT_EQ(PJ::log_view::revealedCount(store, all, 2.0), 3u);
  EXPECT_EQ(PJ::log_view::revealedCount(store, all, 1e300), 4u);
  EXPECT_EQ(PJ::log_view::revealedCount(store, all, -1e300), 0u);
}

TEST(LogStore, RemoveWireKeepsTheOthers) {
  LogStore store;
  store.insert(0, {row(1.0, 0), row(3.0, 1)});
  store.insert(1, {row(2.0, 0), row(4.0, 1)});
  const auto change = store.removeWire(0);
  EXPECT_EQ(change.old_tail, 0u);
  EXPECT_EQ(times(store), (std::vector<double>{2.0, 4.0}));
  EXPECT_EQ(wireRows(store, 1), 2u);
}

TEST(LogStore, CapacityDropsTheOldest) {
  LogStore store(5);
  store.insert(0, {row(1.0, 0), row(2.0, 1), row(3.0, 2)});
  const auto change = store.insert(1, {row(4.0, 0), row(5.0, 1), row(6.0, 2), row(7.0, 3)});
  EXPECT_EQ(change.dropped_front, 2u);
  EXPECT_EQ(change.old_tail, 3u);
  EXPECT_EQ(times(store), (std::vector<double>{3.0, 4.0, 5.0, 6.0, 7.0}));
  EXPECT_EQ(wireRows(store, 0), 1u);
  EXPECT_EQ(wireRows(store, 1), 4u);
}

TEST(LogStore, RowKeySurvivesAReloadThatRenumbersTheWire) {
  LogStore store;
  store.insert(0, {row(1.0, 0, "w0 a"), row(2.0, 1, "w0 b"), row(2.0, 2, "w0 c")});
  store.insert(1, {row(2.0, 0, "w1 a")});
  const LogStore::RowKey key = store.keyOf(2);  // "w0 c", the second wire-0 row at 2.0
  ASSERT_EQ(store.row(2).message, "w0 c");
  // A late insert before it: the reload numbers every row anew.
  store.replaceWire(0, {row(0.5, 0, "w0 late"), row(1.0, 1, "w0 a"), row(2.0, 2, "w0 b"), row(2.0, 3, "w0 c")});
  const auto found = store.indexOf(key);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(store.row(*found).message, "w0 c");
  store.removeWire(0);
  EXPECT_FALSE(store.indexOf(key).has_value());
}

TEST(LogStore, KeptRowsMoveByTheEvictionAndOthersAreNotKept) {
  const StoreChange change{2, 6, false};  // rows 0..1 evicted, 2..5 unchanged, 6.. rebuilt
  EXPECT_FALSE(change.kept(1).has_value());
  EXPECT_EQ(change.kept(2), 0u);
  EXPECT_EQ(change.kept(5), 3u);
  EXPECT_FALSE(change.kept(6).has_value());
  EXPECT_FALSE((StoreChange{0, 6, true}.kept(3).has_value()));
}

TEST(LogStore, ChangesCompose) {
  LogStore store;
  store.insert(0, {row(1.0, 0), row(2.0, 1), row(3.0, 2)});
  auto change = StoreChange::none(store.size());
  change.then(store.insert(1, {row(2.5, 0)}));  // old_tail 2
  change.then(store.insert(0, {row(4.0, 3)}));  // appended: old_tail 4 in the middle state
  EXPECT_FALSE(change.reset);
  EXPECT_EQ(change.dropped_front, 0u);
  EXPECT_EQ(change.old_tail, 2u);
}

TEST(StripTerminalEscapes, KeepsTextAndDropsSequences) {
  const auto strip = [](std::string text) {
    PJ::log_view::stripTerminalEscapes(text);
    return text;
  };
  EXPECT_EQ(strip("plain text"), "plain text");
  EXPECT_EQ(strip("\x1b[1;31mred\x1b[0m and \x1b[38;5;208morange\x1b[m"), "red and orange");
  EXPECT_EQ(strip("a\x1b]0;title\ab"), "ab");
  EXPECT_EQ(strip("a\x1b]8;;http://x\x1b\\link\x1b]8;;\x1b\\b"), "alinkb");
  EXPECT_EQ(strip("keep\nlines\x1b[0m"), "keep\nlines");
  EXPECT_EQ(strip("truncated \x1b[31"), "truncated ");
  EXPECT_EQ(strip("trailing \x1b"), "trailing ");
  EXPECT_EQ(strip("\x1b(Bcharset"), "charset");
}

// --------------------------------------------------------------------------------------------- filter

class LogFilterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    add(0, 1.0, LogLevel::kDebug, "/a", "Starting motor");
    add(0, 2.0, LogLevel::kInfo, "/a", "motor ready");
    add(1, 3.0, LogLevel::kWarning, "/b", "Battery LOW");
    add(1, 4.0, LogLevel::kError, "/b", "motor fault");
    add(0, 5.0, LogLevel::kFatal, "/a", "abort");
    add(1, 6.0, LogLevel::kUnknown, "/b", "no level");
  }

  void add(
      uint32_t wire, double time, LogLevel level, const std::string& source, const std::string& message,
      const std::string& category = {}) {
    LogRow r = row(time, seq_++, message, level);
    r.source = store_.intern(source);
    r.category = store_.internCategory(category);
    store_.insert(wire, {r});
  }

  std::vector<double> pass(const LogFilter& filter) const {
    std::vector<uint32_t> indices;
    CompiledFilter(filter, store_).collect(0, store_.size(), indices);
    std::vector<double> out;
    for (uint32_t index : indices) {
      out.push_back(store_.row(index).time);
    }
    return out;
  }

  LogStore store_;
  uint64_t seq_ = 0;
};

TEST_F(LogFilterTest, LevelsSourceTextTopic) {
  EXPECT_EQ(pass(LogFilter{}).size(), 6u);

  LogFilter levels;
  levels.levels[static_cast<size_t>(LogLevel::kDebug)] = false;
  levels.levels[static_cast<size_t>(LogLevel::kInfo)] = false;
  EXPECT_EQ(pass(levels), (std::vector<double>{3.0, 4.0, 5.0, 6.0}));

  LogFilter by_source;
  by_source.source = "B";  // a case-insensitive substring
  EXPECT_EQ(pass(by_source), (std::vector<double>{3.0, 4.0, 6.0}));
  by_source.source = "/never-seen";
  EXPECT_TRUE(pass(by_source).empty());

  LogFilter text;
  text.text = "MOTOR";
  EXPECT_EQ(pass(text), (std::vector<double>{1.0, 2.0, 4.0}));

  LogFilter topic;
  topic.hidden_wires = {1};
  EXPECT_EQ(pass(topic), (std::vector<double>{1.0, 2.0, 5.0}));

  LogFilter combined;
  combined.levels[static_cast<size_t>(LogLevel::kError)] = false;
  combined.source = "/a";
  combined.text = "motor";
  EXPECT_EQ(pass(combined), (std::vector<double>{1.0, 2.0}));
}

TEST_F(LogFilterTest, Category) {
  add(0, 7.0, LogLevel::kInfo, "/a", "joint limit", "motion");
  add(1, 8.0, LogLevel::kInfo, "/b", "link up", "net");
  EXPECT_EQ(store_.categories().size(), 2u);  // the empty category of the others is not one

  LogFilter category;
  category.category = "MOT";
  EXPECT_EQ(pass(category), (std::vector<double>{7.0}));
  category.source = "/b";
  EXPECT_TRUE(pass(category).empty());
}

TEST_F(LogFilterTest, UnknownLevelIsAlwaysShown) {
  LogFilter none;
  none.levels.fill(false);
  EXPECT_EQ(pass(none), (std::vector<double>{6.0}));
}

TEST(LogFilterText, CaseInsensitiveSubstring) {
  EXPECT_TRUE(PJ::log_view::containsCaseInsensitive("Hello World", "o w"));
  EXPECT_TRUE(PJ::log_view::containsCaseInsensitive("anything", ""));
  EXPECT_FALSE(PJ::log_view::containsCaseInsensitive("abc", "abcd"));
}

// ---------------------------------------------------------------------------------------------- model

std::vector<std::string> g_warnings;

void recordWarnings(QtMsgType type, const QMessageLogContext& /*context*/, const QString& message) {
  if (type != QtDebugMsg && type != QtInfoMsg) {
    g_warnings.push_back(message.toStdString());
  }
}

/// Rows at times first, first + 1, ...; odd times are Debug, so a filter without Debug keeps every other one.
std::vector<LogRow> rows(int first, int count, uint64_t& seq) {
  std::vector<LogRow> out;
  for (int t = first; t < first + count; ++t) {
    out.push_back(row(t, seq++, "t=" + std::to_string(t), t % 2 != 0 ? LogLevel::kDebug : LogLevel::kInfo));
  }
  return out;
}

QString message(const LogTableModel& model, int row) {
  return model.data(model.index(row, LogTableModel::kMessage)).toString();
}

class LogTableModelEviction : public ::testing::TestWithParam<FutureRows> {
 protected:
  void SetUp() override {
    g_warnings.clear();
    previous_ = qInstallMessageHandler(recordWarnings);
    model_.setFutureRows(GetParam());
  }
  void TearDown() override {
    qInstallMessageHandler(previous_);
    for (const std::string& warning : g_warnings) {
      ADD_FAILURE() << warning;
    }
  }

  void insert(int first, int count) {
    model_.applyStoreChange(store_.insert(0, rows(first, count, seq_)));
    model_.setTracker(tracker_);
  }

  LogStore store_{5};
  LogTableModel model_{store_, nullptr, nullptr};
  QAbstractItemModelTester tester_{&model_, QAbstractItemModelTester::FailureReportingMode::Warning};
  QtMessageHandler previous_ = nullptr;
  uint64_t seq_ = 0;
  double tracker_ = 5.0;
};

TEST_P(LogTableModelEviction, EveryRowFollowsTheStore) {
  insert(1, 5);  // t=1..5, all revealed
  insert(6, 3);  // evicts t=1..3; t=6..8 are after the tracker
  ASSERT_EQ(store_.size(), 5u);
  EXPECT_EQ(model_.revealedRows(), 2);  // t=4, t=5
  EXPECT_EQ(model_.rowCount(), GetParam() == FutureRows::kGrey ? 5 : 2);
  for (int r = 0; r < model_.rowCount(); ++r) {
    const auto index = static_cast<size_t>(r);  // nothing filtered: model rows are store rows
    EXPECT_EQ(message(model_, r).toStdString(), store_.row(index).message) << r;
    EXPECT_EQ(model_.storeIndex(r), index) << r;
    EXPECT_EQ(model_.rowOf(index), r) << r;
  }
  EXPECT_EQ(message(model_, 0), "t=4");
}

TEST_P(LogTableModelEviction, FilteredRowsShiftByTheEvictedStoreRows) {
  LogFilter no_debug;
  no_debug.levels[static_cast<size_t>(LogLevel::kDebug)] = false;
  model_.setFilter(no_debug);
  insert(1, 5);  // shown: t=2, 4
  tracker_ = 7.0;
  insert(6, 4);  // evicts t=1..4; store t=5..9, shown t=6, 8 (t=8 after the tracker)
  EXPECT_EQ(model_.revealedRows(), 1);
  EXPECT_EQ(model_.rowCount(), GetParam() == FutureRows::kGrey ? 2 : 1);
  EXPECT_EQ(message(model_, 0), "t=6");
  EXPECT_EQ(model_.storeIndex(0), 1u);  // t=5 is store row 0, and hidden
  EXPECT_EQ(model_.rowOf(0), -1);
}

INSTANTIATE_TEST_SUITE_P(
    FutureRowModes, LogTableModelEviction, ::testing::Values(FutureRows::kGrey, FutureRows::kHide),
    [](const ::testing::TestParamInfo<FutureRows>& param_info) {
      return param_info.param == FutureRows::kGrey ? std::string("Grey") : std::string("Hide");
    });

// The Time column prints what formatTimeLabel prints for the host's format: the playback readout's text.
TEST(LogTableModelTime, TimeColumnFollowsTheHostFormat) {
  LogStore store;
  uint64_t seq = 0;
  LogTableModel model(store, nullptr, nullptr);
  std::vector<LogRow> list;
  list.push_back(row(1779215727.239, seq++, "instant"));
  list.push_back(row(1779215730.5, seq++, "later"));
  model.applyStoreChange(store.insert(0, std::move(list)));
  model.setTracker(1e300);
  const auto time_text = [&model]() { return model.data(model.index(0, LogTableModel::kTime)).toString(); };
  EXPECT_EQ(time_text(), "1779215727.239");

  model.setTimeLabelFormat(TimeLabelFormat::kUtcClock, 3);
  EXPECT_EQ(time_text(), PJ::formatTimeLabel(1779215727.239, TimeLabelFormat::kUtcClock, 3, true));
  EXPECT_TRUE(time_text().endsWith(":27.239"));
  model.setTimeLabelFormat(TimeLabelFormat::kLocalClock, 1);
  EXPECT_EQ(time_text(), PJ::formatTimeLabel(1779215727.239, TimeLabelFormat::kLocalClock, 1, true));
}

}  // namespace
