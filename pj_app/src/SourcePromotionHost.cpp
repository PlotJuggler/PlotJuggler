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
  // Direct connections (the loader lives on the GUI thread too), filtered by ticket.
  connect(
      loader_.data(), &FileLoader::loadCommitting, this,
      [this](quint64 ticket, const QVector<DatasetId>& produced, const QString&, const QString&) {
        const auto it = by_ticket_.find(ticket);
        if (it == by_ticket_.end()) {
          return;
        }
        const std::shared_ptr<Promotion>& promotion = it->second;
        if (!produced.contains(promotion->dataset)) {
          // Not expected under require_replacement, but ok=true must imply the record is attached.
          promotion->failure_reason = tr("the committed load did not preserve dataset %1").arg(promotion->dataset);
          return;
        }
        // Before the catalog publishes, so record and catalog appear together. provider_id is the binding's,
        // never request data.
        session_.attachSourceRecord(
            promotion->dataset, SourceRecord{
                                    .provider_id = provider_id_,
                                    .source_identity = promotion->source_identity,
                                    .descriptor_json = promotion->descriptor_json,
                                });
        promotion->record_attached = true;
      });
  connect(loader_.data(), &FileLoader::fileLoadFailed, this, [this](const QString& path, const QString& reason) {
    // loadFinished carries no reason; this signal does, synchronously: inside our loadFileTicketed (issuing_) or
    // for the loader's current request. The path check keeps an unrelated rejected enqueue, which emits it too,
    // from taking over a promotion's reason.
    if (issuing_ != nullptr) {
      issuing_->failure_reason = reason;
      return;
    }
    if (loader_.isNull()) {
      return;
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
      return;
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
  // A promotion load still running completes as a plain replace (its data is valid); the disconnect keeps its
  // loadFinished from reaching this object.
  if (!loader_.isNull()) {
    QObject::disconnect(loader_.data(), nullptr, this, nullptr);
  }
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
    return promotionReject(out_error, "result_cb must not be null");
  }
  try {
    // Read only fields struct_size covers; every v1 field is required.
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

    // The caller's views die at return.
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
        // Queued even from the GUI thread, so the entry path is never re-entrant. Posted while holding mu_:
        // shutdown() takes mu_ for its sweep, so the object cannot die between admission and post, and a queued
        // call purged by ~QObject is failed by that sweep. A call after destruction would lock a destroyed mutex;
        // the owners prevent it by joining the plugin's workers while this object is alive.
        QMetaObject::invokeMethod(self, &SourcePromotionHost::processPromotion, Qt::QueuedConnection, promotion_id);
      } catch (...) {
        // Not accepted after all: the sweep must not call result_cb for it.
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
      return;  // failed by shutdown()
    }
    promotion = it->second;
  }

  // Stands in for a generation token. Liveness is require_replacement's job.
  if (!owns_dataset_ || !owns_dataset_(promotion->dataset)) {
    finishPromotion(
        promotion, false, tr("dataset %1 was not produced by this plugin's ingest").arg(promotion->dataset));
    return;
  }

  LoadHints hints;
  hints.expected_manifest_id = promotion->loader_plugin_id;
  hints.require_expected_plugin = true;
  hints.preset_config_json = promotion->loader_config_json;
  // No dialogs; the preset is authoritative, and its filepath, which names the
  // provider's staging area, is rewritten to the file being loaded.
  hints.dialog_policy = DialogPolicy::kNever;
  hints.replace_dataset_id = promotion->dataset;
  hints.require_replacement = true;

  // issuing_ must not survive a throwing enqueue, or a later unrelated fileLoadFailed would land here.
  issuing_ = promotion;
  LoadRequestId ticket = 0;
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
  // Its loadFinished arrives through the event loop, never before this returns.
  by_ticket_.emplace(ticket, std::move(promotion));
}

void SourcePromotionHost::finishPromotion(
    const std::shared_ptr<Promotion>& promotion, bool ok, const QString& message) {
  // Before taking `fired`: if the allocation throws, the promotion stays pending and shutdown() still fails it.
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
