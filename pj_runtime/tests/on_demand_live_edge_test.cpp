// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The on-demand live edge (docs/derived_objects/ARCHITECTURE.md §16): a live announcement requests each demanded
// recipe of its dataset at its inputs' newest stamp, the time the docks show, whatever the playhead does; a finding
// at its pin when an input arrived at or before it. Driven through a real AppSession with the fake script run.

#include <gtest/gtest.h>

#include <QVector>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "event_pump_test_utils.h"
#include "on_demand_session_test_utils.h"
#include "pj_base/sdk/platform.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/InFlightBudget.h"
#include "pj_runtime/TransformService.h"
#include "pj_tf/tf_buffer.h"

#if defined(__linux__)
#include <unistd.h>
#endif

namespace PJ {
namespace {

using Status = NodeScheduler::NodeState::Status;
using test::cloudSpec;
using test::kSec;

constexpr Timestamp kMs = 1'000'000;

/// `owner/<id>` reading `inputs`; its script is the fake's to run.
RecipeGraph::Recipe recipe(std::string id, std::vector<std::string> inputs = {"cloud"}) {
  return test::onDemandRecipe(std::move(id), std::move(inputs), {cloudSpec()});
}

RecipeGraph::Recipe finding(std::string id, Timestamp pinned_t, std::vector<std::string> inputs = {"cloud"}) {
  RecipeGraph::Recipe pinned = recipe(std::move(id), std::move(inputs));
  pinned.pinned_t = pinned_t;
  return pinned;
}

struct LiveFixture : test::OnDemandSessionFixture {
  /// A streaming tick of `dataset`, announced as the poll announces it (its scalar topics, the dataset named), then
  /// everything it submitted published.
  void tick(DatasetId dataset = 0) {
    tickNoWait(dataset);
    settle();
  }
  void tickNoWait(DatasetId dataset = 0) {
    const DatasetId target = dataset != 0 ? dataset : ds;
    const std::vector<TopicId> topics = manager.createReader().listTopics(target);
    manager.notifyIngest(QVector<TopicId>(topics.begin(), topics.end()), /*live=*/true, target);
  }
  /// The scalar series `gain/value` of `ds`, with `samples` (stamp, value).
  void addGain(const std::vector<std::pair<Timestamp, double>>& samples) {
    DataWriter writer = manager.dataEngine().createWriter();
    if (!gain) {
      gain = writer.registerScalarSeries(ds, "gain", NumericType::kFloat64).value();
    }
    for (const auto& [t, value] : samples) {
      writer.appendScalar(*gain, t, value);
    }
    ASSERT_FALSE(manager.commitChunks(writer.flushAll()).empty());
    session.catalogModel().rebuildFromDatastore();
  }
  [[nodiscard]] Timestamp newest(const std::string& name, DatasetId dataset = 0) const {
    const std::vector<Timestamp> stamps = ring(name, dataset);
    return stamps.empty() ? -1 : stamps.back();
  }

  std::optional<ScalarSeriesHandle> gain;
};

// STR-02, D30: a live tick evaluates a demanded recipe once per new input stamp; a tick with nothing new computes
// nothing.
TEST(OnDemandLiveEdgeTest, LiveIngestEvaluatesOncePerNewStamp) {
  LiveFixture fx;
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  fx.settle();
  ASSERT_EQ(fx.fake.finished(), 0) << "the playhead sits before the first cloud";

  fx.push(fx.cloud, 30 * kSec);
  fx.tick();
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{30 * kSec}));
  EXPECT_EQ(fx.fake.finished(), 1);
  fx.tick();
  EXPECT_EQ(fx.fake.finished(), 1) << "a tick with nothing new computes nothing";

  fx.push(fx.cloud, 40 * kSec);
  fx.tick();
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{30 * kSec, 40 * kSec}));
  EXPECT_EQ(fx.fake.finished(), 2);
  EXPECT_EQ(fx.state("owner/crop")->status, Status::kReady);
  EXPECT_EQ(fx.state("owner/crop")->requested_t, 40 * kSec);
}

// An announcement whose inputs are unchanged requests nothing: the cursor stays where the playhead put it.
TEST(OnDemandLiveEdgeTest, RepeatedUnchangedAnnouncementsDoNoWork) {
  LiveFixture fx;
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  fx.push(fx.cloud, 30 * kSec);
  fx.tick();
  fx.seek(10 * kSec);
  ASSERT_EQ(fx.state("owner/crop")->requested_t, 10 * kSec);
  const int runs = fx.fake.finished();

  for (int repeat = 0; repeat < 3; ++repeat) {
    fx.tick();
  }
  EXPECT_EQ(fx.state("owner/crop")->requested_t, 10 * kSec) << "nothing new at the edge: no request";
  EXPECT_EQ(fx.fake.finished(), runs);
}

