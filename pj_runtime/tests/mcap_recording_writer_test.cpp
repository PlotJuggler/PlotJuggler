// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
// McapRecordingWriter: what the recorder writes must read back as ordinary
// MCAP channels carrying the binding signature and the pj.* metadata.
#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// No MCAP_IMPLEMENTATION here: McapRecordingWriter.cpp is this binary's
// single mcap implementation TU (see the comment there).
#include <mcap/reader.hpp>

#include "pj_runtime/McapRecordingWriter.h"
#include "pj_runtime/RecordingFormat.h"
#include "recording_test_utils.h"

namespace {

PJ::RecordedBinding imuBinding() {
  return PJ::RecordedBinding{
      .topic = "/imu",
      .encoding = "cdr",
      .type_name = "sensor_msgs/msg/Imu",
      .schema_bytes = "float64 x",
  };
}

/// The imu binding re-pointed at another topic, optionally with other schema
/// bytes — the two axes the schema dedup key is built from.
PJ::RecordedBinding bindingOn(std::string topic, std::string schema_bytes) {
  PJ::RecordedBinding binding = imuBinding();
  binding.topic = std::move(topic);
  binding.schema_bytes = std::move(schema_bytes);
  return binding;
}

/// One source of one capture, the only shape a recording has: the file carries
/// that identity once, at file level.
PJ::RecordingInfo sourceInfo() {
  PJ::RecordingInfo info;
  info.app_version = "test";
  info.capture_id = "6f1b0f8e-0000-4000-8000-000000000001";
  info.source_display_name = "ROS2 Stream";
  info.source_plugin_id = "ros2_stream_plugin";
  info.capture_ordinal = 2;
  info.started_utc = "2026-08-31T10:00:00Z";
  return info;
}

PJ::RecordingSummary finalSummary() {
  PJ::RecordingSummary summary;
  summary.stopped_utc = "2026-08-31T10:01:00Z";
  summary.terminal_cause = std::string(PJ::kTerminalCauseTruncated);
  summary.truncated = true;
  summary.truncated_reason = "disk full";
  summary.messages = 2;
  summary.payload_bytes = 5;
  summary.dropped_messages = 3;
  return summary;
}

PJ::Span<const uint8_t> asSpan(const std::vector<uint8_t>& bytes) {
  return PJ::Span<const uint8_t>(bytes.data(), bytes.size());
}

/// A payload zstd cannot shrink away, so a chunk holding it really is large.
/// Deterministic (a plain LCG), because a test comparing chunk layouts must not
/// depend on the run.
std::vector<uint8_t> incompressiblePayload(size_t size) {
  std::vector<uint8_t> bytes(size);
  uint32_t state = 0x12345678;
  for (uint8_t& byte : bytes) {
    state = state * 1664525u + 1013904223u;
    byte = static_cast<uint8_t>(state >> 24);
  }
  return bytes;
}

TEST(McapRecordingWriter, RoundTripsChannelsMessagesAndMetadata) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path path = std::filesystem::path(dir.path().toStdString()) / "rec.mcap";

  PJ::McapRecordingWriter writer;
  ASSERT_TRUE(writer.open(path, sourceInfo()).has_value());

