// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/type_tree.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {
namespace {

class SeriesReaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto dataset_or = engine_.createDataset(DatasetDescriptor{.source_name = "series"});
    ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();
    dataset_id_ = *dataset_or;

    DataWriter writer = engine_.createWriter();
    auto schema_or = writer.registerSchema(
        "row", makeStruct(
                   "row", {
                              makePrimitive("dense", PrimitiveType::kFloat64),
                              makePrimitive("sparse", PrimitiveType::kFloat64),
                              makePrimitive("text", PrimitiveType::kString),
                              makePrimitive("flag", PrimitiveType::kBool),
                              makePrimitive("all_null", PrimitiveType::kFloat64),
                          }));
    ASSERT_TRUE(schema_or.has_value()) << schema_or.error();

    TopicDescriptor descriptor;
    descriptor.name = "/topic";
    descriptor.schema_id = *schema_or;
    descriptor.max_chunk_rows = 3;
    auto topic_or = writer.registerTopic(dataset_id_, descriptor);
    ASSERT_TRUE(topic_or.has_value()) << topic_or.error();
    topic_id_ = *topic_or;

    for (int i = 0; i < 6; ++i) {
      ASSERT_TRUE(writer.beginRow(topic_id_, static_cast<Timestamp>(i * 10)).has_value());
      writer.set(topic_id_, 0, static_cast<double>(i));
      if (i == 0) {
        writer.set(topic_id_, 1, 10.0);
      } else if (i == 2) {
        writer.set(topic_id_, 1, 20.0);
      } else if (i == 5) {
        writer.set(topic_id_, 1, -5.0);
      } else {
        writer.setNull(topic_id_, 1);
      }
      writer.set(topic_id_, 2, std::string_view("text"));
      if (i == 1 || i == 4) {
        writer.set(topic_id_, 3, i == 4);
      } else {
        writer.setNull(topic_id_, 3);
      }
      writer.setNull(topic_id_, 4);
      ASSERT_TRUE(writer.finishRow(topic_id_).has_value());
    }

    const auto changed = engine_.commitChunks(writer.flushAll());
    ASSERT_FALSE(changed.empty());
  }

  DataEngine engine_;
  DatasetId dataset_id_ = 0;
  TopicId topic_id_ = 0;
};

TEST_F(SeriesReaderTest, SparseSeriesExposesOnlyValueBearingSamples) {
  DataReader reader = engine_.createReader();
  auto series_or = reader.series(topic_id_, 1);
  ASSERT_TRUE(series_or.has_value()) << series_or.error();
  const SeriesReader& series = *series_or;

  EXPECT_EQ(series.size(), 3U);
  EXPECT_FALSE(series.empty());

  const auto first = series.sampleAt(0);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->timestamp, 0);
  EXPECT_DOUBLE_EQ(first->value, 10.0);
  EXPECT_EQ(first->row_index, 0U);

  const auto second = series.sampleAt(1);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->timestamp, 20);
  EXPECT_DOUBLE_EQ(second->value, 20.0);
  EXPECT_EQ(second->row_index, 2U);

  const auto third = series.sampleAt(2);
  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(third->timestamp, 50);
  EXPECT_DOUBLE_EQ(third->value, -5.0);
  EXPECT_EQ(third->row_index, 2U);

  EXPECT_FALSE(series.sampleAt(3).has_value());
}

