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
#include "FileLoader.h"  // DialogPolicy, LoadRequestId
#include "LayoutXml.h"
#include "LoadInput.h"
#include "ToolboxPresenter.h"
#include "pj_base/builtin/builtin_object.hpp"  // sdk::BuiltinObjectType — onPlaceholderTopicDropped's slot parameter
#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/expected.hpp"       // PJ::Status — the plot-tab and viewport host services report through it
#include "pj_base/plugin_data_api.h"  // PJ_service_registry_t — toolboxSessionServices returns it
#include "pj_base/time.hpp"           // PJ::Timepoint — the frame-invariant absolute instant the reference line stores
#include "pj_base/types.hpp"
#include "pj_plotting/CurveTracker.h"
#include "pj_plugins/host_qt/embedded_view_host.hpp"  // makeEmbeddedViewHost returns it by value
#include "pj_runtime/CurveDescriptor.h"               // openFilterEditor takes std::vector<CurveDescriptor> by value
#include "pj_runtime/HistoryScope.h"
#include "pj_runtime/RecordingService.h"  // CaptureResult / SourceRecordingStart — recording slot parameters
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

// Legend corner placement. Four corner buttons in the right toolbar act
// as an exclusive group: click sets the position, click the active one
// again hides the legend. Stored as int in QSettings ("MainWindow.legendStatus").
enum class LegendStatus {
  kBottomRight = 0,
  kBottomLeft = 1,
  kTopRight = 2,
  kTopLeft = 3,
  kHidden = 4,
};

class MainWindow : public QMainWindow {
  Q_OBJECT
  // Headless layout round-trip test reaches the private data-source save/apply
  // seam and the Source Timeline controller through this peer.
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
  // Creates the main window using the default extension directory.
  explicit MainWindow(QWidget* parent = nullptr);

  // Creates the main window using an explicit extension directory.
  explicit MainWindow(QString extensions_dir, QWidget* parent = nullptr);

  // Releases UI resources and the application session.
  ~MainWindow() override;

  // Populates the session with generated data for smoke testing.
  [[nodiscard]] bool populateTestData();

#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  // Acceptance-only canonical layout fixture: serializes the live workspace
  // through the same state serializers used by production save, but never
  // writes a path.
  [[nodiscard]] QByteArray wasmProbeLayoutBytes() const;
  [[nodiscard]] qsizetype wasmProbeDatasetCount() const;
  [[nodiscard]] std::size_t wasmProbeResidentPoolCapacity() const;
  void wasmProbeReportFilterResult() const;
  void wasmProbeReportToolboxTransformResult() const;
  // Reports streaming controls, catalog/tree state, playback range, and the
  // first plotted curve without triggering any application action.
  void wasmProbeReportStreamingState() const;
  // Supersedes an in-flight source-replay picker without opening another host
  // picker, making late-callback rejection deterministic in browser tests.
  void wasmProbeSupersedeBrowserReplayPicker();
  // Installs the plot readback shortcuts (F8, F9, F10); defined with the other
  // acceptance probes in tests/wasm_acceptance_probes.inc.
  void installWasmPlotAcceptanceProbes();
#endif

  // Loads `path` on startup and auto-reloads its data source(s) without prompting
  // (the --layout CLI option). Safe no-op if the layout binds to no source.
  void loadLayoutAtStartup(const QString& path);

  // Loads explicit files before applying the optional layout against their data.
  // A failed/cancelled file aborts the layout and settles it false. Dialogs are
  // allowed only for attended file loads; layout replay never prompts here.
  void loadAtStartup(const QStringList& data_files, const QString& layout_path, bool interactive);

  // Startup step, before any layout restore: see CustomWidgetController::reviewUncleanShutdown. A
  // non-interactive run never shows the question.
  void reviewUncleanShutdown(bool interactive);

  // Builds a Custom Widget dock for widget `type` of plugin `plugin_id` and adopts it into the
  // empty `dock`. The dock is adopted even when the widget cannot be created (library missing,
  // Qt build mismatch): it then shows the reason and keeps the choice for the layout. Returns
  // whether the plugin widget was created. The widget picker calls this on a click.
  bool createCustomWidgetDock(DockWidget* dock, const QString& plugin_id, const QString& type);

  // Opens the layout at `url` (http/https) as a preset, with no picker: its
  // <fileInfo> sources are downloaded from beside it one at a time, staged and
  // checked against their fingerprints, then imported all or nothing — any
  // failure or cancel removes what this load imported and restores the previous
  // workspace. Scene3D resources resolve against the same location. A layout
  // load already pending makes this a no-op logged with reason=busy. Every call
  // ends in exactly one layoutUrlLoadFinished.
  void loadLayoutFromUrl(const QUrl& url);
  // Stops the loadLayoutFromUrl in flight, if any: its download is dropped, its
  // own queued and running imports are cancelled (discarding their data) and
  // the workspace comes back once they wind down. Other loads are left alone.
  void cancelLayoutFromUrl();

  // The page this build was opened from: fills a replayed stream's deployment variables
  // (${deployment.*}). The browser build sets it at startup; the desktop app leaves it empty.
  void setDeploymentPageUrl(const QUrl& page_url) {
    deployment_page_url_ = page_url;
  }

#ifdef PJ_TARGET_WASM
  // Looks for the presets the page at `page_url` publishes (presets/index.json
  // beside it) and, once found, offers them in the welcome dialog and under
  // File → Presets, or opens the one a ?preset=<id> deep link names. A page
  // without a usable index starts as if this were never called. Call once,
  // from the event loop, after the window is shown.
  void startBrowserPresets(const QUrl& page_url);
#endif

  // Arms the --autoplay CLI option: begins looping playback the first time a data
  // source provides a non-empty time range (so it works for both the synchronous
  // --test-data path and the async --layout load). Starts once — the user's later
  // pause/seek is respected. No-op if no data ever loads. Call before loading data.
  void enableAutoplay();

  [[nodiscard]] TitleBar* titleBar() const {
    return title_bar_;
  }

  // Diagnostic sink that fans out to the title-bar bell + popup via the
  // DiagnosticHistory service. Use this from tools / CLI / dev feeds to
  // emit through the same pipeline plugins use.
  [[nodiscard]] DiagnosticSink diagnosticSink() const;

  // The Qt-side bridge every in-process diagnostic re-emits through (queued).
  // Exposed for additional subscribers — the --dump-diagnostics collector —
  // beside the DiagnosticHistory bell service; never a mutation surface.
  [[nodiscard]] QtDiagnosticBridge* diagnosticBridge() const {
    return diagnostic_bridge_;
  }

  // Shows a transient bottom-right toast. The message may contain rich text
  // (an `<a href>` opens in the system browser). Lazily creates the toast
  // manager on first use.
  void showToast(const QString& message, const QPixmap& icon = QPixmap());

  // Kicks off a one-shot GitHub release check (lazily creating the
  // UpdateChecker). A newer release always pops a toast. `interactive`
  // distinguishes the two entry points: false = automatic startup check, which
  // stays silent unless there's an update (no "up to date" / no error noise);
  // true = the manual Help ▸ Check for Updates action, which also toasts the
  // "you're up to date" and "couldn't check" outcomes.
  void checkForUpdates(bool interactive);

  // One-shot startup scan for extension/plugin updates (distinct from the app
  // release check above): fetches the marketplace registry and compares each
  // entry against the installed version via ExtensionManager::hasUpdate. When
  // any installed extension has a newer registry version, reveals the magenta
  // "Update" button in the title bar (tooltip carries the count); clicking it
  // opens the Marketplace. Fully silent on network/parse failure — no button,
  // no toast. Lazily creates its own RegistryManager on first call.
  void checkExtensionUpdates();

#ifndef PJ_TARGET_WASM
  // main.cpp's gate skipped checkExtensionUpdates() for this launch (the
  // --screenshot flag, or "check for updates on startup" turned off). Only
  // tells the feature catalog why no registry is known; fetches nothing.
  void noteRegistryScanSkipped();

  // Reports every fetch `manager` starts from now on to the feature catalog,
  // which reuses them instead of fetching on its own.
  void trackRegistryFetches(RegistryManager* manager);
#endif

  // Sends the anonymous daily-user ping (lazily creating the TelemetryPing).
  // Fully silent — no user-facing notification. `installation` is the
  // PJ_INSTALLATION build stamp, passed by main.cpp (the only pj_version.h
  // consumer). The opt-out gate lives at the call site, not here.
  void sendTelemetryPing(const QString& installation);

  // The PJ_INSTALLATION build stamp, passed by main.cpp. Decides where
  // checkForUpdates() looks for a newer release and how the toast offers it.
  void setInstallation(const QString& installation);

  // Presents the embedded external-process view in the central area (via
  // presentPanel) and restores the chart when the session ends. Idempotent:
  // a no-op if a panel is already presented.
  void openEmbeddedConsole();

  // Global Chrome metrics for toolbar/panel buttons. Persisted to
  // QSettings (ui/icon_size, ui/icon_padding, ui/layout_padding,
  // ui/layout_spacing) and broadcast via chromeMetricsChanged so each
  // icon-bearing widget can re-render.
  [[nodiscard]] const ChromeMetrics& chromeMetrics() const {
    return chrome_metrics_;
  }

  // Persist the current chrome metrics to QSettings in one write. The setters
  // below only apply live (so a Preferences sizing-scrubber drag previews
  // without thrashing the .ini); PreferencesDialog calls this on OK to commit.
  void persistChromeMetrics() const;

  // Plugin search-folder configuration for the Preferences "Plugins" page;
  // delegates to the extension catalog. Custom-folder edits apply on next launch.
  [[nodiscard]] QStringList customPluginFolders() const;
  void setCustomPluginFolders(const QStringList& folders);
  [[nodiscard]] QStringList builtinPluginFolders() const;

  // Where launchToolbox presents the toolbox: the default chart-area
  // takeover, or directly pinned as a central tab (the layout-restore path
  // of the "migrate to tab" gesture). Lives in the public: section — moc
  // rejects type declarations inside slots/signals sections.
  enum class ToolboxLaunchTarget { kTakeover, kPinnedTab };

  // Marketplace registry URL — the single owner of the Marketplace/registryUrl
  // settings key. registryUrlSetting() is the raw persisted value, "" when
  // unset (the built-in default applies); the setter persists immediately (""
  // clears the override); effectiveRegistryUrl() is the validated read the
  // marketplace opens with (invalid stored values fall back to the default),
  // and isValidRegistryUrl() is the one validation rule (http/https/file) the
  // Preferences editor and the read path share.
  [[nodiscard]] QString registryUrlSetting() const;
  void setRegistryUrlSetting(const QString& url);
  [[nodiscard]] QUrl effectiveRegistryUrl() const;
  [[nodiscard]] static bool isValidRegistryUrl(const QString& url);

  // The built-in registry URL — the Preferences editor's placeholder.
  [[nodiscard]] static QString defaultRegistryUrl();

  // Preferences key (bool) that opts in to PlotJuggler's own window frame
  // instead of the system's. Read once at construction, so a change applies
  // on the next launch.
  static constexpr QLatin1StringView kCustomTitleBarKey{"Preferences::custom_title_bar"};

  // Preferences key (bool): open/save through the platform's file picker
  // (FileDialog::setUseSystemDialog). Applied at construction and on
  // Preferences OK.
  static constexpr QLatin1StringView kSystemFileDialogKey{"Preferences::system_file_dialog"};

  // Preferences key (bool): wall-clock time labels show UTC instead of the
  // system time zone. Applied at construction and on Preferences OK.
  static constexpr QLatin1StringView kClockUtcKey{"Preferences::clock_utc"};

  // Key (bool) of the date-time toggle (buttonUseDateTime): time labels read as
  // a clock instead of seconds. The name is PJ3's.
  static constexpr QLatin1StringView kDateTimeDisplayKey{"MainWindow.dateTimeDisplay"};

  // Whether the user gets a choice of frame on this platform. False on macOS
  // (the platform's window controls are not ours to imitate) and in the
  // browser (see usesCustomFrame); the Preferences toggle is hidden there.
  [[nodiscard]] static bool customFrameSelectable();

  // Whether a new MainWindow draws its own frameless frame: the saved
  // preference where selectable, always in the browser, never elsewhere.
  [[nodiscard]] static bool usesCustomFrame();

  // --- Agent control: the services and actions the MCP server (src/mcp/) drives. ---

