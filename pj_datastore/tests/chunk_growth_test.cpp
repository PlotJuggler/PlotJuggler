// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A topic's last chunk grows in place (TopicStorage::appendRows): how often a writer commits must not change what is
// stored. The reference is the same rows committed at once, as sealed chunks.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_datastore/writer.hpp"

namespace PJ {
namespace {

constexpr uint32_t kMaxRows = 64;

using Cell = std::variant<std::monostate, double, float, int32_t, int64_t, uint64_t, bool, std::string>;

struct Row {
  Timestamp timestamp = 0;
  std::vector<Cell> cells;  // monostate: not set (a null)
};

struct Column {
  const char* name;
  PrimitiveType type;
};

// Every storage kind, plus the columns that pick each encoding: a constant, whole-number floats (frame of reference),
// a column no row sets, a column few rows set.
const std::vector<Column> kColumns = {
    {"f64", PrimitiveType::kFloat64},
    {"f32", PrimitiveType::kFloat32},
    {"i32", PrimitiveType::kInt32},
    {"i64", PrimitiveType::kInt64},
    {"u64", PrimitiveType::kUint64},
    {"flag", PrimitiveType::kBool},
    {"text", PrimitiveType::kString},
    {"constant", PrimitiveType::kFloat64},
    {"whole", PrimitiveType::kFloat64},
    {"never_set", PrimitiveType::kFloat64},
    {"sparse", PrimitiveType::kInt64},
    {"constant_flag", PrimitiveType::kBool},
    {"constant_text", PrimitiveType::kString},
};

struct Fixture {
  DataEngine engine;
  DataWriter writer = engine.createWriter();
  TopicId topic = 0;

  explicit Fixture(std::size_t columns = kColumns.size()) {
    const auto dataset = engine.createDataset(DatasetDescriptor{.source_name = "growth", .time_domain_id = 0});
    EXPECT_TRUE(dataset.has_value());
    const auto registered =
        writer.registerTopic(*dataset, TopicDescriptor{.name = "topic", .max_chunk_rows = kMaxRows});
    EXPECT_TRUE(registered.has_value());
    topic = *registered;
    for (std::size_t c = 0; c < columns; ++c) {
      EXPECT_TRUE(writer.ensureColumn(topic, kColumns[c].name, kColumns[c].type).has_value());
    }
  }

  void write(const Row& row) {
    ASSERT_TRUE(writer.beginRow(topic, row.timestamp).has_value());
    for (std::size_t c = 0; c < row.cells.size(); ++c) {
      std::visit(
          [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
              // not set: finishRow() makes it a null
            } else if constexpr (std::is_same_v<T, std::string>) {
              writer.set<std::string_view>(topic, c, value);
            } else {
              writer.set<T>(topic, c, value);
            }
          },
          row.cells[c]);
    }
    ASSERT_TRUE(writer.finishRow(topic).has_value());
  }

  void commitGrowing() {
    (void)writer.commitRows();
  }

  void commitSealed() {
    engine.commitChunks(writer.flushAll());
  }