  auto channel = writer.addChannel(imuBinding());
  ASSERT_TRUE(channel.has_value()) << channel.error();
  const std::vector<uint8_t> first{1, 2, 3};
  const std::vector<uint8_t> second{4, 5};
  ASSERT_TRUE(writer.write(*channel, 1000, asSpan(first)).has_value());
  ASSERT_TRUE(writer.write(*channel, 2000, asSpan(second)).has_value());
  ASSERT_TRUE(writer.close(finalSummary()).has_value());

  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(path.string()).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).ok());

  const auto channels = reader.channels();
  ASSERT_EQ(channels.size(), 1u);
  const auto& mcap_channel = channels.begin()->second;
  EXPECT_EQ(mcap_channel->topic, "/imu");
  EXPECT_EQ(mcap_channel->messageEncoding, "cdr");
  // A recording holds one source, so its identity is a file-level fact and the
  // channel repeats nothing.
  EXPECT_TRUE(mcap_channel->metadata.empty());
  const auto schema = reader.schema(mcap_channel->schemaId);
  ASSERT_NE(schema, nullptr);
  EXPECT_EQ(schema->name, "sensor_msgs/msg/Imu");
  EXPECT_EQ(schema->encoding, "cdr");
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(schema->data.data()), schema->data.size()), "float64 x");

  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> messages;
  std::vector<uint32_t> sequences;
  for (const auto& view : reader.readMessages()) {
    messages.emplace_back(
        view.message.logTime, std::vector<uint8_t>(
                                  reinterpret_cast<const uint8_t*>(view.message.data),
                                  reinterpret_cast<const uint8_t*>(view.message.data) + view.message.dataSize));
    sequences.push_back(view.message.sequence);
  }
  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0].first, 1000u);
  EXPECT_EQ(messages[0].second, first);
  EXPECT_EQ(messages[1].first, 2000u);
  EXPECT_EQ(messages[1].second, second);
  EXPECT_EQ(sequences, (std::vector<uint32_t>{0, 1}));  // per-channel counter

  // Two pj.recording records: open() writes start facts only, close() adds the
  // final ones, and a reader takes the last.
  const auto records = PJ::test::recordingRecords(reader);
  ASSERT_EQ(records.size(), 2u);
  // The start facts, provenance included, are already in the file before the
  // first message: a recording abandoned mid-stream still says what it is.
  EXPECT_EQ(records.front().count("truncated"), 0u);
  EXPECT_EQ(records.front().at("started_utc"), "2026-08-31T10:00:00Z");
  EXPECT_EQ(records.front().at("version"), std::to_string(PJ::kRecordingFormatVersion));
  EXPECT_EQ(records.front().at("capture_id"), "6f1b0f8e-0000-4000-8000-000000000001");
  EXPECT_EQ(records.front().at("source_display_name"), "ROS2 Stream");
  EXPECT_EQ(records.front().at("source_plugin_id"), "ros2_stream_plugin");
  EXPECT_EQ(records.front().at("capture_ordinal"), "2");
  EXPECT_EQ(records.back().at("truncated"), "true");
  EXPECT_EQ(records.back().at("truncated_reason"), "disk full");
  EXPECT_EQ(records.back().at("terminal_cause"), std::string(PJ::kTerminalCauseTruncated));
  EXPECT_EQ(records.back().at("messages"), "2");
  EXPECT_EQ(records.back().at("payload_bytes"), "5");
  EXPECT_EQ(records.back().at("dropped_messages"), "3");
  EXPECT_EQ(records.back().at("stopped_utc"), "2026-08-31T10:01:00Z");
  EXPECT_EQ(records.back().at("capture_id"), "6f1b0f8e-0000-4000-8000-000000000001");
}

TEST(McapRecordingWriter, DeduplicatesSchemasBySignature) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path path = std::filesystem::path(dir.path().toStdString()) / "rec.mcap";

  PJ::McapRecordingWriter writer;
  ASSERT_TRUE(writer.open(path, sourceInfo()).has_value());
  // Same (type_name, encoding, schema_bytes) on two topics; the third differs
  // only in schema_bytes, so it must not share their schema. Each channel
  // carries a message: mcap emits schema and channel records lazily, on the
  // first message that references them.
  const std::vector<uint8_t> payload{1};
  for (const auto& binding :
       {bindingOn("/imu", "float64 x"), bindingOn("/imu_2", "float64 x"), bindingOn("/imu_3", "float64 y")}) {
    const auto channel = writer.addChannel(binding);
    ASSERT_TRUE(channel.has_value()) << channel.error();
    ASSERT_TRUE(writer.write(*channel, 1000, asSpan(payload)).has_value());
  }
  ASSERT_TRUE(writer.close(PJ::RecordingSummary{}).has_value());

  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(path.string()).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).ok());

  const auto channels = reader.channels();  // returned by value; snapshot before iterating
  std::unordered_map<std::string, uint16_t> schema_of_topic;
  for (const auto& entry : channels) {
    schema_of_topic[entry.second->topic] = entry.second->schemaId;
  }
  ASSERT_EQ(schema_of_topic.size(), 3u);
  EXPECT_EQ(schema_of_topic.at("/imu"), schema_of_topic.at("/imu_2"));
  EXPECT_NE(schema_of_topic.at("/imu"), schema_of_topic.at("/imu_3"));
  EXPECT_EQ(reader.schemas().size(), 2u);
}

