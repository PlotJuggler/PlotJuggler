// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "pj_runtime/ActiveWidgetsMarker.h"

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

using Entry = ActiveWidgetsMarker::Entry;

// Pids the probe below treats as gone; anything else is a live process.
constexpr qint64 kDeadPid = 1001;
constexpr qint64 kOtherDeadPid = 1002;
constexpr qint64 kLivePid = 2001;

bool onlyLivePidIsAlive(qint64 pid) {
  return pid == kLivePid;
}

class ActiveWidgetsMarkerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.isValid());
    records_ = dir_.filePath(u"nested"_s);
  }

  [[nodiscard]] QString recordOf(qint64 pid) const {
    return QDir(records_).filePath(ActiveWidgetsMarker::fileNameFor(pid));
  }

  void writeRaw(const QString& path, const QByteArray& bytes) {
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
  }

  /// What a run of `pid` that died with `plugin_id`/`type` live leaves on disk.
  void plant(qint64 pid, const QString& plugin_id, const QString& type) const {
    ActiveWidgetsMarker run(records_, pid);
    const int panel = 0;
    run.add(&panel, plugin_id, type);
    // The process "dies" here: no remove, no clear.
  }

  QTemporaryDir dir_;
  QString records_;
};

TEST_F(ActiveWidgetsMarkerTest, TheFileIsNamedAfterThisProcess) {
  const ActiveWidgetsMarker marker(records_);
  EXPECT_EQ(QFileInfo(marker.path()).fileName(), u"active_widgets.%1.json"_s.arg(ActiveWidgetsMarker::currentPid()));
  EXPECT_EQ(marker.directory(), records_);
}

TEST_F(ActiveWidgetsMarkerTest, LivePanelsAreOnDiskAndTheLastReleaseRemovesTheFile) {
  ActiveWidgetsMarker marker(records_);
  int first = 0;
  int second = 0;
  int third = 0;
  marker.add(&first, u"gauges"_s, u"needle"_s);
  marker.add(&second, u"gauges"_s, u"needle"_s);
  marker.add(&third, u"tables"_s, u"grid"_s);

  ActiveWidgetsMarker::ReadResult read = ActiveWidgetsMarker::read(marker.path());
  EXPECT_TRUE(read.error.isEmpty());
  EXPECT_EQ(read.entries, (std::vector<Entry>{{u"gauges"_s, u"needle"_s}, {u"tables"_s, u"grid"_s}}))
      << "one entry per distinct widget";

  marker.remove(&third);
  EXPECT_EQ(ActiveWidgetsMarker::read(marker.path()).entries, (std::vector<Entry>{{u"gauges"_s, u"needle"_s}}));
  marker.remove(&first);
  EXPECT_TRUE(QFile::exists(marker.path())) << "the second panel of the same widget is still live";
  marker.remove(&second);
  EXPECT_FALSE(QFile::exists(marker.path()));
}

TEST_F(ActiveWidgetsMarkerTest, ReAddingAnOwnerReplacesItsEntry) {
  ActiveWidgetsMarker marker(records_);
  int panel = 0;
  marker.add(&panel, u"gauges"_s, u"needle"_s);
  marker.add(&panel, u"tables"_s, u"grid"_s);
  EXPECT_EQ(ActiveWidgetsMarker::read(marker.path()).entries, (std::vector<Entry>{{u"tables"_s, u"grid"_s}}));
}

TEST_F(ActiveWidgetsMarkerTest, ClearDeletesOnlyThisProcessesFile) {
  plant(kLivePid, u"other_instance"_s, u"gauge"_s);
  ActiveWidgetsMarker marker(records_);
  int panel = 0;
  marker.add(&panel, u"gauges"_s, u"needle"_s);
  marker.clear();
  EXPECT_FALSE(QFile::exists(marker.path()));
  EXPECT_TRUE(QFile::exists(recordOf(kLivePid))) << "another instance's record is not this process's to erase";
  marker.remove(&panel);
  EXPECT_FALSE(QFile::exists(marker.path())) << "a release after the clean exit writes nothing";
}

TEST_F(ActiveWidgetsMarkerTest, TakeConsumesTheRecordOfADeadRunExactlyOnce) {
  plant(kDeadPid, u"gauges"_s, u"needle"_s);
  const ActiveWidgetsMarker::ReadResult taken = ActiveWidgetsMarker::take(records_, &onlyLivePidIsAlive);
  EXPECT_TRUE(taken.error.isEmpty());
  EXPECT_EQ(taken.entries, (std::vector<Entry>{{u"gauges"_s, u"needle"_s}}));
  EXPECT_FALSE(QFile::exists(recordOf(kDeadPid)));
  EXPECT_TRUE(ActiveWidgetsMarker::take(records_, &onlyLivePidIsAlive).entries.empty());
}

