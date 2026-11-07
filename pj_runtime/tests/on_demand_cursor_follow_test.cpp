// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// AppSession drives the on-demand engine (docs/derived_objects/ARCHITECTURE.md §8): the playhead requests every
// demanded recipe at its dataset's raw time, a display reference (the demand tracker) makes a recipe demanded, a
// recipe just installed is requested where the cursor is parked, and a pinned finding at its pin. Driven through a
// real AppSession with the fake script run (`test::FakeRun`) the session's scheduler uses.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QTemporaryDir>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "on_demand_session_test_utils.h"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/EnginePool.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/TopicDemandTracker.h"

namespace PJ {
namespace {

using Status = NodeScheduler::NodeState::Status;

using test::canonicalCloud;
using test::cloudSpec;
using test::kSec;
using CursorFixture = test::OnDemandSessionFixture;

/// `owner/<id>` reading `input`; its script is the fake's to run.
RecipeGraph::Recipe recipe(std::string id, std::string input = "cloud") {
  return test::onDemandRecipe(std::move(id), {std::move(input)}, {cloudSpec()});
}

std::string frameOf(Timestamp query_time) {
  return std::to_string(query_time);
}

// CUR-10: the playhead requests every demanded recipe at its instant; a revisited instant is a hit (no run, no
// publication).
TEST(OnDemandCursorFollowTest, MovingThePlayheadMaterializesEveryRecipeAtThatInstant) {
  CursorFixture fx;
  fx.install(recipe("crop"));
  fx.install(recipe("crop2"));
  fx.display("owner/crop/cropped");
  fx.display("owner/crop2/cropped");
  fx.settle();
  EXPECT_TRUE(fx.ring("owner/crop/cropped").empty()) << "the install at the cursor's start publishes nothing yet";
  const int before = fx.fake.finished();

  fx.seek(20 * kSec);
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{20 * kSec}));
  EXPECT_EQ(fx.ring("owner/crop2/cropped"), (std::vector<Timestamp>{20 * kSec}));
  EXPECT_EQ(fx.shown("owner/crop/cropped", 20 * kSec).frame_id, frameOf(20 * kSec));
  fx.seek(5 * kSec);
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{5 * kSec, 20 * kSec}));
  const int runs = fx.fake.finished();
  EXPECT_EQ(runs - before, 4);

  fx.seek(20 * kSec);
  EXPECT_EQ(fx.fake.finished(), runs) << "a revisit is a hit";
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{5 * kSec, 20 * kSec}));
  EXPECT_EQ(fx.state("owner/crop")->status, Status::kReady);
}

// CUR-11, D6: before the first input nothing runs and nothing is written; the state names the input.
TEST(OnDemandCursorFollowTest, AnInstantBeforeTheFirstInputPublishesNothingAndDoesNotThrow) {
  CursorFixture fx;
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  fx.seek(1 * kSec);
  EXPECT_TRUE(fx.ring("owner/crop/cropped").empty());
  EXPECT_EQ(fx.fake.started(), 0);
  ASSERT_TRUE(fx.state("owner/crop").has_value());
  EXPECT_EQ(fx.state("owner/crop")->status, Status::kMissingInput);
  EXPECT_NE(fx.state("owner/crop")->detail.find("'cloud'"), std::string::npos) << fx.state("owner/crop")->detail;
}

// CUR-12: a topic displayed before its recipe exists (an embedded scene view) is evaluated at the parked cursor
// when the recipe is installed, with no tick.
TEST(OnDemandCursorFollowTest, UpsertAtAParkedCursorEvaluatesWithoutATick) {
  CursorFixture fx;
  fx.seek(20 * kSec);
  fx.display("owner/crop3/cropped");
  fx.install(recipe("crop3"));
  fx.settle();
  EXPECT_EQ(fx.ring("owner/crop3/cropped"), (std::vector<Timestamp>{20 * kSec}));
  EXPECT_EQ(fx.shown("owner/crop3/cropped", 20 * kSec).frame_id, frameOf(20 * kSec));
}