  // Before deleting `removed_topics`, find every derived series (transform) that
  // depends on them transitively, warn the user, and — if confirmed — remove those
  // transforms and their Custom Topics entries, then run `proceed`. `proceed` runs
  // at once when nothing depends on the removed series, and never on cancel or a
  // failed removal. It may run after this returns (the browser prompt is
  // non-blocking), so it must re-validate anything that could change meanwhile.
  // `still_allowed`, when given, is asked once the prompt is accepted and before
  // anything is removed: false abandons the whole delete (it reports why itself).
  // `cancelled`, when given, runs instead of `proceed` when the delete is abandoned
  // (the user cancelled, `still_allowed` refused, or removing the processors failed),
  // so an asynchronous caller always hears exactly one outcome. `on_dialog`, when given,
  // is told when the prompt opens and when it is gone.
  void confirmAndRemoveDependentTransforms(
      const std::vector<TopicId>& removed_topics, std::function<void()> proceed,
      std::function<bool()> still_allowed = {}, std::function<void()> cancelled = {},
      dialog_presenter::DialogWatch on_dialog = {});

  // Erases `dataset_ids` with no prompt (the caller has confirmed, and has already
  // removed the derived series depending on them), then re-seeds playback (unless a
  // stream is live) and resets undo once for the batch.
  void removeDatasets(const QList<DatasetId>& dataset_ids);

  // Reloads `dataset_id` from its file with the plugin + config recorded when that
  // file was loaded. `policy` overrides the dialog policy the interactive reload picks
  // (skip the dialog when both are known); `still_allowed` becomes the load's
  // LoadHints::still_allowed. Returns the load ticket, 0 when the dataset is not
  // file-backed or the load was rejected.
  LoadRequestId reloadDataset(
      DatasetId dataset_id, std::optional<DialogPolicy> policy = std::nullopt,
      std::function<bool()> still_allowed = {});

  // Re-derives the Custom Topics rows from the live transform recipes and the
  // persistent on-demand recipes (one flat row per object output, with its node's
  // state) and hands the whole set to the panel.
  //
  // The panel cannot maintain that list on its own: only the services know which
  // catalog topics are derived. Every on-demand install (onDemandRecipeInstalled)
  // calls this after the catalog has relisted; call it from any other path that changes the recipe set WITHOUT
  // retiring the outputs' catalog items — a plugin creating or withdrawing a
  // transform, and a layout restore. A path that does retire them needs nothing:
  // the panel prunes on CatalogModel::itemsRemoved. Call it AFTER the catalog
  // rebuild, or the new outputs have no keys yet.
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
  // The feature catalog `about` serves (desktop only).
  [[nodiscard]] FeatureCatalogHost* featureCatalog() const {
    return feature_catalog_.get();
  }
#endif

  // Preferences key (bool, default on): open MCP public access so AI agents can control this
  // window. Read at startup by main.cpp, which ignores it for an unattended run; the Preferences
  // switch shows whether public access is open and applies on OK.
  static constexpr QLatin1StringView kMcpServerKey{"Preferences::mcp_server"};

  // Opens MCP public access: any local agent (the bridge) may connect with a random token, which
  // the discovery file advertises. Starts the server on a free 127.0.0.1 port unless it already
  // runs for a toolbox plugin's endpoint (a failure goes to the diagnostics). A no-op while open.
  // Shows a status-bar indicator while a public client is active.
  void openMcpPublicAccess();
  // Closes public access (token and discovery file); agents' tabs and derived series stay. The
  // server keeps listening, refusing everything but toolbox plugins' private endpoints.
  void closeMcpPublicAccess();
  [[nodiscard]] bool mcpPublicAccess() const;
  // Opens public access and links it to the agent relay at `relay` (McpBrowserLink): how the
  // browser build, which cannot listen, reaches an agent (the page's ?mcp= parameter; --mcp-relay
  // on the desktop). False, after a diagnostic, when `relay` is not a loopback http URL or the
  // server cannot start. A second call replaces the link.
  bool connectMcpRelay(const QUrl& relay);

 public slots:
  // Apply-only: clamp, update chrome_metrics_, broadcast chromeMetricsChanged.
  // These do NOT write QSettings — persistence is deferred to
  // persistChromeMetrics() so dragging a sizing scrubber previews live but
  // commits once (see PreferencesDialog).
  void setIconSize(int size);
  void setIconPadding(int padding);
  void setLayoutPadding(int padding);
  void setLayoutSpacing(int spacing);

 signals:
  // Fires after qApp's stylesheet is applied; subwidgets refresh
  // palette-tinted icons via their onStylesheetChanged slots.
  void stylesheetChanged(QString theme);

  // The settlement boundary of a loadLayoutFromPath restore (the
  // --exit-after-layout observation channel): emitted exactly once per layout
  // load that reaches a terminal outcome. `success` is true only when the
  // layout COMMITTED (the sync apply leg's tail, or the progressive drain
  // after every restore waiter cleared and the import batch finished — so a
  // quit on this signal can never tear down a still-active batch); false on
  // an open/parse/apply failure, a user cancel, or a cancelled import batch.
  // A progressive restore superseded by a newer load never settles — the
  // superseding load emits its own settlement instead.
  void layoutRestoreSettled(bool success);

  // The terminal of one loadLayoutFromUrl: `reason` is empty on success, else
  // the token its PJ_WASM_LAYOUT_LOAD_FAILED line names (busy, download, size,
  // fingerprint, source-url, staging, import, source-shape, resources,
  // apply-after-source-load, …) or "cancelled". A busy refusal is reported at
  // once for the refused URL while the load in flight carries on. Emitted after
  // the load's state is cleared, so a handler may start the next one.
  void layoutUrlLoadFinished(const QUrl& url, bool ok, const QString& reason);

  // A data source of the loadLayoutFromUrl for `url` is downloading: `received` bytes so far of `total` (its
  // declared size, or -1 when unknown), at most about ten times a second. Each source ends with received == total,
  // its size, once it has fully arrived. A layout with several sources reports each in turn.
  void layoutUrlLoadProgress(const QUrl& url, qint64 received, qint64 total);

  // Fires when any chrome metric changes. Bundled so consumers always
  // recompute layout from a consistent snapshot:
  //   band height       = (icon_size + icon_padding) + 2 * layout_padding
  //   chrome margins    = layout_padding
  //   chrome / list gap = layout_spacing
  // layout_padding feeds QLayout::setContentsMargins (band grows to
  // absorb it), layout_spacing feeds QLayout::setSpacing and the
  // CurveEditor list-row gap.
  void chromeMetricsChanged(const ChromeMetrics& metrics);

 private slots:
  // Layout file flow: open / save / replay-recent. Persists the chosen file
  // path into the recent-layouts list (cap kMaxRecentEntries) regardless of
  // whether the on-disk format is meaningful yet. onLoadRecentLayout is invoked
  // from the LeftPanel recent popup's Layouts section.
  void onLoadLayout();
  void onSaveLayout();
#ifdef PJ_TARGET_WASM
  void onSaveSourceLayout();
#endif
  void onLoadRecentLayout(const QString& path);

  // Opens the extension marketplace dialog.
  void onOpenMarketplace();

  // Opens the file-load workflow.
  void onLoadDataRequested();

  // Reloads the remembered data source with its recorded plugin config.
  void onReloadDataRequested();

  // Reloads ONE dataset from its tracked source path (curve tree "Reload") —
  // the per-dataset variant of onReloadDataRequested for sessions with several
  // files loaded. Recovers the plugin + config recorded for that path, not
  // just the most recent load.
  void onReloadDatasetRequested(DatasetId dataset_id);

  // Curve tree "Replace": pick a different file and transactionally replace
  // the dataset's data with it (FileLoader::replaceFromDialog).
  void onReplaceDatasetRequested(DatasetId dataset_id);

  // Curve tree "Info": show the dataset-info dialog (display name, backing
  // file, and provenance from the dataset's SourceRecord when it has one).
  void onDatasetInfoRequested(DatasetId dataset_id);

  // Updates playback bounds after a data file has populated datastore and
  // object-store topics.
  void onFileLoaded(
      const QString& path, const QString& prefix, const QString& plugin_id, const QString& plugin_config_json,
      const QString& plugin_manifest_id);

  // Removes selected catalog entries from the curve/object tree.
  void onCatalogTrashRequested(QStringList keys, bool covers_all);

  // Removes the selected datasets (curve tree "Remove"): shows one
  // combined confirmation, then removeDatasets(). Widget sync is signal-driven.
  void onRemoveDatasetsRequested(const QList<DatasetId>& dataset_ids);

  // Merges the selected datasets (curve tree "Merge"): shows the shared
  // destructive-merge confirmation, performs the merge, and marks the result on
  // the Source Timeline so its bar reads as merged.
  void onMergeDatasetsRequested(const QList<DatasetId>& dataset_ids);

  // Erase one dataset's data (TF buffer, object payloads, catalog items, then the
  // engine's scalar storage — in that order) and drop its file association. Shared
  // body of the multi-remove; does NOT confirm, re-seed playback, or reset undo —
  // the caller does those once for the whole batch.
  void removeDatasetData(DatasetId dataset_id);

  void onShowPreferencesDialog();

  // Opens the modal About box (Help ▸ About PlotJuggler…).
  void onShowAboutDialog();

  // Help ▸ Check for Updates… — a manual, always-runs release check that also
  // reports when the app is already current (unlike the silent startup check).
  void onCheckForUpdates();

  // Target of the update toast's "Update now" link (the `pj-update:` scheme):
  // starts the Windows maintenance tool in updater mode, then closes the app,
  // whose files Windows keeps locked while it runs.
  void onUpdateLinkActivated(const QUrl& url);

  // Rebuilds the Help ▸ Installed Extensions submenu from the current
  // ExtensionCatalogService snapshot. Informational only (disabled
  // entries): data sources, message parsers, toolboxes. Managing
  // extensions happens in the Marketplace (File menu).
  void onRebuildExtensionsMenu();

  // Rebuilds the title-bar Toolbox menu from the current toolbox
  // catalog. Lists only the launchable, *non-cloud* toolboxes (cloud
  // toolboxes live in the Sources panel instead); each entry launches
  // its toolbox via launchToolbox(). Built lazily on aboutToShow so it
  // tracks whatever the catalog currently has loaded.
  void onRebuildToolboxMenu();

  // Point the title-bar progress strip at the last-started interactive toolbox
  // import (title from toolbox_ingest_label_, busy bar, delayed show) and take
  // displayed ownership for it. Callers gate on FileLoader being idle — the
  // strip has one owner at a time and file loads win.
  void adoptToolboxIngestStrip();

  // Permille progress on the strip (counts may be bytes far beyond int range);
  // total 0 keeps the busy/indeterminate bar.
  void setIngestStripProgress(quint64 current, quint64 total);

  // D8 survivor pick + display: hands the strip to the eligible active ingest
  // per the arbitration rule at ingest_strip_owner_kind_, skipping `exclude`
  // (a dataset that is ending/ineligible; 0 = none) and restoring the
  // survivor's recorded label/progress. Returns false when no eligible ingest
  // survives (the caller decides between linger-hide and leaving the strip to
  // a file load). Precondition: callers invoke this only while the strip is
  // already engaged (shown, or its show delay pending) — it never (re)starts
  // the show timer.
  bool displaySurvivingIngestOnStrip(DatasetId exclude);

  // The strip's displayed ingest ended (or became ineligible): switch to a
  // survivor, or clear ownership and start the linger-hide (skipped while a
  // file load is about to own the widget). No-op when `ended` is not the
  // displayed dataset.
  void releaseIngestStripOwner(DatasetId ended);

  // Flag-only cooperative cancel of every live toolbox bulk import: dedups by
  // host, skips entries whose owning panel has closed (dead weak owner), and
  // never dereferences a host until that owner check passes. Keep-partial — the
  // toolbox ABI has no host-side rollback.
  void stopAllToolboxImports();

  // Launches a toolbox by id: builds a ToolboxRuntimeHost, binds the
  // toolbox, hosts its dialog in a PanelEngine, and presents it in the
  // chart area (or pins it as a tab, per `target`). Close tears it all
  // down. Shared by the Toolbox menu and LeftPanel::cloudToolboxRequested
  // ("cloud" is just a manifest tag). If the toolbox is already pinned as a
  // tab, launching focuses that tab instead (one live instance per id).
  // `initial_config` (optional) is handed to the toolbox via loadConfig() before
  // its dialog is built — used to open the Transform Editor pre-populated for an
  // in-place edit of an existing derived series. `pin_tab_name` (kPinnedTab
  // only) overrides the plugin display name as the tab label, so a layout
  // restore re-creates a renamed tab born with its saved name.
  void launchToolbox(
      const QString& plugin_id, const QString& initial_config = QString(),
      ToolboxLaunchTarget target = ToolboxLaunchTarget::kTakeover, const QString& pin_tab_name = QString());

