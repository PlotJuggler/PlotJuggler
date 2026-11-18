#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QMetaObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "DialogWatch.h"
#include "FileLoader.h"
#include "LayoutXml.h"
#include "LoadInput.h"
#include "ToolboxPresenter.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/plugin_data_api.h"
#include "pj_base/time.hpp"
#include "pj_base/types.hpp"
#include "pj_plotting/CurveTracker.h"
#include "pj_plugins/host_qt/embedded_view_host.hpp"
#include "pj_runtime/CurveDescriptor.h"
#include "pj_runtime/HistoryScope.h"
#include "pj_runtime/RecordingService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/ChromeMetrics.h"
#include "pj_widgets/TimeFormat.h"
#include "pj_widgets/VisualizationKind.h"

class QAction;
class QLabel;
class QButtonGroup;
class QCloseEvent;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QMenu;
class QPaintEvent;
class QPushButton;
class QSettings;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace Ui {
class MainWindow;
}

#ifdef PJ_WITH_SCENE3D
namespace pj::scene3d {
class FixedFrameMemory;
}  // namespace pj::scene3d
#endif

namespace PJ {

class TransformService;

class AppSession;
struct CatalogItem;
struct CustomTopicDeleteRefusal;
struct DerivedDiagnostic;
struct DerivedSnapshot;
class CurveEditor;
class DerivedObjects;
class DiagnosticHistory;
class DiagnosticsDialog;
class DockWidget;
class FileLoader;
class McpJobsDialog;
class McpServer;
class IngestProgressController;
class IDataWidget;
class LayoutImportBatch;
class MarketplaceWindow;
class PanelEngine;
class PlotDocker;
class PlotWidget;
class PreferencesDialog;
class CustomWidgetController;
class CustomWidgetDockWidget;
class StateTransitionsDockWidget;
class PendingDisplayBinder;
class SourceCacheStore;
class SourceCaptureService;
class QtDiagnosticBridge;
class SceneDockWidget;
class StreamingSourceManager;
class IngestProgressWidget;
class MessageBox;
class ToolboxRuntimeHost;
struct ToolboxPanelSession;
struct RuntimeToolboxPlugin;
class PluginPlotTabsController;
class SourceTimelineController;
class TopicDemandController;
class SvgButton;
class Timeline;
class RecentFilesMenu;
class Theme;
class TitleBar;
class ToastManager;
class UpdateChecker;
class WelcomePresetsDialog;
class RegistryManager;
class FeatureCatalogHost;
class TelemetryPing;
class CoalescingTrigger;

// Persisted as int under QSettings "MainWindow.legendStatus": the values are fixed.
enum class LegendStatus {
  kBottomRight = 0,
  kBottomLeft = 1,
  kTopRight = 2,
  kTopLeft = 3,
  kHidden = 4,
};

class MainWindow : public QMainWindow {
  Q_OBJECT
  friend class MainWindowSourceLayoutTestPeer;
  friend class MainWindowFanoutAmbiguousTestPeer;
  friend class MainWindowViewportReframeTestPeer;
  friend class MainWindowPluginViewsTestPeer;
  friend class MainWindowHistoryTestPeer;
  friend class MainWindowPanelGeometryTestPeer;
  friend class MainWindowLayoutImportTestPeer;
  friend class MainWindowMarkerGeneratorTestPeer;
  friend class MainWindowMarkerStatusTestPeer;
  friend class MainWindowLayoutDatasetPathsTestPeer;
  friend class MainWindowReloadButtonTestPeer;
  friend class MainWindowHistoryExemptTestPeer;
  friend class MainWindowCustomSeriesTestPeer;
  friend class MainWindowCustomTopicsTestPeer;
  friend class MainWindowRelease41LayoutTestPeer;
  friend class MainWindowScene3dDropFramingTestPeer;
  friend class MainWindowOnDemandRestoreTestPeer;
  friend class MainWindowEditorIntegrationTestPeer;
  friend class PresetTestPeer;
  friend class ToolboxPanelFoldTestPeer;
  friend class FeatureCatalogTestPeer;
  friend class McpTestPeer;
  friend class ToolboxPresenter;

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  explicit MainWindow(QString extensions_dir, QWidget* parent = nullptr);
  ~MainWindow() override;

  // --test-data: generated sin/cos series and one image.
  [[nodiscard]] bool populateTestData();

#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  // Acceptance-build probes. The layout bytes come from the production save serializers; nothing is written.
  [[nodiscard]] QByteArray wasmProbeLayoutBytes() const;
  [[nodiscard]] qsizetype wasmProbeDatasetCount() const;
  [[nodiscard]] std::size_t wasmProbeResidentPoolCapacity() const;
  void wasmProbeReportFilterResult() const;
  void wasmProbeReportToolboxTransformResult() const;
  // Opens a toolbox as its Toolbox-menu entry does.
  void wasmProbeLaunchToolbox(const QString& plugin_id);
  // Reports streaming controls, catalog state, playback range and the first plotted curve; read-only.
  void wasmProbeReportStreamingState() const;
  // Supersedes an in-flight source-replay picker without opening another, so browser tests can drive the
  // late-callback rejection.
  void wasmProbeSupersedeBrowserReplayPicker();
  // Plot readback shortcuts (F8, F9, F10); defined in tests/wasm_acceptance_probes.inc.
  void installWasmPlotAcceptanceProbes();
#endif

  // --layout: reloads the layout's data sources without prompting.
  void loadLayoutAtStartup(const QString& path);

  // Loads explicit files, then applies the optional layout against their data. A failed or cancelled file
  // aborts the layout and settles it false. Only attended file loads may open dialogs; the layout never prompts.
  void loadAtStartup(const QStringList& data_files, const QString& layout_path, bool interactive);

  // Startup step, before any layout restore: see CustomWidgetController::reviewUncleanShutdown. A
  // non-interactive run never shows the question.
  void reviewUncleanShutdown(bool interactive);

  // Adopts a Custom Widget dock for widget `type` of `plugin_id` into the empty `dock`, also when the widget
  // cannot be created (library missing, Qt build mismatch): it then shows the reason and keeps the choice for
  // the layout. Returns whether the plugin widget was created.
  bool createCustomWidgetDock(DockWidget* dock, const QString& plugin_id, const QString& type);

  // Opens the layout at `url` (http/https) as a preset, with no picker: its <fileInfo> sources download from
  // beside it one at a time, are checked against their fingerprints, then imported all or nothing (a failure or
  // cancel removes what this load imported and restores the previous workspace). Scene3D resources resolve
  // against the same location. Refused with reason "busy" while a layout load is pending. Every call ends in
  // exactly one layoutUrlLoadFinished.
  void loadLayoutFromUrl(const QUrl& url);
  // Stops the loadLayoutFromUrl in flight, if any: its download is dropped, its own imports are cancelled
  // (discarding their data) and the workspace comes back once they wind down. Other loads are left alone.
  void cancelLayoutFromUrl();

  // The page this build was opened from: fills a replayed stream's deployment variables
  // (${deployment.*}). The browser build sets it at startup; the desktop app leaves it empty.
  void setDeploymentPageUrl(const QUrl& page_url) {
    deployment_page_url_ = page_url;
  }

#ifdef PJ_TARGET_WASM
  // Looks for presets/index.json beside `page_url` and offers its presets in the welcome dialog and under
  // File → Presets, or opens the one a ?preset=<id> deep link names; without a usable index nothing changes.
  // Call once, from the event loop, after the window is shown.
  void startBrowserPresets(const QUrl& page_url);
#endif

  // --autoplay: starts looping playback once, the first time a data source provides a non-empty time range.
  // Call before loading data.
  void enableAutoplay();

  [[nodiscard]] TitleBar* titleBar() const {
    return title_bar_;
  }

  // The pipeline plugins report through (DiagnosticHistory, the title-bar bell).
  [[nodiscard]] DiagnosticSink diagnosticSink() const;

  // Every in-process diagnostic re-emits through it, queued. For extra subscribers (--dump-diagnostics).
  [[nodiscard]] QtDiagnosticBridge* diagnosticBridge() const {
    return diagnostic_bridge_;
  }

  // `message` may be rich text; an `<a href>` opens in the system browser.
  void showToast(const QString& message, const QPixmap& icon = QPixmap());

  // One release check. A newer release always toasts; only an `interactive` check (Help ▸ Check for Updates)
  // also toasts "up to date" and "couldn't check".
  void checkForUpdates(bool interactive);

  // Startup scan of the installed extensions against the marketplace registry: reveals the title bar's Update
  // button when any has a newer version. Silent on a network or parse failure.
  void checkExtensionUpdates();

#ifndef PJ_TARGET_WASM
  // main.cpp skipped checkExtensionUpdates(): tells the feature catalog why no registry is known.
  void noteRegistryScanSkipped();

  // Reports every fetch `manager` starts from now on to the feature catalog,
  // which reuses them instead of fetching on its own.
  void trackRegistryFetches(RegistryManager* manager);
#endif

  // The anonymous daily-user ping, silent. `installation` is the PJ_INSTALLATION build stamp; the opt-out gate
  // is the caller's.
  void sendTelemetryPing(const QString& installation);

#ifndef PJ_TARGET_WASM
  // The anonymous daily usage report (docs/TELEMETRY.md). Call once at startup. Points usageCounters() at its storage
  // file in every run, so an opt-out deletes what an earlier run stored. With the preference on, `may_report` (no
  // automation) and a reporting installation: enables the counters, keeps the installed-plugin inventory current,
  // flushes periodically (~MainWindow flushes the rest) and uploads the finished days once the public registry is
  // known (its ids, with the shipped ones, are the plugins a report may name; a failed fetch keeps the days).
  void startUsageReporting(const QString& installation, bool may_report);
#endif

  // The PJ_INSTALLATION build stamp: decides where checkForUpdates() looks and how the toast offers an update.
  void setInstallation(const QString& installation);

  // Presents the embedded external-process view in place of the chart area (replacing any presented panel)
  // until its session ends. Desktop only.
  void openEmbeddedConsole();

  // Persisted under QSettings ui/icon_size, ui/icon_padding, ui/layout_padding, ui/layout_spacing.
  [[nodiscard]] const ChromeMetrics& chromeMetrics() const {
    return chrome_metrics_;
  }