TEST_F(SeriesReaderTest, TimeLookupsUseSeriesIndicesAndSkipNullRows) {
  DataReader reader = engine_.createReader();
  auto series_or = reader.series(topic_id_, 1);
  ASSERT_TRUE(series_or.has_value()) << series_or.error();
  const SeriesReader& series = *series_or;

  EXPECT_FALSE(series.indexAtOrBeforeTime(-1).has_value());
  EXPECT_EQ(series.indexAtOrBeforeTime(0), 0U);
  EXPECT_EQ(series.indexAtOrBeforeTime(10), 0U);
  EXPECT_EQ(series.indexAtOrBeforeTime(20), 1U);
  EXPECT_EQ(series.indexAtOrBeforeTime(49), 1U);
  EXPECT_EQ(series.indexAtOrBeforeTime(50), 2U);

  EXPECT_EQ(series.indexAtOrAfterTime(1), 1U);
  EXPECT_EQ(series.indexAtOrAfterTime(21), 2U);
  EXPECT_FALSE(series.indexAtOrAfterTime(51).has_value());

  const auto before = series.sampleAtOrBeforeTime(49);
  ASSERT_TRUE(before.has_value());
  EXPECT_EQ(before->timestamp, 20);
  EXPECT_DOUBLE_EQ(before->value, 20.0);

  const auto after = series.sampleAtOrAfterTime(21);
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->timestamp, 50);
  EXPECT_DOUBLE_EQ(after->value, -5.0);
}

TEST_F(SeriesReaderTest, SeriesCursorFiltersByTimeRange) {
  DataReader reader = engine_.createReader();
  auto series_or = reader.series(topic_id_, 1);
  ASSERT_TRUE(series_or.has_value()) << series_or.error();
  const SeriesReader& series = *series_or;

  std::vector<Timestamp> timestamps;
  std::vector<double> values;
  auto cursor = series.samples(Range<Timestamp>{.min = 10, .max = 50});
  cursor.forEach([&](const SeriesSample& sample) {
    timestamps.push_back(sample.timestamp);
    values.push_back(sample.value);
  });

  ASSERT_EQ(timestamps.size(), 2U);
  EXPECT_EQ(timestamps[0], 20);
  EXPECT_EQ(timestamps[1], 50);
  EXPECT_DOUBLE_EQ(values[0], 20.0);
  EXPECT_DOUBLE_EQ(values[1], -5.0);
}

TEST_F(SeriesReaderTest, BoundsUseOnlySeriesSamples) {
  DataReader reader = engine_.createReader();
  auto series_or = reader.series(topic_id_, 1);
  ASSERT_TRUE(series_or.has_value()) << series_or.error();
  const SeriesReader& series = *series_or;

  const auto bounds = series.bounds();
  ASSERT_TRUE(bounds.has_value());
  EXPECT_EQ(bounds->time.min, 0);
  EXPECT_EQ(bounds->time.max, 50);
  EXPECT_DOUBLE_EQ(bounds->value.min, -5.0);
  EXPECT_DOUBLE_EQ(bounds->value.max, 20.0);
  EXPECT_EQ(bounds->sample_count, 3U);

  const auto partial = series.bounds(Range<Timestamp>{.min = 1, .max = 49});
  ASSERT_TRUE(partial.has_value());
  EXPECT_EQ(partial->time.min, 20);
  EXPECT_EQ(partial->time.max, 20);
  EXPECT_DOUBLE_EQ(partial->value.min, 20.0);
  EXPECT_DOUBLE_EQ(partial->value.max, 20.0);
  EXPECT_EQ(partial->sample_count, 1U);
}

TEST_F(SeriesReaderTest, AllNullColumnIsAnEmptySeries) {
  DataReader reader = engine_.createReader();
  auto series_or = reader.series(topic_id_, 4);
  ASSERT_TRUE(series_or.has_value()) << series_or.error();
  const SeriesReader& series = *series_or;

  EXPECT_EQ(series.size(), 0U);
  EXPECT_TRUE(series.empty());
  EXPECT_FALSE(series.sampleAt(0).has_value());
  EXPECT_FALSE(series.bounds().has_value());

  std::size_t count = 0;
  series.samples(Range<Timestamp>{.min = 0, .max = 50}).forEach([&](const SeriesSample&) { ++count; });
  EXPECT_EQ(count, 0U);
}

