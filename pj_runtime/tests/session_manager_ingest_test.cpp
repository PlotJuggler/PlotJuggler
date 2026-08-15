// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QSignalSpy>
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

// Qt needs a main function for tests.
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
