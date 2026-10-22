// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The on-demand series (REQUIREMENTS D10; docs/derived_objects/ARCHITECTURE.md §15) through a real AppSession: a node
// with an object input and a number output writes a DataEngine topic named like its key, one row per entry of its
// object inputs, while its topic is displayed or a request is open. The runs are `test::FakeRun`'s: a number output is
// the run's query time.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "event_pump_test_utils.h"
#include "on_demand_session_test_utils.h"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/settings_store_host.hpp"
#include "pj_bench_stats.h"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/SeriesRunner.h"
#include "pj_runtime/ToolboxRuntimeHost.h"
#include "pj_scripting/image_decoder.h"

namespace PJ {
namespace {

using test::cloudSpec;
using test::kSec;
using Row = std::pair<Timestamp, double>;

scripting::OutputSpec numberSpec(std::string name = "n") {
  return {std::move(name), scripting::OutputType::kNumber, sdk::BuiltinObjectType::kNone};
}

/// `owner/<id>` reading `inputs`, with one number output `n` unless told otherwise.
RecipeGraph::Recipe seriesRecipe(
    std::string id, std::vector<std::string> inputs = {"cloud"},
    std::vector<scripting::OutputSpec> outputs = {numberSpec()}) {
  return test::onDemandRecipe(std::move(id), std::move(inputs), std::move(outputs));
}

/// The instants `first`..`last` seconds, one per second.
std::vector<Timestamp> seconds(int first, int last) {
  std::vector<Timestamp> times;
  for (int second = first; second <= last; ++second) {
    times.push_back(second * kSec);
  }
  return times;
}

/// `parts` joined in order.
std::vector<Timestamp> concat(std::initializer_list<std::vector<Timestamp>> parts) {
  std::vector<Timestamp> joined;
  for (const std::vector<Timestamp>& part : parts) {
    joined.insert(joined.end(), part.begin(), part.end());
  }
  return joined;
}

/// A toolbox plugin's view of the session's data: its whole-series reads go through `read_series_arrow`.
struct PluginReader {
  explicit PluginReader(SessionManager& manager)
      : host(manager.dataEngine(), manager.objectStore(), settings, ToolboxRuntimeHost::Callbacks{}) {
    EXPECT_TRUE(host.registerServices(builder).has_value());
    registry.emplace(builder.view());
  }
  [[nodiscard]] sdk::ToolboxHostView toolbox() {
    return sdk::ToolboxHostView(registry->require<sdk::ToolboxHostService>().value());
  }
  /// The first field of the topic named `name`.
  [[nodiscard]] std::optional<sdk::FieldHandle> field(std::string_view name) {
    auto snapshot = toolbox().catalogSnapshot();
    EXPECT_TRUE(snapshot.has_value());
    for (const auto& topic : snapshot->topics()) {
      if (sdk::toStringView(topic.name) == name) {
        return snapshot->fields()[topic.first_field].handle;
      }
    }
    return std::nullopt;
  }

  sdk::InMemorySettingsBackend settings;
  ToolboxRuntimeHost host;
  ServiceRegistryBuilder builder;
  std::optional<sdk::ServiceRegistry> registry;
};

struct SeriesFixture : test::OnDemandSessionFixture {
  ~SeriesFixture() {
    fake.allow(1 << 20);  // a gated run left waiting must not hold the pool's join
  }

  SeriesRunner& series() {
    return manager.onDemandService().series();
  }
  [[nodiscard]] std::optional<SeriesRunner::State> seriesState(const std::string& key) {
    return series().state(key);
  }
  /// The rows of column `column` of `key`'s series topic.
  [[nodiscard]] std::vector<Row> rows(const std::string& key, std::size_t column = 0) {
    std::vector<Row> out;
    const auto found = seriesState(key);
    if (!found) {
      return out;
    }
    const DataReader reader = manager.createReader();
    auto values = reader.series(found->topic, column);
    if (values) {
      values->samples(Range<Timestamp>{std::numeric_limits<Timestamp>::min(), std::numeric_limits<Timestamp>::max()})
          .forEach([&](const SeriesSample& sample) { out.emplace_back(sample.timestamp, sample.value); });
    }
    return out;
  }
  /// Pumps until `key`'s replay reached the end of its clock and computes nothing; false after 5 s.
  bool waitComplete(const std::string& key) {
    const auto done = [&] {
      const auto found = seriesState(key);
      return found && found->complete && !found->replaying;
    };
    test::pumpUntil(done, 5000);
    return done();
  }
  [[nodiscard]] bool listed(TopicId topic) const {
    const std::vector<TopicId> topics = manager.dataEngine().listTopics(ds);
    return std::ranges::find(topics, topic) != topics.end();
  }
  /// Pumps the event loop (a chunk's room comes from a drain) until `runs` runs have started; false after 5 s.
  [[nodiscard]] bool waitStarted(int runs) {
    test::pumpUntil([&] { return fake.started() >= runs; }, 5000);
    return fake.started() >= runs;
  }
  /// Query times the fake ran at, in order of completion.
  [[nodiscard]] std::vector<Timestamp> runs() const {
    std::vector<Timestamp> times;
    for (const test::FakeRun::Record& record : fake.records()) {
      times.push_back(record.query_time);
    }
    return times;
  }
  /// A live tick of `ds`, announced as the poll announces it, then everything it submitted published.
  void tick() {
    const std::vector<TopicId> topics = manager.createReader().listTopics(ds);
    manager.notifyIngest(QVector<TopicId>(topics.begin(), topics.end()), /*live=*/true, ds);
    settle();
  }
  /// The scalar series `gain` of `ds`, registered on the first call, with samples at `stamps` added.
  void addGain(const std::vector<Timestamp>& stamps) {
    DataWriter writer = manager.dataEngine().createWriter();
    if (!gain) {
      gain = writer.registerScalarSeries(ds, "gain", NumericType::kFloat64).value();
    }
    for (const Timestamp t : stamps) {
      writer.appendScalar(*gain, t, 1.0);
    }
    ASSERT_FALSE(manager.commitChunks(writer.flushAll()).empty());
    session.catalogModel().rebuildFromDatastore();
  }
  std::optional<ScalarSeriesHandle> gain;
  /// The plots of `ds` start their visible range at raw `t` (the session takes display seconds).
  void showFrom(Timestamp t) {
    manager.setSeriesVisibleStart(ds, rawToDisplaySeconds(t, manager.displayOffset(ds)));
  }
  /// A live tick of `ds`, announced as the poll announces it, without waiting for the pool (a gated run holds it).
  void tickNoWait() {
    const std::vector<TopicId> topics = manager.createReader().listTopics(ds);
    manager.notifyIngest(QVector<TopicId>(topics.begin(), topics.end()), /*live=*/true, ds);
    QCoreApplication::processEvents();
  }
  /// `count` more clouds after the fixture's three, one per second from 21 s.
  void pushMany(int count) {
    for (int index = 0; index < count; ++index) {
      push(cloud, (21 + index) * kSec);
    }
  }
};

// ---- Replay -------------------------------------------------------------------------------------------------------

// SER-02: a displayed series writes one row per entry of its object input into a DataEngine topic named like its key.
TEST(SeriesRunnerTest, ReplayWritesOneRowPerClockEntry) {
  SeriesFixture fx;
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));

