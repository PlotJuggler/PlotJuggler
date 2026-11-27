// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// LogStore, LogFilter and LogTableModel over VectorLogRowSource topics. The model tests evict a topic's oldest rows
// under it; QAbstractItemModelTester checks every row signal against rowCount() and data(), its complaints arriving as
// warnings.

#include <gtest/gtest.h>

#include <QAbstractItemModelTester>
#include <QColor>
#include <QCoreApplication>
#include <QPalette>
#include <QWidget>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pj_log/LogRowSource.h"
#include "pj_log/LogTableModel.h"

namespace {

using PJ::LogEntry;
using PJ::LogLevel;
using PJ::TimeLabelFormat;
using PJ::VectorLogRowSource;
using PJ::log_view::CompiledFilter;
using PJ::log_view::FutureRows;
using PJ::log_view::LogFilter;
using PJ::log_view::LogStore;
using PJ::log_view::LogTableModel;
using PJ::log_view::StoreChange;

LogEntry entry(
    double time, std::string message = {}, LogLevel level = LogLevel::kInfo, std::string source = {},
    std::string category = {}) {
  return LogEntry{
      .time = time,
      .stamp_ns = 0,
      .level = level,
      .source = std::move(source),
      .category = std::move(category),
      .message = std::move(message)};
}

/// The topics of one store, each a VectorLogRowSource fed as a host feeds LogView.
struct Feed {
  explicit Feed(LogStore& target) : store(target) {}

  LogStore& store;
  std::map<uint32_t, std::shared_ptr<VectorLogRowSource>> sources;

  std::shared_ptr<VectorLogRowSource>& source(uint32_t wire) {
    auto& slot = sources[wire];
    if (!slot) {
      slot = std::make_shared<VectorLogRowSource>();
    }
    return slot;
  }
  StoreChange add(uint32_t wire, std::vector<LogEntry> entries) {
    auto& src = source(wire);
    const uint64_t from = src->merge(std::move(entries));
    return store.insert(wire, src, from, src->evicted() + src->size());
  }
  StoreChange evict(uint32_t wire, size_t count) {
    auto& src = source(wire);
    src->dropFront(count);
    return store.dropFront(wire, src->evicted());
  }
  StoreChange replace(uint32_t wire, std::vector<LogEntry> entries) {
    sources[wire] = std::make_shared<VectorLogRowSource>();
    sources[wire]->merge(std::move(entries));
    return store.replaceWire(wire, sources[wire]);
  }
  StoreChange remove(uint32_t wire) {
    sources.erase(wire);
    return store.removeWire(wire);
  }
};

std::vector<double> times(const LogStore& store) {
  std::vector<double> out;
  for (size_t i = 0; i < store.size(); ++i) {
    out.push_back(store.ref(i).time);
  }
  return out;
}

size_t wireRows(const LogStore& store, uint32_t wire) {
  size_t count = 0;
  for (size_t i = 0; i < store.size(); ++i) {
    count += store.ref(i).wire == wire ? 1 : 0;
  }
  return count;
}

// ---------------------------------------------------------------------------------------------- store

TEST(LogStore, EqualTimesOrderByWireThenArrival) {
  LogStore store;
  Feed feed{store};
  feed.add(1, {entry(1.0, "w1 a"), entry(2.0, "w1 b"), entry(2.0, "w1 c")});
  feed.add(0, {entry(2.0, "w0 a"), entry(3.0, "w0 b")});
  ASSERT_EQ(store.size(), 5u);
  const std::vector<std::string> expected = {"w1 a", "w0 a", "w1 b", "w1 c", "w0 b"};
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(store.row(i).message, expected[i]) << i;
  }
}

TEST(LogStore, RevealCountAtTheEdges) {
  LogStore store;
  Feed feed{store};
  std::vector<uint32_t> all;
  EXPECT_EQ(PJ::log_view::revealedCount(store, all, 10.0), 0u);
  feed.add(0, {entry(1.0), entry(2.0), entry(2.0), entry(3.0)});
  all = {0, 1, 2, 3};
  EXPECT_EQ(PJ::log_view::revealedCount(store, all, 0.5), 0u);
  EXPECT_EQ(PJ::log_view::revealedCount(store, all, 1.0), 1u);
  EXPECT_EQ(PJ::log_view::revealedCount(store, all, 2.0), 3u);
  EXPECT_EQ(PJ::log_view::revealedCount(store, all, 1e300), 4u);
  EXPECT_EQ(PJ::log_view::revealedCount(store, all, -1e300), 0u);
}

