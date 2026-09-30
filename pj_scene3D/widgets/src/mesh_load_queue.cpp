// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "mesh_load_queue.h"

#include <QObject>
#include <QUrl>
#include <algorithm>
#include <exception>

#include "mesh_import.h"
#include "mesh_loader.h"
#include "url_fetcher.h"

namespace pj::scene3d {

MeshLoadQueue::MeshLoadQueue(QObject* context) : context_(context), loader_(std::make_unique<MeshLoader>()) {}

MeshLoadQueue::~MeshLoadQueue() = default;

MeshLoadQueue::JobId MeshLoadQueue::enqueue(
    std::function<std::optional<Source>()> resolve, std::function<void(Outcome)> done) {
  const JobId id = next_id_++;
  queued_.push_back(Queued{.id = id, .resolve = std::move(resolve), .done = std::move(done)});
  return id;
}

void MeshLoadQueue::fetch(const QUrl& url, std::function<void(FetchResult)> done) {
  UrlFetcher* current = &fetcher();
  ++pending_fetches_;
  current->fetch(url, [this, current, done = std::move(done)](FetchResult result) {
    if (fetcher_.get() != current) {
      return;  // delivered by a retired fetcher before its deferred delete
    }
    --pending_fetches_;
    done(std::move(result));
  });
}

void MeshLoadQueue::cancel(JobId id) {
  if (std::erase_if(queued_, [id](const Queued& queued) { return queued.id == id; }) > 0) {
    return;
  }
  const auto running = running_.find(id);
  if (running == running_.end()) {
    return;
  }
  running->second.done = nullptr;
  if (running->second.import == nullptr) {
    abortObsoleteFetches();
  }
}

void MeshLoadQueue::cancelAll() {
  queued_.clear();
  bool fetching = pending_fetches_ > 0;
  for (auto& [id, job] : running_) {
    job.done = nullptr;
    fetching = fetching || job.import == nullptr;
  }
  if (fetching) {
    retireFetcher();
  }
}

void MeshLoadQueue::pump() {
  while (running_.size() < maxConcurrentMeshLoads() && !queued_.empty()) {
    Queued next = std::move(queued_.front());
    queued_.pop_front();
    std::optional<Source> source = next.resolve();
    if (!source.has_value()) {
      continue;
    }
    const bool fetch = source->kind == Source::Kind::kUrl;
    running_.emplace(next.id, Running{.done = std::move(next.done), .source = std::move(*source), .import = nullptr});
    if (fetch) {
      startFetch(next.id);
    } else {
      startImport(next.id, {});
    }
  }
}

void MeshLoadQueue::clearLoaderCache() {
  loader_->clearCache();
}

std::size_t MeshLoadQueue::importsRunning() const {
  return static_cast<std::size_t>(std::count_if(
      running_.begin(), running_.end(), [](const auto& entry) { return entry.second.import != nullptr; }));
}

UrlFetcher& MeshLoadQueue::fetcher() {
  if (fetcher_ == nullptr) {
    fetcher_ = std::make_unique<UrlFetcher>();
  }
  return *fetcher_;
}

void MeshLoadQueue::startFetch(JobId id) {
  fetcher().fetch(
      QUrl(running_.at(id).source.location), [this, id](FetchResult result) { onFetched(id, std::move(result)); });
}

void MeshLoadQueue::onFetched(JobId id, FetchResult result) {
  const auto running = running_.find(id);
  if (running == running_.end()) {
    return;  // delivered by a retired fetcher before its deferred delete
  }
  if (running->second.done == nullptr) {
    running_.erase(running);  // cancelled: its slot is free now
  } else if (!result.ok) {
    Outcome outcome;
    outcome.mesh.error = std::move(result.error);
    outcome.fetch_failed = true;
    settle(id, std::move(outcome));
  } else {
    startImport(id, std::move(result.bytes));
  }
  pump();
}

void MeshLoadQueue::startImport(JobId id, QByteArray bytes) {
  Running& job = running_.at(id);
  QString error;
  try {
    const Source& source = job.source;
    QFuture<MeshData> future =
        source.kind == Source::Kind::kPath
            ? loader_->load(source.location, source.flip_override)
            : loader_->loadFromMemory(
                  source.kind == Source::Kind::kBytes ? source.bytes : bytes, source.format_hint, source.flip_override);
    job.source.bytes = {};  // the worker holds its own reference
    job.import = std::make_unique<MeshImport>(
        std::move(future), context_, [this, id]() { onImported(id); }, source.kind == Source::Kind::kPath);
    return;
  } catch (const std::exception& exception) {
    error = QObject::tr("could not start import: %1").arg(QString::fromUtf8(exception.what()));
  } catch (...) {
    error = QObject::tr("could not start import");
  }
  Outcome outcome;
  outcome.mesh.error = std::move(error);
  settle(id, std::move(outcome));
}

void MeshLoadQueue::onImported(JobId id) {
  const auto running = running_.find(id);
  if (running == running_.end()) {
    return;
  }
  // Destroyed after settle(): the watcher defers its own deletion, since this
  // runs inside its finished() emission.
  const std::unique_ptr<MeshImport> import = std::move(running->second.import);
  Outcome outcome;
  if (running->second.done != nullptr) {
    outcome.mesh = import->take();
    if (!outcome.mesh.ok && outcome.mesh.error.isEmpty()) {
      outcome.mesh.error = QObject::tr("mesh contains no renderable geometry");
    }
  }
  settle(id, std::move(outcome));
  pump();
}

void MeshLoadQueue::settle(JobId id, Outcome outcome) {
  const auto running = running_.find(id);
  if (running == running_.end()) {
    return;
  }
  const std::function<void(Outcome)> done = std::move(running->second.done);
  running_.erase(running);
  if (done != nullptr) {
    done(std::move(outcome));
  }
}

void MeshLoadQueue::abortObsoleteFetches() {
  if (pending_fetches_ > 0) {
    return;
  }
  bool fetching = false;
  for (const auto& [id, job] : running_) {
    if (job.import != nullptr) {
      continue;
    }
    if (job.done != nullptr) {
      return;  // still wanted: the obsolete ones complete and are ignored
    }
    fetching = true;
  }
  if (fetching) {
    retireFetcher();
  }
}

void MeshLoadQueue::retireFetcher() {
  if (fetcher_ == nullptr) {
    return;
  }
  // Deferred: this can run inside one of the fetcher's own callbacks. Its
  // destruction aborts the requests; a delivery before that finds no running job
  // and returns. Parented to the context so a context destroyed first takes the
  // fetcher and its queued deliveries along.
  UrlFetcher* obsolete = fetcher_.release();
  obsolete->setParent(context_);
  obsolete->deleteLater();
  pending_fetches_ = 0;
  std::erase_if(running_, [](const auto& entry) { return entry.second.import == nullptr; });
}

}  // namespace pj::scene3d
