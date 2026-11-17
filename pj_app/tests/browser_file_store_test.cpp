// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <memory>
#include <optional>

#include "BrowserFileStore.h"

namespace PJ {
namespace {

// stageAsync() schedules each step with QTimer::singleShot(0), so the runner's QCoreApplication must be alive.
QCoreApplication& testApp() {
  return *QCoreApplication::instance();
}

[[nodiscard]] bool pumpUntil(const bool& done, int timeout_ms = 5000) {
  QDeadlineTimer deadline(timeout_ms);
  while (!done && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return done;
}

TEST(BrowserFileStore, SanitizesUntrustedBrowserNames) {
  EXPECT_EQ(
      BrowserFileStore::sanitizedBasename(QStringLiteral("../folder\\unsafe name?.csv")),
      QStringLiteral("unsafe_name_.csv"));
  EXPECT_EQ(BrowserFileStore::sanitizedBasename(QStringLiteral("..")), QStringLiteral("upload.bin"));
}

TEST(BrowserFileStore, RecoversDisplayNameFromOpaqueIdentity) {
  QTemporaryDir temporary;
  ASSERT_TRUE(temporary.isValid());
  BrowserFileStore store(temporary.path());
  auto result = store.stage(QStringLiteral("folder/run 01+imu.csv"), QByteArray("x"));
  ASSERT_TRUE(result.input.has_value()) << result.error.toStdString();
  EXPECT_EQ(
      result.input->content_sha256, QStringLiteral("2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881"));
  EXPECT_EQ(
      BrowserFileStore::displayNameForIdentity(result.input->source_identity), QStringLiteral("folder/run 01+imu.csv"));
  EXPECT_TRUE(BrowserFileStore::displayNameForIdentity(QStringLiteral("/tmp/run.csv")).isEmpty());
  EXPECT_TRUE(BrowserFileStore::displayNameForIdentity(QStringLiteral("pj-upload://broken")).isEmpty());
  EXPECT_TRUE(BrowserFileStore::displayNameForIdentity(QStringLiteral("pj-upload://broken/name")).isEmpty());
  EXPECT_TRUE(
      BrowserFileStore::displayNameForIdentity(
          QStringLiteral("pj-upload://00000000-0000-0000-0000-000000000000/1/name.csv"))
          .isEmpty());
  EXPECT_TRUE(
      BrowserFileStore::displayNameForIdentity(
          QStringLiteral("pj-upload://01234567-89ab-cdef-0123-456789abcdef/not-an-id/name.csv"))
          .isEmpty());
}

TEST(BrowserFileStore, DuplicateBasenamesHaveIndependentLeases) {
  QTemporaryDir temporary;
  ASSERT_TRUE(temporary.isValid());
  BrowserFileStore store(temporary.path());

  auto first_result = store.stage(QStringLiteral("log.mcap"), QByteArray("first\0payload", 13));
  auto second_result = store.stage(QStringLiteral("log.mcap"), QByteArray("second", 6));
  ASSERT_TRUE(first_result.input.has_value()) << first_result.error.toStdString();
  ASSERT_TRUE(second_result.input.has_value()) << second_result.error.toStdString();

  LoadInput first = std::move(*first_result.input);
  LoadInput second = std::move(*second_result.input);
  first_result.input.reset();
  second_result.input.reset();
  EXPECT_NE(first.backing_path, second.backing_path);
  EXPECT_NE(first.source_identity, second.source_identity);
  EXPECT_EQ(first.content_sha256, QStringLiteral("1a8a735cabefb221b50bafda33d755dc858c4549e50ad52914c8f4c3d37cec5a"));
  EXPECT_EQ(second.content_sha256, QStringLiteral("16367aacb67a4a017c8da8ab95682ccb390863780f7114dda0a0e0c55644c7c4"));
  EXPECT_TRUE(isBrowserUploadIdentity(first.source_identity));

  QFile first_file(first.backing_path);
  ASSERT_TRUE(first_file.open(QIODevice::ReadOnly));
  EXPECT_EQ(first_file.readAll(), QByteArray("first\0payload", 13));
  first_file.close();

  const QString first_path = first.backing_path;
  const QString second_path = second.backing_path;
  first.lease.reset();
  EXPECT_FALSE(QFile::exists(first_path));
  EXPECT_TRUE(QFile::exists(second_path));

  second.lease.reset();
  EXPECT_FALSE(QFile::exists(second_path));
}

// reserve() only places the file (the caller creates it); the lease removes it and its directories, like a staged one.
TEST(BrowserFileStore, ReservedPlacementIsReleasedWithItsLease) {
  QTemporaryDir temporary;
  ASSERT_TRUE(temporary.isValid());
  BrowserFileStore store(temporary.path());

  auto result = store.reserve(QStringLiteral("dir/recording.mcap"));
  ASSERT_TRUE(result.input.has_value()) << result.error.toStdString();
  EXPECT_TRUE(isBrowserUploadIdentity(result.input->source_identity));
  EXPECT_EQ(
      BrowserFileStore::displayNameForIdentity(result.input->source_identity), QStringLiteral("dir/recording.mcap"));
  EXPECT_TRUE(result.input->content_sha256.isEmpty());
  const QString path = result.input->backing_path;
  EXPECT_EQ(QFileInfo(path).fileName(), QStringLiteral("recording.mcap"));
  EXPECT_FALSE(QFile::exists(path)) << "the caller creates the file";

  QFile written(path);
  ASSERT_TRUE(written.open(QIODevice::WriteOnly));
  ASSERT_EQ(written.write("streamed"), 8);
  written.close();
  result.input.reset();
  EXPECT_FALSE(QFile::exists(path));
  EXPECT_TRUE(QDir(temporary.path()).isEmpty()) << "the lease removes its directories too";

  EXPECT_FALSE(store.reserve(QString()).input.has_value());
}

TEST(BrowserFileStore, ZeroByteFileIsValid) {
  QTemporaryDir temporary;
  ASSERT_TRUE(temporary.isValid());
  BrowserFileStore store(temporary.path());
  auto result = store.stage(QStringLiteral("empty.csv"), {});
  ASSERT_TRUE(result.input.has_value()) << result.error.toStdString();
  EXPECT_EQ(QFileInfo(result.input->backing_path).size(), 0);
  EXPECT_EQ(
      result.input->content_sha256, QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

// A payload spanning several 16 MiB chunks lands byte-identical (multi-yield chunk loop).
TEST(BrowserFileStore, StageAsyncMultiChunkIsByteIdentical) {
  testApp();
  QTemporaryDir temporary;
  ASSERT_TRUE(temporary.isValid());
  BrowserFileStore store(temporary.path());

  // Non-repeating pattern so a mis-ordered or short chunk write is detectable.
  const qsizetype payload_size = 40 * 1024 * 1024 + 7;
  QByteArray payload(payload_size, Qt::Uninitialized);
  for (qsizetype i = 0; i < payload_size; ++i) {
    payload[i] = static_cast<char>((i * 31 + 7) & 0xFF);
  }

  bool done = false;
  std::optional<BrowserFileStore::StageResult> captured;
  store.stageAsync(QStringLiteral("big.mcap"), payload, [&](BrowserFileStore::StageResult result) {
    captured = std::move(result);
    done = true;
  });
  ASSERT_TRUE(pumpUntil(done)) << "stageAsync did not complete within the timeout";
  ASSERT_TRUE(captured.has_value());
  ASSERT_TRUE(captured->input.has_value()) << captured->error.toStdString();

  EXPECT_TRUE(isBrowserUploadIdentity(captured->input->source_identity));
  EXPECT_EQ(captured->input->display_name, QStringLiteral("big.mcap"));
  EXPECT_EQ(
      captured->input->content_sha256,
      QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()));

  QFile staged(captured->input->backing_path);
  ASSERT_TRUE(staged.open(QIODevice::ReadOnly));
  EXPECT_EQ(staged.size(), payload_size);
  EXPECT_EQ(staged.readAll(), payload);
  staged.close();
}

// MEMFS regrows (and copies) a file's backing array as appends extend it, so stageAsync sizes the file up front.
TEST(BrowserFileStore, StageAsyncSizesFileBeforeFirstChunk) {
  testApp();
  QTemporaryDir temporary;
  ASSERT_TRUE(temporary.isValid());
  BrowserFileStore store(temporary.path());
  const qsizetype payload_size = 40 * 1024 * 1024 + 7;
  bool done = false;
  std::optional<BrowserFileStore::StageResult> captured;
  store.stageAsync(
      QStringLiteral("big.mcap"), QByteArray(payload_size, 'p'), [&](BrowserFileStore::StageResult result) {
        captured = std::move(result);
        done = true;
      });

  QDirIterator staged_files(temporary.path(), QDir::Files, QDirIterator::Subdirectories);
  ASSERT_TRUE(staged_files.hasNext()) << "stageAsync did not create its file up front";
  EXPECT_EQ(QFileInfo(staged_files.next()).size(), payload_size) << "no chunk is written yet";

  ASSERT_TRUE(pumpUntil(done));
  ASSERT_TRUE(captured->input.has_value()) << captured->error.toStdString();
  QFile staged(captured->input->backing_path);
  ASSERT_TRUE(staged.open(QIODevice::ReadOnly));
  EXPECT_EQ(staged.readAll(), QByteArray(payload_size, 'p'));
}

TEST(BrowserFileStore, StageAsyncSingleChunkSmallFile) {
  testApp();
  QTemporaryDir temporary;
  ASSERT_TRUE(temporary.isValid());
  BrowserFileStore store(temporary.path());

  const QByteArray payload("hello\0async\0staging", 19);
  bool done = false;
  std::optional<BrowserFileStore::StageResult> captured;
  store.stageAsync(QStringLiteral("small.csv"), payload, [&](BrowserFileStore::StageResult result) {
    captured = std::move(result);
    done = true;
  });
  ASSERT_TRUE(pumpUntil(done));
  ASSERT_TRUE(captured.has_value());
  ASSERT_TRUE(captured->input.has_value()) << captured->error.toStdString();
  EXPECT_EQ(
      captured->input->content_sha256,
      QStringLiteral("be734d02bcd33c0805d00a1e46e5ca1ee6abc0dae3fa781ea722cb06fc53a464"));

  QFile staged(captured->input->backing_path);
  ASSERT_TRUE(staged.open(QIODevice::ReadOnly));
  EXPECT_EQ(staged.readAll(), payload);
  staged.close();
}

TEST(BrowserFileStore, StageAsyncZeroByteFile) {
  testApp();
  QTemporaryDir temporary;
  ASSERT_TRUE(temporary.isValid());
  BrowserFileStore store(temporary.path());

  bool done = false;
  std::optional<BrowserFileStore::StageResult> captured;
  store.stageAsync(QStringLiteral("empty.csv"), QByteArray(), [&](BrowserFileStore::StageResult result) {
    captured = std::move(result);
    done = true;
  });
  ASSERT_TRUE(pumpUntil(done));
  ASSERT_TRUE(captured.has_value());
  ASSERT_TRUE(captured->input.has_value()) << captured->error.toStdString();
  EXPECT_EQ(
      captured->input->content_sha256,
      QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  EXPECT_EQ(QFileInfo(captured->input->backing_path).size(), 0);
}

// Completion arrives through the event loop, never re-entrantly from stageAsync().
TEST(BrowserFileStore, StageAsyncCompletesOnEventLoopNotReentrantly) {
  testApp();
  QTemporaryDir temporary;
  ASSERT_TRUE(temporary.isValid());
  BrowserFileStore store(temporary.path());

  bool done = false;
  store.stageAsync(
      QStringLiteral("small.csv"), QByteArray("x", 1), [&](BrowserFileStore::StageResult) { done = true; });
  EXPECT_FALSE(done) << "completion ran re-entrantly from stageAsync()";
  EXPECT_TRUE(pumpUntil(done));
}

// The job's own copy has no public seam, so only the caller-side move is observable (the job clears its copy
// before completion).
TEST(BrowserFileStore, StageAsyncReleasesSourceByCompletion) {
  testApp();
  QTemporaryDir temporary;
  ASSERT_TRUE(temporary.isValid());
  BrowserFileStore store(temporary.path());

  QByteArray payload(20 * 1024 * 1024, 'A');
  bool done = false;
  store.stageAsync(QStringLiteral("big.bin"), std::move(payload), [&](BrowserFileStore::StageResult result) {
    ASSERT_TRUE(result.input.has_value()) << result.error.toStdString();
    done = true;
  });
  EXPECT_TRUE(payload.isEmpty()) << "std::move into stageAsync did not transfer ownership";
  ASSERT_TRUE(pumpUntil(done));
}

// Pump until QTimer::singleShot's self-deleting timers (and the functors they capture) are gone or `witness` expired.
void pumpUntilExpired(const std::weak_ptr<int>& witness) {
  QDeadlineTimer deadline(5000);
  while (!witness.expired() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
}

// The job owns the completion until it is destroyed, so a token captured by the completion witnesses the job's
// destruction. A job storing a callable that captures its own shared_ptr would never release it.
TEST(BrowserFileStore, StageAsyncDestroysJobAfterSuccess) {
  testApp();
  QTemporaryDir temporary;
  ASSERT_TRUE(temporary.isValid());
  BrowserFileStore store(temporary.path());

  auto token = std::make_shared<int>(0);
  std::weak_ptr<int> witness = token;
  bool done = false;
  store.stageAsync(
      QStringLiteral("witness.bin"), QByteArray(1024, 'W'),
      [&done, token = std::move(token)](BrowserFileStore::StageResult result) {
        ASSERT_TRUE(result.input.has_value()) << result.error.toStdString();
        done = true;
      });
  ASSERT_TRUE(pumpUntil(done));
  pumpUntilExpired(witness);
  EXPECT_TRUE(witness.expired()) << "async staging job (and its completion) leaked after success";
}

// Same witness on the jobless failure path (placement rejected before any job exists).
TEST(BrowserFileStore, StageAsyncDestroysCompletionAfterPlacementFailure) {
  testApp();
  QTemporaryDir temporary;
  ASSERT_TRUE(temporary.isValid());
  BrowserFileStore store(temporary.path());

  auto token = std::make_shared<int>(0);
  std::weak_ptr<int> witness = token;
  bool done = false;
  store.stageAsync(QString(), QByteArray("x"), [&done, token = std::move(token)](BrowserFileStore::StageResult result) {
    EXPECT_FALSE(result.input.has_value());
    EXPECT_FALSE(result.error.isEmpty());
    done = true;
  });
  ASSERT_TRUE(pumpUntil(done));
  pumpUntilExpired(witness);
  EXPECT_TRUE(witness.expired()) << "completion functor leaked after placement failure";
}

}  // namespace
}  // namespace PJ
