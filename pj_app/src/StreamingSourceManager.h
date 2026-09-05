#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_runtime/RecordingService.h"
#include "pj_widgets/ChromeMetrics.h"

QT_BEGIN_NAMESPACE
class QThread;
class QWidget;
QT_END_NAMESPACE

namespace PJ {

struct AdvertisedTopic;
class CatalogModel;
class DataEngine;
class DataSourceRuntimeHost;
class ExtensionCatalogService;
class ObjectStore;
class SessionManager;
class TopicDemandTracker;

/// What the live sessions offer a recording: the ones the recorder can tap,
/// plus the display names of those it cannot (D1), so the caller can say what
/// will be missing instead of silently recording less than the user sees.
struct RecordingTargets {
  std::vector<RecordingTarget> targets;
  QStringList excluded;
};

// Drives the streaming-source path: pick a stream plugin from the catalog,
// build a per-session ingest host (mirrors FileLoader's flow), run the
// plugin's start() and then a worker thread that calls poll() + flush()
// at a fixed cadence until stopped. Multistream-capable by design: each Start
// creates an independent dataset/handle/worker triple keyed by DatasetId,
// even though the LeftPanel UX currently drives one active stream at a time.
//
// Lives in pj_app because it talks to dialog_presenter and owns Qt threads.
class StreamingSourceManager : public QObject {
  Q_OBJECT

 public:
  StreamingSourceManager(
      SessionManager& session, ExtensionCatalogService& extensions, CatalogModel& catalog,
      TopicDemandTracker& topic_demand_tracker, QWidget* dialog_parent, QObject* parent = nullptr);
  ~StreamingSourceManager() override;

  StreamingSourceManager(const StreamingSourceManager&) = delete;
  StreamingSourceManager& operator=(const StreamingSourceManager&) = delete;

  // Supplies the app's live chrome metrics so a plugin's stream dialog sizes its
  // SectionHeaderBands to the canonical band height (matching the toolboxes).
  // Injected by the shell to keep this manager free of a MainWindow link — same
  // pattern as FileLoader::setFilePicker. Read at dialog-show time, so it always
  // reflects the current icon-size preference. Unset -> bands keep default size.
  using ChromeMetricsProvider = std::function<ChromeMetrics()>;
  void setChromeMetricsProvider(ChromeMetricsProvider provider) {
    chrome_metrics_provider_ = std::move(provider);
  }

  // True iff any session is currently live.
  bool hasActiveSession() const;

  // Requests cooperative stop for one session and joins its worker before
  // returning. Used before real-deleting a dataset so no writer can race the
  // engine/object-store erase. Returns false when the dataset is not streaming.
  bool stopDatasetAndWait(DatasetId dataset_id, const QString& reason);

  // Stop-and-join every live session, preserving streamStopped emissions per
  // dataset. Used by remove-all paths before erasing the whole datastore.
  void stopAllAndWait(const QString& reason);

  // The live streaming sessions as recordable targets for RecordingService.
  // GUI thread only. Each target is keyed by its DatasetId — the identity the
  // recording addresses it by — and its lambdas look the session up by that id
  // at call time, so a session that ended mid-recording reports a failed attach
  // rather than a dangling host. source_id is the plugin display name and
  // nothing more: names may repeat, and the service gives each file its own
  // name. Sources without delegated ingest go into `excluded` instead of
  // `targets`. Not const: it drops the previous recording's ended-session
  // counters.
  [[nodiscard]] RecordingTargets recordingTargets();

  // Point `host`'s write targets at the primary or the secondary tail buffer
  // according to the current pause state. Single source of truth for the
  // decision, shared by onPauseToggled (both branches) and startSession.
  void applyWriteTargetsForCurrentPauseState(DataSourceRuntimeHost& host);

  // Test seams for the secondary tail buffers (the pause-target endpoints).
  DataEngine& secondaryEngineForTests();
  ObjectStore& secondaryStoreForTests();

 public slots:
  // Wired from LeftPanel::streamingStartRequested. Starts a session for the
  // plugin currently held in selected_plugin_. There is no UI-driven stop —
  // sessions live until app shutdown or an unrecoverable plugin error.
  void onStartRequested();

  // Wired from LeftPanel::streamingPauseToggled. Toggles follow-live for the
  // active session(s): when paused, the worker keeps polling and committing
  // samples to the datastore but the `live` flag carried by samplesIngested
  // is forced to false so PlotWidget stops auto-fitting the viewport to the
  // live edge. Mirrors PJ3 pause semantics.
  void onPauseToggled(bool paused);

  // Wired from LeftPanel::streamingSourceChanged. Remembers the currently
  // selected plugin name; consumed by the next onStartRequested().
  void onSourceChanged(const QString& plugin_id);

  // Wired from LeftPanel::streamingBufferChanged. Records the buffer size
  // (in seconds). v1 stores the value but does not yet apply it to topic
  // retention budgets — follow-up wiring.
  void onBufferChanged(int seconds);

 signals:
  // Emitted on the UI thread once a session is wired and the plugin's
  // start() has returned successfully.
  void streamStarted(DatasetId dataset_id);

  // Emitted on the UI thread when a streaming dataset's object-retention window
  // is established or changes (session start, and on every buffer-size change for
  // each active session). `window_ns` is the time window the ObjectStore trims
  // each topic to. Consumers that retain history derived from those objects (the
  // 3D TF buffer) use it to bound their own memory in step with the store.
  void retentionWindowChanged(DatasetId dataset_id, qint64 window_ns);