// An entry replaced at the same stamp has a new identity: the edge does not move, the result does.
TEST(OnDemandLiveEdgeTest, SameStampReplacementIsNotLost) {
  LiveFixture fx;
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  fx.push(fx.cloud, 30 * kSec, 4);
  fx.tick();
  ASSERT_EQ(fx.shown("owner/crop/cropped", 30 * kSec).width, 4u);

  fx.push(fx.cloud, 30 * kSec, 6);
  fx.tick();
  EXPECT_EQ(fx.fake.finished(), 2);
  EXPECT_EQ(fx.shown("owner/crop/cropped", 30 * kSec).width, 6u);
}

// The docks show the live edge whatever the playhead does, so a parked playhead does not stop the recipe there.
TEST(OnDemandLiveEdgeTest, APausedPlayheadDoesNotStopTheLiveEdge) {
  LiveFixture fx;
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  fx.seek(10 * kSec);
  ASSERT_FALSE(fx.session.playbackEngine().isPlaying());
  const DisplaySeconds parked = fx.session.playbackEngine().currentTime();

  fx.push(fx.cloud, 30 * kSec);
  fx.tick();
  fx.push(fx.cloud, 31 * kSec);
  fx.tick();
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{10 * kSec, 30 * kSec, 31 * kSec}));
  EXPECT_EQ(fx.state("owner/crop")->requested_t, 31 * kSec);
  EXPECT_EQ(fx.session.playbackEngine().currentTime().value, parked.value) << "the request moves no playhead";
}

// A recipe nothing displays is not requested; a finding stays at its pin while inputs arrive after it.
TEST(OnDemandLiveEdgeTest, PinnedAndUndemandedNodesDoNotFollowTheEdge) {
  LiveFixture fx;
  fx.install(recipe("idle"));
  fx.install(finding("finding", 10 * kSec));
  fx.settle();
  const int runs = fx.fake.finished();
  ASSERT_EQ(runs, 1) << "the finding, once at its pin";

  fx.push(fx.cloud, 30 * kSec);
  fx.tick();
  fx.tick();
  EXPECT_FALSE(fx.state("owner/idle").has_value()) << "never requested";
  EXPECT_TRUE(fx.ring("owner/idle/cropped").empty());
  EXPECT_EQ(fx.state("owner/finding")->requested_t, 10 * kSec);
  EXPECT_EQ(fx.fake.finished(), runs) << "an input after the pin changes nothing the finding reads";
}

// PIN-02: a sample arriving at or before a finding's pin (a late one) evaluates the finding again there; one after
// it does not, and an input evicted from under the pin keeps the finding's result.
TEST(OnDemandLiveEdgeTest, APinnedFindingIsRefreshedWhenAnInputArrivesBeforeItsPin) {
  LiveFixture fx;
  fx.install(finding("finding", 25 * kSec));
  fx.settle();
  ASSERT_EQ(fx.shown("owner/finding/cropped", 100 * kSec).width, 4u) << "the entry at 20 s";

  fx.push(fx.cloud, 22 * kSec, 6);
  fx.tick();
  EXPECT_EQ(fx.fake.finished(), 2);
  EXPECT_EQ(fx.shown("owner/finding/cropped", 100 * kSec).width, 6u);
  EXPECT_EQ(fx.state("owner/finding")->requested_t, 25 * kSec);

  fx.push(fx.cloud, 30 * kSec, 8);
  fx.tick();
  EXPECT_EQ(fx.fake.finished(), 2) << "after the pin";

  test::prune(fx.manager.objectStore(), fx.cloud);
  fx.push(fx.cloud, 40 * kSec);
  fx.tick();
  EXPECT_EQ(fx.state("owner/finding")->status, Status::kReady) << fx.state("owner/finding")->detail;
  EXPECT_EQ(fx.shown("owner/finding/cropped", 100 * kSec).width, 6u) << "nothing arrived: the input left";
}

// A scalar side input's new sample moves the edge: the result advances with no new cloud. A paused tick (not live)
// requests nothing.
TEST(OnDemandLiveEdgeTest, ScalarSideInputAdvancesTheLiveResultWithoutANewCloud) {
  LiveFixture fx;
  fx.addGain({{15 * kSec, 1.0}});
  fx.install(recipe("scaled", {"cloud", "gain/value"}));
  fx.display("owner/scaled/cropped");
  fx.tick();
  ASSERT_EQ(fx.ring("owner/scaled/cropped"), (std::vector<Timestamp>{20 * kSec}));

  fx.addGain({{25 * kSec, 2.0}});
  fx.tick();
  EXPECT_EQ(fx.ring("owner/scaled/cropped"), (std::vector<Timestamp>{20 * kSec, 25 * kSec}));
  EXPECT_EQ(fx.fake.finished(), 2);

  fx.addGain({{35 * kSec, 3.0}});
  const std::vector<TopicId> topics = fx.manager.createReader().listTopics(fx.ds);
  ASSERT_FALSE(topics.empty());
  fx.manager.notifyIngest(QVector<TopicId>(topics.begin(), topics.end()), /*live=*/false, fx.ds);
  fx.settle();
  EXPECT_EQ(fx.ring("owner/scaled/cropped"), (std::vector<Timestamp>{20 * kSec, 25 * kSec})) << "paused";
}