// CUR-13: an edit of a demanded recipe clears its output and evaluates again at the parked cursor.
TEST(OnDemandCursorFollowTest, EditingADemandedRecipeReEvaluatesWithoutATick) {
  CursorFixture fx;
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  fx.seek(20 * kSec);
  ASSERT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{20 * kSec}));

  RecipeGraph::Recipe edited = recipe("crop");
  edited.params_json = R"({"zmin": 1})";
  fx.install(edited);
  EXPECT_TRUE(fx.ring("owner/crop/cropped").empty()) << "an edit clears the stale result";
  fx.settle();
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{20 * kSec}));
  EXPECT_EQ(fx.fake.finished(), 2);
}

// DEM-01: an undemanded recipe never follows the playhead.
TEST(OnDemandCursorFollowTest, OnlyDemandedRecipesFollowThePlayhead) {
  CursorFixture fx;
  fx.install(recipe("crop"));
  fx.install(recipe("crop2"));
  fx.display("owner/crop/cropped");
  fx.seek(20 * kSec);
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{20 * kSec}));
  EXPECT_TRUE(fx.ring("owner/crop2/cropped").empty());
  EXPECT_FALSE(fx.state("owner/crop2").has_value()) << "never requested";
}

// DEM-02: demand arriving while the cursor is parked evaluates now.
TEST(OnDemandCursorFollowTest, DemandArrivingAtAStationaryCursorEvaluatesNow) {
  CursorFixture fx;
  fx.install(recipe("crop2"));
  fx.seek(20 * kSec);
  ASSERT_TRUE(fx.ring("owner/crop2/cropped").empty());
  fx.display("owner/crop2/cropped");
  fx.settle();
  EXPECT_EQ(fx.ring("owner/crop2/cropped"), (std::vector<Timestamp>{20 * kSec}));
}

// DEM-03 (decided), GAP-09: when demand leaves, the pending request is dropped and the node is idle; the running run
// completes and its result enters the history; nothing runs after it.
TEST(OnDemandCursorFollowTest, DemandLeavingDropsThePendingRequestAndKeepsTheRunningResult) {
  CursorFixture fx;
  fx.fake.gated = true;
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  fx.seekNoWait(10 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  fx.seekNoWait(20 * kSec);  // pending behind the running run
  ASSERT_EQ(fx.state("owner/crop")->status, Status::kPending);

  fx.hide("owner/crop/cropped");
  EXPECT_EQ(fx.state("owner/crop")->status, Status::kIdle);
  fx.fake.allow(2);
  fx.settle();
  EXPECT_EQ(fx.fake.started(), 1) << "the dropped 20 s request never ran";
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{10 * kSec})) << "the 10 s run is kept";
  EXPECT_EQ(fx.state("owner/crop")->status, Status::kIdle);

  fx.seek(5 * kSec);
  EXPECT_EQ(fx.fake.started(), 1) << "an undemanded node does not follow the playhead";
}

// ASY-09 (changed by D30): a seek during a run does not cancel it; the run publishes, then the new instant runs.
TEST(OnDemandCursorFollowTest, SeekDuringComputeFinishesTheRunAndThenServesTheNewInstant) {
  CursorFixture fx;
  fx.fake.gated = true;
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  fx.seekNoWait(10 * kSec);
  ASSERT_TRUE(fx.fake.waitStarted(1));
  fx.seekNoWait(20 * kSec);
  fx.fake.allow(2);
  fx.settle();
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_FALSE(records[0].cancelled);
  EXPECT_EQ(records[0].query_time, 10 * kSec);
  EXPECT_EQ(records[1].query_time, 20 * kSec);
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{10 * kSec, 20 * kSec}));
  EXPECT_EQ(fx.state("owner/crop")->status, Status::kReady);
  EXPECT_EQ(fx.state("owner/crop")->requested_t, 20 * kSec);
}

