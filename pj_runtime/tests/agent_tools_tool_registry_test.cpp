// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Drives the ToolRegistry executors against fake C-ABI hosts: a ToolboxTestStore
// for the read tools (list/describe/read) and a hand-rolled recording
// data-processors host for the write tools (create_derived_series /
// create_markers), asserting the exact script + inputs the tool synthesizes.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_plugins/testing/toolbox_test_store.hpp>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "fake_catalog_host.hpp"
#include "fake_multi_dataset_host.hpp"
#include "fake_object_read_host.hpp"
#include "fake_playback_viewport_hosts.hpp"
#include "fake_plot_tabs_host.hpp"
#include "pj_runtime/agent_tools/object_ops_catalog.hpp"
#include "pj_runtime/agent_tools/tool_registry.hpp"
#include "recording_dp_host.hpp"

namespace {

using nlohmann::json;
using PJ::agent_tools::ToolContext;
using PJ::agent_tools::ToolRegistry;
using PJ::testing::FakeCatalogHost;
using PJ::testing::FakeMultiDatasetHost;
using PJ::testing::FakeObjectReadHost;
using PJ::testing::FakePlaybackHost;
using PJ::testing::FakePlotTabsHost;
using PJ::testing::FakeViewportHost;
using PJ::testing::RecordingDpHost;

constexpr std::int64_t kSec = 1'000'000'000;

// Populate a small catalog: /imu with fields x (0..4) and y (constant 2).
// ToolboxTestStore is non-movable (it hands out pointers into itself), so
// callers construct it locally and pass it here by reference.
void populate(PJ::testing::ToolboxTestStore& store) {
  store.addTopic("/imu");
  store.addField("/imu", "x", {0, kSec, 2 * kSec, 3 * kSec, 4 * kSec}, {0.0, 1.0, 2.0, 3.0, 4.0});
  store.addField("/imu", "y", {0, kSec, 2 * kSec}, {2.0, 2.0, 2.0});
}

ToolContext makeCtx(PJ::testing::ToolboxTestStore& store, RecordingDpHost* dp, FakeObjectReadHost* objects = nullptr) {
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(store.makeHost());
  if (dp != nullptr) {
    ctx.dp = dp->view();
  }
  if (objects != nullptr) {
    ctx.objects = PJ::sdk::ToolboxObjectReadHostView(objects->makeHost());
  }
  return ctx;
}

TEST(ToolRegistry, ListsAllToolsAndSchemas) {
  ToolRegistry reg;
  EXPECT_EQ(reg.tools().size(), 14u);
  EXPECT_EQ(reg.toMcpToolsList().size(), 14u);
  EXPECT_NE(reg.find("evaluate"), nullptr);
  EXPECT_NE(reg.find("create_derived_series"), nullptr);
  EXPECT_NE(reg.find("create_derived_object"), nullptr);
  EXPECT_NE(reg.find("playback"), nullptr);
  EXPECT_EQ(reg.find("play"), nullptr);
  EXPECT_NE(reg.find("plot_tab"), nullptr);
  EXPECT_NE(reg.find("scene_view"), nullptr);
  // zoom_to_time_range/zoom_reset were folded into plot_tab's 'zoom' action:
  // the user's plots are no longer reachable from any tool, only the tabs
  // this assistant composed itself.
  EXPECT_EQ(reg.find("zoom_to_time_range"), nullptr);
  EXPECT_EQ(reg.find("zoom_reset"), nullptr);
  // remove_markers exists but is scoped to the assistant's own marker set;
  // no tool can touch user data destructively.
  EXPECT_NE(reg.find("remove_markers"), nullptr);
  EXPECT_EQ(reg.find("delete_everything"), nullptr);
}

TEST(ToolRegistry, UnknownToolIsCleanFailure) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("nope", json::object(), ctx);
  EXPECT_FALSE(r.ok);
}

TEST(ToolRegistry, ListTopics) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("list_topics", json::object(), ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["topics"][0]["topic"], "/imu");
  EXPECT_EQ(j["topics"][0]["fields"], 2);
}

// Object topics from catalog snapshot v2 (point clouds, scene entities…) show
// up alongside scalar ones, tagged by kind so the model can tell them apart
// without a second call.
TEST(ToolRegistry, ListTopicsIncludesObjectTopicsFromSnapshotV2) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addTopic("/imu");
  host.addField("/imu", "x");
  host.addObjectTopic("/cloud", "kPointCloud", 42, 0, 5 * kSec);
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  auto r = reg.execute("list_topics", json::object(), ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["count"], 2);
  EXPECT_FALSE(j.contains("objects")) << "v2 is supported here; the fallback note must not appear";
  bool found_scalar = false;
  bool found_object = false;
  for (const auto& t : j["topics"]) {
    if (t["topic"] == "/imu") {
      found_scalar = true;
      EXPECT_EQ(t["kind"], "scalar");
    }
    if (t["topic"] == "/cloud") {
      found_object = true;
      EXPECT_EQ(t["kind"], "object");
      EXPECT_EQ(t["type"], "kPointCloud");
      EXPECT_EQ(t["entries"], 42);
      EXPECT_EQ(t["derived"], false);
      EXPECT_EQ(t["time_basis"], "raw") << "no playback view bound in this test";
      ASSERT_TRUE(t.contains("t_range_s"));
      EXPECT_NEAR(t["t_range_s"][0].get<double>(), 0.0, 1e-9);
      EXPECT_NEAR(t["t_range_s"][1].get<double>(), 5.0, 1e-6);
    }
  }
  EXPECT_TRUE(found_scalar);
  EXPECT_TRUE(found_object);
}

// A host built before catalog snapshot v2 existed: object topics are simply
// not visible, and the response says so rather than silently under-reporting.
TEST(ToolRegistry, ListTopicsFallsBackToV1AndSaysSo) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addTopic("/imu");
  host.addField("/imu", "x");
  host.supportsV2(false);
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  auto r = reg.execute("list_topics", json::object(), ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["topics"][0]["topic"], "/imu");
  ASSERT_TRUE(j.contains("objects"));
  EXPECT_EQ(j["objects"], "not listed (host predates catalog snapshot v2)");
}

// Marker sets are object topics too, but they are drawn, not read -- they
// must never reach the model through list_topics.
TEST(ToolRegistry, ListTopicsExcludesMarkerTopics) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("__markers__/imu/x", "kPlotMarkers", 1, 0, kSec);
  host.addObjectTopic("/cloud", "kPointCloud", 3, 0, kSec);
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  auto r = reg.execute("list_topics", json::object(), ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["count"], 1);
  EXPECT_EQ(j["topics"][0]["topic"], "/cloud");
}

TEST(ToolRegistry, DescribeTopicListsFieldPaths) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("describe_topic", {{"topic", "/imu"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  ASSERT_EQ(j["fields"].size(), 2u);
  EXPECT_EQ(j["fields"][0]["path"], "/imu/x");
}

TEST(ToolRegistry, DescribeUnknownTopicFails) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("describe_topic", {{"topic", "/nope"}}, ctx);
  EXPECT_FALSE(r.ok);
}

// An object topic that exists on two datasets is described like every other object input: the bare name is
// refused with the qualified candidates, and "dataset:name" picks one.
TEST(ToolRegistry, DescribeObjectTopicHonoursTheDatasetQualifierAndReportsAmbiguity) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, kSec, R"({"builtin_object_type":"kPointCloud"})", "runA");
  host.addObjectTopic("/cloud", "kImage", 7, 0, kSec, R"({"builtin_object_type":"kImage"})", "runB");
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());

  auto ambiguous = reg.execute("describe_topic", {{"topic", "/cloud"}}, ctx);
  EXPECT_FALSE(ambiguous.ok) << ambiguous.content;
  EXPECT_NE(ambiguous.content.find("ambiguous"), std::string::npos) << ambiguous.content;
  EXPECT_NE(ambiguous.content.find("runA:/cloud"), std::string::npos) << ambiguous.content;
  EXPECT_NE(ambiguous.content.find("runB:/cloud"), std::string::npos) << ambiguous.content;

  auto b = reg.execute("describe_topic", {{"topic", "runB:/cloud"}}, ctx);
  ASSERT_TRUE(b.ok) << b.content;
  EXPECT_EQ(json::parse(b.content)["type"], "kImage");
  EXPECT_EQ(json::parse(b.content)["entries"], 7);
  auto a = reg.execute("describe_topic", {{"topic", "runA:/cloud"}}, ctx);
  ASSERT_TRUE(a.ok) << a.content;
  EXPECT_EQ(json::parse(a.content)["type"], "kPointCloud");
}

// A scalar topic on two datasets: "dataset:name" describes that dataset's topic, and its field paths keep the
// qualifier, so read_series takes them as they are.
TEST(ToolRegistry, DescribeScalarTopicHonoursTheDatasetQualifier) {
  ToolRegistry reg;
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu"}).addDataset("run_friday.mcap", {"/imu", "/speed"});
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  for (const std::string dataset : {"run_monday.mcap", "run_friday.mcap"}) {
    auto r = reg.execute("describe_topic", {{"topic", dataset + ":/imu"}}, ctx);
    ASSERT_TRUE(r.ok) << r.content;
    const json j = json::parse(r.content);
    EXPECT_EQ(j["dataset"], dataset);
    EXPECT_EQ(j["topic"], "/imu");
    EXPECT_EQ(j["fields"][0]["path"], dataset + ":/imu/value");
  }
  EXPECT_FALSE(reg.execute("describe_topic", {{"topic", "run_monday.mcap:/speed"}}, ctx).ok);

  // Resolved like read_series: a name on several datasets is refused with the qualified candidates, a unique one
  // (leading '/' optional) answers with paths read_series takes as they are.
  const auto ambiguous = reg.execute("describe_topic", {{"topic", "/imu"}}, ctx);
  EXPECT_FALSE(ambiguous.ok);
  EXPECT_NE(ambiguous.content.find("run_monday.mcap:/imu"), std::string::npos) << ambiguous.content;
  EXPECT_NE(ambiguous.content.find("run_friday.mcap:/imu"), std::string::npos) << ambiguous.content;
  const auto unique = reg.execute("describe_topic", {{"topic", "speed"}}, ctx);
  ASSERT_TRUE(unique.ok) << unique.content;
  EXPECT_EQ(json::parse(unique.content)["fields"][0]["path"], "run_friday.mcap:/speed/value");
}

// describe_topic on an object topic walks its field table (PointCloud's own
// fields, a list shown as "kind":"list", the raw "data" buffer carrying
// nothing past its name/kind) and lists the operations a script may call on
// it, mirroring pj_scripting/src/object_binder.cpp in PJ4.
TEST(ToolRegistry, DescribeObjectTopicWalksTheFieldTableAndListsOperations) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, kSec);
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  auto r = reg.execute("describe_topic", {{"topic", "/cloud"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["kind"], "object");
  EXPECT_EQ(j["type"], "kPointCloud");
  EXPECT_EQ(j["entries"], 100);

  std::vector<std::string> field_names;
  for (const auto& f : j["fields"]) {
    field_names.push_back(f["name"].get<std::string>());
    if (f["name"] == "fields") {
      EXPECT_EQ(f["kind"], "list");
    }
    if (f["name"] == "data") {
      EXPECT_EQ(f["kind"], "buffer");
      EXPECT_FALSE(f.contains("fields")) << "a buffer field carries nothing past its name/kind";
    }
  }
  EXPECT_NE(std::find(field_names.begin(), field_names.end(), "frame_id"), field_names.end());
  EXPECT_NE(std::find(field_names.begin(), field_names.end(), "width"), field_names.end());
  EXPECT_NE(std::find(field_names.begin(), field_names.end(), "fields"), field_names.end());
  EXPECT_NE(std::find(field_names.begin(), field_names.end(), "data"), field_names.end());

  bool found_crop_box = false;
  for (const auto& op : j["operations"]) {
    if (op["name"].get<std::string>().find("crop_box") != std::string::npos) {
      found_crop_box = true;
    }
  }
  EXPECT_TRUE(found_crop_box) << j.dump();
}

TEST(ToolRegistry, DescribeScalarTopicSaysKindScalar) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("describe_topic", {{"topic", "/imu"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["kind"], "scalar");
}

TEST(ToolRegistry, ReadSeriesStats) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["stats"]["count"], 5);
  EXPECT_DOUBLE_EQ(j["stats"]["min"].get<double>(), 0.0);
  EXPECT_DOUBLE_EQ(j["stats"]["max"].get<double>(), 4.0);
  // The spacing facts ride along in the same report, self-described by key.
  EXPECT_TRUE(j["stats"].contains("max_gap_s"));
  EXPECT_TRUE(j["stats"].contains("max_gap_at_s"));
}

// A NaN sample must not silently poison min/max/mean/stddev: the tool reports
// it as 'invalid'/'invalid_fraction' instead, and only when it is present.
TEST(ToolRegistry, ReadSeriesStatsFlagsNonFiniteValues) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  store.addField("/imu", "nan_x", {0, kSec, 2 * kSec, 3 * kSec}, {1.0, std::nan(""), 3.0, 4.0});
  auto ctx = makeCtx(store, nullptr);

  auto dirty = reg.execute("read_series", {{"series", "/imu/nan_x"}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(dirty.ok) << dirty.content;
  auto dj = json::parse(dirty.content);
  EXPECT_EQ(dj["stats"]["invalid"], 1);
  EXPECT_DOUBLE_EQ(dj["stats"]["invalid_fraction"].get<double>(), 0.25);
  EXPECT_DOUBLE_EQ(dj["stats"]["min"].get<double>(), 1.0);
  EXPECT_DOUBLE_EQ(dj["stats"]["max"].get<double>(), 4.0);

  auto clean = reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(clean.ok) << clean.content;
  auto cj = json::parse(clean.content);
  EXPECT_FALSE(cj["stats"].contains("invalid"));
  EXPECT_FALSE(cj["stats"].contains("invalid_fraction"));
}

TEST(ToolRegistry, ReadSeriesBuckets) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "buckets"}, {"max_points", 3}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  ASSERT_TRUE(j.contains("buckets"));
  EXPECT_TRUE(j["buckets"].contains("t0"));
  EXPECT_TRUE(j["buckets"].contains("dt"));
  EXPECT_LE(j["buckets"]["n"].size(), 5u);
  EXPECT_EQ(j["buckets"]["min"].size(), j["buckets"]["n"].size());
  EXPECT_EQ(j["buckets"]["max"].size(), j["buckets"]["n"].size());
  EXPECT_EQ(j["buckets"]["mean"].size(), j["buckets"]["n"].size());
}

