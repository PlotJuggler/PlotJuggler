// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
// RecordingService: one batch folder per Record press holding one file per
// source — naming and collision handling, the transactional start, per-source
// termination and its exclusive outcome buckets, the serialized finalization
// executor, and the settings round-trip.
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// No MCAP_IMPLEMENTATION here: pj_runtime carries the single implementation TU.
#include <mcap/reader.hpp>

#include "pj_runtime/RecordingFormat.h"
#include "pj_runtime/RecordingService.h"
#include "pj_runtime/RecordingSink.h"
#include "recording_test_utils.h"

using namespace Qt::StringLiterals;

namespace {

/// Spins the event loop until `predicate` holds. Finalization is synchronous,
/// so this is only ever waiting for the service's 500 ms poll timer.
bool waitFor(const std::function<bool()>& predicate, int timeout_ms = 10000) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate()) {
    if (timer.elapsed() > timeout_ms) {
      return false;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

/// Stands in for a streaming source: counts the attaches and the detaches (a
/// null tap), keeps the tap so a test can drive it, and reports a canned
/// pure-lazy skip count. Declare one BEFORE the service that records it — the
/// service's destructor finalizes the capture, which detaches every target.
struct FakeTarget {
  int attach_calls = 0;
  int detach_calls = 0;
  bool attach_succeeds = true;  ///< false: the source vanished between the press and the attach
  bool attach_throws = false;
  uint64_t skipped_lazy = 0;
  std::shared_ptr<PJ::RecordTap> tap;

  PJ::RecordingTarget descriptor(uint64_t target_key, std::string source_id, std::string plugin_id) {
    PJ::RecordingTarget target;
    target.target_key = target_key;
    target.source_id = std::move(source_id);
    target.plugin_id = std::move(plugin_id);
    target.attach = [this](std::shared_ptr<PJ::RecordTap> installed) {
      if (installed == nullptr) {
        ++detach_calls;
        tap.reset();
        return true;
      }
      ++attach_calls;
      if (attach_throws) {
        throw std::runtime_error("the source exploded while taking the tap");
      }
      if (!attach_succeeds) {
        return false;
      }
      tap = std::move(installed);
      return true;
    };
    target.skipped_lazy = [this] { return skipped_lazy; };
    return target;
  }
};

/// The binding view a runtime host carries on each message.
PJ::RecordedBindingView topicBinding() {
  return PJ::RecordedBindingView{
      .topic = "/t",
      .encoding = "json",
      .type_name = "T",
      .schema_bytes = "{}",
  };
}

void pushMessage(const std::shared_ptr<PJ::RecordTap>& tap, int64_t log_time_ns) {
  const std::vector<uint8_t> payload{1, 2, 3};
  (void)tap->onMessage(1, topicBinding(), log_time_ns, PJ::Span<const uint8_t>(payload.data(), payload.size()));
}

/// What one fake sink does. Scripted per creation index, which is the sorted
/// target order the service builds its recorders in.
struct SinkScript {
  std::string fail_open{};
  std::string fail_write{};
  std::string fail_close{};
  bool throw_on_open = false;  ///< open() throws instead of returning, as a sink with a bug would
};

/// Hands out the fake sinks of one test and lets it drive them from the GUI
/// thread while they are used by the service's executor.
class SinkFarm {
 public:
  void setScript(int index, SinkScript script) {
    std::lock_guard lock(mu_);
    scripts_[index] = std::move(script);
  }

  [[nodiscard]] SinkScript take(int index) {
    std::lock_guard lock(mu_);
    const auto found = scripts_.find(index);
    return found == scripts_.end() ? SinkScript{} : found->second;
  }

  [[nodiscard]] int nextIndex() {
    std::lock_guard lock(mu_);
    return created_++;
  }

  void noteCloseEntered() {
    std::lock_guard lock(mu_);
    ++closes_entered_;
  }

  [[nodiscard]] int closesEntered() const {
    std::lock_guard lock(mu_);
    return closes_entered_;
  }

 private:
  mutable std::mutex mu_;
  std::map<int, SinkScript> scripts_;
  int created_ = 0;
  int closes_entered_ = 0;
};

/// A sink whose failures are scripted. It creates the file at open() like the
/// real one, so a test sees the recording appear on disk.
class FakeSink final : public PJ::RecordingSink {
 public:
  FakeSink(std::shared_ptr<SinkFarm> farm, SinkScript script) : farm_(std::move(farm)), script_(std::move(script)) {}

  PJ::Status open(const std::filesystem::path& path, const PJ::RecordingInfo& /*info*/) override {
    if (script_.throw_on_open) {
      throw std::runtime_error("open exploded");
    }
    if (!script_.fail_open.empty()) {
      return PJ::unexpected(script_.fail_open);
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) {
      return PJ::unexpected("fake sink could not create " + path.string());
    }
    file << "fake";
    return PJ::okStatus();
  }
  PJ::Expected<uint16_t> addChannel(const PJ::RecordedBinding& /*binding*/) override {
    return uint16_t{0};
  }
  PJ::Status write(uint16_t /*channel*/, int64_t /*log_time_ns*/, PJ::Span<const uint8_t> /*bytes*/) override {
    return script_.fail_write.empty() ? PJ::okStatus() : PJ::unexpected(script_.fail_write);
  }
  PJ::Status close(const PJ::RecordingSummary& /*summary*/) override {
    farm_->noteCloseEntered();
    return script_.fail_close.empty() ? PJ::okStatus() : PJ::unexpected(script_.fail_close);
  }

 private:
  std::shared_ptr<SinkFarm> farm_;
  SinkScript script_;
};

class RecordingServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(settings_.isValid());
    ASSERT_TRUE(recordings_dir_.isValid());
  }

  /// A service whose recordings land in this test's temporary folder.
  [[nodiscard]] PJ::RecordingService::Settings temporarySettings() const {
    PJ::RecordingService::Settings settings;
    settings.directory = recordings_dir_.path();
    return settings;
  }

  /// Arms `service` with this test's fake sinks and returns the farm driving them.
  std::shared_ptr<SinkFarm> useFakeSinks(PJ::RecordingService& service) {
    auto farm = std::make_shared<SinkFarm>();
    service.setSinkFactoryForTest([farm] {
      const int index = farm->nextIndex();
      return std::make_unique<FakeSink>(farm, farm->take(index));
    });
    return farm;
  }

  /// The batch folders under the recordings folder.
  [[nodiscard]] QStringList captureFolders() const {
    return QDir(recordings_dir_.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
  }

  [[nodiscard]] static QVector<PJ::SourceRecordingStart> startedSources(const QSignalSpy& started) {
    return started.at(0).at(2).value<QVector<PJ::SourceRecordingStart>>();
  }

  [[nodiscard]] static PJ::CaptureResult capture(const QSignalSpy& stopped) {
    return stopped.at(0).at(0).value<PJ::CaptureResult>();
  }

  PJ::test::IsolatedQtSettings settings_{u"PlotJugglerRecordingServiceTest"_s, u"RecordingServiceTest"_s};
  QTemporaryDir recordings_dir_;
};

// Recording is a runtime capability, not a build flag: this binary is built
// only where the MCAP sink is, so the query must agree with the sink it has.
TEST_F(RecordingServiceTest, ThisBuildReportsARecordingSink) {
  EXPECT_TRUE(PJ::RecordingService::isSupported());
}

TEST_F(RecordingServiceTest, StartOpensOneFilePerSourceInOneBatchFolder) {
  FakeTarget first;
  // A live host's pure-lazy counter is already non-zero when Record is pressed;
  // only what it gains while the recording runs belongs to this file.
  first.skipped_lazy = 5;
  FakeTarget second;
  PJ::RecordingService service;
  service.setSettings(temporarySettings());

  QSignalSpy started(&service, &PJ::RecordingService::started);
  QSignalSpy stopped(&service, &PJ::RecordingService::stopped);

  std::vector<PJ::RecordingTarget> targets;
  // Handed over out of order: the service sorts by key, so the ordinals and the
  // reported order do not depend on how the owner enumerated its sources.
  targets.push_back(second.descriptor(7, "src-b", "plugin-b"));
  targets.push_back(first.descriptor(3, "src-a", "plugin-a"));

  const auto directory = service.start(std::move(targets), "test-version");
  ASSERT_TRUE(directory.has_value()) << directory.error();
  EXPECT_TRUE(service.isRecording());

  const QFileInfo folder(*directory);
  EXPECT_EQ(folder.dir().absolutePath(), QDir(recordings_dir_.path()).absolutePath());
  EXPECT_TRUE(QRegularExpression(u"^pj_\\d{8}_\\d{6}$"_s).match(folder.fileName()).hasMatch())
      << folder.fileName().toStdString();

  ASSERT_EQ(started.count(), 1);
  EXPECT_FALSE(started.at(0).at(0).toString().isEmpty());  // the capture id
  EXPECT_EQ(started.at(0).at(1).toString(), *directory);
  const auto sources = startedSources(started);
  ASSERT_EQ(sources.size(), 2);
  EXPECT_EQ(sources[0].target_key, 3u);
  EXPECT_EQ(sources[0].source, u"src-a"_s);
  EXPECT_EQ(sources[1].target_key, 7u);
  EXPECT_EQ(QFileInfo(sources[0].path).fileName(), u"src-a.mcap"_s);
  EXPECT_EQ(QFileInfo(sources[1].path).fileName(), u"src-b.mcap"_s);
  for (const PJ::SourceRecordingStart& source : sources) {
    EXPECT_TRUE(QFileInfo::exists(source.path)) << source.path.toStdString();
  }
  EXPECT_EQ(first.attach_calls, 1);
  EXPECT_EQ(second.attach_calls, 1);

  first.skipped_lazy = 8;  // three pure-lazy pushes this recording could not see

  service.stop();
  ASSERT_EQ(stopped.count(), 1);  // finalization is synchronous
  EXPECT_FALSE(service.isRecording());
  EXPECT_EQ(first.detach_calls, 1);
  EXPECT_EQ(second.detach_calls, 1);

  const PJ::CaptureResult result = capture(stopped);
  EXPECT_EQ(result.directory, *directory);
  EXPECT_EQ(result.capture_id, started.at(0).at(0).toString());
  ASSERT_EQ(result.sources.size(), 2);
  EXPECT_EQ(result.sources[0].source, u"src-a"_s);
  EXPECT_EQ(result.sources[0].outcome, PJ::RecordingOutcome::kLossy);  // its three pure-lazy pushes
  EXPECT_EQ(result.sources[0].skipped_messages, 3u);
  EXPECT_EQ(result.sources[0].terminal_cause, QString::fromUtf8(PJ::kTerminalCauseStopped.data()));
  EXPECT_EQ(result.sources[1].outcome, PJ::RecordingOutcome::kClean);
  EXPECT_TRUE(result.sources[1].reason.isEmpty());
  EXPECT_EQ(result.total_skipped, 3u);
  EXPECT_EQ(result.clean_count, 1);
  EXPECT_EQ(result.lossy_count, 1);
  EXPECT_EQ(result.truncated_count, 0);
  EXPECT_EQ(result.incomplete_count, 0);

  service.stop();  // idempotent
  EXPECT_EQ(stopped.count(), 1);
  EXPECT_EQ(first.detach_calls, 1);
}

TEST_F(RecordingServiceTest, ConsecutiveCapturesNeverShareAFolder) {
  FakeTarget target;
  PJ::RecordingService service;
  service.setSettings(temporarySettings());

  std::vector<PJ::RecordingTarget> first_targets;
  first_targets.push_back(target.descriptor(1, "src-a", "plugin-a"));
  const auto first = service.start(std::move(first_targets), "test-version");
  ASSERT_TRUE(first.has_value()) << first.error();
  service.stop();
  ASSERT_FALSE(service.isRecording());

  // Record -> Stop -> Record inside the same second: the first folder is taken,
  // so the second press must claim another one rather than fail.
  std::vector<PJ::RecordingTarget> second_targets;
  second_targets.push_back(target.descriptor(1, "src-a", "plugin-a"));
  const auto second = service.start(std::move(second_targets), "test-version");
  ASSERT_TRUE(second.has_value()) << second.error();
  service.stop();
  ASSERT_FALSE(service.isRecording());

  EXPECT_NE(*first, *second);
  EXPECT_TRUE(QFileInfo(*first).isDir());
  EXPECT_TRUE(QFileInfo(*second).isDir());
}

// Two sources may present the same display name, or names that only differ in
// what sanitization erases. Each still gets its own file.
TEST_F(RecordingServiceTest, CollidingSourceNamesGetOrdinals) {
  FakeTarget first;
  FakeTarget second;
  FakeTarget third;
  PJ::RecordingService service;
  service.setSettings(temporarySettings());
  QSignalSpy started(&service, &PJ::RecordingService::started);

  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(first.descriptor(1, "a/b", "plugin"));
  targets.push_back(second.descriptor(2, "a:b", "plugin"));  // sanitizes onto the same name
  targets.push_back(third.descriptor(3, "A_B", "plugin"));   // and so does this one, case aside
  ASSERT_TRUE(service.start(std::move(targets), "test-version").has_value());

  const auto sources = startedSources(started);
  ASSERT_EQ(sources.size(), 3);
  EXPECT_EQ(QFileInfo(sources[0].path).fileName(), u"a_b.mcap"_s);
  EXPECT_EQ(QFileInfo(sources[1].path).fileName(), u"a_b_2.mcap"_s);
  EXPECT_EQ(QFileInfo(sources[2].path).fileName(), u"A_B_3.mcap"_s);
  for (const PJ::SourceRecordingStart& source : sources) {
    EXPECT_TRUE(QFileInfo::exists(source.path)) << source.path.toStdString();
  }
  service.stop();
  ASSERT_FALSE(service.isRecording());
}

TEST_F(RecordingServiceTest, SourceNamesAreSanitizedIntoOneFileName) {
  EXPECT_EQ(PJ::RecordingService::sanitizeSourceName(u"ROS2 Stream"_s), u"ROS2_Stream"_s);
  EXPECT_EQ(PJ::RecordingService::sanitizeSourceName(u"/dev/ttyUSB0"_s), u"_dev_ttyUSB0"_s);
  EXPECT_EQ(PJ::RecordingService::sanitizeSourceName(uR"(a\b:c*d?e"f<g>h|i)"_s), u"a_b_c_d_e_f_g_h_i"_s);
  EXPECT_EQ(PJ::RecordingService::sanitizeSourceName(u"tabs\tand   spaces"_s), u"tabs_and_spaces"_s);
  EXPECT_EQ(PJ::RecordingService::sanitizeSourceName(u"trailing dots..."_s), u"trailing_dots"_s);
  // Every Unicode separator folds, not just the spaces: U+2028 LINE SEPARATOR
  // and U+2029 PARAGRAPH SEPARATOR would otherwise reach a file name.
  EXPECT_EQ(PJ::RecordingService::sanitizeSourceName(u"line\u2028sep\u2029end"_s), u"line_sep_end"_s);
  EXPECT_EQ(PJ::RecordingService::sanitizeSourceName(QString()), u"source"_s);
  EXPECT_EQ(PJ::RecordingService::sanitizeSourceName(u"///"_s), u"_"_s);
  // Unicode survives, and the two spellings of one accented name converge.
  const QString decomposed = u"café"_s;
  const QString composed = u"café"_s;
  EXPECT_EQ(PJ::RecordingService::sanitizeSourceName(decomposed), composed);
  EXPECT_EQ(PJ::RecordingService::sanitizeSourceName(composed), composed);

  // Cut at 80 characters. Two long names sharing that prefix reduce to the same
  // file name, which the ordinal (and, last of all, the writer's exclusive
  // create) is what separates.
  const QString long_name = QString(90, u'x') + u"-alpha"_s;
  const QString cut = PJ::RecordingService::sanitizeSourceName(long_name);
  EXPECT_EQ(cut, QString(80, u'x'));
  EXPECT_EQ(cut, PJ::RecordingService::sanitizeSourceName(long_name));
}

TEST_F(RecordingServiceTest, StartRefusesWhenAlreadyRecordingOrFolderUnwritable) {
  FakeTarget target;
  PJ::RecordingService service;
  service.setSettings(temporarySettings());

  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(target.descriptor(1, "src-a", "plugin-a"));
  ASSERT_TRUE(service.start(std::move(targets), "test-version").has_value());

  std::vector<PJ::RecordingTarget> second_targets;
  second_targets.push_back(target.descriptor(1, "src-a", "plugin-a"));
  const auto refused = service.start(std::move(second_targets), "test-version");
  EXPECT_FALSE(refused.has_value());
  EXPECT_EQ(target.attach_calls, 1);

  service.stop();
  ASSERT_FALSE(service.isRecording());

  // A folder under a regular file can never be created, root included, and the
  // attempt stays inside the temporary tree.
  const QString blocking_file = QDir(recordings_dir_.path()).absoluteFilePath(u"not-a-dir"_s);
  {
    std::ofstream file(blocking_file.toStdString());
    ASSERT_TRUE(file.good());
  }
  PJ::RecordingService::Settings unwritable;
  unwritable.directory = blocking_file + u"/sub"_s;
  service.setSettings(unwritable);
  std::vector<PJ::RecordingTarget> blocked_targets;
  blocked_targets.push_back(target.descriptor(1, "src-a", "plugin-a"));
  const auto failed = service.start(std::move(blocked_targets), "test-version");
  EXPECT_FALSE(failed.has_value());
  EXPECT_FALSE(service.isRecording());
}

TEST_F(RecordingServiceTest, StartRefusesAnEmptyBatchATargetWithoutAttachAndDuplicateKeys) {
  PJ::RecordingService service;
  service.setSettings(temporarySettings());

  EXPECT_FALSE(service.start({}, "test-version").has_value());

  PJ::RecordingTarget incomplete;
  incomplete.target_key = 1;
  incomplete.source_id = "src-a";
  incomplete.plugin_id = "plugin-a";
  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(std::move(incomplete));
  const auto no_attach = service.start(std::move(targets), "test-version");
  ASSERT_FALSE(no_attach.has_value());
  EXPECT_NE(no_attach.error().find("src-a"), std::string::npos) << no_attach.error();

  FakeTarget first;
  FakeTarget second;
  std::vector<PJ::RecordingTarget> duplicated;
  duplicated.push_back(first.descriptor(4, "src-a", "plugin-a"));
  duplicated.push_back(second.descriptor(4, "src-b", "plugin-b"));
  const auto duplicate_key = service.start(std::move(duplicated), "test-version");
  ASSERT_FALSE(duplicate_key.has_value());
  EXPECT_NE(duplicate_key.error().find("share the key"), std::string::npos) << duplicate_key.error();

  EXPECT_FALSE(service.isRecording());
  EXPECT_TRUE(captureFolders().isEmpty());  // nothing was created for any of them
}

// A source that vanished between the press and the attach fails the WHOLE
// capture: half a batch would record some sources and silently miss others.
TEST_F(RecordingServiceTest, AFailedAttachRollsTheWholeCaptureBack) {
  FakeTarget first;
  FakeTarget vanished;
  vanished.attach_succeeds = false;
  PJ::RecordingService service;
  service.setSettings(temporarySettings());
  QSignalSpy started(&service, &PJ::RecordingService::started);

  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(first.descriptor(1, "src-a", "plugin-a"));
  targets.push_back(vanished.descriptor(2, "src-gone", "plugin-b"));
  const auto failed = service.start(std::move(targets), "test-version");

  ASSERT_FALSE(failed.has_value());
  EXPECT_NE(failed.error().find("src-gone"), std::string::npos) << failed.error();
  EXPECT_EQ(started.count(), 0);
  EXPECT_FALSE(service.isRecording());
  // The failing target is detached too: it may have installed the tap before
  // reporting failure.
  EXPECT_EQ(first.detach_calls, 1);
  EXPECT_EQ(vanished.detach_calls, 1);
  EXPECT_TRUE(captureFolders().isEmpty());
}

TEST_F(RecordingServiceTest, AnAttachThatThrowsCountsAsAFailedAttach) {
  FakeTarget exploding;
  exploding.attach_throws = true;
  PJ::RecordingService service;
  service.setSettings(temporarySettings());

  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(exploding.descriptor(1, "src-boom", "plugin-a"));
  const auto failed = service.start(std::move(targets), "test-version");

  ASSERT_FALSE(failed.has_value());
  EXPECT_NE(failed.error().find("src-boom"), std::string::npos) << failed.error();
  EXPECT_FALSE(service.isRecording());
  EXPECT_TRUE(captureFolders().isEmpty());
}

// The same rollback from the other failure point: the second recorder cannot
// open its file, so the first one's file must not survive either.
TEST_F(RecordingServiceTest, ARecorderThatCannotOpenRollsTheWholeCaptureBack) {
  FakeTarget first;
  FakeTarget second;
  PJ::RecordingService service;
  service.setSettings(temporarySettings());
  auto farm = useFakeSinks(service);
  farm->setScript(1, SinkScript{.fail_open = "permission denied"});

  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(first.descriptor(1, "src-a", "plugin-a"));
  targets.push_back(second.descriptor(2, "src-b", "plugin-b"));
  const auto failed = service.start(std::move(targets), "test-version");

  ASSERT_FALSE(failed.has_value());
  EXPECT_NE(failed.error().find("permission denied"), std::string::npos) << failed.error();
  EXPECT_EQ(first.attach_calls, 0);  // nothing is attached until every file is open
  EXPECT_EQ(first.detach_calls, 1);
  EXPECT_FALSE(service.isRecording());
  EXPECT_TRUE(captureFolders().isEmpty());
}

// One source truncating does not end the capture: its file is finalized with
// that outcome while every other source keeps recording.
TEST_F(RecordingServiceTest, ATruncatingSourceIsFinalizedAloneAndTheBatchContinues) {
  FakeTarget healthy;
  FakeTarget failing;
  PJ::RecordingService service;
  service.setSettings(temporarySettings());
  auto farm = useFakeSinks(service);
  farm->setScript(1, SinkScript{.fail_write = "disk full"});
  QSignalSpy started(&service, &PJ::RecordingService::started);
  QSignalSpy stopped(&service, &PJ::RecordingService::stopped);

  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(healthy.descriptor(1, "src-ok", "plugin-a"));
  targets.push_back(failing.descriptor(2, "src-bad", "plugin-b"));
  ASSERT_TRUE(service.start(std::move(targets), "test-version").has_value());
  ASSERT_NE(healthy.tap, nullptr);
  ASSERT_NE(failing.tap, nullptr);

  pushMessage(healthy.tap, 1000);
  pushMessage(failing.tap, 1000);

  // The poll notices the truncation and finalizes that source alone.
  ASSERT_TRUE(waitFor([&farm] { return farm->closesEntered() == 1; }));
  EXPECT_TRUE(service.isRecording());
  EXPECT_EQ(stopped.count(), 0);
  EXPECT_EQ(failing.detach_calls, 1);
  EXPECT_EQ(healthy.detach_calls, 0);

  pushMessage(healthy.tap, 2000);  // the healthy source is untouched
  service.stop();
  ASSERT_EQ(stopped.count(), 1);  // finalization is synchronous

  const PJ::CaptureResult result = capture(stopped);
  ASSERT_EQ(result.sources.size(), 2);
  EXPECT_EQ(result.clean_count, 1);
  EXPECT_EQ(result.truncated_count, 1);
  EXPECT_EQ(result.lossy_count, 0);
  EXPECT_EQ(result.incomplete_count, 0);
  EXPECT_EQ(result.sources[0].outcome, PJ::RecordingOutcome::kClean);
  EXPECT_EQ(result.sources[0].messages, 2u);
  EXPECT_EQ(result.sources[1].outcome, PJ::RecordingOutcome::kTruncated);
  EXPECT_EQ(result.sources[1].terminal_cause, QString::fromUtf8(PJ::kTerminalCauseTruncated.data()));
  EXPECT_NE(result.sources[1].reason.indexOf(u"disk full"_s), -1) << result.sources[1].reason.toStdString();
  // Both files are still on disk: a truncation ends the data, not the file.
  const auto sources = startedSources(started);
  for (const PJ::SourceRecordingStart& source : sources) {
    EXPECT_TRUE(QFileInfo::exists(source.path)) << source.path.toStdString();
  }
}

// Two sources report the end and the user then presses Stop: every source is
// finalized exactly once, and one capture is published, not three. The empty
// reason is what an orderly requested stream stop reports — it must leave that
// source clean, which is why a requested stop never lands in the host's
// lastError().
TEST_F(RecordingServiceTest, EndedSourcesAndAGlobalStopProduceExactlyOneResultEach) {
  FakeTarget first;
  FakeTarget second;
  FakeTarget third;
  PJ::RecordingService service;
  service.setSettings(temporarySettings());
  auto farm = useFakeSinks(service);
  QSignalSpy stopped(&service, &PJ::RecordingService::stopped);

  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(first.descriptor(1, "src-a", "plugin-a"));
  targets.push_back(second.descriptor(2, "src-b", "plugin-b"));
  targets.push_back(third.descriptor(3, "src-c", "plugin-c"));
  ASSERT_TRUE(service.start(std::move(targets), "test-version").has_value());

  service.onSourceEnded(1, u"stream ended"_s);
  service.onSourceEnded(2, QString());
  EXPECT_EQ(farm->closesEntered(), 2);   // each ended source closed its own file
  service.onSourceEnded(1, u"again"_s);  // a repeat must not reopen or re-close it
  EXPECT_EQ(farm->closesEntered(), 2);
  EXPECT_TRUE(service.isRecording());  // the third source is still recording
  EXPECT_EQ(stopped.count(), 0);

  service.stop();
  service.stop();  // idempotent
  EXPECT_EQ(stopped.count(), 1);
  EXPECT_EQ(farm->closesEntered(), 3);
  EXPECT_FALSE(service.isRecording());

  const PJ::CaptureResult result = capture(stopped);
  ASSERT_EQ(result.sources.size(), 3);
  EXPECT_EQ(result.sources[0].terminal_cause, QString::fromUtf8(PJ::kTerminalCauseSourceEnded.data()));
  EXPECT_EQ(result.sources[1].terminal_cause, QString::fromUtf8(PJ::kTerminalCauseSourceEnded.data()));
  EXPECT_EQ(result.sources[2].terminal_cause, QString::fromUtf8(PJ::kTerminalCauseStopped.data()));
  // The reason of the FIRST end is what is reported; the repeat changes nothing.
  EXPECT_EQ(result.sources[0].outcome, PJ::RecordingOutcome::kLossy);
  EXPECT_NE(result.sources[0].reason.indexOf(u"stream ended"_s), -1) << result.sources[0].reason.toStdString();
  EXPECT_EQ(result.sources[0].reason.indexOf(u"again"_s), -1) << result.sources[0].reason.toStdString();
  // An orderly end (no reason) leaves the source clean.
  EXPECT_EQ(result.sources[1].outcome, PJ::RecordingOutcome::kClean);
  EXPECT_EQ(result.sources[2].outcome, PJ::RecordingOutcome::kClean);
  EXPECT_EQ(first.detach_calls, 1);
  EXPECT_EQ(second.detach_calls, 1);
  EXPECT_EQ(third.detach_calls, 1);
}

// A user Stop landing on the same tick as the poll's truncation must not
// finalize that recorder twice, nor report it twice.
TEST_F(RecordingServiceTest, APollDetectedTruncationRacingStopStillReportsOnce) {
  FakeTarget target;
  PJ::RecordingService service;
  service.setSettings(temporarySettings());
  auto farm = useFakeSinks(service);
  farm->setScript(0, SinkScript{.fail_write = "disk full"});
  QSignalSpy stopped(&service, &PJ::RecordingService::stopped);

  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(target.descriptor(1, "src-a", "plugin-a"));
  ASSERT_TRUE(service.start(std::move(targets), "test-version").has_value());
  ASSERT_NE(target.tap, nullptr);

  pushMessage(target.tap, 1000);
  service.stop();  // races whatever the poll is about to notice
  ASSERT_TRUE(waitFor([&stopped] { return stopped.count() == 1; }));

  const PJ::CaptureResult result = capture(stopped);
  ASSERT_EQ(result.sources.size(), 1);
  EXPECT_EQ(result.sources[0].outcome, PJ::RecordingOutcome::kTruncated);
  EXPECT_EQ(result.truncated_count, 1);
  EXPECT_EQ(target.detach_calls, 1);
  // Let the poll run once more: nothing may follow a published capture.
  ASSERT_FALSE(waitFor([&stopped] { return stopped.count() > 1; }, 800));
}

// The whole Record -> Stop -> Record cycle can run inside one poll's progress
// slot, and the next capture may reuse the same target keys. The poll must not
// carry its truncation verdict across that boundary and finalize a capture it
// never read.
TEST_F(RecordingServiceTest, APollThatOutlivesItsCaptureNeverTouchesTheNextOne) {
  FakeTarget target;
  PJ::RecordingService service;
  service.setSettings(temporarySettings());
  auto farm = useFakeSinks(service);
  farm->setScript(0, SinkScript{.fail_write = "disk full"});  // the FIRST capture truncates
  QSignalSpy stopped(&service, &PJ::RecordingService::stopped);

  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(target.descriptor(1, "src-a", "plugin-a"));
  ASSERT_TRUE(service.start(std::move(targets), "test-version").has_value());
  ASSERT_NE(target.tap, nullptr);
  pushMessage(target.tap, 1000);

  // Stop from the progress slot, and start the next capture over the same key
  // from the stopped slot: exactly what a Stop/Record double-press does.
  int stops = 0;
  int restarts = 0;
  QObject::connect(&service, &PJ::RecordingService::progress, &service, [&service, &stops] {
    if (stops++ == 0) {
      service.stop();  // once: the second capture must be left to run
    }
  });
  QObject::connect(&service, &PJ::RecordingService::stopped, &service, [&service, &target, &restarts] {
    if (restarts++ > 0) {
      return;
    }
    std::vector<PJ::RecordingTarget> again;
    again.push_back(target.descriptor(1, "src-a", "plugin-a"));
    EXPECT_TRUE(service.start(std::move(again), "test-version").has_value());
  });

  ASSERT_TRUE(waitFor([&stopped] { return stopped.count() == 1; }));
  EXPECT_EQ(capture(stopped).sources.at(0).outcome, PJ::RecordingOutcome::kTruncated);

  // The second capture is untouched by the poll that published the first: it is
  // still running, and let another poll or two go by to prove it stays that way.
  EXPECT_TRUE(service.isRecording());
  ASSERT_FALSE(waitFor([&stopped] { return stopped.count() > 1; }, 1200));
  EXPECT_TRUE(service.isRecording());
  EXPECT_EQ(restarts, 1);

  service.stop();
  ASSERT_EQ(stopped.count(), 2);
  const PJ::CaptureResult second = stopped.at(1).at(0).value<PJ::CaptureResult>();
  ASSERT_EQ(second.sources.size(), 1);
  EXPECT_EQ(second.sources[0].outcome, PJ::RecordingOutcome::kClean);
  EXPECT_EQ(second.truncated_count, 0);
}

// Aggregated progress is live recorders plus the FROZEN results of the finished
// ones, so a source retiring never makes the totals step backwards.
TEST_F(RecordingServiceTest, AggregateProgressNeverDecreases) {
  FakeTarget first;
  FakeTarget second;
  PJ::RecordingService service;
  service.setSettings(temporarySettings());
  QSignalSpy progress(&service, &PJ::RecordingService::progress);

  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(first.descriptor(1, "src-a", "plugin-a"));
  targets.push_back(second.descriptor(2, "src-b", "plugin-b"));
  ASSERT_TRUE(service.start(std::move(targets), "test-version").has_value());
  for (int index = 0; index < 4; ++index) {
    pushMessage(first.tap, 1000 + index);
    pushMessage(second.tap, 1000 + index);
  }

  ASSERT_TRUE(waitFor([&progress] { return !progress.isEmpty() && progress.back().at(0).toULongLong() == 8u; }));
  const qsizetype emissions_before_end = progress.count();
  service.onSourceEnded(1, QString());
  // One source has retired; its counters must survive in the totals.
  ASSERT_TRUE(waitFor([&progress, emissions_before_end] { return progress.count() > emissions_before_end; }));

  quint64 highest_messages = 0;
  quint64 highest_payload = 0;
  quint64 highest_dropped = 0;
  for (const QList<QVariant>& emission : progress) {
    const auto messages = emission.at(0).toULongLong();
    const auto payload_bytes = emission.at(1).toULongLong();
    const auto dropped = emission.at(2).toULongLong();
    EXPECT_GE(messages, highest_messages);
    EXPECT_GE(payload_bytes, highest_payload);
    EXPECT_GE(dropped, highest_dropped);
    highest_messages = messages;
    highest_payload = payload_bytes;
    highest_dropped = dropped;
  }
  EXPECT_EQ(progress.back().at(0).toULongLong(), 8u);

  service.stop();
  ASSERT_FALSE(service.isRecording());
}

TEST_F(RecordingServiceTest, SettingsRoundTripThroughQSettings) {
  PJ::RecordingService::Settings settings;
  settings.directory = recordings_dir_.path();
  settings.queue_budget_mib = 16;
  PJ::RecordingService::saveSettings(settings);

  const auto loaded = PJ::RecordingService::loadSettings();
  EXPECT_EQ(loaded.directory, settings.directory);
  EXPECT_EQ(loaded.queue_budget_mib, 16);

  QSettings raw;
  EXPECT_EQ(raw.value(u"Preferences::recording_directory"_s).toString(), settings.directory);
  EXPECT_EQ(raw.value(u"Preferences::recording_queue_budget_mib"_s).toInt(), 16);
}

// A hand-edited .ini must not be able to arm a 1 TiB queue.
TEST_F(RecordingServiceTest, LoadedSettingsAreClampedToTheAcceptedRanges) {
  {
    QSettings raw;
    raw.setValue(u"Preferences::recording_queue_budget_mib"_s, 1'000'000);
  }
  EXPECT_EQ(PJ::RecordingService::loadSettings().queue_budget_mib, 4096);

  {
    QSettings raw;
    raw.setValue(u"Preferences::recording_queue_budget_mib"_s, 0);
  }
  EXPECT_EQ(PJ::RecordingService::loadSettings().queue_budget_mib, 8);
}

TEST_F(RecordingServiceTest, RecordedFileIsAValidMcapCarryingItsCaptureIdentity) {
  FakeTarget target;
  PJ::RecordingService service;
  service.setSettings(temporarySettings());
  QSignalSpy started(&service, &PJ::RecordingService::started);
  QSignalSpy stopped(&service, &PJ::RecordingService::stopped);

  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(target.descriptor(9, "src-a", "plugin-a"));
  ASSERT_TRUE(service.start(std::move(targets), "test-version").has_value());
  ASSERT_NE(target.tap, nullptr);
  pushMessage(target.tap, 1000);

  service.stop();
  ASSERT_EQ(stopped.count(), 1);  // finalization is synchronous
  const auto sources = startedSources(started);
  ASSERT_EQ(sources.size(), 1);
  ASSERT_TRUE(QFileInfo::exists(sources[0].path));

  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(sources[0].path.toStdString()).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).ok());

  const auto channels = reader.channels();  // returned by value; snapshot before iterating
  ASSERT_EQ(channels.size(), 1u);
  const auto& channel = channels.begin()->second;
  EXPECT_EQ(channel->topic, "/t");
  EXPECT_EQ(channel->messageEncoding, "json");
  EXPECT_TRUE(channel->metadata.empty());

  size_t message_count = 0;
  for (const auto& view : reader.readMessages()) {
    EXPECT_EQ(view.message.logTime, 1000u);
    ++message_count;
  }
  EXPECT_EQ(message_count, 1u);

  const auto recording = PJ::test::lastRecordingRecord(reader);
  EXPECT_EQ(recording.at("app_version"), "test-version");
  EXPECT_EQ(recording.at("source_display_name"), "src-a");
  EXPECT_EQ(recording.at("source_plugin_id"), "plugin-a");
  EXPECT_EQ(recording.at("capture_ordinal"), "1");
  EXPECT_EQ(recording.at("capture_id"), started.at(0).at(0).toString().toStdString());
  EXPECT_EQ(recording.at("terminal_cause"), std::string(PJ::kTerminalCauseStopped));
}

