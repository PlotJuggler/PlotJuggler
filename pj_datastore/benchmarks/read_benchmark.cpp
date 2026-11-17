// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "benchmark/benchmark.h"
#include "pj_base/type_tree.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/column_buffer.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/topic_storage.hpp"

namespace PJ {
namespace {

constexpr int kPointCount = 100'000;
constexpr uint32_t kChunkSize = 1024;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

ColumnDescriptor makeDescriptor(PrimitiveType type, std::string path) {
  return ColumnDescriptor{/*field_id=*/0, type, std::move(path)};
}

// Build a single sealed TopicChunk with kPointCount rows and one column.
template <typename SetFn>
TopicChunk buildTypedChunk(PrimitiveType type, SetFn set_fn) {
  std::vector<ColumnDescriptor> cols = {makeDescriptor(type, "value")};
  TopicChunkBuilder builder(/*topic_id=*/1, /*schema_id=*/1, cols, kPointCount);

  for (int i = 0; i < kPointCount; ++i) {
    builder.beginRow(static_cast<Timestamp>(i));
    set_fn(builder, 0, i);
    builder.finishRow();
  }
  return builder.seal();
}

// Build a deque of sealed chunks (kChunkSize rows each) for cursor tests.
template <typename SetFn>
std::deque<TopicChunk> buildChunkedDeque(PrimitiveType type, SetFn set_fn) {
  std::deque<TopicChunk> chunks;
  std::vector<ColumnDescriptor> cols = {makeDescriptor(type, "value")};

  TopicChunkBuilder* builder = nullptr;
  std::unique_ptr<TopicChunkBuilder> owned;

  for (int i = 0; i < kPointCount; ++i) {
    if (!builder || builder->isFull()) {
      if (builder) {
        chunks.push_back(builder->seal());
      }
      owned = std::make_unique<TopicChunkBuilder>(
          /*topic_id=*/1, /*schema_id=*/1, cols, kChunkSize);
      builder = owned.get();
    }
    builder->beginRow(static_cast<Timestamp>(i));
    set_fn(*builder, 0, i);
    builder->finishRow();
  }
  if (builder && builder->rowCount() > 0) {
    chunks.push_back(builder->seal());
  }
  return chunks;
}

template <typename T, typename F>
std::deque<std::pair<Timestamp, T>> buildDequeData(F value_fn) {
  std::deque<std::pair<Timestamp, T>> data;
  for (int i = 0; i < kPointCount; ++i) {
    data.emplace_back(static_cast<Timestamp>(i), value_fn(i));
  }
  return data;
}

// A column's encoded bytes per row.
double encodedBytesPerRow(const TopicChunk& chunk, std::size_t col) {
  switch (chunk.columnEncoding(col)) {
    case EncodingType::kConstant: {
      const auto& enc = std::get<encoding::ConstantEncoded>(chunk.columns[col].data);
      return static_cast<double>(enc.value_size) / chunk.stats.row_count;
    }
    case EncodingType::kFrameOfReference: {
      const auto& enc = std::get<encoding::FrameOfReferenceEncoded>(chunk.columns[col].data);
      return static_cast<double>(enc.offsets.size()) / chunk.stats.row_count;
    }
    case EncodingType::kDictionary: {
      const auto& dict = std::get<encoding::DictionaryEncoded>(chunk.columns[col].data);
      std::size_t dict_bytes = 0;
      for (const auto& s : dict.dictionary) {
        dict_bytes += s.size();
      }
      return static_cast<double>(dict.indices.size() + dict_bytes) / chunk.stats.row_count;
    }
    default:
      return static_cast<double>(std::get<RawBuffer>(chunk.columns[col].data).size()) / chunk.stats.row_count;
  }
}

// ===========================================================================
// Tier 1 — TypedColumnBuffer (raw unencoded read)
// ===========================================================================

void BM_ColumnBuffer_ReadFloat32(benchmark::State& state) {
  static TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kFloat32, "value"));
  static bool init = [&] {
    for (int i = 0; i < kPointCount; ++i) {
      buf.appendFloat32(static_cast<float>(i) * 0.1f);
    }
    return true;
  }();
  (void)init;