  void onThemeChanged(const QString& theme);

  // Wires callbacks for a newly created plot tab.
  void onPlotTabAdded(PlotDocker* docker);
  /// Focus the requesting dock and toggle the shared settings sidebar.
  void onSettingsPanelToggleRequested(DockWidget* dock);

  // A placeholder 2D/3D icon was clicked: builds an empty scene dock of the
  // matching kind (the shell owns the family→kind mapping) and adopts it into
  // `dock`. Plot clicks are handled inside DockWidget itself.
  void onObjectFamilyRequested(DockWidget* dock, VisualizationKind family);

  // A catalog drop named an advertised placeholder (see
  // DockWidget::placeholderTopicDropped): routes to
  // TopicDemandController::handlePlaceholderPlotDrop (object_type == kNone) or
  // handleSceneDockPlaceholderDrop, instead of the normal add-curve/add-layer path.
  void onPlaceholderTopicDropped(
      DockWidget* dock, DatasetId dataset_id, QString topic_name, sdk::BuiltinObjectType object_type);

  // Routes the focused DockWidget to the right config page and updates
  // the curve-editor binding. Plot-only state changes still go through
  // bindEditorToPlot.
  void onDockFocused(DockWidget* dock);

  // Wires callbacks for a newly created plot widget.
  void onPlotAdded(PlotWidget* plot);

  // Opens the Filter Editor as a chart-area takeover panel (presentPanel) scoped
  // to `sources`. On Apply each filtered output replaces its source curve on
  // `origin` (PlotWidget::replaceCurve — same colour, in place); Close/Apply
  // restore the chart area.
  void openFilterEditor(std::vector<CurveDescriptor> sources, PlotWidget* origin);
  // Push the current global grid / curve-style / curve-width onto a PlotWidget
  // embedded in a plugin toolbox panel (e.g. the Transform Editor plugin), so the
  // app's right-side display buttons drive the plugin preview too.
  void syncPanelPreviewDisplay();
  // Deferred syncPanelPreviewDisplay for a panel that was just shown: its
  // embedded PlotWidget only exists after the engine's first tick.
  void schedulePanelPreviewSync();
  // Push the current global grid / curve-style / curve-width onto an open Filter
  // Editor's preview plot so it matches the real plots. No-op unless the presented
  // panel is a FilterEditorPanel. Called on open and from the viz-toolbar handlers.
  void syncFilterEditorPreviewDisplay();

  /// Inherit the linked time range after a curve drop has fitted the receiving plot.
  void onPlotCurvesDropped(bool was_empty);
  // Mirrors X zoom to linked plots.
  void onPlotZoomChanged(PlotWidget* modified, QRectF rect);
  // Linked-zoom feed from a State Transitions strip: fan its visible window's X
  // onto plots and the other strips (mirror of onPlotZoomChanged, gated by the
  // same link toggle).
  void onStateTransitionsRangeChanged(StateTransitionsDockWidget* source, double t_min, double t_max);

  // Updates playback time from a plot tracker move.
  void onTrackerMovedFromWidget(QPointF point);

  // "Use time offset" toggled: flip the SessionManager frame (plots re-fit via
  // displayOffsetChanged), re-seed the playback range, and shift the playhead by
  // the per-dataset offset delta so the cursor stays on the same real instant.
  void onUseTimeOffsetToggled(bool checked);

  // The representative dataset whose offset frames the global playhead and blue
  // reference line: the active streaming dataset, else the first loaded one (0
  // when the session is empty). Single-dataset sessions are exact; multi-dataset
  // alignment is the future refinement the displayOffset seam leaves room for.
  [[nodiscard]] DatasetId representativeDatasetId() const;

  // The blue reference line's position in display-axis seconds, PROJECTED on
  // demand from reference_instant_ through the current representative-dataset
  // offset. nullopt when no reference is set. Re-evaluated on every frame change
  // (displayOffsetChanged), so the line tracks the offset instead of going stale.
  [[nodiscard]] std::optional<double> referenceDisplaySeconds() const;

  // Push the current reference-line position (referenceDisplaySeconds(), computed
  // once) to every plot and the Source Timeline.
  void broadcastReferenceLine();

  // Recomputes time_label_format_ from the date-time toggle and the UTC
  // preference, and pushes it to every plot, the playback readout and the Source
  // Timeline. Cheap to repeat: each receiver no-ops on an unchanged format.
  void applyTimeLabelFormat();

  // Drop-time fast path for the live-ingest seed below: if data is already present
  // when the user drops a scalar curve or object topic, set the playback range and
  // playhead immediately. Live ingest can also seed without any drop.
  void seedStreamingPlaybackFromDrop();
  // The first topic dropped into an empty click-created object view: seeds streaming playback and frames it.
  void onFirstObjectTopicAdded(DockWidget* dock);

  // Locks seeking (playback slider + Source Timeline) exactly while a followed stream
  // is playing; called on every change of either condition.
  void refreshStreamingSeekLock();

  // Put the Source Timeline into read-only mode (called with streaming && playing,
  // from refreshStreamingSeekLock). While the cursor is glued to the live
  // tip, editing source offsets / order / merges or seeking the needle is unsafe;
  // pausing the stream clears it so the retained window can be scrubbed/realigned.
  // Locks the Timeline widget's manipulation gestures (setInteractionLocked) AND
  // greys the external align rail (whose buttons drive the controller directly).
  void setSourceTimelineStreamingLock(bool locked);

  // Records a user-visible plot layout change. `force_new_state` pushes a fresh,
  // non-coalescing undo entry — set it for discrete operations that must never merge
  // into a preceding edit (e.g. applying/removing a filter).
  void onUndoableChange(bool force_new_state = false);

  // Restores the previous layout snapshot.
  void onUndo();

  // Restores the next layout snapshot after undo.
  void onRedo();

  // Record button toggled in the streaming strip: records every live streaming
  // source into one batch folder, one file each, or ends the running capture. A
  // start that cannot proceed unchecks the button and explains itself in a
  // toast, so the button never lies about the state.
  void onRecordToggled(bool record);

  // The capture opened: every file exists and every tap is attached.
  void onRecordingStarted(
      const QString& capture_id, const QString& directory, const QVector<SourceRecordingStart>& sources);

  // Terminal report of one capture. The toast names one file when there is one,
  // the folder when every source is clean, and the failing sources otherwise;
  // the per-source detail always goes to the log.
  void onRecordingStopped(const CaptureResult& capture);

  // 500 ms progress tick: refreshes the elapsed/size label and, once the byte
  // budget has had to drop messages, says how many.
  void onRecordingProgress(quint64 messages, quint64 payload_bytes, quint64 dropped);

 private:
  // Re-reads the recording settings the Preferences Recording page persists, so
  // the next recording uses them without an app restart. Called on every
  // Preferences dialog this window opens.
  void bindRecordingPreferences(PreferencesDialog& dialog);

  // Pushes the derived-markers mode and reload rate the Preferences Plotting page
  // persists to the batch engine: at startup, and on OK of every Preferences dialog
  // this window opens (bindPlottingPreferences).
  void applyDerivedBatchPreferences();
  // Connects the Plotting page's OK signals: the derived-markers settings above and
  // the clock time zone (applyTimeLabelFormat).
  void bindPlottingPreferences(PreferencesDialog& dialog);

  // Tells every plot which of the marker rules it displays are computing or failed
  // (PlotWidget::setMarkerStatus), on BatchEngine::runningChanged. "Computing" waits
  // until a rule has been running for 300 ms, so a quick run never flickers a note.
  void wireMarkerPlotStatus();
  // Reports the standing marker status to `plot` (wireMarkerPlotStatus's push), when it is wired.
  void reportMarkerStatus(PlotWidget* plot);
  void onPlotMarkerTargetsChanged();
  std::function<void(PlotWidget*)> push_marker_status_;
  // Tells `only`, or every plot when null, which on-demand series are not computed yet
  // (PlotWidget::setSeriesStatus); on SessionManager::seriesStateChanged and for a new plot.
  void pushSeriesStatus(PlotWidget* only);

  // Reloads a recorded source through its plugin + config (dialog skipped when both are known,
  // unless `policy` says otherwise). Shared by the Reload button and reloadDataset (the per-dataset
  // reload, also an MCP tool's); the loader asks `still_allowed` before applying anything. Returns
  // the load ticket, 0 when the loader refused the request outright.
  LoadRequestId reloadSource(
      const QString& path, const QString& plugin_id, const QString& plugin_config_json,
      std::optional<DialogPolicy> policy = std::nullopt, std::function<bool()> still_allowed = {});

  // Enables the Reload button iff a reloadable source is recorded. reloadSource disables it for the
  // request's duration; call this from every way that request can end.
  void refreshReloadEnabled();

  // Sets legend position (or hides if `position` already matches the
  // current state — clicking the active corner toggles the legend off).
  // Updates QSettings, refreshes button checked states, and re-applies
  // to every plot.
  void setLegendStatus(LegendStatus position);

  // Pushes the current toolbar toggle state (show_points / legend_status /
  // activate_grid / dots) into one plot, so newly added plots match.
  void applyGlobalToggles(PlotWidget* plot);
  void applyShowPointsToDock(DockWidget* dock);
  void applyShowPointsTo2DWidgets();
  void applyLegendStatus(PlotWidget* plot);
  // Slot: left-click cycles or restores the legend position.
  void onLegendButtonClicked();
  // Toggles dots overlay on Lines/LinesAndDots curves only; curves in
  // Dots/Sticks/Steps keep their style.
  void applyDots(PlotWidget* plot);

  // Refreshes button_time_tracker_'s icon to match tracker_info_.
  void updateTimeTrackerIcon();
  // Slot: cycles tracker_info_ to the next state and applies to every plot.
  void onTimeTrackerButtonClicked();

  // Per-tab union of X-ranges across non-XY plots when buttonLink is checked;
  // independent zoom-out otherwise. XY plots always zoom out individually
  // (their X axis is a curve value, not time, so the union is meaningless).
  void linkedZoomOut();

  /// `SessionManager::displayTimeForSource` on the ABI's source handle, unwrapped to the axis double.
  [[nodiscard]] std::optional<double> displayTimeForSource(PJ_data_source_handle_t source, int64_t absolute_ns) const;
  /// displayTimeForSource on the dataset owning `topic` (empty: representativeDatasetId()); nullopt for an
  /// unknown topic. The `to_display_time` of every `pj.playback.v1` the shell offers.
  [[nodiscard]] std::optional<double> displayTimeForTopic(std::string_view topic, int64_t absolute_ns) const;

  // Convenience: emit a diagnostic into the session's sink. Source/id
  // are stable string literals; message is a translated QString. The
  // sink fans out to QtDiagnosticBridge → DiagnosticHistory and from
  // there to the bell label + popup.
  void emitDiagnostic(DiagnosticLevel level, const char* source, const char* id, const QString& message);

  // Refreshes the streaming source selector from loaded plugins.
  void refreshStreamingCombo();
  // Recomputes whether any Custom Widget type is installed and hands it, with the Custom-Widget-only object
  // types, to every dock (CustomWidgetSupport).
  void refreshCustomWidgetsAvailability();

  // Wires callbacks for plots already present after UI setup.
  void wireExistingPlots();

  // Applies operation to each dock widget.
  void forEachDock(const std::function<void(DockWidget*)>& operation);

  // Applies operation to each dock widget on the active top-level tab only (the
  // on-screen docks). forEachDock's spatial sibling, used by the per-tick fan-out.
  void forEachVisibleDock(const std::function<void(DockWidget*)>& operation);

  // Pushes the current tracker time (display-axis seconds) to every data dock and
  // refreshes the curve-list Value column. The single seam for "the time cursor
  // moved": playback ticks, seeks, and layout/undo restores all route through here.
  void broadcastTrackerTime(double display_seconds);

  // Like broadcastTrackerTime, but only the active top-level tab's docks (plus the
  // always-visible curve-list Value column). The per-tick playback/scrub fan-out
  // uses this so off-screen tabs do no replot/decode/composite work; structural
  // seeds use broadcastTrackerTime (all tabs) and the tab-switch handler re-seeds
  // the newly active tab so a revealed tab is never stale.
  void broadcastTrackerTimeToVisible(double display_seconds);

  // Applies operation to each 2D/3D scene dock hosted in a DockWidget.
  void forEachSceneDock(const std::function<void(SceneDockWidget*)>& operation);

