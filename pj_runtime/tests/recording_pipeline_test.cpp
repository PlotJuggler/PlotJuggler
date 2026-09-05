// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
// The whole recording pipeline, end to end and headless: real
// DataSourceRuntimeHosts (with a real parser plugin loaded through a hermetic
// catalog) -> RecordTap -> Recorder -> McapRecordingWriter, orchestrated by
// RecordingService, with the produced files reopened as ordinary MCAP. The unit
// tests around each seam cannot see what only the assembled pipeline shows:
// that pressing Record mid-stream captures topics bound long before, that
// nothing pushed before the start leaks in, that two live sources land in two
// files of one capture, and that ingest is untouched.
#include <gtest/gtest.h>

#include <QFileInfo>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// No MCAP_IMPLEMENTATION here: pj_runtime carries the single implementation TU.
#include <mcap/reader.hpp>

#include "pj_runtime/RecordingFormat.h"
#include "pj_runtime/RecordingService.h"
#include "recording_test_utils.h"
#include "runtime_host_test_fixture.h"

using namespace Qt::StringLiterals;

namespace {

constexpr const char* kSourceId = "pipeline_source";
constexpr const char* kPluginId = "pipeline_plugin";

/// One channel of a recorded file, reduced to the facts the replay path keys on.
struct RecordedChannel {
  std::string message_encoding;
  std::string schema_name;      ///< where the type name lives
  size_t metadata_entries = 0;  ///< a recording holds one source, so this is 0
};

struct RecordedMessage {
  std::string topic;
  uint64_t log_time = 0;
  std::vector<uint8_t> bytes;
};

/// A recorded file read back whole: channels by topic, messages in file order
/// (= the order the writer thread drained them), and the last pj.recording
/// record -- the one a reader takes, since close() appends the final facts.
struct RecordedFile {
  std::map<std::string, RecordedChannel> channels;
  std::vector<RecordedMessage> messages;
  mcap::KeyValueMap recording;
};

RecordedFile readAll(const QString& path) {
  RecordedFile result;
  mcap::McapReader reader;
  if (!reader.open(path.toStdString()).ok()) {
    ADD_FAILURE() << "cannot open the recording at " << path.toStdString();
    return result;
  }
  // NoFallbackScan on purpose: a recording that stopped cleanly must carry a
  // complete summary section and footer, so accepting a rebuilt-by-scan index
  // here would hide exactly the finalization failure this test exists to catch.
  if (!reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan).ok()) {
    ADD_FAILURE() << "cannot read the summary of " << path.toStdString();
    return result;
  }

  const auto channels = reader.channels();  // returned by value; snapshot before iterating
  for (const auto& [id, channel] : channels) {
    RecordedChannel facts;
    facts.message_encoding = channel->messageEncoding;
    facts.metadata_entries = channel->metadata.size();
    if (const auto schema = reader.schema(channel->schemaId); schema != nullptr) {
      facts.schema_name = schema->name;
    }
    result.channels.emplace(channel->topic, std::move(facts));
  }

  const mcap::ProblemCallback fail_on_problem = [&path](const mcap::Status& problem) {
    ADD_FAILURE() << "problem reading " << path.toStdString() << ": " << problem.message;
  };
  for (const auto& view : reader.readMessages(fail_on_problem)) {
    const auto* first = reinterpret_cast<const uint8_t*>(view.message.data);
    result.messages.push_back(
        RecordedMessage{
            .topic = view.channel->topic,
            .log_time = view.message.logTime,
            .bytes = std::vector<uint8_t>(first, first + view.message.dataSize),
        });
  }

  result.recording = PJ::test::lastRecordingRecord(reader);
  return result;
}

/// The recorded messages of one topic, in file order.
std::vector<RecordedMessage> messagesOn(const RecordedFile& file, std::string_view topic) {
  std::vector<RecordedMessage> selected;
  for (const RecordedMessage& message : file.messages) {
    if (message.topic == topic) {
      selected.push_back(message);
    }
  }
  return selected;
}

class RecordingPipelineTest : public PJ::test::RuntimeHostFixture {
 protected:
  RecordingPipelineTest() : RuntimeHostFixture(kSourceId) {}

  void SetUp() override {
    ASSERT_TRUE(settings_.isValid());
    ASSERT_TRUE(recordings_dir_.isValid());
    RuntimeHostFixture::SetUp();
  }

