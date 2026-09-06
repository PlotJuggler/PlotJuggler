// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/SourceCaptureService.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <limits>
#include <mcap/reader.hpp>
#include <mutex>
#include <nlohmann/json.hpp>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/McapRecordingWriter.h"
#include "pj_runtime/Recorder.h"
#include "pj_runtime/RecordingFormat.h"
#include "pj_runtime/RecordingSink.h"

namespace PJ {

namespace fs = std::filesystem;

std::string sourceCacheIdentity(std::string_view provider_id, std::string_view descriptor_json) {
  // Length-framing makes the concatenation injective; the version token makes
  // a future framing change mint disjoint identities instead of aliasing.
  std::string identity;
  identity.reserve(provider_id.size() + descriptor_json.size() + 32);
  identity += "pj.source.v1|";
  identity += std::to_string(provider_id.size());
  identity += '|';
  identity += provider_id;
  identity += '|';
  identity += descriptor_json;
  return identity;
}

std::string sourceCacheIdentityDigest(std::string_view provider_id, std::string_view descriptor_json) {
  const std::string identity = sourceCacheIdentity(provider_id, descriptor_json);
  const QByteArray digest =
      QCryptographicHash::hash(
          QByteArrayView(identity.data(), static_cast<qsizetype>(identity.size())), QCryptographicHash::Sha256)
          .toHex()
          .left(32);
  return "sha256/128:" + std::string(digest.constData(), static_cast<std::size_t>(digest.size()));
}

namespace {

/// Coverage ledger shared between the counting tap (push threads) and
/// finalize (owner thread, after producers quiesced). Keyed by binding id so
/// the per-message path is an integer hash increment — the topic string is
/// captured once, on the binding's first message (binding ids are stable per
/// host, the same fact the recorder's channel table relies on).
struct CaptureLedger {
  std::mutex mu;
  std::unordered_map<uint32_t, uint64_t> messages_by_binding;
  std::unordered_map<uint32_t, std::string> topic_by_binding;
  uint64_t total = 0;

  /// Fold bindings into per-topic counts (a re-typed topic can span two
  /// binding ids). Owner thread, after producers quiesced.
  [[nodiscard]] std::map<std::string, uint64_t> messagesByTopic() {
    std::lock_guard<std::mutex> lock(mu);
    std::map<std::string, uint64_t> by_topic;
    for (const auto& [binding_id, count] : messages_by_binding) {
      by_topic[topic_by_binding.at(binding_id)] += count;
    }
    return by_topic;
  }
};

/// Counts coverage, then forwards to the recorder's tap. Counting first means
/// a message the recorder later fails on is still "seen" — the recorder's own
/// truncation verdict vetoes publication anyway.
class CountingTap final : public RecordTap {
 public:
  CountingTap(std::shared_ptr<CaptureLedger> ledger, std::shared_ptr<RecordTap> inner)
      : ledger_(std::move(ledger)), inner_(std::move(inner)) {}

  TapVerdict onMessage(
      uint32_t binding_id, const RecordedBindingView& binding, int64_t log_time_ns,
      Span<const uint8_t> bytes) override {
    {
      std::lock_guard<std::mutex> lock(ledger_->mu);
      ledger_->topic_by_binding.try_emplace(binding_id, binding.topic);
      ledger_->messages_by_binding[binding_id]++;
      ledger_->total++;
    }
    return inner_->onMessage(binding_id, binding, log_time_ns, bytes);
  }

 private:
  std::shared_ptr<CaptureLedger> ledger_;
  std::shared_ptr<RecordTap> inner_;
};

std::string manifestJson(const CaptureManifest& manifest) {
  nlohmann::json topics = nlohmann::json::object();
  for (const auto& [topic, count] : manifest.requested_topic_messages) {
    topics[topic] = count;
  }
  const nlohmann::json body{
      {"version", manifest.version},
      {"provider_id", manifest.provider_id},
      {"identity", manifest.identity},
      {"attests_empty_topics", manifest.attests_empty_topics},
      {"requested_topic_messages", std::move(topics)},
      {"total_messages", manifest.total_messages},
  };
  return body.dump();
}

}  // namespace

/// Everything one armed capture owns. `mu` orders the stream-thread arming
/// (onAttached) against the owner thread (cancel/finalize/destruction).
struct SourceCaptureService::Capture::Impl {
  SourceCacheStore* store = nullptr;
  DataSourceRuntimeHost* host = nullptr;  ///< the bound generation; must outlive this capture
  std::string provider_id;
  uint64_t queue_budget_bytes = 0;
  SourceCaptureService::SinkFactory sink_factory;  ///< null = McapRecordingWriter