TEST_F(SeriesReaderTest, BoolColumnsAreNumericSeries) {
  DataReader reader = engine_.createReader();
  auto series_or = reader.series(topic_id_, 3);
  ASSERT_TRUE(series_or.has_value()) << series_or.error();
  const SeriesReader& series = *series_or;

  ASSERT_EQ(series.size(), 2U);
  ASSERT_TRUE(series.sampleAt(0).has_value());
  ASSERT_TRUE(series.sampleAt(1).has_value());
  EXPECT_DOUBLE_EQ(series.sampleAt(0)->value, 0.0);
  EXPECT_DOUBLE_EQ(series.sampleAt(1)->value, 1.0);
}

TEST_F(SeriesReaderTest, SeriesCreationValidatesTopicColumnAndType) {
  DataReader reader = engine_.createReader();

  EXPECT_FALSE(reader.series(topic_id_ + 9999U, 0).has_value());
  EXPECT_FALSE(reader.series(topic_id_, 99).has_value());
  EXPECT_FALSE(reader.series(topic_id_, 2).has_value());
}

// A held value can sit chunks behind the cut: every newer chunk leaves the column
// null. Equal timestamps across commits resolve like rows inside one chunk do
// (sealing keeps arrival order): the later commit wins.
TEST(SeriesReaderChunksTest, TimeLookupsHoldAcrossAllNullChunksAndPreferTheLaterCommitOnTies) {
  DataEngine engine;
  auto dataset_or = engine.createDataset(DatasetDescriptor{.source_name = "sparse"});
  ASSERT_TRUE(dataset_or.has_value());

  DataWriter writer = engine.createWriter();
  auto schema_or = writer.registerSchema(
      "row",
      makeStruct(
          "row", {makePrimitive("held", PrimitiveType::kFloat64), makePrimitive("other", PrimitiveType::kFloat64)}));
  ASSERT_TRUE(schema_or.has_value());
  TopicDescriptor descriptor;
  descriptor.name = "/topic";
  descriptor.schema_id = *schema_or;
  auto topic_or = writer.registerTopic(*dataset_or, descriptor);
  ASSERT_TRUE(topic_or.has_value());
  const TopicId topic_id = *topic_or;

  // One commit (chunk) per row; std::nullopt leaves `held` null in that row.
  const auto commit_row = [&](Timestamp ts, std::optional<double> held) {
    ASSERT_TRUE(writer.beginRow(topic_id, ts).has_value());
    if (held.has_value()) {
      writer.set(topic_id, 0, *held);
    } else {
      writer.setNull(topic_id, 0);
    }
    writer.set(topic_id, 1, 0.0);
    ASSERT_TRUE(writer.finishRow(topic_id).has_value());
    ASSERT_FALSE(engine.commitChunks(writer.flushAll()).empty());
  };
  commit_row(100, 1.0);
  commit_row(100, 2.0);  // same timestamp, later commit
  for (Timestamp ts = 200; ts <= 600; ts += 100) {
    commit_row(ts, std::nullopt);
  }

  DataReader reader = engine.createReader();
  auto series_or = reader.series(topic_id, 0);
  ASSERT_TRUE(series_or.has_value());
  const SeriesReader& series = *series_or;

  const auto at_tie = series.sampleAtOrBeforeTime(100);
  ASSERT_TRUE(at_tie.has_value());
  EXPECT_DOUBLE_EQ(at_tie->value, 2.0);

  const auto held = series.sampleAtOrBeforeTime(650);
  ASSERT_TRUE(held.has_value());
  EXPECT_EQ(held->timestamp, 100);
  EXPECT_DOUBLE_EQ(held->value, 2.0);

  EXPECT_FALSE(series.sampleAtOrAfterTime(101).has_value());
}

