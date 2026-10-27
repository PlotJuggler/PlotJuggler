// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "mesh_load_queue.h"

#include <QBuffer>
#include <QImageReader>
#include <QObject>
#include <QUrl>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "mesh_import.h"
#include "mesh_loader.h"
#include "pj_runtime/HttpUrl.h"
#include "pj_scene3d_core/scene_limits.h"
#include "url_fetcher.h"

namespace pj::scene3d {
namespace {

// Sniffs the header only: a body that is no image (an error page served with 200)
// would otherwise fail the whole mesh when the renderer decodes it.
bool isImage(const QByteArray& bytes) {
  QBuffer buffer;
  buffer.setData(bytes);
  return buffer.open(QIODevice::ReadOnly) && !QImageReader::imageFormat(&buffer).isEmpty();
}

// The URL a texture ref names relative to its mesh; empty for no ref or a
// non-network one (a remote mesh must not reach local files).
QUrl textureUrl(const QUrl& mesh_url, const QString& ref) {
  if (ref.isEmpty()) {
    return {};
  }
  const QUrl url = mesh_url.resolved(QUrl(ref));
  return PJ::isHttpUrl(url) ? url : QUrl{};
}

bool hasUnresolvedRef(const Material& material) {
  const auto textures = material.textureSlots();
  return std::any_of(textures.cbegin(), textures.cend(), [](const TextureSource* texture) {
    return !texture->unresolved_ref.isEmpty();
  });
}

// Repoints the submeshes at private, fillable copies of the materials that carry an
// unresolved ref (each distinct material once); the others stay shared.
std::vector<std::shared_ptr<Material>> detachMaterials(MeshData& mesh) {
  std::unordered_map<const Material*, std::shared_ptr<Material>> copies;
  std::vector<std::shared_ptr<Material>> materials;
  for (SubMesh& submesh : mesh.submeshes) {
    if (submesh.material == nullptr || !hasUnresolvedRef(*submesh.material)) {
      continue;
    }
    auto [copy, inserted] = copies.try_emplace(submesh.material.get());
    if (inserted) {
      copy->second = std::make_shared<Material>(*submesh.material);
      materials.push_back(copy->second);
    }
    submesh.material = copy->second;
  }
  return materials;
}

// Fills each slot whose URL (its `key`, set when the fetch started) was fetched; the
// others drop the key. Each filled slot keeps its own copy, so each is charged (after
// the mesh's other textures) to the per-mesh encoded-texture budget, as the loader
// charges embedded ones: the one place that budget bounds fetched textures.
void attachFetchedTextures(
    const MeshData& mesh, const std::vector<std::shared_ptr<Material>>& materials,
    const std::unordered_map<std::string, QByteArray>& fetched) {
  std::uint64_t charged = encodedTextureBytes(mesh);
  for (const std::shared_ptr<Material>& material : materials) {
    for (TextureSource* texture : material->textureSlots()) {
      if (texture->unresolved_ref.isEmpty()) {
        continue;
      }
      const auto body = fetched.find(texture->key);
      if (body != fetched.end() && !body->second.isEmpty()) {
        const std::uint64_t total = saturatedAdd(charged, static_cast<std::uint64_t>(body->second.size()));
        if (total <= kSceneLimits.max_model_encoded_texture_bytes_per_mesh) {
          charged = total;
          texture->bytes.assign(body->second.cbegin(), body->second.cend());
          continue;
        }
      }
      texture->key.clear();
    }
  }
}

}  // namespace

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
    if (outcome.mesh.ok && startTextureFetches(id, outcome.mesh)) {
      return;  // the last texture fetch settles the job and pumps
    }
  }
  settle(id, std::move(outcome));
  pump();
}

bool MeshLoadQueue::startTextureFetches(JobId id, MeshData& mesh) {
  Running& job = running_.at(id);
  if (job.source.kind != Source::Kind::kUrl) {
    return false;
  }
  const QUrl mesh_url(job.source.location);
  std::vector<std::shared_ptr<Material>> materials = detachMaterials(mesh);
  std::vector<std::string> urls;
  for (const std::shared_ptr<Material>& material : materials) {
    for (TextureSource* texture : material->textureSlots()) {
      const QUrl url = textureUrl(mesh_url, texture->unresolved_ref);
      if (url.isEmpty()) {
        continue;
      }
      texture->key = url.toString().toStdString();
      if (job.textures.try_emplace(texture->key).second) {
        urls.push_back(texture->key);
      }
    }
  }
  if (urls.empty()) {
    return false;
  }
  job.mesh = std::move(mesh);
  job.materials = std::move(materials);
  job.textures_pending = urls.size();
  for (const std::string& url : urls) {
    fetcher().fetch(QUrl(QString::fromStdString(url)), [this, id, url](FetchResult result) {
      onTextureFetched(id, url, std::move(result));
    });
  }
  return true;
}

void MeshLoadQueue::onTextureFetched(JobId id, const std::string& url, FetchResult result) {
  const auto running = running_.find(id);
  if (running == running_.end()) {
    return;  // delivered by a retired fetcher before its deferred delete
  }
  Running& job = running->second;
  if (result.ok && isImage(result.bytes)) {
    job.textures[url] = std::move(result.bytes);
  }
  if (--job.textures_pending > 0) {
    return;
  }
  Outcome outcome;
  if (job.done != nullptr) {
    outcome.mesh = std::move(job.mesh);
    attachFetchedTextures(outcome.mesh, job.materials, job.textures);
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
