// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include "pj_scene3d_widgets/mesh_data.h"

class QObject;
class QUrl;

namespace pj::scene3d {

class MeshImport;
class MeshLoader;
class UrlFetcher;
struct FetchResult;

// Bounded fetch -> import scheduling for ONE model layer: at most
// maxConcurrentMeshLoads() jobs fetch or import at once. What to load, whether
// to load it at all (remote-fetch permission, source-size caps) and whether a
// result fits the layer's byte budget stay with the layer; the queue only runs
// the jobs it is handed and delivers the results that are still wanted.
//
// Invariants:
//  - A cancelled job never reaches its `done`. A cancelled running import keeps
//    its slot until its worker finishes (the worker cannot be stopped), so
//    replacing sources never runs more than the bound at once.
//  - A cancelled fetch keeps its slot until it completes, except that once no
//    running fetch is still wanted the fetcher is retired: its requests abort
//    and their slots free immediately. cancelAll() retires it whenever it has a
//    request in flight, fetch() ones included.
//  - `done` runs on the GUI thread through the event loop, except when an
//    import cannot even be started: that failure is delivered from inside
//    pump(). The queue pumps itself after every settled job.
//  - GUI-thread only. Owned by `context`'s object (a layer member): the retired
//    fetchers are parented to `context`, so they die with it.
class MeshLoadQueue {
 public:
  using JobId = std::uint64_t;

  // What a job imports once it holds a slot.
  struct Source {
    enum class Kind : std::uint8_t {
      kPath,   // a local file, imported through MeshLoader::load (cached by path)
      kBytes,  // `bytes` in hand
      kUrl,    // fetched through UrlFetcher, then imported from memory
    };
    Kind kind = Kind::kPath;
    QString location;  // path (kPath) or URL (kUrl)
    QByteArray bytes;  // kBytes only
    QString format_hint;
    std::optional<bool> flip_override;
  };

  // A settled job. On failure `mesh.ok` is false and `mesh.error` says why.
  struct Outcome {
    MeshData mesh;
    bool fetch_failed = false;  // failed while fetching, before any import
  };

  explicit MeshLoadQueue(QObject* context);
  ~MeshLoadQueue();
  MeshLoadQueue(const MeshLoadQueue&) = delete;
  MeshLoadQueue& operator=(const MeshLoadQueue&) = delete;

  // Queue one job; it starts on a later pump(). `resolve` runs when the job
  // gets a slot and returns its source (resolved late, so queued jobs hold no
  // bytes); nullopt drops the job without `done`. `done` runs once with the
  // outcome unless the job is cancelled or the queue destroyed first.
  JobId enqueue(std::function<std::optional<Source>()> resolve, std::function<void(Outcome)> done);
  // Fetches `url` through the jobs' fetcher, outside the bound and without an
  // import: a model's description then shares the connections and cache its
  // meshes use. `done` runs once through the event loop unless cancelAll(), the
  // queue's destruction or a retirement intervenes first; while it is pending,
  // cancel() never retires the fetcher.
  void fetch(const QUrl& url, std::function<void(FetchResult)> done);
  // Unknown and already-settled ids are ignored.
  void cancel(JobId id);
  // Drops every queued job and every pending fetch() callback, and clears the
  // `done` of running jobs (their workers/requests still hold slots). Retires the
  // fetcher when any request is in flight, freeing the fetching jobs' slots.
  void cancelAll();
  // Start queued jobs while fewer than maxConcurrentMeshLoads() run.
  void pump();
  // Drops the loader's cached imports (a reload must re-import).
  void clearLoaderCache();

  // Running jobs, cancelled ones included: what the bound limits.
  [[nodiscard]] std::size_t runningCount() const {
    return running_.size();
  }
  // Imports whose worker has not finished, cancelled ones included.
  [[nodiscard]] std::size_t importsRunning() const;

 private:
  struct Queued {
    JobId id = 0;
    std::function<std::optional<Source>()> resolve;
    std::function<void(Outcome)> done;
  };
  struct Running {
    std::function<void(Outcome)> done;  // empty once cancelled
    Source source;
    std::unique_ptr<MeshImport> import;  // null while fetching
  };

  // The shared fetcher, created on first use and again after a retirement.
  UrlFetcher& fetcher();
  // Begins fetching a running kUrl job's location.
  void startFetch(JobId id);
  // Fetch completion: frees a cancelled job's slot, settles a failure, else
  // starts the import. Ignores jobs already dropped by a retirement.
  void onFetched(JobId id, FetchResult result);
  // Starts the import of a running job; an import that cannot start settles it.
  void startImport(JobId id, QByteArray bytes);
  // Import completion: settles the job with the worker's mesh (or, if the job was
  // cancelled, frees its slot without delivering).
  void onImported(JobId id);
  // Removes a running job and hands `outcome` to its `done` (if still wanted).
  void settle(JobId id, Outcome outcome);
  // Retires the fetcher once none of its running fetches is still wanted.
  void abortObsoleteFetches();
  // Aborts every request of the fetcher and frees the fetching jobs' slots.
  void retireFetcher();

  QObject* context_;
  std::unique_ptr<MeshLoader> loader_;
  std::unique_ptr<UrlFetcher> fetcher_;
  int pending_fetches_ = 0;  // fetch() calls the current fetcher still owes
  std::deque<Queued> queued_;
  std::unordered_map<JobId, Running> running_;
  JobId next_id_ = 1;
};

}  // namespace pj::scene3d