// A source that produced nothing still gets its file: an empty recording is a
// fact about the session, not a failure.
TEST_F(RecordingServiceTest, ASourceThatRecordedNothingKeepsACleanFile) {
  FakeTarget target;
  PJ::RecordingService service;
  service.setSettings(temporarySettings());
  QSignalSpy stopped(&service, &PJ::RecordingService::stopped);

  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(target.descriptor(1, "src-quiet", "plugin-a"));
  ASSERT_TRUE(service.start(std::move(targets), "test-version").has_value());
  service.stop();
  ASSERT_EQ(stopped.count(), 1);  // finalization is synchronous

  const PJ::CaptureResult result = capture(stopped);
  ASSERT_EQ(result.sources.size(), 1);
  EXPECT_EQ(result.sources[0].messages, 0u);
  EXPECT_EQ(result.sources[0].outcome, PJ::RecordingOutcome::kClean);
  EXPECT_EQ(result.clean_count, 1);
  EXPECT_TRUE(QFileInfo::exists(result.sources[0].path));
}

// A quit that never reaches stop(): the destructor must still finalize every
// file, without an event loop and without emitting anything.
TEST_F(RecordingServiceTest, TheDestructorFinalizesAnActiveCaptureDurably) {
  FakeTarget target;  // outlives the service, as a live runtime host does
  QString path;
  {
    PJ::RecordingService service;
    service.setSettings(temporarySettings());
    QSignalSpy started(&service, &PJ::RecordingService::started);
    QSignalSpy stopped(&service, &PJ::RecordingService::stopped);

    std::vector<PJ::RecordingTarget> targets;
    targets.push_back(target.descriptor(1, "src-a", "plugin-a"));
    ASSERT_TRUE(service.start(std::move(targets), "test-version").has_value());
    ASSERT_NE(target.tap, nullptr);
    pushMessage(target.tap, 1000);
    path = startedSources(started)[0].path;
    EXPECT_EQ(stopped.count(), 0);
  }
  EXPECT_EQ(target.detach_calls, 1);

  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(path.toStdString()).ok());
  // NoFallbackScan: a durably finalized file carries its own summary section.
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan).ok());
  const auto recording = PJ::test::lastRecordingRecord(reader);
  EXPECT_EQ(recording.at("terminal_cause"), std::string(PJ::kTerminalCauseShutdown));
  EXPECT_EQ(recording.at("messages"), "1");
}