/// A displayed recipe reading the cloud and the string state of `/robot/status`, and one reading the state alone,
/// both shown at the live edge with the state "idle" at 20 s; `writer` appends to `status`.
struct StringStateFixture : LiveFixture {
  StringStateFixture() {
    status = test::registerStringTopic(writer, ds, "/robot/status");
    test::appendString(writer, status, 20 * kSec, "idle");
    EXPECT_FALSE(manager.commitChunks(writer.flushAll()).empty());
    session.catalogModel().rebuildFromDatastore();
    install(recipe("annotate", {"cloud", "/robot/status/state"}));
    install(recipe("label", {"/robot/status/state"}));
    display("owner/annotate/cropped");
    display("owner/label/cropped");
  }
  /// A row at `t` whose state is null, committed.
  void appendNullState(Timestamp t) {
    test::appendNullString(writer, status, t);
    EXPECT_FALSE(manager.commitChunks(writer.flushAll()).empty());
  }
  /// Both recipes were requested at `t`, found the state missing there and show nothing.
  void expectClearedAt(Timestamp t) const {
    for (const std::string key : {"owner/annotate", "owner/label"}) {
      EXPECT_EQ(state(key)->requested_t, t) << key;
      EXPECT_EQ(state(key)->status, Status::kMissingInput) << key << ": " << state(key)->detail;
      EXPECT_EQ(shown(key + "/cropped", t).width, 0u) << key << ": the previous state's result is not shown";
    }
  }

  DataWriter writer = manager.dataEngine().createWriter();
  TopicId status = 0;
};

// D6: a null state arriving on the live stream is a missing input at its time: the result the previous state made is
// cleared, also for a recipe that reads the string alone, though no cloud came with it.
TEST(OnDemandLiveEdgeTest, ANullStringRowClearsTheLiveResult) {
  StringStateFixture fx;
  fx.tick();
  ASSERT_EQ(fx.state("owner/annotate")->status, Status::kReady) << fx.state("owner/annotate")->detail;
  ASSERT_EQ(fx.state("owner/label")->status, Status::kReady) << fx.state("owner/label")->detail;

  fx.appendNullState(30 * kSec);
  fx.tick();
  fx.expectClearedAt(30 * kSec);
}

// The same null arriving while the transport is paused is seen when it follows the live edge again.
TEST(OnDemandLiveEdgeTest, ANullStringRowArrivingWhilePausedClearsTheResultOnResume) {
  StringStateFixture fx;
  fx.tick();
  ASSERT_EQ(fx.state("owner/label")->status, Status::kReady) << fx.state("owner/label")->detail;
  PlaybackEngine& playback = fx.session.playbackEngine();
  playback.setHoldAtRangeMax(true);
  fx.seek(5 * kSec);  // paused, scrubbed back

  fx.appendNullState(30 * kSec);
  const std::vector<TopicId> topics = fx.manager.createReader().listTopics(fx.ds);
  fx.manager.notifyIngest(QVector<TopicId>(topics.begin(), topics.end()), /*live=*/false, fx.ds);
  fx.settle();
  playback.play();
  fx.settle();
  fx.expectClearedAt(30 * kSec);
  playback.pause();
}

// DAG-09: a displayed reader of an upstream nothing displays follows its upstream's raw inputs.
TEST(OnDemandLiveEdgeTest, LiveIngestReachesDemandedDescendants) {
  LiveFixture fx;
  fx.install(recipe("z"));
  fx.install(recipe("a", {"owner/z/cropped"}));
  fx.display("owner/a/cropped");
  fx.push(fx.cloud, 30 * kSec);
  fx.tick();
  EXPECT_EQ(fx.newest("owner/a/cropped"), 30 * kSec);
  EXPECT_EQ(fx.newest("owner/z/cropped"), 30 * kSec);

  fx.push(fx.cloud, 40 * kSec);
  fx.tick();
  EXPECT_EQ(fx.newest("owner/a/cropped"), 40 * kSec);
}

// STR-05/07 removed: no changed-object feed. A dataset of object topics only announces no ids, and a write that no
// write host saw (a toolbox's, straight into the store) is found by inspecting the store after the announcement.
TEST(OnDemandLiveEdgeTest, ObjectOnlyAndToolboxWritesReachTheLiveEdge) {
  LiveFixture fx;
  const DatasetId stream = fx.addDataset("stream");
  const ObjectTopicId scan = fx.addCloudTopic(stream, "scan", {});
  fx.install(recipe("crop", {"scan"}));
  fx.display("owner/crop/cropped", stream);
  ASSERT_TRUE(fx.manager.createReader().listTopics(stream).empty());

  fx.push(scan, 30 * kSec);
  fx.tick(stream);
  EXPECT_EQ(fx.ring("owner/crop/cropped", stream), (std::vector<Timestamp>{30 * kSec}));
}

