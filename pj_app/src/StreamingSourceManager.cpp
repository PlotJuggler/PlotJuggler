// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "StreamingSourceManager.h"

#include <QLoggingCategory>
#include <QMetaObject>
#include <QSettings>
#include <QThread>
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
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

Q_LOGGING_CATEGORY(lcStream, "pj.app.streamingmanager")

// v1 hardcoded. Promote to a member + setter if/when we want it configurable.
// 50ms (20Hz) balances perceived latency against host overhead.
constexpr int kPollPeriodMs = 50;

// ObjectStore memory ceiling per object topic. Finite so the tail buffer
// (post-pause pushes accumulating on the secondary store) is bounded;
// at the cap the standard FIFO retention drops the oldest tail entries.
// 64 MiB is low enough to expose tail growth quickly with raw 1080p
// imagery during testing (~10 frames at 6 MB/frame). Production might
// raise this; image-compression (Fase 3) will revisit.
constexpr size_t kStreamingObjectMemoryBudget = 64U * 1024U * 1024U;

constexpr const char* kPluginConfigKeyPrefix = "PluginConfig/";
constexpr const char* kStreamingBufferKey = "MainWindow.streamingBufferValue";

QString pluginConfigKey(const std::string& plugin_id) {
  return QString::fromLatin1(kPluginConfigKeyPrefix) + QString::fromStdString(plugin_id);
}

// Looks up a streaming plugin by display name. Returns nullptr when no
// streaming plugin matches — caller surfaces the error.
const LoadedDataSource* resolveStreamSource(const ExtensionCatalogService& extensions, const QString& plugin_id) {
  for (const auto* ds : extensions.streamSources()) {
    if (QString::fromStdString(ds->name) == plugin_id) {
      return ds;
    }
  }
  return nullptr;
}

}  // namespace

// Per-session state. Held by unique_ptr in the manager's map so the worker
// thread can hold a stable pointer to it across map mutations.
struct StreamingSourceManager::StreamingSession {
  std::string plugin_id;    ///< display name, as shown in the streaming combo
  std::string manifest_id;  ///< the plugin's stable manifest id, for provenance
  DatasetId dataset_id = 0;
  DataSourceHandle handle;
  /// handle.capabilities(), read once after start(): a plugin call, and the
  /// ABI gives no way for the mask to change once the source runs, so every
  /// later gate (per-topic pause, recordability) reads this instead.
  uint64_t capabilities = 0;
  std::unique_ptr<DataSourceRuntimeHost> runtime_host;
  std::unique_ptr<QThread> worker;

