// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QList>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

#include "PresetController.h"
#include "http_test_utils.h"
#include "pj_runtime/PresetCatalog.h"
#include "support/route_server.h"

using namespace Qt::StringLiterals;
using PJ::test::LocalHttpServer;

namespace {

struct Completion {
  int calls = 0;
  QByteArray bytes;
  QUrl final_url;
  QString error;
};

PJ::PresetController::DownloadDone recordInto(Completion& completion) {
  return [&completion](QByteArray bytes, QUrl final_url, QString error) {
    ++completion.calls;
    completion.bytes = std::move(bytes);
    completion.final_url = std::move(final_url);
    completion.error = std::move(error);
  };
}

// Lets any stale delivery that is going to happen, happen.
void settle() {
  QTest::qWait(150);
}

}  // namespace

TEST(PresetControllerTest, DownloadDeliversBytesAndFinalUrl) {
  LocalHttpServer server;
  server.setResponse("200 OK", "layout bytes");
  PJ::PresetController controller;
  Completion completion;

  controller.download(server.url(), recordInto(completion));

  ASSERT_TRUE(QTest::qWaitFor([&] { return completion.calls > 0; }, 5000));
  EXPECT_EQ(completion.bytes, QByteArray("layout bytes"));
  EXPECT_EQ(completion.final_url, server.url());
  EXPECT_TRUE(completion.error.isEmpty()) << completion.error.toStdString();
}

TEST(PresetControllerTest, HttpErrorIsReported) {
  LocalHttpServer server;
  server.setResponse("404 Not Found", "missing");
  PJ::PresetController controller;
  Completion completion;

  controller.download(server.url(), recordInto(completion));

  ASSERT_TRUE(QTest::qWaitFor([&] { return completion.calls > 0; }, 5000));
  EXPECT_FALSE(completion.error.isEmpty());
  EXPECT_TRUE(completion.bytes.isEmpty());
}

TEST(PresetControllerTest, BodyOverTheCapIsAnError) {
  LocalHttpServer server;
  server.setResponse("200 OK", QByteArray(PJ::kMaxPresetDownloadBytes + 1, 'x'));
  PJ::PresetController controller;
  Completion completion;

  controller.download(server.url(), recordInto(completion));

  ASSERT_TRUE(QTest::qWaitFor([&] { return completion.calls > 0; }, 10000));
  EXPECT_FALSE(completion.error.isEmpty());
  EXPECT_TRUE(completion.bytes.isEmpty());
}

// A download is capped at the caller's limit, not the default (a preset's recording takes a larger one).
TEST(PresetControllerTest, DownloadHonoursTheCallersLimit) {
  LocalHttpServer server;
  server.setResponse("200 OK", QByteArray(2048, 'x'));
  PJ::PresetController controller;
  Completion completion;

  controller.download(server.url(), recordInto(completion), 1024);

  ASSERT_TRUE(QTest::qWaitFor([&] { return completion.calls > 0; }, 10000));
  EXPECT_FALSE(completion.error.isEmpty());
  EXPECT_TRUE(completion.bytes.isEmpty());
}

// A recording lands in the caller's file with its size and fingerprint, never handed over as bytes.
TEST(PresetControllerTest, DownloadToFileWritesTheBodyAndItsFingerprint) {
  LocalHttpServer server;
  server.setResponse("200 OK", "recording bytes");
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  const QString path = directory.filePath(u"data.mcap"_s);
  PJ::PresetController controller;
  std::optional<PJ::PresetController::FileDownload> result;

  controller.downloadToFile(
      server.url(), path, 1024, [&result](PJ::PresetController::FileDownload file) { result = std::move(file); });

  ASSERT_TRUE(QTest::qWaitFor([&] { return result.has_value(); }, 5000));
  EXPECT_TRUE(result->error.isEmpty()) << result->error.toStdString();
  EXPECT_FALSE(result->limit_exceeded);
  EXPECT_EQ(result->size, 15);
  EXPECT_EQ(
      result->sha256,
      QString::fromLatin1(QCryptographicHash::hash("recording bytes", QCryptographicHash::Sha256).toHex()));
  QFile written(path);
  ASSERT_TRUE(written.open(QIODevice::ReadOnly));
  EXPECT_EQ(written.readAll(), QByteArray("recording bytes"));
}