  std::mutex mu;
  std::string identity;                                   ///< set on the arming attach
  std::optional<SourceCacheStore::WriteTransaction> txn;  ///< the partial the recorder writes into
  std::shared_ptr<Recorder> recorder;
  std::shared_ptr<CaptureLedger> ledger;
  std::shared_ptr<RecordTap> installed_tap_;  ///< OUR tap, for the conditional drop
  std::string arming_error;                   ///< a failure while arming on the stream thread; surfaced at finalize
  bool cancelled = false;
  bool finalized = false;

  /// Stream thread, from the host's on_source_record_attached hook. A
  /// replacement attach (different bytes, before the first push) tears the
  /// previous arming down and re-arms under the new identity.
  void onAttached(std::string_view descriptor, bool replaced) {
    std::lock_guard<std::mutex> lock(mu);
    if (finalized) {
      return;
    }
    if (replaced) {
      disarmLocked();
    }
    if (recorder != nullptr) {
      return;  // idempotent re-attach of identical bytes
    }
    arming_error.clear();
    identity = sourceCacheIdentity(provider_id, descriptor);
    auto transaction = store->beginPublish(identity);
    if (!transaction) {
      arming_error = "cache transaction refused: " + transaction.error().message;
      return;
    }
    RecorderOptions options;
    options.path = transaction->partialPath();
    options.queue_budget_bytes = queue_budget_bytes;
    options.overflow_policy = RecorderOptions::OverflowPolicy::kBlock;
    options.info.capture_id = "source-cache";
    options.info.source_display_name = provider_id;
    options.info.source_plugin_id = provider_id;
    options.info.capture_ordinal = 1;
    std::unique_ptr<RecordingSink> sink =
        sink_factory ? sink_factory() : std::unique_ptr<RecordingSink>(std::make_unique<McapRecordingWriter>());
    if (sink == nullptr) {
      arming_error = "the capture sink factory returned no sink";
      transaction->abort();
      return;
    }
    auto candidate = std::make_shared<Recorder>(std::move(sink), options);
    if (auto started = candidate->start(); !started) {
      arming_error = "cannot start the capture recorder: " + started.error();
      transaction->abort();
      return;
    }
    txn = std::move(*transaction);
    recorder = std::move(candidate);
    ledger = std::make_shared<CaptureLedger>();
    installed_tap_ = std::make_shared<CountingTap>(ledger, recorder->tapFor());
    host->setRecordTap(installed_tap_);
  }

  /// Wake blocked producers. Any thread.
  void wake() {
    std::shared_ptr<Recorder> snapshot;
    {
      std::lock_guard<std::mutex> lock(mu);
      snapshot = recorder;
    }
    if (snapshot) {
      snapshot->requestStop();
    }
  }

  /// Under mu: stop and discard the current arming (recorder, tap, partial).
  void disarmLocked() {
    if (host != nullptr && installed_tap_ != nullptr) {
      host->dropRecordTap(installed_tap_);
      installed_tap_.reset();
    }
    if (recorder) {
      recorder->requestStop();
      (void)recorder->stop(std::string(kTerminalCauseShutdown));
      recorder.reset();
    }
    if (txn.has_value()) {
      txn->abort();
      txn.reset();
    }
    ledger.reset();
    identity.clear();
  }

