#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#ifdef __EMSCRIPTEN__
#error "SourceCacheStore is desktop-only (no persistent disk in the browser)"
#endif

#include <QString>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "pj_base/expected.hpp"
#include "pj_base/sdk/descriptor_import/request_cache.hpp"

namespace PJ {

/// Host-side source cache (RECORDING_AND_CACHING.md §3.4): one MCAP artifact
/// per provider request identity, pinned while a dataset reads it, LRU-evicted
/// under a byte budget, quarantined when it fails to load.
///
/// A thin wrapper over the SDK's RequestArtifactCache, which owns the
/// mechanics (per-identity cross-process locks, lease-then-validate lookup,
/// atomic publish, LRU stamps, cleanup that never touches leased files).
/// This class adds the host policy: a host-generic identity scheme (provider
/// identities are opaque strings, C1 — the store digests the STRING, so one
/// cache serves every provider), the recovery step after a retryable commit,
/// quarantine, and the default root/budget.
///
/// There is deliberately NO separate index file (C7): the artifact's
/// existence, its digest-derived filename and its LRU stamp are the index.
/// Losing the cache directory loses no data — every entry is a re-download.
///
/// The digest in an artifact's filename is over the identity STRING, never
/// the content (validation is a cheap structural check, not a re-hash). A
/// provider therefore owns making its identities CONTENT-DETERMINING: two
/// requests that can return different data must mint different identities,
/// or the cache will serve stale bytes as a hit.
///
/// The cross-process exclusion relies on advisory file locks; on network
/// filesystems where flock is a no-op (some NFS mounts) a cache root on a
/// network home directory loses that protection.
///
/// Threading: instances are cheap value-like handles over the filesystem;
/// methods are thread-safe against each other and other processes through
/// the per-identity locks. cleanup() scans the directory — call it from a
/// worker thread, never on the GUI thread and never inside a lookup path.
class SourceCacheStore {
 public:
  using ReadLease = sdk::descriptor_import::ReadLease;
  using WriteTransaction = sdk::descriptor_import::RequestArtifactCache::WriteTransaction;
  using CacheError = sdk::descriptor_import::CacheError;
  using CleanupResult = sdk::descriptor_import::CleanupResult;

  /// A cache hit: a validated artifact path plus the pin that keeps it
  /// un-evictable. Hold the pin for the DATASET's lifetime (never a UI
  /// object's); it is host-internal and must not cross any plugin ABI.
  struct Pinned {
    std::filesystem::path path;
    ReadLease pin;
  };

  /// 10 GB (C6). The cache is disposable; recordings live elsewhere and are
  /// never evicted.
  static constexpr std::uintmax_t kDefaultBudgetBytes = 10ull * 1000 * 1000 * 1000;

  /// `root` is created lazily by the first publish. `budget_bytes` bounds
  /// cleanup(); it is best-effort — pinned artifacts are never evicted, so
  /// staying over target is a reported, legitimate outcome.
  explicit SourceCacheStore(std::filesystem::path root, std::uintmax_t budget_bytes = kDefaultBudgetBytes);

  /// `<AppLocalDataLocation>/source_cache` — the cache's own folder, distinct
  /// from the recordings folder (C6).
  [[nodiscard]] static std::filesystem::path defaultRoot();

  /// The user-facing configuration (Preferences), mirroring
  /// RecordingService::Settings. The owner constructs the store from it ONCE
  /// at startup — a changed folder/budget takes effect on the next launch,
  /// because live captures hold the store they were armed against.
  struct Settings {
    QString directory;   ///< empty = defaultRoot()
    int budget_gb = 10;  ///< decimal GB, clamped on load to 1..1000
  };
  /// QSettings keys: Preferences::source_cache_directory,
  /// Preferences::source_cache_budget_gb (clamped on load).
  [[nodiscard]] static Settings loadSettings();
  static void saveSettings(const Settings& settings);
  /// A store built from `settings` (empty directory = defaultRoot()).
  /// unique_ptr because the store is not movable (the SDK cache is not).
  [[nodiscard]] static std::unique_ptr<SourceCacheStore> fromSettings(const Settings& settings);

  /// The artifact path `identity` would occupy; empty for an empty identity.
  /// Takes no position on existence.
  [[nodiscard]] std::filesystem::path pathFor(std::string_view identity) const;

  /// Pin-then-validate: the pin is taken BEFORE the artifact is checked, so
  /// an evictor cannot remove the file between the check and the use. A
  /// contended identity (someone is publishing or quarantining it right now)
  /// is a MISS with a retry hint in `miss_reason`, never an error and never
  /// an unpinned hit. A validator rejection reports why and leaves the file
  /// in place (the next publish renames over it).
  [[nodiscard]] std::optional<Pinned> lookup(std::string_view identity, std::string* miss_reason = nullptr);

  /// Begin materializing `identity`: write the artifact to the transaction's
  /// partialPath(), CLOSE it, then hand the transaction to publish() with the
  /// SAME identity (the pair is not checked — a mismatched identity publishes
  /// under one name and recovers under another). Retryable while the identity
  /// is pinned or being published elsewhere.
  [[nodiscard]] Expected<WriteTransaction, CacheError> beginPublish(std::string_view identity);

  /// Commit the transaction and return the published artifact, pinned. The
  /// SDK's commit can publish the file yet lose the lock handoff to a
  /// concurrent exclusive attempt; per its contract the recovery is exactly
  /// one lookup, performed here so callers never see that window.
  [[nodiscard]] Expected<Pinned, CacheError> publish(std::string_view identity, WriteTransaction&& txn);

  /// The heal path (C4): after a loader failure, evict the artifact (and its
  /// LRU stamp) so the next lookup is an absent miss and the next import
  /// re-downloads. Requires the identity's exclusive lock: when the artifact
  /// is pinned elsewhere the eviction is refused with a reason — reported,
  /// never forced. Idempotent for an absent artifact.
  bool quarantine(std::string_view identity, std::string* reason = nullptr);

  /// Evict oldest-first down to the budget, skipping pinned artifacts and
  /// in-flight publishes. Best-effort: `target_met=false` with
  /// `bytes_held_over_target` when pins hold the size above budget. Call
  /// from a worker thread.
  [[nodiscard]] CleanupResult cleanup();

  [[nodiscard]] const std::filesystem::path& root() const noexcept;

 private:
  sdk::descriptor_import::RequestArtifactCache cache_;
  std::uintmax_t budget_bytes_ = kDefaultBudgetBytes;
};

}  // namespace PJ
