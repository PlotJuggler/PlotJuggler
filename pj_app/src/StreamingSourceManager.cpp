// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "StreamingSourceManager.h"

#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QSettings>
#include <QThread>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DialogPresenter.h"
#include "pj_base/data_source_protocol.h"
#include "pj_base/dataset.hpp"
#include "pj_base/span.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_plugins/host/data_source_handle.hpp"
#include "pj_plugins/host/data_source_library.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/TopicDemandTracker.h"
#include "pj_runtime/TsanAnnotations.h"
#include "pj_runtime/UsageCounters.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

Q_LOGGING_CATEGORY(lcStream, "pj.app.streamingmanager")

// 20 Hz: perceived latency against per-poll host overhead (reasoning, not measured).
constexpr int kPollPeriodMs = 50;

// ObjectStore memory ceiling per object topic. Finite so the paused tail buffer stays bounded: at the cap, FIFO
// retention drops the oldest entries.
constexpr size_t kStreamingObjectMemoryBudget = 64U * 1024U * 1024U;

constexpr const char* kPluginConfigKeyPrefix = "PluginConfig/";
constexpr const char* kStreamingBufferKey = "MainWindow.streamingBufferValue";

QString pluginConfigKey(const std::string& plugin_id) {
  return QString::fromLatin1(kPluginConfigKeyPrefix) + QString::fromStdString(plugin_id);
}

}  // namespace

struct StreamingSourceManager::StreamingSession {
  std::string plugin_id;    ///< display name, as shown in the streaming combo
  std::string manifest_id;  ///< the plugin's stable manifest id, for provenance
  /// The dataset is "[stream] <label>": the layout record's source_name, else the plugin name.
  QString label;
  /// The layout record this session started from; empty for an interactive start.
  QString layout_id;
  /// What a source-bound save writes back: the layout's template, or the configuration the dialog produced. Set
  /// before the worker starts and never after, so the GUI thread reads it without calling into the plugin.
  QString config_template;
  DatasetId dataset_id = 0;
  DataSourceHandle handle;
  /// handle.capabilities(), read once after start(): it is a plugin call, and the ABI gives the mask no way to change
  /// once the source runs, so every later gate (per-topic pause, recordability) reads this copy.
  uint64_t capabilities = 0;
  std::unique_ptr<DataSourceRuntimeHost> runtime_host;
  std::unique_ptr<QThread> worker;

  explicit StreamingSession(DataSourceHandle&& h) : handle(std::move(h)) {}
};

struct StreamingSourceManager::PendingStart {
  std::unique_ptr<StreamingSession> session;  ///< the dialog borrows its handle
  dialog_presenter::DialogCancelFn cancel;    ///< closes the dialog as a rejection
};

StreamingSourceManager::StreamingSourceManager(
    SessionManager& session, ExtensionCatalogService& extensions, CatalogModel& catalog,
    TopicDemandTracker& topic_demand_tracker, QWidget* dialog_parent, QObject* parent)
    : QObject(parent),
      session_manager_(session),
      extensions_(extensions),
      catalog_(catalog),
      topic_demand_tracker_(topic_demand_tracker),
      dialog_parent_(dialog_parent),
      secondary_object_store_(std::make_unique<ObjectStore>()),
      secondary_data_engine_(std::make_unique<DataEngine>()) {
  retention_seconds_ = QSettings().value(kStreamingBufferKey, retention_seconds_).toInt();
  connect(
      &topic_demand_tracker_, &TopicDemandTracker::activeTopicsChanged, this,
      &StreamingSourceManager::onActiveTopicsChanged);
}

StreamingSourceManager::~StreamingSourceManager() {
  cancelPendingStarts();  // their dialogs borrow session handles that die with this object
  // Stop and join here: the queued onWorkerFinished will never run. All stops are requested before the first join.
  for (auto& [dataset_id, sess] : sessions_) {
    if (sess->runtime_host != nullptr) {
      sess->runtime_host->requestStop();  // flag-only: the workers are still running
    }
  }
  for (auto& [dataset_id, sess] : sessions_) {
    if (sess->worker != nullptr) {
      sess->worker->wait();
      tsan::acquire(sess.get());  // see the end of workerLoop
    }
  }
}

bool StreamingSourceManager::hasActiveSession() const {
  return !sessions_.empty();
}

DataEngine& StreamingSourceManager::secondaryEngineForTests() {
  return *secondary_data_engine_;
}

ObjectStore& StreamingSourceManager::secondaryStoreForTests() {
  return *secondary_object_store_;
}