// Zero and negative stamps are samples: "no live sample" is no stamp, not 0.
TEST(OnDemandLiveEdgeTest, ZeroAndNegativeLiveStampsAreValid) {
  LiveFixture fx;
  const DatasetId early = fx.addDataset("early");
  const ObjectTopicId scan = fx.addCloudTopic(early, "scan", {});
  fx.install(recipe("crop", {"scan"}));
  fx.display("owner/crop/cropped", early);
  fx.tick(early);
  EXPECT_FALSE(fx.manager.onDemandService().liveEdgeOf("owner/crop").has_value()) << "no sample yet";

  fx.push(scan, -2 * kSec);
  fx.tick(early);
  EXPECT_EQ(fx.manager.onDemandService().liveEdgeOf("owner/crop"), -2 * kSec);
  EXPECT_EQ(fx.ring("owner/crop/cropped", early), (std::vector<Timestamp>{-2 * kSec}));
  fx.push(scan, 0);
  fx.tick(early);
  EXPECT_EQ(fx.ring("owner/crop/cropped", early), (std::vector<Timestamp>{-2 * kSec, 0}));
}

// A tick that only moves the TF retries a recipe that read the TF, under the snapshot brake: within the brake period
// its run's snapshot still serves, after it the new revision runs it again.
TEST(OnDemandLiveEdgeTest, ATfOnlyTickRetriesARecipeThatReadTheTf) {
  auto now = std::chrono::steady_clock::now();  // outlives the fixture, whose manager reads it through the clock
  LiveFixture fx;
  TransformService transforms(fx.manager);
  fx.manager.setTransformService(&transforms);
  const auto write = [&](Timestamp t) {
    pj::tf::StampedTransform tf;
    tf.stamp = fromRaw(t);
    tf.parent_frame = "map";
    tf.child_frame = "base_link";
    tf.transform.t.x = static_cast<double>(t);
    ASSERT_TRUE(transforms.transformBuffer(fx.ds)->setTransform(tf).has_value());
  };
  fx.manager.setTfClock([&now] { return now; });
  write(1 * kSec);
  fx.fake.observe_tf = true;
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  fx.push(fx.cloud, 30 * kSec);
  fx.tick();
  ASSERT_EQ(fx.fake.finished(), 1);

  write(2 * kSec);
  fx.tick();
  EXPECT_EQ(fx.fake.finished(), 1) << "within the brake period the run's snapshot still serves";
  now += std::chrono::milliseconds(600);  // past SessionManager::kTfSnapshotPeriod (500 ms)
  fx.tick();
  EXPECT_EQ(fx.fake.finished(), 2) << "the TF moved: the recipe that read it runs again";
  fx.tick();
  EXPECT_EQ(fx.fake.finished(), 2);
  fx.manager.setTransformService(nullptr);
}

// GAP-10, the scheduling half: a 100 Hz stream for longer than the ring's window keeps the derived topic publishing
// at every tick (the ring evicts, it never refuses).
TEST(OnDemandLiveEdgeTest, LiveEdgeKeepsPublishingAt100Hz) {
  LiveFixture fx;
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  constexpr int kTicks = 3'500;  // 35 s
  const ObjectTopicId derived = fx.topic("owner/crop/cropped");
  for (int index = 1; index <= kTicks; ++index) {
    const Timestamp t = 20 * kSec + index * 10 * kMs;
    fx.push(fx.cloud, t);
    fx.tick();
    ASSERT_EQ(fx.manager.objectStore().timeRange(derived).second, t) << "tick " << index;
  }
  EXPECT_EQ(fx.fake.finished(), kTicks);
  EXPECT_EQ(fx.state("owner/crop")->status, Status::kReady);
}

// D30: a recipe slower than the stream is never cancelled by a tick: each run publishes, the pending request keeps
// only the newest edge, and once the ingest stops the recipe converges on it.
TEST(OnDemandLiveEdgeTest, SlowLiveRecipeKeepsPublishingAndConvergesAfterIngestStops) {
  LiveFixture fx;
  fx.fake.gated = true;
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  fx.settle();
  fx.push(fx.cloud, 30 * kSec);
  fx.tickNoWait();
  ASSERT_TRUE(fx.fake.waitStarted(1));
  fx.push(fx.cloud, 31 * kSec);
  fx.tickNoWait();
  fx.push(fx.cloud, 32 * kSec);
  fx.tickNoWait();
  EXPECT_EQ(fx.fake.started(), 1) << "the newer edges wait";

  fx.fake.allow(1);
  test::pumpUntil([&] { return fx.fake.started() == 2; });
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{30 * kSec})) << "published while outrun";
  EXPECT_EQ(fx.state("owner/crop")->status, Status::kPending);
  fx.fake.allow(1);
  fx.settle();
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{30 * kSec, 32 * kSec}));
  EXPECT_EQ(fx.state("owner/crop")->status, Status::kReady);
  EXPECT_EQ(fx.fake.started(), 2) << "31 s was superseded while it waited";
  for (const test::FakeRun::Record& record : fx.fake.records()) {
    EXPECT_FALSE(record.cancelled);
  }
}

