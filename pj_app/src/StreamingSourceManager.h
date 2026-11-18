#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DialogWatch.h"
#include "LayoutXml.h"
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
struct RuntimeDataSourcePlugin;
class SessionManager;
class TopicDemandTracker;

/// What the live sessions offer a recording: the ones the recorder can tap, plus the display names of those it
/// cannot, so the caller can say what will be missing instead of silently recording less than the user sees.
struct RecordingTargets {
  std::vector<RecordingTarget> targets;
  QStringList excluded;
};

// Runs stream sources. Each start creates an independent dataset, plugin handle, runtime host and worker thread,
// keyed by DatasetId; the worker calls poll() + flush() at a fixed cadence until stopped. A session starts
// interactively (the plugin's dialog) or from a layout record (startFromLayout, no dialog); both go through
// createSession/launchSession.
//
// Threading: configuration, start() and stop requests run on the GUI thread; once launched, a session's worker is
// the only caller of its plugin (poll(), then stop()). What a save needs is captured before the worker starts, so
// layoutSources() never calls into a plugin while it polls.
class StreamingSourceManager : public QObject {
  Q_OBJECT

 public:
  StreamingSourceManager(
      SessionManager& session, ExtensionCatalogService& extensions, CatalogModel& catalog,
      TopicDemandTracker& topic_demand_tracker, QWidget* dialog_parent, QObject* parent = nullptr);
  ~StreamingSourceManager() override;

  StreamingSourceManager(const StreamingSourceManager&) = delete;
  StreamingSourceManager& operator=(const StreamingSourceManager&) = delete;

  // Read each time a stream dialog opens, so its SectionHeaderBands match the toolboxes' band height at the
  // current icon size. Unset: the bands keep their default size.
  using ChromeMetricsProvider = std::function<ChromeMetrics()>;
  void setChromeMetricsProvider(ChromeMetricsProvider provider) {
    chrome_metrics_provider_ = std::move(provider);
  }

  bool hasActiveSession() const;

  // Starts `source` with `expanded_config` (its saved template with deployment variables filled in) and no dialog:
  // the configuration is never read from or written to the remembered plugin settings, and a plugin that refuses it
  // fails the start instead of falling back to anything else. The plugin resolves by manifest id first, display name
  // second. Returns the new session's dataset, or 0 with `error` set and nothing left behind. Emits streamStarted on
  // success and never setupError/streamError for a failed start; failures after start() take the usual
  // streamError/streamStopped path. GUI thread only; synchronous (the plugin's start() must not block).
  [[nodiscard]] DatasetId startFromLayout(
      const layout_xml::StreamingSourceRef& source, const QString& expanded_config, QString& error);

  // One record per live session, in DatasetId order (not launch order: an interactive start creates its dataset when
  // its dialog opens), as a source-bound save writes it: the configuration is the
  // template it started from (layout sessions) or the one the dialog produced (interactive ones), with each
  // session's current display offset. Ids are unique: a session keeps its layout id unless an earlier session holds
  // it (then "<id>-2", ...); interactive ones are named "stream". The timeline order is left to the caller. Reads
  // only state captured before each worker began polling, so it is safe while the workers run. GUI thread only.
  [[nodiscard]] QList<layout_xml::StreamingSourceRef> layoutSources() const;

  // The id layoutSources() gives the session streaming into `dataset_id`; empty when no session does. GUI thread
  // only.
  [[nodiscard]] QString layoutSourceId(DatasetId dataset_id) const;

  // Each live session's dataset and the record id layoutSources() gives it, in DatasetId order. GUI thread only.
  [[nodiscard]] std::vector<std::pair<DatasetId, QString>> layoutIds() const;

  // The live session `source` would start again: same resolved plugin and same configuration template, byte for
  // byte, so a layout opened while its stream runs binds to that session instead of opening a second connection.
  // Skips the sessions in `taken` (bound to another record of the same layout) and, of the rest, prefers the one
  // whose record id is `source.id`, so two identical streams keep their own views. 0 when none matches. GUI thread
  // only.
  [[nodiscard]] DatasetId liveSessionFor(
      const layout_xml::StreamingSourceRef& source, const QSet<DatasetId>& taken = {}) const;

  // Requests a cooperative stop and joins the worker before returning, so no writer races the caller's erase of the
  // dataset. Returns false when the dataset is not streaming; a start still at its dialog for that dataset is
  // withdrawn (and discards it).
  bool stopDatasetAndWait(DatasetId dataset_id, const QString& reason);

  // Drops what `dataset_id` holds in the secondary (paused) buffers. A stopped or discarded stream's teardown does
  // it; whoever erases a dataset must too: a stream that ended on its own while paused leaves its rows behind, and
  // rows of a dataset the primary no longer has make every later resume (DataEngine::flushTo) merge nothing, for
  // every stream.
  void removeSecondaryDataset(DatasetId dataset_id);

  // Asks the session streaming into `dataset_id` to stop, without waiting: streamStopped follows from the event loop
  // once its worker has wound down. For a caller that must not block (nor erase the dataset next). Returns false
  // when the dataset is not streaming.
  bool requestStopDataset(DatasetId dataset_id, const QString& reason);

