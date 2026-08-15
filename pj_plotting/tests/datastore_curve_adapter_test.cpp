// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/DatastoreCurveAdapter.h"
#include "pj_plotting/PointSeriesXY.h"
#include "pj_runtime/SessionManager.h"

namespace PJ {
namespace {

constexpr Timestamp kNs = 1000000000;
constexpr Timestamp kDisplayOffset = 2 * kNs;

class DatastoreCurveAdapterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto domain_or = session_.dataEngine().createTimeDomain("test");
    ASSERT_TRUE(domain_or.has_value()) << domain_or.error();
    time_domain_id_ = *domain_or;
    session_.dataEngine().setDisplayOffset(time_domain_id_, kDisplayOffset);

    auto dataset_or = session_.dataEngine().createDataset(
        DatasetDescriptor{.source_name = "test", .time_domain_id = time_domain_id_});
    ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();
    dataset_id_ = *dataset_or;

    auto writer = session_.dataEngine().createWriter();
    auto schema_or = writer.registerSchema("sample", makePrimitive("value", PrimitiveType::kFloat64));
    ASSERT_TRUE(schema_or.has_value()) << schema_or.error();

    TopicDescriptor descriptor;
    descriptor.name = "/series";
    descriptor.schema_id = *schema_or;
    descriptor.max_chunk_rows = 4;
    auto topic_or = writer.registerTopic(dataset_id_, descriptor);
    ASSERT_TRUE(topic_or.has_value()) << topic_or.error();
    topic_id_ = *topic_or;

    auto handle_or = writer.bindTopicWriter(topic_id_);
    ASSERT_TRUE(handle_or.has_value()) << handle_or.error();

    appendRows(writer, topic_id_, 0, 10);
    EXPECT_FALSE(session_.commitChunks(writer.flushAll()).empty());

    descriptor_ = CurveDescriptor{
        .name = "/series/value",
        .topic_id = topic_id_,
        .dataset_id = dataset_id_,
        .column_index = 0,
        .field_path = "value",
    };
    adapter_ = std::make_unique<DatastoreCurveAdapter>(&session_, descriptor_);
  }

  void appendMoreRows(int first, int last_exclusive) {
    auto writer = session_.dataEngine().createWriter();
    auto handle_or = writer.bindTopicWriter(topic_id_);
    ASSERT_TRUE(handle_or.has_value()) << handle_or.error();
    appendRows(writer, topic_id_, first, last_exclusive);
    EXPECT_FALSE(session_.commitChunks(writer.flushAll()).empty());
  }

  static void appendRows(DataWriter& writer, TopicId topic_id, int first, int last_exclusive) {
    for (int i = first; i < last_exclusive; ++i) {
      ASSERT_TRUE(writer.beginRow(topic_id, static_cast<Timestamp>(i) * kNs).has_value());
      if (i == 5) {
        writer.setNull(topic_id, 0);
      } else {
        writer.set(topic_id, 0, 10.0 + static_cast<double>(i));
      }
      ASSERT_TRUE(writer.finishRow(topic_id).has_value());
    }
  }

  SessionManager session_;
  TimeDomainId time_domain_id_ = 0;
  DatasetId dataset_id_ = 0;
  TopicId topic_id_ = 0;
  CurveDescriptor descriptor_;
  std::unique_ptr<DatastoreCurveAdapter> adapter_;
};

TEST_F(DatastoreCurveAdapterTest, DefaultAllRowsWorksBeforeRectOfInterest) {
  ASSERT_EQ(adapter_->size(), 9U);

  const QPointF first = adapter_->sample(0);
  EXPECT_DOUBLE_EQ(first.x(), -2.0);
  EXPECT_DOUBLE_EQ(first.y(), 10.0);

  const QPointF last = adapter_->sample(8);
  EXPECT_DOUBLE_EQ(last.x(), 7.0);
  EXPECT_DOUBLE_EQ(last.y(), 19.0);
}

TEST_F(DatastoreCurveAdapterTest, RectOfInterestNarrowsAndAddsBoundaryGuards) {
  adapter_->setRectOfInterest(QRectF(QPointF(3.0, -1.0), QPointF(4.0, 1.0)));

  // One off-screen guard per side, even when a sample sits exactly on the
  // window edge — the segment leaving the viewport toward the next sample
  // must still be drawn (Qwt clips it at the boundary).
  ASSERT_EQ(adapter_->size(), 3U);
  EXPECT_DOUBLE_EQ(adapter_->sample(0).x(), 2.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(0).y(), 14.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(1).x(), 4.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(1).y(), 16.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(2).x(), 5.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(2).y(), 17.0);
}

