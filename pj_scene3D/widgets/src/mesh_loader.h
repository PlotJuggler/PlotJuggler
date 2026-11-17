#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// CPU-side async mesh loader: imports STL / Collada(DAE) / OBJ / glTF via assimp on a worker thread
// (QtConcurrent::run) and returns a QFuture<MeshData> of interleaved vertices + indices. No GPU work here: the view's
// model pass owns buffer upload. Results are cached by resolved path so a mesh referenced by several links loads once.

#include <QByteArray>
#include <QFuture>
#include <QHash>
#include <QMutex>
#include <QString>
#include <optional>

#include "pj_scene3d_widgets/mesh_data.h"

namespace pj::scene3d {

// Loader with a path-keyed cache; holds no GPU state. Both entry points run assimp on a QtConcurrent thread and hand
// back a QFuture (a test may call .result() to block).
class MeshLoader {
 public:
  MeshLoader() = default;

  // Loads from a resolved filesystem path; a cached future when the same (path, effective-flip) pair was loaded
  // before. assimp infers the format from the extension. `flip_override`, when set, forces the Y-up -> Z-up flip
  // on/off instead of the per-format default (wantsZUpFlip); RobotModelLayer passes false to honor the COLLADA "ignore
  // up_axis" toggle. The effective flip is part of the cache key, so toggling it yields a distinct entry.
  QFuture<MeshData> load(const QString& resolved_path, std::optional<bool> flip_override = std::nullopt);

  // Loads from an in-memory buffer (embedded glTF path). `format_hint` is an assimp extension hint without the dot
  // ("glb", "gltf", "dae") so assimp can pick the importer for a headerless buffer. External texture refs are left in
  // TextureSource::unresolved_ref (no directory to resolve against).
  QFuture<MeshData> loadFromMemory(
      const QByteArray& bytes, const QString& format_hint, std::optional<bool> flip_override = std::nullopt);

  void clearCache();

  // Drops one cached path so the next load() re-imports it, evicting BOTH flip variants (the cache is keyed by
  // path+effective-flip). Callers evict failed loads, or a Retry would be handed the cached failure forever. An
  // in-flight future keeps running detached; its result is simply no longer shared.
  void evict(const QString& resolved_path);

  // Synchronous workers, exposed for unit tests and reused by the async paths. `flip_to_z_up` applies the explicit Y-up
  // -> Z-up (+90deg about X) rotation needed for glTF and Collada (see wantsZUpFlip); STL/OBJ pass through.
  static MeshData importFromFile(const QString& path, bool flip_to_z_up);
  static MeshData importFromMemory(const QByteArray& bytes, const QString& format_hint, bool flip_to_z_up);

 private:
  static bool wantsZUpFlip(const QString& format_hint);

  // Path-keyed cache of in-flight and completed loads. Mutex-guarded: the entry points run on the GUI thread while
  // earlier loads still run on QtConcurrent workers.
  QMutex cache_mutex_;
  QHash<QString, QFuture<MeshData>> cache_;
};

}  // namespace pj::scene3d
