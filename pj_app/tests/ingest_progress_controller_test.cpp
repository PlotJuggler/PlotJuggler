// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QTimer>
#include <map>

#include "IngestProgressController.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/CurveTreeView.h"

namespace PJ {

// One QCoreApplication for the whole binary. The controller's linger/flash
// timers are the subject of these tests and QTimer::start() is a no-op without
// an event dispatcher on the calling thread, so isActive() would report false
// for a timer the controller did arm. Nothing here builds a widget, so the
// core application (no display needed) is enough.
struct QtEnvironment : ::testing::Environment {
  void SetUp() override {
    static int argc = 0;
    app_ = new QCoreApplication(argc, nullptr);
  }
  void TearDown() override {
    delete app_;
    app_ = nullptr;
  }
  QCoreApplication* app_ = nullptr;
};

// QTimer::timeout is declared with a QPrivateSignal, so no caller outside
// QTimer can emit it. Invoking it by name goes through the meta-object —
// the private tag is not part of the registered signature — which is what
// lets a test advance a controller-owned timer without real elapsed time.
void fireTimer(QTimer* timer) {
  ASSERT_NE(timer, nullptr);
  EXPECT_TRUE(QMetaObject::invokeMethod(timer, "timeout"));
}

class IngestProgressControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    session_ = std::make_unique<SessionManager>();

    // Create timers that we can control manually
    timers_.clear();
    controller_ = std::make_unique<IngestProgressController>(nullptr, [this]() {
      auto timer = new QTimer();
      timers_.push_back(timer);
      return timer;
    });

    controller_->setSessionManager(session_.get());

    // Stand-in for the catalog: tests control which datasets currently have a
    // name, so a dataset can be made to vanish mid-row (the discard case).
    resolved_paths_.clear();
    controller_->setDatasetPathResolver([this](DatasetId dataset_id) -> QString {
      const auto it = resolved_paths_.find(dataset_id);
      return it == resolved_paths_.end() ? QString() : it->second;
    });

    // Connect to signals to capture state changes
    QObject::connect(
        controller_.get(), &IngestProgressController::progressUpdated,
        [this](QHash<quint64, CurveTreeView::DatasetProgress> by_row_key) { onProgressUpdated(by_row_key); });
    QObject::connect(
        controller_.get(), &IngestProgressController::rowKeyChanged,
        [this](QString tree_path, quint64 row_key) { onRowKeyChanged(tree_path, row_key); });
  }

  void TearDown() override {
    // QTimer cleanup handled by controller
    controller_.reset();
    session_.reset();
  }

  void onProgressUpdated(QHash<quint64, CurveTreeView::DatasetProgress> by_row_key) {
    last_progress_ = by_row_key;
    progress_updates_.push_back(by_row_key);
  }

  void onRowKeyChanged(QString tree_path, quint64 row_key) {
    row_keys_[tree_path] = row_key;
  }

  std::unique_ptr<SessionManager> session_;
  std::unique_ptr<IngestProgressController> controller_;
  QHash<quint64, CurveTreeView::DatasetProgress> last_progress_;
  QVector<QHash<quint64, CurveTreeView::DatasetProgress>> progress_updates_;
  QMap<QString, quint64> row_keys_;
  QVector<QTimer*> timers_;
  std::map<DatasetId, QString> resolved_paths_;
};