// Regular buckets (an even time grid, none dropped) reconstruct from t0/dt
// alone -- no explicit "t" array, and t0/dt exactly reproduce what the old
// per-bucket "t" values were.
TEST(ToolRegistry, BucketsAreColumnar) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/rc");
  constexpr int kSamples = 500;
  std::vector<std::int64_t> ts;
  std::vector<double> vals;
  ts.reserve(kSamples);
  vals.reserve(kSamples);
  for (int i = 0; i < kSamples; ++i) {
    ts.push_back(static_cast<std::int64_t>(i) * (kSec / 100));  // 100 Hz, perfectly regular
    vals.push_back(std::sin(static_cast<double>(i) * 0.01));
  }
  store.addField("/rc", "ch", ts, vals);
  auto ctx = makeCtx(store, nullptr);

  auto r = reg.execute("read_series", {{"series", "/rc/ch"}, {"mode", "buckets"}, {"max_points", 10}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  ASSERT_TRUE(j.contains("buckets")) << r.content;
  const json& b = j["buckets"];
  EXPECT_FALSE(b.contains("t")) << "a regular grid must not pay for an explicit 't' array: " << b.dump();
  ASSERT_GE(b["n"].size(), 2u);
  const double t0 = b["t0"].get<double>();
  const double dt = b["dt"].get<double>();
  EXPECT_NEAR(t0, 0.0, 1e-9) << "first bucket starts at the series' own first sample";
  EXPECT_GT(dt, 0.0);
  // 500 samples at 100 Hz span just under 5 s; 10 buckets over that span put
  // dt somewhere around 0.5 s -- loosely, to avoid pinning bucketize's exact
  // edge-rounding here.
  EXPECT_NEAR(dt, 0.499, 0.05);
}

// bucketize's degenerate "one bucket per sample" path (fewer samples than
// max_points) over UNEVEN timestamps cannot be reconstructed from t0/dt, so
// the explicit "t" array must carry the true per-sample times -- same values
// the old per-bucket "t" field held.
TEST(ToolRegistry, IrregularBucketsCarryExplicitTimes) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/uneven");
  // 4 samples, irregular spacing: 0, 1, 3, 4 s -- fewer than max_points(10),
  // so bucketize emits one bucket per sample verbatim.
  store.addField("/uneven", "x", {0, kSec, 3 * kSec, 4 * kSec}, {10.0, 11.0, 12.0, 13.0});
  auto ctx = makeCtx(store, nullptr);

  auto r = reg.execute("read_series", {{"series", "/uneven/x"}, {"mode", "buckets"}, {"max_points", 10}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  const json& b = j["buckets"];
  ASSERT_TRUE(b.contains("t")) << "uneven spacing must carry explicit times: " << b.dump();
  const std::vector<double> expected_t = {0.0, 1.0, 3.0, 4.0};
  ASSERT_EQ(b["t"].size(), expected_t.size());
  for (std::size_t i = 0; i < expected_t.size(); ++i) {
    EXPECT_DOUBLE_EQ(b["t"][i].get<double>(), expected_t[i]);
  }
}

// A bucket with count == 0 (all its samples non-finite) reports null
// min/max/mean but keeps a real 0 in "n" -- the data is missing, not silently
// dropped from the arrays.
TEST(ToolRegistry, EmptyBucketIsNull) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/bad");
  // 3 buckets worth of evenly spaced samples; the middle third is all NaN so
  // its bucket has count == 0 but invalid > 0.
  std::vector<std::int64_t> ts;
  std::vector<double> vals;
  for (int i = 0; i < 30; ++i) {
    ts.push_back(static_cast<std::int64_t>(i) * (kSec / 10));
    vals.push_back(i >= 10 && i < 20 ? std::nan("") : static_cast<double>(i));
  }
  store.addField("/bad", "x", ts, vals);
  auto ctx = makeCtx(store, nullptr);

  auto r = reg.execute("read_series", {{"series", "/bad/x"}, {"mode", "buckets"}, {"max_points", 3}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  const json& b = j["buckets"];
  ASSERT_TRUE(b.contains("invalid")) << b.dump();
  bool found_null_bucket = false;
  for (std::size_t i = 0; i < b["n"].size(); ++i) {
    if (b["n"][i].get<int>() == 0) {
      found_null_bucket = true;
      EXPECT_TRUE(b["min"][i].is_null());
      EXPECT_TRUE(b["max"][i].is_null());
      EXPECT_TRUE(b["mean"][i].is_null());
      EXPECT_GT(b["invalid"][i].get<int>(), 0);
    }
  }
  EXPECT_TRUE(found_null_bucket) << b.dump();
}

// The whole point of going columnar: under the same 16 KiB response cap, many
// more buckets fit than the old one-object-per-bucket shape allowed (~240 for
// a spiky series measured on real data). A 5000-sample series should now keep
// several hundred buckets instead of coarsening down toward the 16-bucket
// floor.
TEST(ToolRegistry, ColumnarFitsMoreBucketsUnderTheCap) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/spiky");
  constexpr int kSamples = 5000;
  std::vector<std::int64_t> ts;
  std::vector<double> vals;
  ts.reserve(kSamples);
  vals.reserve(kSamples);
  for (int i = 0; i < kSamples; ++i) {
    ts.push_back(static_cast<std::int64_t>(i) * (kSec / 100));
    // Spiky, high-entropy values so min/max/mean rarely repeat and rounding
    // to 6 significant digits does not accidentally shrink the payload.
    vals.push_back(std::sin(static_cast<double>(i) * 0.137) * 1000.0 + static_cast<double>(i % 7) * 0.0001234567);
  }
  store.addField("/spiky", "x", std::move(ts), std::move(vals));
  auto ctx = makeCtx(store, nullptr);

  auto r = reg.execute("read_series", {{"series", "/spiky/x"}, {"mode", "buckets"}, {"max_points", 500}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_LE(r.content.size(), 16u * 1024u) << "response cap: " << r.content.size();
  const json j = json::parse(r.content);
  const json& b = j["buckets"];
  const std::size_t bucket_count = b["n"].size();
  EXPECT_GE(bucket_count, 400u) << "columnar buckets should fit far more than the old shape did: " << bucket_count;

  // Reconstruct the equivalent old one-object-per-bucket array from this
  // response's own arrays (same bucket count, same values) and dump it, so
  // the before/after byte-per-bucket comparison below is measured on the
  // exact same data rather than a hand-picked example.
  json old_shape = json::array();
  for (std::size_t i = 0; i < bucket_count; ++i) {
    const double t = b.contains("t") ? b["t"][i].get<double>()
                                     : b["t0"].get<double>() + static_cast<double>(i) * b["dt"].get<double>();
    json entry = {{"t", t}, {"n", b["n"][i]}};
    if (!b["min"][i].is_null()) {
      entry["min"] = b["min"][i];
      entry["max"] = b["max"][i];
      entry["mean"] = b["mean"][i];
    }
    if (b.contains("invalid") && b["invalid"][i].get<int>() > 0) {
      entry["invalid"] = b["invalid"][i];
    }
    old_shape.push_back(std::move(entry));
  }
  const std::size_t old_bytes = old_shape.dump().size();
  const std::size_t new_bytes = b.dump().size();
  std::cerr << "bucket encoding on " << bucket_count << " buckets: old shape " << old_bytes << " bytes ("
            << (static_cast<double>(old_bytes) / static_cast<double>(bucket_count)) << " bytes/bucket), columnar "
            << new_bytes << " bytes (" << (static_cast<double>(new_bytes) / static_cast<double>(bucket_count))
            << " bytes/bucket)\n";
  EXPECT_LT(new_bytes, old_bytes) << "columnar must be smaller than the old per-bucket-object shape on the same data";
}

// /imu/y (populate()) never changes: {2.0, 2.0, 2.0}.
TEST(ToolRegistry, ReadSeriesStatsFlagsConstantSeries) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "/imu/y"}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_TRUE(j["stats"]["constant"].get<bool>());
  EXPECT_FALSE(j["stats"].contains("flat_span_s"));
  EXPECT_FALSE(j["stats"].contains("flat_span_at_s"));
  ASSERT_TRUE(j["stats"].contains("constant_note")) << r.content;
  const std::string note = j["stats"]["constant_note"].get<std::string>();
  EXPECT_NE(note.find("entire recording"), std::string::npos) << note;
  EXPECT_NE(note.find("not something that changed"), std::string::npos) << note;

  // /imu/x varies throughout -> neither key at all, and no note either.
  auto varying = reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(varying.ok) << varying.content;
  auto vj = json::parse(varying.content);
  EXPECT_FALSE(vj["stats"].contains("constant"));
  EXPECT_FALSE(vj["stats"].contains("flat_span_s"));
  EXPECT_FALSE(vj["stats"].contains("constant_note"));
  EXPECT_FALSE(vj["stats"].contains("flat_span_note"));
}

// A servo-like channel: varies for most of the recording, then freezes for
// the tail — the real aileron-jam shape this feature exists to surface.
TEST(ToolRegistry, ReadSeriesStatsFlagsLongFlatSpan) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  // 20 samples, 1 Hz: index 0..14 vary, 15..19 freeze at the same value —
  // 4 s of freeze out of a 19 s span (~21%), starting at t=15 s.
  std::vector<std::int64_t> ts;
  std::vector<double> v;
  for (int i = 0; i < 20; ++i) {
    ts.push_back(static_cast<std::int64_t>(i) * kSec);
    v.push_back(i < 15 ? static_cast<double>(i % 5) : 42.0);
  }
  store.addField("/imu", "servo", ts, v);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "/imu/servo"}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_FALSE(j["stats"].contains("constant"));
  EXPECT_FALSE(j["stats"].contains("constant_note"));
  ASSERT_TRUE(j["stats"].contains("flat_span_s")) << r.content;
  EXPECT_NEAR(j["stats"]["flat_span_s"].get<double>(), 4.0, 1e-9);
  EXPECT_NEAR(j["stats"]["flat_span_at_s"].get<double>(), 15.0, 1e-9);
  ASSERT_TRUE(j["stats"].contains("flat_span_note")) << r.content;
  const std::string note = j["stats"]["flat_span_note"].get<std::string>();
  // Render the expected numbers from the fixture's own values (flat_span_s,
  // flat_span_at_s) rather than hard-coding a guess at the format.
  std::ostringstream expected_span;
  expected_span << std::fixed << std::setprecision(1) << j["stats"]["flat_span_s"].get<double>() << " s";
  std::ostringstream expected_at;
  expected_at << std::fixed << std::setprecision(1) << j["stats"]["flat_span_at_s"].get<double>() << " s";
  EXPECT_NE(note.find(expected_span.str()), std::string::npos) << note;
  EXPECT_NE(note.find(expected_at.str()), std::string::npos) << note;
  EXPECT_NE(note.find("within the recording"), std::string::npos) << note;
}

// The flat span is a whole-recording fact while t_start_display_s is the window's first sample, so a windowed read
// gives the span's own display time: the freeze at 15 s of a recording that starts at display 0 is at display 15,
// whatever window was read.
TEST(ToolRegistry, WindowedReadGivesTheFlatSpansOwnDisplayTime) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  std::vector<std::int64_t> ts;
  std::vector<double> v;
  for (int i = 0; i < 20; ++i) {
    ts.push_back(static_cast<std::int64_t>(i) * kSec);
    v.push_back(i < 15 ? static_cast<double>(i % 5) : 42.0);
  }
  store.addField("/imu", "servo", ts, v);
  FakePlaybackHost pb;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  auto r = reg.execute("read_series", {{"series", "/imu/servo"}, {"t_start_s", 5.0}, {"t_end_s", 10.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json stats = json::parse(r.content)["stats"];
  EXPECT_DOUBLE_EQ(stats["t_start_display_s"].get<double>(), 5.0);
  EXPECT_NEAR(stats["flat_span_at_s"].get<double>(), 15.0, 1e-9);
  ASSERT_TRUE(stats.contains("flat_span_display_s")) << r.content;
  EXPECT_NEAR(stats["flat_span_display_s"].get<double>(), 15.0, 1e-9);
  const std::string note = stats["flat_span_note"].get<std::string>();
  EXPECT_NE(note.find("display 15.0 s"), std::string::npos) << note;
  EXPECT_EQ(note.find("t_start_display_s"), std::string::npos) << note;
}

TEST(ToolRegistry, ReadUnknownSeriesFails) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "/imu/z"}}, ctx);
  EXPECT_FALSE(r.ok);
}

// A missing series must point the model at the one check that would settle it,
// instead of letting it conclude the signal does not exist from a stale listing.
TEST(ToolRegistry, UnknownSeriesErrorTellsTheModelToVerifyOnce) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "/imu/nope"}}, ctx);
  ASSERT_FALSE(r.ok);
  EXPECT_NE(r.content.find("list_topics"), std::string::npos) << r.content;
  EXPECT_NE(r.content.find("once"), std::string::npos) << r.content;
}

// An abbreviated path resolves when only one series can match — this is what
// keeps a slightly-wrong path from costing a correction round-trip.
TEST(ToolRegistry, AbbreviatedSeriesPathResolvesWhenUnambiguous) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);  // /imu with fields x and y
  auto ctx = makeCtx(store, nullptr);
  // "x" alone names exactly one series (/imu/x), so it must resolve.
  auto r = reg.execute("read_series", {{"series", "x"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["stats"]["count"], 5);
}

// ...but never by guessing: two candidates must come back as a listed choice.
TEST(ToolRegistry, AmbiguousSeriesPathListsCandidatesInsteadOfGuessing) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/left");
  store.addField("/left", "speed", {0, kSec}, {1.0, 2.0});
  store.addTopic("/right");
  store.addField("/right", "speed", {0, kSec}, {3.0, 4.0});
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "speed"}}, ctx);
  ASSERT_FALSE(r.ok) << "picking one of two same-named signals silently is worse than failing";
  EXPECT_NE(r.content.find("/left/speed"), std::string::npos) << r.content;
  EXPECT_NE(r.content.find("/right/speed"), std::string::npos) << r.content;
}

// Whole-segment matching: half a segment must NOT resolve, or "/im" would
// silently attach to "/imu/x".
TEST(ToolRegistry, PartialSegmentDoesNotResolve) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "/im"}}, ctx);
  EXPECT_FALSE(r.ok) << r.content;
  EXPECT_NE(r.content.find("unknown series"), std::string::npos) << r.content;
}

// A topic with `count` numeric fields w0..w(count-1), each a single dummy
// sample — only the FIELD COUNT matters for the over-cap tests below, not
// the data shape. ToolboxTestStore hardcodes every catalog field to
// float64 (see acquireCatalogSnapshot in toolbox_test_store.hpp), so every
// field this populates is numeric; there is no way to add a string/bool
// field through this fixture to exercise the "excluded from expansion" half
// of "numeric = every type except string and bool".
void populateWideTopic(PJ::testing::ToolboxTestStore& store, const std::string& topic, int count) {
  store.addTopic(topic);
  for (int i = 0; i < count; ++i) {
    store.addField(topic, "w" + std::to_string(i), {0}, {static_cast<double>(i)});
  }
}

// A servo topic with a dozen array fields, read one path at a time, is the
// real-data pattern that motivated this: models asked for channel 0-4 and
// never noticed the jammed surface sat on channel 5. A bare topic path now
// reads every numeric field at once.
TEST(ToolRegistry, WholeTopicExpandsToItsNumericFields) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);  // /imu with fields x and y, both numeric
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"paths", json::array({"/imu"})}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["count"], 2);
  ASSERT_EQ(j["read"].size(), 2u);
  EXPECT_EQ(j["read"][0]["series"], "/imu/x");
  EXPECT_EQ(j["read"][1]["series"], "/imu/y");
  EXPECT_FALSE(j["read"][0].contains("error")) << j.dump();
  EXPECT_FALSE(j["read"][1].contains("error")) << j.dump();
}

// Hosts differ on whether a topic name carries its leading '/'; the model
// may or may not echo it back either way.
TEST(ToolRegistry, WholeTopicWithLeadingSlashOrWithout) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto with_slash = reg.execute("read_series", {{"paths", json::array({"/imu"})}, {"mode", "stats"}}, ctx);
  auto without_slash = reg.execute("read_series", {{"paths", json::array({"imu"})}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(with_slash.ok) << with_slash.content;
  ASSERT_TRUE(without_slash.ok) << without_slash.content;
  EXPECT_EQ(json::parse(with_slash.content), json::parse(without_slash.content));
}

// A field of the topic named explicitly, alongside the topic itself, must
// not be read twice.
TEST(ToolRegistry, WholeTopicMixesWithFieldPaths) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"paths", json::array({"/imu/x", "/imu"})}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["count"], 2) << "the topic's own x must not duplicate the explicitly named /imu/x: " << j.dump();
  std::vector<std::string> names;
  for (const auto& entry : j["read"]) {
    names.push_back(entry["series"].get<std::string>());
  }
  EXPECT_NE(std::find(names.begin(), names.end(), "/imu/x"), names.end()) << j.dump();
  EXPECT_NE(std::find(names.begin(), names.end(), "/imu/y"), names.end()) << j.dump();
}

// Buckets share one response cap across the batch (kMaxBucketsBatchPaths ==
// 8), so a topic alone naming more numeric fields than that cannot be
// expanded silently — it fails as its own entry, naming the way out
// ('stats' first), while any other requested path in the same call still
// succeeds.
TEST(ToolRegistry, WholeTopicBucketsOverCapFailsThatEntry) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  populateWideTopic(store, "/wide", 9);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"paths", json::array({"/wide", "/imu/x"})}, {"mode", "buckets"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["count"], 2);
  const json* wide_entry = nullptr;
  const json* imu_entry = nullptr;
  for (const auto& entry : j["read"]) {
    if (entry["series"] == "/wide") {
      wide_entry = &entry;
    } else if (entry["series"] == "/imu/x") {
      imu_entry = &entry;
    }
  }
  ASSERT_NE(wide_entry, nullptr) << j.dump();
  ASSERT_NE(imu_entry, nullptr) << j.dump();
  ASSERT_TRUE(wide_entry->contains("error")) << j.dump();
  EXPECT_NE((*wide_entry)["error"].get<std::string>().find("stats first"), std::string::npos) << j.dump();
  EXPECT_FALSE(imu_entry->contains("error")) << "the other requested path must still succeed: " << j.dump();
}

// Stats' own cap (kMaxBatchPaths == 32) is loose enough that a big topic is
// worth reading partially rather than refusing outright: the first 32
// numeric fields come back, with a note explaining the truncation instead
// of silently dropping the rest.
TEST(ToolRegistry, WholeTopicStatsOverCapReadsFirstAndSaysSo) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populateWideTopic(store, "/wide", 40);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"paths", json::array({"/wide"})}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["count"], 32);
  ASSERT_EQ(j["read"].size(), 32u);
  ASSERT_TRUE(j.contains("note")) << j.dump();
  const std::string note = j["note"].get<std::string>();
  EXPECT_NE(note.find("40"), std::string::npos) << note;
  EXPECT_NE(note.find("32"), std::string::npos) << note;
}

// The expansion path must carry the same whole-series facts a directly
// named read gets — /imu/y (populate()) never changes.
TEST(ToolRegistry, WholeTopicCarriesTheWholeSeriesFacts) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"paths", json::array({"/imu"})}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  const json* y_entry = nullptr;
  for (const auto& entry : j["read"]) {
    if (entry["series"] == "/imu/y") {
      y_entry = &entry;
    }
  }
  ASSERT_NE(y_entry, nullptr) << j.dump();
  EXPECT_EQ((*y_entry)["stats"]["constant"], true) << j.dump();
  EXPECT_TRUE((*y_entry)["stats"].contains("constant_note")) << j.dump();
}

// Reading only some of a topic's numeric fields must say so, in the same
// response, at the moment of the decision -- putting the fact where the
// model already is beat putting it in the system prompt (measured: zero use
// of the bare-topic form across 20 real cells even after the prompt said
// so). /imu (populate()) only has x and y; a third field is added locally so
// this test can exercise a genuinely partial read without disturbing every
// other test's assumption that /imu has exactly two fields.
TEST(ToolRegistry, PartialTopicReadListsTheUnreadFields) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);  // /imu: x, y
  store.addField("/imu", "z", {0, kSec, 2 * kSec}, {5.0, 5.0, 5.0});
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  ASSERT_TRUE(j.contains("unread")) << j.dump();
  ASSERT_TRUE(j["unread"].contains("/imu")) << j.dump();
  const json& unread = j["unread"]["/imu"];
  EXPECT_EQ(unread["read"], 1);
  EXPECT_EQ(unread["numeric_fields"], 3);
  EXPECT_EQ(unread["fields"], json::array({"y", "z"})) << unread.dump();
  EXPECT_EQ(unread["hint"], "a bare topic path reads every field of the topic in one call");
}

// The bare-topic form itself reads every numeric field, so it never triggers
// its own disclosure.
TEST(ToolRegistry, WholeTopicReadHasNoUnread) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"paths", json::array({"/imu"})}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_FALSE(j.contains("unread")) << j.dump();
}

// The unread field list is capped so a very wide topic does not blow up the
// response the disclosure is meant to keep small; a truncated list ends in a
// literal ellipsis entry rather than silently dropping the rest.
TEST(ToolRegistry, UnreadIsCappedAtTwelve) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populateWideTopic(store, "/wide", 20);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "/wide/w0"}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  ASSERT_TRUE(j.contains("unread")) << j.dump();
  const json& unread = j["unread"]["/wide"];
  EXPECT_EQ(unread["read"], 1);
  EXPECT_EQ(unread["numeric_fields"], 20);
  ASSERT_EQ(unread["fields"].size(), 13u) << unread.dump();
  EXPECT_EQ(unread["fields"].back(), "…") << unread.dump();
  for (std::size_t i = 0; i < 12; ++i) {
    EXPECT_EQ(unread["fields"][i], "w" + std::to_string(i + 1)) << unread.dump();
  }
}

// header/*-style fields are timestamps and sequence numbers, not signals:
// they must not count toward "numeric_fields" nor show up as "unread" once
// every OTHER numeric field of the topic has been read.
TEST(ToolRegistry, HeaderFieldsDoNotCount) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/hdr");
  store.addField("/hdr", "header/stamp", {0}, {0.0});
  store.addField("/hdr", "header/seq", {0}, {1.0});
  store.addField("/hdr", "value", {0, kSec}, {1.0, 2.0});
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"series", "/hdr/value"}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_FALSE(j.contains("unread")) << j.dump();
}

// The bare-topic expansion excludes header/*-style fields the same way the
// unread disclosure does (HeaderFieldsDoNotCount above): a topic with a ROS
// header plus real data fields expands to just the data fields, so a
// buckets/raw batch cap is never spent on header/stamp/* or header/seq.
TEST(ToolRegistry, BareTopicExpansionExcludesHeaderFields) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/hdr");
  store.addField("/hdr", "header/stamp/sec", {0}, {0.0});
  store.addField("/hdr", "header/seq", {0}, {1.0});
  store.addField("/hdr", "a", {0, kSec}, {1.0, 2.0});
  store.addField("/hdr", "b", {0, kSec}, {3.0, 4.0});
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"paths", json::array({"/hdr"})}, {"mode", "buckets"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["count"], 2) << j.dump();
  ASSERT_EQ(j["read"].size(), 2u) << j.dump();
  EXPECT_EQ(j["read"][0]["series"], "/hdr/a") << j.dump();
  EXPECT_EQ(j["read"][1]["series"], "/hdr/b") << j.dump();
}

// Every read_series shape attaches the same disclosure: the batch envelope
// (mode 'stats' over several paths) gets it once at the top level, with
// exactly one entry for the topic that was actually partial; 'buckets' on a
// single partial field gets it too.
TEST(ToolRegistry, UnreadAppearsInBatchAndBucketsModes) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);  // /imu: x, y
  store.addField("/imu", "z", {0, kSec, 2 * kSec}, {5.0, 5.0, 5.0});
  store.addTopic("/single");
  store.addField("/single", "v", {0, kSec}, {1.0, 2.0});
  auto ctx = makeCtx(store, nullptr);

  auto stats = reg.execute("read_series", {{"paths", json::array({"/single/v", "/imu/x"})}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(stats.ok) << stats.content;
  auto sj = json::parse(stats.content);
  ASSERT_TRUE(sj.contains("unread")) << sj.dump();
  EXPECT_EQ(sj["unread"].size(), 1u) << sj.dump();
  EXPECT_TRUE(sj["unread"].contains("/imu")) << sj.dump();

  auto buckets = reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "buckets"}}, ctx);
  ASSERT_TRUE(buckets.ok) << buckets.content;
  auto bj = json::parse(buckets.content);
  EXPECT_TRUE(bj.contains("unread")) << bj.dump();
}