  // The setters only apply live, so a Preferences scrubber drag previews without writing; PreferencesDialog
  // commits through this on OK.
  void persistChromeMetrics() const;

  // Custom-folder edits apply on the next launch.
  [[nodiscard]] QStringList customPluginFolders() const;
  void setCustomPluginFolders(const QStringList& folders);
  [[nodiscard]] QStringList builtinPluginFolders() const;

  // Where launchToolbox presents the toolbox; kPinnedTab is the layout restore of a pinned tab. Declared here
  // because moc rejects type declarations inside slots/signals sections.
  enum class ToolboxLaunchTarget { kTakeover, kPinnedTab };

  // The only owner of the "Marketplace/registryUrl" key. registryUrlSetting() is the raw value, "" for the
  // built-in default; the setter persists at once. effectiveRegistryUrl() falls back to the default for an
  // invalid stored value; isValidRegistryUrl() (http/https/file) is the rule the Preferences editor shares.
  [[nodiscard]] QString registryUrlSetting() const;
  void setRegistryUrlSetting(const QString& url);
  [[nodiscard]] QUrl effectiveRegistryUrl() const;
  [[nodiscard]] static bool isValidRegistryUrl(const QString& url);

  [[nodiscard]] static QString defaultRegistryUrl();

  // Preferences key (bool): PlotJuggler's own window frame. Read at construction; a change applies on the next
  // launch.
  static constexpr QLatin1StringView kCustomTitleBarKey{"Preferences::custom_title_bar"};

  // Preferences key (bool): the platform's file picker (FileDialog::setUseSystemDialog).
  static constexpr QLatin1StringView kSystemFileDialogKey{"Preferences::system_file_dialog"};

  // Preferences key (bool, default on): the anonymous daily usage report. Turning it off deletes what was counted at
  // once; turning it on applies from the next launch.
  static constexpr QLatin1StringView kUsageStatsKey{"Preferences::send_usage_stats"};

  // Preferences key (bool): wall-clock time labels in UTC instead of the system time zone.
  static constexpr QLatin1StringView kClockUtcKey{"Preferences::clock_utc"};

  // Key (bool) of the date-time toggle (buttonUseDateTime): time labels as a clock instead of seconds.
  static constexpr QLatin1StringView kDateTimeDisplayKey{"MainWindow.dateTimeDisplay"};

  // False on macOS (its window controls are not ours to imitate) and in the browser, where the Preferences
  // toggle is hidden.
  [[nodiscard]] static bool customFrameSelectable();

  // The saved preference where selectable, always in the browser, never elsewhere.
  [[nodiscard]] static bool usesCustomFrame();

  // --- Agent control: the services and actions the MCP server (src/mcp/) drives. ---

  // Before deleting `removed_topics`: finds the transforms depending on them transitively, asks, and on
  // confirmation removes them and their Custom Topics entries, then runs `proceed` (at once when nothing
  // depends on them). `proceed` may run after this returns (the browser prompt is non-blocking), so it must
  // re-validate anything that could change meanwhile. `still_allowed` is asked after the prompt is accepted and
  // before anything is removed; false abandons the delete (it reports why itself). `cancelled` runs instead of
  // `proceed` when the delete is abandoned (user cancel, `still_allowed` refusal, failed removal), so a caller
  // always hears exactly one outcome. `on_dialog` is told when the prompt opens and closes.
  void confirmAndRemoveDependentTransforms(
      const std::vector<TopicId>& removed_topics, std::function<void()> proceed,
      std::function<bool()> still_allowed = {}, std::function<void()> cancelled = {},
      dialog_presenter::DialogWatch on_dialog = {});

  // No prompt: the caller has confirmed and removed the dependent derived series. Re-seeds playback (unless a
  // stream is live) and resets undo once for the batch.
  void removeDatasets(const QList<DatasetId>& dataset_ids);

  // Reloads `dataset_id` from its file with the plugin and config recorded at its load. `policy` overrides the
  // interactive reload's dialog policy; `still_allowed` becomes LoadHints::still_allowed. Returns the load
  // ticket, 0 when the dataset is not file-backed or the load was rejected.
  LoadRequestId reloadDataset(
      DatasetId dataset_id, std::optional<DialogPolicy> policy = std::nullopt,
      std::function<bool()> still_allowed = {});

  // Rebuilds the Custom Topics rows from the transform and on-demand recipes; only the services know which
  // catalog topics are derived. Call it, AFTER the catalog rebuild (or the new outputs have no keys yet), from
  // every path that changes the recipe set without retiring the outputs' catalog items (an on-demand install,
  // a plugin creating or withdrawing a transform, a layout restore); a retiring path needs nothing, the panel
  // prunes on CatalogModel::itemsRemoved.
  void syncCustomTopicsPanel();

  // The session services the MCP server reads and drives; they live as long as this window.
  [[nodiscard]] AppSession& appSession() const {
    return *session_;
  }
  [[nodiscard]] FileLoader& fileLoader() const {
    return *file_loader_;
  }
  [[nodiscard]] StreamingSourceManager& streamingSources() const {
    return *streaming_manager_;
  }
  [[nodiscard]] PluginPlotTabsController& pluginPlotTabs() const {
    return *plugin_plot_tabs_;
  }

  // The service assembly a toolbox plugin with id `owner` would get (data processors, object
  // reads, playback, its own plot and scene tabs) with no plugin loaded: an MCP agent's tools run
  // against it. Null after reporting the failure to the diagnostics. The caller must release it
  // before this window's services die (~MainWindow destroys the MCP server right after the
  // toolboxes).
  [[nodiscard]] std::shared_ptr<ToolboxPanelSession> buildAgentSession(const QString& owner, const QString& label);
  // The service registry of `session`; valid while the session lives.
  [[nodiscard]] static PJ_service_registry_t toolboxSessionServices(ToolboxPanelSession& session);

#ifndef PJ_TARGET_WASM
  // The feature catalog `about` serves.
  [[nodiscard]] FeatureCatalogHost* featureCatalog() const {
    return feature_catalog_.get();
  }
#endif

  // Preferences key (bool, default on): open MCP public access so AI agents can control this
  // window. Read at startup by main.cpp, which ignores it for an unattended run; the Preferences
  // switch shows whether public access is open and applies on OK.
  static constexpr QLatin1StringView kMcpServerKey{"Preferences::mcp_server"};

  // Any local agent (the bridge) may connect with a random token the discovery file advertises. Starts the
  // server on a free 127.0.0.1 port unless it already runs for a plugin endpoint (a failure goes to the
  // diagnostics). A no-op while open.
  void openMcpPublicAccess();
  // Closes public access (token and discovery file); agents' tabs and derived series stay. The
  // server keeps listening, refusing everything but toolbox plugins' private endpoints.
  void closeMcpPublicAccess();
  [[nodiscard]] bool mcpPublicAccess() const;
  // Opens public access and links it to the agent relay at `relay` (McpBrowserLink): how the browser build,
  // which cannot listen, reaches an agent (?mcp=; --mcp-relay on the desktop). False, after a diagnostic, when
  // `relay` is not a loopback http URL or the server cannot start. A second call replaces the link.
  bool connectMcpRelay(const QUrl& relay);

 public slots:
  // Apply live only (clamp, broadcast chromeMetricsChanged); persistChromeMetrics() writes QSettings.
  void setIconSize(int size);
  void setIconPadding(int padding);
  void setLayoutPadding(int padding);
  void setLayoutSpacing(int spacing);

 signals:
  // After qApp's stylesheet is applied.
  void stylesheetChanged(QString theme);

  // Exactly once per loadLayoutFromPath that reaches a terminal outcome (--exit-after-layout observes it).
  // `success` only when the layout committed, which for a progressive restore is after every restore waiter
  // cleared and the import batch finished, so quitting here never tears down an active batch. False on an
  // open/parse/apply failure, a user cancel or a cancelled batch. A superseded restore never settles.
  void layoutRestoreSettled(bool success);

  // The terminal of one loadLayoutFromUrl: `reason` is empty on success, else the token its
  // PJ_WASM_LAYOUT_LOAD_FAILED line names (busy, download, size, fingerprint, …) or "cancelled". A busy refusal
  // is reported at once while the load in flight carries on. Emitted after the load's state is cleared, so a
  // handler may start the next one.
  void layoutUrlLoadFinished(const QUrl& url, bool ok, const QString& reason);

  // A source of the loadLayoutFromUrl for `url` is downloading: `total` is its declared size or -1. Throttled;
  // each source ends with received == total once it has fully arrived. Sources report one after another.
  void layoutUrlLoadProgress(const QUrl& url, qint64 received, qint64 total);

  // One signal for all metrics, so consumers recompute from a consistent snapshot:
  //   band height       = (icon_size + icon_padding) + 2 * layout_padding
  //   chrome margins    = layout_padding (QLayout::setContentsMargins)
  //   chrome / list gap = layout_spacing (QLayout::setSpacing, CurveEditor row gap)
  void chromeMetricsChanged(const ChromeMetrics& metrics);

 private slots:
  void onLoadLayout();
  void onSaveLayout();
#ifdef PJ_TARGET_WASM
  void onSaveSourceLayout();
#endif
  void onLoadRecentLayout(const QString& path);

  void onOpenMarketplace();
  void onLoadDataRequested();
  // Reloads the most recent source with its recorded plugin and config.
  void onReloadDataRequested();
  // Curve tree "Reload": that dataset's own recorded plugin and config, not the most recent load's.
  void onReloadDatasetRequested(DatasetId dataset_id);
  // Curve tree "Replace": transactionally replaces the dataset's data with another file.
  void onReplaceDatasetRequested(DatasetId dataset_id);
  void onDatasetInfoRequested(DatasetId dataset_id);

  void onFileLoaded(
      const QString& path, const QString& prefix, const QString& plugin_id, const QString& plugin_config_json,
      const QString& plugin_manifest_id);

  void onCatalogTrashRequested(QStringList keys, bool covers_all);
  // One combined confirmation, then removeDatasets().
  void onRemoveDatasetsRequested(const QList<DatasetId>& dataset_ids);
  // Confirms, merges, and marks the result on the Source Timeline.
  void onMergeDatasetsRequested(const QList<DatasetId>& dataset_ids);

