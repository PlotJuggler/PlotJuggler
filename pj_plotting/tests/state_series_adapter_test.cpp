// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <memory>
#include <string_view>
#include <tuple>

#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/StateSeriesAdapter.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

constexpr Timestamp kNs = 1'000'000'000;

// A string-typed single-column topic in a synthetic engine, with helpers to
// append (value|null) samples and commit — the substrate every case reuses.
class StateSeriesAdapterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto dataset_or = session_.dataEngine().createDataset(DatasetDescriptor{.source_name = "test"});
    ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();
    dataset_id_ = *dataset_or;

    auto writer = session_.dataEngine().createWriter();
    auto schema_or = writer.registerSchema("state_sample", makePrimitive("state", PrimitiveType::kString));
    ASSERT_TRUE(schema_or.has_value()) << schema_or.error();

    TopicDescriptor descriptor;
    descriptor.name = "/robot/state";
    descriptor.schema_id = *schema_or;
    descriptor.max_chunk_rows = 4;  // small chunks so multi-chunk paths run
    auto topic_or = writer.registerTopic(dataset_id_, descriptor);
    ASSERT_TRUE(topic_or.has_value()) << topic_or.error();
    topic_id_ = *topic_or;
    ASSERT_TRUE(writer.bindTopicWriter(topic_id_).has_value());
    std::ignore = session_.commitChunks(writer.flushAll());  // registration only, no rows yet

    adapter_ = std::make_unique<StateSeriesAdapter>(
        &session_,
        CurveDescriptor{
            .name = "/robot/state/state",
            .topic_name = "/robot/state",
            .field_name = "state",
            .topic_id = topic_id_,
            .dataset_id = dataset_id_,
            .column_index = 0,
            .field_path = "state",
        },
        PrimitiveType::kString);
  }

  /// A single-column topic of `type` plus its adapter — the non-string cases.
  struct TypedSeries {
    TopicId topic_id = 0;
    std::unique_ptr<StateSeriesAdapter> adapter;
  };
  [[nodiscard]] TypedSeries makeTypedSeries(const char* topic_name, PrimitiveType type) {
    auto writer = session_.dataEngine().createWriter();
    auto schema_or = writer.registerSchema(topic_name, makePrimitive("value", type));
    EXPECT_TRUE(schema_or.has_value());
    TopicDescriptor descriptor;
    descriptor.name = topic_name;
    descriptor.schema_id = *schema_or;
    auto topic_or = writer.registerTopic(dataset_id_, descriptor);
    EXPECT_TRUE(topic_or.has_value());
    EXPECT_TRUE(writer.bindTopicWriter(*topic_or).has_value());
    std::ignore = session_.commitChunks(writer.flushAll());
    return TypedSeries{
        .topic_id = *topic_or,
        .adapter = std::make_unique<StateSeriesAdapter>(
            &session_, CurveDescriptor{.topic_id = *topic_or, .dataset_id = dataset_id_, .column_index = 0}, type),
    };
  }

  /// Append typed samples to a makeTypedSeries topic; `set` writes one value.
  template <typename SetValue>
  void appendTyped(TopicId topic_id, std::initializer_list<Timestamp> seconds, SetValue&& set) {
    auto writer = session_.dataEngine().createWriter();
    ASSERT_TRUE(writer.bindTopicWriter(topic_id).has_value());
    std::size_t sample_index = 0;
    for (const Timestamp second : seconds) {
      ASSERT_TRUE(writer.beginRow(topic_id, second * kNs).has_value());
      set(writer, sample_index++);
      ASSERT_TRUE(writer.finishRow(topic_id).has_value());
    }
    EXPECT_FALSE(session_.commitChunks(writer.flushAll()).empty());
  }

  /// Append one (timestamp, value) sample; nullptr value = null sample.
  void append(std::initializer_list<std::pair<Timestamp, const char*>> samples) {
    auto writer = session_.dataEngine().createWriter();
    ASSERT_TRUE(writer.bindTopicWriter(topic_id_).has_value());
    for (const auto& [seconds, value] : samples) {
      ASSERT_TRUE(writer.beginRow(topic_id_, seconds * kNs).has_value());
      if (value == nullptr) {
        writer.setNull(topic_id_, 0);
      } else {
        writer.set(topic_id_, 0, std::string_view{value});
      }
      ASSERT_TRUE(writer.finishRow(topic_id_).has_value());
    }
    EXPECT_FALSE(session_.commitChunks(writer.flushAll()).empty());
  }

  SessionManager session_;
  DatasetId dataset_id_ = 0;
  TopicId topic_id_ = 0;
  std::unique_ptr<StateSeriesAdapter> adapter_;
};