// A path that names neither a series nor a topic is unaffected by this
// feature: the existing unknown-series error still fires.
TEST(ToolRegistry, UnknownTopicStillErrors) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("read_series", {{"paths", json::array({"/nope"})}, {"mode", "stats"}}, ctx);
  EXPECT_FALSE(r.ok) << r.content;
  EXPECT_NE(r.content.find("unknown series"), std::string::npos) << r.content;
}

// Inputs are resolved before the transform is installed. Without this a
// mistyped input installs happily and yields an empty curve — a failure that
// looks like success, which is the worst outcome available here.
TEST(ToolRegistry, CreateDerivedSeriesRejectsAnInputThatNamesNothing) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series",
      {{"name", "bogus"}, {"inputs", json::array({"/imu/does_not_exist"})}, {"expression", "value * 2"}}, ctx);
  EXPECT_FALSE(r.ok) << r.content;
  EXPECT_TRUE(dp.last_inputs.empty()) << "nothing should have been installed";
}

// And an abbreviated input is expanded to the real path on the way through, so
// the host receives what it expects.
TEST(ToolRegistry, CreateDerivedSeriesExpandsAnAbbreviatedInput) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "doubled"}, {"inputs", json::array({"x"})}, {"expression", "value * 2"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  ASSERT_EQ(dp.last_inputs.size(), 1u);
  EXPECT_EQ(dp.last_inputs[0], "/imu/x");
}

// list_topics is re-sent on every later round-trip of a turn, so its payload
// has to stay bounded however large a limit the model asks for.
TEST(ToolRegistry, ListTopicsCapsAnAbsurdLimit) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  for (int i = 0; i < 600; ++i) {
    const std::string name = "/t" + std::to_string(i);
    store.addTopic(name);
    store.addField(name, "value", {0}, {1.0});
  }
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("list_topics", {{"limit", 100000}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["count"], 600);
  EXPECT_LE(j["shown"].get<int>(), 500);
  EXPECT_TRUE(j.contains("note")) << "a truncated listing must say so";
}

TEST(ToolRegistry, CreateDerivedSeriesFromExpression) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value * 2"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.validate_calls, 1);
  EXPECT_EQ(dp.create_calls, 1);
  EXPECT_EQ(dp.last_kind, "transform");
  EXPECT_EQ(dp.last_inputs, (std::vector<std::string>{"/imu/x"}));
  EXPECT_EQ(dp.last_outputs, (std::vector<std::string>{"x2"}));
  EXPECT_NE(dp.last_script.find("return (value * 2)"), std::string::npos);
}

TEST(ToolRegistry, CreateDerivedSeriesStatefulBody) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series",
      {{"name", "deriv"},
       {"inputs", json::array({"/imu/x"})},
       {"global", "local pt, pv"},
       {"body", "return value - (pv or value)"}},
      ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_NE(dp.last_script.find("local pt, pv"), std::string::npos);
  EXPECT_NE(dp.last_script.find("return value - (pv or value)"), std::string::npos);
}

TEST(ToolRegistry, CreateDerivedSeriesSurfacesValidateError) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.fail_validate = true;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "bad"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value +"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(dp.create_calls, 0);  // never installed on a compile failure
}

// The runtime cross-read: a single declared input, with the OTHER series read
// inside the body via the marker-rule vocabulary. The empty-join guard only
// inspects declared inputs, so this sidesteps it — and the real host then
// refuses the install, because `series(...)` does not exist in a transform's
// environment (verified in the application, 2026-08-14). The fake host mirrors
// that refusal; this test pins that the refusal reaches the model as a plain
// failure, so the benchmark measures the reaction the product would produce.
TEST(ToolRegistry, CreateDerivedSeriesRuntimeCrossReadIsRefusedLikeTheRealHost) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series",
      {{"name", "mix"},
       {"inputs", json::array({"/imu/x"})},
       {"body", "local other = series(\"/imu/y\"):atTime(time)\nreturn value + other"}},
      ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("series"), std::string::npos) << r.content;
  EXPECT_EQ(dp.liveCount(), 0) << "nothing may be left installed after the refusal";
}

TEST(ToolRegistry, CreateDerivedSeriesUnboundDpDegrades) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);  // no data-processors host
  auto r = reg.execute(
      "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value*2"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("data_processors"), std::string::npos);
}

TEST(ToolRegistry, CreateMarkersBuildsThresholdRule) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 2.5}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_kind, "markers");
  // Default style is "region": one shaded band per exceedance stretch.
  EXPECT_NE(dp.last_script.find("startMarker"), std::string::npos);
  EXPECT_NE(dp.last_script.find("closeMarker"), std::string::npos);
  EXPECT_NE(dp.last_script.find("> 2.5"), std::string::npos);

  // style="line" keeps the per-sample vertical markers.
  r = reg.execute(
      "create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 2.5}, {"style", "line"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_NE(dp.last_script.find("createVerticalMarker"), std::string::npos);
}

TEST(ToolRegistry, CreateMarkersRawRulePassesThroughVerbatim) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  const std::string rule = "local s = series(\"/imu/x\")\nstartMarker(0)\ncloseMarker(100)\n";
  auto r = reg.execute("create_markers", {{"inputs", json::array({"/imu/x"})}, {"rule", rule}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_kind, "markers");
  EXPECT_EQ(dp.last_script, rule);  // authored by the model, never rewritten
  ASSERT_EQ(dp.last_inputs.size(), 1u);
  EXPECT_EQ(dp.last_inputs[0], "/imu/x");
  // Default output marker topic = first input.
  ASSERT_EQ(dp.last_outputs.size(), 1u);
  EXPECT_EQ(dp.last_outputs[0], "/imu/x");
}

TEST(ToolRegistry, CreateMarkersRejectsMixedForms) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  // rule + template fields together is ambiguous -> clean failure, no create.
  auto r = reg.execute(
      "create_markers", {{"rule", "startMarker(0)"}, {"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 1.0}},
      ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(dp.create_calls, 0);
  // rule without inputs -> clean failure.
  r = reg.execute("create_markers", {{"rule", "startMarker(0)"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(dp.create_calls, 0);
}

TEST(ToolRegistry, RemoveMarkersRemovesOwnSet) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute("remove_markers", json::object(), ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_removed, "assistant_markers");
}

TEST(ToolRegistry, CreateMarkersDegradesOnHostRejection) {
  // Mirrors PJ4 main, where the host rejects kind="markers": the tool surfaces
  // the host error to the model rather than crashing.
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.fail_create = true;
  auto ctx = makeCtx(store, &dp);
  auto r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 1.0}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("rejected"), std::string::npos);
}

TEST(ToolRegistry, ReportStatus) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);
  auto r = reg.execute("report_status", json::object(), ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["topics"], 1);
  EXPECT_EQ(j["fields"], 2);
}

TEST(ToolRegistry, ReportStatusCountsObjects) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addTopic("/imu");
  host.addField("/imu", "x");
  host.addObjectTopic("/cloud", "kPointCloud", 5, 0, kSec);
  host.addObjectTopic(
      "/derived_cloud", "kPointCloud", 5, 0, kSec, R"({"builtin_object_type":"kPointCloud","pj_derived":"true"})");
  host.addObjectTopic("__markers__/imu/x", "kPlotMarkers", 1, 0, kSec);
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  auto r = reg.execute("report_status", json::object(), ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  EXPECT_EQ(j["object_topics"], 2) << "the marker topic must not be counted";
  EXPECT_EQ(j["derived_object_topics"], 1);
}

// --- what the model is told about what it just made ------------------------

namespace {
// A marker set as the host would publish it: `regions` shaded spans followed by
// `events` per-sample ticks.
PJ::sdk::PlotMarkers makeSet(std::size_t regions, std::size_t events) {
  PJ::sdk::PlotMarkers set;
  for (std::size_t i = 0; i < regions; ++i) {
    PJ::sdk::PlotMarker m;
    m.kind = PJ::sdk::MarkerKind::kRegion;
    m.t_start = static_cast<PJ::Timestamp>(i) * kSec;
    m.t_end = m.t_start + kSec / 2;
    set.markers.push_back(m);
  }
  for (std::size_t i = 0; i < events; ++i) {
    PJ::sdk::PlotMarker m;
    m.kind = PJ::sdk::MarkerKind::kEvent;
    m.t_start = static_cast<PJ::Timestamp>(i) * (kSec / 100);
    set.markers.push_back(m);
  }
  return set;
}
}  // namespace

// The whole point of the read-back: from the model's seat, twelve tidy regions
// and four thousand overlapping lines are the same "created" message. The
// breakdown by kind is what separates them.
TEST(ToolRegistry, CreateMarkersReportsWhatWasActuallyPublished) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  FakeObjectReadHost objects;
  objects.publish("/imu/x", makeSet(/*regions=*/3, /*events=*/0));
  ToolContext ctx = makeCtx(store, &dp, &objects);

  auto r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 2.5}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["markers_created"], 3);
  EXPECT_EQ(j["by_kind"]["regions"], 3);
  EXPECT_FALSE(j["by_kind"].contains("events"));
  // Three half-second regions cover 1.5 s; the envelope they sit in is 2.5 s.
  EXPECT_DOUBLE_EQ(j["covered_s"].get<double>(), 1.5);
  EXPECT_FALSE(j.contains("span_s"));
}

// The field is named `covered_s` and it has to earn the name: overlapping
// regions merge instead of double-counting, and the stretch BETWEEN regions is
// not covered. The envelope of this set is 11 s and the naive duration sum is
// 5 s; only the union, 4 s, is what a user would call "time covered". Its
// predecessor `span_s` reported the envelope, and models quoted it as coverage
// in every session that created markers.
TEST(ToolRegistry, CoveredTimeIsTheUnionOfRegionsNotTheEnvelope) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  FakeObjectReadHost objects;
  PJ::sdk::PlotMarkers set;
  for (const auto& [start_s, end_s] : {std::pair{0, 2}, {1, 3}, {10, 11}}) {
    PJ::sdk::PlotMarker m;
    m.kind = PJ::sdk::MarkerKind::kRegion;
    m.t_start = static_cast<PJ::Timestamp>(start_s) * kSec;
    m.t_end = static_cast<PJ::Timestamp>(end_s) * kSec;
    set.markers.push_back(m);
  }
  objects.publish("/imu/x", set);
  ToolContext ctx = makeCtx(store, &dp, &objects);

  auto r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 2.5}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_DOUBLE_EQ(j["covered_s"].get<double>(), 4.0);
}

TEST(ToolRegistry, CreateMarkersMakesAWallVisibleAsSuch) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  FakeObjectReadHost objects;
  objects.publish("/imu/x", makeSet(/*regions=*/0, /*events=*/4182));
  ToolContext ctx = makeCtx(store, &dp, &objects);

  auto r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 2.5}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["markers_created"], 4182);
  EXPECT_EQ(j["by_kind"]["events"], 4182);
  // Zero-duration marks cover nothing; an absent field beats a meaningless 0.
  EXPECT_FALSE(j.contains("covered_s"));
}

// entry_count() reports 1 for any set, because MarkerService pushes the whole
// blob as a single entry. If the production code ever regresses to trusting it,
// this is the test that catches it: the count would read 1 instead of 3.
TEST(ToolRegistry, MarkerCountComesFromThePayloadNotTheEntryCount) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  FakeObjectReadHost objects;
  objects.publish("/imu/x", makeSet(/*regions=*/3, /*events=*/0));
  ToolContext ctx = makeCtx(store, &dp, &objects);

  auto r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 2.5}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(json::parse(r.content)["markers_created"], 3) << "1 here means entry_count() was trusted";
}

// The read service is optional. Without it the answer simply carries no count —
// creating markers must not start failing on a host that omits it.
TEST(ToolRegistry, CreateMarkersSucceedsWithoutTheObjectReadService) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);  // no object host

  auto r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 2.5}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["created_markers_on"], "/imu/x");
  EXPECT_FALSE(j.contains("markers_created"));
}

// --- refusing to build an empty curve --------------------------------------

// Two inputs that exist, are numeric, and share not one timestamp: the host
// joins multi-input transforms on exact timestamp equality, so this would
// install a series with zero points and report success. Reproduced from the
// real shape of the problem — same rate, offset by half a sample — because a
// rate comparison would call these compatible and be wrong.
TEST(ToolRegistry, RefusesATransformWhoseInputsShareNoTimestamps) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/a");
  store.addField("/a", "v", {0, kSec, 2 * kSec}, {1.0, 2.0, 3.0});
  store.addTopic("/b");
  store.addField("/b", "v", {kSec / 2, 3 * kSec / 2, 5 * kSec / 2}, {1.0, 2.0, 3.0});
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute(
      "create_derived_series",
      {{"name", "mix"}, {"inputs", json::array({"/a/v", "/b/v"})}, {"expression", "value + v1"}}, ctx);

  EXPECT_FALSE(r.ok);
  EXPECT_EQ(dp.persistent_creates, 0) << "nothing may be installed";
  // The message has to carry both halves: why it refused, and what does work.
  EXPECT_NE(r.content.find("share no timestamps"), std::string::npos) << r.content;
  EXPECT_NE(r.content.find("read_series"), std::string::npos) << r.content;
}

// The other half of the contract, and the one that keeps this from becoming an
// over-restrictive guard: inputs on a common clock must still work.
TEST(ToolRegistry, AllowsATransformWhoseInputsShareATimeline) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/a");
  store.addField("/a", "v", {0, kSec, 2 * kSec}, {1.0, 2.0, 3.0});
  store.addTopic("/b");
  store.addField("/b", "v", {0, kSec, 2 * kSec}, {4.0, 5.0, 6.0});
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute(
      "create_derived_series",
      {{"name", "mix"}, {"inputs", json::array({"/a/v", "/b/v"})}, {"expression", "value + v1"}}, ctx);

  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.persistent_creates, 1);
  // Fully overlapping, so no partial-join warning is emitted.
  EXPECT_FALSE(json::parse(r.content).contains("joined_points"));
}

// A partial overlap is legal and gets built — but silently dropping most of the
// rows is exactly the kind of surprise the model cannot see, so it is named.
TEST(ToolRegistry, ReportsHowManyPointsSurviveAPartialJoin) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/a");
  store.addField("/a", "v", {0, kSec, 2 * kSec, 3 * kSec}, {1.0, 2.0, 3.0, 4.0});
  store.addTopic("/b");
  store.addField("/b", "v", {0, 5 * kSec, 6 * kSec, 7 * kSec}, {4.0, 5.0, 6.0, 7.0});
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute(
      "create_derived_series",
      {{"name", "mix"}, {"inputs", json::array({"/a/v", "/b/v"})}, {"expression", "value + v1"}}, ctx);

  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["points"], 1) << "only t=0 is common";
  EXPECT_EQ(j["shortest_input_points"], 4);
  // The result states the outcome and stops there. It used to append
  // "verify_with: read_series on ...", and models obliged — a full round trip
  // spent fetching a number this response already had. What to do about the
  // result is the model's call; this tool's job is to report it.
  EXPECT_FALSE(j.contains("verify_with")) << "a tool result must not prescribe the next call";
}

// Single-input transforms have nothing to join, so the guard must not touch
// them — this is the overwhelmingly common case.
TEST(ToolRegistry, SingleInputTransformIsNeverForecast) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute(
      "create_derived_series", {{"name", "doubled"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value * 2"}},
      ctx);

  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.persistent_creates, 1);
  const json j = json::parse(r.content);
  EXPECT_FALSE(j.contains("shortest_input_points")) << "nothing joined, so nothing was lost to a join";
  // Length still gets reported, because it is the fact the model would otherwise
  // spend a round trip to fetch. With one input there is no join to shorten it,
  // so the output is exactly as long as the input.
  EXPECT_EQ(j["points"], 5) << "as long as its only input, which populate() gives 5 samples";
  EXPECT_FALSE(j.contains("verify_with")) << "a tool result must not prescribe the next call";
}

// --- evaluate(): run Luau, get numbers back, leave nothing behind ----------

TEST(ToolRegistry, EvaluateHappyPathCreatesReadsAndRemoves) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  // Bridges the ephemeral create's resolved output back into `store`, so the
  // read_series-shaped read that follows create() has something to read.
  dp.ephemeral_series_store = &store;
  dp.ephemeral_series_ts = {0, kSec, 2 * kSec, 3 * kSec, 4 * kSec};
  dp.ephemeral_series_vals = {0.0, 2.0, 4.0, 6.0, 8.0};
  ToolContext ctx = makeCtx(store, &dp);
  int notify_calls = 0;
  ctx.notify_data_changed = [&]() { ++notify_calls; };

  auto r =
      reg.execute("evaluate", {{"inputs", json::array({"/imu/x"})}, {"expression", "value * 2"}, {"buckets", 3}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.create_calls, 1);
  EXPECT_NE(dp.last_flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL, 0u);
  EXPECT_EQ(dp.persistent_creates, 0) << "an ephemeral node never joins the live set";
  EXPECT_EQ(dp.last_removed, dp.last_id) << "removed by the same id it was created under";
  EXPECT_EQ(notify_calls, 0) << "evaluate must never notify_data_changed -- nothing changed that the user can see";
  const json j = json::parse(r.content);
  EXPECT_TRUE(j.contains("stats"));
  EXPECT_TRUE(j.contains("buckets"));
}

// evaluate's stats go through the same statsWithDisplayStart path as
// read_series, so the whole-series flat-run facts must ride along there too.
TEST(ToolRegistry, EvaluateStatsCarryFlatSpanFacts) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.ephemeral_series_store = &store;
  dp.ephemeral_series_ts = {0, kSec, 2 * kSec, 3 * kSec, 4 * kSec};
  dp.ephemeral_series_vals = {9.0, 9.0, 9.0, 9.0, 9.0};  // never changes
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute("evaluate", {{"inputs", json::array({"/imu/x"})}, {"expression", "value * 0 + 9"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  ASSERT_TRUE(j.contains("stats"));
  EXPECT_TRUE(j["stats"]["constant"].get<bool>());
  ASSERT_TRUE(j["stats"].contains("constant_note")) << r.content;
  const std::string note = j["stats"]["constant_note"].get<std::string>();
  EXPECT_NE(note.find("entire recording"), std::string::npos) << note;
  EXPECT_NE(note.find("not something that changed"), std::string::npos) << note;
}

TEST(ToolRegistry, EvaluateSurfacesValidateErrorWithoutCreating) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.fail_validate = true;
  ToolContext ctx = makeCtx(store, &dp);
  auto r = reg.execute("evaluate", {{"inputs", json::array({"/imu/x"})}, {"expression", "value +"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(dp.create_calls, 0);
}

// No bridge to a series for the resolved output: create succeeds, the read
// that follows it does not. The ephemeral node must still come out.
TEST(ToolRegistry, EvaluateRemovesTheEphemeralNodeEvenWhenTheReadFails) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);
  auto r = reg.execute("evaluate", {{"inputs", json::array({"/imu/x"})}, {"expression", "value * 2"}}, ctx);
  EXPECT_FALSE(r.ok) << r.content;
  EXPECT_EQ(dp.create_calls, 1);
  EXPECT_EQ(dp.last_removed, dp.last_id) << "the node must be removed even when the read after it fails";
}

TEST(ToolRegistry, EvaluateCallsUseDistinctIds) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);
  auto first = reg.execute("evaluate", {{"inputs", json::array({"/imu/x"})}, {"expression", "value * 2"}}, ctx);
  const std::string first_id = dp.last_id;
  auto second = reg.execute("evaluate", {{"inputs", json::array({"/imu/x"})}, {"expression", "value * 3"}}, ctx);
  const std::string second_id = dp.last_id;
  (void)first;
  (void)second;
  EXPECT_FALSE(first_id.empty());
  EXPECT_NE(first_id, second_id);
}