  // Erases one dataset's data in this order: TF buffer, object payloads, catalog items, the engine's scalar
  // storage; drops its file association. Does not confirm, re-seed playback or reset undo: the caller does
  // those once per batch.
  void removeDatasetData(DatasetId dataset_id);

  void onShowPreferencesDialog();
  void onShowAboutDialog();
  void onCheckForUpdates();

  // The update toast's `pj-update:` link: starts the Windows maintenance tool in updater mode, then closes the
  // app, whose files Windows keeps locked while it runs.
  void onUpdateLinkActivated(const QUrl& url);

  // Help ▸ Installed Extensions, informational (disabled entries).
  void onRebuildExtensionsMenu();
  // The title-bar Toolbox menu, rebuilt on aboutToShow: launchable non-cloud toolboxes only (cloud ones are in
  // the Sources panel).
  void onRebuildToolboxMenu();

  // Points the progress strip at the last-started interactive toolbox import and takes displayed ownership.
  // Callers check FileLoader is idle: the strip has one owner and file loads win.
  void adoptToolboxIngestStrip();

  // Permille on the strip (counts may be bytes beyond int range); total 0 keeps the busy bar.
  void setIngestStripProgress(quint64 current, quint64 total);

  // Hands the strip to a surviving eligible ingest (rule at ingest_strip_owner_kind_), skipping `exclude`
  // (0 = none), with its recorded label and progress. False when none survives. Call only while the strip is
  // engaged (shown or its show delay pending): it never starts the show timer.
  bool displaySurvivingIngestOnStrip(DatasetId exclude);

  // The displayed ingest ended or became ineligible: switch to a survivor, or clear ownership and start the
  // linger-hide (skipped while a file load is about to own the strip). No-op unless `ended` is displayed.
  void releaseIngestStripOwner(DatasetId ended);

  // Flag-only cooperative cancel of every live toolbox import, keep-partial (the toolbox ABI has no host-side
  // rollback). A host is dereferenced only after its owning panel's weak owner is checked alive.
  void stopAllToolboxImports();

  // Builds the runtime host and presents the toolbox in the chart area, or pins it as a tab per `target`. One
  // live instance per id: an already-pinned toolbox gets its tab focused. `initial_config` reaches the toolbox
  // through loadConfig() before its dialog is built (the Transform Editor's in-place edit). `pin_tab_name`
  // (kPinnedTab only) restores a renamed tab under its saved name.
  void launchToolbox(
      const QString& plugin_id, const QString& initial_config = QString(),
      ToolboxLaunchTarget target = ToolboxLaunchTarget::kTakeover, const QString& pin_tab_name = QString());

  void onThemeChanged(const QString& theme);

  void onPlotTabAdded(PlotDocker* docker);
  void onSettingsPanelToggleRequested(DockWidget* dock);

  // A placeholder 2D/3D icon was clicked: adopts an empty scene dock of that kind into `dock` (plot clicks are
  // handled inside DockWidget).
  void onObjectFamilyRequested(DockWidget* dock, VisualizationKind family);

  // A drop of an advertised placeholder topic: routed to TopicDemandController (handlePlaceholderPlotDrop for
  // kNone, else handleSceneDockPlaceholderDrop) instead of the normal add-curve/add-layer path.
  void onPlaceholderTopicDropped(
      DockWidget* dock, DatasetId dataset_id, QString topic_name, sdk::BuiltinObjectType object_type);

  // Picks the right-panel config page for the focused dock and rebinds the curve editor.
  void onDockFocused(DockWidget* dock);

  void onPlotAdded(PlotWidget* plot);

  // A chart-area takeover scoped to `sources`; on Apply each filtered output replaces its source curve on
  // `origin` in place (PlotWidget::replaceCurve).
  void openFilterEditor(std::vector<CurveDescriptor> sources, PlotWidget* origin);
  // Mirror the global grid, curve style and width onto a PlotWidget embedded in a toolbox panel (the
  // Transform Editor's preview).
  void syncPanelPreviewDisplay();
  // For a panel just shown: its embedded PlotWidget exists only after the engine's first tick.
  void schedulePanelPreviewSync();
  // The same for an open Filter Editor's preview plot; no-op for any other panel.
  void syncFilterEditorPreviewDisplay();

  /// Inherits the linked time range after a curve drop has fitted the receiving plot.
  void onPlotCurvesDropped(bool was_empty);
  void onPlotZoomChanged(PlotWidget* modified, QRectF rect);
  // A State Transitions strip's visible X window, fanned onto plots and the other strips under the link toggle.
  void onStateTransitionsRangeChanged(StateTransitionsDockWidget* source, double t_min, double t_max);

  void onTrackerMovedFromWidget(QPointF point);

  // Flips the SessionManager frame, re-seeds the playback range and shifts the playhead by the offset delta, so
  // the cursor stays on the same real instant.
  void onUseTimeOffsetToggled(bool checked);

  // The dataset whose offset frames the playhead and the reference line: the active stream, else the first
  // loaded one, 0 when empty. Exact only for a single-dataset session.
  [[nodiscard]] DatasetId representativeDatasetId() const;

  // The reference line in display-axis seconds, projected from reference_instant_ through the current offset;
  // nullopt when unset. Re-evaluated on every displayOffsetChanged.
  [[nodiscard]] std::optional<double> referenceDisplaySeconds() const;

  void broadcastReferenceLine();

  // From the date-time toggle and the UTC preference, to every plot, the playback readout and the Source
  // Timeline; receivers ignore an unchanged format.
  void applyTimeLabelFormat();

  // When data is already present at a scalar curve or object topic drop, seeds the playback range and playhead
  // at once; live ingest also seeds without a drop.
  void seedStreamingPlaybackFromDrop();
  // The first topic dropped into an empty click-created object view: seeds streaming playback and frames it.
  void onFirstObjectTopicAdded(DockWidget* dock);

  // Locks seeking (playback slider, Source Timeline) exactly while a followed stream is playing; call on every
  // change of either condition.
  void refreshStreamingSeekLock();

  // Editing offsets, order or merges, or seeking, is unsafe while the cursor is glued to the live tip. Locks the
  // Timeline's gestures and greys the align rail, whose buttons drive the controller directly.
  void setSourceTimelineStreamingLock(bool locked);

  // `force_new_state` pushes a non-coalescing undo entry, for discrete operations that must not merge into a
  // preceding edit (applying or removing a filter).
  void onUndoableChange(bool force_new_state = false);

  void onUndo();
  void onRedo();

  // Records every live stream into one batch folder, one file each, or ends the capture. A start that cannot
  // proceed unchecks the button and says why in a toast.
  void onRecordToggled(bool record);

  // Every file exists and every tap is attached.
  void onRecordingStarted(
      const QString& capture_id, const QString& directory, const QVector<SourceRecordingStart>& sources);

  // The toast names the one file, the folder when every source is clean, or the failing sources; per-source
  // detail goes to the log only.
  void onRecordingStopped(const CaptureResult& capture);

  // `dropped`: messages the byte budget had to drop.
  void onRecordingProgress(quint64 messages, quint64 payload_bytes, quint64 dropped);

 private:
  // So the next recording uses the Recording page's settings without a restart.
  void bindRecordingPreferences(PreferencesDialog& dialog);

  // Pushes the Plotting page's derived-markers mode and reload rate to the batch engine.
  void applyDerivedBatchPreferences();
  // On OK: applyDerivedBatchPreferences and applyTimeLabelFormat.
  void bindPlottingPreferences(PreferencesDialog& dialog);

  // PlotWidget::setMarkerStatus on BatchEngine::runningChanged. "Computing" shows only after a rule has run for
  // kMarkerStatusShowDelay, so a quick run never flickers a note.
  void wireMarkerPlotStatus();
  // The standing marker status, once wireMarkerPlotStatus has run.
  void reportMarkerStatus(PlotWidget* plot);
  void onPlotMarkerTargetsChanged();
  std::function<void(PlotWidget*)> push_marker_status_;
  // PlotWidget::setSeriesStatus to `only`, or every plot when null.
  void pushSeriesStatus(PlotWidget* only);

  // The dialog is skipped when plugin and config are known, unless `policy` says otherwise; the loader asks
  // `still_allowed` before applying anything. Returns the load ticket, 0 when the loader refused the request.
  LoadRequestId reloadSource(
      const QString& path, const QString& plugin_id, const QString& plugin_config_json,
      std::optional<DialogPolicy> policy = std::nullopt, std::function<bool()> still_allowed = {});

  // Enables the Reload button iff a reloadable source is recorded. reloadSource disables it for the
  // request's duration; call this from every way that request can end.
  void refreshReloadEnabled();

  // Persists `position` (kHidden hides), updates the legend button and applies to every plot.
  void setLegendStatus(LegendStatus position);

  // The global toolbar toggles, for a new plot.
  void applyGlobalToggles(PlotWidget* plot);
  void applyShowPointsToDock(DockWidget* dock);
  void applyShowPointsTo2DWidgets();
  void applyLegendStatus(PlotWidget* plot);
  // Left-click: next corner, hidden after the last one.
  void onLegendButtonClicked();
  // Switches the plot's default style between Lines and Lines+Dots; any other style is left alone.
  void applyDots(PlotWidget* plot);

  void updateTimeTrackerIcon();
  void onTimeTrackerButtonClicked();

  // With buttonLink checked, the per-tab union of non-XY plots' X ranges. XY plots always zoom out alone: their
  // X axis is a value, not time.
  void linkedZoomOut();

  /// `SessionManager::displayTimeForSource` on the ABI's source handle, unwrapped to the axis double.
  [[nodiscard]] std::optional<double> displayTimeForSource(PJ_data_source_handle_t source, int64_t absolute_ns) const;
  /// displayTimeForSource on the dataset owning `topic` (empty: representativeDatasetId()); nullopt for an
  /// unknown topic. The `to_display_time` of every `pj.playback.v1` the shell offers.
  [[nodiscard]] std::optional<double> displayTimeForTopic(std::string_view topic, int64_t absolute_ns) const;

  // `source` and `id` are stable string literals; `message` is translated.
  void emitDiagnostic(DiagnosticLevel level, const char* source, const char* id, const QString& message);

  void refreshStreamingCombo();
  // Tells every dock (CustomWidgetSupport) whether any Custom Widget type is installed, with the
  // Custom-Widget-only object types.
  void refreshCustomWidgetsAvailability();

  void wireExistingPlots();

