// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "SourcePromotionHost.h"

#include <QMetaObject>
#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "LoadInput.h"
#include "pj_base/sdk/descriptor_import.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/ServiceRegistration.h"
#include "pj_runtime/SessionManager.h"

namespace PJ {

namespace {
constexpr const char* kDomain = "source_promotion";

bool promotionReject(PJ_error_t* out_error, const char* message) noexcept {
  sdk::fillError(out_error, 1, kDomain, message);
  return false;
}

QString toQString(PJ_string_view_t view) {
  // sdk::toStringView already encodes the null-data rule (empty view).
  const std::string_view text = sdk::toStringView(view);
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}
}  // namespace

SourcePromotionHost::SourcePromotionHost(
    FileLoader& loader, SessionManager& session, QString provider_id, std::function<bool(DatasetId)> owns_dataset,
    QObject* parent)
    : QObject(parent),
      loader_(&loader),
      session_(session),
      provider_id_(std::move(provider_id)),
      owns_dataset_(std::move(owns_dataset)),
      vtable_{
          .protocol_version = 1,
          .struct_size = sizeof(PJ_source_promotion_host_vtable_t),
          .promote_to_file_source = &SourcePromotionHost::onPromote,
      },
      raw_{this, &vtable_} {
  // All three observers run on the GUI thread (direct connections — the
  // loader lives there too) and filter by ticket, so unrelated loads pass
  // through untouched.
  connect(
      loader_.data(), &FileLoader::loadCommitting, this,
      [this](quint64 ticket, const QVector<DatasetId>& produced, const QString&, const QString&) {
        const auto it = by_ticket_.find(ticket);
        if (it == by_ticket_.end()) {
          return;
        }
        const std::shared_ptr<Promotion>& promotion = it->second;
        if (!produced.contains(promotion->dataset)) {
          // Cannot happen under require_replacement (the commit keeps the
          // DatasetId), but ok=true must IMPLY "record attached" — so guard
          // it instead of assuming.
          promotion->failure_reason = tr("the committed load did not preserve dataset %1").arg(promotion->dataset);
          return;
        }
        // The pre-catalog seam: dataset, source path, and captured config are
        // installed and the publishing catalog rebuild follows immediately —
        // attaching here makes record and catalog publication atomic. The
        // provider identity is the HOST-side binding id, never request data.
        session_.attachSourceRecord(
            promotion->dataset, SourceRecord{
                                    .provider_id = provider_id_,
                                    .source_identity = promotion->source_identity,
                                    .descriptor_json = promotion->descriptor_json,
                                });
        promotion->record_attached = true;
      });
  connect(loader_.data(), &FileLoader::fileLoadFailed, this, [this](const QString& path, const QString& reason) {
    // Terminal loadFinished carries no reason; this legacy signal does.
    // It fires synchronously inside the failing path: either while OUR
    // loadFileTicketed call is still on the stack (issuing_ — the ticket
    // is not known yet) or for the loader's CURRENT request. The identity
    // check keeps an unrelated rejected enqueue (which emits this without
    // touching the current ticket) from polluting a promotion's reason.
    if (issuing_ != nullptr) {
      issuing_->failure_reason = reason;
      return;
    }
    if (loader_.isNull()) {
      return;  // loader was destroyed
    }
    const auto it = by_ticket_.find(loader_->currentLoadTicket());
    if (it != by_ticket_.end() && FileLoader::sameSourceIdentity(path, it->second->local_path)) {
      it->second->failure_reason = reason;
    }
  });
  connect(
      loader_.data(), &FileLoader::loadFinished, this,
      [this](quint64 ticket, LoadOutcome outcome, const QString&, DatasetId, const QVector<DatasetId>&) {
        const auto it = by_ticket_.find(ticket);
        if (it == by_ticket_.end()) {
          return;
        }
        const std::shared_ptr<Promotion> promotion = it->second;
        by_ticket_.erase(it);
        if (outcome == LoadOutcome::kLoaded && promotion->record_attached) {
          finishPromotion(promotion, true, tr("promoted"));
          return;
        }
        QString message = promotion->failure_reason;
        if (message.isEmpty()) {
          message = outcome == LoadOutcome::kCancelled ? tr("the replacement load was cancelled")
                    : outcome == LoadOutcome::kLoaded  ? tr("the load committed without attaching the source record")
                                                       : tr("the replacement load failed");
        }
        finishPromotion(promotion, false, message);
      });
}

SourcePromotionHost::~SourcePromotionHost() {
  shutdown();
}

void SourcePromotionHost::shutdown() {
  std::vector<std::shared_ptr<Promotion>> unfinished;
  {
    const std::lock_guard lock(mu_);
    if (shutting_down_) {
      return;  // idempotent: the destructor re-runs this after an explicit shutdown()
    }
    shutting_down_ = true;
    for (auto& [id, promotion] : pending_) {
      if (!promotion->fired) {
        promotion->fired = true;
        unfinished.push_back(promotion);
      }
    }
    pending_.clear();
  }
  // Stop observing the loader: a promotion load that outlives this host
  // completes as an ordinary strict replace with no record attach — that is
  // deliberate (the replaced data is still valid; only the promotion
  // transaction is reported failed below), and the disconnect guarantees its
  // later loadFinished cannot re-fire anything. Guard the dereference in case
  // the loader was destroyed.
  if (!loader_.isNull()) {
    QObject::disconnect(loader_.data(), nullptr, this, nullptr);
  }
  // Fail every accepted-but-unfinished promotion while the plugin instance /
  // DSO behind result_cb is still alive (the owners run shutdown() BEFORE
  // releasing the plugin handle). New arrivals are rejected synchronously
  // from here on (shutting_down_) — and keep being rejected safely for as
  // long as the object lives, which is what lets the owners destroy the
  // host only AFTER the plugin's workers are joined.
  by_ticket_.clear();
  constexpr const char* kMessage = "the promotion host is shutting down";
  for (const auto& promotion : unfinished) {
    promotion->result_cb(
        promotion->callback_ctx, false, PJ_string_view_t{kMessage, std::char_traits<char>::length(kMessage)});
  }
}

Status SourcePromotionHost::registerServices(ServiceRegistryBuilder& registry) {
  return registerRequiredService<sdk::SourcePromotionHostService>(registry, raw_);
}

bool SourcePromotionHost::onPromote(
    void* ctx, const PJ_source_promotion_request_v1_t* request, PJ_source_promotion_result_fn result_cb,
    void* callback_ctx, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<SourcePromotionHost*>(ctx);
  if (self == nullptr || request == nullptr) {
    return promotionReject(out_error, "invalid arguments");
  }
  if (result_cb == nullptr) {
    // An accepted call promises result_cb exactly once — unfulfillable here.
    return promotionReject(out_error, "result_cb must not be null");
  }
  try {
    // Growth-contract gate: read only fields wholly covered by struct_size.
    // Every v1 field is required, so anything smaller is malformed.
    constexpr uint32_t kMinRequestSize =
        offsetof(PJ_source_promotion_request_v1_t, descriptor_json) + sizeof(PJ_string_view_t);
    if (request->struct_size < kMinRequestSize) {
      return promotionReject(out_error, "request struct_size does not cover the v1 fields");
    }
    if (request->dataset.id == 0) {
      return promotionReject(out_error, "request names no dataset (zero handle)");
    }
    const QString local_path = toQString(request->local_path_utf8);
    if (local_path.isEmpty()) {
      return promotionReject(out_error, "request carries an empty local path");
    }

    // Copy the COMPLETE request before returning — the caller's views die at
    // return, and the job crosses onto the GUI thread.
    auto promotion = std::make_shared<Promotion>();
    promotion->dataset = static_cast<DatasetId>(request->dataset.id);
    promotion->source_identity = toQString(request->source_identity);
    promotion->local_path = local_path;
    promotion->loader_plugin_id = toQString(request->loader_plugin_id);
    promotion->loader_config_json = toQString(request->loader_config_json);
    promotion->descriptor_json = toQString(request->descriptor_json);
    promotion->result_cb = result_cb;
    promotion->callback_ctx = callback_ctx;

    {
      const std::lock_guard lock(self->mu_);
      if (self->shutting_down_) {
        return promotionReject(out_error, "the promotion host is shutting down");
      }
      const quint64 promotion_id = ++self->next_promotion_id_;
      promotion->id = promotion_id;
      self->pending_.emplace(promotion_id, std::move(promotion));
      try {
        // Queued unconditionally (even from the GUI thread): promote_to_file_
        // source is [asynchronous] and this keeps the entry path re-entrancy-
        // free. Queued metacalls die with the QObject, so a teardown before
        // the job runs leaves only the destructor's pending_ sweep — which is
        // exactly the accepted-then-failed contract. Posted while STILL
        // HOLDING mu_ (QueuedConnection never runs the lambda synchronously,
        // so no recursion): the destructor must take mu_ for its sweep, so it
        // cannot free the QObject between this thread's admission and its
        // post — ~QObject's posted-event purge runs strictly after. A call
        // arriving entirely AFTER the destructor returned would lock a
        // destroyed mutex — unprotectable inside the class; safety rests on
        // import-job quiescence ordering (the panel session tears the
        // plugin's jobs down while this host is still alive).
        QMetaObject::invokeMethod(self, &SourcePromotionHost::processPromotion, Qt::QueuedConnection, promotion_id);
      } catch (...) {
        // Never accepted after all: the sweep must not fire result_cb for it.
        self->pending_.erase(promotion_id);
        throw;
      }
    }
    return true;
  } catch (const std::exception& e) {
    return promotionReject(out_error, e.what());
  } catch (...) {
    return promotionReject(out_error, "unknown error accepting the promotion request");
  }
}

void SourcePromotionHost::processPromotion(quint64 promotion_id) {
  std::shared_ptr<Promotion> promotion;
  {
    const std::lock_guard lock(mu_);
    const auto it = pending_.find(promotion_id);
    if (it == pending_.end()) {
      return;  // already resolved (teardown sweep)
    }
    promotion = it->second;
  }

  // Ownership gate: the target must have been produced by the bound
  // provider's ingest — the host-side substitute for a generation token.
  // Liveness needs no separate check: require_replacement makes FileLoader
  // fail a vanished target instead of degrading to a fresh load.
  if (!owns_dataset_ || !owns_dataset_(promotion->dataset)) {
    finishPromotion(
        promotion, false, tr("dataset %1 was not produced by this plugin's ingest").arg(promotion->dataset));
    return;
  }

  LoadHints hints;
  hints.expected_manifest_id = promotion->loader_plugin_id;
  hints.require_expected_plugin = true;
  hints.preset_config_json = promotion->loader_config_json;
  hints.dialog_policy = DialogPolicy::kNever;  // no dialogs; the preset is authoritative
  hints.rewrite_preset_filepath = true;        // the preset's filepath names the provider's staging area
  hints.replace_dataset_id = promotion->dataset;
  hints.require_replacement = true;  // strict: a vanished target FAILS, never a fresh load

  // A synchronous failure inside loadFileTicketed emits fileLoadFailed before
  // the ticket is known — issuing_ routes that reason to this promotion. It
  // must not survive a throwing enqueue, or a later unrelated fileLoadFailed
  // would misroute its reason here.
  issuing_ = promotion;
  LoadRequestId ticket = 0;
  // Guard against loader being destroyed before this queued call runs.
  if (loader_.isNull()) {
    issuing_.reset();
    finishPromotion(promotion, false, tr("the file loader was destroyed"));
    return;
  }
  try {
    ticket = loader_->loadFileTicketed(LoadInput::fromNativePath(promotion->local_path), nullptr, hints);
  } catch (...) {
    issuing_.reset();
    throw;
  }
  issuing_.reset();
  if (ticket == 0) {
    finishPromotion(
        promotion, false,
        promotion->failure_reason.isEmpty() ? tr("the file loader rejected the request") : promotion->failure_reason);
    return;
  }
  // Accepted by the loader: loadFinished for this ticket arrives through the
  // event loop, never before this method returns.
  by_ticket_.emplace(ticket, std::move(promotion));
}

void SourcePromotionHost::finishPromotion(
    const std::shared_ptr<Promotion>& promotion, bool ok, const QString& message) {
  // Convert BEFORE taking the fired flag: if the allocation throws, the
  // promotion stays pending and the destructor sweep still fails it.
  const QByteArray utf8 = message.toUtf8();
  {
    const std::lock_guard lock(mu_);
    if (promotion->fired) {
      return;
    }
    promotion->fired = true;
    pending_.erase(promotion->id);
  }
  // Outside the lock: result_cb may legally re-enter promote_to_file_source.
  promotion->result_cb(
      promotion->callback_ctx, ok, PJ_string_view_t{utf8.constData(), static_cast<uint64_t>(utf8.size())});
}

}  // namespace PJ