// The bytes received so far are reported as they arrive, against the declared size, ending at the whole body
// before the completion runs.
TEST(PresetControllerTest, DownloadToFileReportsProgress) {
  LocalHttpServer server;
  const QByteArray body(2 * 1024 * 1024, 'x');
  server.setResponse("200 OK", body);
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  PJ::PresetController controller;
  std::optional<PJ::PresetController::FileDownload> result;
  QList<std::pair<qint64, qint64>> progress;
  qsizetype progress_at_completion = -1;

  controller.downloadToFile(
      server.url(), directory.filePath(u"data.mcap"_s), 4 * 1024 * 1024,
      [&](PJ::PresetController::FileDownload file) {
        result = std::move(file);
        progress_at_completion = progress.size();
      },
      [&progress](qint64 received, qint64 total) { progress.push_back({received, total}); });

  ASSERT_TRUE(QTest::qWaitFor([&] { return result.has_value(); }, 10000));
  settle();
  EXPECT_TRUE(result->error.isEmpty()) << result->error.toStdString();
  ASSERT_FALSE(progress.isEmpty());
  EXPECT_EQ(progress_at_completion, progress.size());
  qint64 previous = 0;
  for (const auto& [received, total] : progress) {
    EXPECT_EQ(total, body.size());
    EXPECT_GE(received, previous);
    previous = received;
  }
  EXPECT_EQ(progress.back().first, body.size());
}

// Over the caller's limit is flagged apart from other failures: the replay words it as its cap.
TEST(PresetControllerTest, DownloadToFileFlagsABodyOverTheLimit) {
  LocalHttpServer server;
  server.setResponse("200 OK", QByteArray(2048, 'x'));
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  PJ::PresetController controller;
  std::optional<PJ::PresetController::FileDownload> result;

  controller.downloadToFile(
      server.url(), directory.filePath(u"data.mcap"_s), 1024,
      [&result](PJ::PresetController::FileDownload file) { result = std::move(file); });

  ASSERT_TRUE(QTest::qWaitFor([&] { return result.has_value(); }, 10000));
  EXPECT_TRUE(result->limit_exceeded);
  EXPECT_FALSE(result->error.isEmpty());
  EXPECT_TRUE(result->sha256.isEmpty());
}

TEST(PresetControllerTest, NonHttpSchemeIsRefusedAsynchronously) {
  PJ::PresetController controller;
  Completion completion;

  controller.download(QUrl(u"file:///etc/hostname"_s), recordInto(completion));

  EXPECT_EQ(completion.calls, 0);  // never from within download()
  ASSERT_TRUE(QTest::qWaitFor([&] { return completion.calls > 0; }, 5000));
  EXPECT_FALSE(completion.error.isEmpty());
  EXPECT_TRUE(completion.bytes.isEmpty());
}

TEST(PresetControllerTest, CancelDropsTheCompletion) {
  LocalHttpServer server;
  server.setResponse("200 OK", "late");
  PJ::PresetController controller;
  Completion completion;

  controller.download(server.url(), recordInto(completion));
  controller.cancel();

  settle();
  EXPECT_EQ(completion.calls, 0);
}

TEST(PresetControllerTest, NewDownloadSupersedesTheInFlightOne) {
  LocalHttpServer server;
  server.setResponse("200 OK", "payload");
  PJ::PresetController controller;
  Completion first;
  Completion second;

  controller.download(server.url(), recordInto(first));
  controller.download(server.url(), recordInto(second));

  ASSERT_TRUE(QTest::qWaitFor([&] { return second.calls > 0; }, 5000));
  settle();
  EXPECT_EQ(first.calls, 0);
  EXPECT_EQ(second.calls, 1);
  EXPECT_EQ(second.bytes, QByteArray("payload"));
}

TEST(PresetControllerTest, CompletionMayStartTheNextDownload) {
  LocalHttpServer server;
  server.setResponse("200 OK", "payload");
  PJ::PresetController controller;
  Completion chained;

  controller.download(
      server.url(), [&](QByteArray, QUrl, QString) { controller.download(server.url(), recordInto(chained)); });

  ASSERT_TRUE(QTest::qWaitFor([&] { return chained.calls > 0; }, 5000));
  EXPECT_EQ(chained.bytes, QByteArray("payload"));
}

TEST(PresetControllerTest, DestroyDropsTheCompletion) {
  LocalHttpServer server;
  server.setResponse("200 OK", "late");
  Completion completion;
  {
    PJ::PresetController controller;
    controller.download(server.url(), recordInto(completion));
  }

  settle();
  EXPECT_EQ(completion.calls, 0);
}

// Destroying the controller mid-transfer cancels the transfer while the controller is still whole:
// the reply finishes during the controller's destruction, and the completion still never runs.
TEST(PresetControllerTest, DestroyMidTransferCancelsWhileTheControllerIsWhole) {
  pj_app_test::RouteServer server;
  server.route(u"/held"_s, "late");
  server.hold(u"/held"_s);
  Completion completion;
  auto controller = std::make_unique<PJ::PresetController>();
  controller->download(server.url(u"/held"_s), recordInto(completion));
  ASSERT_TRUE(QTest::qWaitFor([&] { return server.requests.size() == 1; }, 5000));
  const QList<QNetworkReply*> replies = controller->findChildren<QNetworkReply*>();
  ASSERT_EQ(replies.size(), 1);
  bool reply_finished = false;
  QObject::connect(replies.front(), &QNetworkReply::finished, [&reply_finished]() { reply_finished = true; });

  controller.reset();

  EXPECT_TRUE(reply_finished) << "the transfer was not cancelled while the controller was alive";
  settle();
  EXPECT_EQ(completion.calls, 0);
}