  /// A second live source on the same engine and catalog: its own dataset,
  /// host and service registry, exactly as a second streaming session. Held by
  /// pointer so the test can drop it before the storage it points into goes.
  [[nodiscard]] std::unique_ptr<PJ::test::HostBox> makeSecondSource(const std::string& source_id) {
    auto second = std::make_unique<PJ::test::HostBox>();
    second->open(engine_, catalog_, object_store_, ingest_taps_, source_id, source_id);
    return second;
  }

  /// The one recordable source of this fixture, wired to the real host exactly
  /// as pj_app's streaming manager wires a live one.
  [[nodiscard]] PJ::RecordingTarget recordingTarget() {
    PJ::RecordingTarget target;
    target.target_key = 1;
    target.source_id = kSourceId;
    target.plugin_id = kPluginId;
    target.attach = [this](std::shared_ptr<PJ::RecordTap> tap) {
      host->setRecordTap(std::move(tap));
      return true;
    };
    target.skipped_lazy = [this] { return host->recordTapSkippedLazy(); };
    return target;
  }

  [[nodiscard]] static PJ::RecordingTarget targetFor(
      PJ::test::HostBox& source, uint64_t target_key, std::string source_id, std::string plugin_id) {
    PJ::RecordingTarget target;
    target.target_key = target_key;
    target.source_id = std::move(source_id);
    target.plugin_id = std::move(plugin_id);
    target.attach = [&source](std::shared_ptr<PJ::RecordTap> tap) {
      source.host->setRecordTap(std::move(tap));
      return true;
    };
    target.skipped_lazy = [&source] { return source.host->recordTapSkippedLazy(); };
    return target;
  }

  [[nodiscard]] PJ::RecordingService::Settings temporarySettings() const {
    PJ::RecordingService::Settings settings;
    settings.directory = recordings_dir_.path();
    settings.queue_budget_mib = 16;
    return settings;
  }

  [[nodiscard]] static QVector<PJ::SourceRecordingStart> startedSources(const QSignalSpy& started) {
    return started.at(0).at(2).value<QVector<PJ::SourceRecordingStart>>();
  }

  [[nodiscard]] static PJ::CaptureResult capture(const QSignalSpy& stopped) {
    return stopped.at(0).at(0).value<PJ::CaptureResult>();
  }

  PJ::test::IsolatedQtSettings settings_{u"PlotJugglerRecordingPipelineTest"_s, u"RecordingPipelineTest"_s};
  QTemporaryDir recordings_dir_;
};