// evaluate's scalar path (no object input, no at_s/window) must be byte-for-
// byte what shipped before objects existed: the object-path routing check
// added in block 3.2 must fall through cleanly, on a host that has no v2
// catalog at all (PJ::testing::ToolboxTestStore, used by every scalar test
// above) as much as on one that does.
TEST(ToolRegistry, ScalarEvaluateIsUntouched) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.ephemeral_series_store = &store;
  dp.ephemeral_series_ts = {0, kSec, 2 * kSec};
  dp.ephemeral_series_vals = {1.0, 2.0, 3.0};
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute("evaluate", {{"inputs", json::array({"/imu/x"})}, {"expression", "value + 1"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.create_calls, 1) << "the scalar ephemeral-transform path, unchanged";
  EXPECT_EQ(dp.create_v2_calls, 0);
  EXPECT_EQ(dp.submit_calls, 0) << "the object/on-demand surface must never be touched here";
  const json j = json::parse(r.content);
  EXPECT_TRUE(j.contains("stats"));
  EXPECT_TRUE(j.contains("evaluated"));
}

// --- evaluate()'s object path: at_s/window over object topics --------------

TEST(ToolRegistry, EvaluateAtCreatesEphemeralSubmitsPollsAndReleases) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
  RecordingDpHost dp;
  FakePlaybackHost pb;
  pb.display_offset_ns = 1000;  // display(raw) = (raw - offset) * 1e-9
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = pb.view();

  auto r = reg.execute(
      "evaluate",
      {{"inputs", json::array({"/cloud"})},
       {"at_s", 2.0},
       {"body", "return { cropped = inputs[\"/cloud\"], count = 1 }"},
       {"outputs", json::array({"cropped:kPointCloud", "count:number"})}},
      ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.submit_calls, 1);
  EXPECT_EQ(dp.create_calls, 0) << "the object path never installs anything, ephemeral or otherwise";
  EXPECT_NE(dp.last_flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL, 0u);
  EXPECT_EQ(dp.last_kind, "on_demand");
  EXPECT_NE(dp.last_time_flags & PJ_DATA_PROCESSOR_TIME_FLAG_INSTANT, 0u);
  // to_raw(2.0 s) = 2.0 * 1e9 + display_offset_ns
  EXPECT_EQ(dp.last_instant_ns, 2'000'001'000);
  EXPECT_EQ(dp.poll_calls, 1) << "the phase-0 host completes inline; one poll suffices";
  ASSERT_EQ(dp.released_handles.size(), 1u);
  const json j = json::parse(r.content);
  ASSERT_TRUE(j.contains("bundles"));
  EXPECT_EQ(j["bundles"].size(), 1u);
}

TEST(ToolRegistry, EvaluateWindowPassesBudgetAndReturnsCoverage) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
  RecordingDpHost dp;
  FakePlaybackHost pb;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = pb.view();

  auto r = reg.execute(
      "evaluate",
      {{"inputs", json::array({"/cloud"})},
       {"window", {{"start_s", 1.0}, {"end_s", 3.0}}},
       {"budget_ms", 2000},
       {"budget_evaluations", 10},
       {"body", "return { cropped = inputs[\"/cloud\"], count = 1 }"},
       {"outputs", json::array({"cropped:kPointCloud", "count:number"})}},
      ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_NE(dp.last_time_flags & PJ_DATA_PROCESSOR_TIME_FLAG_WINDOW, 0u);
  EXPECT_EQ(dp.last_window_start_ns, 1'000'000'000);
  EXPECT_EQ(dp.last_window_end_ns, 3'000'000'000);
  EXPECT_EQ(dp.last_budget_max_millis, 2000u);
  EXPECT_EQ(dp.last_budget_max_evaluations, 10u);
  const json j = json::parse(r.content);
  ASSERT_TRUE(j.contains("coverage")) << "coverage rides along verbatim";
  EXPECT_EQ(j["coverage"]["candidates"], 1);
}

// The plugin does not copy the host's budget ceiling: it asks for what the call says (positive, with
// defaults) and the host clamps.
TEST(ToolRegistry, EvaluateRequestsTheBudgetUncappedAndPositive) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
  RecordingDpHost dp;
  FakePlaybackHost pb;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = pb.view();

  const auto call = [&](json budget) {
    json args = {
        {"inputs", json::array({"/cloud"})},
        {"at_s", 1.0},
        {"body", "return { count = 1 }"},
        {"outputs", json::array({"count:number"})}};
    args.update(budget);
    return reg.execute("evaluate", args, ctx);
  };
  ASSERT_TRUE(call({{"budget_ms", 60000}, {"budget_evaluations", 5000}}).ok);
  EXPECT_EQ(dp.last_budget_max_millis, 60000u);
  EXPECT_EQ(dp.last_budget_max_evaluations, 5000u);
  ASSERT_TRUE(call({{"budget_ms", 0}, {"budget_evaluations", -3}}).ok);
  EXPECT_EQ(dp.last_budget_max_millis, 1u);
  EXPECT_EQ(dp.last_budget_max_evaluations, 1u);
  ASSERT_TRUE(call(json::object()).ok);
  EXPECT_EQ(dp.last_budget_max_millis, 1000u);
  EXPECT_EQ(dp.last_budget_max_evaluations, 50u);
}

TEST(ToolRegistry, EvaluateWithObjectInputRequiresTypedOutputs) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
  RecordingDpHost dp;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();

  auto r = reg.execute("evaluate", {{"inputs", json::array({"/cloud"})}, {"body", "return { count = 1 }"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("outputs"), std::string::npos) << r.content;
  EXPECT_EQ(dp.submit_calls, 0);
}

TEST(ToolRegistry, EvaluateWaitsThroughPendingPolls) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
  RecordingDpHost dp;
  dp.pending_polls = 2;
  FakePlaybackHost pb;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = pb.view();

  auto r = reg.execute(
      "evaluate",
      {{"inputs", json::array({"/cloud"})},
       {"at_s", 1.0},
       {"body", "return { cropped = inputs[\"/cloud\"], count = 1 }"},
       {"outputs", json::array({"cropped:kPointCloud", "count:number"})}},
      ctx);
  ASSERT_TRUE(r.resume);
  EXPECT_EQ(dp.poll_calls, 1);
  EXPECT_TRUE(dp.released_handles.empty());
  r = r.resume(ctx);
  ASSERT_TRUE(r.resume);
  r = r.resume(ctx);
  EXPECT_FALSE(r.resume);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.poll_calls, 3) << "2 PENDING answers, then COMPLETED";
  EXPECT_EQ(dp.released_handles.size(), 1u);
}

// A host built before pj.data_processors.v1 gained submit_evaluation: the
// v1 slots (including validate_data_processor_script) still work, so the
// script validates cleanly, and only submitEvaluation itself reports the
// gap -- a clean "not supported" the model can relay, never a crash.
TEST(ToolRegistry, EvaluateOnOldHostReportsNotSupported) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
  RecordingDpHost dp;
  dp.supports_v2 = false;
  FakePlaybackHost pb;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = pb.view();

  auto r = reg.execute(
      "evaluate",
      {{"inputs", json::array({"/cloud"})},
       {"at_s", 1.0},
       {"body", "return { cropped = inputs[\"/cloud\"], count = 1 }"},
       {"outputs", json::array({"cropped:kPointCloud", "count:number"})}},
      ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("submit_evaluation"), std::string::npos) << r.content;
  EXPECT_EQ(dp.validate_calls, 1) << "the v1 slot still works; only submit_evaluation is missing";
}

TEST(ToolRegistry, EvaluateTruncatesLongReports) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
  RecordingDpHost dp;
  json bundles = json::array();
  for (int i = 0; i < 60; ++i) {
    bundles.push_back(
        {{"requested_ns", static_cast<std::int64_t>(i) * kSec},
         {"stamp_ns", static_cast<std::int64_t>(i) * kSec},
         {"inputs", json::array()},
         {"outputs", json::object()}});
  }
  const json report = {{"coverage", {{"candidates", 60}, {"evaluated", 60}, {"complete", true}}}, {"bundles", bundles}};
  dp.canned_report_json = report.dump();
  FakePlaybackHost pb;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = pb.view();

  auto r = reg.execute(
      "evaluate",
      {{"inputs", json::array({"/cloud"})},
       {"window", {{"start_s", 0.0}, {"end_s", 60.0}}},
       {"body", "return { cropped = inputs[\"/cloud\"], count = 1 }"},
       {"outputs", json::array({"cropped:kPointCloud", "count:number"})}},
      ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  ASSERT_TRUE(j.contains("bundles"));
  EXPECT_EQ(j["bundles"].size(), 50u);
  ASSERT_TRUE(j.contains("truncated"));
  EXPECT_TRUE(j["truncated"].get<bool>());
  // The LAST 50 of 60 survive, so the first one left is index 10.
  ASSERT_TRUE(j["bundles"].front().contains("requested_s"));
  EXPECT_DOUBLE_EQ(j["bundles"].front()["requested_s"].get<double>(), 10.0);
}

// --- create_derived_object ---------------------------------------------------

#ifdef PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT
TEST(ToolRegistry, CreateDerivedObjectPinsAFinding) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
  RecordingDpHost dp;
  dp.config_history_exempt = true;
  dp.pending_polls = 2;
  FakePlaybackHost pb;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = pb.view();
  int notify_calls = 0;
  ctx.notify_data_changed = [&]() { ++notify_calls; };

  auto r = reg.execute(
      "create_derived_object",
      {{"name", "cropped_cloud"},
       {"inputs", json::array({"/cloud"})},
       {"outputs", json::array({"cropped:kPointCloud", "count:number"})},
       {"body", "return { cropped = inputs[\"/cloud\"], count = 1 }"},
       {"pin_at_s", 2.5},
       {"label", "cropped at 2.5s"}},
      ctx);
  ASSERT_TRUE(r.resume);
  EXPECT_EQ(dp.create_v2_calls, 1);
  r = r.resume(ctx);
  ASSERT_TRUE(r.resume);
  r = r.resume(ctx);
  EXPECT_FALSE(r.resume);
  EXPECT_EQ(dp.released_handles.size(), 1u);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.create_v2_calls, 1);
  EXPECT_EQ(dp.persistent_creates, 1) << "a pin is HISTORY_EXEMPT, not EPHEMERAL -- it is kept";
  EXPECT_EQ(dp.last_kind, "on_demand");
  EXPECT_NE(dp.last_create_v2_flags & PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT, 0u);
  EXPECT_NE(dp.last_create_v2_time_flags & PJ_DATA_PROCESSOR_TIME_FLAG_INSTANT, 0u);
  EXPECT_EQ(dp.last_create_v2_instant_ns, 2'500'000'000);
  EXPECT_EQ(notify_calls, 1);
  EXPECT_EQ(dp.submit_calls, 1) << "evaluated once after creating, so the model sees a finding";
  const json j = json::parse(r.content);
  EXPECT_EQ(j["created"], "cropped_cloud");
  ASSERT_TRUE(j.contains("bundle"));
  EXPECT_FALSE(j.contains("undo_protection"));
  EXPECT_EQ(j["pinned_at_s"], 2.5);
  EXPECT_FALSE(j.contains("live"));
}
#endif

TEST(ToolRegistry, CreateDerivedObjectDisclosesMissingUndoProtection) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
  RecordingDpHost dp;  // config_history_exempt left unset: the recipe omits the key
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();

  auto r = reg.execute(
      "create_derived_object",
      {{"name", "cropped_cloud"},
       {"inputs", json::array({"/cloud"})},
       {"outputs", json::array({"cropped:kPointCloud", "count:number"})},
       {"body", "return { cropped = inputs[\"/cloud\"], count = 1 }"}},
      ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["undo_protection"], "unavailable: an undo can remove this");
  EXPECT_EQ(j["live"], true) << "no pin_at_s: the object recomputes as the playhead moves";
  EXPECT_FALSE(j.contains("pinned_at_s"));
}

// The contract for evaluating an INSTALLED node: id/kind/flags/time only. The host refuses a language,
// params, inputs, outputs or dataset on it, because the stored recipe is authoritative.
namespace {
json createObjectArgs() {
  return {
      {"name", "lidar_crop10"},
      {"inputs", json::array({"/cloud"})},
      {"outputs", json::array({"cropped:kPointCloud", "count:number"})},
      {"body", "return { cropped = inputs[\"/cloud\"], count = 1 }"},
      {"params", {{"radius", 10}}}};
}
}  // namespace

TEST(ToolRegistry, CreateDerivedObjectEvaluatesTheInstalledNodeById) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
  RecordingDpHost dp;
  FakePlaybackHost pb;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = pb.view();

  auto r = reg.execute("create_derived_object", createObjectArgs(), ctx);
  ASSERT_TRUE(r.ok) << r.content;
  ASSERT_EQ(dp.submits.size(), 1u);
  const auto& submit = dp.submits.front();
  EXPECT_EQ(submit.language, "");
  EXPECT_EQ(submit.script, "");
  EXPECT_EQ(submit.input_count, 0u);
  EXPECT_EQ(submit.output_count, 0u);
  EXPECT_EQ(submit.params_json, "");
  EXPECT_EQ(submit.dataset_id, 0u);
  EXPECT_EQ(dp.last_id, "lidar_crop10");
}

TEST(ToolRegistry, AFailedFirstEvaluationRemovesTheNodeAndSaysWhy) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
  RecordingDpHost dp;
  dp.terminal_state = PJ_EVALUATION_STATE_FAILED;
  dp.canned_report_json = R"({"coverage":{"complete":false,"error":"evaluation failed"},"bundles":[]})";
  dp.canned_config_json = R"({"kind":"on_demand","state":"error","detail":"bad argument 'center': expected array"})";
  FakePlaybackHost pb;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = pb.view();
  int notify_calls = 0;
  ctx.notify_data_changed = [&]() { ++notify_calls; };

  auto r = reg.execute("create_derived_object", createObjectArgs(), ctx);
  EXPECT_FALSE(r.ok) << r.content;
  EXPECT_NE(r.content.find("bad argument 'center': expected array"), std::string::npos) << r.content;
  EXPECT_NE(r.content.find("Nothing was kept"), std::string::npos) << r.content;
  EXPECT_EQ(dp.last_removed, "lidar_crop10");
  EXPECT_EQ(dp.liveCount(), 0);
  EXPECT_EQ(notify_calls, 2) << "once for the create, once for the removal";
}

TEST(ToolRegistry, AFailedFirstEvaluationOfASoundNodeKeepsItAndReportsBundleUnavailable) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
  RecordingDpHost dp;
  dp.terminal_state = PJ_EVALUATION_STATE_FAILED;
  dp.canned_report_json = R"({"coverage":{"complete":false,"error":"missing input at t=0"},"bundles":[]})";
  dp.canned_config_json = R"({"kind":"on_demand","state":"ready"})";
  FakePlaybackHost pb;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = pb.view();

  auto r = reg.execute("create_derived_object", createObjectArgs(), ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["bundle_unavailable"], "missing input at t=0");
  EXPECT_EQ(j["created"], "lidar_crop10");
  EXPECT_TRUE(dp.last_removed.empty());
  EXPECT_EQ(dp.liveCount(), 1);
}

// --- PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT: set when the SDK has it, with a
// fallback for a host that rejects the unknown bit -----------------------

#ifdef PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT

TEST(ToolRegistry, CreatesCarryTheHistoryExemptFlagWhenTheSdkExposesIt) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  {
    RecordingDpHost dp;
    dp.config_history_exempt = true;
    ToolContext ctx = makeCtx(store, &dp);
    auto r = reg.execute(
        "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value*2"}}, ctx);
    ASSERT_TRUE(r.ok) << r.content;
    EXPECT_NE(dp.last_flags & PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT, 0u);
    EXPECT_EQ(dp.config_calls, 1);
    EXPECT_EQ(dp.last_config_id, "x2");
    EXPECT_FALSE(json::parse(r.content).contains("undo_protection"));
  }
  {
    RecordingDpHost dp;
    dp.config_history_exempt = true;
    ToolContext ctx = makeCtx(store, &dp);
    auto r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 1.0}}, ctx);
    ASSERT_TRUE(r.ok) << r.content;
    EXPECT_NE(dp.last_flags & PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT, 0u);
    EXPECT_EQ(dp.config_calls, 1);
    EXPECT_EQ(dp.last_config_id, "assistant_markers");
    EXPECT_FALSE(json::parse(r.content).contains("undo_protection"));
  }
  {
    RecordingDpHost dp;
    dp.config_history_exempt = true;
    ToolContext ctx = makeCtx(store, &dp);
    const std::string rule = "local s = series(\"/imu/x\")\nstartMarker(0)\ncloseMarker(100)\n";
    auto r = reg.execute("create_markers", {{"inputs", json::array({"/imu/x"})}, {"rule", rule}}, ctx);
    ASSERT_TRUE(r.ok) << r.content;
    EXPECT_NE(dp.last_flags & PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT, 0u);
    EXPECT_EQ(dp.config_calls, 1);
    EXPECT_EQ(dp.last_config_id, "assistant_markers");
    EXPECT_FALSE(json::parse(r.content).contains("undo_protection"));
  }
}

TEST(ToolRegistry, DisclosesWhenHistoryExemptIsFalseInTheCreatedRecipe) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.config_history_exempt = false;
  ToolContext ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value*2"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(json::parse(r.content)["undo_protection"], "unavailable: an undo can remove this");

  r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 1.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(json::parse(r.content)["undo_protection"], "unavailable: an undo can remove this");

  const std::string rule = "local s = series(\"/imu/x\")\nstartMarker(0)\ncloseMarker(100)\n";
  r = reg.execute("create_markers", {{"inputs", json::array({"/imu/x"})}, {"rule", rule}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(json::parse(r.content)["undo_protection"], "unavailable: an undo can remove this");
  EXPECT_EQ(dp.config_calls, 3) << "each persistent create site must verify the stored flag";
}

TEST(ToolRegistry, DisclosesWhenHistoryExemptIsMissingFromTheCreatedRecipe) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value*2"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.config_calls, 1);
  EXPECT_EQ(json::parse(r.content)["undo_protection"], "unavailable: an undo can remove this");
}

TEST(ToolRegistry, DisclosesWhenTheCreatedRecipeCannotBeRead) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.fail_config = true;
  ToolContext ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value*2"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.config_calls, 1);
  EXPECT_EQ(json::parse(r.content)["undo_protection"], "unavailable: an undo can remove this");
}