  void forEachDock(const std::function<void(DockWidget*)>& operation);
  // The active top-level tab's docks only.
  void forEachVisibleDock(const std::function<void(DockWidget*)>& operation);

  // The one seam for "the time cursor moved" (display-axis seconds): every data dock and the curve list's Value
  // column, on every tab.
  void broadcastTrackerTime(double display_seconds);

  // The per-tick playback and scrub fan-out: only the active tab's docks (plus the Value column), so off-screen
  // tabs do no work. Structural seeds use broadcastTrackerTime, and a tab switch re-seeds the revealed tab.
  void broadcastTrackerTimeToVisible(double display_seconds);

  void forEachSceneDock(const std::function<void(SceneDockWidget*)>& operation);
  void forEachPlot(const std::function<void(PlotWidget*)>& operation);
  // Every mounted State Transitions strip across all tabs.
  void forEachStateStrip(const std::function<void(StateTransitionsDockWidget*)>& operation);
  // Every mounted Custom Widget panel across all tabs.
  void forEachCustomWidgetDock(const std::function<void(CustomWidgetDockWidget*)>& operation);

  // On the catalog's cleared()/itemsRemoved(): every data widget prunes its dead curves or layers (an emptied
  // object dock returns to the placeholder).
  void syncWidgetsToCatalog();

  // Icons not owned by a subwidget with its own onStylesheetChanged.
  void applyIcons(QString theme);

  // The always-visible Chart and Legend icon column left of the local panel.
  void buildGlobalToolbar();
  // The Source Timeline's align actions, under the global toolbar; shown only while the strip is open
  // (applyBottomPanelConstraints).
  void buildTimelineAlignRail();
  // The Curve Width and Curve Style strips above the CurveEditor.
  void buildLocalToolbar();

  // To every curve of the editor's bound plot; no-op when unbound.
  void applyActivePlotWidth(double width);
  void applyActivePlotStyle(int style);

  /// The source qualifiers only let a source-replacement snapshot map a reminted DatasetId; history replay
  /// requires the raw id to be live.
  struct TimelineTrackState {
    DatasetId dataset_id = 0;
    qint64 display_offset_ns = 0;
    int timeline_order = -1;
    QString source_name;
    QString source_path;
    int source_index = -1;

    [[nodiscard]] bool operator==(const TimelineTrackState&) const = default;
  };

  /// Source Timeline offsets/order and view chrome kept outside layout XML.
  struct TimelineState {
    std::vector<TimelineTrackState> tracks;
    double zoom = 1.0;
    qint64 scroll_left_ns = 0;
    int scroll_top_px = 0;
    int name_column_width = 0;
    bool snap = true;

    [[nodiscard]] bool operator==(const TimelineState&) const = default;
  };

  using SnapshotScope = PJ::SnapshotScope;

  /// One atomic workspace snapshot: the layout XML, plus the session-only timeline state the XML does not hold.
  struct CapturedWorkspace {
    QByteArray xml;
    TimelineState timeline;

    [[nodiscard]] bool operator==(const CapturedWorkspace&) const = default;
  };

  struct PendingSourceReplacement {
    CapturedWorkspace workspace;
    QString path;
    // The arming load's LoadRequestId. The disarm compares tickets, not paths: loadFinished is queued, so a
    // stale same-path terminal from another load would otherwise kill a live load's capture.
    quint64 ticket = 0;
  };

  struct TimelineChromeState {
    double zoom = 1.0;
    qint64 scroll_left_ns = 0;
    int scroll_top_px = 0;
    int name_column_width = 0;
    bool snap = true;
  };

  using TimelineResolutionPlan = std::vector<DatasetId>;

  // Whether the load may open dialogs (reload, trust, missing-curve prompts). Passed explicitly through the whole
  // restore, LayoutImportBatch included (captured at construction): late continuations run long after the
  // caller's stack, and any startup flag, has unwound.
  enum class LayoutLoadInteractivity {
    kInteractive,  ///< user-driven (menu / recent list): prompts allowed
    kAutomated,    ///< --layout CLI: auto-reload, never any dialog
  };
  void loadLayoutFromPath(const QString& path, LayoutLoadInteractivity interactivity, bool use_current_data = false);
  // Byte-level layout loads: browser uploads and recipes, and loadLayoutFromUrl (which is why they build on the
  // desktop too). Every terminal outcome goes through finishBrowserLayoutLoad, which logs
  // PJ_WASM_LAYOUT_LOAD_OK/FAILED.
  enum class BrowserLayoutLoadResult { kApplied, kPending, kFailed };

  [[nodiscard]] QDomDocument browserGenericLayoutDocument() const;
  [[nodiscard]] std::optional<QDomDocument> browserSourceLayoutDocument(QString& error) const;
  // The live streams as a source-bound save writes them (configuration template, display offset, timeline
  // slot), and in `identity_for_dataset` each stream dataset's pj-stream:// identity for
  // layout_xml::stampDatasetIdentities. A session without a configuration is left out: nothing could restart it.
  [[nodiscard]] QList<layout_xml::StreamingSourceRef> liveStreamRecords(
      QHash<std::uint32_t, QString>& identity_for_dataset) const;
  // Each dataset's top-to-bottom Source Timeline slot; a dataset without a bar has no entry.
  [[nodiscard]] QHash<DatasetId, int> timelineSlots() const;
  // `layout_url` is where the bytes came from (empty for an upload): Scene3D resources and file sources resolve
  // against it, and its sources download instead of being reselected.
  void loadLayoutFromBytes(const QByteArray& bytes, const QString& browser_name, const QUrl& layout_url = {});
  [[nodiscard]] BrowserLayoutLoadResult loadBrowserParsedLayout(
      QDomDocument doc, const QString& browser_name, const QUrl& layout_url);
  void restoreBrowserChromeAndPanels(const QDomDocument& doc, const QString& browser_name);
  void beginBrowserSourceReplay(
      QDomDocument doc, const QString& browser_name, QList<layout_xml::DataSourceRef> sources, const QUrl& layout_url);
  [[nodiscard]] quint64 beginBrowserReplayPickerGeneration();
  void selectBrowserReplaySource();
  void loadBrowserReplaySource(
      quint64 picker_generation, QString browser_name, std::optional<LoadInput> input, QString error);
  // Accepts the checked `input` for the current source slot and moves on; `name` is only logged.
  void stageReplayInput(LoadInput input, const QString& name);
  // Relabels the decision dialog for the replay's next file to pick (a data file or a side file).
  void promptBrowserReplayFile();
  // True when more than one file of the replay has this basename: a fingerprint must then tell them apart.
  [[nodiscard]] bool isAmbiguousReplayName(const QString& basename) const;
  // The decision text asking for side file `number` of `total`.
  [[nodiscard]] static QString browserReplaySideFilePrompt(
      const QString& name, const QString& source_path, int number, int total);
  // Remote replay: stages the download under the same generation token a picker selection uses.
  void downloadNextReplaySource();
  // Whether the replay still waits for the download started under `generation`.
  [[nodiscard]] bool isActiveReplayDownload(quint64 generation) const;
  // End the replay with reason "download", or "size" (over kMaxPresetDataBytes).
  void failReplayDownload(const QString& error);
  void failReplayOverCap();
  void addDownloadedReplaySource(quint64 generation, std::optional<LoadInput> input, const QString& error);
  // Cancels (discarding) every import of the replay still awaiting its terminal.
  void cancelBrowserReplayImports();
  void startBrowserReplayImports();
  void finishBrowserReplayImports();
  void rollbackBrowserReplayImports();
  void finishBrowserLayoutLoad(const QString& browser_name, bool applied, const QString& reason = {});
  void updateBrowserLayoutActions();
  // How a complete-snapshot restore handles curves no loaded dataset can provide.
  enum class MissingCurvePolicy {
    kPrompt,      ///< layout load: prompt the user (cancel aborts, remove strips them)
    kSilentDrop,  ///< compatibility restore: unresolved curves may be discarded
    kExact,       ///< history/rollback: fail rather than drop unresolved state
    // Non-interactive batch restore: no dialog; unresolved intents are retained (never stripped) and diagnosed,
    // so a later binding pass can resolve them once their data arrives.
    kRetainAndDiagnose,
  };

  // Shared by the early offset write and the later track-order restore, so the two cannot bind different
  // datasets.
  struct LayoutTimelineTrack {
    DatasetId dataset_id;
    qint64 display_offset_ns;
    bool has_display_offset;
    int timeline_order;
  };
  using LayoutTimelinePlan = std::vector<LayoutTimelineTrack>;
  // Where a layout's stream records landed: pj-stream:// identity to dataset (layout_xml::bindStreamQualifiers)
  // and each dataset's saved timeline track. add() fills both, so they never disagree.
  struct LayoutStreamBinding {
    QHash<QString, std::uint32_t> dataset_for_identity;
    LayoutTimelinePlan tracks;
    void add(const layout_xml::StreamingSourceRef& record, DatasetId dataset_id) {
      dataset_for_identity.insert(layout_xml::streamSourceIdentity(record.id), dataset_id);
      tracks.push_back(
          LayoutTimelineTrack{
              dataset_id, record.dataset.display_offset_ns, record.dataset.has_display_offset,
              record.dataset.timeline_order});
    }
  };

  // The synchronous leg: applies a parsed layout to already-loaded data (the progressive leg is
  // beginProgressiveLayoutRestore). `stream_tracks`, the timeline tracks of the streams this load started or
  // bound, join the files' offsets and order; a layout with some applies though their datasets are still empty.
  void applyRestoredLayout(
      QDomDocument doc, const QString& path, MissingCurvePolicy policy, const LayoutTimelinePlan& stream_tracks);
  // Starts each stream from its saved configuration, without its plugin's dialog, and adds it to `binding`.
  // Returns one "Stream '<id>': why" line per stream that did not start.
  [[nodiscard]] QStringList startLayoutStreams(
      const QList<layout_xml::StreamingSourceRef>& streams, LayoutStreamBinding& binding);
  // `policy` comes from the load's interactivity and is captured for the drain, never inferred from whether a
  // batch exists.
  void beginProgressiveLayoutRestore(
      QDomDocument doc, const QString& path, MissingCurvePolicy policy, const LayoutTimelinePlan& stream_tracks = {});
  void cancelProgressiveLayoutRestore();
  [[nodiscard]] bool rollbackProgressiveWorkspace();
  [[nodiscard]] bool abortProgressiveRestore();
  void flushPendingCurveBindings(const std::vector<CatalogItem>& items);
  /// Re-registers the document's unresolved curves with the pending binder
  /// (live scene pends survive; see PendingDisplayBinder::collect).
  void collectPendingDisplayBindings(const QDomDocument& doc);
  /// collectPendingDisplayBindings plus an immediate flush, after restore and rollback.
  void rebuildPendingDisplayBindings(const QDomDocument& doc);
  /// Coalesces plot-owned intent changes into one binder rebuild on the event loop.
  void schedulePendingDisplayBindingRebuild();
  /// Settles every scene dock after a restore: dataset choice, a last pending retry, then the verdict
  /// (permanent failure, unresolved blocking topics).
  struct SceneRestoreVerdict {
    bool failed = false;
    QStringList blocking_topics;
  };
  [[nodiscard]] SceneRestoreVerdict settleSceneRestores();