  /// Owner thread: detach every hook this capture installed on the host.
  void unhookHost() {
    if (host == nullptr) {
      return;
    }
    host->setSourceRecordAttachedHook(nullptr);
    host->setStopRequestedHook(nullptr);
    if (installed_tap_ != nullptr) {
      host->dropRecordTap(installed_tap_);  // conditional: never remove someone else's tap
    }
  }
};

SourceCaptureService::Capture::Capture() : impl_(std::make_shared<Impl>()) {}

SourceCaptureService::Capture::~Capture() {
  if (impl_ == nullptr || impl_->finalized) {
    return;
  }
  impl_->unhookHost();
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->disarmLocked();
}

SourceCaptureService::SourceCaptureService(SourceCacheStore& store, SinkFactory sink_factory)
    : store_(store), sink_factory_(std::move(sink_factory)) {}

std::unique_ptr<SourceCaptureService::Capture> SourceCaptureService::arm(
    DataSourceRuntimeHost& host, std::string provider_id, uint64_t queue_budget_bytes) {
  auto capture = std::make_unique<Capture>();
  auto* impl = capture->impl_.get();
  impl->store = &store_;
  impl->host = &host;
  impl->provider_id = std::move(provider_id);
  impl->queue_budget_bytes = queue_budget_bytes;
  impl->sink_factory = sink_factory_;
  // weak_ptr: a hook call snapshotted just before unhooking may still run
  // after the Capture is gone — it must find nothing, not freed memory.
  std::weak_ptr<Capture::Impl> weak = capture->impl_;
  host.setSourceRecordAttachedHook([weak](std::string_view descriptor, bool replaced) {
    if (auto strong = weak.lock()) {
      strong->onAttached(descriptor, replaced);
    }
  });
  host.setStopRequestedHook([weak] {
    if (auto strong = weak.lock()) {
      strong->wake();
    }
  });
  return capture;
}

void SourceCaptureService::cancel(Capture& capture) {
  {
    std::lock_guard<std::mutex> lock(capture.impl_->mu);
    capture.impl_->cancelled = true;
  }
  capture.impl_->wake();
}

SourceCaptureService::FinalizeResult SourceCaptureService::finalize(
    std::unique_ptr<Capture> capture, bool transaction_committed, bool cancelled) {
  FinalizeResult result;
  if (!capture) {
    result.reason = "no capture";
    return result;
  }
  auto* impl = capture->impl_.get();
  DataSourceRuntimeHost& host = *impl->host;
  impl->unhookHost();  // no further arming, taps or wake hooks past this point
  std::lock_guard<std::mutex> lock(impl->mu);
  impl->finalized = true;

  // ---- the publication gate ------------------------------------------------
  auto refuse = [&](std::string why) {
    result.reason = std::move(why);
    impl->disarmLocked();
    return std::move(result);
  };

  if (const auto veto = host.captureVetoReason(); !veto.empty()) {
    return refuse("capture vetoed: " + veto);
  }
  if (cancelled || impl->cancelled) {
    return refuse("the download was cancelled");
  }
  if (!transaction_committed) {
    return refuse("the ingest transaction did not commit");
  }
  const auto descriptor = host.sourceRecordDescriptor();
  if (!descriptor) {
    return refuse("no source record was attached");
  }
  if (!impl->arming_error.empty()) {
    return refuse("the capture never armed: " + impl->arming_error);
  }
  if (impl->recorder == nullptr || !impl->txn.has_value()) {
    return refuse("the capture never armed");
  }
  const auto completion = host.ingestCompletion();
  if (!completion) {
    return refuse("no ingest terminal was reported");
  }
  if (completion->outcome != sdk::IngestOutcome::kCompleted) {
    return refuse("the ingest terminal was not COMPLETED");
  }
  if (const auto failures = host.ingestCallbackFailures(); failures != 0) {
    return refuse(std::to_string(failures) + " ingest callback failure(s) were latched");
  }
  if (const auto skipped = host.recordTapSkippedLazy(); skipped != 0) {
    return refuse(std::to_string(skipped) + " message(s) bypassed the tap as pure-lazy");
  }

  // Coverage: the declared set vs what the tap actually saw, cross-checked
  // against the host's own count so a dropped tap can never hide a hole.
  CaptureManifest manifest;
  manifest.version = kCaptureManifestVersion;
  manifest.provider_id = impl->provider_id;
  manifest.identity = impl->identity;
  manifest.attests_empty_topics = completion->attestsEmptyTopics();
  if (completion->requested_topics.empty()) {
    return refuse("the completion declared an empty requested set");
  }
  const auto recorded = impl->ledger->messagesByTopic();
  {
    std::lock_guard<std::mutex> ledger_lock(impl->ledger->mu);
    manifest.total_messages = impl->ledger->total;
  }
  if (manifest.total_messages != host.tapEligibleMessages()) {
    return refuse("the capture ledger disagrees with the host's message count (a tap was dropped mid-stream?)");
  }
  if (manifest.total_messages == 0) {
    return refuse("an entirely empty capture is not cacheable");
  }
  for (const auto& topic : completion->requested_topics) {
    const auto it = recorded.find(topic);
    const uint64_t count = it == recorded.end() ? 0 : it->second;
    if (count == 0 && !manifest.attests_empty_topics) {
      return refuse("requested topic captured no messages and emptiness was not attested: " + topic);
    }
    manifest.requested_topic_messages[topic] = count;
  }
  for (const auto& [topic, count] : recorded) {
    if (!manifest.requested_topic_messages.contains(topic)) {
      return refuse("a recorded topic is outside the declared requested set: " + topic);
    }
  }

  // ---- finalize the file with the manifest inside --------------------------
  impl->recorder->setExtraMetadata(std::string(kCaptureMetadataName), manifestJson(manifest));
  const RecordingSummary summary = impl->recorder->stop(std::string(kTerminalCauseStopped));
  if (summary.truncated) {
    return refuse("the capture recording was truncated: " + summary.truncated_reason);
  }
  if (summary.file_incomplete) {
    return refuse("the capture file could not be finalized");
  }
  if (summary.dropped_messages != 0) {
    return refuse("the capture recording dropped messages");
  }
  if (summary.messages != manifest.total_messages) {
    return refuse("the recorder wrote fewer messages than the capture observed");
  }

  // ---- publish -------------------------------------------------------------
  auto published = store_.publish(impl->identity, std::move(*impl->txn));
  impl->txn.reset();
  impl->recorder.reset();
  if (!published) {
    result.reason = "cache publish failed: " + published.error().message;
    return result;
  }
  result.published = true;
  result.artifact = std::move(*published);
  return result;
}

std::optional<SourceCaptureService::Resolved> SourceCaptureService::resolve(
    std::string_view provider_id, std::string_view descriptor_json, std::string* miss_reason,
    SourceCacheStore::MissKind* miss_kind) {
  if (miss_kind != nullptr) {
    *miss_kind = SourceCacheStore::MissKind::kAbsent;
  }
  const std::string identity = sourceCacheIdentity(provider_id, descriptor_json);
  auto pinned = store_.lookup(identity, miss_reason, miss_kind);
  if (!pinned) {
    return std::nullopt;
  }

  // Every quarantine below is a verification failure of OUR pinned artifact —
  // an absent-style miss (provider fallback is the heal), never contended.
  auto quarantine = [&](const std::string& why) {
    pinned.reset();  // release our pin before quarantining
    std::string refusal;
    const bool healed = store_.quarantine(identity, &refusal);
    if (miss_reason != nullptr) {
      *miss_reason = why + (healed ? " (quarantined)" : " (quarantine refused: " + refusal + ")");
    }
    return std::nullopt;
  };

  auto manifest = readManifest(pinned->path);
  if (!manifest) {
    return quarantine("manifest unreadable: " + manifest.error());
  }
  if (manifest->identity != identity) {
    return quarantine("manifest identity mismatch");
  }
  if (manifest->provider_id != provider_id) {
    return quarantine("manifest provider mismatch");
  }

  // Strict completeness: the file's own statistics must agree with the
  // manifest, channel by channel — a partially recovered or tampered
  // artifact must never become a transparent hit. The verdict is computed in
  // the reader's own scope and quarantine runs strictly AFTER it: the open
  // McapReader handle would block the quarantine rename on Windows (a
  // sharing violation Linux does not surface).
  std::string mismatch;
  {
    mcap::McapReader reader;
    if (const auto opened = reader.open(pinned->path.string()); !opened.ok()) {
      mismatch = "artifact unreadable: " + opened.message;
    } else if (const auto summary = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan); !summary.ok()) {
      mismatch = "artifact summary unreadable: " + summary.message;
    } else if (const auto statistics = reader.statistics(); !statistics.has_value()) {
      mismatch = "artifact carries no statistics";
    } else {
      const auto channels = reader.channels();  // by value: the reader's map is not stable across calls
      std::map<std::string, uint64_t> file_counts;
      for (const auto& [channel_id, channel] : channels) {
        const auto count_it = statistics->channelMessageCounts.find(channel_id);
        const uint64_t count = count_it == statistics->channelMessageCounts.end() ? 0 : count_it->second;
        file_counts[channel->topic] += count;
      }
      uint64_t file_total = 0;
      for (const auto& [topic, count] : file_counts) {
        const auto declared = manifest->requested_topic_messages.find(topic);
        if (declared == manifest->requested_topic_messages.end()) {
          mismatch = "artifact channel outside the manifest: " + topic;
          break;
        }
        if (declared->second != count) {
          mismatch = "artifact message count disagrees with the manifest for: " + topic;
          break;
        }
        file_total += count;
      }
      if (mismatch.empty() && file_total != manifest->total_messages) {
        mismatch = "artifact total disagrees with the manifest";
      }
    }
  }
  if (!mismatch.empty()) {
    return quarantine(mismatch);
  }
  return Resolved{.artifact = std::move(*pinned), .manifest = std::move(*manifest)};
}

