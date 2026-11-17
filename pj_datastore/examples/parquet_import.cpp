// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// parquet_import — load a Parquet file into DataEngine, report memory stats.
//
// Usage: ./parquet_import <file.parquet> [chunk_rows]

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/writer.h>
#include <parquet/arrow/reader.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pj_base/number_parse.hpp"
#include "pj_base/span.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/arrow_import.hpp"
#include "pj_datastore/chunk.hpp"
#include "pj_datastore/encoding.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/topic_storage.hpp"

namespace {

using PJ::DataEngine;
using PJ::EncodingType;
using PJ::PrimitiveType;
using PJ::TopicChunk;

// ── Local column mapping for report formatting ──────────────────────────────

struct ColumnMapping {
  int arrow_index;           // column index in Arrow table
  std::size_t pj_col_index;  // column index in PJ schema
  PrimitiveType pj_type;
  std::string name;
};

std::string_view encoding_name(EncodingType enc) {
  switch (enc) {
    case EncodingType::kRaw:
      return "Raw";
    case EncodingType::kDictionary:
      return "Dictionary";
    case EncodingType::kPackedBool:
      return "PackedBool";
    case EncodingType::kConstant:
      return "Constant";
    case EncodingType::kFrameOfReference:
      return "FrameOfRef";
  }
  return "Unknown";
}

std::string arrow_type_name(const std::shared_ptr<arrow::DataType>& type) {
  return type->name();
}

// ── Per-column memory measurement ───────────────────────────────────────────

struct ColumnMemory {
  std::size_t actual_bytes = 0;
  std::size_t theoretical_bytes = 0;
  EncodingType dominant_encoding = EncodingType::kRaw;
};

std::size_t encoded_column_bytes(const TopicChunk& chunk, std::size_t col) {
  return std::visit(
      [](const auto& v) -> std::size_t {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, PJ::RawBuffer>) {
          return v.size();
        } else if constexpr (std::is_same_v<T, PJ::encoding::ConstantEncoded>) {
          return v.value_size;
        } else if constexpr (std::is_same_v<T, PJ::encoding::FrameOfReferenceEncoded>) {
          return v.offsets.size();
        } else if constexpr (std::is_same_v<T, PJ::encoding::DictionaryEncoded>) {
          std::size_t dict_bytes = 0;
          for (const auto& s : v.dictionary) {
            dict_bytes += s.size();
          }
          return v.indices.size() + dict_bytes;
        } else if constexpr (std::is_same_v<T, PJ::encoding::PackedBools>) {
          return v.bits.size();
        }
      },
      chunk.columns[col].data);
}

std::vector<ColumnMemory> measure_memory(
    const std::deque<TopicChunk>& chunks, std::size_t num_columns, const std::vector<ColumnMapping>& mappings,
    const std::shared_ptr<arrow::Table>& table) {
  std::vector<ColumnMemory> result(num_columns);

  constexpr int kNumEncodings = 5;
  std::vector<std::vector<uint32_t>> enc_counts(num_columns, std::vector<uint32_t>(kNumEncodings, 0));

  for (const auto& chunk : chunks) {
    for (std::size_t col = 0; col < num_columns; ++col) {
      result[col].actual_bytes += encoded_column_bytes(chunk, col);
      if (chunk.columns[col].validity_bitmap) {
        result[col].actual_bytes += chunk.columns[col].validity_bitmap->sizeBytes();
      }
      enc_counts[col][static_cast<uint8_t>(chunk.columnEncoding(col))]++;
    }
  }

  for (std::size_t col = 0; col < num_columns; ++col) {
    uint32_t max_count = 0;
    for (int enc = 0; enc < kNumEncodings; ++enc) {
      if (enc_counts[col][static_cast<std::size_t>(enc)] > max_count) {
        max_count = enc_counts[col][static_cast<std::size_t>(enc)];
        result[col].dominant_encoding = static_cast<EncodingType>(enc);
      }
    }
  }

  // Theoretical bytes: the uncompressed size, Arrow byte width × rows (string bytes for strings, a byte per bool).
  auto total_rows = static_cast<std::size_t>(table->num_rows());
  for (std::size_t col = 0; col < num_columns; ++col) {
    const auto& mapping = mappings[col];
    auto arrow_col = table->column(mapping.arrow_index);
    auto arrow_type = arrow_col->type();

    if (arrow_type->id() == arrow::Type::STRING || arrow_type->id() == arrow::Type::LARGE_STRING) {
      std::size_t string_bytes = 0;
      for (int i = 0; i < arrow_col->num_chunks(); ++i) {
        if (arrow_type->id() == arrow::Type::STRING) {
          auto arr = std::static_pointer_cast<arrow::StringArray>(arrow_col->chunk(i));
          string_bytes += static_cast<std::size_t>(arr->total_values_length());
        } else {
          auto arr = std::static_pointer_cast<arrow::LargeStringArray>(arrow_col->chunk(i));
          string_bytes += static_cast<std::size_t>(arr->total_values_length());
        }
      }
      result[col].theoretical_bytes = string_bytes;
    } else if (arrow_type->id() == arrow::Type::BOOL) {
      result[col].theoretical_bytes = total_rows;
    } else {
      auto byte_width = static_cast<std::size_t>(arrow_type->byte_width());
      result[col].theoretical_bytes = byte_width * total_rows;
    }
  }

  return result;
}

