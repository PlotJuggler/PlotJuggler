// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// LogEntries, the columnar storage of a kLog series: where it shares OrderedEntries' contract (ordering, UID cursors,
// identity revisions) the two are driven with the same inputs and compared.

#include "pj_datastore/log_entries.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <variant>
#include <vector>

#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_datastore/ordered_entries.hpp"

namespace PJ {
namespace {

sdk::Log makeLog(
    std::string message, std::string source = "node", sdk::Log::Level level = sdk::Log::Level::kInfo,
    Timestamp own_stamp = 0, std::string category = {}) {
  sdk::Log log;
  log.timestamp_ns = own_stamp;
  log.level = level;
  log.message = std::move(message);
  log.source = std::move(source);
  log.category = std::move(category);
  return log;
}

ObjectEntry entryAt(Timestamp ts, SequentialUID uid) {
  ObjectEntry entry;
  entry.timestamp = ts;
  entry.sequential_uid = uid;
  return entry;
}

TEST(LogEntries, AppendsRoundTripEveryField) {
  LogEntries log;
  const std::vector<sdk::Log> in = {
      makeLog("first", "nav", sdk::Log::Level::kWarning, 105, "planner"),
      makeLog("second", "ctl", sdk::Log::Level::kDebug, 0),
      makeLog("third", "nav", sdk::Log::Level::kFatal, 293, "x"),
  };
  log.push(100, SequentialUID::getNext(), in[0]);
  log.push(200, SequentialUID::getNext(), in[1]);
  log.push(300, SequentialUID::getNext(), in[2]);

  ASSERT_EQ(log.size(), 3u);
  for (size_t i = 0; i < in.size(); ++i) {
    EXPECT_EQ(log.recordAt(i).log, in[i]) << i;
  }
  EXPECT_EQ(log.ownStamp(0), 105);
  EXPECT_EQ(log.ownStamp(1), 0);
  EXPECT_EQ(log.ownStamp(2), 293);
}

TEST(LogEntries, OutOfOrderInsertMatchesOrderedEntries) {
  OrderedEntries ordered;
  LogEntries log;
  for (const Timestamp ts : {30, 10, 20, 10}) {
    const SequentialUID uid = SequentialUID::getNext();
    const auto ordered_order = ordered.push(entryAt(ts, uid));
    const auto log_order = log.push(ts, uid, makeLog("m" + std::to_string(ts)));
    EXPECT_EQ(log_order, ordered_order) << ts;
  }
  ASSERT_EQ(log.timestamps(), ordered.timestamps());
  for (size_t i = 0; i < log.size(); ++i) {
    EXPECT_EQ(log.uidAt(i), ordered.entryAt(i).sequential_uid) << i;
    EXPECT_EQ(log.recordAt(i).log.message, "m" + std::to_string(log.timestamps()[i])) << i;
  }
  for (Timestamp t = 0; t <= 40; ++t) {
    EXPECT_EQ(log.indexAtOrBefore(t), ordered.indexAtOrBefore(t)) << t;
    EXPECT_EQ(log.maxUidAtOrBefore(t), ordered.maxUidAtOrBefore(t)) << t;
  }
  EXPECT_EQ(log.rangeByTime(5, 25), ordered.rangeByTime(5, 25));
  EXPECT_EQ(log.firstUid(), ordered.firstUid());
  for (SequentialUID u = ordered.firstUid(); u.valid(); u = ordered.nextUidAfter(u)) {
    EXPECT_EQ(log.nextUidAfter(u), ordered.nextUidAfter(u));
    EXPECT_EQ(log.indexOfUid(u), ordered.indexOfUid(u));
    EXPECT_EQ(log.oldestTimestampAfterUid(u), ordered.oldestTimestampAfterUid(u));
  }
}

TEST(LogEntries, IdentityMovesLikeOrderedEntries) {
  LogEntries log;
  log.push(10, SequentialUID::getNext(), makeLog("a"));
  const auto before_append = log.contentIdentity();
  log.push(20, SequentialUID::getNext(), makeLog("b"));
  const auto after_append = log.contentIdentity();
  EXPECT_NE(after_append.content, before_append.content);
  EXPECT_EQ(after_append.rewrite, before_append.rewrite);
  EXPECT_EQ(after_append.removal, before_append.removal);

  log.push(5, SequentialUID::getNext(), makeLog("late"));
  const auto after_insert = log.contentIdentity();
  EXPECT_NE(after_insert.content, after_append.content);
  EXPECT_NE(after_insert.rewrite, after_append.rewrite);
  EXPECT_EQ(after_insert.removal, after_append.removal);
}

TEST(LogEntries, CanonicalBytesDecodeBackToTheLog) {
  LogEntries log;
  const sdk::Log in = makeLog("hello", "src", sdk::Log::Level::kError, 42, "cat");
  log.push(7, SequentialUID::getNext(), in);
  const std::vector<uint8_t> bytes = log.canonicalBytesAt(0);
  auto decoded = deserializeBuiltinObject(sdk::BuiltinObjectType::kLog, bytes.data(), bytes.size());
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  const sdk::Log* out = decoded->get<sdk::Log>();
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(*out, in);
}

TEST(LogEntries, SourcesShareDictionaryIds) {
  LogEntries log;
  log.push(1, SequentialUID::getNext(), makeLog("a", "nav"));
  log.push(2, SequentialUID::getNext(), makeLog("b", "ctl"));
  log.push(3, SequentialUID::getNext(), makeLog("c", "nav"));
  EXPECT_EQ(log.sourceId(0), log.sourceId(2));
  EXPECT_NE(log.sourceId(0), log.sourceId(1));
  EXPECT_EQ(log.dictionaryText(log.sourceId(0)), "nav");
  EXPECT_EQ(log.categoryId(0), 0u);
  EXPECT_EQ(log.dictionaryText(0), "");
}

// --- Mutations, driven alike on both storages ---

// An OrderedEntries and a LogEntries with the same rows; the ordered payload carries the message, so the two can be
// compared row by row after a mutation moves rows around.
struct Twin {
  OrderedEntries ordered;
  LogEntries log;