TEST(ToolRegistry, DegradesOnceWhenAnOlderHostRejectsTheReservedBit) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.reject_unknown_flags = true;  // simulates a host that only knows EPHEMERAL
  ToolContext ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value*2"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.create_calls, 2) << "first call refused, retried once with flags cleared";
  EXPECT_EQ(dp.last_flags, 0u) << "the call that finally succeeded carried no flags";
  EXPECT_EQ(dp.config_calls, 0) << "the reserved-bit retry already established that protection is unavailable";
  const json j = json::parse(r.content);
  EXPECT_EQ(j["undo_protection"], "unavailable: an undo can remove this");
}

#else

TEST(ToolRegistry, CreatesCarryNoHistoryExemptFlagWithoutSdkSupport) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value*2"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_flags, 0u) << "no HISTORY_EXEMPT bit exists in this SDK build to set";
  EXPECT_EQ(dp.config_calls, 0) << "an SDK without the flag must not probe the recipe";

  EXPECT_EQ(json::parse(r.content)["undo_protection"], "unavailable: an undo can remove this")
      << "this build cannot request the exemption, so the model must be told the node is in undo's "
         "reach -- this is the configuration that ships until the SDK pin reaches the flag";

  r = reg.execute("create_markers", {{"series", "/imu/x"}, {"comparison", ">"}, {"threshold", 1.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_flags, 0u);
  EXPECT_EQ(dp.config_calls, 0);
  EXPECT_EQ(json::parse(r.content)["undo_protection"], "unavailable: an undo can remove this");

  const std::string rule = "local s = series(\"/imu/x\")\nstartMarker(0)\ncloseMarker(100)\n";
  r = reg.execute("create_markers", {{"inputs", json::array({"/imu/x"})}, {"rule", rule}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_flags, 0u);
  EXPECT_EQ(dp.config_calls, 0);
  EXPECT_EQ(json::parse(r.content)["undo_protection"], "unavailable: an undo can remove this");
}

#endif

// A rejection that is not the reserved-bit wording must not be retried --
// only a specific, matched refusal is worth a second attempt.
TEST(ToolRegistry, ANonReservedBitFailureIsNotRetried) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.fail_create = true;
  ToolContext ctx = makeCtx(store, &dp);
  auto r = reg.execute(
      "create_derived_series", {{"name", "x2"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value*2"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(dp.create_calls, 1) << "a generic rejection must not trigger the flags=0 retry";
}

// --- addressing a series when several datasets are loaded -------------------

// The host's own display convention, "dataset:topic/field", addresses one
// run's series when both runs share every topic name.
TEST(ResolveSeriesPath, QualifiedPathPicksItsDataset) {
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu", "/speed"}).addDataset("run_friday.mcap", {"/imu", "/speed"});
  auto view = PJ::sdk::ToolboxHostView(host.makeHost());
  auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto monday = PJ::agent_tools::resolveSeriesPath(*catalog, "run_monday.mcap:/speed/value");
  const auto friday = PJ::agent_tools::resolveSeriesPath(*catalog, "run_friday.mcap:/speed/value");
  ASSERT_TRUE(monday.resolved.has_value());
  ASSERT_TRUE(friday.resolved.has_value());
  EXPECT_NE(monday.resolved->handle.topic.id, friday.resolved->handle.topic.id)
      << "both qualifiers resolved to the same underlying series";
  EXPECT_EQ(monday.resolved->path, "run_monday.mcap:/speed/value");
}

// The failure the correctness sweep caught on screen: the bare path used to
// resolve silently to whichever file loaded first, for reads AND installs.
TEST(ResolveSeriesPath, RefusesABarePathThatExistsInSeveralDatasets) {
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu", "/speed"}).addDataset("run_friday.mcap", {"/imu", "/speed"});
  auto view = PJ::sdk::ToolboxHostView(host.makeHost());
  auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto lookup = PJ::agent_tools::resolveSeriesPath(*catalog, "/speed/value");
  EXPECT_FALSE(lookup.resolved.has_value()) << "resolved to '" << lookup.resolved->path << "' instead of refusing";
  EXPECT_TRUE(lookup.ambiguous);
  ASSERT_EQ(lookup.candidates.size(), 2u);
  // Candidates arrive in qualified form, copy-pasteable back as-is — this is
  // what turns the refusal into a one-round-trip correction.
  EXPECT_EQ(lookup.candidates[0], "run_monday.mcap:/speed/value");
  EXPECT_EQ(lookup.candidates[1], "run_friday.mcap:/speed/value");
}

// A topic that lives in only one of the datasets keeps resolving bare, and the
// resolved path discloses which dataset it came from.
TEST(ResolveSeriesPath, BarePathStillResolvesWhenUniqueAcrossDatasets) {
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu"}).addDataset("run_friday.mcap", {"/speed"});
  auto view = PJ::sdk::ToolboxHostView(host.makeHost());
  auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto lookup = PJ::agent_tools::resolveSeriesPath(*catalog, "/speed/value");
  ASSERT_TRUE(lookup.resolved.has_value());
  EXPECT_EQ(lookup.resolved->path, "run_friday.mcap:/speed/value");
}

// Stream datasets carry names like "[stream] UDP Server" — spaces, brackets.
// Matching the qualifier against the known names (instead of parsing at ':')
// is what makes those work with no escaping rules.
TEST(ResolveSeriesPath, StreamStyleDatasetNamesNeedNoEscaping) {
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/udp/data"}).addDataset("[stream] UDP Server", {"/udp/data"});
  auto view = PJ::sdk::ToolboxHostView(host.makeHost());
  auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto lookup = PJ::agent_tools::resolveSeriesPath(*catalog, "[stream] UDP Server:/udp/data/value");
  ASSERT_TRUE(lookup.resolved.has_value());
  EXPECT_EQ(lookup.resolved->path, "[stream] UDP Server:/udp/data/value");
}

// The host's create interface addresses inputs by BARE name, so a qualified
// create on a path that several datasets share cannot be delivered — the
// honest outcome is a loud refusal here, not an artifact on whichever dataset
// the host resolves first. (Reads are unaffected: they go by handle.)
TEST(ToolRegistry, CreateOnADuplicatedPathIsRefusedInsteadOfMistargeted) {
  ToolRegistry reg;
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu", "/speed"}).addDataset("run_friday.mcap", {"/imu", "/speed"});
  RecordingDpHost dp;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();

  auto r = reg.execute(
      "create_markers", {{"series", "run_friday.mcap:/speed/value"}, {"comparison", ">"}, {"threshold", 1.0}}, ctx);
  ASSERT_FALSE(r.ok) << r.content;
  EXPECT_NE(r.content.find("cannot target that specific dataset"), std::string::npos) << r.content;
  EXPECT_EQ(dp.create_calls, 0) << "nothing may reach the host after the refusal";
}

// When the bare name IS unique across datasets, a qualified create goes
// through — and what reaches the host is the bare form it understands, while
// the result reports the qualified form the model used.
TEST(ToolRegistry, QualifiedCreateOnAUniqueNameHandsTheHostItsBareForm) {
  ToolRegistry reg;
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu"}).addDataset("run_friday.mcap", {"/speed"});
  RecordingDpHost dp;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();

  auto r = reg.execute(
      "create_markers", {{"series", "run_friday.mcap:/speed/value"}, {"comparison", ">"}, {"threshold", 1.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_NE(dp.last_script.find("series(\"/speed/value\")"), std::string::npos) << dp.last_script;
  EXPECT_EQ(json::parse(r.content)["created_markers_on"], "run_friday.mcap:/speed/value");
}

// A transform input the host takes dataset-qualified ("dataset_source:topic/field", which picks that dataset), so a
// qualified path whose bare form several datasets share reaches the host qualified instead of being refused.
TEST(ToolRegistry, TransformOnADuplicatedPathHandsTheHostTheQualifiedPath) {
  ToolRegistry reg;
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu", "/speed"}).addDataset("run_friday.mcap", {"/imu", "/speed"});
  RecordingDpHost dp;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();

  const auto created = reg.execute(
      "create_derived_series",
      {{"name", "doubled"}, {"inputs", {"run_friday.mcap:/speed/value"}}, {"expression", "value * 2"}}, ctx);
  ASSERT_TRUE(created.ok) << created.content;
  EXPECT_EQ(dp.last_inputs, (std::vector<std::string>{"run_friday.mcap:/speed/value"}));

  dp.last_inputs.clear();
  const auto evaluated =
      reg.execute("evaluate", {{"inputs", {"run_friday.mcap:/speed/value"}}, {"expression", "value"}}, ctx);
  EXPECT_EQ(evaluated.content.find("cannot target"), std::string::npos) << evaluated.content;
  EXPECT_EQ(dp.last_inputs, (std::vector<std::string>{"run_friday.mcap:/speed/value"}));
}

// One dataset loaded: paths stay bare in both directions — no qualifier to
// learn, no extra characters in any result. The overwhelmingly common case
// pays nothing.
TEST(ResolveSeriesPath, SingleDatasetKeepsBarePaths) {
  FakeMultiDatasetHost host;
  host.addDataset("only.mcap", {"/imu", "/speed"});
  auto view = PJ::sdk::ToolboxHostView(host.makeHost());
  auto catalog = view.catalogSnapshot();
  ASSERT_TRUE(catalog);

  const auto lookup = PJ::agent_tools::resolveSeriesPath(*catalog, "/speed/value");
  ASSERT_TRUE(lookup.resolved.has_value());
  EXPECT_EQ(lookup.resolved->path, "/speed/value");
}

TEST(ToolRegistry, DescribeTopicNamesItsDataset) {
  ToolRegistry reg;
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu"}).addDataset("run_friday.mcap", {"/speed"});
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());

  auto r = reg.execute("describe_topic", {{"topic", "/speed"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(json::parse(r.content)["dataset"], "run_friday.mcap");
}

TEST(ToolRegistry, ReportStatusNamesTheDatasets) {
  ToolRegistry reg;
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu"}).addDataset("run_friday.mcap", {"/speed"});
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());

  auto r = reg.execute("report_status", json::object(), ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["data_sources"], 2);
  EXPECT_EQ(j["dataset_names"][0], "run_monday.mcap");
  EXPECT_EQ(j["dataset_names"][1], "run_friday.mcap");
}

// --- one node, several outputs ---------------------------------------------

TEST(ToolRegistry, CreatesSeveralOutputsFromOneTransform) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute(
      "create_derived_series",
      {{"name", "split"},
       {"inputs", json::array({"/imu/x"})},
       {"outputs", json::array({"roll", "pitch", "yaw"})},
       {"body", "    return value, value * 2, value * 3"}},
      ctx);

  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_outputs, (std::vector<std::string>{"roll", "pitch", "yaw"}));
  const json j = json::parse(r.content);
  EXPECT_EQ(j["series"].size(), 3u);
  EXPECT_EQ(j["series"][0], "roll/value");
}

// Omitting `outputs` must behave exactly as before — this is the path every
// existing conversation takes.
TEST(ToolRegistry, DefaultsToASingleOutputNamedAfterTheSeries) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute(
      "create_derived_series", {{"name", "doubled"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value * 2"}},
      ctx);

  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_outputs, (std::vector<std::string>{"doubled"}));
  EXPECT_EQ(json::parse(r.content)["series"], "doubled/value");
}

// --- what the tool surface costs, on every call ----------------------------

// The whole schema is re-sent on every API call, and one turn with tool use is
// several calls — so a description is not paid once per message, it is paid per
// round trip. This is a budget, not a style rule.
//
// The ceiling moves only against a measurement: what a description costs per
// round trip (fluent prose tokenizes far denser than chars/4 suggests; that rule
// of thumb overestimates this surface by about 50%) against what it saves (fewer
// round trips, a shape the model gets right the first time). Prose that cannot
// point at a saving is what the ceiling exists to stop; when it binds, cut prose
// rather than drop a capability.
//
// Each tool also pays a fixed JSON envelope before a word of prose, so related
// verbs share one tool with an `action` argument (plot_tab's zoom, scene_view's
// attach/detach/focus); a new tool earns its place only with a request shape no
// existing tool's arguments fit (evaluate, create_derived_object, scene_view's
// separate ownership scope).
TEST(ToolRegistry, ToolSchemaStaysWithinItsBudget) {
  ToolRegistry reg;
  const std::size_t chars = reg.toMcpToolsList().dump().size();
  std::cerr << "tool schema: " << chars << " chars across " << reg.tools().size() << " tools\n";
  for (const auto& t : reg.tools()) {
    std::cerr << "  " << t.name << ": " << t.description.size() << "\n";
  }
  EXPECT_LT(chars, 14200u) << "the tool surface outgrew its budget — trim descriptions before adding capability";
}

// --- playback / viewport tools ----------------------------------------------

TEST(ToolRegistry, PlaybackToolsEchoFullState) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  for (const char* action : {"play", "pause", "state"}) {
    auto r = reg.execute("playback", {{"action", action}}, ctx);
    ASSERT_TRUE(r.ok) << action << ": " << r.content;
    auto j = json::parse(r.content);
    EXPECT_EQ(j["playing"], false) << action;
    EXPECT_DOUBLE_EQ(j["current_time_s"].get<double>(), 3.0) << action;
    EXPECT_DOUBLE_EQ(j["range"]["min_s"].get<double>(), 0.0) << action;
    EXPECT_DOUBLE_EQ(j["range"]["max_s"].get<double>(), 10.0) << action;
    EXPECT_DOUBLE_EQ(j["rate"].get<double>(), 1.0) << action;
  }
  EXPECT_TRUE(pb.play_called);
  EXPECT_TRUE(pb.pause_called);
}

TEST(ToolRegistry, SeekForwardsTimeAndValidatesArgs) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  auto r = reg.execute("playback", {{"action", "seek"}, {"time_s", 7.25}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_DOUBLE_EQ(pb.last_seek_s, 7.25);

  // Missing / wrong-typed time_s -> clean failure, host untouched.
  pb.last_seek_s = -1.0;
  EXPECT_FALSE(reg.execute("playback", {{"action", "seek"}}, ctx).ok);
  EXPECT_FALSE(reg.execute("playback", {{"action", "seek"}, {"time_s", "later"}}, ctx).ok);
  EXPECT_DOUBLE_EQ(pb.last_seek_s, -1.0);

  // Out-of-range seek: the echoed current_time_s is where the cursor LANDED
  // (the host clamps) — the contract the tool description promises.
  r = reg.execute("playback", {{"action", "seek"}, {"time_s", 999.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_DOUBLE_EQ(json::parse(r.content)["current_time_s"].get<double>(), 10.0);
}

TEST(ToolRegistry, SetPlaybackRateValidatesArgs) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  EXPECT_FALSE(reg.execute("playback", {{"action", "rate"}}, ctx).ok);
  EXPECT_FALSE(reg.execute("playback", {{"action", "rate"}, {"rate", "fast"}}, ctx).ok);
  EXPECT_DOUBLE_EQ(pb.last_rate, -1.0);  // host untouched
}

TEST(ToolRegistry, StateReadFailureDegradesCleanly) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  pb.fail_state = true;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  // action "state" converts the failed read into a tool failure.
  auto r = reg.execute("playback", {{"action", "state"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("state boom"), std::string::npos);

  // A mutation still succeeds; the echo self-describes the missing state.
  r = reg.execute("playback", {{"action", "play"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_TRUE(pb.play_called);
  EXPECT_NE(r.content.find("state_unavailable"), std::string::npos);
}

TEST(ToolRegistry, SetPlaybackRateClampsPluginSide) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  ASSERT_TRUE(reg.execute("playback", {{"action", "rate"}, {"rate", 0.25}}, ctx).ok);
  EXPECT_DOUBLE_EQ(pb.last_rate, 0.25);
  ASSERT_TRUE(reg.execute("playback", {{"action", "rate"}, {"rate", 10000.0}}, ctx).ok);
  EXPECT_DOUBLE_EQ(pb.last_rate, 20.0);  // clamped high
  ASSERT_TRUE(reg.execute("playback", {{"action", "rate"}, {"rate", 0.0}}, ctx).ok);
  EXPECT_DOUBLE_EQ(pb.last_rate, 0.05);  // clamped low (never 0 -> host reject loop)
}

TEST(ToolRegistry, PlaybackToolsDegradeWithoutHost) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);  // no playback host bound

  for (const char* action : {"play", "pause", "state"}) {
    auto r = reg.execute("playback", {{"action", action}}, ctx);
    EXPECT_FALSE(r.ok) << action;
    EXPECT_NE(r.content.find("pj.playback.v1"), std::string::npos) << action;
  }
  EXPECT_FALSE(reg.execute("playback", {{"action", "seek"}, {"time_s", 1.0}}, ctx).ok);
}

TEST(ToolRegistry, PlaybackRejectsAnUnknownAction) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  auto r = reg.execute("playback", {{"action", "fly"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_FALSE(pb.play_called);
  EXPECT_FALSE(pb.pause_called);
}

TEST(ToolRegistry, PlaybackRequiresAnAction) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  EXPECT_FALSE(reg.execute("playback", json::object(), ctx).ok);
}

// --- the assistant's own plot tabs ------------------------------------------
//
// plot_tab replaced zoom_to_time_range/zoom_reset as one tool with an 'action'
// argument. The product rule it exists to enforce: the model composes only in
// tabs it created, and the user's tabs are unreachable — a rule the HOST
// enforces (FakePlotTabsHost models that), not the plugin, so every test here
// drives the real ownership check rather than trusting the plugin's own
// bookkeeping.

TEST(ToolRegistry, PlotTabCreateReportsTheTabTheHostHolds) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlotTabsHost tabs;
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();

  auto r = reg.execute("plot_tab", {{"action", "create"}, {"tab", "analysis"}, {"title", "Analysis"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["tab"], "analysis");
  EXPECT_EQ(j["title"], "Analysis");
  EXPECT_TRUE(j["curves"].empty());
}

TEST(ToolRegistry, PlotTabAddReportsWhatActuallyLanded) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  store.addTopic("/left");
  store.addField("/left", "speed", {0, kSec}, {1.0, 2.0});
  FakePlotTabsHost tabs;
  tabs.unresolvable.insert("/left");  // the host cannot place this one
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "view"}}, ctx).ok);

  auto r = reg.execute(
      "plot_tab", {{"action", "add"}, {"tab", "view"}, {"curves", json::array({"/imu/x", "/left/speed"})}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  // The host silently dropped /left/speed: the read-back shows only what
  // actually landed, so the dropped curve is absent, not silently claimed.
  ASSERT_EQ(j["curves"].size(), 1u);
  EXPECT_EQ(j["curves"][0]["topic"], "/imu");
  EXPECT_EQ(j["curves"][0]["field"], "x");

  // A path the catalog cannot resolve at all is a request error; when every
  // requested curve is like that, the call fails outright.
  auto none = reg.execute("plot_tab", {{"action", "add"}, {"tab", "view"}, {"curves", "/does/not/exist"}}, ctx);
  EXPECT_FALSE(none.ok) << none.content;

  // The harder failure, and the one the read-back exists for: a path the
  // CATALOG resolves, which the host then accepts and silently places nowhere.
  // Every call returned success, so only the tab's own contents can tell the
  // truth — reporting this as a drawing would be the lie.
  auto dropped = reg.execute("plot_tab", {{"action", "add"}, {"tab", "view"}, {"curves", "/left/speed"}}, ctx);
  EXPECT_FALSE(dropped.ok) << dropped.content;
  EXPECT_NE(dropped.content.find("did not land"), std::string::npos) << dropped.content;
}

// The product rule stated as a test: the host, not the plugin, is what keeps
// this assistant out of the user's own tabs.
TEST(ToolRegistry, PlotTabRefusesATabItDoesNotOwn) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlotTabsHost tabs;
  tabs.addForeignTab("user-1");
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();

  EXPECT_FALSE(
      reg.execute("plot_tab", {{"action", "add"}, {"tab", "user-1"}, {"curves", json::array({"/imu/x"})}}, ctx).ok);
  EXPECT_FALSE(
      reg.execute("plot_tab", {{"action", "remove"}, {"tab", "user-1"}, {"curves", json::array({"/imu/x"})}}, ctx).ok);
  // zoom never touches pj.plot_tabs.v1 at all (it goes through ctx.viewport,
  // left unbound here), so it fails on the missing service rather than ever
  // reaching the fake -- still "every one fails" as required.
  EXPECT_FALSE(reg.execute("plot_tab", {{"action", "zoom"}, {"tab", "user-1"}}, ctx).ok);
  EXPECT_FALSE(reg.execute("plot_tab", {{"action", "close"}, {"tab", "user-1"}}, ctx).ok);

  EXPECT_EQ(tabs.foreign_mutations, 0);
}

// The structural version of the property above: loop over every registered
// tool (not just plot_tab) with a foreign tab named in its args, so this
// keeps holding automatically as tools are added later.
TEST(ToolRegistry, NoToolReachesAForeignTab) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlotTabsHost tabs;
  tabs.addForeignTab("user-1");
  RecordingDpHost dp;
  FakePlaybackHost pb;
  FakeViewportHost vp;
  auto ctx = makeCtx(store, &dp);
  ctx.plot_tabs = tabs.view();
  ctx.playback = pb.view();
  ctx.viewport = vp.view();

  const json plot_tab_args = {{"action", "add"}, {"tab", "user-1"}, {"curves", json::array({"/imu/x"})}};
  const json generic_args = {{"tab", "user-1"}};
  for (const auto& tool : reg.tools()) {
    std::ignore = reg.execute(tool.name, plot_tab_args, ctx);
    std::ignore = reg.execute(tool.name, generic_args, ctx);
  }

  EXPECT_EQ(tabs.foreign_mutations, 0) << "some tool call reached a tab this plugin does not own";
}

TEST(ToolRegistry, PlotTabListShowsOnlyItsOwn) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlotTabsHost tabs;
  tabs.addForeignTab("user-1");
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "a"}}, ctx).ok);
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "b"}}, ctx).ok);

  auto r = reg.execute("plot_tab", {{"action", "list"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(json::parse(r.content)["count"], 2);
  EXPECT_EQ(r.content.find("user-1"), std::string::npos) << r.content;
}

// Owning nothing is an answer, not an error.
TEST(ToolRegistry, PlotTabListWithNoTabsSucceeds) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlotTabsHost tabs;
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();

  auto r = reg.execute("plot_tab", {{"action", "list"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(json::parse(r.content)["count"], 0);
}

TEST(ToolRegistry, PlotTabResolvesAbbreviatedCurves) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);  // /imu with fields x and y -- "x" alone is unambiguous
  FakePlotTabsHost tabs;
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "view"}}, ctx).ok);

  auto r = reg.execute("plot_tab", {{"action", "add"}, {"tab", "view"}, {"curves", "x"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  ASSERT_EQ(tabs.find("view")->curves.size(), 1u);
  EXPECT_EQ(tabs.find("view")->curves[0].topic, "/imu");
  EXPECT_EQ(tabs.find("view")->curves[0].field, "x");
}

TEST(ToolRegistry, PlotTabRefusesAnAmbiguousCurve) {
  ToolRegistry reg;
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu", "/speed"}).addDataset("run_friday.mcap", {"/imu", "/speed"});
  FakePlotTabsHost tabs;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.plot_tabs = tabs.view();
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "view"}}, ctx).ok);

  auto r = reg.execute("plot_tab", {{"action", "add"}, {"tab", "view"}, {"curves", "/speed/value"}}, ctx);
  EXPECT_FALSE(r.ok) << r.content;
  EXPECT_NE(r.content.find("run_monday.mcap:/speed/value"), std::string::npos) << r.content;
  EXPECT_NE(r.content.find("run_friday.mcap:/speed/value"), std::string::npos) << r.content;
  EXPECT_TRUE(tabs.find("view")->curves.empty());
}