TEST_F(RecordingServiceTest, AnUnclosableSinkAutoStopsAndReportsAnUnfinalizedFile) {
  FakeTarget target;
  PJ::RecordingService service;
  service.setSettings(temporarySettings());
  auto farm = useFakeSinks(service);
  // What a full disk does to a real sink: the write fails, and so does the
  // finalize that follows.
  farm->setScript(0, SinkScript{.fail_write = "fake sink write failed", .fail_close = "fake sink write failed"});
  QSignalSpy started(&service, &PJ::RecordingService::started);
  QSignalSpy stopped(&service, &PJ::RecordingService::stopped);

  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(target.descriptor(1, "src-a", "plugin-a"));
  ASSERT_TRUE(service.start(std::move(targets), "test-version").has_value());
  ASSERT_NE(target.tap, nullptr);
  pushMessage(target.tap, 1000);

  // Nobody calls stop(): the poll must notice the recorder truncated itself.
  ASSERT_TRUE(waitFor([&stopped] { return stopped.count() == 1; }));
  EXPECT_FALSE(service.isRecording());
  EXPECT_EQ(target.detach_calls, 1);

  const PJ::CaptureResult result = capture(stopped);
  ASSERT_EQ(result.sources.size(), 1);
  EXPECT_EQ(result.sources[0].outcome, PJ::RecordingOutcome::kIncomplete);
  EXPECT_EQ(result.incomplete_count, 1);
  EXPECT_EQ(result.truncated_count, 0);  // the buckets are exclusive
  EXPECT_FALSE(result.sources[0].reason.isEmpty());
  EXPECT_EQ(result.sources[0].path, startedSources(started)[0].path);
  EXPECT_TRUE(QFileInfo::exists(result.sources[0].path));  // what was written is still there
}