  [[nodiscard]] TopicStorage& storage() {
    return *engine.getTopicStorage(topic);
  }
};

std::vector<Row> makeRows(std::size_t count, uint32_t seed, Timestamp start = 0) {
  std::mt19937 rng(seed);
  const auto pick = [&rng](uint32_t n) { return static_cast<uint32_t>(rng() % n); };
  const std::vector<std::string> words = {"", "idle", "moving", "a longer state name than the small buffer", "x"};
  std::vector<Row> rows;
  Timestamp t = start;
  for (std::size_t i = 0; i < count; ++i) {
    t += pick(3);  // repeats a timestamp often
    Row row{.timestamp = t, .cells = std::vector<Cell>(kColumns.size())};
    if (pick(10) != 0) {
      static constexpr double kSpecial[] = {std::numeric_limits<double>::quiet_NaN(), -0.0, 1e300};
      const uint32_t kind = pick(12);
      row.cells[0] = kind < 3 ? kSpecial[kind] : static_cast<double>(pick(1000)) / 7.0;
    }
    if (pick(10) != 0) {
      row.cells[1] = static_cast<float>(pick(1000)) / 3.0F;
    }
    if (pick(10) != 0) {
      row.cells[2] = static_cast<int32_t>(pick(2000)) - 1000;
    }
    if (pick(10) != 0) {
      row.cells[3] = pick(20) == 0 ? std::numeric_limits<int64_t>::min() + pick(5)
                                   : (int64_t{1} << 60) + static_cast<int64_t>(pick(50));  // past 2^53
    }
    if (pick(10) != 0) {
      row.cells[4] = std::numeric_limits<uint64_t>::max() - pick(50);
    }
    if (pick(10) != 0) {
      row.cells[5] = pick(2) == 0;
    }
    if (pick(10) != 0) {
      row.cells[6] = words[pick(static_cast<uint32_t>(words.size()))] + (pick(4) == 0 ? std::to_string(pick(300)) : "");
    }
    if (pick(10) != 0) {
      row.cells[7] = 42.5;
    }
    if (pick(10) != 0) {
      row.cells[8] = static_cast<double>(pick(200)) - 100.0;
    }
    if (pick(50) == 0) {
      row.cells[10] = static_cast<int64_t>(pick(9));
    }
    if (pick(10) != 0) {
      row.cells[11] = true;
    }
    if (pick(10) != 0) {
      row.cells[12] = std::string("always");
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

[[nodiscard]] bool sameDouble(double a, double b) {
  return std::memcmp(&a, &b, sizeof(a)) == 0 || (std::isnan(a) && std::isnan(b));
}

// The values and the stats readers use. `same_storage` also compares what only seal() decides: each column's encoding
// and whether it has a bitmap.
void expectSameChunk(const TopicChunk& actual, const TopicChunk& expected, bool same_storage) {
  ASSERT_EQ(actual.stats.row_count, expected.stats.row_count);
  ASSERT_EQ(actual.timestamps, expected.timestamps);
  EXPECT_EQ(actual.stats.t_min, expected.stats.t_min);
  EXPECT_EQ(actual.stats.t_max, expected.stats.t_max);
  ASSERT_EQ(actual.columns.size(), expected.columns.size());
  for (std::size_t c = 0; c < actual.columns.size(); ++c) {
    SCOPED_TRACE("column " + actual.columns[c].descriptor->field_path);
    const StorageKind kind = storageKindOf(actual.columns[c].descriptor->logical_type);
    for (std::size_t r = 0; r < actual.stats.row_count; ++r) {
      ASSERT_EQ(actual.isNull(c, r), expected.isNull(c, r)) << "row " << r;
      if (actual.isNull(c, r)) {
        continue;
      }
      switch (kind) {
        case StorageKind::kString:
          ASSERT_EQ(actual.readString(c, r), expected.readString(c, r)) << "row " << r;
          break;
        case StorageKind::kBool:
          ASSERT_EQ(actual.readBool(c, r), expected.readBool(c, r)) << "row " << r;
          break;
        case StorageKind::kUint64:
          ASSERT_EQ(actual.readNumericAsUint64(c, r), expected.readNumericAsUint64(c, r)) << "row " << r;
          break;
        case StorageKind::kInt32:
        case StorageKind::kInt64:
          ASSERT_EQ(actual.readNumericAsInt64(c, r), expected.readNumericAsInt64(c, r)) << "row " << r;
          break;
        default:
          ASSERT_TRUE(sameDouble(actual.readNumericAsDouble(c, r), expected.readNumericAsDouble(c, r))) << "row " << r;
          break;
      }
    }
    const ColumnStats& a = actual.stats.column_stats[c];
    const ColumnStats& e = expected.stats.column_stats[c];
    EXPECT_EQ(a.null_count, e.null_count);
    if (kind == StorageKind::kFloat32 || kind == StorageKind::kFloat64) {
      EXPECT_EQ(a.all_integral, e.all_integral);
    }
    if (same_storage) {
      EXPECT_EQ(actual.columnEncoding(c), expected.columnEncoding(c));
      EXPECT_EQ(actual.columns[c].validity_bitmap.has_value(), expected.columns[c].validity_bitmap.has_value());
    }
  }
}

// The range a reader takes from a column's stats instead of scanning its rows (SeriesReader::bounds): none unless
// both extremes are finite.
std::optional<std::pair<double, double>> usableRange(const ColumnStats& stats) {
  if (!stats.min_value.has_value() || !stats.max_value.has_value() || !std::isfinite(*stats.min_value) ||
      !std::isfinite(*stats.max_value) || *stats.min_value > *stats.max_value) {
    return std::nullopt;
  }
  return std::pair{*stats.min_value, *stats.max_value};
}

// A chunk's per-column range against its own values: a NaN is in neither extreme, whichever path built the chunk.
void expectRangesMatchValues(const TopicChunk& chunk) {
  for (std::size_t c = 0; c < chunk.columns.size(); ++c) {
    const StorageKind kind = storageKindOf(chunk.columns[c].descriptor->logical_type);
    if (kind == StorageKind::kString) {
      continue;
    }
    std::optional<std::pair<double, double>> range;
    for (std::size_t r = 0; r < chunk.stats.row_count; ++r) {
      if (chunk.isNull(c, r)) {
        continue;
      }
      double v = 0;
      if (kind == StorageKind::kBool) {
        v = chunk.readBool(c, r) ? 1.0 : 0.0;
      } else if (kind == StorageKind::kUint64) {
        v = static_cast<double>(chunk.readNumericAsUint64(c, r));
      } else if (kind == StorageKind::kInt32 || kind == StorageKind::kInt64) {
        v = static_cast<double>(chunk.readNumericAsInt64(c, r));
      } else {
        v = chunk.readNumericAsDouble(c, r);
      }
      if (std::isnan(v)) {
        continue;
      }
      range = range ? std::pair{std::min(range->first, v), std::max(range->second, v)} : std::pair{v, v};
    }
    const auto stored = usableRange(chunk.stats.column_stats[c]);
    ASSERT_EQ(stored.has_value(), range.has_value()) << chunk.columns[c].descriptor->field_path;
    if (range.has_value()) {
      EXPECT_EQ(stored->first, range->first) << chunk.columns[c].descriptor->field_path;
      EXPECT_EQ(stored->second, range->second) << chunk.columns[c].descriptor->field_path;
    }
  }
}

// The storage's cached aggregates against a scan of its chunks.
void expectAggregatesMatchChunks(const TopicStorage& storage) {
  uint64_t rows = 0;
  uint64_t bytes = 0;
  Timestamp t_min = std::numeric_limits<Timestamp>::max();
  Timestamp t_max = std::numeric_limits<Timestamp>::min();
  std::map<FieldId, std::pair<bool, bool>> whole;  // field -> (has a value, every chunk whole)
  for (const TopicChunk& chunk : storage.sealedChunks()) {
    rows += chunk.stats.row_count;
    bytes += chunk.stats.encoded_byte_size;
    t_min = std::min(t_min, chunk.stats.t_min);
    t_max = std::max(t_max, chunk.stats.t_max);
    for (std::size_t c = 0; c < chunk.columns.size(); ++c) {
      const StorageKind kind = storageKindOf(chunk.columns[c].descriptor->logical_type);
      if ((kind != StorageKind::kFloat32 && kind != StorageKind::kFloat64) ||
          chunk.stats.column_stats[c].null_count >= chunk.stats.row_count) {
        continue;
      }
      auto [it, inserted] = whole.try_emplace(chunk.columns[c].descriptor->field_id, true, true);
      it->second.second = it->second.second && chunk.stats.column_stats[c].all_integral;
    }
  }
  const TopicMetadata metadata = storage.metadata();
  EXPECT_EQ(metadata.total_row_count, rows);
  EXPECT_EQ(metadata.total_byte_size, bytes);
  if (rows > 0) {
    EXPECT_EQ(metadata.time_range_min, std::max(t_min, storage.retentionFloor()));
    EXPECT_EQ(metadata.time_range_max, t_max);
  }
  for (const ColumnDescriptor& column : storage.columnDescriptors()) {
    const auto found = whole.find(column.field_id);
    const std::optional<bool> expected =
        found == whole.end() ? std::nullopt : std::optional<bool>(found->second.second);
    EXPECT_EQ(storage.wholeNumbersOnly(column.field_id), expected) << column.field_path;
  }
}

// Whether every value of float column `c` among `rows` is a whole number; nullopt when none has a value.
std::optional<bool> wholeInRows(const std::vector<Row>& rows, std::size_t count, std::size_t c) {
  std::optional<bool> whole;
  for (std::size_t i = 0; i < count; ++i) {
    const Cell& cell = rows[i].cells[c];
    double value = 0;
    if (const auto* d = std::get_if<double>(&cell)) {
      value = *d;
    } else if (const auto* f = std::get_if<float>(&cell)) {
      value = *f;
    } else {
      continue;
    }
    whole = whole.value_or(true) && value == std::trunc(value) && std::isfinite(value) && std::abs(value) < 0x1p63;
  }
  return whole;
}

class ChunkGrowthProperty : public ::testing::TestWithParam<uint32_t> {};

TEST_P(ChunkGrowthProperty, CommitCadenceDoesNotChangeWhatIsStored) {
  const uint32_t seed = GetParam();
  const std::vector<Row> rows = makeRows(1000, seed);

  Fixture reference;
  for (const Row& row : rows) {
    reference.write(row);
  }
  reference.commitSealed();
  const auto& expected = reference.storage().sealedChunks();

  Fixture growing;
  std::mt19937 rng(seed ^ 0x9E3779B9U);
  // (chunk id, row) -> timestamp and first column: a row never changes under its chunk's id.
  std::map<std::pair<ChunkId, std::size_t>, std::pair<Timestamp, double>> seen;
  std::size_t written = 0;
  while (written < rows.size()) {
    // Mostly a few rows a commit, sometimes more than a chunk.
    const std::size_t step = 1 + (rng() % 10 == 0 ? rng() % (std::size_t{3} * kMaxRows) : rng() % 9);
    const std::size_t end = std::min(rows.size(), written + step);
    for (; written < end; ++written) {
      growing.write(rows[written]);
    }
    const uint64_t rows_before = growing.storage().metadata().total_row_count;
    (void)growing.storage().wholeNumbersSignature();  // keeps the whole-number aggregate live, as the catalog does
    growing.commitGrowing();

    const TopicStorage& storage = growing.storage();
    ASSERT_EQ(storage.metadata().total_row_count, written);
    ASSERT_GT(storage.metadata().total_row_count, rows_before);
    expectAggregatesMatchChunks(storage);
    for (std::size_t c : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{8}, std::size_t{9}}) {
      EXPECT_EQ(storage.wholeNumbersOnly(static_cast<FieldId>(c)), wholeInRows(rows, written, c)) << kColumns[c].name;
    }
    const auto& chunks = storage.sealedChunks();
    ChunkId previous_id = 0;
    for (std::size_t k = 0; k < chunks.size(); ++k) {
      const TopicChunk& chunk = chunks[k];
      ASSERT_GT(chunk.id, previous_id) << "chunk ids ascend";
      previous_id = chunk.id;
      // Cut by rows alone: every chunk but the last is full.
      ASSERT_EQ(chunk.stats.row_count, k + 1 < chunks.size() ? kMaxRows : written - k * kMaxRows);
      expectRangesMatchValues(chunk);
      for (std::size_t r = 0; r < chunk.stats.row_count; ++r) {
        const std::pair<Timestamp, double> value{
            chunk.timestamps[r], chunk.isNull(0, r) ? -1.0 : chunk.readNumericAsDouble(0, r)};
        const auto [it, inserted] = seen.try_emplace({chunk.id, r}, value);
        ASSERT_EQ(it->second.first, value.first) << "row " << r << " of chunk " << chunk.id << " moved";
        ASSERT_TRUE(sameDouble(it->second.second, value.second)) << "row " << r << " of chunk " << chunk.id;
      }
    }
  }

  const auto& actual = growing.storage().sealedChunks();
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t k = 0; k < actual.size(); ++k) {
    SCOPED_TRACE("chunk " + std::to_string(k));
    // Full chunks were re-encoded when they filled; the last one is still in growable form.
    expectSameChunk(actual[k], expected[k], /*same_storage=*/k + 1 < actual.size());
  }
  growing.storage().compactLastChunk();
  expectSameChunk(actual.back(), expected.back(), /*same_storage=*/true);
  expectRangesMatchValues(actual.back());
  expectAggregatesMatchChunks(growing.storage());
  EXPECT_EQ(growing.storage().metadata().total_byte_size, reference.storage().metadata().total_byte_size);
}

TEST_P(ChunkGrowthProperty, RowsStampedOutOfOrderAreAllKeptInSortedFullChunks) {
  const uint32_t seed = GetParam();
  std::mt19937 rng(seed ^ 0xA5A5A5A5U);
  // Several publishers on one topic, each stamping its own rows: locally out of order, by up to a few rows.
  std::vector<Row> rows = makeRows(700, seed);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    rows[i].timestamp = static_cast<Timestamp>(i) * 10 + static_cast<Timestamp>(rng() % 60);
  }

  Fixture growing;
  std::map<std::pair<ChunkId, std::size_t>, Timestamp> seen;
  std::size_t written = 0;
  while (written < rows.size()) {
    const std::size_t end = std::min(rows.size(), written + 1 + rng() % 12);
    for (; written < end; ++written) {
      growing.write(rows[written]);
    }
    (void)growing.storage().wholeNumbersSignature();
    growing.commitGrowing();
    expectAggregatesMatchChunks(growing.storage());
    for (const TopicChunk& chunk : growing.storage().sealedChunks()) {
      ASSERT_TRUE(std::is_sorted(chunk.timestamps.begin(), chunk.timestamps.end()));
      for (std::size_t r = 0; r < chunk.stats.row_count; ++r) {
        // A row that moved did so under a new chunk id.
        const auto [it, inserted] = seen.try_emplace({chunk.id, r}, chunk.timestamps[r]);
        ASSERT_EQ(it->second, chunk.timestamps[r]) << "row " << r << " of chunk " << chunk.id << " moved";
      }
    }
  }

  // Every row is there, once: timestamp and the int32 column (set in most rows) against the model.
  std::multimap<Timestamp, std::optional<int32_t>> expected;
  for (const Row& row : rows) {
    const auto* value = std::get_if<int32_t>(&row.cells[2]);
    expected.emplace(row.timestamp, value != nullptr ? std::optional<int32_t>(*value) : std::nullopt);
  }
  std::multimap<Timestamp, std::optional<int32_t>> actual;
  std::size_t full_chunks = 0;
  const auto& chunks = growing.storage().sealedChunks();
  for (const TopicChunk& chunk : chunks) {
    full_chunks += chunk.stats.row_count == kMaxRows ? 1U : 0U;
    expectRangesMatchValues(chunk);
    for (std::size_t r = 0; r < chunk.stats.row_count; ++r) {
      actual.emplace(
          chunk.timestamps[r], chunk.isNull(2, r)
                                   ? std::nullopt
                                   : std::optional<int32_t>(static_cast<int32_t>(chunk.readNumericAsInt64(2, r))));
    }
  }
  const auto sorted = [](const std::multimap<Timestamp, std::optional<int32_t>>& rows_by_time) {
    std::vector<std::pair<Timestamp, std::optional<int32_t>>> out(rows_by_time.begin(), rows_by_time.end());
    std::sort(out.begin(), out.end());
    return out;
  };
  EXPECT_EQ(sorted(actual), sorted(expected));
  // Late rows are merged into the chunk that has room for them, so all but the last chunk fill.
  EXPECT_EQ(chunks.size(), (rows.size() + kMaxRows - 1) / kMaxRows);
  EXPECT_GE(full_chunks + 1, chunks.size());

  // The reader merges the chunks, which overlap where one filled between two late rows.
  Timestamp previous = std::numeric_limits<Timestamp>::min();
  std::size_t read = 0;
  const DataReader reader = growing.engine.createReader();
  auto cursor = reader.rangeQuery(
      QueryRange{
          .topic_id = growing.topic,
          .t_min = std::numeric_limits<Timestamp>::min(),
          .t_max = std::numeric_limits<Timestamp>::max()});
  ASSERT_TRUE(cursor.has_value());
  cursor->forEach([&](const SampleRow& row) {
    EXPECT_GE(row.timestamp, previous);
    previous = row.timestamp;
    ++read;
  });
  EXPECT_EQ(read, rows.size());
}

INSTANTIATE_TEST_SUITE_P(Seeds, ChunkGrowthProperty, ::testing::Range(1U, 13U));

std::vector<std::pair<Timestamp, double>> readAll(DataEngine& engine, TopicId topic) {
  std::vector<std::pair<Timestamp, double>> out;
  const DataReader reader = engine.createReader();
  auto cursor = reader.rangeQuery(
      QueryRange{
          .topic_id = topic,
          .t_min = std::numeric_limits<Timestamp>::min(),
          .t_max = std::numeric_limits<Timestamp>::max()});
  EXPECT_TRUE(cursor.has_value());
  if (cursor.has_value()) {
    cursor->forEach([&out](const SampleRow& row) {
      out.emplace_back(row.timestamp, row.chunk->readNumericAsDouble(0, row.row_index));
    });
  }
  return out;
}

Row numberRow(Timestamp t, double value) {
  return Row{.timestamp = t, .cells = {Cell{value}}};
}

TEST(ChunkGrowthTest, RowsOutOfOrderWithinACommitAreSortedBeforeTheyLand) {
  Fixture fixture(1);
  for (const Timestamp t : {10, 30, 20}) {
    fixture.write(numberRow(t, static_cast<double>(t)));
  }
  fixture.commitGrowing();
  for (const Timestamp t : {50, 40, 30}) {  // 30 ties with the chunk's newest row: it goes after it
    fixture.write(numberRow(t, static_cast<double>(t) + 0.5));
  }
  fixture.commitGrowing();

  const auto& chunks = fixture.storage().sealedChunks();
  ASSERT_EQ(chunks.size(), 1U);
  EXPECT_EQ(chunks[0].timestamps, (std::vector<Timestamp>{10, 20, 30, 30, 40, 50}));
  EXPECT_EQ(
      readAll(fixture.engine, fixture.topic),
      (std::vector<std::pair<Timestamp, double>>{
          {10, 10.0}, {20, 20.0}, {30, 30.0}, {30, 30.5}, {40, 40.5}, {50, 50.5}}));
}

TEST(ChunkGrowthTest, RowsOlderThanTheLastChunkAreMergedIntoItUnderANewId) {
  Fixture fixture(1);
  for (const Timestamp t : {10, 20, 30}) {
    fixture.write(numberRow(t, static_cast<double>(t)));
  }
  fixture.commitGrowing();
  const ChunkId first_id = fixture.storage().sealedChunks().front().id;
  const uint64_t generation = fixture.storage().seriesGeneration();

  fixture.write(numberRow(25, 25.0));  // goes before row 30
  fixture.write(numberRow(20, 20.5));  // ties with a row of the chunk: after it
  (void)fixture.storage().wholeNumbersSignature();
  fixture.commitGrowing();

  const auto& chunks = fixture.storage().sealedChunks();
  ASSERT_EQ(chunks.size(), 1U);
  EXPECT_GT(chunks[0].id, first_id) << "rows moved, so the chunk is not the one readers remember";
  EXPECT_EQ(chunks[0].timestamps, (std::vector<Timestamp>{10, 20, 20, 25, 30}));
  EXPECT_GT(fixture.storage().seriesGeneration(), generation) << "readers must rebuild: rows landed behind theirs";
  EXPECT_EQ(
      readAll(fixture.engine, fixture.topic),
      (std::vector<std::pair<Timestamp, double>>{{10, 10.0}, {20, 20.0}, {20, 20.5}, {25, 25.0}, {30, 30.0}}));
  expectAggregatesMatchChunks(fixture.storage());
  expectRangesMatchValues(chunks[0]);

  // Rows in order extend the merged chunk as any other.
  const ChunkId merged_id = chunks[0].id;
  fixture.write(numberRow(31, 31.0));
  fixture.commitGrowing();
  ASSERT_EQ(chunks.size(), 1U);
  EXPECT_EQ(chunks[0].id, merged_id);
  EXPECT_EQ(chunks[0].stats.row_count, 6U);
}

TEST(ChunkGrowthTest, LateRowsThatBringANewColumnAreMergedToo) {
  Fixture fixture(1);
  fixture.write(numberRow(10, 1.0));
  fixture.write(numberRow(30, 3.0));
  fixture.commitGrowing();
  ASSERT_TRUE(fixture.writer.ensureColumn(fixture.topic, "text", PrimitiveType::kString).has_value());
  fixture.write(Row{.timestamp = 20, .cells = {Cell{2.0}, Cell{std::string("late")}}});
  fixture.commitGrowing();

  const auto& chunks = fixture.storage().sealedChunks();
  ASSERT_EQ(chunks.size(), 1U);
  const TopicChunk& chunk = chunks.front();
  ASSERT_EQ(chunk.columns.size(), 2U);
  EXPECT_EQ(chunk.timestamps, (std::vector<Timestamp>{10, 20, 30}));
  EXPECT_TRUE(chunk.isNull(1, 0));
  EXPECT_EQ(chunk.readString(1, 1), "late");
  EXPECT_TRUE(chunk.isNull(1, 2));
  EXPECT_EQ(chunk.readNumericAsDouble(0, 1), 2.0);
  expectAggregatesMatchChunks(fixture.storage());
}

TEST(ChunkGrowthTest, LateRowsThatDoNotFitTheLastChunkStartAChunkOfTheirOwn) {
  Fixture fixture(1);
  // A second writer holds four rows before the first commits: its builder was sized when the topic was empty.
  DataWriter late = fixture.engine.createWriter();
  for (const Timestamp t : {1, 2, 3, 4}) {
    ASSERT_TRUE(late.beginRow(fixture.topic, t).has_value());
    late.set<double>(fixture.topic, 0, 2.0);
    ASSERT_TRUE(late.finishRow(fixture.topic).has_value());
  }
  for (Timestamp t = 0; t < static_cast<Timestamp>(kMaxRows) - 2; ++t) {
    fixture.write(numberRow(100 + t, 1.0));
  }
  fixture.commitGrowing();
  const ChunkId first_id = fixture.storage().sealedChunks().front().id;

  (void)late.commitRows();  // older than every row there, and two rows more than the chunk has room for

  const auto& chunks = fixture.storage().sealedChunks();
  ASSERT_EQ(chunks.size(), 2U);
  EXPECT_EQ(chunks[0].id, first_id) << "untouched";
  EXPECT_EQ(chunks[0].stats.row_count, kMaxRows - 2);
  EXPECT_EQ(chunks[1].timestamps, (std::vector<Timestamp>{1, 2, 3, 4}));
  EXPECT_EQ(readAll(fixture.engine, fixture.topic).front().first, 1);
  expectAggregatesMatchChunks(fixture.storage());
}

TEST(ChunkGrowthTest, OnlyRowsStrictlyAfterEverySampleKeepTheSeriesGeneration) {
  Fixture fixture(1);
  fixture.write(numberRow(10, 1.0));
  fixture.commitGrowing();
  const uint64_t generation = fixture.storage().seriesGeneration();

  fixture.write(numberRow(11, 2.0));
  fixture.commitGrowing();
  EXPECT_EQ(fixture.storage().seriesGeneration(), generation);

  fixture.write(numberRow(11, 3.0));  // ties with the newest sample
  fixture.commitGrowing();
  EXPECT_GT(fixture.storage().seriesGeneration(), generation);
  EXPECT_EQ(fixture.storage().sealedChunks().size(), 1U);
}

TEST(ChunkGrowthTest, TheLastChunkGainsAColumnAddedLater) {
  Fixture fixture(1);
  fixture.write(numberRow(10, 1.0));
  fixture.commitGrowing();
  const ChunkId id = fixture.storage().sealedChunks().front().id;
  ASSERT_TRUE(fixture.writer.ensureColumn(fixture.topic, "f32", PrimitiveType::kFloat32).has_value());
  fixture.write(Row{.timestamp = 20, .cells = {Cell{2.0}, Cell{3.0F}}});
  fixture.commitGrowing();

  const auto& chunks = fixture.storage().sealedChunks();
  ASSERT_EQ(chunks.size(), 1U) << "a new column does not cut the chunk";
  const TopicChunk& chunk = chunks.front();
  EXPECT_EQ(chunk.id, id);
  ASSERT_EQ(chunk.columns.size(), 2U);
  EXPECT_EQ(chunk.readNumericAsDouble(0, 0), 1.0);
  EXPECT_TRUE(chunk.isNull(1, 0)) << "the row written before the column existed";
  EXPECT_FALSE(chunk.isNull(1, 1));
  EXPECT_EQ(chunk.readNumericAsDouble(1, 1), 3.0);
  EXPECT_EQ(chunk.stats.column_stats[1].null_count, 1U);
  expectAggregatesMatchChunks(fixture.storage());
  expectRangesMatchValues(chunk);
}

TEST(ChunkGrowthTest, RowsOfAWriterWithAnOlderLayoutTakeNullsInTheNewColumns) {
  Fixture fixture(1);
  DataWriter other = fixture.engine.createWriter();
  // The other writer has its builder, with one column, before the first adds a second.
  ASSERT_TRUE(other.beginRow(fixture.topic, 30).has_value());
  other.set<double>(fixture.topic, 0, 7.0);
  ASSERT_TRUE(other.finishRow(fixture.topic).has_value());

  ASSERT_TRUE(fixture.writer.ensureColumn(fixture.topic, "text", PrimitiveType::kString).has_value());
  fixture.write(Row{.timestamp = 10, .cells = {Cell{1.0}, Cell{std::string("first")}}});
  fixture.commitGrowing();
  (void)other.commitRows();

  const auto& chunks = fixture.storage().sealedChunks();
  ASSERT_EQ(chunks.size(), 1U);
  const TopicChunk& chunk = chunks.front();
  ASSERT_EQ(chunk.columns.size(), 2U);
  ASSERT_EQ(chunk.stats.row_count, 2U);
  EXPECT_EQ(chunk.readString(1, 0), "first");
  EXPECT_EQ(chunk.readNumericAsDouble(0, 1), 7.0);
  EXPECT_TRUE(chunk.isNull(1, 1));
  expectAggregatesMatchChunks(fixture.storage());
}

TEST_P(ChunkGrowthProperty, ColumnsAddedMidStreamKeepEveryValueAndDoNotCutChunks) {
  const uint32_t seed = GetParam();
  const std::vector<Row> rows = makeRows(600, seed);
  std::mt19937 rng(seed ^ 0x51ED270BU);

  // The row at which each column appears: the first three exist from the start.
  std::vector<std::size_t> appears(kColumns.size(), 0);
  for (std::size_t c = 3; c < kColumns.size(); ++c) {
    appears[c] = appears[c - 1] + rng() % 90;
  }

  Fixture growing(3);
  std::size_t columns = 3;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    while (columns < kColumns.size() && appears[columns] <= i) {
      ASSERT_TRUE(
          growing.writer.ensureColumn(growing.topic, kColumns[columns].name, kColumns[columns].type).has_value());
      ++columns;
    }
    Row row = rows[i];
    row.cells.resize(columns);
    growing.write(row);
    if (rng() % 4 == 0) {
      (void)growing.storage().wholeNumbersSignature();
      growing.commitGrowing();
      expectAggregatesMatchChunks(growing.storage());
    }
  }
  growing.commitGrowing();
  growing.writer = growing.engine.createWriter();  // nothing pending

