#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#ifdef __EMSCRIPTEN__
#error "SourceCaptureService is desktop-only (it records into the on-disk source cache)"
#endif

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "pj_base/expected.hpp"
#include "pj_runtime/SourceCacheStore.h"

namespace PJ {

class DataSourceRuntimeHost;

/// The one derivation of a source-cache identity (RECORDING_AND_CACHING.md
/// §3.4): versioned and length-framed over the UNSPOOFABLE provider id (from
/// the host's plugin binding, never from plugin data) plus the exact canonical
/// descriptor bytes. Every producer and consumer of cache keys goes through
/// this function so the framing can never disagree between them. Matching is
/// byte-exact downstream (the store digests this string), so a provider
/// re-serializing the same request must emit identical descriptor bytes.
[[nodiscard]] std::string sourceCacheIdentity(std::string_view provider_id, std::string_view descriptor_json);

/// The versioned completion manifest a published artifact carries inside its
/// MCAP (`pj.capture` metadata): everything a later restore needs to validate
/// the artifact against a layout's source record without a provider call.
struct CaptureManifest {
  uint32_t version = 0;
  std::string provider_id;
  std::string identity;  ///< sourceCacheIdentity() of this artifact
  bool attests_empty_topics = false;
  /// The FULL requested set from the completion, with the messages this
  /// capture recorded per topic (0 allowed only under the attestation).
  std::map<std::string, uint64_t> requested_topic_messages;
  uint64_t total_messages = 0;
};

/// Captures one delegated-ingest download into the source cache and resolves
/// later restores of the same request from disk (M3's SourceCaptureService).
///
/// One instance per SourceCacheStore; one Capture per ingest context, BOUND
/// to that context: arm() hooks the host, and everything after — opening the
/// cache transaction, recording, finalizing — acts on that one host, so a
/// stale capture can never publish under a replacement ingest's evidence.
///
/// Recording is armed lazily, on the source's attach_source_record: the
/// identity is only known then, and the contract guarantees attachment
/// precedes the first push, so nothing is missed. The recorder writes
/// DIRECTLY into the cache transaction's partialPath() — a crash leaves the
/// partial under the store's own cleanup, never an orphan temp file — in
/// lossless kBlock mode (the host's stop request wakes blocked producers via
/// the on_stop_requested hook, satisfying #629's wake-before-join ordering).
///
/// Threading: arm/finalize on the owner's thread; the arming work runs on
/// the source's stream thread (inside attach_source_record); the tap on the
/// push threads. The Capture must not outlive its host, and finalize() must
/// run only after the source's producers have quiesced.
class SourceCaptureService {
 public:
  explicit SourceCaptureService(SourceCacheStore& store);

  /// One armed capture. Move-only; hand it back to finalize() (publishes or
  /// discards). Destruction without finalize unhooks the host, wakes any
  /// blocked producer and aborts the transaction (removing the partial) —
  /// and, like finalize(), requires the source's threads joined first.
  class Capture {
   public:
    Capture();
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

   private:
    friend class SourceCaptureService;
    struct Impl;
    std::shared_ptr<Impl> impl_;
  };

  /// Hook `host` for capture. Recording starts when the source attaches its
  /// descriptor; an ingest that never attaches simply records nothing. The
  /// budget bounds the lossless (kBlock) recorder queue: producers wait,
  /// never drop. `provider_id` comes from the plugin binding (unspoofable).
  /// A failure while arming ON ATTACH never affects ingest — it is surfaced
  /// as a finalize refusal instead.
  [[nodiscard]] std::unique_ptr<Capture> arm(
      DataSourceRuntimeHost& host, std::string provider_id, uint64_t queue_budget_bytes = 64ull * 1024 * 1024);

  /// Latch cancellation and wake producers blocked on the capture queue.
  /// The owner MUST call this (or the host's requestStop, which the capture
  /// hooks) on its cancellation path BEFORE joining the source's threads.
  /// The latch alone makes finalize() refuse, whatever the caller passes.
  static void cancel(Capture& capture);

  struct FinalizeResult {
    bool published = false;
    /// Why publication was refused (gate condition, recorder verdict, or a
    /// cache error). Empty when published.
    std::string reason;
    std::optional<SourceCacheStore::Pinned> artifact;
  };

  /// Evaluate the publication gate and publish or discard. Call AFTER the
  /// bound host's producers have quiesced. `transaction_committed` = the
  /// owning ingest transaction committed its dataset; `cancelled` = the
  /// owner cancelled by a path the capture could not observe. The gate
  /// publishes only when ALL of: no veto, an explicit COMPLETED terminal, an
  /// attached descriptor (with the capture successfully armed on it),
  /// committed, not cancelled (argument OR latch), zero latched callback
  /// failures, zero pure-lazy skips, ledger == the host's tap-eligible
  /// count, coverage against the declared set (zero-message topics only
  /// under the empty-topic attestation; nothing recorded outside the set;
  /// at least one message overall), and a clean recorder close whose written
  /// count equals the ledger. Ingested session data is never touched — a
  /// refusal only aborts the cache transaction.
  [[nodiscard]] FinalizeResult finalize(std::unique_ptr<Capture> capture, bool transaction_committed, bool cancelled);

  struct Resolved {
    SourceCacheStore::Pinned artifact;
    CaptureManifest manifest;
  };

  /// Cache-first restore: derive the identity, look up a pinned artifact,
  /// read its manifest back and STRICTLY verify the artifact against it
  /// (provider and identity match, per-topic message counts from the MCAP
  /// statistics equal the manifest, no undeclared channels, totals agree).
  /// An artifact that fails verification is quarantined (pin released
  /// first; a refusal because another reader holds a pin is reported as the
  /// store's reason). The caller loads the artifact through the stock MCAP
  /// loader; a LOAD failure afterwards is the caller's cue to quarantine.
  [[nodiscard]] std::optional<Resolved> resolve(
      std::string_view provider_id, std::string_view descriptor_json, std::string* miss_reason = nullptr);

  /// Read and validate the `pj.capture` manifest (exactly one record; exact
  /// version; typed, bounded fields; per-topic counts summing to the total).
  /// Public for the restore caller's re-verification and for tests.
  [[nodiscard]] static Expected<CaptureManifest> readManifest(const std::filesystem::path& artifact);

  [[nodiscard]] SourceCacheStore& store() noexcept {
    return store_;
  }

 private:
  SourceCacheStore& store_;
};

}  // namespace PJ