TEST_F(DatastoreCurveAdapterTest, SampleLookupWorksAcrossChunksAndNonSequentialAccess) {
  EXPECT_DOUBLE_EQ(adapter_->sample(3).y(), 13.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(4).y(), 14.0);

  EXPECT_DOUBLE_EQ(adapter_->sample(8).y(), 19.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(0).y(), 10.0);
}

TEST_F(DatastoreCurveAdapterTest, NullRowsAreSkipped) {
  ASSERT_EQ(adapter_->size(), 9U);
  const QPointF sample_after_null = adapter_->sample(5);
  EXPECT_DOUBLE_EQ(sample_after_null.x(), 4.0);
  EXPECT_DOUBLE_EQ(sample_after_null.y(), 16.0);
}

TEST_F(DatastoreCurveAdapterTest, DisplayOffsetShiftsSamplesAndFullBounds) {
  adapter_->setRectOfInterest(QRectF(QPointF(3.0, -1.0), QPointF(4.0, 1.0)));

  const QRectF bounds = adapter_->boundingRect();
  EXPECT_DOUBLE_EQ(bounds.left(), -2.0);
  EXPECT_DOUBLE_EQ(bounds.right(), 7.0);
  EXPECT_DOUBLE_EQ(bounds.top(), 10.0);
  EXPECT_DOUBLE_EQ(bounds.bottom(), 19.0);
}

TEST_F(DatastoreCurveAdapterTest, OnDisplayOffsetChangedReshiftsWithoutReingest) {
  // Reset the fixture's offset so the raw->display mapping is identity, then the
  // sample at raw t=2 s (index 2, value 12) sits at display X = 2.0 s.
  session_.dataEngine().setDisplayOffset(time_domain_id_, 0);
  adapter_->onDisplayOffsetChanged();

  const QPointF before = adapter_->sample(2);
  EXPECT_DOUBLE_EQ(before.x(), 2.0);
  EXPECT_DOUBLE_EQ(before.y(), 12.0);

  // Apply a +1 s offset on the dataset's domain, then invalidate the adapter.
  // display = (raw - offset) / 1e9, so the curve slides 1 s earlier on the axis.
  session_.dataEngine().setDisplayOffset(time_domain_id_, kNs);  // +1 s
  adapter_->onDisplayOffsetChanged();

  const QPointF after = adapter_->sample(2);
  EXPECT_DOUBLE_EQ(after.x(), 1.0);         // 2 s - 1 s, no re-ingest
  EXPECT_DOUBLE_EQ(after.y(), before.y());  // value unchanged
}

TEST_F(DatastoreCurveAdapterTest, BoundsCacheInvalidatesOnTopicCommittedAndDataCleared) {
  const QRectF before = adapter_->boundingRect();
  EXPECT_DOUBLE_EQ(before.right(), 7.0);

  appendMoreRows(10, 13);
  EXPECT_DOUBLE_EQ(adapter_->boundingRect().right(), 7.0);

  adapter_->onTopicCommitted();
  EXPECT_DOUBLE_EQ(adapter_->boundingRect().right(), 10.0);
  EXPECT_DOUBLE_EQ(adapter_->boundingRect().bottom(), 22.0);

  ASSERT_NE(session_.dataEngine().getTopicStorage(topic_id_), nullptr);
  session_.dataEngine().getTopicStorage(topic_id_)->clearChunks();
  adapter_->onDataCleared();
  EXPECT_EQ(adapter_->size(), 0U);
  EXPECT_FALSE(adapter_->boundingRect().isValid());
}

TEST_F(DatastoreCurveAdapterTest, VisibleYRangeUsesSeriesSamples) {
  const auto full_chunk = adapter_->visibleYRange(Range<double>{.min = 2.0, .max = 5.0});
  ASSERT_TRUE(full_chunk.has_value());
  EXPECT_DOUBLE_EQ(full_chunk->min, 14.0);
  EXPECT_DOUBLE_EQ(full_chunk->max, 17.0);

  const auto partial = adapter_->visibleYRange(Range<double>{.min = 3.0, .max = 4.0});
  ASSERT_TRUE(partial.has_value());
  EXPECT_DOUBLE_EQ(partial->min, 16.0);
  EXPECT_DOUBLE_EQ(partial->max, 16.0);
}

