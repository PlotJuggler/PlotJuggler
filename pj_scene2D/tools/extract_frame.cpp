// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/codecs.h"

#define MCAP_IMPLEMENTATION
#include <mcap/reader.hpp>

namespace {

struct CdrJpegExtractor {
  static std::pair<const uint8_t*, size_t> extract(const uint8_t* raw, size_t size) {
    if (size < 24) {
      return {nullptr, 0};
    }
    size_t offset = 4;  // skip CDR header (4 bytes: encapsulation kind + options)
    offset += 8;        // skip stamp (sec + nsec)
    if (offset + 4 > size) {
      return {nullptr, 0};
    }
    uint32_t str_len = 0;
    std::memcpy(&str_len, raw + offset, 4);
    offset += 4 + str_len;  // skip frame_id

    if (offset + 4 > size) {
      return {nullptr, 0};
    }
    std::memcpy(&str_len, raw + offset, 4);
    offset += 4 + str_len;  // skip format

    if (offset + 4 > size) {
      return {nullptr, 0};
    }
    uint32_t data_len = 0;
    std::memcpy(&data_len, raw + offset, 4);
    offset += 4;

    if (offset + data_len > size) {
      return {nullptr, 0};
    }
    return {raw + offset, data_len};
  }
};

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: extract_frame <file.mcap> [output.ppm] [frame_index]\n";
    return 1;
  }

  std::string mcap_path = argv[1];
  std::string output_path = argc > 2 ? argv[2] : "frame.ppm";
  size_t frame_index = argc > 3 ? static_cast<size_t>(std::atoi(argv[3])) : 0;

  mcap::McapReader reader;
  auto status = reader.open(mcap_path);
  if (!status.ok()) {
    std::cerr << "Failed to open: " << status.message << "\n";
    return 1;
  }

  status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
  if (!status.ok()) {
    std::cerr << "Failed to read summary: " << status.message << "\n";
    return 1;
  }

  uint16_t target_chan = 0;
  std::string topic_name;
  std::string encoding;
  for (const auto& [chan_id, chan_ptr] : reader.channels()) {
    if (chan_ptr == nullptr) {
      continue;
    }
    auto schemas = reader.schemas();
    auto schema_it = schemas.find(chan_ptr->schemaId);
    if (schema_it != schemas.end() && schema_it->second != nullptr) {
      const auto& schema_name = schema_it->second->name;
      if (schema_name.find("CompressedImage") != std::string::npos) {
        target_chan = chan_id;
        topic_name = chan_ptr->topic;
        encoding = chan_ptr->messageEncoding;
        std::cout << "Found image topic: " << topic_name << " (schema: " << schema_name << ", encoding: " << encoding
                  << ")\n";
        break;
      }
    }
  }

  if (target_chan == 0) {
    std::cerr << "No image topic found\n";
    return 1;
  }

  PJ::ObjectStore store;
  auto id_or = store.registerTopic({.dataset_id = 1, .topic_name = topic_name, .metadata_json = "{}"});
  if (!id_or.has_value()) {
    std::cerr << "Failed to register topic\n";
    return 1;
  }
  auto topic_id = *id_or;

  mcap::ReadMessageOptions opts;
  opts.topicFilter = [&topic_name](std::string_view t) { return t == topic_name; };
  auto view = reader.readMessages([](const mcap::Status&) {}, opts);

  size_t msg_count = 0;
  for (auto it = view.begin(); it != view.end(); ++it) {
    if (it->message.channelId != target_chan) {
      continue;
    }
    auto ts = static_cast<PJ::Timestamp>(it->message.logTime);
    auto* data = reinterpret_cast<const uint8_t*>(it->message.data);
    auto sz = it->message.dataSize;
    store.pushOwned(topic_id, ts, std::vector<uint8_t>(data, data + sz));
    ++msg_count;
  }
  std::cout << "Loaded " << msg_count << " messages\n";

  if (frame_index >= msg_count) {
    std::cerr << "Frame index " << frame_index << " out of range (0.." << msg_count - 1 << ")\n";
    return 1;
  }

  auto entry = store.at(topic_id, frame_index);
  if (!entry.has_value() || entry->payload.bytes.empty()) {
    std::cerr << "Failed to resolve entry\n";
    return 1;
  }

  const auto raw = entry->payload.bytes;
  std::cout << "Raw message size: " << raw.size() << " bytes\n";

  // Try CDR extraction (ROS2 CompressedImage)
  const uint8_t* jpeg_data = nullptr;
  size_t jpeg_size = 0;

  if (encoding == "ros2msg" || encoding == "cdr") {
    auto [ptr, sz] = CdrJpegExtractor::extract(raw.data(), raw.size());
    jpeg_data = ptr;
    jpeg_size = sz;
  }

  // Fallback: try treating the whole thing as JPEG
  if (jpeg_data == nullptr && raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xD8) {
    jpeg_data = raw.data();
    jpeg_size = raw.size();
  }

  if (jpeg_data == nullptr) {
    std::cerr << "Could not extract JPEG data (encoding: " << encoding << ")\n";
    std::cerr << "First 16 bytes: ";
    for (size_t i = 0; i < std::min<size_t>(16, raw.size()); ++i) {
      std::fprintf(stderr, "%02x ", raw[i]);
    }
    std::cerr << "\n";
    return 1;
  }

  std::cout << "JPEG size: " << jpeg_size << " bytes\n";

  PJ::DecodedFrame encoded;
  encoded.pixels = std::make_shared<std::vector<uint8_t>>(jpeg_data, jpeg_data + jpeg_size);
  PJ::JpegCodec decoder;
  auto frame_or = decoder.decode(encoded);
  if (!frame_or.has_value()) {
    std::cerr << "Decode failed: " << frame_or.error() << "\n";
    return 1;
  }

  auto& frame = *frame_or;
  std::cout << "Decoded: " << frame.width << "x" << frame.height << " RGB\n";

  std::ofstream out(output_path, std::ios::binary);
  out << "P6\n" << frame.width << " " << frame.height << "\n255\n";
  out.write(reinterpret_cast<const char*>(frame.pixels->data()), static_cast<std::streamsize>(frame.pixels->size()));
  out.close();

  std::cout << "Saved to " << output_path << "\n";
  return 0;
}
