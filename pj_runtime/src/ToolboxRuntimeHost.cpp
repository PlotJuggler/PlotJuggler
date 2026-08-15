// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/ToolboxRuntimeHost.h"

#include <QMetaObject>
#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/ServiceRegistration.h"

namespace PJ {

ToolboxRuntimeHost::ToolboxRuntimeHost(
    DataEngine& engine, ObjectStore& object_store, sdk::SettingsBackend& settings, Callbacks callbacks)
    : ToolboxRuntimeHost(engine, object_store, settings, std::move(callbacks), ParserIngestDeps{}) {}

ToolboxRuntimeHost::ToolboxRuntimeHost(
    DataEngine& engine, ObjectStore& object_store, sdk::SettingsBackend& settings, Callbacks callbacks,
    ParserIngestDeps parser_ingest)
    : write_host_(engine, object_store),
      read_host_(object_store),
      settings_host_(settings),
      callbacks_(std::move(callbacks)),
      engine_(engine),
      object_store_(object_store),
      parser_ingest_deps_(std::move(parser_ingest)),
      runtime_vtable_{
          PJ_TOOLBOX_PLUGIN_PROTOCOL_VERSION,        sizeof(PJ_toolbox_runtime_host_vtable_t),
          &ToolboxRuntimeHost::onReportMessage,      &ToolboxRuntimeHost::onNotifyDataChanged,
          &ToolboxRuntimeHost::onCreateParserIngest, &ToolboxRuntimeHost::onReleaseParserIngest,
      },
      runtime_{this, &runtime_vtable_} {}

ToolboxRuntimeHost::~ToolboxRuntimeHost() {
  // Flush anything a toolbox left unreleased so rows aren't lost on teardown.
  // Mirror the release path: take the contexts out under the lock, run plugin
  // parser destructors and engine flushes outside it.
  std::unordered_map<uint32_t, std::unique_ptr<DataSourceRuntimeHost>> contexts;
  std::unordered_map<uint32_t, std::shared_ptr<IngestProgress>> progress;
  {
    std::lock_guard lock(parser_ingest_mu_);
    contexts.swap(parser_ingests_);
    progress.swap(ingest_progress_);
  }
  // Close any import the plugin never finished AND deliver any terminal that
  // was queued but not yet pumped: the shell's started/finished callbacks must
  // stay paired or its progress UI / SessionManager lifecycle wedges on a
  // mid-import panel close. Direct call — teardown runs on the construction
  // thread (the same contract every marshalled callback already assumes), and
  // a queued metacall is purged with marshaller_ before delivery. The phase
  // exchange is the sweep's claim: kActive (never finished) and kFinishQueued
  // (queued but unpumped) both fire; a DELIVERED terminal already returned to
  // kIdle — exactly-once holds in both directions. A swept finish whose BEGIN
  // was still queued (it dies with marshaller_) lands on an absent key at the
  // consumer — endIngest's documented silent no-op — so the pairing contract
  // degrades safely rather than wedging.
  for (auto& [id, state] : progress) {
    const uint64_t prior = state->state.exchange(0);
    if ((prior & IngestProgress::kPhaseMask) != IngestProgress::kPhaseIdle && callbacks_.on_ingest_finished) {
      callbacks_.on_ingest_finished(static_cast<DatasetId>(id));
    }
  }
  for (auto& [id, host] : contexts) {
    host->flushAll();
  }
}

uint64_t ToolboxRuntimeHost::claimIngestFinish(IngestProgress& progress) {
  // Pairing gate: only an ACTIVE sequence emits finished. progressFinish
  // without a start is a documented safe no-op (the SDK's finite-import
  // pattern calls it unconditionally), and release after a clean finish must
  // add nothing — an unpaired finished would disturb the shell's bookkeeping
  // for some other import. Single-shot on the loaded word, NEVER retried: a
  // failed CAS means the sequence was already terminated or superseded by a
  // re-arm, and retrying against a re-armed word would claim the NEWER
  // sequence's terminal.
  uint64_t expected = progress.state.load(std::memory_order_acquire);
  if ((expected & IngestProgress::kPhaseMask) != IngestProgress::kPhaseActive) {
    return 0;
  }
  const uint64_t claimed = (expected & ~IngestProgress::kPhaseMask) | IngestProgress::kPhaseFinishQueued;
  return progress.state.compare_exchange_strong(expected, claimed) ? claimed : 0;
}

void ToolboxRuntimeHost::postIngestFinished(
    DatasetId dataset_id, const std::shared_ptr<IngestProgress>& progress, uint64_t claimed_word) {
  QMetaObject::invokeMethod(
      &marshaller_,
      [self = this, dataset_id, progress, claimed_word]() {
        // Delivery consumes exactly the claimed word (same generation, same
        // phase). A re-armed sequence changed the generation and the sweep
        // zeroed the word, so either makes this CAS fail — a stale delivery
        // can neither double-fire nor steal a newer sequence's terminal.
        uint64_t expected = claimed_word;
        const uint64_t idle = claimed_word & ~IngestProgress::kPhaseMask;
        if (!progress->state.compare_exchange_strong(expected, idle)) {
          return;
        }
        if (self->callbacks_.on_ingest_finished) {
          self->callbacks_.on_ingest_finished(dataset_id);
        }
        // Deliberately NO map cleanup here: a re-created context for the same
        // dataset holds this exact entry (onCreateParserIngest reuses the
        // slot) and may still be idle-phase — indistinguishable, from this
        // lambda, from a dead slot. Erasing would untrack that live context:
        // its release would find no entry, its finished would never queue,
        // and the shell's keyed pairing would wedge. Entries are retained
        // until host teardown.
      },
      // QueuedConnection, never Auto: release is a [thread-safe] slot, so
      // this post may legally run ON the marshaller thread while the
      // sequence's begin — queued by an off-thread progress_start — is still
      // in flight. A direct delivery would emit finish BEFORE begin: the end
      // lands on an absent key (endIngest's documented silent no-op) and the
      // late begin re-inserts an entry nothing will ever erase. Forced
      // queuing keeps every terminal FIFO behind its own sequence's begin
      // through marshaller_ — the begin is always enqueued no later than the
      // claim that produced this post.
      Qt::QueuedConnection);
}

bool ToolboxRuntimeHost::hasIngestForDataset(DatasetId dataset_id) const {
  const std::lock_guard lock(parser_ingest_mu_);
  return ingest_progress_.find(dataset_id) != ingest_progress_.end();
}

bool ToolboxRuntimeHost::hasActiveIngests() const {
  const std::lock_guard lock(parser_ingest_mu_);
  // parser_ingests_ holds only live contexts: entries appear on create and are
  // erased on release, unlike the ingest_progress_ bookkeeping above.
  return !parser_ingests_.empty();
}

void ToolboxRuntimeHost::requestStopActiveIngests() {
  std::lock_guard lock(parser_ingest_mu_);
  for (auto& [id, host] : parser_ingests_) {
    // Flag-only overload: safe from this (GUI) thread against a worker that
    // may be inside fail()/last_error_ right now.
    host->requestStop();
  }
}

Status ToolboxRuntimeHost::registerServices(ServiceRegistryBuilder& registry) {
  if (auto status = registerRequiredService<sdk::ToolboxHostService>(registry, write_host_.raw()); !status) {
    return status;
  }
  registerOptionalService<sdk::ToolboxObjectReadHostService>(registry, read_host_.raw());
  if (auto status = registerRequiredService<sdk::ToolboxRuntimeHostService>(registry, runtime_); !status) {
    return status;
  }
  registerOptionalService<sdk::SettingsStoreService>(registry, settings_host_.view());
  return {};
}

void ToolboxRuntimeHost::onReportMessage(
    void* ctx, PJ_toolbox_message_level_t level, PJ_string_view_t message) noexcept {
  auto* self = static_cast<ToolboxRuntimeHost*>(ctx);
  if (self == nullptr || !self->callbacks_.on_message) {
    return;
  }
  try {
    std::string text = message.data != nullptr ? std::string(message.data, message.size) : std::string();
    // Marshal to the host thread (AutoConnection: direct when already there,
    // queued from a worker thread) so on_message can touch Qt UI safely.
    QMetaObject::invokeMethod(
        &self->marshaller_,
        [self, level, text = std::move(text)]() mutable {
          if (self->callbacks_.on_message) {
            self->callbacks_.on_message(level, std::move(text));
          }
        },
        Qt::AutoConnection);
  } catch (...) {
    // noexcept C-ABI boundary: never let an exception escape into the plugin.
  }
}

void ToolboxRuntimeHost::onNotifyDataChanged(void* ctx) noexcept {
  auto* self = static_cast<ToolboxRuntimeHost*>(ctx);
  if (self == nullptr) {
    return;
  }
  try {
    // Marshal to the host thread (AutoConnection: direct when already there,
    // queued from a worker thread). Seal buffered writes so the freshly written
    // rows/objects are visible before the host rebuilds its catalog — flush and
    // rebuild then always run together on the GUI thread.
    QMetaObject::invokeMethod(
        &self->marshaller_,
        [self]() {
          try {
            self->write_host_.flushPending();
            // Report the bulk-import datasets: created/released contexts are
            // drained per notify, while a MID-IMPORT context (active progress
            // sequence) re-reports on EVERY notify so the shell's playback
            // focus follows the growing import instead of collapsing back to
            // the union between ticks. Composed here (GUI thread) so the set
            // and the callback observing it stay ordered.
            std::vector<DatasetId> ingested;
            {
              std::lock_guard lock(self->parser_ingest_mu_);
              ingested.swap(self->pending_ingest_datasets_);
              for (const auto& [id, progress] : self->ingest_progress_) {
                const auto ds_id = static_cast<DatasetId>(id);
                if ((progress->state.load() & IngestProgress::kPhaseMask) == IngestProgress::kPhaseActive &&
                    std::find(ingested.begin(), ingested.end(), ds_id) == ingested.end()) {
                  ingested.push_back(ds_id);
                }
              }
            }
            if (self->callbacks_.on_data_changed) {
              self->callbacks_.on_data_changed(std::move(ingested));
            }
          } catch (...) {}
        },
        Qt::AutoConnection);
  } catch (...) {
    // noexcept C-ABI boundary.
  }
}

namespace {
// Same error taxonomy as DataSourceRuntimeHost::fail (the sibling ingest
// surface), under this host's own domain.
bool parserIngestFail(PJ_error_t* out_error, const char* msg) noexcept {
  sdk::fillError(out_error, 1, "pj.runtime.toolbox_ingest", msg);
  return false;
}
}  // namespace

bool ToolboxRuntimeHost::onCreateParserIngest(
    void* ctx, uint32_t data_source_id, PJ_data_source_runtime_host_t* out_host, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<ToolboxRuntimeHost*>(ctx);
  if (self == nullptr || out_host == nullptr) {
    return parserIngestFail(out_error, "invalid arguments");
  }
  try {
    if (self->parser_ingest_deps_.catalog == nullptr) {
      return parserIngestFail(out_error, "parser ingest is not configured on this host");
    }
    // Same validation the toolbox write host applies to object topics
    // (plugin_data_host.cpp toolboxRegisterObjectTopic): the id must be a
    // live dataset.
    if (self->engine_.getDataset(static_cast<DatasetId>(data_source_id)) == nullptr) {
      return parserIngestFail(out_error, "data source not found — call createDataSource first");
    }
    std::lock_guard lock(self->parser_ingest_mu_);
    // Construct first, insert only on success: a throwing constructor must not
    // leave a null entry in the map (the release path dereferences entries).
    auto it = self->parser_ingests_.find(data_source_id);
    if (it == self->parser_ingests_.end()) {
      auto host = std::make_unique<DataSourceRuntimeHost>(
          self->engine_, *self->parser_ingest_deps_.catalog, static_cast<DatasetId>(data_source_id),
          PJ_data_source_handle_t{data_source_id}, self->object_store_,
          "toolbox-ingest-" + std::to_string(data_source_id), self->parser_ingest_deps_.register_object_parser,
          // No secondary store/engine (toolbox ingest writes straight into the
          // primary), and no plugin-DSO keepalive — the toolbox isn't a
          // DataSource plugin; its render parsers come from the catalog, which
          // owns their .so lifetime and outlives every ingest context.
          /*secondary_object_store=*/nullptr, /*secondary_data_engine=*/nullptr,
          /*library_keepalive=*/std::shared_ptr<void>{}, self->parser_ingest_deps_.ingest_taps);
      it = self->parser_ingests_.emplace(data_source_id, std::move(host)).first;

      // Progress hooks: the plugin's progress_start/update/finish calls on the
      // fat pointer drive the shell's progressive-import surface. They run on
      // the plugin's ingest thread — flushes are engine-lock-safe there, and
      // UI-facing callbacks marshal through marshaller_ like every other
      // callback. `raw` is the context that invokes the hook, so it outlives
      // every invocation; `self` outlives the plugin per the teardown contract.
      DataSourceRuntimeHost* raw = it->second.get();
      // REUSE the dataset's progress entry across release/re-create rather
      // than replacing it: a replaced entry would orphan a still-undelivered
      // terminal (Phase::kFinishQueued) from the previous context, hiding it
      // from the destructor's sweep. phase/total are re-armed by
      // progress_start.
      auto& progress_slot = self->ingest_progress_[data_source_id];
      if (progress_slot == nullptr) {
        progress_slot = std::make_shared<IngestProgress>();
      }
      auto progress = progress_slot;
      const auto hook_ds = static_cast<DatasetId>(data_source_id);
      raw->on_progress_start = [self, hook_ds, progress](std::string_view label, uint64_t total, bool cancellable) {
        try {
          progress->total.store(total);
          // Re-arm: bump the generation and go active in ONE atomic word, so
          // any finish still queued for the PREVIOUS sequence is structurally
          // superseded (its delivery CAS carries the old generation).
          uint64_t prev = progress->state.load(std::memory_order_relaxed);
          uint64_t armed;
          do {
            armed =
                ((prev & ~IngestProgress::kPhaseMask) + IngestProgress::kGenerationStep) | IngestProgress::kPhaseActive;
          } while (!progress->state.compare_exchange_weak(prev, armed, std::memory_order_acq_rel));
          QMetaObject::invokeMethod(
              &self->marshaller_,
              [self, hook_ds, label = std::string(label), total, cancellable]() {
                if (self->callbacks_.on_ingest_started) {
                  self->callbacks_.on_ingest_started(hook_ds, label, total, cancellable);
                }
              },
              Qt::AutoConnection);
        } catch (...) {}
      };
      raw->on_progress_update = [self, hook_ds, progress, raw](uint64_t current) -> bool {
        try {
          const auto now = std::chrono::steady_clock::now();
          if (now - progress->last_flush < std::chrono::milliseconds(self->flush_throttle_ms_)) {
            return true;
          }
          progress->last_flush = now;
          // Seal both write surfaces so the marshalled on_ingest_progress can
          // treat "this tick" as "these rows are reader-visible": the context's
          // own parser-binding writers AND the toolbox write host the plugin
          // may be appending through (the cloud connector's Arrow/object path).
          raw->flushPending();
          self->write_host_.flushPending();
          QMetaObject::invokeMethod(
              &self->marshaller_,
              [self, hook_ds, current, total = progress->total.load()]() {
                if (self->callbacks_.on_ingest_progress) {
                  self->callbacks_.on_ingest_progress(hook_ds, current, total);
                }
              },
              Qt::AutoConnection);
        } catch (...) {}
        return true;  // host-side stop travels via stop_requested_, checked before this hook
      };
      raw->on_progress_finish = [self, hook_ds, progress]() {
        try {
          // Exactly-once terminal via the generation-tagged word: the claim
          // takes THIS sequence's word (or no-ops if it already terminated or
          // was superseded), and the posted delivery consumes exactly that
          // word — surviving host teardown before the metacall is pumped.
          if (const uint64_t claimed = claimIngestFinish(*progress); claimed != 0) {
            self->postIngestFinished(hook_ds, progress, claimed);
          }
        } catch (...) {}
      };
    }
    // Remember the dataset for the next notify_data_changed: an ingest context
    // marks a bulk import the host will want to focus playback on.
    const auto ds_id = static_cast<DatasetId>(data_source_id);
    auto& pending = self->pending_ingest_datasets_;
    if (std::find(pending.begin(), pending.end(), ds_id) == pending.end()) {
      pending.push_back(ds_id);
    }
    *out_host = it->second->hostHandle();  // out_host written ONLY on success (documented contract)
    return true;
  } catch (const std::exception& e) {
    return parserIngestFail(out_error, e.what());
  } catch (...) {
    return parserIngestFail(out_error, "unknown exception in create_parser_ingest");
  }
}

bool ToolboxRuntimeHost::onReleaseParserIngest(void* ctx, uint32_t data_source_id, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<ToolboxRuntimeHost*>(ctx);
  if (self == nullptr) {
    return parserIngestFail(out_error, "invalid arguments");
  }
  try {
    std::unique_ptr<DataSourceRuntimeHost> victim;
    std::shared_ptr<IngestProgress> progress;
    uint64_t claimed_word = 0;
    {
      std::lock_guard lock(self->parser_ingest_mu_);
      auto it = self->parser_ingests_.find(data_source_id);
      if (it == self->parser_ingests_.end()) {
        return true;  // idempotent
      }
      victim = std::move(it->second);
      self->parser_ingests_.erase(it);
      // COPY, don't erase: the entry must outlive this release so a finished
      // metacall queued below that teardown then purges is still visible to
      // the destructor's sweep (see IngestProgress::Phase). Entries are
      // retained until teardown — see the ingest_progress_ member doc.
      if (auto pit = self->ingest_progress_.find(data_source_id); pit != self->ingest_progress_.end()) {
        progress = pit->second;
        // CLAIM the terminal while still under the lock: a re-arm needs this
        // same mutex (create) before its progress_start can bump the
        // generation, so the claim can only take THIS context's sequence — a
        // claim after unlock could race a re-create and steal the newer
        // sequence's terminal. The marshalled post waits until after the
        // terminal flush below, preserving rows-sealed-before-finished.
        claimed_word = claimIngestFinish(*progress);
      }
    }
    // Seal rows BEFORE destruction so the next notify_data_changed/catalog
    // rebuild sees everything the parsers wrote.
    victim->flushAll();
    victim.reset();
    {
      // A released context marks a COMPLETED bulk import: report it on the
      // next notify so the shell runs its terminal focus/reconcile pass over
      // the finished dataset (the create-time report was drained long ago on
      // a progressive import). Published only AFTER the terminal flush above —
      // release and notify are both [thread-safe], so a concurrent notifier
      // must not drain this marker while the parser writers are half-flushed.
      std::lock_guard lock(self->parser_ingest_mu_);
      const auto ds_id = static_cast<DatasetId>(data_source_id);
      auto& pending = self->pending_ingest_datasets_;
      if (std::find(pending.begin(), pending.end(), ds_id) == pending.end()) {
        pending.push_back(ds_id);
      }
    }
    // Releasing without progress_finish must still pair the shell's
    // started/finished callbacks, or its progress UI never hides. Marshalled
    // (release is a [thread-safe] slot) with the word claimed under the lock
    // above; the delivery consumes exactly that word and survives a teardown
    // that purges the queued metacall (the sweep sees kFinishQueued).
    if (progress != nullptr && claimed_word != 0) {
      self->postIngestFinished(static_cast<DatasetId>(data_source_id), progress, claimed_word);
    }
    return true;
  } catch (const std::exception& e) {
    return parserIngestFail(out_error, e.what());
  } catch (...) {
    return parserIngestFail(out_error, "unknown exception in release_parser_ingest");
  }
}

}  // namespace PJ