TEST_F(StateSeriesAdapterTest, RleCollapsesRuns) {
  append({{0, "A"}, {1, "A"}, {2, "A"}, {3, "B"}, {4, "B"}, {5, "A"}});
  ASSERT_TRUE(adapter_->rebuild());
  const auto& segments = adapter_->segments();
  ASSERT_EQ(segments.size(), 3U);
  EXPECT_EQ(segments[0].value, "A");
  EXPECT_EQ(segments[0].t_start_raw_ns, 0);
  EXPECT_EQ(segments[0].t_end_raw_ns, 3 * kNs);
  EXPECT_EQ(segments[1].value, "B");
  EXPECT_EQ(segments[1].t_end_raw_ns, 5 * kNs);
  EXPECT_EQ(segments[2].value, "A");
  // Trailing run stays open — the consumer pins it to the display-range max.
  EXPECT_EQ(segments[2].t_end_raw_ns, RawStateSegment::kOpenEnd);
}

TEST_F(StateSeriesAdapterTest, NullClosesRunLeavingGap) {
  append({{0, "A"}, {2, nullptr}, {4, "B"}});
  ASSERT_TRUE(adapter_->rebuild());
  const auto& segments = adapter_->segments();
  ASSERT_EQ(segments.size(), 2U);
  EXPECT_EQ(segments[0].t_end_raw_ns, 2 * kNs);  // closed by the null, gap until 4
  EXPECT_EQ(segments[1].t_start_raw_ns, 4 * kNs);
}

TEST_F(StateSeriesAdapterTest, EqualTimestampLastWriteWins) {
  append({{0, "A"}, {1, "B"}, {1, "C"}, {2, "C"}});
  ASSERT_TRUE(adapter_->rebuild());
  const auto& segments = adapter_->segments();
  // B never materializes (zero-duration run collapses to the last write, C).
  ASSERT_EQ(segments.size(), 2U);
  EXPECT_EQ(segments[0].value, "A");
  EXPECT_EQ(segments[0].t_end_raw_ns, 1 * kNs);
  EXPECT_EQ(segments[1].value, "C");
  EXPECT_EQ(segments[1].t_start_raw_ns, 1 * kNs);
}

TEST_F(StateSeriesAdapterTest, OutOfOrderIngestMatchesFromScratchScan) {
  append({{10, "B"}, {11, "B"}, {12, "C"}});
  ASSERT_TRUE(adapter_->rebuild());
  // Late batch arrives BEFORE the existing data; the full-rebuild model must
  // produce the same segments a from-scratch scan of the merged store would.
  append({{0, "A"}, {5, "B"}});
  ASSERT_TRUE(adapter_->rebuild());
  const auto& segments = adapter_->segments();
  ASSERT_EQ(segments.size(), 3U);
  EXPECT_EQ(segments[0].value, "A");
  EXPECT_EQ(segments[0].t_start_raw_ns, 0);
  EXPECT_EQ(segments[0].t_end_raw_ns, 5 * kNs);
  EXPECT_EQ(segments[1].value, "B");  // 5..10..11 fuses into ONE run across batches
  EXPECT_EQ(segments[1].t_end_raw_ns, 12 * kNs);
  EXPECT_EQ(segments[2].value, "C");
}

TEST_F(StateSeriesAdapterTest, AppendContinuingRunFusesAcrossCommits) {
  append({{0, "A"}, {1, "A"}});
  ASSERT_TRUE(adapter_->rebuild());
  append({{2, "A"}, {3, "B"}});
  ASSERT_TRUE(adapter_->rebuild());
  const auto& segments = adapter_->segments();
  ASSERT_EQ(segments.size(), 2U);
  EXPECT_EQ(segments[0].value, "A");
  EXPECT_EQ(segments[0].t_end_raw_ns, 3 * kNs);
}