  EXPECT_EQ(fx.rows("owner/ser"), (std::vector<Row>{{5 * kSec, 5e9}, {10 * kSec, 10e9}, {20 * kSec, 20e9}}));
  const SeriesRunner::State state = *fx.seriesState("owner/ser");
  EXPECT_EQ(state.rows, 3u);
  EXPECT_EQ(state.failed, 0u);
  EXPECT_EQ(state.done, 3u);
  EXPECT_EQ(state.total, 3u);
  EXPECT_FALSE(state.stopped);
  const TopicStorage* storage = fx.manager.dataEngine().getTopicStorage(state.topic);
  ASSERT_NE(storage, nullptr);
  EXPECT_EQ(storage->descriptor().name, "owner/ser");
  EXPECT_EQ(storage->descriptor().dataset_id, fx.ds);
}

// A Python series computes its chunks on the Python lane, one row per entry, as a Luau one does.
TEST(SeriesRunnerTest, PythonSeriesRunsOnThePythonLane) {
  SeriesFixture fx;
  const std::thread::id python_lane = test::pythonLaneThread(fx.manager.enginePool());
  fx.install(test::pythonRecipe("ser", {"cloud"}, {numberSpec()}));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));

  EXPECT_EQ(fx.rows("owner/ser"), (std::vector<Row>{{5 * kSec, 5e9}, {10 * kSec, 10e9}, {20 * kSec, 20e9}}));
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 3u);
  for (const test::FakeRun::Record& record : records) {
    EXPECT_EQ(record.thread, python_lane) << record.query_time;
  }
}

// A series whose topic is displayed before its install (a layout's plot bound first) replays at once.
TEST(SeriesRunnerTest, ASeriesDisplayedBeforeItsInstallReplaysAtOnce) {
  SeriesFixture fx;
  fx.display("owner/ser");
  fx.install(seriesRecipe("ser"));
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.rows("owner/ser").size(), 3u);
}

// GAP-16: an installed series computes nothing until its topic is displayed; editing or reloading it while nothing
// displays it computes nothing either.
TEST(SeriesRunnerTest, SeriesReplaysOnlyWhileDemanded) {
  SeriesFixture fx;
  fx.install(seriesRecipe("ser"));
  fx.settle();
  EXPECT_EQ(fx.fake.started(), 0);
  ASSERT_TRUE(fx.seriesState("owner/ser").has_value());
  EXPECT_FALSE(fx.seriesState("owner/ser")->replaying);
  EXPECT_FALSE(fx.series().isDemanded("owner/ser"));

  auto edited = seriesRecipe("ser");
  edited.params_json = R"({"v":2})";
  fx.install(edited);
  {
    RefillGuard refill = fx.manager.beginRefill(fx.ds);
    fx.push(fx.cloud, 5 * kSec);
    refill.commit();
  }
  fx.settle();
  EXPECT_EQ(fx.fake.started(), 0) << "an undemanded edit or reload computes nothing";

  fx.display("owner/ser");
  EXPECT_TRUE(fx.series().isDemanded("owner/ser"));
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.rows("owner/ser").size(), 1u) << "the reloaded data";
}

// GAP-17: the clock is the union of every object input's entries; a scalar side input's changes add no row.
TEST(SeriesRunnerTest, SeriesClockIsUnionOfObjectInputs) {
  SeriesFixture fx;
  fx.addCloudTopic(fx.ds, "fast", {5 * kSec, 6 * kSec, 15 * kSec});
  fx.addGain({0, 6 * kSec});
  fx.install(seriesRecipe("ser", {"cloud", "fast", "gain"}));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  std::vector<Timestamp> times;
  for (const Row& row : fx.rows("owner/ser")) {
    times.push_back(row.first);
  }
  EXPECT_EQ(times, (std::vector<Timestamp>{5 * kSec, 6 * kSec, 10 * kSec, 15 * kSec, 20 * kSec}));
  EXPECT_EQ(fx.seriesState("owner/ser")->failed, 0u) << fx.seriesState("owner/ser")->first_error;
}

// D4: a string side input is read at each clock instant like a number's. A window refuses one (GAP-54) because its grid
// would skip the string's changes; a series' clock never takes a side input's changes, so a series computes.
TEST(SeriesRunnerTest, AStringSideInputIsReadAtEachClockInstant) {
  SeriesFixture fx;  // clouds at 5, 10, 20 s
  DataWriter writer = fx.manager.dataEngine().createWriter();
  const TopicId status = test::registerStringTopic(writer, fx.ds, "/robot/status");
  test::appendString(writer, status, 0, "idle");
  test::appendString(writer, status, 8 * kSec, "moving");
  ASSERT_FALSE(fx.manager.commitChunks(writer.flushAll()).empty());
  fx.session.catalogModel().rebuildFromDatastore();
  fx.install(seriesRecipe("ser", {"cloud", "/robot/status/state"}));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));

  EXPECT_EQ(fx.seriesState("owner/ser")->failed, 0u) << fx.seriesState("owner/ser")->first_error;
  EXPECT_EQ(fx.rows("owner/ser"), (std::vector<Row>{{5 * kSec, 5e9}, {10 * kSec, 10e9}, {20 * kSec, 20e9}}));
  std::vector<std::vector<std::string>> strings;
  for (const test::FakeRun::Record& record : fx.fake.records()) {
    strings.push_back(record.strings);
  }
  EXPECT_EQ(strings, (std::vector<std::vector<std::string>>{{"idle"}, {"moving"}, {"moving"}}));
}

