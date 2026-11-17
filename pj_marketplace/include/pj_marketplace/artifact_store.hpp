#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file artifact_store.hpp
 * @brief Write-once, content-addressed marketplace artifact storage.
 */

#include <QString>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_marketplace/generation_digest.hpp"
#include "pj_marketplace/store_policy.hpp"
#include "pj_marketplace/store_session.hpp"
#include "pj_marketplace/stored_file.hpp"

namespace PJ {

/// Transport-verified bytes handed to the immutable store without another archive read.
struct VerifiedArtifact {
  QString path;
  GenerationDigest digest;
};

/// Result of publishing or finding one object. `artifact_digest` is always the content-manifest digest (an aggregate
/// over `path\0sha256:<hex>\n` records), computed identically for extracted archives and adopted directories, so
/// identical bytes share one identity whatever their origin. The transport archive's own SHA-256, when one existed, is
/// kept as `transport_digest` (empty for adoptions): provenance, not identity.
struct StoredArtifact {
  std::string artifact_digest;
  std::string transport_digest;
  QString artifact_path;
  std::vector<StoredFile> files;
  /// Original archive wrapper removed from the object but retained for the initial archive-admission materialization.
  std::optional<std::string> stripped_archive_root;
  bool already_present = false;
};

/// Called with the object's content digest after hashing and BEFORE the publish rename makes it visible to garbage
/// collection, so a caller can register an in-flight GC root with no window in which a concurrent collection sees the
/// object unrooted. Runs on the calling thread; if the commit then fails, the caller releases whatever it registered.
using ArtifactDigestKnownHook = std::function<void(const GenerationDigest&)>;

/// Commit fault boundaries exposed for deterministic crash-consistency tests.
enum class ArtifactStoreCommitFault {
  kNone,
  kBeforeRename,
  kAfterRename,
};

/// Durable byte layer for marketplace generations. `<canonical-root>.artifacts`, `.extraction` and `.profiles` are
/// hidden suffix siblings of the canonical store root, so lifecycle state is never discovered as scanned content; all
/// must share its filesystem, because publication is a single directory rename, never a copy. Extracted files are
/// forced to durable storage before close and the artifacts parent after the rename; success is reported only after
/// both. Every mutation requires the PID-liveness writer lease shared with ExtensionManager.
class ArtifactStore {
 public:
  /// Uses a caller-owned session (which must outlive the store) and its already-acquired writer lease. QLockFile is not
  /// reentrant, even within one process, so ExtensionManager and ArtifactStore must share one session.
  explicit ArtifactStore(StoreSession& session, StoreQuotas quotas = {});

  /// Standalone convenience factory that opens and owns a store session.
  [[nodiscard]] static Expected<std::unique_ptr<ArtifactStore>, StoreRejection> acquire(
      const QString& store_root, StoreQuotas quotas = {});
  ~ArtifactStore();
  ArtifactStore(const ArtifactStore&) = delete;
  ArtifactStore& operator=(const ArtifactStore&) = delete;

  /// True only while the injected session holds the cooperative writer lease.
  [[nodiscard]] bool hasStoreWriteAccess() const noexcept;

  /// Classified lease refusal; null exactly when writable.
  [[nodiscard]] const StoreRejection* writeRefusal() const noexcept;

  /// Canonical managed-store identity from which every sibling is derived.
  [[nodiscard]] const QString& storeRoot() const noexcept;

  /// Write-once `<canonical-root>.artifacts/<digest>/` parent.
  [[nodiscard]] const QString& artifactsRoot() const noexcept;

  /// The store directory that holds (or would hold) the digest's object: the one home of the objects-root layout.
  /// Never derive "<artifactsRoot>/<hex>" yourself.
  [[nodiscard]] QString artifactRootFor(const GenerationDigest& digest) const;

  /// Operation-private extraction parent; never a runtime scan root.
  [[nodiscard]] const QString& extractionRoot() const noexcept;

  /// Validates, extracts and atomically publishes transport-verified bytes. The transport digest verifies arrival; the
  /// published identity is the content-manifest digest of the extracted files (see StoredArtifact). The sole directory
  /// wrapping every archive file is removed, so the object is descriptor-rooted like adopted directories. The
  /// extractor's incrementally computed file digests are reused and a cheap file-set/type/size check guards staging
  /// mutation. Files and the objects parent are forced to durable storage before success.
  [[nodiscard]] Expected<StoredArtifact, StoreRejection> commitArchive(
      const VerifiedArtifact& artifact, const std::atomic<bool>* cancel_requested = nullptr,
      const ArtifactDigestKnownHook& digest_known = {});

  /// Publishes already-resident bytes through the same hashed write-once tail. The source is never a runtime root:
  /// every regular file is copied into private staging, rehashed, manifested and atomically renamed into the
  /// artifact-addressed object namespace.
  [[nodiscard]] Expected<StoredArtifact, StoreRejection> adoptDirectory(
      const QString& directory, const std::atomic<bool>* cancel_requested = nullptr,
      const ArtifactDigestKnownHook& digest_known = {});

  /// Materializes an object this process just committed into an empty `destination`, skipping the resident
  /// re-verification: `artifact` carries the known-good file list and the bytes were written under this session's lease
  /// moments ago. The post-copy rehash still runs, so copy corruption is caught; `verify_copy_hashes=false` skips it
  /// for a throwaway materialization that is discarded before promotion. For an object of unknown age, call
  /// verifyArtifact() first.
  [[nodiscard]] Expected<StoredArtifact, StoreRejection> materializeArtifact(
      const StoredArtifact& artifact, const QString& destination, const std::atomic<bool>* cancel_requested = nullptr,
      bool verify_copy_hashes = true) const;

  /// Rehash and verify one resident artifact without mutating the store.
  [[nodiscard]] Expected<StoredArtifact, StoreRejection> verifyArtifact(const GenerationDigest& digest) const;

  /// Deletes every resident object whose digest is not in `live`: a mark-and-sweep GC, so `live` must be the COMPLETE
  /// set of still-referenced digests, or a live object is lost. Only canonical object directories are candidates:
  /// in-flight scratch directories and any non-digest name are left alone, and a directory that cannot be removed
  /// (e.g. a mapped file) waits for a later sweep. No-op without the writer lease. Returns the number of objects
  /// removed.
  size_t collectUnreferenced(const std::vector<GenerationDigest>& live);

  /// Test-only write interposition used to model a full or failed volume.
  void setExtractionWriteHookForTesting(std::function<bool(const std::filesystem::path&, const char*, size_t)> hook);

  /// Test-only interposition after extraction and before structural validation.
  void setBeforeCommitHookForTesting(std::function<void(const std::filesystem::path&)> hook);

  /// Test-only fault at either side of the one visibility-changing rename.
  void setCommitFaultForTesting(ArtifactStoreCommitFault fault) noexcept;

 private:
  ArtifactStore(std::unique_ptr<StoreSession> session, StoreQuotas quotas);

  std::unique_ptr<StoreSession> owned_session_;
  StoreSession& session_;
  StoreQuotas quotas_;
  std::function<bool(const std::filesystem::path&, const char*, size_t)> extraction_write_hook_;
  std::function<void(const std::filesystem::path&)> before_commit_hook_;
  ArtifactStoreCommitFault commit_fault_ = ArtifactStoreCommitFault::kNone;
};

}  // namespace PJ
