// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_scene3d_widgets/mesh_data.h"
#include "url_fetcher.h"

class QObject;

namespace pj::scene3d {

// The QSettings bool "pj_scene3d/allow_remote_model_fetch", default on (datasets routinely name their meshes by URL;
// a fetch is bounded by UrlFetcher's redirect/size/timeout limits): whether model content may reach any http(s)
// origin. Off, a model reaches only the origins its user chose. Layers read it once per newly seen model source and
// pass it as Source::allow_remote_fetch.
[[nodiscard]] bool remoteModelFetchAllowed();

class MeshImport;
class MeshLoader;
class UrlFetcher;
struct FetchResult;

// Bounded fetch -> import scheduling for ONE model layer: at most `max_loads` jobs are in flight and at most
// `max_imports` of them import at once, so downloads overlap their latency while imports stay bounded. Only kUrl jobs
// use the load slots beyond the import ones: a kPath or kBytes job leaves the queue only for a free import slot. Jobs
// leave the queue in enqueue order (one at its head waiting for an import slot holds back those behind it), and fetched
// jobs import in enqueue order. What to load, whether to load at all (remote-fetch permission, source-size caps) and
// whether a result fits the layer's byte budget stay with the layer; the queue runs the jobs it is handed, gates
// every http(s) request a job makes by its Source's remote-fetch policy, and delivers the results still wanted.
//
// Invariants:
//  - Source bytes not yet imported are held only by fetched kUrl jobs waiting for an import slot, so by at most
//    `max_loads` jobs; queued jobs hold none. A job keeps its load slot until it settles, so a slow import throttles
//    the downloads behind it.
//  - A cancelled job never reaches its `done`. A cancelled running import keeps its slots until its worker finishes
//    (the worker cannot be stopped), so replacing sources never runs more than the bounds at once. A cancelled job
//    waiting for an import slot drops its bytes and frees its slot at once.
//  - A cancelled fetch keeps its slot while another running fetch is still wanted or a fetch() is pending; once neither
//    holds (checked on cancel(), on every pump() and when a fetch() lands), the fetcher is retired: its requests abort
//    and their slots free at once. cancelAll() retires it whenever a request is in flight, fetch() ones included.
//  - `done` runs on the GUI thread through the event loop, except when an import cannot even be started: that failure
//    is delivered from inside pump(). The queue pumps itself after every landed fetch, finished import and settled job.
//  - A kUrl job also fetches the external textures its mesh left unresolved (TextureSource::unresolved_ref, resolved
//    against the mesh URL, http(s) only, each distinct URL once) through the same fetcher before it settles, holding
//    its load slot (not an import slot) meanwhile. A texture that fails to fetch, is not an image or exceeds the
//    per-mesh encoded-texture budget leaves its slot empty; it never fails the mesh.
//  - GUI-thread only. Owned by `context`'s object (a layer member); the retired fetchers are parented to `context`
//    and die with it.
class MeshLoadQueue {
 public:
  using JobId = std::uint64_t;

  struct Source {
    enum class Kind : std::uint8_t {
      kPath,   // a local file, imported through MeshLoader::load (cached by path)
      kBytes,  // `bytes` in hand
      kUrl,    // fetched through UrlFetcher, then imported from memory
    };
    Kind kind = Kind::kPath;
    QString location;  // path (kPath) or URL (kUrl)
    QByteArray bytes;  // kBytes: handed in; kUrl: the fetched body while waiting for an import slot
    QString format_hint;
    std::optional<bool> flip_override;
    // The remote-fetch policy, applied to every http(s) request of the job (the mesh at an http(s) `location` and
    // each texture it fetches): with `allow_remote_fetch` off, only a URL on one of `allowed_origins` (scheme, host
    // and port; the source the user chose) is requested, and a redirect off that URL's origin fails it
    // (RemoteFetch::kOwnOrigin). A refused mesh settles as a fetch failure; a refused texture leaves its slot empty, as
    // a texture that fails to fetch does.
    bool allow_remote_fetch = false;
    std::vector<QUrl> allowed_origins;
  };

  struct Outcome {
    MeshData mesh;
    bool fetch_failed = false;  // failed while fetching, before any import
  };