// SER-03: a failed run and an unavailable output each leave a gap, counted with the first reason; the replay completes.
TEST(SeriesRunnerTest, FailingAndUnavailableEntriesLeaveGaps) {
  SeriesFixture fx;
  fx.fake.fail_at = {10 * kSec};
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.rows("owner/ser"), (std::vector<Row>{{5 * kSec, 5e9}, {20 * kSec, 20e9}}));
  EXPECT_EQ(fx.seriesState("owner/ser")->failed, 1u);
  EXPECT_EQ(fx.seriesState("owner/ser")->first_error, "boom");

  fx.fake.fail_at.clear();
  fx.fake.unavailable = {"n"};
  fx.install(seriesRecipe("gaps"));
  fx.display("owner/gaps");
  ASSERT_TRUE(fx.waitComplete("owner/gaps"));
  EXPECT_TRUE(fx.rows("owner/gaps").empty());
  EXPECT_EQ(fx.seriesState("owner/gaps")->failed, 3u);
  EXPECT_EQ(fx.seriesState("owner/gaps")->first_error, "on-demand: output 'n' unavailable: no points");
}

// SER-04: a side input missing before its first sample is a gap, not a stop.
TEST(SeriesRunnerTest, MissingSideInputBeforeItsFirstSampleIsAGap) {
  SeriesFixture fx;
  fx.addGain({8 * kSec});
  fx.install(seriesRecipe("ser", {"cloud", "gain"}));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.rows("owner/ser"), (std::vector<Row>{{10 * kSec, 10e9}, {20 * kSec, 20e9}}));
  EXPECT_EQ(fx.seriesState("owner/ser")->failed, 1u);
  EXPECT_EQ(fx.seriesState("owner/ser")->first_error, "on-demand: missing input 'gain'");
}

// ---- Live, edit, reload, remove -------------------------------------------------------------------------------------

// SER-05: a live tick evaluates only the entries after the watermark, every one of them; an empty tick adds nothing.
TEST(SeriesRunnerTest, LiveAppendAddsRowsAfterTheWatermarkOnly) {
  SeriesFixture fx;
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  fx.push(fx.cloud, 30 * kSec);
  fx.push(fx.cloud, 40 * kSec);
  fx.tick();
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.rows("owner/ser").size(), 5u);
  EXPECT_EQ(fx.runs(), (std::vector<Timestamp>{5 * kSec, 10 * kSec, 20 * kSec, 30 * kSec, 40 * kSec}));
  fx.tick();
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.runs().size(), 5u) << "an empty tick computes nothing";
  EXPECT_EQ(fx.seriesState("owner/ser")->total, 5u);
}

// GAP-55: an entry that arrives before the watermark (out of order) was in no chunk's clock: the next tick empties the
// topic and replays it, so the rows match the data. One that republishes the watermark's stamp (a latched source)
// replays nothing.
TEST(SeriesRunnerTest, ALateEntryReplaysTheSeriesARepublishedStampDoesNot) {
  SeriesFixture fx;
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  fx.push(fx.cloud, 15 * kSec);  // between two evaluated entries
  fx.tick();
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(
      fx.rows("owner/ser"),
      (std::vector<Row>{{5 * kSec, 5e9}, {10 * kSec, 10e9}, {15 * kSec, 15e9}, {20 * kSec, 20e9}}));
  EXPECT_EQ(fx.runs().size(), 3u + 4u) << "replayed from the start";

  fx.push(fx.cloud, 20 * kSec);  // republishes the watermark's stamp
  fx.tick();
  fx.tick();
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.rows("owner/ser").size(), 4u);
  EXPECT_EQ(fx.runs().size(), 7u) << "nothing replayed";
  EXPECT_EQ(fx.seriesState("owner/ser")->done, 4u);
}

// GAP-60: while the dataset is live, the clock ends at the oldest of its inputs' newest entries, so an input that lags
// the others (a lidar behind a camera) appends after the watermark instead of replaying the series: nothing computes
// before both inputs have data, then every instant runs once, and the stream's end computes the tail.
TEST(SeriesRunnerTest, ALaggingInputAppendsInsteadOfReplaying) {
  SeriesFixture fx;  // the camera, `cloud`: 5, 10, 20 s
  const ObjectTopicId lidar = fx.addCloudTopic(fx.ds, "lidar", {});
  fx.tick();
  fx.install(seriesRecipe("ser", {"cloud", "lidar"}));
  fx.display("owner/ser");
  fx.settle();
  EXPECT_EQ(fx.seriesState("owner/ser")->done, 0u) << "the lidar has no entry yet: no instant, not even a gap";

  fx.push(lidar, 5 * kSec);
  fx.tick();
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.runs(), (std::vector<Timestamp>{5 * kSec}));
  fx.push(lidar, 12 * kSec);  // behind the camera's 20 s
  fx.tick();
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  fx.push(lidar, 22 * kSec);
  fx.push(fx.cloud, 25 * kSec);
  fx.tick();
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.runs(), (std::vector<Timestamp>{5 * kSec, 10 * kSec, 12 * kSec, 20 * kSec, 22 * kSec}))
      << "each instant once, no replay";

  fx.manager.notifyStreamEnded(fx.ds);
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.runs().back(), 25 * kSec) << "the tail";
  EXPECT_EQ(fx.rows("owner/ser").size(), 6u);
  EXPECT_EQ(fx.seriesState("owner/ser")->failed, 0u) << fx.seriesState("owner/ser")->first_error;
}

// GAP-60: a scalar side input that lags holds a live clock back the same way, so no instant runs before its value.
TEST(SeriesRunnerTest, ALaggingScalarSideInputHoldsTheLiveClockBack) {
  SeriesFixture fx;  // clouds at 5, 10, 20 s
  fx.addGain({0, 6 * kSec});
  fx.tick();
  fx.install(seriesRecipe("ser", {"cloud", "gain"}));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.runs(), (std::vector<Timestamp>{5 * kSec}));
  fx.addGain({21 * kSec});
  fx.tick();
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.runs(), (std::vector<Timestamp>{5 * kSec, 10 * kSec, 20 * kSec}));
}

// An edit while a request is open replays the new revision to the end: the request carries over to it.
TEST(SeriesRunnerTest, AnEditKeepsAnOpenRequest) {
  SeriesFixture fx;
  fx.pushMany(200);
  fx.fake.gated = true;
  fx.install(seriesRecipe("ser"));
  fx.series().request("owner/ser");
  ASSERT_TRUE(fx.waitStarted(1));
  auto edited = seriesRecipe("ser");
  edited.params_json = R"({"v":2})";
  fx.install(edited);
  fx.fake.allow(1 << 20);
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.rows("owner/ser").size(), 203u);
}