  explicit StreamingSession(DataSourceHandle&& h) : handle(std::move(h)) {}
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
  // Cooperative stop + join for every live session. We can't go through
  // onWorkerFinished here because the UI event loop won't run again.
  for (auto& [dataset_id, sess] : sessions_) {
    if (sess->runtime_host != nullptr) {
      sess->runtime_host->requestStop();  // flag-only: the workers are still running
    }
  }
  for (auto& [dataset_id, sess] : sessions_) {
    if (sess->worker != nullptr) {
      sess->worker->wait();
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
    return false;
  }

  StreamingSession* sess = it->second.get();
  if (sess->runtime_host != nullptr) {
    // Flag-only: a requested stop is orderly, not a failure, so it records no
    // reason — `reason` belongs to the log.
    qCInfo(lcStream) << "stopping stream" << dataset_id << ":" << reason;
    sess->runtime_host->requestStop();
  }
  if (sess->worker != nullptr) {
    sess->worker->wait();
  }

  // streamStopped carries only the FAILURE that ended the stream, if the
  // worker hit one before this stop landed. A requested stop is orderly, so
  // the signal's reason stays empty and a recording of this source stays clean
  // (RecordingService marks any non-empty reason non-clean).
  QString stopped_reason;
  if (sess->runtime_host != nullptr) {
    stopped_reason = QString::fromStdString(sess->runtime_host->stopFailure());
  }

  rememberSkippedLazy(*sess);
  sessions_.erase(it);
  for (const ObjectTopicId topic_id : secondary_object_store_->listTopics(dataset_id)) {
    secondary_object_store_->removeTopic(topic_id);
  }
  secondary_data_engine_->removeDataset(dataset_id);
  // After erase: clearDataset's activeTopicsChanged emission finds no session for
  // this id in onActiveTopicsChanged and is silently ignored, instead of pushing
  // one redundant empty set through a handle about to be destroyed.
  // Capability flag first: clearDataset's empty-set emission triggers the
  // panel's dimming refresh, which must already see the dataset as
  // non-pausable — otherwise rows with data stay grey after disconnect.
  catalog_.setPerTopicPauseCapable(dataset_id, false);
  topic_demand_tracker_.clearDataset(dataset_id);
  catalog_.clearAdvertisedTopics(dataset_id);
  emit streamStopped(dataset_id, stopped_reason);
  return true;
}

void StreamingSourceManager::stopAllAndWait(const QString& reason) {
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
    // D1: the recorder taps the delegated-ingest seam. A source that writes
    // decoded samples straight into the datastore never passes through it, so a
    // tap on it would produce an empty, channel-less file.
    if ((session->capabilities & kCapabilityDelegatedIngest) == 0) {
      result.excluded << QString::fromStdString(session->plugin_id);
      continue;
    }
    result.targets.push_back(
        RecordingTarget{
            // The dataset id IS the identity a recording addresses this source
            // by, so the display name stays a display name.
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
              // The session ended mid-recording: report the counter as it stood
              // when its host died, so the holes it left still reach the user.
              const auto ended = ended_skipped_lazy_.find(dataset_id);
              return ended != ended_skipped_lazy_.end() ? ended->second : 0;
            },
        });
  }
  return result;
}

void StreamingSourceManager::onSourceChanged(const QString& plugin_id) {
  selected_plugin_ = plugin_id;
}

void StreamingSourceManager::onBufferChanged(int seconds) {
  // The worker loop re-applies the budget on each UI-thread hop (see
  // workerLoop below), so simply storing the new value is enough — the
  // change reaches every live session on the next iteration (~50 ms).
  retention_seconds_ = seconds;
  // Re-publish the window to history-retaining consumers (the 3D TF buffer):
  // the worker only re-applies the ObjectStore budget, not the derived caches.
  const auto window_ns = static_cast<qint64>(retention_seconds_) * 1'000'000'000LL;
  for (const auto& [dataset_id, _sess] : sessions_) {
    emit retentionWindowChanged(dataset_id, window_ns);
  }
}

void StreamingSourceManager::onStartRequested() {
  if (selected_plugin_.isEmpty()) {
    qCWarning(lcStream) << "Start requested but no stream source selected.";
    emit setupError(tr("No stream source selected."));
    return;
  }
  startSession(selected_plugin_);
}