  int retryPendingSceneRestores(const std::vector<CatalogItem>& items);
  [[nodiscard]] QStringList unresolvedPendingSceneRestores();
  void clearPendingSceneRestores();
  void onProgressiveLayoutDrained();
  // The end-of-drain owner of every unresolved-state decision (binder intents, blocking scene pends), on the same
  // policy applyWorkspace honours: kRetainAndDiagnose keeps the intents and diagnoses, kPrompt prompts. False
  // when the user cancelled (the restore was aborted).
  [[nodiscard]] bool finalizeUnresolvedRestoreState(MissingCurvePolicy policy);
  // A drain-time restore failure: diagnostic under kRetainAndDiagnose, warning dialog otherwise.
  void reportLayoutRestoreIssue(MissingCurvePolicy policy, const char* id, const QString& message);
  // For a restore with materialize-bearing sources; the shell effects (workspace checkpoint, removeDatasetData,
  // trust prompt, diagnostics) are its hooks. Takes bool: the batch is MainWindow-free and cannot name
  // LayoutLoadInteractivity.
  [[nodiscard]] std::unique_ptr<LayoutImportBatch> makeLayoutImportBatch(bool interactive);
  // A pending import keeps the restore alive even when FileLoader is idle.
  [[nodiscard]] bool layoutImportBatchActive() const;
  // The one predicate for a restore that must go progressive: the loader still has the reload queue, or a batch
  // import is producing data the drain must wait for.
  [[nodiscard]] bool restoreHasPendingAsyncWork(bool reload_requested) const;
  // The progressive-drain gate: proceeds only when no restore waiter is pending. A cancelled batch unwinds the
  // progressive state instead: its rollback already restored the pre-layout workspace.
  void maybeSettleProgressiveRestore();
  // Connects SessionManager's three ingest lifecycle signals at the batch handover only. The batch's announced
  // dataset shows under kLayoutBatch ownership; every other ingest takes the normal strip arbitration.
  void installLayoutBatchObservers();
  // Disconnects the observers and releases a batch-owned strip. Must run before every layout_import_batch_ reset
  // or destruction, or a lifecycle event or Stop click reaches a dangling batch.
  void teardownLayoutBatchObservation();
  // A still-running batch takes a hard shutdown (children cancelled and joined, no rollback). Outside a restore
  // it also ends the retention channel a kRetainAndDiagnose drain left (pending_items_added_conn_).
  void resetLayoutImportBatch();
  // Tears down an in-flight progressive restore and retires the batch. Every new layout load and closeEvent go
  // through it: a load that stays synchronous never reaches beginProgressiveLayoutRestore to clean up the old one.
  void supersedeActiveRestore();
  // Applies the timeline state a progressive restore stashed, once the reloaded datasets' paths are registered.
  // Runs before the processor replay (an all_datasets marker generator must see the settled offset) and before
  // the viewport re-frame (the saved window converts with it). Returns whether any offset moved.
  bool applyPendingTimelineState();
  // kExact, then the file sources' timeline state plus `stream_tracks` (the replay's streams, still without
  // data, so an empty catalog is not an error when there are some).
  [[nodiscard]] bool applyBrowserRestoredLayout(
      QDomDocument doc, const QString& browser_name, const LayoutTimelinePlan& stream_tracks = {});
  // The replay's last step: applies it, rolls back on a failure or replaces the previous preset's data on a URL
  // load's success, and finishes the load. `apply_failure` is the token if the layout itself does not apply.
  void applyBrowserReplay(QDomDocument apply_doc, const QString& apply_failure);
  // Starts the replay's streams, binds the pj-stream:// qualifiers to the new sessions and applies. Returns the
  // failure token ("stream", else `apply_failure`), empty on success.
  [[nodiscard]] QString startReplayStreamsAndApply(QDomDocument apply_doc, const QString& apply_failure);
  // After a preset load succeeded: records what it brought and removes only what the preset before it brought.
  void replacePreviousPresetData();
  void restoreChromeAndPanels(
      const QDomDocument& doc, const QString& path, const LayoutTimelinePlan* resolved_timeline = nullptr);
  void saveLayoutToPath(const QString& path, bool include_data_source);
  void recordRecentLayout(const QString& path);
  [[nodiscard]] QStringList recentLayouts() const;

  // Rewrites every curve's stable topic+field path to the catalog key of the first loaded dataset that has it.
  // Returns the paths no dataset provides.
  [[nodiscard]] QList<layout_xml::SeriesPath> rebindCurvesToLoadedDatasets(QDomDocument& doc);

  enum class TimelineRestoreMode {
    kExact,
    kPortableSourceReplacement,
  };
  enum class RestoreResult {
    kApplied,    ///< plots/toggles applied to the session
    kCancelled,  ///< user cancelled at the missing-curve prompt (kPrompt only)
    kFailed,     ///< xmlLoadState rejected the document
  };
  using RestoreIntent = PJ::RestoreIntent;
  // Restores a complete workspace snapshot; the one path for layout load and undo/redo, so the two cannot drift.
  // Order: display offsets, processors and marker generators, then curve rebind and xmlLoadState. Snapshots
  // carry stable topic/field paths, so one survives a data reload. Callers set applying_state_ as needed.
  // `out_reason` receives a `kHistory` restore's refusal (checkDataProcessorDependencies), untouched otherwise.
  [[nodiscard]] RestoreResult restoreWorkspaceState(
      QDomDocument& doc, MissingCurvePolicy policy, const CapturedWorkspace* rollback_to = nullptr,
      RestoreIntent intent = RestoreIntent::kReplace, QString* out_reason = nullptr);
  [[nodiscard]] RestoreResult restoreWorkspaceState(
      const CapturedWorkspace& target, MissingCurvePolicy policy, TimelineRestoreMode timeline_mode,
      const CapturedWorkspace* rollback_to = nullptr, RestoreIntent intent = RestoreIntent::kReplace,
      QString* out_reason = nullptr);

  [[nodiscard]] CapturedWorkspace captureWorkspace(SnapshotScope scope) const;
  // SnapshotScope::kFull.
  [[nodiscard]] CapturedWorkspace captureWorkspace() const;
  // A non-zero `stamp_override_id` is stamped with `stamp_override_path` instead of its tracked path: a
  // replacement load retires it for datasets from the new path, and the rebind maps its charts onto them.
  [[nodiscard]] CapturedWorkspace capturePortableWorkspace(
      DatasetId stamp_override_id = 0, const QString& stamp_override_path = {}) const;
  [[nodiscard]] TimelineState captureTimelineState() const;
  [[nodiscard]] TimelineChromeState captureTimelineChrome() const;
  void applyTimelineChrome(const TimelineChromeState& state);
  [[nodiscard]] std::optional<DatasetId> resolveTimelineTrack(
      const TimelineTrackState& track, TimelineRestoreMode mode,
      const std::vector<std::pair<DatasetId, QString>>& live_datasets) const;
  // Resolves every id and checks every overflow guard before the first offset is written.
  [[nodiscard]] std::optional<TimelineResolutionPlan> validateTimelineState(
      const TimelineState& state, TimelineRestoreMode mode) const;
  // Runs before the processor replay, so a marker generator sees the settled offset. `plan` comes from
  // validateTimelineState; false on a size mismatch, mutating nothing.
  [[nodiscard]] bool applyTimelineOffsets(const TimelineState& state, const TimelineResolutionPlan& plan);
  [[nodiscard]] bool applyTimelineState(const TimelineState& state, const TimelineResolutionPlan& plan);
  [[nodiscard]] RestoreResult applyWorkspace(
      QDomDocument& doc, MissingCurvePolicy policy, const TimelineState* timeline_state,
      const TimelineResolutionPlan* timeline_plan, RestoreIntent intent, QString* out_reason = nullptr);
  [[nodiscard]] RestoreResult restoreWorkspaceStateImpl(
      QDomDocument& doc, MissingCurvePolicy policy, const TimelineState* timeline_state,
      TimelineRestoreMode timeline_mode, const CapturedWorkspace* rollback_to,
      RestoreIntent intent = RestoreIntent::kReplace, QString* out_reason = nullptr);

  enum class MissingCurveChoice { kRemove, kCancel };

  // Modal. A long `names` list is truncated. `import_failures` ("path: why" per source the import could not bring
  // back) tells the user why the curves are missing.
  [[nodiscard]] MissingCurveChoice promptMissingCurves(
      const QStringList& names, const QStringList& import_failures = {});
  // The active batch's failed sources as "path: why" lines (empty without a batch).
  [[nodiscard]] QStringList layoutImportFailureLines() const;

  // <previouslyLoaded_Datafiles>: paths relative to `layout_dir` when at or beneath it, absolute otherwise. A null
  // element when no source is recorded.
  [[nodiscard]] QDomElement appendDataSourceElement(QDomDocument& doc, const QDir& layout_dir) const;