  // Applies operation to each plot widget.
  void forEachPlot(const std::function<void(PlotWidget*)>& operation);
  // Every mounted State Transitions strip across all tabs.
  void forEachStateStrip(const std::function<void(StateTransitionsDockWidget*)>& operation);
  // Every mounted Custom Widget panel across all tabs.
  void forEachCustomWidgetDock(const std::function<void(CustomWidgetDockWidget*)>& operation);

  // Re-syncs every data widget to the catalog after a removal: each prunes its
  // own dead pieces (plots drop dead curves; object viewers drop dead layers,
  // resetting the dock to the placeholder when empty). Connected to the
  // catalog's cleared()/itemsRemoved() signals.
  void syncWidgetsToCatalog();

  // Icons not owned by a subwidget with its own onStylesheetChanged.
  void applyIcons(QString theme);

  // Builds the global toolbar column — vertical stack of Chart + Legend
  // icons in a fixed 24-px wide strip that sits left of the local panel.
  // No headers, no flow-layout, always visible regardless of the local
  // panel's toggle state.
  void buildGlobalToolbar();

  // Builds the timeline panel's align rail — a 24-px icon column on the right
  // edge of the bottom panel that visually continues the global toolbar above
  // it. Holds the Source Timeline alignment actions (align starts / centers);
  // shown only while the strip is open (applyBottomPanelConstraints drives that).
  void buildTimelineAlignRail();

  // Builds the local panel — Curve Width + Curve Style header bands and
  // their flow-layout icon strips above the CurveEditor. Snap/compact
  // behaviour still applies (hidden by the "Toggle Right Panel" button,
  // headers fold below ~72 px wide).
  void buildLocalToolbar();

  // Apply to every curve of the editor's bound plot. No-op when unbound.
  void applyActivePlotWidth(double width);
  void applyActivePlotStyle(int style);

  /// Exact in-memory state of one dataset track. Portable qualifiers are carried
  /// only so a source-replacement snapshot can map a reminted DatasetId; history
  /// replay still requires the raw id to remain live.
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

  /// One atomic workspace snapshot. XML stays on the established schema while
  /// session-only timeline identity and chrome remain in memory.
  struct CapturedWorkspace {
    QByteArray xml;
    TimelineState timeline;

    [[nodiscard]] bool operator==(const CapturedWorkspace&) const = default;
  };

  struct PendingSourceReplacement {
    CapturedWorkspace workspace;
    QString path;
    // The arming load's LoadRequestId. The disarm compares tickets, not paths:
    // loadFinished is queued, so a stale same-path terminal from another load
    // could otherwise kill a live load's capture.
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

  // Layout helpers.
  //
  // Whether the load may open dialogs (reload / trust / missing-curve
  // prompts). Threaded EXPLICITLY through the whole restore — including any
  // LayoutImportBatch, which captures it at construction — because late
  // async continuations run long after the caller's stack (and any
  // startup-time flag) has unwound (D5).
  enum class LayoutLoadInteractivity {
    kInteractive,  ///< user-driven (menu / recent list): prompts allowed
    kAutomated,    ///< --layout CLI: auto-reload, never any dialog
  };
  void loadLayoutFromPath(const QString& path, LayoutLoadInteractivity interactivity, bool use_current_data = false);
  // Byte-level layout loads: browser uploads and recipes, and loadLayoutFromUrl
  // (which is why they also build on the desktop). Every terminal outcome goes
  // through finishBrowserLayoutLoad, which logs PJ_WASM_LAYOUT_LOAD_OK/FAILED.
  enum class BrowserLayoutLoadResult { kApplied, kPending, kFailed };

  [[nodiscard]] QDomDocument browserGenericLayoutDocument() const;
  [[nodiscard]] std::optional<QDomDocument> browserSourceLayoutDocument(QString& error) const;
  // `layout_url` is where the bytes came from (empty for an upload): relative
  // Scene3D resources and file sources resolve against it, and with it the
  // sources download instead of being reselected.
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
  // Accepts `input` (already checked) for the replay's current source slot and moves to the
  // next one; `name` is only logged.
  void stageReplayInput(LoadInput input, const QString& name);
  // Remote replay (layout_url set): downloads the next source, then stages it
  // under the same generation token a picker selection uses.
  void downloadNextReplaySource();
  // Whether the replay still waits for the download started under `generation`.
  [[nodiscard]] bool isActiveReplayDownload(quint64 generation) const;
  // End the replay over its current source: reason "download", or "size" (over kMaxPresetDataBytes).
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
    // Non-interactive batch restore (D5): never prompts, never opens a
    // dialog — unresolved intents are RETAINED (never stripped/cleared) and
    // reported through a diagnostic, so a later binding pass can still
    // resolve them once their data arrives.
    kRetainAndDiagnose,
  };

  // Applies a parsed layout to already-loaded data: curve rebind, plot/panel
  // restore, recent-files. `policy` governs unresolved curves (the sync leg
  // of the same policy the progressive drain captures). The progressive
  // reload path uses beginProgressiveLayoutRestore instead so the structure
  // can appear before the load queue drains.
  void applyRestoredLayout(QDomDocument doc, const QString& path, MissingCurvePolicy policy);
  // `policy` is the caller-derived unresolved-intent policy (the load's
  // interactivity) — captured into progressive_missing_curve_policy_ for the
  // drain; never inferred from batch presence.
  void beginProgressiveLayoutRestore(QDomDocument doc, const QString& path, MissingCurvePolicy policy);
  void cancelProgressiveLayoutRestore();
  [[nodiscard]] bool rollbackProgressiveWorkspace();
  [[nodiscard]] bool abortProgressiveRestore();
  void flushPendingCurveBindings(const std::vector<CatalogItem>& items);
  /// Re-registers the document's unresolved curves with the pending binder
  /// (live scene pends survive; see PendingDisplayBinder::collect).
  void collectPendingDisplayBindings(const QDomDocument& doc);
  /// collectPendingDisplayBindings + an immediate drain-pass flush — the
  /// post-replay shape used by restore and rollback.
  void rebuildPendingDisplayBindings(const QDomDocument& doc);
  /// Coalesces plot-owned intent changes into one binder rebuild on the event loop.
  void schedulePendingDisplayBindingRebuild();
  /// One post-restore settlement of every scene dock: dataset choice, final
  /// pending retry, then the folded verdict (permanent failure / unresolved
  /// BLOCKING topics).
  struct SceneRestoreVerdict {
    bool failed = false;
    QStringList blocking_topics;
  };
  [[nodiscard]] SceneRestoreVerdict settleSceneRestores();

  int retryPendingSceneRestores(const std::vector<CatalogItem>& items);
  [[nodiscard]] QStringList unresolvedPendingSceneRestores();
  void clearPendingSceneRestores();
  void onProgressiveLayoutDrained();
  // The end-of-drain owner of every unresolved-state teardown decision
  // (binder intents, blocking scene pends, the scene-pend clear), switching
  // on the SAME policy enum applyWorkspace honors: kRetainAndDiagnose keeps
  // the intents (D5) and diagnoses; kPrompt runs the missing-curve prompt.
  // Returns false when the user cancelled (the restore was aborted).
  [[nodiscard]] bool finalizeUnresolvedRestoreState(MissingCurvePolicy policy);
  // Dialog-vs-diagnostic fork for drain-time restore failures: a
  // kRetainAndDiagnose (non-interactive) restore reports through the
  // diagnostic sink, every other policy through a warning dialog.
  void reportLayoutRestoreIssue(MissingCurvePolicy policy, const char* id, const QString& message);
  // Builds the batch coordinator for one materialize-bearing restore, wiring
  // the shell effects (the one-shot workspace checkpoint, removeDatasetData,
  // the consolidated trust prompt, the diagnostic sink) as its hook seams.
  // Takes bool (not LayoutLoadInteractivity) deliberately: the batch is
  // MainWindow-free and must not name a MainWindow-scoped enum.
  [[nodiscard]] std::unique_ptr<LayoutImportBatch> makeLayoutImportBatch(bool interactive);
  // True while a layout-import batch still has work in flight — a pending
  // import keeps the restore alive even when FileLoader is idle.
  [[nodiscard]] bool layoutImportBatchActive() const;
  // The single routing predicate for a restore that must go progressive:
  // either the loader is still chewing the reload queue, or a batch import
  // is producing data the drain must wait for (§6.2 "a pending import keeps
  // the restore alive").
  [[nodiscard]] bool restoreHasPendingAsyncWork(bool reload_requested) const;
  // The progressive-drain gate: proceeds only when NO registered restore
  // waiter is still pending (FileLoader's queue and the batch — the batch's
  // waiter is also what holds the drain open for T7's mid-import binding;
  // the binder work registers nothing of its own here). A cancelled batch
  // instead unwinds the progressive state: its rollback already restored
  // the pre-layout workspace.
  void maybeSettleProgressiveRestore();
  // D1: batch-scoped observation of SessionManager's ingest lifecycle —
  // connects the three signals at the batch HANDOVER (and only then; plain
  // layouts never install them). The batch job's announced dataset displays
  // under kLayoutBatch ownership; every other began/progress rides the same
  // arbitration as a concurrent interactive ingest.
  void installLayoutBatchObservers();
  // Disconnects the observers and releases a batch-owned strip display. MUST
  // run BEFORE every layout_import_batch_.reset()/destruction (D1 ordering:
  // no lifecycle event or Stop click may reach a dangling batch).
  void teardownLayoutBatchObservation();
  // Drops the current batch (graceful if finished; HARD shutdown — children
  // cancelled+joined, no rollback — if still running). Also ends the D5
  // retention channel a kRetainAndDiagnose drain left alive (see
  // pending_items_added_conn_).
  void resetLayoutImportBatch();
  // The FULL supersede: tears down any in-flight progressive transaction
  // (waiters/doc/binder/flag) AND retires the batch — every new layout load
  // and closeEvent go through this, because a load that stays synchronous
  // never reaches beginProgressiveLayoutRestore to clean up the old one.
  void supersedeActiveRestore();
  // Re-applies the timeline state (offsets + track order) stashed by a progressive
  // restore, now that the async worker has registered the reloaded datasets' source
  // paths. Called from onProgressiveLayoutDrained BEFORE the processor graph replay
  // (an all_datasets marker generator must recompute under the settled offset, not
  // the pre-apply one) and BEFORE the viewport re-frame, so the saved absolute
  // window converts with the settled offset. Returns whether any offset moved.
  // No-op when nothing was stashed.
  bool applyPendingTimelineState();
  // One resolved layout source track, shared by the early offset write and
  // the later track-order restore so neither pass can bind a different dataset.
  struct LayoutTimelineTrack {
    DatasetId dataset_id;
    qint64 display_offset_ns;
    bool has_display_offset;
    int timeline_order;
  };
  using LayoutTimelinePlan = std::vector<LayoutTimelineTrack>;
  // Applies a byte-level layout with kExact, then its file sources' timeline
  // state plus `stream_tracks` (the replay's streams, which have no data yet,
  // so an empty catalog is not an error when there are some).
  [[nodiscard]] bool applyBrowserRestoredLayout(
      QDomDocument doc, const QString& browser_name, const LayoutTimelinePlan& stream_tracks = {});
  // The replay's last step: applies it (startReplayStreamsAndApply), rolls it back on a
  // failure or replaces the previous preset's data on a URL load's success, and finishes
  // the load. `apply_failure` is the failure token when the layout itself does not apply.
  void applyBrowserReplay(QDomDocument apply_doc, const QString& apply_failure);
  // Starts the replay's streams from their records, binds what the layout qualified by
  // pj-stream:// to the new sessions and applies it. Returns the failure token ("stream",
  // else `apply_failure`), empty on success.
  [[nodiscard]] QString startReplayStreamsAndApply(QDomDocument apply_doc, const QString& apply_failure);
  // After a preset load succeeded: records what it brought and removes what the
  // preset before it brought (its file datasets and streams), nothing else.
  void replacePreviousPresetData();
  void restoreChromeAndPanels(
      const QDomDocument& doc, const QString& path, const LayoutTimelinePlan* resolved_timeline = nullptr);
  void saveLayoutToPath(const QString& path, bool include_data_source);
  void recordRecentLayout(const QString& path);
  [[nodiscard]] QStringList recentLayouts() const;

  // Rewrites every curve's stable topic+field path to a concrete catalog key,
  // resolving across all loaded datasets (first dataset that has the path).
  // Returns the stable paths no loaded dataset could provide. Shared by layout
  // load (which prompts on the unresolved set) and undo/redo restore.
  [[nodiscard]] QList<layout_xml::SeriesPath> rebindCurvesToLoadedDatasets(QDomDocument& doc);