TEST(LogStore, RemoveWireKeepsTheOthers) {
  LogStore store;
  Feed feed{store};
  feed.add(0, {entry(1.0), entry(3.0)});
  feed.add(1, {entry(2.0), entry(4.0)});
  const auto change = feed.remove(0);
  EXPECT_EQ(change.old_tail, 0u);
  EXPECT_EQ(times(store), (std::vector<double>{2.0, 4.0}));
  EXPECT_EQ(wireRows(store, 1), 2u);
}

TEST(LogStore, ALateRowLeavesWithItsWire) {
  LogStore store;
  Feed feed{store};
  feed.add(0, {entry(5.0), entry(6.0)});
  feed.add(1, {entry(3.0)});
  feed.add(0, {entry(1.0)});  // earlier than every row the wire holds
  EXPECT_EQ(times(store), (std::vector<double>{1.0, 3.0, 5.0, 6.0}));
  const auto change = feed.replace(0, {entry(7.0)});
  EXPECT_EQ(change.old_tail, 0u);
  EXPECT_EQ(times(store), (std::vector<double>{3.0, 7.0}));
  feed.add(0, {entry(2.0)});
  feed.remove(0);
  EXPECT_EQ(times(store), std::vector<double>{3.0});
  EXPECT_EQ(wireRows(store, 0), 0u);
}

TEST(LogStore, ALateRowReindexesOnlyItsWiresTail) {
  LogStore store;
  Feed feed{store};
  feed.add(0, {entry(1.0, "a"), entry(4.0, "d")});
  feed.add(1, {entry(2.0, "x")});
  const auto change = feed.add(0, {entry(3.0, "c")});
  EXPECT_EQ(change.dropped_front, 0u);
  EXPECT_EQ(change.old_tail, 2u);  // "d" moved: everything from it on is new
  std::vector<std::string> messages;
  for (size_t i = 0; i < store.size(); ++i) {
    messages.push_back(store.row(i).message);
  }
  EXPECT_EQ(messages, (std::vector<std::string>{"a", "x", "c", "d"}));
}

TEST(LogStore, EvictingATopicDropsItsRowsOnly) {
  LogStore store;
  Feed feed{store};
  feed.add(0, {entry(1.0), entry(2.0), entry(3.0)});
  feed.add(1, {entry(4.0), entry(5.0), entry(6.0), entry(7.0)});
  const auto change = feed.evict(0, 2);
  EXPECT_EQ(change.dropped_front, 2u);
  EXPECT_EQ(change.old_tail, 7u);
  EXPECT_EQ(times(store), (std::vector<double>{3.0, 4.0, 5.0, 6.0, 7.0}));
  // A topic whose evicted rows sit behind another topic's: the rows after the first gone one count as new.
  const auto inner = feed.evict(1, 1);
  EXPECT_EQ(inner.dropped_front, 0u);
  EXPECT_EQ(inner.old_tail, 1u);
  EXPECT_EQ(times(store), (std::vector<double>{3.0, 5.0, 6.0, 7.0}));
}

TEST(LogStore, StoreHoldsNoCapacityLimit) {
  constexpr size_t kOldCapacity = 1'000'000;
  LogStore store;
  Feed feed{store};
  std::vector<LogEntry> entries;
  entries.reserve(kOldCapacity + 10);
  for (size_t i = 0; i < kOldCapacity + 10; ++i) {
    entries.push_back(entry(static_cast<double>(i), i == 0 ? "first" : std::string()));
  }
  feed.add(0, std::move(entries));
  ASSERT_EQ(store.size(), kOldCapacity + 10);
  EXPECT_EQ(store.ref(0).time, 0.0);
  EXPECT_EQ(store.row(0).message, "first");
}

TEST(LogStore, RowKeySurvivesAReloadThatRenumbersTheWire) {
  LogStore store;
  Feed feed{store};
  feed.add(0, {entry(1.0, "w0 a"), entry(2.0, "w0 b"), entry(2.0, "w0 c")});
  feed.add(1, {entry(2.0, "w1 a")});
  const LogStore::RowKey key = store.keyOf(2);  // "w0 c", the second wire-0 row at 2.0
  ASSERT_EQ(store.row(2).message, "w0 c");
  // A late insert before it: the reload numbers every row anew.
  feed.replace(0, {entry(0.5, "w0 late"), entry(1.0, "w0 a"), entry(2.0, "w0 b"), entry(2.0, "w0 c")});
  const auto found = store.indexOf(key);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(store.row(*found).message, "w0 c");
  feed.remove(0);
  EXPECT_FALSE(store.indexOf(key).has_value());
}