// SER-06: an edit empties the same topic at once and replays it; a new set of number outputs is a new topic, and the
// old one is retired.
TEST(SeriesRunnerTest, EditTruncatesAndReplaysOnTheSameTopic) {
  SeriesFixture fx;
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  const TopicId topic = fx.seriesState("owner/ser")->topic;

  fx.fake.gated = true;
  auto edited = seriesRecipe("ser");
  edited.params_json = R"({"v":2})";
  fx.install(edited);
  EXPECT_EQ(fx.seriesState("owner/ser")->topic, topic);
  EXPECT_TRUE(fx.rows("owner/ser").empty()) << "emptied before the replay";
  fx.fake.allow(3);
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.rows("owner/ser").size(), 3u);
  EXPECT_EQ(fx.runs().size(), 6u);

  fx.install(seriesRecipe("ser", {"cloud"}, {numberSpec("renamed")}));
  ASSERT_NE(fx.seriesState("owner/ser")->topic, topic);
  EXPECT_FALSE(fx.listed(topic)) << "the old topic is retired";
  EXPECT_TRUE(fx.listed(fx.seriesState("owner/ser")->topic));
}

// SER-07: a reload empties the topic at once and replays it over the new data; the series topic is not a source topic
// the reload replaces or prunes.
TEST(SeriesRunnerTest, EpochBumpReplays) {
  SeriesFixture fx;
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  const TopicId topic = fx.seriesState("owner/ser")->topic;
  {
    RefillGuard refill = fx.manager.beginRefill(fx.ds);
    EXPECT_TRUE(fx.rows("owner/ser").empty());
    for (const Timestamp t : {5 * kSec, 10 * kSec, 20 * kSec, 40 * kSec}) {
      fx.push(fx.cloud, t);
    }
    refill.pruneVanishedTopics();
    EXPECT_TRUE(fx.listed(topic)) << "empty, but not a topic the new file lost";
    refill.commit();
  }
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.seriesState("owner/ser")->topic, topic);
  EXPECT_TRUE(fx.listed(topic));
  EXPECT_EQ(fx.rows("owner/ser").size(), 4u);
}

// SER-08: a removal retires the topic and drops the state; a chunk that was computing writes nothing later.
TEST(SeriesRunnerTest, RemoveRetiresTheTopic) {
  SeriesFixture fx;
  fx.fake.gated = true;
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitStarted(1));
  const TopicId topic = fx.seriesState("owner/ser")->topic;
  (void)fx.manager.onDemandService().remove("owner/ser");
  EXPECT_FALSE(fx.seriesState("owner/ser").has_value());
  EXPECT_FALSE(fx.listed(topic));
  fx.fake.allow(10);
  fx.settle();
  test::pumpFor(20);
  EXPECT_TRUE(fx.manager.dataEngine().getTopicStorage(topic)->sealedChunks().empty());
}

// SER-09: a stop ends the replay keeping its rows; a request resumes it from the watermark to the end, then lapses.
TEST(SeriesRunnerTest, StopKeepsRowsAndARequestResumes) {
  SeriesFixture fx;
  fx.pushMany(200);
  fx.fake.gated = true;
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  fx.fake.allow(static_cast<int>(SeriesRunner::kInstantsPerChunk));
  test::pumpUntil([&] { return fx.rows("owner/ser").size() == SeriesRunner::kInstantsPerChunk; }, 5000);
  ASSERT_TRUE(fx.waitStarted(static_cast<int>(SeriesRunner::kInstantsPerChunk) + 1)) << "the second chunk";
  EXPECT_EQ(fx.seriesState("owner/ser")->total, 203u);

  EXPECT_TRUE(fx.series().stop("owner/ser"));
  EXPECT_FALSE(fx.series().stop("owner/ser")) << "nothing computes";
  fx.fake.allow(1);
  fx.settle();
  EXPECT_EQ(fx.rows("owner/ser").size(), SeriesRunner::kInstantsPerChunk) << "the rows stay, nothing more is written";
  EXPECT_TRUE(fx.seriesState("owner/ser")->stopped);
  EXPECT_FALSE(fx.seriesState("owner/ser")->replaying);

  fx.fake.gated = false;
  fx.hide("owner/ser");
  fx.series().request("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.rows("owner/ser").size(), 203u);
  EXPECT_FALSE(fx.seriesState("owner/ser")->stopped);
  const std::vector<Timestamp> times = fx.runs();
  EXPECT_EQ(std::count(times.begin(), times.end(), 5 * kSec), 1) << "resumed after the watermark, not replayed";

  fx.push(fx.cloud, 300 * kSec);
  fx.tick();
  EXPECT_EQ(fx.rows("owner/ser").size(), 203u) << "the request lapsed with its replay";
}

// Demand leaving stops a replay keeping its rows; demand returning resumes it from its watermark.
TEST(SeriesRunnerTest, UndemandedSeriesResumesFromItsWatermark) {
  SeriesFixture fx;
  fx.pushMany(100);
  fx.fake.gated = true;
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  fx.fake.allow(static_cast<int>(SeriesRunner::kInstantsPerChunk));
  test::pumpUntil([&] { return fx.rows("owner/ser").size() == SeriesRunner::kInstantsPerChunk; }, 5000);
  ASSERT_TRUE(fx.waitStarted(static_cast<int>(SeriesRunner::kInstantsPerChunk) + 1));
  fx.hide("owner/ser");
  EXPECT_FALSE(fx.seriesState("owner/ser")->replaying);
  fx.fake.allow(1);
  fx.settle();
  fx.fake.gated = false;

  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.rows("owner/ser").size(), 103u);
  EXPECT_EQ(fx.runs().size(), 103u + 1) << "every instant once, and the one the stop cancelled again";
}

// ---- Chains, outputs, ownership ------------------------------------------------------------------------------------