  const auto& chunks = growing.storage().sealedChunks();
  ASSERT_EQ(chunks.size(), (rows.size() + kMaxRows - 1) / kMaxRows);
  std::size_t row_index = 0;
  for (std::size_t k = 0; k < chunks.size(); ++k) {
    const TopicChunk& chunk = chunks[k];
    ASSERT_EQ(chunk.stats.row_count, std::min<std::size_t>(kMaxRows, rows.size() - k * kMaxRows));
    expectRangesMatchValues(chunk);
    for (std::size_t r = 0; r < chunk.stats.row_count; ++r, ++row_index) {
      ASSERT_EQ(chunk.timestamps[r], rows[row_index].timestamp);
      for (std::size_t c = 0; c < kColumns.size(); ++c) {
        // Set in the model only from the row its column appeared at.
        const Cell& expected = rows[row_index].cells[c];
        const bool set = row_index >= appears[c] && !std::holds_alternative<std::monostate>(expected);
        if (c >= chunk.columns.size()) {
          ASSERT_FALSE(set) << kColumns[c].name << " row " << row_index;
          continue;
        }
        ASSERT_EQ(!chunk.isNull(c, r), set) << kColumns[c].name << " row " << row_index;
        if (!set) {
          continue;
        }
        std::visit(
            [&](const auto& value) {
              using T = std::decay_t<decltype(value)>;
              if constexpr (std::is_same_v<T, std::string>) {
                ASSERT_EQ(chunk.readString(c, r), value);
              } else if constexpr (std::is_same_v<T, bool>) {
                ASSERT_EQ(chunk.readBool(c, r), value);
              } else if constexpr (std::is_same_v<T, uint64_t>) {
                ASSERT_EQ(chunk.readNumericAsUint64(c, r), value);
              } else if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t>) {
                ASSERT_EQ(chunk.readNumericAsInt64(c, r), static_cast<int64_t>(value));
              } else if constexpr (std::is_same_v<T, double> || std::is_same_v<T, float>) {
                ASSERT_TRUE(sameDouble(chunk.readNumericAsDouble(c, r), static_cast<double>(value)));
              }
            },
            expected);
      }
    }
  }
  ASSERT_EQ(row_index, rows.size());
}

