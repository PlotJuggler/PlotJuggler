// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// UrlFetcher contract tests: always-asynchronous delivery (local files
// included), scheme gating, the no-callback-after-destruction guarantee, and
// the response size cap. Network paths are exercised against an in-process
// QTcpServer so nothing here touches a real network.

#include "url_fetcher.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <functional>
#include <memory>
using namespace Qt::StringLiterals;

namespace {

// Pump the event loop until `done` returns true or `timeout_ms` elapses.
bool pumpUntil(const std::function<bool()>& done, int timeout_ms) {
  QElapsedTimer timer;
  timer.start();
  while (!done() && timer.elapsed() < timeout_ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return done();
}

QString writeTempFile(const QTemporaryDir& dir, const char* name, const QByteArray& bytes) {
  const QString path = dir.filePath(QLatin1String(name));
  QFile file(path);
  EXPECT_TRUE(file.open(QIODevice::WriteOnly));
  file.write(bytes);
  return path;
}

}  // namespace

TEST(UrlFetcherTest, LocalFileUrlDeliversBytesAsynchronously) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QByteArray payload("hello mesh bytes");
  const QString path = writeTempFile(dir, "payload.bin", payload);

  pj::scene3d::UrlFetcher fetcher;
  bool called = false;
  pj::scene3d::FetchResult result;
  fetcher.fetch(QUrl::fromLocalFile(path), [&called, &result](pj::scene3d::FetchResult fetched) {
    called = true;
    result = std::move(fetched);
  });
  // The contract: the callback is NEVER invoked synchronously from fetch() —
  // a caller must be safe kicking a fetch from inside its own load path.
  EXPECT_FALSE(called) << "callback ran before fetch() returned";

  ASSERT_TRUE(pumpUntil([&called]() { return called; }, 5000));
  EXPECT_TRUE(result.ok) << result.error.toStdString();
  EXPECT_EQ(result.bytes, payload);
}

TEST(UrlFetcherTest, SchemelessBarePathReadsLocalFile) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QByteArray payload("bare path bytes");
  const QString path = writeTempFile(dir, "bare.bin", payload);

  pj::scene3d::UrlFetcher fetcher;
  bool called = false;
  pj::scene3d::FetchResult result;
  fetcher.fetch(QUrl(path), [&called, &result](pj::scene3d::FetchResult fetched) {
    called = true;
    result = std::move(fetched);
  });
  ASSERT_TRUE(pumpUntil([&called]() { return called; }, 5000));
  EXPECT_TRUE(result.ok) << result.error.toStdString();
  EXPECT_EQ(result.bytes, payload);
}

TEST(UrlFetcherTest, MissingLocalFileFailsWithError) {
  pj::scene3d::UrlFetcher fetcher;
  bool called = false;
  pj::scene3d::FetchResult result;
  fetcher.fetch(
      QUrl::fromLocalFile(u"/nonexistent/url_fetcher_test/missing.bin"_s),
      [&called, &result](pj::scene3d::FetchResult fetched) {
        called = true;
        result = std::move(fetched);
      });
  ASSERT_TRUE(pumpUntil([&called]() { return called; }, 5000));
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.error.isEmpty());
}

TEST(UrlFetcherTest, UnsupportedSchemeFailsAsynchronously) {
  pj::scene3d::UrlFetcher fetcher;
  bool called = false;
  pj::scene3d::FetchResult result;
  fetcher.fetch(QUrl(u"ftp://example.invalid/mesh.stl"_s), [&called, &result](pj::scene3d::FetchResult fetched) {
    called = true;
    result = std::move(fetched);
  });
  EXPECT_FALSE(called) << "even immediate errors must be delivered through the event loop";
  ASSERT_TRUE(pumpUntil([&called]() { return called; }, 5000));
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.error.contains("unsupported URL scheme"_L1)) << result.error.toStdString();
}

// Regression (Windows): an absolute path like "C:/dir/file.bin" becomes a QUrl
// whose scheme() is the single-letter drive ("c"), NOT empty — so the fetcher
// must still treat it as a local file, never reject it as an unsupported scheme.
// Cross-platform check: point at a missing drive-letter path and assert the
// failure is a FILE error, not "unsupported URL scheme 'c'".
TEST(UrlFetcherTest, WindowsDriveLetterPathTreatedAsLocalFile) {
  pj::scene3d::UrlFetcher fetcher;
  bool called = false;
  pj::scene3d::FetchResult result;
  fetcher.fetch(QUrl(u"C:/no/such/url_fetcher_test/drive.bin"_s), [&called, &result](pj::scene3d::FetchResult fetched) {
    called = true;
    result = std::move(fetched);
  });
  ASSERT_TRUE(pumpUntil([&called]() { return called; }, 5000));
  EXPECT_FALSE(result.ok);  // the file does not exist on the test host
  EXPECT_FALSE(result.error.contains("unsupported URL scheme"_L1))
      << "drive-letter path misrouted as a URL scheme: " << result.error.toStdString();
}

