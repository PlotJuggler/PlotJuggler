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
#include <QList>
#include <QNetworkReply>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <functional>
#include <memory>
#include <optional>

#include "local_path.h"
#include "mock_parser_support.h"

using namespace Qt::StringLiterals;

namespace {

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
  fetcher.fetch(
      QUrl::fromLocalFile(path),
      [&called, &result](pj::scene3d::FetchResult fetched) {
        called = true;
        result = std::move(fetched);
      },
      pj::scene3d::RemoteFetch::kAnyOrigin);
  // The contract: the callback is NEVER invoked synchronously from fetch() (a caller kicks a fetch from its load path).
  EXPECT_FALSE(called) << "callback ran before fetch() returned";

  ASSERT_TRUE(pumpUntil([&called]() { return called; }, 5000));
  EXPECT_TRUE(result.ok) << result.error.toStdString();
  EXPECT_EQ(result.bytes, payload);
  EXPECT_TRUE(result.final_url.isEmpty()) << "a local read has no served-from URL";
}

TEST(UrlFetcherTest, SchemelessBarePathReadsLocalFile) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QByteArray payload("bare path bytes");
  const QString path = writeTempFile(dir, "bare.bin", payload);

  pj::scene3d::UrlFetcher fetcher;
  bool called = false;
  pj::scene3d::FetchResult result;
  fetcher.fetch(
      QUrl(path),
      [&called, &result](pj::scene3d::FetchResult fetched) {
        called = true;
        result = std::move(fetched);
      },
      pj::scene3d::RemoteFetch::kAnyOrigin);
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
      },
      pj::scene3d::RemoteFetch::kAnyOrigin);
  ASSERT_TRUE(pumpUntil([&called]() { return called; }, 5000));
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.error.isEmpty());
}

TEST(UrlFetcherTest, UnsupportedSchemeFailsAsynchronously) {
  pj::scene3d::UrlFetcher fetcher;
  bool called = false;
  pj::scene3d::FetchResult result;
  fetcher.fetch(
      QUrl(u"ftp://example.invalid/mesh.stl"_s),
      [&called, &result](pj::scene3d::FetchResult fetched) {
        called = true;
        result = std::move(fetched);
      },
      pj::scene3d::RemoteFetch::kAnyOrigin);
  EXPECT_FALSE(called) << "even immediate errors must be delivered through the event loop";
  ASSERT_TRUE(pumpUntil([&called]() { return called; }, 5000));
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.error.contains("unsupported URL scheme"_L1)) << result.error.toStdString();
}

// A file:// URL naming a host other than localhost, or a scheme-less "//host/..." path, names a network share (SMB on
// Windows), never a local file. POSIX reads "//tmp/x" as "/tmp/x", so there an existing file proves no read happens.
TEST(UrlFetcherTest, NetworkSharePathsAreNeverRead) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = writeTempFile(dir, "share.bin", "share bytes");
  QList<QUrl> shares{QUrl(u"file://203.0.113.9/share/m.dae"_s)};
#ifndef Q_OS_WIN
  shares << QUrl::fromLocalFile(u"/"_s + path) << QUrl(u"/"_s + path);
#endif
  for (const QUrl& url : shares) {
    SCOPED_TRACE(url.toString().toStdString());
    pj::scene3d::UrlFetcher fetcher;
    std::optional<pj::scene3d::FetchResult> result;
    fetcher.fetch(
        url, [&result](pj::scene3d::FetchResult fetched) { result = std::move(fetched); },
        pj::scene3d::RemoteFetch::kAnyOrigin);
    ASSERT_TRUE(pumpUntil([&result]() { return result.has_value(); }, 5000));
    EXPECT_FALSE(result->ok);
    EXPECT_TRUE(result->error.contains("network share refused"_L1)) << result->error.toStdString();
  }
  pj::scene3d::UrlFetcher fetcher;
  std::optional<pj::scene3d::FetchResult> local;
  QUrl localhost = QUrl::fromLocalFile(path);
  localhost.setHost(u"localhost"_s);
  fetcher.fetch(
      localhost, [&local](pj::scene3d::FetchResult fetched) { local = std::move(fetched); },
      pj::scene3d::RemoteFetch::kAnyOrigin);
  ASSERT_TRUE(pumpUntil([&local]() { return local.has_value(); }, 5000));
  EXPECT_TRUE(local->ok) << "file://localhost/ names this host: " << local->error.toStdString();
}