Expected<CaptureManifest> SourceCaptureService::readManifest(const std::filesystem::path& artifact) {
  mcap::McapReader reader;
  if (const auto opened = reader.open(artifact.string()); !opened.ok()) {
    return unexpected("cannot open the artifact: " + opened.message);
  }
  if (const auto summary = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan); !summary.ok()) {
    return unexpected("cannot read the artifact summary: " + summary.message);
  }
  std::optional<mcap::Metadata> found;
  bool duplicate = false;
  if (auto scanned = forEachMcapMetadata(
          reader, kCaptureMetadataName,
          [&](const mcap::Metadata& metadata) {
            if (found.has_value()) {
              duplicate = true;
            }
            found = metadata;
          });
      !scanned) {
    return unexpected(std::move(scanned).error());
  }
  if (duplicate) {
    return unexpected("the artifact carries more than one pj.capture manifest");
  }
  if (!found.has_value()) {
    return unexpected("the artifact carries no pj.capture manifest");
  }
  const auto json_it = found->metadata.find("json");
  if (json_it == found->metadata.end()) {
    return unexpected("pj.capture record has no json body");
  }
  const auto body = nlohmann::json::parse(json_it->second, nullptr, false);
  if (body.is_discarded() || !body.is_object()) {
    return unexpected("pj.capture body is not a JSON object");
  }
  try {
    CaptureManifest manifest;
    const auto& version = body.at("version");
    if (!version.is_number_unsigned() || version.get<uint64_t>() != kCaptureManifestVersion) {
      return unexpected("unknown pj.capture manifest version");
    }
    manifest.version = kCaptureManifestVersion;
    manifest.provider_id = body.at("provider_id").get<std::string>();
    if (manifest.provider_id.empty()) {
      return unexpected("pj.capture provider_id is empty");
    }
    manifest.identity = body.at("identity").get<std::string>();
    const auto& attests = body.at("attests_empty_topics");
    if (!attests.is_boolean()) {
      return unexpected("pj.capture attests_empty_topics is not a boolean");
    }
    manifest.attests_empty_topics = attests.get<bool>();
    const auto& topics = body.at("requested_topic_messages");
    if (!topics.is_object()) {
      return unexpected("pj.capture requested_topic_messages is not an object");
    }
    uint64_t sum = 0;
    for (const auto& [topic, count] : topics.items()) {
      if (topic.empty() || !count.is_number_unsigned()) {
        return unexpected("pj.capture carries a malformed topic count");
      }
      const auto value = count.get<uint64_t>();
      if (value > std::numeric_limits<uint64_t>::max() - sum) {
        return unexpected("pj.capture topic counts overflow");
      }
      manifest.requested_topic_messages[topic] = value;
      sum += value;
    }
    const auto& total = body.at("total_messages");
    if (!total.is_number_unsigned() || total.get<uint64_t>() != sum) {
      return unexpected("pj.capture total disagrees with its per-topic counts");
    }
    manifest.total_messages = sum;
    return manifest;
  } catch (const nlohmann::json::exception& e) {
    return unexpected(std::string("malformed pj.capture body: ") + e.what());
  }
}

}  // namespace PJ
