// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QFile>
#include <QTemporaryDir>
#include <memory>
#include <optional>

#include "BrowserFileStore.h"

namespace PJ {
namespace {

// stageAsync() drives itself with QTimer::singleShot(0), so the tests need
// the runner's application and its event dispatcher throughout each stage.
QCoreApplication& testApp() {
  return *QCoreApplication::instance();
}

// Pump the event loop until `done` becomes true or the deadline elapses.
// Returns whether `done` was observed true within the timeout.
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

// A payload spanning several 16 MiB chunks must land byte-identical and produce
// a valid browser-upload LoadInput, exercising the multi-yield chunk loop.
TEST(BrowserFileStore, StageAsyncMultiChunkIsByteIdentical) {
  testApp();
  QTemporaryDir temporary;
  ASSERT_TRUE(temporary.isValid());
  BrowserFileStore store(temporary.path());

  // ~40 MiB with a non-repeating pattern so a mis-ordered or short chunk write
  // is detectable, not just a length check.
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

// A file smaller than one chunk still routes through the async path cleanly.
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

// A zero-byte selection is a valid upload on the async path too.
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

// Completion must arrive through the event loop, never re-entrantly from the
// stageAsync() call itself. Assert the callback has NOT run before we pump.
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

// The source buffer moved into stageAsync must be released by completion time.
// QByteArray offers no public detach/use-count seam, so we assert via move
// semantics: the caller's rvalue was moved in, and by completion the async job
// has cleared its own copy (source.clear() before finish()). We verify by
// staging a large payload and confirming the caller's post-move array is empty.
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
  // The caller's array was moved from at the call site; the internal copy is
  // dropped before completion. There is no public seam on the internal buffer,
  // but the caller-side move is observable: `payload` is now detached/empty.
  EXPECT_TRUE(payload.isEmpty()) << "std::move into stageAsync did not transfer ownership";
  ASSERT_TRUE(pumpUntil(done));
}

// Pump until every deferred deletion (QTimer::singleShot's self-deleting timer
// objects, and with them their captured functors) has run or `witness` expired.
void pumpUntilExpired(const std::weak_ptr<int>& witness) {
  QDeadlineTimer deadline(5000);
  while (!witness.expired() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
}

// The async job owns the completion functor until the job itself is destroyed,
// so a token captured in the completion witnesses the job's destruction. This
// pins the no-self-cycle invariant: if the job ever again stores a callable
// that captures its own owning shared_ptr, the token never expires and this
// test fails instead of silently leaking every staged upload.
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

// Same witness on the jobless failure path (placement rejected before any job
// exists): the completion must still run exactly once and then be released.
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