// The share predicate on every platform's spellings. A Windows long path to a drive ("\\?\C:\...") is local; one to
// a UNC share ("\\?\UNC\host\...") is not. On macOS the automounter serves "/net/<host>/..." from that host.
TEST(LocalPathTest, NetworkSharePredicate) {
  using pj::scene3d::isNetworkSharePath;
  for (const bool macos : {false, true}) {
    SCOPED_TRACE(macos ? "macOS" : "other");
    for (const QString& share :
         {u"//host/share/m.dae"_s, u"\\\\host\\share\\m.dae"_s, u"/\\host/share"_s, u"\\\\?\\UNC\\host\\share\\m.dae"_s,
          u"//?/UNC/host/share/m.dae"_s, u"\\\\?\\C:\\..\\UNC\\host\\share\\t.png"_s,
          u"//?/C:/../UNC/host/share/t.png"_s, u"//?/C:/models/../../UNC/host/t.png"_s,
          u"\\??\\UNC\\host\\share\\t.png"_s, u"/?\?/UNC/host/share/t.png"_s, u"\\??\\C:\\t.png"_s}) {
      EXPECT_TRUE(isNetworkSharePath(share, macos)) << share.toStdString();
    }
    for (const QString& local :
         {u"/home/user/m.dae"_s, u"C:/models/m.dae"_s, u"C:\\models\\m.dae"_s, u"\\\\?\\C:\\models\\m.dae"_s,
          u"//?/C:/models/m.dae"_s, u"relative/m.dae"_s, u"/network/m.dae"_s, u""_s}) {
      EXPECT_FALSE(isNetworkSharePath(local, macos)) << local.toStdString();
    }
    for (const QString& automount :
         {u"/net/host/share/m.dae"_s, u"/net/./host/m.dae"_s, u"/tmp/../net/host/m"_s, u"/NET/host/m"_s}) {
      EXPECT_EQ(isNetworkSharePath(automount, macos), macos) << automount.toStdString();
    }
    EXPECT_FALSE(isNetworkSharePath(u"/net"_s, macos));
  }
}

// What a file import opens for its model's content (MeshLoader's IOSystem). The decision is tested here rather than
// through assimp: assimp retries a refused path with its separators collapsed, which on POSIX turns the "//tmp/x"
// stand-in for a share into a plain local path.
TEST(LocalPathTest, ImportOpensNoShareOutsideAChosenSharesFolder) {
  using pj::scene3d::importMayOpen;
  for (const bool macos : {false, true}) {
    SCOPED_TRACE(macos ? "macOS" : "other");
    EXPECT_TRUE(importMayOpen(u"/home/u/m.obj"_s, u"/home/u/../x.mtl"_s, macos));
    EXPECT_FALSE(importMayOpen(u"/home/u/m.obj"_s, u"//host/s/x.mtl"_s, macos));
    EXPECT_FALSE(importMayOpen(u"C:/u/m.obj"_s, u"\\\\host\\s\\x.mtl"_s, macos));
    EXPECT_TRUE(importMayOpen(u"//host/s/a/m.obj"_s, u"//host/s/a/x.mtl"_s, macos));
    EXPECT_TRUE(importMayOpen(u"\\\\host\\s\\a\\m.obj"_s, u"\\\\host\\s\\a\\tex\\t.png"_s, macos));
    EXPECT_FALSE(importMayOpen(u"//host/s/a/m.obj"_s, u"//host/s/a/../b/x.mtl"_s, macos));
    EXPECT_FALSE(importMayOpen(u"//host/s/a/m.obj"_s, u"//other/s/a/x.mtl"_s, macos));
    EXPECT_EQ(importMayOpen(u"/Users/u/m.obj"_s, u"/net/host/a/t.png"_s, macos), !macos);
    EXPECT_TRUE(importMayOpen(u"/net/host/a/m.obj"_s, u"/net/host/a/t.png"_s, macos));
    EXPECT_EQ(importMayOpen(u"/net/host/a/m.obj"_s, u"/net/host/b/t.png"_s, macos), !macos);
  }
}