  // Withdraws every pending start, then stops and joins every session, one streamStopped per dataset. Before
  // erasing the whole datastore.
  void stopAllAndWait(const QString& reason);

  // The live sessions as RecordingService targets. GUI thread only. Each target is keyed by its DatasetId, the
  // identity the recording addresses it by, and its lambdas look the session up by that id at call time, so a
  // session that ended mid-recording reports a failed attach rather than a dangling host. source_id is the plugin
  // display name and nothing more: names may repeat, and the service gives each file its own name.
  // Sources without delegated ingest go into `excluded` instead of `targets`. Not const: it drops the previous
  // recording's ended-session counters.
  [[nodiscard]] RecordingTargets recordingTargets();

  // Points `host`'s write targets at the primary or the secondary tail buffer according to the pause state: the one
  // place that decides, for onPauseToggled and launchSession.
  void applyWriteTargetsForCurrentPauseState(DataSourceRuntimeHost& host);

  DataEngine& secondaryEngineForTests();
  ObjectStore& secondaryStoreForTests();

  // Runs one ingest tick for `dataset_id`. GUI thread only.
  void ingestTickForTests(DatasetId dataset_id);

  // How an interactive start ended: `started` is the stream's dataset, or 0 with `error` the
  // failure, empty when the user cancelled the dialog or `still_allowed` refused.
  using StartDone = std::function<void(DatasetId started, const QString& error)>;

  // The interactive start of stream source `plugin_id` (display name), whatever the Stream combo shows: builds the
  // session, configures it from the remembered settings and the plugin's dialog (its title prefixed by
  // `dialog_title_prefix`, e.g. who asked), then launches it. The dialog is modal, but this does NOT wait for it (a
  // browser cannot block on a dialog): the start settles when the user answers, or before this returns for a source
  // with no dialog. `still_allowed`, when given, is asked once the dialog is accepted: false abandons the start as a
  // cancel does, applying none of the user's choices. Emits streamStarted() on success, setupError()/streamError()
  // on a failure, nothing on a cancel. A start that does not launch, a cancelled dialog included, leaves no dataset
  // behind. `on_dialog` is told when the dialog opens and when it is gone; `done`, how the start ended.
  void startSession(
      const QString& plugin_id, const QString& dialog_title_prefix = {},
      const std::function<bool()>& still_allowed = {}, const dialog_presenter::DialogWatch& on_dialog = {},
      StartDone done = {});

  [[nodiscard]] bool hasPendingStart() const;

  // Closes the dialog of every start still at one, as a rejection: each then cleans up after itself (its session
  // destroyed, its dataset discarded). Call before anything that erases datasets, destroys sessions, or tears down
  // what listens to the catalog; stopAllAndWait and the destructor do it themselves.
  void cancelPendingStarts();

 public slots:
  // Starts the source picked in the Stream combo. There is no Stop button: a session ends when its dataset is
  // removed, an agent stops it, the app shuts down, or the plugin ends it.
  void onStartRequested();

  // Pause redirects every session's writes into the secondary tail buffers, so the primary (what the views read)
  // stays frozen while the workers keep polling; ingest ticks carry live=false. Resume merges the tail into the
  // primary, catches the eager processors up, re-trims, then announces a live ingest.
  void onPauseToggled(bool paused);

  void onSourceChanged(const QString& plugin_id);

  // The retention window in seconds; every session's next ingest tick applies it, and retentionWindowChanged
  // fires for each live session at once.
  void onBufferChanged(int seconds);

 signals:
  // GUI thread, once the session is wired and the plugin's start() has returned successfully.
  void streamStarted(DatasetId dataset_id);

  // GUI thread, at session start and on every buffer-size change, for each live session. `window_ns` is the time
  // window the stores trim the dataset to; consumers that keep history derived from its objects (the 3D TF buffer)
  // bound their own memory with it.
  void retentionWindowChanged(DatasetId dataset_id, qint64 window_ns);

  // GUI thread, after the worker has exited and the session is torn down. `reason` is the FAILURE that ended the
  // stream ("plugin poll error: ...", or the plugin's own request_stop(FAILED) reason), empty for an orderly end
  // (requested, or the plugin's request_stop(STOPPED) at end of stream). Consumers treat a non-empty reason as a
  // problem (a recording marks the source non-clean), so it never carries a diagnostic the plugin recovered from.
  void streamStopped(DatasetId dataset_id, QString reason);

  // GUI thread, for a failure before the session's dataset exists (plugin not found, createHandle, time domain or
  // dataset creation failed).
  void setupError(QString message);

  // GUI thread, for a failure once the session's dataset exists: an interactive start that fails after its dialog
  // opened (plugin contract violation, refused configuration, start() failure), or a launched session that fails
  // while running (poll error). In the first case the dataset is already discarded and no streamStopped follows; in
  // the second, streamStopped follows once the worker has wound down.
  void streamError(DatasetId dataset_id, QString message);