// Two consumers of one output (two layers): the node stays demanded until the last one leaves.
TEST(OnDemandCursorFollowTest, ASecondConsumerKeepsTheNodeDemanded) {
  CursorFixture fx;
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  fx.display("owner/crop/cropped");
  fx.hide("owner/crop/cropped");
  EXPECT_TRUE(fx.manager.onDemandService().isDemanded("owner/crop"));
  fx.seek(10 * kSec);
  EXPECT_EQ(fx.ring("owner/crop/cropped"), (std::vector<Timestamp>{10 * kSec}));

  fx.hide("owner/crop/cropped");
  EXPECT_FALSE(fx.manager.onDemandService().isDemanded("owner/crop"));
  const int runs = fx.fake.started();
  fx.seek(20 * kSec);
  EXPECT_EQ(fx.fake.started(), runs);
}

// PIN-05: a displayed finding is never followed by the playhead.
TEST(OnDemandCursorFollowTest, PinnedRecipesAreNotFollowed) {
  CursorFixture fx;
  RecipeGraph::Recipe pinned = recipe("pin");
  pinned.pinned_t = 20 * kSec;
  fx.display("owner/pin/cropped");
  fx.install(pinned);
  fx.settle();
  ASSERT_EQ(fx.fake.finished(), 1);
  EXPECT_FALSE(fx.manager.onDemandService().isDemanded("owner/pin")) << "a finding is never demanded";

  fx.seek(5 * kSec);
  fx.seek(10 * kSec);
  EXPECT_EQ(fx.fake.finished(), 1);
  EXPECT_EQ(fx.ring("owner/pin/cropped"), (std::vector<Timestamp>{kFindingTimestamp}));
  EXPECT_EQ(fx.state("owner/pin")->requested_t, 20 * kSec);
}

// PIN-05 (changed by N7/D26): a finding is evaluated once, on install, at its pin, even undisplayed; its single
// entry sits at the finding timestamp and its payload carries the pinned time.
TEST(OnDemandCursorFollowTest, PinnedRecipeIsEvaluatedOnInstallOnly) {
  CursorFixture fx;
  fx.seek(5 * kSec);
  RecipeGraph::Recipe pinned = recipe("pin");
  pinned.pinned_t = 20 * kSec;
  fx.install(pinned);
  fx.settle();
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].query_time, 20 * kSec);
  EXPECT_EQ(fx.ring("owner/pin/cropped"), (std::vector<Timestamp>{kFindingTimestamp}));
  EXPECT_EQ(fx.shown("owner/pin/cropped", 0).frame_id, frameOf(20 * kSec));

  fx.seek(20 * kSec);
  EXPECT_EQ(fx.fake.finished(), 1);
}

// GAP-33: evaluations, findings and publications never move the playhead.
TEST(OnDemandCursorFollowTest, EvaluationNeverMovesTheCursor) {
  CursorFixture fx;
  fx.seek(5 * kSec);
  const DisplaySeconds cursor = fx.session.playbackEngine().currentTime();
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  RecipeGraph::Recipe pinned = recipe("pin");
  pinned.pinned_t = 20 * kSec;
  fx.install(pinned);
  fx.settle();
  ASSERT_EQ(fx.fake.finished(), 2);
  EXPECT_EQ(fx.session.playbackEngine().currentTime().value, cursor.value);
}

// Each dataset's recipes are requested at the playhead's raw time in that dataset (its display offset).
TEST(OnDemandCursorFollowTest, CursorConversionUsesEachDatasetsDisplayOffset) {
  CursorFixture fx;
  const DatasetId shifted = fx.addDataset("shifted", 30 * kSec);
  fx.addCloudTopic(shifted, "cloud_b", {40 * kSec});
  fx.install(recipe("crop"));
  fx.install(recipe("crop_b", "cloud_b"));
  fx.display("owner/crop/cropped");
  fx.display("owner/crop_b/cropped", shifted);
  fx.seek(20 * kSec);  // display 20 s: raw 20 s in `log`, raw 50 s in `shifted`
  EXPECT_EQ(fx.state("owner/crop")->requested_t, 20 * kSec);
  EXPECT_EQ(fx.state("owner/crop_b")->requested_t, 50 * kSec);
  EXPECT_EQ(fx.ring("owner/crop_b/cropped", shifted), (std::vector<Timestamp>{40 * kSec}));
}