// Two parallel ingests should remain independent
TEST_F(IngestProgressControllerTest, ParallelIngests) {
  // Start first ingest
  auto token1 = session_->beginIngest(DatasetId(1), "Dataset A", 100, false, [](bool) {});
  EXPECT_EQ(last_progress_.size(), 1);
  auto row_key1 = last_progress_.begin().key();

  // Progress on first
  session_->updateIngest(token1, 50, 100);
  EXPECT_DOUBLE_EQ(last_progress_[row_key1].fraction, 0.5);
  EXPECT_EQ(last_progress_[row_key1].state, CurveTreeView::DatasetProgress::State::kLoading);

  // Start second ingest (parallel)
  auto token2 = session_->beginIngest(DatasetId(2), "Dataset B", 200, false, [](bool) {});
  EXPECT_EQ(last_progress_.size(), 2);

  // Find the row key of the second ingest (not the first one)
  quint64 row_key2 = 0;
  for (auto key : last_progress_.keys()) {
    if (key != row_key1) {
      row_key2 = key;
      break;
    }
  }
  ASSERT_NE(row_key2, 0);

  // Progress on second
  session_->updateIngest(token2, 100, 200);
  EXPECT_DOUBLE_EQ(last_progress_[row_key2].fraction, 0.5);

  // Verify first is still at 50%
  EXPECT_DOUBLE_EQ(last_progress_[row_key1].fraction, 0.5);

  // Complete first
  session_->endIngest(token1, IngestOutcome::kCompleted);
  EXPECT_EQ(last_progress_[row_key1].state, CurveTreeView::DatasetProgress::State::kCompleted);

  // Verify second is still loading
  EXPECT_EQ(last_progress_[row_key2].state, CurveTreeView::DatasetProgress::State::kLoading);
}

// A stale token should be silently ignored
TEST_F(IngestProgressControllerTest, StaleTokenDropped) {
  auto token1 = session_->beginIngest(DatasetId(1), "Dataset A", 100, false, [](bool) {});
  EXPECT_EQ(last_progress_.size(), 1);

  // Restart on same dataset with a new token
  auto token2 = session_->beginIngest(DatasetId(1), "Dataset A (reloaded)", 100, false, [](bool) {});
  EXPECT_EQ(last_progress_.size(), 1);

  // The second token should be in use; first is stale
  session_->updateIngest(token1, 50, 100);
  // Old token should be ignored, no change. Three emissions so far: the first
  // begin, the terminal the restart reports for it, and the second begin.
  EXPECT_EQ(progress_updates_.size(), 3);

  // End with old token should be ignored
  session_->endIngest(token1, IngestOutcome::kCompleted);
  // Still 1 entry and not completed
  EXPECT_EQ(last_progress_.size(), 1);
  EXPECT_EQ(last_progress_.begin()->state, CurveTreeView::DatasetProgress::State::kLoading);

  // Verify the surviving token can be updated
  session_->updateIngest(token2, 25, 100);
  EXPECT_DOUBLE_EQ(last_progress_.begin()->fraction, 0.25);
}

// The bar freezes when the stop is acknowledged, and it is THIS layer that
// freezes it: the runtime keeps reporting the rows the producer really delivers
// until it notices the stop, so a tick arriving mid-stop must be absorbed. The
// row must never fall back to kLoading either, which would un-acknowledge a
// click the user already sees acknowledged.
TEST_F(IngestProgressControllerTest, StoppingRowAbsorbsLateProgressTicks) {
  auto token = session_->beginIngest(DatasetId(1), "Dataset", 100, true, [](bool) {});
  const auto row_key = last_progress_.begin().key();
  session_->updateIngest(token, 40, 100);
  ASSERT_DOUBLE_EQ(last_progress_[row_key].fraction, 0.4);

  ASSERT_TRUE(session_->requestCancel(token, /*keep_partial=*/true));
  ASSERT_EQ(last_progress_[row_key].state, CurveTreeView::DatasetProgress::State::kStopping);

  // The producer has not noticed yet and keeps delivering.
  session_->updateIngest(token, 80, 100);
  EXPECT_EQ(last_progress_[row_key].state, CurveTreeView::DatasetProgress::State::kStopping)
      << "a late tick dragged the stopping row back to loading";
  EXPECT_DOUBLE_EQ(last_progress_[row_key].fraction, 0.4) << "the frozen bar advanced after the stop was acknowledged";
}