TEST_F(StateSeriesAdapterTest, ClearDropsSegmentsAndRebuildRestores) {
  append({{0, "A"}});
  ASSERT_TRUE(adapter_->rebuild());
  ASSERT_EQ(adapter_->segments().size(), 1U);
  adapter_->clear();
  EXPECT_TRUE(adapter_->segments().empty());
  ASSERT_TRUE(adapter_->rebuild());
  EXPECT_EQ(adapter_->segments().size(), 1U);
}

TEST_F(StateSeriesAdapterTest, FailedQueryKeepsPreviousSegments) {
  append({{0, "A"}});
  ASSERT_TRUE(adapter_->rebuild());
  ASSERT_EQ(adapter_->segments().size(), 1U);

  StateSeriesAdapter bogus(
      &session_, CurveDescriptor{.topic_id = 987654, .dataset_id = dataset_id_, .column_index = 0},
      PrimitiveType::kString);
  EXPECT_FALSE(bogus.rebuild());
  EXPECT_TRUE(bogus.segments().empty());
}

TEST_F(StateSeriesAdapterTest, IntegerSeriesFormatsDecimalLabels) {
  TypedSeries series = makeTypedSeries("/robot/mode", PrimitiveType::kInt64);
  const std::array<int64_t, 4> values = {0, 0, 3, -7};
  appendTyped(series.topic_id, {0, 1, 2, 3}, [&](DataWriter& writer, std::size_t i) {
    writer.set(series.topic_id, 0, values[i]);
  });
  ASSERT_TRUE(series.adapter->rebuild());
  const auto& segments = series.adapter->segments();
  ASSERT_EQ(segments.size(), 3U);
  EXPECT_EQ(segments[0].value, u"0"_s);
  EXPECT_EQ(segments[0].t_end_raw_ns, 2 * kNs);  // 0,0 fuse into one run
  EXPECT_EQ(segments[1].value, u"3"_s);
  EXPECT_EQ(segments[2].value, u"-7"_s);
}

TEST_F(StateSeriesAdapterTest, IntegerExtremesStayExact) {
  TypedSeries min_series = makeTypedSeries("/robot/int_min", PrimitiveType::kInt64);
  appendTyped(min_series.topic_id, {0}, [&](DataWriter& writer, std::size_t) {
    writer.set(min_series.topic_id, 0, std::numeric_limits<int64_t>::min());
  });
  ASSERT_TRUE(min_series.adapter->rebuild());
  ASSERT_EQ(min_series.adapter->segments().size(), 1U);
  EXPECT_EQ(min_series.adapter->segments()[0].value, u"-9223372036854775808"_s);

  // Above 2^53: a double round-trip would corrupt this; the uint64 read must not.
  TypedSeries max_series = makeTypedSeries("/robot/uint_max", PrimitiveType::kUint64);
  appendTyped(max_series.topic_id, {0}, [&](DataWriter& writer, std::size_t) {
    writer.set(max_series.topic_id, 0, std::numeric_limits<uint64_t>::max());
  });
  ASSERT_TRUE(max_series.adapter->rebuild());
  ASSERT_EQ(max_series.adapter->segments().size(), 1U);
  EXPECT_EQ(max_series.adapter->segments()[0].value, u"18446744073709551615"_s);
}

TEST_F(StateSeriesAdapterTest, AlternatingBoolReadsPackedEncoding) {
  // Alternating values force a non-constant (packed) bool column — the case
  // where a numeric read would flatten everything to "false".
  TypedSeries series = makeTypedSeries("/robot/estop", PrimitiveType::kBool);
  appendTyped(series.topic_id, {0, 1, 2, 3}, [&](DataWriter& writer, std::size_t i) {
    writer.set(series.topic_id, 0, (i % 2) == 0);
  });
  ASSERT_TRUE(series.adapter->rebuild());
  const auto& segments = series.adapter->segments();
  ASSERT_EQ(segments.size(), 4U);
  EXPECT_EQ(segments[0].value, u"true"_s);
  EXPECT_EQ(segments[1].value, u"false"_s);
  EXPECT_EQ(segments[2].value, u"true"_s);
  EXPECT_EQ(segments[3].value, u"false"_s);
}

