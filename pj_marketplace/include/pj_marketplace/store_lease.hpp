#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file store_lease.hpp
 * @brief Shared single-writer lease and atomic-rename filesystem checks.
 */

#include <QString>
#include <memory>

#include "pj_base/expected.hpp"
#include "pj_marketplace/store_rejection.hpp"

class QLockFile;

namespace PJ {

/// Construction wait for the cooperative writer lease; contention becomes read-only promptly.
inline constexpr int kStoreLockTimeoutMilliseconds = 200;

/// Path of the lock-file sibling keyed by the canonical store identity.
[[nodiscard]] QString storeLockPath(const QString& canonical_store_root);

/// Derive a suffix-named sibling from the canonical store identity.
[[nodiscard]] QString storeSiblingPath(const QString& canonical_store_root, const QString& suffix);

/// True only when two existing paths share a volume and rename is atomic between them.
[[nodiscard]] bool sameFilesystem(const QString& first, const QString& second);

/// RAII ownership of the marketplace's one cooperative writer lease.
///
/// Staleness is based only on owner-PID liveness. Age-based stealing is disabled,
/// so a large live download cannot lose its store while it is still writing.
class StoreWriteLease {
 public:
  ~StoreWriteLease();
  StoreWriteLease(StoreWriteLease&&) noexcept;
  StoreWriteLease& operator=(StoreWriteLease&&) noexcept;
  StoreWriteLease(const StoreWriteLease&) = delete;
  StoreWriteLease& operator=(const StoreWriteLease&) = delete;

  /// Acquire the lock keyed by @p canonical_store_root or return a classified refusal.
  [[nodiscard]] static Expected<StoreWriteLease, StoreRejection> acquire(
      const QString& canonical_store_root, int timeout_milliseconds = kStoreLockTimeoutMilliseconds);

 private:
  explicit StoreWriteLease(std::unique_ptr<QLockFile> lock);

  std::unique_ptr<QLockFile> lock_;
};

}  // namespace PJ