TEST(ToolRegistry, PlotTabQualifiedCurveCarriesItsDataset) {
  ToolRegistry reg;
  FakeMultiDatasetHost host;
  host.addDataset("run_monday.mcap", {"/imu", "/speed"}).addDataset("run_friday.mcap", {"/imu", "/speed"});
  FakePlotTabsHost tabs;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.plot_tabs = tabs.view();
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "view"}}, ctx).ok);

  auto r =
      reg.execute("plot_tab", {{"action", "add"}, {"tab", "view"}, {"curves", "run_friday.mcap:/speed/value"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  ASSERT_EQ(tabs.find("view")->curves.size(), 1u);
  EXPECT_EQ(tabs.find("view")->curves[0].dataset, "run_friday.mcap");
}

TEST(ToolRegistry, PlotTabZoomValidatesItsRange) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlotTabsHost tabs;
  FakeViewportHost vp;
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();
  ctx.viewport = vp.view();
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "view"}}, ctx).ok);

  // start >= end is rejected BEFORE the host sees it.
  EXPECT_FALSE(
      reg.execute("plot_tab", {{"action", "zoom"}, {"tab", "view"}, {"start_s", 9.0}, {"end_s", 1.0}}, ctx).ok);
  EXPECT_FALSE(
      reg.execute("plot_tab", {{"action", "zoom"}, {"tab", "view"}, {"start_s", 5.0}, {"end_s", 5.0}}, ctx).ok);
  // Only one of start_s/end_s given is refused too.
  EXPECT_FALSE(reg.execute("plot_tab", {{"action", "zoom"}, {"tab", "view"}, {"start_s", 1.0}}, ctx).ok);
  EXPECT_EQ(vp.zoom_calls, 0);

  // Neither given -> fit the view.
  auto r = reg.execute("plot_tab", {{"action", "zoom"}, {"tab", "view"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_TRUE(vp.reset_called);
}

// Zoom names one of the caller's tabs: one it does not own (the user's, or none at all) is refused with the tabs it
// does own, and the host's per-tab zoom gets the tab named, never the viewport's every-tab zoom.
TEST(ToolRegistry, PlotTabZoomFramesOnlyTheNamedOwnTab) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlotTabsHost tabs;
  tabs.addForeignTab("user-1");
  FakeViewportHost vp;
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();
  ctx.viewport = vp.view();
  std::vector<std::pair<std::string, std::optional<std::pair<double, double>>>> zooms;
  ctx.zoom_tab = [&zooms](const std::string& tab, std::optional<std::pair<double, double>> range) {
    zooms.emplace_back(tab, range);
    return PJ::okStatus();
  };
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "a"}}, ctx).ok);
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "b"}}, ctx).ok);

  for (const char* stranger : {"user-1", "nope"}) {
    const auto refused =
        reg.execute("plot_tab", {{"action", "zoom"}, {"tab", stranger}, {"start_s", 1.0}, {"end_s", 2.0}}, ctx);
    EXPECT_FALSE(refused.ok) << stranger;
    EXPECT_NE(refused.content.find("a, b"), std::string::npos) << refused.content;
  }
  EXPECT_TRUE(zooms.empty());

  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "zoom"}, {"tab", "a"}, {"start_s", 1.0}, {"end_s", 2.0}}, ctx).ok);
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "zoom"}, {"tab", "b"}}, ctx).ok);
  ASSERT_EQ(zooms.size(), 2U);
  EXPECT_EQ(zooms[0].first, "a");
  EXPECT_EQ(zooms[0].second, (std::optional<std::pair<double, double>>{{1.0, 2.0}}));
  EXPECT_EQ(zooms[1].first, "b");
  EXPECT_FALSE(zooms[1].second.has_value());
  EXPECT_EQ(vp.zoom_calls, 0);
  EXPECT_FALSE(vp.reset_called);
  EXPECT_EQ(tabs.foreign_mutations, 0);
}

TEST(ToolRegistry, PlotTabDegradesWithoutTheService) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);  // ctx.plot_tabs left unbound

  auto r = reg.execute("plot_tab", {{"action", "list"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("pj.plot_tabs.v1"), std::string::npos) << r.content;
}

TEST(ToolRegistry, PlotTabUnknownActionIsACleanFailure) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlotTabsHost tabs;
  auto ctx = makeCtx(store, nullptr);
  ctx.plot_tabs = tabs.view();

  EXPECT_FALSE(reg.execute("plot_tab", {{"action", "levitate"}}, ctx).ok);
  EXPECT_FALSE(reg.execute("plot_tab", json::object(), ctx).ok);
}

// --- the assistant's own scene views ----------------------------------------
//
// scene_view is plot_tab's counterpart for the 3D/2D object viewer: same
// create/attach-or-detach/close/list shape, same host-enforced ownership
// (FakePlotTabsHost models that), same
// read-the-view-back-rather-than-trust-the-call-result discipline. Attaching
// takes object-topic paths (point clouds, scene entities...), resolved
// through the v2 catalog by resolveObjectTopic rather than plot_tab's
// topic/field series resolution.

TEST(ToolRegistry, SceneViewCreateAttachReadsBack) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 42, 0, 5 * kSec);
  FakePlotTabsHost scenes;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.plot_tabs = scenes.view();

  auto created = reg.execute("scene_view", {{"action", "create"}, {"view", "scene"}, {"title", "Cloud"}}, ctx);
  ASSERT_TRUE(created.ok) << created.content;
  json cj = json::parse(created.content);
  EXPECT_EQ(cj["view"], "scene");
  EXPECT_EQ(cj["kind"], "3d");  // default
  EXPECT_EQ(cj["title"], "Cloud");
  EXPECT_TRUE(cj["topics"].empty());

  auto attached = reg.execute("scene_view", {{"action", "attach"}, {"view", "scene"}, {"topics", "/cloud"}}, ctx);
  ASSERT_TRUE(attached.ok) << attached.content;
  json aj = json::parse(attached.content);
  ASSERT_EQ(aj["topics"].size(), 1u);
  EXPECT_EQ(aj["topics"][0]["topic"], "/cloud");
  ASSERT_EQ(aj["results"].size(), 1u);
  EXPECT_EQ(aj["results"][0]["topic"], "/cloud");
  EXPECT_EQ(aj["results"][0]["attached"], true);
}

// Three ways a requested topic can fail to end up attached: it is not in the
// catalog at all, the catalog knows it but the host silently drops it (the
// harder case -- every call still "succeeded"), and a "2d" view refuses a
// type it does not accept. All three must be reported per-topic rather than
// leaving the model to infer a miss from a shorter-than-expected list.
TEST(ToolRegistry, SceneViewAttachUnknownTopicIsReported) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 5, 0, kSec);
  host.addObjectTopic("/image", "kImage", 3, 0, kSec);
  FakePlotTabsHost scenes;
  scenes.unresolvable_topics.insert("/cloud");  // catalog knows it, the host does not place it
  scenes.kind_rejects.insert("/image");         // the "2d" view below refuses this one
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.plot_tabs = scenes.view();
  ASSERT_TRUE(reg.execute("scene_view", {{"action", "create"}, {"view", "flat"}, {"kind", "2d"}}, ctx).ok);

  // A path the catalog cannot resolve at all is a request error.
  auto missing =
      reg.execute("scene_view", {{"action", "attach"}, {"view", "flat"}, {"topics", "/does/not/exist"}}, ctx);
  EXPECT_FALSE(missing.ok) << missing.content;
  EXPECT_NE(missing.content.find("not a loaded object topic"), std::string::npos) << missing.content;

  // The catalog resolves it, the host accepts the call and places it nowhere.
  auto dropped = reg.execute("scene_view", {{"action", "attach"}, {"view", "flat"}, {"topics", "/cloud"}}, ctx);
  EXPECT_FALSE(dropped.ok) << dropped.content;
  EXPECT_NE(dropped.content.find("did not land"), std::string::npos) << dropped.content;

  // The host refuses the type outright for this view's kind.
  auto refused = reg.execute("scene_view", {{"action", "attach"}, {"view", "flat"}, {"topics", "/image"}}, ctx);
  EXPECT_FALSE(refused.ok) << refused.content;
  json rj = json::parse(refused.content);
  ASSERT_EQ(rj["results"].size(), 1u);
  EXPECT_TRUE(rj["results"][0].contains("error"));
  EXPECT_TRUE(scenes.find("flat")->topics.empty());
}

// The product rule stated as a test: the host, not the plugin, is what keeps
// this assistant out of the user's own scene docks.
TEST(ToolRegistry, SceneViewForeignViewIsUnreachable) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 5, 0, kSec);
  FakePlotTabsHost scenes;
  scenes.addForeignView("user-1");
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.plot_tabs = scenes.view();

  EXPECT_FALSE(
      reg.execute("scene_view", {{"action", "attach"}, {"view", "user-1"}, {"topics", json::array({"/cloud"})}}, ctx)
          .ok);
  EXPECT_FALSE(
      reg.execute("scene_view", {{"action", "detach"}, {"view", "user-1"}, {"topics", json::array({"/cloud"})}}, ctx)
          .ok);
  EXPECT_FALSE(reg.execute("scene_view", {{"action", "focus"}, {"view", "user-1"}}, ctx).ok);
  EXPECT_FALSE(reg.execute("scene_view", {{"action", "close"}, {"view", "user-1"}}, ctx).ok);

  EXPECT_EQ(scenes.foreign_mutations, 0);
}

TEST(ToolRegistry, SceneViewDetachAndClose) {
  ToolRegistry reg;
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 5, 0, kSec);
  FakePlotTabsHost scenes;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.plot_tabs = scenes.view();
  ASSERT_TRUE(reg.execute("scene_view", {{"action", "create"}, {"view", "scene"}}, ctx).ok);
  ASSERT_TRUE(reg.execute("scene_view", {{"action", "attach"}, {"view", "scene"}, {"topics", "/cloud"}}, ctx).ok);

  auto detached = reg.execute("scene_view", {{"action", "detach"}, {"view", "scene"}, {"topics", "/cloud"}}, ctx);
  ASSERT_TRUE(detached.ok) << detached.content;
  json dj = json::parse(detached.content);
  EXPECT_TRUE(dj["topics"].empty());
  ASSERT_EQ(dj["results"].size(), 1u);
  EXPECT_EQ(dj["results"][0]["detached"], true);

  auto closed = reg.execute("scene_view", {{"action", "close"}, {"view", "scene"}}, ctx);
  ASSERT_TRUE(closed.ok) << closed.content;
  EXPECT_EQ(json::parse(closed.content)["closed"], "scene");
  EXPECT_EQ(scenes.find("scene"), nullptr);
}

TEST(ToolRegistry, SceneViewListIsReadBack) {
  ToolRegistry reg;
  FakeCatalogHost host;
  FakePlotTabsHost scenes;
  scenes.addForeignView("user-1");
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.plot_tabs = scenes.view();
  ASSERT_TRUE(reg.execute("scene_view", {{"action", "create"}, {"view", "a"}, {"kind", "3d"}}, ctx).ok);
  ASSERT_TRUE(reg.execute("scene_view", {{"action", "create"}, {"view", "b"}, {"kind", "2d"}}, ctx).ok);

  auto r = reg.execute("scene_view", {{"action", "list"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(json::parse(r.content)["count"], 2);
  EXPECT_EQ(r.content.find("user-1"), std::string::npos) << r.content;

  // Owning nothing is an answer, not an error.
  FakePlotTabsHost empty_scenes;
  ToolContext empty_ctx;
  empty_ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  empty_ctx.plot_tabs = empty_scenes.view();
  auto empty_r = reg.execute("scene_view", {{"action", "list"}}, empty_ctx);
  ASSERT_TRUE(empty_r.ok) << empty_r.content;
  EXPECT_EQ(json::parse(empty_r.content)["count"], 0);
}

TEST(ToolRegistry, SceneViewOnOldHostReportsTheMissingSlot) {
  ToolRegistry reg;
  FakePlotTabsHost tabs;
  tabs.struct_size = 64;  // a 0.35 host: the seven v1 slots, no tail
  ToolContext ctx;
  ctx.plot_tabs = tabs.view();

  auto r = reg.execute("scene_view", {{"action", "create"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("does not support create_tab_v2"), std::string::npos) << r.content;
  auto listed = reg.execute("scene_view", {{"action", "list"}}, ctx);
  ASSERT_TRUE(listed.ok) << listed.content;
  EXPECT_EQ(json::parse(listed.content)["count"], 0) << "such a host has no scene tabs";

  // Full-size vtable with NULL tail slots (a host with no scene workspace).
  FakePlotTabsHost no_scene;
  no_scene.null_tail = true;
  ctx.plot_tabs = no_scene.view();
  EXPECT_FALSE(reg.execute("scene_view", {{"action", "create"}}, ctx).ok);

  // No plot tabs service at all.
  ToolContext unbound;
  auto u = reg.execute("scene_view", {{"action", "list"}}, unbound);
  EXPECT_FALSE(u.ok);
  EXPECT_NE(u.content.find("not bound"), std::string::npos) << u.content;
}

// The host list is the union of plot and scene tabs; each tool shows its own kind.
TEST(ToolRegistry, PlotTabAndSceneViewListsAreDisjoint) {
  ToolRegistry reg;
  FakeCatalogHost host;
  FakePlotTabsHost tabs;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.plot_tabs = tabs.view();
  ASSERT_TRUE(reg.execute("plot_tab", {{"action", "create"}, {"tab", "p"}}, ctx).ok);
  ASSERT_TRUE(reg.execute("scene_view", {{"action", "create"}, {"view", "s"}}, ctx).ok);

  auto plots = reg.execute("plot_tab", {{"action", "list"}}, ctx);
  ASSERT_TRUE(plots.ok) << plots.content;
  json pj = json::parse(plots.content);
  ASSERT_EQ(pj["count"], 1);
  EXPECT_EQ(pj["tabs"][0]["tab"], "p");

  auto scenes = reg.execute("scene_view", {{"action", "list"}}, ctx);
  ASSERT_TRUE(scenes.ok) << scenes.content;
  json sj = json::parse(scenes.content);
  ASSERT_EQ(sj["count"], 1);
  EXPECT_EQ(sj["views"][0]["view"], "s");
}

TEST(ToolRegistry, ReadSeriesStatsGainDisplayStartWhenPlaybackBound) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  pb.display_offset_ns = -2'000'000'000;  // display = absolute + 2 s
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  auto r = reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "stats"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  auto j = json::parse(r.content);
  ASSERT_TRUE(j["stats"].contains("t_start_display_s")) << r.content;
  // /imu/x starts at absolute 0 ns -> display 2.0 s under the fake offset.
  EXPECT_DOUBLE_EQ(j["stats"]["t_start_display_s"].get<double>(), 2.0);
  EXPECT_EQ(pb.last_topic, "/imu");  // converted against the OWNING topic

  // Without a playback host the field is absent (not an error).
  auto plain = makeCtx(store, nullptr);
  r = reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "stats"}}, plain);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_FALSE(json::parse(r.content)["stats"].contains("t_start_display_s"));
}

// --- t_start_s/t_end_s: narrowing a read to a display-axis window ----------
//
// /imu/x (populate()) has samples at absolute 0..4 s; with the FakePlaybackHost
// offset of -2 s (display = absolute + 2 s), that is display [2, 6].

TEST(ToolRegistry, WindowSelectsSamplesOnDisplayAxis) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  pb.display_offset_ns = -2'000'000'000;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  auto r = reg.execute(
      "read_series",
      {{"series", "/imu/x"}, {"mode", "buckets"}, {"max_points", 10}, {"t_start_s", 3.0}, {"t_end_s", 5.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  ASSERT_TRUE(j.contains("window")) << r.content;
  EXPECT_EQ(j["window"]["axis"], "display");
  EXPECT_DOUBLE_EQ(j["window"]["t_start_s"].get<double>(), 3.0);
  EXPECT_DOUBLE_EQ(j["window"]["t_end_s"].get<double>(), 5.0);
  // display [3, 5] selects the absolute samples at 1, 2, 3 s (display 3, 4, 5).
  EXPECT_EQ(j["stats"]["count"], 3);
  EXPECT_DOUBLE_EQ(j["stats"]["t_start_display_s"].get<double>(), 3.0);
  // Bucket 't0' is relative to the first sample INSIDE the window, so the
  // first bucket starts at 0, not at whatever t_start_s was.
  ASSERT_FALSE(j["buckets"]["n"].empty());
  EXPECT_DOUBLE_EQ(j["buckets"]["t0"].get<double>(), 0.0);
}

TEST(ToolRegistry, WindowAppliesToStatsToo) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  pb.display_offset_ns = -2'000'000'000;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  auto r = reg.execute("read_series", {{"series", "/imu/x"}, {"t_start_s", 3.0}, {"t_end_s", 5.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["stats"]["count"], 3);
  EXPECT_DOUBLE_EQ(j["stats"]["min"].get<double>(), 1.0);
  EXPECT_DOUBLE_EQ(j["stats"]["max"].get<double>(), 3.0);
  ASSERT_TRUE(j.contains("window")) << r.content;
  EXPECT_EQ(j["window"]["axis"], "display");
}

// --- mode: "raw" -------------------------------------------------------
//
// raw returns actual sample values instead of a summary, so unlike stats and
// buckets it REQUIRES a t_start_s/t_end_s window -- there is no sane default
// short of the whole recording.

TEST(ToolRegistry, RawNeedsAWindow) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);

  auto neither = reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "raw"}}, ctx);
  EXPECT_FALSE(neither.ok);
  EXPECT_NE(neither.content.find("t_start_s"), std::string::npos) << neither.content;
  EXPECT_NE(neither.content.find("t_end_s"), std::string::npos) << neither.content;

  // Half a window is not enough either.
  auto only_start = reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "raw"}, {"t_start_s", 1.0}}, ctx);
  EXPECT_FALSE(only_start.ok);
  auto only_end = reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "raw"}, {"t_end_s", 3.0}}, ctx);
  EXPECT_FALSE(only_end.ok);
}

