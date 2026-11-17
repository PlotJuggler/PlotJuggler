// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QString>
#include <vector>

#include "pj_runtime/TopicDemandTracker.h"

namespace {

constexpr PJ::DatasetId kDatasetA = 1;
constexpr PJ::DatasetId kDatasetB = 2;

// Pulls the (DatasetId, active_topics) payload out of the Nth QSignalSpy emission.
std::vector<QString> emittedTopics(const QSignalSpy& spy, int index) {
  return spy.at(index).at(1).value<std::vector<QString>>();
}

TEST(TopicDemandTrackerTest, SharedTopicIsReferenceCounted) {
  PJ::TopicDemandTracker tracker;
  QSignalSpy spy(&tracker, &PJ::TopicDemandTracker::activeTopicsChanged);

  tracker.addReference(kDatasetA, "/imu");
  tracker.addReference(kDatasetA, "/imu");  // second display of the same topic
  EXPECT_EQ(spy.count(), 1) << "topic already active — the 2nd reference must not re-emit";
  EXPECT_EQ(emittedTopics(spy, 0), (std::vector<QString>{"/imu"}));

  tracker.removeReference(kDatasetA, "/imu");
  EXPECT_EQ(spy.count(), 1) << "still referenced once — must not emit";
  EXPECT_EQ(tracker.activeTopics(kDatasetA), (std::vector<QString>{"/imu"}));

  tracker.removeReference(kDatasetA, "/imu");
  ASSERT_EQ(spy.count(), 2);
  EXPECT_TRUE(emittedTopics(spy, 1).empty());
  EXPECT_TRUE(tracker.activeTopics(kDatasetA).empty());
}

TEST(TopicDemandTrackerTest, InfrastructureTopicStaysActiveAcrossDisplayChurn) {
  PJ::TopicDemandTracker tracker;
  QSignalSpy spy(&tracker, &PJ::TopicDemandTracker::activeTopicsChanged);

  tracker.setInfrastructureTopics(kDatasetA, {"/tf"});
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(emittedTopics(spy, 0), (std::vector<QString>{"/tf"}));

  // The infra topic is already active, so displaying and un-displaying it changes nothing.
  tracker.addReference(kDatasetA, "/tf");
  tracker.removeReference(kDatasetA, "/tf");
  EXPECT_EQ(spy.count(), 1) << "infra topic's active-membership never changed";
  EXPECT_EQ(tracker.activeTopics(kDatasetA), (std::vector<QString>{"/tf"}));
}

TEST(TopicDemandTrackerTest, ForcedTopicStreamsWithoutAnyDisplayReference) {
  PJ::TopicDemandTracker tracker;
  QSignalSpy spy(&tracker, &PJ::TopicDemandTracker::activeTopicsChanged);

  tracker.setTopicForced(kDatasetA, "/odom", true);
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(emittedTopics(spy, 0), (std::vector<QString>{"/odom"}));
  EXPECT_TRUE(tracker.isTopicForced(kDatasetA, "/odom"));

  tracker.setTopicForced(kDatasetA, "/odom", false);
  ASSERT_EQ(spy.count(), 2);
  EXPECT_TRUE(emittedTopics(spy, 1).empty());
  EXPECT_FALSE(tracker.isTopicForced(kDatasetA, "/odom"));
}

TEST(TopicDemandTrackerTest, StoppingForcedStreamingNeverPausesADisplayedTopic) {
  PJ::TopicDemandTracker tracker;
  QSignalSpy spy(&tracker, &PJ::TopicDemandTracker::activeTopicsChanged);

  tracker.addReference(kDatasetA, "/odom");  // displayed on a plot
  tracker.setTopicForced(kDatasetA, "/odom", true);
  EXPECT_EQ(spy.count(), 1) << "already active via the reference — forcing must not re-emit";

  // Stopping forced streaming only drops the FORCED hold; the display
  // reference keeps the topic streaming (force is a tier, not an override).
  tracker.setTopicForced(kDatasetA, "/odom", false);
  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(tracker.activeTopics(kDatasetA), (std::vector<QString>{"/odom"}));

  // And the inverse: a forced topic survives its display going away.
  tracker.setTopicForced(kDatasetA, "/odom", true);
  tracker.removeReference(kDatasetA, "/odom");
  EXPECT_EQ(tracker.activeTopics(kDatasetA), (std::vector<QString>{"/odom"}));
}

TEST(TopicDemandTrackerTest, RedundantForcingEmitsNothing) {
  PJ::TopicDemandTracker tracker;
  QSignalSpy spy(&tracker, &PJ::TopicDemandTracker::activeTopicsChanged);

  tracker.setTopicForced(kDatasetA, "/odom", true);
  tracker.setTopicForced(kDatasetA, "/odom", true);  // idempotent
  EXPECT_EQ(spy.count(), 1);
  tracker.setTopicForced(kDatasetA, "/never_forced", false);  // no-op unforce
  EXPECT_EQ(spy.count(), 1);
  EXPECT_FALSE(tracker.isTopicForced(kDatasetB, "/odom")) << "datasets are independent";
}

TEST(TopicDemandTrackerTest, ForcedTopicsChangedFiresEvenWhenActiveSetDoesNot) {
  PJ::TopicDemandTracker tracker;
  QSignalSpy forced_spy(&tracker, &PJ::TopicDemandTracker::forcedTopicsChanged);
  QSignalSpy active_spy(&tracker, &PJ::TopicDemandTracker::activeTopicsChanged);

  // The topic is already active through a display reference — forcing it must
  // still notify the badge UI even though the active set is unchanged.
  tracker.addReference(kDatasetA, "/odom");
  tracker.setTopicForced(kDatasetA, "/odom", true);
  EXPECT_EQ(forced_spy.count(), 1);
  EXPECT_EQ(active_spy.count(), 1) << "active set unchanged by forcing an already-active topic";
  EXPECT_EQ(tracker.forcedTopics(kDatasetA), (std::vector<QString>{"/odom"}));

  tracker.setTopicForced(kDatasetA, "/odom", true);  // idempotent — no signal
  EXPECT_EQ(forced_spy.count(), 1);

  tracker.clearDataset(kDatasetA);
  EXPECT_EQ(forced_spy.count(), 2) << "teardown drops the forced set — badges must clear";
  EXPECT_TRUE(tracker.forcedTopics(kDatasetA).empty());
}

TEST(TopicDemandTrackerTest, ClearDatasetDropsForcedTopics) {
  PJ::TopicDemandTracker tracker;
  tracker.setTopicForced(kDatasetA, "/odom", true);
  tracker.clearDataset(kDatasetA);
  EXPECT_TRUE(tracker.activeTopics(kDatasetA).empty());
  EXPECT_FALSE(tracker.isTopicForced(kDatasetA, "/odom"));
}

TEST(TopicDemandTrackerTest, RedundantMutationsEmitNothing) {
  PJ::TopicDemandTracker tracker;
  tracker.addReference(kDatasetA, "/imu");
  tracker.setInfrastructureTopics(kDatasetA, {"/tf"});

  QSignalSpy spy(&tracker, &PJ::TopicDemandTracker::activeTopicsChanged);
  tracker.addReference(kDatasetA, "/imu");              // already active, count only bumps
  tracker.setInfrastructureTopics(kDatasetA, {"/tf"});  // identical set
  EXPECT_EQ(spy.count(), 0);
}

TEST(TopicDemandTrackerTest, ClearDatasetEmitsEmptySetOnlyIfItHadActiveTopics) {
  PJ::TopicDemandTracker tracker;
  tracker.addReference(kDatasetA, "/imu");

  QSignalSpy spy(&tracker, &PJ::TopicDemandTracker::activeTopicsChanged);
  tracker.clearDataset(kDatasetA);
  ASSERT_EQ(spy.count(), 1);
  EXPECT_TRUE(emittedTopics(spy, 0).empty());
  EXPECT_TRUE(tracker.activeTopics(kDatasetA).empty());

  // Already empty or unknown: must not emit again.
  tracker.clearDataset(kDatasetA);
  tracker.clearDataset(kDatasetB);
  EXPECT_EQ(spy.count(), 1);
}

TEST(TopicDemandTrackerTest, UnbalancedRemoveReferenceIsSilentlyIgnored) {
  PJ::TopicDemandTracker tracker;
  QSignalSpy spy(&tracker, &PJ::TopicDemandTracker::activeTopicsChanged);

  tracker.removeReference(kDatasetA, "/never-added");  // unknown dataset entirely
  EXPECT_EQ(spy.count(), 0);

  tracker.addReference(kDatasetA, "/imu");
  tracker.removeReference(kDatasetA, "/imu");  // count now at 0
  tracker.removeReference(kDatasetA, "/imu");  // second remove — must not go negative or emit
  EXPECT_EQ(spy.count(), 2);                   // add + first remove only
  EXPECT_TRUE(tracker.activeTopics(kDatasetA).empty());

  tracker.removeReference(kDatasetA, "/other-unknown-topic");
  EXPECT_EQ(spy.count(), 2);
}

TEST(TopicDemandTrackerTest, ActiveSetIsSortedAndDeduplicated) {
  PJ::TopicDemandTracker tracker;
  tracker.setInfrastructureTopics(kDatasetA, {"/tf", "/zeta"});
  tracker.addReference(kDatasetA, "/zeta");  // overlaps infra
  tracker.addReference(kDatasetA, "/beta");
  tracker.addReference(kDatasetA, "/alpha");

  const std::vector<QString> expected{"/alpha", "/beta", "/tf", "/zeta"};
  EXPECT_EQ(tracker.activeTopics(kDatasetA), expected);

  QSignalSpy spy(&tracker, &PJ::TopicDemandTracker::activeTopicsChanged);
  tracker.addReference(kDatasetA, "/gamma");
  ASSERT_EQ(spy.count(), 1);
  const std::vector<QString> expected_after{"/alpha", "/beta", "/gamma", "/tf", "/zeta"};
  EXPECT_EQ(emittedTopics(spy, 0), expected_after);
}

TEST(TopicDemandTrackerTest, ReentrantMutationFromSlotDeliversNewestSetLast) {
  PJ::TopicDemandTracker tracker;

  // Slot A (connected first) reacts to the first non-empty set by clearing the
  // dataset — a synchronous reentrant mutation while the emission is in flight.
  bool cleared_once = false;
  QObject::connect(
      &tracker, &PJ::TopicDemandTracker::activeTopicsChanged, &tracker,
      [&](PJ::DatasetId ds, const std::vector<QString>& topics) {
        if (!topics.empty() && !cleared_once) {
          cleared_once = true;
          tracker.clearDataset(ds);
        }
      });

  // Slot B (connected after A) records every delivery; its LAST delivery must
  // reflect the tracker's final state (empty), never a stale superseded set.
  std::vector<std::vector<QString>> b_deliveries;
  QObject::connect(
      &tracker, &PJ::TopicDemandTracker::activeTopicsChanged, &tracker,
      [&](PJ::DatasetId /*ds*/, const std::vector<QString>& topics) { b_deliveries.push_back(topics); });

  tracker.addReference(kDatasetA, "/imu");

  ASSERT_FALSE(b_deliveries.empty());
  EXPECT_TRUE(b_deliveries.back().empty()) << "last delivery must match the final (cleared) state";
  EXPECT_TRUE(tracker.activeTopics(kDatasetA).empty());
}

TEST(TopicDemandTrackerTest, EmptyInfraOnUnknownDatasetIsANoOp) {
  PJ::TopicDemandTracker tracker;
  QSignalSpy spy(&tracker, &PJ::TopicDemandTracker::activeTopicsChanged);

  tracker.setInfrastructureTopics(kDatasetA, {});
  EXPECT_EQ(spy.count(), 0);
  EXPECT_TRUE(tracker.activeTopics(kDatasetA).empty());
}

TEST(TopicDemandTrackerTest, DatasetsAreIndependent) {
  PJ::TopicDemandTracker tracker;
  tracker.addReference(kDatasetA, "/imu");
  tracker.setInfrastructureTopics(kDatasetA, {"/tf"});

  EXPECT_TRUE(tracker.activeTopics(kDatasetB).empty());

  QSignalSpy spy(&tracker, &PJ::TopicDemandTracker::activeTopicsChanged);
  tracker.addReference(kDatasetB, "/gps");
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.at(0).at(0).value<PJ::DatasetId>(), kDatasetB);
  EXPECT_EQ(emittedTopics(spy, 0), (std::vector<QString>{"/gps"}));

  const std::vector<QString> expected_a{"/imu", "/tf"};
  EXPECT_EQ(tracker.activeTopics(kDatasetA), expected_a);

  tracker.clearDataset(kDatasetB);
  const std::vector<QString> still_expected_a{"/imu", "/tf"};
  EXPECT_EQ(tracker.activeTopics(kDatasetA), still_expected_a);
}

}  // namespace
