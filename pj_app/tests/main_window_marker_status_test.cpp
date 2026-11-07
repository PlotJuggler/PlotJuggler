// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A marker rule that is running puts a "computing markers" note on the plots that
// display its markers and nowhere else; the status bar says nothing about it.

#include <gtest/gtest.h>

#include <QApplication>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <atomic>
#include <string>

#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "pj_base/builtin/plot_markers.hpp"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"

using namespace Qt::StringLiterals;

namespace PJ {

class MainWindowMarkerStatusTestPeer {
 public:
  [[nodiscard]] static AppSession& session(MainWindow& window) {
    return *window.session_;
  }

  [[nodiscard]] static TabbedPlotWidget& tabs(MainWindow& window) {
    return *window.findChild<TabbedPlotWidget*>();
  }
};

}  // namespace PJ

namespace {

constexpr const char* kFailsFromFiveScript =
    "local s = series(\"in\")\nif s:at(0).v >= 5 then error(\"value too big\") end\ncreateMarker(0.0)\n";

// A plot of `docker`'s first dock bound to the session, showing the curve of `topic`.
PJ::PlotWidget* plotShowing(PJ::AppSession& app, PJ::PlotDocker* docker, PJ::TopicId topic) {
  if (docker == nullptr || docker->plotCount() == 0) {
    return nullptr;
  }
  PJ::DockWidget* dock = docker->plotAt(0);
  if (dock == nullptr) {
    return nullptr;
  }
  auto* plot = new PJ::PlotWidget(&app.sessionManager(), &app.catalogModel(), dock);
  dock->setPlotWidget(plot);
  for (const auto& curve : app.catalogModel().curves()) {
    const auto descriptor = app.catalogModel().curveDescriptor(curve.name);
    if (descriptor && descriptor->topic_id == topic) {
      return plot->addCurve(curve.name) != nullptr ? plot : nullptr;
    }
  }
  return nullptr;
}

// Two datasets, each shown by its own plot (the second in another tab), and a marker rule that can be applied to
// either. The pool computes can be held so a refresh stays in flight.
class MainWindowMarkerStatusTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    window_ = std::make_unique<PJ::MainWindow>(extensions_dir_.path());
    app_ = &PJ::MainWindowMarkerStatusTestPeer::session(*window_);
    engine_ = &app_->sessionManager().batchEngine();
    dataset_a_ = pj_test::createDataset(*app_, "a");
    dataset_b_ = pj_test::createDataset(*app_, "b");
    const PJ::TopicId topic_a = pj_test::addScalarTopic(*app_, dataset_a_, "/sensor");
    const PJ::TopicId topic_b = pj_test::addScalarTopic(*app_, dataset_b_, "/sensor");
    ASSERT_TRUE(dataset_a_ != 0 && dataset_b_ != 0 && topic_a != 0 && topic_b != 0);
    PJ::TabbedPlotWidget& tabs = PJ::MainWindowMarkerStatusTestPeer::tabs(*window_);
    plot_a_ = plotShowing(*app_, tabs.dockerAt(0), topic_a);
    plot_b_ = plotShowing(*app_, tabs.addTab(u"other"_s), topic_b);
    ASSERT_NE(plot_a_, nullptr);
    ASSERT_NE(plot_b_, nullptr);
    topic_a_ = topic_a;
  }

  void TearDown() override {
    release();
    PJ::BatchEngine::setComputeThreadProbeForTest({});
  }

  PJ::BatchEngine::GeneratorRecipe rule(PJ::DatasetId dataset, std::string id = "test/status-rule") const {
    PJ::BatchEngine::GeneratorRecipe recipe;
    recipe.id = std::move(id);
    recipe.dataset_id = dataset;
    recipe.inputs = {"/sensor/value"};
    recipe.outputs = {std::string(PJ::sdk::kGlobalMarkerTopic)};
    recipe.script = R"(
      local s = series("/sensor/value")
      for i = 0, s:size() - 1 do createMarker(s:at(i).t) end
    )";
    return recipe;
  }

  // Replaces the series resolver by one whose "in" series reads `value` on dataset a and 1 on any other,
  // and makes `recipe` read it: a rule that fails from 5 on.
  PJ::BatchEngine::GeneratorRecipe valueRule(PJ::DatasetId dataset, std::string id = "test/value-rule") {
    engine_->setResolver(
        [this](
            PJ::DatasetId dataset_id, const std::string& key,
            const PJ::BatchEngine::Window&) -> std::optional<PJ::BatchEngine::ResolvedSeries> {
          if (key != "in") {
            return std::nullopt;
          }
          return PJ::BatchEngine::ResolvedSeries{
              {0.0}, {dataset_id == dataset_a_ ? static_cast<double>(value_.load()) : 1.0}};
        });
    PJ::BatchEngine::GeneratorRecipe recipe = rule(dataset, std::move(id));
    recipe.inputs = {"in"};
    recipe.script = kFailsFromFiveScript;
    return recipe;
  }

  void apply(const PJ::BatchEngine::GeneratorRecipe& recipe) {
    const auto created = engine_->upsertGenerator(recipe);
    ASSERT_TRUE(created.has_value()) << (created.has_value() ? std::string{} : created.error());
  }

  // Every compute on the pool waits (up to 5 s) until release(); the GUI thread's passes through.
  void hold() {
    released_ = false;
    QThread* const gui = QThread::currentThread();
    PJ::BatchEngine::setComputeThreadProbeForTest([this, gui](QThread* thread) {
      for (int waited_ms = 0; thread != gui && !released_.load() && waited_ms < 5000; ++waited_ms) {
        QThread::msleep(1);
      }
    });
  }

  void release() {
    released_ = true;
  }

  static bool hasNote(const PJ::PlotWidget* plot) {
    return !plot->markerStatusNote().isEmpty();
  }

  QTemporaryDir extensions_dir_;
  std::unique_ptr<PJ::MainWindow> window_;
  PJ::AppSession* app_ = nullptr;
  PJ::BatchEngine* engine_ = nullptr;
  PJ::DatasetId dataset_a_ = 0;
  PJ::DatasetId dataset_b_ = 0;
  PJ::TopicId topic_a_ = 0;
  PJ::PlotWidget* plot_a_ = nullptr;
  PJ::PlotWidget* plot_b_ = nullptr;
  std::atomic<bool> released_{true};
  std::atomic<int> value_{1};
};