bool StreamingSourceManager::stopDatasetAndWait(DatasetId dataset_id, const QString& reason) {
  auto it = sessions_.find(dataset_id);
  if (it == sessions_.end()) {
    // A start still at its dialog owns this dataset too: withdrawn, it discards it itself.
    const auto pending = std::find_if(pending_starts_.begin(), pending_starts_.end(), [dataset_id](const auto& start) {
      return start->session != nullptr && start->session->dataset_id == dataset_id;
    });
    if (pending != pending_starts_.end() && (*pending)->cancel) {
      const dialog_presenter::DialogCancelFn cancel = (*pending)->cancel;
      cancel();
    }
    return false;
  }

  StreamingSession* sess = it->second.get();
  requestStopDataset(dataset_id, reason);
  if (sess->worker != nullptr) {
    sess->worker->wait();
    tsan::acquire(sess);  // see the end of workerLoop
  }

  // Only a FAILURE the worker hit before this stop landed: a requested stop is orderly, so the reason stays empty
  // and a recording of this source stays clean (RecordingService marks any non-empty reason non-clean).
  QString stopped_reason;
  if (sess->runtime_host != nullptr) {
    stopped_reason = QString::fromStdString(sess->runtime_host->stopFailure());
  }

  rememberSkippedLazy(*sess);
  sessions_.erase(it);
  removeSecondaryDataset(dataset_id);
  // After the erase, so clearDataset's activeTopicsChanged finds no session and pushes nothing through a dying
  // handle. The capability flag goes first: that emission triggers the panel's dimming refresh, which must already
  // see the dataset as non-pausable, or rows with data stay grey after the disconnect.
  catalog_.setPerTopicPauseCapable(dataset_id, false);
  topic_demand_tracker_.clearDataset(dataset_id);
  catalog_.clearAdvertisedTopics(dataset_id);
  emit streamStopped(dataset_id, stopped_reason);
  return true;
}

bool StreamingSourceManager::requestStopDataset(DatasetId dataset_id, const QString& reason) {
  const auto it = sessions_.find(dataset_id);
  if (it == sessions_.end() || it->second->runtime_host == nullptr) {
    return false;
  }
  // Flag-only: a requested stop is orderly, not a failure, so it records no reason (`reason` belongs
  // to the log). Whoever does not join the worker hears of the end through onWorkerFinished.
  qCInfo(lcStream) << "stopping stream" << dataset_id << ":" << reason;
  it->second->runtime_host->requestStop();
  return true;
}

void StreamingSourceManager::stopAllAndWait(const QString& reason) {
  cancelPendingStarts();  // a start still at its dialog has a dataset too
  std::vector<DatasetId> dataset_ids;
  dataset_ids.reserve(sessions_.size());
  for (const auto& [dataset_id, _sess] : sessions_) {
    dataset_ids.push_back(dataset_id);
  }
  for (const DatasetId dataset_id : dataset_ids) {
    stopDatasetAndWait(dataset_id, reason);
  }
}

void StreamingSourceManager::rememberSkippedLazy(const StreamingSession& session) {
  if (session.runtime_host != nullptr) {
    ended_skipped_lazy_[session.dataset_id] = session.runtime_host->recordTapSkippedLazy();
  }
}

RecordingTargets StreamingSourceManager::recordingTargets() {
  // Whatever ended before this recording belongs to the previous one.
  ended_skipped_lazy_.clear();

  RecordingTargets result;
  result.targets.reserve(sessions_.size());
  for (const auto& [dataset_id, session] : sessions_) {
    // The recorder taps the delegated-ingest seam. A source that writes decoded samples straight into the datastore
    // never passes through it, so a tap on it would produce an empty, channel-less file.
    if ((session->capabilities & kCapabilityDelegatedIngest) == 0) {
      result.excluded << QString::fromStdString(session->plugin_id);
      continue;
    }
    result.targets.push_back(
        RecordingTarget{
            .target_key = static_cast<uint64_t>(dataset_id),
            .source_id = session->plugin_id,
            .plugin_id = session->manifest_id,
            .attach =
                [this, dataset_id](std::shared_ptr<RecordTap> tap) {
                  auto found = sessions_.find(dataset_id);
                  if (found == sessions_.end()) {
                    return false;  // the session ended between the press and the attach
                  }
                  found->second->runtime_host->setRecordTap(std::move(tap));
                  return true;
                },
            .skipped_lazy = [this, dataset_id]() -> uint64_t {
              if (const auto found = sessions_.find(dataset_id); found != sessions_.end()) {
                return found->second->runtime_host->recordTapSkippedLazy();
              }
              // Ended mid-recording: the counter as it stood when its host died, so its holes still reach the user.
              const auto ended = ended_skipped_lazy_.find(dataset_id);
              return ended != ended_skipped_lazy_.end() ? ended->second : 0;
            },
        });
  }
  return result;
}

void StreamingSourceManager::onBufferChanged(int seconds) {
  // Every session's next onIngestTick applies the new window to its stores; consumers with their own history
  // (the 3D TF buffer) hear it through retentionWindowChanged.
  retention_seconds_ = seconds;
  const auto window_ns = static_cast<qint64>(retention_seconds_) * 1'000'000'000LL;
  for (const auto& [dataset_id, _sess] : sessions_) {
    emit retentionWindowChanged(dataset_id, window_ns);
  }
}