  // The per-<fileInfo> Source Timeline state, applied in two passes over one resolution. DatasetIds are
  // re-minted each session, so resolveLayoutTimelineTracks matches each entry to a live dataset by source path
  // (empty for generic layouts and those without timeline attributes). applyLayoutTimelineOffsets runs before
  // the processor replay, so all_datasets generators see the settled frame, and returns whether an offset moved;
  // applyLayoutTimelineOrder runs when the chrome is restored. applyTimelineStateFromLayout runs all three.
  // `streams` (a layout's stream tracks) take the same first-wins dedupe as the files, so two records bound to
  // one session never shift it twice or draw it in two slots.
  [[nodiscard]] LayoutTimelinePlan resolveLayoutTimelineTracks(
      const QList<layout_xml::DataSourceRef>& sources, const LayoutTimelinePlan& streams = {}) const;
  bool applyLayoutTimelineOffsets(const LayoutTimelinePlan& plan);
  void applyLayoutTimelineOrder(const LayoutTimelinePlan& plan);
  bool applyTimelineStateFromLayout(const QList<layout_xml::DataSourceRef>& sources);

  // <source_timeline zoom scroll_left_ns name_column_width snap/>: view chrome only; per-source offsets and order
  // ride <fileInfo>. Always emitted.
  [[nodiscard]] QDomElement saveSourceTimelineViewState(QDomDocument& doc) const;

  // Zoom, then scroll, then name-column width, then snap; a missing attribute keeps its value. Must run after the
  // datasets reload, or zoom and scroll map onto the old scene.
  void restoreSourceTimelineViewState(const QDomElement& element);

  // <right_panel_state visible width style splitter_sizes/>, always emitted; the caller appends it.
  [[nodiscard]] QDomElement saveRightPanelState(QDomDocument& doc) const;

  // Missing or mismatched values are ignored. Never writes QSettings: a layout must not change the user's
  // defaults.
  void restoreRightPanelState(const QDomElement& element);

  // <chrome_state main_splitter_sizes timeline_splitter_sizes/>. Panel open/closed state is not saved: it is a
  // QSettings preference, not document state.
  [[nodiscard]] QDomElement saveChromeState(QDomDocument& doc) const;

  // Missing or mismatched values are ignored; panel visibility is left as is.
  void restoreChromeState(const QDomElement& element);

  // Thin wrappers over pj_runtime's DerivedObjects, which owns the <data_processors> persistence. This one is
  // parse + checkDataProcessorDependencies + applyDataProcessors; `out_reason` is cleared on entry and set only
  // when a `kHistory` restore is refused.
  [[nodiscard]] bool restoreDataProcessors(
      const QDomElement& root, RestoreIntent intent = RestoreIntent::kReplace, QString* out_reason = nullptr);

  [[nodiscard]] DerivedObjects derivedObjects() const;
  // Reports each derived-recipe diagnostic as a "Layout" warning.
  void reportDerived(const std::vector<DerivedDiagnostic>& diagnostics);
  // A `kHistory` restore's read-only preflight: false (reported, reason in `out_reason`) when removing the
  // snapshot's non-exempt processors would strand an exempt transform or marker generator. Runs before
  // applyTimelineOffsets, so a refusal never has to undo a moved offset.
  [[nodiscard]] bool checkDataProcessorDependencies(const DerivedSnapshot& snapshot, QString* out_reason = nullptr);
  // Also reports its warnings and re-derives the Custom Topics list. An empty snapshot clears every recipe.
  [[nodiscard]] bool applyDataProcessors(DerivedSnapshot snapshot);

  // For a node-state or offset change: reuses the last transform rows unless the catalog changed. Costs per
  // derived row, never per catalog row.
  void refreshCustomTopicsPanel();
  // Coalesces to one refreshCustomTopicsPanel per event-loop turn (a node's state changes on every request).
  void scheduleCustomTopicsSync();
  // Mirrors the extension catalog's Custom Topics editor into the panel.
  void syncCustomTopicsEditor();
  // An on-demand recipe's object topic or a transform output. Only the Custom Topics editor's rows are deleted;
  // another owner's is refused with a warning (the backstop behind the panel's disabled action).
  void deleteCustomTopic(const QString& catalog_key);
  // Each recipe once, however many of its rows are among `catalog_keys`, then the transform outputs.
  void deleteCustomTopics(const QStringList& catalog_keys);
  // Confirms when other recipes read it (they stay, missing their input). A non-exempt removal is one undo step.
  void deleteOnDemandRecipe(const std::string& key);
  // With its dependents, after confirming.
  void deleteTransformOutput(const QString& catalog_key);
  // transformDeleteRefusal; a refusal is reported before returning false.
  [[nodiscard]] bool mayDeleteTransformWriting(TopicId output_topic);
  // Words a refusal of CustomTopicsPolicy.h as a warning; a row whose recipe is gone is re-synced instead.
  void reportDeleteRefusal(const CustomTopicDeleteRefusal& refusal);

  // The only owner of the bottom panel's height constraints: open strip, a minimum height so it cannot be
  // dragged to a clipped sliver; closed, fixed to the playback bar so the splitter cannot drag it open. Call it
  // from every site that changes the strip's visibility and from the chrome-metrics pass.
  void applyBottomPanelConstraints();

  // Puts the name column's separator under the start of the playback slider track. No-op until the playback bar
  // is laid out.
  void alignNameColumnToPlayback();

  // `scope` governs processors and plot tabs; the no-arg overload is the full layout-file form.
  [[nodiscard]] QDomDocument xmlSaveState(SnapshotScope scope) const;
  [[nodiscard]] QDomDocument xmlSaveState() const;

  bool xmlLoadState(const QDomDocument& state_document, RestoreIntent intent = RestoreIntent::kReplace);

  void pushInitialUndoState();

  // After a data removal: older snapshots reference the deleted curves, and replaying one would resurrect a
  // layout pointing at missing data.
  void resetUndoHistory();

  void pushUndoState(bool force_new_state = false);

  // Replaces the history tip after data growth or navigation, without adding an undo step.
  void hydrateCurrentUndoState(bool refresh_data_universe = true);
  void restoreHistoryState(const CapturedWorkspace& target, bool undo);
  void reconcileHistoryWithDataUniverse();

  // The raw storage identities that keep the history safe to replay; processor outputs are workspace state and
  // excluded.
  [[nodiscard]] QSet<QString> captureHistoryDataUniverse() const;

  void updateUndoRedoActions();

  // Also enables the width and style buttons, which need a bound plot.
  void bindEditorToPlot(PlotWidget* plot);
  [[nodiscard]] PlotWidget* firstPlotOfActiveTab() const;
  // The active tab's focused dock, else its first: refreshes the right panel on events without ADS focus (tab
  // switch, data clear) without mirroring a dock of a hidden tab.
  [[nodiscard]] DockWidget* activeFocusedDock() const;

 protected:
  void closeEvent(QCloseEvent* event) override;

  // The first show is when the splitters have real geometry: restores the left-panel width once (a later
  // --layout still overrides it) and sizes the bottom panel after the first layout pass (sizing in the
  // constructor does not stick).
  void showEvent(QShowEvent* event) override;

  void resizeEvent(QResizeEvent* event) override;

  // Custom frame only: edge resize from mouse events on this window or any descendant.
  bool eventFilter(QObject* watched, QEvent* event) override;

  // Local files dropped on the window load as if picked in Load Data; folders are not expanded. Plots and
  // placeholders ignore file drags, so Qt offers them to this ancestor; floating docks are separate windows and
  // take no file drops.
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dragMoveEvent(QDragMoveEvent* event) override;
  void dropEvent(QDropEvent* event) override;

  // Custom frame only: a 1-px edge from the PJ::theme:: tokens, since QSS cannot reach a top-level widget. None
  // while maximized or fullscreen.
  void paintEvent(QPaintEvent* event) override;

 private:
  // Presents `panel` (non-null) in place of the chart area, dismissing any panel already up. restoreCentralArea
  // tears it down; releaseCentralPanel returns it hidden, still a child of the stack, for "migrate to tab".
  // Neither reparents: the destination does that once, because every reparent re-polishes the whole widget tree.
  void presentPanel(QWidget* panel);
  void restoreCentralArea();
  QWidget* releaseCentralPanel();

  // Tears down the chart-area takeover, if any, before a pinned toolbox tab is focused or restored (the takeover
  // would hide it). A takeover with work in flight is folded into a background tab instead: teardown destroys
  // the plugin instance, which kills its job, and opening another panel must not cancel a download.
  void dismissTakeoverPanel();

  // Only when `owner` is the presenting toolbox's sessionOwner(): another panel's ingest must not move this one.
  // `owner` is compared by address, never dereferenced. `transient` marks a fold the host chose (not saved in
  // layouts). No-op once pinned.
  void foldTakeoverPanelIfOwnedBy(const void* owner, bool transient);
  // A plugin created a tab: when that plugin is the takeover, which would hide the tab, folds it into a
  // transient tab and makes `docker` current.
  void revealOwnedTabOverTakeover(const QString& plugin_id, PlotDocker* docker);

  // Null when there is no takeover or it is not a toolbox (the marketplace, the filter editor).
  [[nodiscard]] ToolboxPresenter* takeoverToolbox() const;

  // Runs a modal event loop: callers must re-validate any iterator or pointer held across it.
  bool confirmCancelRunningJob(const QString& label);
  // For the browser, which cannot run a modal loop without Asyncify: `on_confirmed` runs only on a cancel,
  // possibly after this returns, so it must re-find anything it acts on.
  void confirmCancelRunningJobAsync(const QString& label, std::function<void()> on_confirmed);

  // Go through the `host_work_in_flight_` / `stop_host_work_` test seams when set. A null host reads idle.
  [[nodiscard]] bool hostHasWorkInFlight(ToolboxRuntimeHost* host) const;
  void stopHostWork(ToolboxRuntimeHost* host);

  // Content and plugin connections survive every presentation change. `apply_chrome(pinned)` switches the banner
  // between tab chrome (dock, float, close) and takeover chrome (tab, float, close).
  struct WrappedToolboxPanel {
    QWidget* container = nullptr;
    std::function<void(bool pinned)> apply_chrome;
  };

  // An absent migration callback hides its button. Close only calls on_close: the close policy (busy fold, tab
  // confirmation) is the caller's.
  struct ToolboxChromeHooks {
    std::function<void()> on_close;
    std::function<void()> on_migrate;
    std::function<void()> on_float;
    std::function<void()> on_dock;
  };

  // Floating uses its own title bar. `persist_key` is the drawer width's QSettings key (empty: not persisted).
  WrappedToolboxPanel wrapToolboxPanel(
      QWidget* content, const QString& title, ToolboxChromeHooks hooks, const QString& persist_key = {});