TEST(McapRecordingWriter, RejectsUseBeforeOpen) {
  PJ::McapRecordingWriter writer;
  EXPECT_FALSE(writer.addChannel(imuBinding()).has_value());
  const std::vector<uint8_t> payload{1};
  EXPECT_FALSE(writer.write(0, 1000, asSpan(payload)).has_value());
  // close() on a writer that never opened is a no-op, not an error: the
  // recorder's teardown path must not have to remember whether open() ran.
  EXPECT_TRUE(writer.close(PJ::RecordingSummary{}).has_value());
}

TEST(McapRecordingWriter, RejectsNegativeLogTime) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path path = std::filesystem::path(dir.path().toStdString()) / "rec.mcap";

  PJ::McapRecordingWriter writer;
  ASSERT_TRUE(writer.open(path, sourceInfo()).has_value());
  const auto channel = writer.addChannel(imuBinding());
  ASSERT_TRUE(channel.has_value()) << channel.error();
  const std::vector<uint8_t> payload{7};
  EXPECT_FALSE(writer.write(*channel, -1, asSpan(payload)).has_value());
  ASSERT_TRUE(writer.write(*channel, 1000, asSpan(payload)).has_value());
  ASSERT_TRUE(writer.close(PJ::RecordingSummary{}).has_value());

  // The rejected message left nothing behind and the file still finalizes.
  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(path.string()).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan).ok());
  size_t message_count = 0;
  for (const auto& view : reader.readMessages()) {
    EXPECT_EQ(view.message.logTime, 1000u);
    ++message_count;
  }
  EXPECT_EQ(message_count, 1u);
}

TEST(McapRecordingWriter, RefusesToOverwriteAnExistingFile) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path path = std::filesystem::path(dir.path().toStdString()) / "taken.mcap";
  const std::string existing = "not an mcap file";
  {
    std::ofstream stream(path, std::ios::binary);
    stream << existing;
  }

  PJ::McapRecordingWriter writer;
  const auto opened = writer.open(path, sourceInfo());
  ASSERT_FALSE(opened.has_value());
  // The reason names the file AND carries the OS detail after it: a bare
  // "could not exclusively create" would not say why.
  EXPECT_NE(opened.error().find("could not exclusively create"), std::string::npos) << opened.error();
  EXPECT_NE(opened.error().find("': "), std::string::npos) << opened.error();

  // Exclusive create: the bytes that were there must be untouched.
  std::ifstream stream(path, std::ios::binary);
  const std::string after((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  EXPECT_EQ(after, existing);
}

TEST(McapRecordingWriter, ReusesOneInstanceAcrossRecordings) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path base = std::filesystem::path(dir.path().toStdString());
  const std::vector<uint8_t> payload{9, 9};

  PJ::McapRecordingWriter writer;
  for (const auto* name : {"first.mcap", "second.mcap"}) {
    ASSERT_TRUE(writer.open(base / name, sourceInfo()).has_value()) << name;
    const auto channel = writer.addChannel(imuBinding());
    ASSERT_TRUE(channel.has_value()) << channel.error();
    ASSERT_TRUE(writer.write(*channel, 1000, asSpan(payload)).has_value());
    ASSERT_TRUE(writer.close(finalSummary()).has_value()) << name;
  }

  // The second file must stand on its own: a complete summary section (no
  // fallback scan) and no problems reported while reading its messages.
  mcap::McapReader reader;
  ASSERT_TRUE(reader.open((base / "second.mcap").string()).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan).ok());
  EXPECT_EQ(reader.channels().size(), 1u);
  EXPECT_EQ(reader.schemas().size(), 1u);

  const mcap::ProblemCallback fail_on_problem = [](const mcap::Status& problem) {
    ADD_FAILURE() << "problem reading recorded messages: " << problem.message;
  };
  size_t message_count = 0;
  for (const auto& view : reader.readMessages(fail_on_problem)) {
    EXPECT_EQ(view.message.logTime, 1000u);
    ++message_count;
  }
  EXPECT_EQ(message_count, 1u);
}