  // Emitted on the UI thread after a worker has exited and the session has
  // been torn down. `reason` is the FAILURE that ended the stream ("plugin
  // poll error: ...", or the plugin's own request_stop(FAILED) reason), empty
  // for an orderly end — user-requested, or the plugin's request_stop(STOPPED)
  // at end of stream. Consumers treat a non-empty reason as a problem (a
  // recording marks the source non-clean), so it never carries a diagnostic
  // the plugin recovered from.
  void streamStopped(DatasetId dataset_id, QString reason);

  // Emitted on the UI thread for setup-time failures that happen before a
  // session is wired (plugin not found, createHandle / createDataset / time
  // domain creation failed, etc.). No DatasetId is available yet.
  void setupError(QString message);

  // Emitted on the UI thread for errors on an already-created session
  // (dialog rejection, bind failure, plugin start/poll error). The session
  // typically stops shortly after via streamStopped().
  void streamError(DatasetId dataset_id, QString message);

 private slots:
  // Joins the QThread, tears the session down, emits streamStopped().
  // Posted by the worker thread via QueuedConnection on loop exit.
  void onWorkerFinished(DatasetId dataset_id);

  // Wired to TopicDemandTracker::activeTopicsChanged (GUI thread, direct
  // connection — the tracker lives on the same thread). Capability-gated: a
  // no-op unless the session's source advertises kCapabilityPerTopicPause, so
  // a non-supporting source behaves exactly as before this feature existed.
  void onActiveTopicsChanged(DatasetId dataset_id, const std::vector<QString>& active_topics);

 private:
  struct StreamingSession;

  // Keeps a dying session's pure-lazy counter reachable, so a recording that
  // outlives it can still count the holes it left. Call immediately before
  // erasing the session.
  void rememberSkippedLazy(const StreamingSession& session);

  // Bootstraps one new session: resolves the plugin by display name, creates
  // handle/dataset/ingest, runs the dialog flow, calls start(), spawns the
  // worker. Emits streamStarted() on success, streamError() and tears down
  // on any failure path.
  void startSession(const QString& plugin_id);

  // Pushes `active_topics` through the "pj.topic_subscription.v1" extension
  // (DataSourceHandle::setActiveTopics), gated on kCapabilityPerTopicPause —
  // a no-op for a non-supporting source. Shared by the initial post-start()
  // push (usually empty, so a demand-capable source truly starts paused) and
  // every activeTopicsChanged re-push.
  void pushActiveTopics(StreamingSession& session, const std::vector<QString>& active_topics);

  // GUI-thread continuation of one notify_available_topics callback (see
  // DataSourceRuntimeHost::on_available_topics): merges the classified set into
  // CatalogModel::setAdvertisedTopics and hands the FrameTransforms/CameraInfo-
  // classified subset to TopicDemandTracker::setInfrastructureTopics. A no-op if
  // the session already tore down before the QueuedConnection marshal landed.
  void applyAdvertisedTopics(DatasetId dataset_id, std::vector<AdvertisedTopic> topics);

  // Worker thread body. Looks the session up by dataset_id (the map is only
  // mutated on the UI thread, so reads from the worker are safe as long as
  // its own entry exists).
  void workerLoop(DatasetId dataset_id);

  // Drain every entry of every object topic in the secondary store back into
  // the primary (SessionManager::objectStore), then evict the secondary so
  // the next pause starts from an empty tail buffer. Called from
  // onPauseToggled(false) AFTER the target swap back to primary so no new
  // pushes are landing on the secondary during the drain.
  void flushSecondaryIntoPrimary();

  // Scalar twin of flushSecondaryIntoPrimary: drain every sealed chunk of
  // every topic in the secondary DataEngine back into the primary, then
  // clear the secondary's topic chunks. Called from onPauseToggled(false)
  // AFTER the target swap back to primary engine so no new chunks are
  // sealing on the secondary during the drain.
  void flushSecondaryDataEngineIntoPrimary();

  SessionManager& session_manager_;
  ExtensionCatalogService& extensions_;
  CatalogModel& catalog_;
  TopicDemandTracker& topic_demand_tracker_;
  QWidget* dialog_parent_;
  ChromeMetricsProvider chrome_metrics_provider_;

  QString selected_plugin_;
  int retention_seconds_ = 5;
  // Follow-live gate. Only mutated and read on the UI thread (the worker
  // reads it inside a QueuedConnection lambda that runs on the UI thread).
  // No atomic needed.
  bool paused_ = false;

  // Streaming two-store secondary. Empty during live ingest; receives every
  // object push during pause via DataSourceRuntimeHost::setObjectStoreTarget,
  // and is drained into the primary at resume. Owned by the manager so a
  // single instance serves every session (matches the pause-global model:
  // §3.1.7 of the streaming-manager report). Cheap when empty.
  std::unique_ptr<ObjectStore> secondary_object_store_;

  // Scalar twin of secondary_object_store_. Empty during live ingest;
  // receives every scalar push (source-level and parser bindings) during
  // pause via DataSourceRuntimeHost::setDataEngineTarget, and is drained
  // into the primary DataEngine at resume. Matching TimeDomainIds,
  // DatasetIds, and TopicIds with the primary are requested explicitly by
  // startSession + cbEnsureParserBinding so chunks can be drained by id
  // without any translation.
  std::unique_ptr<DataEngine> secondary_data_engine_;

  // DatasetId → owning session. unique_ptr so the worker thread can keep a
  // stable pointer to the session struct across map mutations on the UI
  // thread.
  std::unordered_map<DatasetId, std::unique_ptr<StreamingSession>> sessions_;
  // Final pure-lazy counter of each session that has ended. Cleared when the
  // next recording's targets are built, the one point at which the previous
  // values stop mattering.
  std::unordered_map<DatasetId, uint64_t> ended_skipped_lazy_;
};

}  // namespace PJ