// SER-10: a series reading an upstream's output evaluates the upstream inside its chunks; the upstream's ring and
// state stay untouched.
TEST(SeriesRunnerTest, ChainedSeriesLeavesTheUpstreamUntouched) {
  SeriesFixture fx;
  fx.install(test::onDemandRecipe("crop", {"cloud"}, {cloudSpec()}));
  fx.install(seriesRecipe("ser", {"owner/crop/cropped"}));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.rows("owner/ser").size(), 3u);
  EXPECT_TRUE(fx.ring("owner/crop/cropped").empty());
  // The series evaluates crop inside its own chunks: the scheduler never ran crop, so it has no state or an idle one.
  const auto crop_state = fx.state("owner/crop");
  EXPECT_TRUE(!crop_state || crop_state->status == NodeScheduler::NodeState::Status::kIdle);

  auto edited = test::onDemandRecipe("crop", {"cloud"}, {cloudSpec()});
  edited.params_json = R"({"v":2})";
  fx.install(edited);
  EXPECT_TRUE(fx.rows("owner/ser").empty()) << "an upstream's new revision empties its reader's series";
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.rows("owner/ser").size(), 3u);
}

// A displayed series streams its raw inputs: they join the demand tracker's derived-input tier.
TEST(SeriesRunnerTest, DisplayingASeriesSubscribesToItsRawInputs) {
  SeriesFixture fx;
  fx.install(seriesRecipe("ser"));
  const auto streams = [&] {
    const std::vector<QString> active = fx.session.topicDemandTracker().activeTopics(fx.ds);
    return std::ranges::find(active, QStringLiteral("cloud")) != active.end();
  };
  EXPECT_FALSE(streams());
  fx.display("owner/ser");
  EXPECT_TRUE(streams());
}

// SER-12: a topic of the series' name in its dataset that is not its own refuses the install, with nothing installed.
TEST(SeriesRunnerTest, ForeignTopicWithTheSameNameRefusesTheInstall) {
  SeriesFixture fx;
  DataWriter writer = fx.manager.dataEngine().createWriter();
  ASSERT_TRUE(writer.registerScalarSeries(fx.ds, "owner/ser", NumericType::kFloat64).has_value());
  const auto installed = fx.manager.onDemandService().install(seriesRecipe("ser"));
  ASSERT_FALSE(installed.has_value());
  EXPECT_NE(installed.error().find("already exists"), std::string::npos) << installed.error();
  EXPECT_EQ(fx.manager.onDemandService().graph().find("owner/ser"), nullptr);
  EXPECT_FALSE(fx.seriesState("owner/ser").has_value());
}

// An output name may hold a comma: the schema names its columns unambiguously, so "a,b" and the pair "a", "b" are two
// schemas, and the second topic writes its second column.
TEST(SeriesRunnerTest, ACommaInAnOutputNameNamesOneColumn) {
  SeriesFixture fx;
  fx.install(seriesRecipe("ser", {"cloud"}, {numberSpec("a,b")}));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  const TopicId one = fx.seriesState("owner/ser")->topic;
  const SchemaId one_schema = fx.manager.dataEngine().getTopicStorage(one)->descriptor().schema_id;

  fx.install(seriesRecipe("ser", {"cloud"}, {numberSpec("a"), numberSpec("b")}));
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  const TopicId two = fx.seriesState("owner/ser")->topic;
  ASSERT_NE(two, one);
  EXPECT_NE(fx.manager.dataEngine().getTopicStorage(two)->descriptor().schema_id, one_schema);
  EXPECT_EQ(fx.rows("owner/ser", 1).size(), 3u) << "the second column";
}

// SER-13: a finding has no series.
TEST(SeriesRunnerTest, PinnedRecipeGetsNoSeries) {
  SeriesFixture fx;
  auto pinned = seriesRecipe("ser");
  pinned.pinned_t = 10 * kSec;
  fx.install(pinned);
  EXPECT_FALSE(fx.seriesState("owner/ser").has_value());
  EXPECT_TRUE(fx.series().topics(false).empty());
}

// SER-14: a recipe with an object and a number output computes the series in chunks, which publish no object; the
// object output follows the cursor alone.
TEST(SeriesRunnerTest, MixedRecipePublishesObjectsAtTheCursorOnly) {
  SeriesFixture fx;
  fx.install(seriesRecipe("mix", {"cloud"}, {cloudSpec(), numberSpec()}));
  fx.display("owner/mix");
  ASSERT_TRUE(fx.waitComplete("owner/mix"));
  EXPECT_EQ(fx.rows("owner/mix").size(), 3u);
  EXPECT_TRUE(fx.ring("owner/mix/cropped").empty());
  fx.display("owner/mix/cropped");
  fx.seek(10 * kSec);
  EXPECT_EQ(fx.ring("owner/mix/cropped"), (std::vector<Timestamp>{10 * kSec}));
}

// SER-15: a preview's series computes without being displayed (its plugin's trial) and the catalog hides it; a
// persistent series is listed.
TEST(SeriesRunnerTest, EphemeralSeriesIsHiddenFromTheCatalogButReadable) {
  SeriesFixture fx;
  auto preview = seriesRecipe("~preview/1/ser");
  preview.ephemeral = true;
  fx.install(preview);
  fx.install(seriesRecipe("ser"));
  ASSERT_TRUE(fx.waitComplete("owner/~preview/1/ser"));
  EXPECT_EQ(fx.rows("owner/~preview/1/ser").size(), 3u);
  EXPECT_EQ(fx.series().topics(true), (std::unordered_set<TopicId>{fx.seriesState("owner/~preview/1/ser")->topic}));
  fx.session.catalogModel().rebuildFromDatastore();
  bool preview_listed = false;
  bool persistent_listed = false;
  for (const CatalogItem& item : fx.session.catalogModel().items()) {
    preview_listed = preview_listed || item.topic_name == "owner/~preview/1/ser";
    persistent_listed = persistent_listed || item.topic_name == "owner/ser";
  }
  EXPECT_FALSE(preview_listed);
  EXPECT_TRUE(persistent_listed);
}

// LAY-05: a restore re-installing a key with the same number outputs keeps its series topic id; one with other outputs
// retires the parked topic; the sweep retires a series no install claimed.
TEST(SeriesRunnerTest, ClearThenUpsertSameKeyKeepsTheSeriesTopicId) {
  SeriesFixture fx;
  OnDemandService& service = fx.manager.onDemandService();
  fx.install(seriesRecipe("same"));
  fx.install(seriesRecipe("other"));
  fx.install(seriesRecipe("gone"));
  const TopicId same = fx.seriesState("owner/same")->topic;
  const TopicId other = fx.seriesState("owner/other")->topic;
  const TopicId gone = fx.seriesState("owner/gone")->topic;

  ASSERT_TRUE(service.clearExcept(RestoreIntent::kReplace, {}));
  EXPECT_TRUE(fx.listed(gone)) << "parked until the sweep";
  fx.install(seriesRecipe("same"));
  fx.install(seriesRecipe("other", {"cloud"}, {numberSpec("m")}));
  (void)service.sweepUnclaimed();
  EXPECT_EQ(fx.seriesState("owner/same")->topic, same);
  EXPECT_NE(fx.seriesState("owner/other")->topic, other);
  EXPECT_FALSE(fx.listed(other));
  EXPECT_FALSE(fx.listed(gone));
  EXPECT_FALSE(fx.seriesState("owner/gone").has_value());
}