// A displayed derived topic keeps its raw inputs (its upstreams' included) in the demand tracker's active set, so a
// demand-capable stream sends them while only the derived topic is displayed.
TEST(OnDemandLiveEdgeTest, DisplayingADerivedTopicSubscribesToItsRawLeaves) {
  LiveFixture fx;
  fx.addGain({{15 * kSec, 1.0}});
  const auto active = [&] {
    std::vector<std::string> names;
    for (const QString& name : fx.session.topicDemandTracker().activeTopics(fx.ds)) {
      names.push_back(name.toStdString());
    }
    return names;
  };
  fx.display("owner/chain/cropped");
  EXPECT_EQ(active(), (std::vector<std::string>{"owner/chain/cropped"}));
  fx.install(recipe("scaled", {"cloud", "gain/value"}));
  fx.install(recipe("chain", {"owner/scaled/cropped"}));
  EXPECT_EQ(active(), (std::vector<std::string>{"cloud", "gain", "owner/chain/cropped"}));

  fx.hide("owner/chain/cropped");
  EXPECT_TRUE(active().empty()) << "nothing displayed, nothing read";
}

// A finding keeps its result when a retry cannot stage it because its upstream's result at the pin left the ring:
// the upstream's window followed the live edge away from the pin, nothing the finding read changed. Once its raw
// input at the pin is evicted too, a tick does not request it (which would pull its upstream back to the pin).
TEST(OnDemandLiveEdgeTest, AFindingKeepsItsResultWhenItsUpstreamsWindowMovesOn) {
  LiveFixture fx;
  fx.install(recipe("b"));  // sorts before the finding: the tick requests it first
  fx.install(finding("f", 10 * kSec, {"owner/b/cropped"}));
  fx.display("owner/b/cropped");
  fx.settle();
  ASSERT_EQ(fx.state("owner/f")->status, Status::kReady);

  for (const Timestamp t : {50 * kSec, 100 * kSec}) {
    fx.push(fx.cloud, t);
    fx.tick();
  }
  ASSERT_TRUE(fx.ring("owner/b/cropped").front() > 10 * kSec) << "the upstream's result at the pin was evicted";
  const auto shown = fx.manager.onDemandService().history().shownAt("owner/f", 10 * kSec);
  ASSERT_TRUE(shown.has_value());
  EXPECT_TRUE(shown->available) << "no tombstone over the finding";
  EXPECT_EQ(fx.state("owner/f")->status, Status::kReady) << fx.state("owner/f")->detail;

  test::prune(fx.manager.objectStore(), fx.cloud);
  fx.push(fx.cloud, 120 * kSec);
  fx.tick();
  EXPECT_EQ(fx.state("owner/b")->requested_t, 120 * kSec) << "the upstream stays at the live edge";
  EXPECT_EQ(fx.state("owner/f")->status, Status::kReady);
}

// Keeping a finding is for inputs that left the window: removing its upstream still clears it.
TEST(OnDemandLiveEdgeTest, RemovingAFindingsUpstreamClearsTheFinding) {
  LiveFixture fx;
  fx.install(recipe("b"));
  fx.install(finding("f", 10 * kSec, {"owner/b/cropped"}));
  fx.settle();
  ASSERT_EQ(fx.state("owner/f")->status, Status::kReady);
  (void)fx.manager.onDemandService().remove("owner/b");
  fx.settle();
  EXPECT_EQ(fx.state("owner/f")->status, Status::kMissingInput);
  const auto shown = fx.manager.onDemandService().history().shownAt("owner/f", 10 * kSec);
  EXPECT_FALSE(shown && shown->available);
}

// The live check compares the raw inputs behind each upstream too: a same-stamp replacement read only through an
// upstream nothing displays is seen.
TEST(OnDemandLiveEdgeTest, ASameStampReplacementBehindAnUndisplayedUpstreamIsSeen) {
  LiveFixture fx;
  fx.install(recipe("z"));
  fx.install(recipe("a", {"owner/z/cropped"}));
  fx.display("owner/a/cropped");
  fx.push(fx.cloud, 30 * kSec, 4);
  fx.tick();
  fx.tick();
  ASSERT_EQ(fx.shown("owner/a/cropped", 30 * kSec).width, 4u);

  fx.push(fx.cloud, 30 * kSec, 6);
  fx.tick();
  EXPECT_EQ(fx.shown("owner/a/cropped", 30 * kSec).width, 6u);
}

// A finding whose upstream had nothing to produce at its pin runs once a late sample reaches that upstream.
TEST(OnDemandLiveEdgeTest, AFindingIsRefreshedThroughAnUpstreamThatHadNotProducedYet) {
  LiveFixture fx;
  const DatasetId late = fx.addDataset("late");
  const ObjectTopicId scan = fx.addCloudTopic(late, "scan", {});
  fx.install(recipe("z", {"scan"}));
  fx.install(finding("f", 25 * kSec, {"owner/z/cropped"}));
  fx.settle();
  ASSERT_EQ(fx.state("owner/f")->status, Status::kMissingInput);

  fx.push(scan, 22 * kSec);
  fx.tick(late);
  EXPECT_EQ(fx.state("owner/f")->status, Status::kReady) << fx.state("owner/f")->detail;
  EXPECT_EQ(fx.ring("owner/f/cropped", late).size(), 1u);
}

