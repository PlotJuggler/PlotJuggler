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

/// Result of publishing or finding one artifact-addressed object generation.
///
/// `artifact_digest` is always the CONTENT-MANIFEST digest (an aggregate over
/// `path\0sha256\n` records), computed identically for extracted archives and
/// adopted directories — so identical bytes share one identity regardless of
/// how they arrived, and the #549 immutable-release check compares like with
/// like across origins. The transport archive's own SHA-256, when one existed,
/// is preserved separately as `transport_digest` (empty for adoptions); it is
/// provenance, not identity.
struct StoredArtifact {
  std::string artifact_digest;
  std::string transport_digest;
  QString artifact_path;
  std::vector<StoredFile> files;
  /// Original archive wrapper removed from the object but retained for the initial archive-admission materialization.
  std::optional<std::string> stripped_archive_root;
  bool already_present = false;
};

/// Called with the object's content digest after hashing and BEFORE the publish
/// rename makes it visible to garbage collection. Callers use this to register
/// an in-flight GC root with no window in which a concurrent collection could
/// observe the object unrooted. The callback runs on the calling thread; if the
/// commit subsequently fails the caller owns releasing whatever it registered.
using ArtifactDigestKnownHook = std::function<void(const GenerationDigest&)>;

/// Commit fault boundaries exposed for deterministic crash-consistency tests.
enum class ArtifactStoreCommitFault {
  kNone,
  kBeforeRename,
  kAfterRename,
};

/// Durable byte layer for marketplace generations.
///
/// `<canonical-root>.artifacts`, `<canonical-root>.extraction`, and
/// `<canonical-root>.profiles` are hidden suffix siblings of the canonical
/// store root, so lifecycle state can never be discovered as recursively
/// scanned content. All must share its filesystem: publication is a single
/// directory rename, never a recursive copy. Extracted files are forced to
/// durable storage before close and the artifacts parent is forced after rename;
/// success is reported only after both steps. Every mutation requires the same
/// PID-liveness writer lease used by ExtensionManager.
class ArtifactStore {
 public:
  /// Use a caller-owned session and its already-acquired writer lease.
  ///
  /// The session must outlive this store. QLockFile is not reentrant, even within
  /// one process, so ExtensionManager and ArtifactStore must share one session.
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

  /// Write-once `<canonical-root>.objects/<artifact-sha256>/` parent.
  [[nodiscard]] const QString& artifactsRoot() const noexcept;

  /// The store directory that holds (or would hold) the digest's object. The
  /// ONE home of the objects-root layout — callers must never derive
  /// "<artifactsRoot>/<hex>" themselves.
  [[nodiscard]] QString artifactRootFor(const GenerationDigest& digest) const;

  /// Operation-private extraction parent; never a runtime scan root.
  [[nodiscard]] const QString& extractionRoot() const noexcept;

  /// Validate, extract, and atomically publish transport-verified bytes.
  ///
  /// The transport digest verifies arrival; the published identity is the
  /// content-manifest digest of the extracted files (see StoredArtifact). The
  /// sole directory wrapping every archive file is removed so the object is
  /// descriptor-rooted like adopted directories. The extractor's incrementally
  /// computed file digests are reused, and a cheap file-set/type/size check
  /// guards staging mutation. Files and the objects parent are forced to durable
  /// storage before success.
  [[nodiscard]] Expected<StoredArtifact, StoreRejection> commitArchive(
      const VerifiedArtifact& artifact, const std::atomic<bool>* cancel_requested = nullptr,
      const ArtifactDigestKnownHook& digest_known = {});

  /// Publish already-resident legacy bytes through the same hashed write-once tail.
  ///
  /// The source is never made a special runtime root: every regular file is
  /// copied into private staging, rehashed, manifested, and atomically renamed
  /// into the ordinary artifact-addressed object namespace.
  [[nodiscard]] Expected<StoredArtifact, StoreRejection> adoptDirectory(
      const QString& directory, const std::atomic<bool>* cancel_requested = nullptr,
      const ArtifactDigestKnownHook& digest_known = {});

  /// Materialize an object this process just committed, skipping the resident
  /// re-verification: `object` already carries the known-good file list, and the
  /// bytes were written under this session's exclusive lease moments ago. The
  /// post-copy rehash still runs, so copy corruption is still caught. Use the
  /// digest overload for objects of unknown age (profile activation, repair).
  /// `verify_copy_hashes=false` skips the post-copy rehash for a THROWAWAY
  /// materialization (the helper's executed candidate, discarded before
  /// promotion): the helper independently rehashes what it inspects, so the
  /// extra pass certified a copy that is deleted moments later. The promoted
  /// copy must keep the default and be rehashed.
  [[nodiscard]] Expected<StoredArtifact, StoreRejection> materializeArtifact(
      const StoredArtifact& artifact, const QString& destination, const std::atomic<bool>* cancel_requested = nullptr,
      bool verify_copy_hashes = true) const;

  /// Rehash and verify one resident artifact without mutating the store.
  [[nodiscard]] Expected<StoredArtifact, StoreRejection> verifyArtifact(const GenerationDigest& digest) const;

  /// Delete every resident object whose digest is not in `live` (a mark-and-sweep
  /// GC; `live` must be the COMPLETE set of still-referenced digests, or a live
  /// object is lost). Only canonical object directories are candidates: in-flight
  /// scratch directories and any non-digest name are left untouched, and a
  /// directory that cannot be removed (e.g. a mapped file) is left for a later
  /// sweep. No-op without the writer lease. Returns the number of objects removed.
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