  // The host services and plugin handle a launched toolbox binds to; null after `report_error`. The wiring it
  // shares with HeadlessDescriptorProviderSession lives in ToolboxHostWiring.h.
  std::shared_ptr<ToolboxPanelSession> buildToolboxPanelSession(
      const RuntimeToolboxPlugin& toolbox, const QString& plugin_id, const QString& source,
      const std::function<void(const QString&, const QString&)>& report_error);
  // Started on a free 127.0.0.1 port when none runs (the browser's does not listen). Null when it cannot
  // listen, reported once per run of failures.
  McpServer* ensureMcpServer();

  // One presenter per plugin id. registerToolbox requires the id absent; teardownToolbox extracts the entry
  // before tearing down, so a callback landing mid-teardown finds no presenter instead of a half-dead one.
  [[nodiscard]] ToolboxPresenter* findToolbox(const QString& plugin_id) const;
  // Deferred callbacks look the presenter up by id through this; they never capture it.
  template <typename Fn>
  void withToolbox(const QString& plugin_id, Fn&& fn) {
    if (ToolboxPresenter* live = findToolbox(plugin_id)) {
      fn(*live);
    }
  }
  [[nodiscard]] ToolboxPresenter* toolboxForContainer(const QWidget* container) const;
  ToolboxPresenter& registerToolbox(const QString& plugin_id, ToolboxPresenter::Spec spec);
  void teardownToolbox(const QString& plugin_id, ToolboxPresenter::Teardown mode);
  // Synchronously, from ~MainWindow while the session, tab strip and services are still alive.
  void teardownAllToolboxes();

  // Pinned toolbox tabs in a layout file, not in the undo snapshot:
  // <pinned_toolboxes><toolbox plugin_id="...">config-json</toolbox>...</>. Transient folds are skipped; a
  // floating window is saved as a pinned tab. Restore closes every pinned tab, floating window and a takeover the
  // layout names, then relaunches (a missing plugin is diagnosed and dropped). False when a busy panel kept the
  // live set: the user declined the cancel (kPrompt), or kRetainAndDiagnose kept it unprompted and diagnosed it.
  [[nodiscard]] QDomElement savePinnedToolboxes(QDomDocument& doc) const;
  bool restorePinnedToolboxes(const QDomElement& root, MissingCurvePolicy policy);

  // The one commit boundary of a layout open, after every abort and rollback path: replaces the pinned toolbox
  // set and re-baselines undo. Both restore legs end here. restoreChromeAndPanels must not take commit-only
  // steps: it also runs while the restore can still abort.
  void commitRestoredLayout(const QDomDocument& doc, MissingCurvePolicy policy);

  // An empty, wired object dock of `kind` ("scene3d", "scene2d", "state_transitions", "CustomWidget"); null for
  // an unknown kind.
  IDataWidget* makeSceneDock(const QString& kind, QWidget* parent);

  // Also seeds the playhead: currentTimeChanged fires only on changes.
  IDataWidget* makeSeededEmptyObjectDock(const QString& kind, QWidget* parent);

  // For plugin panels' `scene_view` frames, attached through PluginSceneViewsController. Empty without a scene
  // family in the build, so the panel announces no scene-view capability.
  EmbeddedViewHost makeEmbeddedViewHost();
  // Not ADS docks, so forEachDock misses them: the tracker broadcasts tick them through tickEmbeddedViews.
  QList<QPointer<SceneDockWidget>> embedded_scene_views_;
  // After a catalog change: re-attaches each topic whose layer is gone under its new id, or records it as
  // evicted (0) so a detach skips it.
  void reattachEmbeddedTopics(SceneDockWidget& view);
  void tickEmbeddedViews(double display_seconds, bool visible_only);

  Ui::MainWindow* ui_;
  QtDiagnosticBridge* diagnostic_bridge_ = nullptr;
  DiagnosticHistory* diagnostic_history_ = nullptr;
  DiagnosticsDialog* diagnostics_dialog_ = nullptr;
  ToastManager* toast_manager_ = nullptr;
  UpdateChecker* update_checker_ = nullptr;
  QString installation_;
  TelemetryPing* telemetry_ping_ = nullptr;
  // Rebound on each checkForUpdates(), so each check carries its own interactivity.
  QMetaObject::Connection update_available_conn_;
  QMetaObject::Connection up_to_date_conn_;
  QMetaObject::Connection check_failed_conn_;
  // Separate from the Marketplace's, so neither aborts the other's fetch.
  RegistryManager* update_scan_registry_ = nullptr;
#ifndef PJ_TARGET_WASM
  // Backs "pj.feature_catalog.v1". Declared before session_ and toolboxes_ so it outlives every bound plugin.
  std::unique_ptr<FeatureCatalogHost> feature_catalog_;
#endif
  // Created on first need. ~MainWindow resets it after the toolboxes (whose endpoints revoke their tokens) and
  // before the services its agent sessions point into.
  std::unique_ptr<McpServer> mcp_server_;
  // A failed listen was reported and no start has succeeded since.
  bool mcp_listen_failure_reported_ = false;
  // Status-bar agent indicator, shown while an MCP client is active or a job runs; opens mcp_jobs_dialog_.
  QToolButton* mcp_agent_button_ = nullptr;
  QString mcp_active_client_;
  // Created on first use, a child of this.
  McpJobsDialog* mcp_jobs_dialog_ = nullptr;
  // Refreshes mcp_jobs_dialog_ while it is open; a child of it.
  QTimer* mcp_jobs_refresh_ = nullptr;
  // Brings mcp_agent_button_ and an open mcp_jobs_dialog_ in line with the server's state.
  void refreshMcpIndicator();
  void showMcpJobs();
  // A child of mcp_server_, so it dies with it. While set, the browser skips its welcome dialog.
  QPointer<QObject> mcp_relay_link_;
  QAction* undo_action_ = nullptr;
  QAction* redo_action_ = nullptr;
  // Shared by every Scene3DDockWidget (setSettings) for the URDF package resolver; outlives the docks.
  std::unique_ptr<QSettings> app_settings_;
  std::unique_ptr<AppSession> session_;
  std::unique_ptr<PendingDisplayBinder> pending_binder_;
  // Declared after pending_binder_, which it forwards placeholder scalar drops to.
  std::unique_ptr<TopicDemandController> topic_demand_controller_;
  // pj.plot_tabs.v1 / pj.viewport.v1 for the toolbox runtime hosts.
  std::unique_ptr<PluginPlotTabsController> plugin_plot_tabs_;
  std::unique_ptr<IngestProgressController> ingest_progress_controller_;
  // Declared after session_ so it dies first: it holds a reference into session_.
#ifdef PJ_WITH_SCENE3D
  std::unique_ptr<PJ::TransformService> transform_service_;
  // The 3D docks' shared "last manual fixed frame" preference.
  std::unique_ptr<pj::scene3d::FixedFrameMemory> fixed_frame_memory_;
#endif
  std::unique_ptr<FileLoader> file_loader_;
  std::unique_ptr<StreamingSourceManager> streaming_manager_;
  // Declared after streaming_manager_, so the runtime hosts holding its taps outlive it; closeEvent and
  // ~MainWindow still stop() it explicitly before any widget or host is gone. Created even where isSupported()
  // is false: only the UI affordances go away there.
  std::unique_ptr<RecordingService> recording_service_;
#ifndef __EMSCRIPTEN__
  // Built once from Preferences: a changed cache folder or budget applies on the next launch, because armed
  // captures hold the store they were opened against. The store comes first: the service references it.
  std::unique_ptr<SourceCacheStore> source_cache_store_;
  std::unique_ptr<SourceCaptureService> source_capture_service_;
#endif
  // Behind the "m:ss | N.N MB" status label; restarted on every start().
  QElapsedTimer recording_elapsed_;
  // Caps the per-cursor-move fan-out (playback ticks, scrubbing, seeks) at kTrackerBroadcastIntervalMs.
  // Structural seeds (load, restore, undo) call broadcastTrackerTime() unthrottled, so a new dock is never blank
  // waiting for the trailing edge, which uses pending_tracker_time_.
  std::unique_ptr<CoalescingTrigger> tracker_broadcast_trigger_;
  double pending_tracker_time_ = 0.0;
  // A QObject child of this window.
  SourceTimelineController* source_timeline_controller_ = nullptr;
  // Owned by timelineStrip.
  Timeline* source_timeline_ = nullptr;
  // The user's name-column width in px; 0 uses the playback-aligned floor.
  int timeline_name_column_width_ = 0;
  // 0 = none. Scopes the playback range to the stream's data, so unrelated file timestamps do not stretch it.
  DatasetId active_streaming_dataset_id_ = 0;
  // Set by the first drop of a streaming topic into a view, which seeds the playback range and playhead once per
  // session: subscribing in the source dialog does not move the slider, and a later drop cannot re-snap a
  // paused, scrubbed-back cursor. While set, live ingest follows the live edge until the user pauses.
  bool streaming_playback_seeded_ = false;
  bool plugin_failure_toast_shown_ = false;
  // While non-null, every "Layout" diagnostic is also appended here, so a failed restore can say why.
  QStringList* layout_issue_capture_ = nullptr;
  std::unique_ptr<Theme> theme_;
  TitleBar* title_bar_ = nullptr;
  // Fixed for the window's lifetime: the native window is created frameless or not.
  const bool custom_frame_ = usesCustomFrame();
  // Owned by title_bar_. Shown after a delay so quick loads do not flash it.
  IngestProgressWidget* ingest_progress_ = nullptr;
  QTimer* ingest_show_timer_ = nullptr;
  // Restarted, not recreated, at each drain site, so overlapping finishes coalesce into one hide.
  QTimer* ingest_hide_timer_ = nullptr;
  // Prevents stacking stop confirmations from repeated clicks.
  QPointer<MessageBox> ingest_stop_dialog_;
  // Stop routing for toolbox imports only; the ingest lifecycle lives in SessionManager. One entry per import
  // dataset between on_ingest_started and on_ingest_finished (host teardown fires finished for anything left
  // open). `owner` weak-guards the panel session running the import; `host` is dereferenced only after that
  // check, so a closed panel is never stop-routed into freed memory. toolbox_ingest_label_ / _dataset_ name
  // the last-started interactive import, which a strip adopt shows; cleared when it ends.
  struct ToolboxIngestRef {
    IngestToken token;
    std::weak_ptr<void> owner;
    PJ::ToolboxRuntimeHost* host = nullptr;
  };
  QHash<PJ::DatasetId, ToolboxIngestRef> toolbox_active_imports_;
  QString toolbox_ingest_label_;
  DatasetId toolbox_ingest_dataset_ = 0;