// ── Report formatting ───────────────────────────────────────────────────────

void print_report(
    const std::vector<ColumnMapping>& mappings, const std::vector<ColumnMemory>& memory,
    const std::shared_ptr<arrow::Table>& table, double ingest_seconds) {
  constexpr int kNameW = 24;
  constexpr int kTypeW = 12;
  constexpr int kEncW = 14;
  constexpr int kBytesW = 12;
  constexpr int kRatioW = 8;
  int total_w = kNameW + kTypeW + kEncW + kBytesW * 2 + kRatioW;

  std::cout << "\n";
  std::cout << "Rows:    " << table->num_rows() << "\n";
  std::cout << "Columns: " << mappings.size() << "\n";
  std::cout << "Ingest:  " << std::fixed << std::setprecision(3) << ingest_seconds << " s\n\n";

  std::cout << std::left << std::setw(kNameW) << "Column" << std::setw(kTypeW) << "Arrow Type" << std::setw(kEncW)
            << "PJ Encoding" << std::right << std::setw(kBytesW) << "Actual" << std::setw(kBytesW) << "Theoretical"
            << std::setw(kRatioW) << "Ratio"
            << "\n";
  std::cout << std::string(static_cast<std::size_t>(total_w), '-') << "\n";

  std::size_t total_actual = 0;
  std::size_t total_theoretical = 0;

  for (std::size_t i = 0; i < mappings.size(); ++i) {
    const auto& m = mappings[i];
    const auto& mem = memory[i];

    auto arrow_type = table->column(m.arrow_index)->type();
    double ratio = mem.theoretical_bytes > 0
                       ? static_cast<double>(mem.actual_bytes) / static_cast<double>(mem.theoretical_bytes)
                       : 0.0;

    std::cout << std::left << std::setw(kNameW) << m.name << std::setw(kTypeW) << arrow_type_name(arrow_type)
              << std::setw(kEncW) << encoding_name(mem.dominant_encoding) << std::right << std::setw(kBytesW)
              << mem.actual_bytes << std::setw(kBytesW) << mem.theoretical_bytes << std::setw(kRatioW - 1) << std::fixed
              << std::setprecision(2) << ratio << "x\n";

    total_actual += mem.actual_bytes;
    total_theoretical += mem.theoretical_bytes;
  }

  std::cout << std::string(static_cast<std::size_t>(total_w), '-') << "\n";

  double total_ratio =
      total_theoretical > 0 ? static_cast<double>(total_actual) / static_cast<double>(total_theoretical) : 0.0;

  std::cout << std::left << std::setw(kNameW) << "TOTAL" << std::setw(kTypeW) << "" << std::setw(kEncW) << ""
            << std::right << std::setw(kBytesW) << total_actual << std::setw(kBytesW) << total_theoretical
            << std::setw(kRatioW - 1) << std::fixed << std::setprecision(2) << total_ratio << "x\n\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <file.parquet> [chunk_rows]\n";
    return 1;
  }

  const std::string path = argv[1];
  uint32_t chunk_rows = 8192;
  if (argc >= 3) {
    const auto parsed = PJ::parseNumber<uint32_t>(argv[2]);
    if (!parsed.has_value() || *parsed == 0) {
      std::cerr << "Invalid chunk_rows value: " << argv[2] << " (expected a positive integer)\n";
      return 1;
    }
    chunk_rows = *parsed;
  }

  // ── 1. Open Parquet file ──────────────────────────────────────────────────

  auto maybe_infile = arrow::io::ReadableFile::Open(path);
  if (!maybe_infile.ok()) {
    std::cerr << "Failed to open file: " << maybe_infile.status().ToString() << "\n";
    return 1;
  }

  auto maybe_reader = parquet::arrow::OpenFile(*maybe_infile, arrow::default_memory_pool());
  if (!maybe_reader.ok()) {
    std::cerr << "Failed to open Parquet reader: " << maybe_reader.status().ToString() << "\n";
    return 1;
  }
  auto arrow_reader = std::move(*maybe_reader);

  std::shared_ptr<arrow::Table> table;
  auto st = arrow_reader->ReadTable(&table);
  if (!st.ok()) {
    std::cerr << "Failed to read table: " << st.ToString() << "\n";
    return 1;
  }

  std::cout << "Loaded " << path << ": " << table->num_rows() << " rows, " << table->num_columns() << " columns\n";

  // ── 2. Serialize Arrow Table to IPC stream bytes ─────────────────────────

  auto sink_result = arrow::io::BufferOutputStream::Create();
  if (!sink_result.ok()) {
    std::cerr << "Failed to create buffer output stream: " << sink_result.status().ToString() << "\n";
    return 1;
  }
  auto sink = *sink_result;

  auto ipc_writer_result = arrow::ipc::MakeStreamWriter(sink, table->schema());
  if (!ipc_writer_result.ok()) {
    std::cerr << "Failed to create IPC writer: " << ipc_writer_result.status().ToString() << "\n";
    return 1;
  }
  auto ipc_writer = *ipc_writer_result;

  st = ipc_writer->WriteTable(*table);
  if (!st.ok()) {
    std::cerr << "IPC WriteTable failed: " << st.ToString() << "\n";
    return 1;
  }
  st = ipc_writer->Close();
  if (!st.ok()) {
    std::cerr << "IPC writer Close failed: " << st.ToString() << "\n";
    return 1;
  }

  auto ipc_buf_result = sink->Finish();
  if (!ipc_buf_result.ok()) {
    std::cerr << "Failed to finish IPC buffer: " << ipc_buf_result.status().ToString() << "\n";
    return 1;
  }
  auto ipc_buffer = *ipc_buf_result;

  PJ::Span<const uint8_t> ipc_bytes(ipc_buffer->data(), static_cast<std::size_t>(ipc_buffer->size()));

  std::cout << "Serialized to IPC: " << ipc_buffer->size() << " bytes\n";

  // ── 3. Map IPC schema → TypeTreeNode via arrow_import ──────────────────

  auto schema_result = PJ::arrow_import::schemaFromIpc(ipc_bytes);
  if (!schema_result.has_value()) {
    std::cerr << "Schema conversion failed: " << schema_result.error() << "\n";
    return 1;
  }
  auto& [type_tree, arrow_mappings] = *schema_result;

  std::vector<ColumnMapping> mappings;
  mappings.reserve(arrow_mappings.size());
  for (const auto& am : arrow_mappings) {
    ColumnMapping m;
    m.arrow_index = am.arrow_column_index;
    m.pj_col_index = am.pj_column_index;
    m.pj_type = am.pj_type;
    m.name = am.field_name;
    mappings.push_back(std::move(m));
  }

  // ── 4. Create DataEngine, dataset, schema, topic ──────────────────────────

  DataEngine engine;

  auto td_or = engine.createTimeDomain("default");
  if (!td_or.has_value()) {
    std::cerr << "Failed to create time domain: " << td_or.error() << "\n";
    return 1;
  }

  PJ::DatasetDescriptor ds_desc;
  ds_desc.source_name = path;
  ds_desc.time_domain_id = *td_or;
  auto ds_or = engine.createDataset(std::move(ds_desc));
  if (!ds_or.has_value()) {
    std::cerr << "Failed to create dataset: " << ds_or.error() << "\n";
    return 1;
  }
  auto dataset_id = *ds_or;

  auto writer = engine.createWriter();

  auto schema_or = writer.registerSchema("parquet_schema", type_tree);
  if (!schema_or.has_value()) {
    std::cerr << "Failed to register schema: " << schema_or.error() << "\n";
    return 1;
  }

  PJ::TopicDescriptor topic_desc;
  topic_desc.name = "parquet_data";
  topic_desc.schema_id = *schema_or;
  topic_desc.dataset_id = dataset_id;
  topic_desc.max_chunk_rows = chunk_rows;

  auto topic_or = writer.registerTopic(dataset_id, std::move(topic_desc));
  if (!topic_or.has_value()) {
    std::cerr << "Failed to register topic: " << topic_or.error() << "\n";
    return 1;
  }
  auto topic_id = *topic_or;

  // ── 5. Bulk ingest via IPC import API ────────────────────────────────────

  auto t_start = std::chrono::steady_clock::now();

  auto import_st = PJ::arrow_import::importIpcStream(writer, topic_id, ipc_bytes, arrow_mappings);
  if (!import_st.has_value()) {
    std::cerr << "import_ipc_stream failed: " << import_st.error() << "\n";
    return 1;
  }

  auto flushed = writer.flushAll();
  engine.commitChunks(std::move(flushed));

  auto t_end = std::chrono::steady_clock::now();
  double ingest_seconds = std::chrono::duration<double>(t_end - t_start).count();

  // ── 6. Memory report ─────────────────────────────────────────────────────

  const auto* storage = engine.getTopicStorage(topic_id);
  if (storage == nullptr) {
    std::cerr << "Topic storage not found after commit.\n";
    return 1;
  }

  auto memory = measure_memory(storage->sealedChunks(), mappings.size(), mappings, table);
  print_report(mappings, memory, table, ingest_seconds);

  // Cross-check with TopicMetadata
  auto meta = storage->metadata();
  std::cout << "TopicMetadata.total_byte_size: " << meta.total_byte_size << "\n";
  std::cout << "TopicMetadata.total_row_count: " << meta.total_row_count << "\n";

  return 0;
}