// What a model's text joined onto its folder opens: never a share the folder is not on.
TEST(LocalPathTest, JoinContentPathNeverLandsOnAShare) {
  using pj::scene3d::joinContentPath;
  for (const bool macos : {false, true}) {
    SCOPED_TRACE(macos ? "macOS" : "other");
    for (const QString& share : {u"//host/share/t.png"_s, u"//?/C:/../UNC/host/t.png"_s, u"/?\?/UNC/host/t.png"_s}) {
      EXPECT_TRUE(joinContentPath(u"/ws/robot"_s, share, macos).isEmpty()) << share.toStdString();
    }
#ifndef Q_OS_WIN  // a POSIX folder: Windows would make it absolute on the current drive
    EXPECT_EQ(joinContentPath(u"/ws/robot"_s, u"meshes/../t.png"_s, macos), u"/ws/robot/t.png"_s);
    EXPECT_EQ(joinContentPath(u"/ws/robot"_s, u"/abs/t.png"_s, macos), u"/abs/t.png"_s);
    EXPECT_EQ(joinContentPath(u"/ws/robot"_s, u"../../net/host/t.png"_s, macos).isEmpty(), macos);
    EXPECT_EQ(joinContentPath(u"/ws/robot"_s, u"../../NET/host/t.png"_s, macos).isEmpty(), macos);
#endif
  }
#ifndef Q_OS_WIN
  // A folder the user chose on a share keeps its own relative files.
  EXPECT_EQ(joinContentPath(u"/net/host/robot"_s, u"t.png"_s, true), u"/net/host/robot/t.png"_s);
#endif
}

// On Windows an absolute path like "C:/dir/file.bin" becomes a QUrl whose scheme() is the single-letter drive ("c"),
// not empty: the fetcher must still treat it as a local file, never as an unsupported scheme. Cross-platform check: a
// missing drive-letter path must fail with a FILE error, not "unsupported URL scheme 'c'".
TEST(UrlFetcherTest, WindowsDriveLetterPathTreatedAsLocalFile) {
  pj::scene3d::UrlFetcher fetcher;
  bool called = false;
  pj::scene3d::FetchResult result;
  fetcher.fetch(
      QUrl(u"C:/no/such/url_fetcher_test/drive.bin"_s),
      [&called, &result](pj::scene3d::FetchResult fetched) {
        called = true;
        result = std::move(fetched);
      },
      pj::scene3d::RemoteFetch::kAnyOrigin);
  ASSERT_TRUE(pumpUntil([&called]() { return called; }, 5000));
  EXPECT_FALSE(result.ok);  // the file does not exist on the test host
  EXPECT_FALSE(result.error.contains("unsupported URL scheme"_L1))
      << "drive-letter path misrouted as a URL scheme: " << result.error.toStdString();
}