void StreamingSourceManager::onStartRequested(const QString& plugin_id) {
  if (plugin_id.isEmpty()) {
    qCWarning(lcStream) << "Start requested but no stream source selected.";
    emit setupError(tr("No stream source selected."));
    return;
  }
  startSession(plugin_id);
}

void StreamingSourceManager::applyWriteTargetsForCurrentPauseState(DataSourceRuntimeHost& host) {
  if (paused_) {
    // The primary, which the views read, stays frozen because nothing writes to it.
    host.setObjectStoreTarget(secondary_object_store_.get());
    host.setDataEngineTarget(secondary_data_engine_.get());
  } else {
    host.setObjectStoreTarget(&session_manager_.objectStore());
    host.setDataEngineTarget(&session_manager_.dataEngine());
  }
}

void StreamingSourceManager::onPauseToggled(bool paused) {
  paused_ = paused;
  if (paused) {
    for (auto& [_id, sess] : sessions_) {
      applyWriteTargetsForCurrentPauseState(*sess->runtime_host);
    }
    return;
  }
  // Resume. Swap targets BEFORE flushing so any push already in flight finishes on the secondary and the next one
  // goes to the primary; otherwise the flush races new arrivals. A push that reaches the primary before
  // the flush is fine: the paused rows still land.
  for (auto& [_id, sess] : sessions_) {
    applyWriteTargetsForCurrentPauseState(*sess->runtime_host);
  }
  flushSecondaryIntoPrimary();
  flushSecondaryDataEngineIntoPrimary();
  // Catch the eager filters up on the merged tail BEFORE re-trimming, so a stateful node (an integral) consumes the
  // interval about to be evicted.
  for (const auto& [streaming_dataset_id, _catchup_sess] : sessions_) {
    const auto filter_inputs = session_manager_.createReader().listTopics(streaming_dataset_id);
    (void)session_manager_.dataProcessorService().advanceOnCommit(filter_inputs);
    // A marker rule whose inputs changed only during the pause would keep drawing
    // pre-pause data: mark it stale, and the batch engine's brake re-runs it.
    session_manager_.recomputeMarkersForChanged(filter_inputs, streaming_dataset_id);
  }
  // DataEngine::flushTo does not trim (ObjectStore::flushTo does). Scoped to each streaming dataset, so a co-loaded
  // file keeps its history.
  {
    const auto window_ns = static_cast<int64_t>(retention_seconds_) * 1'000'000'000LL;
    for (const auto& [streaming_dataset_id, _sess] : sessions_) {
      session_manager_.dataEngine().enforceRetention(window_ns, streaming_dataset_id);
    }
  }
  // A live ingest, so follow-live consumers (plot auto-fit, Scene2D latest frame) land on the merged live edge.
  for (const auto& [dataset_id, _] : sessions_) {
    const auto topic_ids = session_manager_.createReader().listTopics(dataset_id);
    session_manager_.notifyIngest(QVector<TopicId>(topic_ids.begin(), topic_ids.end()), /*live=*/true, dataset_id);
  }
}

void StreamingSourceManager::flushSecondaryDataEngineIntoPrimary() {
  // Moves chunk deques, copies no buffer. Topics match by dataset id and name (the ids are mirrored at session
  // creation); the secondary keeps its topics registered, so the next pause does not re-mirror. Fails as a whole
  // when one source topic has no destination.
  if (auto result = secondary_data_engine_->flushTo(session_manager_.dataEngine()); !result) {
    qCWarning(lcStream) << "flushSecondaryDataEngineIntoPrimary failed:" << QString::fromStdString(result.error());
  }
}

void StreamingSourceManager::flushSecondaryIntoPrimary() {
  // Moves each entry (a shared payload or a fetch closure), so no payload bytes are copied and no lazy fetcher runs.
  // Topics match by descriptor (DataSourceRuntimeHost::cbEnsureParserBinding registers them on both stores) and stay
  // registered on the secondary.
  if (auto result = secondary_object_store_->flushTo(session_manager_.objectStore()); !result) {
    qCWarning(lcStream) << "flushSecondaryIntoPrimary failed:" << QString::fromStdString(result.error());
  }
}