// The lifetime guarantee layers rely on: destroying the fetcher while a request
// is in flight aborts it and the callback never fires.
TEST(UrlFetcherTest, DestructionDropsInFlightCallback) {
  // A server that accepts the connection but never responds keeps the request
  // in flight for as long as the test wants.
  QTcpServer server;
  ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0));

  auto fetcher = std::make_unique<pj::scene3d::UrlFetcher>();
  bool called = false;
  const QUrl url(u"http://127.0.0.1:%1/never"_s.arg(server.serverPort()));
  fetcher->fetch(url, [&called](pj::scene3d::FetchResult) { called = true; });

  // Let the connection actually establish, then kill the fetcher mid-flight.
  pumpUntil([&server]() { return server.hasPendingConnections(); }, 2000);
  fetcher.reset();

  pumpUntil([]() { return false; }, 300);  // give a stray queued callback time to fire
  EXPECT_FALSE(called) << "callback fired after the fetcher was destroyed";
}

// Size cap: a response whose declared Content-Length exceeds kMaxFetchBytes is
// aborted instead of buffered.
TEST(UrlFetcherTest, ResponseBeyondSizeCapIsAborted) {
  QTcpServer server;
  ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0));
  QObject::connect(&server, &QTcpServer::newConnection, &server, [&server]() {
    QTcpSocket* socket = server.nextPendingConnection();
    QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
      socket->readAll();  // consume the request; the content is irrelevant
      // 100 MiB declared > the 64 MiB cap: the fetcher must abort on the
      // headers (metaDataChanged) without waiting for the body to stream.
      socket->write("HTTP/1.1 200 OK\r\nContent-Length: 104857600\r\n\r\n");
      socket->write(QByteArray(4096, 'x'));
      socket->flush();
    });
  });

  pj::scene3d::UrlFetcher fetcher;
  bool called = false;
  pj::scene3d::FetchResult result;
  const QUrl url(u"http://127.0.0.1:%1/huge"_s.arg(server.serverPort()));
  fetcher.fetch(url, [&called, &result](pj::scene3d::FetchResult fetched) {
    called = true;
    result = std::move(fetched);
  });

  ASSERT_TRUE(pumpUntil([&called]() { return called; }, 10000)) << "capped fetch never completed";
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.error.contains("limit"_L1)) << result.error.toStdString();
}

namespace {

// Wipe the on-disk model cache (PJ_MODEL_CACHE_DIR, pointed at a throwaway dir in
// main) so each cache test starts cold. QNetworkDiskCache recreates it on demand.
void clearModelCache() {
  QDir(qEnvironmentVariable("PJ_MODEL_CACHE_DIR")).removeRecursively();
}

// A one-shot HTTP responder: serves `body` with the given Cache-Control on each
// accepted connection and counts connections. Lives as long as the test wants it.
struct CountingHttpServer {
  QTcpServer server;
  int connections = 0;
  explicit CountingHttpServer(const QByteArray& body, const QByteArray& cache_control) {
    EXPECT_TRUE(server.listen(QHostAddress::LocalHost, 0));
    QObject::connect(&server, &QTcpServer::newConnection, &server, [this, body, cache_control]() {
      ++connections;
      QTcpSocket* socket = server.nextPendingConnection();
      QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, body, cache_control]() {
        socket->readAll();
        QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: model/gltf-binary\r\nETag: \"v1\"\r\n";
        response += "Cache-Control: " + cache_control + "\r\nContent-Length: ";
        response += QByteArray::number(body.size());
        response += "\r\n\r\n";
        response += body;
        socket->write(response);
        socket->flush();
        socket->disconnectFromHost();
      });
    });
  }
  quint16 port() const {
    return server.serverPort();
  }
};

pj::scene3d::FetchResult fetchSync(pj::scene3d::UrlFetcher& fetcher, const QUrl& url) {
  pj::scene3d::FetchResult result;
  bool done = false;
  fetcher.fetch(url, [&result, &done](pj::scene3d::FetchResult r) {
    result = std::move(r);
    done = true;
  });
  EXPECT_TRUE(pumpUntil([&done]() { return done; }, 5000));
  return result;
}

}  // namespace