TEST_F(StateSeriesAdapterTest, EmptyStringIsALabelDistinctFromNull) {
  append({{0, "A"}, {1, ""}, {2, nullptr}, {4, "A"}});
  ASSERT_TRUE(adapter_->rebuild());
  const auto& segments = adapter_->segments();
  ASSERT_EQ(segments.size(), 3U);
  EXPECT_EQ(segments[1].value, u""_s);           // the empty label IS a state...
  EXPECT_EQ(segments[1].t_end_raw_ns, 2 * kNs);  // ...closed by the null (gap)
  EXPECT_EQ(segments[2].t_start_raw_ns, 4 * kNs);
}

TEST_F(StateSeriesAdapterTest, OutOfRangeColumnYieldsNoSegments) {
  // A column index the chunks do not carry (columns can appear mid-stream, so
  // older chunks legitimately have fewer): every sample reads as null.
  append({{0, "A"}, {1, "B"}});
  StateSeriesAdapter beyond(
      &session_, CurveDescriptor{.topic_id = topic_id_, .dataset_id = dataset_id_, .column_index = 5},
      PrimitiveType::kString);
  ASSERT_TRUE(beyond.rebuild());
  EXPECT_TRUE(beyond.segments().empty());
}

TEST_F(StateSeriesAdapterTest, MismatchedLogicalTypeYieldsNoSegmentsNotAThrow) {
  // An adapter whose declared type disagrees with the column (only possible if
  // a caller sources them from different schema walks): every sample reads as
  // null — uniformly, including the string arm, which would otherwise throw
  // through readString on a non-dictionary column.
  TypedSeries series = makeTypedSeries("/robot/mislabeled", PrimitiveType::kInt64);
  appendTyped(series.topic_id, {0, 1}, [&](DataWriter& writer, std::size_t i) {
    writer.set(series.topic_id, 0, static_cast<int64_t>(i));
  });
  StateSeriesAdapter mismatched(
      &session_, CurveDescriptor{.topic_id = series.topic_id, .dataset_id = dataset_id_, .column_index = 0},
      PrimitiveType::kString);
  ASSERT_TRUE(mismatched.rebuild());
  EXPECT_TRUE(mismatched.segments().empty());
}

TEST_F(StateSeriesAdapterTest, FractionalFloatValuesYieldNoSegments) {
  // A float adapter labels only whole numbers; fractional samples read as null.
  TypedSeries series = makeTypedSeries("/robot/speed", PrimitiveType::kFloat64);
  appendTyped(series.topic_id, {0, 1}, [&](DataWriter& writer, std::size_t i) {
    writer.set(series.topic_id, 0, static_cast<double>(i) + 0.5);
  });
  ASSERT_TRUE(series.adapter->rebuild());
  EXPECT_TRUE(series.adapter->segments().empty());
}

TEST_F(StateSeriesAdapterTest, WholeNumberFloatSeriesFormatsIntegerLabels) {
  // JSON integers parsed as double: labels read "3", not "3.0".
  TypedSeries series = makeTypedSeries("/robot/mode", PrimitiveType::kFloat64);
  const std::vector<double> values = {3.0, 3.0, -1.0, 1e15};
  appendTyped(series.topic_id, {0, 1, 2, 3}, [&](DataWriter& writer, std::size_t i) {
    writer.set(series.topic_id, 0, values[i]);
  });
  ASSERT_TRUE(series.adapter->rebuild());
  const auto& segments = series.adapter->segments();
  ASSERT_EQ(segments.size(), 3U);
  EXPECT_EQ(segments[0].value, u"3"_s);
  EXPECT_EQ(segments[0].t_end_raw_ns, 2 * kNs);
  EXPECT_EQ(segments[1].value, u"-1"_s);
  EXPECT_EQ(segments[2].value, u"1000000000000000"_s);
}

}  // namespace
}  // namespace PJ