TEST_F(DatastoreCurveAdapterTest, TopicCommitGrowsIndexedSize) {
  ASSERT_EQ(adapter_->size(), 9U);

  appendMoreRows(10, 13);
  adapter_->onTopicCommitted();

  EXPECT_EQ(adapter_->size(), 12U);
  EXPECT_DOUBLE_EQ(adapter_->sample(11).x(), 10.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(11).y(), 22.0);
}

TEST_F(DatastoreCurveAdapterTest, CrossChunkBoundaryGuardsIncludeAdjacentChunks) {
  // Chunks are sealed every max_chunk_rows=4 rows. With 10 rows we have
  // chunk[0]=rows 0..3, chunk[1]=rows 4..7, chunk[2]=rows 8..9.
  // Window cuts strictly between rows 3 and 4 (display 1.5..1.6 → raw 3.5..3.6 sec):
  // - row 3 (raw 3 sec, display 1.0) is in chunk[0], outside the window
  // - row 4 (raw 4 sec, display 2.0) is in chunk[1], outside the window
  // The cross-chunk guard rule must surface row 3 as the left guard and row 4
  // as the right guard so the segment crossing the gap still renders.
  adapter_->setRectOfInterest(QRectF(QPointF(1.5, -1.0), QPointF(1.6, 1.0)));

  ASSERT_EQ(adapter_->size(), 2U);
  EXPECT_DOUBLE_EQ(adapter_->sample(0).x(), 1.0);  // row 3 from chunk[0] (last row)
  EXPECT_DOUBLE_EQ(adapter_->sample(0).y(), 13.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(1).x(), 2.0);  // row 4 from chunk[1] (first row)
  EXPECT_DOUBLE_EQ(adapter_->sample(1).y(), 14.0);
}

TEST_F(DatastoreCurveAdapterTest, InOrderAbsorbNeverDuplicatesTheBoundarySample) {
  // Build a window whose newest cached sample sits EXACTLY at the built-range
  // edge (no data exists beyond it), then deliver commit notifications. The
  // in-order absorb path must not re-read the boundary sample (an inverted
  // cursor range would be silently swapped and replay it), and a later
  // in-order append must land exactly once.
  adapter_->setRectOfInterest(QRectF(QPointF(5.0, -1.0), QPointF(6.0, 1.0)));
  const std::size_t initial = adapter_->size();

  adapter_->onTopicCommitted();  // notifications with no new data
  adapter_->onTopicCommitted();
  EXPECT_EQ(adapter_->size(), initial);

  appendMoreRows(10, 11);        // one strictly in-order sample (raw 10 s)
  adapter_->onTopicCommitted();  // absorbed into the cache, beyond the slice
  adapter_->onTopicCommitted();
  EXPECT_EQ(adapter_->size(), initial);

  // Slide the window over the absorbed region while STAYING inside the built
  // range (raw 8.5..9.0 vs built end 9), so this is served by re-narrowing the
  // absorbed cache — duplicates hidden by the previous slice surface here as
  // extra points; a range escaping the build would rebuild and mask them.
  adapter_->setRectOfInterest(QRectF(QPointF(6.5, -1.0), QPointF(7.0, 1.0)));
  ASSERT_EQ(adapter_->size(), 3U);
  EXPECT_DOUBLE_EQ(adapter_->sample(0).x(), 6.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(0).y(), 18.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(1).x(), 7.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(1).y(), 19.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(2).x(), 8.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(2).y(), 20.0);
}

TEST_F(DatastoreCurveAdapterTest, BoundaryGuardsSurviveOutOfOrderChunks) {
  // A late commit stores a chunk that lies entirely BEFORE the first chunk in
  // time (out-of-order ingest keeps chunks in commit order). A window in the
  // gap must still find its left guard inside that late chunk — a
  // chronological-chunk assumption stops at the first stored chunk and drops it.
  appendMoreRows(-5, -2);  // raw -5..-3 sec → display -7..-5
  adapter_->onTopicCommitted();

  // Window strictly inside the gap between display -5 (i=-3) and -2 (i=0).
  adapter_->setRectOfInterest(QRectF(QPointF(-4.5, -1.0), QPointF(-3.5, 1.0)));

  ASSERT_EQ(adapter_->size(), 2U);
  EXPECT_DOUBLE_EQ(adapter_->sample(0).x(), -5.0);  // left guard from the late chunk
  EXPECT_DOUBLE_EQ(adapter_->sample(0).y(), 7.0);
  EXPECT_DOUBLE_EQ(adapter_->sample(1).x(), -2.0);  // right guard from the first chunk
  EXPECT_DOUBLE_EQ(adapter_->sample(1).y(), 10.0);
}

