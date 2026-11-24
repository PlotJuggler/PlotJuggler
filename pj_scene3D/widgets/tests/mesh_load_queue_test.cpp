// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// MeshLoadQueue: the per-layer bounds (fetches overlap up to the load bound while
// imports stay within theirs; cancelled imports keep their slots until their
// workers finish), cancellation that never reaches `done`, aborting fetches nobody
// wants, destruction during fetch/import, and a URL mesh's relative textures
// fetched beside it.

#include "mesh_load_queue.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mesh_import.h"  // maxConcurrentMeshLoads, maxConcurrentMeshImports
#include "mock_parser_support.h"
#include "url_fetcher.h"

using namespace Qt::StringLiterals;
using pj::scene3d::maxConcurrentMeshImports;
using pj::scene3d::maxConcurrentMeshLoads;
using pj::scene3d::MeshLoadQueue;
using pj::scene3d::test::cubeCopies;
using pj::scene3d::test::FileHttpServer;
using pj::scene3d::test::pumpUntil;
using pj::scene3d::test::readFixtureBytes;
using pj::scene3d::test::silentServerUrl;

namespace {

MeshLoadQueue::Source pathSource(const QString& path) {
  MeshLoadQueue::Source source;
  source.kind = MeshLoadQueue::Source::Kind::kPath;
  source.location = path;
  return source;
}

MeshLoadQueue::Source urlSource(const QString& url) {
  MeshLoadQueue::Source source;
  source.kind = MeshLoadQueue::Source::Kind::kUrl;
  source.location = url;
  source.format_hint = u"glb"_s;
  return source;
}

MeshLoadQueue::JobId enqueue(MeshLoadQueue& queue, MeshLoadQueue::Source source, std::vector<std::string>& log) {
  return queue.enqueue(
      [source]() { return std::optional(source); },
      [&log, label = source.location.toStdString()](const MeshLoadQueue::Outcome& outcome) {
        log.push_back(label + (outcome.mesh.ok ? ":ok" : ":failed"));
      });
}

std::optional<MeshLoadQueue::Outcome> loadQuadFrom(const FileHttpServer& server) {
  QObject context;
  MeshLoadQueue queue(&context);
  MeshLoadQueue::Source source = urlSource(server.url(u"/meshes/quad.dae"_s));
  source.format_hint = u"dae"_s;
  std::optional<MeshLoadQueue::Outcome> outcome;
  queue.enqueue(
      [source]() { return std::optional(source); },
      [&outcome](MeshLoadQueue::Outcome settled) { outcome = std::move(settled); });
  queue.pump();
  EXPECT_TRUE(pumpUntil([&] { return outcome.has_value(); }, std::chrono::seconds(5)));
  return outcome;
}

// `count` binary STL meshes at /meshes/<index>.stl.
QHash<QString, QByteArray> stlFiles(int count) {
  QHash<QString, QByteArray> files;
  for (int index = 0; index < count; ++index) {
    files.insert(u"/meshes/%1.stl"_s.arg(index), readFixtureBytes("cube.stl"));
  }
  return files;
}

MeshLoadQueue::Source stlUrlSource(const FileHttpServer& server, int index) {
  MeshLoadQueue::Source source = urlSource(server.url(u"/meshes/%1.stl"_s.arg(index)));
  source.format_hint = u"stl"_s;
  return source;
}

}  // namespace

// A URL mesh's relative texture is fetched beside it, once, into inline bytes
// keyed by its URL.
TEST(MeshLoadQueueTest, UrlMeshFetchesItsRelativeTexture) {
  const FileHttpServer server({
      {u"/meshes/quad.dae"_s, readFixtureBytes("textured_quad.dae")},
      {u"/meshes/red_4x4.png"_s, readFixtureBytes("red_4x4.png")},
  });
  const std::optional<MeshLoadQueue::Outcome> outcome = loadQuadFrom(server);
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->mesh.ok) << outcome->mesh.error.toStdString();
  EXPECT_EQ(server.paths().count(u"/meshes/red_4x4.png"_s), 1);
  ASSERT_FALSE(outcome->mesh.submeshes.empty());
  const pj::scene3d::TextureSource& base = outcome->mesh.submeshes.front().material->base_color;
  EXPECT_FALSE(base.bytes.empty());
  EXPECT_EQ(base.key, server.url(u"/meshes/red_4x4.png"_s).toStdString());
}

// A texture that cannot be fetched leaves its slot empty; the mesh still loads.
TEST(MeshLoadQueueTest, MissingUrlTextureLeavesTheMeshUntextured) {
  const FileHttpServer server({{u"/meshes/quad.dae"_s, readFixtureBytes("textured_quad.dae")}});
  const std::optional<MeshLoadQueue::Outcome> outcome = loadQuadFrom(server);
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->mesh.ok) << outcome->mesh.error.toStdString();
  EXPECT_TRUE(server.paths().contains(u"/meshes/red_4x4.png"_s));
  ASSERT_FALSE(outcome->mesh.submeshes.empty());
  EXPECT_TRUE(outcome->mesh.submeshes.front().material->base_color.empty());
}