// /imu/x (populate()) has samples at absolute 0..4 s; with the FakePlaybackHost
// offset of -2 s (display = absolute + 2 s), display window [3, 5] selects the
// absolute samples at 1, 2, 3 s -- same window WindowSelectsSamplesOnDisplayAxis
// uses for buckets.
TEST(ToolRegistry, RawReturnsSamplesInsideTheWindow) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  pb.display_offset_ns = -2'000'000'000;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  auto r =
      reg.execute("read_series", {{"series", "/imu/x"}, {"mode", "raw"}, {"t_start_s", 3.0}, {"t_end_s", 5.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["series"], "/imu/x");
  EXPECT_DOUBLE_EQ(j["t0_display_s"].get<double>(), 3.0);
  EXPECT_EQ(j["count"], 3);
  EXPECT_FALSE(j.contains("truncated"));
  ASSERT_EQ(j["t"].size(), 3u);
  ASSERT_EQ(j["v"].size(), 3u);
  EXPECT_DOUBLE_EQ(j["t"][0].get<double>(), 0.0);
  EXPECT_DOUBLE_EQ(j["t"][1].get<double>(), 1.0);
  EXPECT_DOUBLE_EQ(j["t"][2].get<double>(), 2.0);
  EXPECT_DOUBLE_EQ(j["v"][0].get<double>(), 1.0);
  EXPECT_DOUBLE_EQ(j["v"][1].get<double>(), 2.0);
  EXPECT_DOUBLE_EQ(j["v"][2].get<double>(), 3.0);
}

TEST(ToolRegistry, RawTruncatesAt200) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/rc");
  constexpr int kSamples = 1000;
  std::vector<std::int64_t> ts;
  std::vector<double> vals;
  ts.reserve(kSamples);
  vals.reserve(kSamples);
  for (int i = 0; i < kSamples; ++i) {
    ts.push_back(static_cast<std::int64_t>(i) * (kSec / 100));  // 100 Hz, spans ~10 s
    vals.push_back(static_cast<double>(i));
  }
  store.addField("/rc", "x", std::move(ts), std::move(vals));
  FakePlaybackHost pb;  // offset 0: display == absolute
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  auto r =
      reg.execute("read_series", {{"series", "/rc/x"}, {"mode", "raw"}, {"t_start_s", 0.0}, {"t_end_s", 100.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["count"], 200);
  EXPECT_TRUE(j["truncated"].get<bool>());
  EXPECT_EQ(j["window_samples"], 1000);
  EXPECT_EQ(j["t"].size(), 200u);
  EXPECT_EQ(j["v"].size(), 200u);
}

// The batch cap check happens before any series is even read, so it fires
// regardless of whether the paths resolve.
TEST(ToolRegistry, RawBatchCappedAtFour) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);

  json many = json::array();
  for (int i = 0; i < 5; ++i) {
    many.push_back("/imu/x");
  }
  auto r = reg.execute("read_series", {{"paths", many}, {"mode", "raw"}, {"t_start_s", 0.0}, {"t_end_s", 4.0}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("at most 4"), std::string::npos) << r.content;
}

// The 4-paths/200-samples raw caps are a guess, not a measurement: a
// realistic batch of long (dataset-qualified-looking) paths at 200 samples
// each can still overrun kMaxResponseBytes. The raw envelope now measures
// its own dump and shrinks the per-series sample count in lockstep, the same
// way buckets' batch envelope already does, until it fits.
TEST(ToolRegistry, RawBatchMeasuresAndShrinksToFitTheResponseCap) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  constexpr int kSamples = 200;
  json paths = json::array();
  // A long, dataset-qualified-looking name per series -- realistic (a
  // "[stream] some very descriptive source name":/topic/field qualifier adds
  // up) -- pushes the total over the cap on top of what 200 samples' worth
  // of "t"/"v" arrays alone already costs, which by itself comes in just
  // under it.
  const std::string long_prefix(400, 'x');
  for (int s = 0; s < 4; ++s) {
    const std::string topic = "/" + long_prefix + "_topic_name_for_series_number_" + std::to_string(s);
    store.addTopic(topic);
    std::vector<std::int64_t> ts;
    std::vector<double> vals;
    ts.reserve(kSamples);
    vals.reserve(kSamples);
    for (int i = 0; i < kSamples; ++i) {
      ts.push_back(static_cast<std::int64_t>(i) * (kSec / 100));  // 100 Hz, spans ~2 s
      vals.push_back(static_cast<double>(i) + 0.123456789);
    }
    store.addField(topic, "value", ts, vals);
    paths.push_back(topic + "/value");
  }
  FakePlaybackHost pb;  // offset 0: display == absolute
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  auto r = reg.execute("read_series", {{"paths", paths}, {"mode", "raw"}, {"t_start_s", 0.0}, {"t_end_s", 100.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_LE(r.content.size(), 16u * 1024u) << r.content;
  const json j = json::parse(r.content);
  ASSERT_EQ(j["count"], 4);
  bool any_truncated = false;
  for (const auto& entry : j["read"]) {
    if (entry.value("truncated", false)) {
      any_truncated = true;
      EXPECT_EQ(entry["window_samples"], kSamples) << entry.dump();
      EXPECT_LT(entry["count"].get<int>(), kSamples) << entry.dump();
    }
  }
  EXPECT_TRUE(any_truncated) << "the batch must have needed shrinking below the request's own 200-sample cap: "
                             << j.dump();
}

TEST(ToolRegistry, RawNullsNonFinite) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/imu");
  store.addField("/imu", "raw_nan", {0, kSec, 2 * kSec, 3 * kSec}, {1.0, std::nan(""), 3.0, std::nan("")});
  FakePlaybackHost pb;  // offset 0: display == absolute
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  auto r = reg.execute(
      "read_series", {{"series", "/imu/raw_nan"}, {"mode", "raw"}, {"t_start_s", 0.0}, {"t_end_s", 3.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  ASSERT_EQ(j["v"].size(), 4u);
  EXPECT_DOUBLE_EQ(j["v"][0].get<double>(), 1.0);
  EXPECT_TRUE(j["v"][1].is_null());
  EXPECT_DOUBLE_EQ(j["v"][2].get<double>(), 3.0);
  EXPECT_TRUE(j["v"][3].is_null());
}

// flat_span_s/flat_span_at_s describe the WHOLE series, even when the read is
// windowed to a slice that does not contain the freeze: "this channel froze
// for 21 s" must survive asking about a different 5 s of the same flight.
TEST(ToolRegistry, FlatSpanSurvivesAWindowOverAMovingPart) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  // 20 samples, 1 Hz: index 0..14 vary, 15..19 freeze at the same value —
  // 4 s of freeze out of a 19 s span, starting at t=15 s.
  std::vector<std::int64_t> ts;
  std::vector<double> v;
  for (int i = 0; i < 20; ++i) {
    ts.push_back(static_cast<std::int64_t>(i) * kSec);
    v.push_back(i < 15 ? static_cast<double>(i % 5) : 42.0);
  }
  store.addField("/imu", "servo", ts, v);
  FakePlaybackHost pb;  // default offset 0: display seconds == absolute seconds
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  // Window over [0, 5] s — entirely inside the varying part; the freeze at
  // [15, 19] s is outside the window.
  auto r = reg.execute("read_series", {{"series", "/imu/servo"}, {"t_start_s", 0.0}, {"t_end_s", 5.0}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  ASSERT_TRUE(j.contains("window")) << r.content;
  EXPECT_LT(j["stats"]["count"].get<int>(), 20) << "the window must have narrowed the read";
  EXPECT_FALSE(j["stats"].contains("constant"));
  ASSERT_TRUE(j["stats"].contains("flat_span_s")) << r.content;
  EXPECT_NEAR(j["stats"]["flat_span_s"].get<double>(), 4.0, 1e-9);
  EXPECT_NEAR(j["stats"]["flat_span_at_s"].get<double>(), 15.0, 1e-9);
}

// The host hooks a windowed read_series decodes only the window with, built over the store's whole read: read_window
// serves the samples stamped in [first, last] plus the one before from a store of their own, so a read that leaned on
// the rest of the series would find it missing; scan_values hands out blocks cut every third sample as chunk
// boundaries cut them, a block of equal values as its one value as a constant chunk is. Each hook counts its calls
// into `calls`.
void addWindowHooks(ToolContext& ctx, int& calls) {
  ctx.read_window = [host = ctx.host, &calls](
                        PJ::sdk::FieldHandle field, std::int64_t first,
                        std::int64_t last) -> PJ::Expected<PJ::sdk::MaterializedSeriesView> {
    ++calls;
    auto whole = host.readSeries(field);
    if (!whole) {
      return PJ::unexpected(whole.error());
    }
    const auto ts = whole->timestamps();
    const double* v = whole->valuesAsFloat64();
    auto lo = static_cast<std::size_t>(std::lower_bound(ts.begin(), ts.end(), first) - ts.begin());
    lo = lo > 0 ? lo - 1 : 0;
    const auto hi = static_cast<std::size_t>(std::upper_bound(ts.begin(), ts.end(), last) - ts.begin());
    if (lo == hi) {
      return PJ::sdk::MaterializedSeriesView{};  // the store cannot hold an empty field; the host reads it back empty
    }
    PJ::testing::ToolboxTestStore window;
    window.addTopic("w").addField(
        "w", "v", std::vector<std::int64_t>(ts.begin() + lo, ts.begin() + hi), std::vector<double>(v + lo, v + hi));
    const PJ::sdk::ToolboxHostView window_host(window.makeHost());
    return window_host.readSeries(window_host.catalogSnapshot()->fields()[0].handle);
  };
  ctx.scan_values = [host = ctx.host, &calls](
                        PJ::sdk::FieldHandle field, const PJ::agent_tools::ValueBlockSink& on_block) -> PJ::Status {
    ++calls;
    auto whole = host.readSeries(field);
    if (!whole) {
      return PJ::unexpected(whole.error());
    }
    const auto ts = whole->timestamps();
    const double* v = whole->valuesAsFloat64();
    for (std::size_t i = 0; i < ts.size(); i += 3) {
      const std::size_t n = std::min<std::size_t>(3, ts.size() - i);
      const bool equal = std::all_of(v + i, v + i + n, [&](double x) { return x == v[i]; });
      on_block(std::span<const std::int64_t>(ts.data() + i, n), std::span<const double>(v + i, equal ? 1 : n));
    }
    return PJ::okStatus();
  };
}

// With the window hooks, a windowed read decodes only the window, and answers exactly what the whole read cut to the
// window answers: the stats, the samples, the whole-series flat facts, and every error.
TEST(ToolRegistry, WindowHooksGiveTheWholeReadsAnswers) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  std::vector<std::int64_t> ts;
  std::vector<double> v;
  for (int i = 0; i < 20; ++i) {
    ts.push_back(static_cast<std::int64_t>(i) * kSec);
    v.push_back(i < 15 ? static_cast<double>(i % 5) : 42.0);
  }
  store.addField("/imu", "servo", ts, v);
  store.addField("/imu", "gaps", {0, kSec, 2 * kSec, 3 * kSec}, {1.0, std::nan(""), std::nan(""), 2.0});
  FakePlaybackHost pb;
  pb.display_offset_ns = -2'000'000'000;  // display = absolute + 2 s
  auto whole = makeCtx(store, nullptr);
  whole.playback = pb.view();
  auto hooked = makeCtx(store, nullptr);
  hooked.playback = pb.view();
  int hook_calls = 0;
  addWindowHooks(hooked, hook_calls);

  const std::vector<json> requests = {
      {{"series", "/imu/servo"}, {"t_start_s", 7.0}, {"t_end_s", 12.0}},
      {{"series", "/imu/servo"}, {"t_start_s", 7.0}, {"t_end_s", 12.0}, {"mode", "raw"}},
      {{"series", "/imu/servo"}, {"t_start_s", 7.0}, {"t_end_s", 12.0}, {"mode", "buckets"}},
      {{"series", "/imu/servo"}, {"t_start_s", 17.5}},
      {{"series", "/imu/servo"}, {"t_end_s", 4.5}},
      {{"series", "/imu/servo"}, {"t_start_s", 7.2}, {"t_end_s", 7.8}},      // between two samples
      {{"series", "/imu/servo"}, {"t_start_s", 100.0}, {"t_end_s", 200.0}},  // after the series
      {{"series", "/imu/servo"}, {"t_start_s", -50.0}, {"t_end_s", -40.0}},  // before it
      {{"paths", {"/imu/x", "/imu/y", "/imu/gaps"}}, {"t_start_s", 3.0}, {"t_end_s", 5.0}},
      {{"series", "/imu"}, {"t_start_s", 3.0}},
      {{"series", "/imu/nope"}, {"t_start_s", 3.0}},
  };
  for (const json& args : requests) {
    const auto expected = reg.execute("read_series", args, whole);
    const auto got = reg.execute("read_series", args, hooked);
    EXPECT_EQ(got.ok, expected.ok) << args.dump();
    EXPECT_EQ(got.content, expected.content) << args.dump();
  }
  EXPECT_GT(hook_calls, 0);
}

TEST(ToolRegistry, WindowWithOneBoundOnly) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  pb.display_offset_ns = -2'000'000'000;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  // t_start_s only: display >= 3 selects absolute 1, 2, 3, 4 s.
  auto start_only = reg.execute("read_series", {{"series", "/imu/x"}, {"t_start_s", 3.0}}, ctx);
  ASSERT_TRUE(start_only.ok) << start_only.content;
  json js = json::parse(start_only.content);
  EXPECT_EQ(js["stats"]["count"], 4);
  EXPECT_TRUE(js["window"].contains("t_start_s"));
  EXPECT_FALSE(js["window"].contains("t_end_s"));

  // t_end_s only: display <= 4 selects absolute 0, 1, 2 s.
  auto end_only = reg.execute("read_series", {{"series", "/imu/x"}, {"t_end_s", 4.0}}, ctx);
  ASSERT_TRUE(end_only.ok) << end_only.content;
  json je = json::parse(end_only.content);
  EXPECT_EQ(je["stats"]["count"], 3);
  EXPECT_TRUE(je["window"].contains("t_end_s"));
  EXPECT_FALSE(je["window"].contains("t_start_s"));
}

TEST(ToolRegistry, WindowWithoutConversionFailsThatEntry) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);  // no playback host bound

  auto r = reg.execute("read_series", {{"series", "/imu/x"}, {"t_start_s", 3.0}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("cannot map the display window"), std::string::npos) << r.content;
}

TEST(ToolRegistry, EmptyWindowIsAnErrorEntry) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  FakePlaybackHost pb;
  pb.display_offset_ns = -2'000'000'000;
  auto ctx = makeCtx(store, nullptr);
  ctx.playback = pb.view();

  // /imu/x spans display [2, 6]; [10, 11] has no overlap.
  auto r = reg.execute("read_series", {{"series", "/imu/x"}, {"t_start_s", 10.0}, {"t_end_s", 11.0}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("no samples in window"), std::string::npos) << r.content;
  EXPECT_NE(r.content.find("spans display"), std::string::npos) << r.content;
}

TEST(ToolRegistry, InvertedWindowFails) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  auto ctx = makeCtx(store, nullptr);

  auto r = reg.execute("read_series", {{"series", "/imu/x"}, {"t_start_s", 5.0}, {"t_end_s", 3.0}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("t_end_s"), std::string::npos) << r.content;
  EXPECT_NE(r.content.find("t_start_s"), std::string::npos) << r.content;
}

// --- seeing and withdrawing its own work -----------------------------------

TEST(ToolRegistry, ListsWhatItHasCreated) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.live_ids = {"doubled", "assistant_markers"};
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute("list_created", json::object(), ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["count"], 2);
  EXPECT_EQ(j["created"][0], "doubled");
}

TEST(ToolRegistry, RemovesADerivedSeriesItCreated) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.live_ids = {"doubled"};
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute("remove_derived_series", {{"name", "doubled"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_EQ(dp.last_removed, "doubled");
}

// The safety property, stated as a test rather than as a promise in a doc: a
// name this assistant never created is refused before the host is asked, and
// the refusal says what it DID create so the model can correct itself.
TEST(ToolRegistry, RefusesToRemoveSomethingItDidNotCreate) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  dp.live_ids = {"doubled"};
  ToolContext ctx = makeCtx(store, &dp);

  auto r = reg.execute("remove_derived_series", {{"name", "/imu/x"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_TRUE(dp.last_removed.empty()) << "the host must not even be asked";
  EXPECT_NE(r.content.find("doubled"), std::string::npos) << r.content;
}

// --- reading several series in one call ------------------------------------

// The measured problem: in a real analysis the model issued 25 read_series, in
// runs of up to 15 back to back, and the CLI never batches two tools into one
// response — so each read cost a full round trip that re-sent the whole
// conversation. Those runs were 45% of the turn's tokens.
TEST(ToolRegistry, ReadsSeveralSeriesInOneCall) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  store.addField("/imu", "z", {0, kSec, 2 * kSec}, {7.0, 8.0, 9.0});
  ToolContext ctx = makeCtx(store, nullptr);

  auto r = reg.execute("read_series", {{"paths", json::array({"/imu/x", "/imu/y", "/imu/z"})}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["count"], 3);
  EXPECT_EQ(j["read"].size(), 3u);
  EXPECT_EQ(j["read"][0]["series"], "/imu/x");
  EXPECT_EQ(j["read"][2]["series"], "/imu/z");
  // Each entry carries its own statistics, not a merged blob.
  EXPECT_EQ(j["read"][0]["stats"]["count"], 5);
  EXPECT_EQ(j["read"][1]["stats"]["count"], 3);
  EXPECT_FALSE(j.contains("failed"));
}

// One typo must not cost the round trip the batch exists to save.
TEST(ToolRegistry, ABadPathDoesNotSpoilTheBatch) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  ToolContext ctx = makeCtx(store, nullptr);

  auto r = reg.execute("read_series", {{"paths", json::array({"/imu/x", "/imu/nope", "/imu/y"})}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["count"], 3);
  EXPECT_EQ(j["failed"], 1);
  EXPECT_TRUE(j["read"][0].contains("stats"));
  EXPECT_TRUE(j["read"][1].contains("error")) << j["read"][1].dump();
  EXPECT_TRUE(j["read"][2].contains("stats"));
}

// A single path keeps exactly the shape it had before batching existed, so
// nothing that already worked starts reading differently to the model.
TEST(ToolRegistry, SinglePathKeepsTheOldShape) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  ToolContext ctx = makeCtx(store, nullptr);

  auto batch = json::parse(reg.execute("read_series", {{"paths", json::array({"/imu/x"})}}, ctx).content);
  auto bare = json::parse(reg.execute("read_series", {{"paths", "/imu/x"}}, ctx).content);
  auto legacy = json::parse(reg.execute("read_series", {{"series", "/imu/x"}}, ctx).content);

  EXPECT_EQ(batch["series"], "/imu/x");
  EXPECT_FALSE(batch.contains("read")) << "a lone path must not be wrapped in the batch envelope";
  EXPECT_EQ(bare, batch);
  EXPECT_EQ(legacy, batch) << "'series' still works so a conversation in flight does not break";
}

// The measured problem: the model reads 6-7 sibling channels one buckets call
// at a time over the same stretch, and each was a full round trip. Several
// paths now come back as a 'stats'-shaped envelope, each entry carrying its
// own buckets.
TEST(ToolRegistry, BucketsAcceptABatch) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  ToolContext ctx = makeCtx(store, nullptr);

  auto r = reg.execute("read_series", {{"paths", json::array({"/imu/x", "/imu/y"})}, {"mode", "buckets"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["count"], 2);
  ASSERT_EQ(j["read"].size(), 2u);
  EXPECT_EQ(j["read"][0]["series"], "/imu/x");
  EXPECT_TRUE(j["read"][0].contains("stats"));
  EXPECT_TRUE(j["read"][0].contains("buckets"));
  EXPECT_EQ(j["read"][1]["series"], "/imu/y");
  EXPECT_FALSE(j.contains("failed"));
}

// One typo must not cost the round trip the batch exists to save, same as
// 'stats'.
TEST(ToolRegistry, ABadPathDoesNotSpoilTheBucketsBatch) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  ToolContext ctx = makeCtx(store, nullptr);

  auto r =
      reg.execute("read_series", {{"paths", json::array({"/imu/x", "/imu/nope", "/imu/y"})}, {"mode", "buckets"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  const json j = json::parse(r.content);
  EXPECT_EQ(j["count"], 3);
  EXPECT_EQ(j["failed"], 1);
  EXPECT_TRUE(j["read"][0].contains("buckets")) << j["read"][0].dump();
  EXPECT_TRUE(j["read"][1].contains("error")) << j["read"][1].dump();
  EXPECT_TRUE(j["read"][2].contains("buckets")) << j["read"][2].dump();
}

// Buckets share ONE response cap across the batch (batchBuckets), so the
// batch itself is capped tighter than the flat 32-path limit -- past this
// many series sharing one cap, every one of them gets coarsened past
// usefulness.
TEST(ToolRegistry, BucketsBatchIsCappedAtEight) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  ToolContext ctx = makeCtx(store, nullptr);

  json many = json::array();
  for (int i = 0; i < 9; ++i) {
    many.push_back("/imu/x");
  }
  auto r = reg.execute("read_series", {{"paths", many}, {"mode", "buckets"}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("at most 8"), std::string::npos) << r.content;
}

// Six long, busy channels over the same stretch: seven single-series buckets
// calls at ~11 KB each (the real ALFA measurement) were 77 KB of context, and
// six independent single-series caps would allow up to 6 x 16 KiB = 96 KiB.
// The shared cap for 6 series is min(6,4) x 16 KiB = 64 KiB, well under
// either, and every series still keeps a usable number of buckets.
TEST(ToolRegistry, BatchBucketsShareTheCap) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  store.addTopic("/rc");
  constexpr int kSamples = 5000;
  for (int ch = 0; ch < 6; ++ch) {
    std::vector<std::int64_t> ts;
    std::vector<double> vals;
    ts.reserve(kSamples);
    vals.reserve(kSamples);
    for (int i = 0; i < kSamples; ++i) {
      ts.push_back(static_cast<std::int64_t>(i) * (kSec / 100));  // 100 Hz
      vals.push_back(std::sin(static_cast<double>(i) * 0.01) + static_cast<double>(ch));
    }
    store.addField("/rc", "ch" + std::to_string(ch), std::move(ts), std::move(vals));
  }
  ToolContext ctx = makeCtx(store, nullptr);

  json paths = json::array();
  for (int ch = 0; ch < 6; ++ch) {
    paths.push_back("/rc/ch" + std::to_string(ch));
  }
  auto r = reg.execute("read_series", {{"paths", paths}, {"mode", "buckets"}}, ctx);
  ASSERT_TRUE(r.ok) << r.content;
  EXPECT_LE(r.content.size(), 64u * 1024u) << "batch size: " << r.content.size();
  const json j = json::parse(r.content);
  EXPECT_EQ(j["count"], 6);
  ASSERT_EQ(j["read"].size(), 6u);
  for (const auto& entry : j["read"]) {
    ASSERT_TRUE(entry.contains("buckets")) << entry.dump();
    EXPECT_GE(entry["buckets"]["n"].size(), 16u) << entry.dump();
  }
}

TEST(ToolRegistry, RejectsAnOversizedBatch) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  ToolContext ctx = makeCtx(store, nullptr);

  json many = json::array();
  for (int i = 0; i < 40; ++i) {
    many.push_back("/imu/x");
  }
  auto r = reg.execute("read_series", {{"paths", many}}, ctx);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.content.find("at most"), std::string::npos) << r.content;
}

}  // namespace

// A name that is already taken is not a judgement call: installing over it
// shadows something the user already has, and "create" was not asked to replace
// anything. The tool description used to send the model to list_created "before
// creating something that may already exist" — an instruction it had to
// remember, in place of a check that costs nothing.
TEST(ToolRegistry, RefusesToCreateOverAnExistingName) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);

  const json args = {{"name", "doubled"}, {"inputs", json::array({"/imu/x"})}, {"expression", "value * 2"}};
  ASSERT_TRUE(reg.execute("create_derived_series", args, ctx).ok);
  ASSERT_EQ(dp.liveCount(), 1);

  const auto again = reg.execute("create_derived_series", args, ctx);
  EXPECT_FALSE(again.ok) << "creating the same name twice must not silently install a second one";
  EXPECT_NE(again.content.find("already exists"), std::string::npos) << again.content;
  EXPECT_EQ(dp.liveCount(), 1) << "the refused create must leave the first one untouched";
  EXPECT_EQ(dp.create_calls, 1) << "and must not reach the host at all";
}

// With explicit outputs the node is still addressed by its name: creating "pair" again, with other outputs or
// another expression, would replace it.
TEST(ToolRegistry, RefusesToCreateOverAnExistingNodeWithNamedOutputs) {
  ToolRegistry reg;
  PJ::testing::ToolboxTestStore store;
  populate(store);
  RecordingDpHost dp;
  ToolContext ctx = makeCtx(store, &dp);

  ASSERT_TRUE(reg.execute(
                     "create_derived_series",
                     {{"name", "pair"},
                      {"inputs", json::array({"/imu/x"})},
                      {"outputs", json::array({"x", "y"})},
                      {"body", "return value, -value"}},
                     ctx)
                  .ok);
  const auto again = reg.execute(
      "create_derived_series",
      {{"name", "pair"},
       {"inputs", json::array({"/imu/x"})},
       {"outputs", json::array({"x", "y"})},
       {"body", "return 0, 0"}},
      ctx);
  EXPECT_FALSE(again.ok);
  EXPECT_NE(again.content.find("'pair' already exists"), std::string::npos) << again.content;
  EXPECT_EQ(dp.create_calls, 1) << "the second create must not reach the host";
}

TEST(ToolRegistry, AsyncEvaluationFailureReleasesExactlyOnceAndNamesTheCoverageError) {
  {
    ToolRegistry reg;
    FakeCatalogHost host;
    host.addObjectTopic("/cloud", "kPointCloud", 100, 0, 5 * kSec);
    RecordingDpHost dp;
    dp.pending_polls = 1;
    dp.terminal_state = PJ_EVALUATION_STATE_FAILED;
    dp.canned_report_json =
        R"({"coverage":{"candidates":1,"evaluated":0,"stopped":"error","error":"deliberate failure","gaps":[]},"bundles":[]})";
    FakePlaybackHost playback;
    ToolContext ctx;
    ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
    ctx.dp = dp.view();
    ctx.playback = playback.view();
    auto r = reg.execute(
        "evaluate",
        {{"inputs", {"/cloud"}}, {"at_s", 1.0}, {"body", "return {count=1}"}, {"outputs", {"count:number"}}}, ctx);
    ASSERT_TRUE(r.resume);
    r = r.resume(ctx);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.resume);
    EXPECT_EQ(dp.released_handles.size(), 1u);
    EXPECT_NE(r.content.find("deliberate failure"), std::string::npos) << r.content;
    EXPECT_EQ(r.content.find("coverage"), std::string::npos) << "the reason, not the raw report: " << r.content;
  }
}

TEST(ToolRegistry, ReportStatusAccountsOnlyOwnFindingsAndDegradesExplicitly) {
  ToolRegistry reg;
  FakeCatalogHost host;
  RecordingDpHost dp;
  dp.live_ids = {"finding"};
  dp.canned_config_json = R"({"kind":"on_demand","pinned_t_ns":0,"memory_bytes":123,"state":"ready"})";
  FakePlaybackHost playback;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = playback.view();
  auto result = reg.execute("report_status", {}, ctx);
  ASSERT_TRUE(result.ok);
  auto report = json::parse(result.content);
  EXPECT_EQ(report["findings"]["pinned"], 1);
  EXPECT_EQ(report["findings"]["bytes"], 123);
  EXPECT_EQ(report["processors"][0]["state"], "ready");
  dp.fail_config = true;
  report = json::parse(reg.execute("report_status", {}, ctx).content);
  EXPECT_EQ(report["findings"]["complete"], false);
}

TEST(ToolRegistry, MediaCatalogIncludesCallableOperationsAndFieldOnlyTypes) {
  using PJ::agent_tools::objectOperationsFor;
  using PJ::sdk::BuiltinObjectType;
  for (auto type :
       {BuiltinObjectType::kPointCloud, BuiltinObjectType::kSceneEntities, BuiltinObjectType::kImage,
        BuiltinObjectType::kDepthImage, BuiltinObjectType::kImageAnnotations, BuiltinObjectType::kVideoFrame}) {
    EXPECT_FALSE(objectOperationsFor(type).empty());
  }
  EXPECT_TRUE(objectOperationsFor(BuiltinObjectType::kCameraInfo).empty());
  EXPECT_TRUE(objectOperationsFor(BuiltinObjectType::kFrameTransforms).empty());
  const auto depth = objectOperationsFor(BuiltinObjectType::kImage);
  EXPECT_TRUE(
      std::any_of(depth.begin(), depth.end(), [](const auto& op) { return op.name.starts_with("to_point_cloud("); }));
}

TEST(ToolRegistry, MediaCatalogHasAllEightSdkFieldTables) {
  ToolRegistry reg;
  FakeCatalogHost host;
  for (const auto* type :
       {"kPointCloud", "kSceneEntities", "kFrameTransforms", "kImageAnnotations", "kImage", "kDepthImage",
        "kCameraInfo", "kVideoFrame"}) {
    const std::string topic = std::string("/") + type;
    host.addObjectTopic(topic, type, 1, 0, kSec);
    ToolContext ctx;
    ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
    const auto result = reg.execute("describe_topic", {{"topic", topic}}, ctx);
    ASSERT_TRUE(result.ok) << result.content;
    const auto description = json::parse(result.content);
    ASSERT_TRUE(description.contains("fields")) << description.dump();
    EXPECT_FALSE(description["fields"].empty()) << type << ": rebuild with SDK media field tables";
  }
}

TEST(ToolRegistry, ListTopicsReportsObjectRangesOnTheDisplayAxis) {
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 2, 1000 * kSec, 1001 * kSec);
  FakePlaybackHost playback;
  playback.display_offset_ns = 1000 * kSec;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.playback = playback.view();
  ToolRegistry registry;
  const auto listed = registry.execute("list_topics", json::object(), ctx);
  ASSERT_TRUE(listed.ok);
  EXPECT_EQ(json::parse(listed.content)["topics"][0]["t_range_s"], json::array({0.0, 1.0}));
}