TEST_F(DatastoreCurveAdapterTest, SampleFromTimeReturnsLatestAtPoint) {
  // Display time 4.5 sec ↔ raw 6.5 sec. latestAt picks row 6 (raw 6 sec, value 16).
  const auto hit = adapter_->sampleFromTime(4.5);
  ASSERT_TRUE(hit.has_value());
  EXPECT_DOUBLE_EQ(hit->x(), 4.0);
  EXPECT_DOUBLE_EQ(hit->y(), 16.0);

  // Before the first sample → no row at-or-before this time → nullopt.
  EXPECT_FALSE(adapter_->sampleFromTime(-100.0).has_value());
}

TEST(PointSeriesXYTest, SameTopicPairsRowsByIndex) {
  SessionManager session;
  auto dataset_or = session.dataEngine().createDataset(DatasetDescriptor{.source_name = "xy"});
  ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();

  auto writer = session.dataEngine().createWriter();
  auto schema_or = writer.registerSchema(
      "xy",
      makeStruct("xy", {makePrimitive("x", PrimitiveType::kFloat64), makePrimitive("y", PrimitiveType::kFloat64)}));
  ASSERT_TRUE(schema_or.has_value()) << schema_or.error();

  TopicDescriptor descriptor;
  descriptor.name = "/xy";
  descriptor.schema_id = *schema_or;
  descriptor.max_chunk_rows = 2;
  auto topic_or = writer.registerTopic(*dataset_or, descriptor);
  ASSERT_TRUE(topic_or.has_value()) << topic_or.error();

  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(writer.beginRow(*topic_or, static_cast<Timestamp>(i) * kNs).has_value());
    writer.set(*topic_or, 0, 10.0 + static_cast<double>(i));
    writer.set(*topic_or, 1, 100.0 + static_cast<double>(i));
    ASSERT_TRUE(writer.finishRow(*topic_or).has_value());
  }
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());

  CurveDescriptor x_descriptor{
      .name = "/xy/x",
      .topic_id = *topic_or,
      .dataset_id = *dataset_or,
      .column_index = 0,
      .field_path = "x",
  };
  CurveDescriptor y_descriptor{
      .name = "/xy/y",
      .topic_id = *topic_or,
      .dataset_id = *dataset_or,
      .column_index = 1,
      .field_path = "y",
  };

  PointSeriesXY series(&session, x_descriptor, y_descriptor);
  ASSERT_EQ(series.size(), 3U);
  EXPECT_DOUBLE_EQ(series.sample(1).x(), 11.0);
  EXPECT_DOUBLE_EQ(series.sample(1).y(), 101.0);

  const QRectF bounds = series.boundingRect();
  EXPECT_DOUBLE_EQ(bounds.left(), 10.0);
  EXPECT_DOUBLE_EQ(bounds.right(), 12.0);
  EXPECT_DOUBLE_EQ(bounds.top(), 100.0);
  EXPECT_DOUBLE_EQ(bounds.bottom(), 102.0);
}