 private slots:
  // Queued by the worker as its last act: joins it, tears the session down, emits streamStopped().
  void onWorkerFinished(DatasetId dataset_id);

  // Direct connection from TopicDemandTracker::activeTopicsChanged (same thread). A no-op unless the source
  // advertises kCapabilityPerTopicPause.
  void onActiveTopicsChanged(DatasetId dataset_id, const std::vector<QString>& active_topics);

 private:
  struct StreamingSession;

  // Keeps a dying session's pure-lazy counter reachable, so a recording that outlives it can still count the holes
  // it left. Call immediately before erasing the session.
  void rememberSkippedLazy(const StreamingSession& session);

  // An interactive start still at its dialog.
  struct PendingStart;

  // startSession after createSession: configures the pending session from the remembered settings and the plugin's
  // dialog, asks `still_allowed` once the dialog is accepted, then launches it. Whatever does not launch destroys the
  // session and discards its dataset, reports a failure through streamError(), and `done` hears the outcome either
  // way.
  void configureAndLaunch(
      const RuntimeDataSourcePlugin& source, const std::shared_ptr<PendingStart>& pending,
      const QString& dialog_title_prefix, const std::function<bool()>& still_allowed,
      const dialog_presenter::DialogWatch& on_dialog, StartDone done);

  // The construction both starts share, up to and including bind(): handle, time domain and dataset
  // ("[stream] <label>", mirrored with the same ids on the secondary engine), runtime host and services.
  // Configuration stays with the caller. Returns null with `error` set, the dataset it created already discarded.
  [[nodiscard]] std::unique_ptr<StreamingSession> createSession(
      const RuntimeDataSourcePlugin& source, const QString& label, QString& error);

  // Points the write hosts at the current pause target, calls start(), records the capabilities, pushes the initial
  // demand, spawns the worker and emits streamStarted(). On a start() failure returns false with `error` set; the
  // session is destroyed, its dataset left for the caller.
  [[nodiscard]] bool launchSession(std::unique_ptr<StreamingSession> session, QString& error);

  // Removes a dataset whose session never launched (or is gone) from both engines, the object stores and the
  // catalog.
  void discardDataset(DatasetId dataset_id);

  // DataSourceHandle::setActiveTopics ("pj.topic_subscription.v1"), a no-op without kCapabilityPerTopicPause. The
  // push right after start() is usually empty, so a demand-capable source starts subscribed to nothing.
  void pushActiveTopics(StreamingSession& session, const std::vector<QString>& active_topics);

  // GUI-thread continuation of DataSourceRuntimeHost::on_available_topics: sets the catalog's advertised topics and
  // hands the FrameTransforms/CameraInfo subset to TopicDemandTracker::setInfrastructureTopics. A no-op when the
  // session is gone by the time the queued call lands.
  void applyAdvertisedTopics(DatasetId dataset_id, std::vector<AdvertisedTopic> topics);

  // The GUI-thread half of one poll cycle, queued by workerLoop after each poll + flush: eager processors consume
  // the new rows, retention trims, then the ingest is announced. A stateful node must see a sample before it can age
  // out of the window, so the processors always run before the trim.
  void onIngestTick(DatasetId dataset_id);

  // Worker thread body. It gets its session at launch and never reads sessions_, which the GUI thread mutates while
  // workers run; the session is erased only after its worker is joined.
  void workerLoop(StreamingSession& session);

  // Moves every object entry of the secondary store into the primary. Call only after the write targets are back on
  // the primary, so no push lands on the secondary during the move.
  void flushSecondaryIntoPrimary();

  // Moves every sealed chunk of the secondary DataEngine into the primary; same call-order rule as
  // flushSecondaryIntoPrimary.
  void flushSecondaryDataEngineIntoPrimary();

  SessionManager& session_manager_;
  ExtensionCatalogService& extensions_;
  CatalogModel& catalog_;
  TopicDemandTracker& topic_demand_tracker_;
  QWidget* dialog_parent_;
  ChromeMetricsProvider chrome_metrics_provider_;

  QString selected_plugin_;
  int retention_seconds_ = 5;
  // GUI thread only (the worker reads it only inside its queued ingest tick), so not atomic.
  bool paused_ = false;

  // Receives every session's object pushes while paused; drained into the primary at resume. One instance for all
  // sessions, because pause is global.
  std::unique_ptr<ObjectStore> secondary_object_store_;

  // Scalar twin of secondary_object_store_. Its time domains and datasets carry the primary's ids (createSession
  // forces them) and DataSourceRuntimeHost mirrors each topic, so the resume merge matches topics by dataset and
  // name with no translation.
  std::unique_ptr<DataEngine> secondary_data_engine_;

  // unique_ptr: the worker keeps a pointer to its session across map mutations on the GUI thread.
  std::unordered_map<DatasetId, std::unique_ptr<StreamingSession>> sessions_;
  std::vector<std::shared_ptr<PendingStart>> pending_starts_;
  // Final pure-lazy counter of each ended session. Cleared when the next recording's targets are built.
  std::unordered_map<DatasetId, uint64_t> ended_skipped_lazy_;
};

}  // namespace PJ