// CUR-07: derived topics (a result, and a finding at the finding timestamp) never move the dataset's range or the
// playback range.
TEST(OnDemandCursorFollowTest, DerivedTopicsDoNotMoveDatasetBounds) {
  CursorFixture fx;
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  RecipeGraph::Recipe pinned = recipe("pin");
  pinned.pinned_t = 20 * kSec;
  fx.install(pinned);
  fx.seek(20 * kSec);
  ASSERT_EQ(fx.ring("owner/pin/cropped"), (std::vector<Timestamp>{kFindingTimestamp}));
  fx.session.catalogModel().rebuildFromDatastore();

  const auto raw = fx.session.datasetRawTimeRange(fx.ds);
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(raw->min, 5 * kSec);
  EXPECT_EQ(raw->max, 20 * kSec);
  const auto display = fx.manager.datasetDisplayRange(fx.ds);
  ASSERT_TRUE(display.has_value());
  EXPECT_DOUBLE_EQ(display->min.value, 5.0);
  EXPECT_DOUBLE_EQ(display->max.value, 20.0);
  ASSERT_TRUE(fx.session.recomputeRange().has_value());
  EXPECT_DOUBLE_EQ(fx.session.playbackEngine().rangeMin().value, 5.0);
  EXPECT_DOUBLE_EQ(fx.session.playbackEngine().rangeMax().value, 20.0);
}

// D4 through the session: a recorded string field, named as the catalog names it, is an input, whose topic the
// displayed recipe subscribes; the playhead hands the run the state at or before it.
TEST(OnDemandCursorFollowTest, AStringFieldFollowsThePlayhead) {
  CursorFixture fx;
  DataWriter writer = fx.manager.dataEngine().createWriter();
  const TopicId status = test::registerStringTopic(writer, fx.ds, "/robot/status");
  test::appendString(writer, status, 5 * kSec, "idle");
  test::appendString(writer, status, 15 * kSec, "moving");
  ASSERT_FALSE(fx.manager.commitChunks(writer.flushAll()).empty());
  fx.session.catalogModel().rebuildFromDatastore();
  fx.install(test::onDemandRecipe("annotate", {"cloud", "/robot/status/state"}, {cloudSpec()}));
  fx.display("owner/annotate/cropped");
  EXPECT_EQ(
      fx.session.topicDemandTracker().activeTopics(fx.ds),
      (std::vector<QString>{"/robot/status", "cloud", "owner/annotate/cropped"}));

  fx.seek(10 * kSec);
  fx.seek(20 * kSec);
  ASSERT_EQ(fx.state("owner/annotate")->status, Status::kReady) << fx.state("owner/annotate")->detail;
  const auto records = fx.fake.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].strings, std::vector<std::string>{"idle"});
  EXPECT_EQ(records[1].strings, std::vector<std::string>{"moving"});
  EXPECT_EQ(fx.ring("owner/annotate/cropped"), (std::vector<Timestamp>{10 * kSec, 20 * kSec}));
}