  enum class TimelineRestoreMode {
    kExact,
    kPortableSourceReplacement,
  };
  // Outcome of restoreWorkspaceState.
  enum class RestoreResult {
    kApplied,    ///< plots/toggles applied to the session
    kCancelled,  ///< user cancelled at the missing-curve prompt (kPrompt only)
    kFailed,     ///< xmlLoadState rejected the document
  };
  using RestoreIntent = PJ::RestoreIntent;
  // Restore a COMPLETE workspace snapshot (filters + curve rebinding + plots/toggles)
  // onto the live session — the single restore path shared by layout load and undo/redo,
  // so the two can never drift (that drift is what let undo silently drop filters).
  // Order matters: apply saved display offsets, recreate processors and marker
  // generators, then rebind curve keys and apply plots/toggles via xmlLoadState.
  // Snapshots carry stable topic/field paths, not per-load keys, so one
  // survives an intervening data reload. Callers run it under applying_state_ as needed.
  // `out_reason`, when non-null, receives the one-sentence rejection reason from
  // a `kHistory` intent's exempt-dependency check (see `checkDataProcessorDependencies`);
  // untouched on every other outcome.
  [[nodiscard]] RestoreResult restoreWorkspaceState(
      QDomDocument& doc, MissingCurvePolicy policy, const CapturedWorkspace* rollback_to = nullptr,
      RestoreIntent intent = RestoreIntent::kReplace, QString* out_reason = nullptr);
  [[nodiscard]] RestoreResult restoreWorkspaceState(
      const CapturedWorkspace& target, MissingCurvePolicy policy, TimelineRestoreMode timeline_mode,
      const CapturedWorkspace* rollback_to = nullptr, RestoreIntent intent = RestoreIntent::kReplace,
      QString* out_reason = nullptr);

  /// Capture/apply helpers shared by history, rollback, progressive restore, and
  /// source replacement. Timeline validation resolves every id and overflow
  /// guard before the first offset is written.
  [[nodiscard]] CapturedWorkspace captureWorkspace(SnapshotScope scope) const;
  // `SnapshotScope::kFull` — the convenience overload every non-history capture uses.
  [[nodiscard]] CapturedWorkspace captureWorkspace() const;
  // `stamp_override_id`, when non-zero, stamps that dataset with
  // `stamp_override_path` instead of its tracked path — used while a
  // replacement load retires it in favor of datasets loaded from the NEW path,
  // so the post-load rebind can map its charts onto them.
  [[nodiscard]] CapturedWorkspace capturePortableWorkspace(
      DatasetId stamp_override_id = 0, const QString& stamp_override_path = {}) const;
  [[nodiscard]] TimelineState captureTimelineState() const;
  [[nodiscard]] TimelineChromeState captureTimelineChrome() const;
  void applyTimelineChrome(const TimelineChromeState& state);
  [[nodiscard]] std::optional<DatasetId> resolveTimelineTrack(
      const TimelineTrackState& track, TimelineRestoreMode mode,
      const std::vector<std::pair<DatasetId, QString>>& live_datasets) const;
  [[nodiscard]] std::optional<TimelineResolutionPlan> validateTimelineState(
      const TimelineState& state, TimelineRestoreMode mode) const;
  // Before the processor replay, writes each track's
  // display offset so a marker generator that replays right after sees the settled
  // offset instead of the pre-restore one. `plan` must already be validated
  // (validateTimelineState); false on a size mismatch, mutating nothing.
  [[nodiscard]] bool applyTimelineOffsets(const TimelineState& state, const TimelineResolutionPlan& plan);
  [[nodiscard]] bool applyTimelineState(const TimelineState& state, const TimelineResolutionPlan& plan);
  [[nodiscard]] RestoreResult applyWorkspace(
      QDomDocument& doc, MissingCurvePolicy policy, const TimelineState* timeline_state,
      const TimelineResolutionPlan* timeline_plan, RestoreIntent intent, QString* out_reason = nullptr);
  [[nodiscard]] RestoreResult restoreWorkspaceStateImpl(
      QDomDocument& doc, MissingCurvePolicy policy, const TimelineState* timeline_state,
      TimelineRestoreMode timeline_mode, const CapturedWorkspace* rollback_to,
      RestoreIntent intent = RestoreIntent::kReplace, QString* out_reason = nullptr);

  // kPlaceholders was removed: the SessionManager API for registering
  // empty placeholder series doesn't exist yet, so the "Create empty
  // placeholders" button was indistinguishable from "Remove from plots"
  // (both just dropped the curves). Re-add the enumerator + the button
  // once the underlying API lands.
  enum class MissingCurveChoice { kRemove, kCancel };

  // Modal prompt mirroring PJ3's missing-curve dialog. `names` is shown to
  // the user (truncated past ~10 entries). Returns the user's pick.
  // `import_failures`: one "path: why" line per layout data source the
  // import could not bring back — shown so the user learns WHY the curves
  // are missing, not just that they are.
  [[nodiscard]] MissingCurveChoice promptMissingCurves(
      const QStringList& names, const QStringList& import_failures = {});
  // The active batch's failed sources as "path: why" lines (empty without a batch).
  [[nodiscard]] QStringList layoutImportFailureLines() const;

  // Builds <previouslyLoaded_Datafiles> from SessionManager's record, using
  // a path relative to `layout_dir` when the source lives at or beneath it,
  // absolute otherwise. Returns a null element when no source is recorded.
  [[nodiscard]] QDomElement appendDataSourceElement(QDomDocument& doc, const QDir& layout_dir) const;

  // The per-source Source Timeline state saved in `sources` (one DataSourceRef
  // per <fileInfo>), applied after the layout's datasets are (re)loaded in two
  // passes over ONE resolution: DatasetIds are re-minted each session, so
  // `resolveLayoutTimelineTracks` matches each saved entry back to a live dataset
  // by source path once (empty for generic layouts and pre-v3 layouts without
  // timeline attrs); `applyLayoutTimelineOffsets` writes the display offsets
  // (before the processor replay, so all_datasets generators see the settled
  // frame) and returns true iff one actually MOVED; `applyLayoutTimelineOrder`
  // rebuilds the timeline's vertical track order afterwards, when the chrome
  // is restored. `applyTimelineStateFromLayout` runs all three back to back.
  [[nodiscard]] LayoutTimelinePlan resolveLayoutTimelineTracks(const QList<layout_xml::DataSourceRef>& sources) const;
  bool applyLayoutTimelineOffsets(const LayoutTimelinePlan& plan);
  void applyLayoutTimelineOrder(const LayoutTimelinePlan& plan);
  bool applyTimelineStateFromLayout(const QList<layout_xml::DataSourceRef>& sources);

  // Builds <source_timeline zoom="…" scroll_left_ns="…" name_column_width="…"
  // snap="…"/> — the timeline's global VIEW chrome (independent of per-source
  // offsets/order, which ride <fileInfo>). Always emitted (pure UI chrome).
  [[nodiscard]] QDomElement saveSourceTimelineViewState(QDomDocument& doc) const;

  // Applies <source_timeline> view attributes: zoom first, then scroll, then the
  // name-column width, then the snap toggle. Missing attributes are left at their
  // current value. Must run AFTER the datasets reload so zoom/scroll map onto the
  // rebuilt scene.
  void restoreSourceTimelineViewState(const QDomElement& element);

  // Builds <right_panel_state visible="…" width="…" style="…"
  // splitter_sizes="…"/> from the four right-panel state sources. Always
  // emits an element (none of the attributes are gated). Caller appends.
  [[nodiscard]] QDomElement saveRightPanelState(QDomDocument& doc) const;

  // Applies <right_panel_state> attributes individually; missing or
  // mismatched values are silently ignored. Never writes to QSettings —
  // layout-driven UI changes don't mutate the global per-user defaults.
  void restoreRightPanelState(const QDomElement& element);

  // Builds <chrome_state main_splitter_sizes="..." timeline_splitter_sizes="..."/>.
  // Covers cross-panel chrome geometry that isn't owned by an individual panel
  // widget. Panel open/closed state is NOT serialized — it is a QSettings-backed
  // app preference, not document state.
  [[nodiscard]] QDomElement saveChromeState(QDomDocument& doc) const;

  // Applies <chrome_state> splitter geometry individually; missing or mismatched
  // values are silently ignored. Panel visibility is intentionally left as-is
  // (see saveChromeState).
  void restoreChromeState(const QDomElement& element);

  // Thin wrappers over `DerivedObjects` (pj_runtime), which owns the derived-recipe
  // layout persistence (`save` writes the <data_processors> block of a snapshot,
  // `parse` reads it for an intent, `restore` replays it). `restoreDataProcessors` =
  // `parse` + `checkDataProcessorDependencies` + `applyDataProcessors`; `out_reason`,
  // when non-null, is cleared on entry and set only when a `kHistory` restore is refused.
  [[nodiscard]] bool restoreDataProcessors(
      const QDomElement& root, RestoreIntent intent = RestoreIntent::kReplace, QString* out_reason = nullptr);

  [[nodiscard]] DerivedObjects derivedObjects() const;
  // Reports each derived-recipe diagnostic as a "Layout" warning.
  void reportDerived(const std::vector<DerivedDiagnostic>& diagnostics);
  // The read-only preflight a `kHistory` snapshot's restore runs before its first
  // clear: false (reported, and the reason in `out_reason`) when removing the
  // snapshot's non-exempt processors would strand an exempt transform or marker
  // generator. `applyWorkspace` runs it before applyTimelineOffsets so a refusal never
  // has to undo a moved offset.
  [[nodiscard]] bool checkDataProcessorDependencies(const DerivedSnapshot& snapshot, QString* out_reason = nullptr);
  // Restores `snapshot` under its intent, reports its parse and apply warnings and
  // re-derives the Custom Topics list. The default (empty) snapshot clears every recipe.
  [[nodiscard]] bool applyDataProcessors(DerivedSnapshot snapshot);

  // The rows again with the transform rows of the last sync (rebuilt only once the catalog changed since):
  // what a node's state or an offset changes. Costs per derived row, never per catalog row.
  void refreshCustomTopicsPanel();
  // Runs refreshCustomTopicsPanel once at the next event-loop turn, however many state
  // changes asked for it before then (a node's state changes on every request).
  void scheduleCustomTopicsSync();
  // Mirrors the extension catalog's Custom Topics editor into the panel.
  void syncCustomTopicsEditor();
  // The panel's Delete: an on-demand recipe's object topic or a transform output. Only
  // the Custom Topics editor's rows are deleted; another owner's is refused with a
  // warning (the panel disables the action; this is the backstop for any other caller).
  void deleteCustomTopic(const QString& catalog_key);
  // The panel's Delete of several rows: each recipe once, however many of its rows (series
  // columns, `id/output` object rows) are among `catalog_keys`, then the transform outputs.
  void deleteCustomTopics(const QStringList& catalog_keys);
  // Removes the recipe `key` after confirming when other recipes read it (they stay,
  // missing their input). A non-exempt recipe's removal is one undo step.
  void deleteOnDemandRecipe(const std::string& key);
  // Removes the transform whose output `catalog_key` is, with its dependents after confirming.
  void deleteTransformOutput(const QString& catalog_key);
  // Whether the Custom Topics editor may delete the transform writing `output_topic` now
  // (`transformDeleteRefusal`); a refusal is reported before it returns false.
  [[nodiscard]] bool mayDeleteTransformWriting(TopicId output_topic);
  // Words a refusal of CustomTopicsPolicy.h as a warning; a row whose recipe is gone is re-synced instead.
  void reportDeleteRefusal(const CustomTopicDeleteRefusal& refusal);

  // Size the bottom panel from the Source Timeline strip's open/closed state:
  // when OPEN, pin a minimum height so the strip can't be dragged to a clipped
  // sliver; when CLOSED, fix the panel to the playback bar so the splitter
  // handle can't drag the (hidden) strip open. The single source of truth for
  // bottom-panel height constraints, called from every site that changes the
  // strip's visibility (restore, toggle, layout load) + the chrome-metrics pass.
  void applyBottomPanelConstraints();

  // Align the Source Timeline's left name column so its right separator sits
  // exactly under the start of the playback bar's slider track. Measures the
  // slider's left x within the playback bar and pins the column to it. No-op
  // until the playback bar is laid out (post first show).
  void alignNameColumnToPlayback();