TEST(LogStore, AnEvictedRowReadsEmpty) {
  LogStore store;
  Feed feed{store};
  feed.add(0, {entry(1.0, "gone"), entry(2.0, "kept")});
  ASSERT_EQ(store.row(0).message, "gone");  // cached now
  feed.source(0)->dropFront(1);             // the source evicts before the store hears of it
  EXPECT_FALSE(store.row(0).present);
  EXPECT_EQ(store.row(0).message, "");
  EXPECT_EQ(store.row(1).message, "kept");
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
  Feed feed{store};
  feed.add(0, {entry(1.0), entry(2.0), entry(3.0)});
  auto change = StoreChange::none(store.size());
  change.then(feed.add(1, {entry(2.5)}));  // old_tail 2
  change.then(feed.add(0, {entry(4.0)}));  // appended: old_tail 4 in the middle state
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
    feed_.add(wire, {entry(time, message, level, source, category)});
  }

  std::vector<double> pass(const LogFilter& filter) const {
    std::vector<uint32_t> indices;
    CompiledFilter(filter, store_).collect(0, store_.size(), indices);
    std::vector<double> out;
    for (uint32_t index : indices) {
      out.push_back(store_.ref(index).time);
    }
    return out;
  }

  LogStore store_;
  Feed feed_{store_};
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
  EXPECT_TRUE(store_.hasSources());
  EXPECT_FALSE(store_.hasCategories());  // an empty category is none
  add(0, 7.0, LogLevel::kInfo, "/a", "joint limit", "motion");
  add(1, 8.0, LogLevel::kInfo, "/b", "link up", "net");
  EXPECT_TRUE(store_.hasCategories());

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

TEST_F(LogFilterTest, EscapesAreStrippedBeforeMatching) {
  add(0, 7.0, LogLevel::kInfo, "/a", "\x1b[31mred\x1b[0m alert");
  LogFilter text;
  text.text = "red alert";
  EXPECT_EQ(pass(text), (std::vector<double>{7.0}));
  EXPECT_EQ(store_.row(6).message, "red alert");
}

/// The substring filter the id-set filter replaced: every row's strings compared one by one.
std::vector<uint32_t> substringFilter(const LogStore& store, const LogFilter& filter) {
  const auto lower = [](std::string text) {
    for (char& c : text) {
      c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return text;
  };
  std::vector<uint32_t> out;
  for (size_t i = 0; i < store.size(); ++i) {
    const auto row = store.row(i);
    const auto level = static_cast<size_t>(row.level);
    if (level != 0 && !filter.levels[level]) {
      continue;
    }
    if (PJ::log_view::containsCaseInsensitive(store.text(row.source), lower(filter.source)) &&
        PJ::log_view::containsCaseInsensitive(store.text(row.category), lower(filter.category)) &&
        PJ::log_view::containsCaseInsensitive(row.message, lower(filter.text))) {
      out.push_back(static_cast<uint32_t>(i));
    }
  }
  return out;
}

class LogFilterFixture1000 : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::vector<std::string> sources = {"/planner", "/Controller", "/camera_driver", ""};
    const std::vector<std::string> categories = {"", "motion", "net", "IO"};
    for (uint32_t wire = 0; wire < 3; ++wire) {
      std::vector<LogEntry> entries;
      for (int i = 0; i < 333 + static_cast<int>(wire); ++i) {
        entries.push_back(entry(
            i * 0.01 + wire * 0.003, "msg " + std::to_string(i) + (i % 7 == 0 ? " Motor FAULT" : " ok"),
            static_cast<LogLevel>(i % 6), sources[(i + wire) % sources.size()],
            categories[(i / 3 + wire) % categories.size()]));
      }
      feed_.add(wire, std::move(entries));
    }
    ASSERT_EQ(store_.size(), 1002u);
  }
  std::vector<LogFilter> filters() const {
    std::vector<LogFilter> out(6);
    out[1].source = "CONT";
    out[2].category = "mot";
    out[3].text = "motor fault";
    out[4].levels[static_cast<size_t>(LogLevel::kDebug)] = false;
    out[4].levels[static_cast<size_t>(LogLevel::kInfo)] = false;
    out[4].source = "a";
    out[5].source = "never";
    return out;
  }
  LogStore store_;
  Feed feed_{store_};
};

TEST_F(LogFilterFixture1000, FilterByDictionaryIdsMatchesSubstringFilter) {
  for (const LogFilter& filter : filters()) {
    std::vector<uint32_t> indices;
    CompiledFilter(filter, store_).collect(0, store_.size(), indices);
    EXPECT_EQ(indices, substringFilter(store_, filter))
        << filter.source << "|" << filter.category << "|" << filter.text;
  }
}