TEST(PointSeriesXYTest, DifferentTopicsPairOnlyExactTimestampsAndInvalidateOnCommit) {
  SessionManager session;
  auto dataset_or = session.dataEngine().createDataset(DatasetDescriptor{.source_name = "xy"});
  ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();

  auto writer = session.dataEngine().createWriter();
  auto schema_or = writer.registerSchema("scalar", makePrimitive("value", PrimitiveType::kFloat64));
  ASSERT_TRUE(schema_or.has_value()) << schema_or.error();

  TopicDescriptor x_topic_descriptor;
  x_topic_descriptor.name = "/x";
  x_topic_descriptor.schema_id = *schema_or;
  x_topic_descriptor.max_chunk_rows = 2;
  auto x_topic_or = writer.registerTopic(*dataset_or, x_topic_descriptor);
  ASSERT_TRUE(x_topic_or.has_value()) << x_topic_or.error();

  TopicDescriptor y_topic_descriptor;
  y_topic_descriptor.name = "/y";
  y_topic_descriptor.schema_id = *schema_or;
  y_topic_descriptor.max_chunk_rows = 2;
  auto y_topic_or = writer.registerTopic(*dataset_or, y_topic_descriptor);
  ASSERT_TRUE(y_topic_or.has_value()) << y_topic_or.error();

  auto append = [&writer](TopicId topic_id, int t, double value) {
    ASSERT_TRUE(writer.beginRow(topic_id, static_cast<Timestamp>(t) * kNs).has_value());
    writer.set(topic_id, 0, value);
    ASSERT_TRUE(writer.finishRow(topic_id).has_value());
  };
  append(*x_topic_or, 0, 1.0);
  append(*x_topic_or, 1, 2.0);
  append(*x_topic_or, 2, 3.0);
  append(*x_topic_or, 3, 4.0);
  append(*y_topic_or, 1, 10.0);
  append(*y_topic_or, 3, 30.0);
  append(*y_topic_or, 4, 40.0);
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());

  CurveDescriptor x_descriptor{
      .name = "/x/value",
      .topic_id = *x_topic_or,
      .dataset_id = *dataset_or,
      .column_index = 0,
      .field_path = "value",
  };
  CurveDescriptor y_descriptor{
      .name = "/y/value",
      .topic_id = *y_topic_or,
      .dataset_id = *dataset_or,
      .column_index = 0,
      .field_path = "value",
  };

  PointSeriesXY series(&session, x_descriptor, y_descriptor);
  ASSERT_EQ(series.size(), 2U);
  EXPECT_DOUBLE_EQ(series.sample(0).x(), 2.0);
  EXPECT_DOUBLE_EQ(series.sample(0).y(), 10.0);
  EXPECT_DOUBLE_EQ(series.sample(1).x(), 4.0);
  EXPECT_DOUBLE_EQ(series.sample(1).y(), 30.0);

  auto second_writer = session.dataEngine().createWriter();
  auto append_second = [&second_writer](TopicId topic_id, int t, double value) {
    ASSERT_TRUE(second_writer.beginRow(topic_id, static_cast<Timestamp>(t) * kNs).has_value());
    second_writer.set(topic_id, 0, value);
    ASSERT_TRUE(second_writer.finishRow(topic_id).has_value());
  };
  append_second(*x_topic_or, 5, 6.0);
  append_second(*y_topic_or, 5, 60.0);
  EXPECT_FALSE(session.commitChunks(second_writer.flushAll()).empty());

  EXPECT_EQ(series.size(), 2U);
  series.onTopicCommitted();
  ASSERT_EQ(series.size(), 3U);
  EXPECT_DOUBLE_EQ(series.sample(2).x(), 6.0);
  EXPECT_DOUBLE_EQ(series.sample(2).y(), 60.0);
}

TEST(PointSeriesXYTest, SampleFromTimePicksAtOrBeforeSample) {
  SessionManager session;
  auto dataset_or = session.dataEngine().createDataset(DatasetDescriptor{.source_name = "xy"});
  ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();

  auto writer = session.dataEngine().createWriter();
  auto schema_or = writer.registerSchema(
      "xy",
      makeStruct("xy", {makePrimitive("x", PrimitiveType::kFloat64), makePrimitive("y", PrimitiveType::kFloat64)}));
  ASSERT_TRUE(schema_or.has_value()) << schema_or.error();

  TopicDescriptor descriptor;
  descriptor.name = "/xy";
  descriptor.schema_id = *schema_or;
  descriptor.max_chunk_rows = 2;  // rows straddle a chunk boundary
  auto topic_or = writer.registerTopic(*dataset_or, descriptor);
  ASSERT_TRUE(topic_or.has_value()) << topic_or.error();

  for (int i = 0; i < 4; ++i) {  // (t=i, x=10+i, y=100+i), no display offset -> display == raw
    ASSERT_TRUE(writer.beginRow(*topic_or, static_cast<Timestamp>(i) * kNs).has_value());
    writer.set(*topic_or, 0, 10.0 + static_cast<double>(i));
    writer.set(*topic_or, 1, 100.0 + static_cast<double>(i));
    ASSERT_TRUE(writer.finishRow(*topic_or).has_value());
  }
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());

  CurveDescriptor x_descriptor{
      .name = "/xy/x", .topic_id = *topic_or, .dataset_id = *dataset_or, .column_index = 0, .field_path = "x"};
  CurveDescriptor y_descriptor{
      .name = "/xy/y", .topic_id = *topic_or, .dataset_id = *dataset_or, .column_index = 1, .field_path = "y"};

  PointSeriesXY series(&session, x_descriptor, y_descriptor);

  const auto exact = series.sampleFromTime(2.0);  // lands exactly on t=2
  ASSERT_TRUE(exact.has_value());
  EXPECT_DOUBLE_EQ(exact->x(), 12.0);
  EXPECT_DOUBLE_EQ(exact->y(), 102.0);

  const auto between = series.sampleFromTime(1.5);  // most recent at-or-before -> t=1
  ASSERT_TRUE(between.has_value());
  EXPECT_DOUBLE_EQ(between->x(), 11.0);
  EXPECT_DOUBLE_EQ(between->y(), 101.0);

  const auto after_end = series.sampleFromTime(99.0);  // clamps to the last pair
  ASSERT_TRUE(after_end.has_value());
  EXPECT_DOUBLE_EQ(after_end->x(), 13.0);
  EXPECT_DOUBLE_EQ(after_end->y(), 103.0);

  EXPECT_FALSE(series.sampleFromTime(-1.0).has_value());  // before the first sample
}