// Out-of-order ingest stores chunks whose time ranges are non-chronological or
// overlapping; the time-based sample queries must inspect every chunk instead
// of assuming commit order == time order.
TEST(SeriesReaderOverlapTest, TimeSampleQueriesMergeAcrossOutOfOrderChunks) {
  DataEngine engine;
  auto dataset_or = engine.createDataset(DatasetDescriptor{.source_name = "overlap"});
  ASSERT_TRUE(dataset_or.has_value());

  DataWriter writer = engine.createWriter();
  auto schema_or = writer.registerSchema("row", makeStruct("row", {makePrimitive("v", PrimitiveType::kFloat64)}));
  ASSERT_TRUE(schema_or.has_value());
  TopicDescriptor descriptor;
  descriptor.name = "/topic";
  descriptor.schema_id = *schema_or;
  auto topic_or = writer.registerTopic(*dataset_or, descriptor);
  ASSERT_TRUE(topic_or.has_value());
  const TopicId topic_id = *topic_or;

  const auto write_rows = [&](std::initializer_list<std::pair<Timestamp, double>> rows) {
    for (const auto& [ts, value] : rows) {
      ASSERT_TRUE(writer.beginRow(topic_id, ts).has_value());
      writer.set(topic_id, 0, value);
      ASSERT_TRUE(writer.finishRow(topic_id).has_value());
    }
    ASSERT_FALSE(engine.commitChunks(writer.flushAll()).empty());
  };
  write_rows({{100, 1.0}, {150, 2.0}, {200, 3.0}});  // chunk A
  write_rows({{50, 4.0}, {75, 5.0}});                // chunk B: late, entirely BEFORE A
  write_rows({{120, 6.0}, {180, 7.0}});              // chunk C: late, interleaves A

  DataReader reader = engine.createReader();
  auto series_or = reader.series(topic_id, 0);
  ASSERT_TRUE(series_or.has_value());
  const SeriesReader& series = *series_or;

  // Around t=80 only chunk B has earlier samples; a chronological-chunk
  // assumption stops at A (t_min=100 > 80) and misses them.
  const auto before_gap = series.sampleAtOrBeforeTime(80);
  ASSERT_TRUE(before_gap.has_value());
  EXPECT_EQ(before_gap->timestamp, 75);
  EXPECT_DOUBLE_EQ(before_gap->value, 5.0);

  // Inside A's range the best neighbors can come from the interleaved chunk C.
  const auto before_mid = series.sampleAtOrBeforeTime(130);
  ASSERT_TRUE(before_mid.has_value());
  EXPECT_EQ(before_mid->timestamp, 120);
  const auto after_mid = series.sampleAtOrAfterTime(121);
  ASSERT_TRUE(after_mid.has_value());
  EXPECT_EQ(after_mid->timestamp, 150);

  const auto strict_before = series.sampleBeforeTime(100);
  ASSERT_TRUE(strict_before.has_value());
  EXPECT_EQ(strict_before->timestamp, 75);
  const auto strict_after = series.sampleAfterTime(120);
  ASSERT_TRUE(strict_after.has_value());
  EXPECT_EQ(strict_after->timestamp, 150);
  const auto strict_before_120 = series.sampleBeforeTime(120);
  ASSERT_TRUE(strict_before_120.has_value());
  EXPECT_EQ(strict_before_120->timestamp, 100);

  // Extremes: nothing strictly before the earliest / after the latest sample.
  EXPECT_FALSE(series.sampleBeforeTime(50).has_value());
  EXPECT_FALSE(series.sampleAfterTime(200).has_value());
}