// Indeterminate progress (total == 0)
TEST_F(IngestProgressControllerTest, IndeterminateProgress) {
  auto token = session_->beginIngest(DatasetId(1), "Dataset", 0, false, [](bool) {});  // 0 = indeterminate
  EXPECT_TRUE(last_progress_[last_progress_.begin().key()].indeterminate);
  EXPECT_DOUBLE_EQ(last_progress_.begin()->fraction, 0.0);

  session_->updateIngest(token, 10, 0);  // Still indeterminate
  EXPECT_TRUE(last_progress_.begin()->indeterminate);
}

// Completed state produces 1s linger
TEST_F(IngestProgressControllerTest, CompletedLinger) {
  auto token = session_->beginIngest(DatasetId(1), "Dataset", 100, false, [](bool) {});
  auto row_key = last_progress_.begin().key();

  // Progress to 100%
  session_->updateIngest(token, 100, 100);
  EXPECT_DOUBLE_EQ(last_progress_[row_key].fraction, 1.0);

  // End with completed
  session_->endIngest(token, IngestOutcome::kCompleted);
  EXPECT_EQ(last_progress_[row_key].state, CurveTreeView::DatasetProgress::State::kCompleted);
  EXPECT_DOUBLE_EQ(last_progress_[row_key].fraction, 1.0);

  // Timer should be running
  ASSERT_EQ(timers_.size(), 1);
  EXPECT_TRUE(timers_[0]->isActive());

  // Simulate linger timeout
  fireTimer(timers_[0]);

  // Entry should be removed
  EXPECT_EQ(last_progress_.size(), 0);
}

// Failed state produces 3x flash + linger
TEST_F(IngestProgressControllerTest, FailedFlashAndLinger) {
  auto token = session_->beginIngest(DatasetId(1), "Dataset", 100, false, [](bool) {});
  auto row_key = last_progress_.begin().key();

  // End with failed
  session_->endIngest(token, IngestOutcome::kFailed);
  EXPECT_EQ(last_progress_[row_key].state, CurveTreeView::DatasetProgress::State::kFailed);
  EXPECT_TRUE(last_progress_[row_key].flash_on);  // Starts on

  // Timer should be running (flash)
  ASSERT_EQ(timers_.size(), 1);
  EXPECT_TRUE(timers_[0]->isActive());
  auto flash_timer = timers_[0];

  // Flash 6 times (3 on/off cycles). The row enters kFailed already on, so the
  // FIRST timeout toggles it OFF and the bar is on after an EVEN number of
  // toggles. The 6th and last timeout hands over to the linger instead of
  // toggling again, so the row holds the dim half through the linger.
  for (int toggles = 1; toggles <= 6; ++toggles) {
    fireTimer(flash_timer);
    // Entry should still exist during flash
    EXPECT_EQ(last_progress_.size(), 1);
    const bool expected_on = toggles < 6 && (toggles % 2 == 0);
    EXPECT_EQ(last_progress_[row_key].flash_on, expected_on) << "after toggle " << toggles;
  }

  // After 6 flashes, linger timer should start
  ASSERT_EQ(timers_.size(), 1);  // Same timer, reused
  EXPECT_TRUE(timers_[0]->isActive());

  // Simulate linger timeout
  fireTimer(timers_[0]);

  // Entry should be removed
  EXPECT_EQ(last_progress_.size(), 0);
}

// Unknown outcome clears immediately
TEST_F(IngestProgressControllerTest, UnknownOutcomeImmediate) {
  auto token = session_->beginIngest(DatasetId(1), "Dataset", 100, false, [](bool) {});
  EXPECT_EQ(last_progress_.size(), 1);

  // End with unknown
  session_->endIngest(token, IngestOutcome::kUnknown);

  // Entry should be removed immediately, no timers
  EXPECT_EQ(last_progress_.size(), 0);
  EXPECT_EQ(timers_.size(), 0);  // No timer created
}