// A finding's install request seeds what the live check compares, so a first tick that only moved the TF does not
// run it again.
TEST(OnDemandLiveEdgeTest, TheFirstTickAfterAFindingsInstallDoesNotRerunItForTheTf) {
  LiveFixture fx;
  TransformService transforms(fx.manager);
  fx.manager.setTransformService(&transforms);
  const auto write = [&](Timestamp t) {
    pj::tf::StampedTransform tf;
    tf.stamp = fromRaw(t);
    tf.parent_frame = "map";
    tf.child_frame = "base_link";
    tf.transform.t.x = static_cast<double>(t);  // an identical sample is not stored
    ASSERT_TRUE(transforms.transformBuffer(fx.ds)->setTransform(tf).has_value());
  };
  write(1 * kSec);
  fx.fake.observe_tf = true;
  fx.install(finding("f", 25 * kSec));
  fx.settle();
  ASSERT_EQ(fx.fake.finished(), 1);
  const uint64_t before = transforms.snapshot(fx.ds)->revision();
  write(2 * kSec);
  ASSERT_NE(transforms.snapshot(fx.ds)->revision(), before);
  fx.tick();
  EXPECT_EQ(fx.fake.finished(), 1);
  fx.manager.setTransformService(nullptr);
}

// While the transport follows the live edge (playing, held at the range's end) on the streaming dataset, the live
// pass owns its recipes' requests; paused, the playhead requests again.
TEST(OnDemandLiveEdgeTest, TheLivePassOwnsTheRequestsWhileTheTransportFollowsTheLiveEdge) {
  LiveFixture fx;
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  fx.seek(5 * kSec);
  PlaybackEngine& playback = fx.session.playbackEngine();
  fx.session.setActiveStreamingDataset(fx.ds);
  playback.setHoldAtRangeMax(true);
  playback.play();
  fx.push(fx.cloud, 30 * kSec);
  fx.tick();
  ASSERT_EQ(fx.state("owner/crop")->requested_t, 30 * kSec);

  playback.setCurrentTime(rawToDisplaySeconds(20 * kSec, fx.manager.displayOffset(fx.ds)));
  fx.settle();
  EXPECT_EQ(fx.state("owner/crop")->requested_t, 30 * kSec) << "following the edge: the playhead requests nothing";

  playback.pause();
  fx.seek(10 * kSec);
  EXPECT_EQ(fx.state("owner/crop")->requested_t, 10 * kSec) << "paused: the cursor requests again";
}

// An upstream that fails at a finding's pin clears the finding (D6): only inputs that left the window keep it.
TEST(OnDemandLiveEdgeTest, AnUpstreamFailingAtThePinClearsTheFinding) {
  LiveFixture fx;
  fx.install(recipe("b"));
  fx.install(finding("f", 25 * kSec, {"owner/b/cropped"}));
  fx.settle();
  ASSERT_EQ(fx.state("owner/f")->status, Status::kReady);

  fx.fake.fail = "boom";
  fx.push(fx.cloud, 22 * kSec);  // a late sample before the pin: the finding and its upstream run again
  fx.tick();
  ASSERT_EQ(fx.state("owner/b")->status, Status::kError);
  EXPECT_NE(fx.state("owner/f")->status, Status::kReady);
  EXPECT_NE(fx.state("owner/f")->detail.find("boom"), std::string::npos) << fx.state("owner/f")->detail;
  const auto shown = fx.manager.onDemandService().history().shownAt("owner/f", 25 * kSec);
  EXPECT_FALSE(shown && shown->available);
}

// A result the ring trimmed while the playhead was scrubbed back is computed again when the transport resumes
// following the live edge, though the input is quiet (the live check alone sees nothing new).
TEST(OnDemandLiveEdgeTest, ResumingTheLiveEdgeRecomputesAResultTrimmedWhileScrubbedBack) {
  auto now = std::chrono::steady_clock::now();  // outlives the fixture, whose manager reads it through the clock
  LiveFixture fx;
  fx.manager.setTfClock([&now] { return now; });
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  fx.push(fx.cloud, 60 * kSec);
  fx.tick();
  ASSERT_EQ(fx.newest("owner/crop/cropped"), 60 * kSec);
  PlaybackEngine& playback = fx.session.playbackEngine();
  playback.setHoldAtRangeMax(true);
  fx.seek(5 * kSec);  // paused, scrubbed back: the ring's window follows the cursor
  fx.tick();
  ASSERT_LT(fx.newest("owner/crop/cropped"), 60 * kSec) << "the result at the edge was trimmed";

  playback.play();
  fx.settle();
  EXPECT_EQ(fx.newest("owner/crop/cropped"), 60 * kSec);
  EXPECT_EQ(fx.state("owner/crop")->requested_t, 60 * kSec);
  playback.pause();
}