  // Serializes the current app layout state. `scope` governs both processors
  // and plot tabs; the no-arg overload is the full layout-file form.
  [[nodiscard]] QDomDocument xmlSaveState(SnapshotScope scope) const;
  [[nodiscard]] QDomDocument xmlSaveState() const;

  // Loads a previously serialized app layout state.
  bool xmlLoadState(const QDomDocument& state_document, RestoreIntent intent = RestoreIntent::kReplace);

  // Initializes the undo stack with the post-construction state.
  void pushInitialUndoState();

  // Discards undo/redo history and re-baselines from the current state. Called
  // after a confirmed data removal: prior snapshots are serialized layouts that
  // reference now-deleted curves by key, so replaying one would resurrect a
  // layout pointing at missing data.
  void resetUndoHistory();

  // Adds or replaces the newest undo snapshot.
  void pushUndoState(bool force_new_state = false);

  // Replace the current history tip after non-undoable additive data growth or
  // successful navigation, without creating a data-load undo operation.
  void hydrateCurrentUndoState(bool refresh_data_universe = true);
  void restoreHistoryState(const CapturedWorkspace& target, bool undo);
  void reconcileHistoryWithDataUniverse();

  // Exact raw storage identities that make existing history snapshots safe to
  // replay. Processor outputs are workspace state and are excluded.
  [[nodiscard]] QSet<QString> captureHistoryDataUniverse() const;

  // Updates enabled state for undo / redo actions.
  void updateUndoRedoActions();

  // Binds the CurveEditor to `plot` and enables/disables the width and
  // style toolbar buttons accordingly (they no-op without an active plot).
  void bindEditorToPlot(PlotWidget* plot);
  [[nodiscard]] PlotWidget* firstPlotOfActiveTab() const;
  // The dock driving the right config panel: the focused dock of the active
  // tab, or its first dock when nothing is focused. Used to refresh the panel
  // on events that don't emit ADS focus (tab switch, data clear) without ever
  // mirroring a dock from a hidden tab.
  [[nodiscard]] DockWidget* activeFocusedDock() const;

 protected:
  // Persists main-window settings before close.
  void closeEvent(QCloseEvent* event) override;

  // First-show hook (runs once the splitters finally have real geometry):
  // (1) restores the remembered left-panel splitter width — one-shot, guarded by
  // left_splitter_restored_; a later --layout load still overrides it; and
  // (2) pins the bottom timeline panel to a clean open/closed height
  // (constructor-time sizing doesn't stick and left the strip squashed), sized
  // deferred after the first layout pass.
  void showEvent(QShowEvent* event) override;

  // Keeps the toast stack pinned to the bottom-right corner as the window
  // resizes.
  void resizeEvent(QResizeEvent* event) override;

  // Frameless-window edge resize (custom frame only): catches mouse events on
  // ourselves or any descendant widget, updates the cursor near edges, and
  // starts a system-resize on press.
  bool eventFilter(QObject* watched, QEvent* event) override;

  // Local files dragged onto the window (from a file manager) load as if picked
  // in Load Data. Plots and placeholders ignore file drags, and Qt offers an
  // ignored drag-enter to each ancestor that accepts drops, so a drop on them
  // reaches here too. Floating docks are separate windows and do not take file
  // drops. Folders are not expanded.
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dragMoveEvent(QDragMoveEvent* event) override;
  void dropEvent(QDropEvent* event) override;

  // Paints a 1-px border flush with the window edge. Self-painted via
  // PJ::theme:: token accessors — FrameworkTokens.h is built for exactly this
  // case (a top-level widget QSS can't reach). Custom frame only (the system
  // frame draws its own edge); no-op while maximized/fullscreen, where the
  // window fills the screen and has no edge to outline.
  void paintEvent(QPaintEvent* event) override;

 private:
  // Presents `panel` (non-null) in place of the chart area, as a page of
  // ui_->centralAreaStack, dismissing any panel already up. restoreCentralArea
  // tears the panel down and shows the chart again; releaseCentralPanel is the
  // non-destructive variant that shows the chart and RETURNS the panel (hidden,
  // still a child of the stack) instead of deleting it — the "migrate to tab"
  // gesture uses it to keep the live toolbox widget. Neither reparents: the
  // destination does that once (and a teardown deletes the panel in place),
  // because every reparent unpolishes and repolishes the whole widget tree
  // against the app stylesheet (~180 ms for a toolbox panel in a browser).
  void presentPanel(QWidget* panel);
  void restoreCentralArea();
  QWidget* releaseCentralPanel();

  // Tears down whatever panel currently occupies the chart-area takeover —
  // the same close-restore-delete sequence presentPanel uses when replacing
  // it. No-op when no takeover is up. Called before focusing or restoring a
  // pinned toolbox tab, which the takeover would otherwise hide.
  //
  // A takeover with work in flight is FOLDED into a background tab instead:
  // teardown destroys the plugin instance, which is also its job's kill
  // switch, so merely opening another panel must not cancel a download.
  void dismissTakeoverPanel();

  // Folds the chart-area takeover into a pinned background tab, but only when
  // `owner` is the identity of the toolbox presenting it (its sessionOwner())
  // — an ingest belonging to some other panel must not move this one. `owner`
  // is opaque: compared by address only, never dereferenced. `transient` marks
  // a fold the host chose rather than the user (excluded from layout save).
  // No-op once the panel is already pinned, so a second import does not
  // re-fold it.
  void foldTakeoverPanelIfOwnedBy(const void* owner, bool transient);
  // A plugin just created one of its own tabs. When that plugin is the chart-area
  // takeover the tab would stay hidden, so fold the takeover into a transient tab
  // and make `docker` current.
  void revealOwnedTabOverTakeover(const QString& plugin_id, PlotDocker* docker);

  // The toolbox presenting the chart-area takeover; null when the takeover is
  // up but not a toolbox (the marketplace, the filter editor) or absent.
  [[nodiscard]] ToolboxPresenter* takeoverToolbox() const;

  // Asks whether to cancel a running job and close the surface it belongs to;
  // false keeps both. Runs a modal event loop, so callers must re-validate any
  // iterator or pointer they held across it. `label` names the surface.
  bool confirmCancelRunningJob(const QString& label);
  // Non-blocking form for the browser, whose Asyncify-free build cannot run a
  // modal loop: `on_confirmed` runs only if the user chose to cancel, possibly
  // after this returns, so it must re-find anything it acts on.
  void confirmCancelRunningJobAsync(const QString& label, std::function<void()> on_confirmed);

  // Work-in-flight query and cooperative cancel for one panel's host. Both go
  // through `host_work_in_flight_` / `stop_host_work_` when set, which is how a
  // test drives the fold rules without a live parser-ingest context. A null
  // host reads idle and stops nothing.
  [[nodiscard]] bool hostHasWorkInFlight(ToolboxRuntimeHost* host) const;
  void stopHostWork(ToolboxRuntimeHost* host);

  // The same content and plugin connections survive all presentation changes.
  // `apply_chrome(pinned)` re-shows the banner in the chrome of a tab (dock +
  // float + close) or of the docked takeover (tab + float + close).
  struct WrappedToolboxPanel {
    QWidget* container = nullptr;
    std::function<void(bool pinned)> apply_chrome;
  };

  // The callbacks a wrapped toolbox panel's chrome drives. Absent migration
  // callbacks (on_migrate / on_float / on_dock) hide that destination's button;
  // the banner's close button only ever calls on_close, so the close policy
  // (busy fold, tab confirmation) is the caller's.
  struct ToolboxChromeHooks {
    std::function<void()> on_close;
    std::function<void()> on_migrate;
    std::function<void()> on_float;
    std::function<void()> on_dock;
  };

  // Wraps content in host chrome. Docked: tab/float/close; pinned: dock/float/close.
  // Floating uses its own title bar (the float button hides the banner).
  // persist_key identifies the drawer width in QSettings (empty disables persistence).
  WrappedToolboxPanel wrapToolboxPanel(
      QWidget* content, const QString& title, ToolboxChromeHooks hooks, const QString& persist_key = {});

  // Builds the host-service assembly + plugin handle a launched toolbox binds
  // to (null after reporting the failure through `report_error`). Kept apart
  // from launchToolbox so the presentation logic reads on its own; the
  // provider services it shares with HeadlessDescriptorProviderSession::create
  // are built the same way there.
  std::shared_ptr<ToolboxPanelSession> buildToolboxPanelSession(
      const RuntimeToolboxPlugin& toolbox, const QString& plugin_id, const QString& source,
      const std::function<void(const QString&, const QString&)>& report_error);
  // The MCP server, started on a free 127.0.0.1 port when none runs (in the browser, where it
  // does not listen, just started); nullptr when it cannot listen (reported to the diagnostics once
  // per run of failures, not per call).
  McpServer* ensureMcpServer();

  // The registry of live toolboxes, one presenter per plugin id whatever its
  // presentation. registerToolbox requires the id to be absent; teardownToolbox
  // extracts the entry FIRST, then runs the teardown, so a callback that lands
  // mid-teardown finds no presenter instead of a half-dead one, and the
  // session dies with the extracted presenter after the engine stopped.
  [[nodiscard]] ToolboxPresenter* findToolbox(const QString& plugin_id) const;
  // Runs `fn` on the presenter registered under `plugin_id`, or nothing: the
  // by-id lookup every deferred callback does instead of capturing a presenter.
  template <typename Fn>
  void withToolbox(const QString& plugin_id, Fn&& fn) {
    if (ToolboxPresenter* live = findToolbox(plugin_id)) {
      fn(*live);
    }
  }
  [[nodiscard]] ToolboxPresenter* toolboxForContainer(const QWidget* container) const;
  ToolboxPresenter& registerToolbox(const QString& plugin_id, ToolboxPresenter::Spec spec);
  void teardownToolbox(const QString& plugin_id, ToolboxPresenter::Teardown mode);
  // Every live toolbox, every presentation, synchronously — the ~MainWindow
  // path, run while the session, tab strip and services are all still alive.
  void teardownAllToolboxes();

  // Layout persistence of pinned toolbox tabs (NOT part of the undo
  // snapshot; see TabbedPlotWidget::xmlSaveState). savePinnedToolboxes emits
  // <pinned_toolboxes><toolbox plugin_id="...">config-json</toolbox>...</>,
  // skipping transient folds — a background fold is not a workspace choice.
  // A floating toolbox window is saved as a pinned tab too: floating is a
  // session gesture, not a persisted presentation, so the layout restores it
  // as a tab. restorePinnedToolboxes closes every live pinned tab AND every
  // floating toolbox window — and a chart-area takeover the layout names,
  // which would otherwise be folded and re-confirmed on its relaunch — then
  // relaunches from the element (missing plugins surface a diagnostic and are
  // dropped); it returns false when a busy panel kept the live set untouched:
  // under kPrompt the user declined
  // the cancel confirmation, under kRetainAndDiagnose (non-interactive
  // restore — D5's no-dialog rule) the busy panels are retained unprompted
  // and reported through the diagnostic sink.
  [[nodiscard]] QDomElement savePinnedToolboxes(QDomDocument& doc) const;
  bool restorePinnedToolboxes(const QDomElement& root, MissingCurvePolicy policy);

  // The single commit boundary of a layout open: runs what must happen only
  // once every abort/rollback path has returned — replacing the pinned
  // toolbox set with the layout's and re-baselining undo history. Both
  // restore legs (sync applyRestoredLayout, progressive
  // onProgressiveLayoutDrained) end here; restoreChromeAndPanels must NOT
  // grow commit-only steps, it also runs on the abortable stretch. `policy`
  // rides through to restorePinnedToolboxes' busy-panel handling (dialog vs
  // diagnostic).
  void commitRestoredLayout(const QDomDocument& doc, MissingCurvePolicy policy);

  // Constructs + wires (but does not populate) an object-widget dock of the
  // given kind ("scene3d" / "scene2d" / "state_transitions" / "CustomWidget"). Shared by both the drop and the
  // layout-restore paths of the object-widget factory; returns nullptr for an
  // unknown kind.
  IDataWidget* makeSceneDock(const QString& kind, QWidget* parent);

  // makeSceneDock + seed the new dock's playhead to the current time, so a
  // freshly built empty dock renders at the right moment (currentTimeChanged
  // only fires on changes). Shared by the factory's restore branch and the
  // click-to-create path. Returns nullptr for an unknown kind.
  IDataWidget* makeSeededEmptyObjectDock(const QString& kind, QWidget* parent);