// A window evaluation reading a string input, directly or behind an upstream, is refused naming the input: the string's
// change instants are not read yet, so the window would skip them and still report full coverage. An instant reads
// the string as a request does.
TEST(OnDemandEvaluationTest, AWindowReadingAStringInputIsRefused) {
  CursorFixture fx;
  DataWriter writer = fx.manager.dataEngine().createWriter();
  const TopicId status = test::registerStringTopic(writer, fx.ds, "/robot/status");
  test::appendString(writer, status, 10 * kSec, "idle");
  test::appendString(writer, status, 20 * kSec, "moving");
  ASSERT_FALSE(fx.manager.commitChunks(writer.flushAll()).empty());
  fx.session.catalogModel().rebuildFromDatastore();
  fx.install(test::onDemandRecipe("label", {"/robot/status/state"}, {cloudSpec()}));
  fx.install(test::onDemandRecipe("annotate", {"cloud", "/robot/status/state"}, {cloudSpec()}));
  fx.install(recipe("count", "owner/annotate/cropped"));
  GridRunner& runner = fx.manager.onDemandService().evaluations();
  const auto submit = [&](const std::string& key, Timestamp start, Timestamp end) {
    return runner.submit(*fx.manager.onDemandService().graph().find(key), true, start, end, {}, [](auto) {});
  };

  for (const std::string key : {"owner/label", "owner/annotate", "owner/count"}) {
    const auto window = submit(key, 10 * kSec, 30 * kSec);
    ASSERT_FALSE(window.has_value()) << key;
    EXPECT_EQ(
        window.error(),
        "on-demand: input '/robot/status/state' is a string series, whose changes a window evaluation cannot read "
        "yet; evaluate an instant")
        << key;
  }
  const auto instant = submit("owner/label", 15 * kSec, 15 * kSec);
  ASSERT_TRUE(instant.has_value()) << instant.error();
  fx.settle();
}

// A window reading a column that is neither a number nor a string (an untyped one, or a topic without columns) is
// refused as unreadable, not as a string series: the refusal names what the column is not. An instant is evaluated.
TEST(OnDemandEvaluationTest, AWindowReadingAnUnreadableColumnIsRefusedAccurately) {
  CursorFixture fx;
  DataWriter writer = fx.manager.dataEngine().createWriter();
  const auto register_topic = [&](const std::string& name, std::shared_ptr<TypeTreeNode> tree) {
    const auto schema = writer.registerSchema(name + "/schema", std::move(tree));
    EXPECT_TRUE(schema.has_value()) << schema.error();
    TopicDescriptor descriptor;
    descriptor.name = name;
    descriptor.schema_id = schema ? *schema : SchemaId{};
    EXPECT_TRUE(writer.registerTopic(fx.ds, descriptor).has_value());
  };
  register_topic("/robot/untyped", makeStruct("untyped", {makePrimitive("untyped", PrimitiveType::kUnspecified)}));
  register_topic("/robot/empty", makeStruct("empty", {}));
  fx.session.catalogModel().rebuildFromDatastore();
  GridRunner& runner = fx.manager.onDemandService().evaluations();

  for (const std::string input : {"/robot/untyped", "/robot/empty"}) {
    fx.install(test::onDemandRecipe("read", {input}, {cloudSpec()}));
    const auto submit = [&](Timestamp start, Timestamp end) {
      return runner.submit(*fx.manager.onDemandService().graph().find("owner/read"), true, start, end, {}, [](auto) {});
    };
    const auto window = submit(10 * kSec, 30 * kSec);
    ASSERT_FALSE(window.has_value()) << input;
    EXPECT_EQ(
        window.error(), "on-demand: input '" + input +
                            "' is neither a numeric nor a string series, so a window evaluation cannot "
                            "read its changes")
        << input;
    const auto instant = submit(15 * kSec, 15 * kSec);
    ASSERT_TRUE(instant.has_value()) << input << ": " << instant.error();
    fx.settle();
  }
}

