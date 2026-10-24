// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// What the AppSession-level on-demand tests (the cursor, the live edge) share: a real AppSession whose scheduler runs
// `test::FakeRun`, one dataset with a point-cloud topic, and readers of the ring.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QTemporaryDir>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "on_demand_test_utils.h"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/EnginePool.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/TopicDemandTracker.h"

namespace PJ::test {

inline constexpr Timestamp kSec = 1'000'000'000;
inline constexpr const char* kCloudMetadata = R"({"builtin_object_type":"kPointCloud"})";

inline scripting::OutputSpec cloudSpec() {
  return {"cropped", scripting::OutputType::kObject, sdk::BuiltinObjectType::kPointCloud};
}

inline std::vector<uint8_t> canonicalCloud(size_t points) {
  return serializeBuiltinObject(sdk::BuiltinObject(makeCloud(points))).value();
}

/// Freezes the on-demand time budgets' clock of every service built while it lives
/// (`OnDemandService::setClockForTest`): the tests count instants, and a slow host must not end a chunk early.
struct FrozenBudgetClock {
  FrozenBudgetClock() {
    OnDemandService::setClockForTest([] { return std::chrono::steady_clock::time_point{}; });
  }
  ~FrozenBudgetClock() {
    OnDemandService::setClockForTest({});
  }
  FrozenBudgetClock(const FrozenBudgetClock&) = delete;
  FrozenBudgetClock& operator=(const FrozenBudgetClock&) = delete;
};

/// A real AppSession whose on-demand runs are `fake`'s, with one dataset `log` holding the cloud topic `cloud` (entries
/// at 5, 10 and 20 s).
struct OnDemandSessionFixture {
  OnDemandSessionFixture() {
    ds = addDataset("log");
    cloud = addCloudTopic(ds, "cloud", {5 * kSec, 10 * kSec, 20 * kSec});
  }

  DatasetId addDataset(const std::string& name, Timestamp display_offset = 0) {
    auto domain = manager.dataEngine().createTimeDomain(name + "_domain");
    EXPECT_TRUE(domain.has_value());
    manager.dataEngine().setDisplayOffset(*domain, display_offset);
    return manager.dataEngine()
        .createDataset(DatasetDescriptor{.source_name = name, .time_domain_id = *domain})
        .value();
  }
  ObjectTopicId addCloudTopic(DatasetId dataset, const std::string& name, const std::vector<Timestamp>& stamps) {
    const ObjectTopicId topic = manager.objectStore().registerTopic({dataset, name, kCloudMetadata}).value();
    for (const Timestamp t : stamps) {
      EXPECT_TRUE(manager.objectStore().pushOwned(topic, t, canonicalCloud(4)).has_value());
    }
    session.catalogModel().rebuildFromDatastore();
    return topic;
  }
  /// Appends a `points`-point cloud to `topic` at `t`.
  void push(ObjectTopicId topic, Timestamp t, size_t points = 4) {
    EXPECT_TRUE(manager.objectStore().pushOwned(topic, t, canonicalCloud(points)).has_value());
  }
  void install(RecipeGraph::Recipe recipe) {
    const auto installed = manager.onDemandService().install(std::move(recipe));
    ASSERT_TRUE(installed.has_value()) << installed.error();
  }
  void display(const std::string& topic, DatasetId dataset = 0) {
    session.topicDemandTracker().addReference(dataset != 0 ? dataset : ds, QString::fromStdString(topic));
  }
  void hide(const std::string& topic, DatasetId dataset = 0) {
    session.topicDemandTracker().removeReference(dataset != 0 ? dataset : ds, QString::fromStdString(topic));
  }
  /// Moves the playhead to the display time of raw `t` in `ds`, without waiting.
  void seekNoWait(Timestamp t) {
    const DisplayOffset offset = manager.displayOffset(ds);
    session.playbackEngine().setRangeAndCurrentTime(
        DisplayRange{rawToDisplaySeconds(0, offset), rawToDisplaySeconds(100 * kSec, offset)},
        rawToDisplaySeconds(t, offset));
  }
  void seek(Timestamp t) {
    seekNoWait(t);
    settle();
  }
  /// Runs what is posted, then publishes everything submitted.
  void settle() {
    QCoreApplication::processEvents();
    manager.enginePool().waitIdle();
  }
  [[nodiscard]] std::optional<NodeScheduler::NodeState> state(const std::string& key) const {
    return manager.onDemandService().state(key);
  }
  [[nodiscard]] ObjectTopicId topic(const std::string& name, DatasetId dataset = 0) const {
    return manager.objectStore().findTopic(dataset != 0 ? dataset : ds, name).value_or(ObjectTopicId{});
  }
  [[nodiscard]] std::vector<Timestamp> ring(const std::string& name, DatasetId dataset = 0) const {
    return timestampsOf(manager.objectStore(), topic(name, dataset));
  }
  [[nodiscard]] sdk::PointCloud shown(const std::string& name, Timestamp t) const {
    return decodeAt<sdk::PointCloud>(manager.objectStore(), topic(name), t);
  }

  FakeRun fake;  ///< outlives the session, whose pool joins the runs
  ScopedRun scoped_run{fake};
  FrozenBudgetClock frozen_clock;
  QTemporaryDir dir;
  AppSession session{dir.path()};
  SessionManager& manager = session.sessionManager();
  DatasetId ds = 0;
  ObjectTopicId cloud;
};

}  // namespace PJ::test