// A texture ref that resolves to no http(s) URL is never requested.
TEST(MeshLoadQueueTest, UrlMeshNeverRequestsANonNetworkTexture) {
  QByteArray dae = readFixtureBytes("textured_quad.dae");
  dae.replace("red_4x4.png", "ftp://127.0.0.1/x.png");
  const FileHttpServer server({{u"/meshes/quad.dae"_s, dae}});
  const std::optional<MeshLoadQueue::Outcome> outcome = loadQuadFrom(server);
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->mesh.ok) << outcome->mesh.error.toStdString();
  EXPECT_EQ(server.paths().join(u',').toStdString(), "/meshes/quad.dae");
  ASSERT_FALSE(outcome->mesh.submeshes.empty());
  EXPECT_TRUE(outcome->mesh.submeshes.front().material->base_color.empty());
}

TEST(MeshLoadQueueTest, CancelledImportsKeepTheirSlotsAndNeverDeliver) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::size_t limit = maxConcurrentMeshImports();
  const std::vector<QString> paths = cubeCopies(dir, static_cast<int>(2 * limit + 1));
  QObject context;
  MeshLoadQueue queue(&context);
  std::vector<std::string> log;

  for (std::size_t index = 0; index <= limit; ++index) {
    enqueue(queue, pathSource(paths[index]), log);
  }
  queue.pump();
  ASSERT_EQ(queue.importsRunning(), limit);

  // No events in between: every first-wave import is still running.
  queue.cancelAll();
  EXPECT_EQ(queue.importsRunning(), limit) << "a cancelled import lost its slot while its worker runs";
  std::vector<std::string> wanted;
  for (std::size_t index = limit + 1; index < paths.size(); ++index) {
    enqueue(queue, pathSource(paths[index]), log);
    wanted.push_back(paths[index].toStdString() + ":ok");
  }
  queue.pump();
  EXPECT_LE(queue.importsRunning(), limit);

  EXPECT_TRUE(pumpUntil(
      [&] {
        EXPECT_LE(queue.importsRunning(), limit);
        return log.size() == wanted.size() && queue.runningCount() == 0;
      },
      std::chrono::seconds(10)));
  std::sort(log.begin(), log.end());
  std::sort(wanted.begin(), wanted.end());
  EXPECT_EQ(log, wanted) << "a cancelled job delivered, or a wanted one did not";
}

TEST(MeshLoadQueueTest, CancelledQueuedAndRunningJobsNeverDeliver) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::vector<QString> paths = cubeCopies(dir, static_cast<int>(maxConcurrentMeshLoads()) + 1);
  QObject context;
  MeshLoadQueue queue(&context);
  std::vector<std::string> log;
  std::vector<MeshLoadQueue::JobId> ids;
  for (const QString& path : paths) {
    ids.push_back(enqueue(queue, pathSource(path), log));
  }
  queue.pump();
  for (const MeshLoadQueue::JobId id : ids) {
    queue.cancel(id);  // the running ones and the one still queued
  }
  EXPECT_TRUE(pumpUntil([&] { return queue.runningCount() == 0; }, std::chrono::seconds(10)));
  EXPECT_TRUE(log.empty()) << log.front();
}

TEST(MeshLoadQueueTest, ResolveCanDropAJobWithoutUsingASlot) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::vector<QString> paths = cubeCopies(dir, 1);
  QObject context;
  MeshLoadQueue queue(&context);
  std::vector<std::string> log;
  int dropped_done = 0;
  for (std::size_t index = 0; index < maxConcurrentMeshLoads(); ++index) {
    queue.enqueue([]() { return std::optional<MeshLoadQueue::Source>(); }, [&](auto) { ++dropped_done; });
  }
  enqueue(queue, pathSource(paths.front()), log);
  queue.pump();
  EXPECT_EQ(queue.importsRunning(), 1U) << "dropped jobs held slots";
  EXPECT_TRUE(pumpUntil([&] { return !log.empty(); }, std::chrono::seconds(10)));
  EXPECT_EQ(dropped_done, 0);
}