TEST_F(LogFilterFixture1000, ResumableCollectMatchesOneShot) {
  for (const LogFilter& filter : filters()) {
    const CompiledFilter compiled(filter, store_);
    std::vector<uint32_t> one_shot;
    compiled.collect(0, store_.size(), one_shot);
    std::vector<uint32_t> sliced;
    size_t position = 0;
    int slices = 0;
    while (position < store_.size()) {
      const size_t next = compiled.collect(position, store_.size(), 97, sliced);
      ASSERT_GT(next, position);
      ASSERT_LE(next - position, 97u);
      position = next;
      ++slices;
    }
    EXPECT_EQ(slices, 11);
    EXPECT_EQ(sliced, one_shot);
  }
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
std::vector<LogEntry> rows(int first, int count) {
  std::vector<LogEntry> out;
  for (int t = first; t < first + count; ++t) {
    out.push_back(entry(t, "t=" + std::to_string(t), t % 2 != 0 ? LogLevel::kDebug : LogLevel::kInfo));
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

  /// Adds rows, then evicts the oldest beyond five, as a store's retention does.
  void insert(int first, int count) {
    StoreChange change = feed_.add(0, rows(first, count));
    const size_t held = feed_.source(0)->size();
    if (held > 5) {
      change.then(feed_.evict(0, held - 5));
    }
    model_.applyStoreChange(change);
    model_.setTracker(tracker_);
  }

  LogStore store_;
  Feed feed_{store_};
  LogTableModel model_{store_, nullptr, nullptr};
  QAbstractItemModelTester tester_{&model_, QAbstractItemModelTester::FailureReportingMode::Warning};
  QtMessageHandler previous_ = nullptr;
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

// A filter over a store larger than one slice runs across event-loop turns; the model shows the previous result until
// the new one is complete.
TEST(LogTableModelSlicedFilter, ShowsThePreviousRowsUntilTheNewFilterIsComplete) {
  LogStore store;
  Feed feed{store};
  LogTableModel model(store, nullptr, nullptr);
  const size_t total = 3 * LogTableModel::kFilterSliceRows;
  std::vector<LogEntry> entries;
  for (size_t i = 0; i < total; ++i) {
    entries.push_back(entry(static_cast<double>(i), i % 2 == 0 ? "even" : "odd"));
  }
  model.applyStoreChange(feed.add(0, std::move(entries)));
  model.setTracker(1e300);
  ASSERT_EQ(model.rowCount(), static_cast<int>(total));
  int applied = 0;
  QObject::connect(&model, &LogTableModel::filterApplied, [&applied]() { ++applied; });
  LogFilter odd;
  odd.text = "odd";
  model.setFilter(odd);
  EXPECT_EQ(model.rowCount(), static_cast<int>(total));  // still the previous result
  EXPECT_EQ(applied, 0);
  for (int turn = 0; turn < 10 && applied == 0; ++turn) {
    QCoreApplication::processEvents();
  }
  EXPECT_EQ(applied, 1);
  EXPECT_EQ(model.rowCount(), static_cast<int>(total / 2));
  EXPECT_EQ(message(model, 0), "odd");
}

// The severity colours the Level cell only; a row after the tracker is greyed whole.
TEST(LogTableModelColours, SeverityColoursTheLevelCellOnly) {
  QWidget palette_source;
  LogStore store;
  Feed feed{store};
  LogTableModel model(store, nullptr, &palette_source);
  model.applyStoreChange(feed.add(
      0, {entry(1.0, "careful", LogLevel::kWarning), entry(2.0, "dead", LogLevel::kFatal),
          entry(3.0, "later", LogLevel::kError)}));
  model.setTracker(2.5);
  const QColor grey = palette_source.palette().color(QPalette::PlaceholderText);
  for (int column = 0; column < LogTableModel::kColumnCount; ++column) {
    const bool level = column == LogTableModel::kLevel;
    EXPECT_EQ(model.index(0, column).data(Qt::ForegroundRole).isValid(), level) << column;
    EXPECT_EQ(model.index(1, column).data(Qt::ForegroundRole).isValid(), level) << column;
    EXPECT_EQ(model.index(1, column).data(Qt::FontRole).isValid(), level) << column;
    EXPECT_EQ(model.index(2, column).data(Qt::ForegroundRole).value<QColor>(), grey) << column;
  }
  EXPECT_TRUE(model.index(1, 0).data(Qt::BackgroundRole).isValid());  // the tracker's row, tinted whole
  EXPECT_TRUE(model.index(1, LogTableModel::kMessage).data(Qt::BackgroundRole).isValid());
}

// The Time column prints what formatTimeLabel prints for the host's format: the playback readout's text.
TEST(LogTableModelTime, TimeColumnFollowsTheHostFormat) {
  LogStore store;
  Feed feed{store};
  LogTableModel model(store, nullptr, nullptr);
  model.applyStoreChange(feed.add(0, {entry(1779215727.239, "instant"), entry(1779215730.5, "later")}));
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