// The live pass owns the requests of every dataset with a live announcement while the transport follows the live
// edge, not only the one the shell marks as the active stream.
TEST(OnDemandLiveEdgeTest, EveryLiveDatasetIsOwnedByTheLivePassWhileFollowing) {
  auto now = std::chrono::steady_clock::now();
  LiveFixture fx;
  fx.manager.setTfClock([&now] { return now; });
  const DatasetId second = fx.addDataset("second");
  const ObjectTopicId scan = fx.addCloudTopic(second, "scan", {});
  fx.install(recipe("crop"));
  fx.install(recipe("scan_crop", {"scan"}));
  fx.display("owner/crop/cropped");
  fx.display("owner/scan_crop/cropped", second);
  PlaybackEngine& playback = fx.session.playbackEngine();
  fx.session.setActiveStreamingDataset(fx.ds);
  playback.setHoldAtRangeMax(true);
  playback.play();
  fx.push(fx.cloud, 30 * kSec);
  fx.push(scan, 30 * kSec);
  fx.tick();
  fx.tick(second);
  ASSERT_EQ(fx.state("owner/scan_crop")->requested_t, 30 * kSec);

  playback.setCurrentTime(rawToDisplaySeconds(20 * kSec, fx.manager.displayOffset(second)));
  fx.settle();
  EXPECT_EQ(fx.state("owner/scan_crop")->requested_t, 30 * kSec);
  EXPECT_EQ(fx.state("owner/crop")->requested_t, 30 * kSec);
  playback.pause();
}

// A displayed finding subscribes its raw inputs too: a late sample before its pin must be able to arrive.
TEST(OnDemandLiveEdgeTest, DisplayingAFindingSubscribesToItsRawInputs) {
  LiveFixture fx;
  fx.install(finding("f", 25 * kSec));
  fx.display("owner/f/cropped");
  const std::vector<QString> active = fx.session.topicDemandTracker().activeTopics(fx.ds);
  EXPECT_EQ(active, (std::vector<QString>{"cloud", "owner/f/cropped"}));
}

// Removing a recipe, or re-binding it on another dataset (a merge), refreshes the raw subscriptions it held.
TEST(OnDemandLiveEdgeTest, RemovingOrRebindingARecipeRefreshesItsRawSubscriptions) {
  LiveFixture fx;
  const DatasetId other = fx.addDataset("other");
  fx.addCloudTopic(other, "cloud_b", {20 * kSec});
  fx.install(recipe("b", {"cloud_b"}));
  fx.display("owner/b/cropped", other);
  ASSERT_EQ(fx.session.topicDemandTracker().activeTopics(other), (std::vector<QString>{"cloud_b", "owner/b/cropped"}));
  ASSERT_EQ(fx.session.mergeDatasets({fx.ds, other}), fx.ds);
  fx.settle();
  EXPECT_EQ(fx.session.topicDemandTracker().activeTopics(other), (std::vector<QString>{"owner/b/cropped"}))
      << "the recipe now reads the anchor";

  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  ASSERT_EQ(fx.session.topicDemandTracker().activeTopics(fx.ds).front(), "cloud");
  (void)fx.manager.onDemandService().remove("owner/crop");
  emit fx.manager.objectTopicsChanged();  // what every remover announces with the retired topics
  EXPECT_EQ(fx.session.topicDemandTracker().activeTopics(fx.ds), (std::vector<QString>{"owner/crop/cropped"}));
}

// ---- Measurement ------------------------------------------------------------------------------------

/// The process's resident set, in bytes; -1 off Linux.
long long residentBytes() {
#if defined(__linux__)
  std::ifstream statm("/proc/self/statm");
  long long pages = 0;
  long long resident = 0;
  if (statm >> pages >> resident) {
    return resident * sysconf(_SC_PAGESIZE);
  }
#endif
  return -1;
}

// PJ_BENCH=1: the per-tick cost of a demanded node among 10k object topics: `liveEdgeOf` (the store's topic lookup,
// a scan, plus the catalog's memoized scalar lookup) and an unchanged `followLiveEdge` (that plus the staging).
TEST(OnDemandLiveEdgeBench, LiveEdgeAmongTenThousandTopics) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  LiveFixture fx;
  for (int index = 0; index < 10'000; ++index) {
    ASSERT_TRUE(fx.manager.objectStore().registerTopic({fx.ds, "noise/" + std::to_string(index), ""}).has_value());
  }
  const ObjectTopicId last = fx.addCloudTopic(fx.ds, "last", {30 * kSec});
  fx.addGain({{15 * kSec, 1.0}});
  (void)last;
  fx.install(recipe("last", {"last", "gain/value"}));
  fx.display("owner/last/cropped");
  fx.tick();
  OnDemandService& service = fx.manager.onDemandService();
  ASSERT_EQ(service.liveEdgeOf("owner/last"), 30 * kSec);
  const double edge = test::medianMicros(101, [&] { EXPECT_TRUE(service.liveEdgeOf("owner/last").has_value()); });
  const double follow = test::medianMicros(101, [&] { service.followLiveEdge(fx.ds); });
  std::printf(
      "[OnDemandLiveEdgeBench] among 10k object topics: liveEdgeOf %.0f ns, an unchanged followLiveEdge %.0f ns\n",
      edge * 1000.0, follow * 1000.0);
}