namespace {

struct Discovery {
  int calls = 0;
  QList<PJ::PresetEntry> entries;
  QString deep_link;
  QString deep_link_error;
};

// Runs start() on a page served beside an index answering `status` + `index`.
Discovery discover(
    const QByteArray& status, const QByteArray& index, const QString& page, QByteArray* request = nullptr) {
  LocalHttpServer server;
  server.setResponse(status, index);
  PJ::PresetController controller;
  Discovery discovery;
  QObject::connect(
      &controller, &PJ::PresetController::discovered,
      [&discovery](const QList<PJ::PresetEntry>& entries, const QString& deep_link, const QString& deep_link_error) {
        ++discovery.calls;
        discovery.entries = entries;
        discovery.deep_link = deep_link;
        discovery.deep_link_error = deep_link_error;
      });
  controller.start(server.url().resolved(QUrl(page)));
  EXPECT_EQ(discovery.calls, 0);  // never from within start()
  EXPECT_TRUE(QTest::qWaitFor([&] { return discovery.calls > 0; }, 5000));
  settle();
  EXPECT_EQ(discovery.calls, 1);
  if (request != nullptr) {
    *request = server.requestHeaders();
  }
  return discovery;
}

const QByteArray kIndex = R"({"presets": [{"id": "fr3", "title": "FR3", "layout": "fr3/layout.pj4.xml"},
                                          {"id": "nav", "layout": "nav/layout.pj4.xml"}]})";

}  // namespace

// The index sits in a presets/ folder beside the page, whatever the page's query
// or fragment, and a ?preset= deep link names one of its entries.
TEST(PresetControllerTest, StartFindsTheIndexBesideThePageAndItsDeepLink) {
  QByteArray request;
  const Discovery linked = discover("200 OK", kIndex, u"app/index.html?x=1&preset=fr3#top"_s, &request);
  EXPECT_TRUE(request.startsWith("GET /app/presets/index.json HTTP/1.1")) << request.toStdString();
  ASSERT_EQ(linked.entries.size(), 2);
  EXPECT_EQ(linked.entries[0].layout_url.path(), u"/app/presets/fr3/layout.pj4.xml"_s);
  EXPECT_EQ(linked.deep_link, u"fr3"_s);
  EXPECT_TRUE(linked.deep_link_error.isEmpty()) << linked.deep_link_error.toStdString();

  const Discovery plain = discover("200 OK", kIndex, u"app/"_s, &request);
  EXPECT_TRUE(request.startsWith("GET /app/presets/index.json HTTP/1.1")) << request.toStdString();
  EXPECT_EQ(plain.entries.size(), 2);
  EXPECT_TRUE(plain.deep_link.isEmpty());
  EXPECT_TRUE(plain.deep_link_error.isEmpty());
}

// No index, an empty one and a malformed one all list nothing and are not
// errors by themselves; a deep link that cannot be honoured is, and so is an
// unknown id or a repeated key.
TEST(PresetControllerTest, StartReportsUnusableDeepLinks) {
  const Discovery missing = discover("404 Not Found", "missing", u"index.html"_s);
  EXPECT_TRUE(missing.entries.isEmpty());
  EXPECT_TRUE(missing.deep_link_error.isEmpty());

  const Discovery empty = discover("200 OK", R"({"presets": []})", u"index.html"_s);
  EXPECT_TRUE(empty.entries.isEmpty());
  EXPECT_TRUE(empty.deep_link_error.isEmpty());

  const Discovery malformed = discover("200 OK", R"({"presets": [{"id": "fr3"}]})", u"index.html"_s);
  EXPECT_TRUE(malformed.entries.isEmpty());
  EXPECT_TRUE(malformed.deep_link_error.isEmpty());

  for (const auto& [status, index, page] : {
           std::tuple{QByteArray("200 OK"), kIndex, u"index.html?preset=unknown"_s},
           std::tuple{QByteArray("200 OK"), kIndex, u"index.html?preset=fr3&preset=nav"_s},
           std::tuple{QByteArray("200 OK"), kIndex, u"index.html?preset=fr3&preset=fr3"_s},
           std::tuple{QByteArray("404 Not Found"), QByteArray("missing"), u"index.html?preset=fr3"_s},
           std::tuple{QByteArray("200 OK"), QByteArray(R"({"presets": [{"id": "fr3"}]})"), u"index.html?preset=fr3"_s},
       }) {
    const Discovery refused = discover(status, index, page);
    EXPECT_TRUE(refused.deep_link.isEmpty()) << page.toStdString();
    EXPECT_FALSE(refused.deep_link_error.isEmpty()) << page.toStdString();
  }
}