  // Bounds: maxConcurrentMeshLoads() and maxConcurrentMeshImports().
  explicit MeshLoadQueue(QObject* context);
  // Requires 1 <= max_imports <= max_loads.
  MeshLoadQueue(QObject* context, std::size_t max_loads, std::size_t max_imports);
  ~MeshLoadQueue();
  MeshLoadQueue(const MeshLoadQueue&) = delete;
  MeshLoadQueue& operator=(const MeshLoadQueue&) = delete;

  // Queues one job; it starts on a later pump(). `kind` is the kind `resolve` must return (asserted): it decides which
  // slots the job waits for. `resolve` runs when the job gets a slot and returns its source (late, so queued jobs hold
  // no bytes); nullopt drops the job without `done`. `done` runs once with the outcome unless the job is cancelled or
  // the queue destroyed first.
  JobId enqueue(Source::Kind kind, std::function<std::optional<Source>()> resolve, std::function<void(Outcome)> done);
  // Fetches `url` through the jobs' fetcher, outside the bound and without an import: a model's description then shares
  // the connections and cache its meshes use. An http(s) `url` is requested only with `allow_remote_fetch`. `done`
  // runs once through the event loop unless cancelAll(), the queue's destruction or a retirement intervenes first;
  // while it is pending, cancel() never retires the fetcher.
  void fetch(const QUrl& url, bool allow_remote_fetch, std::function<void(FetchResult)> done);
  void cancel(JobId id);
  // Drops every queued job, every job waiting for an import slot and every pending
  // fetch() callback, and clears the `done` of running jobs (their workers/requests
  // still hold slots). Retires the fetcher when any request is in flight, freeing
  // the fetching jobs' slots.
  void cancelAll();
  void pump();
  void clearLoaderCache();

  // Running jobs, cancelled ones included: what `max_loads` limits.
  [[nodiscard]] std::size_t runningCount() const {
    return running_.size();
  }
  // Imports whose worker has not finished, cancelled ones included: what `max_imports` limits.
  [[nodiscard]] std::size_t importsRunning() const;
  // Jobs holding their source bytes until an import slot frees.
  [[nodiscard]] std::size_t waitingCount() const;

 private:
  struct Queued {
    JobId id = 0;
    Source::Kind kind = Source::Kind::kPath;
    std::function<std::optional<Source>()> resolve;
    std::function<void(Outcome)> done;
  };
  enum class Phase : std::uint8_t {
    kFetching,   // the mesh request is in flight
    kWaiting,    // holds its bytes until an import slot frees
    kImporting,  // `import`'s worker runs
    kTextures,   // imported; its texture requests are in flight
  };
  struct Running {
    std::function<void(Outcome)> done;  // empty once cancelled
    Source source;
    Phase phase = Phase::kFetching;
    std::unique_ptr<MeshImport> import;
    MeshData mesh{};  // imported, waiting for its textures
    std::vector<std::shared_ptr<Material>> materials{};
    std::unordered_map<std::string, QByteArray> textures{};
    std::size_t textures_pending = 0;
  };

  UrlFetcher& fetcher();
  void startFetch(JobId id);
  // Fetch completion: frees a cancelled job's slot, settles a failure, else
  // queues the import. Ignores jobs already dropped by a retirement.
  void onFetched(JobId id, FetchResult result);
  void startImport(JobId id, Running& job);
  void onImported(JobId id);
  // A kUrl job's import landed: starts fetching the textures `mesh` left unresolved, taking `mesh` into the job. False,
  // leaving `mesh` with the caller, when none is fetchable.
  bool startTextureFetches(JobId id, MeshData& mesh);
  void onTextureFetched(JobId id, const std::string& url, FetchResult result);
  void settle(JobId id, Outcome outcome);
  void abortObsoleteFetches();
  void retireFetcher();

  QObject* context_;
  std::size_t max_loads_;
  std::size_t max_imports_;
  std::unique_ptr<MeshLoader> loader_;
  std::unique_ptr<UrlFetcher> fetcher_;
  int pending_fetches_ = 0;  // fetch() calls the current fetcher still owes
  std::deque<Queued> queued_;
  std::unordered_map<JobId, Running> running_;
  JobId next_id_ = 1;
};

// How `source`'s job may request `url`: anything with the remote-fetch setting on, else only its own origins, and
// then without leaving the origin through a redirect.
[[nodiscard]] RemoteFetch remoteFetchFor(const MeshLoadQueue::Source& source, const QUrl& url);

}  // namespace pj::scene3d