// The refusal of a window follows the column's real type: a string column a stream adds after rows are stored
// (ensureColumn) is a string series, as one its schema declares; a topic without any column, whose exact name reads
// column 0 past the last one, is neither a number nor a string. The numeric column added before stays readable.
TEST(OnDemandEvaluationTest, AWindowReadingAColumnAddedMidStreamIsRefusedByItsType) {
  CursorFixture fx;
  DataWriter writer = fx.manager.dataEngine().createWriter();
  const auto register_schemaless = [&](const std::string& name) {
    TopicDescriptor descriptor;
    descriptor.name = name;
    const auto topic = writer.registerTopic(fx.ds, descriptor);
    EXPECT_TRUE(topic.has_value()) << topic.error();
    return topic ? *topic : TopicId{};
  };
  const TopicId stream = register_schemaless("/robot/stream");
  register_schemaless("/robot/bare");

  ASSERT_TRUE(writer.ensureColumn(stream, "gain", PrimitiveType::kFloat64).has_value());
  ASSERT_TRUE(writer.beginRow(stream, 10 * kSec).has_value());
  writer.set(stream, 0, 1.5);
  ASSERT_TRUE(writer.finishRow(stream).has_value());
  ASSERT_FALSE(fx.manager.commitChunks(writer.flushAll()).empty());
  const auto label = writer.ensureColumn(stream, "label", PrimitiveType::kString);
  ASSERT_TRUE(label.has_value()) << label.error();
  ASSERT_TRUE(writer.beginRow(stream, 20 * kSec).has_value());
  writer.set(stream, 0, 2.5);
  writer.set(stream, *label, std::string_view("moving"));
  ASSERT_TRUE(writer.finishRow(stream).has_value());
  ASSERT_FALSE(fx.manager.commitChunks(writer.flushAll()).empty());
  fx.session.catalogModel().rebuildFromDatastore();

  GridRunner& runner = fx.manager.onDemandService().evaluations();
  const auto window_error = [&](const std::string& input) {
    fx.install(test::onDemandRecipe("read", {input}, {cloudSpec()}));
    const auto window = runner.submit(
        *fx.manager.onDemandService().graph().find("owner/read"), true, 10 * kSec, 30 * kSec, {}, [](auto) {});
    return window.has_value() ? std::string() : window.error();
  };
  EXPECT_EQ(
      window_error("/robot/stream/label"),
      "on-demand: input '/robot/stream/label' is a string series, whose changes a window evaluation cannot read yet; "
      "evaluate an instant");
  EXPECT_EQ(
      window_error("/robot/bare"),
      "on-demand: input '/robot/bare' is neither a numeric nor a string series, so a window evaluation cannot read "
      "its changes");
  EXPECT_EQ(window_error("/robot/stream/gain"), std::string()) << "a numeric column is read";
  fx.settle();
}

// A merge re-binds a consumed dataset's recipe on the anchor once the catalog lists the merged series there: a
// scalar input resolves through the catalog, which AppSession rebuilds after the store's merge.
TEST(AppSessionTest, MergeReBindsAScalarInputOnTheAnchor) {
  CursorFixture fx;
  const DatasetId other = fx.addDataset("other");
  fx.addCloudTopic(other, "cloud_b", {20 * kSec});
  DataWriter writer = fx.manager.dataEngine().createWriter();
  const ScalarSeriesHandle gain = writer.registerScalarSeries(other, "gain", NumericType::kFloat64).value();
  writer.appendScalar(gain, 15 * kSec, 2.0);
  ASSERT_FALSE(fx.manager.commitChunks(writer.flushAll()).empty());
  fx.session.catalogModel().rebuildFromDatastore();
  fx.install(test::onDemandRecipe("scaled", {"cloud_b", "gain/value"}, {cloudSpec()}));
  fx.display("owner/scaled/cropped", other);
  fx.seek(20 * kSec);
  ASSERT_EQ(fx.state("owner/scaled")->status, Status::kReady);

  ASSERT_EQ(fx.session.mergeDatasets({fx.ds, other}), fx.ds);
  fx.settle();
  const RecipeGraph::Node* node = fx.manager.onDemandService().graph().find("owner/scaled");
  ASSERT_NE(node, nullptr) << "the recipe still resolves";
  EXPECT_EQ(node->dataset, fx.ds);
  EXPECT_EQ(fx.state("owner/scaled")->status, Status::kReady) << fx.state("owner/scaled")->detail;
  EXPECT_EQ(fx.ring("owner/scaled/cropped"), (std::vector<Timestamp>{20 * kSec}));
}