TEST(ChunkGrowthTest, ACompactedLastChunkStillGrows) {
  const std::vector<Row> rows = makeRows(40, 7);

  Fixture reference;
  for (const Row& row : rows) {
    reference.write(row);
  }
  reference.commitSealed();

  Fixture growing;
  for (std::size_t i = 0; i < 15; ++i) {
    growing.write(rows[i]);
  }
  growing.commitGrowing();
  const ChunkId id = growing.storage().sealedChunks().front().id;
  growing.storage().compactLastChunk();  // as when a source ends
  EXPECT_FALSE(growing.storage().sealedChunks().front().isGrowable());
  expectAggregatesMatchChunks(growing.storage());

  for (std::size_t i = 15; i < rows.size(); ++i) {
    growing.write(rows[i]);
  }
  growing.commitGrowing();  // another source continues the topic
  ASSERT_EQ(growing.storage().sealedChunks().size(), 1U);
  EXPECT_EQ(growing.storage().sealedChunks().front().id, id);
  expectSameChunk(growing.storage().sealedChunks().front(), reference.storage().sealedChunks().front(), false);
  expectAggregatesMatchChunks(growing.storage());

  growing.storage().compactLastChunk();
  expectSameChunk(growing.storage().sealedChunks().front(), reference.storage().sealedChunks().front(), true);
}