std::unique_ptr<StreamingSourceManager::StreamingSession> StreamingSourceManager::createSession(
    const LoadedDataSource& source, const QString& label, QString& error) {
  const QString source_name = QString::fromStdString(source.name);
  DataSourceHandle handle = source.library.createHandle();
  if (!handle.valid()) {
    qCWarning(lcStream) << "createHandle failed for" << source_name;
    error = tr("Plugin '%1': createHandle failed.").arg(source_name);
    return nullptr;
  }

  DataEngine& engine = session_manager_.dataEngine();
  const QString display_name = u"[stream] %1"_s.arg(label);

  // One TimeDomain per stream, so each shifts independently on the Source Timeline. The secondary must hold the
  // same id, forced explicitly: the engines' counters drift whenever the primary gets a domain the secondary does
  // not (a file load between streams).
  auto td_or = engine.createTimeDomain(display_name.toStdString());
  if (!td_or.has_value()) {
    error = tr("Could not create the time domain.");
    return nullptr;
  }
  const TimeDomainId td_id = *td_or;
  auto mirrored_td = secondary_data_engine_->createTimeDomain(display_name.toStdString(), td_id);
  if (!mirrored_td.has_value() || *mirrored_td != td_id) {
    error = tr("secondary time domain lockstep failed.");
    return nullptr;
  }

  auto dataset_or =
      engine.createDataset(DatasetDescriptor{.source_name = display_name.toStdString(), .time_domain_id = td_id});
  if (!dataset_or.has_value()) {
    error = tr("createDataset failed: %1").arg(QString::fromStdString(dataset_or.error()));
    return nullptr;
  }
  const DatasetId dataset_id = static_cast<DatasetId>(*dataset_or);
  const PJ_data_source_handle_t source_handle{static_cast<uint32_t>(dataset_id)};

  // Same id and label on the secondary: the id forced for the same reason as the time domain, the label passed so the
  // mirror never derives its own. DataSourceRuntimeHost mirrors the topics under it (cbEnsureParserBinding).
  auto mirrored_dataset = secondary_data_engine_->createDataset(
      DatasetDescriptor{.source_name = display_name.toStdString(), .time_domain_id = td_id}, dataset_id,
      engine.datasetLabel(dataset_id));
  if (!mirrored_dataset.has_value()) {
    error = tr("secondary createDataset failed: %1").arg(QString::fromStdString(mirrored_dataset.error()));
    discardDataset(dataset_id);
    return nullptr;
  }

  // Read BEFORE `handle` moves into the session (a moved-from handle yields a null token). The runtime host stores
  // lazy ObjectStore anchors/fetchers that are plugin-library code and can outlive the catalog entry (a marketplace
  // reload mid-session, app close), so the library must stay mapped until they drop.
  auto library_keepalive = handle.libraryOwner();
  auto session = std::make_unique<StreamingSession>(std::move(handle));
  session->plugin_id = source.name;
  session->manifest_id = source.id;
  session->label = label;
  session->dataset_id = dataset_id;
  session->runtime_host = std::make_unique<DataSourceRuntimeHost>(
      engine, extensions_, dataset_id, source_handle, session_manager_.objectStore(), source.name,
      [this](ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
        session_manager_.registerObjectTopicParser(id, std::move(parser));
      },
      secondary_object_store_.get(), secondary_data_engine_.get(), std::move(library_keepalive),
      session_manager_.ingestTapsShared());
  // No file to re-read: a lazy policy would re-invoke each message's fetcher on every pull, so object bytes stay
  // resident.
  session->runtime_host->policyResolver().setDefault(sdk::ObjectIngestPolicy::kEager);
#ifdef PJ_TARGET_WASM
  // Browser RobotDescription handlers are object-only: defer their payloads until RobotModel requests one instead
  // of invoking the absent scalar path.
  session->runtime_host->policyResolver().setForType(
      sdk::BuiltinObjectType::kRobotDescription, sdk::ObjectIngestPolicy::kPureLazy);
#endif
  // Called on the plugin's thread (inside start() or poll()): hop to the GUI thread before touching CatalogModel or
  // TopicDemandTracker. It may fire before start() returns or after the session is gone, so applyAdvertisedTopics
  // re-checks the session.
  session->runtime_host->on_available_topics =
      [this, dataset_id](std::vector<DataSourceRuntimeHost::AdvertisedTopicInfo> topics) {
        std::vector<AdvertisedTopic> owned;
        owned.reserve(topics.size());
        for (auto& topic : topics) {
          owned.push_back(AdvertisedTopic{QString::fromStdString(topic.topic_name), topic.classification});
        }
        tsan::invokeQueued(this, [this, dataset_id, owned = std::move(owned)]() mutable {
          applyAdvertisedTopics(dataset_id, std::move(owned));
        });
      };

  ServiceRegistryBuilder registry;
  if (auto status = session->runtime_host->registerServices(registry); !status) {
    error = tr("Plugin '%1': service registration failed: %2").arg(source_name, QString::fromStdString(status.error()));
    session.reset();
    discardDataset(dataset_id);
    return nullptr;
  }
  if (auto status = session->handle.bind(registry.view()); !status) {
    error = tr("Plugin '%1': bind failed: %2").arg(source_name, QString::fromStdString(status.error()));
    session.reset();
    discardDataset(dataset_id);
    return nullptr;
  }
  return session;
}

void StreamingSourceManager::removeSecondaryDataset(DatasetId dataset_id) {
  for (const ObjectTopicId topic_id : secondary_object_store_->listTopics(dataset_id)) {
    secondary_object_store_->removeTopic(topic_id);
  }
  secondary_data_engine_->removeDataset(dataset_id);
}

