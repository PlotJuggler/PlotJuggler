// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <optional>
#include <string>
#include <vector>

#include "pj_datastore/engine.hpp"
#include "pj_runtime/SessionManager.h"

namespace PJ::test {

class SessionManagerIngestCancelTest : public ::testing::Test {
 protected:
  // Mints a dataset in the session's own engine. Every ingest case needs one,
  // and createDataset() reports failure rather than throwing, so a storage
  // failure surfaces here instead of as a confusing ingest assertion later.
  DatasetId makeDataset(const char* source_name = "ingest.mcap") {
    const auto dataset = manager_.dataEngine().createDataset(DatasetDescriptor{.source_name = source_name});
    EXPECT_TRUE(dataset.has_value());
    return dataset.has_value() ? *dataset : DatasetId{};
  }

  SessionManager manager_;
};

TEST_F(SessionManagerIngestCancelTest, BeginIngestReturnsValidToken) {
  const DatasetId dataset_id = makeDataset();

  IngestCancelFn cancel_fn = [](bool) {};
  const IngestToken token = manager_.beginIngest(dataset_id, "test_label", 100, /*cancellable=*/true, cancel_fn);

  EXPECT_NE(token.id, 0);
  EXPECT_EQ(token.dataset_id, dataset_id);
  EXPECT_TRUE(manager_.hasActiveIngests());
}

TEST_F(SessionManagerIngestCancelTest, IngestBeganSignalEmitted) {
  const DatasetId dataset_id = makeDataset();
  QSignalSpy spy(&manager_, &SessionManager::ingestBegan);

  IngestCancelFn cancel_fn = [](bool) {};
  const IngestToken token = manager_.beginIngest(dataset_id, "test_label", 100, /*cancellable=*/true, cancel_fn);

  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(spy[0][0].value<IngestToken>().id, token.id);
}

TEST_F(SessionManagerIngestCancelTest, UpdateIngestSignal) {
  const DatasetId dataset_id = makeDataset();
  QSignalSpy spy(&manager_, &SessionManager::ingestProgressed);

  IngestCancelFn cancel_fn = [](bool) {};
  const IngestToken token = manager_.beginIngest(dataset_id, "test_label", 100, /*cancellable=*/true, cancel_fn);

  manager_.updateIngest(token, 50, 100);

  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(spy[0][0].value<IngestToken>().id, token.id);
  EXPECT_EQ(spy[0][1].toULongLong(), 50);
  EXPECT_EQ(spy[0][2].toULongLong(), 100);
}

TEST_F(SessionManagerIngestCancelTest, EndIngestSignal) {
  const DatasetId dataset_id = makeDataset();
  QSignalSpy spy(&manager_, &SessionManager::ingestEnded);

  IngestCancelFn cancel_fn = [](bool) {};
  const IngestToken token = manager_.beginIngest(dataset_id, "test_label", 100, /*cancellable=*/true, cancel_fn);

  manager_.endIngest(token, IngestOutcome::kCompleted);

  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(spy[0][0].value<IngestToken>().id, token.id);
  EXPECT_EQ(spy[0][1].value<IngestOutcome>(), IngestOutcome::kCompleted);
  EXPECT_FALSE(manager_.hasActiveIngests());
}

TEST_F(SessionManagerIngestCancelTest, StaleTokenIsNoOp) {
  const DatasetId dataset_id = makeDataset();
  QSignalSpy began_spy(&manager_, &SessionManager::ingestBegan);
  QSignalSpy progress_spy(&manager_, &SessionManager::ingestProgressed);
  QSignalSpy end_spy(&manager_, &SessionManager::ingestEnded);

  IngestCancelFn cancel_fn1 = [](bool) {};
  const IngestToken token1 = manager_.beginIngest(dataset_id, "label1", 100, /*cancellable=*/true, cancel_fn1);
  EXPECT_EQ(began_spy.count(), 1);

  // Start a new ingest on the same dataset, invalidating the first token. The
  // superseded run gets a real terminal, so observers never have to infer the
  // supersede from the began that follows it.
  IngestCancelFn cancel_fn2 = [](bool) {};
  const IngestToken token2 = manager_.beginIngest(dataset_id, "label2", 100, /*cancellable=*/true, cancel_fn2);
  EXPECT_EQ(began_spy.count(), 2);
  ASSERT_EQ(end_spy.count(), 1) << "superseding a live ingest must report its terminal";
  EXPECT_EQ(end_spy[0][0].value<IngestToken>().id, token1.id);
  EXPECT_EQ(end_spy[0][1].value<IngestOutcome>(), IngestOutcome::kCancelled);

  // Old token's updateIngest should be silently ignored.
  manager_.updateIngest(token1, 50, 100);
  EXPECT_EQ(progress_spy.count(), 0);  // No new signal.

  // Old token's endIngest should be silently ignored.
  manager_.endIngest(token1, IngestOutcome::kCompleted);
  EXPECT_EQ(end_spy.count(), 1);             // Still just the supersede terminal.
  EXPECT_TRUE(manager_.hasActiveIngests());  // New ingest still active.

  // New token's signals should work.
  manager_.updateIngest(token2, 50, 100);
  EXPECT_EQ(progress_spy.count(), 1);

  manager_.endIngest(token2, IngestOutcome::kCompleted);
  EXPECT_EQ(end_spy.count(), 2);
  EXPECT_FALSE(manager_.hasActiveIngests());
}

TEST_F(SessionManagerIngestCancelTest, RequestCancelInvokesHandler) {
  const DatasetId dataset_id = makeDataset();

  bool cancel_called = false;
  bool keep_partial_value = false;
  IngestCancelFn cancel_fn = [&](bool keep_partial) {
    cancel_called = true;
    keep_partial_value = keep_partial;
  };

  const IngestToken token = manager_.beginIngest(dataset_id, "label", 100, /*cancellable=*/true, cancel_fn);

  const bool result = manager_.requestCancel(token, /*keep_partial=*/true);

  EXPECT_TRUE(result);
  EXPECT_TRUE(cancel_called);
  EXPECT_TRUE(keep_partial_value);
}

TEST_F(SessionManagerIngestCancelTest, RequestCancelReturnsFailureForNonCancellable) {
  const DatasetId dataset_id = makeDataset();

  IngestCancelFn cancel_fn = [](bool) {};
  const IngestToken token = manager_.beginIngest(dataset_id, "label", 100, /*cancellable=*/false, cancel_fn);

  const bool result = manager_.requestCancel(token, /*keep_partial=*/true);

  EXPECT_FALSE(result);
}

TEST_F(SessionManagerIngestCancelTest, RequestCancelReturnsFailureForStaleToken) {
  const DatasetId dataset_id = makeDataset();

  IngestCancelFn cancel_fn1 = [](bool) {};
  const IngestToken token1 = manager_.beginIngest(dataset_id, "label1", 100, /*cancellable=*/true, cancel_fn1);

  // Start a new ingest, invalidating token1.
  IngestCancelFn cancel_fn2 = [](bool) {};
  manager_.beginIngest(dataset_id, "label2", 100, /*cancellable=*/true, cancel_fn2);

  // Attempt to cancel with the old token should fail.
  const bool result = manager_.requestCancel(token1, /*keep_partial=*/true);

  EXPECT_FALSE(result);
}

TEST_F(SessionManagerIngestCancelTest, CancelAfterTerminalIsIgnored) {
  const DatasetId dataset_id = makeDataset();
  QSignalSpy end_spy(&manager_, &SessionManager::ingestEnded);

  IngestCancelFn cancel_fn = [](bool) {};
  const IngestToken token = manager_.beginIngest(dataset_id, "label", 100, /*cancellable=*/true, cancel_fn);

  manager_.endIngest(token, IngestOutcome::kCompleted);
  EXPECT_EQ(end_spy.count(), 1);

  // Attempt to cancel after the terminal should fail.
  const bool result = manager_.requestCancel(token, /*keep_partial=*/true);
  EXPECT_FALSE(result);

  // No additional signal should be emitted.
  EXPECT_EQ(end_spy.count(), 1);
}

TEST_F(SessionManagerIngestCancelTest, CancelRecordsIntentForTerminal) {
  const DatasetId dataset_id = makeDataset();
  QSignalSpy end_spy(&manager_, &SessionManager::ingestEnded);

  IngestCancelFn cancel_fn = [](bool) {};
  const IngestToken token = manager_.beginIngest(dataset_id, "label", 100, /*cancellable=*/true, cancel_fn);

  // Request cancellation.
  const bool cancel_result = manager_.requestCancel(token, /*keep_partial=*/true);
  EXPECT_TRUE(cancel_result);

  // End the ingest with kCompleted, but since cancel was requested, it should
  // report kCancelled instead.
  manager_.endIngest(token, IngestOutcome::kCompleted);

  EXPECT_EQ(end_spy.count(), 1);
  EXPECT_EQ(end_spy[0][1].value<IngestOutcome>(), IngestOutcome::kCancelled);
}

// Progress reporting is FACTUAL: the producer goes on delivering rows until it
// notices a cooperative stop, and the runtime reports every one of them. Whether
// a stopping row's bar should freeze is a presentation policy and lives in
// IngestProgressController; suppressing the signal here would instead hide from
// every other observer that data was still arriving.
TEST_F(SessionManagerIngestCancelTest, ProgressKeepsBeingReportedAfterAStopIsRequested) {
  const DatasetId dataset_id = makeDataset();
  QSignalSpy progress_spy(&manager_, &SessionManager::ingestProgressed);

  IngestCancelFn cancel_fn = [](bool) {};
  const IngestToken token = manager_.beginIngest(dataset_id, "label", 100, /*cancellable=*/true, cancel_fn);
  ASSERT_TRUE(manager_.requestCancel(token, /*keep_partial=*/true));

  manager_.updateIngest(token, 60, 100);
  ASSERT_EQ(progress_spy.count(), 1) << "a stop request must not silence the producer's real progress";
  EXPECT_EQ(progress_spy[0][1].toULongLong(), 60);
  EXPECT_EQ(manager_.activeIngests().at(dataset_id).current, 60U) << "the recorded state must track it too";
}

// requestCancel emits ingestStopping synchronously, and an observer is entitled
// to terminalize the ingest from inside it (the row's stop is exactly the moment
// a producer that stops instantly reports its end). That erases the map element
// the call was holding, so everything after the emit must already have what it
// needs — including the producer's cancel handler.
TEST_F(SessionManagerIngestCancelTest, CancelSurvivesAnObserverEndingTheIngestDuringStopping) {
  const DatasetId dataset_id = makeDataset();
  bool handler_ran = false;
  const IngestToken token = manager_.beginIngest(
      dataset_id, "label", 100, /*cancellable=*/true, [&handler_ran](bool) { handler_ran = true; });

  int ended = 0;
  QObject::connect(&manager_, &SessionManager::ingestStopping, &manager_, [&](IngestToken stopping, bool) {
    manager_.endIngest(stopping, IngestOutcome::kCompleted);  // re-entrant terminal
  });
  QObject::connect(&manager_, &SessionManager::ingestEnded, &manager_, [&](IngestToken, IngestOutcome) { ++ended; });

  EXPECT_TRUE(manager_.requestCancel(token, /*keep_partial=*/true));
  EXPECT_EQ(ended, 1) << "the observer's terminal must land exactly once";
  EXPECT_TRUE(handler_ran) << "the producer's handler must still run after the entry is gone";
  EXPECT_FALSE(manager_.hasActiveIngests());
}

// Same signal, the other re-entrant move: an observer restarts the dataset.
// The restart supersedes the entry mid-call, so the tail must not touch it.
TEST_F(SessionManagerIngestCancelTest, CancelSurvivesAnObserverRestartingTheIngestDuringStopping) {
  const DatasetId dataset_id = makeDataset();
  const IngestToken token = manager_.beginIngest(dataset_id, "first", 100, /*cancellable=*/true, [](bool) {});

  bool restarted = false;
  QObject::connect(&manager_, &SessionManager::ingestStopping, &manager_, [&](IngestToken, bool) {
    if (!restarted) {
      restarted = true;
      (void)manager_.beginIngest(dataset_id, "second", 100, /*cancellable=*/true, [](bool) {});
    }
  });

  EXPECT_TRUE(manager_.requestCancel(token, /*keep_partial=*/true));
  ASSERT_TRUE(restarted);
  EXPECT_TRUE(manager_.hasActiveIngests()) << "the restart's ingest must survive the cancel it interrupted";
  EXPECT_EQ(manager_.activeIngests().at(dataset_id).label, QStringLiteral("second"));
}

// The supersede terminal is emitted synchronously from beginIngest. An observer
// reading the session back during it must see this dataset's lifecycle as
// continuous — the replacement already installed — not as a gap.
TEST_F(SessionManagerIngestCancelTest, SupersededTerminalObserverSeesTheReplacementAlreadyLive) {
  const DatasetId dataset_id = makeDataset();
  const IngestToken first = manager_.beginIngest(dataset_id, "first", 100, /*cancellable=*/false, {});

  bool observed_active = false;
  QString observed_label;
  IngestToken observed_token{};
  QObject::connect(&manager_, &SessionManager::ingestEnded, &manager_, [&](IngestToken ended, IngestOutcome) {
    observed_token = ended;
    observed_active = manager_.ingestActive(dataset_id);
    const auto live = manager_.activeIngests();
    observed_label = live.count(dataset_id) != 0 ? live.at(dataset_id).label : QString();
  });

  const IngestToken second = manager_.beginIngest(dataset_id, "second", 100, /*cancellable=*/false, {});

  EXPECT_EQ(observed_token.id, first.id) << "the terminal names the run that was superseded";
  EXPECT_TRUE(observed_active) << "the dataset must never look idle between the two runs";
  EXPECT_EQ(observed_label, QStringLiteral("second"));
  // The stale token still resolves to nothing, so late traffic on it no-ops.
  manager_.updateIngest(first, 10, 100);
  EXPECT_EQ(manager_.activeIngests().at(dataset_id).current, 0U);
  EXPECT_EQ(manager_.activeIngests().at(dataset_id).token.id, second.id);
}

// The supersede terminal is synchronous, so an observer can begin a THIRD ingest
// on the dataset from inside it. That successor supersedes the entry the outer
// call installed but had not announced yet — and announcing it afterwards would
// put a began AFTER the began of the token that replaced it. Every observer keyed
// by dataset would then bind its row to a token that is already dead.
TEST_F(SessionManagerIngestCancelTest, SupersededBeginSuppressesItsOwnStaleBegan) {
  const DatasetId dataset_id = makeDataset();

  std::vector<std::string> events;
  QObject::connect(&manager_, &SessionManager::ingestBegan, &manager_, [&events](IngestToken token, const QString&) {
    events.push_back("began:" + std::to_string(token.id));
  });
  QObject::connect(&manager_, &SessionManager::ingestEnded, &manager_, [&events](IngestToken token, IngestOutcome) {
    events.push_back("ended:" + std::to_string(token.id));
  });

  const IngestToken first = manager_.beginIngest(dataset_id, "first", 100, /*cancellable=*/false, {});

  // The observer of first's terminal restarts the dataset, superseding the run
  // that is emitting that very terminal.
  IngestToken third{};
  bool restarted = false;
  QObject::connect(&manager_, &SessionManager::ingestEnded, &manager_, [&](IngestToken, IngestOutcome) {
    if (!restarted) {
      restarted = true;
      third = manager_.beginIngest(dataset_id, "third", 100, /*cancellable=*/false, {});
    }
  });

  const IngestToken second = manager_.beginIngest(dataset_id, "second", 100, /*cancellable=*/false, {});
  ASSERT_TRUE(restarted);

  const std::vector<std::string> expected{
      "began:" + std::to_string(first.id), "ended:" + std::to_string(first.id), "ended:" + std::to_string(second.id),
      "began:" + std::to_string(third.id)};
  EXPECT_EQ(events, expected) << "a superseded run must not announce itself after its successor did";
  EXPECT_EQ(manager_.activeIngests().at(dataset_id).token.id, third.id);
  // The suppressed token is stale like any other: late traffic on it no-ops.
  manager_.endIngest(second, IngestOutcome::kCompleted);
  EXPECT_EQ(events.size(), expected.size());
}

// A kStopOnly producer (the toolbox ingest ABI, which has no host-side rollback)
// can stop but cannot throw away what arrived. The refusal is the whole point of
// the declaration: silently downgrading a discard to a keep would leave the user
// with data they asked to be rid of, and it must not even count as a stop — the
// producer keeps running and its own outcome still stands.
TEST_F(SessionManagerIngestCancelTest, StopOnlyIngestRefusesADiscardAndKeepsRunning) {
  const DatasetId dataset_id = makeDataset();
  QSignalSpy began_spy(&manager_, &SessionManager::ingestBegan);
  QSignalSpy stopping_spy(&manager_, &SessionManager::ingestStopping);
  QSignalSpy end_spy(&manager_, &SessionManager::ingestEnded);

  bool handler_ran = false;
  const IngestToken token = manager_.beginIngest(
      dataset_id, "label", 100, /*cancellable=*/true, [&handler_ran](bool) { handler_ran = true; },
      IngestStop::kStopOnly);

  ASSERT_EQ(began_spy.count(), 1);
  EXPECT_FALSE(began_spy[0][4].toBool()) << "the row must be told the bin is unavailable";
  EXPECT_FALSE(manager_.activeIngests().at(dataset_id).discardable);

  EXPECT_FALSE(manager_.requestCancel(token, /*keep_partial=*/false)) << "a discard must be refused outright";
  EXPECT_FALSE(handler_ran) << "the producer must not be asked to stop by a refused request";
  EXPECT_EQ(stopping_spy.count(), 0) << "a refused request must not acknowledge a stop";
  EXPECT_TRUE(manager_.ingestActive(dataset_id));

  // No cancellation intent was recorded, so the producer's own outcome stands.
  manager_.endIngest(token, IngestOutcome::kCompleted);
  ASSERT_EQ(end_spy.count(), 1);
  EXPECT_EQ(end_spy[0][1].value<IngestOutcome>(), IngestOutcome::kCompleted);
}

// The other half of the contract: kStopOnly still STOPS. Only the discard is
// refused, so the ✕ works exactly as it does on any other cancellable row.
TEST_F(SessionManagerIngestCancelTest, StopOnlyIngestAcceptsAKeep) {
  const DatasetId dataset_id = makeDataset();
  QSignalSpy stopping_spy(&manager_, &SessionManager::ingestStopping);
  QSignalSpy end_spy(&manager_, &SessionManager::ingestEnded);

  std::optional<bool> handler_keep_partial;
  const IngestToken token = manager_.beginIngest(
      dataset_id, "label", 100, /*cancellable=*/true,
      [&handler_keep_partial](bool keep_partial) { handler_keep_partial = keep_partial; }, IngestStop::kStopOnly);

  EXPECT_TRUE(manager_.requestCancel(token, /*keep_partial=*/true));
  ASSERT_TRUE(handler_keep_partial.has_value());
  EXPECT_TRUE(*handler_keep_partial);
  ASSERT_EQ(stopping_spy.count(), 1);
  EXPECT_TRUE(stopping_spy[0][1].toBool());

  manager_.endIngest(token, IngestOutcome::kCompleted);
  ASSERT_EQ(end_spy.count(), 1);
  EXPECT_EQ(end_spy[0][1].value<IngestOutcome>(), IngestOutcome::kCancelled) << "cancellation intent wins the terminal";
}

// A non-cancellable ingest has no stop at all, so kStopOnly's "keep is fine"
// half must not be read as permission to stop it.
TEST_F(SessionManagerIngestCancelTest, NonCancellableStopOnlyIngestRefusesEvenAKeep) {
  const DatasetId dataset_id = makeDataset();
  const IngestToken token =
      manager_.beginIngest(dataset_id, "label", 100, /*cancellable=*/false, {}, IngestStop::kStopOnly);

  EXPECT_FALSE(manager_.requestCancel(token, /*keep_partial=*/true));
  EXPECT_FALSE(manager_.requestCancel(token, /*keep_partial=*/false));
  EXPECT_TRUE(manager_.ingestActive(dataset_id));
}

// ingestStopping is emitted synchronously and an observer may react by asking to
// stop again (a second click, a coordinator forwarding the stop). Without the
// already-requested gate that request recurses through the emit, re-announcing
// the stop and re-running the producer's handler on every turn.
TEST_F(SessionManagerIngestCancelTest, ReentrantStopRequestDoesNotRecurseOrReannounce) {
  const DatasetId dataset_id = makeDataset();
  int handler_calls = 0;
  const IngestToken token =
      manager_.beginIngest(dataset_id, "label", 100, /*cancellable=*/true, [&handler_calls](bool) { ++handler_calls; });

  int stopping_events = 0;
  QObject::connect(&manager_, &SessionManager::ingestStopping, &manager_, [&](IngestToken stopping, bool) {
    ++stopping_events;
    if (stopping_events < 5) {  // bounded: an ungated re-entry would run away
      (void)manager_.requestCancel(stopping, /*keep_partial=*/true);
    }
  });

  EXPECT_TRUE(manager_.requestCancel(token, /*keep_partial=*/true));
  EXPECT_EQ(stopping_events, 1) << "a re-entrant stop request must not re-announce the stop";
  EXPECT_EQ(handler_calls, 1) << "the producer must be asked to stop exactly once";
}

// The second click cannot overrule the first. A stop already being acted on is
// reported as accepted, but nothing about it is re-run: the producer would
// otherwise receive a keep_partial contradicting the one it is already honouring.
TEST_F(SessionManagerIngestCancelTest, SecondStopRequestCannotFlipTheRecordedChoice) {
  const DatasetId dataset_id = makeDataset();
  std::vector<bool> handler_keep_partial;
  const IngestToken token = manager_.beginIngest(
      dataset_id, "label", 100, /*cancellable=*/true,
      [&handler_keep_partial](bool keep_partial) { handler_keep_partial.push_back(keep_partial); });
  QSignalSpy stopping_spy(&manager_, &SessionManager::ingestStopping);

  ASSERT_TRUE(manager_.requestCancel(token, /*keep_partial=*/true));
  EXPECT_TRUE(manager_.requestCancel(token, /*keep_partial=*/false)) << "the stop already accepted still stands";

  ASSERT_EQ(handler_keep_partial.size(), 1u) << "the producer must not be handed a second, contradicting choice";
  EXPECT_TRUE(handler_keep_partial.front());
  EXPECT_EQ(stopping_spy.count(), 1);
}

TEST_F(SessionManagerIngestCancelTest, MonotonicTokenIds) {
  const DatasetId dataset_id1 = makeDataset();
  const DatasetId dataset_id2 = makeDataset();

  IngestCancelFn cancel_fn = [](bool) {};

  const IngestToken token1 = manager_.beginIngest(dataset_id1, "label1", 100, /*cancellable=*/false, cancel_fn);
  const IngestToken token2 = manager_.beginIngest(dataset_id2, "label2", 100, /*cancellable=*/false, cancel_fn);

  EXPECT_GT(token2.id, token1.id);
}

TEST_F(SessionManagerIngestCancelTest, InvalidTokensIgnored) {
  const IngestToken invalid_token{0, 0};

  // These should all silently no-op without crashing.
  manager_.updateIngest(invalid_token, 50, 100);
  manager_.endIngest(invalid_token, IngestOutcome::kCompleted);
  const bool result = manager_.requestCancel(invalid_token, true);

  EXPECT_FALSE(result);
  EXPECT_FALSE(manager_.hasActiveIngests());
}

}  // namespace PJ::test