  for (auto _ : state) {
    double sum = 0.0;
    for (int i = 0; i < kPointCount; ++i) {
      sum += buf.readFloat32(static_cast<std::size_t>(i));
    }
    benchmark::DoNotOptimize(sum);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
}

void BM_ColumnBuffer_ReadInt64(benchmark::State& state) {
  static TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kInt64, "value"));
  static bool init = [&] {
    for (int i = 0; i < kPointCount; ++i) {
      buf.appendInt64(static_cast<int64_t>(i));
    }
    return true;
  }();
  (void)init;

  for (auto _ : state) {
    int64_t sum = 0;
    for (int i = 0; i < kPointCount; ++i) {
      sum += buf.readInt64(static_cast<std::size_t>(i));
    }
    benchmark::DoNotOptimize(sum);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
}

void BM_ColumnBuffer_ReadString(benchmark::State& state) {
  static const std::vector<std::string> k_enum_values = {"IDLE", "RUN", "WARN", "ERROR"};
  static TypedColumnBuffer buf(makeDescriptor(PrimitiveType::kString, "state"));
  static bool init = [&] {
    for (int i = 0; i < kPointCount; ++i) {
      buf.appendString(k_enum_values[static_cast<std::size_t>(i) % 4]);
    }
    return true;
  }();
  (void)init;

  for (auto _ : state) {
    std::size_t total_len = 0;
    for (int i = 0; i < kPointCount; ++i) {
      total_len += buf.readString(static_cast<std::size_t>(i)).size();
    }
    benchmark::DoNotOptimize(total_len);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
}

BENCHMARK(BM_ColumnBuffer_ReadFloat32);
BENCHMARK(BM_ColumnBuffer_ReadInt64);
BENCHMARK(BM_ColumnBuffer_ReadString);

// ===========================================================================
// Tier 2 — Sealed chunk decode (encoded read)
// ===========================================================================

void BM_Chunk_ReadFloat32(benchmark::State& state) {
  static TopicChunk chunk = buildTypedChunk(PrimitiveType::kFloat32, [](TopicChunkBuilder& b, std::size_t col, int i) {
    b.set(col, static_cast<float>(i) * 0.1f);
  });

  for (auto _ : state) {
    double sum = 0.0;
    for (int i = 0; i < kPointCount; ++i) {
      sum += chunk.readNumericAsDouble(0, static_cast<std::size_t>(i));
    }
    benchmark::DoNotOptimize(sum);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
  state.counters["bytes_per_row"] = encodedBytesPerRow(chunk, 0);
}

void BM_Chunk_ReadInt64(benchmark::State& state) {
  static TopicChunk chunk = buildTypedChunk(
      PrimitiveType::kInt64, [](TopicChunkBuilder& b, std::size_t col, int i) { b.set(col, static_cast<int64_t>(i)); });

  for (auto _ : state) {
    double sum = 0.0;
    for (int i = 0; i < kPointCount; ++i) {
      sum += chunk.readNumericAsDouble(0, static_cast<std::size_t>(i));
    }
    benchmark::DoNotOptimize(sum);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
  state.counters["bytes_per_row"] = encodedBytesPerRow(chunk, 0);
}

void BM_Chunk_ReadString(benchmark::State& state) {
  static const std::vector<std::string> k_enum_values = {"IDLE", "RUN", "WARN", "ERROR"};
  static TopicChunk chunk = buildTypedChunk(PrimitiveType::kString, [](TopicChunkBuilder& b, std::size_t col, int i) {
    b.set(col, std::string_view(k_enum_values[static_cast<std::size_t>(i) % 4]));
  });

  for (auto _ : state) {
    std::size_t total_len = 0;
    for (int i = 0; i < kPointCount; ++i) {
      total_len += chunk.readString(0, static_cast<std::size_t>(i)).size();
    }
    benchmark::DoNotOptimize(total_len);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
  state.counters["bytes_per_row"] = encodedBytesPerRow(chunk, 0);
}

BENCHMARK(BM_Chunk_ReadFloat32);
BENCHMARK(BM_Chunk_ReadInt64);
BENCHMARK(BM_Chunk_ReadString);

// ===========================================================================
// Tier 2b — Bulk read (switch-once, tight inner loop)
// ===========================================================================

void BM_Chunk_BulkReadFloat32(benchmark::State& state) {
  static TopicChunk chunk = buildTypedChunk(PrimitiveType::kFloat32, [](TopicChunkBuilder& b, std::size_t col, int i) {
    b.set(col, static_cast<float>(i) * 0.1f);
  });

  std::vector<double> buf(kPointCount);
  for (auto _ : state) {
    chunk.readColumnAsDoubles(0, Span<double>(buf), 0);
    benchmark::DoNotOptimize(buf.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
  state.counters["bytes_per_row"] = encodedBytesPerRow(chunk, 0);
}

void BM_Chunk_BulkReadInt64(benchmark::State& state) {
  static TopicChunk chunk = buildTypedChunk(
      PrimitiveType::kInt64, [](TopicChunkBuilder& b, std::size_t col, int i) { b.set(col, static_cast<int64_t>(i)); });

  std::vector<double> buf(kPointCount);
  for (auto _ : state) {
    chunk.readColumnAsDoubles(0, Span<double>(buf), 0);
    benchmark::DoNotOptimize(buf.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
  state.counters["bytes_per_row"] = encodedBytesPerRow(chunk, 0);
}

BENCHMARK(BM_Chunk_BulkReadFloat32);
BENCHMARK(BM_Chunk_BulkReadInt64);

// ===========================================================================
// Tier 2c — FOR and Constant compressed benchmarks
// ===========================================================================

void BM_Chunk_ReadInt64_FOR(benchmark::State& state) {
  // Values 0..99, so frame-of-reference with 1-byte offsets.
  static TopicChunk chunk = buildTypedChunk(PrimitiveType::kInt64, [](TopicChunkBuilder& b, std::size_t col, int i) {
    b.set(col, static_cast<int64_t>(i % 100));
  });

  for (auto _ : state) {
    double sum = 0.0;
    for (int i = 0; i < kPointCount; ++i) {
      sum += chunk.readNumericAsDouble(0, static_cast<std::size_t>(i));
    }
    benchmark::DoNotOptimize(sum);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
  state.counters["bytes_per_row"] = encodedBytesPerRow(chunk, 0);
  state.counters["encoding"] = static_cast<double>(chunk.columnEncoding(0));
}

void BM_Chunk_BulkReadInt64_FOR(benchmark::State& state) {
  static TopicChunk chunk = buildTypedChunk(PrimitiveType::kInt64, [](TopicChunkBuilder& b, std::size_t col, int i) {
    b.set(col, static_cast<int64_t>(i % 100));
  });

  std::vector<double> buf(kPointCount);
  for (auto _ : state) {
    chunk.readColumnAsDoubles(0, Span<double>(buf), 0);
    benchmark::DoNotOptimize(buf.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
  state.counters["bytes_per_row"] = encodedBytesPerRow(chunk, 0);
  state.counters["encoding"] = static_cast<double>(chunk.columnEncoding(0));
}

void BM_Chunk_ReadInt32_Constant(benchmark::State& state) {
  static TopicChunk chunk =
      buildTypedChunk(PrimitiveType::kInt32, [](TopicChunkBuilder& b, std::size_t col, int /*i*/) { b.set(col, 42); });

  for (auto _ : state) {
    double sum = 0.0;
    for (int i = 0; i < kPointCount; ++i) {
      sum += chunk.readNumericAsDouble(0, static_cast<std::size_t>(i));
    }
    benchmark::DoNotOptimize(sum);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
  state.counters["bytes_per_row"] = encodedBytesPerRow(chunk, 0);
  state.counters["encoding"] = static_cast<double>(chunk.columnEncoding(0));
}

void BM_Chunk_BulkReadInt32_Constant(benchmark::State& state) {
  static TopicChunk chunk =
      buildTypedChunk(PrimitiveType::kInt32, [](TopicChunkBuilder& b, std::size_t col, int /*i*/) { b.set(col, 42); });

  std::vector<double> buf(kPointCount);
  for (auto _ : state) {
    chunk.readColumnAsDoubles(0, Span<double>(buf), 0);
    benchmark::DoNotOptimize(buf.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
  state.counters["bytes_per_row"] = encodedBytesPerRow(chunk, 0);
  state.counters["encoding"] = static_cast<double>(chunk.columnEncoding(0));
}

BENCHMARK(BM_Chunk_ReadInt64_FOR);
BENCHMARK(BM_Chunk_BulkReadInt64_FOR);
BENCHMARK(BM_Chunk_ReadInt32_Constant);
BENCHMARK(BM_Chunk_BulkReadInt32_Constant);

// ===========================================================================
// Tier 3 — RangeCursor iteration (cross-chunk)
// ===========================================================================

void BM_Cursor_ReadFloat32(benchmark::State& state) {
  static std::deque<TopicChunk> chunks = buildChunkedDeque(
      PrimitiveType::kFloat32,
      [](TopicChunkBuilder& b, std::size_t col, int i) { b.set(col, static_cast<float>(i) * 0.1f); });

  for (auto _ : state) {
    double sum = 0.0;
    auto cursor = rangeQuery(chunks, 0, kPointCount - 1);
    cursor.forEach([&](const SampleRow& row) { sum += row.chunk->readNumericAsDouble(0, row.row_index); });
    benchmark::DoNotOptimize(sum);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
}

void BM_Cursor_ReadInt64(benchmark::State& state) {
  static std::deque<TopicChunk> chunks = buildChunkedDeque(
      PrimitiveType::kInt64, [](TopicChunkBuilder& b, std::size_t col, int i) { b.set(col, static_cast<int64_t>(i)); });

  for (auto _ : state) {
    double sum = 0.0;
    auto cursor = rangeQuery(chunks, 0, kPointCount - 1);
    cursor.forEach([&](const SampleRow& row) { sum += row.chunk->readNumericAsDouble(0, row.row_index); });
    benchmark::DoNotOptimize(sum);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
}

void BM_Cursor_ReadString(benchmark::State& state) {
  static const std::vector<std::string> k_enum_values = {"IDLE", "RUN", "WARN", "ERROR"};
  static std::deque<TopicChunk> chunks =
      buildChunkedDeque(PrimitiveType::kString, [](TopicChunkBuilder& b, std::size_t col, int i) {
        b.set(col, std::string_view(k_enum_values[static_cast<std::size_t>(i) % 4]));
      });

  for (auto _ : state) {
    std::size_t total_len = 0;
    auto cursor = rangeQuery(chunks, 0, kPointCount - 1);
    cursor.forEach([&](const SampleRow& row) { total_len += row.chunk->readString(0, row.row_index).size(); });
    benchmark::DoNotOptimize(total_len);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
}

BENCHMARK(BM_Cursor_ReadFloat32);
BENCHMARK(BM_Cursor_ReadInt64);
BENCHMARK(BM_Cursor_ReadString);

// ===========================================================================
// Tier 3b — Chunk-at-a-time cursor + bulk read
// ===========================================================================

void BM_Cursor_ChunkAtATime_Float32(benchmark::State& state) {
  static std::deque<TopicChunk> chunks = buildChunkedDeque(
      PrimitiveType::kFloat32,
      [](TopicChunkBuilder& b, std::size_t col, int i) { b.set(col, static_cast<float>(i) * 0.1f); });

  std::vector<double> buf(kChunkSize);
  for (auto _ : state) {
    double sum = 0.0;
    auto cursor = rangeQuery(chunks, 0, kPointCount - 1);
    cursor.forEachChunk([&](const ChunkRowRange& range) {
      const std::size_t n = range.row_end - range.row_start;
      range.chunk->readColumnAsDoubles(0, Span<double>(buf.data(), n), range.row_start);
      for (std::size_t i = 0; i < n; ++i) {
        sum += buf[i];
      }
    });
    benchmark::DoNotOptimize(sum);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
}

void BM_Cursor_ChunkAtATime_Int64(benchmark::State& state) {
  static std::deque<TopicChunk> chunks = buildChunkedDeque(
      PrimitiveType::kInt64, [](TopicChunkBuilder& b, std::size_t col, int i) { b.set(col, static_cast<int64_t>(i)); });

  std::vector<double> buf(kChunkSize);
  for (auto _ : state) {
    double sum = 0.0;
    auto cursor = rangeQuery(chunks, 0, kPointCount - 1);
    cursor.forEachChunk([&](const ChunkRowRange& range) {
      const std::size_t n = range.row_end - range.row_start;
      range.chunk->readColumnAsDoubles(0, Span<double>(buf.data(), n), range.row_start);
      for (std::size_t i = 0; i < n; ++i) {
        sum += buf[i];
      }
    });
    benchmark::DoNotOptimize(sum);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
}

BENCHMARK(BM_Cursor_ChunkAtATime_Float32);
BENCHMARK(BM_Cursor_ChunkAtATime_Int64);

// ===========================================================================
// Tier 4 — Deque baseline (reference overhead)
// ===========================================================================

void BM_Deque_ReadFloat(benchmark::State& state) {
  static std::deque<std::pair<Timestamp, float>> data =
      buildDequeData<float>([](int i) { return static_cast<float>(i) * 0.1f; });

  for (auto _ : state) {
    double sum = 0.0;
    for (auto& [ts, v] : data) {
      benchmark::DoNotOptimize(ts);
      sum += v;
    }
    benchmark::DoNotOptimize(sum);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
}

void BM_Deque_ReadInt64(benchmark::State& state) {
  static std::deque<std::pair<Timestamp, int64_t>> data =
      buildDequeData<int64_t>([](int i) { return static_cast<int64_t>(i); });

  for (auto _ : state) {
    int64_t sum = 0;
    for (auto& [ts, v] : data) {
      benchmark::DoNotOptimize(ts);
      sum += v;
    }
    benchmark::DoNotOptimize(sum);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
}

void BM_Deque_ReadString(benchmark::State& state) {
  static const std::vector<std::string> k_enum_values = {"IDLE", "RUN", "WARN", "ERROR"};
  static std::deque<std::pair<Timestamp, std::string>> data = buildDequeData<std::string>(
      [](int i) { return k_enum_values[static_cast<std::size_t>(i) % k_enum_values.size()]; });

  for (auto _ : state) {
    std::size_t total_len = 0;
    for (auto& [ts, v] : data) {
      benchmark::DoNotOptimize(ts);
      total_len += v.size();
    }
    benchmark::DoNotOptimize(total_len);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kPointCount);
}

// ---------------------------------------------------------------------------
// TopicStorage::metadata() over a growing chunk deque; the app calls it per topic on ingest ticks. Parameter: the
// retained chunk count.
// ---------------------------------------------------------------------------

void BM_TopicMetadata(benchmark::State& state) {
  const auto chunk_count = static_cast<std::size_t>(state.range(0));
  TopicStorage storage(/*topic_id=*/1, TopicDescriptor{.name = "bench", .schema_id = 1});
  std::vector<ColumnDescriptor> cols = {makeDescriptor(PrimitiveType::kFloat64, "value")};
  for (std::size_t c = 0; c < chunk_count; ++c) {
    TopicChunkBuilder builder(/*topic_id=*/1, /*schema_id=*/1, cols, kChunkSize);
    for (uint32_t i = 0; i < kChunkSize; ++i) {
      builder.beginRow(static_cast<Timestamp>(c * kChunkSize + i));
      builder.set(0, static_cast<double>(i));
      builder.finishRow();
    }
    if (!storage.appendSealedChunk(builder.seal())) {
      state.SkipWithError("appendSealedChunk failed");
      return;
    }
  }

  for (auto _ : state) {
    TopicMetadata meta = storage.metadata();
    benchmark::DoNotOptimize(meta);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

BENCHMARK(BM_TopicMetadata)->Arg(100)->Arg(1000)->Arg(5000);

BENCHMARK(BM_Deque_ReadFloat);
BENCHMARK(BM_Deque_ReadInt64);
BENCHMARK(BM_Deque_ReadString);

}  // namespace
}  // namespace PJ

BENCHMARK_MAIN();