TEST_F(ActiveWidgetsMarkerTest, TakeLeavesTheRecordOfALiveProcessAlone) {
  plant(kLivePid, u"running"_s, u"gauge"_s);
  const ActiveWidgetsMarker::ReadResult taken = ActiveWidgetsMarker::take(records_, &onlyLivePidIsAlive);
  EXPECT_TRUE(taken.entries.empty()) << "a running instance did not crash";
  EXPECT_TRUE(QFile::exists(recordOf(kLivePid)));
}

TEST_F(ActiveWidgetsMarkerTest, TakeMergesEveryDeadRunAndSkipsTheLiveOnes) {
  plant(kDeadPid, u"gauges"_s, u"needle"_s);
  {
    ActiveWidgetsMarker run(records_, kOtherDeadPid);
    const int a = 0;
    const int b = 0;
    run.add(&a, u"gauges"_s, u"needle"_s);
    run.add(&b, u"tables"_s, u"grid"_s);
  }
  plant(kLivePid, u"running"_s, u"gauge"_s);
  writeRaw(QDir(records_).filePath(u"active_widgets.json"_s), "{}");  // no pid: not a record
  writeRaw(QDir(records_).filePath(u"active_widgets.x1.json"_s), "{}");

  const ActiveWidgetsMarker::ReadResult taken = ActiveWidgetsMarker::take(records_, &onlyLivePidIsAlive);
  EXPECT_TRUE(taken.error.isEmpty()) << taken.error.toStdString();
  EXPECT_EQ(taken.entries, (std::vector<Entry>{{u"gauges"_s, u"needle"_s}, {u"tables"_s, u"grid"_s}}))
      << "one review for every dead run, each widget once";
  EXPECT_FALSE(QFile::exists(recordOf(kDeadPid)));
  EXPECT_FALSE(QFile::exists(recordOf(kOtherDeadPid)));
  EXPECT_TRUE(QFile::exists(recordOf(kLivePid)));
}

TEST_F(ActiveWidgetsMarkerTest, ThisProcessIsAliveAndAnImpossiblePidIsNot) {
  EXPECT_TRUE(ActiveWidgetsMarker::isProcessAlive(ActiveWidgetsMarker::currentPid()));
  // Above every platform's pid range (Linux caps pid_max at 2^22).
  EXPECT_FALSE(ActiveWidgetsMarker::isProcessAlive(0x7FFFFFF0));
}

TEST_F(ActiveWidgetsMarkerTest, TheDefaultProbeSkipsThisProcessesOwnRecord) {
  ActiveWidgetsMarker marker(records_);
  int panel = 0;
  marker.add(&panel, u"gauges"_s, u"needle"_s);
  EXPECT_TRUE(ActiveWidgetsMarker::take(records_).entries.empty());
  EXPECT_TRUE(QFile::exists(marker.path()));
}

TEST_F(ActiveWidgetsMarkerTest, MissingFileMeansNoCulpritsAndNoError) {
  const ActiveWidgetsMarker::ReadResult read = ActiveWidgetsMarker::read(recordOf(kDeadPid));
  EXPECT_TRUE(read.entries.empty());
  EXPECT_TRUE(read.error.isEmpty());
  EXPECT_TRUE(ActiveWidgetsMarker::take(records_, &onlyLivePidIsAlive).error.isEmpty()) << "no directory either";
}

TEST_F(ActiveWidgetsMarkerTest, MalformedFileMeansNoCulpritsAndAnError) {
  for (const QByteArray& bytes :
       {QByteArray("{\"widgets\": [ truncated"), QByteArray("[1, 2]"), QByteArray("{\"widgets\": 3}")}) {
    writeRaw(recordOf(kDeadPid), bytes);
    const ActiveWidgetsMarker::ReadResult read = ActiveWidgetsMarker::read(recordOf(kDeadPid));
    EXPECT_TRUE(read.entries.empty()) << bytes.constData();
    EXPECT_FALSE(read.error.isEmpty()) << bytes.constData();
    const ActiveWidgetsMarker::ReadResult taken = ActiveWidgetsMarker::take(records_, &onlyLivePidIsAlive);
    EXPECT_FALSE(taken.error.isEmpty()) << bytes.constData();
    EXPECT_FALSE(QFile::exists(recordOf(kDeadPid))) << "an unusable record is consumed too";
  }
}

TEST_F(ActiveWidgetsMarkerTest, EntriesWithoutAPluginIdAreSkipped) {
  writeRaw(recordOf(kDeadPid), R"({"version":1,"widgets":[{"type":"orphan"},{"plugin":"gauges","type":"needle"},7]})");
  const ActiveWidgetsMarker::ReadResult read = ActiveWidgetsMarker::read(recordOf(kDeadPid));
  EXPECT_TRUE(read.error.isEmpty());
  EXPECT_EQ(read.entries, (std::vector<Entry>{{u"gauges"_s, u"needle"_s}}));
}

}  // namespace
}  // namespace PJ