// The lifetime guarantee layers rely on: destroying the fetcher while a request
// is in flight aborts it and the callback never fires.
TEST(UrlFetcherTest, DestructionDropsInFlightCallback) {
  // A server that accepts the connection but never responds keeps the request in flight.
  QTcpServer server;
  auto fetcher = std::make_unique<pj::scene3d::UrlFetcher>();
  bool called = false;
  const QUrl url(pj::scene3d::test::silentServerUrl(server, u"never"_s));
  fetcher->fetch(url, [&called](pj::scene3d::FetchResult) { called = true; }, pj::scene3d::RemoteFetch::kAnyOrigin);

  pumpUntil([&server]() { return server.hasPendingConnections(); }, 2000);
  fetcher.reset();

  pumpUntil([]() { return false; }, 300);  // give a stray queued callback time to fire
  EXPECT_FALSE(called) << "callback fired after the fetcher was destroyed";
}

// Destroying the fetcher mid-request cancels the request while the fetcher is still whole: the
// reply finishes during the fetcher's destruction, and the callback still never runs.
TEST(UrlFetcherTest, DestructionCancelsInFlightRequestsWhileTheFetcherIsWhole) {
  QTcpServer server;
  auto fetcher = std::make_unique<pj::scene3d::UrlFetcher>();
  bool called = false;
  fetcher->fetch(
      QUrl(pj::scene3d::test::silentServerUrl(server, u"never"_s)),
      [&called](pj::scene3d::FetchResult) { called = true; }, pj::scene3d::RemoteFetch::kAnyOrigin);
  ASSERT_TRUE(pumpUntil([&server]() { return server.hasPendingConnections(); }, 2000));
  const QList<QNetworkReply*> replies = fetcher->findChildren<QNetworkReply*>();
  ASSERT_EQ(replies.size(), 1);
  bool reply_finished = false;
  QObject::connect(replies.front(), &QNetworkReply::finished, [&reply_finished]() { reply_finished = true; });

  fetcher.reset();

  EXPECT_TRUE(reply_finished) << "the request was not cancelled while the fetcher was alive";
  pumpUntil([]() { return false; }, 300);
  EXPECT_FALSE(called);
}

namespace {
// A Content-Length one byte over the cap, which differs per renderer build.
QByteArray overCapLength() {
  return QByteArray::number(pj::scene3d::UrlFetcher::kMaxFetchBytes + 1);
}
}  // namespace

// Size cap: a response whose declared Content-Length exceeds kMaxFetchBytes is
// aborted instead of buffered.
TEST(UrlFetcherTest, ResponseBeyondSizeCapIsAborted) {
  QTcpServer server;
  ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0));
  QObject::connect(&server, &QTcpServer::newConnection, &server, [&server]() {
    QTcpSocket* socket = server.nextPendingConnection();
    QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
      socket->readAll();  // consume the request; the content is irrelevant
      // One byte over the cap declared: the fetcher must abort on the
      // headers (metaDataChanged) without waiting for the body to stream.
      socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + overCapLength() + "\r\n\r\n");
      socket->write(QByteArray(4096, 'x'));
      socket->flush();
    });
  });

  pj::scene3d::UrlFetcher fetcher;
  bool called = false;
  pj::scene3d::FetchResult result;
  const QUrl url(u"http://127.0.0.1:%1/huge"_s.arg(server.serverPort()));
  fetcher.fetch(
      url,
      [&called, &result](pj::scene3d::FetchResult fetched) {
        called = true;
        result = std::move(fetched);
      },
      pj::scene3d::RemoteFetch::kAnyOrigin);

  ASSERT_TRUE(pumpUntil([&called]() { return called; }, 10000)) << "capped fetch never completed";
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.error.contains("limit"_L1)) << result.error.toStdString();
}