// bounds() against a full scan of samples(), over overlapping chunks holding nulls, NaN and infinities (and a chunk
// whose first value is NaN, which poisons its min/max statistics), for random windows and retention floors.
TEST(SeriesReaderBounds, MatchesAFullScan) {
  DataEngine engine;
  auto dataset = engine.createDataset(DatasetDescriptor{.source_name = "bounds"});
  ASSERT_TRUE(dataset.has_value());
  DataWriter writer = engine.createWriter();
  auto schema = writer.registerSchema(
      "row",
      makeStruct("row", {makePrimitive("v", PrimitiveType::kFloat64), makePrimitive("b", PrimitiveType::kBool)}));
  ASSERT_TRUE(schema.has_value());
  TopicDescriptor descriptor;
  descriptor.name = "/t";
  descriptor.schema_id = *schema;
  descriptor.max_chunk_rows = 7;
  auto topic = writer.registerTopic(*dataset, descriptor);
  ASSERT_TRUE(topic.has_value());

  std::mt19937 rng(7);
  const double specials[] = {
      std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity()};
  // Three commits interleave in time, so their chunks overlap.
  for (int pass = 0; pass < 3; ++pass) {
    for (int i = 0; i < 200; ++i) {
      ASSERT_TRUE(writer.beginRow(*topic, static_cast<Timestamp>(i * 3 + pass)).has_value());
      const int roll = static_cast<int>(rng() % 40);
      if (pass == 1 && i % 7 == 0) {
        writer.set(*topic, 0, specials[0]);  // a NaN first value in every chunk of this pass
      } else if (roll == 0) {
        writer.setNull(*topic, 0);
      } else if (roll < 4) {
        writer.set(*topic, 0, specials[roll - 1]);
      } else {
        writer.set(*topic, 0, std::uniform_real_distribution<double>(-100.0, 100.0)(rng));
      }
      writer.set(*topic, 1, (rng() & 1U) != 0);
      ASSERT_TRUE(writer.finishRow(*topic).has_value());
    }
    ASSERT_FALSE(engine.commitChunks(writer.flushAll()).empty());
  }

  const auto lock = engine.lockEngine();
  const TopicStorage* storage = engine.getTopicStorage(*topic);
  ASSERT_NE(storage, nullptr);
  const auto reference = [](const SeriesReader& series, Range<Timestamp> window) {
    std::optional<SeriesBounds> out;
    SeriesBounds b;
    bool any = false;
    bool finite = false;
    series.samples(window).forEach([&](const SeriesSample& s) {
      b.time.min = any ? std::min(b.time.min, s.timestamp) : s.timestamp;
      b.time.max = any ? std::max(b.time.max, s.timestamp) : s.timestamp;
      any = true;
      if (std::isfinite(s.value)) {
        b.value.min = finite ? std::min(b.value.min, s.value) : s.value;
        b.value.max = finite ? std::max(b.value.max, s.value) : s.value;
        finite = true;
      }
      ++b.sample_count;
    });
    if (any && finite) {
      out = b;
    }
    return out;
  };
  for (const std::size_t column : {std::size_t{0}, std::size_t{1}}) {
    for (int trial = 0; trial < 300; ++trial) {
      const Timestamp floor = trial % 3 == 0 ? static_cast<Timestamp>(rng() % 600) : kNoRetentionFloor;
      const SeriesReader series(storage->sealedChunks(), column, floor);
      auto a = static_cast<Timestamp>(rng() % 620) - 10;
      auto b = static_cast<Timestamp>(rng() % 620) - 10;
      const Range<Timestamp> window =
          trial % 10 == 0
              ? Range<
                    Timestamp>{.min = std::numeric_limits<Timestamp>::min(), .max = std::numeric_limits<Timestamp>::max()}
              : Range<Timestamp>{.min = a, .max = b};
      const auto expected = reference(series, window);
      const auto actual = series.bounds(window);
      ASSERT_EQ(actual.has_value(), expected.has_value()) << "column " << column << " trial " << trial;
      if (expected.has_value()) {
        EXPECT_EQ(actual->time.min, expected->time.min) << trial;
        EXPECT_EQ(actual->time.max, expected->time.max) << trial;
        EXPECT_EQ(actual->value.min, expected->value.min) << trial;
        EXPECT_EQ(actual->value.max, expected->value.max) << trial;
        EXPECT_EQ(actual->sample_count, expected->sample_count) << trial;
      }
    }
  }
}

}  // namespace
}  // namespace PJ
