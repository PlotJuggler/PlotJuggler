// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// SeriesProgressController (SER-17): the replay of an on-demand series is a loading entry of its topic, stoppable but
// never discardable; its end lingers briefly as completed or cancelled, then the entry goes. Through a real AppSession
// whose runs are `test::FakeRun`'s.

#include <gtest/gtest.h>

#include <QHash>
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "SeriesProgressController.h"
#include "event_pump_test_utils.h"
#include "on_demand_session_test_utils.h"
#include "pj_runtime/SeriesRunner.h"

namespace PJ {
namespace {

using Progress = CurveTreeView::DatasetProgress;
using ProgressSet = QHash<quint64, Progress>;

/// `owner/ser`: one number output over the fixture's cloud topic, so a series.
RecipeGraph::Recipe seriesRecipe() {
  return test::onDemandRecipe("ser", {"cloud"}, {{"n", scripting::OutputType::kNumber, sdk::BuiltinObjectType::kNone}});
}

struct Fixture : test::OnDemandSessionFixture {
  Fixture() {
    controller.setLingerMs(10);
    controller.setSessionManager(&manager);
    QObject::connect(
        &controller, &SeriesProgressController::progressUpdated, &controller,
        [this](const ProgressSet& by_topic) { updates.push_back(by_topic); });
  }
  ~Fixture() {
    fake.allow(1 << 20);  // a gated run left waiting must not hold the pool's join
  }
  [[nodiscard]] std::optional<SeriesRunner::State> seriesState() {
    return manager.onDemandService().series().state(recipe.key());
  }
  /// Whether some update carried `topic` in `state`.
  [[nodiscard]] bool sawState(TopicId topic, Progress::State state) const {
    return std::any_of(updates.begin(), updates.end(), [&](const ProgressSet& by_topic) {
      const auto found = by_topic.constFind(topic);
      return found != by_topic.cend() && found->state == state;
    });
  }

  RecipeGraph::Recipe recipe = seriesRecipe();
  SeriesProgressController controller;
  std::vector<ProgressSet> updates;
};

// A requested replay shows as a loading entry (cancellable, not discardable: a stop keeps the rows computed), ends as
// kCompleted, and the entry goes after its linger.
TEST(SeriesProgressControllerTest, ReplayIsALoadingEntryThatLingersWhenDone) {
  Fixture fx;
  fx.install(fx.recipe);
  fx.manager.onDemandService().series().request(fx.recipe.key());
  test::pumpUntil([&] {
    const auto state = fx.seriesState();
    return state && state->complete && !state->replaying && !fx.updates.empty() && fx.updates.back().isEmpty();
  });
  const auto state = fx.seriesState();
  ASSERT_TRUE(state.has_value());
  ASSERT_TRUE(state->complete);
  ASSERT_FALSE(fx.updates.empty());

  const auto first = fx.updates.front().constFind(state->topic);
  ASSERT_NE(first, fx.updates.front().cend()) << "the replay's first update names its topic";
  EXPECT_EQ(first->state, Progress::State::kLoading);
  EXPECT_TRUE(first->cancellable);
  EXPECT_FALSE(first->discardable) << "stopping a series keeps what it computed";
  EXPECT_TRUE(fx.sawState(state->topic, Progress::State::kCompleted));
  EXPECT_TRUE(fx.updates.back().isEmpty()) << "the entry goes after its linger";
}

// A stop by topic ends the replay keeping the rows written: the series is stopped and incomplete, the entry lingers as
// kCancelled and goes, and nothing more is computed.
TEST(SeriesProgressControllerTest, StopKeepsRowsAndEndsTheBar) {
  Fixture fx;
  for (int second = 21; second <= 220; ++second) {
    fx.push(fx.cloud, second * test::kSec);  // more than one chunk of instants
  }
  fx.fake.gated = true;
  fx.install(fx.recipe);
  fx.manager.onDemandService().series().request(fx.recipe.key());
  fx.fake.allow(static_cast<int>(SeriesRunner::kInstantsPerChunk));
  test::pumpUntil([&] {
    const auto state = fx.seriesState();
    return state && state->rows == SeriesRunner::kInstantsPerChunk;
  });
  const auto running = fx.seriesState();
  ASSERT_TRUE(running.has_value());
  ASSERT_EQ(running->rows, SeriesRunner::kInstantsPerChunk) << "the first chunk is written";
  ASSERT_TRUE(running->replaying);
  ASSERT_TRUE(fx.sawState(running->topic, Progress::State::kLoading));

  fx.controller.stopSeriesOfTopic(running->topic);
  const auto stopped = fx.seriesState();
  ASSERT_TRUE(stopped.has_value());
  EXPECT_TRUE(stopped->stopped);
  EXPECT_FALSE(stopped->complete);
  EXPECT_EQ(stopped->rows, SeriesRunner::kInstantsPerChunk);

  test::pumpUntil([&] { return !fx.updates.empty() && fx.updates.back().isEmpty(); });
  EXPECT_TRUE(fx.sawState(running->topic, Progress::State::kCancelled));
  EXPECT_TRUE(fx.updates.back().isEmpty()) << "the entry goes after its linger";
  fx.fake.allow(1 << 20);
  test::pumpFor(50);
  EXPECT_EQ(fx.seriesState()->rows, SeriesRunner::kInstantsPerChunk) << "a stopped run computes nothing more";
}

}  // namespace
}  // namespace PJ