// A row that finished must stay finished: the entry outlives its terminal for
// the completion linger, and a progress tick landing in that window used to
// replace the full bar with the tick's fraction (a finished reload redrawing at
// ~70%).
TEST_F(IngestProgressControllerTest, LateProgressCannotResurrectAFinishedRow) {
  auto token = session_->beginIngest(DatasetId(1), "Dataset", 100, false, {});
  const auto row_key = last_progress_.begin().key();
  session_->updateIngest(token, 70, 100);
  EXPECT_DOUBLE_EQ(last_progress_[row_key].fraction, 0.7);

  session_->endIngest(token, IngestOutcome::kCompleted);
  ASSERT_EQ(last_progress_[row_key].state, CurveTreeView::DatasetProgress::State::kCompleted);
  EXPECT_DOUBLE_EQ(last_progress_[row_key].fraction, 1.0);

  // A tick that raced the terminal must be ignored, not honoured. Emitted
  // straight from the session so this exercises the production signal path.
  emit session_->ingestProgressed(token, 70, 100);
  EXPECT_EQ(last_progress_[row_key].state, CurveTreeView::DatasetProgress::State::kCompleted)
      << "a finished row was dragged back to loading";
  EXPECT_DOUBLE_EQ(last_progress_[row_key].fraction, 1.0) << "the finished bar lost its 100%";
}

// A cancelled row retires AT ONCE: the user stopped the load so they already
// know the outcome, and a discard has deleted the dataset the row describes.
TEST_F(IngestProgressControllerTest, CancelledRetiresImmediatelyWithoutLinger) {
  bool cancel_called = false;
  auto token = session_->beginIngest(DatasetId(1), "Dataset", 100, true, [&](bool) { cancel_called = true; });
  auto row_key = last_progress_.begin().key();

  // Request cancellation through the SESSION, which is the production route:
  // the view's affordance reaches SessionManager::requestCancel via
  // wireIngestProgress, never through the controller.
  EXPECT_TRUE(session_->requestCancel(token, /*keep_partial=*/false));
  EXPECT_TRUE(cancel_called);

  // End (producer might report kCompleted, but cancel intent overrides)
  session_->endIngest(token, IngestOutcome::kCompleted);

  EXPECT_EQ(last_progress_.size(), 0) << "a cancelled row must not survive its terminal";
  EXPECT_FALSE(last_progress_.contains(row_key));
  EXPECT_EQ(timers_.size(), 0) << "no linger timer may be armed for a cancellation";
}

// The session announces a superseded run's terminal synchronously, and an
// observer may restart the dataset from inside it. What must survive here is
// exactly the run that is actually live: a began arriving after its successor's
// retires the live row (same dataset) and leaves an orphan nothing will retire.
TEST_F(IngestProgressControllerTest, ReentrantRestartLeavesOnlyTheSurvivingRow) {
  (void)session_->beginIngest(DatasetId(1), "first", 100, false, [](bool) {});

  IngestToken third{};
  bool restarted = false;
  QObject::connect(session_.get(), &SessionManager::ingestEnded, session_.get(), [&](IngestToken, IngestOutcome) {
    if (!restarted) {
      restarted = true;
      third = session_->beginIngest(DatasetId(1), "third", 100, false, [](bool) {});
    }
  });

  const IngestToken second = session_->beginIngest(DatasetId(1), "second", 100, false, [](bool) {});
  ASSERT_TRUE(restarted);

  ASSERT_EQ(last_progress_.size(), 1) << "one dataset, one row";
  EXPECT_TRUE(last_progress_.contains(third.id)) << "the live run lost its row";
  EXPECT_FALSE(last_progress_.contains(second.id)) << "an orphan row for a run that is already over";
  EXPECT_EQ(last_progress_[third.id].display_name, QStringLiteral("third"));
}