// F10 (TF R5), PJ_BENCH=1: a simulated live session of 30 s, streaming ticks every 50 ms (the poll's period), a cloud
// input at 20, 10, 5 and 1 Hz, and live TF at 100 Hz on frames the recipe does not read (`odom` <- `imu`), the recipe's
// own `map` <- `base_link` written once. Prints the runs past the necessary ones (one per new cloud), over every phase
// of the clouds against the TF snapshot brake (500 ms): each was caused by a TF revision bump alone, which R5
// (recording the edges and samples a run used) would avoid.
TEST(OnDemandLiveEdgeBench, TfOnlyRerunsOnALiveSession) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  // One session: TF-only runs within 30 s, the cloud arriving `phase` ticks after the brake's first snapshot.
  const auto session = [](bool reads_tf, int ticks_per_cloud, int phase, int& clouds) {
    auto now = std::chrono::steady_clock::now();  // outlives the fixture, whose manager reads it through the clock
    LiveFixture fx;
    TransformService transforms(fx.manager);
    fx.manager.setTransformService(&transforms);
    fx.manager.setTfClock([&now] { return now; });
    const auto write = [&](const char* parent, const char* child, Timestamp t, double x) {
      pj::tf::StampedTransform tf;
      tf.stamp = fromRaw(t);
      tf.parent_frame = parent;
      tf.child_frame = child;
      tf.transform.t.x = x;
      EXPECT_TRUE(transforms.transformBuffer(fx.ds)->setTransform(tf).has_value());
    };
    constexpr Timestamp kStart = 30 * kSec;
    write("map", "base_link", kStart, 1.0);
    fx.fake.observe_tf = reads_tf;
    fx.install(recipe("crop"));
    fx.display("owner/crop/cropped");
    fx.tick();  // the live edge at the fixture's newest cloud, before the counted session
    const int before = fx.fake.finished();
    clouds = 0;
    for (int tick = 1; tick <= 600; ++tick) {
      const Timestamp t = kStart + tick * 50 * kMs;
      for (Timestamp tf_t = t - 40 * kMs; tf_t <= t; tf_t += 10 * kMs) {
        write("odom", "imu", tf_t, static_cast<double>(tf_t));
      }
      if ((tick + phase) % ticks_per_cloud == 0) {
        fx.push(fx.cloud, t);
        ++clouds;
      }
      now += std::chrono::milliseconds(50);
      fx.tick();
    }
    const int runs = fx.fake.finished() - before;
    fx.manager.setTransformService(nullptr);
    return runs - clouds;
  };
  for (const bool reads_tf : {false, true}) {
    for (const int cloud_hz : {20, 10, 5, 1}) {
      const int ticks_per_cloud = 20 / cloud_hz;
      int least = 1 << 30;
      int most = 0;
      int clouds = 0;
      for (int phase = 0; phase < ticks_per_cloud; ++phase) {
        const int extra = session(reads_tf, ticks_per_cloud, phase, clouds);
        least = std::min(least, extra);
        most = std::max(most, extra);
      }
      std::printf(
          "[OnDemandLiveEdgeBench] recipe %s TF, cloud %2d Hz, TF 100 Hz elsewhere, 30 s: %3d necessary runs, %2d-%2d "
          "TF-only over %2d phases (%.0f-%.0f%% of the runs)\n",
          reads_tf ? "reads  " : "ignores", cloud_hz, clouds, least, most, ticks_per_cloud,
          100.0 * least / (clouds + least), 100.0 * most / (clouds + most));
    }
  }
}

// PERF-02 (PJ_STREAM_BENCH=1): 5 minutes of a 10 Hz stream of 2048-point clouds kept 5 s, one demanded recipe at the
// live edge, simulated tick by tick (no wall clock): the raw entries, the ring and the cache plateau, the ring under
// its budget.
TEST(DerivedStreamingBench, FiveMinuteRetentionAndMemoryPlateau) {
  if (!sdk::getEnv("PJ_STREAM_BENCH")) {
    GTEST_SKIP() << "set PJ_STREAM_BENCH=1";
  }
  constexpr int kTicks = 3'000;
  constexpr size_t kPoints = 2'048;
  LiveFixture fx;
  ObjectStore& store = fx.manager.objectStore();
  store.setRetentionBudget(fx.cloud, RetentionBudget{.time_window_ns = 5 * kSec});
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  const ResultHistory& history = fx.manager.onDemandService().history();
  const ResultHistory::Limits limits;
  for (int index = 1; index <= kTicks; ++index) {
    fx.push(fx.cloud, 20 * kSec + index * 100 * kMs, kPoints);
    fx.tick();
    if (index % 600 == 0) {
      std::printf(
          "[DerivedStreamingBench] %d min: raw entries %zu, derived entries %zu, ring %zu B, cache %zu B, protected "
          "excess %zu B, reserved %zu B, RSS %lld B\n",
          index / 600, store.entryCount(fx.cloud), store.entryCount(fx.topic("owner/crop/cropped")),
          history.ringBytes(), history.cacheBytes(), history.protectedExcessBytes(),
          fx.manager.inFlightBudget().reserved(), residentBytes());
    }
    ASSERT_LE(history.ringBytes(), limits.ring_bytes);
    ASSERT_LE(store.entryCount(fx.cloud), 51u);
  }
}

}  // namespace
}  // namespace PJ