TEST_F(MainWindowMarkerStatusTest, ARunningRuleShowsTheNoteOnThePlotThatDisplaysIt) {
  const PJ::BatchEngine::GeneratorRecipe recipe = rule(dataset_a_);
  apply(recipe);
  hold();
  engine_->refresh(recipe.id);
  ASSERT_TRUE(engine_->isRunning(recipe.id));

  QApplication::processEvents();
  EXPECT_FALSE(hasNote(plot_a_)) << "a quick run never flickers a note";
  EXPECT_TRUE(QTest::qWaitFor([&] { return hasNote(plot_a_); }, 5000));
  EXPECT_FALSE(hasNote(plot_b_)) << "the other plot does not display this rule's markers";
  EXPECT_FALSE(window_->statusBar()->currentMessage().contains(u"omputing"_s)) << "the note replaces the status line";

  release();
  EXPECT_TRUE(QTest::qWaitFor([&] { return !engine_->isRunning(recipe.id); }, 5000));
  EXPECT_TRUE(QTest::qWaitFor([&] { return !hasNote(plot_a_); }, 5000));
}

// One rule applied to both datasets, refreshed on a only: b's plot says nothing, running or failed.
TEST_F(MainWindowMarkerStatusTest, ABindingsStateIsShownOnItsOwnPlotOnly) {
  const PJ::BatchEngine::GeneratorRecipe on_a = valueRule(dataset_a_);
  PJ::BatchEngine::GeneratorRecipe on_b = on_a;
  on_b.dataset_id = dataset_b_;
  apply(on_a);
  apply(on_b);
  hold();
  value_ = 5;
  engine_->refreshAll(dataset_a_);
  ASSERT_TRUE(engine_->isBindingRunning(on_a));
  EXPECT_TRUE(QTest::qWaitFor([&] { return hasNote(plot_a_); }, 5000));
  EXPECT_FALSE(hasNote(plot_b_)) << "b's binding is idle";

  release();
  EXPECT_TRUE(QTest::qWaitFor([&] { return !engine_->isBindingRunning(on_a); }, 5000));
  EXPECT_TRUE(QTest::qWaitFor([&] { return plot_a_->markerStatusNote() == u"Marker rule failed"_s; }, 5000));
  EXPECT_FALSE(hasNote(plot_b_)) << "a's failure is not b's";
}