// MCAP channel ids are uint16_t and mcap hands them out by incrementing, so
// past 65535 they would wrap onto live records. Refusing the channel truncates
// the recording with a real reason instead of silently corrupting it.
TEST(McapRecordingWriter, RefusesChannelsBeyondThe16BitIdSpace) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path path = std::filesystem::path(dir.path().toStdString()) / "ids.mcap";

  PJ::McapRecordingWriter writer;
  ASSERT_TRUE(writer.open(path, sourceInfo()).has_value());
  // One schema, 65535 distinct topics: only the channel id space runs out.
  for (int index = 0; index < 65535; ++index) {
    const auto channel = writer.addChannel(bindingOn("/t" + std::to_string(index), "float64 x"));
    ASSERT_TRUE(channel.has_value()) << index << ": " << channel.error();
  }
  const auto overflowed = writer.addChannel(bindingOn("/one_too_many", "float64 x"));
  ASSERT_FALSE(overflowed.has_value());
  EXPECT_NE(overflowed.error().find("channel ids"), std::string::npos) << overflowed.error();
  EXPECT_TRUE(writer.close(finalSummary()).has_value());
}

// A lazy reader decompresses a whole chunk to reach one message, so a large
// payload must not share its chunk with small ones. Log times are distinct, so
// a chunk's [messageStartTime, messageEndTime] range names exactly the messages
// inside it: a range collapsed onto one large message's time is proof that it
// is alone in there.
TEST(McapRecordingWriter, LargeMessagesGetAChunkToThemselves) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path path = std::filesystem::path(dir.path().toStdString()) / "chunks.mcap";

  PJ::McapRecordingWriter writer;
  ASSERT_TRUE(writer.open(path, sourceInfo()).has_value());
  const auto channel = writer.addChannel(imuBinding());
  ASSERT_TRUE(channel.has_value()) << channel.error();

  const std::vector<uint8_t> small{1, 2, 3};
  const auto large = incompressiblePayload(300 * 1024);  // over the isolation threshold
  const std::vector<std::pair<uint64_t, const std::vector<uint8_t>*>> written{
      {100, &small}, {200, &small}, {300, &large}, {400, &small}, {500, &large}, {600, &large},
  };
  for (const auto& [log_time, payload] : written) {
    ASSERT_TRUE(writer.write(*channel, static_cast<int64_t>(log_time), asSpan(*payload)).has_value()) << log_time;
  }
  ASSERT_TRUE(writer.close(finalSummary()).has_value());

  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(path.string()).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan).ok());

  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  for (const auto& chunk : reader.chunkIndexes()) {
    ranges.emplace_back(chunk.messageStartTime, chunk.messageEndTime);
  }
  std::sort(ranges.begin(), ranges.end());
  // The two small messages share one chunk; each large message has its own, the
  // two consecutive ones included; the small message after a large one starts a
  // fresh chunk rather than joining it.
  const std::vector<std::pair<uint64_t, uint64_t>> expected{{100, 200}, {300, 300}, {400, 400}, {500, 500}, {600, 600}};
  EXPECT_EQ(ranges, expected);

  const mcap::ProblemCallback fail_on_problem = [](const mcap::Status& problem) {
    ADD_FAILURE() << "problem reading the recording: " << problem.message;
  };
  size_t index = 0;
  for (const auto& view : reader.readMessages(fail_on_problem)) {
    ASSERT_LT(index, written.size());
    EXPECT_EQ(view.message.logTime, written[index].first);
    const auto* first = reinterpret_cast<const uint8_t*>(view.message.data);
    EXPECT_EQ(std::vector<uint8_t>(first, first + view.message.dataSize), *written[index].second) << index;
    ++index;
  }
  EXPECT_EQ(index, written.size());
}

}  // namespace
