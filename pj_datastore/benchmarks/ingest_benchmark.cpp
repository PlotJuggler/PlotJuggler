// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "pj_base/dataset.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/column_buffer.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/plugin_data_host.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {
namespace {

constexpr int kRowCount = 100'000;
constexpr uint32_t kChunkSize = 16'384;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

ColumnDescriptor makeDescriptor(PrimitiveType type, std::string path) {
  return ColumnDescriptor{/*field_id=*/0, type, std::move(path)};
}

// Pre-generate test data arrays (allocated once, reused across iterations).
struct TestData {
  std::vector<Timestamp> timestamps;
  std::vector<float> floats;
  std::vector<double> doubles;
  std::vector<int32_t> int32s;
  std::vector<int64_t> int64s;

  TestData() {
    timestamps.resize(kRowCount);
    floats.resize(kRowCount);
    doubles.resize(kRowCount);
    int32s.resize(kRowCount);
    int64s.resize(kRowCount);
    for (int i = 0; i < kRowCount; ++i) {
      timestamps[static_cast<std::size_t>(i)] = static_cast<Timestamp>(i);
      floats[static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.1f;
      doubles[static_cast<std::size_t>(i)] = static_cast<double>(i) * 0.1;
      int32s[static_cast<std::size_t>(i)] = i % 100;
      int64s[static_cast<std::size_t>(i)] = static_cast<int64_t>(i);
    }
  }
};

static const TestData& getTestData() {
  static TestData data;
  return data;
}

// ===========================================================================
// TopicChunkBuilder: row-at-a-time vs bulk (single column)
// ===========================================================================

void BM_Builder_RowAtATime_Float32(benchmark::State& state) {
  const auto& data = getTestData();
  std::vector<ColumnDescriptor> cols = {makeDescriptor(PrimitiveType::kFloat32, "value")};

  for (auto _ : state) {
    TopicChunkBuilder builder(1, 1, cols, kRowCount);
    for (int i = 0; i < kRowCount; ++i) {
      builder.beginRow(data.timestamps[static_cast<std::size_t>(i)]);
      builder.set(0, data.floats[static_cast<std::size_t>(i)]);
      builder.finishRow();
    }
    auto chunk = builder.seal();
    benchmark::DoNotOptimize(chunk);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kRowCount);
}

void BM_Builder_Bulk_Float32(benchmark::State& state) {
  const auto& data = getTestData();
  std::vector<ColumnDescriptor> cols = {makeDescriptor(PrimitiveType::kFloat32, "value")};

  for (auto _ : state) {
    TopicChunkBuilder builder(1, 1, cols, kRowCount);
    builder.appendTimestamps(data.timestamps);
    builder.appendColumn<float>(0, data.floats);
    builder.finishBulkAppend();
    auto chunk = builder.seal();
    benchmark::DoNotOptimize(chunk);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kRowCount);
}

BENCHMARK(BM_Builder_RowAtATime_Float32);
BENCHMARK(BM_Builder_Bulk_Float32);

// ===========================================================================
// TopicChunkBuilder: row-at-a-time vs bulk (int64)
// ===========================================================================

void BM_Builder_RowAtATime_Int64(benchmark::State& state) {
  const auto& data = getTestData();
  std::vector<ColumnDescriptor> cols = {makeDescriptor(PrimitiveType::kInt64, "value")};

  for (auto _ : state) {
    TopicChunkBuilder builder(1, 1, cols, kRowCount);
    for (int i = 0; i < kRowCount; ++i) {
      builder.beginRow(data.timestamps[static_cast<std::size_t>(i)]);
      builder.set(0, data.int64s[static_cast<std::size_t>(i)]);
      builder.finishRow();
    }
    auto chunk = builder.seal();
    benchmark::DoNotOptimize(chunk);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kRowCount);
}

void BM_Builder_Bulk_Int64(benchmark::State& state) {
  const auto& data = getTestData();
  std::vector<ColumnDescriptor> cols = {makeDescriptor(PrimitiveType::kInt64, "value")};

  for (auto _ : state) {
    TopicChunkBuilder builder(1, 1, cols, kRowCount);
    builder.appendTimestamps(data.timestamps);
    builder.appendColumn<int64_t>(0, data.int64s);
    builder.finishBulkAppend();
    auto chunk = builder.seal();
    benchmark::DoNotOptimize(chunk);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kRowCount);
}

BENCHMARK(BM_Builder_RowAtATime_Int64);
BENCHMARK(BM_Builder_Bulk_Int64);

// ===========================================================================
// Multi-column: row-at-a-time vs bulk (10 float32 columns)
// ===========================================================================

constexpr int kMultiColCount = 10;

void BM_Builder_RowAtATime_MultiCol(benchmark::State& state) {
  const auto& data = getTestData();
  std::vector<ColumnDescriptor> cols;
  for (int c = 0; c < kMultiColCount; ++c) {
    cols.push_back(makeDescriptor(PrimitiveType::kFloat32, "col_" + std::to_string(c)));
  }

  for (auto _ : state) {
    TopicChunkBuilder builder(1, 1, cols, kRowCount);
    for (int i = 0; i < kRowCount; ++i) {
      builder.beginRow(data.timestamps[static_cast<std::size_t>(i)]);
      for (int c = 0; c < kMultiColCount; ++c) {
        builder.set(static_cast<std::size_t>(c), data.floats[static_cast<std::size_t>(i)]);
      }
      builder.finishRow();
    }
    auto chunk = builder.seal();
    benchmark::DoNotOptimize(chunk);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kRowCount * kMultiColCount);
}

void BM_Builder_Bulk_MultiCol(benchmark::State& state) {
  const auto& data = getTestData();
  std::vector<ColumnDescriptor> cols;
  for (int c = 0; c < kMultiColCount; ++c) {
    cols.push_back(makeDescriptor(PrimitiveType::kFloat32, "col_" + std::to_string(c)));
  }

  for (auto _ : state) {
    TopicChunkBuilder builder(1, 1, cols, kRowCount);
    builder.appendTimestamps(data.timestamps);
    for (int c = 0; c < kMultiColCount; ++c) {
      builder.appendColumn<float>(static_cast<std::size_t>(c), data.floats);
    }
    builder.finishBulkAppend();
    auto chunk = builder.seal();
    benchmark::DoNotOptimize(chunk);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kRowCount * kMultiColCount);
}

BENCHMARK(BM_Builder_RowAtATime_MultiCol);
BENCHMARK(BM_Builder_Bulk_MultiCol);

// ===========================================================================
// DataWriter: row-at-a-time vs appendColumns (end-to-end through engine)
// ===========================================================================

void BM_Writer_RowAtATime_Float32(benchmark::State& state) {
  const auto& data = getTestData();

  // Build type tree once outside the loop (structure is cheap, reusable)
  auto value_field = makePrimitive("value", PrimitiveType::kFloat32);
  auto root = makeStruct("data", {value_field});

  for (auto _ : state) {
    DataEngine engine;
    auto ds_id = *engine.createDataset(DatasetDescriptor{.source_name = "bench", .time_domain_id = 0});
    auto writer = engine.createWriter();
    auto schema_id = *writer.registerSchema("bench_schema", root);
    TopicDescriptor desc;
    desc.name = "bench_topic";
    desc.schema_id = schema_id;
    auto topic_id = *writer.registerTopic(ds_id, desc);

    for (int i = 0; i < kRowCount; ++i) {
      (void)writer.beginRow(topic_id, data.timestamps[static_cast<std::size_t>(i)]);
      writer.set(topic_id, 0, data.floats[static_cast<std::size_t>(i)]);
      (void)writer.finishRow(topic_id);
    }
    auto chunks = writer.flush(topic_id);
    benchmark::DoNotOptimize(chunks);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kRowCount);
}

void BM_Writer_AppendColumns_Float32(benchmark::State& state) {
  const auto& data = getTestData();

  auto value_field = makePrimitive("value", PrimitiveType::kFloat32);
  auto root = makeStruct("data", {value_field});

  for (auto _ : state) {
    DataEngine engine;
    auto ds_id = *engine.createDataset(DatasetDescriptor{.source_name = "bench", .time_domain_id = 0});
    auto writer = engine.createWriter();
    auto schema_id = *writer.registerSchema("bench_schema", root);
    TopicDescriptor desc;
    desc.name = "bench_topic";
    desc.schema_id = schema_id;
    auto topic_id = *writer.registerTopic(ds_id, desc);

    std::vector<ColumnData> columns = {ColumnData::float32(0, data.floats)};
    (void)writer.appendColumns(topic_id, data.timestamps, columns);
    auto chunks = writer.flush(topic_id);
    benchmark::DoNotOptimize(chunks);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kRowCount);
}

BENCHMARK(BM_Writer_RowAtATime_Float32);
BENCHMARK(BM_Writer_AppendColumns_Float32);

// ===========================================================================
// Plugin source write host: bound rows across many topics
// ===========================================================================

// The shape of a large flight log (e.g. ULog: ~1k topics x tens of fields): the
// host resolves every field of every record in a map keyed by (topic, field),
// so this is sensitive to how well that key hashes at realistic cardinality.
void BM_SourceHost_BoundAppend_ManyTopics(benchmark::State& state) {
  constexpr int kTopics = 1000;
  constexpr int kFields = 50;
  constexpr int kRowsPerTopic = 20;

  for (auto _ : state) {
    DataEngine engine;
    ObjectStore object_store;
    DatastoreToolboxHost toolbox_impl{engine, object_store};
    sdk::ToolboxHostView toolbox{toolbox_impl.raw()};
    DatastoreSourceWriteHost write_impl(engine, *toolbox.createDataSource("bench"));
    sdk::SourceWriteHostView writer(write_impl.raw());

    std::vector<sdk::TopicHandle> topics;
    std::vector<std::vector<sdk::BoundFieldValue>> rows(kTopics);
    for (int t = 0; t < kTopics; ++t) {
      topics.push_back(*writer.ensureTopic("topic_" + std::to_string(t)));
      for (int f = 0; f < kFields; ++f) {
        auto field = *writer.ensureField(topics.back(), "field_" + std::to_string(f), PrimitiveType::kFloat32);
        rows[static_cast<std::size_t>(t)].push_back({.field = field, .value = static_cast<float>(f)});
      }
    }
    for (int r = 0; r < kRowsPerTopic; ++r) {
      for (std::size_t t = 0; t < topics.size(); ++t) {
        (void)writer.appendBoundRecord(
            topics[t], Timestamp{r}, Span<const sdk::BoundFieldValue>(rows[t].data(), rows[t].size()));
      }
    }
    write_impl.flushPending();
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kTopics * kFields * kRowsPerTopic);
}

BENCHMARK(BM_SourceHost_BoundAppend_ManyTopics)->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace PJ

BENCHMARK_MAIN();
