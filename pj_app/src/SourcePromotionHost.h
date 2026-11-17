#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "FileLoader.h"
#include "pj_base/descriptor_import_protocol.h"
#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"

namespace PJ {

class ServiceRegistryBuilder;
class SessionManager;

// Host side of the SDK service "pj.source_promotion.v1": a descriptor-import provider hands its materialized file
// to the host, which reloads it in place of the provider's dataset as a stock file source. One instance per bound
// toolbox instance (MainWindow::launchToolbox, HeadlessDescriptorProviderSession), so the provider identity comes
// from the binding (`provider_id`) and a plugin cannot claim another's.
//
// An accepted promotion: checks that the bound instance's ingest produced the dataset (`owns_dataset`); loads the
// file with the given loader plugin and preset, no dialog, require_replacement (a vanished target fails, never
// becomes a fresh load); attaches the SourceRecord at loadCommitting, atomically with catalog publication; and
// calls result_cb exactly once. A false return from promote_to_file_source is a synchronous reject and result_cb
// never runs; an accepted request that then fails reports ok=false.
//
// promote_to_file_source may be called from any thread: it copies the request, validates its structure and queues
// the work onto this object's (GUI) thread. result_cb runs on the GUI thread, outside the lock, and may call
// promote_to_file_source again.
//
// Teardown does not cancel an in-flight replacement load: it completes as a plain replace with no record, and only
// the promotion reports failure.
//
// Not movable: the service fat pointer stores `this`.
class SourcePromotionHost : public QObject {
  Q_OBJECT
 public:
  // `provider_id` is the plugin's manifest id from the host's binding. `owns_dataset`: did this instance's ingest
  // produce the dataset (null: every promotion fails).
  SourcePromotionHost(
      FileLoader& loader, SessionManager& session, QString provider_id, std::function<bool(DatasetId)> owns_dataset,
      QObject* parent = nullptr);
  ~SourcePromotionHost() override;

  // Idempotent; the destructor calls it too. Stops observing the loader, rejects every later
  // promote_to_file_source synchronously and fails every accepted, unfinished promotion. It calls plugin code, so
  // run it while the plugin instance is alive; then keep this object alive until the plugin's workers are joined
  // (instance destroyed), so a straggler call hits the rejection instead of freed memory.
  void shutdown();

  SourcePromotionHost(const SourcePromotionHost&) = delete;
  SourcePromotionHost& operator=(const SourcePromotionHost&) = delete;

  // Before the handle's bind(). On failure do not bind the plugin: its requests would reach a host that was never
  // published.
  [[nodiscard]] Status registerServices(ServiceRegistryBuilder& registry);

  [[nodiscard]] PJ_source_promotion_host_t raw() const noexcept {
    return raw_;
  }

 private:
  // One accepted promotion. Request fields are immutable; `fired` (the exactly-once gate shared with shutdown())
  // is guarded by mu_; failure_reason and record_attached are GUI-thread only. The load ticket lives only in
  // by_ticket_.
  struct Promotion {
    quint64 id = 0;
    DatasetId dataset = 0;
    QString source_identity;
    QString local_path;
    QString loader_plugin_id;
    QString loader_config_json;
    QString descriptor_json;
    PJ_source_promotion_result_fn result_cb = nullptr;
    void* callback_ctx = nullptr;
    bool fired = false;
    QString failure_reason;
    bool record_attached = false;
  };

  static bool onPromote(
      void* ctx, const PJ_source_promotion_request_v1_t* request, PJ_source_promotion_result_fn result_cb,
      void* callback_ctx, PJ_error_t* out_error) noexcept;

  void processPromotion(quint64 promotion_id);
  // No-op when already fired.
  void finishPromotion(const std::shared_ptr<Promotion>& promotion, bool ok, const QString& message);

  QPointer<FileLoader> loader_;
  SessionManager& session_;
  const QString provider_id_;
  const std::function<bool(DatasetId)> owns_dataset_;

  // Guards pending_, next_promotion_id_, shutting_down_ and every Promotion's `fired`: what the any-thread entry
  // point touches.
  std::mutex mu_;
  bool shutting_down_ = false;
  quint64 next_promotion_id_ = 0;
  std::unordered_map<quint64, std::shared_ptr<Promotion>> pending_;

  // GUI thread only. issuing_ is the promotion inside loadFileTicketed: a synchronous failure emits fileLoadFailed
  // before the ticket is known.
  std::unordered_map<quint64, std::shared_ptr<Promotion>> by_ticket_;
  std::shared_ptr<Promotion> issuing_;

  PJ_source_promotion_host_vtable_t vtable_;
  PJ_source_promotion_host_t raw_;
};

}  // namespace PJ