TEST(PointSeriesXYTest, SampleFromTimeMapsThroughDisplayOffset) {
  SessionManager session;
  auto domain_or = session.dataEngine().createTimeDomain("xy");
  ASSERT_TRUE(domain_or.has_value()) << domain_or.error();
  session.dataEngine().setDisplayOffset(*domain_or, 2 * kNs);  // display = (raw - 2s)

  auto dataset_or =
      session.dataEngine().createDataset(DatasetDescriptor{.source_name = "xy", .time_domain_id = *domain_or});
  ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();

  auto writer = session.dataEngine().createWriter();
  auto schema_or = writer.registerSchema(
      "xy",
      makeStruct("xy", {makePrimitive("x", PrimitiveType::kFloat64), makePrimitive("y", PrimitiveType::kFloat64)}));
  ASSERT_TRUE(schema_or.has_value()) << schema_or.error();

  TopicDescriptor descriptor;
  descriptor.name = "/xy";
  descriptor.schema_id = *schema_or;
  descriptor.max_chunk_rows = 4;
  auto topic_or = writer.registerTopic(*dataset_or, descriptor);
  ASSERT_TRUE(topic_or.has_value()) << topic_or.error();

  for (int i = 0; i < 4; ++i) {  // raw t=i, x=10+i, y=100+i
    ASSERT_TRUE(writer.beginRow(*topic_or, static_cast<Timestamp>(i) * kNs).has_value());
    writer.set(*topic_or, 0, 10.0 + static_cast<double>(i));
    writer.set(*topic_or, 1, 100.0 + static_cast<double>(i));
    ASSERT_TRUE(writer.finishRow(*topic_or).has_value());
  }
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());

  CurveDescriptor x_descriptor{
      .name = "/xy/x", .topic_id = *topic_or, .dataset_id = *dataset_or, .column_index = 0, .field_path = "x"};
  CurveDescriptor y_descriptor{
      .name = "/xy/y", .topic_id = *topic_or, .dataset_id = *dataset_or, .column_index = 1, .field_path = "y"};

  PointSeriesXY series(&session, x_descriptor, y_descriptor);

  const auto hit = series.sampleFromTime(0.0);  // display 0.0 ↔ raw 2s -> pair at t=2
  ASSERT_TRUE(hit.has_value());
  EXPECT_DOUBLE_EQ(hit->x(), 12.0);
  EXPECT_DOUBLE_EQ(hit->y(), 102.0);

  const auto first = series.sampleFromTime(-2.0);  // display -2.0 ↔ raw 0s -> first pair
  ASSERT_TRUE(first.has_value());
  EXPECT_DOUBLE_EQ(first->x(), 10.0);

  EXPECT_FALSE(series.sampleFromTime(-3.0).has_value());  // display -3.0 ↔ raw -1s -> before first
}

TEST_F(DatastoreCurveAdapterTest, MissingTopicReturnsNanWithoutCrashing) {
  // CurveDescriptor pointing at a topic that doesn't exist in the engine —
  // simulates the "topic deleted mid-paint" path: getTopicStorage() → nullptr.
  CurveDescriptor stale = descriptor_;
  stale.topic_id = static_cast<TopicId>(topic_id_ + 9999U);
  DatastoreCurveAdapter dangling(&session_, stale);

  EXPECT_EQ(dangling.size(), 0U);
  const QPointF s = dangling.sample(0);
  EXPECT_TRUE(std::isnan(s.y()));
  EXPECT_FALSE(dangling.boundingRect().isValid());
  EXPECT_FALSE(dangling.visibleYRange(Range<double>{.min = 0.0, .max = 10.0}).has_value());
  EXPECT_FALSE(dangling.sampleFromTime(0.0).has_value());
}

}  // namespace
}  // namespace PJ