// The "press Record mid-stream" path: /before was subscribed and pushing long
// before the recording started, /during appears while it runs.
TEST_F(RecordingPipelineTest, RecordsAStreamStartedBeforeAndDuringRecording) {
  const auto before = bindTopic("/before");
  const std::vector<std::vector<uint8_t>> pre_recording{{0xEE, 0x01}, {0xEE, 0x02}, {0xEE, 0x03}};
  PJ::Timestamp pre_timestamp = 10;
  for (const auto& payload : pre_recording) {
    push(before, pre_timestamp++, payload);
  }

  PJ::RecordingService service;
  service.setSettings(temporarySettings());
  QSignalSpy started(&service, &PJ::RecordingService::started);
  QSignalSpy stopped(&service, &PJ::RecordingService::stopped);

  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(recordingTarget());
  const auto directory = service.start(std::move(targets), "pipeline-test");
  ASSERT_TRUE(directory.has_value()) << directory.error();
  ASSERT_EQ(started.count(), 1);
  const auto sources = startedSources(started);
  ASSERT_EQ(sources.size(), 1);
  const QString path = sources[0].path;
  EXPECT_TRUE(QFileInfo::exists(path));

  std::vector<RecordedMessage> expected_before;
  for (uint8_t index = 0; index < 5; ++index) {
    const std::vector<uint8_t> payload{0xB0, index};
    push(before, 100 + index, payload);
    expected_before.push_back(RecordedMessage{.topic = "/before", .log_time = 100u + index, .bytes = payload});
  }

  // A topic bound while the recording runs opens its channel the same way.
  const auto during = bindTopic("/during");
  std::vector<RecordedMessage> expected_during;
  for (uint8_t index = 0; index < 2; ++index) {
    const std::vector<uint8_t> payload{0xD0, index};
    push(during, 110 + index, payload);
    expected_during.push_back(RecordedMessage{.topic = "/during", .log_time = 110u + index, .bytes = payload});
  }

  const std::vector<uint8_t> last{0xBF, 0xFF};
  push(before, 120, last);
  expected_before.push_back(RecordedMessage{.topic = "/before", .log_time = 120u, .bytes = last});

  service.stop();
  ASSERT_EQ(stopped.count(), 1);  // finalization is synchronous

  EXPECT_FALSE(service.isRecording());
  const PJ::CaptureResult result = capture(stopped);
  EXPECT_EQ(result.directory, *directory);
  ASSERT_EQ(result.sources.size(), 1);
  EXPECT_EQ(result.sources[0].path, path);
  EXPECT_EQ(result.sources[0].outcome, PJ::RecordingOutcome::kClean);
  EXPECT_TRUE(result.sources[0].reason.isEmpty()) << result.sources[0].reason.toStdString();
  EXPECT_EQ(result.sources[0].skipped_messages, 0u);
  EXPECT_EQ(result.sources[0].dropped_messages, 0u);
  EXPECT_EQ(result.clean_count, 1);
  EXPECT_TRUE(QFileInfo::exists(path));
  EXPECT_EQ(host->recordTapSkippedLazy(), 0u);

  const RecordedFile file = readAll(path);
  ASSERT_EQ(file.channels.size(), 2u);
  for (const auto& [topic, channel] : file.channels) {
    EXPECT_EQ(channel.message_encoding, kEncoding) << topic;
    EXPECT_EQ(channel.schema_name, kTypeName) << topic;
    EXPECT_EQ(channel.metadata_entries, 0u) << topic;
  }
  EXPECT_EQ(file.channels.count("/before"), 1u);
  EXPECT_EQ(file.channels.count("/during"), 1u);

  const auto recorded_before = messagesOn(file, "/before");
  ASSERT_EQ(recorded_before.size(), expected_before.size());
  for (size_t index = 0; index < expected_before.size(); ++index) {
    EXPECT_EQ(recorded_before[index].log_time, expected_before[index].log_time) << index;
    EXPECT_EQ(recorded_before[index].bytes, expected_before[index].bytes) << index;
  }
  const auto recorded_during = messagesOn(file, "/during");
  ASSERT_EQ(recorded_during.size(), expected_during.size());
  for (size_t index = 0; index < expected_during.size(); ++index) {
    EXPECT_EQ(recorded_during[index].log_time, expected_during[index].log_time) << index;
    EXPECT_EQ(recorded_during[index].bytes, expected_during[index].bytes) << index;
  }
  ASSERT_EQ(file.messages.size(), 8u);
  for (const RecordedMessage& message : file.messages) {
    for (const auto& payload : pre_recording) {
      EXPECT_NE(message.bytes, payload) << "a message pushed before start() leaked into the recording";
    }
  }

  EXPECT_EQ(file.recording.at("app_version"), "pipeline-test");
  EXPECT_EQ(file.recording.at("source_display_name"), kSourceId);
  EXPECT_EQ(file.recording.at("source_plugin_id"), kPluginId);
  EXPECT_EQ(file.recording.at("capture_ordinal"), "1");
  EXPECT_EQ(file.recording.at("capture_id"), result.capture_id.toStdString());
  EXPECT_EQ(file.recording.at("terminal_cause"), std::string(PJ::kTerminalCauseStopped));
  EXPECT_EQ(file.recording.at("truncated"), "false");
  EXPECT_EQ(file.recording.at("messages"), "8");

  // Recording observes ingest, it never diverts it: every push landed.
  EXPECT_EQ(objectEntryCount("/before"), 9u);
  EXPECT_EQ(objectEntryCount("/during"), 2u);
}