// A long-running rule must not make a short rule on another plot show its note at once.
TEST_F(MainWindowMarkerStatusTest, TheDelayIsPerBinding) {
  const PJ::BatchEngine::GeneratorRecipe long_rule = rule(dataset_a_, "test/long");
  const PJ::BatchEngine::GeneratorRecipe short_rule = rule(dataset_b_, "test/short");
  apply(long_rule);
  apply(short_rule);
  hold();
  engine_->refresh(long_rule.id);
  ASSERT_TRUE(QTest::qWaitFor([&] { return hasNote(plot_a_); }, 5000)) << "the long rule is past its delay";

  engine_->refresh(short_rule.id);
  ASSERT_TRUE(engine_->isRunning(short_rule.id));
  QApplication::processEvents();
  EXPECT_FALSE(hasNote(plot_b_)) << "the short rule has not run for 300 ms";
  EXPECT_TRUE(QTest::qWaitFor([&] { return hasNote(plot_b_); }, 5000)) << "but it does once it has";
  release();
  EXPECT_TRUE(QTest::qWaitFor([&] { return !hasNote(plot_a_) && !hasNote(plot_b_); }, 5000));
}

// An idle failed rule leaves its note when it is removed, or applied again successfully.
TEST_F(MainWindowMarkerStatusTest, ARemovedOrFixedFailedRuleLeavesNoNote) {
  const PJ::BatchEngine::GeneratorRecipe recipe = valueRule(dataset_a_);
  apply(recipe);
  value_ = 5;
  EXPECT_TRUE(engine_->recomputeForDataset(dataset_a_).empty());
  ASSERT_FALSE(engine_->errorOf(recipe.id).empty());
  EXPECT_TRUE(QTest::qWaitFor([&] { return plot_a_->markerStatusNote() == u"Marker rule failed"_s; }, 5000));

  value_ = 1;
  apply(recipe);
  EXPECT_TRUE(QTest::qWaitFor([&] { return !hasNote(plot_a_); }, 5000)) << "applied again and fixed";

  value_ = 5;
  EXPECT_TRUE(engine_->recomputeForDataset(dataset_a_).empty());
  ASSERT_TRUE(QTest::qWaitFor([&] { return hasNote(plot_a_); }, 5000));
  ASSERT_TRUE(engine_->removeGenerator(recipe.id).has_value());
  EXPECT_TRUE(QTest::qWaitFor([&] { return !hasNote(plot_a_); }, 5000)) << "removed";
}

// A plot gets the standing status when it appears, and loses it when its marker targets stop matching.
TEST_F(MainWindowMarkerStatusTest, APlotsStatusFollowsItsMarkerTargets) {
  const PJ::BatchEngine::GeneratorRecipe recipe = valueRule(dataset_a_);
  apply(recipe);
  value_ = 5;
  EXPECT_TRUE(engine_->recomputeForDataset(dataset_a_).empty());
  ASSERT_TRUE(QTest::qWaitFor([&] { return hasNote(plot_a_); }, 5000));

  PJ::TabbedPlotWidget& tabs = PJ::MainWindowMarkerStatusTestPeer::tabs(*window_);
  PJ::PlotWidget* later = plotShowing(*app_, tabs.addTab(u"later"_s), topic_a_);
  ASSERT_NE(later, nullptr);
  EXPECT_TRUE(QTest::qWaitFor([&] { return hasNote(later); }, 5000)) << "a plot added after the failure";

  plot_a_->setDatasetMarkerScopeVisible(dataset_a_, PJ::MarkerScope::kDataset, false);
  EXPECT_TRUE(QTest::qWaitFor([&] { return !hasNote(plot_a_); }, 5000)) << "its marker scope is hidden";
  plot_a_->setDatasetMarkerScopeVisible(dataset_a_, PJ::MarkerScope::kDataset, true);
  EXPECT_TRUE(QTest::qWaitFor([&] { return hasNote(plot_a_); }, 5000));
  plot_a_->removeAllCurves();
  EXPECT_TRUE(QTest::qWaitFor([&] { return !hasNote(plot_a_); }, 5000)) << "its last curve is removed";
}

}  // namespace