// A kStopOnly producer's row must SAY it cannot discard: the view greys the bin
// from this flag, and the alternative — an enabled bin whose click the runtime
// then refuses — offers the user an action that silently does nothing.
TEST_F(IngestProgressControllerTest, StopOnlyIngestPaintsALiveStopAndAnInertDiscard) {
  auto token = session_->beginIngest(DatasetId(1), "Importing", 100, true, [](bool) {}, IngestStop::kStopOnly);
  const auto row_key = last_progress_.begin().key();
  EXPECT_TRUE(last_progress_[row_key].cancellable) << "the row must still offer a stop";
  EXPECT_FALSE(last_progress_[row_key].discardable) << "the bin must be presented as unavailable";

  // The refused click leaves the row exactly as it was: no stop was accepted,
  // so acknowledging one would be a lie about a load that is still running.
  EXPECT_FALSE(session_->requestCancel(token, /*keep_partial=*/false));
  EXPECT_EQ(last_progress_[row_key].state, CurveTreeView::DatasetProgress::State::kLoading);

  // The ✕ works: kStopOnly restricts the choice, not the stop itself.
  EXPECT_TRUE(session_->requestCancel(token, /*keep_partial=*/true));
  EXPECT_EQ(last_progress_[row_key].state, CurveTreeView::DatasetProgress::State::kStopping);
}

// The row is named after the DATASET, not the producer's progress title, and
// keeps that name once adopted — a discard deletes the dataset while the row is
// still on screen, and a row that renames itself mid-flight reads as a
// different row.
TEST_F(IngestProgressControllerTest, RowAdoptsDatasetNameAndKeepsItWhenTheDatasetVanishes) {
  resolved_paths_[DatasetId(1)] = QStringLiteral("costmap.zstd.mcap");
  auto token = session_->beginIngest(DatasetId(1), "Importing MCAP", 100, true, [](bool) {});
  const auto row_key = last_progress_.begin().key();
  EXPECT_EQ(last_progress_[row_key].display_name, QStringLiteral("costmap.zstd.mcap"))
      << "the dataset name must win over the producer's progress title";

  // The dataset is deleted (discard) while the row is still live.
  resolved_paths_.erase(DatasetId(1));
  session_->updateIngest(token, 50, 100);
  EXPECT_EQ(last_progress_[row_key].display_name, QStringLiteral("costmap.zstd.mcap"))
      << "the adopted name must survive the dataset's removal";
}

// A dataset's catalog row appears partway through its load, or — for a short
// one — only after the last progress tick. Until the entry resolves onto it the
// row is a GHOST, and a ghost that outlives the appearance of the real row
// shadows it: two rows for one dataset, the phantom carrying the producer's
// progress title. It must convert as soon as the dataset has a row.
TEST_F(IngestProgressControllerTest, GhostConvertsToTheDatasetRowAsSoonAsOneExists) {
  (void)session_->beginIngest(DatasetId(1), "Importing MCAP", 100, false, [](bool) {});
  const auto row_key = last_progress_.begin().key();
  ASSERT_EQ(last_progress_[row_key].display_name, QStringLiteral("Importing MCAP")) << "a ghost until a row exists";
  ASSERT_FALSE(row_keys_.contains(QStringLiteral("recording.mcap"))) << "nothing to file it under yet";

  // The dataset materializes in the catalog mid-load.
  resolved_paths_[DatasetId(1)] = QStringLiteral("recording.mcap");
  controller_->refreshDatasetPaths();

  EXPECT_EQ(last_progress_[row_key].display_name, QStringLiteral("recording.mcap"))
      << "the row must adopt the dataset it belongs to";
  EXPECT_EQ(row_keys_.value(QStringLiteral("recording.mcap")), row_key)
      << "and be re-filed onto that dataset's tree row";
}