TEST(ChunkGrowthTest, AChunkThatStopsBeingTheLastIsEncoded) {
  Fixture fixture(1);
  for (const Timestamp t : {10, 20, 30}) {
    fixture.write(numberRow(t, 7.0));  // a constant column: raw while the chunk grows
  }
  fixture.commitGrowing();
  ASSERT_TRUE(fixture.storage().sealedChunks().front().isGrowable());

  fixture.write(numberRow(40, 8.0));
  fixture.commitSealed();  // a chunk of its own after it: the first will not gain rows again
  const auto& chunks = fixture.storage().sealedChunks();
  ASSERT_EQ(chunks.size(), 2U);
  EXPECT_EQ(chunks[0].columnEncoding(0), EncodingType::kConstant);
  EXPECT_EQ(chunks[0].readNumericAsDouble(0, 2), 7.0);
  expectAggregatesMatchChunks(fixture.storage());
}

TEST(ChunkGrowthTest, AFullChunkKeepsNoSpareCapacity) {
  Fixture fixture(1);
  for (Timestamp t = 0; t < static_cast<Timestamp>(kMaxRows); ++t) {
    fixture.write(numberRow(t, static_cast<double>(t) + 0.5));  // fractions: the column stays raw
    if (t % 3 == 0) {
      fixture.commitGrowing();
    }
  }
  fixture.commitGrowing();
  const TopicChunk& chunk = fixture.storage().sealedChunks().front();
  ASSERT_EQ(chunk.stats.row_count, kMaxRows);
  const auto& raw = std::get<RawBuffer>(chunk.columns[0].data);
  EXPECT_EQ(raw.capacity(), raw.size());
  EXPECT_EQ(chunk.timestamps.capacity(), chunk.timestamps.size());
}