// GAP-18: runs decode and compute on the pool, never on the GUI thread.
TEST(SeriesRunnerTest, ADecodeRunsOnTheWorker) {
  SeriesFixture fx;
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  ASSERT_EQ(fx.fake.records().size(), 3u);
  for (const test::FakeRun::Record& record : fx.fake.records()) {
    EXPECT_NE(record.thread, std::this_thread::get_id());
    EXPECT_NE(record.input_points, nullptr) << "the cloud was decoded before the run";
  }
}

// MED-06 / GAP-26: a series chunk is given the session's image decoder like any run, so a script's `decode` on an image
// input does not report "no image decoder".
TEST(SeriesRunnerTest, AChunkGetsTheSessionsImageDecoder) {
  struct NoDecoder final : scripting::IImageDecoder {
    Expected<scripting::DecodedPixels> decode(
        const sdk::Image&, const std::shared_ptr<scripting::native::MemoryBudget>&) const override {
      return unexpected("unused");
    }
  };
  SeriesFixture fx;
  fx.manager.setImageDecoder(std::make_shared<NoDecoder>());
  const ObjectTopicId frames =
      fx.manager.objectStore().registerTopic({fx.ds, "frames", R"({"builtin_object_type":"kImage"})"}).value();
  for (const Timestamp t : {1 * kSec, 2 * kSec}) {
    sdk::Image frame;
    frame.encoding = "rgb8";
    frame.timestamp_ns = t;
    ASSERT_TRUE(
        fx.manager.objectStore().pushOwned(frames, t, serializeBuiltinObject(sdk::BuiltinObject(frame)).value()));
  }
  fx.session.catalogModel().rebuildFromDatastore();
  fx.install(seriesRecipe("ser", {"frames"}));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));

  ASSERT_EQ(fx.fake.records().size(), 2u);
  for (const test::FakeRun::Record& record : fx.fake.records()) {
    EXPECT_TRUE(record.has_image_decoder);
  }
}

// ---- Visible range first, plugin reads ------------------------------------------------------------------------------

constexpr auto kChunk = static_cast<int>(SeriesRunner::kInstantsPerChunk);

// GAP-40: a series whose plot starts its view after the watermark computes [visible start, end] first, then backfills
// its clock up to the visible start; every instant runs once, and the progress counts the whole clock from the start.
TEST(SeriesRunnerTest, PriorityRangeIsComputedFirst) {
  SeriesFixture fx;
  fx.pushMany(200);  // 5, 10, 20 s, then 21..220 s
  fx.showFrom(100 * kSec);
  fx.fake.gated = true;
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  fx.fake.allow(kChunk);
  test::pumpUntil([&] { return fx.rows("owner/ser").size() == SeriesRunner::kInstantsPerChunk; }, 5000);
  ASSERT_EQ(fx.rows("owner/ser").size(), SeriesRunner::kInstantsPerChunk);
  EXPECT_EQ(fx.rows("owner/ser").front().first, 100 * kSec) << "the visible start first";
  EXPECT_EQ(fx.seriesState("owner/ser")->total, 203u) << "the whole clock, the backfill included";
  EXPECT_FALSE(fx.seriesState("owner/ser")->complete);

  fx.fake.allow(1 << 20);
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.runs(), concat({seconds(100, 220), {5 * kSec, 10 * kSec, 20 * kSec}, seconds(21, 99)}));
  EXPECT_EQ(fx.rows("owner/ser").size(), 203u);
  EXPECT_EQ(fx.seriesState("owner/ser")->total, 203u);
  EXPECT_EQ(fx.seriesState("owner/ser")->done, 203u);
}

// GAP-40: while a series has its two ranges, a pan into the backfill's part of the clock computes the backfill next; it
// never starts a third range.
TEST(SeriesRunnerTest, APanReordersWhatIsLeft) {
  SeriesFixture fx;
  fx.pushMany(200);
  fx.showFrom(100 * kSec);
  fx.fake.gated = true;
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitStarted(1)) << "the first chunk of [100 s, end]";
  fx.showFrom(50 * kSec);
  fx.fake.allow(2 * kChunk);
  test::pumpUntil([&] { return fx.rows("owner/ser").size() == 2 * SeriesRunner::kInstantsPerChunk; }, 5000);
  EXPECT_EQ(fx.seriesState("owner/ser")->total, 203u) << "a backfill chunk keeps the whole clock's count";
  fx.fake.allow(1 << 20);
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(
      fx.runs(), concat({seconds(100, 163), {5 * kSec, 10 * kSec, 20 * kSec}, seconds(21, 99), seconds(164, 220)}));
  EXPECT_EQ(fx.rows("owner/ser").size(), 203u);
}

// GAP-40: one split per replay: a pan past the forward range's watermark after the backfill finished starts no other
// range, the forward range goes on in order.
TEST(SeriesRunnerTest, APanAfterTheBackfillSplitsNoMore) {
  SeriesFixture fx;
  fx.pushMany(400);  // 5, 10, 20 s, then 21..420 s
  fx.showFrom(100 * kSec);
  fx.fake.gated = true;
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitStarted(1));
  fx.showFrom(50 * kSec);      // the backfill next, then the forward range again
  fx.fake.allow(kChunk + 82);  // [100 s, 163 s], then the backfill [5 s, 99 s]
  ASSERT_TRUE(fx.waitStarted(kChunk + 82 + 1)) << "the forward range's second chunk";
  fx.showFrom(300 * kSec);
  fx.fake.allow(1 << 20);
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(
      fx.runs(), concat({seconds(100, 163), {5 * kSec, 10 * kSec, 20 * kSec}, seconds(21, 99), seconds(164, 420)}));
}