void StreamingSourceManager::applyWriteTargetsForCurrentPauseState(DataSourceRuntimeHost& host) {
  if (paused_) {
    // Secondary tail buffer while paused: the primary stays frozen by absence
    // of writes — that's what consumers (Scene2DDockWidget, PlotWidget) read.
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
  // Resume. Swap targets BEFORE flushing so any push already in flight
  // finishes on the secondary and the NEXT push goes to primary; otherwise
  // the flush would race against new arrivals.
  for (auto& [_id, sess] : sessions_) {
    applyWriteTargetsForCurrentPauseState(*sess->runtime_host);
  }
  flushSecondaryIntoPrimary();
  flushSecondaryDataEngineIntoPrimary();
  // Catch up eager filters on the whole paused tail now merged into the primary,
  // BEFORE re-trimming, so a stateful integral consumes the soon-to-be-evicted
  // interval (the pause/resume arm of the streaming correctness order).
  for (const auto& [streaming_dataset_id, _catchup_sess] : sessions_) {
    const auto filter_inputs = session_manager_.createReader().listTopics(streaming_dataset_id);
    (void)session_manager_.dataProcessorService().advanceOnCommit(filter_inputs);
  }
  // DataEngine::flushTo moves chunks without trimming; re-trim the merged primary
  // to the window (objects are already trimmed by ObjectStore::flushTo). Scope the
  // trim to each streaming dataset so a co-loaded file's history is left intact.
  {
    const auto window_ns = static_cast<int64_t>(retention_seconds_) * 1'000'000'000LL;
    for (const auto& [streaming_dataset_id, _sess] : sessions_) {
      session_manager_.dataEngine().enforceRetention(window_ns, streaming_dataset_id);
    }
  }
  // Catch-up nudge so consumers (PlotWidget auto-fit, Scene2DDockWidget jump
  // to latest frame) land on the post-flush live edge. Mirrors PJ3 "flush at
  // play" semantics.
  for (const auto& [dataset_id, _] : sessions_) {
    const auto topic_ids = session_manager_.createReader().listTopics(dataset_id);
    QVector<TopicId> ids;
    ids.reserve(static_cast<qsizetype>(topic_ids.size()));
    for (const TopicId id : topic_ids) {
      ids.push_back(id);
    }
    session_manager_.notifyIngest(std::move(ids), /*live=*/true);
  }
}

void StreamingSourceManager::flushSecondaryDataEngineIntoPrimary() {
  // Zero-copy bulk transfer: DataEngine::flushTo moves every sealed chunk
  // deque from secondary to primary by pointer move; no chunk buffer is
  // reconstructed. Topics are matched by descriptor (dataset_id + name),
  // and the lockstep registration in startSession guarantees the matching
  // descriptors exist on both sides. The secondary's topics stay registered
  // (only contents transfer) so the next pause does not re-mirror.
  if (auto result = secondary_data_engine_->flushTo(session_manager_.dataEngine()); !result) {
    qCWarning(lcStream) << "flushSecondaryDataEngineIntoPrimary failed:" << QString::fromStdString(result.error());
  }
}

void StreamingSourceManager::flushSecondaryIntoPrimary() {
  // Zero-copy bulk transfer: ObjectStore::flushTo moves every ObjectEntry
  // from secondary into primary by value-moving the entry (its std::variant
  // / std::any holds either a shared_ptr or a closure; both transfer with
  // their captured state intact, so no payload bytes are copied and no lazy
  // closure is invoked during the flush). Topics matched by descriptor;
  // lockstep registration in DataSourceRuntimeHost::cbEnsureParserBinding
  // guarantees compatibility. Topics stay registered on the secondary.
  if (auto result = secondary_object_store_->flushTo(session_manager_.objectStore()); !result) {
    qCWarning(lcStream) << "flushSecondaryIntoPrimary failed:" << QString::fromStdString(result.error());
  }
}

void StreamingSourceManager::startSession(const QString& plugin_id) {
  const LoadedDataSource* source_ptr = resolveStreamSource(extensions_, plugin_id);
  if (source_ptr == nullptr) {
    qCWarning(lcStream) << "Stream plugin not found:" << plugin_id;
    emit setupError(tr("Stream plugin '%1' not found.").arg(plugin_id));
    return;
  }
  const LoadedDataSource& source = *source_ptr;
  const QString source_name = QString::fromStdString(source.name);

  DataSourceHandle handle = source.library.createHandle();
  if (!handle.valid()) {
    qCWarning(lcStream) << "createHandle failed for" << source_name;
    emit setupError(tr("Plugin '%1': createHandle failed.").arg(source_name));
    return;
  }

  DataEngine& engine = session_manager_.dataEngine();
  const QString display_name = u"[stream] %1"_s.arg(source_name);

  // One TimeDomain per stream so each is independently time-shiftable on the
  // Source Timeline. DUAL-ENGINE: the secondary (pause/tail buffer) must hold
  // the SAME id, so force it explicitly — the two engines' counters drift
  // whenever the primary gets a domain the secondary doesn't (e.g. a file load
  // between streams). Relying on counter order would desync them.
  auto td_or = engine.createTimeDomain(display_name.toStdString());
  if (!td_or.has_value()) {
    emit setupError(tr("Could not create the time domain."));
    return;
  }
  const TimeDomainId td_id = *td_or;
  auto mirrored_td = secondary_data_engine_->createTimeDomain(display_name.toStdString(), td_id);
  if (!mirrored_td.has_value() || *mirrored_td != td_id) {
    emit setupError(tr("secondary time domain lockstep failed."));
    return;
  }

  auto dataset_or =
      engine.createDataset(DatasetDescriptor{.source_name = display_name.toStdString(), .time_domain_id = td_id});
  if (!dataset_or.has_value()) {
    emit setupError(tr("createDataset failed: %1").arg(QString::fromStdString(dataset_or.error())));
    return;
  }
  const DatasetId dataset_id = static_cast<DatasetId>(*dataset_or);
  const PJ_data_source_handle_t source_handle{static_cast<uint32_t>(dataset_id)};

  // Lockstep mirror onto the secondary engine. Topics are mirrored
  // individually inside DataSourceRuntimeHost; the dataset has to exist here so
  // cbEnsureParserBinding can use the same DatasetId on both engines without
  // translation. Create it with the SAME id explicitly: the two engines'
  // auto-increment counters drift whenever the primary gets a dataset the
  // secondary doesn't (e.g. a file load between streams), so a plain
  // createDataset() on the secondary would return a different id and the next
  // stream would abort here. Forcing the id keeps them in lockstep regardless.
  auto mirrored_dataset = secondary_data_engine_->createDataset(
      DatasetDescriptor{.source_name = display_name.toStdString(), .time_domain_id = td_id}, dataset_id);
  if (!mirrored_dataset.has_value()) {
    emit setupError(tr("secondary createDataset failed: %1").arg(QString::fromStdString(mirrored_dataset.error())));
    return;
  }

  // Capture the plugin's DSO token BEFORE moving `handle` into the session. The
  // streaming runtime host stores lazy ObjectStore anchors/fetchers that are
  // plugin-DSO code and can outlive the catalog entry (mid-session marketplace
  // reload, or app close), so the .so must stay mapped until those drop. Reading
  // handle.libraryOwner() AFTER the std::move would yield a moved-from (null)
  // token, silently disabling the protection — the exact gap this site had.
  auto library_keepalive = handle.libraryOwner();
  auto session = std::make_unique<StreamingSession>(std::move(handle));
  session->plugin_id = source.name;
  session->manifest_id = source.id;
  session->dataset_id = dataset_id;
  session->runtime_host = std::make_unique<DataSourceRuntimeHost>(
      engine, extensions_, dataset_id, source_handle, session_manager_.objectStore(), source.name,
      [this](ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
        session_manager_.registerObjectTopicParser(id, std::move(parser));
      },
      secondary_object_store_.get(), secondary_data_engine_.get(), std::move(library_keepalive),
      session_manager_.ingestTapsShared());
  // Streaming has no persistent file to re-read: a lazy policy's re-fetch
  // closure would re-invoke each message's fetcher on every pull, so keep
  // object bytes resident (kEager) instead of the resolver's lazy default.
  session->runtime_host->policyResolver().setDefault(sdk::ObjectIngestPolicy::kEager);
#ifdef PJ_TARGET_WASM
  // Browser RobotDescription handlers are object-only. Defer their payloads
  // until RobotModel requests one instead of invoking the absent scalar path.
  // The native streaming policy remains untouched.
  session->runtime_host->policyResolver().setForType(
      sdk::BuiltinObjectType::kRobotDescription, sdk::ObjectIngestPolicy::kPureLazy);
#endif
  // [worker/poll thread, per DataSourceRuntimeHost::on_available_topics] — hop to
  // the GUI thread before touching CatalogModel/TopicDemandTracker, mirroring the
  // samplesIngested marshal in workerLoop below. The lambda may run before start()
  // even returns (some plugins advertise everything up front) or repeatedly during
  // poll(), so applyAdvertisedTopics re-checks the session still exists.
  session->runtime_host->on_available_topics = [this, dataset_id](
                                                   std::vector<DataSourceRuntimeHost::AdvertisedTopicInfo> topics) {
    std::vector<AdvertisedTopic> owned;
    owned.reserve(topics.size());
    for (auto& topic : topics) {
      owned.push_back(AdvertisedTopic{QString::fromStdString(topic.topic_name), topic.classification});
    }
    QMetaObject::invokeMethod(
        this,
        [this, dataset_id, owned = std::move(owned)]() mutable { applyAdvertisedTopics(dataset_id, std::move(owned)); },
        Qt::QueuedConnection);
  };

  ServiceRegistryBuilder registry;
  if (auto status = session->runtime_host->registerServices(registry); !status) {
    emit streamError(
        dataset_id,
        tr("Plugin '%1': service registration failed: %2").arg(source_name, QString::fromStdString(status.error())));
    return;
  }

  if (auto status = session->handle.bind(registry.view()); !status) {
    emit streamError(
        dataset_id, tr("Plugin '%1': bind failed: %2").arg(source_name, QString::fromStdString(status.error())));
    return;
  }

  QSettings persisted_settings;
  const QString config_key = pluginConfigKey(source.name);
  std::string config = persisted_settings.value(config_key, QString()).toString().toStdString();
  // Always invoke loadConfig — even on a first run with empty config. Stream
  // plugins use this hook to wire the runtime host into their dialog (e.g.
  // populating the available-encodings combo via list_available_encodings).
  // Skipping it leaves the dialog with "(no parsers available)" and the OK
  // button disabled.
  if (auto status = session->handle.loadConfig(config); !status) {
    qCInfo(lcStream) << "loadConfig failed; proceeding with plugin defaults:" << QString::fromStdString(status.error());
  }

  // Extract parser config saved from a previous session so the embedded
  // parser sub-dialog (e.g. JSON "Use embedded timestamp") is pre-populated.
  std::string initial_parser_config;
  {
    auto saved_cfg = nlohmann::json::parse(config, nullptr, false);
    if (!saved_cfg.is_discarded() && saved_cfg.contains("_parser_config")) {
      initial_parser_config = saved_cfg["_parser_config"].get<std::string>();
    }
  }

  const auto dlg = dialog_presenter::showDataSourceDialog({
      .source = source,
      .handle = session->handle,
      .catalog = extensions_,
      .parent = dialog_parent_,
      .initial_parser_config = initial_parser_config,
      .section_band_metrics =
          chrome_metrics_provider_ ? std::optional<ChromeMetrics>(chrome_metrics_provider_()) : std::nullopt,
  });
  if (dlg.outcome == dialog_presenter::Outcome::kPluginContractViolation) {
    emit streamError(
        dataset_id, tr("Plugin contract violation: %1. Reinstall the plugin from the Marketplace.")
                        .arg(QString::fromStdString(dlg.error)));
    return;
  }
  if (dlg.outcome == dialog_presenter::Outcome::kRejected) {
    qCInfo(lcStream) << "User cancelled stream dialog for" << source_name;
    return;
  }
  if (dlg.payload.has_value()) {
    config = dlg.payload->saved_config;
    // Embed the parser sub-dialog config (e.g. JSON embedded-timestamp
    // settings) into the main plugin config under "_parser_config" so the
    // stream plugin forwards it to ensureParserBinding when each topic's
    // parser binding is created in the worker loop.
    if (!dlg.payload->parser_config.empty()) {
      auto cfg = nlohmann::json::parse(config, nullptr, false);
      if (!cfg.is_discarded()) {
        cfg["_parser_config"] = dlg.payload->parser_config;
        config = cfg.dump();
      }
    }
    if (auto status = session->handle.loadConfig(config); !status) {
      emit streamError(
          dataset_id, tr("Plugin '%1': loadConfig (post-dialog) failed: %2")
                          .arg(source_name, QString::fromStdString(status.error())));
      return;
    }
  }
  persisted_settings.setValue(config_key, QString::fromStdString(config));

  // Apply the current pause state to this session's write hosts BEFORE start(),
  // so a source added mid-pause writes into the secondary tail buffer from its
  // very first sample. This is the latest safe seam: the modal source dialog
  // above runs a nested event loop where the user could toggle pause, so we read
  // paused_ AFTER it returns; and start() is the plugin's "go" signal, so setting
  // the target before it closes the window for a push-model plugin that delivers
  // from its own thread inside start(). From here on, startSession runs
  // synchronously (no event loop), so paused_ cannot change before the worker
  // starts; onPauseToggled keeps every live session in step thereafter.
  applyWriteTargetsForCurrentPauseState(*session->runtime_host);

  if (auto status = session->handle.start(); !status) {
    emit streamError(
        dataset_id, tr("Plugin '%1': start failed: %2").arg(source_name, QString::fromStdString(status.error())));
    return;
  }

  session->capabilities = session->handle.capabilities();
  // Record the capability for the UI (CurveListPanel dims a topic as
  // "unsubscribed" only for a dataset that actually supports the pause — a
  // non-demand source sends everything regardless of display state).
  catalog_.setPerTopicPauseCapable(dataset_id, (session->capabilities & kCapabilityPerTopicPause) != 0);

  // Push the current (usually empty — nothing is displayed yet) active-topic set
  // once, right after start(), so a demand-capable source is explicitly told to
  // subscribe to nothing rather than relying on its own default (e.g. a stale
  // discovery-filter selection from a previous session). Later changes arrive via
  // TopicDemandTracker::activeTopicsChanged -> onActiveTopicsChanged.
  pushActiveTopics(*session, topic_demand_tracker_.activeTopics(dataset_id));

  // QThread::create runs the lambda as the thread's run(); no Qt event loop
  // needed in the worker — we only post back via QueuedConnection.
  session->worker.reset(QThread::create([this, dataset_id]() { workerLoop(dataset_id); }));

  QThread* worker_ptr = session->worker.get();
  sessions_.emplace(dataset_id, std::move(session));
  worker_ptr->start();

  emit streamStarted(dataset_id);
  // Publish the new session's retention window so history-retaining consumers
  // (the 3D TF buffer) can bound themselves in step with the ObjectStore.
  emit retentionWindowChanged(dataset_id, static_cast<qint64>(retention_seconds_) * 1'000'000'000LL);
}

void StreamingSourceManager::workerLoop(DatasetId dataset_id) {
  auto it = sessions_.find(dataset_id);
  if (it == sessions_.end()) {
    return;
  }
  StreamingSession* sess = it->second.get();

  while (!sess->runtime_host->stopRequested()) {
    if (auto status = sess->handle.poll(); !status) {
      const QString err = QString::fromStdString(status.error());
      QMetaObject::invokeMethod(
          this, [this, dataset_id, err]() { emit streamError(dataset_id, err); }, Qt::QueuedConnection);
      sess->runtime_host->requestStop(std::string("plugin poll error: ") + err.toStdString());
      break;
    }
    sess->runtime_host->flushPending();
    // Broadcast live-advance on the UI thread so plot adapters invalidate
    // their sample caches AND follow-live consumers auto-fit. The write
    // host wrote directly through DataEngine and bypassed SessionManager's
    // own commitChunks → neither Qt signal would fire otherwise.
    //
    // The `live` flag is gated on `paused_`: when the user has paused the
    // stream, samples keep being committed but the plot stops following the
    // live edge (PlotWidget skips resetZoom and just replots in place).
    //
    // Retention enforcement piggybacks on the same UI-thread hop so
    // `retention_seconds_` stays UI-thread-only (no atomic needed). Trim
    // runs before notifyIngest so consumers observe the post-trim engine.
    QMetaObject::invokeMethod(
        this,
        [this, dataset_id]() {
          const auto window_ns = static_cast<int64_t>(retention_seconds_) * 1'000'000'000LL;
          // Apply the budget on every tick: cheap (one map iteration per
          // session) and idempotent. Solves the cold-start problem where the
          // plugin creates new object topics mid-stream (post-startSession)
          // and would otherwise miss the budget set by setObjectRetentionBudget.
          if (auto session_it = sessions_.find(dataset_id); session_it != sessions_.end()) {
            session_it->second->runtime_host->setObjectRetentionBudget(window_ns, kStreamingObjectMemoryBudget);
          }
          // Run eager filters over the freshly committed input BEFORE retention
          // can evict it (the streaming correctness order, plan §5/D6): a stateful
          // node must consume every sample before it ages out of the window. On
          // the UI thread here, serialized with the Filter Editor's own
          // apply/update and with the retention trim just below. While paused the
          // writes land in the secondary engine (no DerivedEngine); the resume
          // catch-up advances the whole buffered tail before re-trimming.
          if (!paused_) {
            const auto filter_inputs = session_manager_.createReader().listTopics(dataset_id);
            (void)session_manager_.dataProcessorService().advanceOnCommit(filter_inputs);
          }

          // Trim the engine currently being written: B while paused (bounds the
          // tail), the primary while live. The frozen engine is never touched,
          // preserving the primary snapshot for scrub-back. Scope the trim to THIS
          // streaming dataset so a file the user loaded into the same engine keeps
          // its full history.
          if (paused_) {
            secondary_data_engine_->enforceRetention(window_ns, dataset_id);
          } else {
            session_manager_.dataEngine().enforceRetention(window_ns, dataset_id);
          }

          const auto topic_ids = session_manager_.createReader().listTopics(dataset_id);
          QVector<TopicId> ids;
          ids.reserve(static_cast<qsizetype>(topic_ids.size()));
          for (const TopicId id : topic_ids) {
            ids.push_back(id);
          }
          // Re-run whole-series marker generators over the freshly committed samples.
          // The streaming write host commits straight to the engine, bypassing
          // SessionManager::commitChunks, so this is the live-data marker recompute
          // hook (cheap no-op when no generators exist). Paused writes land in the
          // secondary engine with no DerivedEngine, matching the filter-advance gate.
          if (!paused_) {
            session_manager_.recomputeMarkersForChanged(topic_ids, dataset_id);
          }
          session_manager_.notifyIngest(std::move(ids), /*live=*/!paused_);
        },
        Qt::QueuedConnection);
    QThread::msleep(kPollPeriodMs);
  }

  sess->handle.stop();
  sess->runtime_host->flushAll();

  QMetaObject::invokeMethod(this, [this, dataset_id]() { onWorkerFinished(dataset_id); }, Qt::QueuedConnection);
}

void StreamingSourceManager::onWorkerFinished(DatasetId dataset_id) {
  auto it = sessions_.find(dataset_id);
  if (it == sessions_.end()) {
    return;
  }
  StreamingSession* sess = it->second.get();
  if (sess->worker != nullptr) {
    sess->worker->wait();
  }
  // Empty for an orderly end — the plugin's own end-of-stream included — and
  // the failure that ended the stream otherwise; a callback failure the plugin
  // recovered from stays in the log and out of the signal. Read past the join,
  // after every writer of it has stopped.
  const QString reason = QString::fromStdString(sess->runtime_host->stopFailure());

  rememberSkippedLazy(*sess);
  sessions_.erase(it);
  catalog_.setPerTopicPauseCapable(dataset_id, false);  // before clearDataset — see requestStop
  topic_demand_tracker_.clearDataset(dataset_id);
  catalog_.clearAdvertisedTopics(dataset_id);
  emit streamStopped(dataset_id, reason);
}

void StreamingSourceManager::pushActiveTopics(StreamingSession& session, const std::vector<QString>& active_topics) {
  if ((session.capabilities & kCapabilityPerTopicPause) == 0) {
    return;  // source doesn't support per-topic pause — behave exactly as before
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
    return;  // session already torn down before this marshal landed
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