  // Whose title and progress the strip shows, so Stop reaches exactly that producer and an ending producer
  // hands the strip over. The rule:
  //   * a file load wins while FileLoader is busy;
  //   * a newly begun toolbox ingest takes the strip (last-started wins), except that the layout batch's never
  //     displaces a displayed interactive one;
  //   * when the displayed ingest ends (or its batch dies), the strip goes to the last-started interactive
  //     ingest if still active, else the lowest surviving interactive DatasetId (ids are monotonic, so the
  //     first created), else the batch's; with no survivor the linger-hide runs.
  enum class IngestStripOwnerKind { kNone, kFile, kInteractiveToolbox, kLayoutBatch };
  IngestStripOwnerKind ingest_strip_owner_kind_ = IngestStripOwnerKind::kNone;
  DatasetId ingest_strip_owner_dataset_ = 0;  ///< set for the two ingest kinds only

  // Up only while a handed-over batch lives; torn down before the batch is destroyed. The dataset is the batch's
  // begun-and-not-ended ingest (0 = none), matched exactly against its activeImportDataset().
  std::vector<QMetaObject::Connection> layout_batch_ingest_conns_;
  DatasetId layout_batch_strip_dataset_ = 0;
  QMenu* installed_extensions_menu_ = nullptr;
  // The parents findChild queries for the exclusive width and style buttons.
  QWidget* curve_width_header_ = nullptr;
  QWidget* curve_style_header_ = nullptr;
  // Exclusivity only; the groups own no widgets.
  QButtonGroup* style_button_group_ = nullptr;
  QButtonGroup* width_button_group_ = nullptr;

  // Re-tinted by applyIcons().
  QAction* action_load_layout_ = nullptr;
  QAction* action_save_layout_ = nullptr;
  QAction* action_open_data_ = nullptr;
  QAction* action_preferences_ = nullptr;
  // Byte-level layout loads and their source replay; desktop file loads never use it.
  struct BrowserLayoutRuntime;
  std::unique_ptr<BrowserLayoutRuntime> browser_layout_runtime_;
  QAction* action_save_source_layout_ = nullptr;  // browser build only
#ifdef PJ_TARGET_WASM
  // Created once the preset index lists a preset; hidden between uses.
  WelcomePresetsDialog* presets_dialog_ = nullptr;
#endif
  // Until finishBrowserLayoutLoad: layout actions disabled, a second byte-level load refused.
  bool layout_selection_pending_ = false;
  // Empty on the desktop, where a configuration naming a deployment variable fails with "missing deployment
  // context".
  QUrl deployment_page_url_;
  std::deque<CapturedWorkspace> undo_states_;
  std::deque<CapturedWorkspace> redo_states_;
  QSet<QString> history_data_universe_;
  QElapsedTimer undo_timer_;
  bool applying_state_ = false;
  bool progressive_layout_in_flight_ = false;
  // The binder's itemsAdded retry channel. Torn down with the restore, except after a kRetainAndDiagnose drain,
  // which leaves it up so retained intents bind when their data arrives, until the next restore or
  // resetLayoutImportBatch.
  QMetaObject::Connection pending_items_added_conn_;
  // One reason the progressive drain must wait, with the connection that re-runs the gate when it may have
  // cleared: FileLoader::isBusy() on queueDrained, and the batch's !isFinished() on finished.
  struct RestoreWaiter {
    std::function<bool()> pending;
    QMetaObject::Connection conn;
  };
  std::vector<RestoreWaiter> restore_waiters_;
  template <typename Sender, typename Signal>
  void addRestoreWaiter(std::function<bool()> pending, Sender* sender, Signal signal) {
    restore_waiters_.push_back(
        RestoreWaiter{
            .pending = std::move(pending),
            .conn = connect(sender, signal, this, &MainWindow::maybeSettleProgressiveRestore),
        });
  }
  void clearRestoreWaiters();
  // Captured at beginProgressiveLayoutRestore from the restore's interactivity, never read late from the batch.
  MissingCurvePolicy progressive_missing_curve_policy_ = MissingCurvePolicy::kPrompt;
  bool pending_binding_rebuild_scheduled_ = false;
  // Trailing edge for scene docks' workspaceChanged: scrubbers emit per drag tick, and each undo push serializes
  // the whole workspace.
  QTimer scene_undo_debounce_;
  // A progressive restore's timeline state, applied at the drain once the reloaded datasets' paths are
  // registered. Empty outside a progressive restore.
  QList<layout_xml::DataSourceRef> pending_timeline_sources_;
  // Resolved with the files' tracks at begin and at the drain; cleared when the restore is cancelled or
  // superseded.
  LayoutTimelinePlan progressive_stream_tracks_;
  // Kept from begin to the drain, where the processors replay.
  QDomDocument progressive_layout_doc_;
  std::optional<CapturedWorkspace> progressive_previous_workspace_;
  std::optional<PendingSourceReplacement> pending_source_replacement_;
  // Non-null from a restore with materialize-bearing sources until its drain settles. Declared after session_
  // and file_loader_: its destructor reaches both.
  std::unique_ptr<LayoutImportBatch> layout_import_batch_;
  // Set by the batch's begin_workspace_checkpoint hook and consumed at beginProgressiveLayoutRestore, so both
  // use one capture.
  std::shared_ptr<const CapturedWorkspace> batch_workspace_checkpoint_;
  // From enableAutoplay() to the first range-driven playback start.
  bool autoplay_pending_ = false;
  // Later shows must not clobber a user or layout adjustment.
  bool left_splitter_restored_ = false;
  bool bottom_panel_sized_ = false;
  CurveEditor* curve_editor_ = nullptr;

  // onDockFocused() picks the page.
  QStackedWidget* right_panel_stack_ = nullptr;
  QWidget* plot_config_page_ = nullptr;
#ifdef PJ_WITH_SCENE2D
  QWidget* scene2d_config_page_ = nullptr;
#endif
#ifdef PJ_WITH_SCENE3D
  QWidget* scene3d_config_page_ = nullptr;
#endif
#ifdef PJ_WITH_SCENE3D
  class Scene3DConfigPanel* scene3d_config_panel_ = nullptr;
#endif
#ifdef PJ_WITH_SCENE2D
  class Scene2DConfigPanel* scene2d_config_panel_ = nullptr;
#endif
  // For a dock still showing its placeholder.
  QWidget* empty_dock_page_ = nullptr;
  // Owned by right_panel_stack_. ~MainWindow clears its active-widgets record once the docks are gone.
  CustomWidgetController* custom_widgets_ = nullptr;

  // The chart-area takeover, at most one. current_panel_engine_ is its PanelEngine when it is a toolbox (set by
  // ToolboxPresenter), a non-owning reference.
  QWidget* current_panel_ = nullptr;
  PanelEngine* current_panel_engine_ = nullptr;
  // A second open re-activates the live marketplace instead of building another. It may have migrated to a tab
  // with another panel now central: `current_panel_ == marketplace_container_` tells the two apart.
  QPointer<MarketplaceWindow> marketplace_panel_;
  QPointer<QWidget> marketplace_container_;

  // Each presenter owns its plugin session. ~MainWindow tears them down first; declared after session_ so they
  // would die before the AppSession they write into anyway. unique_ptr: presenters must not move.
  std::map<QString, std::unique_ptr<ToolboxPresenter>> toolboxes_;

  // Test seams for the fold rules (an ingest context needs a real parser plugin); unset in production.
  std::function<bool(QString)> confirm_running_job_;
  std::function<bool(ToolboxRuntimeHost*)> host_work_in_flight_;
  std::function<void(ToolboxRuntimeHost*)> stop_host_work_;
  // The plot an open Filter Editor's preview mirrors; null otherwise.
  QPointer<PlotWidget> filter_editor_origin_;

  // Built in buildGlobalToolbar(), not in the .ui.
  QToolButton* button_link_ = nullptr;
  SvgButton* button_time_tracker_ = nullptr;
  QToolButton* button_show_point_ = nullptr;
  QToolButton* button_grid_ = nullptr;
  QToolButton* button_zoom_out_ = nullptr;
  QToolButton* button_ratio_ = nullptr;
  QToolButton* button_dots_ = nullptr;
  QToolButton* button_reference_point_ = nullptr;
  // Left-click cycles the corners, right-click shows or hides. Checked while shown; the icon shows the active
  // corner, or the one the next show restores.
  SvgButton* button_legend_ = nullptr;
  // The corner the next show restores.
  LegendStatus previous_legend_corner_ = LegendStatus::kTopRight;

  // QSettings preferences, never in layouts or undo snapshots; show_points_ also reaches 2D image viewers.
  LegendStatus legend_status_ = LegendStatus::kTopRight;
  bool show_points_ = false;
  bool activate_grid_ = false;
  // Derived by applyTimeLabelFormat().
  TimeLabelFormat time_label_format_ = TimeLabelFormat::kSeconds;
  bool dots_ = false;

  bool custom_topics_sync_pending_ = false;
  int custom_topics_syncs_ = 0;            ///< refreshCustomTopicsPanel runs, read by the coalescing test
  int custom_topics_transform_scans_ = 0;  ///< transform-row rebuilds, read by the refresh test
  /// One Custom Topics row of a transform output, as the last rebuild matched it to the catalog.
  struct TransformTopicRow {
    QString key;
    QString owner;
  };
  std::vector<TransformTopicRow> custom_topic_transform_rows_;
  /// The catalog changed, or a sync asked for it, since custom_topic_transform_rows_ were built.
  bool custom_topic_transform_rows_stale_ = true;

  // Loaded before any child widget is built, so each widget's first applyIcons() uses it.
  ChromeMetrics chrome_metrics_;
  CurveTracker::Parameter tracker_info_ = CurveTracker::kValue;
  // Session-only. An absolute instant, not a display coordinate, so the line holds its instant when the display
  // offset shifts ("Use time offset", or a load that lowers a dataset's earliest stamp).
  std::optional<Timepoint> reference_instant_;
  bool keep_ratio_ = true;
};

}  // namespace PJ