void StreamingSourceManager::discardDataset(DatasetId dataset_id) {
  removeSecondaryDataset(dataset_id);
  session_manager_.evictDatasetObjects(dataset_id);
  catalog_.removeDataset(dataset_id, /*tombstone=*/false);
  if (session_manager_.removeDataset(dataset_id)) {
    catalog_.rebuildIfChanged();
  }
}

void StreamingSourceManager::startSession(
    const QString& plugin_id, const QString& dialog_title_prefix, const std::function<bool()>& still_allowed,
    const dialog_presenter::DialogWatch& on_dialog, StartDone done) {
  const auto refuse = [this, &done](const QString& error) {
    emit setupError(error);
    if (done) {
      done(0, error);
    }
  };
  const LoadedDataSource* source_ptr = findDataSourceByIdentity(extensions_.streamSources(), {}, plugin_id);
  if (source_ptr == nullptr) {
    qCWarning(lcStream) << "Stream plugin not found:" << plugin_id;
    refuse(tr("Stream plugin '%1' not found.").arg(plugin_id));
    return;
  }
  const LoadedDataSource& source = *source_ptr;
  QString error;
  std::unique_ptr<StreamingSession> session = createSession(source, QString::fromStdString(source.name), error);
  if (session == nullptr) {
    refuse(error);
    return;
  }
  auto pending = std::make_shared<PendingStart>();
  pending->session = std::move(session);
  pending_starts_.push_back(pending);
  configureAndLaunch(source, pending, dialog_title_prefix, still_allowed, on_dialog, std::move(done));
}

bool StreamingSourceManager::hasPendingStart() const {
  return !pending_starts_.empty();
}

void StreamingSourceManager::cancelPendingStarts() {
  // A copy: each cancel runs its start's completion, which takes it off the list.
  const std::vector<std::shared_ptr<PendingStart>> pending = pending_starts_;
  for (const std::shared_ptr<PendingStart>& start : pending) {
    if (start->cancel) {
      start->cancel();
    }
  }
}

void StreamingSourceManager::configureAndLaunch(
    const RuntimeDataSourcePlugin& source, const std::shared_ptr<PendingStart>& pending,
    const QString& dialog_title_prefix, const std::function<bool()>& still_allowed,
    const dialog_presenter::DialogWatch& on_dialog, StartDone done) {
  StreamingSession& session = *pending->session;
  const QString source_name = QString::fromStdString(source.name);
  const QString config_key = pluginConfigKey(source.name);
  const std::string config = QSettings().value(config_key, QString()).toString().toStdString();
  // Always, even with an empty config: stream plugins wire the runtime host into their dialog here (the encodings
  // combo, via list_available_encodings); skipped, the dialog shows "(no parsers available)" and OK stays disabled.
  if (auto status = session.handle.loadConfig(config); !status) {
    qCInfo(lcStream) << "loadConfig failed; proceeding with plugin defaults:" << QString::fromStdString(status.error());
  }

  // Pre-populates the embedded parser sub-dialog (e.g. JSON "Use embedded timestamp").
  std::string initial_parser_config;
  {
    auto saved_cfg = nlohmann::json::parse(config, nullptr, false);
    if (!saved_cfg.is_discarded() && saved_cfg.contains("_parser_config")) {
      initial_parser_config = saved_cfg["_parser_config"].get<std::string>();
    }
  }

  // The rest of the start, once the dialog is answered (at once for a source that has none). It owns the cleanup:
  // createSession made the dataset before the dialog opened, so any exit that does not launch takes it back, or an
  // empty dataset lingers and counts in the catalog's duplicate-label ordinals. The session goes first (as in
  // startFromLayout) and the dataset is gone before the failure is reported.
  auto finish = [self = QPointer<StreamingSourceManager>(this), this, pending, source_name, config_key, config,
                 still_allowed, done = std::move(done)](const dialog_presenter::DataSourceResult& dlg) {
    if (self == nullptr) {
      return;  // the manager went first, without withdrawing this start: nothing is left to clean
    }
    std::erase(pending_starts_, pending);
    const DatasetId dataset_id = pending->session->dataset_id;
    QString error;
    bool launched = false;
    if (dlg.outcome == dialog_presenter::Outcome::kPluginContractViolation) {
      error = tr("Plugin contract violation: %1. Reinstall the plugin from the Marketplace.")
                  .arg(QString::fromStdString(dlg.error));
    } else if (dlg.outcome == dialog_presenter::Outcome::kRejected) {
      qCInfo(lcStream) << "User cancelled stream dialog for" << source_name;
    } else if (still_allowed && !still_allowed()) {
      qCInfo(lcStream) << "Stream start no longer allowed after its dialog:" << source_name;
    } else {
      std::string chosen = config;
      bool configured = true;
      if (dlg.payload.has_value()) {
        chosen = dlg.payload->saved_config;
        // Under "_parser_config", the key the stream plugin forwards to ensureParserBinding for each topic.
        if (!dlg.payload->parser_config.empty()) {
          auto cfg = nlohmann::json::parse(chosen, nullptr, false);
          if (!cfg.is_discarded()) {
            cfg["_parser_config"] = dlg.payload->parser_config;
            chosen = cfg.dump();
          }
        }
        if (auto status = pending->session->handle.loadConfig(chosen); !status) {
          error = tr("Plugin '%1': loadConfig (post-dialog) failed: %2")
                      .arg(source_name, QString::fromStdString(status.error()));
          configured = false;
        }
      }
      if (configured) {
        QSettings().setValue(config_key, QString::fromStdString(chosen));
        pending->session->config_template = QString::fromStdString(chosen);
        launched = launchSession(std::move(pending->session), error);
      }
    }
    if (!launched) {
      pending->session.reset();
      discardDataset(dataset_id);
      if (!error.isEmpty()) {
        emit streamError(dataset_id, error);
      }
    }
    if (done) {
      done(launched ? dataset_id : 0, error);
    }
  };
  // Never the blocking presenter: a browser cannot run a nested event loop, and one path for every platform keeps
  // the desktop tests covering what the browser runs. The dialog is still modal.
  pending->cancel = dialog_presenter::showDataSourceDialogAsync(
      {
          .source = source,
          .handle = session.handle,
          .catalog = extensions_,
          .parent = dialog_parent_,
          .initial_parser_config = initial_parser_config,
          .section_band_metrics =
              chrome_metrics_provider_ ? std::optional<ChromeMetrics>(chrome_metrics_provider_()) : std::nullopt,
          .title_prefix = dialog_title_prefix,
          // The closer is in place before anyone hears the dialog is up: a listener may withdraw
          // the start at once.
          .on_dialog =
              [pending, on_dialog](dialog_presenter::DialogCancelFn close) {
                if (close) {
                  pending->cancel = close;
                }
                if (on_dialog) {
                  on_dialog(std::move(close));
                }
              },
      },
      std::move(finish));
}