  // The EmbeddedViewHost handed to plugin panels (`scene_view` frames): seeded
  // scene widgets of the kind asked for, attached through the plugin scene tabs'
  // own path (PluginSceneViewsController). Empty when this build has no scene
  // family, so the panel then announces no scene-view capability.
  EmbeddedViewHost makeEmbeddedViewHost();
  // The views it built. They are not ADS docks, so forEachDock never reaches
  // them; the tracker broadcasts tick them here, and QPointer drops the ones
  // that died with their panel.
  QList<QPointer<SceneDockWidget>> embedded_scene_views_;
  // After a catalog change: every topic `view` attached whose layer is gone is attached
  // again, under the id it now has, or recorded as evicted (0) so a detach skips it.
  void reattachEmbeddedTopics(SceneDockWidget& view);
  void tickEmbeddedViews(double display_seconds, bool visible_only);

  Ui::MainWindow* ui_;
  QtDiagnosticBridge* diagnostic_bridge_ = nullptr;
  DiagnosticHistory* diagnostic_history_ = nullptr;
  // Bell dialog, created on first click and reused (modeless).
  DiagnosticsDialog* diagnostics_dialog_ = nullptr;
  ToastManager* toast_manager_ = nullptr;
  UpdateChecker* update_checker_ = nullptr;
  QString installation_;
  TelemetryPing* telemetry_ping_ = nullptr;
  // Outcome handlers are rebound on each checkForUpdates() call so the check's
  // interactivity (silent startup vs. noisy Help ▸ Check for Updates) is captured
  // per-request rather than living in shared mutable state.
  QMetaObject::Connection update_available_conn_;
  QMetaObject::Connection up_to_date_conn_;
  QMetaObject::Connection check_failed_conn_;
  // Registry fetcher for the startup extension-update scan (separate instance
  // from the Marketplace window's own, so neither aborts the other's fetch).
  RegistryManager* update_scan_registry_ = nullptr;
#ifndef PJ_TARGET_WASM
  // Backs "pj.feature_catalog.v1" for every toolbox session. Declared before
  // session_ and the toolbox registry, so it outlives every plugin bound to it.
  std::unique_ptr<FeatureCatalogHost> feature_catalog_;
#endif
  // The MCP server, from the first need (public access or a plugin's endpoint) until ~MainWindow
  // destroys it, after the toolboxes (whose endpoints revoke their tokens) and before the services
  // its agent sessions point into.
  std::unique_ptr<McpServer> mcp_server_;
  // A failed listen was reported and no start has succeeded since.
  bool mcp_listen_failure_reported_ = false;
  // Status-bar indicator of AI agent activity: names the active MCP client and counts the jobs
  // agents have running, and opens mcp_jobs_dialog_. Created on first use; shown while a client is
  // active or a job runs.
  QToolButton* mcp_agent_button_ = nullptr;
  QString mcp_active_client_;
  // The user's view of running agent jobs (Stop per job); created on first use, a child of this.
  McpJobsDialog* mcp_jobs_dialog_ = nullptr;
  // Refreshes mcp_jobs_dialog_ once a second while it is open; a child of it.
  QTimer* mcp_jobs_refresh_ = nullptr;
  // Brings mcp_agent_button_ and an open mcp_jobs_dialog_ in line with the server's state.
  void refreshMcpIndicator();
  void showMcpJobs();
  // The relay link connectMcpRelay made (a child of mcp_server_, so it goes with it; its destructor
  // touches nothing of the server); null for none. While set the browser skips its welcome dialog.
  QPointer<QObject> mcp_relay_link_;
  QAction* undo_action_ = nullptr;
  QAction* redo_action_ = nullptr;
  // App-wide QSettings instance injected into each Scene3DDockWidget
  // (setSettings) for URDF package-resolver persistence — per-source package
  // mappings and global search roots. Owned here so every dock shares one
  // instance whose lifetime outlasts them all.
  std::unique_ptr<QSettings> app_settings_;
  std::unique_ptr<AppSession> session_;
  std::unique_ptr<PendingDisplayBinder> pending_binder_;
  // Resolves displayed plots/scene docks to (DatasetId, topic_name) demand
  // references (see TopicDemandTracker). Declared after pending_binder_, which
  // it forwards placeholder scalar drops to.
  std::unique_ptr<TopicDemandController> topic_demand_controller_;
  // The plugin-composed plot tabs (pj.plot_tabs.v1 / pj.viewport.v1); builds the
  // per-plugin bridge callbacks launchToolbox hands to the runtime hosts.
  std::unique_ptr<PluginPlotTabsController> plugin_plot_tabs_;
  // Bridges progressive ingest signals from SessionManager to CurveTreeView
  // via CurveListPanel. Maintains linger/flash timers and ghost-row lifetime.
  std::unique_ptr<IngestProgressController> ingest_progress_controller_;
  // Owns the per-dataset TF buffers + load-time ingest. Declared after session_
  // so it is destroyed first (it holds a reference into session_).
#ifdef PJ_WITH_SCENE3D
  std::unique_ptr<PJ::TransformService> transform_service_;
  // The 3D docks' shared "last manual fixed frame" preference.
  std::unique_ptr<pj::scene3d::FixedFrameMemory> fixed_frame_memory_;
#endif
  std::unique_ptr<FileLoader> file_loader_;
  std::unique_ptr<StreamingSourceManager> streaming_manager_;
  // Declared AFTER streaming_manager_ on purpose: members die in reverse order,
  // so the runtime hosts holding this service's taps outlive it. Finalization
  // does not rely on that ordering — closeEvent and ~MainWindow both stop() the
  // recording explicitly, before any widget or host is gone. Always created,
  // even where isSupported() is false: only the UI affordances go away there.
  std::unique_ptr<RecordingService> recording_service_;
#ifndef __EMSCRIPTEN__
  // M3 transparent source cache (desktop only), constructed ONCE at startup
  // from Preferences (SourceCacheStore::loadSettings) — a changed cache
  // folder/budget takes effect on the next launch, because armed captures
  // hold the store they were opened against. store first: the service holds a
  // reference into it.
  std::unique_ptr<SourceCacheStore> source_cache_store_;
  std::unique_ptr<SourceCaptureService> source_capture_service_;
#endif
  // Wall clock behind the "m:ss | N.N MB" status label; restarted on every start().
  QElapsedTimer recording_elapsed_;
  // ~30 Hz rate cap for the tracker-time fan-out. Every per-cursor-move driver
  // (playback ticks, scrubbing, seeks) routes through one coalescer so the
  // expensive per-widget work (plot replot, scene decode/composite, value column)
  // runs at most ~30 Hz regardless of how fast the cursor changes. Structural
  // seeds (load/restore/undo) call broadcastTrackerTime() directly, unthrottled,
  // so a freshly shown dock is never blank waiting for the trailing edge.
  // pending_tracker_time_ holds the newest cursor time the trailing edge will use.
  std::unique_ptr<CoalescingTrigger> tracker_broadcast_trigger_;
  double pending_tracker_time_ = 0.0;
  // Binds the Source Timeline widget (mounted in timelineStrip) to the runtime.
  // Parented to this MainWindow (QObject-owned), so it is a raw pointer.
  SourceTimelineController* source_timeline_controller_ = nullptr;
  // The Source Timeline widget itself (QObject-owned by timelineStrip). Held so
  // the reference-line toggle can drive its blue reference needle.
  Timeline* source_timeline_ = nullptr;
  // User's chosen name-column width (px); 0 = none, use the playback-aligned
  // floor. Tracked from Timeline::nameColumnWidthChanged and restored from a
  // layout so alignNameColumnToPlayback keeps the column at this width.
  int timeline_name_column_width_ = 0;
  // Active streaming dataset id while a session is live (0 = none).
  // Scopes the playback slider range to this dataset's data only so unrelated
  // file/scalar timestamps in the global store don't stretch the slider into
  // ranges where no streamable data exists.
  DatasetId active_streaming_dataset_id_ = 0;
  // Flips to true the first time a streaming topic is dropped into a view —
  // a scalar curve into a plot (PlotWidget::curvesDropped) or an object topic
  // into a 2D/3D dock (PlotDocker::firstObjectTopicAdded) — which seeds the
  // playback range + playhead. Until then the slider is left untouched so merely
  // subscribing to topics in the source dialog does not move it. Seeding is
  // one-shot per session (seedStreamingPlaybackFromDrop early-returns once set),
  // so a later drop can't re-snap a paused, scrubbed-back cursor. While true,
  // live ingest tracks the live edge until the user pauses.
  bool streaming_playback_seeded_ = false;
  bool plugin_failure_toast_shown_ = false;
  // While non-null, every "Layout" diagnostic is also appended here, so a
  // failed restore can tell the user WHY instead of pointing at the bell.
  QStringList* layout_issue_capture_ = nullptr;
  std::unique_ptr<Theme> theme_;
  TitleBar* title_bar_ = nullptr;
  // Fixed for the window's lifetime: the native window is created frameless or not.
  const bool custom_frame_ = usesCustomFrame();
  // Non-modal load progress strip parked in the title bar's center region (owned
  // by title_bar_ once injected). Shown after a short delay so quick loads don't
  // flash it; hidden a moment after the load queue drains.
  IngestProgressWidget* ingest_progress_ = nullptr;
  QTimer* ingest_show_timer_ = nullptr;
  // Restartable single-shot linger before the strip hides after the last load /
  // toolbox import drains; restarted (not re-minted) from each drain site so
  // overlapping finishes coalesce into one hide.
  QTimer* ingest_hide_timer_ = nullptr;
  // Owned by QObject parentage while open; guards against stacking multiple
  // stop confirmations from repeated title-bar clicks.
  QPointer<MessageBox> ingest_stop_dialog_;
  // Toolbox bulk-import STOP ROUTING only (the second producer of
  // ingest_progress_, arbitration: FileLoader wins a collision). The lifecycle
  // bookkeeping — which datasets are actively importing, label/progress, and
  // the "still growing" query the on_data_changed TF bridge uses — lives in
  // SessionManager (beginIngest/updateIngest/endIngest, #470 hoist); this hash
  // keeps only what stop routing needs and mirrors that lifecycle: one entry
  // per import dataset between on_ingest_started and on_ingest_finished (host
  // teardown / release fire finished for anything the plugin left open, so
  // pairing holds). `owner` weak-guards the ToolboxPanelSession whose
  // ToolboxRuntimeHost runs the import — a closed panel can never be
  // stop-routed into freed memory; `host` is dereferenced only after the owner
  // check succeeds. `toolbox_ingest_label_`/`toolbox_ingest_dataset_` name the
  // last-STARTED interactive import — the identity a fresh strip adopt (or a
  // re-adopt once the file queue drains) shows; cleared when that import ends.
  struct ToolboxIngestRef {
    IngestToken token;
    std::weak_ptr<void> owner;
    PJ::ToolboxRuntimeHost* host = nullptr;
  };
  QHash<PJ::DatasetId, ToolboxIngestRef> toolbox_active_imports_;
  QString toolbox_ingest_label_;
  DatasetId toolbox_ingest_dataset_ = 0;

  // D8: the strip's DISPLAYED OWNER — whose title/progress the widget shows,
  // so Stop routes to exactly that producer and an ending producer hands the
  // strip over instead of leaving stale text. The arbitration rule
  // (deterministic):
  //   * a file load always wins while FileLoader is busy (unchanged);
  //   * a newly BEGUN toolbox ingest takes the strip (last-started wins,
  //     unchanged), EXCEPT the layout batch's ingest never displaces a
  //     displayed interactive one — a background restore must not steal the
  //     strip from the user's own import;
  //   * when the displayed ingest ends (or its batch dies), the strip
  //     switches to a surviving eligible ingest with its recorded
  //     label/progress — the last-started interactive if still active, else
  //     the lowest surviving interactive DatasetId (engine ids are monotonic,
  //     so lowest = first-created), else the batch job's; with no survivor
  //     the normal linger-hide runs.
  enum class IngestStripOwnerKind { kNone, kFile, kInteractiveToolbox, kLayoutBatch };
  IngestStripOwnerKind ingest_strip_owner_kind_ = IngestStripOwnerKind::kNone;
  DatasetId ingest_strip_owner_dataset_ = 0;  ///< set for the two ingest kinds only