// A FRESH cached entry is reused across UrlFetcher instances (i.e. across
// sessions) WITHOUT any network: a new fetcher serves the model from disk even
// after the origin server is gone. This is the "don't re-download the car" win.
TEST(UrlFetcherTest, FreshEntryServedCrossSessionWithoutNetwork) {
  clearModelCache();
  const QByteArray body("MESHBYTES");
  auto server = std::make_unique<CountingHttpServer>(body, "max-age=3600");
  const QUrl url(u"http://127.0.0.1:%1/lexus.glb"_s.arg(server->port()));

  {
    pj::scene3d::UrlFetcher first_session;
    const pj::scene3d::FetchResult r = fetchSync(first_session, url);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.bytes, body);
    ASSERT_EQ(server->connections, 1);
  }

  server.reset();  // origin is gone — a fresh, still-valid cache entry must not need it

  pj::scene3d::UrlFetcher second_session;
  const pj::scene3d::FetchResult r = fetchSync(second_session, url);
  EXPECT_TRUE(r.ok) << "fresh cached model not reused cross-session: " << r.error.toStdString();
  EXPECT_EQ(r.bytes, body);
}

// A STALE cached entry is served when the network is unreachable: the model
// rendered last session still shows up offline instead of regressing to the grey
// cube. Without the on-error AlwaysCache fallback, the stale revalidation fails.
TEST(UrlFetcherTest, StaleEntryServedFromCacheWhenOffline) {
  clearModelCache();
  const QByteArray body("MESHBYTES");
  auto server = std::make_unique<CountingHttpServer>(body, "max-age=0");  // cacheable but immediately stale
  const QUrl url(u"http://127.0.0.1:%1/lexus.glb"_s.arg(server->port()));

  pj::scene3d::UrlFetcher fetcher;
  const pj::scene3d::FetchResult first = fetchSync(fetcher, url);
  ASSERT_TRUE(first.ok) << first.error.toStdString();
  EXPECT_EQ(first.bytes, body);

  server.reset();  // go "offline": the stale entry now needs a revalidation that can't happen

  const pj::scene3d::FetchResult second = fetchSync(fetcher, url);
  EXPECT_TRUE(second.ok) << "stale cached model not served offline: " << second.error.toStdString();
  EXPECT_EQ(second.bytes, body);
}

// A size-capped (aborted) response must not leave a usable cache entry: a later
// fetch re-hits the network rather than being served truncated bytes as success.
TEST(UrlFetcherTest, SizeCappedResponseDoesNotPoisonCache) {
  clearModelCache();
  QTcpServer server;
  ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0));
  int connections = 0;
  QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, &connections]() {
    ++connections;
    QTcpSocket* socket = server.nextPendingConnection();
    QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
      socket->readAll();
      // 100 MiB declared (> 64 MiB cap) but cacheable headers: the fetcher aborts.
      socket->write("HTTP/1.1 200 OK\r\nCache-Control: max-age=3600\r\nContent-Length: 104857600\r\n\r\n");
      socket->write(QByteArray(4096, 'x'));
      socket->flush();
    });
  });
  const QUrl url(u"http://127.0.0.1:%1/huge.glb"_s.arg(server.serverPort()));

  pj::scene3d::UrlFetcher fetcher;
  const pj::scene3d::FetchResult first = fetchSync(fetcher, url);
  EXPECT_FALSE(first.ok);
  ASSERT_EQ(connections, 1);

  const pj::scene3d::FetchResult second = fetchSync(fetcher, url);
  EXPECT_FALSE(second.ok) << "a capped response must not be served from cache as success";
  EXPECT_EQ(connections, 2) << "second fetch must re-hit the network, not a poisoned partial cache entry";
}

// The shared main's QCoreApplication must die BEFORE exit handlers run — QtNetwork
// registers global cleanup that a function-local-static app would outlive,
// crashing at exit (same pattern as pj_marketplace's download_manager_test).
// Point the model cache at a throwaway dir (auto-removed at exit) so UrlFetcher's
// QNetworkDiskCache never touches the developer's real ~/.local/share.
namespace {

class UrlFetcherEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    static QTemporaryDir model_cache_dir;
    qputenv("PJ_MODEL_CACHE_DIR", (model_cache_dir.path() + u"/models"_s).toUtf8());
  }
};

static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new UrlFetcherEnvironment);

}  // namespace