TEST(MeshLoadQueueTest, FailedFetchIsReportedAsAFetchFailure) {
  quint16 port = 0;
  {
    QTcpServer probe;  // bound then released: the connection is refused at once
    ASSERT_TRUE(probe.listen(QHostAddress::LocalHost, 0));
    port = probe.serverPort();
  }
  QObject context;
  MeshLoadQueue queue(&context);
  std::optional<MeshLoadQueue::Outcome> outcome;
  queue.enqueue(
      [port]() { return std::optional(urlSource(u"http://127.0.0.1:%1/missing.glb"_s.arg(port))); },
      [&outcome](MeshLoadQueue::Outcome settled) { outcome = std::move(settled); });
  queue.pump();
  ASSERT_TRUE(pumpUntil([&] { return outcome.has_value(); }, std::chrono::seconds(5)));
  EXPECT_TRUE(outcome->fetch_failed);
  EXPECT_FALSE(outcome->mesh.ok);
  EXPECT_FALSE(outcome->mesh.error.isEmpty());
  EXPECT_EQ(queue.runningCount(), 0U);
}

// A cancelled fetch keeps its slot while another fetch on the shared fetcher is
// still wanted; once none is, the fetcher is retired and every slot frees.
TEST(MeshLoadQueueTest, UnwantedFetchesAreAbortedTogether) {
  if (maxConcurrentMeshLoads() < 2) {
    GTEST_SKIP() << "needs two concurrent fetches";
  }
  QTcpServer server;  // accepts and never responds: fetches stay in flight
  QObject context;
  MeshLoadQueue queue(&context);
  std::vector<std::string> log;
  const MeshLoadQueue::JobId first = enqueue(queue, urlSource(silentServerUrl(server, u"a.glb"_s)), log);
  const MeshLoadQueue::JobId second = enqueue(queue, urlSource(silentServerUrl(server, u"b.glb"_s)), log);
  queue.pump();
  ASSERT_EQ(queue.runningCount(), 2U);

  queue.cancel(first);
  EXPECT_EQ(queue.runningCount(), 2U) << "a wanted fetch was aborted with the unwanted one";
  queue.cancel(second);
  queue.pump();
  EXPECT_EQ(queue.runningCount(), 0U) << "unwanted fetches kept their slots";

  // The retired fetcher lives on as a child of the context until its deferred
  // delete, so a context destroyed first takes it along.
  EXPECT_EQ(context.findChildren<pj::scene3d::UrlFetcher*>().size(), 1);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  EXPECT_TRUE(context.findChildren<pj::scene3d::UrlFetcher*>().isEmpty());
  EXPECT_TRUE(log.empty());
}

// fetch() goes through the jobs' fetcher: cancelAll() retires it, which aborts
// the request, and its callback never runs.
TEST(MeshLoadQueueTest, CancelAllAbortsAPlainFetch) {
  QTcpServer server;
  QObject context;
  MeshLoadQueue queue(&context);
  bool called = false;
  queue.fetch(QUrl(silentServerUrl(server, u"robot.urdf"_s)), [&called](pj::scene3d::FetchResult) { called = true; });
  ASSERT_TRUE(pumpUntil([&] { return server.hasPendingConnections(); }, std::chrono::seconds(2)));
  queue.cancelAll();
  EXPECT_EQ(context.findChildren<pj::scene3d::UrlFetcher*>().size(), 1) << "the fetcher was not retired";
  pumpUntil([] { return false; }, std::chrono::milliseconds(200));
  EXPECT_FALSE(called);
}

// A pending fetch() keeps the fetcher when its last wanted job fetch is
// cancelled, and delivers.
TEST(MeshLoadQueueTest, PendingPlainFetchKeepsTheFetcher) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::vector<QString> paths = cubeCopies(dir, 1);
  QTcpServer server;
  QObject context;
  MeshLoadQueue queue(&context);
  std::vector<std::string> log;
  const MeshLoadQueue::JobId job = enqueue(queue, urlSource(silentServerUrl(server, u"a.glb"_s)), log);
  queue.pump();
  std::optional<pj::scene3d::FetchResult> fetched;
  queue.fetch(
      QUrl::fromLocalFile(paths.front()), [&fetched](pj::scene3d::FetchResult result) { fetched = std::move(result); });
  queue.cancel(job);
  EXPECT_EQ(queue.runningCount(), 1U) << "the fetcher was retired under a pending fetch()";
  ASSERT_TRUE(pumpUntil([&] { return fetched.has_value(); }, std::chrono::seconds(5)));
  EXPECT_TRUE(fetched->ok) << fetched->error.toStdString();
  EXPECT_FALSE(fetched->bytes.isEmpty());
  EXPECT_TRUE(log.empty());
}

TEST(MeshLoadQueueTest, DestructionDuringFetchDropsTheCompletion) {
  QTcpServer server;
  QObject context;
  auto queue = std::make_unique<MeshLoadQueue>(&context);
  std::vector<std::string> log;
  enqueue(*queue, urlSource(silentServerUrl(server, u"slow.glb"_s)), log);
  queue->pump();
  ASSERT_EQ(queue->runningCount(), 1U);
  queue.reset();
  pumpUntil([] { return false; }, std::chrono::milliseconds(200));
  EXPECT_TRUE(log.empty());
}