TEST(ChunkGrowthTest, AKeptBuilderTakesTheColumnsAnotherWriterAdded) {
  Fixture fixture(1);
  fixture.write(numberRow(10, 1.0));
  fixture.commitGrowing();  // the writer keeps its one-column builder

  DataWriter other = fixture.engine.createWriter();
  ASSERT_TRUE(other.ensureColumn(fixture.topic, "added", PrimitiveType::kFloat64).has_value());

  // The first writer is handed the existing column, and writes it.
  const auto column = fixture.writer.ensureColumn(fixture.topic, "added", PrimitiveType::kFloat64);
  ASSERT_TRUE(column.has_value());
  ASSERT_EQ(*column, 1U);
  fixture.write(Row{.timestamp = 20, .cells = {Cell{2.0}, Cell{3.0}}});
  fixture.commitGrowing();

  const TopicChunk& chunk = fixture.storage().sealedChunks().front();
  ASSERT_EQ(chunk.columns.size(), 2U);
  ASSERT_EQ(chunk.stats.row_count, 2U);
  EXPECT_TRUE(chunk.isNull(1, 0));
  EXPECT_EQ(chunk.readNumericAsDouble(1, 1), 3.0);
}

TEST(ChunkGrowthTest, AChunkRestoredUnderNewerIdsTakesANewIdBeforeItGrows) {
  Fixture fixture(1);
  fixture.write(numberRow(10, 1.0));
  fixture.commitGrowing();
  const ChunkId restored_id = fixture.storage().sealedChunks().front().id;

  // A refill that is rolled back: the topic held a newer chunk meanwhile, which a derived node may have read.
  TopicStorage::DetachedState detached = fixture.storage().detachChunks();
  fixture.write(numberRow(500, 5.0));
  fixture.commitGrowing();
  const ChunkId newer_id = fixture.storage().sealedChunks().front().id;
  ASSERT_GT(newer_id, restored_id);
  fixture.storage().restoreChunks(std::move(detached));
  ASSERT_EQ(fixture.storage().sealedChunks().front().id, restored_id);

  fixture.write(numberRow(20, 2.0));
  fixture.commitGrowing();
  const auto& chunks = fixture.storage().sealedChunks();
  ASSERT_EQ(chunks.size(), 1U);
  EXPECT_EQ(chunks[0].timestamps, (std::vector<Timestamp>{10, 20}));
  EXPECT_GT(chunks[0].id, newer_id) << "under the old id the new row would sit below a watermark already past it";
}

