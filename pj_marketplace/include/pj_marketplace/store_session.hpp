#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file store_session.hpp
 * @brief Canonical marketplace store layout and one-owner writer session.
 */

#include <QString>
#include <memory>
#include <optional>

#include "pj_base/expected.hpp"
#include "pj_marketplace/store_rejection.hpp"

namespace PJ {

/// On-disk naming conventions shared by the artifact store (which CREATES
/// these names) and profile-store garbage collection (which GATES deletion on
/// them). They must agree or GC deletes live staging / leaks it forever.
inline constexpr char kArtifactScratchPrefix[] = ".pj-artifact-";
inline constexpr char kArtifactManifestFileName[] = ".pj-artifact.json";

class StoreWriteLease;

/// Canonical store identity and its lifecycle-state suffix siblings.
class StoreLayout {
 public:
  StoreLayout(const StoreLayout&) = default;
  StoreLayout& operator=(const StoreLayout&) = default;
  StoreLayout(StoreLayout&&) noexcept = default;
  StoreLayout& operator=(StoreLayout&&) noexcept = default;

  /// Canonicalize the root once, prepare every sibling, and verify one volume.
  [[nodiscard]] static Expected<StoreLayout, StoreRejection> create(const QString& store_root);

  [[nodiscard]] const QString& storeRoot() const noexcept;
  [[nodiscard]] const QString& artifactsRoot() const noexcept;
  [[nodiscard]] const QString& extractionRoot() const noexcept;
  [[nodiscard]] const QString& profilesRoot() const noexcept;

 private:
  StoreLayout(QString store_root, QString artifacts_root, QString extraction_root, QString profiles_root);

  QString store_root_;
  QString artifacts_root_;
  QString extraction_root_;
  QString profiles_root_;
};

/// Shared lifetime for one canonical layout and, when available, its writer lease.
class StoreSession {
 public:
  /// Open the layout and try to acquire its lease. Contention yields a valid read-only session.
  [[nodiscard]] static Expected<StoreSession, StoreRejection> open(const QString& store_root);

  ~StoreSession();
  StoreSession(StoreSession&&) noexcept;
  StoreSession& operator=(StoreSession&&) noexcept;
  StoreSession(const StoreSession&) = delete;
  StoreSession& operator=(const StoreSession&) = delete;

  [[nodiscard]] const StoreLayout& layout() const noexcept;
  [[nodiscard]] bool hasStoreWriteAccess() const noexcept;
  [[nodiscard]] const StoreRejection* writeRefusal() const noexcept;

 private:
  StoreSession(StoreLayout layout, std::unique_ptr<StoreWriteLease> lease, std::optional<StoreRejection> refusal);

  StoreLayout layout_;
  std::unique_ptr<StoreWriteLease> lease_;
  std::optional<StoreRejection> refusal_;
};

}  // namespace PJ