// A redirect the disk cache serves is held to the request's RemoteFetch as one the network answers: a kOwnOrigin
// fetch never reaches the other origin the cached redirect names.
TEST(UrlFetcherTest, CachedRedirectStaysOnTheRequestedOrigin) {
  using pj::scene3d::test::FileHttpServer;
  QTemporaryDir cache_dir;
  ASSERT_TRUE(cache_dir.isValid());
  const QByteArray previous_cache_dir = qgetenv("PJ_MODEL_CACHE_DIR");
  qputenv("PJ_MODEL_CACHE_DIR", cache_dir.path().toUtf8());
  const FileHttpServer elsewhere({{u"/m.bin"_s, QByteArray("payload")}});
  const FileHttpServer own({}, {{u"/m.bin"_s, elsewhere.url(u"/m.bin"_s)}}, "max-age=3600");
  pj::scene3d::UrlFetcher fetcher;
  const auto fetch = [&fetcher, &own](pj::scene3d::RemoteFetch remote) {
    std::optional<pj::scene3d::FetchResult> result;
    fetcher.fetch(
        QUrl(own.url(u"/m.bin"_s)), [&result](pj::scene3d::FetchResult fetched) { result = std::move(fetched); },
        remote);
    EXPECT_TRUE(pumpUntil([&result]() { return result.has_value(); }, 10000));
    return result.value_or(pj::scene3d::FetchResult{});
  };
  const pj::scene3d::FetchResult followed = fetch(pj::scene3d::RemoteFetch::kAnyOrigin);
  EXPECT_TRUE(followed.ok) << followed.error.toStdString();
  EXPECT_EQ(followed.bytes, "payload");
  const pj::scene3d::FetchResult refused = fetch(pj::scene3d::RemoteFetch::kOwnOrigin);
  EXPECT_FALSE(refused.ok);
  EXPECT_TRUE(refused.error.contains("another origin"_L1)) << refused.error.toStdString();
  EXPECT_EQ(own.paths().size(), 1) << "the second fetch did not take the cached redirect";
  EXPECT_EQ(elsewhere.paths().size(), 1) << elsewhere.paths().join(u',').toStdString();
  qputenv("PJ_MODEL_CACHE_DIR", previous_cache_dir);
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
  fetcher.fetch(
      url,
      [&result, &done](pj::scene3d::FetchResult r) {
        result = std::move(r);
        done = true;
      },
      pj::scene3d::RemoteFetch::kAnyOrigin);
  EXPECT_TRUE(pumpUntil([&done]() { return done; }, 5000));
  return result;
}

}  // namespace

// A FRESH cached entry is reused across UrlFetcher instances (i.e. across sessions) WITHOUT any network: a new fetcher
// serves the model from disk even after the origin server is gone.
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

// A redirected response reports where it was finally served, so a URDF's
// relative meshes and package roots anchor at that location.
TEST(UrlFetcherTest, FinalUrlFollowsRedirect) {
  clearModelCache();
  QTcpServer server;
  ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0));
  QObject::connect(&server, &QTcpServer::newConnection, &server, [&server]() {
    QTcpSocket* socket = server.nextPendingConnection();
    QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
      const QByteArray request = socket->readAll();
      if (request.startsWith("GET /start/robot.urdf ")) {
        socket->write("HTTP/1.1 302 Found\r\nLocation: /served/v2/robot.urdf\r\nContent-Length: 0\r\n\r\n");
      } else {
        socket->write("HTTP/1.1 200 OK\r\nCache-Control: no-store\r\nContent-Length: 5\r\n\r\nrobot");
      }
      socket->flush();
    });
  });

  pj::scene3d::UrlFetcher fetcher;
  const pj::scene3d::FetchResult result =
      fetchSync(fetcher, QUrl(u"http://127.0.0.1:%1/start/robot.urdf"_s.arg(server.serverPort())));
  ASSERT_TRUE(result.ok) << result.error.toStdString();
  EXPECT_EQ(result.bytes, QByteArray("robot"));
  EXPECT_EQ(
      result.final_url.toString().toStdString(),
      u"http://127.0.0.1:%1/served/v2/robot.urdf"_s.arg(server.serverPort()).toStdString());
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
      socket->write(
          "HTTP/1.1 200 OK\r\nCache-Control: max-age=3600\r\nContent-Length: " + overCapLength() + "\r\n\r\n");
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