TEST(ChunkGrowthTest, SealedChunksCommittedWholeAreNotMergedIntoEachOther) {
  Fixture fixture(1);
  fixture.write(numberRow(10, 1.0));
  fixture.commitSealed();
  fixture.write(numberRow(20, 2.0));
  fixture.commitSealed();
  EXPECT_EQ(fixture.storage().sealedChunks().size(), 2U);

  // Rows committed unsealed extend whatever chunk is last.
  fixture.write(numberRow(30, 3.0));
  fixture.commitGrowing();
  ASSERT_EQ(fixture.storage().sealedChunks().size(), 2U);
  EXPECT_EQ(fixture.storage().sealedChunks().back().timestamps, (std::vector<Timestamp>{20, 30}));
  expectAggregatesMatchChunks(fixture.storage());
}

TEST(ChunkGrowthTest, RetentionDropsAChunkThatGrewLikeAnyOther) {
  Fixture fixture(1);
  for (Timestamp t = 0; t < static_cast<Timestamp>(kMaxRows) + 10; ++t) {
    fixture.write(numberRow(t, static_cast<double>(t)));
    fixture.commitGrowing();
  }
  ASSERT_EQ(fixture.storage().sealedChunks().size(), 2U);
  const ChunkId last_id = fixture.storage().sealedChunks().back().id;

  fixture.storage().evictBefore(static_cast<Timestamp>(kMaxRows) + 5);  // inside the growing chunk
  ASSERT_EQ(fixture.storage().sealedChunks().size(), 1U);
  EXPECT_EQ(fixture.storage().sealedChunks().front().id, last_id);
  expectAggregatesMatchChunks(fixture.storage());

  fixture.write(numberRow(1000, 1000.0));
  fixture.commitGrowing();
  ASSERT_EQ(fixture.storage().sealedChunks().size(), 1U);
  EXPECT_EQ(fixture.storage().sealedChunks().front().stats.row_count, 11U);
  expectAggregatesMatchChunks(fixture.storage());
  const auto visible = readAll(fixture.engine, fixture.topic);
  ASSERT_FALSE(visible.empty());
  EXPECT_EQ(visible.front().first, static_cast<Timestamp>(kMaxRows) + 5) << "rows under the floor stay hidden";
}

}  // namespace
}  // namespace PJ