// GAP-40: a live tick while the backfill computes continues the forward range after its watermark, then the backfill
// resumes from its own.
TEST(SeriesRunnerTest, LiveAppendContinuesTheForwardRange) {
  SeriesFixture fx;
  fx.pushMany(200);
  fx.showFrom(100 * kSec);
  fx.fake.gated = true;
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  fx.fake.allow(121);  // [100 s, 220 s]
  ASSERT_TRUE(fx.waitStarted(122)) << "the backfill's first run";
  fx.push(fx.cloud, 230 * kSec);
  fx.tickNoWait();
  fx.fake.allow(1 << 20);
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(
      fx.runs(),
      concat({seconds(100, 220), {5 * kSec, 10 * kSec, 20 * kSec}, seconds(21, 81), {230 * kSec}, seconds(82, 99)}));
  EXPECT_EQ(fx.rows("owner/ser").size(), 204u);
}

// An entry that arrived, while the series stopped, inside what the backfill will compute is the backfill's: the split
// replays nothing.
TEST(SeriesRunnerTest, AnArrivalInTheBackfillRangeReplaysNothing) {
  SeriesFixture fx;
  fx.pushMany(200);
  fx.fake.gated = true;
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  fx.fake.allow(kChunk);
  test::pumpUntil([&] { return fx.rows("owner/ser").size() == SeriesRunner::kInstantsPerChunk; }, 5000);
  ASSERT_TRUE(fx.waitStarted(kChunk + 1));
  fx.hide("owner/ser");
  fx.fake.allow(1);
  fx.settle();
  fx.fake.gated = false;

  fx.push(fx.cloud, 90 * kSec + kSec / 2);  // after the watermark (81 s), before the visible start
  fx.showFrom(100 * kSec);
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.rows("owner/ser").size(), 204u);
  EXPECT_EQ(fx.runs().size(), 204u + 1) << "every instant once, and the one the stop cancelled again";
}

// A view that starts inside the computed rows splits nothing: a live tick appends after the watermark.
TEST(SeriesRunnerTest, AViewInsideTheComputedRowsSplitsNothing) {
  SeriesFixture fx;  // clouds at 5, 10, 20 s
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  fx.showFrom(10 * kSec);
  fx.push(fx.cloud, 30 * kSec);
  fx.tick();
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.runs(), (std::vector<Timestamp>{5 * kSec, 10 * kSec, 20 * kSec, 30 * kSec}));
}

// While the dataset is live, a view past the live edge splits nothing: the instants that arrive before it are computed.
TEST(SeriesRunnerTest, ALiveViewPastTheEdgeSplitsNothing) {
  SeriesFixture fx;  // clouds at 5, 10, 20 s
  fx.tick();
  fx.showFrom(100 * kSec);
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  fx.push(fx.cloud, 30 * kSec);
  fx.tick();
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.runs(), (std::vector<Timestamp>{5 * kSec, 10 * kSec, 20 * kSec, 30 * kSec}));
}

// GAP-40: a replay, its last chunk included, is not complete while it computes (the legend says "computing N%"); a
// live append to a complete series keeps it complete (the legend says nothing) while `replaying` reports the append.
TEST(SeriesRunnerTest, ALiveAppendIsNotAReplay) {
  SeriesFixture fx;  // clouds at 5, 10, 20 s: one chunk
  fx.fake.gated = true;
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  fx.fake.allow(2);
  ASSERT_TRUE(fx.waitStarted(3)) << "the last run of the replay's only chunk";
  EXPECT_TRUE(fx.seriesState("owner/ser")->replaying);
  EXPECT_FALSE(fx.seriesState("owner/ser")->complete);
  fx.fake.allow(1);
  ASSERT_TRUE(fx.waitComplete("owner/ser"));

  fx.push(fx.cloud, 30 * kSec);
  fx.tickNoWait();
  ASSERT_TRUE(fx.waitStarted(4)) << "the append";
  EXPECT_TRUE(fx.seriesState("owner/ser")->replaying);
  EXPECT_TRUE(fx.seriesState("owner/ser")->complete) << "an append keeps the series complete";
  fx.fake.allow(1);
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.rows("owner/ser").size(), 4u);
}

// GAP-40 (decision 2): a plugin's whole-series read of a series that is not computed is an explicit request: the read
// returns the rows so far and the series computes to the end, even undisplayed; a read from a plugin's worker thread
// too.
TEST(SeriesRunnerTest, APluginReadOfAnUncomputedSeriesStartsIt) {
  SeriesFixture fx;
  fx.install(seriesRecipe("ser"));
  fx.install(seriesRecipe("other"));
  fx.settle();
  ASSERT_EQ(fx.fake.started(), 0);

  PluginReader plugin(fx.manager);
  sdk::ToolboxHostView toolbox = plugin.toolbox();
  const std::optional<sdk::FieldHandle> ser = plugin.field("owner/ser");
  const std::optional<sdk::FieldHandle> other = plugin.field("owner/other");
  ASSERT_TRUE(ser.has_value());
  ASSERT_TRUE(other.has_value());

  auto partial = toolbox.readSeries(*ser);
  ASSERT_TRUE(partial.has_value()) << partial.error();
  EXPECT_EQ(partial->rowCount(), 0u) << "the rows so far";
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.rows("owner/ser").size(), 3u);
  EXPECT_FALSE(fx.series().isDemanded("owner/ser"));

  std::thread([&] { (void)toolbox.readSeries(*other); }).join();
  ASSERT_TRUE(fx.waitComplete("owner/other"));
  EXPECT_EQ(fx.rows("owner/other").size(), 3u);
}

// Codex 11b-1: a backfilled series' rows were committed after the forward range's, yet a plugin reads them in time
// order (the Arrow export merges the topic's chunks), with each value at its own timestamp.
TEST(SeriesRunnerTest, APluginReadOfABackfilledSeriesIsInTimeOrder) {
  SeriesFixture fx;
  fx.pushMany(200);
  fx.showFrom(100 * kSec);
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  PluginReader plugin(fx.manager);
  const std::optional<sdk::FieldHandle> field = plugin.field("owner/ser");
  ASSERT_TRUE(field.has_value());
  auto read = plugin.toolbox().readSeries(*field);
  ASSERT_TRUE(read.has_value()) << read.error();
  ASSERT_EQ(read->rowCount(), 203u);
  const auto times = read->timestamps();
  const std::vector<Timestamp> exported(times.begin(), times.end());
  EXPECT_EQ(exported, concat({{5 * kSec, 10 * kSec, 20 * kSec}, seconds(21, 220)}));
  const double* values = read->valuesAsFloat64();
  ASSERT_NE(values, nullptr);
  for (std::size_t row = 0; row < exported.size(); ++row) {
    ASSERT_EQ(values[row], static_cast<double>(exported[row])) << "row " << row;
  }
}