  // T7 batch observation (D1): connections to the three SessionManager ingest
  // lifecycle signals, installed ONLY while a handed-over batch lives and torn
  // down (with ownership release) strictly before the batch is destroyed.
  // `layout_batch_strip_dataset_` is the batch job's begun-and-not-ended
  // ingest (0 = none), recorded by exact equality with the batch's announced
  // activeImportDataset() — the classification key the arbitration uses.
  std::vector<QMetaObject::Connection> layout_batch_ingest_conns_;
  DatasetId layout_batch_strip_dataset_ = 0;
  // Help ▸ Installed Extensions — informational, rebuilt on aboutToShow.
  QMenu* installed_extensions_menu_ = nullptr;
  // Local-panel header bands (grey "Curve Width" / "Curve Style" labels).
  // Kept as members so build_section's findChild lookups for the
  // exclusive radio buttons have a stable parent to query.
  QWidget* curve_width_header_ = nullptr;
  QWidget* curve_style_header_ = nullptr;
  // Curve-style + Curve-width buttons each form an exclusive radio-style
  // group (one checked at a time; defaults: "Lines" / 1.0 px). The group
  // owns no widgets — it just enforces the mutual-exclusion semantics on
  // the existing toolbar buttons.
  QButtonGroup* style_button_group_ = nullptr;
  QButtonGroup* width_button_group_ = nullptr;

  // Stored so applyIcons() can re-tint them on theme change.
  QAction* action_load_layout_ = nullptr;
  QAction* action_save_layout_ = nullptr;
  QAction* action_open_data_ = nullptr;
  QAction* action_preferences_ = nullptr;
  // Byte-level layout loads (browser uploads and recipes, loadLayoutFromUrl on
  // every platform) and their source replay. Desktop file loads never use it.
  struct BrowserLayoutRuntime;
  std::unique_ptr<BrowserLayoutRuntime> browser_layout_runtime_;
  QAction* action_save_source_layout_ = nullptr;  // browser build only
#ifdef PJ_TARGET_WASM
  // Created once the page's preset index lists at least one preset; a child of
  // this window, hidden between uses.
  WelcomePresetsDialog* presets_dialog_ = nullptr;
#endif
  // A byte-level layout load is in flight: layout actions stay disabled and a
  // second one is refused until finishBrowserLayoutLoad.
  bool layout_selection_pending_ = false;
  // The page this browser build was opened from (setDeploymentPageUrl); fills a
  // replayed stream's deployment variables. Empty on the desktop, where a
  // configuration that names one fails with "missing deployment context".
  QUrl deployment_page_url_;
  std::deque<CapturedWorkspace> undo_states_;
  std::deque<CapturedWorkspace> redo_states_;
  QSet<QString> history_data_universe_;
  QElapsedTimer undo_timer_;
  bool applying_state_ = false;
  bool progressive_layout_in_flight_ = false;
  // The progressive binder's itemsAdded retry channel. Torn down with the
  // restore on every arm EXCEPT a kRetainAndDiagnose drain, which leaves it
  // alive as D5's retention channel (retained intents bind when their data
  // arrives later — main_window_layout_import_binder_test pins this
  // mid-import) until the next restore or resetLayoutImportBatch supersedes
  // it.
  QMetaObject::Connection pending_items_added_conn_;
  // One reason the progressive drain must keep waiting, plus the connection
  // that re-runs the gate when that reason may have cleared. The gate
  // (maybeSettleProgressiveRestore) proceeds only once no waiter is pending;
  // FileLoader registers isBusy() on queueDrained, the batch registers
  // !isFinished() on finished — and that batch waiter doubles as the drain
  // signal for the T7 mid-import binding (no third waiter exists).
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
  // The drain-time policy for unresolved intents, captured at
  // beginProgressiveLayoutRestore from the SAME batch-interactivity decision
  // the whole restore rides (D5: policy is data captured up front, never a
  // late read of live batch state).
  MissingCurvePolicy progressive_missing_curve_policy_ = MissingCurvePolicy::kPrompt;
  bool pending_binding_rebuild_scheduled_ = false;
  // Trailing-edge coalescer for scene-dock workspaceChanged: layer/view
  // scrubbers emit per drag tick, and each push would serialize the whole
  // workspace — one capture fires when the gesture goes quiet (the plot-side
  // twin lives in the emitting widgets; scene docks have too many emitters,
  // so the shell debounces its one subscription instead).
  QTimer scene_undo_debounce_;
  // Timeline state (per-source offsets + track order) extracted during a progressive
  // restore but not yet applicable: the async worker had not registered the reloaded
  // datasets' source paths when restoreChromeAndPanels ran, so the offsets were
  // skipped. onProgressiveLayoutDrained re-applies these once the paths settle. Empty
  // outside a progressive restore.
  QList<layout_xml::DataSourceRef> pending_timeline_sources_;
  // The saved target must outlive begin so processors can replay only at drain.
  QDomDocument progressive_layout_doc_;
  std::optional<CapturedWorkspace> progressive_previous_workspace_;
  std::optional<PendingSourceReplacement> pending_source_replacement_;
  // The layout-import transaction owner (§6.2): non-null only from a restore
  // that found materialize-bearing sources until its drain settles. Declared
  // AFTER session_/file_loader_ so its destructor (which reaches both) runs
  // first. Ordinary layouts never construct one.
  std::unique_ptr<LayoutImportBatch> layout_import_batch_;
  // One-shot handover of the batch's workspace checkpoint into
  // beginProgressiveLayoutRestore, so the batch hook's capture and the
  // progressive snapshot are ONE capture: set by the
  // begin_workspace_checkpoint hook, consumed (or dropped) at begin.
  std::shared_ptr<const CapturedWorkspace> batch_workspace_checkpoint_;
  // True between enableAutoplay() and the first range-driven playback start (the
  // --autoplay one-shot); cleared once playback begins so user control is respected.
  bool autoplay_pending_ = false;
  // One-shot guard so the remembered left-panel splitter width is restored only on
  // the first showEvent (later shows must not clobber a user/layout adjustment).
  bool left_splitter_restored_ = false;
  bool bottom_panel_sized_ = false;  // first-show guard for the bottom-panel sizing
  // Lives inside localToolbarWidget; visibility piggybacks on the
  // right-panel toggle in the tab strip.
  CurveEditor* curve_editor_ = nullptr;

  // Right-sidepanel content swap: the stack hosts a plot-config page
  // (Curve Width / Style strips + CurveEditor) and per-family pages
  // for 2D and 3D scenes. onDockFocused() picks the active page from
  // the focused DockWidget's content type.
  QStackedWidget* right_panel_stack_ = nullptr;
  QWidget* plot_config_page_ = nullptr;
#ifdef PJ_WITH_SCENE2D
  QWidget* scene2d_config_page_ = nullptr;
#endif
#ifdef PJ_WITH_SCENE3D
  QWidget* scene3d_config_page_ = nullptr;
#endif
  // Concrete widget instance behind scene3d_config_page_; held as a
  // distinct member so onDockFocused() can call bindDock() on it
  // without an extra qobject_cast.
#ifdef PJ_WITH_SCENE3D
  class Scene3DConfigPanel* scene3d_config_panel_ = nullptr;
#endif
  // Same, for the 2D scene's layer panel (binds to the focused Scene2DDockWidget).
#ifdef PJ_WITH_SCENE2D
  class Scene2DConfigPanel* scene2d_config_panel_ = nullptr;
#endif
  // Shown when the focused dock holds the VisualizationPlaceholderWidget —
  // nothing to configure yet.
  QWidget* empty_dock_page_ = nullptr;
  // Custom Widget picker and config pages plus the active-widgets record. Owned by
  // right_panel_stack_ (QObject parent); the record is cleared in ~MainWindow once the docks are gone.
  CustomWidgetController* custom_widgets_ = nullptr;

  // Active toolbox panel presented in place of the chart area by
  // presentPanel()/restoreCentralArea(). At most one at a time.
  // current_panel_engine_ is the owning PanelEngine when the panel is a
  // toolbox dialog (null for non-toolbox panels like the console / filter
  // editor); presentPanel() uses it to tear the previous toolbox down before
  // showing a new one, so launching a toolbox replaces the open panel.
  QWidget* current_panel_ = nullptr;
  PanelEngine* current_panel_engine_ = nullptr;
  // The marketplace controller while its panel holds the central area, so a
  // second open request re-activates it instead of building a second one (the
  // title-bar Update button and the File menu both route to onOpenMarketplace).
  // Cleared by the controller's own destruction, whichever exit the panel takes.
  //
  // Paired with `marketplace_container_` — the guard has to distinguish
  // "marketplace IS the central panel" from "marketplace was migrated to a tab
  // and some other panel now sits centrally": both leave `marketplace_panel_`
  // non-null, but only the first should re-activate. `current_panel_ ==
  // marketplace_container_` is the exact test.
  QPointer<MarketplaceWindow> marketplace_panel_;
  QPointer<QWidget> marketplace_container_;

  // Every live toolbox, keyed by plugin id (one instance per toolbox across
  // takeover, tabs and floating windows alike). Each presenter owns its
  // plugin session, so this is declared after session_: the entries are torn
  // down in ~MainWindow and would in any case die before the AppSession they
  // write into. unique_ptr: presenters are addressed by pointer from Qt
  // callbacks and must not move.
  std::map<QString, std::unique_ptr<ToolboxPresenter>> toolboxes_;

  // Test seams for the fold rules. `confirm_running_job_` answers
  // confirmCancelRunningJob without a modal; the other two stand in for a
  // panel host's work-in-flight query and cooperative cancel, which a test
  // cannot otherwise drive (an ingest context needs a real parser plugin).
  // All unset in production.
  std::function<bool(QString)> confirm_running_job_;
  std::function<bool(ToolboxRuntimeHost*)> host_work_in_flight_;
  std::function<void(ToolboxRuntimeHost*)> stop_host_work_;
  // The plot a Filter Editor panel was opened on. Its style/width drive the
  // before/after preview (the preview mirrors THAT plot, not a global default).
  // Set after presentPanel() succeeds; cleared in restoreCentralArea(). QPointer so
  // a torn-down origin reads back null. Only non-null while a FilterEditorPanel is up.
  QPointer<PlotWidget> filter_editor_origin_;

  // Global-column "Chart" icons — built in buildGlobalToolbar(), so
  // stored as member pointers (no ui_-> accessor).
  QToolButton* button_link_ = nullptr;
  SvgButton* button_time_tracker_ = nullptr;
  QToolButton* button_show_point_ = nullptr;
  QToolButton* button_grid_ = nullptr;
  QToolButton* button_zoom_out_ = nullptr;
  QToolButton* button_ratio_ = nullptr;
  QToolButton* button_dots_ = nullptr;
  QToolButton* button_reference_point_ = nullptr;
  // Global-column "Legend" button — single icon that combines a corner
  // picker (left-click) with a show/hide toggle (right-click). Checked
  // while the legend is shown at one of the four corners; unchecked
  // when hidden. The icon always reflects the "current position": the
  // active corner while checked, or the saved corner that will be
  // restored on the next show while unchecked.
  SvgButton* button_legend_ = nullptr;  // SvgButton so setLegendStatus can setIconPath() the corner glyph
  // Saved corner used while the legend is hidden. Updated on every
  // visit to a corner so right-click → show restores the user's last
  // position rather than always jumping back to a fixed default.
  LegendStatus previous_legend_corner_ = LegendStatus::kTopRight;

  // Global-column view toggles. Persisted to QSettings; XML
  // round-tripped; show_points_ applies to plots and 2D image viewers.
  LegendStatus legend_status_ = LegendStatus::kTopRight;
  bool show_points_ = false;
  bool activate_grid_ = false;
  // Derived from kDateTimeDisplayKey + kClockUtcKey by applyTimeLabelFormat();
  // new plots pick it up in applyGlobalToggles.
  TimeLabelFormat time_label_format_ = TimeLabelFormat::kSeconds;
  bool dots_ = false;

  // A refreshCustomTopicsPanel is queued by scheduleCustomTopicsSync and has not run yet.
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

  // Loaded from QSettings before any child widget is built so the first
  // applyIcons() of each widget already uses the saved metrics.
  ChromeMetrics chrome_metrics_;
  // Three-state cycle for the playback tracker info level (line / +value /
  // +value+name). Default kValue matches PJ3 (mainwindow.cpp:154).
  CurveTracker::Parameter tracker_info_ = CurveTracker::kValue;
  // Session-only — PJ3 doesn't persist this either. Set at toggle-ON to the
  // playback instant; tracker renders Δ values until cleared. Stored as a
  // frame-invariant absolute Timepoint (NOT a display coordinate) so the blue
  // line holds its instant when the display offset shifts — the "Use time
  // offset" toggle, or a load that lowers a dataset's earliest stamp. The axis
  // position is derived on demand by referenceDisplaySeconds().
  std::optional<Timepoint> reference_instant_;
  // PJ3 parity: 1:1 aspect is the expected default for XY plots.
  bool keep_ratio_ = true;
};

}  // namespace PJ