DatasetId StreamingSourceManager::startFromLayout(
    const layout_xml::StreamingSourceRef& source, const QString& expanded_config, QString& error) {
  error.clear();
  const LoadedDataSource* plugin =
      findDataSourceByIdentity(extensions_.streamSources(), source.plugin_manifest_id, source.plugin_id);
  if (plugin == nullptr) {
    error = tr("Stream plugin '%1' is not installed.")
                .arg(source.plugin_id.isEmpty() ? source.plugin_manifest_id : source.plugin_id);
    return 0;
  }
  const QString label = source.source_name.isEmpty() ? QString::fromStdString(plugin->name) : source.source_name;
  std::unique_ptr<StreamingSession> session = createSession(*plugin, label, error);
  if (session == nullptr) {
    return 0;
  }
  const DatasetId dataset_id = session->dataset_id;
  session->layout_id = source.id;
  session->config_template = source.plugin_config_json;
  if (auto status = session->handle.loadConfig(expanded_config.toStdString()); !status) {
    error = tr("Plugin '%1' refused the saved configuration: %2")
                .arg(QString::fromStdString(plugin->name), QString::fromStdString(status.error()));
    session.reset();
    discardDataset(dataset_id);
    return 0;
  }
  if (!launchSession(std::move(session), error)) {
    discardDataset(dataset_id);
    return 0;
  }
  return dataset_id;
}

