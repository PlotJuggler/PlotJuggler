// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// UsageCounters: per-UTC-day buckets, persistence across instances, the bounded backlog, and the
// opt-out contract (disabled counts nothing and deletes what was stored).

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include "pj_runtime/UsageCounters.h"

using namespace Qt::StringLiterals;
using PJ::DayUsage;
using PJ::PluginUse;
using PJ::UsageCounters;
using PJ::UsageFeature;

namespace {

// A clock the test moves by hand.
struct FakeClock {
  QDate date{2026, 11, 1};
  std::function<QDate()> fn() {
    return [this]() { return date; };
  }
};

}  // namespace

TEST(UsageCountersTest, DisabledCountsNothing) {
  FakeClock clock;
  UsageCounters counters({}, u"4.1.0"_s, clock.fn());
  counters.count(UsageFeature::kLayoutLoad);
  counters.countPlugin(PluginUse::kToolboxOpen, "pj.toolbox");
  clock.date = clock.date.addDays(1);
  counters.setEnabled(true);
  EXPECT_TRUE(counters.completedDays().empty());
  EXPECT_TRUE(counters.today().features.empty());
}

TEST(UsageCountersTest, CountsLandInTodayAndCompleteAtMidnight) {
  FakeClock clock;
  UsageCounters counters({}, u"4.1.0"_s, clock.fn());
  counters.setEnabled(true);
  counters.count(UsageFeature::kLayoutLoad);
  counters.count(UsageFeature::kLayoutLoad);
  counters.countPlugin(PluginUse::kFileLoad, "pj.mcap");
  counters.countPlugin(PluginUse::kStreamStart, "pj.mcap");
  counters.countPlugin(PluginUse::kParserBind, "");  // ignored
  EXPECT_TRUE(counters.completedDays().empty());

  clock.date = clock.date.addDays(1);
  const std::vector<DayUsage> days = counters.completedDays();
  ASSERT_EQ(days.size(), 1u);
  EXPECT_EQ(days[0].day, QDate(2026, 11, 1));
  EXPECT_EQ(days[0].app_version, u"4.1.0"_s);
  EXPECT_FALSE(days[0].report_id.isEmpty());
  EXPECT_EQ(days[0].features.at("layout.load"), 2);
  EXPECT_EQ(days[0].plugin_uses.size(), 2u);
  EXPECT_EQ((days[0].plugin_uses.at({"data_source", "file", "pj.mcap"})), 1);
  EXPECT_EQ((days[0].plugin_uses.at({"data_source", "stream", "pj.mcap"})), 1);
}

TEST(UsageCountersTest, PersistsAcrossInstancesWithTheSameReportId) {
  QTemporaryDir dir;
  const QString file = dir.filePath(u"usage.json"_s);
  FakeClock clock;
  QString report_id;
  {
    UsageCounters counters(file, u"4.1.0"_s, clock.fn());
    counters.setEnabled(true);
    counters.count(UsageFeature::kRecordingStart);
    counters.setInstalledPlugins({{"pj.mcap", "1.2.0", "data_source"}});
    counters.flush();
    report_id = counters.today().report_id;
  }
  clock.date = clock.date.addDays(1);
  UsageCounters reloaded(file, u"4.2.0"_s, clock.fn());
  reloaded.setEnabled(true);
  const auto days = reloaded.completedDays();
  ASSERT_EQ(days.size(), 1u);
  EXPECT_EQ(days[0].report_id, report_id);     // a retried upload stays deduplicable
  EXPECT_EQ(days[0].app_version, u"4.1.0"_s);  // the version that counted, not the one uploading
  EXPECT_EQ(days[0].features.at("recording.start"), 1);
  ASSERT_EQ(days[0].installed.size(), 1u);
  EXPECT_EQ(days[0].installed[0].version, "1.2.0");
}

TEST(UsageCountersTest, AcknowledgeForgetsOnlyTheSentDays) {
  FakeClock clock;
  UsageCounters counters({}, u"4.1.0"_s, clock.fn());
  counters.setEnabled(true);
  counters.count(UsageFeature::kLayoutSave);
  clock.date = clock.date.addDays(1);
  counters.count(UsageFeature::kLayoutSave);
  clock.date = clock.date.addDays(1);
  counters.count(UsageFeature::kLayoutSave);  // today: never acknowledged

  auto days = counters.completedDays();
  ASSERT_EQ(days.size(), 2u);
  counters.acknowledge({days[0].report_id, counters.today().report_id});
  days = counters.completedDays();
  ASSERT_EQ(days.size(), 1u);
  EXPECT_EQ(days[0].day, QDate(2026, 11, 2));
  EXPECT_EQ(counters.today().features.at("layout.save"), 1);
}

TEST(UsageCountersTest, KeepsAtMostAWeekOfCompletedDays) {
  FakeClock clock;
  UsageCounters counters({}, u"4.1.0"_s, clock.fn());
  counters.setEnabled(true);
  for (int i = 0; i < 12; ++i) {
    counters.count(UsageFeature::kPanelScene3D);
    clock.date = clock.date.addDays(1);
  }
  counters.count(UsageFeature::kPanelScene3D);  // opens today's bucket, which prunes
  const auto days = counters.completedDays();
  ASSERT_EQ(days.size(), static_cast<size_t>(UsageCounters::kMaxStoredDays));
  EXPECT_EQ(days.front().day, clock.date.addDays(-UsageCounters::kMaxStoredDays));
}

TEST(UsageCountersTest, DistinctPluginRowsAreCapped) {
  FakeClock clock;
  UsageCounters counters({}, u"4.1.0"_s, clock.fn());
  counters.setEnabled(true);
  for (size_t i = 0; i < UsageCounters::kMaxPluginRowsPerDay + 10; ++i) {
    counters.countPlugin(PluginUse::kParserBind, "p" + std::to_string(i));
  }
  EXPECT_EQ(counters.today().plugin_uses.size(), UsageCounters::kMaxPluginRowsPerDay);
}

TEST(UsageCountersTest, OptOutDeletesTheStoredFile) {
  QTemporaryDir dir;
  const QString file = dir.filePath(u"usage.json"_s);
  FakeClock clock;
  UsageCounters counters(file, u"4.1.0"_s, clock.fn());
  counters.setEnabled(true);
  counters.count(UsageFeature::kLayoutLoad);
  counters.flush();
  ASSERT_TRUE(QFile::exists(file));
  counters.setEnabled(false);
  EXPECT_FALSE(QFile::exists(file));
  counters.setEnabled(true);
  EXPECT_TRUE(counters.today().features.empty());
}

TEST(UsageCountersTest, ProcessWideCountersStartDisabled) {
  EXPECT_FALSE(PJ::usageCounters().enabled());
}