// A sink whose open() throws instead of returning an error is one failed
// source like any other: start() returns the error, rolls the whole capture
// back (every target detached, the file the first sink created and the batch
// folder removed) and emits nothing.
TEST_F(RecordingServiceTest, ARecorderWhoseOpenThrowsRollsTheWholeCaptureBack) {
  FakeTarget first;
  FakeTarget second;
  PJ::RecordingService service;
  service.setSettings(temporarySettings());
  auto farm = useFakeSinks(service);
  farm->setScript(1, SinkScript{.throw_on_open = true});  // the FIRST sink opens its file fine
  QSignalSpy started(&service, &PJ::RecordingService::started);

  std::vector<PJ::RecordingTarget> targets;
  targets.push_back(first.descriptor(1, "src-a", "plugin-a"));
  targets.push_back(second.descriptor(2, "src-b", "plugin-b"));
  PJ::Expected<QString> outcome = PJ::unexpected("start() did not return");
  EXPECT_NO_THROW(outcome = service.start(std::move(targets), "test-version"))
      << "the sink's open() exception escaped RecordingService::start()";
  EXPECT_FALSE(outcome.has_value());

  EXPECT_EQ(started.count(), 0);
  EXPECT_FALSE(service.isRecording());
  EXPECT_EQ(first.attach_calls, 0);  // nothing is attached until every file is open
  EXPECT_EQ(first.detach_calls, 1) << "rollback never ran: the first target was not detached";
  EXPECT_EQ(second.detach_calls, 1) << "rollback never ran: the second target was not detached";
  EXPECT_TRUE(captureFolders().isEmpty())
      << "rollback never ran: the batch folder and the first source's file were left behind";
}

}  // namespace