// A short load can commit without a single progress tick after its catalog row
// appears. The terminal must still land on the real row: otherwise the whole
// completion linger is spent as a phantom row above the dataset it describes.
TEST_F(IngestProgressControllerTest, TerminalResolvesTheRowInsteadOfLingeringAsAGhost) {
  auto token = session_->beginIngest(DatasetId(1), "Importing MCAP", 100, false, [](bool) {});
  const auto row_key = last_progress_.begin().key();
  resolved_paths_[DatasetId(1)] = QStringLiteral("recording.mcap");

  session_->endIngest(token, IngestOutcome::kCompleted);

  EXPECT_EQ(last_progress_[row_key].display_name, QStringLiteral("recording.mcap"));
  EXPECT_EQ(row_keys_.value(QStringLiteral("recording.mcap")), row_key);
  EXPECT_EQ(last_progress_[row_key].state, CurveTreeView::DatasetProgress::State::kCompleted);
}

// The catalog can relabel a dataset mid-ingest: removing a sibling turns
// "foo (2)" back into "foo", moving the tree row out from under the entry. The
// row must follow it, not stay filed under a path that no longer exists.
TEST_F(IngestProgressControllerTest, RowFollowsAMidIngestCatalogRelabel) {
  resolved_paths_[DatasetId(1)] = QStringLiteral("foo (2)");
  const auto token = session_->beginIngest(DatasetId(1), "Importing", 100, false, [](bool) {});
  const auto row_key = last_progress_.begin().key();
  ASSERT_EQ(row_keys_.value(QStringLiteral("foo (2)")), row_key);

  resolved_paths_[DatasetId(1)] = QStringLiteral("foo");
  session_->updateIngest(token, 50, 100);

  EXPECT_EQ(row_keys_.value(QStringLiteral("foo")), row_key) << "the row must re-file under the new label";
  EXPECT_EQ(last_progress_[row_key].display_name, QStringLiteral("foo"));
}

// ...but a dataset that VANISHES under a live row (a discard deletes it) must
// not demote the row back to a ghost on its way out: it keeps its adopted name
// and its place until it retires.
TEST_F(IngestProgressControllerTest, RowKeepsItsPlaceWhenTheDatasetVanishesMidIngest) {
  resolved_paths_[DatasetId(1)] = QStringLiteral("costmap.mcap");
  const auto token = session_->beginIngest(DatasetId(1), "Importing", 100, false, [](bool) {});
  const auto row_key = last_progress_.begin().key();
  ASSERT_EQ(row_keys_.value(QStringLiteral("costmap.mcap")), row_key);

  resolved_paths_.erase(DatasetId(1));
  session_->updateIngest(token, 50, 100);

  EXPECT_EQ(last_progress_[row_key].display_name, QStringLiteral("costmap.mcap"));
  EXPECT_EQ(row_keys_.value(QStringLiteral("costmap.mcap")), row_key) << "the row must not re-file onto a ghost";
}

// Ghost row (entry without matching catalog node)
TEST_F(IngestProgressControllerTest, GhostRowCreatedAndReaped) {
  auto token = session_->beginIngest(DatasetId(1), "My Dataset", 100, false, [](bool) {});
  auto row_key = last_progress_.begin().key();

  // The entry has a display_name and can serve as a ghost
  EXPECT_EQ(last_progress_[row_key].display_name, "My Dataset");

  // End with failed to test ghost animation + removal
  session_->endIngest(token, IngestOutcome::kFailed);

  // Ghost is still there, animating
  EXPECT_EQ(last_progress_.size(), 1);
  EXPECT_EQ(last_progress_[row_key].state, CurveTreeView::DatasetProgress::State::kFailed);

  // Flash 6 times
  for (int i = 0; i < 6; ++i) {
    fireTimer(timers_[0]);
  }

  // Now in linger
  EXPECT_EQ(last_progress_.size(), 1);

  // Linger timeout removes ghost
  fireTimer(timers_[0]);
  EXPECT_EQ(last_progress_.size(), 0);
}

}  // namespace PJ

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::AddGlobalTestEnvironment(new PJ::QtEnvironment);
  return RUN_ALL_TESTS();
}