bool StreamingSourceManager::launchSession(std::unique_ptr<StreamingSession> session, QString& error) {
  const DatasetId dataset_id = session->dataset_id;
  // Before start(), so a source added mid-pause writes into the tail buffer from its first sample (a push-model
  // plugin may deliver from its own thread inside start()), and after the dialog, during which the user may have
  // toggled pause. From here to the worker's start nothing runs the event loop, so paused_ cannot change;
  // onPauseToggled keeps every live session in step afterwards.
  applyWriteTargetsForCurrentPauseState(*session->runtime_host);

  if (auto status = session->handle.start(); !status) {
    error = tr("Plugin '%1': start failed: %2")
                .arg(QString::fromStdString(session->plugin_id), QString::fromStdString(status.error()));
    return false;
  }

  session->capabilities = session->handle.capabilities();
  usageCounters().countPlugin(PluginUse::kStreamStart, session->manifest_id);
  // CurveListPanel dims a topic as unsubscribed only for a dataset that supports the pause.
  catalog_.setPerTopicPauseCapable(dataset_id, (session->capabilities & kCapabilityPerTopicPause) != 0);

  // Usually empty (nothing is displayed yet): a demand-capable source is told to subscribe to nothing instead of
  // keeping its own default (a stale selection from a previous session). Later changes arrive through
  // onActiveTopicsChanged.
  pushActiveTopics(*session, topic_demand_tracker_.activeTopics(dataset_id));

  // No event loop in the worker: it only posts back. It gets its session, never a sessions_ lookup (the GUI thread
  // inserts and erases other sessions while it runs); the session is erased only after the join.
  StreamingSession* const worker_session = session.get();
  session->worker.reset(QThread::create([this, worker_session]() { workerLoop(*worker_session); }));

  QThread* worker_ptr = session->worker.get();
  sessions_.emplace(dataset_id, std::move(session));
  worker_ptr->start();

  emit streamStarted(dataset_id);
  emit retentionWindowChanged(dataset_id, static_cast<qint64>(retention_seconds_) * 1'000'000'000LL);
  return true;
}

std::vector<std::pair<DatasetId, QString>> StreamingSourceManager::layoutIds() const {
  std::vector<DatasetId> dataset_ids;
  dataset_ids.reserve(sessions_.size());
  for (const auto& [dataset_id, unused_session] : sessions_) {
    (void)unused_session;
    dataset_ids.push_back(dataset_id);
  }
  std::sort(dataset_ids.begin(), dataset_ids.end());
  std::vector<std::pair<DatasetId, QString>> ids;
  ids.reserve(dataset_ids.size());
  QSet<QString> taken;
  for (const DatasetId dataset_id : dataset_ids) {
    const QString& layout_id = sessions_.at(dataset_id)->layout_id;
    const QString base = layout_id.isEmpty() ? u"stream"_s : layout_id;
    QString id = base;
    for (int suffix = 2; taken.contains(id); ++suffix) {
      id = u"%1-%2"_s.arg(base).arg(suffix);
    }
    taken.insert(id);
    ids.emplace_back(dataset_id, std::move(id));
  }
  return ids;
}

QList<layout_xml::StreamingSourceRef> StreamingSourceManager::layoutSources() const {
  QList<layout_xml::StreamingSourceRef> sources;
  for (const auto& [dataset_id, id] : layoutIds()) {
    const StreamingSession& session = *sessions_.at(dataset_id);
    layout_xml::StreamingSourceRef source;
    source.id = id;
    source.source_name = session.label;
    source.plugin_id = QString::fromStdString(session.plugin_id);
    source.plugin_manifest_id = QString::fromStdString(session.manifest_id);
    source.plugin_config_json = session.config_template;
    source.dataset.display_offset_ns = session_manager_.sourceDisplayOffset(dataset_id).value.count();
    source.dataset.has_display_offset = true;
    sources.push_back(std::move(source));
  }
  return sources;
}

QString StreamingSourceManager::layoutSourceId(DatasetId dataset_id) const {
  if (!sessions_.contains(dataset_id)) {
    return {};
  }
  for (const auto& [session_dataset, id] : layoutIds()) {
    if (session_dataset == dataset_id) {
      return id;
    }
  }
  return {};
}

DatasetId StreamingSourceManager::liveSessionFor(
    const layout_xml::StreamingSourceRef& source, const QSet<DatasetId>& taken) const {
  const LoadedDataSource* plugin =
      findDataSourceByIdentity(extensions_.streamSources(), source.plugin_manifest_id, source.plugin_id);
  if (plugin == nullptr) {
    return 0;
  }
  DatasetId first_match = 0;
  for (const auto& [dataset_id, id] : layoutIds()) {
    const StreamingSession& session = *sessions_.at(dataset_id);
    if (taken.contains(dataset_id) || session.manifest_id != plugin->id ||
        session.config_template != source.plugin_config_json) {
      continue;
    }
    if (id == source.id) {
      return dataset_id;
    }
    if (first_match == 0) {
      first_match = dataset_id;
    }
  }
  return first_match;
}

void StreamingSourceManager::workerLoop(StreamingSession& session) {
  const DatasetId dataset_id = session.dataset_id;
  StreamingSession* sess = &session;

  while (!sess->runtime_host->stopRequested()) {
    if (auto status = sess->handle.poll(); !status) {
      const QString err = QString::fromStdString(status.error());
      tsan::invokeQueued(this, [this, dataset_id, err]() { emit streamError(dataset_id, err); });
      sess->runtime_host->requestStop(std::string("plugin poll error: ") + err.toStdString());
      break;
    }
    sess->runtime_host->flushPending();
    // The write host bypasses SessionManager::commitChunks, so the GUI-thread tick announces the ingest. It also
    // runs the trim, which keeps retention_seconds_ and paused_ GUI-thread only.
    tsan::invokeQueued(this, [this, dataset_id]() { onIngestTick(dataset_id); });
    QThread::msleep(kPollPeriodMs);
  }

  sess->handle.stop();
  sess->runtime_host->flushAll();

  tsan::invokeQueued(this, [this, dataset_id]() { onWorkerFinished(dataset_id); });
  // Last act: the joins acquire this per-session token once wait() returns (QThread::wait() is a
  // futex inside prebuilt Qt that TSan cannot see), so the GUI thread's teardown of the session is
  // ordered after flushAll() and everything above.
  tsan::release(sess);
}

void StreamingSourceManager::ingestTickForTests(DatasetId dataset_id) {
  onIngestTick(dataset_id);
}

void StreamingSourceManager::onIngestTick(DatasetId dataset_id) {
  const auto window_ns = static_cast<int64_t>(retention_seconds_) * 1'000'000'000LL;
  // Every tick, because the plugin creates object topics mid-stream and a one-time budget would miss them;
  // idempotent and cheap.
  if (const auto session_it = sessions_.find(dataset_id); session_it != sessions_.end()) {
    session_it->second->runtime_host->setObjectRetentionBudget(window_ns, kStreamingObjectMemoryBudget);
  }
  const auto topic_ids = session_manager_.createReader().listTopics(dataset_id);
  // Eager filters run BEFORE the trim: a stateful node must consume every sample before it ages out of the window.
  // On the GUI thread, serialized with the Filter Editor's apply/update. Every topic of the dataset, not only the
  // poll's: rows also arrive from a toolbox committing straight to the engine and from the terminal flush after the
  // last tick, and a node with nothing new is cheap (docs/derived_objects/ARCHITECTURE.md "Streaming"). While
  // paused the writes land in the secondary engine (no IncrementalEngine); the resume catches up.
  if (!paused_) {
    (void)session_manager_.dataProcessorService().advanceOnCommit(topic_ids);
  }

  // Trim only the engine being written (the secondary while paused): the frozen primary keeps its snapshot for
  // scrub-back. Scoped to this dataset, so a co-loaded file keeps its full history.
  if (paused_) {
    secondary_data_engine_->enforceRetention(window_ns, dataset_id);
  } else {
    session_manager_.dataEngine().enforceRetention(window_ns, dataset_id);
  }

  // The live-data marker hook (the write host bypasses SessionManager::commitChunks): marks stale the marker
  // generators whose inputs moved; its per-topic stamp (newest timestamp, row count, retention floor, series
  // generation) also sees a retention-only change on a quiet topic. Skipped while paused, like the filters.
  if (!paused_) {
    session_manager_.recomputeMarkersForChanged(topic_ids, dataset_id);
  }
  // Every topic: processor outputs and retention-only changes reach no write host, so a filtered curve refreshes
  // only through this list. Never skipped when nothing changed: `live` alone drives the object-only consumers
  // (Scene2D / Scene3D docks), and the dataset reaches the on-demand live edge even with no scalar topic.
  session_manager_.notifyIngest(QVector<TopicId>(topic_ids.begin(), topic_ids.end()), /*live=*/!paused_, dataset_id);
}

void StreamingSourceManager::onWorkerFinished(DatasetId dataset_id) {
  auto it = sessions_.find(dataset_id);
  if (it == sessions_.end()) {
    return;
  }
  StreamingSession* sess = it->second.get();
  if (sess->worker != nullptr) {
    sess->worker->wait();
    tsan::acquire(sess);  // see the end of workerLoop
  }
  // Empty for an orderly end, the plugin's own end-of-stream included; a callback failure the plugin recovered from
  // stays in the log. Read after the join, once every writer of it has stopped.
  const QString reason = QString::fromStdString(sess->runtime_host->stopFailure());

  rememberSkippedLazy(*sess);
  sessions_.erase(it);
  catalog_.setPerTopicPauseCapable(dataset_id, false);  // before clearDataset: see stopDatasetAndWait
  topic_demand_tracker_.clearDataset(dataset_id);
  catalog_.clearAdvertisedTopics(dataset_id);
  emit streamStopped(dataset_id, reason);
}

void StreamingSourceManager::pushActiveTopics(StreamingSession& session, const std::vector<QString>& active_topics) {
  if ((session.capabilities & kCapabilityPerTopicPause) == 0) {
    return;
  }
  std::vector<std::string> owned;
  owned.reserve(active_topics.size());
  for (const QString& name : active_topics) {
    owned.push_back(name.toStdString());
  }
  std::vector<std::string_view> views(owned.begin(), owned.end());
  if (auto status = session.handle.setActiveTopics(Span<const std::string_view>(views.data(), views.size())); !status) {
    qCWarning(lcStream) << "setActiveTopics failed:" << QString::fromStdString(status.error());
  }
}

void StreamingSourceManager::onActiveTopicsChanged(DatasetId dataset_id, const std::vector<QString>& active_topics) {
  auto it = sessions_.find(dataset_id);
  if (it == sessions_.end()) {
    return;
  }
  pushActiveTopics(*it->second, active_topics);
}

void StreamingSourceManager::applyAdvertisedTopics(DatasetId dataset_id, std::vector<AdvertisedTopic> topics) {
  if (sessions_.find(dataset_id) == sessions_.end()) {
    return;  // torn down before this queued call landed
  }
  std::vector<QString> infra_names;
  for (const AdvertisedTopic& topic : topics) {
    if (topic.classification == sdk::BuiltinObjectType::kFrameTransforms ||
        topic.classification == sdk::BuiltinObjectType::kCameraInfo) {
      infra_names.push_back(topic.topic_name);
    }
  }
  catalog_.setAdvertisedTopics(dataset_id, topics);
  topic_demand_tracker_.setInfrastructureTopics(dataset_id, infra_names);
}

}  // namespace PJ