TEST(ToolRegistry, SceneDetachVerifiesDatasetAndTopicTogether) {
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 2, 0, kSec, "{}", "A");
  host.addObjectTopic("/cloud", "kPointCloud", 2, 0, kSec, "{}", "B");
  FakePlotTabsHost scenes;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.plot_tabs = scenes.view();
  ToolRegistry registry;
  ASSERT_TRUE(registry.execute("scene_view", {{"action", "create"}, {"view", "clouds"}}, ctx).ok);
  ASSERT_TRUE(registry
                  .execute(
                      "scene_view",
                      {{"action", "attach"}, {"view", "clouds"}, {"topics", json::array({"A:/cloud", "B:/cloud"})}},
                      ctx)
                  .ok);
  auto result = registry.execute(
      "scene_view", {{"action", "detach"}, {"view", "clouds"}, {"topics", json::array({"A:/cloud"})}}, ctx);
  ASSERT_TRUE(result.ok) << result.content;
  const auto response = json::parse(result.content);
  EXPECT_EQ(response["results"][0]["detached"], true);
  ASSERT_EQ(response["topics"].size(), 1u);
  EXPECT_EQ(response["topics"][0]["dataset"], "B");
}

TEST(ToolRegistry, ObjectEvaluationPreservesQualifiedDatasetAndScriptAliases) {
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 2, 0, kSec, "{}", "A");
  host.addObjectTopic("/cloud", "kPointCloud", 2, 0, kSec, "{}", "B:run");
  RecordingDpHost dp;
  FakePlaybackHost playback;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = playback.view();
  ToolRegistry registry;
  const json args = {
      {"inputs", json::array({"B:run:/cloud"})},
      {"at_s", 0.0},
      {"body", "return {count=inputs[\"/cloud\"]:count()}"},
      {"outputs", json::array({"count:number"})}};
  auto result = registry.execute("evaluate", args, ctx);
  ASSERT_TRUE(result.ok) << result.content;
  EXPECT_EQ(dp.last_inputs, std::vector<std::string>({"B:run:/cloud"}));
  EXPECT_NE(dp.last_script.find("[\"/cloud\"] = inputs[\"B:run:/cloud\"]"), std::string::npos);
  EXPECT_NE(dp.last_script.find("[\"B:run:/cloud\"] = inputs[\"B:run:/cloud\"]"), std::string::npos);
  EXPECT_EQ(playback.last_source_id, 2u);

  auto create_args = args;
  create_args.erase("at_s");
  create_args["name"] = "selected_cloud";
  create_args["pin_at_s"] = 0.0;
  result = registry.execute("create_derived_object", create_args, ctx);
  ASSERT_TRUE(result.ok) << result.content;
  EXPECT_EQ(dp.create_v2_calls, 1);
  EXPECT_EQ(dp.last_create_v2_inputs, std::vector<std::string>({"B:run:/cloud"}));
  EXPECT_NE(dp.last_create_v2_script.find("[\"/cloud\"] = inputs[\"B:run:/cloud\"]"), std::string::npos);
  // The post-create submit names the installed node; its input-free request
  // must not replace the persisted binding. The validation sees its alias map.
  EXPECT_NE(dp.last_validate_script.find("[\"/cloud\"] = inputs[\"B:run:/cloud\"]"), std::string::npos);
}

TEST(ToolRegistry, ObjectEvaluationAndCreationRejectCrossDatasetSelectionBeforeHostMutation) {
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 2, 0, kSec, "{}", "A");
  host.addObjectTopic("/cloud", "kPointCloud", 2, 0, kSec, "{}", "B");
  host.addObjectTopic("/camera", "kImage", 2, 0, kSec, "{}", "B");
  RecordingDpHost dp;
  FakePlaybackHost playback;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = playback.view();
  ToolRegistry registry;
  for (const auto* tool : {"evaluate", "create_derived_object"}) {
    auto result = registry.execute(
        tool,
        {{"name", "wrong_source"},
         {"inputs", json::array({"A:/cloud", "B:/camera"})},
         {"at_s", 0.0},
         {"outputs", json::array({"count:number"})},
         {"body", "return {count=1}"}},
        ctx);
    EXPECT_FALSE(result.ok) << result.content;
    EXPECT_NE(result.content.find("one dataset"), std::string::npos);
  }
  EXPECT_EQ(dp.submit_calls, 0);
  EXPECT_EQ(dp.create_v2_calls, 0);
}

TEST(ToolRegistry, ObjectAliasesCannotOverwriteAnotherQualifiedInput) {
  FakeCatalogHost host;
  host.addObjectTopic("/cloud", "kPointCloud", 2, 0, kSec, "{}", "A");
  host.addObjectTopic("A:/cloud", "kPointCloud", 2, 0, kSec, "{}", "A");
  RecordingDpHost dp;
  FakePlaybackHost playback;
  ToolContext ctx;
  ctx.host = PJ::sdk::ToolboxHostView(host.makeHost());
  ctx.dp = dp.view();
  ctx.playback = playback.view();
  ToolRegistry registry;
  auto result = registry.execute(
      "evaluate",
      {{"inputs", json::array({"A:/cloud", "A:A:/cloud"})},
       {"at_s", 0.0},
       {"outputs", json::array({"count:number"})},
       {"body", "return {count=1}"}},
      ctx);
  EXPECT_FALSE(result.ok) << result.content;
  EXPECT_NE(result.content.find("ambiguous input alias"), std::string::npos);
  EXPECT_EQ(dp.submit_calls, 0);
}