// One press, two live sources: two files in one folder, each holding only its
// own topics and both stamped with the same capture.
TEST_F(RecordingPipelineTest, EachSourceGetsItsOwnFileInOneCaptureFolder) {
  auto second = makeSecondSource("pipeline_source_two");
  ASSERT_NE(second->host, nullptr);
  const auto first_topic = bindTopic("/first");
  const auto second_topic = second->bindTopic("/second");

  PJ::RecordingService service;
  service.setSettings(temporarySettings());
  QSignalSpy started(&service, &PJ::RecordingService::started);
  QSignalSpy stopped(&service, &PJ::RecordingService::stopped);

  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(recordingTarget());
  targets.push_back(targetFor(*second, 2, "pipeline_source_two", "pipeline_plugin_two"));
  const auto directory = service.start(std::move(targets), "pipeline-test");
  ASSERT_TRUE(directory.has_value()) << directory.error();

  push(first_topic, 10, {0x01});
  push(first_topic, 11, {0x02});
  second->push(second_topic, 20, {0x03});

  service.stop();
  ASSERT_EQ(stopped.count(), 1);  // finalization is synchronous

  const auto sources = startedSources(started);
  ASSERT_EQ(sources.size(), 2);
  EXPECT_EQ(QFileInfo(sources[0].path).fileName(), u"pipeline_source.mcap"_s);
  EXPECT_EQ(QFileInfo(sources[1].path).fileName(), u"pipeline_source_two.mcap"_s);
  EXPECT_EQ(QFileInfo(sources[0].path).absolutePath(), QFileInfo(sources[1].path).absolutePath());

  const PJ::CaptureResult result = capture(stopped);
  EXPECT_EQ(result.clean_count, 2);
  EXPECT_EQ(result.total_messages, 3u);

  const RecordedFile first_file = readAll(sources[0].path);
  const RecordedFile second_file = readAll(sources[1].path);
  ASSERT_EQ(first_file.channels.size(), 1u);
  EXPECT_EQ(first_file.channels.count("/first"), 1u);
  ASSERT_EQ(second_file.channels.size(), 1u);
  EXPECT_EQ(second_file.channels.count("/second"), 1u);
  EXPECT_EQ(first_file.messages.size(), 2u);
  EXPECT_EQ(second_file.messages.size(), 1u);

  // One capture: the same id in both files, and the ordinals of the sorted
  // target order.
  EXPECT_EQ(first_file.recording.at("capture_id"), result.capture_id.toStdString());
  EXPECT_EQ(second_file.recording.at("capture_id"), result.capture_id.toStdString());
  EXPECT_EQ(first_file.recording.at("capture_ordinal"), "1");
  EXPECT_EQ(second_file.recording.at("capture_ordinal"), "2");
  EXPECT_EQ(first_file.recording.at("source_display_name"), kSourceId);
  EXPECT_EQ(second_file.recording.at("source_display_name"), "pipeline_source_two");
  EXPECT_EQ(first_file.recording.at("terminal_cause"), std::string(PJ::kTerminalCauseStopped));
  EXPECT_EQ(second_file.recording.at("terminal_cause"), std::string(PJ::kTerminalCauseStopped));
}

// A second recording over the same live bindings must start from an empty
// channel table: its tap is new, so every binding is unknown again and none of
// the first recording's state may reach the new file.
TEST_F(RecordingPipelineTest, SecondRecordingOnTheSameHostStartsClean) {
  const auto only = bindTopic("/only");

  PJ::RecordingService service;
  service.setSettings(temporarySettings());

  {
    QSignalSpy started(&service, &PJ::RecordingService::started);
    std::vector<PJ::RecordingTarget> first_targets;
    first_targets.push_back(recordingTarget());
    ASSERT_TRUE(service.start(std::move(first_targets), "pipeline-test").has_value());
    push(only, 10, {0x01});
    push(only, 11, {0x02});
    service.stop();
    ASSERT_FALSE(service.isRecording());
  }

  QSignalSpy started(&service, &PJ::RecordingService::started);
  std::vector<PJ::RecordingTarget> second_targets;
  second_targets.push_back(recordingTarget());
  ASSERT_TRUE(service.start(std::move(second_targets), "pipeline-test").has_value());
  push(only, 20, {0x03});
  push(only, 21, {0x04});
  service.stop();
  ASSERT_FALSE(service.isRecording());

  const auto sources = startedSources(started);
  ASSERT_EQ(sources.size(), 1);
  const RecordedFile file = readAll(sources[0].path);
  ASSERT_EQ(file.channels.size(), 1u);
  EXPECT_EQ(file.channels.count("/only"), 1u);
  ASSERT_EQ(file.messages.size(), 2u);
  EXPECT_EQ(file.messages[0].log_time, 20u);
  EXPECT_EQ(file.messages[0].bytes, (std::vector<uint8_t>{0x03}));
  EXPECT_EQ(file.messages[1].log_time, 21u);
  EXPECT_EQ(file.messages[1].bytes, (std::vector<uint8_t>{0x04}));
  EXPECT_EQ(file.recording.at("messages"), "2");
}

}  // namespace
