// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QFuture>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QThread>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "pj_scene3d_core/model_budget.h"
#include "pj_scene3d_widgets/mesh_data.h"

namespace pj::scene3d {

// How many mesh fetches + imports one layer keeps in flight. The browser has a
// single worker, so one; desktop overlaps a few CPU-bound assimp imports. Each
// result is admitted against the byte budgets when it lands, on the GUI thread,
// so parallelism never lets a layer exceed them.
[[nodiscard]] inline std::size_t maxConcurrentMeshLoads() {
#ifdef PJ_TARGET_WASM
  return 1;
#else
  return static_cast<std::size_t>(std::clamp(QThread::idealThreadCount(), 1, 4));
#endif
}

// Encoded texture bytes a mesh holds, each shared material counted once.
[[nodiscard]] inline std::uint64_t encodedTextureBytes(const MeshData& mesh) {
  std::uint64_t texture_bytes = 0;
  std::unordered_set<const Material*> counted;
  for (const SubMesh& submesh : mesh.submeshes) {
    if (submesh.material == nullptr || !counted.insert(submesh.material.get()).second) {
      continue;
    }
    for (const TextureSource* texture : submesh.material->textureSlots()) {
      texture_bytes = saturatedAdd(texture_bytes, texture->bytes.size());
    }
  }
  return texture_bytes;
}

// Retained CPU footprint of an imported mesh (geometry + encoded textures);
// kSaturated when the estimate overflows.
[[nodiscard]] inline std::uint64_t estimateRetainedMeshBytes(const MeshData& mesh) {
  return modelRetainedByteCount(
             mesh.vertices.size(), sizeof(Vertex), mesh.indices.size(), mesh.submeshes.size(), sizeof(SubMesh),
             encodedTextureBytes(mesh))
      .value_or(kSaturated);
}

// One off-thread MeshLoader import watched from the GUI thread. `on_done` runs
// once through `context`'s event loop when the future finishes (also when it had
// already finished). Destroying the import drops the callback; the worker keeps
// running detached and its result is discarded.
class MeshImport {
 public:
  // `shared`: other holders read the same future (MeshLoader's path cache), so
  // take() copies the result instead of moving it out.
  MeshImport(QFuture<MeshData> future, QObject* context, std::function<void()> on_done, bool shared = true)
      : future_(std::move(future)), watcher_(new QFutureWatcher<MeshData>()), shared_(shared) {
    // Connected before setFuture so an already-finished (cached) load still fires.
    QObject::connect(watcher_, &QFutureWatcher<MeshData>::finished, context, std::move(on_done));
    watcher_->setFuture(future_);
  }
  ~MeshImport() {
    // Deferred: the owner typically destroys the import from inside the
    // watcher's own finished() emission.
    watcher_->disconnect();
    watcher_->deleteLater();
  }
  MeshImport(const MeshImport&) = delete;
  MeshImport& operator=(const MeshImport&) = delete;

  // The imported mesh, once, after `on_done`: moved out of an unshared future,
  // copied out of a shared one. A stored worker exception comes back as an
  // error MeshData instead of being rethrown on the GUI thread.
  [[nodiscard]] MeshData take() {
    static_assert(std::is_nothrow_move_constructible_v<MeshData>, "takeResult() would copy");
    MeshData mesh;
    try {
      mesh = shared_ ? future_.result() : future_.takeResult();
    } catch (const std::exception& error) {
      mesh.error = QObject::tr("mesh import threw: %1").arg(QString::fromUtf8(error.what()));
    } catch (...) {
      mesh.error = QObject::tr("mesh import threw an unknown exception");
    }
    return mesh;
  }

 private:
  QFuture<MeshData> future_;
  QFutureWatcher<MeshData>* watcher_;
  bool shared_;
};

}  // namespace pj::scene3d