// Codex 11b-1: a view past the last sample leaves the forward range empty; the backfill's end merges the two at the
// backfill's own watermark, so a later live sample is computed.
TEST(SeriesRunnerTest, AnEmptyForwardRangeMergesIntoTheBackfill) {
  SeriesFixture fx;  // clouds at 5, 10, 20 s
  fx.showFrom(100 * kSec);
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  fx.push(fx.cloud, 30 * kSec);
  fx.tick();
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.runs(), (std::vector<Timestamp>{5 * kSec, 10 * kSec, 20 * kSec, 30 * kSec}));
}

// Codex 11b-1: a late entry restarts a complete series' clock: it is no live append, so it is not complete from the
// chunk's submission on.
TEST(SeriesRunnerTest, ARestartIsNotComplete) {
  SeriesFixture fx;  // clouds at 5, 10, 20 s
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  fx.fake.gated = true;
  fx.push(fx.cloud, 15 * kSec);  // before the watermark
  fx.tickNoWait();
  ASSERT_TRUE(fx.waitStarted(4)) << "the replay from the start";
  EXPECT_TRUE(fx.seriesState("owner/ser")->replaying);
  EXPECT_FALSE(fx.seriesState("owner/ser")->complete);
  fx.fake.allow(1 << 20);
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  EXPECT_EQ(fx.rows("owner/ser").size(), 4u);
}

// Codex 11b-1: the session keeps the plots' view start on the display axis, so a change of the dataset's display offset
// moves the raw instant the series computes first.
TEST(SeriesRunnerTest, ADisplayOffsetChangeMovesTheVisibleStart) {
  SeriesFixture fx;
  fx.pushMany(200);
  fx.showFrom(100 * kSec);  // display 100 s while the offset is 0
  fx.manager.setDisplayOffset(fx.ds, DisplayOffset{Duration{50 * kSec}});
  fx.install(seriesRecipe("ser"));
  fx.display("owner/ser");
  ASSERT_TRUE(fx.waitComplete("owner/ser"));
  ASSERT_FALSE(fx.runs().empty());
  EXPECT_EQ(fx.runs().front(), 150 * kSec) << "display 100 s is raw 150 s under the new offset";
}

// ---- Measurement ----------------------------------------------------------------------------------------------------

double threadCpuMicros() {
  return pj_bench::threadCpuMs() * 1e3;
}

// PERF-03: a replay over 400 clouds of 24k points (the fake run, instant): the GUI thread's longest event-loop pass and
// its CPU time per row, the test's pump included; then the same replay with the view starting mid-clock (GAP-40: the
// forward range, then the backfill).
void benchReplay(std::optional<Timestamp> visible_start) {
  SeriesFixture fx;
  const ObjectTopicId big = fx.addCloudTopic(fx.ds, "big", {});
  for (int index = 0; index < 400; ++index) {
    fx.push(big, index * kSec / 10, 24'000);
  }
  if (visible_start) {
    fx.showFrom(*visible_start);
  }
  fx.install(seriesRecipe("ser", {"big"}));
  const double begin = threadCpuMicros();
  double longest = 0;
  fx.display("owner/ser");
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 20000) {
    const double before = threadCpuMicros();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    longest = std::max(longest, threadCpuMicros() - before);
    const auto state = fx.seriesState("owner/ser");
    if (state && state->complete && !state->replaying) {
      break;
    }
  }
  const double total = threadCpuMicros() - begin;
  ASSERT_EQ(fx.rows("owner/ser").size(), 400u);
  std::printf(
      "[SeriesRunnerBench] 400 rows over 24k-point clouds%s: GUI CPU %.1f us per row, longest pass %.0f us\n",
      visible_start ? ", view from mid-clock" : "", total / 400, longest);
}

TEST(SeriesRunnerBench, ReplayOver400Clouds) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  benchReplay(std::nullopt);
  benchReplay(20 * kSec);
}

// A one-hour 10 Hz replay (36,000 rows over 4-point clouds) through AppSession, from the display to the last row, wall
// time: the fake run (instant), then the real Luau run of `script` on each worker's sandboxed state. Their difference
// is what the Luau runs cost; with a trivial script it is the per-call setup.
double replay36kMillis(const char* script) {
  SeriesFixture fx;
  fx.fake.real = script != nullptr;
  const ObjectTopicId big = fx.addCloudTopic(fx.ds, "big", {});
  for (int index = 0; index < 36'000; ++index) {
    fx.push(big, index * kSec / 10);
  }
  RecipeGraph::Recipe recipe = seriesRecipe("ser", {"big"});
  if (script != nullptr) {
    recipe.script = script;
  }
  fx.install(std::move(recipe));
  QElapsedTimer timer;
  timer.start();
  fx.display("owner/ser");
  while (timer.elapsed() < 120'000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    const auto state = fx.seriesState("owner/ser");
    if (state && state->complete && !state->replaying) {
      break;
    }
  }
  const double millis = static_cast<double>(timer.nsecsElapsed()) / 1e6;
  EXPECT_EQ(fx.rows("owner/ser").size(), 36'000u);
  return millis;
}

// F8 (Luau VM reuse): the share of a 36,000-row series replay that the per-row Luau runs take, over a trivial script
// (the per-call setup alone) and one that reads its cloud.
TEST(SeriesRunnerBench, Replay36kRowsLuauRunShare) {
  if (!sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  const double fake = replay36kMillis(nullptr);
  std::printf("[SeriesRunnerBench] 36k rows, fake instant run: %.0f ms\n", fake);
  for (const char* script :
       {"local inputs = ...\nreturn { n = 1 }\n",
        "local inputs = ...\nlocal b = inputs.big:bounds()\nreturn { n = b and b.max[1] - b.min[1] or 0 }\n"}) {
    const double real = replay36kMillis(script);
    std::printf(
        "[SeriesRunnerBench] 36k rows, real Luau run: %.0f ms; the runs take %.0f ms (%.1f us per row, %.0f%% of the "
        "replay)\n%s",
        real, real - fake, (real - fake) * 1000.0 / 36'000.0, 100.0 * (real - fake) / real, script);
  }
}

}  // namespace
}  // namespace PJ
