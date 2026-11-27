// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// LogEntries, the columnar storage of a kLog series: where it shares OrderedEntries' contract (ordering, UID cursors,
// identity revisions) the two are driven with the same inputs and compared.

#include "pj_datastore/log_entries.hpp"

#include <gtest/gtest.h>

#include <string>
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

}  // namespace
}  // namespace PJ