// A merge installs the re-bound recipes producers first: a reader of an upstream and of a scalar of the consumed
// dataset re-binds once its upstream is on the anchor (in key order the reader comes first).
TEST(AppSessionTest, MergeReinstallsReadersAfterTheirUpstreams) {
  CursorFixture fx;
  const DatasetId other = fx.addDataset("other");
  fx.addCloudTopic(other, "cloud_b", {20 * kSec});
  DataWriter writer = fx.manager.dataEngine().createWriter();
  const ScalarSeriesHandle gain = writer.registerScalarSeries(other, "gain", NumericType::kFloat64).value();
  writer.appendScalar(gain, 15 * kSec, 2.0);
  ASSERT_FALSE(fx.manager.commitChunks(writer.flushAll()).empty());
  fx.session.catalogModel().rebuildFromDatastore();
  fx.install(recipe("z", "cloud_b"));
  fx.install(test::onDemandRecipe("a", {"owner/z/cropped", "gain/value"}, {cloudSpec()}));
  fx.display("owner/a/cropped", other);
  fx.seek(20 * kSec);
  ASSERT_EQ(fx.state("owner/a")->status, Status::kReady);

  ASSERT_EQ(fx.session.mergeDatasets({fx.ds, other}), fx.ds);
  fx.settle();
  for (const std::string key : {"owner/z", "owner/a"}) {
    const RecipeGraph::Node* node = fx.manager.onDemandService().graph().find(key);
    ASSERT_NE(node, nullptr) << key << " still resolves";
    EXPECT_EQ(node->dataset, fx.ds) << key;
  }
  EXPECT_EQ(fx.state("owner/a")->status, Status::kReady) << fx.state("owner/a")->detail;
}

// A recipe a layout restore left bound to a dataset (its saved input dataset) that a merge then consumes is bound to
// the anchor, not dropped for naming a dataset that no longer exists.
TEST(AppSessionTest, MergeRebindsARecipeBoundToTheConsumedDataset) {
  CursorFixture fx;
  const DatasetId other = fx.addDataset("other");
  fx.addCloudTopic(other, "cloud_b", {20 * kSec});
  RecipeGraph::Recipe bound = recipe("crop", "cloud_b");
  bound.dataset = other;
  fx.install(bound);
  ASSERT_EQ(fx.manager.onDemandService().graph().find("owner/crop")->dataset, other);

  ASSERT_EQ(fx.session.mergeDatasets({fx.ds, other}), fx.ds);
  fx.settle();
  const RecipeGraph::Node* node = fx.manager.onDemandService().graph().find("owner/crop");
  ASSERT_NE(node, nullptr) << "the recipe survives the merge";
  EXPECT_EQ(node->dataset, fx.ds);
  EXPECT_EQ(node->recipe.dataset, fx.ds);
}

// A display offset changed while the playhead is parked moves the raw time its demanded recipes are requested at.
TEST(OnDemandCursorFollowTest, ADisplayOffsetChangeRequestsTheNewRawTime) {
  CursorFixture fx;
  fx.install(recipe("crop"));
  fx.display("owner/crop/cropped");
  fx.seek(10 * kSec);
  ASSERT_EQ(fx.state("owner/crop")->requested_t, 10 * kSec);
  fx.manager.setDisplayOffset(fx.ds, DisplayOffset{std::chrono::seconds(5)});
  fx.settle();
  EXPECT_EQ(fx.state("owner/crop")->requested_t, 15 * kSec);
  fx.manager.setUseTimeOffset(true);  // the global frame: display time relative to the earliest sample, 5 s
  fx.settle();
  EXPECT_EQ(fx.state("owner/crop")->requested_t, 20 * kSec);
}

// DEM-05: a scene layer's display reference (the AppSession tracker) reaches the on-demand demand set.
TEST(AppSessionTest, SceneLayerReferenceReachesTheOnDemandDemandSet) {
  CursorFixture fx;
  fx.install(recipe("crop"));
  const OnDemandService& service = fx.manager.onDemandService();
  EXPECT_FALSE(service.isDemanded("owner/crop"));
  fx.display("owner/crop/cropped");
  EXPECT_TRUE(service.isDemanded("owner/crop"));
  EXPECT_EQ(service.demandedKeys(), (std::vector<std::string>{"owner/crop"}));
  fx.hide("owner/crop/cropped");
  EXPECT_FALSE(service.isDemanded("owner/crop"));
  EXPECT_TRUE(service.demandedKeys().empty());
}

}  // namespace
}  // namespace PJ