TEST(MeshLoadQueueTest, DestructionDuringImportDropsTheCompletion) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::vector<QString> paths = cubeCopies(dir, 1);
  QObject context;
  auto queue = std::make_unique<MeshLoadQueue>(&context);
  std::vector<std::string> log;
  enqueue(*queue, pathSource(paths.front()), log);
  queue->pump();
  ASSERT_EQ(queue->importsRunning(), 1U);
  queue.reset();  // the worker runs on, detached
  pumpUntil([] { return false; }, std::chrono::milliseconds(500));
  EXPECT_TRUE(log.empty());
}

// URL meshes download side by side up to the load bound, however tight the import
// bound: a fetch does not wait for another job's import.
TEST(MeshLoadQueueTest, UrlFetchesOverlapUpToTheLoadBound) {
  QTcpServer server;
  QObject context;
  MeshLoadQueue queue(&context, /*max_loads=*/3, /*max_imports=*/1);
  std::vector<std::string> log;
  for (int index = 0; index < 5; ++index) {
    enqueue(queue, urlSource(silentServerUrl(server, u"%1.glb"_s.arg(index))), log);
  }
  queue.pump();
  EXPECT_EQ(queue.runningCount(), 3U) << "fetches did not overlap";
  int connections = 0;
  pumpUntil(
      [&] {
        while (server.hasPendingConnections()) {
          server.nextPendingConnection();
          ++connections;
        }
        return false;
      },
      std::chrono::milliseconds(300));
  EXPECT_EQ(connections, 3) << "requests in flight at once";
  EXPECT_EQ(queue.importsRunning(), 0U);
  queue.cancelAll();
  EXPECT_EQ(queue.runningCount(), 0U);
  EXPECT_TRUE(log.empty());
}

// Landed bytes wait for an import slot: imports never exceed their bound while
// the next meshes download, and every mesh loads.
TEST(MeshLoadQueueTest, ImportsStayWithinTheirBoundWhileFetchesOverlap) {
  constexpr int kMeshes = 8;
  const FileHttpServer server(stlFiles(kMeshes));
  QObject context;
  MeshLoadQueue queue(&context, /*max_loads=*/4, /*max_imports=*/1);
  std::vector<std::string> log;
  std::vector<std::string> wanted;
  for (int index = 0; index < kMeshes; ++index) {
    const MeshLoadQueue::Source source = stlUrlSource(server, index);
    enqueue(queue, source, log);
    wanted.push_back(source.location.toStdString() + ":ok");
  }
  queue.pump();
  EXPECT_EQ(queue.runningCount(), 4U);
  std::size_t peak_imports = 0;
  EXPECT_TRUE(pumpUntil(
      [&] {
        peak_imports = std::max(peak_imports, queue.importsRunning());
        EXPECT_LE(queue.runningCount(), 4U);
        return queue.runningCount() == 0 && log.size() == wanted.size();
      },
      std::chrono::seconds(10)));
  EXPECT_EQ(peak_imports, 1U);
  std::sort(log.begin(), log.end());
  std::sort(wanted.begin(), wanted.end());
  EXPECT_EQ(log, wanted);
}

// cancelAll() while several meshes are downloading, waiting for the import slot or
// importing: none of them delivers, every slot drains, and later jobs load.
TEST(MeshLoadQueueTest, CancelAllWhileFetchesOverlapNeverDelivers) {
  constexpr int kMeshes = 8;
  const FileHttpServer server(stlFiles(kMeshes + 2));
  QObject context;
  MeshLoadQueue queue(&context, /*max_loads=*/4, /*max_imports=*/1);
  std::vector<std::string> log;
  for (int index = 0; index < kMeshes; ++index) {
    enqueue(queue, stlUrlSource(server, index), log);
  }
  queue.pump();
  ASSERT_TRUE(pumpUntil([&] { return !log.empty(); }, std::chrono::seconds(10)));
  queue.cancelAll();
  const std::size_t delivered_before = log.size();
  std::vector<std::string> wanted;
  for (int index = kMeshes; index < kMeshes + 2; ++index) {
    const MeshLoadQueue::Source source = stlUrlSource(server, index);
    enqueue(queue, source, log);
    wanted.push_back(source.location.toStdString() + ":ok");
  }
  queue.pump();
  EXPECT_TRUE(pumpUntil(
      [&] {
        EXPECT_LE(queue.importsRunning(), 1U);
        return queue.runningCount() == 0 && log.size() == delivered_before + wanted.size();
      },
      std::chrono::seconds(10)));
  std::vector<std::string> after(log.begin() + static_cast<std::ptrdiff_t>(delivered_before), log.end());
  std::sort(after.begin(), after.end());
  EXPECT_EQ(after, wanted) << "a cancelled job delivered, or a wanted one did not";
}