  void push(Timestamp ts, const std::string& message) {
    const SequentialUID uid = SequentialUID::getNext();
    ObjectEntry entry = entryAt(ts, uid);
    entry.payload = std::make_shared<const std::vector<uint8_t>>(message.begin(), message.end());
    ordered.push(std::move(entry));
    log.push(ts, uid, makeLog(message));
  }

  static std::string orderedMessage(const ObjectEntry& entry) {
    const auto& bytes = *std::get<SharedBuffer>(entry.payload);
    return {bytes.begin(), bytes.end()};
  }

  void expectSame(const std::string& scenario) const {
    ASSERT_EQ(log.size(), ordered.size()) << scenario;
    EXPECT_EQ(log.timestamps(), ordered.timestamps()) << scenario;
    for (size_t i = 0; i < log.size(); ++i) {
      EXPECT_EQ(log.uidAt(i), ordered.entryAt(i).sequential_uid) << scenario << " row " << i;
      EXPECT_EQ(log.recordAt(i).log.message, orderedMessage(ordered.entryAt(i))) << scenario << " row " << i;
    }
    std::vector<SequentialUID> log_walk;
    std::vector<SequentialUID> ordered_walk;
    for (SequentialUID u = log.firstUid(); u.valid(); u = log.nextUidAfter(u)) {
      log_walk.push_back(u);
    }
    for (SequentialUID u = ordered.firstUid(); u.valid(); u = ordered.nextUidAfter(u)) {
      ordered_walk.push_back(u);
    }
    EXPECT_EQ(log_walk, ordered_walk) << scenario;
  }
};

struct Moved {
  bool content;
  bool rewrite;
  bool removal;
  bool operator==(const Moved&) const = default;
};
Moved moved(const OrderedEntries::ContentIdentity& before, const OrderedEntries::ContentIdentity& after) {
  return {before.content != after.content, before.rewrite != after.rewrite, before.removal != after.removal};
}

void fill(Twin& twin) {
  for (const Timestamp ts : {10, 40, 20, 60, 20, 80, 100, 30}) {
    twin.push(ts, "m" + std::to_string(ts) + "-" + std::to_string(twin.log.size()));
  }
}

std::vector<SequentialUID> uidsOf(const std::vector<ObjectEntry>& entries) {
  std::vector<SequentialUID> out;
  for (const ObjectEntry& entry : entries) {
    out.push_back(entry.sequential_uid);
  }
  return out;
}

TEST(LogEntries, MutatorsMatchOrderedEntries) {
  struct Scenario {
    std::string name;
    std::function<std::vector<SequentialUID>(OrderedEntries&)> on_ordered;
    std::function<std::vector<SequentialUID>(LogEntries&)> on_log;
  };
  const EntryPredicate every_twentieth = [](Timestamp t, SequentialUID) { return t % 20 == 0; };
  const std::vector<Scenario> scenarios = {
      {"evictFront",
       [](OrderedEntries& o) {
         std::vector<ObjectEntry> removed;
         o.evictFront(2, removed);
         return uidsOf(removed);
       },
       [](LogEntries& l) { return l.evictFront(2); }},
      {"remove",
       [&](OrderedEntries& o) {
         std::vector<ObjectEntry> removed;
         o.remove(o.planRemoval(every_twentieth), removed);
         return uidsOf(removed);
       },
       [&](LogEntries& l) { return l.remove(l.planRemoval(every_twentieth)); }},
      {"removeAndPush",
       [&](OrderedEntries& o) {
         std::vector<ObjectEntry> removed;
         ObjectEntry entry = entryAt(50, SequentialUID::getNext());
         const std::string message = "pushed";
         entry.payload = std::make_shared<const std::vector<uint8_t>>(message.begin(), message.end());
         // Same UID on both sides: the log scenario reads it back from the ordered one.
         o.removeAndPush(o.planRemoval(every_twentieth), std::move(entry), removed);
         return uidsOf(removed);
       },
       nullptr},
      {"clear",
       [](OrderedEntries& o) {
         o.clear();
         return std::vector<SequentialUID>{};
       },
       [](LogEntries& l) {
         l.clear();
         return std::vector<SequentialUID>{};
       }},
      {"shift",
       [](OrderedEntries& o) {
         o.shift(100);
         return std::vector<SequentialUID>{};
       },
       [](LogEntries& l) {
         l.shift(100);
         return std::vector<SequentialUID>{};
       }},
  };
  for (const Scenario& scenario : scenarios) {
    Twin twin;
    fill(twin);
    const auto ordered_before = twin.ordered.contentIdentity();
    const auto log_before = twin.log.contentIdentity();
    std::vector<SequentialUID> ordered_removed = scenario.on_ordered(twin.ordered);
    std::vector<SequentialUID> log_removed;
    if (scenario.on_log) {
      log_removed = scenario.on_log(twin.log);
    } else {
      // removeAndPush: push the entry the ordered side just took, with its UID.
      const size_t at = *twin.ordered.indexAtOrBefore(50);
      auto result = twin.log.removeAndPush(
          twin.log.planRemoval(every_twentieth), 50, twin.ordered.entryAt(at).sequential_uid, makeLog("pushed"));
      log_removed = std::move(result.second);
    }
    EXPECT_EQ(log_removed, ordered_removed) << scenario.name;
    EXPECT_EQ(moved(log_before, twin.log.contentIdentity()), moved(ordered_before, twin.ordered.contentIdentity()))
        << scenario.name;
    twin.expectSame(scenario.name);
  }
}

TEST(LogEntries, ReuidAndRebuildMatchOrderedEntries) {
  Twin twin;
  fill(twin);
  const auto ordered_before = twin.ordered.contentIdentity();
  const auto log_before = twin.log.contentIdentity();
  twin.ordered.reuidAll();
  twin.log.reuidAll();
  EXPECT_EQ(moved(log_before, twin.log.contentIdentity()), moved(ordered_before, twin.ordered.contentIdentity()));
  ASSERT_EQ(twin.log.timestamps(), twin.ordered.timestamps());
  for (size_t i = 1; i < twin.log.size(); ++i) {
    EXPECT_LT(twin.log.uidAt(i - 1), twin.log.uidAt(i)) << i;
  }
  EXPECT_EQ(twin.log.firstUid(), twin.log.uidAt(0));
}

TEST(LogEntries, AppendReuidFromMatchesOrderedEntries) {
  Twin destination;
  fill(destination);
  Twin source;
  for (const Timestamp ts : {25, 60, 120}) {
    source.push(ts, "s" + std::to_string(ts));
  }
  const auto ordered_before = destination.ordered.contentIdentity();
  const auto log_before = destination.log.contentIdentity();
  destination.ordered.appendReuidFrom(source.ordered);
  destination.log.appendReuidFrom(source.log);
  EXPECT_TRUE(source.log.empty());
  EXPECT_EQ(
      moved(log_before, destination.log.contentIdentity()),
      moved(ordered_before, destination.ordered.contentIdentity()));
  ASSERT_EQ(destination.log.timestamps(), destination.ordered.timestamps());
  for (size_t i = 0; i < destination.log.size(); ++i) {
    EXPECT_EQ(destination.log.recordAt(i).log.message, Twin::orderedMessage(destination.ordered.entryAt(i))) << i;
  }
  // The destination's rows keep their UIDs; the moved rows' are fresh and above them.
  EXPECT_EQ(destination.log.indexOfUid(destination.ordered.entryAt(0).sequential_uid), 0u);
}

TEST(LogEntries, AdoptReuidFromTakesEveryRow) {
  LogEntries destination;
  destination.push(1, SequentialUID::getNext(), makeLog("old"));
  LogEntries source;
  source.push(5, SequentialUID::getNext(), makeLog("new", "a"));
  source.push(6, SequentialUID::getNext(), makeLog("newer", "b"));
  destination.adoptReuidFrom(source);
  EXPECT_TRUE(source.empty());
  ASSERT_EQ(destination.size(), 2u);
  EXPECT_EQ(destination.recordAt(0).log, makeLog("new", "a"));
  EXPECT_EQ(destination.recordAt(1).log, makeLog("newer", "b"));
}

TEST(LogEntries, EvictedCountIsMonotonic) {
  LogEntries log;
  for (Timestamp ts = 1; ts <= 5; ++ts) {
    log.push(ts, SequentialUID::getNext(), makeLog("m"));
  }
  (void)log.evictFront(2);
  (void)log.evictFront(1);
  EXPECT_EQ(log.evicted(), 3u);
  log.clear();
  EXPECT_EQ(log.evicted(), 3u);
}

TEST(LogEntries, ShiftMovesOwnStampsButNotMissingOnes) {
  LogEntries log;
  log.push(100, SequentialUID::getNext(), makeLog("a", "n", sdk::Log::Level::kInfo, 105));
  log.push(200, SequentialUID::getNext(), makeLog("b", "n", sdk::Log::Level::kInfo, 0));
  EXPECT_TRUE(log.shift(1000));
  EXPECT_EQ(log.timestamps()[0], 1100);
  EXPECT_EQ(log.ownStamp(0), 1105);
  EXPECT_EQ(log.ownStamp(1), 0);
  EXPECT_EQ(log.recordAt(0).log.timestamp_ns, 1105);
  EXPECT_EQ(log.recordAt(1).log.timestamp_ns, 0);
}

TEST(LogEntries, AppendFromRemapsDictionaries) {
  LogEntries destination;
  destination.push(1, SequentialUID::getNext(), makeLog("x", "b"));
  LogEntries source;
  source.push(2, SequentialUID::getNext(), makeLog("y", "a", sdk::Log::Level::kInfo, 0, "cat"));
  destination.appendReuidFrom(source);
  ASSERT_EQ(destination.size(), 2u);
  EXPECT_EQ(destination.dictionaryText(destination.sourceId(0)), "b");
  EXPECT_EQ(destination.dictionaryText(destination.sourceId(1)), "a");
  EXPECT_EQ(destination.dictionaryText(destination.categoryId(1)), "cat");
}

TEST(LogEntries, MergeSortedIsStableByTimestamp) {
  LogEntries first;
  first.push(10, SequentialUID::getNext(), makeLog("first@10"));
  first.push(30, SequentialUID::getNext(), makeLog("first@30"));
  LogEntries second;
  second.push(10, SequentialUID::getNext(), makeLog("second@10"));
  second.push(20, SequentialUID::getNext(), makeLog("second@20"));
  first.mergeReuidFrom({&second});
  const LogEntries& merged = first;
  ASSERT_EQ(merged.size(), 4u);
  EXPECT_EQ(merged.recordAt(0).log.message, "first@10");
  EXPECT_EQ(merged.recordAt(1).log.message, "second@10");
  EXPECT_EQ(merged.recordAt(2).log.message, "second@20");
  EXPECT_EQ(merged.recordAt(3).log.message, "first@30");
  EXPECT_TRUE(second.empty());
  for (size_t i = 1; i < merged.size(); ++i) {
    EXPECT_LT(merged.uidAt(i - 1), merged.uidAt(i));
  }
}

// --- Sealed chunks ---

// rosout-like: 8 sources, 3 levels, 30% exact repeats of earlier messages, 40-120 chars, every third with an own stamp.
std::vector<sdk::Log> rosoutLike(size_t count, uint32_t seed = 7) {
  std::mt19937 rng(seed);
  std::vector<sdk::Log> out;
  out.reserve(count);
  const std::vector<std::string> words = {"goal",    "reached",   "planner", "timeout", "waiting",
                                          "for",     "transform", "map",     "odom",    "base_link",
                                          "costmap", "updated",   "x=",      "y=",      "ok"};
  for (size_t i = 0; i < count; ++i) {
    if (i > 0 && rng() % 10 < 3) {
      out.push_back(out[rng() % out.size()]);
      continue;
    }
    std::string message;
    const size_t target = 40 + rng() % 81;
    while (message.size() < target) {
      message += words[rng() % words.size()];
      message += rng() % 4 == 0 ? std::to_string(rng() % 10000) : " ";
    }
    out.push_back(makeLog(
        std::move(message), "node_" + std::to_string(rng() % 8), static_cast<sdk::Log::Level>(1 + rng() % 3),
        i % 3 == 0 ? Timestamp(i * 1000 + 17) : 0, rng() % 2 == 0 ? "cat" : ""));
  }
  return out;
}

LogEntries fillWith(const std::vector<sdk::Log>& logs) {
  LogEntries log;
  for (size_t i = 0; i < logs.size(); ++i) {
    log.push(Timestamp(i * 1000), SequentialUID::getNext(), logs[i]);
  }
  return log;
}

TEST(LogEntries, SealedChunksRoundTripEveryField) {
  const std::vector<sdk::Log> in = rosoutLike(3 * LogEntries::kChunkRows + 7);
  const LogEntries log = fillWith(in);
  ASSERT_EQ(log.size(), in.size());
  size_t checked = 0;
  log.records(0, log.size(), [&](size_t i, LogRecord&& record) {
    EXPECT_EQ(record.log, in[i]) << i;
    EXPECT_EQ(log.level(i), in[i].level) << i;
    EXPECT_EQ(log.dictionaryText(log.sourceId(i)), in[i].source) << i;
    EXPECT_EQ(log.ownStamp(i), in[i].timestamp_ns) << i;
    ++checked;
  });
  EXPECT_EQ(checked, in.size());
  EXPECT_EQ(log.recordAt(5).log, in[5]);
}

TEST(LogEntries, LateInsertIntoSealedChunkKeepsEveryMessage) {
  std::vector<sdk::Log> in = rosoutLike(2 * LogEntries::kChunkRows + 3);
  LogEntries log = fillWith(in);
  const sdk::Log late = makeLog("late arrival", "late_node");
  log.push(1500, SequentialUID::getNext(), late);  // between rows 1 and 2, in the first (sealed) chunk
  in.insert(in.begin() + 2, late);
  ASSERT_EQ(log.size(), in.size());
  log.records(0, log.size(), [&](size_t i, LogRecord&& record) { EXPECT_EQ(record.log, in[i]) << i; });
}

TEST(LogEntries, MessagesRoundTripEmptyHugeAndEscaped) {
  std::vector<sdk::Log> in = rosoutLike(2 * LogEntries::kChunkRows + 10);  // the first chunk sealed
  in[3].message.clear();
  in[4].message = std::string(1 << 20, 'z');
  in[4].message[12345] = '\0';
  in[5].message = "\x1b[31mred\x1b[0m";
  const LogEntries log = fillWith(in);
  for (const size_t i : {3, 4, 5}) {
    EXPECT_EQ(log.recordAt(i).log.message, in[i].message) << i;
  }
  std::vector<std::string> read;
  log.messages(3, 3, [&read](size_t, std::string_view message) { read.emplace_back(message); });
  ASSERT_EQ(read.size(), 3u);
  EXPECT_EQ(read[1], in[4].message);
}

TEST(LogEntries, SealingShrinksMemory) {
  const size_t rows = 40 * LogEntries::kChunkRows;  // the two open chunks at the tail weigh little
  const LogEntries log = fillWith(rosoutLike(rows));
  const double per_row = static_cast<double>(log.memoryBytes()) / static_cast<double>(rows);
  // The generator salts every fourth word with random digits, so its messages compress worse (3.8x) than typical
  // rosout text; the bound guards the layout (index + narrowed columns + one zstd frame), not a compression ratio.
  EXPECT_LT(per_row, 60.0);
  std::cout << "LogEntries bytes/row: " << per_row << "\n";
}

/// Rows with an own stamp on every row, each at its own offset from the store stamp (as ROS logs stamped by their
/// sender), so no sealed chunk stores the offsets as a constant.
LogEntries senderStamped(size_t rows) {
  std::vector<sdk::Log> in = rosoutLike(rows);
  for (size_t i = 0; i < in.size(); ++i) {
    in[i].timestamp_ns = Timestamp(i * 1000 - (i * 37) % 900 + 1);
  }
  return fillWith(in);
}

TEST(LogEntries, ConsecutiveReadsOfASealedChunkDecompressItOnce) {
  const LogEntries log = senderStamped(3 * LogEntries::kChunkRows);
  const uint64_t before = log.decompressions();
  for (size_t i = 0; i < 100; ++i) {
    (void)log.ownStamp(i);
    log.messages(i, 1, [](size_t, std::string_view) {});
    (void)log.recordAt(i);
  }
  EXPECT_EQ(log.decompressions() - before, 1u);
}

TEST(LogEntries, AFullChunkStaysOpenUntilTheNextOneFills) {
  std::vector<sdk::Log> in = rosoutLike(LogEntries::kChunkRows + 10);
  LogEntries log = fillWith(in);
  const uint64_t before = log.decompressions();
  const sdk::Log late = makeLog("late arrival");
  log.push(1500, SequentialUID::getNext(), late);  // into the first, full chunk
  in.insert(in.begin() + 2, late);
  EXPECT_EQ(log.decompressions(), before);  // nothing was unsealed
  log.records(0, log.size(), [&](size_t i, LogRecord&& record) { EXPECT_EQ(record.log, in[i]) << i; });
}

TEST(LogEntries, AResealShowsTheNewRowsNotTheCachedOnes) {
  std::vector<sdk::Log> in = rosoutLike(3 * LogEntries::kChunkRows);
  LogEntries log = fillWith(in);
  EXPECT_EQ(log.recordAt(3).log, in[3]);  // caches the first chunk
  const sdk::Log late = makeLog("late arrival");
  log.push(1500, SequentialUID::getNext(), late);
  in.insert(in.begin() + 2, late);
  for (const size_t i : {2, 3, 4}) {
    EXPECT_EQ(log.recordAt(i).log, in[i]) << i;
  }
}

TEST(LogEntries, FrontEvictionOfASealedChunkDecompressesNothing) {
  const std::vector<sdk::Log> in = rosoutLike(3 * LogEntries::kChunkRows);
  LogEntries log = fillWith(in);
  const uint64_t before = log.decompressions();
  const size_t bytes = log.memoryBytes();
  for (size_t i = 0; i < 10; ++i) {
    (void)log.evictFront(7);
  }
  EXPECT_EQ(log.decompressions(), before);
  EXPECT_LE(log.memoryBytes(), bytes);
  EXPECT_EQ(log.recordAt(0).log, in[70]);
  EXPECT_EQ(log.evicted(), 70u);
  // Evicting past the chunk frees it, and the rows after it read on.
  (void)log.evictFront(LogEntries::kChunkRows);
  EXPECT_LT(log.memoryBytes(), bytes);
  EXPECT_EQ(log.recordAt(0).log, in[70 + LogEntries::kChunkRows]);
  log.records(0, log.size(), [&](size_t i, LogRecord&& record) {
    EXPECT_EQ(record.log, in[i + 70 + LogEntries::kChunkRows]) << i;
  });
}

TEST(LogEntries, FirstRowChangedSinceAnIndexedRevision) {
  LogEntries log = fillWith(rosoutLike(100));
  (void)log.evictFront(10);
  const uint64_t indexed = log.contentIdentity().rewrite;
  EXPECT_EQ(log.firstRowChangedSince(indexed), UINT64_MAX);
  log.push(Timestamp(99'000), SequentialUID::getNext(), makeLog("in order"));
  EXPECT_EQ(log.firstRowChangedSince(indexed), UINT64_MAX);                // an append changes no row
  log.push(Timestamp(50'500), SequentialUID::getNext(), makeLog("late"));  // after row 50, at index 41
  EXPECT_EQ(log.firstRowChangedSince(indexed), 51u);
  log.push(Timestamp(30'500), SequentialUID::getNext(), makeLog("later still"));
  EXPECT_EQ(log.firstRowChangedSince(indexed), 31u);
  (void)log.shift(1);
  EXPECT_EQ(log.firstRowChangedSince(indexed), 0u);  // every row moved
}

}  // namespace
}  // namespace PJ
