// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "MainWindow.h"

#include <qwt_plot_curve.h>
#include <qwt_text.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QByteArray>
#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QLoggingCategory>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <unordered_set>
#include <utility>

#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
#include <emscripten/emscripten.h>

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#endif
#include <vector>

#include "BuiltinPlugins.h"
#ifdef PJ_TARGET_WASM
#include "BrowserFileStore.h"
#include "BrowserPersistence.h"
#endif
#include "DatasetMergeActions.h"
#include "DebugUi.h"
#include "FileLoader.h"
#include "IngestProgressController.h"
#include "IngestProgressWiring.h"
#ifdef PJ_TARGET_WASM
#include "FanoutConfig.h"
#include "FileSelectionService.h"
#endif
#include "LayoutImportBatch.h"
#include "LayoutReplayHints.h"
#include "LayoutXml.h"
#include "PendingDisplayBinder.h"
#include "PreferencesDialog.h"
#include "RasterKeyMap.h"
#include "SourcePromotionHost.h"
#include "SourceTimelineController.h"
#include "StreamingSourceManager.h"
#include "Theme.h"
#include "TitleBar.h"
#include "ToolboxHostWiring.h"
#include "TopicDemandController.h"
#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/dataset.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/data_processor.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_marketplace/extension_manager.hpp"
#ifndef PJ_TARGET_WASM
#include "pj_marketplace/marketplace_window.hpp"
#endif
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_marketplace/registry_manager.hpp"
#include "pj_plotting/CurveEditor.h"
#include "pj_plotting/CurveTracker.h"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/FilterEditorPanel.h"
#include "pj_plotting/PlotDocker.h"
#ifdef PJ_TARGET_WASM
#include "pj_plotting/PlotRhiCanvas.h"
#endif
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/StateTransitionsDockWidget.h"
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
#include "pj_plotting/XYCurveDialog.h"
#include "pj_widgets/LayerListView.h"
#include "pj_widgets/RealSlider.h"
#endif
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_plugins/host/dialog_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_plugins/host/toolbox_handle.hpp"
#include "pj_plugins/host_qt/panel_engine.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/DataProcessorsKindRouter.h"
#include "pj_runtime/DataProcessorsRuntimeHost.h"
#include "pj_runtime/DiagnosticHistory.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/IObjectViewer.h"
#include "pj_runtime/MarkerService.h"
#include "pj_runtime/MarkersRuntimeHost.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/QSettingsBackend.h"
#include "pj_runtime/RecordingService.h"
#include "pj_runtime/SessionManager.h"
#ifndef __EMSCRIPTEN__
#include "pj_runtime/SourceCacheStore.h"
#include "pj_runtime/SourceCaptureService.h"
#endif
#include "pj_runtime/TelemetryPing.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/ToolboxRuntimeHost.h"
#include "pj_runtime/TopicDemandTracker.h"
#include "pj_runtime/UpdateChecker.h"
#ifdef PJ_WITH_SCENE2D
#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#include "pj_scene2d_widgets/media_viewer_widget.h"
#endif
#ifdef PJ_WITH_SCENE3D
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/transform_service.h"
#endif
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_widgets/CoalescingTrigger.h"
#include "pj_widgets/FileDialog.h"
#include "pj_widgets/FlowLayout.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/IngestProgressWidget.h"
#include "pj_widgets/MessageBox.h"
#ifndef PJ_TARGET_WASM
#include "pj_widgets/RasterStreamView.h"
#endif
#include "pj_widgets/Scrollbar.h"
#include "pj_widgets/SectionHeaderBand.h"
#include "pj_widgets/SvgButton.h"
#include "pj_widgets/SvgUtil.h"
#include "pj_widgets/Timeline.h"
#include "pj_widgets/ToastManager.h"
#include "scene_object_classification.h"
#include "ui/AboutDialog.h"
#include "ui/CurveListPanel.h"
#include "ui/DiagnosticsDetailDialog.h"
#include "ui/LeftPanel.h"
#ifdef PJ_WITH_SCENE2D
#include "ui/Scene2DConfigPanel.h"
#endif
#ifdef PJ_WITH_SCENE3D
#include "ui/Scene3DConfigPanel.h"
#endif
#include "ui/TimelineWidget.h"
#include "ui_MainWindow.h"
using namespace Qt::StringLiterals;

#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
#include "../tests/wasm_acceptance_probes.inc"
#ifdef PJ_WITH_SCENE3D
#include "../tests/wasm_scene3d_acceptance_probes.h"
#endif
#endif

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcMain, "pj.app.main")

// Check the button with id `id` in an exclusive QButtonGroup as a passive UI
// resync, with the group's signals blocked. The block is load-bearing: a passive
// resync (e.g. binding the toolbar to a newly-focused plot) must not run the Curve
// Width/Style groups' click side effects — refreshing the open preview panels.
void checkGroupButton(QButtonGroup* group, int id) {
  if (group == nullptr) {
    return;
  }
  if (auto* btn = group->button(id)) {
    const QSignalBlocker blocker(group);
    btn->setChecked(true);
  }
}

// Layout-relative form of `absolute_path` for persisting a data-source
// reference: the relative path when the data lives at or beneath `layout_dir`,
// else the absolute path. This diverges from PJ3 (which always stores relative)
// — PJ4 avoids brittle ../.. paths so moving a layout file doesn't silently break
// the data reference. A relative path counts as a "subpath" only when Qt's
// relativeFilePath did NOT emit a "../" prefix or the literal ".." path; the
// simpler `!rel.startsWith("..")` check would misclassify legitimate filenames
// like "..foo" or "..bar/data.csv" as escaping the dir.
[[nodiscard]] QString relocatableSubpath(const QString& absolute_path, const QDir& layout_dir) {
  const QString relative = layout_dir.relativeFilePath(absolute_path);
  const bool is_subpath = relative != ".."_L1 && !relative.startsWith("../"_L1);
  return is_subpath ? relative : absolute_path;
}

constexpr auto kDefaultRegistryUrl =
    "https://raw.githubusercontent.com/PlotJuggler/pj-plugin-registry/"
    "refs/heads/development/registry.json";
constexpr auto kRegistryUrlSettingsKey = "Marketplace/registryUrl";
constexpr auto kPanelBottomExpandedKey = "MainWindow.panelBottomExpandedHeight";
// Minimum height (px) of the Source Timeline strip when the bottom panel is open
// — enough for the ruler + a few source bars so it never opens clipped.
constexpr int kMinTimelineStripHeight = 150;

// Directory of the most recently saved or loaded layout. Re-using PJ3's
// QSettings key keeps cross-version migration trivial (a user who
// upgrades from PJ3 lands in their existing layout directory). Fallback
// when unset is QDir::currentPath() — matches PJ3.
#ifndef PJ_TARGET_WASM
constexpr auto kLastLayoutDirKey = "MainWindow.lastLayoutDirectory";
#endif

// Layout schema version. Bumped only on incompatible changes (additive
// elements/attributes don't need a bump — the loader silently skips
// unknown content). Read by loadLayoutFromPath to flag layouts saved
// by newer PJ4 builds; the layout still loads best-effort.
// v2: curves identified by stable topic+field path (rebound per-dataset on
// load) instead of the opaque per-load catalog key; <root binding=...> marks
// generic vs source-bound layouts.
// v3: per-plot <range> stores the X (time) axis in ABSOLUTE seconds — the per-dataset
// display offset is a visualization concern applied at load, never persisted. No marker
// is written; the plot mode (time-series vs XY) decides on load whether to undo the
// offset, and an unmarked legacy range is also read as absolute. The global toolbar
// toggles + panel visibility are no longer serialized (they are QSettings-only app
// preferences, not document state).
// v3 also round-trips the Source Timeline. Per-source: each <fileInfo> carries
// display_offset_ns + timeline_order, re-bound by source path so a dataset's bar
// offset and vertical slot restore exactly. Global view chrome: a <source_timeline>
// element carries zoom + horizontal/vertical scroll + name_column_width + snap. Additive — older
// readers ignore the new attributes/element; this build tolerates their absence in
// pre-v3 layouts.
// v4 adds one <dataset> child per fan-out member under each <fileInfo>
// (source_name + source_index + per-source display_offset_ns + timeline_order),
// so a single file that fans out into several datasets round-trips each track
// independently. The offset basis also changed: v3 wrote SessionManager::
// displayOffset() (per-source alignment + global reference); v4 writes
// sourceDisplayOffset() only, and the loader subtracts its current global
// reference when reading a <=v3 layout (read-only migration, placement preserved).
constexpr int kLayoutSchemaVersion = 4;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr int kTestSampleCount = 1000;
constexpr double kTestDurationSeconds = 10.0;
constexpr int kResizeMargin = 6;
// ~30 Hz cap for the tracker-time fan-out (plots/scenes/value column). Kept equal
// to PlaybackEngine's kTickIntervalMs (30 Hz) so playback passes through ~1:1; the
// coalescer's job is to additionally cap faster drivers (scrubbing at mouse-move
// rate, programmatic seeks) that bypass the playback tick.
constexpr int kTrackerBroadcastIntervalMs = 33;
// Anti-flash delay before the title-bar ingest strip appears — quick loads that
// finish inside this window never flash it. WASM shortens this for deterministic
// cancellation fixtures (PJ_WASM_ENABLE_INGRESS_PROBE below).
constexpr int kIngestStripShowDelayMs = 500;
constexpr int kIngestStripShowDelayWasmMs = 50;
// Linger after the last load/import drains before the strip hides.
constexpr int kIngestStripHideLingerMs = 1000;
// Progress-bar resolution: byte counts exceed QProgressBar's int range, so the
// bar tracks progress as this-many permille of total rather than raw counts.
constexpr int kIngestProgressResolution = 1000;
// Per-section cap for the recent popup — at most this many Layouts AND this
// many Files are retained (the two lists are independent).
constexpr int kMaxRecentEntries = 8;
constexpr auto kRecentLayoutsKey = "Layout/recent";
constexpr auto kLayoutFilter = "PlotJuggler 4 Layout (*.pj4.xml)";
// Extension itself is the single source of truth in LayoutXml::kLayoutExtension.
constexpr int kMaxUndoStates = 100;
constexpr qint64 kUndoCoalesceMs = 100;
constexpr auto kIconSizeKey = "ui/icon_size";
constexpr auto kIconPaddingKey = "ui/icon_padding";
constexpr auto kLayoutPaddingKey = "ui/layout_padding";
constexpr auto kLayoutSpacingKey = "ui/layout_spacing";
constexpr Range<int> kIconSizeRange{12, 48};
constexpr Range<int> kIconPaddingRange{0, 32};
constexpr Range<int> kLayoutPaddingRange{0, 16};
constexpr Range<int> kLayoutSpacingRange{0, 16};
constexpr int kIconSizeDefault = 24;
constexpr int kIconPaddingDefault = 4;
constexpr int kLayoutPaddingDefault = 2;
constexpr int kLayoutSpacingDefault = 2;

#ifdef PJ_TARGET_WASM
void showBrowserLayoutWarning(QWidget* parent, const QString& title, const QString& text) {
  // The browser main thread cannot enter MessageBox::warning's nested exec().
  // Parent ownership plus WA_DeleteOnClose gives the same application-modal
  // presentation without borrowing stack state across the async lifetime.
  auto* dialog = new MessageBox(parent);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowModality(Qt::ApplicationModal);
  dialog->setTitle(title);
  dialog->setText(text);
  dialog->addButton(QObject::tr("OK"), MessageBox::kPrimaryRole);
  dialog->show();
}
#endif

// Scene scrubber drags emit workspaceChanged per tick; one history snapshot
// publishes after the gesture goes quiet.
constexpr int kSceneUndoDebounceMs = 200;

Qt::Edges edgesAtPoint(const QSize& window_size, const QPoint& pos) {
  Qt::Edges edges;
  if (pos.x() <= kResizeMargin) {
    edges |= Qt::LeftEdge;
  } else if (pos.x() >= window_size.width() - kResizeMargin) {
    edges |= Qt::RightEdge;
  }
  if (pos.y() <= kResizeMargin) {
    edges |= Qt::TopEdge;
  } else if (pos.y() >= window_size.height() - kResizeMargin) {
    edges |= Qt::BottomEdge;
  }
  return edges;
}

Qt::CursorShape cursorForEdges(Qt::Edges edges) {
  switch (static_cast<int>(edges)) {
    case Qt::TopEdge | Qt::LeftEdge:
    case Qt::BottomEdge | Qt::RightEdge:
      return Qt::SizeFDiagCursor;
    case Qt::TopEdge | Qt::RightEdge:
    case Qt::BottomEdge | Qt::LeftEdge:
      return Qt::SizeBDiagCursor;
    case Qt::TopEdge:
    case Qt::BottomEdge:
      return Qt::SizeVerCursor;
    case Qt::LeftEdge:
    case Qt::RightEdge:
      return Qt::SizeHorCursor;
    default:
      return Qt::ArrowCursor;
  }
}

struct PanelToggle {
  QPushButton* button;
  QWidget* target;
  const char* icon_path_on;   // filled glyph — panel visible.
  const char* icon_path_off;  // unfilled glyph — panel hidden.
};

std::array<PanelToggle, 3> panelToggles(Ui::MainWindow* ui) {
  // Material's "Dock to Left" / "Dock to Right" glyphs fill the half
  // of the frame opposite to the side they nominally dock toward, so
  // the left-panel toggle reads correctly with the panel_right.svg
  // asset and vice versa.
  return {{
      {ui->tabbedPlotWidget->leftPanelButton(), ui->leftColumn, ":/resources/svg/panel_right.svg",
       ":/resources/svg/panel_right_off.svg"},
      // Toggle target is timelineStrip, NOT the whole bottomPanel — the
      // playback strip (timelineWidget) sits above the strip in the same
      // panel and must remain visible at all times. Resize of the
      // bottomPanel via the splitter handle grows the strip; the playback
      // keeps its fixed height (sizePolicy Fixed-vertical in MainWindow.ui).
      {ui->tabbedPlotWidget->bottomPanelButton(), ui->timelineStrip, ":/resources/svg/panel_bottom.svg",
       ":/resources/svg/panel_bottom_off.svg"},
      {ui->tabbedPlotWidget->rightPanelButton(), ui->localToolbarWidget, ":/resources/svg/panel_left.svg",
       ":/resources/svg/panel_left_off.svg"},
  }};
}

// Curve-Width radio mapping. Hoisted from buildLocalToolbar so the
// layout-save path can encode the float value (rebuild-stable) and the
// layout-load path can map the stored value back to a button.
inline constexpr std::array<std::pair<const char*, double>, 4> kWidthButtonSpecs{{
    {"globalWidth1_0", 1.0},
    {"globalWidth1_5", 1.5},
    {"globalWidth2_0", 2.0},
    {"globalWidth3_0", 3.0},
}};

// The scalar payload behind `item` when it is the plottable series a marker generator
// names `key` (markerSeriesKey naming), else nullptr. Shared by the two readers of that
// question — the generator's series resolver and the dataset it binds to — which must
// agree: a generator bound to a dataset whose series the resolver cannot find runs
// against no data.
[[nodiscard]] const PJ::ScalarFieldPayload* markerSeriesField(const PJ::CatalogItem& item, const std::string& key) {
  const PJ::ScalarFieldPayload* scalar = PJ::asScalarField(item);
  if (scalar == nullptr || !PJ::isPlottablePrimitive(scalar->logical_type)) {
    return nullptr;
  }
  const std::string topic = item.topic_name.toStdString();
  if (PJ::sdk::markerSeriesKey(topic, scalar->field_path.toStdString()) != key &&
      PJ::sdk::markerSeriesKey(topic, scalar->field_name.toStdString()) != key) {
    return nullptr;
  }
  return scalar;
}
}  // namespace

#ifdef PJ_TARGET_WASM
struct MainWindow::BrowserLayoutRuntime {
  struct DatasetShape {
    DatasetId dataset_id = 0;
    QString source_name;
  };

  struct SourceRecord {
    QString display_name;
    QString prefix;
    QString content_sha256;
    QString plugin_id;
    QString plugin_manifest_id;
    QString plugin_config_json;
    std::vector<DatasetShape> datasets;
  };

  struct Replay {
    QDomDocument doc;
    QString layout_name;
    QList<layout_xml::DataSourceRef> sources;
    std::vector<LoadInput> staged_inputs;
    QStringList selected_identities;
    QSet<QString> loaded_identities;
    int selection_index = 0;
    bool load_failed = false;
    QString load_failure;
    CapturedWorkspace previous_workspace;
    QPointer<QPushButton> selection_button;
    quint64 active_picker_generation = 0;
    QMetaObject::Connection loaded_connection;
    QMetaObject::Connection failed_connection;
    QMetaObject::Connection drained_connection;
  };

  QHash<QString, SourceRecord> sources;
  std::optional<Replay> replay;
  QPointer<MessageBox> decision_dialog;
  QString pending_recipe_name;
  bool pending_generic_recipe = false;
  quint64 next_picker_generation = 0;
};
#endif

MainWindow::MainWindow(QWidget* parent) : MainWindow(QString{}, parent) {}

MainWindow::MainWindow(QString extensions_dir, QWidget* parent)
    : QMainWindow(parent),
      ui_(new Ui::MainWindow),
      diagnostic_bridge_(new QtDiagnosticBridge(this)),
      app_settings_(std::make_unique<QSettings>()),
      session_(
          std::make_unique<AppSession>(
              std::move(extensions_dir), diagnostic_bridge_->sink(), pj_app::builtInPlugins())),
      pending_binder_(
          std::make_unique<PendingDisplayBinder>(session_->catalogModel(), &session_->topicDemandTracker())),
      topic_demand_controller_(
          std::make_unique<TopicDemandController>(
              session_->catalogModel(), session_->topicDemandTracker(),
              session_->sessionManager().dataProcessorService(), *pending_binder_,
              session_->sessionManager().dataEngine())),
      ingest_progress_controller_(std::make_unique<IngestProgressController>(this)),
      theme_(std::make_unique<Theme>()) {
#ifdef PJ_TARGET_WASM
  browser_layout_runtime_ = std::make_unique<BrowserLayoutRuntime>();
#endif
  // The 3D transform service owns the per-dataset TF buffers + load-time
  // ingest. It lives in the shell (not pj_runtime) so the runtime stays
  // domain-neutral; it reads only SessionManager's neutral surface.
#ifdef PJ_WITH_SCENE3D
  transform_service_ = std::make_unique<pj::scene3d::TransformService>(session_->sessionManager());
  transform_service_->activateIngestTap();
#endif
  // Pull saved icon metrics before setupUi so the literals we feed into
  // build*Toolbar() pick up the correct values on first paint. Widgets
  // that auto-construct from the .ui still draw at their default sizes
  // during setupUi, but we re-broadcast at the end of the constructor.
  {
    QSettings s;
    chrome_metrics_.icon_size =
        kIconSizeRange.clamp(s.value(QString::fromLatin1(kIconSizeKey), kIconSizeDefault).toInt());
    chrome_metrics_.icon_padding =
        kIconPaddingRange.clamp(s.value(QString::fromLatin1(kIconPaddingKey), kIconPaddingDefault).toInt());
    chrome_metrics_.layout_padding =
        kLayoutPaddingRange.clamp(s.value(QString::fromLatin1(kLayoutPaddingKey), kLayoutPaddingDefault).toInt());
    chrome_metrics_.layout_spacing =
        kLayoutSpacingRange.clamp(s.value(QString::fromLatin1(kLayoutSpacingKey), kLayoutSpacingDefault).toInt());
  }

  ui_->setupUi(this);

  // Hard-zero contents margins on the central widget and every
  // intermediate container down to the chrome rows. Qt's main-window
  // layout or platform style can otherwise add a tiny implicit gap below
  // the menuWidget (TitleBar), which the user sees as a strip of
  // titlebar-background between the title bar and the first chrome row
  // of the central area.
  //
  // The QMainWindow's own contentsMargins are the one exception: they're
  // set to the border stroke width (not zero) so paintEvent()'s 1-px
  // border has a reserved band to paint into. Child widgets (TitleBar,
  // dock panels) are laid out inside contentsRect and are painted after
  // paintEvent() runs, so without this reservation they'd sit flush with
  // the window edge and paint over the border on every side they touch.
  const int chrome_border_width = PJ::theme::stroke(theme::Stroke::Hairline);
  setContentsMargins(chrome_border_width, chrome_border_width, chrome_border_width, chrome_border_width);
  ui_->centralWidget->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  ui_->upperArea->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  ui_->leftColumn->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  ui_->bottomPanel->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  ui_->leftPanel->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  ui_->curveListPanel->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  ui_->tabbedPlotWidget->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  ui_->timelineSplitter->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  ui_->mainSplitter->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  ui_->rightToolbarSplitter->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  ui_->plotsAndGlobalContainer->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  ui_->globalToolbarWidget->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));

  // Qt 6.8 QRhiWidget needs an RHI-capable top-level backing store from
  // the first show(). Keep a zero-size viewer in an existing visible layout
  // so image docks created later can initialize their QRhi.
  // Excluded on wasm: an early WebGL context can be lost across a cold
  // browser file-picker round-trip, and Emscripten then dereferences null
  // WebGL context attributes (see docs/research/wasm_reboot_plan.md, W8a).
#if defined(PJ_WITH_SCENE2D) && !defined(PJ_TARGET_WASM)
  auto* rhi_bootstrap = new MediaViewerWidget(ui_->globalToolbarWidget);
  rhi_bootstrap->setObjectName(u"rhi_bootstrap"_s);
  rhi_bootstrap->setMaximumSize(0, 0);
  rhi_bootstrap->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  if (auto* global_toolbar_layout = qobject_cast<QVBoxLayout*>(ui_->globalToolbarWidget->layout())) {
    global_toolbar_layout->addWidget(rhi_bootstrap);
  }
#endif

  ui_->mainSplitter->setHandleWidth(1);
  ui_->timelineSplitter->setHandleWidth(1);
  ui_->rightToolbarSplitter->setHandleWidth(1);
  // Plain QWidget doesn't paint QSS borders unless this attribute is
  // set; needed for #timelineStrip's `border-top` (the 1-px separator
  // between the playback bar and the timeline strip).
  ui_->timelineStrip->setAttribute(Qt::WA_StyledBackground, true);

  // Vertical splitter between the upper plot/panels area and the
  // timeline strip — extra window height grows the upper area, the
  // timeline keeps its requested size unless the user drags the
  // handle. Initial bottom size matches the playback bar's height
  // exactly so no empty strip is allocated below the playback on
  // first launch; user grows the strip by dragging the handle.
  ui_->timelineSplitter->setStretchFactor(0, 1);
  ui_->timelineSplitter->setStretchFactor(1, 0);
  ui_->timelineSplitter->setSizes({1000, ui_->timelineWidget->minimumHeight()});

  // The left column's width belongs to the user: whatever width the row gains
  // or loses — a window resize, a side panel folding away, a toolbox panel
  // whose size hint grows the window — is absorbed by the chart pane alone.
  // Without these factors QSplitter shares the change between both panes and
  // the sidebar creeps out toward its 600-px cap on its own.
  ui_->mainSplitter->setStretchFactor(0, 0);
  ui_->mainSplitter->setStretchFactor(1, 1);

  // Horizontal splitter with two panes — the plots-and-global-column
  // container (left sidebar + plot area + fixed 24-px global icon
  // column, separated by a static
  // 1-px QFrame border) and the foldable local panel. The only
  // draggable boundary is between the global column and the local
  // panel. Toggle Right Panel hides/shows the local panel; there is no
  // snap or compact mode.
  ui_->rightToolbarSplitter->setStretchFactor(0, 1);
  ui_->rightToolbarSplitter->setStretchFactor(1, 0);
  // Right side-panel floor: wide enough for the scene config panels' label
  // columns (and well past the 6 × 24-px Curve Style icon strip so the
  // FlowLayout never wraps it into two rows).
  ui_->localToolbarWidget->setMinimumWidth(250);
  ui_->rightToolbarSplitter->setSizes({2000, 240});
  ui_->rightToolbarSplitter->setOpaqueResize(true);

  // Pin the playback strip's minimum height to its preferred height so
  // the bottomPanel's minimumSizeHint (computed by its QVBoxLayout) ends
  // up at exactly the playback's natural height. With childrenCollapsible
  // false on the splitter, this becomes the floor: the user can't drag
  // the splitter handle low enough to clip the playback controls.
  ui_->timelineWidget->setMinimumHeight(ui_->timelineWidget->sizeHint().height());

  // Frameless window: drop the WM-drawn chrome so our TitleBar can own
  // the top of the window. Edge resize is implemented via an
  // application-level event filter installed below.
  setWindowFlag(Qt::FramelessWindowHint, true);
  setMouseTracking(true);

  // The TitleBar owns the QMenuBar's popup menus; we just push actions
  // into them.
  title_bar_ = new TitleBar(this);
  connect(title_bar_, &TitleBar::diagnosticActivated, this, [this](const DiagnosticRecord& r) {
    auto* dlg = new DiagnosticsDetailDialog(r, this);
    dlg->setChromeMetrics(chrome_metrics_);
    dlg->show();
  });

  // File menu: layout persistence + marketplace + preferences + quit.
  // Recent layouts are no longer a File-menu submenu — they live in the
  // LeftPanel "recent" popup (Layouts section) alongside recent data files,
  // wired below via LeftPanel::recentLayoutSelected.
  QMenu* file_menu = title_bar_->fileMenu();
  // Desktop retains its path-based layout dialogs. WASM exposes W9's
  // content-based layout open/download actions; Preferences remains omitted
  // until its nested dialog flow has a browser-safe continuation.
#ifndef PJ_TARGET_WASM
  action_load_layout_ = file_menu->addAction(tr("Load Layout..."), this, &MainWindow::onLoadLayout);
  action_save_layout_ = file_menu->addAction(tr("Save Layout..."), this, &MainWindow::onSaveLayout);
  file_menu->addSeparator();
  file_menu->addAction(ui_->actionMarketplace);
  action_preferences_ = file_menu->addAction(tr("Preferences..."), this, &MainWindow::onShowPreferencesDialog);
#else
  // W9 uses content-based open and generic browser download; no native paths.
  action_load_layout_ = file_menu->addAction(tr("Load Layout..."), this, &MainWindow::onLoadLayout);
  action_load_layout_->setObjectName(u"actionLoadLayout"_s);
  action_save_layout_ = file_menu->addAction(tr("Save Layout..."), this, &MainWindow::onSaveLayout);
  action_save_layout_->setObjectName(u"actionSaveLayout"_s);
  action_save_source_layout_ =
      file_menu->addAction(tr("Save Layout with Sources..."), this, &MainWindow::onSaveSourceLayout);
  action_save_source_layout_->setObjectName(u"actionSaveSourceLayout"_s);
  action_save_source_layout_->setEnabled(false);
  ui_->actionMarketplace->setVisible(false);
#endif
  file_menu->addSeparator();
  file_menu->addAction(ui_->actionExit);

  // Toolbox menu: lazily lists the launchable (non-cloud) toolboxes so
  // the list reflects whatever the catalog currently has loaded.
  connect(title_bar_->toolboxMenu(), &QMenu::aboutToShow, this, &MainWindow::onRebuildToolboxMenu);

  // Help menu: About + web links + the informational extension list
  // (rebuilt on aboutToShow; managing extensions happens in the
  // Marketplace, reached from the File menu).
  QMenu* help_menu = title_bar_->helpMenu();
#ifndef PJ_TARGET_WASM
  // AboutDialog::exec() blocks on a nested event loop, unusable on wasm.
  // Async port deferred to W8.
  help_menu->addAction(tr("About PlotJuggler..."), this, &MainWindow::onShowAboutDialog);
#endif
  help_menu->addAction(tr("Check for Updates..."), this, &MainWindow::onCheckForUpdates);
  help_menu->addAction(
      tr("Documentation"), this, []() { QDesktopServices::openUrl(QUrl(u"https://plotjuggler.io"_s)); });
  help_menu->addAction(tr("Report an Issue"), this, []() {
    QDesktopServices::openUrl(QUrl(u"https://github.com/PlotJuggler/PJ4/issues"_s));
  });
  help_menu->addSeparator();
  installed_extensions_menu_ = help_menu->addMenu(tr("Installed Extensions"));
  installed_extensions_menu_->setObjectName(u"PJMenu"_s);
  connect(installed_extensions_menu_, &QMenu::aboutToShow, this, &MainWindow::onRebuildExtensionsMenu);

  setMenuWidget(title_bar_);

  // The panel toggles live in the title bar's right cluster (left of
  // the bell). TabbedPlotWidget creates them — reparenting here keeps
  // the panelToggles() wiring and QSettings persistence untouched.
  title_bar_->addRightClusterWidget(ui_->tabbedPlotWidget->leftPanelButton());
  title_bar_->addRightClusterWidget(ui_->tabbedPlotWidget->bottomPanelButton());
  title_bar_->addRightClusterWidget(ui_->tabbedPlotWidget->rightPanelButton());

  // The event filter sees mouse events delivered to any descendant of
  // this window. Required so a click on the TimelineWidget's empty
  // bottom 6px still starts a window resize.
  qApp->installEventFilter(this);

  // The DiagnosticHistory is the single source of truth for diagnostics.
  // It listens to the bridge (so plugins / core services flow in) and is
  // observed by the title-bar bell + popup.
  diagnostic_history_ = new DiagnosticHistory(this);
  diagnostic_history_->connectBridge(diagnostic_bridge_);
  title_bar_->setDiagnosticHistory(diagnostic_history_);

  ui_->tabbedPlotWidget->setDataServices(&session_->sessionManager(), &session_->catalogModel());
#if defined(PJ_WITH_SCENE2D) || defined(PJ_WITH_SCENE3D)
  ui_->tabbedPlotWidget->setObjectWidgetFactory(
      [this](const QString& kind, const ObjectDropSeed* seed, QWidget* dock_parent) -> IDataWidget* {
        // One factory for both paths. Layout restore passes the saved XML tag as
        // `kind` with a null seed (the dock reloads its own state); a catalog
        // drop passes an empty kind with a seed, which we classify into a kind,
        // construct, and populate. Family routing for v0:
        //   3D-ish (point clouds, transforms, grids, poses)                          → scene3d
        //   2D-ish (kImage/kDepthImage/kImageAnnotations/kSceneEntities/kVideoFrame) → scene2d
        QString resolved_kind = kind;
        if (resolved_kind.isEmpty() && seed != nullptr) {
          // Classify the dropped object into a scene kind — the single routing
          // decision shared with the curve list's drag veto (see
          // scene_object_classification.h for the 2D-first rationale).
          resolved_kind = sceneKindForObjectType(seed->object_type);
        }
        if (seed == nullptr) {
          // Restore / click-create path: build the empty dock and seed its
          // playhead. On restore, PlotDocker then calls xmlLoadState() to
          // repopulate topics + per-layer config; on click-create the dock waits
          // for its first dropped topic. Seeding first means each restored layer
          // comes up at the current playhead, not its first sample
          // (currentTimeChanged only fires on changes; this path also rebuilds
          // docks on undo/redo — M.1). An unknown kind yields nullptr, which on
          // this (seedless) path just means "not my kind" and is silent.
          return makeSeededEmptyObjectDock(resolved_kind, dock_parent);
        }

        if (seed->topic_id == ObjectTopicId{}) {
          // Placeholder drop: the topic is advertised (classified) but has no
          // storage id yet — data only starts flowing once the drop registers
          // demand. Build the empty dock of the resolved family; the drop site
          // stages a pending drop that completes with the real ObjectTopicId
          // when the subscription delivers the first sample
          // (TopicDemandController::handleSceneDockPlaceholderDrop).
          return makeSeededEmptyObjectDock(resolved_kind, dock_parent);
        }

        // Drop path: build, then populate the first topic and apply view
        // side-effects. Failures here surface as transient toasts (non-blocking,
        // WASM-safe): the empty-kind branch is defensive-only (the curve list
        // refuses drags of undisplayable types), but the per-dock refusals below
        // are reachable — the drag veto is type-level and cannot see runtime
        // parser availability.
        IDataWidget* widget = makeSceneDock(resolved_kind, dock_parent);
        if (widget == nullptr) {
          showToast(tr("This object topic cannot be displayed (type %1).")
                        .arg(QString::fromUtf8(sdk::name(seed->object_type))));
          return nullptr;
        }
        QWidget* qwidget = widget->widget();
#ifdef PJ_WITH_SCENE3D
        if (auto* scene3d = qobject_cast<Scene3DDockWidget*>(qwidget)) {
          if (!scene3d->addTopic(seed->topic_id, seed->object_type, seed->title)) {
            scene3d->deleteLater();
            showToast(tr("This object topic cannot be displayed in a 3D view (type %1). "
                         "The 3D view requires a registered parser that emits one of its supported "
                         "canonical objects (PointCloud, CompressedPointCloud, OccupancyGrid, "
                         "SceneEntities, or FrameTransforms).")
                          .arg(QString::fromUtf8(sdk::name(seed->object_type))));
            return nullptr;
          }
          // A 3D-only stream must seed playback here too (see header doc).
          seedStreamingPlaybackFromDrop();
          // currentTimeChanged only fires on changes, so a brand-new widget
          // never gets the current playhead — seed it now to render at the right
          // time immediately.
          scene3d->onTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
        } else
#endif
#ifdef PJ_WITH_SCENE2D
            if (auto* media2d = qobject_cast<Scene2DDockWidget*>(qwidget)) {
          if (!media2d->setImageTopic(seed->topic_id, seed->object_type, seed->title)) {
            media2d->deleteLater();
            showToast(tr("This object topic cannot be displayed in a 2D view (type %1). "
                         "Typically this means the source did not register a parser for the topic, "
                         "or the type is not yet supported by the built-in viewer.")
                          .arg(QString::fromUtf8(sdk::name(seed->object_type))));
            return nullptr;
          }
          seedStreamingPlaybackFromDrop();
          media2d->setPointInspectorEnabled(show_points_);
          media2d->onTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
        } else
#endif
        {
          // makeSceneDock produced a kind this populate switch doesn't handle —
          // a programming error if a new family is added without a branch here.
          // Fail loudly rather than returning an unpopulated dock.
          qWarning("MainWindow: makeSceneDock returned an unhandled object-widget kind");
          widget->widget()->deleteLater();
          return nullptr;
        }
        return widget;
      });
#endif
  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::tabAdded, this, &MainWindow::onPlotTabAdded);
  wireExistingPlots();
  ui_->curveListPanel->setCatalog(&session_->catalogModel());
  ui_->curveListPanel->setTopicDemandTracker(&session_->topicDemandTracker());
  ui_->curveListPanel->setTopicDemandController(topic_demand_controller_.get());

  // Wire the progressive ingest controller to the SessionManager and curve list
  // panel through the one shared seam (IngestProgressWiring).
  // The row's two affordances ARE the choice (✕ keeps, bin discards), so the
  // wiring routes a click straight to requestCancel. The keep/discard prompt
  // survives only on the title-bar strip path, whose single stop button cannot
  // express it.
  wireIngestProgress(
      session_->sessionManager(), session_->catalogModel(), *ingest_progress_controller_, *ui_->curveListPanel, this);

  // Marker generators read their inputs through these two catalog-backed callbacks.
  // They are SESSION state, not panel state: a generator can also arrive from a saved
  // layout, which is replayed before any toolbox panel exists, so they are installed
  // once here rather than per toolbox launch. The resolver maps an input key
  // (markerSeriesKey) to its samples using the same naming the producer and the
  // overlay use, so a generator yields the same markers in GUI and CI.
  session_->sessionManager().markerService().setResolver(
      [this](PJ::DatasetId dataset, const std::string& key) -> std::optional<PJ::MarkerService::ResolvedSeries> {
        const PJ::DataReader reader = session_->sessionManager().createReader();
        for (const PJ::CatalogItem& item : session_->catalogModel().items()) {
          if (item.dataset_id != dataset) {
            continue;
          }
          const PJ::ScalarFieldPayload* scalar = markerSeriesField(item, key);
          if (scalar == nullptr) {
            continue;
          }
          PJ::Expected<PJ::SeriesReader> series = reader.series(scalar->topic_id, scalar->column_index);
          if (!series.has_value()) {
            return std::nullopt;
          }
          PJ::MarkerService::ResolvedSeries out;
          const std::size_t n = series->size();
          out.timestamps.reserve(n);
          out.values.reserve(n);
          for (std::size_t i = 0; i < n; ++i) {
            if (const std::optional<PJ::SeriesSample> s = series->sampleAt(i)) {
              out.timestamps.push_back(static_cast<double>(s->timestamp));
              out.values.push_back(s->value);
            }
          }
          return out;
        }
        return std::nullopt;
      });
  // For `all_datasets` (global-across-all) generators: list every loaded dataset.
  session_->sessionManager().markerService().setDatasetLister([this]() -> std::vector<PJ::DatasetId> {
    std::vector<PJ::DatasetId> ids;
    for (const std::pair<PJ::DatasetId, QString>& ds : session_->catalogModel().datasets()) {
      ids.push_back(ds.first);
    }
    return ids;
  });

  // Keep data widgets coherent with catalog removals, whoever triggers them.
  // Each widget prunes its OWN pieces against the live catalog/store. One pass
  // per removal.
  connect(&session_->catalogModel(), &CatalogModel::cleared, this, [this]() { syncWidgetsToCatalog(); });
  connect(&session_->catalogModel(), &CatalogModel::itemsRemoved, this, [this](const QStringList&) {
    syncWidgetsToCatalog();
  });
  connect(session_.get(), &AppSession::datasetsMerged, this, [this](DatasetId anchor, QList<DatasetId> consumed) {
#ifdef PJ_WITH_SCENE3D
    if (transform_service_ != nullptr) {
      for (const DatasetId id : consumed) {
        transform_service_->invalidateDataset(id);
      }
      transform_service_->invalidateDataset(anchor);
      transform_service_->ingestFrameTransformsForDataset(anchor);
    }
#else
    Q_UNUSED(anchor);
    Q_UNUSED(consumed);
#endif
    syncWidgetsToCatalog();
    resetUndoHistory();
  });

  connect(ui_->curveListPanel, &CurveListPanel::trashRequested, this, &MainWindow::onCatalogTrashRequested);
  connect(ui_->curveListPanel, &CurveListPanel::toastRequested, this, [this](const QString& message) {
    showToast(message);
  });
  connect(ui_->curveListPanel, &CurveListPanel::removeDatasetsRequested, this, &MainWindow::onRemoveDatasetsRequested);
  connect(ui_->curveListPanel, &CurveListPanel::mergeDatasetsRequested, this, &MainWindow::onMergeDatasetsRequested);
  // The "+" in Custom Series opens the Transform Editor — now provided by the
  // toolbox plugin (the native panel was retired). Launch it like any toolbox.
  connect(ui_->curveListPanel, &CurveListPanel::createCustomSeriesRequested, this, [this]() {
    launchToolbox(u"toolbox-transform-editor"_s);
  });
  // Edit (pencil): open the Transform Editor pre-populated with the selected custom
  // series' saved editor state (stored in the recipe's params_json at create time),
  // so the user can Modify it in place.
  connect(ui_->curveListPanel, &CurveListPanel::editCustomSeriesRequested, this, [this](const QString& catalog_key) {
    // The custom view yields a catalog KEY (same as the delete path); resolve it to
    // the output topic name, then find that transform's saved editor state.
    QString output_name;
    for (const auto& item : session_->catalogModel().items()) {
      if (item.key == catalog_key) {
        output_name = item.topic_name;
        break;
      }
    }
    auto& dp = session_->sessionManager().dataProcessorService();
    QString initial_config;
    for (const auto& recipe : dp.transformRecipes()) {
      const bool owns = std::any_of(recipe.outputs.begin(), recipe.outputs.end(), [&](const std::string& o) {
        return QString::fromStdString(o) == output_name;
      });
      if (owns) {
        initial_config = QString::fromStdString(recipe.params_json);
        break;
      }
    }
    launchToolbox(u"toolbox-transform-editor"_s, initial_config);
  });
  connect(ui_->curveListPanel, &CurveListPanel::deleteCustomSeriesRequested, this, [this](const QString& catalog_key) {
    const std::optional<CurveDescriptor> output = session_->catalogModel().curveDescriptor(catalog_key);
    if (!output.has_value()) {
      return;
    }
    if (!confirmAndRemoveDependentTransforms({output->topic_id})) {
      return;
    }
    auto& dp = session_->sessionManager().dataProcessorService();
    bool removed = false;
    for (const auto& recipe : dp.transformRecipes()) {
      const bool owns = std::find(recipe.output_topic_ids.begin(), recipe.output_topic_ids.end(), output->topic_id) !=
                        recipe.output_topic_ids.end();
      if (owns) {
        removed = dp.removeTransform(recipe.key).has_value();
        break;
      }
    }
    if (!removed) {
      return;
    }
    session_->catalogModel().rebuildFromDatastore();
    ui_->curveListPanel->removeCustomCurve(catalog_key);
  });
  connect(ui_->curveListPanel, &CurveListPanel::clearAllCurvesRequested, this, [this]() {
    if (streaming_manager_ != nullptr && streaming_manager_->hasActiveSession()) {
      streaming_manager_->stopAllAndWait(tr("dataset removed"));
      active_streaming_dataset_id_ = 0;
      streaming_playback_seeded_ = false;
    }
    // Confirmed full wipe: free all objects before mutating the catalog, so the
    // cleared() subscription sees the topics gone and resets the object viewers.
    // Eviction lives at the confirmed-removal site, not in clearAll(), so the
    // low-level catalog op stays safe for speculative callers. Keep
    // lastLoadedSource for the quick-reload path (#99).
    // An all_datasets generator is bound to the session, not to a dataset, so
    // removeDataset() below would leave it alive to publish onto whatever loads next.
    session_->sessionManager().markerService().clearAllGenerators();
    session_->sessionManager().clearAllObjects();
    // TF buffers derive from the just-evicted objects; drop them with the data.
#ifdef PJ_WITH_SCENE3D
    if (transform_service_ != nullptr) {
      transform_service_->invalidateAll();
    }
#endif
    // REAL delete: drop the catalog (no tombstone — nothing is kept) so widgets tear down
    // their curve adapters via cleared(), THEN erase every dataset's scalar storage from
    // the engine (adapters gone, so DataEngine::removeDataset's invalidate-first contract
    // holds). Without the engine erase a later reload would reattach to the emptied shells.
    session_->catalogModel().clearAll(/*tombstone=*/false);
    SessionManager& session_manager = session_->sessionManager();
    for (const DatasetId id : session_manager.dataEngine().listDatasets()) {
      session_manager.removeDataset(id);
    }
    resetUndoHistory();
#ifdef PJ_TARGET_WASM
    updateBrowserLayoutActions();
#endif
  });

  QSettings settings;
  // Right-toolbar global view toggles persisted across sessions. Buttons
  // themselves are created later by buildGlobalToolbar(); we load state
  // first so the buttons can pick up the correct initial check state.
  show_points_ = settings.value(u"MainWindow.buttonShowpoint"_s, true).toBool();
  activate_grid_ = settings.value(u"MainWindow.buttonActivateGrid"_s, false).toBool();
  dots_ = settings.value(u"MainWindow.buttonDots"_s, false).toBool();
  tracker_info_ = static_cast<CurveTracker::Parameter>(
      settings.value(u"MainWindow.timeTrackerSetting"_s, static_cast<int>(CurveTracker::kValue)).toInt());
  keep_ratio_ = settings.value(u"MainWindow.buttonRatio"_s, true).toBool();
  legend_status_ = static_cast<LegendStatus>(
      settings.value(u"MainWindow.legendStatus"_s, static_cast<int>(LegendStatus::kHidden)).toInt());

  // Push the just-loaded toggle states into every plot already created by
  // wireExistingPlots(). New plots will pick this up via onPlotAdded.
  forEachPlot([this](PlotWidget* plot) { applyGlobalToggles(plot); });
  applyShowPointsTo2DWidgets();

  // Panel toggle buttons in the tab strip drive shell-level visibility
  // for the left column, the timeline strip, and the right toolbar.
  // The buttons are NOT checkable — visibility is communicated through
  // the icon glyph itself (filled = panel visible, outlined = hidden),
  // swapped via the "iconPath" dynamic property + applyIcons() so
  // theme-tinting flows through one codepath. Fixed launch layout, not persisted:
  // left panel visible, right panel and bottom timeline strip collapsed.
  for (const PanelToggle& toggle : panelToggles(ui_)) {
    const bool visible = (toggle.target == ui_->leftColumn);
    toggle.target->setVisible(visible);
    toggle.button->setCheckable(false);
    toggle.button->setProperty("iconPath", QString::fromLatin1(visible ? toggle.icon_path_on : toggle.icon_path_off));
    // The bottom panel's open/closed height constraints are applied centrally by
    // applyBottomPanelConstraints() — driven here by the chrome-metrics pass
    // emitted later in the constructor (which also resolves the playback height).
    QPushButton* button = toggle.button;
    QWidget* target = toggle.target;
    const QString icon_on = QString::fromLatin1(toggle.icon_path_on);
    const QString icon_off = QString::fromLatin1(toggle.icon_path_off);
    connect(button, &QPushButton::clicked, this, [this, button, target, icon_on, icon_off]() {
      const bool now_visible = !target->isVisible();
      target->setVisible(now_visible);
      const QString icon = now_visible ? icon_on : icon_off;
      button->setProperty("iconPath", icon);
      button->setIcon(loadSvg(icon, theme_->currentTheme()));
      // Bottom-panel toggle: collapse/restore the splitter so the playback stays
      // glued to the top with no empty gap when folded, and the strip's previous
      // expanded height is preserved across fold cycles. applyBottomPanelConstraints
      // owns the open-min / closed-lock invariants.
      if (target == ui_->timelineStrip) {
        const QList<int> sizes = ui_->timelineSplitter->sizes();
        const int total = sizes[0] + sizes[1];
        if (now_visible) {
          applyBottomPanelConstraints();  // lifts the cap, pins the open minimum
          const int floor = ui_->bottomPanel->minimumHeight();
          const int expanded = std::max(QSettings().value(kPanelBottomExpandedKey, sizes[1]).toInt(), floor);
          ui_->timelineSplitter->setSizes({total - expanded, expanded});
          // Fit all source bars horizontally each time the strip is opened, once the
          // strip's new geometry has settled (deferred a tick so the view is sized).
          if (source_timeline_ != nullptr) {
            QTimer::singleShot(0, source_timeline_, [tl = source_timeline_]() { tl->zoomToFit(); });
          }
        } else {
          QSettings().setValue(kPanelBottomExpandedKey, sizes[1]);
          applyBottomPanelConstraints();  // pins the panel to the playback bar (drag can't reopen)
        }
      }
    });
  }

  applyIcons(theme_->currentTheme());

  // Apply QSS + force ToolTip palette to the theme. QToolTip's background
  // is decided by both QSS and QPalette::ToolTipBase; setting only the
  // QSS sometimes leaves the platform palette's tooltip colour in place
  // (showing as a yellow / brown box). Setting the palette here and on
  // every theme change keeps them in sync.
  auto apply_theme_chrome = [this]() {
    qApp->setStyleSheet(theme_->expandedQss());
    const bool light = theme_->currentTheme().contains("light");
    const auto token_theme = theme::themeFor(light);
    const QColor tip_bg = theme::surface(theme::Surface::Backdrop, token_theme);
    const QColor tip_fg = theme::text(token_theme);
    QPalette p = qApp->palette();
    p.setColor(QPalette::ToolTipBase, tip_bg);
    p.setColor(QPalette::ToolTipText, tip_fg);
    qApp->setPalette(p);
    // QToolTip keeps its own palette separate from QApplication's; in
    // Qt 6 it's this one that wins for the actual tooltip widget.
    QPalette tp = QToolTip::palette();
    tp.setColor(QPalette::ToolTipBase, tip_bg);
    tp.setColor(QPalette::ToolTipText, tip_fg);
    tp.setColor(QPalette::Window, tip_bg);
    tp.setColor(QPalette::WindowText, tip_fg);
    QToolTip::setPalette(tp);
  };

  connect(theme_.get(), &Theme::themeChanged, this, &MainWindow::onThemeChanged, Qt::QueuedConnection);
  connect(theme_.get(), &Theme::qssChanged, this, apply_theme_chrome);
  connect(this, &MainWindow::stylesheetChanged, ui_->leftPanel, &LeftPanel::onStylesheetChanged);
  connect(this, &MainWindow::stylesheetChanged, ui_->curveListPanel, &CurveListPanel::onStylesheetChanged);
  connect(this, &MainWindow::stylesheetChanged, ui_->timelineWidget, &TimelineWidget::onStylesheetChanged);
  connect(this, &MainWindow::stylesheetChanged, ui_->tabbedPlotWidget, &TabbedPlotWidget::onStylesheetChanged);
  connect(this, &MainWindow::stylesheetChanged, title_bar_, &TitleBar::onStylesheetChanged);

  // Icon-metrics broadcast — runs alongside the theme broadcast. Each
  // listener caches both values and re-runs its applyIcons pass. The
  // tabbedPlotWidget is not currently in scope; see
  // docs/superpowers/specs/2026-05-15-icon-size-preference-design.md
  // ("panel-toggle buttons out of scope").
  connect(this, &MainWindow::chromeMetricsChanged, ui_->leftPanel, &LeftPanel::onChromeMetricsChanged);
  connect(this, &MainWindow::chromeMetricsChanged, ui_->curveListPanel, &CurveListPanel::onChromeMetricsChanged);
  connect(this, &MainWindow::chromeMetricsChanged, ui_->timelineWidget, &TimelineWidget::onChromeMetricsChanged);
  connect(this, &MainWindow::chromeMetricsChanged, ui_->tabbedPlotWidget, &TabbedPlotWidget::onChromeMetricsChanged);
  connect(this, &MainWindow::chromeMetricsChanged, title_bar_, &TitleBar::onChromeMetricsChanged);

  apply_theme_chrome();

  // Reflow chrome dimensions after each widget has re-applied its
  // icons — runs LAST so the per-widget slots above have already
  // updated sizeHint(). The timeline strip's minimum height tracks
  // its content; the local toolbar's minimum width fits six chrome
  // buttons on one row plus a small flow-layout margin.
  //
  // The splitter and the bottomPanel cap need explicit nudges: Qt's
  // QSplitter doesn't re-layout on a child's min-height change, and
  // when the bottom strip is hidden we clamp bottomPanel's max-height
  // to the playback's old min-height during toggle. Without these
  // pushes, the playback bar grows upward only and its icons get
  // clipped against bottomPanel's stale bottom edge.
  connect(this, &MainWindow::chromeMetricsChanged, this, [this](const ChromeMetrics& metrics) {
    // The .ui pins timelineWidget to maximumHeight 24 — override both
    // min and max so the playback bar grows with its buttons. sizePolicy
    // is Preferred-Fixed in the .ui, so the widget's height stays equal
    // to sizeHint regardless of available space.
    const int playback_height = ui_->timelineWidget->sizeHint().height();
    ui_->timelineWidget->setMinimumHeight(playback_height);
    ui_->timelineWidget->setMaximumHeight(playback_height);

    const int band_extent = (metrics.icon_size + metrics.icon_padding) + (2 * metrics.layout_padding);
    // Local toolbar fits six chrome buttons on one row at the band extent, plus
    // a small margin for the FlowLayout to wrap — but never narrower than the
    // 250-px side-panel floor.
    ui_->localToolbarWidget->setMinimumWidth(qMax((6 * band_extent) + 6, 250));

    // Re-apply the bottom panel's open/closed height constraints against the
    // freshly-resolved playback height (the QSplitter caches stale minimums, so
    // this also re-floors the strip and re-pins the closed lock).
    applyBottomPanelConstraints();
    // The playback controls' size feeds the slider's start x; re-align the name
    // column separator to it (no-op until the playback bar is laid out, post-show).
    alignNameColumnToPlayback();
  });

  // Dev-only widget inspector: Ctrl+Shift+D = pesticide outline overlay,
  // Ctrl+Shift+Q = QSS debug border layer. Drop the include + this call +
  // DebugUi.{cpp,h} (plus the CMakeLists entries) to remove the feature.
  DebugUi::installInto(this, theme_.get());

  auto& playback = session_->playbackEngine();
  // Start in the empty state: no data loaded means an empty range, so the
  // transport renders disabled until the first dataset arrives (same state a
  // full removal returns to).
  playback.setRangeAndCurrentTime(displayRange(0.0, 0.0), displaySeconds(0.0));
  ui_->timelineWidget->setPlaybackEngine(&playback);
  // Cap the cursor-move fan-out at ~30 Hz (kTrackerBroadcastIntervalMs). currentTimeChanged
  // fires on playback ticks, scrubbing, and seeks — all of which can exceed 30 Hz (a fast
  // scrub emits at mouse-move rate). The coalescer runs broadcastTrackerTime(latest) on the
  // leading edge and once more on the trailing edge, dropping the intermediate cursor times.
  tracker_broadcast_trigger_ = std::make_unique<CoalescingTrigger>(
      kTrackerBroadcastIntervalMs, [this]() { broadcastTrackerTimeToVisible(pending_tracker_time_); });
  connect(&playback, &PlaybackEngine::currentTimeChanged, this, [this](double time) {
    pending_tracker_time_ = time;
    tracker_broadcast_trigger_->request();
  });
  Timestamp last_global_reference = session_->sessionManager().globalTimeReference();
  connect(
      &session_->sessionManager(), qOverload<>(&SessionManager::displayOffsetChanged), this,
      [this, last_global_reference]() mutable {
        SessionManager& manager = session_->sessionManager();
        PlaybackEngine& engine = session_->playbackEngine();
        const Timestamp new_global_reference = manager.globalTimeReference();
        const DisplaySeconds old_time = engine.currentTime();
        const double frame_delta = timestampDifferenceSeconds(last_global_reference, new_global_reference);
        last_global_reference = new_global_reference;
        if (active_streaming_dataset_id_ != 0) {
          if (const auto range = manager.datasetDisplayRange(active_streaming_dataset_id_); range.has_value()) {
            engine.setRange(*range);
          }
        } else {
          session_->recomputeRange();
        }
        engine.setCurrentTime(DisplaySeconds{old_time.value + frame_delta});
        broadcastReferenceLine();
        pending_tracker_time_ = toAxisDouble(engine.currentTime());
        tracker_broadcast_trigger_->request();
      });
  connect(
      &session_->sessionManager(), qOverload<DatasetId>(&SessionManager::displayOffsetChanged), this,
      [this](DatasetId) {
        broadcastReferenceLine();
        pending_tracker_time_ = toAxisDouble(session_->playbackEngine().currentTime());
        tracker_broadcast_trigger_->request();
      });

  // Mount the multi-track Source Timeline into the reserved bottom strip and
  // bind it to the runtime. timelineStrip is an empty native widget in the .ui;
  // give it a layout and host the widget. The existing bottom-panel toggle +
  // splitter target timelineStrip, so show/hide/resize come for free.
  auto* source_timeline = new PJ::Timeline(ui_->timelineStrip);
  source_timeline_ = source_timeline;
  auto* strip_layout = new QVBoxLayout(ui_->timelineStrip);
  strip_layout->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  strip_layout->addWidget(source_timeline);
  source_timeline_controller_ = new PJ::SourceTimelineController(source_timeline, session_.get(), this);
  connect(source_timeline_controller_, &SourceTimelineController::workspaceChangeCommitted, this, [this]() {
    onUndoableChange(/*force_new_state=*/true);
  });
  connect(source_timeline_controller_, &SourceTimelineController::trackAdded, this, [this](DatasetId) {
    if (applying_state_ || progressive_layout_in_flight_) {
      return;
    }
    reconcileHistoryWithDataUniverse();
  });
  connect(source_timeline, &Timeline::viewStateChangeCommitted, this, [this]() { onUndoableChange(); });
  // Remember the user's name-column width so it sticks across rebuilds / panel
  // toggles (alignNameColumnToPlayback re-applies it over the playback-aligned
  // floor) and is persisted into layouts.
  connect(source_timeline, &PJ::Timeline::nameColumnWidthChanged, this, [this](int width_px) {
    timeline_name_column_width_ = width_px;
  });
  // Timeline auto-zoom is always on (no longer a user preference): a newly-loaded
  // file (and each alignment) zooms the timeline to its full extent.
  source_timeline->setAutoZoomEnabled(true);
  // Align rail on the right edge of the timeline panel; binds its buttons to the
  // controller above, so it must be built after the controller exists.
  buildTimelineAlignRail();
  // Dragging the timeline's blue reference needle repositions the reference and
  // re-renders every plot's delta-from-reference (mirrors the toolbar toggle).
  connect(source_timeline, &PJ::Timeline::referenceLineMoved, this, [this](double display_seconds) {
    // The needle reports a playback-frame display-seconds position (the widget undoes
    // its time-frame offset before emitting); store it frame-invariantly as an
    // absolute instant through the representative's full displayOffset (source +
    // global), so it survives offset changes, then re-push the position to plots.
    reference_instant_ = toAbsolute(
        fromAxisDouble(display_seconds), session_->sessionManager().displayOffset(representativeDatasetId()));
    const std::optional<double> ref_sec = referenceDisplaySeconds();
    forEachPlot([ref_sec](PlotWidget* plot) { plot->setReferenceLine(ref_sec); });
  });

  streaming_manager_ = std::make_unique<StreamingSourceManager>(
      session_->sessionManager(), session_->extensionCatalog(), session_->catalogModel(),
      session_->topicDemandTracker(), this, this);
  // Feed the stream dialog live chrome metrics so its SectionHeaderBands match
  // the panel-hosted toolboxes' canonical band height.
  streaming_manager_->setChromeMetricsProvider([this] { return chrome_metrics_; });
  // Interactive placeholder drops stage pending binds OUTSIDE any layout restore
  // (which has its own restore-scoped duplicate of this connection); flush them
  // whenever the catalog gains topics. Idempotent — an entry binds once and the
  // no-entries early-return makes the steady-state cost nil.
  connect(&session_->catalogModel(), &CatalogModel::itemsAdded, this, [this](const std::vector<CatalogItem>& items) {
    // Dock queues own scene completion. The binder observes the completed queue
    // only afterwards and releases the corresponding demand reference.
    retryPendingSceneRestores(items);
    flushPendingCurveBindings(items);
  });
  connect(
      ui_->leftPanel, &LeftPanel::streamingSourceChanged, streaming_manager_.get(),
      &StreamingSourceManager::onSourceChanged);
  connect(
      ui_->leftPanel, &LeftPanel::streamingBufferChanged, streaming_manager_.get(),
      &StreamingSourceManager::onBufferChanged);
  connect(
      ui_->leftPanel, &LeftPanel::streamingStartRequested, streaming_manager_.get(),
      &StreamingSourceManager::onStartRequested);
  connect(
      ui_->leftPanel, &LeftPanel::streamingPauseToggled, streaming_manager_.get(),
      &StreamingSourceManager::onPauseToggled);

  // Recording is a runtime capability: the service is wired unconditionally so
  // every recording path below reads the same on every platform, and only the
  // Record affordances follow isSupported().
  ui_->leftPanel->setRecordingSupported(RecordingService::isSupported());
  recording_service_ = std::make_unique<RecordingService>(this);
  recording_service_->setSettings(RecordingService::loadSettings());

#ifndef __EMSCRIPTEN__
  // M3 transparent source cache: built once from Preferences (see the member
  // doc for why settings changes apply at the next launch). Wired into every
  // toolbox binding (interactive + headless) and into layout restore.
  source_cache_store_ = SourceCacheStore::fromSettings(SourceCacheStore::loadSettings());
  source_capture_service_ = std::make_unique<SourceCaptureService>(*source_cache_store_);
#endif
  connect(ui_->leftPanel, &LeftPanel::streamingRecordToggled, this, &MainWindow::onRecordToggled);
  connect(recording_service_.get(), &RecordingService::started, this, &MainWindow::onRecordingStarted);
  connect(recording_service_.get(), &RecordingService::stopped, this, &MainWindow::onRecordingStopped);
  connect(recording_service_.get(), &RecordingService::progress, this, &MainWindow::onRecordingProgress);

  const auto refresh_streaming_seek_lock = [this]() {
    if (session_ == nullptr) {
      return;
    }
    // While a stream is live AND playback is playing, the cursor stays glued to the
    // tip (held there by PlaybackEngine::onTick + each live ingest) and the user must
    // not be able to drag it off: grey the playback slider and put the Source Timeline
    // read-only (scene edits + needle seek). Pausing (stream still on) clears the lock
    // so the retained window can be scrubbed/realigned.
    const bool lock = active_streaming_dataset_id_ != 0 && session_->playbackEngine().isPlaying();
    ui_->timelineWidget->setSeekLocked(lock);
    setSourceTimelineStreamingLock(lock);
  };
  const auto snap_streaming_playback_to_live_edge = [this]() -> bool {
    if (active_streaming_dataset_id_ == 0 || session_ == nullptr) {
      return false;
    }
    if (const auto range = session_->sessionManager().datasetDisplayRange(active_streaming_dataset_id_);
        range.has_value()) {
      streaming_playback_seeded_ = true;
      session_->playbackEngine().setRangeAndCurrentTime(*range, range->max);
      pending_tracker_time_ = toAxisDouble(range->max);
      broadcastTrackerTimeToVisible(pending_tracker_time_);
      return true;
    }
    return false;
  };

  // Streaming → playback range wiring. The slider range is scoped to the
  // active streaming dataset only — unioning with every catalog-visible
  // dataset (as AppSession::seedPlaybackFromSession does for file loads) can
  // stretch the slider across unrelated historical data, leaving the actual
  // streamed window as a sliver where intermediate scrub positions resolve to
  // "before first entry" or "at the live edge" with nothing in between.
  //
  // The slider starts at the placeholder range, then the first live ingest with a
  // real range seeds it from the active streaming dataset. A curve/object drop can
  // still force the same seed immediately, but it is not required: streaming data
  // itself must be enough to advance playback.
  //
  // Timeline playback state is the follow-live switch. While playing, the slider
  // is locked to the live edge and the engine is held at rangeMax between ingest
  // ticks instead of auto-pausing at the end. Pausing playback unlocks scrubbing
  // within the retained window; pressing play snaps back to the newest sample.
  connect(
      streaming_manager_.get(), &StreamingSourceManager::streamStarted, this,
      [this, refresh_streaming_seek_lock](DatasetId id) {
        active_streaming_dataset_id_ = id;
        session_->setActiveStreamingDataset(id);  // scope recomputeRange to the live tip while playing
        streaming_playback_seeded_ = false;
        auto& engine = session_->playbackEngine();
        engine.setHoldAtRangeMax(true);
        engine.setCurrentTime(engine.rangeMax());
        engine.play();
        refresh_streaming_seek_lock();
      });
  connect(
      streaming_manager_.get(), &StreamingSourceManager::streamStopped, this,
      [this, refresh_streaming_seek_lock](DatasetId id, const QString& reason) {
        if (active_streaming_dataset_id_ == id) {
          active_streaming_dataset_id_ = 0;
          session_->setActiveStreamingDataset(0);
          streaming_playback_seeded_ = false;
          session_->playbackEngine().setHoldAtRangeMax(false);
          refresh_streaming_seek_lock();
        }
        // That source's file is finalized on its own; the rest of the capture
        // keeps recording, and the batch ends with its last live source. Taps
        // are attached at start() only, so a stream started later is never
        // recorded.
        if (recording_service_ != nullptr && recording_service_->isRecording()) {
          recording_service_->onSourceEnded(static_cast<quint64>(id), reason);
        }
      });
  connect(
      &playback, &PlaybackEngine::playingChanged, this,
      [this, snap_streaming_playback_to_live_edge, refresh_streaming_seek_lock](bool playing) {
        if (active_streaming_dataset_id_ != 0 && playing) {
          session_->playbackEngine().setHoldAtRangeMax(true);
          (void)snap_streaming_playback_to_live_edge();
        }
        refresh_streaming_seek_lock();
      });
  // Bound the 3D TF buffer's history for live-streaming datasets in step with
  // the ObjectStore retention window, so a long streaming session doesn't retain
  // every TF sample forever (H.11). File loads keep the buffer's kKeepAll
  // default. The factor keeps TF resolvable slightly past the oldest scrubbable
  // object entry — the store and the TF buffer trim on independent ticks, so the
  // headroom avoids a TF lookup failing on a frame whose object still exists.
#ifdef PJ_WITH_SCENE3D
  if (transform_service_ != nullptr) {
    connect(
        streaming_manager_.get(), &StreamingSourceManager::retentionWindowChanged, this,
        [this](DatasetId id, qint64 window_ns) {
          constexpr int kTfWindowHeadroomFactor = 2;
          transform_service_->setLiveCacheWindow(id, std::chrono::nanoseconds(kTfWindowHeadroomFactor * window_ns));
        });
  }
#endif
  connect(
      &session_->sessionManager(), &SessionManager::samplesIngested, this,
      [this, refresh_streaming_seek_lock](const QVector<TopicId>&, bool live) {
        auto& engine = session_->playbackEngine();
        if (live) {
          // Streaming: follow the live edge (range grows, cursor tracks newest).
          if (active_streaming_dataset_id_ == 0) {
            return;
          }
          if (const auto range = session_->sessionManager().datasetDisplayRange(active_streaming_dataset_id_);
              range.has_value()) {
            streaming_playback_seeded_ = true;
            if (engine.isPlaying()) {
              engine.setRangeAndCurrentTime(*range, range->max);
              pending_tracker_time_ = toAxisDouble(range->max);
              broadcastTrackerTimeToVisible(pending_tracker_time_);
            } else {
              engine.setRange(*range);
            }
            refresh_streaming_seek_lock();
          }
          return;
        }
        // Non-live (file) ingest: grow the playback range from the UNION of every
        // loaded dataset (recomputeRange) so the timeline + auto-fitting plots reveal
        // data as it arrives. Using a SINGLE dataset's range here was a bug: with a
        // multi-file (multi-select) load, each file's range excludes the earlier ones,
        // so setRange clamps the cursor FORWARD to each new file's start. The union
        // keeps the seeded cursor in range. The authoritative cursor is still set once
        // on completion by AppSession::seedPlaybackFromSession (onFileLoaded).
        session_->recomputeRange();
      });

  // Populate the combo only after the manager is wired so the initial
  // streamingSourceChanged emission from setStreamingSources() reaches it.
  refreshStreamingCombo();
  connect(
      &session_->extensionCatalog(), &ExtensionCatalogService::catalogChanged, this,
      &MainWindow::refreshStreamingCombo);

  // Populate the Cloud page from the catalog now and on every catalog change.
  ui_->leftPanel->populateCloudToolboxes(session_->extensionCatalog().toolboxes());
  connect(&session_->extensionCatalog(), &ExtensionCatalogService::catalogChanged, this, [this]() {
    ui_->leftPanel->populateCloudToolboxes(session_->extensionCatalog().toolboxes());
  });

  file_loader_ = std::make_unique<FileLoader>(
      session_->sessionManager(), session_->extensionCatalog(), session_->catalogModel(), this);
#ifdef PJ_WITH_SCENE3D
  file_loader_->setTransformService(transform_service_.get());
#endif
  // Passing the MainWindow as the metrics source primes the file dialog's
  // toolbar icon size and keeps it in step via chromeMetricsChanged. Injected
  // here so FileLoader itself never links MainWindow (keeps it testable).
  file_loader_->setFilePicker(
      [](QWidget* dialog_parent, const QString& caption, const QString& dir, const QString& filter, bool multi) {
        auto* metrics_source = dialog_parent != nullptr ? qobject_cast<MainWindow*>(dialog_parent->window()) : nullptr;
        if (multi) {
          return FileDialog::getOpenFileNames(dialog_parent, caption, dir, filter, metrics_source);
        }
        const QString path = FileDialog::getOpenFileName(dialog_parent, caption, dir, filter, metrics_source);
        return path.isEmpty() ? QStringList{} : QStringList{path};
      });
  // Gates the per-dataset Reload/Replace context-menu items to file-backed
  // datasets (a streaming/test dataset has no source path to re-read).
  ui_->curveListPanel->setDatasetSourcePathResolver(
      [this](DatasetId dataset_id) { return file_loader_->sourcePathForDataset(dataset_id); });
  connect(ui_->curveListPanel, &CurveListPanel::reloadDatasetRequested, this, &MainWindow::onReloadDatasetRequested);
  connect(ui_->curveListPanel, &CurveListPanel::replaceDatasetRequested, this, &MainWindow::onReplaceDatasetRequested);
  connect(ui_->leftPanel, &LeftPanel::loadDataRequested, this, &MainWindow::onLoadDataRequested);
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  // Qt Widgets live in one browser canvas, so automation has no DOM element to
  // target when Chromium occasionally drops a synthetic canvas gesture after
  // many fresh WASM contexts. Install probe-build-only browser callbacks that
  // invoke the exact application slot / embedded QPushButton. Production WASM
  // and desktop do not expose them.
  pj_wasm_install_browser_test_probes();
#ifdef PJ_WITH_SCENE3D
  installWasmScene3dAcceptanceProbes();
#endif
  const auto run_wasm_plot_probe = [this](bool fit_complete_series) {
    PlotWidget* plot = firstPlotOfActiveTab();
    const auto curves = session_->catalogModel().curves();
    if (plot == nullptr || curves.empty()) {
      qWarning("PJ_WASM_PLOT_PROBE_FAILED no plot or scalar curve");
      return;
    }
    auto selected = std::find_if(
        curves.begin(), curves.end(), [](const CurveDescriptor& curve) { return curve.field_name == u"temp"_s; });
    if (selected == curves.end()) {
      selected = curves.begin();
    }
    auto* added = plot->addCurve(selected->name);
    if (added == nullptr) {
      qWarning("PJ_WASM_PLOT_PROBE_FAILED addCurve rejected %s", qPrintable(selected->name));
      return;
    }
    if (fit_complete_series) {
      plot->zoomOut(/*emit_signal=*/false);
    }
    auto* canvas = plot->findChild<PlotRhiCanvas*>();
    const QColor curve_color = added->curve->pen().color();
    QTimer::singleShot(500, canvas, [canvas, curve_color]() {
      const QImage image = canvas->grabFramebuffer();
      qsizetype curve_pixels = 0;
      int min_x = image.width();
      int min_y = image.height();
      int max_x = -1;
      int max_y = -1;
      for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
          const QColor pixel = image.pixelColor(x, y);
          if (std::abs(pixel.red() - curve_color.red()) <= 2 && std::abs(pixel.green() - curve_color.green()) <= 2 &&
              std::abs(pixel.blue() - curve_color.blue()) <= 2) {
            ++curve_pixels;
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
          }
        }
      }
      qInfo(
          "PJ_WASM_PLOT_READBACK size=%dx%d curve=%s curve_pixels=%lld curve_bounds=%d,%d..%d,%d", image.width(),
          image.height(), qPrintable(curve_color.name(QColor::HexArgb)), static_cast<long long>(curve_pixels), min_x,
          min_y, max_x, max_y);
    });
  };
  auto* wasm_plot_probe_shortcut = new QShortcut(QKeySequence(Qt::Key_F9), this);
  connect(wasm_plot_probe_shortcut, &QShortcut::activated, this, [run_wasm_plot_probe]() {
    run_wasm_plot_probe(/*fit_complete_series=*/false);
  });
  auto* wasm_reduction_probe_shortcut = new QShortcut(QKeySequence(Qt::Key_F10), this);
  connect(wasm_reduction_probe_shortcut, &QShortcut::activated, this, [run_wasm_plot_probe]() {
    run_wasm_plot_probe(/*fit_complete_series=*/true);
  });
  auto* wasm_plot_state_shortcut = new QShortcut(QKeySequence(Qt::Key_F8), this);
  connect(wasm_plot_state_shortcut, &QShortcut::activated, this, [this]() {
    PlotWidget* plot = firstPlotOfActiveTab();
    if (plot == nullptr) {
      qWarning("PJ_WASM_PLOT_STATE_FAILED no plot");
      return;
    }
    auto* canvas = plot->findChild<PlotRhiCanvas*>();
    if (canvas == nullptr) {
      qWarning("PJ_WASM_PLOT_STATE_FAILED no RHI canvas");
      return;
    }
    QStringList titles;
    std::vector<std::pair<QString, QColor>> curve_colors;
    for (const PlotWidgetBase::CurveInfo& info : plot->curveList()) {
      if (info.curve == nullptr) {
        continue;
      }
      const QString title = info.curve->title().text();
      titles.push_back(title);
      curve_colors.emplace_back(title, info.curve->pen().color());
    }
    const QPoint canvas_position = canvas->mapTo(this, QPoint(0, 0));
    const QRectF view = plot->currentBoundingRect();
    const QColor tracker_color =
        theme::interaction(theme::Variant::Highlight, theme::State::Checked, theme::appTheme());
    const QColor grid_color =
        theme::outline(theme::OutlineRole::Gridline, theme::OutlineState::Rest, theme::appTheme());
    const QColor hover_fill_color =
        theme::interaction(theme::Variant::Emphasis, theme::State::Nominal, theme::appTheme());
    const QColor canvas_color = canvas->palette().color(QPalette::Window);
    const WasmHoverState hover = findWasmHoverState(plot);
    const QByteArray encoded_hover_label = QUrl::toPercentEncoding(hover.label);
    const int curve_style = static_cast<int>(plot->defaultCurveStyle());
    const int line_width = static_cast<int>(plot->lineWidth());
    qInfo(
        "PJ_WASM_PLOT_STATE count=%lld titles=%s canvas=%d,%d,%dx%d view=%.17g,%.17g,%.17g,%.17g "
        "time=%.17g tracker_enabled=%d tracker_parameter=%d xy=%d style=%d width=%d",
        static_cast<long long>(titles.size()), qPrintable(titles.join(',')), canvas_position.x(), canvas_position.y(),
        canvas->width(), canvas->height(), view.left(), view.right(), view.bottom(), view.top(),
        toAxisDouble(session_->playbackEngine().currentTime()), plot->trackerEnabled() ? 1 : 0,
        static_cast<int>(plot->trackerParameter()), plot->isXYPlot() ? 1 : 0, curve_style, line_width);
    qInfo(
        "PJ_WASM_PLOT_HOVER enabled=%d visible=%d point=%.17g,%.17g label=%s", plot->showPoints() ? 1 : 0,
        hover.visible ? 1 : 0, hover.point.x(), hover.point.y(), encoded_hover_label.constData());

    QTimer::singleShot(
        250, canvas,
        [canvas, curve_colors = std::move(curve_colors), tracker_color, grid_color, hover_fill_color, canvas_color,
         curve_style, line_width]() {
          const QImage image = canvas->grabFramebuffer();
          QStringList counts;
          WasmPixelBounds first_curve;
          for (const auto& [title, color] : curve_colors) {
            const WasmPixelBounds pixels = findWasmExactColor(image, color);
            if (counts.empty()) {
              first_curve = pixels;
            }
            counts.push_back(u"%1:%2:%3"_s.arg(title, color.name(QColor::HexArgb)).arg(pixels.pixels));
          }
          qInfo("PJ_WASM_PLOT_CURVE_PIXELS curves=%s", qPrintable(counts.join(',')));
          if (!curve_colors.empty()) {
            qInfo(
                "PJ_WASM_PLOT_STYLE_PIXELS style=%d width=%d pixels=%lld bounds=%d,%d..%d,%d hash=%llu "
                "framebuffer=%dx%d",
                curve_style, line_width, static_cast<long long>(first_curve.pixels), first_curve.min_x,
                first_curve.min_y, first_curve.max_x, first_curve.max_y,
                static_cast<unsigned long long>(first_curve.coordinate_hash), image.width(), image.height());
          }

          const WasmLineCadence major_grid = findWasmVerticalCadence(image, grid_color, 6, 4);
          const WasmLineCadence minor_grid = findWasmVerticalCadence(image, grid_color, 3, 1);
          qInfo(
              "PJ_WASM_PLOT_GRID_PIXELS major_pixels=%lld major_column=%d major_period=%d "
              "major_period_matches=%d major_period_comparisons=%d major_phase_mask=%d major_on_phases=%d "
              "minor_pixels=%lld minor_column=%d minor_period=%d minor_period_matches=%d "
              "minor_period_comparisons=%d minor_phase_mask=%d minor_on_phases=%d",
              static_cast<long long>(major_grid.pixels), major_grid.column, major_grid.period,
              major_grid.period_matches, major_grid.period_comparisons, major_grid.phase_mask, major_grid.on_phases,
              static_cast<long long>(minor_grid.pixels), minor_grid.column, minor_grid.period,
              minor_grid.period_matches, minor_grid.period_comparisons, minor_grid.phase_mask, minor_grid.on_phases);

          const WasmPixelBounds tracker = findWasmBlendedColor(image, tracker_color, canvas_color);
          qInfo(
              "PJ_WASM_PLOT_TRACKER color=%s pixels=%lld bounds=%d,%d..%d,%d framebuffer=%dx%d",
              qPrintable(tracker_color.name(QColor::HexArgb)), static_cast<long long>(tracker.pixels), tracker.min_x,
              tracker.min_y, tracker.max_x, tracker.max_y, image.width(), image.height());
          const WasmPixelBounds text = findWasmDarkPixels(image);
          qInfo(
              "PJ_WASM_PLOT_TEXT_PIXELS pixels=%lld bounds=%d,%d..%d,%d framebuffer=%dx%d",
              static_cast<long long>(text.pixels), text.min_x, text.min_y, text.max_x, text.max_y, image.width(),
              image.height());
          const WasmPixelBounds hover_fill = findWasmExactColor(image, hover_fill_color);
          WasmPixelBounds hover_border;
          if (hover_fill.pixels > 0) {
            const QRect hover_border_rect =
                QRect(QPoint(hover_fill.min_x, hover_fill.min_y), QPoint(hover_fill.max_x, hover_fill.max_y))
                    .adjusted(-3, -3, 3, 3);
            hover_border = findWasmDarkPixels(image, hover_border_rect);
          }
          qInfo(
              "PJ_WASM_PLOT_HOVER_PIXELS yellow_pixels=%lld yellow_bounds=%d,%d..%d,%d border_pixels=%lld "
              "framebuffer=%dx%d",
              static_cast<long long>(hover_fill.pixels), hover_fill.min_x, hover_fill.min_y, hover_fill.max_x,
              hover_fill.max_y, static_cast<long long>(hover_border.pixels), image.width(), image.height());
        });
  });
#endif
  connect(ui_->leftPanel, &LeftPanel::reloadDataRequested, this, &MainWindow::onReloadDataRequested);
  connect(ui_->leftPanel, &LeftPanel::cloudToolboxRequested, this, [this](const QString& id) { launchToolbox(id); });
  scene_undo_debounce_.setSingleShot(true);
  scene_undo_debounce_.setInterval(kSceneUndoDebounceMs);
  connect(&scene_undo_debounce_, &QTimer::timeout, this, [this]() { onUndoableChange(); });

  connect(
      file_loader_.get(), &FileLoader::sourceReplacementAboutToCommit, this,
      [this](quint64 ticket, const QString& path, DatasetId replaced_id) {
        if (progressive_layout_in_flight_) {
          return;
        }
        // Stamp the retiring dataset with the INCOMING path (a "Replace
        // dataset" load points it at a different file) so the post-load rebind
        // maps its charts onto the replacement datasets.
        const QString incoming = QFileInfo(path).absoluteFilePath();
        pending_source_replacement_ = PendingSourceReplacement{
            .workspace = capturePortableWorkspace(replaced_id, incoming), .path = incoming, .ticket = ticket};
      });
  connect(file_loader_.get(), &FileLoader::fileLoaded, this, &MainWindow::onFileLoaded);
  // A reload disables the Reload button until completion (reloadSource); only
  // onFileLoaded re-enables it, so a FAILED load would leave it stuck disabled.
  connect(file_loader_.get(), &FileLoader::fileLoadFailed, this, [this](const QString&, const QString&) {
    ui_->leftPanel->setReloadEnabled(session_->sessionManager().lastLoadedSource().has_value());
  });
  // Disarm a replacement snapshot whose load did not commit. Bound to
  // loadFinished, not the legacy signals: it is the one terminal every accepted
  // request reaches, including the exits fileLoadFailed stays silent on (a
  // rejected config dialog, a queued cancel, shutdown). The snapshot describes
  // the datasets that are still loaded — the load rolled back — so leaving it
  // armed would let a LATER successful load of the same path rebind the
  // workspace onto an obsolete picture of it. Ticket equality, NOT a path
  // match: loadFinished is queued, so a stale same-path terminal from another
  // load (a cancelled queued duplicate, a failed predecessor) could otherwise
  // disarm a live load's capture and silently skip its workspace rebind.
  connect(
      file_loader_.get(), &FileLoader::loadFinished, this,
      [this](quint64 ticket, LoadOutcome outcome, const QString&, DatasetId, const QVector<DatasetId>&) {
        if (outcome == LoadOutcome::kLoaded || !pending_source_replacement_.has_value()) {
          return;
        }
        if (pending_source_replacement_->ticket == ticket) {
          pending_source_replacement_.reset();
        }
      });
  // Track successful loads for the recent-files popup.
  connect(
      file_loader_.get(), &FileLoader::fileLoaded, this,
      [this](
          const QString& path, const QString& /*prefix*/, const QString& /*plugin_id*/,
          const QString& /*plugin_config_json*/) {
        // A browser upload token and its MEMFS backing file both expire with
        // this page. Keep it out of the desktop-style recent-file list; W9c
        // source layouts reselect bytes explicitly instead.
        if (isBrowserUploadIdentity(path)) {
          return;
        }
        QSettings recent_settings;
        QStringList recent = recent_settings.value(u"File/recent"_s).toStringList();
        recent.removeAll(path);
        recent.prepend(path);
        while (recent.size() > kMaxRecentEntries) {
          recent.removeLast();
        }
        recent_settings.setValue(u"File/recent"_s, recent);
        ui_->leftPanel->setRecentEnabled(true);
      });
  // Recent files reopen through the normal load flow, including the dialog.
  connect(ui_->leftPanel, &LeftPanel::recentFileSelected, this, [this](const QString& path) {
    file_loader_->loadFile(path, this);
  });
  // Recent layouts load through the same validated path as the (removed) File
  // menu submenu — onLoadRecentLayout checks existence and prunes dead entries.
  connect(ui_->leftPanel, &LeftPanel::recentLayoutSelected, this, &MainWindow::onLoadRecentLayout);
#ifdef PJ_TARGET_WASM
  connect(ui_->leftPanel, &LeftPanel::clearRecentLayoutsRequested, this, [this]() {
    if (BrowserPersistence::instance() != nullptr) {
      BrowserPersistence::instance()->clearRecentLayouts();
    }
    ui_->leftPanel->setRecentEnabled(false);
  });
#endif
  // Enable the popup immediately when prior sessions recorded recent files or
  // layouts (either section is enough to make the popup worth showing).
#ifdef PJ_TARGET_WASM
  const bool had_recent =
      BrowserPersistence::instance() != nullptr && !BrowserPersistence::instance()->recentLayouts().isEmpty();
#else
  const bool had_recent = !settings.value(u"File/recent"_s).toStringList().isEmpty() ||
                          !settings.value(u"Layout/recent"_s).toStringList().isEmpty();
#endif
  ui_->leftPanel->setRecentEnabled(had_recent);

  // Title-bar load progress strip — the non-modal replacement for the import
  // dialog on single-instance loads. Shown after a 500 ms delay so quick loads
  // never flash it; hidden 1 s after the load queue drains. The single stop
  // button opens a confirmation dialog that routes to FileLoader::cancelCurrent.
  ingest_progress_ = new IngestProgressWidget(this);
  ingest_progress_->setActive(false);
  // One icon-only stop button on the left. The three real choices (keep /
  // discard / resume) live in the confirmation dialog below, so the button
  // itself is meaning-light — its tooltip just invites a stop.
  ingest_progress_->setPrimaryButton({}, u":/resources/svg/cancel.svg"_s, tr("Stop loading…"));
  title_bar_->setCenterWidget(ingest_progress_);
  ingest_show_timer_ = new QTimer(this);
  ingest_show_timer_->setSingleShot(true);
  // Per-dataset progress now lives on the curve-tree rows (IngestProgressController),
  // so the title-bar strip stays silent. Everything behind it — the displayed-owner
  // arbitration, the stop dialog, the counter/label plumbing — is retained and still
  // fed by its producers; flip this to true to bring the strip back.
  // static: the lambda below reads it with no capture-default, which clang
  // rejects for an automatic constexpr (odr-use) even though GCC accepts it.
  static constexpr bool kShowTitleBarIngestStrip = false;
  connect(ingest_show_timer_, &QTimer::timeout, this, [this]() {
    if constexpr (kShowTitleBarIngestStrip) {
      ingest_progress_->setActive(true);
    }
  });
  ingest_hide_timer_ = new QTimer(this);
  ingest_hide_timer_->setSingleShot(true);
  ingest_hide_timer_->setInterval(kIngestStripHideLingerMs);
  connect(ingest_hide_timer_, &QTimer::timeout, this, [this]() {
    // Only hide once nothing is still producing into the strip: a file load may
    // have started, or a toolbox import may have re-adopted it, since the linger
    // began. Hiding retires the displayed owner (e.g. a drained file load's).
    if (!session_->sessionManager().hasActiveIngests() && !file_loader_->isBusy()) {
      ingest_progress_->setActive(false);
      ingest_strip_owner_kind_ = IngestStripOwnerKind::kNone;
      ingest_strip_owner_dataset_ = 0;
    }
  });
  connect(ingest_progress_, &IngestProgressWidget::actionRequested, this, [this](IngestProgressWidget::Action) {
    // Stop pressed → ask what to do with the in-progress load. The worker keeps
    // loading (and the bar keeps updating) while this modal dialog is up.
    if (!file_loader_->isBusy()) {
      // No file load to stop: Stop routes by DISPLAYED owner (D8). The layout
      // batch's job takes the nonblocking keep-partial per-job cancel (D3) —
      // that job concludes kCancelled, the batch CONTINUES with its next
      // source, nothing is rolled back; NEVER batch->cancel() and never a
      // faked ToolboxIngestRef for a headless job.
      if (ingest_strip_owner_kind_ == IngestStripOwnerKind::kLayoutBatch && layoutImportBatchActive()) {
        layout_import_batch_->cancelActiveImportJob();
        return;  // the strip hands off when the job's ingest ends
      }
      // Interactive toolbox imports keep the pre-D8 route: stop every live
      // importing host (a closed panel's entry fails the owner lock and is
      // skipped) — flag-only cooperative cancel, no keep/discard dialog, and
      // inherently batch-free (the batch never registers a ToolboxIngestRef).
      if (session_->sessionManager().hasActiveIngests()) {
        stopAllToolboxImports();
        return;  // strip hides when on_ingest_finished drains the import set
      }
      // The load already finished (e.g. clicked during the post-completion
      // linger): there is nothing to stop, so just dismiss the strip.
      ingest_progress_->setActive(false);
      return;
    }
    if (!ingest_stop_dialog_.isNull()) {
      ingest_stop_dialog_->raise();
      ingest_stop_dialog_->activateWindow();
      return;
    }

    // The browser main thread must never enter a nested QDialog event loop.
    // Keep the instance alive through QObject parentage and continue from its
    // finished signal instead.
    auto* dialog = new MessageBox(this);
    ingest_stop_dialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    // App-modal (not window-modal): window-modality leaves floating ADS dock
    // tool-windows interactive. show() below honors it without a nested loop.
    dialog->setWindowModality(Qt::ApplicationModal);
    dialog->setTitle(tr("Stop loading?"));
    dialog->setText(tr("This data is still loading. Keep what has loaded so far, remove all of it, or keep loading?"));
    dialog->addButton(tr("Remove All"), MessageBox::kDestructiveRole);  // index 0 — stop and discard
    dialog->addButton(tr("Stop and Keep"), MessageBox::kPrimaryRole);   // index 1 — stop, keep partial
    dialog->addButton(tr("Cancel"), MessageBox::kCancelRole);           // index 2 — resume loading

    // Bind this confirmation to the SPECIFIC load it was raised for. If that
    // load finishes, its premise is gone: either the queue drains (queueDrained)
    // or a queued next load takes over (loadGenerationAdvanced) — auto-dismiss
    // on both so the dialog can never act on, or block, the wrong load.
    const std::uint64_t tracked_generation = file_loader_->loadGeneration();
    const QMetaObject::Connection advanced = connect(
        file_loader_.get(), &FileLoader::loadGenerationAdvanced, dialog,
        [dialog, tracked_generation](std::uint64_t generation) {
          if (generation != tracked_generation) {
            dialog->reject();
          }
        });
    const QMetaObject::Connection drained =
        connect(file_loader_.get(), &FileLoader::queueDrained, dialog, [dialog]() { dialog->reject(); });
    connect(dialog, &QDialog::finished, this, [this, dialog, tracked_generation, advanced, drained](int) {
      QObject::disconnect(advanced);
      QObject::disconnect(drained);
      const int clicked = dialog->clickedIndex();
      ingest_stop_dialog_.clear();
      switch (clicked) {
        case 0:
          file_loader_->cancelCurrent(tracked_generation, /*keep_partial=*/false);  // Remove All — stop and discard
          break;
        case 1:
          file_loader_->cancelCurrent(tracked_generation, /*keep_partial=*/true);  // Stop and Keep
          break;
        default:
          break;  // Cancel or window-close — leave the load alone
      }
    });
    // show(), not open(): QDialog::open() force-downgrades ApplicationModal to
    // WindowModal. show() honors the app-modality set above and still fires finished.
    dialog->show();
  });
  connect(
      file_loader_.get(), &FileLoader::ingestStarted, this,
      [this](const QString& title, int index, int total, bool /*determinate*/) {
        // File loads always win the strip (D8: displayed ownership tracks it,
        // even over a currently displayed toolbox/batch ingest).
        ingest_strip_owner_kind_ = IngestStripOwnerKind::kFile;
        ingest_strip_owner_dataset_ = 0;
        ingest_progress_->setTitle(title);
        ingest_progress_->setCounterText(total > 1 ? u"%1/%2"_s.arg(index).arg(total) : QString());
        ingest_progress_->setRange(0, 0);  // busy until the first determinate progress tick
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
        // Keep browser cancellation fixtures small and deterministic. Production
        // retains the anti-flash delay below.
        ingest_show_timer_->start(kIngestStripShowDelayWasmMs);
#else
        ingest_show_timer_->start(kIngestStripShowDelayMs);
#endif
      });
  connect(file_loader_.get(), &FileLoader::ingestProgress, this, [this](int current, int maximum) {
    ingest_progress_->setRange(0, maximum);  // maximum 0 keeps the bar in busy/indeterminate mode
    ingest_progress_->setValue(current);
  });
  connect(file_loader_.get(), &FileLoader::queueDrained, this, [this]() {
    // A toolbox/batch ingest that started while this file load owned the strip
    // takes it over now (D8 survivor pick — its own label and progress, never
    // a stale finished import's) instead of the strip hiding on a
    // still-running import.
    if (displaySurvivingIngestOnStrip(0)) {
      return;
    }
    ingest_strip_owner_kind_ = IngestStripOwnerKind::kNone;
    ingest_strip_owner_dataset_ = 0;
    ingest_show_timer_->stop();
    ingest_hide_timer_->start();
  });

#ifndef PJ_TARGET_WASM
  connect(ui_->actionMarketplace, &QAction::triggered, this, &MainWindow::onOpenMarketplace);
#endif
  connect(ui_->actionExit, &QAction::triggered, this, &QWidget::close);

  // The title-bar "Update" button is a one-shot: clicking it hides the badge
  // (per session) and opens the Marketplace where the user applies updates. The
  // next launch's startup scan decides whether to show it again.
  connect(title_bar_, &TitleBar::extensionUpdateRequested, this, [this]() {
    title_bar_->setExtensionUpdateCount(0);
    onOpenMarketplace();
  });

  // Undo/Redo apply to plot-layout snapshots. They are keyboard-only
  // (Ctrl+Z / Ctrl+Y) and deliberately NOT in any menu. Registering them on
  // the main window via addAction is what makes the shortcuts fire window-wide;
  // an action that lives only in a popup menu (as these used to) never
  // activates its shortcut while that menu is closed.
  undo_action_ = new QAction(tr("Undo"), this);
  undo_action_->setShortcuts(QKeySequence::Undo);
  connect(undo_action_, &QAction::triggered, this, &MainWindow::onUndo);
  addAction(undo_action_);
  redo_action_ = new QAction(tr("Redo"), this);
  // The platform's standard redo set (Ctrl+Y and/or Ctrl+Shift+Z depending on
  // desktop-theme detection) plus both explicitly, so redo works the same
  // everywhere regardless of how Qt resolves QKeySequence::Redo.
  QList<QKeySequence> redo_shortcuts = QKeySequence::keyBindings(QKeySequence::Redo);
  redo_shortcuts.append(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z));
  redo_shortcuts.append(QKeySequence(Qt::CTRL | Qt::Key_Y));
  redo_action_->setShortcuts(redo_shortcuts);
  connect(redo_action_, &QAction::triggered, this, &MainWindow::onRedo);
  addAction(redo_action_);

  // Plot-layout changes from TabbedPlotWidget feed the undo stack. Wrapped in a lambda
  // because onUndoableChange now takes a (defaulted) force_new_state arg, and Qt's
  // function-pointer connect rejects a slot whose arity exceeds the signal's.
  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::undoableChange, this, [this] { onUndoableChange(); });

  // Apply the persisted "align starts" frame before the toolbar is built so its
  // button seeds from the live SessionManager state. Default OFF: datasets show at
  // their natural relative times so the Source Timeline reveals their offsets and
  // the user aligns them (manually, or via this toggle). No data yet, so this is a
  // no-op at startup; toggling it later bulk-writes each dataset's offset.
  session_->sessionManager().setUseTimeOffset(QSettings().value(u"MainWindow.useTimeOffset"_s, false).toBool());

  // Global column on the right of the plot area — Chart + Legend icons,
  // pinned at 24 px wide, never collapses. Always visible regardless of
  // the local panel's toggle state.
  buildGlobalToolbar();

  // Right sidepanel content switches by focused widget type via a
  // QStackedWidget. Page 0 is the plot-config UI (Curve Width / Style
  // strips + CurveEditor). Pages 1 and 2 are per-family placeholders
  // (Scene2D, Scene3D); onDockFocused() picks the active page.
  auto* outer_layout = qobject_cast<QVBoxLayout*>(ui_->localToolbarWidget->layout());
  outer_layout->setSpacing(PJ::theme::space(theme::Space::None));
  outer_layout->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  right_panel_stack_ = new QStackedWidget(ui_->localToolbarWidget);
  outer_layout->addWidget(right_panel_stack_, /*stretch=*/1);

  plot_config_page_ = new QWidget(right_panel_stack_);
  auto* plot_config_layout = new QVBoxLayout(plot_config_page_);
  plot_config_layout->setSpacing(PJ::theme::space(theme::Space::None));
  plot_config_layout->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  right_panel_stack_->addWidget(plot_config_page_);

  auto make_placeholder = [this](const QString& text) {
    auto* page = new QWidget(right_panel_stack_);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(
        PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
        PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None));
    layout->addStretch(1);
    auto* label = new QLabel(text, page);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    layout->addWidget(label);
    layout->addStretch(1);
    return page;
  };
#ifdef PJ_WITH_SCENE2D
  scene2d_config_panel_ = new Scene2DConfigPanel(right_panel_stack_);
  scene2d_config_page_ = scene2d_config_panel_;
  scene2d_config_panel_->onStylesheetChanged(theme_->currentTheme());
  connect(this, &MainWindow::stylesheetChanged, scene2d_config_panel_, &Scene2DConfigPanel::onStylesheetChanged);
#endif
#ifdef PJ_WITH_SCENE3D
  scene3d_config_panel_ = new Scene3DConfigPanel(right_panel_stack_);
  scene3d_config_page_ = scene3d_config_panel_;
  // Push the active theme into the panel so its row icons paint in the
  // right ink on first show, then keep them tracking subsequent theme
  // toggles via the standard stylesheetChanged signal.
  scene3d_config_panel_->onStylesheetChanged(theme_->currentTheme());
  connect(this, &MainWindow::stylesheetChanged, scene3d_config_panel_, &Scene3DConfigPanel::onStylesheetChanged);
#endif
  empty_dock_page_ = make_placeholder(tr("No widget selected"));
#ifdef PJ_WITH_SCENE2D
  right_panel_stack_->addWidget(scene2d_config_page_);
#endif
#ifdef PJ_WITH_SCENE3D
  right_panel_stack_->addWidget(scene3d_config_page_);
#endif
  right_panel_stack_->addWidget(empty_dock_page_);
  right_panel_stack_->setCurrentWidget(plot_config_page_);

  // Local panel to the right of the global column — Curve Width / Curve
  // Style header bands and their FlowLayout icon strips, followed by the
  // per-curve CurveEditor. Hidden by the "Toggle Right Panel" button;
  // its width snaps/folds as the user drags the splitter handle.
  buildLocalToolbar();

  // Push the loaded metrics through every chrome-aware widget so
  // anything that picked up .ui defaults — including the global and local
  // toolbar columns, which install their own listeners inside the build*
  // functions above — re-renders at the saved values.
  emit chromeMetricsChanged(chrome_metrics_);

  // CurveEditor lives below the icon strips inside the plot-config
  // page; same right-panel toggle controls visibility (the stack is
  // a child of localToolbarWidget). Rebinds to the active plot on tab
  // changes.
  curve_editor_ = new CurveEditor(plot_config_page_);
  plot_config_layout->addWidget(curve_editor_, /*stretch=*/1);
  // Trailing stretch keeps the icon strips anchored at the top of the
  // panel when the CurveEditor below is hidden. CurveEditor's own
  // stretch factor (1) outweighs the spacer's default (0), so while
  // the editor is visible it still fills the remaining vertical space.
  plot_config_layout->addStretch(0);
  curve_editor_->onStylesheetChanged(theme_->currentTheme());
  curve_editor_->onChromeMetricsChanged(chrome_metrics_);
  connect(this, &MainWindow::stylesheetChanged, curve_editor_, &CurveEditor::onStylesheetChanged);
  connect(this, &MainWindow::chromeMetricsChanged, curve_editor_, &CurveEditor::onChromeMetricsChanged);
  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::currentTabChanged, this, [this](PlotDocker* /*docker*/) {
    // Switching tabs doesn't emit ADS focus, so drive the right panel (and the
    // editor binding it subsumes) from the new tab's focused dock.
    onDockFocused(activeFocusedDock());
    // Catch-up for the visibility gate: the per-tick fan-out skips inactive tabs,
    // so the tab being switched TO missed every cursor move while hidden — and
    // currentTimeChanged only fires on a time CHANGE, not on a tab switch. Re-seed
    // the now-visible tab with the current cursor so its plots/scenes aren't stale.
    if (session_ != nullptr) {
      broadcastTrackerTimeToVisible(toAxisDouble(session_->playbackEngine().currentTime()));
    }
  });
  bindEditorToPlot(firstPlotOfActiveTab());

  pushInitialUndoState();
  updateUndoRedoActions();

  // Give every scroll area present in the shell at startup (Datasets / Custom
  // Series / Sources trees, the curve list, …) the canonical overlay pill
  // scrollbars. Dynamically-created panels attach their own (config panels in
  // their ctors; plugin panels via the dialog host).
  attachPillScrollbars(this);
}

IDataWidget* MainWindow::makeSceneDock(const QString& kind, QWidget* parent) {
  // Single construct + wire site for object-widget docks, shared by the drop
  // and layout-restore paths. Wiring (session / transform service / theme) is
  // identical regardless of how the dock is later populated.
#ifdef PJ_WITH_SCENE3D
  if (kind == "scene3d"_L1) {
    auto* widget = new Scene3DDockWidget(parent);
    connect(widget, &SceneDockWidget::pendingRestoresChanged, this, &MainWindow::schedulePendingDisplayBindingRebuild);
    connect(widget, &SceneDockWidget::workspaceChanged, &scene_undo_debounce_, qOverload<>(&QTimer::start));
    widget->setSessionManager(&session_->sessionManager());
    widget->setTransformService(transform_service_.get());
    widget->setSettings(app_settings_.get());
    topic_demand_controller_->registerSceneDock(widget);
    // Seed the resolver's per-source remembered-roots map + auto search roots
    // from the most recent load. Single-path approximation (the dock's own
    // dataset may differ in a multi-file session); see setSourcePath's doc.
    if (const auto src = session_->sessionManager().lastLoadedSource(); src.has_value()) {
      widget->setSourcePath(src->path);
    }
    // Apply the persisted scene controls once the lazily-created view exists, so
    // a dock created by layout restore (never bound to the panel) still matches
    // the shared look. Connect for the deferred view, and apply now if it is
    // already realized (M.3). The QSettings schema stays owned by the panel.
    if (scene3d_config_panel_ != nullptr) {
      auto* panel = scene3d_config_panel_;
      connect(widget, &Scene3DDockWidget::sceneViewReady, panel, [panel, widget]() {
        panel->applySceneControlsTo(widget);
      });
      if (widget->sceneView() != nullptr) {
        panel->applySceneControlsTo(widget);
      }
    }
    // No theme push needed: SceneViewWidget derives dark/light from its own
    // palette luminance and repaints on QEvent::PaletteChange.
    return widget;
  }
#endif
#ifdef PJ_WITH_SCENE2D
  if (kind == "scene2d"_L1) {
    auto* widget = new Scene2DDockWidget(parent);
    connect(widget, &SceneDockWidget::pendingRestoresChanged, this, &MainWindow::schedulePendingDisplayBindingRebuild);
    connect(widget, &SceneDockWidget::workspaceChanged, &scene_undo_debounce_, qOverload<>(&QTimer::start));
    widget->setSessionManager(&session_->sessionManager());
    topic_demand_controller_->registerSceneDock(widget);
    return widget;
  }
#endif
  if (kind == "state_transitions"_L1) {
    auto* widget = new StateTransitionsDockWidget(
        &session_->sessionManager(), &session_->catalogModel(), &session_->playbackEngine(), parent);
    // Series add/remove + committed view-chrome changes ride the same debounced
    // workspace-undo snapshot route the scene docks use.
    connect(widget, &StateTransitionsDockWidget::workspaceChanged, &scene_undo_debounce_, qOverload<>(&QTimer::start));
    connect(
        widget->view(), &StateTransitionsView::visibleRangeChanged, this,
        [this, widget](double t_min, double t_max) { onStateTransitionsRangeChanged(widget, t_min, t_max); });
    topic_demand_controller_->registerStateTransitionsDock(widget);
    return widget;
  }
  return nullptr;
}

IDataWidget* MainWindow::makeSeededEmptyObjectDock(const QString& kind, QWidget* parent) {
  IDataWidget* widget = makeSceneDock(kind, parent);
  if (widget != nullptr) {
    widget->onTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
  }
  return widget;
}

void MainWindow::onObjectFamilyRequested(DockWidget* dock, VisualizationKind family) {
  if (dock == nullptr) {
    return;
  }
  // The shell is the only module that maps a UI family to a concrete object-widget
  // kind. A switch (no default) makes a future enumerator a compile error here —
  // the one site that owns the mapping. Plot never reaches this slot (DockWidget
  // builds plots itself), so it is a defensive no-op.
  QString kind;
  switch (family) {
    case VisualizationKind::kScene2D:
      kind = u"scene2d"_s;
      break;
    case VisualizationKind::kScene3D:
      kind = u"scene3d"_s;
      break;
    case VisualizationKind::kStateTransitions:
      kind = u"state_transitions"_s;
      break;
    case VisualizationKind::kPlot:
      return;
  }
  IDataWidget* widget = makeSeededEmptyObjectDock(kind, dock);
  if (widget == nullptr) {
    // Both scene kinds are valid, so this is a programming error rather than a
    // user-facing one — log it instead of silently leaving the placeholder.
    qCWarning(lcMain) << "onObjectFamilyRequested: could not build scene dock for kind" << kind;
  }
  dock->adoptObjectWidget(widget);  // null still reverts to a usable placeholder
}

void MainWindow::onPlaceholderTopicDropped(
    DockWidget* dock, DatasetId dataset_id, QString topic_name, sdk::BuiltinObjectType object_type) {
  if (dock == nullptr) {
    return;
  }
  if (object_type == sdk::BuiltinObjectType::kNone) {
    if (PlotWidget* plot = dock->plotWidget(); plot != nullptr) {
      topic_demand_controller_->handlePlaceholderPlotDrop(plot, dataset_id, topic_name);
    }
    return;
  }
  IDataWidget* object_widget = dock->objectWidget();
  if (auto* scene_dock = qobject_cast<SceneDockWidget*>(object_widget != nullptr ? object_widget->widget() : nullptr);
      scene_dock != nullptr) {
    topic_demand_controller_->handleSceneDockPlaceholderDrop(scene_dock, dataset_id, topic_name, object_type);
  }
}

MainWindow::~MainWindow() {
  // FIRST, before `delete ui_` below: stop() emits stopped(), whose slot
  // touches the left panel. A quit that never ran closeEvent (--screenshot,
  // --exit-after-layout) would otherwise reach that slot with the widgets gone.
  if (recording_service_ != nullptr) {
    recording_service_->stop();
  }
  // Dtor-without-closeEvent path (same precedent as closeAllPinnedToolboxTabs
  // below): a still-running batch's host teardown fires on_ingest_finished ->
  // endIngest -> the still-connected observer, so the D1 teardown must run
  // FIRST — structurally, not by the member-declaration-order accident.
  teardownLayoutBatchObservation();
  // Pinned toolboxes hold plugin sessions (ToolboxRuntimeHost writing into
  // the AppSession's DataEngine). Tear them down synchronously while the
  // session, tab strip, and engines are all still alive — a deferred
  // teardown would run after member destruction and touch a dead engine.
  closeAllPinnedToolboxTabs();
  // The chart-area takeover panel holds the same kind of session, but its
  // PanelEngine is a QObject child of this window — the base-class child sweep
  // would destroy it only AFTER the members (incl. session_) are gone, so its
  // teardown flush would write into a dead engine. Delete it synchronously
  // now; the panel widget itself dies with ui_ below, releasing the banner
  // closures' remaining PanelSession refs while the engine is still alive.
  delete current_panel_engine_;
  current_panel_engine_ = nullptr;
  // Destroy the wrapper container widget that holds closures capturing
  // shared_ptr<PanelSession>. The session must die synchronously before member
  // destruction, while file_loader_ is still alive (needed by
  // SourcePromotionHost::shutdown). releaseCentralPanel reparents the widget
  // and nulls current_panel_; we delete immediately instead of deferring.
  if (current_panel_ != nullptr) {
    delete releaseCentralPanel();
  }
  // Break the widget-owned pointers to services before session_ destroys
  // the engine — guarantees no late signal dereferences a dead pointer.
  ui_->timelineWidget->setPlaybackEngine(nullptr);
  ui_->curveListPanel->setCatalog(nullptr);
  delete ui_;
}

bool MainWindow::populateTestData() {
  DataEngine& engine = session_->sessionManager().dataEngine();
  auto time_domain_or = engine.createTimeDomain("test_data");
  if (!time_domain_or.has_value()) {
    qCWarning(lcMain) << "createTimeDomain failed:" << QString::fromStdString(time_domain_or.error());
    return false;
  }

  auto dataset_or =
      engine.createDataset(DatasetDescriptor{.source_name = "test-data", .time_domain_id = *time_domain_or});
  if (!dataset_or.has_value()) {
    qCWarning(lcMain) << "createDataset failed:" << QString::fromStdString(dataset_or.error());
    return false;
  }

  DataWriter writer = engine.createWriter();
  auto sin_or = writer.registerScalarSeries(*dataset_or, "test/sin", NumericType::kFloat64);
  auto cos_or = writer.registerScalarSeries(*dataset_or, "test/cos", NumericType::kFloat64);
  if (!sin_or.has_value() || !cos_or.has_value()) {
    qCWarning(lcMain) << "registerScalarSeries failed:"
                      << QString::fromStdString(!sin_or.has_value() ? sin_or.error() : cos_or.error());
    return false;
  }

  for (int index = 0; index < kTestSampleCount; ++index) {
    const double fraction = static_cast<double>(index) / static_cast<double>(kTestSampleCount - 1);
    const double time_sec = fraction * kTestDurationSeconds;
    const auto timestamp = static_cast<Timestamp>(std::llround(time_sec * kNanosecondsPerSecond));
    const double phase = kTwoPi * time_sec;
    writer.appendScalar(*sin_or, timestamp, std::sin(phase));
    writer.appendScalar(*cos_or, timestamp, std::cos(phase));
  }

  const std::vector<TopicId> changed_topics = session_->sessionManager().commitChunks(writer.flushAll());
  if (changed_topics.empty()) {
    qCWarning(lcMain) << "test data commit produced no datastore changes";
    return false;
  }
  session_->catalogModel().rebuildFromDatastore();
  session_->playbackEngine().setRange(displayRange(0.0, kTestDurationSeconds));
  emitDiagnostic(DiagnosticLevel::kInfo, "TestData", "loaded", tr("Loaded test sin/cos data"));
  return true;
}

#ifdef PJ_TARGET_WASM
QDomDocument MainWindow::browserGenericLayoutDocument() const {
  QDomDocument doc = xmlSaveState();
  doc.documentElement().setAttribute(u"binding"_s, u"generic"_s);
  layout_xml::removeDatasetQualifiersForGenericLayout(doc);
  doc.documentElement().appendChild(saveRightPanelState(doc));
  doc.documentElement().appendChild(ui_->leftPanel->saveSourcesState(doc));
  doc.documentElement().appendChild(ui_->curveListPanel->saveListState(doc));
  doc.documentElement().appendChild(saveChromeState(doc));
  doc.documentElement().appendChild(saveSourceTimelineViewState(doc));
  return doc;
}

std::optional<QDomDocument> MainWindow::browserSourceLayoutDocument(QString& error) const {
  error.clear();
  if (browser_layout_runtime_ == nullptr) {
    error = tr("Browser source state is unavailable.");
    return std::nullopt;
  }

  const std::vector<std::pair<DatasetId, QString>> live_datasets = session_->catalogModel().datasets();
  if (live_datasets.empty()) {
    error = tr("Open a browser data source before saving a source-bound layout.");
    return std::nullopt;
  }

  struct SerializableSource {
    QString identity;
    const BrowserLayoutRuntime::SourceRecord* record = nullptr;
  };
  std::vector<SerializableSource> sources;
  QSet<QString> seen_sources;
  for (const auto& [dataset_id, unused_name] : live_datasets) {
    (void)unused_name;
    const QString identity = file_loader_->sourcePathForDataset(dataset_id);
    if (!isBrowserUploadIdentity(identity)) {
      error = tr("Every loaded dataset must be backed by a browser-selected file.");
      return std::nullopt;
    }
    const auto record_it = browser_layout_runtime_->sources.constFind(identity);
    if (record_it == browser_layout_runtime_->sources.cend()) {
      error = tr("A loaded browser source can no longer be identified.");
      return std::nullopt;
    }
    if (!seen_sources.contains(identity)) {
      seen_sources.insert(identity);
      sources.push_back({.identity = identity, .record = &record_it.value()});
    }
  }

  // A source is replayable only while its complete original fan-out shape is
  // live. IDs make a removed-and-coincidentally-similar sibling insufficient;
  // names additionally guard order/shape drift inside one fan-out.
  for (const SerializableSource& source : sources) {
    std::vector<BrowserLayoutRuntime::DatasetShape> current_shape;
    for (const auto& [dataset_id, unused_name] : live_datasets) {
      (void)unused_name;
      if (file_loader_->sourcePathForDataset(dataset_id) != source.identity) {
        continue;
      }
      current_shape.push_back({
          .dataset_id = dataset_id,
          .source_name = session_->catalogModel().datasetSourceName(dataset_id).value_or(QString{}),
      });
    }
    if (source.record == nullptr || source.record->datasets.empty() ||
        current_shape.size() != source.record->datasets.size() ||
        !std::equal(
            current_shape.cbegin(), current_shape.cend(), source.record->datasets.cbegin(),
            [](const auto& current, const auto& original) {
              return current.dataset_id == original.dataset_id && current.source_name == original.source_name;
            })) {
      error = tr("A browser source no longer has its complete original dataset set.");
      return std::nullopt;
    }
  }

  QDomDocument doc = xmlSaveState();
  QDomElement root = doc.documentElement();
  root.setAttribute(u"binding"_s, u"source"_s);

  QDomElement wrapper = doc.createElement(u"previouslyLoaded_Datafiles"_s);
  QHash<DatasetId, int> timeline_order;
  if (source_timeline_controller_ != nullptr) {
    const std::vector<DatasetId> order = source_timeline_controller_->currentTrackOrder();
    for (int index = 0; index < static_cast<int>(order.size()); ++index) {
      timeline_order.insert(order[static_cast<std::size_t>(index)], index);
    }
  }

  QHash<DatasetId, QString> logical_path_for_dataset;
  QHash<QString, QString> logical_path_for_identity;
  for (std::size_t source_index = 0; source_index < sources.size(); ++source_index) {
    const SerializableSource& source = sources[source_index];
    const BrowserLayoutRuntime::SourceRecord& record = *source.record;
    const QString basename = BrowserFileStore::sanitizedBasename(record.display_name);
    if (basename.isEmpty()) {
      error = tr("A browser source has no usable filename.");
      return std::nullopt;
    }
    if (record.content_sha256.isEmpty()) {
      error = tr("A browser source no longer has its staged content fingerprint.");
      return std::nullopt;
    }
    // Preserve W9c.2's compact one-source document. Multi-source documents use
    // an ordinal directory for every source, so duplicate basenames can never
    // alias in fileInfo, curve, processor, or scene qualifiers.
    const QString logical_path =
        sources.size() == 1 ? basename : u"pj4-sources/%1/%2"_s.arg(source_index).arg(basename);
    logical_path_for_identity.insert(source.identity, logical_path);

    QDomElement file_info = doc.createElement(u"fileInfo"_s);
    file_info.setAttribute(u"filename"_s, logical_path);
    file_info.setAttribute(u"prefix"_s, record.prefix);
    file_info.setAttribute(u"content_sha256"_s, record.content_sha256);
    for (std::size_t dataset_index = 0; dataset_index < record.datasets.size(); ++dataset_index) {
      const BrowserLayoutRuntime::DatasetShape& shape = record.datasets[dataset_index];
      logical_path_for_dataset.insert(shape.dataset_id, logical_path);
      QDomElement dataset = doc.createElement(u"dataset"_s);
      dataset.setAttribute(u"source_index"_s, QString::number(dataset_index));
      if (!shape.source_name.isEmpty()) {
        dataset.setAttribute(u"source_name"_s, shape.source_name);
      }
      const QString offset =
          QString::number(session_->sessionManager().sourceDisplayOffset(shape.dataset_id).value.count());
      dataset.setAttribute(u"display_offset_ns"_s, offset);
      if (const auto order_it = timeline_order.constFind(shape.dataset_id); order_it != timeline_order.constEnd()) {
        dataset.setAttribute(u"timeline_order"_s, QString::number(order_it.value()));
      }
      file_info.appendChild(dataset);
      if (dataset_index == 0) {
        file_info.setAttribute(u"display_offset_ns"_s, offset);
        if (dataset.hasAttribute(u"timeline_order"_s)) {
          file_info.setAttribute(u"timeline_order"_s, dataset.attribute(u"timeline_order"_s));
        }
      }
    }

    if (!record.plugin_id.isEmpty()) {
      QDomElement plugin = doc.createElement(u"plugin"_s);
      plugin.setAttribute(u"ID"_s, record.plugin_id);
      // Both identities, exactly like the desktop writer: old readers keep
      // consuming ID; new readers match by the stable manifest id first.
      if (!record.plugin_manifest_id.isEmpty()) {
        plugin.setAttribute(u"manifest_id"_s, record.plugin_manifest_id);
      }
      plugin.setAttribute(u"filepath_mode"_s, u"source"_s);
      const std::string logical_config =
          detail::rewriteReplayFilepaths(record.plugin_config_json.toStdString(), logical_path);
      layout_xml::appendJsonAsCdata(doc, plugin, QString::fromStdString(logical_config));
      file_info.appendChild(plugin);
    }
    wrapper.appendChild(file_info);
  }
  root.appendChild(wrapper);

  layout_xml::stampDatasetSourcePaths(doc, [&logical_path_for_dataset](std::uint32_t id) {
    return logical_path_for_dataset.value(static_cast<DatasetId>(id));
  });
  layout_xml::remapDatasetSourcePaths(
      doc, [&logical_path_for_identity](const QString& path) { return logical_path_for_identity.value(path, path); });
  layout_xml::removeUnvalidatedDatasetIds(doc);

  root.appendChild(saveRightPanelState(doc));
  root.appendChild(ui_->leftPanel->saveSourcesState(doc));
  root.appendChild(ui_->curveListPanel->saveListState(doc));
  root.appendChild(saveChromeState(doc));
  root.appendChild(saveSourceTimelineViewState(doc));

  const QByteArray serialized = doc.toByteArray(2);
  if (layout_xml::containsEphemeralBrowserPath(serialized)) {
    error = tr("The layout still contains an ephemeral browser path and was not downloaded.");
    return std::nullopt;
  }
  return doc;
}
#endif

#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
QByteArray MainWindow::wasmProbeLayoutBytes() const {
  // The fixture and browser download use the exact same serialized document.
  const QDomDocument doc = browserGenericLayoutDocument();
  return doc.toByteArray(2);
}

qsizetype MainWindow::wasmProbeDatasetCount() const {
  return static_cast<qsizetype>(session_->catalogModel().datasets().size());
}

void MainWindow::wasmProbeReportFilterResult() const {
  const auto recipes = session_->sessionManager().dataProcessorService().recipes();
  QStringList processor_ids;
  QStringList output_names;
  processor_ids.reserve(static_cast<qsizetype>(recipes.size()));
  output_names.reserve(static_cast<qsizetype>(recipes.size()));
  for (const DataProcessorService::FilterRecipe& recipe : recipes) {
    processor_ids.push_back(QString::fromStdString(recipe.processor_id));
    output_names.push_back(QString::fromStdString(recipe.output_name).toUtf8().toBase64());
  }
  qInfo(
      "PJ_WASM_FILTER_RECIPES count=%llu ids=%s outputs_b64=%s", static_cast<unsigned long long>(recipes.size()),
      qPrintable(processor_ids.join(u","_s)), qPrintable(output_names.join(u","_s)));

  int visible_plots = 0;
  int visible_curves = 0;
  for (PJ::PlotWidget* plot : findChildren<PJ::PlotWidget*>()) {
    if (!plot->isVisibleTo(this)) {
      continue;
    }
    ++visible_plots;
    for (const PJ::PlotWidgetBase::CurveInfo& info : plot->curveList()) {
      if (info.curve == nullptr || info.curve->data() == nullptr || !info.curve->isVisible()) {
        continue;
      }
      ++visible_curves;
      const auto* data = info.curve->data();
      QStringList y_values;
      const std::size_t count = data->size();
      const std::size_t reported = std::min<std::size_t>(count, 32);
      y_values.reserve(static_cast<qsizetype>(reported));
      for (std::size_t index = 0; index < reported; ++index) {
        y_values.push_back(QString::number(data->sample(index).y(), 'g', 17));
      }
      const QByteArray title = info.curve->title().text().toUtf8().toBase64();
      const QByteArray source = info.source_name.toUtf8().toBase64();
      qInfo(
          "PJ_WASM_FILTER_CURVE title_b64=%s source_b64=%s samples=%llu y=%s truncated=%d", title.constData(),
          source.constData(), static_cast<unsigned long long>(count), qPrintable(y_values.join(u","_s)),
          reported < count ? 1 : 0);
    }
  }
  qInfo("PJ_WASM_FILTER_RESULT visible_plots=%d visible_curves=%d", visible_plots, visible_curves);
}

void MainWindow::wasmProbeReportToolboxTransformResult() const {
  const auto recipes = session_->sessionManager().dataProcessorService().transformRecipes();
  qInfo("PJ_WASM_TOOLBOX_TRANSFORMS count=%llu", static_cast<unsigned long long>(recipes.size()));

  auto* custom = findChild<QTreeWidget*>(u"customView"_s);
  for (const DataProcessorService::TransformRecipe& recipe : recipes) {
    QStringList inputs;
    QStringList outputs;
    inputs.reserve(static_cast<qsizetype>(recipe.inputs.size()));
    outputs.reserve(static_cast<qsizetype>(recipe.outputs.size()));
    for (const std::string& input : recipe.inputs) {
      inputs.push_back(QString::fromStdString(input).toUtf8().toBase64());
    }
    for (const std::string& output : recipe.outputs) {
      outputs.push_back(QString::fromStdString(output).toUtf8().toBase64());
    }

    QPoint custom_center(-1, -1);
    if (custom != nullptr && !recipe.outputs.empty()) {
      const QString wanted = QString::fromStdString(recipe.outputs.front());
      for (QTreeWidgetItemIterator it(custom); *it != nullptr; ++it) {
        QTreeWidgetItem* item = *it;
        if (item->text(0) != wanted) {
          continue;
        }
        custom_center = mapFromGlobal(custom->viewport()->mapToGlobal(custom->visualItemRect(item).center()));
        break;
      }
    }

    const QByteArray key = QString::fromStdString(recipe.key).toUtf8().toBase64();
    const QByteArray owner = QString::fromStdString(recipe.owner_plugin).toUtf8().toBase64();
    const QByteArray user_id = QString::fromStdString(recipe.user_id).toUtf8().toBase64();
    qInfo(
        "PJ_WASM_TOOLBOX_TRANSFORM key_b64=%s owner_b64=%s user_b64=%s backend=%s inputs_b64=%s "
        "outputs_b64=%s ephemeral=%d custom=%d,%d",
        key.constData(), owner.constData(), user_id.constData(), recipe.backend.c_str(),
        qPrintable(inputs.join(u","_s)), qPrintable(outputs.join(u","_s)), recipe.ephemeral ? 1 : 0, custom_center.x(),
        custom_center.y());
  }
}

void MainWindow::wasmProbeReportStreamingState() const {
  static quint64 sequence = 0;
  const quint64 current_sequence = ++sequence;

  const auto widget_center = [this](const QWidget* widget) {
    if (widget == nullptr || !widget->isVisibleTo(this)) {
      return QPoint(-1, -1);
    }
    return mapFromGlobal(widget->mapToGlobal(widget->rect().center()));
  };

  auto* source_combo = findChild<QComboBox*>(u"comboStreaming"_s);
  auto* stream_tab = findChild<QToolButton*>(u"tabStream"_s);
  auto* start_button = findChild<QPushButton*>(u"buttonStreamingOptions"_s);
  auto* pause_button = findChild<QPushButton*>(u"buttonStreamingPause"_s);
  auto* buffer = findChild<QWidget*>(u"streamingSpinBox"_s);
  auto* tree = findChild<QTreeWidget*>(u"treeView"_s);
  auto* datasets_menu = findChild<QToolButton*>(u"buttonDatasetsMenu"_s);

  QPushButton* remove_all = nullptr;
  for (QPushButton* button : findChildren<QPushButton*>()) {
    if (button->text() == tr("Remove all Datasets")) {
      remove_all = button;
      break;
    }
  }

  const auto item_center = [this, tree](QTreeWidgetItem* item) {
    if (tree == nullptr || item == nullptr || !tree->isVisibleTo(this) || item->isHidden()) {
      return QPoint(-1, -1);
    }
    for (QTreeWidgetItem* parent = item->parent(); parent != nullptr; parent = parent->parent()) {
      if (!parent->isExpanded() || parent->isHidden()) {
        return QPoint(-1, -1);
      }
    }
    const QRect visual = tree->visualItemRect(item);
    if (!visual.isValid() || visual.isEmpty() || !tree->viewport()->rect().intersects(visual)) {
      return QPoint(-1, -1);
    }
    return mapFromGlobal(tree->viewport()->mapToGlobal(visual.center()));
  };

  QTreeWidgetItem* dataset_item = tree != nullptr && tree->topLevelItemCount() > 0 ? tree->topLevelItem(0) : nullptr;
  QTreeWidgetItem* topic_item = nullptr;
  QTreeWidgetItem* field_item = nullptr;
  int tree_rows = 0;
  if (tree != nullptr) {
    for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
      QTreeWidgetItem* item = *it;
      ++tree_rows;
      if (item->text(0) == u"dummy/sin_cos"_s) {
        topic_item = item;
      }
      if (item->text(0) == u"sin"_s && item->childCount() == 0) {
        field_item = item;
      }
    }
  }

  PlotWidget* visible_plot = nullptr;
  PlotRhiCanvas* canvas = nullptr;
  QWidget* plot_drop_target = nullptr;
  const QwtSeriesData<QPointF>* plotted_data = nullptr;
  QString plotted_title;
  quint64 vertices = 0;
  for (PlotWidget* plot : findChildren<PlotWidget*>()) {
    if (!plot->isVisibleTo(this)) {
      continue;
    }
    visible_plot = plot;
    canvas = plot->findChild<PlotRhiCanvas*>();
    if (canvas != nullptr) {
      vertices = canvas->lastVertexCount();
      plot_drop_target = canvas;
    }
    for (const PlotWidgetBase::CurveInfo& info : plot->curveList()) {
      if (info.curve != nullptr && info.curve->isVisible() && info.curve->data() != nullptr) {
        plotted_data = info.curve->data();
        plotted_title = info.curve->title().text();
        break;
      }
    }
    break;
  }
  if (plot_drop_target == nullptr) {
    for (DockWidget* dock : findChildren<DockWidget*>()) {
      if (dock != nullptr && dock->isVisibleTo(this) && dock->window() == this) {
        plot_drop_target = dock;
        break;
      }
    }
  }

  const std::size_t sample_count = plotted_data != nullptr ? plotted_data->size() : 0;
  QPointF first_sample;
  QPointF last_sample;
  if (sample_count > 0) {
    first_sample = plotted_data->sample(0);
    last_sample = plotted_data->sample(sample_count - 1);
  }

  const QString source = source_combo != nullptr ? source_combo->currentText() : QString();
  const QByteArray source_b64 = source.toUtf8().toBase64();
  const QByteArray dataset_b64 = dataset_item != nullptr ? dataset_item->text(0).toUtf8().toBase64() : QByteArray();
  const QByteArray topic_b64 = topic_item != nullptr ? topic_item->text(0).toUtf8().toBase64() : QByteArray();
  const QByteArray field_b64 = field_item != nullptr ? field_item->text(0).toUtf8().toBase64() : QByteArray();
  const QByteArray title_b64 = plotted_title.toUtf8().toBase64();
  const auto& playback = session_->playbackEngine();
  qInfo(
      "PJ_WASM_STREAM_STATE sequence=%llu sources=%d source_b64=%s active=%d datasets=%lld catalog=%llu paused=%d "
      "playing=%d range=%.17g,%.17g current=%.17g",
      static_cast<unsigned long long>(current_sequence), source_combo != nullptr ? source_combo->count() : 0,
      source_b64.constData(), streaming_manager_ != nullptr && streaming_manager_->hasActiveSession() ? 1 : 0,
      static_cast<long long>(wasmProbeDatasetCount()),
      static_cast<unsigned long long>(session_->catalogModel().curves().size()),
      pause_button != nullptr && pause_button->isChecked() ? 1 : 0, playback.isPlaying() ? 1 : 0,
      toAxisDouble(playback.rangeMin()), toAxisDouble(playback.rangeMax()), toAxisDouble(playback.currentTime()));

  const QPoint tab_center = widget_center(stream_tab);
  const QPoint combo_center = widget_center(source_combo);
  const QPoint start_center = widget_center(start_button);
  const QPoint pause_center = widget_center(pause_button);
  const QPoint buffer_center = widget_center(buffer);
  const QPoint tree_center = widget_center(tree);
  const QPoint menu_center = widget_center(datasets_menu);
  const QPoint remove_center = widget_center(remove_all);
  const QPoint canvas_center = widget_center(plot_drop_target);
  qInfo(
      "PJ_WASM_STREAM_CONTROLS sequence=%llu tab=%d,%d combo=%d,%d start=%d,%d pause=%d,%d buffer=%d,%d "
      "tree=%d,%d menu=%d,%d remove=%d,%d canvas=%d,%d",
      static_cast<unsigned long long>(current_sequence), tab_center.x(), tab_center.y(), combo_center.x(),
      combo_center.y(), start_center.x(), start_center.y(), pause_center.x(), pause_center.y(), buffer_center.x(),
      buffer_center.y(), tree_center.x(), tree_center.y(), menu_center.x(), menu_center.y(), remove_center.x(),
      remove_center.y(), canvas_center.x(), canvas_center.y());

  const QPoint dataset_center = item_center(dataset_item);
  const QPoint topic_center = item_center(topic_item);
  const QPoint field_center = item_center(field_item);
  qInfo(
      "PJ_WASM_STREAM_TREE sequence=%llu rows=%d dataset_b64=%s dataset=%d,%d topic_b64=%s topic=%d,%d "
      "field_b64=%s field=%d,%d",
      static_cast<unsigned long long>(current_sequence), tree_rows, dataset_b64.constData(), dataset_center.x(),
      dataset_center.y(), topic_b64.constData(), topic_center.x(), topic_center.y(), field_b64.constData(),
      field_center.x(), field_center.y());
  qInfo(
      "PJ_WASM_STREAM_CURVE sequence=%llu plotted=%d title_b64=%s samples=%llu first=%.17g,%.17g "
      "last=%.17g,%.17g vertices=%llu",
      static_cast<unsigned long long>(current_sequence), visible_plot != nullptr && plotted_data != nullptr ? 1 : 0,
      title_b64.constData(), static_cast<unsigned long long>(sample_count), first_sample.x(), first_sample.y(),
      last_sample.x(), last_sample.y(), static_cast<unsigned long long>(vertices));
}

void MainWindow::wasmProbeSupersedeBrowserReplayPicker() {
  const quint64 generation = beginBrowserReplayPickerGeneration();
  qInfo("PJ_WASM_SOURCE_LAYOUT_PICKER_SUPERSEDED generation=%llu", static_cast<unsigned long long>(generation));
}
#endif

void MainWindow::onOpenMarketplace() {
#ifndef PJ_TARGET_WASM
  auto& catalog = session_->extensionCatalog();

  // Both the File menu and the title-bar Update button land here, so a request
  // can arrive while the panel is already up. Re-activating beats rebuilding:
  // the user keeps their scroll position and filter, and the registry is not
  // re-read for nothing.
  //
  // The guard fires only when the marketplace IS the central panel — comparing
  // against its own container. Panels migrated into a tab keep the controller
  // alive but leave the central area free (or occupied by an unrelated toolbox);
  // there a fresh central-area instance is the right answer, and this guard
  // stays out of the way.
  if (!marketplace_panel_.isNull() && current_panel_ == marketplace_container_.data() && current_panel_ != nullptr) {
    marketplace_panel_->activateEmbedded();
    return;
  }

  // Embed the marketplace in the central area (like a toolbox), not a modal
  // window. The MarketplaceWindow is a hidden controller: it owns the UI and
  // receives its child-widget signals; its content widget is lifted into a
  // banner-wrapped takeover panel that can be closed or migrated to a persistent
  // central tab.
  auto* mkt = new MarketplaceWindow(&catalog.extensionManager(), effectiveRegistryUrl(), this);
  mkt->setChromeMetrics(chrome_metrics_);
  QWidget* content = mkt->contentWidget();

  // Gear → Preferences ▸ Plugins (deferred so the click's own teardown doesn't
  // run inside the panel's event handling).
  connect(mkt, &MarketplaceWindow::pluginPreferencesRequested, this, [this]() {
    QTimer::singleShot(0, this, [this]() {
      PreferencesDialog prefs(*theme_, this);
      prefs.setChromeMetrics(chrome_metrics_);
      bindRecordingPreferences(prefs);
      prefs.showPage(PreferencesDialog::kPluginsPage);
      prefs.exec();
    });
  });

  // Reload the plugin catalog if anything was installed/uninstalled while open.
  // Guarded because the controller's lifetime is tied to the container below: on
  // the migrate exit this runs when the tab closes, by which point the container
  // may already have taken the controller with it.
  const auto reload_if_changed = [this, mkt = QPointer<MarketplaceWindow>(mkt)]() {
    if (!mkt.isNull() && mkt->installationsChanged()) {
      session_->extensionCatalog().reload();
    }
  };

  const WrappedToolboxPanel wrapped = wrapToolboxPanel(
      content, tr("Marketplace"),
      /*on_close=*/
      [this, reload_if_changed]() {
        reload_if_changed();
        restoreCentralArea();
      },
      /*on_migrate=*/
      [this, reload_if_changed]() {
        QWidget* released = releaseCentralPanel();
        if (released == nullptr) {
          return;
        }
        ui_->tabbedPlotWidget->addWidgetTab(
            tr("Marketplace"), released, [reload_if_changed]() { reload_if_changed(); });
      });

  // The container owns every widget the controller's ui_ points at, so the
  // controller must not outlive it. Closing and migrating are not the only exits:
  // presentPanel() dismisses whatever panel is already up, which deletes the
  // container without running either callback. A controller left behind stays
  // connected to the shared ExtensionManager's install/uninstall signals, and the
  // next one to fire writes through ui_ into freed widgets.
  //
  // Passing the controller as the context object makes Qt drop the connection when
  // the controller dies, so the migrate exit (where the container survives inside
  // a tab) cannot fire this against a stale pointer.
  connect(wrapped.container, &QObject::destroyed, mkt, &QObject::deleteLater);

  if (!presentPanel(wrapped.container)) {
    wrapped.container->deleteLater();
    return;
  }
  // QPointer, so the re-activation guard above sees empty slots again as soon
  // as the controller (and its container) go — no explicit clearing on any of
  // the exit paths.
  marketplace_panel_ = mkt;
  marketplace_container_ = wrapped.container;
  mkt->activateEmbedded();
#endif
}

void MainWindow::onLoadDataRequested() {
  file_loader_->openFromDialog(this);
}

void MainWindow::onReloadDataRequested() {
  const auto src = session_->sessionManager().lastLoadedSource();
  if (!src.has_value()) {
    return;
  }
  reloadSource(src->path, src->plugin_id, src->plugin_config_json);
}

void MainWindow::reloadSource(const QString& path, const QString& plugin_id, const QString& plugin_config_json) {
  // Prevent re-entry; success re-enables via onFileLoaded.
  ui_->leftPanel->setReloadEnabled(false);
  LoadHints hints{
      .expected_manifest_id = QString(),  // interactive reload: the display name is the recorded identity
      .expected_plugin_id = plugin_id,
      .preset_config_json = plugin_config_json,
      .dialog_policy = !plugin_id.isEmpty() && !plugin_config_json.isEmpty() ? DialogPolicy::kPreferPreset
                                                                             : DialogPolicy::kInteractive,
  };
  file_loader_->loadFile(path, this, hints);
}

void MainWindow::onReloadDatasetRequested(DatasetId dataset_id) {
  const QString path = file_loader_->sourcePathForDataset(dataset_id);
  if (path.isEmpty()) {
    return;  // not file-backed; the menu item should have been disabled
  }
  // Recover the plugin + config recorded when THIS path was loaded (not just the
  // most recent load) so a multi-file session reloads each source with its own
  // settings. An untracked path (e.g. record pruned) reloads through the dialog.
  const auto& sources = session_->sessionManager().loadedSources();
  const auto recorded = std::find_if(sources.begin(), sources.end(), [&path](const auto& src) {
    return FileLoader::sameSourceIdentity(src.path, path);
  });
  if (recorded != sources.end()) {
    reloadSource(recorded->path, recorded->plugin_id, recorded->plugin_config_json);
  } else {
    reloadSource(path, QString(), QString());
  }
}

void MainWindow::onReplaceDatasetRequested(DatasetId dataset_id) {
  file_loader_->replaceFromDialog(dataset_id, this);
}

QSet<QString> MainWindow::captureHistoryDataUniverse() const {
  QSet<QString> universe;
  const QChar separator(QLatin1Char('\x1f'));
  const auto signature = [separator](const QStringList& fields) { return fields.join(separator); };

  // Processor outputs belong to workspace state and may be recreated by undo.
  // Only raw inputs constrain whether an older snapshot remains restorable.
  const DataProcessorService& processors = session_->sessionManager().dataProcessorService();
  std::unordered_set<TopicId> processor_outputs = processors.ephemeralOutputTopics();
  for (const DataProcessorService::FilterRecipe& recipe : processors.recipes()) {
    processor_outputs.insert(recipe.output_topic_id);
  }
  for (const DataProcessorService::TransformRecipe& recipe : processors.transformRecipes()) {
    processor_outputs.insert(recipe.output_topic_ids.begin(), recipe.output_topic_ids.end());
  }

  // Use datastore identities, including hidden catalog rows. A processor can
  // retain a raw dependency after its input has disappeared from the browser.
  SessionManager& session_manager = session_->sessionManager();
  const DataEngine& engine = session_manager.dataEngine();
  QHash<DatasetId, QStringList> dataset_identities;
  {
    auto lock = engine.lockEngine();
    for (const DatasetId dataset_id : engine.listDatasets()) {
      const DatasetInfo* dataset = engine.getDataset(dataset_id);
      if (dataset == nullptr) {
        continue;
      }
      const QStringList dataset_identity{
          QString::number(dataset_id), QString::fromStdString(dataset->source_name),
          session_manager.datasetSourcePath(dataset_id), QString::number(dataset->time_domain.id)};
      dataset_identities.insert(dataset_id, dataset_identity);
      for (const TopicId topic_id : engine.listTopics(dataset_id)) {
        if (processor_outputs.contains(topic_id)) {
          continue;
        }
        const TopicStorage* storage = engine.getTopicStorage(topic_id);
        if (storage == nullptr) {
          continue;
        }
        const TopicDescriptor& topic_descriptor = storage->descriptor();
        QStringList topic_identity{u"scalar-topic"_s};
        topic_identity.append(dataset_identity);
        topic_identity.append(
            {QString::number(topic_id), QString::fromStdString(topic_descriptor.name),
             QString::number(topic_descriptor.schema_id)});
        universe.insert(signature(topic_identity));
        const auto& columns = storage->columnDescriptors();
        for (std::size_t column = 0; column < columns.size(); ++column) {
          const ColumnDescriptor& column_descriptor = columns[column];
          QStringList column_identity{u"scalar-column"_s};
          column_identity.append(dataset_identity);
          column_identity.append(
              {QString::number(topic_id), QString::number(static_cast<qulonglong>(column)),
               QString::number(column_descriptor.field_id),
               QString::number(static_cast<int>(column_descriptor.logical_type)),
               QString::fromStdString(column_descriptor.field_path)});
          universe.insert(signature(column_identity));
        }
      }
    }
  }

  // ObjectStore has an independent lock; never nest it under DataEngine's.
  const ObjectStore& objects = session_manager.objectStore();
  for (const ObjectTopicId topic_id : objects.listTopics()) {
    const ObjectTopicDescriptor object_descriptor = objects.descriptor(topic_id);
    QStringList object_identity{u"object-topic"_s};
    object_identity.append(dataset_identities.value(
        object_descriptor.dataset_id, {QString::number(object_descriptor.dataset_id), QString(),
                                       session_manager.datasetSourcePath(object_descriptor.dataset_id), QString()}));
    object_identity.append(
        {QString::number(topic_id.id), QString::fromStdString(object_descriptor.topic_name),
         QString::fromStdString(object_descriptor.metadata_json)});
    universe.insert(signature(object_identity));
  }
  return universe;
}

void MainWindow::reconcileHistoryWithDataUniverse() {
  const QSet<QString> current_universe = captureHistoryDataUniverse();
  if (!current_universe.contains(history_data_universe_)) {
    resetUndoHistory();
    return;
  }
  history_data_universe_ = current_universe;
  hydrateCurrentUndoState(/*refresh_data_universe=*/false);
}

void MainWindow::onFileLoaded(
    const QString& path, const QString& prefix, const QString& plugin_id, const QString& plugin_config_json,
    const QString& plugin_manifest_id) {
  // MainWindow is the shell that wires load completion to the runtime —
  // recording the source (incl. plugin id + json) and seeding playback are
  // pj_runtime concerns; we just relay. Prefix is empty in v1 until the
  // load dialog gains a prefix input.
  // A browser upload is backed by ephemeral MEMFS and cannot be replayed after
  // refresh, so never expose its logical identity as a recent/reloadable path.
  // The dataset itself still carries that identity for same-session disambiguation.
  if (!isBrowserUploadIdentity(path)) {
    session_->sessionManager().recordLoadedSource(path, prefix, plugin_id, plugin_config_json, plugin_manifest_id);
  }
#ifdef PJ_TARGET_WASM
  if (isBrowserUploadIdentity(path) && browser_layout_runtime_ != nullptr) {
    const QString display_name = BrowserFileStore::displayNameForIdentity(path);
    if (!display_name.isEmpty()) {
      std::vector<BrowserLayoutRuntime::DatasetShape> datasets;
      for (const auto& [dataset_id, unused_name] : session_->catalogModel().datasets()) {
        (void)unused_name;
        if (file_loader_->sourcePathForDataset(dataset_id) == path) {
          datasets.push_back({
              .dataset_id = dataset_id,
              .source_name = session_->catalogModel().datasetSourceName(dataset_id).value_or(QString{}),
          });
        }
      }
      browser_layout_runtime_->sources.insert(
          path, BrowserLayoutRuntime::SourceRecord{
                    .display_name = display_name,
                    .prefix = prefix,
                    .content_sha256 = file_loader_->browserContentSha256(path),
                    .plugin_id = plugin_id,
                    .plugin_manifest_id = plugin_manifest_id,
                    .plugin_config_json = plugin_config_json,
                    .datasets = std::move(datasets),
                });
    }
  }
#endif
#ifdef PJ_WITH_SCENE3D
  // Feed the loaded source path to every existing 3D dock so its URDF package
  // resolver can key per-source remembered roots and auto-seed search roots from
  // the file's directory (M.2/M.16). Docks created later pick it up from
  // lastLoadedSource() in makeSceneDock.
  forEachDock([&path](DockWidget* dock) {
    if (dock->objectWidget() == nullptr) {
      return;
    }
    if (auto* scene3d = qobject_cast<Scene3DDockWidget*>(dock->objectWidget()->widget())) {
      scene3d->setSourcePath(path);
    }
  });
#endif
  // TODO(embedded-assets): route in-band embedded assets to Scene3D docks once the
  // load path surfaces the extracted asset map (resolver step 0).
  if (pending_source_replacement_.has_value() && layout_xml::isSamePath(pending_source_replacement_->path, path)) {
    const CapturedWorkspace replacement = pending_source_replacement_->workspace;
    if (restoreWorkspaceState(
            replacement, MissingCurvePolicy::kExact, TimelineRestoreMode::kPortableSourceReplacement) !=
        RestoreResult::kApplied) {
      emitDiagnostic(
          DiagnosticLevel::kWarning, "Reload", "workspace-rebind-failed",
          tr("The source was reloaded, but some workspace bindings could not be mapped to its new datasets."));
    }
    pending_source_replacement_.reset();
  }
  session_->seedPlaybackFromSession();
  // A same-source reload evicts the old dataset's objects AFTER the removeDataset
  // signal fired, so re-run the coherence pass here to reset any 2D viewer still
  // bound to an evicted topic. Idempotent for a first/additive load.
  syncWidgetsToCatalog();
  if (!progressive_layout_in_flight_) {
    reconcileHistoryWithDataUniverse();
  }
  // Seed every data widget AND the curve-list Value column with the just-set
  // playhead. seedPlaybackFromSession positions the cursor, but currentTimeChanged
  // only fires on an actual change — so a fresh load that lands the cursor where it
  // already was (e.g. 0) would otherwise leave the value column blank until the
  // first scrub. Mirrors PJ3's update2ndColumnValues-after-load.
  broadcastTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
  // Browser uploads deliberately are not recorded as reloadable paths: their
  // logical identity is not a host path and the browser must grant a fresh file
  // selection gesture. Do not leave a button enabled that can only no-op.
  ui_->leftPanel->setReloadEnabled(session_->sessionManager().lastLoadedSource().has_value());
#ifdef PJ_TARGET_WASM
  updateBrowserLayoutActions();
#endif
}

bool MainWindow::confirmAndRemoveDependentTransforms(const std::vector<TopicId>& removed_topics) {
  auto& dp = session_->sessionManager().dataProcessorService();
  const std::vector<TopicId> outputs = dp.dependentProcessorOutputs(removed_topics);
  if (outputs.empty()) {
    return true;  // nothing depends on it — proceed
  }
  const std::unordered_set<TopicId> dependent_outputs(outputs.begin(), outputs.end());

  // One catalog pass collects both what the dialog shows and what gets removed
  // from the curve list after confirmation.
  QStringList names;
  QStringList dependent_curve_keys;
  for (const CatalogItem& item : session_->catalogModel().items()) {
    if (const ScalarFieldPayload* scalar = asScalarField(item);
        scalar != nullptr && dependent_outputs.count(scalar->topic_id) != 0) {
      names
          << (scalar->field_path.isEmpty() ? item.topic_name : item.topic_name + QLatin1Char('/') + scalar->field_path);
      dependent_curve_keys << item.key;
    }
  }
  names.removeDuplicates();
  if (names.isEmpty()) {
    names << tr("%n derived series", nullptr, static_cast<int>(dependent_outputs.size()));
  }
  const int choice = MessageBox::question(
      this, tr("Delete derived series?"),
      tr("These derived series depend on what you are deleting and will also be removed:\n\n• %1")
          .arg(names.join(u"\n• "_s)),
      {{tr("Delete"), MessageBox::kDestructiveRole}, {tr("Cancel"), MessageBox::kCancelRole}});
  if (choice != 0) {
    return false;  // user cancelled — leave everything intact
  }
  for (const QString& key : dependent_curve_keys) {
    ui_->curveListPanel->removeCustomCurve(key);
  }
  if (const Status removed = dp.removeProcessorsDependingOn(removed_topics); !removed.has_value()) {
    emitDiagnostic(
        DiagnosticLevel::kError, "Processors", "dependent-remove-failed", QString::fromStdString(removed.error()));
    return false;
  }
  session_->catalogModel().rebuildFromDatastore();
  return true;
}

void MainWindow::onCatalogTrashRequested(QStringList keys, bool covers_all) {
  CatalogModel& catalog = session_->catalogModel();
  if (covers_all) {
    if (streaming_manager_ != nullptr && streaming_manager_->hasActiveSession()) {
      streaming_manager_->stopAllAndWait(tr("dataset removed"));
      active_streaming_dataset_id_ = 0;
      streaming_playback_seeded_ = false;
    }
    if (pending_binder_ != nullptr) {
      pending_binder_->clear();
    }
    clearPendingSceneRestores();
    // Free ObjectStore topics before the catalog wipe so the cleared()
    // subscription sees them gone and resets 2D viewers (symmetric with the
    // "Remove all Datasets" path). Keep lastLoadedSource for reload.
    // Session-scoped all_datasets generators go too — see the sibling wipe path.
    session_->sessionManager().markerService().clearAllGenerators();
    session_->sessionManager().clearAllObjects();
    // TF buffers derive from the just-evicted objects; drop them with the data.
#ifdef PJ_WITH_SCENE3D
    if (transform_service_ != nullptr) {
      transform_service_->invalidateAll();
    }
#endif
    // REAL delete: drop the catalog (no tombstone), then erase every dataset's scalar
    // storage from the engine. clearAll()'s cleared() tears down curve adapters first, so
    // the engine erase satisfies DataEngine::removeDataset's invalidate-first contract
    // (and resets the transport to empty via the AppSession cleared() hook).
    catalog.clearAll(/*tombstone=*/false);
    SessionManager& session_manager = session_->sessionManager();
    for (const DatasetId id : session_manager.dataEngine().listDatasets()) {
      session_manager.removeDataset(id);
    }
    resetUndoHistory();
#ifdef PJ_TARGET_WASM
    updateBrowserLayoutActions();
#endif
    return;
  }
  // Cascade to derived series that depend on the trashed ones (warn + remove).
  {
    std::vector<TopicId> removed_topics;
    for (const QString& key : keys) {
      if (const auto d = catalog.curveDescriptor(key)) {
        removed_topics.push_back(d->topic_id);
      }
    }
    if (!confirmAndRemoveDependentTransforms(removed_topics)) {
      return;  // user cancelled
    }
  }

  // Evict the trashed object topics before mutating the catalog, so the
  // itemsRemoved subscription's revalidateObjects() (which checks the
  // ObjectStore, not the catalog) drops their 2D layers. Scalar keys are ignored
  // here (engine is append-only).
  std::vector<ObjectTopicId> trashed_objects;
  for (const QString& key : keys) {
    if (const auto item = catalog.itemDescriptor(key); item.has_value()) {
      forEachSceneDock([&item](SceneDockWidget* scene_dock) {
        scene_dock->discardPendingRestoresForTopic(item->dataset_id, item->topic_name);
      });
      if (const ObjectTopicPayload* obj = asObjectTopic(*item)) {
        trashed_objects.push_back(obj->object_topic_id);
      }
    }
  }
  session_->sessionManager().evictObjectTopics(trashed_objects);
  catalog.removeItems(std::vector<QString>(keys.begin(), keys.end()));
  if (pending_binder_ != nullptr) {
    static_cast<void>(pending_binder_->flush({}));
  }
  // Shrink the playback range to the surviving visible data right away,
  // rather than only on the next load — unless a streaming dataset exists:
  // the slider is then scoped to the active stream (see the streaming range
  // wiring in the constructor) and a catalog-wide recompute would stomp it.
  if (active_streaming_dataset_id_ == 0) {
    session_->seedPlaybackFromSession();
  }
  resetUndoHistory();
#ifdef PJ_TARGET_WASM
  updateBrowserLayoutActions();
#endif
}

void MainWindow::removeDatasetData(DatasetId dataset_id) {
  forEachSceneDock(
      [dataset_id](SceneDockWidget* scene_dock) { scene_dock->discardPendingRestoresForDataset(dataset_id); });
  if (pending_binder_ != nullptr) {
    static_cast<void>(pending_binder_->flush({}));
  }
  if (dataset_id == active_streaming_dataset_id_) {
    if (streaming_manager_ != nullptr) {
      streaming_manager_->stopDatasetAndWait(dataset_id, tr("dataset removed"));
    }
    active_streaming_dataset_id_ = 0;
    streaming_playback_seeded_ = false;
  }
  // A REAL delete: erase the dataset's data from the object store AND the engine —
  // no tombstone — so a later load of the same source is a clean fresh load (mints
  // a new DatasetId), not a reattach to an emptied shell.
  //
  // Order matters. Invalidate the TF buffer first (it was built from these object
  // topics; invalidateDataset walks listTopics(dataset_id), empty once eviction
  // runs). Then evict the object payloads (so the catalog's itemsRemoved prunes
  // 2D/3D layers). Then drop the catalog items (tearing down every curve adapter).
  // ONLY THEN erase the engine's scalar storage: DataEngine::removeDataset requires
  // all readers/adapters invalidated first, which the synchronous teardown above guarantees.
#ifdef PJ_WITH_SCENE3D
  if (transform_service_ != nullptr) {
    transform_service_->invalidateDataset(dataset_id);
  }
#endif
  session_->sessionManager().evictDatasetObjects(dataset_id);
  session_->catalogModel().removeDataset(dataset_id, /*tombstone=*/false);
  session_->sessionManager().removeDataset(dataset_id);
  // Drop this dataset's file association (hygiene). Resurrection is prevented by
  // the layout-save liveness filter (appendDataSourceElement), not by mutating
  // loaded_sources_ — that list is kept whole so the quick-reload button still works.
  file_loader_->untrackDataset(dataset_id);
#ifdef PJ_TARGET_WASM
  updateBrowserLayoutActions();
#endif
}

void MainWindow::onRemoveDatasetsRequested(const QList<DatasetId>& dataset_ids) {
  if (dataset_ids.isEmpty()) {
    return;
  }
  // One combined confirmation. Single removal keeps the named wording; a batch
  // shows the count.
  QString message;
  if (dataset_ids.size() == 1) {
    QString name = QString::number(dataset_ids.front());
    for (const auto& [id, dataset_name] : session_->catalogModel().datasets()) {
      if (id == dataset_ids.front()) {
        name = dataset_name;
        break;
      }
    }
    message = tr("Are you sure you want to remove '%1' and its data?").arg(name);
  } else {
    message = tr("Are you sure you want to remove these %1 datasets and their data?").arg(dataset_ids.size());
  }
  const int choice = MessageBox::question(
      this, dataset_ids.size() == 1 ? tr("Remove dataset") : tr("Remove datasets"), message,
      {{tr("Remove"), MessageBox::kDestructiveRole}, {tr("Cancel"), MessageBox::kCancelRole}});
  if (choice != 0) {
    return;  // Cancel / Esc
  }

  // Cascade to derived series whose input lives in any of these datasets (their
  // output is typically in another dataset, so removeDataset alone would orphan
  // them). Warn + remove those transforms and their Custom Series entries; abort
  // the whole removal on cancel. No-op when nothing depends on the removed data.
  {
    std::vector<TopicId> removed_topics;
    for (const DatasetId dataset_id : dataset_ids) {
      const std::vector<TopicId> dataset_topics = session_->sessionManager().dataEngine().listTopics(dataset_id);
      removed_topics.insert(removed_topics.end(), dataset_topics.begin(), dataset_topics.end());
    }
    if (!confirmAndRemoveDependentTransforms(removed_topics)) {
      return;  // user cancelled
    }
  }

  for (const DatasetId id : dataset_ids) {
    removeDatasetData(id);
  }
  // A confirmed removal changes the data universe under the playhead, so halt
  // playback and shrink the range to the survivors (or reset to empty when none
  // remain) — unless a live stream is active, where the slider stays scoped to
  // the tip and follow-live keeps running. Reset undo once.
  if (active_streaming_dataset_id_ == 0) {
    session_->playbackEngine().pause();
    session_->seedPlaybackFromSession();
  }
  resetUndoHistory();
}

void MainWindow::onMergeDatasetsRequested(const QList<DatasetId>& dataset_ids) {
  std::vector<DatasetId> ids;
  ids.reserve(static_cast<std::size_t>(dataset_ids.size()));
  for (const DatasetId id : dataset_ids) {
    ids.push_back(id);
  }
  // confirmAndMergeDatasets shows the shared destructive-merge warning and gates on
  // ≥2 data-bearing datasets; mark the surviving anchor on the Source Timeline.
  if (const auto anchor = confirmAndMergeDatasets(this, *session_, ids)) {
    if (source_timeline_controller_ != nullptr) {
      source_timeline_controller_->markDatasetMerged(*anchor);
    }
#ifdef PJ_TARGET_WASM
    // A merge result cannot be recreated by replaying one browser file. Remove
    // its save-ledger entry so the single-source action cannot claim otherwise.
    if (browser_layout_runtime_ != nullptr) {
      browser_layout_runtime_->sources.remove(file_loader_->sourcePathForDataset(*anchor));
    }
    updateBrowserLayoutActions();
#endif
  }
}

void MainWindow::onShowPreferencesDialog() {
  PreferencesDialog dlg(*theme_, this);
  dlg.setChromeMetrics(chrome_metrics_);
  bindRecordingPreferences(dlg);
  dlg.exec();
}

void MainWindow::bindRecordingPreferences(PreferencesDialog& dialog) {
  connect(&dialog, &PreferencesDialog::recordingSettingsChanged, this, [this]() {
    recording_service_->setSettings(RecordingService::loadSettings());
  });
}

void MainWindow::onShowAboutDialog() {
  AboutDialog dialog(this);
  dialog.setChromeMetrics(chrome_metrics_);
  dialog.exec();
}

void MainWindow::showToast(const QString& message, const QPixmap& icon) {
  if (!toast_manager_) {
    toast_manager_ = new ToastManager(this);
  }
  toast_manager_->showToast(message, icon);
}

void MainWindow::checkForUpdates(bool interactive) {
  if (!update_checker_) {
    update_checker_ = new UpdateChecker(this);
  }

  // Rebind the outcome handlers so this call's `interactive` is captured
  // per-request: checkLatestRelease() aborts any prior in-flight check (whose
  // handlers are disconnected here regardless), so the "up to date" / "couldn't
  // check" toasts can never be attributed to the wrong request.
  disconnect(update_available_conn_);
  disconnect(up_to_date_conn_);
  disconnect(check_failed_conn_);

  update_available_conn_ =
      connect(update_checker_, &UpdateChecker::updateAvailable, this, [this](const ReleaseInfo& release) {
        QString message = tr("New release available: <b>%1</b>").arg(release.name.toHtmlEscaped());
        if (!release.html_url.isEmpty()) {
          message += u"<br>"_s + tr("<a href=\"%1\">View on GitHub</a>").arg(release.html_url);
        }
        showToast(message, QPixmap(u":/resources/success_kid.png"_s));
      });

  up_to_date_conn_ = connect(update_checker_, &UpdateChecker::upToDate, this, [this, interactive]() {
    if (interactive) {
      showToast(tr("PlotJuggler is up to date."));
    }
  });

  check_failed_conn_ =
      connect(update_checker_, &UpdateChecker::checkFailed, this, [this, interactive](const QString& reason) {
        qWarning("Update check failed: %s", qUtf8Printable(reason));
        if (interactive) {
          showToast(tr("Could not check for updates. Please try again later."));
        }
      });

  update_checker_->checkLatestRelease();
}

void MainWindow::onCheckForUpdates() {
  checkForUpdates(/*interactive=*/true);
}

void MainWindow::checkExtensionUpdates() {
#ifndef PJ_TARGET_WASM
  if (update_scan_registry_ == nullptr) {
    update_scan_registry_ = new RegistryManager(this);
    // Wired once (the scan runs at most once per launch). A failed fetch
    // (offline / parse error) leaves the badge hidden — no user-facing noise.
    connect(update_scan_registry_, &RegistryManager::fetchFinished, this, [this](bool ok) {
      if (!ok) {
        return;
      }
      const ExtensionManager& manager = session_->extensionCatalog().extensionManager();
      int updatable = 0;
      for (const Extension& ext : update_scan_registry_->extensions()) {
        if (manager.hasUpdate(ext)) {
          ++updatable;
        }
      }
      title_bar_->setExtensionUpdateCount(updatable);
    });
  }
  update_scan_registry_->fetchRegistry(effectiveRegistryUrl());
#endif  // PJ_TARGET_WASM: browser builds install nothing dynamically — no updates to badge
}

void MainWindow::sendTelemetryPing(const QString& installation) {
  if (!telemetry_ping_) {
    telemetry_ping_ = new TelemetryPing(this);
  }
  telemetry_ping_->send(installation);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  if (toast_manager_) {
    toast_manager_->updatePosition();
  }
}

void MainWindow::paintEvent(QPaintEvent* event) {
  QMainWindow::paintEvent(event);
  if (isMaximized() || isFullScreen()) {
    return;  // The window fills the screen — no edge to outline.
  }

  const theme::Theme t = theme::appTheme();
  const int border_width = PJ::theme::stroke(theme::Stroke::Hairline);

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  // 1-px border flush with the window edge.
  QPen border_pen(PJ::theme::outline(theme::OutlineRole::Default, theme::OutlineState::Rest, t));
  border_pen.setWidthF(border_width);
  painter.setPen(border_pen);
  const qreal half_border = border_width / 2.0;
  painter.drawRect(QRectF(rect()).adjusted(half_border, half_border, -half_border, -half_border));
}

void MainWindow::onThemeChanged(const QString& theme) {
  // Connected with Qt::QueuedConnection (see ctor): this slot's heavy work —
  // applyIcons, the stylesheetChanged fan-out, and replotting every plot — must
  // NOT run synchronously inside Theme::setTheme, which is emitted from a
  // PreferencesDialog widget event. Running it reentrantly there segfaulted, so
  // it is deferred to the next event-loop turn.
  //
  // The QSS itself and the tooltip palette are applied by apply_theme_chrome
  // (the qssChanged handler), which fires synchronously inside setTheme — i.e.
  // BEFORE this queued slot. We deliberately do NOT re-apply them here: it would
  // duplicate that work and, worse, run last and overwrite any QSS that a later
  // qssChanged listener (e.g. DebugUi) layered on top of expandedQss().
  applyIcons(theme);
  emit stylesheetChanged(theme);
  forEachPlot([](PlotWidget* plot) { plot->replot(); });
}

void MainWindow::setIconSize(int size) {
  const int clamped = kIconSizeRange.clamp(size);
  if (clamped == chrome_metrics_.icon_size) {
    return;
  }
  chrome_metrics_.icon_size = clamped;
  emit chromeMetricsChanged(chrome_metrics_);
}

void MainWindow::setIconPadding(int padding) {
  const int clamped = kIconPaddingRange.clamp(padding);
  if (clamped == chrome_metrics_.icon_padding) {
    return;
  }
  chrome_metrics_.icon_padding = clamped;
  emit chromeMetricsChanged(chrome_metrics_);
}

void MainWindow::setLayoutPadding(int padding) {
  const int clamped = kLayoutPaddingRange.clamp(padding);
  if (clamped == chrome_metrics_.layout_padding) {
    return;
  }
  chrome_metrics_.layout_padding = clamped;
  emit chromeMetricsChanged(chrome_metrics_);
}

void MainWindow::setLayoutSpacing(int spacing) {
  const int clamped = kLayoutSpacingRange.clamp(spacing);
  if (clamped == chrome_metrics_.layout_spacing) {
    return;
  }
  chrome_metrics_.layout_spacing = clamped;
  emit chromeMetricsChanged(chrome_metrics_);
}

void MainWindow::persistChromeMetrics() const {
  // One QSettings instance ⇒ one INI rewrite for all four keys on sync().
  QSettings settings;
  settings.setValue(kIconSizeKey, chrome_metrics_.icon_size);
  settings.setValue(kIconPaddingKey, chrome_metrics_.icon_padding);
  settings.setValue(kLayoutPaddingKey, chrome_metrics_.layout_padding);
  settings.setValue(kLayoutSpacingKey, chrome_metrics_.layout_spacing);
}

QStringList MainWindow::customPluginFolders() const {
  return session_->extensionCatalog().customPluginFolders();
}

void MainWindow::setCustomPluginFolders(const QStringList& folders) {
  session_->extensionCatalog().setCustomPluginFolders(folders);
}

QStringList MainWindow::builtinPluginFolders() const {
  return session_->extensionCatalog().builtinPluginFolders();
}

QString MainWindow::registryUrlSetting() const {
  return QSettings().value(QLatin1String(kRegistryUrlSettingsKey)).toString();
}

void MainWindow::setRegistryUrlSetting(const QString& url) {
  {
    // Scoped so the write is flushed before effectiveRegistryUrl() reads it back
    // through its own QSettings below.
    QSettings settings;
    if (url.isEmpty()) {
      settings.remove(QLatin1String(kRegistryUrlSettingsKey));
    } else {
      settings.setValue(QLatin1String(kRegistryUrlSettingsKey), url);
    }
  }
#ifndef PJ_TARGET_WASM
  // A marketplace panel the user left open captured the URL when it was built
  // and would keep serving the previous registry until closed and reopened.
  // This method is the single owner of the settings key, so pushing the new
  // value from here reaches that panel whichever caller changed the setting.
  if (!marketplace_panel_.isNull()) {
    marketplace_panel_->setRegistryUrl(effectiveRegistryUrl());
  }
#endif
}

QString MainWindow::defaultRegistryUrl() {
  return QString::fromLatin1(kDefaultRegistryUrl);
}

bool MainWindow::isValidRegistryUrl(const QString& url) {
  const QUrl parsed(url);
  return parsed.isValid() &&
         (parsed.scheme() == u"http"_s || parsed.scheme() == u"https"_s || parsed.scheme() == u"file"_s);
}

QUrl MainWindow::effectiveRegistryUrl() const {
  const QString raw = registryUrlSetting();
  if (raw.isEmpty()) {
    return QUrl(defaultRegistryUrl());
  }
  if (!isValidRegistryUrl(raw)) {
    qCWarning(lcMain) << "Invalid" << kRegistryUrlSettingsKey << "in QSettings:" << raw << "— falling back to default.";
    return QUrl(defaultRegistryUrl());
  }
  return QUrl(raw);
}

void MainWindow::applyIcons(QString theme) {
  // Right-side buttons (Chart + Legend in the global column, Width and
  // Line-style in the local panel) are created programmatically by
  // buildGlobalToolbar() / buildLocalToolbar() and re-tinted by their
  // own stylesheetChanged hooks via each button's "iconPath" property.
  for (const PanelToggle& toggle : panelToggles(ui_)) {
    // The "iconPath" property carries whichever variant (on / off) the
    // toggle currently shows; falls back to the on variant on first
    // paint before the toggle handler has run.
    const QString icon_path = toggle.button->property("iconPath").toString();
    const QString resolved = icon_path.isEmpty() ? QString::fromLatin1(toggle.icon_path_on) : icon_path;
    toggle.button->setIcon(loadSvg(resolved, theme));
  }
  // Title-bar menus: their QActions persist across theme changes, so
  // re-tint here.
  ui_->actionExit->setIcon(QIcon(loadSvg(":/resources/svg/logout.svg", theme)));
  ui_->actionMarketplace->setIcon(QIcon(loadSvg(":/resources/svg/archive.svg", theme)));
  if (action_preferences_ != nullptr) {
    action_preferences_->setIcon(QIcon(loadSvg(":/resources/svg/settings_cog_light.svg", theme)));
  }
  if (action_load_layout_ != nullptr) {
    action_load_layout_->setIcon(QIcon(loadSvg(":/resources/svg/dashboard_load.svg", theme)));
  }
  if (action_save_layout_ != nullptr) {
    action_save_layout_->setIcon(QIcon(loadSvg(":/resources/svg/save_as.svg", theme)));
  }
#ifdef PJ_TARGET_WASM
  if (action_save_source_layout_ != nullptr) {
    action_save_source_layout_->setIcon(QIcon(loadSvg(":/resources/svg/save_as.svg", theme)));
  }
#endif
}

void MainWindow::onPlotTabAdded(PlotDocker* docker) {
  if (docker == nullptr) {
    return;
  }
  connect(docker, &PlotDocker::plotWidgetAdded, this, &MainWindow::onPlotAdded, Qt::UniqueConnection);
  // ADS focus is the single source of truth for "which widget is active":
  // it fires on canvas click and on tab-titlebar click. PlotDocker re-emits
  // it as dockFocused(DockWidget*) so MainWindow can route to the right
  // sidepanel page (plot config / 2D / 3D) without pulling in ADS types.
  connect(docker, &PlotDocker::dockFocused, this, &MainWindow::onDockFocused, Qt::UniqueConnection);
  // Placeholder 2D/3D icon click → build + adopt the empty scene dock here (the
  // shell owns the family→kind mapping). The first topic dropped into such an
  // empty dock seeds streaming playback, matching the placeholder→drop path.
  connect(docker, &PlotDocker::objectFamilyRequested, this, &MainWindow::onObjectFamilyRequested, Qt::UniqueConnection);
  connect(
      docker, &PlotDocker::firstObjectTopicAdded, this, &MainWindow::seedStreamingPlaybackFromDrop,
      Qt::UniqueConnection);
  connect(
      docker, &PlotDocker::placeholderTopicDropped, this, &MainWindow::onPlaceholderTopicDropped, Qt::UniqueConnection);
  for (int index = 0; index < docker->plotCount(); ++index) {
    if (DockWidget* dock = docker->plotAt(index)) {
      onPlotAdded(dock->plotWidget());
    }
  }
  bindEditorToPlot(firstPlotOfActiveTab());
}

void MainWindow::onPlotAdded(PlotWidget* plot) {
  if (plot == nullptr) {
    return;
  }
  connect(plot, &PlotWidget::rectChanged, this, &MainWindow::onPlotZoomChanged, Qt::UniqueConnection);
  connect(plot, &PlotWidget::trackerMoved, this, &MainWindow::onTrackerMovedFromWidget, Qt::UniqueConnection);
  // Dropping a scalar curve into a plot during streaming seeds playback, exactly
  // like dropping an object topic into a 2D/3D dock (PlotDocker::firstObjectTopicAdded).
  // Without this, a scalar-only stream never sets streaming_playback_seeded_, so the
  // live-ingest handler early-returns forever and the slider stays at the placeholder
  // range. seedStreamingPlaybackFromDrop is a no-op unless a stream is active and is
  // one-shot per session, so file-curve drops and later stream drops are harmless.
  connect(plot, &PlotWidget::curvesDropped, this, &MainWindow::seedStreamingPlaybackFromDrop, Qt::UniqueConnection);
  connect(plot, &PlotWidget::filterEditorRequested, this, &MainWindow::openFilterEditor, Qt::UniqueConnection);
  connect(
      plot, &PlotWidget::pendingCurveIntentsChanged, this, &MainWindow::schedulePendingDisplayBindingRebuild,
      Qt::UniqueConnection);
  connect(plot, &PlotWidget::statusMessageRequested, this, [this](const QString& message) {
    emitDiagnostic(DiagnosticLevel::kInfo, "Plot", "status", message);
  });
  // registerPlot is idempotent (layout load/undo re-runs onPlotAdded for live
  // plots) and owns the placeholderCurveDropped routing.
  topic_demand_controller_->registerPlot(plot);
  plot->setTrackerPosition(toAxisDouble(session_->playbackEngine().currentTime()));
  applyGlobalToggles(plot);
  if (curve_editor_ != nullptr && curve_editor_->plot() == nullptr) {
    bindEditorToPlot(plot);
  }
}

void MainWindow::openFilterEditor(std::vector<CurveDescriptor> sources, PlotWidget* origin) {
  if (origin == nullptr || session_ == nullptr) {
    return;
  }
  // Snapshot the source colours so the preview's before/after curves match the
  // post-Apply result (the filtered output inherits the source's colour).
  QHash<QString, QColor> source_colors;
  for (const auto& [key, color] : origin->curveColors()) {
    source_colors.insert(key, color);
  }

  auto* panel = new FilterEditorPanel(
      &session_->sessionManager(), &session_->catalogModel(), std::move(sources), std::move(source_colors));

  // Apply: replace each source curve with its filtered output, in place, on the
  // originating plot, then restore the chart area. Guard `origin` with QPointer in
  // case its dock was torn down while the panel was open.
  const QPointer<PlotWidget> origin_guard(origin);
  connect(
      panel, &FilterEditorPanel::applied, this,
      [this, origin_guard](const QList<QPair<QString, QString>>& replacements) {
        if (origin_guard != nullptr) {
          for (const auto& replacement : replacements) {
            origin_guard->replaceCurve(replacement.first, replacement.second);
          }
          origin_guard->replot();
          // Force a discrete undo entry: applying/removing a filter must never coalesce
          // into a preceding edit, so each filter op is its own undo step.
          onUndoableChange(/*force_new_state=*/true);
        }
        restoreCentralArea();
      });
  connect(panel, &FilterEditorPanel::closed, this, [this]() { restoreCentralArea(); });
  connect(panel, &FilterEditorPanel::diagnostic, this, [this](const QString& message) {
    emitDiagnostic(DiagnosticLevel::kWarning, "Filter", "apply", message);
  });

  if (!presentPanel(panel)) {
    // Another chart-area panel (e.g. a toolbox) is already open.
    emitDiagnostic(DiagnosticLevel::kWarning, "Filter", "panel", tr("Close the open panel before applying a filter"));
    panel->deleteLater();
    return;
  }
  filter_editor_origin_ = origin;  // the preview mirrors this plot's grid/style/width
  syncFilterEditorPreviewDisplay();
}

void MainWindow::syncFilterEditorPreviewDisplay() {
  auto* panel = qobject_cast<FilterEditorPanel*>(current_panel_);
  if (panel == nullptr || filter_editor_origin_ == nullptr) {
    return;
  }
  // Mirror the plot the editor was opened on. Grid is global (applied to every plot),
  // so activate_grid_ already equals the origin's grid state.
  panel->setPreviewDisplay(
      activate_grid_, filter_editor_origin_->defaultCurveStyle(), filter_editor_origin_->lineWidth());
}

void MainWindow::syncPanelPreviewDisplay() {
  // A plugin toolbox panel (e.g. the Transform Editor plugin) embeds a real
  // PlotWidget inside its chart QFrame. It has no originating plot to mirror, so its
  // preview keeps the PlotWidget default curve style/width; only the global grid
  // toggle is pushed. Reach the plot generically as a child of the presented panel
  // — and of every pinned toolbox tab, whose previews track the grid the same way.
  const auto apply_to_panel = [this](QWidget* panel_root) {
    for (auto* plot : panel_root->findChildren<PlotWidget*>()) {
      // Stash the grid state on the chart frame so the dialog-host binding can re-apply
      // it on every preview rebuild (source/function changes recreate the curves).
      if (QWidget* frame = plot->parentWidget()) {
        frame->setProperty("_pj_view_set", true);
        frame->setProperty("_pj_view_grid", activate_grid_);
      }
      // Apply immediately too, so a grid toggle updates the preview without waiting for
      // the next chart tick.
      plot->setGridVisible(activate_grid_);
      plot->replot();
    }
  };
  if (current_panel_ != nullptr) {
    apply_to_panel(current_panel_);
  }
  for (const PinnedToolbox& toolbox : pinned_toolboxes_) {
    if (!toolbox.container.isNull()) {
      apply_to_panel(toolbox.container);
    }
  }
}

namespace {
// SVG path for the legend button when the legend is at the given corner.
// kHidden is never a valid argument — callers must remap to the saved
// corner (previous_legend_corner_) before looking up the icon.
[[nodiscard]] QString legendCornerIcon(LegendStatus corner) {
  switch (corner) {
    case LegendStatus::kBottomRight:
      return u":/resources/svg/position_bottom_right.svg"_s;
    case LegendStatus::kBottomLeft:
      return u":/resources/svg/position_bottom_left.svg"_s;
    case LegendStatus::kTopRight:
      return u":/resources/svg/position_top_right.svg"_s;
    case LegendStatus::kTopLeft:
      return u":/resources/svg/position_top_left.svg"_s;
    case LegendStatus::kHidden:
      return u":/resources/svg/position_top_right.svg"_s;
  }
  return u":/resources/svg/position_top_right.svg"_s;
}

// Four-corner forward cycle. Used while the legend is visible to walk
// TR → TL → BL → BR; the BR-to-hidden wrap and the hidden-to-visible
// restore are handled by the click handler so it can also reset the
// saved corner to TR at the cycle boundary.
[[nodiscard]] LegendStatus nextLegendCorner(LegendStatus current) {
  switch (current) {
    case LegendStatus::kTopRight:
      return LegendStatus::kTopLeft;
    case LegendStatus::kTopLeft:
      return LegendStatus::kBottomLeft;
    case LegendStatus::kBottomLeft:
      return LegendStatus::kBottomRight;
    case LegendStatus::kBottomRight:
      return LegendStatus::kTopRight;
    case LegendStatus::kHidden:
      return LegendStatus::kTopRight;
  }
  return LegendStatus::kTopRight;
}
}  // namespace

void MainWindow::setLegendStatus(LegendStatus position) {
  legend_status_ = position;
  // Track the "current position" used by the icon while hidden, so
  // right-click → show restores the user's last corner.
  if (position != LegendStatus::kHidden) {
    previous_legend_corner_ = position;
  }
  QSettings().setValue(u"MainWindow.legendStatus"_s, static_cast<int>(legend_status_));
  if (button_legend_ != nullptr) {
    const bool visible = (position != LegendStatus::kHidden);
    button_legend_->setChecked(visible);
    button_legend_->setIconPath(legendCornerIcon(visible ? position : previous_legend_corner_));
  }
  forEachPlot([this](PlotWidget* plot) { applyLegendStatus(plot); });
}

void MainWindow::applyLegendStatus(PlotWidget* plot) {
  if (plot == nullptr) {
    return;
  }
  const bool visible = legend_status_ != LegendStatus::kHidden;
  plot->setLegendVisible(visible);
  if (!visible) {
    return;
  }
  Qt::Alignment alignment;
  switch (legend_status_) {
    case LegendStatus::kBottomRight:
      alignment = Qt::AlignBottom | Qt::AlignRight;
      break;
    case LegendStatus::kBottomLeft:
      alignment = Qt::AlignBottom | Qt::AlignLeft;
      break;
    case LegendStatus::kTopRight:
      alignment = Qt::AlignTop | Qt::AlignRight;
      break;
    case LegendStatus::kTopLeft:
      alignment = Qt::AlignTop | Qt::AlignLeft;
      break;
    case LegendStatus::kHidden:
      return;
  }
  plot->setLegendAlignment(alignment);
}

void MainWindow::onLegendButtonClicked() {
  // Three branches for the cycle:
  //   * Hidden: re-show at previous_legend_corner_. This is TR right
  //     after a BR-to-hidden wrap, or the last visible corner after
  //     a right-click hide.
  //   * BR (cycle end): reset previous_legend_corner_ to TR so the
  //     unchecked button paints the TR icon, then hide. The next
  //     left-click takes the Hidden branch and re-enters at TR.
  //   * Any other visible corner: advance to the next corner.
  if (legend_status_ == LegendStatus::kHidden) {
    setLegendStatus(previous_legend_corner_);
  } else if (legend_status_ == LegendStatus::kBottomRight) {
    previous_legend_corner_ = LegendStatus::kTopRight;
    setLegendStatus(LegendStatus::kHidden);
  } else {
    setLegendStatus(nextLegendCorner(legend_status_));
  }
}

void MainWindow::applyGlobalToggles(PlotWidget* plot) {
  if (plot == nullptr) {
    return;
  }
  plot->setShowPoints(show_points_);
  plot->setGridVisible(activate_grid_);
  applyDots(plot);
  plot->setReferenceLine(referenceDisplaySeconds());
  plot->setKeepRatioXY(keep_ratio_);
  applyLegendStatus(plot);
  plot->setTrackerParameter(tracker_info_);
}

void MainWindow::applyShowPointsToDock(DockWidget* dock) {
#ifdef PJ_WITH_SCENE2D
  if (dock == nullptr || dock->objectWidget() == nullptr) {
    return;
  }
  auto* media = qobject_cast<Scene2DDockWidget*>(dock->objectWidget()->widget());
  if (media != nullptr) {
    media->setPointInspectorEnabled(show_points_);
  }
#else
  Q_UNUSED(dock);
#endif
}

void MainWindow::applyShowPointsTo2DWidgets() {
  forEachDock([this](DockWidget* dock) { applyShowPointsToDock(dock); });
}

void MainWindow::updateTimeTrackerIcon() {
  if (button_time_tracker_ == nullptr) {
    return;
  }
  // setIconPath (not setIcon): the SvgButton re-tints the glyph on theme
  // switches, so the state swap keeps working in both themes.
  switch (tracker_info_) {
    case CurveTracker::kLineOnly:
      button_time_tracker_->setIconPath(u":/resources/svg/line_tracker.svg"_s);
      break;
    case CurveTracker::kValue:
      button_time_tracker_->setIconPath(u":/resources/svg/line_tracker_1.svg"_s);
      break;
    case CurveTracker::kValueName:
      button_time_tracker_->setIconPath(u":/resources/svg/line_tracker_a.svg"_s);
      break;
  }
}

void MainWindow::onTimeTrackerButtonClicked() {
  switch (tracker_info_) {
    case CurveTracker::kLineOnly:
      tracker_info_ = CurveTracker::kValue;
      break;
    case CurveTracker::kValue:
      tracker_info_ = CurveTracker::kValueName;
      break;
    case CurveTracker::kValueName:
      tracker_info_ = CurveTracker::kLineOnly;
      break;
  }
  QSettings().setValue(u"MainWindow.timeTrackerSetting"_s, static_cast<int>(tracker_info_));
  updateTimeTrackerIcon();
  forEachPlot([this](PlotWidget* plot) {
    plot->setTrackerParameter(tracker_info_);
    plot->replot();
  });
}

void MainWindow::applyDots(PlotWidget* plot) {
  if (plot == nullptr) {
    return;
  }
  // The Dots toggle flips the plot-level style between Lines and Lines+Dots,
  // leaving other styles (Dots, Sticks, Steps) chosen in the Curve Style panel
  // alone. setDefaultStyle restyles every curve and is inherited by new ones.
  const auto from = dots_ ? PlotWidgetBase::kLines : PlotWidgetBase::kLinesAndDots;
  const auto to = dots_ ? PlotWidgetBase::kLinesAndDots : PlotWidgetBase::kLines;
  if (plot->defaultCurveStyle() == from) {
    plot->setDefaultStyle(to);
  }
}

void MainWindow::onPlotZoomChanged(PlotWidget* modified, QRectF rect) {
  if (!button_link_->isChecked()) {
    return;
  }
  if (modified == nullptr || modified->isEmpty() || modified->isXYPlot() || !modified->isZoomLinkEnabled()) {
    return;
  }

  forEachPlot([modified, rect](PlotWidget* plot) {
    if (plot == modified || plot->isEmpty() || plot->isXYPlot() || !plot->isZoomLinkEnabled()) {
      return;
    }
    // Linked zoom only ever aligns the horizontal (time) range: copy the source's
    // X extent onto each peer while leaving its vertical range exactly as the user
    // left it. A purely-vertical gesture (wheel on the left axis, "Zoom Out
    // Vertically") therefore carries an unchanged X and resolves to a no-op here.
    QRectF peer_rect = plot->currentBoundingRect();
    peer_rect.setLeft(rect.left());
    peer_rect.setRight(rect.right());
    plot->setZoomRectangle(peer_rect, false);
    plot->replot();
  });
  // The State Transitions strips share the plots' time axis (setVisibleRange
  // never echoes back, so no feedback loop).
  forEachStateStrip(
      [rect](StateTransitionsDockWidget* strip) { strip->view()->setVisibleRange(rect.left(), rect.right()); });
}

void MainWindow::onStateTransitionsRangeChanged(StateTransitionsDockWidget* source, double t_min, double t_max) {
  if (!button_link_->isChecked() || !(t_max > t_min)) {
    return;
  }
  forEachPlot([t_min, t_max](PlotWidget* plot) {
    if (plot->isEmpty() || plot->isXYPlot() || !plot->isZoomLinkEnabled()) {
      return;
    }
    QRectF peer_rect = plot->currentBoundingRect();
    peer_rect.setLeft(t_min);
    peer_rect.setRight(t_max);
    plot->setZoomRectangle(peer_rect, false);
    plot->replot();
  });
  forEachStateStrip([source, t_min, t_max](StateTransitionsDockWidget* strip) {
    if (strip != source) {
      strip->view()->setVisibleRange(t_min, t_max);
    }
  });
}

void MainWindow::onTrackerMovedFromWidget(QPointF point) {
  session_->playbackEngine().setCurrentTime(displaySeconds(point.x()));
}

DatasetId MainWindow::representativeDatasetId() const {
  if (active_streaming_dataset_id_ != 0) {
    return active_streaming_dataset_id_;
  }
  if (const auto datasets = session_->sessionManager().dataEngine().listDatasets(); !datasets.empty()) {
    return datasets.front();
  }
  return 0;
}

std::optional<double> MainWindow::referenceDisplaySeconds() const {
  if (!reference_instant_.has_value()) {
    return std::nullopt;
  }
  // Project the frame-invariant instant into the CURRENT frame, exactly as a
  // curve adapter projects its raw samples — line and data share one offset, so
  // the line can never drift off the re-fitted axis.
  const DisplayOffset offset = session_->sessionManager().displayOffset(representativeDatasetId());
  return toAxisDouble(toDisplaySeconds(*reference_instant_, offset));
}

void MainWindow::broadcastReferenceLine() {
  const std::optional<double> ref_sec = referenceDisplaySeconds();
  forEachPlot([ref_sec](PlotWidget* plot) { plot->setReferenceLine(ref_sec); });
  if (source_timeline_controller_ != nullptr) {
    source_timeline_controller_->setReferenceLine(ref_sec);
  }
}

void MainWindow::onUseTimeOffsetToggled(bool checked) {
  // The synchronous global displayOffsetChanged handler owns range/playhead
  // translation for both explicit toggles and automatic origin rebases.
  session_->sessionManager().setUseTimeOffset(checked);
}

void MainWindow::seedStreamingPlaybackFromDrop() {
  if (active_streaming_dataset_id_ == 0) {
    return;
  }
  // One-shot per streaming session: a drop may seed the range before the next
  // ingest tick, but later drops must not re-snap a paused scrub. Playback state
  // decides whether this also moves the cursor to the live edge.
  if (streaming_playback_seeded_) {
    return;
  }
  streaming_playback_seeded_ = true;
  if (const auto range = session_->sessionManager().datasetDisplayRange(active_streaming_dataset_id_);
      range.has_value()) {
    auto& engine = session_->playbackEngine();
    if (engine.isPlaying()) {
      engine.setRangeAndCurrentTime(*range, range->max);
    } else {
      engine.setRange(*range);
    }
  }
}

void MainWindow::setSourceTimelineStreamingLock(bool locked) {
  if (source_timeline_ != nullptr) {
    source_timeline_->setInteractionLocked(locked);
  }
  // The align/snap/reset rail drives the controller's align slots directly (not
  // through the widget), so the widget lock alone wouldn't stop a rail click —
  // grey the whole rail too. timelineAlignRail is a .ui container member.
  if (ui_->timelineAlignRail != nullptr) {
    ui_->timelineAlignRail->setEnabled(!locked);
  }
}

DiagnosticSink MainWindow::diagnosticSink() const {
  return diagnostic_bridge_->sink();
}

void MainWindow::emitDiagnostic(DiagnosticLevel level, const char* source, const char* id, const QString& message) {
  Diagnostic d;
  d.level = level;
  d.source = (source != nullptr) ? source : "";
  d.id = (id != nullptr) ? id : "";
  d.message = message.toStdString();
  diagnostic_bridge_->sink()(d);
}

void MainWindow::refreshStreamingCombo() {
  QStringList names;
  for (const auto* ds : session_->extensionCatalog().streamSources()) {
    names << QString::fromStdString(ds->name);
  }
  ui_->leftPanel->setStreamingSources(names);
}

void MainWindow::onRecordToggled(bool record) {
  if (!record) {
    recording_service_->stop();
    return;
  }
  auto recordable = streaming_manager_->recordingTargets();
  if (recordable.targets.empty()) {
    ui_->leftPanel->setRecordingActive(false);
    showToast(
        recordable.excluded.isEmpty()
            ? tr("Nothing to record: start a streaming source first.")
            : tr("Nothing to record: %n active source(s) write decoded samples directly, which cannot be recorded.",
                 nullptr, static_cast<int>(recordable.excluded.size())));
    return;
  }
  // The app version goes into each file's pj.recording metadata. Take it from
  // the QCoreApplication property main.cpp seeded from pj_version.h, so
  // versions.env stays the single source of truth.
  const auto directory =
      recording_service_->start(std::move(recordable.targets), QCoreApplication::applicationVersion().toStdString());
  if (!directory) {
    ui_->leftPanel->setRecordingActive(false);
    showToast(tr("Recording could not start: %1").arg(QString::fromStdString(directory.error())));
    return;
  }
  if (!recordable.excluded.isEmpty()) {
    // Say it up front: finding out on replay that the file is missing these
    // sources is far worse than a toast now.
    showToast(tr("%n source(s) write decoded samples directly and are not recorded: %1", nullptr,
                 static_cast<int>(recordable.excluded.size()))
                  .arg(recordable.excluded.join(u", "_s)));
  }
}

void MainWindow::onRecordingStarted(
    const QString& capture_id, const QString& directory, const QVector<SourceRecordingStart>& sources) {
  recording_elapsed_.start();
  ui_->leftPanel->setRecordingActive(true);
  ui_->leftPanel->setRecordingStatusText(tr("0:00 | %1").arg(QLocale().formattedDataSize(0)));
  qCInfo(lcMain) << "recording capture" << capture_id << "into" << directory;
  for (const SourceRecordingStart& source : sources) {
    qCInfo(lcMain) << "  recording" << source.source << "to" << source.path;
  }
}

void MainWindow::onRecordingStopped(const CaptureResult& capture) {
  ui_->leftPanel->setRecordingActive(false);
  // Per-source detail belongs in the log: a toast that listed one line per file
  // would be unreadable the moment a session has more than a couple of sources.
  for (const SourceRecordingResult& source : capture.sources) {
    if (source.outcome == RecordingOutcome::kClean) {
      qCInfo(lcMain) << "recorded" << source.source << "to" << source.path << source.messages << "messages";
    } else {
      qCWarning(lcMain) << "recorded" << source.source << "to" << source.path
                        << "with problems:" << outcomeName(source.outcome) << source.reason;
    }
  }

  const int problem_count = capture.lossy_count + capture.truncated_count + capture.incomplete_count;
  QString message;
  if (problem_count == 0) {
    message = capture.sources.size() == 1
                  ? tr("Recording saved to %1").arg(capture.sources.front().path)
                  : tr("%n recording(s) saved to %1", nullptr, static_cast<int>(capture.sources.size()))
                        .arg(capture.directory);
  } else {
    // At most two names: enough to know which source to look at, short enough
    // to stay a toast. The log carries the rest.
    QStringList failing;
    for (const SourceRecordingResult& source : capture.sources) {
      if (source.outcome == RecordingOutcome::kClean || failing.size() >= 2) {
        continue;
      }
      failing << tr("%1 (%2)").arg(source.source, source.reason);
    }
    message = tr("%n of %1 recording(s) had problems: %2", nullptr, problem_count)
                  .arg(capture.sources.size())
                  .arg(failing.join(u", "_s));
  }
  if (capture.incomplete_count > 0) {
    message += tr(" An unfinalized file still opens; run 'mcap recover' to rebuild its index.");
  }
  if (capture.total_dropped > 0) {
    // A complete file with holes says nothing about them itself, so say it here.
    const auto capped = static_cast<int>(std::min<quint64>(capture.total_dropped, std::numeric_limits<int>::max()));
    message += tr(" (%n message(s) dropped: the disk could not keep up)", nullptr, capped);
  }
  if (capture.total_skipped > 0) {
    const auto capped = static_cast<int>(std::min<quint64>(capture.total_skipped, std::numeric_limits<int>::max()));
    message += tr(" (%n message(s) could not be recorded)", nullptr, capped);
  }
  showToast(message);
}

void MainWindow::onRecordingProgress(quint64 /*messages*/, quint64 payload_bytes, quint64 dropped) {
  const qint64 seconds = recording_elapsed_.elapsed() / 1000;
  QString text = tr("%1:%2 | %3")
                     .arg(seconds / 60)
                     .arg(seconds % 60, 2, 10, QLatin1Char('0'))
                     .arg(QLocale().formattedDataSize(static_cast<qint64>(payload_bytes)));
  // The sink is not keeping up and the budget is shedding the biggest messages.
  // The recording carries on with holes, so show the count while it grows.
  if (dropped > 0) {
    text += tr(" | %1 dropped").arg(dropped);
  }
  ui_->leftPanel->setRecordingStatusText(text);
}

void MainWindow::wireExistingPlots() {
  forEachDocker([this](PlotDocker* docker) { onPlotTabAdded(docker); });
}

void MainWindow::forEachDocker(const std::function<void(PlotDocker*)>& operation) {
  // Hoisted: dockerCount()/dockerAt() are linear scans since widget tabs
  // joined the tab vector; re-evaluating the count per iteration would make
  // this loop quadratic.
  const int docker_count = ui_->tabbedPlotWidget->dockerCount();
  for (int index = 0; index < docker_count; ++index) {
    if (PlotDocker* docker = ui_->tabbedPlotWidget->dockerAt(index)) {
      operation(docker);
    }
  }
}

void MainWindow::forEachStateStrip(const std::function<void(StateTransitionsDockWidget*)>& operation) {
  forEachDock([&operation](DockWidget* dock) {
    if (dock->objectWidget() == nullptr) {
      return;
    }
    if (auto* strip = qobject_cast<StateTransitionsDockWidget*>(dock->objectWidget()->widget())) {
      operation(strip);
    }
  });
}

void MainWindow::forEachDock(const std::function<void(DockWidget*)>& operation) {
  forEachDocker([&operation](PlotDocker* docker) {
    for (int index = 0; index < docker->plotCount(); ++index) {
      DockWidget* dock = docker->plotAt(index);
      if (dock != nullptr) {
        operation(dock);
      }
    }
  });
}

void MainWindow::forEachVisibleDock(const std::function<void(DockWidget*)>& operation) {
  // Only the active top-level tab's docks are on screen (the other tabs live in a
  // hidden QStackedWidget page). A dock hidden by a maximized sibling still lives
  // in the active docker, so it IS visited — the conservative gate is per-tab.
  //
  // Limitation (pre-existing, NOT introduced here): plotAt() returns each area's
  // currentDockWidget(), so a dock tabbed BEHIND another inside the same area is
  // not visited per-tick — and its sub-tab switch emits no currentTabChanged, so
  // the catch-up does not re-seed it. origin/main's forEachDock had the same blind
  // spot. Normal PJ4 docking never creates inner tabs (splits insert into
  // OuterDockAreas, which exclude the center/tabify area), so this is unreachable
  // via the UI; if inner tabbing is ever enabled, re-seed from CDockAreaWidget::currentChanged.
  PlotDocker* active = ui_->tabbedPlotWidget->currentTab();
  if (active == nullptr) {
    return;
  }
  for (int index = 0; index < active->plotCount(); ++index) {
    if (DockWidget* dock = active->plotAt(index); dock != nullptr) {
      operation(dock);
    }
  }
}

void MainWindow::broadcastTrackerTime(double display_seconds) {
  forEachDock([display_seconds](DockWidget* dock) { dock->onTrackerTime(display_seconds); });
  ui_->curveListPanel->refreshValues(display_seconds);
}

void MainWindow::broadcastTrackerTimeToVisible(double display_seconds) {
  // Per-tick playback/scrub fan-out: only the active top-level tab is on screen, so
  // forEachVisibleDock skips every other tab's docks — a hidden-tab plot's replot
  // still does the full canvas work (and, on an RHI window, the per-frame composite)
  // for nothing. This is the temporal cap's spatial sibling: cap-the-rate (broadcast
  // throttle) x skip-the-invisible (here). Structural seeds keep using
  // broadcastTrackerTime() (ALL tabs) so a hidden tab is correct the instant it is
  // revealed, and the currentTabChanged handler re-seeds the newly active tab.
  forEachVisibleDock([display_seconds](DockWidget* dock) { dock->onTrackerTime(display_seconds); });
  ui_->curveListPanel->refreshValues(display_seconds);
}

void MainWindow::forEachSceneDock(const std::function<void(SceneDockWidget*)>& operation) {
  forEachDock([&operation](DockWidget* dock) {
    if (dock->objectWidget() == nullptr) {
      return;
    }
    QWidget* object_widget = dock->objectWidget()->widget();
    if (auto* scene_dock = qobject_cast<SceneDockWidget*>(object_widget); scene_dock != nullptr) {
      operation(scene_dock);
    }
  });
}

void MainWindow::forEachPlot(const std::function<void(PlotWidget*)>& operation) {
  forEachDock([&operation](DockWidget* dock) {
    if (PlotWidget* plot = dock->plotWidget()) {
      operation(plot);
    }
  });
}

void MainWindow::syncWidgetsToCatalog() {
  // Each widget prunes its OWN now-invalid pieces against the live catalog/store:
  //  - plots drop curves whose source key is gone (empty plot stays, reusable);
  //  - object viewers drop layers whose topic was evicted, reporting empty so the
  //    shell resets that dock to the reusable placeholder.
  forEachPlot([](PlotWidget* plot) { plot->revalidate(); });
  forEachDock([](DockWidget* dock) {
    auto* viewer = dynamic_cast<IObjectViewer*>(dock->objectWidget());
    if (viewer != nullptr && !viewer->revalidateObjects()) {
      dock->clearToPlaceholder();
    }
  });
  // Clearing a dock to its placeholder doesn't change ADS focus, so the right
  // panel won't refresh on its own. Re-evaluate it against the active tab's
  // focused dock: if that dock was just emptied, the panel falls back to the
  // empty page; otherwise this is a no-op. Keying off the active tab (rather
  // than a remembered pointer) avoids ever painting a hidden tab's dock.
  onDockFocused(activeFocusedDock());
}

void MainWindow::linkedZoomOut() {
  if (!button_link_->isChecked()) {
    forEachPlot([](PlotWidget* plot) { plot->zoomOut(false); });
    return;
  }
  forEachDocker([](PlotDocker* docker) {
    auto plot_at = [docker](int index) -> PlotWidget* {
      DockWidget* dock = docker->plotAt(index);
      PlotWidget* plot = dock != nullptr ? dock->plotWidget() : nullptr;
      return (plot != nullptr && !plot->isEmpty()) ? plot : nullptr;
    };

    auto strip_at = [docker](int index) -> StateTransitionsDockWidget* {
      DockWidget* dock = docker->plotAt(index);
      if (dock == nullptr || dock->objectWidget() == nullptr) {
        return nullptr;
      }
      return qobject_cast<StateTransitionsDockWidget*>(dock->objectWidget()->widget());
    };

    // Collect the X union across BOTH widget kinds first (State Transitions
    // strips contribute their data extent, so a strip-only tab still zooms out
    // to its own content), then apply it — the union must be complete before
    // any widget moves.
    std::optional<Range<double>> x_union;
    const auto fold = [&x_union](double min, double max) {
      if (!x_union) {
        x_union = Range<double>{min, max};
      } else {
        x_union->min = std::min(x_union->min, min);
        x_union->max = std::max(x_union->max, max);
      }
    };
    for (int index = 0; index < docker->plotCount(); ++index) {
      if (PlotWidget* plot = plot_at(index); plot != nullptr && !plot->isXYPlot()) {
        const QRectF rect = plot->maxZoomRect();
        fold(rect.left(), rect.right());
      }
      if (StateTransitionsDockWidget* strip = strip_at(index)) {
        if (const auto extent = strip->view()->dataExtentSeconds()) {
          fold(extent->first, extent->second);
        }
      }
    }

    for (int index = 0; index < docker->plotCount(); ++index) {
      if (PlotWidget* plot = plot_at(index)) {
        if (plot->isXYPlot() || !x_union) {
          plot->zoomOut(false);
        } else {
          QRectF rect = plot->maxZoomRect();
          rect.setLeft(x_union->min);
          rect.setRight(x_union->max);
          plot->setZoomRectangle(rect, false);
          plot->replot();
        }
      }
      StateTransitionsDockWidget* strip = strip_at(index);
      if (strip != nullptr && x_union) {
        strip->view()->setVisibleRange(x_union->min, x_union->max);
      }
    }
  });
}

void MainWindow::closeEvent(QCloseEvent* event) {
  // Any in-flight restore (progressive transaction + layout-import batch) is
  // torn down FIRST — the batch hard-shut (provider jobs cancelled+joined,
  // its FileLoader tickets discarded) so no provider worker can call back
  // into the loader/session while they tear down.
  supersedeActiveRestore();
  // Stop and join any in-flight worker load (discard) and drain the queue BEFORE
  // the session/datastore tear down, so a worker can't write into a freed engine
  // or fire a queued completion at a half-destroyed window.
  if (file_loader_ != nullptr) {
    file_loader_->joinForShutdown();
  }
  // Courtesy stop for in-flight toolbox bulk imports: the panel teardown in
  // ~MainWindow joins their workers synchronously, so signalling the stop here
  // lets the download wind down during the remaining shutdown instead of the
  // join blocking on a full fetch.
  stopAllToolboxImports();
  // Finalize before anything tears the streaming sources down: stop() detaches
  // the taps from live runtime hosts, and a normal quit would otherwise leave
  // the recording without its summary section.
  if (recording_service_ != nullptr) {
    recording_service_->stop();
  }
#ifdef PJ_TARGET_WASM
  // Closing the last Qt window in a browser hides the canvas, but Emscripten may
  // keep the live runtime instead of unwinding main() immediately. Do not rely
  // on MainWindow's stack destructor to stop worker threads in that case: the
  // user-facing File -> Quit action is the terminal boundary, so cooperatively
  // stop and join every live source before accepting it. Desktop keeps its
  // existing destructor-driven shutdown behavior unchanged.
  if (streaming_manager_ != nullptr && streaming_manager_->hasActiveSession()) {
    streaming_manager_->stopAllAndWait(tr("application shutdown"));
    active_streaming_dataset_id_ = 0;
    streaming_playback_seeded_ = false;
  }
#endif
  QSettings settings;
  settings.setValue(u"MainWindow.buttonLink"_s, button_link_->isChecked());
  // Remember the left-panel width (the whole splitter layout) so the next launch
  // restores it instead of falling back to the narrow .ui default.
  settings.setValue(u"MainWindow.mainSplitterState"_s, ui_->mainSplitter->saveState());
  QMainWindow::closeEvent(event);
}

void MainWindow::showEvent(QShowEvent* event) {
  QMainWindow::showEvent(event);

#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  if (!property("pjWasmReadyReported").toBool()) {
    setProperty("pjWasmReadyReported", true);
    // Report readiness only after the real top-level has entered showEvent.
    // Browser acceptance can now wait for an application-owned boundary
    // instead of sleeping for an arbitrary five seconds after catalog setup.
    QTimer::singleShot(0, this, []() { qInfo("PJ_WASM_APP_READY"); });
  }
#endif

  // (1) Restore the remembered left-panel width once, after the splitter has real
  // geometry. Done here rather than in the constructor so the saved sizes aren't
  // overwritten by the first layout pass; an explicit --layout load runs later
  // and still wins (it re-applies splitter sizes through restoreChromeState).
  if (!left_splitter_restored_) {
    left_splitter_restored_ = true;
    const QByteArray state = QSettings().value(u"MainWindow.mainSplitterState"_s).toByteArray();
    if (state.isEmpty() || !ui_->mainSplitter->restoreState(state)) {
      // First-ever launch (nothing remembered) — or a stale/mismatched saved blob
      // that restoreState rejected: open the left panel at a comfortable default
      // rather than letting the splitter collapse it toward its 280 px minimum.
      // Clamped by the leftColumn's 280..600 px size constraints.
      constexpr int kDefaultLeftPanelWidth = 400;
      const int total = ui_->mainSplitter->width();
      if (total > kDefaultLeftPanelWidth) {
        ui_->mainSplitter->setSizes({kDefaultLeftPanelWidth, total - kDefaultLeftPanelWidth});
      }
    }
  }

  // (2) Size the bottom (timeline) panel definitively on first show. Deferred to
  // after the first layout pass: only once the splitter has real pixel geometry
  // does an absolute setSizes() stick — doing it in the constructor leaves a ratio
  // that resolves to a squashed sliver. Closed → playback bar only; open → a
  // definite strip height (last expanded height, floored at the open minimum).
  if (!bottom_panel_sized_) {
    bottom_panel_sized_ = true;
    QTimer::singleShot(0, this, [this]() {
      applyBottomPanelConstraints();
      if (ui_->timelineStrip == nullptr || ui_->timelineStrip->isHidden()) {
        return;  // closed: applyBottomPanelConstraints already pinned it to the playback bar
      }
      const QList<int> sizes = ui_->timelineSplitter->sizes();
      if (sizes.size() != 2) {
        return;
      }
      const int total = sizes[0] + sizes[1];
      const int floor = ui_->bottomPanel->minimumHeight();  // playback + kMinTimelineStripHeight
      const int expanded = std::max(QSettings().value(kPanelBottomExpandedKey, floor).toInt(), floor);
      ui_->timelineSplitter->setSizes({total - expanded, expanded});
      // The playback bar is laid out now, so the slider's start x is real — line the
      // name column separator up under it.
      alignNameColumnToPlayback();
    });
  }
}

void MainWindow::onLoadLayout() {
#ifdef PJ_TARGET_WASM
  if (layout_selection_pending_) {
    return;
  }
  layout_selection_pending_ = true;
  updateBrowserLayoutActions();
  qInfo("PJ_WASM_LAYOUT_PICKER_OPEN");
  QPointer<MainWindow> self(this);
  FileSelectionService::selectFileContent(
      this, tr(kLayoutFilter), [self](FileSelectionService::Selection selection) mutable {
        if (self.isNull()) {
          return;
        }
        if (selection.browser_name.isEmpty()) {
          self->layout_selection_pending_ = false;
          self->updateBrowserLayoutActions();
          qInfo("PJ_WASM_LAYOUT_PICKER_CANCELLED");
          return;
        }
        self->loadLayoutFromBytes(selection.bytes, selection.browser_name);
      });
  return;
#else
  // Remember the last directory across sessions (matches PJ3 behaviour
  // and the data-file loader's pattern at FileLoader.cpp:82). Default
  // to the working directory on first run rather than ~/Documents,
  // since layout files commonly live in project directories.
  const QString start_dir = QSettings().value(kLastLayoutDirKey, QDir::currentPath()).toString();
  // Passing `this` as the metrics source primes the dialog with the
  // current icon size and keeps it in step if chromeMetricsChanged fires.
  const QString path = FileDialog::getOpenFileName(this, tr("Load Layout"), start_dir, tr(kLayoutFilter), this);
  if (path.isEmpty()) {
    return;
  }
  QSettings().setValue(kLastLayoutDirKey, QFileInfo(path).absolutePath());
  loadLayoutFromPath(path, LayoutLoadInteractivity::kInteractive);
#endif
}

void MainWindow::onSaveLayout() {
#ifdef PJ_TARGET_WASM
  const QString file_name = u"plotjuggler-layout.pj4.xml"_s;
  const QByteArray bytes = browserGenericLayoutDocument().toByteArray(2);
  if (layout_xml::containsEphemeralBrowserPath(bytes)) {
    const QString error = tr("The layout still contains an ephemeral browser path and was not downloaded.");
    showBrowserLayoutWarning(this, tr("Save Layout"), error);
    emitDiagnostic(DiagnosticLevel::kWarning, "Layout", "ephemeral-browser-path", error);
    qWarning("PJ_WASM_LAYOUT_DOWNLOAD_FAILED reason=%s", qPrintable(error));
    return;
  }
  FileSelectionService::saveFileContent(this, bytes, file_name);
  emitDiagnostic(
      DiagnosticLevel::kInfo, "Layout", "download-requested", tr("Requested layout download: %1").arg(file_name));
  qInfo(
      "PJ_WASM_LAYOUT_DOWNLOAD_REQUESTED name=%s bytes=%lld", qPrintable(file_name),
      static_cast<long long>(bytes.size()));
  return;
#else
  const QString start_dir = QSettings().value(kLastLayoutDirKey, QDir::currentPath()).toString();
  // setDefaultSuffix (passed through PJ::FileDialog) wants the extension
  // without the leading dot.
  const QString default_suffix = QString::fromLatin1(layout_xml::kLayoutExtension).mid(1);

  // Checked = source-bound: embed the data-source reference so opening the
  // layout reloads this exact file. Unchecked = generic: the layout carries
  // no file reference and binds its curves (by topic+field) to whatever
  // dataset is loaded when it's opened — for reuse across similar recordings.
  // Default ON matches PJ3 and the common "save, reload later" workflow.
  const FileDialog::ExtraOption save_source_opt{tr("Bind to this data source"), /*default_checked=*/true};
  const auto result = FileDialog::getSaveFileNameWithOptions(
      this, tr("Save Layout"), start_dir, tr(kLayoutFilter), default_suffix, {save_source_opt}, this);

  if (result.path.isEmpty()) {
    return;
  }
  // Backstops the dialog's defaultSuffix: a bare typed name (no extension)
  // becomes a .pj4.xml file even if the platform dialog skipped the suffix.
  const QString save_path = layout_xml::ensureLayoutExtension(result.path);
  QSettings().setValue(kLastLayoutDirKey, QFileInfo(save_path).absolutePath());
  const bool include_data_source = !result.option_states.empty() && result.option_states[0];
  saveLayoutToPath(save_path, include_data_source);
#endif
}

#ifdef PJ_TARGET_WASM
void MainWindow::onSaveSourceLayout() {
  QString error;
  const std::optional<QDomDocument> doc = browserSourceLayoutDocument(error);
  if (!doc.has_value()) {
    showBrowserLayoutWarning(this, tr("Save Layout with Sources"), error);
    qWarning("PJ_WASM_SOURCE_LAYOUT_DOWNLOAD_FAILED reason=%s", qPrintable(error));
    return;
  }

  const QString file_name = u"plotjuggler-source-layout.pj4.xml"_s;
  const QByteArray bytes = doc->toByteArray(2);
  FileSelectionService::saveFileContent(this, bytes, file_name);
  emitDiagnostic(
      DiagnosticLevel::kInfo, "Layout", "download-requested",
      tr("Requested source-bound layout download: %1").arg(file_name));
  qInfo(
      "PJ_WASM_SOURCE_LAYOUT_DOWNLOAD_REQUESTED name=%s bytes=%lld", qPrintable(file_name),
      static_cast<long long>(bytes.size()));
}
#endif

void MainWindow::onLoadRecentLayout(const QString& path) {
  if (path.isEmpty()) {
    return;
  }
#ifdef PJ_TARGET_WASM
  BrowserPersistence* persistence = BrowserPersistence::instance();
  const std::optional<BrowserPersistence::LayoutRecipe> recipe =
      persistence != nullptr ? persistence->recentLayout(path) : std::nullopt;
  if (!recipe.has_value()) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "missing-browser-recipe",
        tr("This browser no longer has that recent layout recipe."));
    if (persistence == nullptr || persistence->recentLayouts().isEmpty()) {
      ui_->leftPanel->setRecentEnabled(false);
    }
    return;
  }
  if (layout_selection_pending_) {
    return;
  }
  layout_selection_pending_ = true;
  updateBrowserLayoutActions();
  loadLayoutFromBytes(recipe->xml, recipe->name);
  return;
#else
  if (!QFileInfo::exists(path)) {
    emitDiagnostic(DiagnosticLevel::kWarning, "Layout", "missing", tr("Layout file no longer exists: %1").arg(path));
    // Drop the dead entry so it stops showing up.
    QStringList recent = recentLayouts();
    recent.removeAll(path);
    QSettings().setValue(kRecentLayoutsKey, recent);
    return;
  }
  loadLayoutFromPath(path, LayoutLoadInteractivity::kInteractive);
#endif
}

void MainWindow::onRebuildExtensionsMenu() {
  QMenu* menu = installed_extensions_menu_;
  menu->clear();

  const auto& catalog = session_->extensionCatalog();
  bool added_any = false;
  const auto append_plugins = [&]<typename Plugin>(const std::vector<Plugin>& plugins) {
    for (const auto& plugin : plugins) {
      const QString name = QString::fromStdString(plugin.name);
      const QString version = QString::fromStdString(plugin.version);
      const QString label = version.isEmpty() ? name : u"%1 (%2)"_s.arg(name, version);
      QAction* action = menu->addAction(label);
      action->setEnabled(false);  // Informational only — manage via Marketplace.
      added_any = true;
    }
  };
  append_plugins(catalog.dataSources());
  append_plugins(catalog.messageParsers());
  append_plugins(catalog.toolboxes());

  if (!added_any) {
    QAction* placeholder = menu->addAction(tr("(no extensions installed)"));
    placeholder->setEnabled(false);
  }
}

void MainWindow::stopAllToolboxImports() {
  QSet<PJ::ToolboxRuntimeHost*> stopped;
  for (const auto& ref : std::as_const(toolbox_active_imports_)) {
    if (ref.host != nullptr && !stopped.contains(ref.host) && ref.owner.lock() != nullptr) {
      stopped.insert(ref.host);
      ref.host->requestStopActiveIngests();
    }
  }
}

void MainWindow::adoptToolboxIngestStrip() {
  ingest_strip_owner_kind_ = IngestStripOwnerKind::kInteractiveToolbox;
  ingest_strip_owner_dataset_ = toolbox_ingest_dataset_;
  ingest_progress_->setTitle(toolbox_ingest_label_);
  ingest_progress_->setCounterText({});
  ingest_progress_->setRange(0, 0);  // busy until the first determinate tick
  ingest_show_timer_->start(kIngestStripShowDelayMs);
}

void MainWindow::setIngestStripProgress(quint64 current, quint64 total) {
  // Counts may be bytes far beyond int range — track progress as permille of
  // total rather than raw counts. total 0 keeps the busy/indeterminate bar.
  if (total == 0) {
    ingest_progress_->setRange(0, 0);
  } else {
    ingest_progress_->setRange(0, kIngestProgressResolution);
    ingest_progress_->setValue(static_cast<int>(std::min(current, total) * kIngestProgressResolution / total));
  }
}

bool MainWindow::displaySurvivingIngestOnStrip(DatasetId exclude) {
  const auto& active = session_->sessionManager().activeIngests();
  const auto eligible = [&](DatasetId id) { return id != 0 && id != exclude && active.count(id) != 0; };
  // Interactive survivors outrank the batch job's ingest; among them, prefer
  // the last-started one (today's re-adopt identity) and fall back to the
  // lowest surviving id (see the arbitration rule at the owner state).
  DatasetId pick = 0;
  IngestStripOwnerKind kind = IngestStripOwnerKind::kInteractiveToolbox;
  if (eligible(toolbox_ingest_dataset_) && toolbox_ingest_dataset_ != layout_batch_strip_dataset_) {
    pick = toolbox_ingest_dataset_;
  } else {
    for (const auto& entry : active) {
      if (entry.first == exclude || entry.first == layout_batch_strip_dataset_) {
        continue;
      }
      if (pick == 0 || entry.first < pick) {
        pick = entry.first;
      }
    }
  }
  if (pick == 0 && eligible(layout_batch_strip_dataset_)) {
    pick = layout_batch_strip_dataset_;
    kind = IngestStripOwnerKind::kLayoutBatch;
  }
  if (pick == 0) {
    return false;
  }
  const SessionManager::ActiveIngest& entry = active.at(pick);
  ingest_strip_owner_kind_ = kind;
  ingest_strip_owner_dataset_ = pick;
  ingest_progress_->setTitle(entry.label);
  ingest_progress_->setCounterText({});
  setIngestStripProgress(entry.current, entry.total);
  return true;
}

void MainWindow::releaseIngestStripOwner(DatasetId ended) {
  const bool displayed = (ingest_strip_owner_kind_ == IngestStripOwnerKind::kInteractiveToolbox ||
                          ingest_strip_owner_kind_ == IngestStripOwnerKind::kLayoutBatch) &&
                         ingest_strip_owner_dataset_ == ended;
  if (!displayed) {
    return;  // the strip shows someone else — nothing to hand over
  }
  if (displaySurvivingIngestOnStrip(ended)) {
    return;
  }
  ingest_strip_owner_kind_ = IngestStripOwnerKind::kNone;
  ingest_strip_owner_dataset_ = 0;
  if (!file_loader_->isBusy()) {
    ingest_show_timer_->stop();
    ingest_hide_timer_->start();
  }
}

void MainWindow::onRebuildToolboxMenu() {
  QMenu* menu = title_bar_->toolboxMenu();
  menu->clear();
  menu->setToolTipsVisible(true);

  // List the launchable toolboxes: every loaded toolbox EXCEPT the
  // cloud-tagged ones, which are reached from the Sources panel instead
  // (same manifest "tags" check as LeftPanel::populateCloudToolboxes,
  // inverted here). Each entry launches its toolbox into the chart area.
  bool added_any = false;
  for (const auto& toolbox : session_->extensionCatalog().toolboxes()) {
    // A null vtable/manifest is a load failure already reported elsewhere.
    const auto* vtable = toolbox.library.vtable();
    if (vtable == nullptr || vtable->manifest_json == nullptr) {
      continue;
    }
    const auto manifest = nlohmann::json::parse(vtable->manifest_json, nullptr, /*allow_exceptions=*/false);
    // Skip toolboxes that opt out of this menu: "cloud" ones are reached from the
    // Sources panel; "hidden" ones are launched elsewhere (e.g. the Transform
    // Editor, opened from the "+" in Custom Series).
    bool skip = false;
    if (manifest.is_object()) {
      if (auto it = manifest.find("tags"); it != manifest.end() && it->is_array()) {
        for (const auto& tag : *it) {
          if (tag.is_string() && (tag.get<std::string>() == "cloud" || tag.get<std::string>() == "hidden")) {
            skip = true;
            break;
          }
        }
      }
    }
    if (skip) {
      continue;
    }

    const QString id = QString::fromStdString(toolbox.id);
    const QString name = toolbox.name.empty() ? id : QString::fromStdString(toolbox.name);
    QAction* action = menu->addAction(name);
    if (manifest.is_object()) {
      if (auto desc = manifest.find("description"); desc != manifest.end() && desc->is_string()) {
        action->setToolTip(QString::fromStdString(desc->get<std::string>()));
      }
    }
    connect(action, &QAction::triggered, this, [this, id]() { launchToolbox(id); });
    added_any = true;
  }

  if (!added_any) {
    QAction* placeholder = menu->addAction(tr("(no toolboxes available)"));
    placeholder->setEnabled(false);
  }
}

// layoutReplayHints — the LoadHints shape shared by the desktop reload, the
// browser source-bound replay, and the batch's stock loads — lives in
// LayoutReplayHints.h.

void MainWindow::loadLayoutFromPath(const QString& path, LayoutLoadInteractivity interactivity) {
  const bool interactive = interactivity == LayoutLoadInteractivity::kInteractive;
  // The interactivity-derived policy governs EVERY failure surface of this
  // load, including the pre-classification entry failures below — a
  // kAutomated (--layout) run must never open a modal (Codex r2-3).
  const MissingCurvePolicy restore_policy =
      interactive ? MissingCurvePolicy::kPrompt : MissingCurvePolicy::kRetainAndDiagnose;
  // 1. Open + parse. Every terminal outcome of this load — these entry
  // failures, the user cancels below, and both restore legs' ends — emits
  // layoutRestoreSettled exactly once (the --exit-after-layout boundary).
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    reportLayoutRestoreIssue(restore_policy, "layout-open-failed", tr("Cannot open '%1' for reading.").arg(path));
    emit layoutRestoreSettled(false);
    return;
  }
  QDomDocument doc;
  const QDomDocument::ParseResult parse_result = doc.setContent(&file);
  if (!parse_result) {
    file.close();
    reportLayoutRestoreIssue(
        restore_policy, "layout-parse-failed",
        tr("'%1' is not a valid PJ4 layout: %2 (line %3, col %4)")
            .arg(path, parse_result.errorMessage)
            .arg(parse_result.errorLine)
            .arg(parse_result.errorColumn));
    emit layoutRestoreSettled(false);
    return;
  }
  file.close();

  // Forward-compat schema check. pj4_version is written by xmlSaveState
  // as an integer string; if we encounter a layout from a newer PJ4
  // that introduced incompatible schema changes, warn but proceed —
  // unknown elements are silently ignored downstream anyway. We never
  // hard-fail on this; the user can always re-save under the current
  // schema.
  const QDomElement root = doc.documentElement();
  bool version_ok = false;
  const int version = root.attribute(u"pj4_version"_s, u"0"_s).toInt(&version_ok);
  if (version_ok && version > kLayoutSchemaVersion) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "schema-newer",
        tr("Layout '%1' was saved by a newer PJ4 (pj4_version=%2 > %3); loading best-effort.")
            .arg(QFileInfo(path).fileName())
            .arg(version)
            .arg(kLayoutSchemaVersion));
  }

  // 2. Source-bound layouts may reload their original file; generic layouts
  // (and source-bound ones whose file is missing or where the user opts out)
  // bind straight to the currently-loaded data. The binding attribute records
  // the save-time intent; this is the load-time override.
  const QString binding = root.attribute(u"binding"_s, u"source"_s);
  const QDir layout_dir(QFileInfo(path).absoluteDir());
  // Dataset-path qualifiers are stored relative whenever the data sits beside or
  // beneath the layout. Resolve them to normalized absolute paths before any
  // binder sees the document, so every widget compares full paths while a layout
  // + data directory remains freely relocatable.
  layout_xml::resolveDatasetSourcePaths(doc, layout_dir);
  // A pre-path or source-only layout persisted a numeric id plus a basename-like
  // source. That integer is unsafe outside the session that minted it: strip it on
  // read as well as on write, leaving the source-only fallback to bind only when
  // unique.
  layout_xml::removeUnvalidatedDatasetIds(doc);
  // Annotate legacy (pre-x_basis) plot ranges with their explicit X coordinate
  // basis before any widget restore, so PlotWidget::xmlLoadState reads the basis
  // straight off the <range> instead of re-inferring it from plot mode. No numeric
  // range values change (see normalizePlotRangeBasis).
  layout_xml::normalizePlotRangeBasis(doc);
  const QList<layout_xml::DataSourceRef> replays = layout_xml::extractDataSource(doc, layout_dir);
  // NOTE: any previous in-flight restore is superseded only at this load's
  // COMMIT POINTS (the do_reload branch below and applyRestoredLayout), not
  // here — a Cancel at the reload or trust prompt must be a complete no-op
  // that leaves the old restore draining, exactly as on main (regression
  // review). Classification below reads only loadedSources()/catalog state
  // and prepare() touches only the NEW document, so neither needs the old
  // restore dead.
  // Set when the user chose "Reload original": the data loads (possibly async on
  // a worker), so the layout apply below must wait for the load queue to drain.
  bool reload_requested = false;
  if (binding != "generic"_L1 && !replays.empty()) {
    // Fork the classification ONCE: materialize-bearing sources are
    // batch-owned (§6.2 — the batch rewrites `doc` and classifies them
    // against their provider's effective paths), plain sources keep today's
    // classification untouched. On WASM everything stays plain and `batch`
    // is never constructed — ordinary layouts traverse zero new code.
    std::unique_ptr<LayoutImportBatch> batch;
#ifndef PJ_TARGET_WASM
    QList<layout_xml::DataSourceRef> plain_refs;
    plain_refs.reserve(replays.size());
    for (const auto& replay : replays) {
      if (!layout_xml::hasMaterializeRecord(replay)) {
        plain_refs.push_back(replay);
      }
    }
    if (plain_refs.size() != replays.size()) {
      batch = makeLayoutImportBatch(interactive);
      batch->prepare(doc, replays);
    }
#else
    const QList<layout_xml::DataSourceRef>& plain_refs = replays;
#endif
    // Classify each referenced file: already loaded (skip), missing on disk
    // (warn + skip), or reloadable. A file counts as already loaded only while
    // the catalog has data — a remembered-but-cleared source must reload.
    const auto& loaded = session_->sessionManager().loadedSources();
    const bool catalog_has_data = !session_->catalogModel().isEmpty();
    QList<layout_xml::DataSourceRef> pending;
    for (const auto& replay : plain_refs) {
      if (replay.resolved_path.isEmpty()) {
        continue;
      }
      const bool already_loaded =
          catalog_has_data && std::any_of(loaded.begin(), loaded.end(), [&replay](const auto& src) {
            return layout_xml::isSamePath(src.path, replay.resolved_path);
          });
      if (already_loaded) {
        continue;
      }
      if (!QFileInfo::exists(replay.resolved_path)) {
        emitDiagnostic(
            DiagnosticLevel::kWarning, "Layout", "data-source-missing",
            tr("Layout's data source '%1' does not exist on disk; applying to current data.")
                .arg(replay.resolved_path));
        continue;
      }
      pending.push_back(replay);
    }

    if (!pending.empty() || (batch != nullptr && batch->hasPendingWork())) {
      // The --layout CLI option auto-reloads the layout's source(s) without
      // prompting; interactively, one consolidated prompt covers the whole pending
      // set (never one box per file; "Load Layout only" is the fall-through).
      bool do_reload = !interactive;
      if (interactive) {
        QStringList file_lines;
        file_lines.reserve(pending.size());
        for (const auto& replay : pending) {
          file_lines.push_back(u"  %1"_s.arg(replay.resolved_path));
        }
        if (batch != nullptr) {
          for (const QString& line : batch->pendingSourceLines()) {
            file_lines.push_back(u"  %1"_s.arg(line));
          }
        }
        // Themed prompt (frameless, vertical button column in the app chrome).
        // The button order below defines the index question() returns:
        // 0 = reload, 1 = layout-only (fall-through), 2 = cancel. "Reload source
        // file" is the primary/default action; Esc maps to the kCancelRole button.
        constexpr int kReloadOriginal = 0;
        constexpr int kCancel = 2;
        const int choice = MessageBox::question(
            this, tr("Load Layout"),
            tr("This layout was saved with %n data source(s):\n\n%1", nullptr, static_cast<int>(file_lines.size()))
                .arg(file_lines.join(QLatin1Char('\n'))),
            {{tr("Reload source file"), MessageBox::kPrimaryRole},
             {tr("Load Layout only"), MessageBox::kNeutralRole},
             {tr("Cancel"), MessageBox::kCancelRole}});
        if (choice == kCancel || choice < 0) {
          emit layoutRestoreSettled(false);
          return;  // Cancel or dialog dismissed → abort the layout load.
        }
        do_reload = (choice == kReloadOriginal);
      }
      if (do_reload) {
        reload_requested = true;
        LayoutImportBatch::StartResult batch_result = LayoutImportBatch::StartResult::kNoAsyncWork;
        if (batch != nullptr) {
          // Trust gate (one consolidated prompt / non-interactive refusal)
          // and then the batch's stock loads + first import job. Runs BEFORE
          // the plain loads below so "Cancel layout load" at the trust gate
          // aborts with NOTHING enqueued yet.
          batch_result = batch->start();
          if (batch_result == LayoutImportBatch::StartResult::kCancelledByUser) {
            // Trust-gate Cancel means "keep what I had": start() guarantees
            // nothing was enqueued or captured on this path (pinned by the
            // batch tests), so returning here leaves any in-flight OLD
            // restore draining untouched — base-equivalent semantics.
            emit layoutRestoreSettled(false);
            return;
          }
        }
        // COMMIT POINT: this load will now mutate the session, so the old
        // restore's transaction is superseded HERE — after every cancel
        // return, before the batch member handover and the plain enqueues.
        // Codex r1 F2's property still holds: the batch's own enqueues above
        // and this teardown share one synchronous stretch (no event
        // delivery in between), so no stale waiter can observe them.
        supersedeActiveRestore();
        if (batch_result == LayoutImportBatch::StartResult::kRunning) {
          layout_import_batch_ = std::move(batch);
          // D1: the ingest-lifecycle observation exists exactly while a
          // handed-over batch does (the supersede above tore down any
          // predecessor's observers first).
          installLayoutBatchObservers();
        }
        // Load each pending file. Distinct files append as separate datasets
        // (FileLoader replaces in place only on a basename match), so the full
        // multi-file session is restored. Interactively, FileLoader shows its
        // own error dialog on failure; an AUTOMATED load passes a null dialog
        // parent — the documented headless-load shape, which suppresses the
        // aggregated load-failure dialog and the plugin message-box service
        // (D5: an unattended load must not open modals; Codex r1 F4). Either
        // way, fall through and let the unresolved-curve handling below catch
        // an empty load.
        for (const auto& replay : pending) {
          file_loader_->loadFile(
              replay.resolved_path, interactive ? this : nullptr,
              layoutReplayHints(replay, /*prefer_reuse=*/true, replay.rewrite_plugin_filepath));
        }
      }
      // choice == 1 (Use current data) → fall through and apply the layout to
      // the currently loaded data. A prepared-but-unstarted batch dies here:
      // "Load Layout only" imports nothing.
    }
  }

  // The reloaded data may still be arriving on the worker thread — or a batch
  // import may be producing it with FileLoader entirely idle. In either case,
  // build the layout structure now and bind remaining curves live as topics
  // arrive. If nothing is loading (generic / use-current / a reload that
  // finished synchronously) apply immediately through the complete restore path.
  // BOTH legs ride the same interactivity-derived policy (D5) — never batch
  // survival: a mixed layout whose materialize sources all fail
  // synchronously discards its batch while a plain sibling still goes
  // progressive (Codex r1 F3). The sync leg is likewise reachable
  // non-interactively (§6.4 cross-machine degradation) and must not open
  // the catalog-empty warning or the missing-curve prompt. Interactive
  // loads keep kPrompt unchanged.
  if (restoreHasPendingAsyncWork(reload_requested)) {
    beginProgressiveLayoutRestore(doc, path, restore_policy);
    return;
  }
  applyRestoredLayout(doc, path, restore_policy);
}

#ifdef PJ_TARGET_WASM
void MainWindow::loadLayoutFromBytes(const QByteArray& bytes, const QString& browser_name) {
  if (browser_layout_runtime_ != nullptr) {
    browser_layout_runtime_->pending_recipe_name.clear();
    browser_layout_runtime_->pending_generic_recipe = false;
  }
  QDomDocument doc;
  const QDomDocument::ParseResult parse_result = doc.setContent(bytes);
  if (!parse_result) {
    showBrowserLayoutWarning(
        this, tr("Load Layout"),
        tr("'%1' is not a valid PJ4 layout: %2 (line %3, col %4)")
            .arg(browser_name, parse_result.errorMessage)
            .arg(parse_result.errorLine)
            .arg(parse_result.errorColumn));
    finishBrowserLayoutLoad(browser_name, false, u"parse"_s);
    return;
  }
  const QDomElement root = doc.documentElement();
  const bool source_bound = root.attribute(u"binding"_s, u"source"_s) != "generic"_L1 &&
                            !layout_xml::extractDataSource(doc, QDir()).isEmpty();
  if (browser_layout_runtime_ != nullptr) {
    browser_layout_runtime_->pending_recipe_name = browser_name;
    // Only source-free documents are retained implicitly. Source-bound layouts
    // can carry plugin configs; users keep those explicitly as downloads and
    // reselect them through W9c instead of leaking them into localStorage.
    browser_layout_runtime_->pending_generic_recipe = !source_bound;
  }
  switch (loadBrowserParsedLayout(std::move(doc), browser_name)) {
    case BrowserLayoutLoadResult::kApplied:
      finishBrowserLayoutLoad(browser_name, true);
      break;
    case BrowserLayoutLoadResult::kFailed:
      finishBrowserLayoutLoad(browser_name, false, u"apply"_s);
      break;
    case BrowserLayoutLoadResult::kPending:
      break;
  }
}

MainWindow::BrowserLayoutLoadResult MainWindow::loadBrowserParsedLayout(QDomDocument doc, const QString& browser_name) {
  const QDomElement root = doc.documentElement();
  bool version_ok = false;
  const int version = root.attribute(u"pj4_version"_s, u"0"_s).toInt(&version_ok);
  if (version_ok && version > kLayoutSchemaVersion) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "schema-newer",
        tr("Layout '%1' was saved by a newer PJ4 (pj4_version=%2 > %3); loading best-effort.")
            .arg(browser_name)
            .arg(version)
            .arg(kLayoutSchemaVersion));
  }

  // A browser selection supplies bytes and a display name, never a durable
  // directory. Do not resolve or replay source paths: retain the intent for the
  // later reselection package and bind this slice only to compatible live data.
  layout_xml::removeUnvalidatedDatasetIds(doc);
  layout_xml::normalizePlotRangeBasis(doc);
  const QString binding = root.attribute(u"binding"_s, u"source"_s);
  const QList<layout_xml::DataSourceRef> sources = layout_xml::extractDataSource(doc, QDir());
  if (binding != "generic"_L1 && !sources.empty()) {
    beginBrowserSourceReplay(std::move(doc), browser_name, sources);
    return BrowserLayoutLoadResult::kPending;
  }
  return applyBrowserRestoredLayout(std::move(doc), browser_name) ? BrowserLayoutLoadResult::kApplied
                                                                  : BrowserLayoutLoadResult::kFailed;
}

void MainWindow::beginBrowserSourceReplay(
    QDomDocument doc, const QString& browser_name, QList<layout_xml::DataSourceRef> sources) {
  if (browser_layout_runtime_ == nullptr || browser_layout_runtime_->replay.has_value()) {
    finishBrowserLayoutLoad(browser_name, false, u"busy"_s);
    return;
  }
  // Cloud-materialized sources cannot be re-obtained in the browser: their
  // provider plugins are desktop-only and the saved descriptor names a fetch
  // this page cannot perform. Fail with an explicit diagnostic instead of
  // silently prompting the user to reselect a cache file that only ever
  // existed on a desktop filesystem.
  for (const layout_xml::DataSourceRef& source : sources) {
    if (!source.materialize_provider.isEmpty() || !source.materialize_identity.isEmpty() ||
        !source.materialize_descriptor_json.isEmpty()) {
      showBrowserLayoutWarning(
          this, tr("Load source-bound layout"),
          tr("This layout references cloud-materialized data sources, which are unsupported in the browser. "
             "Open it in the desktop PlotJuggler instead."));
      finishBrowserLayoutLoad(browser_name, false, u"materialized-source"_s);
      return;
    }
  }
  browser_layout_runtime_->replay.emplace(
      BrowserLayoutRuntime::Replay{
          .doc = std::move(doc),
          .layout_name = browser_name,
          .sources = std::move(sources),
          .staged_inputs = {},
          .selected_identities = {},
          .loaded_identities = {},
          .selection_index = 0,
          .load_failed = false,
          .load_failure = {},
          .previous_workspace = captureWorkspace(),
          .selection_button = {},
          .active_picker_generation = 0,
          .loaded_connection = {},
          .failed_connection = {},
          .drained_connection = {},
      });

  BrowserLayoutRuntime::Replay& replay = *browser_layout_runtime_->replay;
  if (replay.sources.isEmpty()) {
    finishBrowserLayoutLoad(browser_name, false, u"source-list"_s);
    return;
  }
  QSet<QString> logical_paths;
  QHash<QString, int> basename_counts;
  for (const layout_xml::DataSourceRef& source : replay.sources) {
    if (source.serialized_path.isEmpty() || logical_paths.contains(source.serialized_path)) {
      showBrowserLayoutWarning(
          this, tr("Load source-bound layout"), tr("The layout does not uniquely identify every saved data source."));
      finishBrowserLayoutLoad(browser_name, false, u"source-list"_s);
      return;
    }
    logical_paths.insert(source.serialized_path);
    const QString basename = BrowserFileStore::sanitizedBasename(source.serialized_path);
    basename_counts.insert(basename, basename_counts.value(basename) + 1);
  }
  for (const layout_xml::DataSourceRef& source : replay.sources) {
    const QString basename = BrowserFileStore::sanitizedBasename(source.serialized_path);
    if (basename_counts.value(basename) > 1 && source.content_sha256.isEmpty()) {
      showBrowserLayoutWarning(
          this, tr("Load source-bound layout"),
          tr("This older layout contains more than one source named '%1', but no content fingerprints to tell "
             "them apart safely. Open the sources separately and use a newer source-bound layout.")
              .arg(basename));
      finishBrowserLayoutLoad(browser_name, false, u"ambiguous-source-name"_s);
      return;
    }
  }

  auto* dialog = new MessageBox(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowModality(Qt::ApplicationModal);
  dialog->setTitle(tr("Load source-bound layout"));
  const QString first_name = BrowserFileStore::sanitizedBasename(replay.sources.front().serialized_path);
  const QString first_context =
      replay.sources.front().plugin_id.isEmpty()
          ? replay.sources.front().serialized_path
          : tr("%1 (plugin: %2)").arg(replay.sources.front().serialized_path, replay.sources.front().plugin_id);
  dialog->setText(
      replay.sources.size() == 1
          ? tr("This layout was saved with data source '%1'. Reselect that file to reload it, or apply the layout "
               "to compatible data already open in this page.")
                .arg(first_name)
          : tr("This layout was saved with %1 data sources. Select source 1 of %1: '%2' for saved slot '%3'. "
               "Every file will be staged before any import begins.")
                .arg(replay.sources.size())
                .arg(first_name, first_context));
  QPushButton* reselect_button = dialog->addButton(
      replay.sources.size() == 1 ? tr("Reselect source") : tr("Select source 1 of %1").arg(replay.sources.size()),
      MessageBox::kPrimaryRole, /*close_on_click=*/false);
  dialog->addButton(tr("Apply to current data"), MessageBox::kNeutralRole);
  dialog->addButton(tr("Cancel"), MessageBox::kCancelRole);
  replay.selection_button = reselect_button;
  browser_layout_runtime_->decision_dialog = dialog;

  QPointer<MainWindow> self(this);
  connect(reselect_button, &QPushButton::clicked, this, [self]() {
    if (!self.isNull()) {
      // Keep the decision surface alive until the picker/stager calls back. If
      // a browser drops a synthetic or denied picker request without a callback,
      // the user can click Reselect again instead of being left in a locked state.
      self->selectBrowserReplaySource();
    }
  });
  QPointer<MessageBox> guarded_dialog(dialog);
  connect(dialog, &QDialog::finished, this, [self, guarded_dialog](int) {
    if (self.isNull() || guarded_dialog.isNull() || self->browser_layout_runtime_ == nullptr ||
        !self->browser_layout_runtime_->replay.has_value() ||
        self->browser_layout_runtime_->decision_dialog != guarded_dialog) {
      return;
    }
    self->browser_layout_runtime_->decision_dialog.clear();
    const int choice = guarded_dialog->clickedIndex();
    if (choice == 0) {
      // The Reselect button itself never finishes the dialog. Reaching this
      // branch means an external close happened after it was clicked; cancel
      // the replay so a vanished surface cannot lock layout actions forever.
    }
    if (choice == 1) {
      BrowserLayoutRuntime::Replay& replay = *self->browser_layout_runtime_->replay;
      QDomDocument apply_doc = replay.doc;
      QHash<QString, QString> identity_for_logical_path;
      QSet<QString> consumed_identities;
      QStringList matched_identities;
      const auto live_datasets = self->session_->catalogModel().datasets();
      for (const layout_xml::DataSourceRef& source : replay.sources) {
        const QString expected_name = BrowserFileStore::sanitizedBasename(source.serialized_path);
        const bool basename_is_ambiguous =
            std::count_if(
                replay.sources.cbegin(), replay.sources.cend(),
                [&expected_name](const layout_xml::DataSourceRef& candidate) {
                  return BrowserFileStore::sanitizedBasename(candidate.serialized_path) == expected_name;
                }) > 1;
        QString matching_identity;
        bool ambiguous = false;
        QSet<QString> inspected;
        for (const auto& [id, unused_name] : live_datasets) {
          (void)unused_name;
          const QString identity = self->file_loader_->sourcePathForDataset(id);
          if (inspected.contains(identity) || consumed_identities.contains(identity)) {
            continue;
          }
          inspected.insert(identity);
          const auto record_it = self->browser_layout_runtime_->sources.constFind(identity);
          if (!isBrowserUploadIdentity(identity) || record_it == self->browser_layout_runtime_->sources.cend() ||
              BrowserFileStore::sanitizedBasename(record_it->display_name) != expected_name) {
            continue;
          }
          if (basename_is_ambiguous && record_it->content_sha256 != source.content_sha256) {
            continue;
          }
          std::vector<BrowserLayoutRuntime::DatasetShape> live_shape;
          for (const auto& [candidate_id, candidate_name] : live_datasets) {
            (void)candidate_name;
            if (self->file_loader_->sourcePathForDataset(candidate_id) == identity) {
              live_shape.push_back({
                  .dataset_id = candidate_id,
                  .source_name = self->session_->catalogModel().datasetSourceName(candidate_id).value_or(QString{}),
              });
            }
          }
          if (live_shape.size() != record_it->datasets.size() ||
              !std::equal(
                  live_shape.cbegin(), live_shape.cend(), record_it->datasets.cbegin(),
                  [](const auto& current, const auto& original) {
                    return current.dataset_id == original.dataset_id && current.source_name == original.source_name;
                  })) {
            continue;
          }
          const int expected_count = source.datasets.isEmpty() ? 1 : source.datasets.size();
          if (static_cast<int>(record_it->datasets.size()) != expected_count) {
            continue;
          }
          bool shape_matches = true;
          for (int index = 0; index < expected_count && !source.datasets.isEmpty(); ++index) {
            const QString& expected_source = source.datasets[index].source_name;
            shape_matches = expected_source.isEmpty() ||
                            record_it->datasets[static_cast<std::size_t>(index)].source_name == expected_source;
            if (!shape_matches) {
              break;
            }
          }
          if (!shape_matches) {
            continue;
          }
          if (!matching_identity.isEmpty()) {
            ambiguous = true;
            break;
          }
          matching_identity = identity;
        }
        if (ambiguous || matching_identity.isEmpty()) {
          showBrowserLayoutWarning(
              self, self->tr("Load source-bound layout"),
              self->tr("The currently open browser data does not uniquely match every saved source."));
          const QString layout_name = replay.layout_name;
          self->finishBrowserLayoutLoad(layout_name, false, u"current-source-shape"_s);
          return;
        }
        consumed_identities.insert(matching_identity);
        matched_identities.push_back(matching_identity);
        identity_for_logical_path.insert(source.serialized_path, matching_identity);
      }

      QDomElement file_info = apply_doc.documentElement()
                                  .firstChildElement(u"previouslyLoaded_Datafiles"_s)
                                  .firstChildElement(u"fileInfo"_s);
      for (int index = 0; index < matched_identities.size() && !file_info.isNull(); ++index) {
        file_info.setAttribute(u"filename"_s, matched_identities[index]);
        file_info = file_info.nextSiblingElement(u"fileInfo"_s);
      }
      layout_xml::remapDatasetSourcePaths(apply_doc, [&identity_for_logical_path](const QString& path) {
        return identity_for_logical_path.value(path, path);
      });
      const QString layout_name = replay.layout_name;
      const bool applied = self->applyBrowserRestoredLayout(std::move(apply_doc), layout_name);
      self->finishBrowserLayoutLoad(layout_name, applied, applied ? QString{} : u"apply"_s);
      return;
    }
    const QString layout_name = self->browser_layout_runtime_->replay->layout_name;
    self->finishBrowserLayoutLoad(layout_name, false, u"cancelled"_s);
  });
  dialog->show();
  qInfo("PJ_WASM_SOURCE_LAYOUT_DECISION name=%s", qPrintable(browser_name));
}

quint64 MainWindow::beginBrowserReplayPickerGeneration() {
  if (browser_layout_runtime_ == nullptr || !browser_layout_runtime_->replay.has_value()) {
    return 0;
  }
  // Each click supersedes any earlier picker that the browser may have left
  // pending. The counter belongs to the whole page lifetime (not one Replay),
  // so a late callback cannot attach itself to a later layout transaction.
  ++browser_layout_runtime_->next_picker_generation;
  if (browser_layout_runtime_->next_picker_generation == 0) {
    ++browser_layout_runtime_->next_picker_generation;
  }
  const quint64 generation = browser_layout_runtime_->next_picker_generation;
  browser_layout_runtime_->replay->active_picker_generation = generation;
  return generation;
}

void MainWindow::selectBrowserReplaySource() {
  const quint64 generation = beginBrowserReplayPickerGeneration();
  if (generation == 0) {
    return;
  }
  QPointer<MainWindow> self(this);
  file_loader_->selectBrowserInput(this, [self, generation](FileLoader::BrowserSelectionResult selection) mutable {
    if (self.isNull()) {
      return;
    }
    self->loadBrowserReplaySource(
        generation, std::move(selection.browser_name), std::move(selection.input), std::move(selection.error));
  });
}

void MainWindow::loadBrowserReplaySource(
    quint64 picker_generation, QString browser_name, std::optional<LoadInput> input, QString error) {
  if (browser_layout_runtime_ == nullptr || !browser_layout_runtime_->replay.has_value()) {
    return;
  }
  BrowserLayoutRuntime::Replay& replay = *browser_layout_runtime_->replay;
  if (replay.active_picker_generation != picker_generation) {
    qInfo(
        "PJ_WASM_SOURCE_LAYOUT_STALE_PICKER generation=%llu active=%llu",
        static_cast<unsigned long long>(picker_generation),
        static_cast<unsigned long long>(replay.active_picker_generation));
    return;
  }
  // Consume this callback exactly once. A duplicate delivery with the same
  // token is stale even if it arrives synchronously before the next click.
  replay.active_picker_generation = 0;
  const QString layout_name = replay.layout_name;
  if (replay.selection_index < 0 || replay.selection_index >= replay.sources.size()) {
    finishBrowserLayoutLoad(layout_name, false, u"selection-index"_s);
    return;
  }
  if (!input.has_value()) {
    if (error.isEmpty()) {
      finishBrowserLayoutLoad(layout_name, false, u"cancelled"_s);
      return;
    }
    showBrowserLayoutWarning(this, tr("Load source-bound layout"), error);
    finishBrowserLayoutLoad(layout_name, false, u"staging"_s);
    return;
  }

  const layout_xml::DataSourceRef& source = replay.sources[replay.selection_index];
  const QString expected_name = BrowserFileStore::sanitizedBasename(source.serialized_path);
  const QString selected_name = BrowserFileStore::sanitizedBasename(browser_name);
  if (selected_name != expected_name) {
    showBrowserLayoutWarning(
        this, tr("Load source-bound layout"),
        tr("Select '%1' to replay this layout (selected '%2').").arg(expected_name, selected_name));
    qInfo("PJ_WASM_SOURCE_LAYOUT_SELECTION_REJECTED index=%d reason=name", replay.selection_index);
    return;
  }
  const bool basename_is_ambiguous =
      std::count_if(
          replay.sources.cbegin(), replay.sources.cend(), [&expected_name](const layout_xml::DataSourceRef& candidate) {
            return BrowserFileStore::sanitizedBasename(candidate.serialized_path) == expected_name;
          }) > 1;
  if (basename_is_ambiguous && input->content_sha256 != source.content_sha256) {
    showBrowserLayoutWarning(
        this, tr("Load source-bound layout"),
        tr("The selected '%1' is not the file saved for source %2 of %3 ('%4'). Select the original file for "
           "that saved slot.")
            .arg(expected_name)
            .arg(replay.selection_index + 1)
            .arg(replay.sources.size())
            .arg(source.serialized_path));
    qInfo("PJ_WASM_SOURCE_LAYOUT_SELECTION_REJECTED index=%d reason=content", replay.selection_index);
    return;
  }

  replay.selected_identities.push_back(input->source_identity);
  replay.staged_inputs.push_back(std::move(*input));
  ++replay.selection_index;
  qInfo(
      "PJ_WASM_SOURCE_LAYOUT_STAGED index=%d total=%d name=%s", replay.selection_index, replay.sources.size(),
      qPrintable(selected_name));

  if (replay.selection_index < replay.sources.size()) {
    const layout_xml::DataSourceRef& next_source = replay.sources[replay.selection_index];
    const QString next_name = BrowserFileStore::sanitizedBasename(next_source.serialized_path);
    const QString next_context = next_source.plugin_id.isEmpty()
                                     ? next_source.serialized_path
                                     : tr("%1 (plugin: %2)").arg(next_source.serialized_path, next_source.plugin_id);
    if (!browser_layout_runtime_->decision_dialog.isNull()) {
      browser_layout_runtime_->decision_dialog->setText(
          tr("Staged %1 of %2 data sources. Select source %3 of %2: '%4' for saved slot '%5'. No import has "
             "started yet.")
              .arg(replay.selection_index)
              .arg(replay.sources.size())
              .arg(replay.selection_index + 1)
              .arg(next_name, next_context));
    }
    if (!replay.selection_button.isNull()) {
      replay.selection_button->setText(
          tr("Select source %1 of %2").arg(replay.selection_index + 1).arg(replay.sources.size()));
    }
    return;
  }

  if (!browser_layout_runtime_->decision_dialog.isNull()) {
    QPointer<MessageBox> decision = browser_layout_runtime_->decision_dialog;
    browser_layout_runtime_->decision_dialog.clear();
    decision->accept();
  }
  startBrowserReplayImports();
}

void MainWindow::startBrowserReplayImports() {
  if (browser_layout_runtime_ == nullptr || !browser_layout_runtime_->replay.has_value()) {
    return;
  }
  BrowserLayoutRuntime::Replay& replay = *browser_layout_runtime_->replay;
  const QString layout_name = replay.layout_name;
  if (replay.sources.size() != replay.selected_identities.size() ||
      replay.sources.size() != static_cast<int>(replay.staged_inputs.size())) {
    finishBrowserLayoutLoad(layout_name, false, u"staging-shape"_s);
    return;
  }

  QPointer<MainWindow> self(this);
  replay.loaded_connection = connect(
      file_loader_.get(), &FileLoader::fileLoaded, this,
      [self](const QString& path, const QString&, const QString&, const QString&) {
        if (self.isNull() || self->browser_layout_runtime_ == nullptr ||
            !self->browser_layout_runtime_->replay.has_value()) {
          return;
        }
        BrowserLayoutRuntime::Replay& active = *self->browser_layout_runtime_->replay;
        if (active.selected_identities.contains(path)) {
          active.loaded_identities.insert(path);
        }
      });
  replay.failed_connection = connect(
      file_loader_.get(), &FileLoader::fileLoadFailed, this, [self](const QString& path, const QString& reason) {
        if (self.isNull() || self->browser_layout_runtime_ == nullptr ||
            !self->browser_layout_runtime_->replay.has_value()) {
          return;
        }
        BrowserLayoutRuntime::Replay& active = *self->browser_layout_runtime_->replay;
        if (active.selected_identities.contains(path)) {
          active.load_failed = true;
          active.load_failure = reason;
        }
      });
  replay.drained_connection = connect(file_loader_.get(), &FileLoader::queueDrained, this, [self]() {
    if (self.isNull() || self->browser_layout_runtime_ == nullptr ||
        !self->browser_layout_runtime_->replay.has_value()) {
      return;
    }
    self->finishBrowserReplayImports();
  });

  for (int index = 0; index < replay.sources.size(); ++index) {
    const layout_xml::DataSourceRef& source = replay.sources[index];
    // The browser preset carries the previous upload's expired MEMFS path, so
    // a saved plugin also opts into the filepath rewrite.
    const bool has_saved_plugin = !source.plugin_manifest_id.isEmpty() || !source.plugin_id.isEmpty();
    const LoadHints hints = layoutReplayHints(source, /*prefer_reuse=*/false, /*rewrite=*/has_saved_plugin);
    if (!file_loader_->loadFile(std::move(replay.staged_inputs[static_cast<std::size_t>(index)]), this, hints)) {
      replay.load_failed = true;
      replay.load_failure = tr("A staged source could not be queued for import.");
    }
  }
}

void MainWindow::finishBrowserReplayImports() {
  if (browser_layout_runtime_ == nullptr || !browser_layout_runtime_->replay.has_value()) {
    return;
  }
  BrowserLayoutRuntime::Replay& replay = *browser_layout_runtime_->replay;
  const QString layout_name = replay.layout_name;
  QString validation_error;
  if (replay.load_failed || replay.loaded_identities.size() != replay.selected_identities.size()) {
    validation_error = replay.load_failure.isEmpty()
                           ? tr("One or more selected sources closed without completing their import.")
                           : replay.load_failure;
  }

  QSet<int> timeline_slots;
  const auto live_datasets = session_->catalogModel().datasets();
  for (int source_index = 0; validation_error.isEmpty() && source_index < replay.sources.size(); ++source_index) {
    const layout_xml::DataSourceRef& source = replay.sources[source_index];
    const QString& identity = replay.selected_identities[source_index];
    std::vector<DatasetId> candidates;
    for (const auto& [dataset_id, unused_name] : live_datasets) {
      (void)unused_name;
      if (file_loader_->sourcePathForDataset(dataset_id) == identity) {
        candidates.push_back(dataset_id);
      }
    }
    const int expected_count = source.datasets.isEmpty() ? 1 : source.datasets.size();
    if (static_cast<int>(candidates.size()) != expected_count) {
      validation_error = tr("Source '%1' produced %2 dataset(s); the layout requires exactly %3.")
                             .arg(BrowserFileStore::sanitizedBasename(source.serialized_path))
                             .arg(static_cast<qulonglong>(candidates.size()))
                             .arg(expected_count);
      break;
    }
    for (int dataset_index = 0; dataset_index < expected_count; ++dataset_index) {
      if (source.datasets.isEmpty()) {
        continue;
      }
      const layout_xml::DataSourceDatasetRef& expected = source.datasets[dataset_index];
      const QString actual_name = session_->catalogModel()
                                      .datasetSourceName(candidates[static_cast<std::size_t>(dataset_index)])
                                      .value_or(QString{});
      if ((expected.source_index >= 0 && expected.source_index != dataset_index) ||
          (!expected.source_name.isEmpty() && expected.source_name != actual_name)) {
        validation_error = tr("Source '%1' did not reproduce the saved fan-out dataset order and names.")
                               .arg(BrowserFileStore::sanitizedBasename(source.serialized_path));
        break;
      }
      if (expected.timeline_order >= 0 && timeline_slots.contains(expected.timeline_order)) {
        validation_error = tr("The saved Source Timeline contains duplicate track positions.");
        break;
      }
      if (expected.timeline_order >= 0) {
        timeline_slots.insert(expected.timeline_order);
      }
      if (!expected.has_display_offset) {
        continue;
      }
      qint64 offset = expected.display_offset_ns;
      if (expected.display_offset_includes_global_reference) {
        const std::optional<qint64> migrated =
            checkedTimelineDifference(offset, session_->sessionManager().globalTimeReference());
        if (!migrated.has_value()) {
          validation_error = tr("A saved Source Timeline offset is outside the supported range.");
          break;
        }
        offset = *migrated;
      }
      if (const auto raw = session_->datasetRawTimeRange(candidates[static_cast<std::size_t>(dataset_index)]);
          raw.has_value() && (!timelineDifferenceFits(raw->min, offset) || !timelineDifferenceFits(raw->max, offset))) {
        validation_error = tr("A saved Source Timeline offset is outside the selected dataset's supported range.");
        break;
      }
    }
  }
  for (int slot = 0; validation_error.isEmpty() && slot < timeline_slots.size(); ++slot) {
    if (!timeline_slots.contains(slot)) {
      validation_error = tr("The saved Source Timeline contains invalid track positions.");
    }
  }

  if (!validation_error.isEmpty()) {
    showBrowserLayoutWarning(this, tr("Load source-bound layout"), validation_error);
    rollbackBrowserReplayImports();
    finishBrowserLayoutLoad(layout_name, false, u"source-shape"_s);
    return;
  }

  QDomDocument apply_doc = replay.doc;
  QHash<QString, QString> identity_for_logical_path;
  QDomElement file_info =
      apply_doc.documentElement().firstChildElement(u"previouslyLoaded_Datafiles"_s).firstChildElement(u"fileInfo"_s);
  for (int index = 0; index < replay.sources.size(); ++index) {
    identity_for_logical_path.insert(replay.sources[index].serialized_path, replay.selected_identities[index]);
    if (!file_info.isNull()) {
      file_info.setAttribute(u"filename"_s, replay.selected_identities[index]);
      file_info = file_info.nextSiblingElement(u"fileInfo"_s);
    }
  }
  layout_xml::remapDatasetSourcePaths(apply_doc, [&identity_for_logical_path](const QString& path) {
    return identity_for_logical_path.value(path, path);
  });
  layout_xml::removeUnvalidatedDatasetIds(apply_doc);
  const bool applied = applyBrowserRestoredLayout(std::move(apply_doc), layout_name);
  finishBrowserLayoutLoad(layout_name, applied, applied ? QString{} : u"apply-after-source-load"_s);
}

void MainWindow::rollbackBrowserReplayImports() {
  if (browser_layout_runtime_ == nullptr || !browser_layout_runtime_->replay.has_value()) {
    return;
  }
  BrowserLayoutRuntime::Replay& replay = *browser_layout_runtime_->replay;
  QSet<QString> imported;
  for (const QString& identity : replay.selected_identities) {
    imported.insert(identity);
  }
  std::vector<DatasetId> remove_ids;
  for (const auto& [dataset_id, unused_name] : session_->catalogModel().datasets()) {
    (void)unused_name;
    if (imported.contains(file_loader_->sourcePathForDataset(dataset_id))) {
      remove_ids.push_back(dataset_id);
    }
  }
  for (const DatasetId dataset_id : remove_ids) {
    removeDatasetData(dataset_id);
  }
  for (const QString& identity : imported) {
    browser_layout_runtime_->sources.remove(identity);
  }
  const CapturedWorkspace previous = replay.previous_workspace;
  if (restoreWorkspaceState(previous, MissingCurvePolicy::kExact, TimelineRestoreMode::kExact) !=
      RestoreResult::kApplied) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "browser-source-rollback-failed",
        tr("The failed source replay was removed, but the previous workspace could not be restored exactly."));
  }
  session_->seedPlaybackFromSession();
}

void MainWindow::finishBrowserLayoutLoad(const QString& browser_name, bool applied, const QString& reason) {
  if (browser_layout_runtime_ != nullptr && browser_layout_runtime_->replay.has_value()) {
    QObject::disconnect(browser_layout_runtime_->replay->loaded_connection);
    QObject::disconnect(browser_layout_runtime_->replay->failed_connection);
    QObject::disconnect(browser_layout_runtime_->replay->drained_connection);
    if (!browser_layout_runtime_->decision_dialog.isNull()) {
      QPointer<MessageBox> decision = browser_layout_runtime_->decision_dialog;
      browser_layout_runtime_->decision_dialog.clear();
      decision->reject();
    }
    browser_layout_runtime_->replay.reset();
  }
  if (browser_layout_runtime_ != nullptr) {
    if (applied && browser_layout_runtime_->pending_generic_recipe && BrowserPersistence::instance() != nullptr) {
      QString persistence_error;
      const QByteArray recipe = browserGenericLayoutDocument().toByteArray(2);
      if (BrowserPersistence::instance()->rememberGenericLayout(
              browser_layout_runtime_->pending_recipe_name, recipe, &persistence_error)) {
        ui_->leftPanel->setRecentEnabled(true);
        qInfo(
            "PJ_WASM_LAYOUT_RECIPE_RECORDED name=%s bytes=%lld count=%lld", qPrintable(browser_name),
            static_cast<long long>(recipe.size()),
            static_cast<long long>(BrowserPersistence::instance()->recentLayouts().size()));
      } else {
        qCWarning(lcMain).noquote() << "Browser layout recipe was not retained:" << persistence_error;
      }
    }
    browser_layout_runtime_->pending_recipe_name.clear();
    browser_layout_runtime_->pending_generic_recipe = false;
  }
  layout_selection_pending_ = false;
  updateBrowserLayoutActions();
  if (applied) {
    qInfo("PJ_WASM_LAYOUT_LOAD_OK name=%s", qPrintable(browser_name));
  } else if (reason == "cancelled"_L1) {
    qInfo("PJ_WASM_SOURCE_LAYOUT_CANCELLED name=%s", qPrintable(browser_name));
  } else {
    qWarning(
        "PJ_WASM_LAYOUT_LOAD_FAILED name=%s reason=%s", qPrintable(browser_name),
        qPrintable(reason.isEmpty() ? u"apply"_s : reason));
  }
}

void MainWindow::updateBrowserLayoutActions() {
  const bool idle = !layout_selection_pending_;
  if (action_load_layout_ != nullptr) {
    action_load_layout_->setEnabled(idle);
  }
  if (action_save_layout_ != nullptr) {
    action_save_layout_->setEnabled(idle);
  }

  bool source_eligible = false;
  if (idle) {
    QString ignored_error;
    source_eligible = browserSourceLayoutDocument(ignored_error).has_value();
  }
  if (action_save_source_layout_ != nullptr) {
    action_save_source_layout_->setEnabled(idle && source_eligible);
  }
}
#endif

void MainWindow::loadLayoutAtStartup(const QString& path) {
  // --layout CLI entry: load the layout and auto-reload its data source(s)
  // with no prompt. Interactivity is threaded as an explicit parameter (and
  // captured by any LayoutImportBatch at construction) instead of a member
  // flag, because a flag reset on return cannot govern late async
  // continuations (D5).
  loadLayoutFromPath(path, LayoutLoadInteractivity::kAutomated);
}

void MainWindow::enableAutoplay() {
  autoplay_pending_ = true;
  // Start looped playback the first time the engine reports a non-empty range — set
  // synchronously by --test-data, or asynchronously once --layout's worker load
  // finishes and seeds the range. Guarded by autoplay_pending_ so it fires exactly
  // once (the rangeChanged connection stays but no-ops afterward), leaving a later
  // manual pause/seek untouched.
  PlaybackEngine& playback = session_->playbackEngine();
  connect(&playback, &PlaybackEngine::rangeChanged, this, [this](double min, double max) {
    if (!autoplay_pending_ || !(max > min)) {
      return;
    }
    autoplay_pending_ = false;
    PlaybackEngine& pb = session_->playbackEngine();
    pb.setLooping(true);
    pb.play();
  });
}

#ifdef PJ_TARGET_WASM
bool MainWindow::applyBrowserRestoredLayout(QDomDocument doc, const QString& browser_name) {
  if (session_->catalogModel().datasets().empty()) {
    showBrowserLayoutWarning(
        this, tr("Load Layout"), tr("No data is loaded. Open a data source before applying this layout."));
    return false;
  }

  // kExact turns an unresolved curve/scene into failure. restoreWorkspaceState
  // rolls any failed application back to the captured workspace, so a browser
  // layout can never silently drop incompatible content.
  if (restoreWorkspaceState(doc, MissingCurvePolicy::kExact) != RestoreResult::kApplied) {
    showBrowserLayoutWarning(
        this, tr("Load Layout"),
        tr("Layout was parsed but could not be applied to the currently loaded data. Open matching data sources "
           "and try again."));
    return false;
  }

  // Source replay validation has already proven an exact dataset/fan-out
  // shape. Apply per-source offsets and vertical order only after the workspace
  // succeeds, so a curve/scene failure cannot leave a partially moved timeline.
  static_cast<void>(applyTimelineStateFromLayout(layout_xml::extractDataSource(doc, QDir())));
  restoreBrowserChromeAndPanels(doc, browser_name);
  resetUndoHistory();
  return true;
}

void MainWindow::restoreBrowserChromeAndPanels(const QDomDocument& doc, const QString& browser_name) {
  ui_->curveListPanel->restoreListState(doc.documentElement().firstChildElement(u"curve_list_state"_s));
  restoreRightPanelState(doc.documentElement().firstChildElement(u"right_panel_state"_s));
  ui_->leftPanel->restoreSourcesState(doc.documentElement().firstChildElement(u"left_panel_state"_s));
  restoreChromeState(doc.documentElement().firstChildElement(u"chrome_state"_s));

  // Explicit source replay applies per-source offsets/order after exact shape
  // validation; this element owns the remaining global view chrome.
  restoreSourceTimelineViewState(doc.documentElement().firstChildElement(u"source_timeline"_s));
  emitDiagnostic(DiagnosticLevel::kInfo, "Layout", "loaded", tr("Loaded layout: %1").arg(browser_name));
}
#endif

void MainWindow::applyRestoredLayout(QDomDocument doc, const QString& path, MissingCurvePolicy policy) {
  // 3. Filters + curve rebinding + plot apply happen together in restoreWorkspaceState
  // below. Each curve resolves against whichever loaded dataset actually holds its
  // topic+field (first match in load order), so a multi-file layout restores
  // each plot against its own source — and a layout built on one recording still
  // reuses on a similar one (same topics/fields). This is the same restore path
  // undo/redo uses; there is deliberately no "apply to which
  // dataset?" prompt — a saved layout binds to its data, not to one chosen set.
  // Paths no loaded dataset can provide are surfaced via the missing-curve prompt.
  // (A restore with pending async work never reaches this leg — the
  // restoreHasPendingAsyncWork routing sent it progressive.)
  if (session_->catalogModel().datasets().empty()) {
    // D5: a kRetainAndDiagnose (non-interactive) apply reports through the
    // diagnostic sink; interactive keeps the modal warning. A no-data return
    // is a NO-OP — an in-flight previous restore keeps draining.
    reportLayoutRestoreIssue(
        policy, "layout-apply-no-data", tr("No data is loaded. Open a data source before applying this layout."));
    emit layoutRestoreSettled(false);
    return;
  }
  // COMMIT POINT: the workspace is about to be replaced — supersede any
  // in-flight previous restore now (a no-op when the do_reload branch
  // already did; see supersedeActiveRestore).
  supersedeActiveRestore();
  // 4. Recreate filters, rebind curves, and apply plots/toggles through the ONE restore
  // path shared with undo/redo (kPrompt: a curve no loaded dataset can provide raises the
  // missing-curve prompt). Filters are recreated BEFORE the curve rebind so each derived
  // output topic is in the catalog. Panel/chrome restores below stay layout-only.
  switch (restoreWorkspaceState(doc, policy)) {
    case RestoreResult::kCancelled:
      emit layoutRestoreSettled(false);
      return;  // user aborted at the missing-curve prompt
    case RestoreResult::kFailed:
      // If step 2 already reloaded a data source AND apply failed, the world is left
      // half-mutated: the new data is loaded but the user's plots/panels never came
      // back. Rolling back a synchronous ingest is not currently feasible (FileLoader
      // has no "unload" API and the DataEngine doesn't support transactional commits).
      // The warning (dialog-vs-diagnostic per the policy) is the best signal
      // we can offer.
      reportLayoutRestoreIssue(
          policy, "layout-apply-failed",
          tr("Layout was parsed but could not be applied. If a data source was reloaded, it is still loaded."));
      emit layoutRestoreSettled(false);
      return;
    case RestoreResult::kApplied:
      break;
  }

  restoreChromeAndPanels(doc, path);
  commitRestoredLayout(doc, policy);
  emit layoutRestoreSettled(true);
}

void MainWindow::restoreChromeAndPanels(const QDomDocument& doc, const QString& path) {
  // 4a. Restore curve-list content state (filters + show_topics/show_values toggles).
  ui_->curveListPanel->restoreListState(doc.documentElement().firstChildElement(u"curve_list_state"_s));

  // 4b. Restore right-panel state.
  restoreRightPanelState(doc.documentElement().firstChildElement(u"right_panel_state"_s));

  // 4c. Restore LeftPanel Sources tab + streaming controls.
  ui_->leftPanel->restoreSourcesState(doc.documentElement().firstChildElement(u"left_panel_state"_s));

  // 4d. Restore chrome state (panel visibilities + splitter sizes).
  restoreChromeState(doc.documentElement().firstChildElement(u"chrome_state"_s));

  // 4e. Restore Source Timeline state (per-source display offsets + track order).
  // Runs after the datasets are (re)loaded so it re-binds them by path. The data
  // source refs were consumed during the reload classification in
  // loadLayoutFromPath; re-extract them here (a pure parse of doc) so this restore
  // step has them in scope.
  const QDir timeline_layout_dir(QFileInfo(path).absoluteDir());
  const QList<layout_xml::DataSourceRef> timeline_sources = layout_xml::extractDataSource(doc, timeline_layout_dir);
  const bool offset_changed = applyTimelineStateFromLayout(timeline_sources);
  if (progressive_layout_in_flight_) {
    // Async reload: the worker has not yet registered the in-flight dataset's source
    // path, so applyTimelineStateFromLayout above found no candidates and skipped its
    // offsets. Stash the refs so onProgressiveLayoutDrained re-applies them (and then
    // re-frames viewports) once the paths settle. Skip-don't-guess is preserved: a
    // ref already consumed on this leg re-applies idempotently (setDisplayOffset no-ops).
    pending_timeline_sources_ = timeline_sources;
  } else if (offset_changed) {
    // Sync leg: the plots were framed by restoreWorkspaceState with the PRE-apply
    // offset, and the per-dataset displayOffsetChanged handler only replots (never
    // reframes). The saved-viewport stash survives xmlLoadState (clear_after=false),
    // so re-convert the saved ABSOLUTE window with the now-settled offset and drop the
    // stash. A plot with no/degenerate saved range falls back to zoomOut.
    forEachPlot([](PlotWidget* plot) { plot->applySavedViewportOrZoom(/*clear_after=*/true); });
  }

  // 4f. Restore the timeline's global view chrome (zoom/scroll/name-column/snap),
  // AFTER 4e so zoom/scroll map onto the offset-adjusted, rebuilt scene.
  restoreSourceTimelineViewState(doc.documentElement().firstChildElement(u"source_timeline"_s));

  // NOTE: pinned toolbox tabs are deliberately NOT restored here. This
  // function also runs on the progressive leg BEFORE the load can still be
  // aborted/rolled back — replacing the pinned set that early would lose
  // the user's live toolboxes on a cancelled restore. Both legs restore
  // them at their COMMIT point instead (applyRestoredLayout's tail /
  // onProgressiveLayoutDrained's tail).

  // 5. Recent files + diagnostic
  recordRecentLayout(path);
  emitDiagnostic(DiagnosticLevel::kInfo, "Layout", "loaded", tr("Loaded layout: %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::beginProgressiveLayoutRestore(QDomDocument doc, const QString& path, MissingCurvePolicy policy) {
  cancelProgressiveLayoutRestore();
  if (layoutImportBatchActive() && batch_workspace_checkpoint_ != nullptr) {
    // ONE capture per batch restore: reuse the checkpoint the batch's
    // begin_workspace_checkpoint hook took inside start() moments ago —
    // only async enqueues happened since (loads queued, jobs launched; no
    // synchronous workspace mutation), so the two capture points are
    // state-equivalent.
    progressive_previous_workspace_ = *batch_workspace_checkpoint_;
  } else {
    progressive_previous_workspace_ = capturePortableWorkspace();
  }
  batch_workspace_checkpoint_.reset();
  // The caller-derived policy (the load's interactivity — NEVER inferred
  // from batch survival, Codex r1 F3), captured now and consumed at the
  // drain long after every caller stack has unwound (D5: policy is data).
  progressive_missing_curve_policy_ = policy;
  progressive_layout_in_flight_ = true;
  progressive_layout_doc_ = doc;

  bool applied = false;
  {
    QScopedValueRollback guard(applying_state_, true);
    // FileLoader is still producing topics. Tear down the previous graph now,
    // but replay nothing until queueDrained, when missing inputs are meaningful.
    static_cast<void>(restoreDataProcessors(QDomElement{}));
    // rebindCurvesToLoadedDatasets rewrites doc in place; its unresolved-paths
    // return is not needed here because the progressive binder reports them at
    // drain.
    static_cast<void>(rebindCurvesToLoadedDatasets(doc));
    if (xmlLoadState(doc)) {
      collectPendingDisplayBindings(doc);
      restoreChromeAndPanels(doc, path);
      broadcastTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
      applied = true;
    }
  }

  if (!applied) {
    static_cast<void>(abortProgressiveRestore());
    // Dialog-vs-diagnostic per the caller-derived policy (Codex r1 F6): a
    // kRetainAndDiagnose (non-interactive) restore never opens the modal.
    reportLayoutRestoreIssue(
        policy, "layout-progressive-apply-failed", tr("Layout was parsed but could not be applied."));
    emit layoutRestoreSettled(false);
    return;
  }

  pending_items_added_conn_ = connect(
      &session_->catalogModel(), &CatalogModel::itemsAdded, this, [this](const std::vector<CatalogItem>& items) {
        retryPendingSceneRestores(items);
        flushPendingCurveBindings(items);
      });
  // The drain gate is a waiter list: each entry names one reason to keep
  // waiting and the signal that may clear it. queueDrained can fire several
  // times between sequential batch children, so waiters are ordinary
  // connections the gate clears when it settles (which also subsumes the
  // old SingleShot special case for ordinary restores).
  addRestoreWaiter([this]() { return file_loader_->isBusy(); }, file_loader_.get(), &FileLoader::queueDrained);
  if (layoutImportBatchActive()) {
    addRestoreWaiter(
        [this]() { return layout_import_batch_ != nullptr && !layout_import_batch_->isFinished(); },
        layout_import_batch_.get(), &LayoutImportBatch::finished);
  }
  retryPendingSceneRestores({});
  flushPendingCurveBindings({});
}

void MainWindow::clearRestoreWaiters() {
  for (const RestoreWaiter& waiter : restore_waiters_) {
    QObject::disconnect(waiter.conn);
  }
  restore_waiters_.clear();
}

void MainWindow::cancelProgressiveLayoutRestore() {
  QObject::disconnect(pending_items_added_conn_);
  pending_items_added_conn_ = {};
  clearRestoreWaiters();
  if (pending_binder_ != nullptr) {
    pending_binder_->clear();
  }
  clearPendingSceneRestores();
  pending_timeline_sources_.clear();
  progressive_layout_doc_.clear();
  progressive_previous_workspace_.reset();
  progressive_layout_in_flight_ = false;
}

bool MainWindow::rollbackProgressiveWorkspace() {
  if (!progressive_previous_workspace_.has_value()) {
    return false;
  }
  CapturedWorkspace previous = *progressive_previous_workspace_;
  const std::vector<std::pair<DatasetId, QString>> live_datasets = session_->catalogModel().datasets();
  previous.timeline.tracks.erase(
      std::remove_if(
          previous.timeline.tracks.begin(), previous.timeline.tracks.end(),
          [&live_datasets](const auto& track) {
            return std::none_of(live_datasets.begin(), live_datasets.end(), [&track](const auto& live) {
              return live.first == track.dataset_id;
            });
          }),
      previous.timeline.tracks.end());
  std::vector<TimelineTrackState*> ordered_tracks;
  for (TimelineTrackState& track : previous.timeline.tracks) {
    if (track.timeline_order >= 0) {
      ordered_tracks.push_back(&track);
    }
  }
  std::sort(ordered_tracks.begin(), ordered_tracks.end(), [](const auto* lhs, const auto* rhs) {
    return lhs->timeline_order < rhs->timeline_order;
  });
  for (int slot = 0; slot < static_cast<int>(ordered_tracks.size()); ++slot) {
    ordered_tracks[static_cast<std::size_t>(slot)]->timeline_order = slot;
  }
  return restoreWorkspaceState(previous, MissingCurvePolicy::kExact, TimelineRestoreMode::kExact) ==
         RestoreResult::kApplied;
}

bool MainWindow::abortProgressiveRestore() {
  const bool rolled_back = rollbackProgressiveWorkspace();
  cancelProgressiveLayoutRestore();
  if (!rolled_back) {
    resetUndoHistory();
  }
  return rolled_back;
}

void MainWindow::flushPendingCurveBindings(const std::vector<CatalogItem>& items) {
  if (pending_binder_ == nullptr || pending_binder_->empty()) {
    return;
  }
  QSet<QString> topics;
  for (const CatalogItem& item : items) {
    if (!item.topic_name.isEmpty()) {
      topics.insert(item.topic_name);
    }
  }
  static_cast<void>(pending_binder_->flush(topics));
}

void MainWindow::collectPendingDisplayBindings(const QDomDocument& doc) {
  if (pending_binder_ == nullptr) {
    return;
  }
  QHash<QString, PlotWidget*> plots_by_state_id;
  forEachPlot([&plots_by_state_id](PlotWidget* plot) {
    if (plot != nullptr && !plot->stateId().isEmpty()) {
      plots_by_state_id.insert(plot->stateId(), plot);
    }
  });
  pending_binder_->collect(doc, plots_by_state_id);
  forEachSceneDock([this](SceneDockWidget* scene_dock) {
    for (const SceneDockWidget::PendingRestoreDemand& demand : scene_dock->pendingRestoreDemands()) {
      pending_binder_->addPendingSceneLayer(scene_dock, demand.topic_name, demand.preferred_dataset);
    }
  });
}

void MainWindow::rebuildPendingDisplayBindings(const QDomDocument& doc) {
  if (pending_binder_ == nullptr) {
    return;
  }
  collectPendingDisplayBindings(doc);
  static_cast<void>(pending_binder_->flush({}));
}

void MainWindow::schedulePendingDisplayBindingRebuild() {
  if (pending_binding_rebuild_scheduled_) {
    return;
  }
  pending_binding_rebuild_scheduled_ = true;
  QTimer::singleShot(0, this, [this]() {
    pending_binding_rebuild_scheduled_ = false;
    if (applying_state_ || progressive_layout_in_flight_) {
      return;
    }
    // Nothing staged and no plot holds an intent: skip the full-workspace
    // serialization the rebuild would otherwise pay.
    bool any_intents = pending_binder_ != nullptr && !pending_binder_->empty();
    if (!any_intents) {
      forEachPlot([&any_intents](PlotWidget* plot) {
        any_intents = any_intents || (plot != nullptr && plot->pendingCurveIntentCount() > 0);
      });
    }
    if (!any_intents) {
      forEachSceneDock([&any_intents](SceneDockWidget* scene_dock) {
        any_intents = any_intents || scene_dock->hasPendingRestoreDemands();
      });
    }
    if (any_intents) {
      rebuildPendingDisplayBindings(xmlSaveState());
    }
  });
}

MainWindow::SceneRestoreVerdict MainWindow::settleSceneRestores() {
  // One final retry so a deferred element whose identity became resolvable is
  // validated now, then fold every dock's verdict. Aggregation lives here —
  // a dock cannot see its siblings.
  retryPendingSceneRestores({});
  SceneRestoreVerdict verdict;
  forEachSceneDock([&verdict](SceneDockWidget* scene_dock) {
    verdict.failed = verdict.failed || scene_dock->workspaceRestoreFailed();
  });
  verdict.blocking_topics = unresolvedPendingSceneRestores();
  return verdict;
}

int MainWindow::retryPendingSceneRestores(const std::vector<CatalogItem>& items) {
  QSet<QString> topics;
  for (const CatalogItem& item : items) {
    if (isObjectTopic(item) && !item.topic_name.isEmpty()) {
      topics.insert(item.topic_name);
    }
  }
  if (!items.empty() && topics.isEmpty()) {
    return 0;
  }

  int restored = 0;
  forEachSceneDock([&](SceneDockWidget* scene_dock) { restored += scene_dock->retryPendingRestores(topics); });
  return restored;
}

QStringList MainWindow::unresolvedPendingSceneRestores() {
  QStringList unresolved;
  QSet<QString> seen;
  forEachSceneDock([&](SceneDockWidget* scene_dock) {
    for (const QString& topic : scene_dock->unresolvedBlockingPendingRestores()) {
      if (!topic.isEmpty() && !seen.contains(topic)) {
        seen.insert(topic);
        unresolved.push_back(topic);
      }
    }
  });
  return unresolved;
}

void MainWindow::clearPendingSceneRestores() {
  forEachSceneDock([](SceneDockWidget* scene_dock) { scene_dock->clearPendingRestores(); });
}

void MainWindow::reportLayoutRestoreIssue(MissingCurvePolicy policy, const char* id, const QString& message) {
  // D5: a kRetainAndDiagnose (non-interactive batch) restore must never open
  // a dialog from a late continuation; every other policy keeps the dialog.
  if (policy == MissingCurvePolicy::kRetainAndDiagnose) {
    emitDiagnostic(DiagnosticLevel::kWarning, "Layout", id, message);
  } else {
    MessageBox::warning(this, tr("Load Layout"), message);
  }
}

bool MainWindow::finalizeUnresolvedRestoreState(MissingCurvePolicy policy) {
  if (pending_binder_ != nullptr) {
    QStringList shown;
    QSet<QString> seen;
    for (const layout_xml::SeriesPath& path : pending_binder_->unresolved()) {
      const QString display = path.display();
      if (!display.isEmpty() && !seen.contains(display)) {
        seen.insert(display);
        shown.push_back(display);
      }
    }
    // Unresolved BLOCKING scene references join the same prompt: Remove drops
    // them (so the committed baseline never carries blocking pends — an exact
    // undo back to it must be able to succeed), Cancel aborts the restore.
    for (const QString& topic : unresolvedPendingSceneRestores()) {
      if (!topic.isEmpty() && !seen.contains(topic)) {
        seen.insert(topic);
        shown.push_back(topic);
      }
    }
    bool retain_unresolved_intents = false;
    if (!shown.isEmpty()) {
      if (policy == MissingCurvePolicy::kRetainAndDiagnose) {
        // No prompt, no dialog — name the unresolved intents in a diagnostic
        // and RETAIN them (no binder clear) so a later binding pass can still
        // resolve them once their data arrives (D5).
        emitDiagnostic(
            DiagnosticLevel::kWarning, "Layout", "layout-import-unresolved-curves",
            tr("%n layout reference(s) could not be bound after the import: %1", nullptr,
               static_cast<int>(shown.size()))
                .arg(shown.join(u", "_s)));
        retain_unresolved_intents = true;
      } else {
        switch (promptMissingCurves(shown)) {
          case MissingCurveChoice::kRemove:
            // Remaining curves were never bound (nothing to strip from live
            // widgets); blocking scene pends are dropped so they cannot poison
            // later exact snapshots.
            forEachSceneDock([](SceneDockWidget* scene_dock) { scene_dock->clearBlockingPendingRestores(); });
            break;
          case MissingCurveChoice::kCancel: {
            static_cast<void>(abortProgressiveRestore());
          }
            return false;
        }
      }
    }
    if (!retain_unresolved_intents) {
      pending_binder_->clear();
    }
  }

  broadcastTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
  const QStringList unresolved_scenes = unresolvedPendingSceneRestores();
  if (!unresolved_scenes.isEmpty()) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "scene_restore_pending",
        tr("%n scene layer(s) could not be rebound after progressive load.", nullptr,
           static_cast<int>(unresolved_scenes.size())));
  }
  clearPendingSceneRestores();
  return true;
}

void MainWindow::onProgressiveLayoutDrained() {
  // The drain-time policy was captured at beginProgressiveLayoutRestore (D5:
  // policy is data, never a late read of live batch state).
  const MissingCurvePolicy policy = progressive_missing_curve_policy_;
  // B3 / D5 retention channel: the kRetainAndDiagnose arm keeps the
  // itemsAdded connection alive past the drain so retained intents can still
  // bind when their data arrives later (main_window_layout_import_binder_test
  // pins this mid-import); every other arm tears it down here as before.
  if (policy != MissingCurvePolicy::kRetainAndDiagnose) {
    QObject::disconnect(pending_items_added_conn_);
    pending_items_added_conn_ = {};
  }

  // All file inputs now exist. Rebuild the complete saved processor graph
  // before the binder's last pass so derived curves resolve to fresh outputs.
  if (!progressive_layout_doc_.isNull() && !restoreDataProcessors(progressive_layout_doc_.documentElement())) {
    const bool rolled_back = abortProgressiveRestore();
    reportLayoutRestoreIssue(
        policy, "processor-restore-failed",
        rolled_back ? tr("A saved data processor could not be restored; the previous workspace was restored.")
                    : tr("A saved data processor could not be restored, and the previous workspace could not be fully "
                         "restored against the reloaded data. The partial layout was kept as the new undo baseline."));
    emit layoutRestoreSettled(false);
    return;
  }

  // A deferred scene identity can become available only at drain, which is
  // also when its payload/type can finally be validated. A permanently
  // rejected element is a transaction failure, not an "unresolved" warning:
  // otherwise the invalid entry is consumed and the partial scene becomes the
  // baseline. Independent of the binder's existence.
  if (settleSceneRestores().failed) {
    const bool rolled_back = abortProgressiveRestore();
    reportLayoutRestoreIssue(
        policy, "scene-restore-failed",
        rolled_back ? tr("A saved scene element was invalid; the previous workspace was restored.")
                    : tr("A saved scene element was invalid, and the previous workspace could not be fully restored "
                         "against the reloaded data. The partial layout was kept as the new undo baseline."));
    emit layoutRestoreSettled(false);
    return;
  }

  if (pending_binder_ != nullptr) {
    static_cast<void>(pending_binder_->flush({}));
    // Now that the worker has registered the reloaded datasets' source paths, apply the
    // timeline state (per-source offsets + track order) that restoreChromeAndPanels
    // could not bind mid-load — it stashed the refs in pending_timeline_sources_. This
    // MUST run before the viewport re-frame below so applySavedViewportOrZoom converts
    // the saved ABSOLUTE window with the settled offset (otherwise the async leg would
    // frame the pre-offset window, the FIX-1 bug on the progressive path).
    static_cast<void>(applyPendingTimelineState());
    // Frame each restored plot to its layout-saved window with the now-settled display
    // offset (the file has finished loading). PR #248 made the saved X ABSOLUTE and
    // applySavedViewportOrZoom converts it with the live offset, so re-applying it is
    // correct — and it pins the plot to its final window so data fills in like streaming
    // rather than the axis auto-fitting/growing. Plots with no/degenerate saved range
    // fall back to zoomOut; clear_after drops the one-shot stash. Still under the
    // in-flight gate, so onUndoableChange stays suppressed until the single snapshot below.
    forEachPlot([](PlotWidget* plot) { plot->applySavedViewportOrZoom(/*clear_after=*/true); });
  }

  // One owner for every unresolved-state teardown decision (binder intents,
  // blocking scene pends, the scene-pend clear), switching on the policy.
  if (!finalizeUnresolvedRestoreState(policy)) {
    emit layoutRestoreSettled(false);
    return;  // user cancelled at the missing-curve prompt
  }

  progressive_layout_in_flight_ = false;
  commitRestoredLayout(progressive_layout_doc_, policy);
  progressive_layout_doc_.clear();
  progressive_previous_workspace_.reset();
  // Settlement AFTER the commit: every waiter cleared (the caller's gate) and
  // the batch — if any — is finished (its waiter required exactly that), so a
  // subscriber quitting here can never tear down a still-active batch. The
  // caller retires the finished batch synchronously right after this returns.
  emit layoutRestoreSettled(true);
}

void MainWindow::commitRestoredLayout(const QDomDocument& doc, MissingCurvePolicy policy) {
  // A kept busy panel (declined confirmation, or a non-interactive restore's
  // unprompted retain) leaves the live pinned toolboxes in place; the rest of
  // the layout has already been applied by here, so the undo baseline is
  // reset either way.
  static_cast<void>(restorePinnedToolboxes(doc.documentElement(), policy));
  resetUndoHistory();
}

bool MainWindow::layoutImportBatchActive() const {
  return layout_import_batch_ != nullptr && layout_import_batch_->isActive();
}

void MainWindow::installLayoutBatchObservers() {
  // D1: MainWindow owns the observation, batch-scoped. Events for the batch
  // job's announced dataset (exact equality with activeImportDataset(); the
  // supported delivery order — D2 — guarantees the correlation is installed
  // before that dataset's began reaches the GUI thread) display under
  // kLayoutBatch ownership. Every OTHER began/progress is a concurrent
  // interactive ingest and rides the same arbitration generically — in
  // production its own host callbacks produce the identical result, so the
  // double handling is idempotent.
  SessionManager& manager = session_->sessionManager();
  layout_batch_ingest_conns_.push_back(connect(
      &manager, &SessionManager::ingestBegan, this, [this](IngestToken token, const QString& label, quint64 /*total*/) {
        const DatasetId dataset = token.dataset_id;
        const bool is_batch = layoutImportBatchActive() && layout_import_batch_->activeImportDataset() == dataset;
        if (is_batch) {
          layout_batch_strip_dataset_ = dataset;
        }
        if (file_loader_->isBusy()) {
          return;  // file loads always win the strip; the drain re-adopts
        }
        if (is_batch && ingest_strip_owner_kind_ == IngestStripOwnerKind::kInteractiveToolbox) {
          return;  // the background batch never displaces the user's import
        }
        ingest_strip_owner_kind_ =
            is_batch ? IngestStripOwnerKind::kLayoutBatch : IngestStripOwnerKind::kInteractiveToolbox;
        ingest_strip_owner_dataset_ = dataset;
        ingest_progress_->setTitle(label);
        ingest_progress_->setCounterText({});
        ingest_progress_->setRange(0, 0);  // busy until the first determinate tick
        ingest_show_timer_->start(kIngestStripShowDelayMs);
      }));
  layout_batch_ingest_conns_.push_back(connect(
      &manager, &SessionManager::ingestProgressed, this, [this](IngestToken token, quint64 current, quint64 total) {
        const DatasetId dataset = token.dataset_id;
        if (file_loader_->isBusy() || !session_->sessionManager().hasActiveIngests()) {
          return;
        }
        if (ingest_strip_owner_kind_ != IngestStripOwnerKind::kInteractiveToolbox &&
            ingest_strip_owner_kind_ != IngestStripOwnerKind::kLayoutBatch && !displaySurvivingIngestOnStrip(0)) {
          return;  // the strip was free but nothing eligible survives
        }
        if (ingest_strip_owner_dataset_ != dataset) {
          return;  // another producer owns the display; its ticks drive the bar
        }
        setIngestStripProgress(current, total);
      }));
  layout_batch_ingest_conns_.push_back(
      connect(&manager, &SessionManager::ingestEnded, this, [this](IngestToken token, IngestOutcome /*outcome*/) {
        const DatasetId dataset = token.dataset_id;
        if (dataset == layout_batch_strip_dataset_) {
          layout_batch_strip_dataset_ = 0;
        }
        releaseIngestStripOwner(dataset);
      }));
}

void MainWindow::teardownLayoutBatchObservation() {
  // D1 ordering: observers off and any batch-owned display released BEFORE
  // the batch object dies, so no lifecycle event (or strip Stop click) can
  // reach a dangling batch. The batch's SessionManager entry may outlive this
  // by a moment (its producer's teardown ends it); it is excluded from the
  // survivor pick here and, no longer classified as the batch's, behaves as a
  // plain ingest until its end arrives.
  for (const QMetaObject::Connection& conn : layout_batch_ingest_conns_) {
    QObject::disconnect(conn);
  }
  layout_batch_ingest_conns_.clear();
  layout_batch_strip_dataset_ = 0;
  if (ingest_strip_owner_kind_ == IngestStripOwnerKind::kLayoutBatch) {
    releaseIngestStripOwner(ingest_strip_owner_dataset_);
  }
}

void MainWindow::resetLayoutImportBatch() {
  // A still-running batch takes the destructor's HARD shutdown (children
  // cancelled+joined, no rollback) — this path replaces or retires it.
  teardownLayoutBatchObservation();
  layout_import_batch_.reset();
  batch_workspace_checkpoint_.reset();
  if (!progressive_layout_in_flight_) {
    // Outside a live restore, the only connection that can still be up is
    // the D5 retention channel a kRetainAndDiagnose drain left alive — it
    // ends when a new restore (or shutdown) supersedes it. A restore still
    // in flight keeps its channel: begin/cancel own it there.
    QObject::disconnect(pending_items_added_conn_);
    pending_items_added_conn_ = {};
  }
}

void MainWindow::supersedeActiveRestore() {
  // Tear down an in-flight progressive transaction COMPLETELY (waiters,
  // itemsAdded channel, binder, old document, in-flight flag), then retire
  // the batch. Invoked on EVERY new layout load and at close: a superseding
  // load that takes the SYNC path never reaches beginProgressiveLayoutRestore,
  // and a stale waiter settled by a late queueDrained from the OLD load
  // would otherwise commit the old document over the new workspace.
  if (progressive_layout_in_flight_) {
    cancelProgressiveLayoutRestore();
  }
  resetLayoutImportBatch();
}

bool MainWindow::restoreHasPendingAsyncWork(bool reload_requested) const {
  // §6.2 "a pending import keeps the restore alive": a provider job is
  // invisible to FileLoader's queue, so the batch is its own source of
  // pending async work. BOTH halves are gated on reload_requested — only the
  // do_reload branch supersedes the previous restore before enqueuing, so
  // only there does layout_import_batch_ provably belong to THIS load; on
  // the non-reload path the member may still be a superseded predecessor's
  // batch, and this load must fall through to the sync apply (which
  // supersedes right before mutating).
  return reload_requested && (file_loader_->isBusy() || layoutImportBatchActive());
}

void MainWindow::maybeSettleProgressiveRestore() {
  if (!progressive_layout_in_flight_) {
    return;
  }
  // The gate: every registered waiter must have cleared. Each waiter's
  // signal re-runs this check.
  for (const RestoreWaiter& waiter : restore_waiters_) {
    if (waiter.pending && waiter.pending()) {
      return;
    }
  }
  // The cancelled-batch unwind is genuinely batch-specific, so it stays
  // inline: the batch's rollback already removed its produced datasets and
  // exact-restored the PRE-LAYOUT workspace; drop the progressive
  // scaffolding without committing (and without a second rollback).
  if (layout_import_batch_ != nullptr && layout_import_batch_->result().cancelled) {
    cancelProgressiveLayoutRestore();
    resetLayoutImportBatch();
    emitDiagnostic(
        DiagnosticLevel::kInfo, "Layout", "layout-import-cancelled",
        tr("Layout import was cancelled; the previous workspace was restored."));
    emit layoutRestoreSettled(false);
    return;
  }
  clearRestoreWaiters();
  onProgressiveLayoutDrained();
  // The batch is retired here — its policy already lives in
  // progressive_missing_curve_policy_, so nothing at (or after) the drain
  // reads it. Observation teardown first (D1: observers/ownership never
  // outlive the batch). The D5 retention channel (pending_items_added_conn_)
  // deliberately survives a kRetainAndDiagnose drain; a plain
  // layout_import_batch_.reset() leaves it untouched.
  teardownLayoutBatchObservation();
  layout_import_batch_.reset();
  batch_workspace_checkpoint_.reset();
}

std::unique_ptr<LayoutImportBatch> MainWindow::makeLayoutImportBatch(bool interactive) {
  LayoutImportBatch::Hooks hooks;
  hooks.begin_workspace_checkpoint = [this]() -> std::function<bool()> {
    // ONE capture serves both rollback surfaces: the closure below (the
    // batch's cancel rollback) and beginProgressiveLayoutRestore, which
    // reuses the handover instead of re-capturing (a full DOM build +
    // serialization on the GUI thread). The batch cannot name the private
    // CapturedWorkspace type, so the capture lives in shared closure state.
    auto captured = std::make_shared<const CapturedWorkspace>(capturePortableWorkspace());
    batch_workspace_checkpoint_ = captured;
    return [this, captured]() {
      return restoreWorkspaceState(*captured, MissingCurvePolicy::kExact, TimelineRestoreMode::kExact) ==
             RestoreResult::kApplied;
    };
  };
  hooks.remove_dataset = [this](DatasetId dataset_id) {
    // The un-import primitive, guarded by liveness (O(1)): a provider-
    // reported id that never materialized (or was already removed) is a no-op.
    if (session_->catalogModel().datasetSourceName(dataset_id).has_value()) {
      removeDatasetData(dataset_id);
    }
  };
  if (interactive) {
    hooks.confirm_import = [this](const QStringList& lines) {
      // The consolidated trust/size confirmation — ONE prompt for the whole
      // batch, shaped like the 3-way layout reload prompt above.
      constexpr int kTrust = 0;
      constexpr int kSkip = 1;
      QStringList shown;
      shown.reserve(lines.size());
      for (const QString& line : lines) {
        shown.push_back(u"  %1"_s.arg(line));
      }
      // §7 guard 1 requires EXPLICIT confirmation of an untrusted origin, so
      // "Trust and import" deliberately does NOT take the primary role: only
      // kPrimaryRole becomes the Enter-default (MessageBox::addButton), and
      // the preceding reload prompt's Enter-default would otherwise chain
      // straight into granting a credentialed network fetch (Enter-Enter).
      // Same shape as promptMissingCurves: no default button; Esc = Cancel.
      const int choice = MessageBox::question(
          this, tr("Load Layout"),
          tr("This layout asks to download %n data source(s) from a cloud provider:\n\n%1", nullptr,
             static_cast<int>(lines.size()))
              .arg(shown.join(QLatin1Char('\n'))),
          {{tr("Trust and import"), MessageBox::kNeutralRole},
           {tr("Skip these sources"), MessageBox::kNeutralRole},
           {tr("Cancel layout load"), MessageBox::kCancelRole}});
      if (choice == kTrust) {
        return LayoutImportBatch::TrustChoice::kTrustAndImport;
      }
      if (choice == kSkip) {
        return LayoutImportBatch::TrustChoice::kSkip;
      }
      return LayoutImportBatch::TrustChoice::kCancelLayout;
    };
  }
  hooks.diagnostics = diagnosticSink();
  // §7 guard 3's per-machine download ceiling. 0 = no ceiling for now — the
  // batch mechanism honors any non-zero value (refuse/confirm at query time
  // + the per-job max_transfer_bytes enforcement channel); the configurable
  // cap is future preferences work.
  constexpr std::uint64_t kLayoutImportMaxTransferBytes = 0;
#ifdef __EMSCRIPTEN__
  SourceCaptureService* capture_service = nullptr;  // no on-disk cache in the browser
#else
  SourceCaptureService* capture_service = source_capture_service_.get();
#endif
  return std::make_unique<LayoutImportBatch>(
      session_->sessionManager(), session_->extensionCatalog(), *file_loader_, session_->catalogModel(),
      capture_service, interactive, kLayoutImportMaxTransferBytes, std::move(hooks));
}

void MainWindow::saveLayoutToPath(const QString& path, bool include_data_source) {
  QDomDocument doc = xmlSaveState();
  // Record the binding intent so load knows whether to reload the original
  // file (source-bound) or adopt the currently-loaded dataset (generic). The
  // data-source element below is only embedded for source-bound layouts.
  doc.documentElement().setAttribute(u"binding"_s, include_data_source ? u"source"_s : u"generic"_s);
  if (include_data_source) {
    const QDir layout_dir(QFileInfo(path).absoluteDir());
    QDomElement ds = appendDataSourceElement(doc, layout_dir);
    if (!ds.isNull()) {
      doc.documentElement().appendChild(ds);
    }
    // Widget serializers know DatasetIds but intentionally know nothing about
    // FileLoader. Stamp one coherent full-path identity across plots (and any
    // future processor/scene qualifiers) as a post-pass. Use the same relocatable
    // subpath rule as <fileInfo>; load resolves it before binding.
    layout_xml::stampDatasetSourcePaths(doc, [this, &layout_dir](std::uint32_t dataset_id) {
      const QString source_path = file_loader_->sourcePathForDataset(dataset_id);
      if (source_path.isEmpty()) {
        return QString{};
      }
      return relocatableSubpath(QFileInfo(source_path).absoluteFilePath(), layout_dir);
    });
    // A non-file dataset has no path with which a future session can validate a
    // numeric id. Keep its source fallback but drop that volatile id so a
    // duplicate basename cannot silently capture the persisted layout.
    layout_xml::removeUnvalidatedDatasetIds(doc);
  } else {
    // A generic layout is intentionally reusable on another recording. Keep exact
    // DatasetId/source qualifiers in undo and source-bound documents, but strip
    // them from this file copy so topic+field paths bind — only when unambiguous —
    // to whatever data is present when the layout is opened.
    layout_xml::removeDatasetQualifiersForGenericLayout(doc);
  }
  // Always save right-panel state — pure UI chrome, no privacy cost,
  // not gated by Save Data Source.
  doc.documentElement().appendChild(saveRightPanelState(doc));
  // Always save the remaining widget state — pure UI chrome, not gated
  // by Save Data Source.
  doc.documentElement().appendChild(ui_->leftPanel->saveSourcesState(doc));
  doc.documentElement().appendChild(ui_->curveListPanel->saveListState(doc));
  doc.documentElement().appendChild(saveChromeState(doc));
  // NOTE: <data_processors> is already emitted by xmlSaveState() (it is part of THE
  // snapshot now, shared with undo/redo); do not append it again here or the layout
  // would carry two copies.
  // Timeline view chrome (zoom/scroll/name-column/snap). Pure UI, not gated by
  // Save Data Source; the per-source offsets/order ride <fileInfo> separately.
  doc.documentElement().appendChild(saveSourceTimelineViewState(doc));
  // Pinned toolbox tabs (plugin id + config). Layout-only: undo snapshots
  // deliberately exclude them (see TabbedPlotWidget::xmlSaveState), so this
  // element rides the layout file, not xmlSaveState().
  doc.documentElement().appendChild(savePinnedToolboxes(doc));
  // QSaveFile gives us write-temp + rename atomicity: a partial write
  // (disk full, signal, broken NFS) leaves the user's prior layout
  // untouched. commit() does the rename; cancelWriting() abandons the
  // tempfile. A QFile::Truncate path would have destroyed prior state
  // before completing the write.
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    MessageBox::warning(this, tr("Save Layout"), tr("Cannot open '%1' for writing.").arg(path));
    return;
  }
  const QByteArray bytes = doc.toByteArray(2);
  if (file.write(bytes) != bytes.size()) {
    const QString err = file.errorString();
    file.cancelWriting();
    MessageBox::warning(this, tr("Save Layout"), tr("Failed to write layout to '%1': %2").arg(path, err));
    return;
  }
  if (!file.commit()) {
    MessageBox::warning(
        this, tr("Save Layout"), tr("Failed to finalize layout '%1': %2").arg(path, file.errorString()));
    return;
  }
  recordRecentLayout(path);
  emitDiagnostic(DiagnosticLevel::kInfo, "Layout", "saved", tr("Saved layout: %1").arg(QFileInfo(path).fileName()));
}

QDomElement MainWindow::saveDataProcessors(QDomDocument& doc) const {
  QDomElement element = doc.createElement(u"data_processors"_s);
  for (const auto& recipe : session_->sessionManager().dataProcessorService().recipes()) {
    // Resolve the input column to a stable (topic, field) path so it rebinds on
    // reload exactly like a plotted curve.
    const QString input_key = u"dataset:%1/topic:%2/column:%3"_s.arg(recipe.dataset_id)
                                  .arg(recipe.input_topic_id)
                                  .arg(recipe.input_column_index);
    const std::optional<CurveDescriptor> input_desc = session_->catalogModel().curveDescriptor(input_key);
    if (!input_desc.has_value()) {
      continue;  // input field is gone; nothing to persist
    }
    QDomElement processor = doc.createElement(u"processor"_s);
    processor.setAttribute(u"input_topic"_s, input_desc->topic_name);
    processor.setAttribute(u"input_field"_s, input_desc->field_path);
    // Dataset qualifiers so the input rebinds to its own source dataset on restore,
    // never a same-topic sibling (first-match was part of the reported undo bug).
    // input_dataset_path is stamped by stampDatasetSourcePaths at layout-file save.
    processor.setAttribute(u"input_dataset_id"_s, QString::number(recipe.dataset_id));
    if (const auto source = session_->catalogModel().datasetSourceName(recipe.dataset_id); source.has_value()) {
      processor.setAttribute(u"input_dataset_source"_s, *source);
    }
    processor.setAttribute(u"processor_id"_s, QString::fromStdString(recipe.processor_id));
    processor.setAttribute(u"output_name"_s, QString::fromStdString(recipe.output_name));
    if (recipe.processor) {
      layout_xml::appendJsonAsCdata(doc, processor, QString::fromStdString(recipe.processor->saveParams()));
    }
    // Embed the filter's Luau source so the layout reopens on a machine without
    // this filter installed (resolved by restore's source_fallback leg). A named
    // child element, NOT a second direct CDATA child — restore reads params via
    // directCdataText, which ignores child elements. Empty for native C++ builtins.
    if (!recipe.filter_source.empty()) {
      QDomElement source_el = doc.createElement(u"source_fallback"_s);
      layout_xml::appendJsonAsCdata(doc, source_el, QString::fromStdString(recipe.filter_source));
      processor.appendChild(source_el);
    }
    element.appendChild(processor);
  }
  // Plugin-created transforms (pj.data_processors.v1): named, owner-tagged nodes
  // that carry their script + bindings BY VALUE, so they replay on load WITHOUT the
  // originating plugin (needed only for re-editing). Persisted in the same snapshot
  // block as the per-curve filters above; restore re-installs them via
  // DataProcessorService::restoreTransform. Inputs/outputs are topic NAMES (the
  // engine resolves them on restore), so no (topic, field) rebinding is needed here.
  for (const auto& recipe : session_->sessionManager().dataProcessorService().transformRecipes()) {
    QDomElement transform = doc.createElement(u"transform"_s);
    transform.setAttribute(u"owner_plugin"_s, QString::fromStdString(recipe.owner_plugin));
    transform.setAttribute(u"id"_s, QString::fromStdString(recipe.user_id));
    transform.setAttribute(u"backend"_s, QString::fromStdString(recipe.backend));
    transform.setAttribute(u"api_version"_s, QString::fromStdString(recipe.api_version));
    if (!recipe.backend_version.empty()) {
      transform.setAttribute(u"backend_version"_s, QString::fromStdString(recipe.backend_version));
    }
    for (std::size_t index = 0; index < recipe.inputs.size(); ++index) {
      const std::string& input_name = recipe.inputs[index];
      QDomElement in = doc.createElement(u"input"_s);
      in.setAttribute(u"name"_s, QString::fromStdString(input_name));
      if (index < recipe.input_bindings.size()) {
        const auto& binding = recipe.input_bindings[index];
        in.setAttribute(u"dataset_id"_s, QString::number(binding.dataset_id));
        in.setAttribute(u"dataset_source"_s, QString::fromStdString(binding.dataset_source));
        in.setAttribute(u"topic"_s, QString::fromStdString(binding.topic_name));
        in.setAttribute(u"field"_s, QString::fromStdString(binding.field_path));
        in.setAttribute(u"column"_s, QString::number(static_cast<qulonglong>(binding.column_index)));
      }
      transform.appendChild(in);
    }
    for (const auto& output_name : recipe.outputs) {
      QDomElement out = doc.createElement(u"output"_s);
      out.setAttribute(u"name"_s, QString::fromStdString(output_name));
      transform.appendChild(out);
    }
    // params (create(params), JSON) and script (the full backend payload, Luau today)
    // each get their own CDATA child so restore reads them back independently.
    if (!recipe.params_json.empty()) {
      QDomElement params = doc.createElement(u"params"_s);
      layout_xml::appendJsonAsCdata(doc, params, QString::fromStdString(recipe.params_json));
      transform.appendChild(params);
    }
    QDomElement script = doc.createElement(u"script"_s);
    layout_xml::appendJsonAsCdata(doc, script, QString::fromStdString(recipe.script));
    transform.appendChild(script);
    element.appendChild(transform);
  }

  // Marker generators (kind=markers of pj.data_processors.v1): whole-series object
  // generators run by MarkerService. Persisted like transforms — script + inputs
  // BY VALUE so they replay on load without the originating plugin. Dataset identity
  // is stamped for cross-session rebind (mirrors the <processor> qualifiers); an
  // all_datasets generator has no single dataset, so it stamps none.
  for (const auto& recipe : session_->sessionManager().markerService().recipes()) {
    QDomElement gen = doc.createElement(u"generator"_s);
    gen.setAttribute(u"id"_s, QString::fromStdString(recipe.id));
    gen.setAttribute(u"language"_s, QString::fromStdString(recipe.language));
    gen.setAttribute(u"all_datasets"_s, recipe.all_datasets ? u"1"_s : u"0"_s);
    if (!recipe.all_datasets) {
      gen.setAttribute(u"dataset_id"_s, QString::number(recipe.dataset_id));
      if (const auto source = session_->catalogModel().datasetSourceName(recipe.dataset_id); source.has_value()) {
        gen.setAttribute(u"dataset_source"_s, *source);
      }
      gen.setAttribute(u"dataset_path"_s, session_->sessionManager().datasetSourcePath(recipe.dataset_id));
    }
    for (const auto& input_name : recipe.inputs) {
      QDomElement in = doc.createElement(u"input"_s);
      in.setAttribute(u"name"_s, QString::fromStdString(input_name));
      gen.appendChild(in);
    }
    for (const auto& output_name : recipe.outputs) {
      QDomElement out = doc.createElement(u"output"_s);
      out.setAttribute(u"name"_s, QString::fromStdString(output_name));
      gen.appendChild(out);
    }
    if (!recipe.params_json.empty()) {
      QDomElement params = doc.createElement(u"params"_s);
      layout_xml::appendJsonAsCdata(doc, params, QString::fromStdString(recipe.params_json));
      gen.appendChild(params);
    }
    QDomElement gen_script = doc.createElement(u"script"_s);
    layout_xml::appendJsonAsCdata(doc, gen_script, QString::fromStdString(recipe.script));
    gen.appendChild(gen_script);
    element.appendChild(gen);
  }
  return element;
}

bool MainWindow::restoreDataProcessors(const QDomElement& root) {
  // Reconcile the live filter set to this snapshot: drop ALL current filters first,
  // then recreate the snapshot's set. This makes restore idempotent for undo/redo (no
  // duplicate or output-name-colliding filters) and correct for a layout load onto an
  // existing session. A snapshot with NO <data_processors> still falls through to clear
  // every filter; the rebuild at the end MUST run on both branches so the now-retired
  // outputs leave the catalog.
  auto& service = session_->sessionManager().dataProcessorService();
  bool restored_all = true;
  service.clearAllFilters();

  const QDomElement element = root.firstChildElement(u"data_processors"_s);
  // A null <data_processors> yields a null firstChildElement, so this loop runs zero
  // times — the clear above is then the whole effect.
  for (QDomElement processor = element.firstChildElement(u"processor"_s); !processor.isNull();
       processor = processor.nextSiblingElement(u"processor"_s)) {
    const QString input_topic = processor.attribute(u"input_topic"_s);
    const QString input_field = processor.attribute(u"input_field"_s);
    // Resolve the input through the same dataset-qualified path a plotted curve
    // uses: the exact source dataset when its qualifiers still agree, a unique
    // fallback otherwise, and never a same-topic sibling by load order.
    const layout_xml::SeriesPath input_path{
        input_topic, input_field, static_cast<DatasetId>(processor.attribute(u"input_dataset_id"_s).toUInt()),
        processor.attribute(u"input_dataset_source"_s), processor.attribute(u"input_dataset_path"_s)};
    std::optional<CurveDescriptor> input_desc;
    if (const auto input_key = resolveSeriesPath(session_->catalogModel(), input_path); input_key.has_value()) {
      input_desc = session_->catalogModel().curveDescriptor(*input_key);
    }
    if (!input_desc.has_value()) {
      // The filter's source signal isn't in any loaded dataset -> the filter is
      // dropped. Tell the user which one, mirroring the unknown/apply-failed cases
      // below (otherwise a derived series silently vanishes on layout load).
      emitDiagnostic(
          DiagnosticLevel::kWarning, "Layout", "processor-input-missing",
          tr("Layout filter on '%1/%2' has no matching data; skipping.").arg(input_topic, input_field));
      restored_all = false;
      continue;
    }
    const std::string id = processor.attribute(u"processor_id"_s).toStdString();
    // Params are the <processor>'s OWN direct CDATA — directCdataText ignores the
    // <source_fallback> child (QDomElement::text() would recurse and merge them).
    const QString params = layout_xml::directCdataText(processor);
    const QString source_fallback = processor.firstChildElement(u"source_fallback"_s).text();
    // Resolve order: live catalogue → embedded source → transitional C++ builtin.
    std::unique_ptr<proc::DataProcessor> built = service.makeRestoredProcessor(
        id, params.isEmpty() ? std::string("{}") : params.toStdString(), source_fallback.toStdString());
    if (!built) {
      emitDiagnostic(
          DiagnosticLevel::kWarning, "Layout", "processor-unknown",
          tr("Layout filter '%1' is unknown to this PlotJuggler; skipping.").arg(QString::fromStdString(id)));
      restored_all = false;
      continue;
    }
    const auto applied = service.applyFilter(
        input_desc->topic_id, input_desc->dataset_id, std::move(built),
        processor.attribute(u"output_name"_s).toStdString(), input_desc->column_index);
    if (!applied.has_value()) {
      emitDiagnostic(
          DiagnosticLevel::kWarning, "Layout", "processor-apply-failed",
          tr("Could not restore filter: %1").arg(QString::fromStdString(applied.error())));
      restored_all = false;
    }
  }

  // Reconcile plugin transforms the same way (clear-all, then replay the snapshot's
  // set) so restore is idempotent for undo/redo and a layout load can't duplicate a
  // transform. Done AFTER filters so a transform whose input is a filter output can
  // resolve it by name. The clear runs unconditionally: a snapshot with no
  // <transform> children then simply leaves every transform torn down.
  service.clearAllTransforms();
  for (QDomElement transform = element.firstChildElement(u"transform"_s); !transform.isNull();
       transform = transform.nextSiblingElement(u"transform"_s)) {
    DataProcessorService::TransformRecipe recipe;
    QString transform_parse_error;
    recipe.owner_plugin = transform.attribute(u"owner_plugin"_s).toStdString();
    recipe.user_id = transform.attribute(u"id"_s).toStdString();
    recipe.key = DataProcessorService::makeTransformKey(recipe.owner_plugin, recipe.user_id);
    recipe.backend = transform.attribute(u"backend"_s, u"luau"_s).toStdString();
    recipe.api_version = transform.attribute(u"api_version"_s, u"1"_s).toStdString();
    recipe.backend_version = transform.attribute(u"backend_version"_s).toStdString();
    std::vector<DataProcessorService::TransformInputBinding> parsed_bindings;
    bool has_persisted_binding = false;
    for (QDomElement in = transform.firstChildElement(u"input"_s); !in.isNull();
         in = in.nextSiblingElement(u"input"_s)) {
      recipe.inputs.push_back(in.attribute(u"name"_s).toStdString());
      DataProcessorService::TransformInputBinding binding;
      const bool has_binding = in.hasAttribute(u"dataset_id"_s) || in.hasAttribute(u"dataset_source"_s) ||
                               in.hasAttribute(u"dataset_path"_s) || in.hasAttribute(u"topic"_s) ||
                               in.hasAttribute(u"field"_s) || in.hasAttribute(u"column"_s);
      has_persisted_binding = has_persisted_binding || has_binding;
      if (in.hasAttribute(u"dataset_id"_s)) {
        bool ok = false;
        const qulonglong raw = in.attribute(u"dataset_id"_s).toULongLong(&ok);
        if (ok && raw <= std::numeric_limits<DatasetId>::max()) {
          binding.dataset_id = static_cast<DatasetId>(raw);
        } else {
          transform_parse_error = tr("invalid input dataset id");
        }
      }
      binding.dataset_source = in.attribute(u"dataset_source"_s).toStdString();
      binding.topic_name = in.attribute(u"topic"_s).toStdString();
      binding.field_path = in.attribute(u"field"_s).toStdString();
      const QString saved_dataset_path = in.attribute(u"dataset_path"_s);
      if (!saved_dataset_path.isEmpty()) {
        const DatasetIdentityResolution resolved = session_->sessionManager().resolveDatasetIdentity(
            binding.dataset_id, QString::fromStdString(binding.dataset_source), saved_dataset_path);
        if (resolved.id.has_value()) {
          binding.dataset_id = *resolved.id;
          if (const DatasetInfo* live = session_->sessionManager().dataEngine().getDataset(*resolved.id)) {
            binding.dataset_source = live->source_name;
          }
        } else {
          transform_parse_error =
              resolved.ambiguous ? tr("ambiguous input dataset path") : tr("input dataset path is not loaded");
        }
      }
      if (in.hasAttribute(u"column"_s)) {
        bool ok = false;
        const qulonglong raw = in.attribute(u"column"_s).toULongLong(&ok);
        if (ok && raw <= std::numeric_limits<std::size_t>::max()) {
          binding.column_index = static_cast<std::size_t>(raw);
        } else {
          transform_parse_error = tr("invalid input column");
        }
      }
      parsed_bindings.push_back(std::move(binding));
    }
    if (has_persisted_binding) {
      recipe.input_bindings = std::move(parsed_bindings);
    }
    for (QDomElement out = transform.firstChildElement(u"output"_s); !out.isNull();
         out = out.nextSiblingElement(u"output"_s)) {
      recipe.outputs.push_back(out.attribute(u"name"_s).toStdString());
    }
    recipe.params_json = layout_xml::directCdataText(transform.firstChildElement(u"params"_s)).toStdString();
    if (recipe.params_json.empty()) {
      recipe.params_json = "{}";
    }
    recipe.script = layout_xml::directCdataText(transform.firstChildElement(u"script"_s)).toStdString();
    if (!transform_parse_error.isEmpty()) {
      emitDiagnostic(
          DiagnosticLevel::kWarning, "Layout", "transform-restore-failed",
          tr("Could not restore transform '%1': %2").arg(QString::fromStdString(recipe.key), transform_parse_error));
      restored_all = false;
      continue;
    }
    if (const auto restored = service.restoreTransform(recipe); !restored.has_value()) {
      emitDiagnostic(
          DiagnosticLevel::kWarning, "Layout", "transform-restore-failed",
          tr("Could not restore transform '%1': %2")
              .arg(QString::fromStdString(recipe.key), QString::fromStdString(restored.error())));
      restored_all = false;
    }
  }

  // Marker generators: clear-all + replay, same as transforms. Done after them so a
  // generator reading a transform/filter output resolves it by name. upsertGenerator
  // re-creates AND re-runs each in one call, so no separate recompute is needed.
  MarkerService& marker_service = session_->sessionManager().markerService();
  marker_service.clearAllGenerators();
  for (QDomElement gen = element.firstChildElement(u"generator"_s); !gen.isNull();
       gen = gen.nextSiblingElement(u"generator"_s)) {
    MarkerService::GeneratorRecipe recipe;
    recipe.id = gen.attribute(u"id"_s).toStdString();
    recipe.kind = GeneratorKind::kMarkers;
    recipe.language = gen.attribute(u"language"_s, u"luau"_s).toStdString();
    recipe.all_datasets = gen.attribute(u"all_datasets"_s) == u"1"_s;
    QString gen_parse_error;
    if (!recipe.all_datasets) {
      bool ok = false;
      const qulonglong raw = gen.attribute(u"dataset_id"_s).toULongLong(&ok);
      if (ok && raw <= std::numeric_limits<DatasetId>::max()) {
        recipe.dataset_id = static_cast<DatasetId>(raw);
      }
      const QString saved_path = gen.attribute(u"dataset_path"_s);
      if (!saved_path.isEmpty()) {
        const DatasetIdentityResolution resolved = session_->sessionManager().resolveDatasetIdentity(
            recipe.dataset_id, gen.attribute(u"dataset_source"_s), saved_path);
        if (resolved.id.has_value()) {
          recipe.dataset_id = *resolved.id;
        } else {
          gen_parse_error = resolved.ambiguous ? tr("ambiguous dataset path") : tr("dataset path is not loaded");
        }
      }
    }
    for (QDomElement in = gen.firstChildElement(u"input"_s); !in.isNull(); in = in.nextSiblingElement(u"input"_s)) {
      recipe.inputs.push_back(in.attribute(u"name"_s).toStdString());
    }
    for (QDomElement out = gen.firstChildElement(u"output"_s); !out.isNull();
         out = out.nextSiblingElement(u"output"_s)) {
      recipe.outputs.push_back(out.attribute(u"name"_s).toStdString());
    }
    recipe.params_json = layout_xml::directCdataText(gen.firstChildElement(u"params"_s)).toStdString();
    recipe.script = layout_xml::directCdataText(gen.firstChildElement(u"script"_s)).toStdString();
    // A generator that cannot be restored is reported and skipped, but does NOT fail
    // the restore: unlike a filter or transform, it publishes only overlay decoration
    // — no topic that a curve, a plot or a scene binds to. Failing the restore here
    // costs the user the whole workspace (applyWorkspace rolls back) to save a set of
    // annotations, and a rule whose input series is legitimately absent in the data
    // this layout was reopened against is an ordinary outcome, not a corrupt layout.
    if (!gen_parse_error.isEmpty()) {
      emitDiagnostic(
          DiagnosticLevel::kWarning, "Layout", "generator-restore-failed",
          tr("Could not restore marker generator '%1': %2").arg(QString::fromStdString(recipe.id), gen_parse_error));
      continue;
    }
    if (const auto restored = marker_service.upsertGenerator(recipe); !restored.has_value()) {
      emitDiagnostic(
          DiagnosticLevel::kWarning, "Layout", "generator-restore-failed",
          tr("Could not restore marker generator '%1': %2")
              .arg(QString::fromStdString(recipe.id), QString::fromStdString(restored.error())));
    }
  }
  session_->catalogModel().rebuildFromDatastore();
  return restored_all;
}

void MainWindow::recordRecentLayout(const QString& path) {
  QStringList recent = recentLayouts();
  recent.removeAll(path);  // dedupe — most-recent-first
  recent.prepend(path);
  while (recent.size() > kMaxRecentEntries) {
    recent.removeLast();
  }
  QSettings().setValue(kRecentLayoutsKey, recent);
  // A recorded layout populates the popup's Layouts section, so light up the
  // recent button even when no data file has been loaded yet.
  ui_->leftPanel->setRecentEnabled(true);
}

QStringList MainWindow::recentLayouts() const {
  return QSettings().value(kRecentLayoutsKey).toStringList();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  const QEvent::Type type = event->type();
  // QTipLabel is the internal widget Qt creates for tooltips. On Linux
  // KWin / Mutter normally paint a drop shadow around any tool/popup
  // window; setting Qt::NoDropShadowWindowHint asks the platform plugin
  // to suppress it. Polish fires once before the first show and the
  // QTipLabel is reused for all tooltips, so a single tweak covers
  // every subsequent tooltip.
  if (type == QEvent::Polish && watched->inherits("QTipLabel")) {
    if (auto* w = qobject_cast<QWidget*>(watched)) {
      w->setWindowFlag(Qt::NoDropShadowWindowHint, true);
    }
    return false;
  }
  // The align rail sits in the window's bottom-right corner, where the default
  // tooltip placement needs an upward flip that fails under Wayland. Show the
  // rail's tooltips ourselves, to the left of the rail and clamped on-screen.
  if (type == QEvent::ToolTip) {
    auto* btn = qobject_cast<QToolButton*>(watched);
    if (btn != nullptr && ui_->timelineAlignRail != nullptr && btn->parentWidget() == ui_->timelineAlignRail &&
        !btn->toolTip().isEmpty()) {
      const QFontMetrics fm(QToolTip::font());
      const QSize tip_size = fm.size(Qt::TextSingleLine, btn->toolTip()) + QSize(12, 8);  // ~QTipLabel chrome
      const QPoint btn_top_left = btn->mapToGlobal(QPoint(0, 0));
      QPoint pos(
          btn_top_left.x() - tip_size.width() - 4,                      // just left of the rail
          btn_top_left.y() + (btn->height() - tip_size.height()) / 2);  // vertically centered on the button
      if (const QScreen* screen = btn->screen()) {
        const QRect avail = screen->availableGeometry();
        pos.setX(std::clamp(pos.x(), avail.left() + 4, avail.right() - tip_size.width() - 4));
        pos.setY(std::clamp(pos.y(), avail.top() + 4, avail.bottom() - tip_size.height() - 4));
      }
      QToolTip::showText(pos, btn->toolTip(), btn);
      return true;
    }
    return false;
  }
  if (type != QEvent::MouseMove && type != QEvent::MouseButtonPress) {
    return false;
  }
  // Only act on mouse events that target a widget in this window.
  auto* widget = qobject_cast<QWidget*>(watched);
  if (widget == nullptr || widget->window() != this) {
    return false;
  }
  if (isMaximized() || isFullScreen()) {
    return false;
  }

  auto* mouse_event = static_cast<QMouseEvent*>(event);
  const QPoint window_pos = mapFromGlobal(mouse_event->globalPosition().toPoint());
  const Qt::Edges edges = edgesAtPoint(size(), window_pos);

  if (type == QEvent::MouseMove) {
    if (edges != 0) {
      setCursor(cursorForEdges(edges));
    } else {
      unsetCursor();
    }
    return false;
  }

  // MouseButtonPress
  if (mouse_event->button() != Qt::LeftButton || edges == 0) {
    return false;
  }
  if (auto* handle = windowHandle()) {
    handle->startSystemResize(edges);
    return true;
  }
  return false;
}

void MainWindow::onUndoableChange(bool force_new_state) {
  if (applying_state_ || progressive_layout_in_flight_) {
    return;
  }
  pushUndoState(force_new_state);
}

QList<layout_xml::SeriesPath> MainWindow::rebindCurvesToLoadedDatasets(QDomDocument& doc) {
  // Resolve each curve's stable topic+field against whichever loaded dataset
  // actually holds it (first match in load order). Shared by layout load and
  // undo/redo restore so both bind curves identically; returns the paths no
  // loaded dataset could provide (the caller decides whether to prompt).
  return layout_xml::rebindCurveKeys(
      doc, [this](const layout_xml::SeriesPath& p) { return resolveSeriesPath(session_->catalogModel(), p); });
}

MainWindow::TimelineChromeState MainWindow::captureTimelineChrome() const {
  TimelineChromeState state;
  if (source_timeline_ != nullptr) {
    state.zoom = source_timeline_->zoom();
    state.scroll_left_ns = source_timeline_->viewportLeftDisplayNs();
    state.scroll_top_px = source_timeline_->viewportTopOffsetPx();
    state.name_column_width = source_timeline_->nameColumnWidth();
    state.snap = source_timeline_->snapEnabled();
  }
  return state;
}

void MainWindow::applyTimelineChrome(const TimelineChromeState& state) {
  if (source_timeline_ != nullptr) {
    source_timeline_->setZoom(state.zoom);
    source_timeline_->setViewportLeftDisplayNs(state.scroll_left_ns);
    timeline_name_column_width_ = state.name_column_width;
    source_timeline_->resizeNameColumn(state.name_column_width);
    source_timeline_->setViewportTopOffsetPx(state.scroll_top_px);
  }
  auto* snap = ui_->timelineAlignRail != nullptr
                   ? ui_->timelineAlignRail->findChild<QToolButton*>(u"buttonTimelineSnap"_s)
                   : nullptr;
  if (snap != nullptr) {
    snap->setChecked(state.snap);
  } else if (source_timeline_ != nullptr) {
    source_timeline_->setSnapEnabled(state.snap);
  }
}

MainWindow::TimelineState MainWindow::captureTimelineState() const {
  const TimelineChromeState chrome = captureTimelineChrome();
  TimelineState state;
  state.zoom = chrome.zoom;
  state.scroll_left_ns = chrome.scroll_left_ns;
  state.scroll_top_px = chrome.scroll_top_px;
  state.name_column_width = chrome.name_column_width;
  state.snap = chrome.snap;

  QHash<DatasetId, int> order_slots;
  if (source_timeline_controller_ != nullptr) {
    const std::vector<DatasetId> order = source_timeline_controller_->currentTrackOrder();
    for (int slot = 0; slot < static_cast<int>(order.size()); ++slot) {
      order_slots.insert(order[static_cast<std::size_t>(slot)], slot);
    }
  }
  const auto datasets = session_->catalogModel().datasets();
  QHash<QString, int> source_ordinals;
  for (const auto& [dataset_id, label] : datasets) {
    (void)label;
    TimelineTrackState track;
    track.dataset_id = dataset_id;
    track.display_offset_ns = session_->sessionManager().sourceDisplayOffset(dataset_id).value.count();
    track.timeline_order = order_slots.value(dataset_id, -1);
    track.source_path = session_->sessionManager().datasetSourcePath(dataset_id);
    track.source_name = session_->catalogModel().datasetSourceName(dataset_id).value_or(QString{});
    if (!track.source_path.isEmpty()) {
      const QString canonical_path = QFileInfo(track.source_path).canonicalFilePath();
      if (!canonical_path.isEmpty()) {
        track.source_index = source_ordinals.value(canonical_path, 0);
        source_ordinals[canonical_path] = track.source_index + 1;
      }
    }
    state.tracks.push_back(std::move(track));
  }
  return state;
}

MainWindow::CapturedWorkspace MainWindow::captureWorkspace() const {
  return CapturedWorkspace{.xml = xmlSaveState().toByteArray(2), .timeline = captureTimelineState()};
}

MainWindow::CapturedWorkspace MainWindow::capturePortableWorkspace(
    DatasetId stamp_override_id, const QString& stamp_override_path) const {
  QDomDocument doc = xmlSaveState();
  const TimelineState timeline = captureTimelineState();
  const QByteArray unstamped = doc.toByteArray(2);
  layout_xml::stampDatasetSourcePaths(doc, [this, stamp_override_id, &stamp_override_path](std::uint32_t dataset_id) {
    if (stamp_override_id != 0 && static_cast<DatasetId>(dataset_id) == stamp_override_id) {
      return stamp_override_path;
    }
    return file_loader_->sourcePathForDataset(static_cast<DatasetId>(dataset_id));
  });
  const QByteArray stamped = doc.toByteArray(2);
  return CapturedWorkspace{.xml = stamped.isEmpty() ? unstamped : stamped, .timeline = timeline};
}

std::optional<DatasetId> MainWindow::resolveTimelineTrack(
    const TimelineTrackState& track, TimelineRestoreMode mode,
    const std::vector<std::pair<DatasetId, QString>>& live_datasets) const {
  const auto live_it = std::find_if(live_datasets.begin(), live_datasets.end(), [&track](const auto& live) {
    return live.first == track.dataset_id;
  });
  if (live_it != live_datasets.end()) {
    return track.dataset_id;
  }
  if (mode == TimelineRestoreMode::kExact || track.source_path.isEmpty()) {
    return std::nullopt;
  }
  std::vector<DatasetId> candidates;
  for (const auto& [candidate_id, label] : live_datasets) {
    (void)label;
    if (layout_xml::isSamePath(session_->sessionManager().datasetSourcePath(candidate_id), track.source_path)) {
      candidates.push_back(candidate_id);
    }
  }
  std::vector<DatasetId> named;
  for (const DatasetId candidate : candidates) {
    const std::optional<QString> source_name = session_->catalogModel().datasetSourceName(candidate);
    if (track.source_name.isEmpty() || (source_name.has_value() && *source_name == track.source_name)) {
      named.push_back(candidate);
    }
  }
  if (named.size() == 1) {
    return named.front();
  }
  if (track.source_index >= 0 && track.source_index < static_cast<int>(candidates.size())) {
    const DatasetId indexed = candidates[static_cast<std::size_t>(track.source_index)];
    if (std::find(named.begin(), named.end(), indexed) != named.end()) {
      return indexed;
    }
  }
  return std::nullopt;
}

std::optional<MainWindow::TimelineResolutionPlan> MainWindow::validateTimelineState(
    const TimelineState& state, TimelineRestoreMode mode) const {
  if (!std::isfinite(state.zoom) || !(state.zoom > 0.0) || state.scroll_top_px < 0 || state.name_column_width <= 0) {
    return std::nullopt;
  }
  const auto live_datasets = session_->catalogModel().datasets();

  QSet<DatasetId> resolved_ids;
  QSet<int> order_slots;
  TimelineResolutionPlan plan;
  plan.reserve(state.tracks.size());
  for (const TimelineTrackState& track : state.tracks) {
    const std::optional<DatasetId> resolved = resolveTimelineTrack(track, mode, live_datasets);
    if (!resolved.has_value() || resolved_ids.contains(*resolved) || track.timeline_order < -1) {
      return std::nullopt;
    }
    plan.push_back(*resolved);
    resolved_ids.insert(*resolved);
    if (track.timeline_order >= 0) {
      if (order_slots.contains(track.timeline_order)) {
        return std::nullopt;
      }
      order_slots.insert(track.timeline_order);
    }
    if (const auto raw = session_->datasetRawTimeRange(*resolved);
        raw.has_value() && (!timelineDifferenceFits(raw->min, track.display_offset_ns) ||
                            !timelineDifferenceFits(raw->max, track.display_offset_ns))) {
      return std::nullopt;
    }
  }
  for (int slot = 0; slot < order_slots.size(); ++slot) {
    if (!order_slots.contains(slot)) {
      return std::nullopt;
    }
  }
  return plan;
}

bool MainWindow::applyTimelineState(const TimelineState& state, const TimelineResolutionPlan& plan) {
  if (plan.size() != state.tracks.size()) {
    return false;
  }

  std::vector<std::pair<int, DatasetId>> ordered;
  for (std::size_t index = 0; index < state.tracks.size(); ++index) {
    const TimelineTrackState& track = state.tracks[index];
    const DatasetId dataset_id = plan[index];
    session_->sessionManager().setDisplayOffset(dataset_id, DisplayOffset{Duration{track.display_offset_ns}});
    if (track.timeline_order >= 0) {
      ordered.emplace_back(track.timeline_order, dataset_id);
    }
  }
  std::sort(ordered.begin(), ordered.end());
  std::vector<DatasetId> order;
  order.reserve(ordered.size());
  for (const auto& [slot, dataset_id] : ordered) {
    (void)slot;
    order.push_back(dataset_id);
  }
  if (source_timeline_controller_ != nullptr) {
    source_timeline_controller_->setDisplayOrder(std::move(order));
  }
  applyTimelineChrome(
      TimelineChromeState{
          .zoom = state.zoom,
          .scroll_left_ns = state.scroll_left_ns,
          .scroll_top_px = state.scroll_top_px,
          .name_column_width = state.name_column_width,
          .snap = state.snap,
      });
  return true;
}

MainWindow::RestoreResult MainWindow::applyWorkspace(
    QDomDocument& doc, MissingCurvePolicy policy, const TimelineState* timeline_state,
    const TimelineResolutionPlan* timeline_plan) {
  const QDomElement root = doc.documentElement();
  // 1. Recreate the snapshot's filters first, so each derived output topic is in the
  //    catalog and its plotted (derived) curve resolves like any other curve.
  if (!restoreDataProcessors(root)) {
    return RestoreResult::kFailed;
  }
  // 2. Rebind every curve's stable topic+field to a concrete catalog key.
  const QList<layout_xml::SeriesPath> unresolved = rebindCurvesToLoadedDatasets(doc);
  if (policy == MissingCurvePolicy::kExact && !unresolved.isEmpty()) {
    return RestoreResult::kFailed;
  }
  if (policy == MissingCurvePolicy::kPrompt && !unresolved.isEmpty()) {
    QStringList shown;
    shown.reserve(unresolved.size());
    for (const layout_xml::SeriesPath& sp : unresolved) {
      shown.push_back(sp.display());
    }
    switch (promptMissingCurves(shown)) {
      case MissingCurveChoice::kCancel:
        return RestoreResult::kCancelled;
      case MissingCurveChoice::kRemove:
        layout_xml::stripUnresolvedCurves(doc);
        break;
    }
  }
  if (policy == MissingCurvePolicy::kRetainAndDiagnose && !unresolved.isEmpty()) {
    // D5: never a dialog. The unresolved curves stay in the document (their
    // intents are re-registered with the pending binder by
    // rebuildPendingDisplayBindings below and RETAINED for later binding);
    // the diagnostic names them. Distinct id from the drain-time
    // "layout-import-unresolved-curves": this is the SYNC restore leg.
    QStringList shown;
    shown.reserve(unresolved.size());
    for (const layout_xml::SeriesPath& sp : unresolved) {
      shown.push_back(sp.display());
    }
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "layout-import-unresolved-curves-sync",
        tr("%n layout curve(s) could not be bound: %1", nullptr, static_cast<int>(shown.size()))
            .arg(shown.join(u", "_s)));
  }
  // kSilentDrop compatibility restores leave unresolved curves for xmlLoadState
  // to discard. Undo/redo uses kExact.
  // 3. Apply plots + global toggles.
  if (!xmlLoadState(doc)) {
    return RestoreResult::kFailed;
  }
  // Plot reconstruction, the timeline, and scene docks are independent restore
  // participants of this bool-and-rollback transaction.
  if (timeline_state != nullptr && (timeline_plan == nullptr || !applyTimelineState(*timeline_state, *timeline_plan))) {
    return RestoreResult::kFailed;
  }
  rebuildPendingDisplayBindings(doc);
  // Scene layers/config topics can be deferred by their dock until the saved
  // dataset identity exists. Blocking loads and history replay have no later
  // queue-drain phase, so make one final attempt now and treat any permanent
  // rejection — or any unresolved BLOCKING element in an exact snapshot — as a
  // failed transaction rather than silently committing a partial scene.
  const SceneRestoreVerdict scenes = settleSceneRestores();
  if (scenes.failed || (policy == MissingCurvePolicy::kExact && !scenes.blocking_topics.isEmpty())) {
    return RestoreResult::kFailed;
  }
  if (policy == MissingCurvePolicy::kPrompt && !scenes.blocking_topics.isEmpty()) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "scene_restore_pending",
        tr("%n scene layer(s) could not be rebound and were omitted.", nullptr,
           static_cast<int>(scenes.blocking_topics.size())));
    forEachSceneDock([](SceneDockWidget* scene_dock) { scene_dock->clearBlockingPendingRestores(); });
  }
  if (policy == MissingCurvePolicy::kRetainAndDiagnose && !scenes.blocking_topics.isEmpty()) {
    // Retained (not cleared): a later binding pass may still satisfy them.
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "scene_restore_pending",
        tr("%n scene layer(s) are not yet bound.", nullptr, static_cast<int>(scenes.blocking_topics.size())));
  }
  forEachPlot([](PlotWidget* plot) { plot->applySavedViewportOrZoom(/*clear_after=*/true); });
  // 4. Seed the just-recreated docks with the current playhead. currentTimeChanged
  // only fires on a CHANGE, so a freshly restored dock would sit at no-tracker-time
  // until the next scrub — scene docks then render blank (TF lookups / image decode
  // key off the tracker instant). Same seeding the drag-drop / click-create paths do
  // (MainWindow.cpp:480, makeSeededEmptyObjectDock); here it covers layout load + undo/redo.
  broadcastTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
  // 5. Re-route the right panel to the restored active dock's family. xmlLoadState
  // rebuilds docks with focus suppressed (PlotDocker::restoring_state_), so no
  // dockFocused signal fires — without this the panel keeps whatever page it last
  // showed (page 0 / plot-config at startup), so a scene-only layout would display
  // curve options over a 3D scene. activeFocusedDock() falls back to the first dock
  // when focus is stale/null after the rebuild.
  onDockFocused(activeFocusedDock());
  return RestoreResult::kApplied;
}

MainWindow::RestoreResult MainWindow::restoreWorkspaceStateImpl(
    QDomDocument& doc, MissingCurvePolicy policy, const TimelineState* timeline_state,
    TimelineRestoreMode timeline_mode, const CapturedWorkspace* rollback_to) {
  std::optional<TimelineResolutionPlan> timeline_plan;
  if (timeline_state != nullptr) {
    timeline_plan = validateTimelineState(*timeline_state, timeline_mode);
    if (!timeline_plan.has_value()) {
      return RestoreResult::kFailed;
    }
  }

  QScopedValueRollback applying_guard(applying_state_, true);
  const CapturedWorkspace previous = rollback_to != nullptr ? *rollback_to : captureWorkspace();
  const RestoreResult result =
      applyWorkspace(doc, policy, timeline_state, timeline_plan.has_value() ? &*timeline_plan : nullptr);
  if (result == RestoreResult::kApplied) {
    return result;
  }

  QDomDocument previous_doc;
  if (previous_doc.setContent(previous.xml)) {
    const std::optional<TimelineResolutionPlan> previous_plan =
        validateTimelineState(previous.timeline, TimelineRestoreMode::kExact);
    if (previous_plan.has_value()) {
      static_cast<void>(
          applyWorkspace(previous_doc, MissingCurvePolicy::kSilentDrop, &previous.timeline, &*previous_plan));
    }
  }
  return result;
}

MainWindow::RestoreResult MainWindow::restoreWorkspaceState(
    QDomDocument& doc, MissingCurvePolicy policy, const CapturedWorkspace* rollback_to) {
  return restoreWorkspaceStateImpl(doc, policy, nullptr, TimelineRestoreMode::kExact, rollback_to);
}

MainWindow::RestoreResult MainWindow::restoreWorkspaceState(
    const CapturedWorkspace& target, MissingCurvePolicy policy, TimelineRestoreMode timeline_mode,
    const CapturedWorkspace* rollback_to) {
  QDomDocument doc;
  if (!doc.setContent(target.xml)) {
    return RestoreResult::kFailed;
  }
  return restoreWorkspaceStateImpl(doc, policy, &target.timeline, timeline_mode, rollback_to);
}

void MainWindow::onUndo() {
  if (progressive_layout_in_flight_ || undo_states_.size() <= 1) {
    return;
  }

  const CapturedWorkspace target_state = undo_states_[undo_states_.size() - 2];
  restoreHistoryState(target_state, /*undo=*/true);
}

void MainWindow::onRedo() {
  if (progressive_layout_in_flight_ || redo_states_.empty()) {
    return;
  }

  const CapturedWorkspace target_state = redo_states_.back();
  restoreHistoryState(target_state, /*undo=*/false);
}

void MainWindow::restoreHistoryState(const CapturedWorkspace& target, bool undo) {
  const CapturedWorkspace current_state = captureWorkspace();
  const bool loaded =
      restoreWorkspaceState(target, MissingCurvePolicy::kExact, TimelineRestoreMode::kExact, &current_state) ==
      RestoreResult::kApplied;
  if (!loaded) {
    statusBar()->showMessage(undo ? tr("Unable to restore undo state") : tr("Unable to restore redo state"), 3000);
  } else if (undo) {
    redo_states_.push_back(current_state);
    undo_states_.pop_back();
    hydrateCurrentUndoState(/*refresh_data_universe=*/false);
  } else {
    undo_states_.back() = current_state;
    undo_states_.push_back(target);
    redo_states_.pop_back();
    hydrateCurrentUndoState(/*refresh_data_universe=*/false);
  }
  undo_timer_.invalidate();
  updateUndoRedoActions();
}

QDomElement MainWindow::appendDataSourceElement(QDomDocument& doc, const QDir& layout_dir) const {
  const auto& sources = session_->sessionManager().loadedSources();
  // Do not save a data-source reference after the catalog was cleared.
  if (sources.empty() || session_->catalogModel().isEmpty()) {
    return QDomElement();
  }

  // Persist only sources that still back a live dataset. A dataset whose curves
  // were all removed (Remove Dataset, or trashing every topic) drops out of
  // datasets(), so its file is neither serialized here nor resurrected on the
  // next reload — while loaded_sources_ itself stays intact for the quick-reload
  // button (which keys off lastLoadedSource, not this list). FileLoader owns the
  // DatasetId->path link the engine's basename-only DatasetInfo can't provide.
  // Keep every DatasetId for each file (in catalog/load order): one file replay
  // may fan out into N datasets, each with its own TimeDomain and timeline track.
  const std::vector<std::pair<DatasetId, QString>> live_datasets = session_->catalogModel().datasets();

  // Source Timeline arrangement: the bar's top-to-bottom slot, persisted per
  // file so the vertical order round-trips (re-bound by path on reload). Build
  // dataset -> slot once; absent => fall back to load order on restore.
  QHash<DatasetId, int> timeline_order;
  if (source_timeline_controller_ != nullptr) {
    const std::vector<DatasetId> order = source_timeline_controller_->currentTrackOrder();
    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
      timeline_order.insert(order[static_cast<std::size_t>(i)], i);
    }
  }

  QDomElement wrapper = doc.createElement(u"previouslyLoaded_Datafiles"_s);

  // One <fileInfo> per loaded file, in load order, so a multi-file session
  // round-trips. Old PJ4 readers that only read the first child degrade to the
  // first file; new readers restore them all.
  for (const auto& src : sources) {
    // Every live dataset this file backs, in catalog/load order. source_index is
    // that stable position (two fan-out members with identical display names
    // must not swap offsets on reload).
    std::vector<DatasetId> datasets_for_file;
    for (const auto& [id, name] : live_datasets) {
      (void)name;
      const QString source_path = file_loader_->sourcePathForDataset(id);
      if (!source_path.isEmpty() && layout_xml::isSamePath(source_path, src.path)) {
        datasets_for_file.push_back(id);
      }
    }
    if (datasets_for_file.empty()) {
      continue;  // dataset removed since load; don't resurrect it on reload
    }
    QDomElement file_info = doc.createElement(u"fileInfo"_s);

    const QString abs = QFileInfo(src.path).absoluteFilePath();
    file_info.setAttribute(u"filename"_s, relocatableSubpath(abs, layout_dir));
    file_info.setAttribute(u"prefix"_s, src.prefix);

    // One <dataset> child per fan-out member. The offset is sourceDisplayOffset()
    // — the per-source alignment WITHOUT the global reference (schema v4 basis).
    // Mirror the first child's state onto the legacy <fileInfo> attributes so a
    // <=v3 reader still gets a single-track view of the file.
    for (std::size_t index = 0; index < datasets_for_file.size(); ++index) {
      const DatasetId id = datasets_for_file[index];
      QDomElement dataset_el = doc.createElement(u"dataset"_s);
      dataset_el.setAttribute(u"source_index"_s, QString::number(index));
      if (const std::optional<QString> source_name = session_->catalogModel().datasetSourceName(id)) {
        dataset_el.setAttribute(u"source_name"_s, *source_name);
      }
      const QString offset = QString::number(session_->sessionManager().sourceDisplayOffset(id).value.count());
      dataset_el.setAttribute(u"display_offset_ns"_s, offset);
      if (const auto order_it = timeline_order.constFind(id); order_it != timeline_order.constEnd()) {
        dataset_el.setAttribute(u"timeline_order"_s, QString::number(order_it.value()));
      }
      file_info.appendChild(dataset_el);
      if (index == 0) {
        file_info.setAttribute(u"display_offset_ns"_s, offset);
        if (dataset_el.hasAttribute(u"timeline_order"_s)) {
          file_info.setAttribute(u"timeline_order"_s, dataset_el.attribute(u"timeline_order"_s));
        }
      }
    }

    // Emit the plugin sub-element whenever the plugin id is known. An
    // empty saveConfig payload is legitimate (some plugins have no
    // user-tunable state) and must NOT cause us to skip — otherwise
    // those plugins would re-prompt on every layout reload. Empty
    // plugin_id means the loader didn't capture a plugin (legacy path
    // or saveConfig failure); only that case skips the child.
    if (!src.plugin_id.isEmpty()) {
      QDomElement plugin = doc.createElement(u"plugin"_s);
      plugin.setAttribute(u"ID"_s, src.plugin_id);
      // Write BOTH identities: old readers keep matching by the display name
      // in ID; new readers prefer the stable manifest id, which survives a
      // plugin display-name change.
      if (!src.plugin_manifest_id.isEmpty()) {
        plugin.setAttribute(u"manifest_id"_s, src.plugin_manifest_id);
      }
      // CDATA so the JSON survives round-tripping without XML escape mangling.
      // appendJsonAsCdata splits across multiple CDATA sections when the JSON
      // contains a literal "]]>" sequence (otherwise it'd terminate the
      // CDATA early and corrupt the layout file).
      layout_xml::appendJsonAsCdata(doc, plugin, src.plugin_config_json);
      file_info.appendChild(plugin);
    }

    // Provider provenance: when this file's primary dataset carries a
    // SessionManager::SourceRecord, persist it as a <materialize> SIBLING of
    // <plugin> (old readers ignore unknown fileInfo children). The descriptor
    // JSON is written VERBATIM as CDATA — the bytes are a cross-repo identity
    // contract, never re-serialized or prettified. Provider sources attach the
    // record to their (single) produced dataset, so the primary — the first
    // live dataset in catalog/load order, the same one mirrored onto the
    // legacy flat attributes above — is scanned first; a record on a later
    // fan-out member (not expected) is used only when the primary has none,
    // and only one record is ever emitted per fileInfo.
    for (const DatasetId id : datasets_for_file) {
      const SourceRecord* record = session_->sessionManager().sourceRecord(id);
      if (record == nullptr || record->provider_id.isEmpty()) {
        continue;  // no provenance, or unroutable without a provider id
      }
      QDomElement materialize = doc.createElement(u"materialize"_s);
      materialize.setAttribute(u"provider"_s, record->provider_id);
      materialize.setAttribute(u"identity"_s, record->source_identity);
      layout_xml::appendJsonAsCdata(doc, materialize, record->descriptor_json);
      file_info.appendChild(materialize);
      break;
    }

    wrapper.appendChild(file_info);
  }
  // Every source was filtered out (all their datasets are gone): emit nothing
  // rather than an empty <previouslyLoaded_Datafiles> wrapper.
  if (!wrapper.hasChildNodes()) {
    return QDomElement();
  }
  return wrapper;
}

bool MainWindow::applyTimelineStateFromLayout(const QList<layout_xml::DataSourceRef>& sources) {
  if (sources.isEmpty()) {
    return false;
  }
  SessionManager& mgr = session_->sessionManager();

  // Re-bind each saved <fileInfo> to every loaded dataset produced from that
  // file. DatasetIds are re-minted per session, so path + fan-out
  // (source_name, source_index) is the stable identity. Apply each track's
  // offset and collect (id, slot) for the vertical-order rebuild.
  std::vector<std::pair<int, DatasetId>> ordered;  // (timeline_order, id)
  QSet<DatasetId> matched_ids;
  bool offset_changed = false;
  const auto apply_state = [this, &mgr, &ordered, &matched_ids, &offset_changed](
                               DatasetId matched, qint64 offset_ns, bool has_offset, bool includes_global_reference,
                               int order) {
    // Multiple <fileInfo>/<dataset> aliases can resolve to one physical dataset.
    // First match wins; never apply its offset twice or draw it in two slots.
    if (matched_ids.contains(matched)) {
      return;
    }
    matched_ids.insert(matched);
    if (has_offset) {
      // v3 wrote displayOffset() (per-source alignment + the global reference);
      // v4 writes sourceDisplayOffset() only. Subtract the current global
      // reference for a v3 read so total placement stays equal without
      // double-applying it (setDisplayOffset writes the per-source alignment).
      if (includes_global_reference) {
        const std::optional<qint64> migrated = checkedTimelineDifference(offset_ns, mgr.globalTimeReference());
        if (!migrated.has_value()) {
          has_offset = false;
        } else {
          offset_ns = *migrated;
        }
      }
      if (const auto raw = session_->datasetRawTimeRange(matched);
          raw.has_value() &&
          (!timelineDifferenceFits(raw->min, offset_ns) || !timelineDifferenceFits(raw->max, offset_ns))) {
        has_offset = false;
      }
      // Track whether this write actually MOVES the offset: plots restored earlier
      // (restoreWorkspaceState) framed their viewport with the pre-apply offset, so
      // the caller must re-frame only when an offset really changed here.
      if (has_offset && mgr.sourceDisplayOffset(matched).value.count() != offset_ns) {
        offset_changed = true;
      }
      if (has_offset) {
        mgr.setDisplayOffset(matched, DisplayOffset{Duration{offset_ns}});
      }
    }
    if (order >= 0) {
      ordered.emplace_back(order, matched);
    }
  };

  // datasets() copies the whole catalog list per call; the candidate scan below
  // reads it once per saved source, so hoist the single snapshot out of the loop.
  const std::vector<std::pair<DatasetId, QString>> live_datasets = session_->catalogModel().datasets();

  for (const layout_xml::DataSourceRef& ref : sources) {
    std::vector<DatasetId> candidates;
    for (const auto& [id, name] : live_datasets) {
      (void)name;
      const QString src_path = file_loader_->sourcePathForDataset(id);
      if (!src_path.isEmpty() && layout_xml::isSamePath(src_path, ref.resolved_path)) {
        candidates.push_back(id);
      }
    }
    if (candidates.empty()) {
      continue;  // file referenced by the layout isn't loaded — nothing to restore
    }

    if (ref.datasets.isEmpty()) {
      // Legacy (<=v3) layout: one state on <fileInfo>, no fan-out description.
      // Deterministically apply it to the first dataset the file created,
      // matching the historical one-track behavior.
      apply_state(
          candidates.front(), ref.display_offset_ns, ref.has_display_offset,
          ref.display_offset_includes_global_reference, ref.timeline_order);
      continue;
    }

    // Fan-out apply: bind each saved <dataset> back to a live candidate through
    // the shared shape-guarded matcher (see layout_xml::matchFanoutDatasets for
    // the name/index policy), then apply each bound child's offset and order.
    const std::vector<std::uint32_t> matches = layout_xml::matchFanoutDatasets(
        ref.datasets, std::vector<std::uint32_t>(candidates.begin(), candidates.end()), [this](std::uint32_t id) {
          return session_->catalogModel().datasetSourceName(static_cast<DatasetId>(id)).value_or(QString{});
        });
    for (qsizetype child_index = 0; child_index < ref.datasets.size(); ++child_index) {
      const DatasetId matched = static_cast<DatasetId>(matches[static_cast<std::size_t>(child_index)]);
      if (matched == 0) {
        continue;  // missing/ambiguous fan-out entry: never shift a sibling
      }
      const layout_xml::DataSourceDatasetRef& saved = ref.datasets[child_index];
      apply_state(
          matched, saved.display_offset_ns, saved.has_display_offset, saved.display_offset_includes_global_reference,
          saved.timeline_order);
    }
  }

  // Rebuild the vertical track order from the saved slots. Sorting by the saved
  // index (not document order) keeps the arrangement exact even if <fileInfo>
  // elements were written in load order rather than display order.
  if (source_timeline_controller_ != nullptr && !ordered.empty()) {
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<DatasetId> order;
    order.reserve(ordered.size());
    for (const auto& [slot, id] : ordered) {
      (void)slot;
      order.push_back(id);
    }
    source_timeline_controller_->setDisplayOrder(std::move(order));
  }
  return offset_changed;
}

bool MainWindow::applyPendingTimelineState() {
  if (pending_timeline_sources_.isEmpty()) {
    return false;
  }
  const QList<layout_xml::DataSourceRef> sources = std::move(pending_timeline_sources_);
  pending_timeline_sources_.clear();
  return applyTimelineStateFromLayout(sources);
}

QDomElement MainWindow::saveSourceTimelineViewState(QDomDocument& doc) const {
  const TimelineChromeState chrome = captureTimelineChrome();
  layout_xml::SourceTimelineViewState state{
      .zoom = chrome.zoom,
      .scroll_left_ns = chrome.scroll_left_ns,
      .scroll_top_px = chrome.scroll_top_px,
      .name_column_width = chrome.name_column_width,
      .snap = chrome.snap,
  };
  return layout_xml::writeSourceTimelineViewState(doc, state);
}

void MainWindow::restoreSourceTimelineViewState(const QDomElement& element) {
  if (source_timeline_ == nullptr) {
    return;
  }
  const layout_xml::SourceTimelineViewState saved = layout_xml::readSourceTimelineViewState(element);
  TimelineChromeState state = captureTimelineChrome();
  if (saved.zoom) {
    state.zoom = *saved.zoom;
  }
  if (saved.scroll_left_ns) {
    state.scroll_left_ns = *saved.scroll_left_ns;
  }
  if (saved.scroll_top_px) {
    state.scroll_top_px = *saved.scroll_top_px;
  }
  if (saved.name_column_width) {
    state.name_column_width = *saved.name_column_width;
  }
  if (saved.snap) {
    state.snap = *saved.snap;
  }
  applyTimelineChrome(state);
}

QDomElement MainWindow::saveRightPanelState(QDomDocument& doc) const {
  QDomElement element = doc.createElement(u"right_panel_state"_s);

  // The right panel's open/closed state (the panel-visibility toggle button) is
  // an app-wide QSettings preference, NOT document state, so it is deliberately
  // not serialized into the layout/undo snapshot. Only the panel's geometry and
  // curve width/style live here.

  // Curve Width: the radio's checkedId is a loop index (0..3); map it
  // back to the canonical double via kWidthButtonSpecs so we encode the
  // value (rebuild-stable) rather than the index (depends on button
  // declaration order).
  if (width_button_group_ != nullptr) {
    const int id = width_button_group_->checkedId();
    if (id >= 0 && id < static_cast<int>(kWidthButtonSpecs.size())) {
      element.setAttribute(u"width"_s, QString::number(kWidthButtonSpecs[id].second, 'g'));
    }
  }

  // Curve Style: the radio's checkedId IS the CurveStyle enum value
  // (assigned at button-group construction). Stable across rebuilds.
  if (style_button_group_ != nullptr) {
    const int id = style_button_group_->checkedId();
    if (id >= 0) {
      element.setAttribute(u"style"_s, QString::number(id));
    }
  }

  if (ui_->rightToolbarSplitter != nullptr) {
    QStringList parts;
    const QList<int> sizes = ui_->rightToolbarSplitter->sizes();
    parts.reserve(sizes.size());
    for (int s : sizes) {
      parts.push_back(QString::number(s));
    }
    element.setAttribute(u"splitter_sizes"_s, parts.join(QLatin1Char(',')));
  }

  return element;
}

void MainWindow::applyBottomPanelConstraints() {
  if (ui_->bottomPanel == nullptr || ui_->timelineWidget == nullptr || ui_->timelineStrip == nullptr) {
    return;
  }
  // The playback bar is fixed-height (sizePolicy Fixed in the .ui; min==max is
  // pinned to its sizeHint by the chrome-metrics pass), so its current minimum
  // height is the playback band's extent.
  const int playback_height = ui_->timelineWidget->minimumHeight();
  // Use the strip's *intended* state (!isHidden), NOT isVisible(): this runs during
  // the constructor's chrome-metrics pass, before the window is shown, where
  // isVisible() is still false for an open strip and would wrongly pick the CLOSED
  // branch (pinning the panel to the playback bar) — which left the strip squashed
  // into a sliver on boot.
  const bool strip_visible = !ui_->timelineStrip->isHidden();
  // The align rail (and its 1-px divider) only make sense beside an open strip;
  // hide them with the panel so a closed timeline shows no orphaned right-edge column.
  if (ui_->timelineAlignRail != nullptr) {
    ui_->timelineAlignRail->setVisible(strip_visible);
  }
  if (ui_->timelineAlignBorder != nullptr) {
    ui_->timelineAlignBorder->setVisible(strip_visible);
  }
  // The playback/timeline divider only makes sense with the strip open; hide it
  // with the panel so a closed timeline shows no stray 1-px line under playback.
  if (ui_->timelinePlaybackBorder != nullptr) {
    ui_->timelinePlaybackBorder->setVisible(strip_visible);
  }
  if (strip_visible) {
    // OPEN: floor the panel at playback + a usable strip height so the Source
    // Timeline never opens (or is dragged) into a clipped sliver; no upper cap.
    const int min_panel = playback_height + kMinTimelineStripHeight;
    ui_->bottomPanel->setMaximumHeight(QWIDGETSIZE_MAX);
    ui_->bottomPanel->setMinimumHeight(min_panel);
    // QSplitter caches old slot sizes and won't re-honor a child's new minimum
    // on its own — push it up to the floor when the strip slot is too short.
    const QList<int> sizes = ui_->timelineSplitter->sizes();
    if (sizes.size() == 2 && sizes[1] < min_panel) {
      ui_->timelineSplitter->setSizes({(sizes[0] + sizes[1]) - min_panel, min_panel});
    }
  } else {
    // CLOSED: pin the panel to exactly the playback bar. With min == max the
    // splitter handle can't drag the (hidden) strip open — only the toggle re-opens it.
    ui_->bottomPanel->setMinimumHeight(playback_height);
    ui_->bottomPanel->setMaximumHeight(playback_height);
    // Collapse the splitter slot too, so no empty gap is left below the playback
    // (QSplitter caches sizes and won't shrink the slot from the max change alone).
    const QList<int> sizes = ui_->timelineSplitter->sizes();
    if (sizes.size() == 2 && sizes[1] != playback_height) {
      ui_->timelineSplitter->setSizes({(sizes[0] + sizes[1]) - playback_height, playback_height});
    }
  }
  if (auto* bottom_layout = ui_->bottomPanel->layout()) {
    bottom_layout->invalidate();
    bottom_layout->activate();
  }
}

void MainWindow::alignNameColumnToPlayback() {
  if (source_timeline_ == nullptr || ui_->timelineWidget == nullptr) {
    return;
  }
  // The playback slider's groove has margin 0 (QSS), so the slider widget's left
  // edge is where the blue track starts. Its x within the playback bar — which
  // shares the bottom panel's left origin with the name column — is the column
  // width that lines the separator up exactly under that track start.
  auto* slider = ui_->timelineWidget->findChild<QWidget*>(u"timeSlider"_s);
  if (slider == nullptr) {
    return;
  }
  const int slider_x = slider->mapTo(ui_->timelineWidget, QPoint(0, 0)).x();
  if (slider_x <= 0) {
    return;  // playback bar not laid out yet (pre first show)
  }
  // slider_x is the floor (separator under the playback track start). If the user
  // widened the column (or a layout restored a wider width), re-apply it on top
  // so the column keeps its chosen width rather than snapping back to the floor.
  source_timeline_->setNameColumnWidth(slider_x);
  if (timeline_name_column_width_ > slider_x) {
    source_timeline_->resizeNameColumn(timeline_name_column_width_);
  }
}

void MainWindow::restoreRightPanelState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "right_panel_state"_L1) {
    return;
  }

  // Panel visibility is intentionally not restored (see saveRightPanelState).

  // Curve Width: look up the button whose canonical value fuzzy-matches
  // the layout's stored value. Block group signals so the idClicked
  // lambda doesn't refresh the previews on this passive resync.
  if (element.hasAttribute(u"width"_s) && width_button_group_ != nullptr) {
    bool ok = false;
    const double wanted = element.attribute(u"width"_s).toDouble(&ok);
    if (ok) {
      for (int i = 0; i < static_cast<int>(kWidthButtonSpecs.size()); ++i) {
        if (qFuzzyCompare(kWidthButtonSpecs[i].second, wanted)) {
          checkGroupButton(width_button_group_, i);
          break;
        }
      }
    }
  }

  // Curve Style: checkedId is the CurveStyle enum value; pass through.
  // Same preview-refresh suppression via QSignalBlocker.
  if (element.hasAttribute(u"style"_s) && style_button_group_ != nullptr) {
    bool ok = false;
    const int wanted = element.attribute(u"style"_s).toInt(&ok);
    if (ok) {
      checkGroupButton(style_button_group_, wanted);
    }
  }

  // Splitter sizes: only apply when the parsed list length matches the
  // splitter's current widget count. A mismatch means the splitter shape
  // changed across PJ4 versions; layout silently skips this piece.
  if (element.hasAttribute(u"splitter_sizes"_s) && ui_->rightToolbarSplitter != nullptr) {
    const QStringList parts = element.attribute(u"splitter_sizes"_s).split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.size() == ui_->rightToolbarSplitter->count()) {
      QList<int> sizes;
      sizes.reserve(parts.size());
      bool all_ok = true;
      for (const QString& p : parts) {
        bool ok = false;
        const int v = p.toInt(&ok);
        if (!ok) {
          all_ok = false;
          break;
        }
        sizes.push_back(v);
      }
      if (all_ok) {
        ui_->rightToolbarSplitter->setSizes(sizes);
      }
    }
  }
}

QDomElement MainWindow::saveChromeState(QDomDocument& doc) const {
  QDomElement element = doc.createElement(u"chrome_state"_s);

  // Persist only the main splitter's left-panel width. Panel visibility and the
  // timeline-strip height are fixed launch state (see the constructor), not saved.

  const auto join_sizes = [](QSplitter* splitter) {
    QStringList parts;
    const QList<int> sizes = splitter->sizes();
    parts.reserve(sizes.size());
    for (int s : sizes) {
      parts.push_back(QString::number(s));
    }
    return parts.join(QLatin1Char(','));
  };

  if (ui_->mainSplitter != nullptr) {
    element.setAttribute(u"main_splitter_sizes"_s, join_sizes(ui_->mainSplitter));
  }

  return element;
}

void MainWindow::restoreChromeState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "chrome_state"_L1) {
    return;
  }

  // Apply only the main splitter's left-panel width (see saveChromeState).
  // Splitter sizes: only apply when parsed length matches the splitter's
  // widget count. Mismatch -> silent no-op.
  const auto apply_splitter = [](QSplitter* splitter, const QString& raw) {
    if (splitter == nullptr) {
      return;
    }
    const QStringList parts = raw.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.size() != splitter->count()) {
      return;
    }
    QList<int> sizes;
    sizes.reserve(parts.size());
    for (const QString& p : parts) {
      bool ok = false;
      const int v = p.toInt(&ok);
      if (!ok) {
        return;
      }
      sizes.push_back(v);
    }
    splitter->setSizes(sizes);
  };

  if (element.hasAttribute(u"main_splitter_sizes"_s)) {
    apply_splitter(ui_->mainSplitter, element.attribute(u"main_splitter_sizes"_s));
  }
}

MainWindow::MissingCurveChoice MainWindow::promptMissingCurves(const QStringList& names) {
  static constexpr int kMaxShown = 10;
  const int name_count = static_cast<int>(names.size());
  QString body = tr("The layout references %n curve(s) not present in the current data:", "", name_count);
  body += u"\n\n"_s;
  const int shown = std::min(name_count, kMaxShown);
  for (int i = 0; i < shown; ++i) {
    body += u"  • "_s + names[i] + u"\n"_s;
  }
  if (name_count > kMaxShown) {
    body += tr("  … and %n more\n", "", name_count - kMaxShown);
  }
  body += u"\n"_s;
  body += tr("Choose how to handle them:");

  // Themed prompt. "Remove from plots" takes the destructive role (purple ink);
  // there is deliberately no default button, because Remove drops all missing
  // curves from every plot in the layout and an accidental Enter should not
  // trigger it on a layout the user just opened. Esc / dismiss → Cancel.
  constexpr int kRemove = 0;
  const int choice = MessageBox::question(
      this, tr("Missing curves"), body,
      {{tr("Remove from plots"), MessageBox::kDestructiveRole}, {tr("Cancel"), MessageBox::kCancelRole}});
  return choice == kRemove ? MissingCurveChoice::kRemove : MissingCurveChoice::kCancel;
}

QDomDocument MainWindow::xmlSaveState() const {
  QDomDocument doc;
  doc.appendChild(doc.createProcessingInstruction(u"xml"_s, u"version='1.0' encoding='UTF-8'"_s));

  QDomElement root = doc.createElement(u"root"_s);
  root.setAttribute(u"format"_s, u"PlotJuggler"_s);
  root.setAttribute(u"pj4_version"_s, QString::number(kLayoutSchemaVersion));
  doc.appendChild(root);

  root.appendChild(ui_->tabbedPlotWidget->xmlSaveState(doc));

  // The global toolbar toggles (link X, show-point, legend, grid, dots, tracker
  // mode, 1:1 ratio, "Use time offset") are deliberately NOT serialized here.
  // They are app-wide UI preferences persisted in QSettings, not document state,
  // so neither a layout file nor any undo/redo snapshot (which shares this
  // serializer) carries them. This is also what lets the per-plot <range> store
  // ABSOLUTE time safely: the restored range frames the right instant no matter
  // which way the "Use time offset" toggle is set when the layout is loaded.

  // Data-Processor filters are workspace/data state, so they belong in THE snapshot
  // used by undo/redo (not just layout save). Without this, undoing across a filter's
  // creation drops its derived curve (its output topic is never recreated on restore).
  // saveLayoutToPath therefore no longer appends this separately.
  root.appendChild(saveDataProcessors(doc));
  return doc;
}

bool MainWindow::xmlLoadState(const QDomDocument& state_document) {
  const QDomElement root = state_document.documentElement();
  if (root.isNull() || root.tagName() != "root"_L1) {
    qCWarning(lcMain) << "No <root> element found at the top-level of the XML document";
    return false;
  }

  QDomElement main_tabbed_widget;
  for (auto tabbed = root.firstChildElement(u"tabbed_widget"_s); !tabbed.isNull();
       tabbed = tabbed.nextSiblingElement(u"tabbed_widget"_s)) {
    if (main_tabbed_widget.isNull()) {
      main_tabbed_widget = tabbed;
    }
    if (tabbed.attribute(u"parent"_s) == "main_window"_L1) {
      main_tabbed_widget = tabbed;
      break;
    }
  }
  if (main_tabbed_widget.isNull()) {
    qCWarning(lcMain) << "No <tabbed_widget> element found in XML document";
    return false;
  }

  const bool loaded = ui_->tabbedPlotWidget->xmlLoadState(main_tabbed_widget);
  if (!loaded) {
    return false;
  }
  wireExistingPlots();

  // The global toolbar toggles are no longer read from the layout/undo document
  // (see xmlSaveState) — they are app-wide QSettings preferences. Freshly loaded
  // plots still adopt the CURRENT global toggle state below, so a restored
  // layout stays visually consistent with the rest of the app without the
  // document dictating the toggle positions.
  forEachPlot([this](PlotWidget* plot) { applyGlobalToggles(plot); });
  applyShowPointsTo2DWidgets();
  return true;
}

void MainWindow::pushInitialUndoState() {
  undo_states_.clear();
  redo_states_.clear();
  undo_states_.push_back(captureWorkspace());
  history_data_universe_ = captureHistoryDataUniverse();
  undo_timer_.invalidate();
  updateUndoRedoActions();
}

void MainWindow::resetUndoHistory() {
  // Re-baselining from the current state is what the initial push does. By the
  // time a removal call returns, the synchronous catalog subscriptions have
  // already pruned the widgets, so this snapshot is clean.
  pushInitialUndoState();
}

void MainWindow::pushUndoState(bool force_new_state) {
  const CapturedWorkspace state = captureWorkspace();
  if (!undo_states_.empty() && undo_states_.back() == state) {
    updateUndoRedoActions();
    return;
  }

  const bool should_coalesce =
      !force_new_state && undo_timer_.isValid() && undo_timer_.elapsed() < kUndoCoalesceMs && undo_states_.size() > 1;
  if (should_coalesce) {
    undo_states_.back() = state;
  } else {
    undo_states_.push_back(state);
  }

  while (undo_states_.size() > kMaxUndoStates) {
    undo_states_.pop_front();
  }
  redo_states_.clear();
  if (force_new_state) {
    // A forced edit is discrete on both sides; the following ordinary edit
    // must not coalesce forward into it.
    undo_timer_.invalidate();
  } else {
    undo_timer_.restart();
  }
  updateUndoRedoActions();
}

void MainWindow::hydrateCurrentUndoState(bool refresh_data_universe) {
  if (applying_state_ || progressive_layout_in_flight_ || undo_states_.empty()) {
    return;
  }
  undo_states_.back() = captureWorkspace();
  if (refresh_data_universe) {
    history_data_universe_ = captureHistoryDataUniverse();
  }
  undo_timer_.invalidate();
  updateUndoRedoActions();
}

void MainWindow::updateUndoRedoActions() {
  if (undo_action_ != nullptr) {
    undo_action_->setEnabled(!progressive_layout_in_flight_ && undo_states_.size() > 1);
  }
  if (redo_action_ != nullptr) {
    redo_action_->setEnabled(!progressive_layout_in_flight_ && !redo_states_.empty());
  }
}

void MainWindow::bindEditorToPlot(PlotWidget* plot) {
  if (curve_editor_ != nullptr) {
    curve_editor_->setPlot(plot);
  }
  // The width/style buttons act on the editor's plot, so keep them
  // visibly disabled when there is none to act on.
  const bool enable = plot != nullptr;
  // Style/width are plot-level, so the toolbar must reflect the plot we just
  // bound to (not the last button clicked). The group ids are the LineWidth
  // index / CurveStyle enum value. Resync happens on every active-plot change;
  // an in-place state swap on the focused plot (xmlLoadState / paste) is not
  // covered, since the bound plot does not change there.
  if (width_button_group_ != nullptr) {
    for (auto* btn : width_button_group_->buttons()) {
      btn->setEnabled(enable);
    }
    if (plot != nullptr) {
      checkGroupButton(width_button_group_, static_cast<int>(plot->lineWidth()));
    }
  }
  if (style_button_group_ != nullptr) {
    for (auto* btn : style_button_group_->buttons()) {
      btn->setEnabled(enable);
    }
    if (plot != nullptr) {
      checkGroupButton(style_button_group_, static_cast<int>(plot->defaultCurveStyle()));
    }
  }
}

PlotWidget* MainWindow::firstPlotOfActiveTab() const {
  auto* docker = ui_->tabbedPlotWidget->currentTab();
  auto* dock = (docker != nullptr && docker->plotCount() > 0) ? docker->plotAt(0) : nullptr;
  return dock != nullptr ? dock->plotWidget() : nullptr;
}

DockWidget* MainWindow::activeFocusedDock() const {
  auto* docker = ui_->tabbedPlotWidget->currentTab();
  if (docker == nullptr) {
    return nullptr;
  }
  if (DockWidget* focused = docker->focusedDock(); focused != nullptr) {
    return focused;
  }
  return docker->plotCount() > 0 ? docker->plotAt(0) : nullptr;
}

void MainWindow::onDockFocused(DockWidget* dock) {
  bindEditorToPlot(dock != nullptr ? dock->plotWidget() : nullptr);

  // Placeholder docks (3-icon picker) and unknown widget kinds fall
  // through to the empty page — "nothing to configure" is the honest
  // signal when there is no curve, image, or scene to act on.
  QWidget* target = empty_dock_page_;
#ifdef PJ_WITH_SCENE3D
  Scene3DDockWidget* scene3d_dock = nullptr;
#endif
#ifdef PJ_WITH_SCENE2D
  SceneDockWidget* scene2d_dock = nullptr;
#endif
  StateTransitionsController* strip_controller = nullptr;
  if (dock != nullptr) {
    if (dock->plotWidget() != nullptr) {
      target = plot_config_page_;
    } else if (dock->objectWidget() != nullptr) {
      QWidget* obj = dock->objectWidget()->widget();
#ifdef PJ_WITH_SCENE2D
      if (auto* s2d = qobject_cast<Scene2DDockWidget*>(obj); s2d != nullptr) {
        target = scene2d_config_page_;
        scene2d_dock = s2d;
      } else
#endif
#ifdef PJ_WITH_SCENE3D
          if (auto* s3d = qobject_cast<Scene3DDockWidget*>(obj); s3d != nullptr) {
        target = scene3d_config_page_;
        scene3d_dock = s3d;
      } else
#endif
          if (auto* strip = qobject_cast<StateTransitionsDockWidget*>(obj); strip != nullptr) {
        // The strip reuses the plot page: the SAME curves table (eye + trash,
        // no color swatch); the width/style strips stay disabled via the
        // bindEditorToPlot(nullptr) call above.
        target = plot_config_page_;
        strip_controller = strip->controller();
      }
    }
  }
  if (curve_editor_ != nullptr) {
    curve_editor_->setStateTransitions(strip_controller);  // null releases the strip binding
  }
  // Width/style sections are meaningless for a state strip — remove them from
  // the page entirely (the Curves table alone remains). Everything in the
  // plot-config layout except the CurveEditor is such a section.
  if (plot_config_page_ != nullptr && plot_config_page_->layout() != nullptr) {
    QLayout* page_layout = plot_config_page_->layout();
    for (int index = 0; index < page_layout->count(); ++index) {
      QWidget* section = page_layout->itemAt(index)->widget();
      if (section != nullptr && section != curve_editor_) {
        section->setVisible(strip_controller == nullptr);
      }
    }
  }
  // Bind / unbind the config panels BEFORE switching the stack so the page is
  // already populated when it becomes visible. Passing nullptr when leaving a
  // scene dock detaches signal connections cleanly.
#ifdef PJ_WITH_SCENE2D
  if (scene2d_config_panel_ != nullptr) {
    scene2d_config_panel_->bindDock(scene2d_dock);
  }
#endif
#ifdef PJ_WITH_SCENE3D
  if (scene3d_config_panel_ != nullptr) {
    scene3d_config_panel_->bindDock(scene3d_dock);
  }
#endif
  if (right_panel_stack_ != nullptr) {
    right_panel_stack_->setCurrentWidget(target);
  }
}

void MainWindow::buildGlobalToolbar() {
  // globalToolbarWidget is a 24-px fixed column packed with Chart icons,
  // a 1-px divider, then Legend icons (4 corner picker + eye toggle).
  // No headers — labels would never fit in a 24-px column. Always
  // visible regardless of the "Toggle Right Panel" button state.
  auto* outer = qobject_cast<QVBoxLayout*>(ui_->globalToolbarWidget->layout());
  if (outer == nullptr) {
    return;
  }
  outer->setSpacing(PJ::theme::space(theme::Space::None));
  outer->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));

  auto add_button = [this, outer](const char* object_name, const char* icon_path, const char* tooltip) -> SvgButton* {
    auto* btn = new SvgButton(ui_->globalToolbarWidget);
    btn->setObjectName(QString::fromLatin1(object_name));
    btn->setIconPath(QString::fromLatin1(icon_path));
    btn->setExtent(chrome_metrics_.icon_size + chrome_metrics_.icon_padding, chrome_metrics_.icon_size);
    btn->setToolTip(tr(tooltip));
    outer->addWidget(btn);
    return btn;
  };

  // "Chart" group — global plot view toggles. Each button is checkable
  // and wires a slot that updates the matching member flag, persists to
  // QSettings, and calls forEachPlot. applying_state_ no-ops the slot
  // during bulk reload (xmlLoadState / undo / redo).
  button_link_ = add_button("buttonLink", ":/resources/svg/link.svg", "Link X axis");
  button_zoom_out_ = add_button("buttonZoomOut", ":/resources/svg/zoom_max.svg", "Zoom Out All");
  button_grid_ = add_button("buttonActivateGrid", ":/resources/svg/grid.svg", "Show/Hide the grid");
  button_time_tracker_ = add_button(
      "buttonTimeTracker", ":/resources/svg/line_tracker.svg",
      "Cycle TimeTracker display: line only / line + value / line + value + name");
  updateTimeTrackerIcon();
  connect(button_time_tracker_, &QToolButton::clicked, this, &MainWindow::onTimeTrackerButtonClicked);

  // "Legend" group — single icon that combines a corner picker with a
  // show/hide toggle.
  //   * Left-click: enable at the current position if hidden, otherwise
  //                 cycle through corners (TR → TL → BL → BR → TR).
  //   * Right-click: toggle show/hide at the current position (no
  //                  cycle).
  // The icon always reflects the "current position": the active corner
  // while checked, or the saved corner that will be restored on the
  // next show while unchecked.
  if (legend_status_ != LegendStatus::kHidden) {
    previous_legend_corner_ = legend_status_;
  }
  const QByteArray initial_icon =
      legendCornerIcon(legend_status_ == LegendStatus::kHidden ? previous_legend_corner_ : legend_status_).toLatin1();
  button_legend_ = add_button(
      "buttonLegendPosition", initial_icon.constData(),
      "Legend position — left-click walks TR → TL → BL → BR, then unchecks at TR before the next "
      "cycle. Right-click toggles show/hide at the current corner.");
  button_legend_->setCheckable(true);
  button_legend_->setChecked(legend_status_ != LegendStatus::kHidden);
  connect(button_legend_, &QToolButton::clicked, this, &MainWindow::onLegendButtonClicked);
  // Right-click: enable customContextMenu so the click event reaches us
  // (Qt's default context menu policy would consume right-clicks for a
  // popup). No menu is shown — the signal is used solely as a
  // right-click hook.
  button_legend_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(button_legend_, &QToolButton::customContextMenuRequested, this, [this](const QPoint& /*pos*/) {
    if (legend_status_ == LegendStatus::kHidden) {
      setLegendStatus(previous_legend_corner_);
    } else {
      setLegendStatus(LegendStatus::kHidden);
    }
  });

  button_show_point_ = add_button("buttonShowpoint", ":/resources/svg/show_point.svg", "Show point in plot");
  button_ratio_ = add_button("buttonRatio", ":/resources/svg/ratio.svg", "Keep aspect ratio of XY plots (1:1)");
  button_dots_ = add_button("buttonDots", ":/resources/svg/scatter_plot.svg", "Show data point markers on curves");
  button_reference_point_ = add_button(
      "buttonReferencePoint", ":/resources/svg/reference_line.svg",
      "Drop a blue reference line at the playback position; values render as delta from there");
  button_t0_ = add_button(
      "buttonTimeOffset", ":/resources/svg/t0.svg",
      "Use time offset: show time relative to each dataset's start (axis begins at 0) instead of "
      "absolute time");
  auto make_checkable = [](QToolButton* btn, bool initial_checked) {
    btn->setCheckable(true);
    btn->setChecked(initial_checked);
  };
  make_checkable(button_link_, QSettings().value(u"MainWindow.buttonLink"_s, true).toBool());
  make_checkable(button_show_point_, show_points_);
  make_checkable(button_grid_, activate_grid_);
  make_checkable(button_ratio_, keep_ratio_);
  make_checkable(button_dots_, dots_);
  make_checkable(button_reference_point_, false);
  // Mirrors the frame the SessionManager already holds (applied from QSettings
  // at startup, before this runs).
  make_checkable(button_t0_, session_->sessionManager().useTimeOffset());
  // buttonZoomOut is a one-shot action, not a toggle — no make_checkable.
  connect(button_zoom_out_, &QToolButton::clicked, this, [this]() {
    linkedZoomOut();
    onUndoableChange();
  });
  connect(button_link_, &QToolButton::toggled, this, [](bool checked) {
    QSettings().setValue(u"MainWindow.buttonLink"_s, checked);
  });
  connect(button_show_point_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    show_points_ = checked;
    QSettings().setValue(u"MainWindow.buttonShowpoint"_s, checked);
    forEachPlot([checked](PlotWidget* plot) { plot->setShowPoints(checked); });
    applyShowPointsTo2DWidgets();
  });
  connect(button_grid_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    activate_grid_ = checked;
    QSettings().setValue(u"MainWindow.buttonActivateGrid"_s, checked);
    forEachPlot([checked](PlotWidget* plot) { plot->setGridVisible(checked); });
    syncFilterEditorPreviewDisplay();
    syncPanelPreviewDisplay();
  });
  connect(button_dots_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    dots_ = checked;
    QSettings().setValue(u"MainWindow.buttonDots"_s, checked);
    forEachPlot([this](PlotWidget* plot) {
      applyDots(plot);
      plot->replot();
    });
    onUndoableChange();
  });
  connect(button_ratio_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    keep_ratio_ = checked;
    QSettings().setValue(u"MainWindow.buttonRatio"_s, checked);
    forEachPlot([checked](PlotWidget* plot) { plot->setKeepRatioXY(checked); });
  });
  // Session-only state — not persisted to QSettings, not in xmlSaveState.
  // Captures the playback INSTANT at the moment of click (frame-invariant, via
  // toAbsolute); subsequent scrubbing does not move the reference, and a frame
  // change re-projects it rather than orphaning a stale display coordinate.
  connect(button_reference_point_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    reference_instant_ = checked ? std::optional<Timepoint>{toAbsolute(
                                       session_->playbackEngine().currentTime(),
                                       session_->sessionManager().displayOffset(representativeDatasetId()))}
                                 : std::nullopt;
    // Push to the plots and mirror it onto the Source Timeline's blue reference
    // needle (the controller bridges the playback frame into the Timeline frame).
    broadcastReferenceLine();
  });
  connect(button_t0_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    QSettings().setValue(u"MainWindow.useTimeOffset"_s, checked);
    onUseTimeOffsetToggled(checked);
  });

  // Trailing stretch pins the icon stack at the top of the column.
  outer->addStretch(1);

  // Resize the global toolbar column. Column width = button_extent +
  // 2 * layout_padding, with the same value pushed as contentsMargins
  // on the inner QVBoxLayout so the buttons grow inward to absorb the
  // padding instead of clipping.
  connect(this, &MainWindow::chromeMetricsChanged, ui_->globalToolbarWidget, [this](const ChromeMetrics& metrics) {
    const int button_extent = metrics.icon_size + metrics.icon_padding;
    const int column_width = button_extent + (2 * metrics.layout_padding);
    ui_->globalToolbarWidget->setMinimumWidth(column_width);
    ui_->globalToolbarWidget->setMaximumWidth(column_width);
    if (auto* layout = ui_->globalToolbarWidget->layout()) {
      layout->setContentsMargins(
          metrics.layout_padding, metrics.layout_padding, metrics.layout_padding, metrics.layout_padding);
      layout->setSpacing(metrics.layout_spacing);
    }
    for (auto* btn : ui_->globalToolbarWidget->findChildren<QToolButton*>()) {
      btn->setIconSize(QSize(metrics.icon_size, metrics.icon_size));
      btn->setFixedSize(button_extent, button_extent);
    }
  });
}

void MainWindow::buildTimelineAlignRail() {
  // A 24-px icon column on the right edge of the timeline panel that visually
  // continues the global toolbar above it (they line up when the right panel is
  // closed). Holds the Source Timeline alignment actions. The buttons mirror the
  // global toolbar's style (theme-tinted SVG, chrome-metrics sizing); the rail's
  // show/hide tracks the strip in applyBottomPanelConstraints.
  auto* outer = qobject_cast<QVBoxLayout*>(ui_->timelineAlignRail->layout());
  if (outer == nullptr) {
    return;
  }
  outer->setSpacing(PJ::theme::space(theme::Space::None));
  outer->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));

  auto add_button = [this, outer](const char* object_name, const char* icon_path, const char* tooltip) -> QToolButton* {
    auto* btn = new SvgButton(ui_->timelineAlignRail);
    btn->setObjectName(QString::fromLatin1(object_name));
    btn->setIconPath(QString::fromLatin1(icon_path));
    btn->setExtent(chrome_metrics_.icon_size + chrome_metrics_.icon_padding, chrome_metrics_.icon_size);
    btn->setToolTip(tr(tooltip));
    outer->addWidget(btn);
    return btn;
  };

  QToolButton* align_left = add_button(
      "buttonAlignStarts", ":/resources/svg/align_horizontal_left.svg",
      "Align sources: line every source's start up at the earliest start");

  QToolButton* align_right = add_button(
      "buttonAlignEnds", ":/resources/svg/align_horizontal_right.svg",
      "Align sources: line every source's end up at the latest end");

  // Zoom-out-horizontally: fit all source bars into the view (the timeline's
  // equivalent of the plot's "Zoom Out Horizontally", same icon). Replaces the
  // former align-centers action on the rail.
  QToolButton* zoom_fit =
      add_button("buttonTimelineFit", ":/resources/svg/zoom_horizontal.svg", "Zoom out to fit all sources");

  // "Snap to" toggle — when on, a dragged bar's start/end snaps to a neighbour's
  // edge (with a guide line). Checkable; on by default.
  QToolButton* snap_toggle = add_button(
      "buttonTimelineSnap", ":/resources/svg/transition_push.svg", "Snap to neighbouring dataset edges while dragging");
  snap_toggle->setCheckable(true);
  snap_toggle->setChecked(true);

  // Stretch pins the align/snap icons to the top of the content area; the reset
  // button below it sits at the BOTTOM of the rail.
  outer->addStretch(1);
  QToolButton* reset_all =
      add_button("buttonTimelineReset", ":/resources/svg/restart_alt.svg", "Reset all timeline changes");

  if (source_timeline_controller_ != nullptr) {
    connect(align_left, &QToolButton::clicked, source_timeline_controller_, &SourceTimelineController::alignStarts);
    connect(align_right, &QToolButton::clicked, source_timeline_controller_, &SourceTimelineController::alignEnds);
    connect(reset_all, &QToolButton::clicked, source_timeline_controller_, &SourceTimelineController::resetAll);
  }
  if (source_timeline_ != nullptr) {
    connect(zoom_fit, &QToolButton::clicked, source_timeline_, &Timeline::zoomToFit);
    connect(snap_toggle, &QToolButton::toggled, source_timeline_, &Timeline::setSnapEnabled);
    source_timeline_->setSnapEnabled(snap_toggle->isChecked());  // seed the initial state
  }

  // Match the global rail's column width + button sizing on chrome-metrics change,
  // so the two rails stay the same width and line up.
  connect(this, &MainWindow::chromeMetricsChanged, ui_->timelineAlignRail, [this](const ChromeMetrics& metrics) {
    const int button_extent = metrics.icon_size + metrics.icon_padding;
    const int column_width = button_extent + (2 * metrics.layout_padding);
    ui_->timelineAlignRail->setMinimumWidth(column_width);
    ui_->timelineAlignRail->setMaximumWidth(column_width);
    if (auto* layout = ui_->timelineAlignRail->layout()) {
      // The rail now sits beside the timeline strip only (the full-width playback
      // bar is above it), so the icons begin at the rail's own top — no inset.
      layout->setContentsMargins(
          metrics.layout_padding, metrics.layout_padding, metrics.layout_padding, metrics.layout_padding);
      layout->setSpacing(metrics.layout_spacing);
    }
    for (auto* btn : ui_->timelineAlignRail->findChildren<QToolButton*>()) {
      btn->setIconSize(QSize(metrics.icon_size, metrics.icon_size));
      btn->setFixedSize(button_extent, button_extent);
    }
  });
}

void MainWindow::buildLocalToolbar() {
  // Populates the plot-config page of the right-sidepanel stack: a
  // "Curve Width" header + flow-strip, a "Curve Style" header +
  // flow-strip, then the CurveEditor (added by the caller). Sections
  // wrap as the panel narrows; below ~72 px the headers hide and the
  // icon strips stack into a 1- or 2-col snap.
  auto* outer = qobject_cast<QVBoxLayout*>(plot_config_page_->layout());
  if (outer == nullptr) {
    return;
  }

  struct ToolSpec {
    const char* object_name;
    const char* icon_path;
    const char* tooltip;
    std::function<void()> on_click;
  };
  const auto on_width = [this](double w) { return [this, w]() { applyActivePlotWidth(w); }; };
  const auto on_style = [this](int s) { return [this, s]() { applyActivePlotStyle(s); }; };

  auto build_section = [this, outer](
                           const QString& heading, const QString& header_object_name,
                           const std::vector<ToolSpec>& specs) -> QWidget* {
    // Heading: the shared titlebar-tone band (pj_widgets SectionHeaderBand,
    // themed via the PJ--SectionHeaderBand QSS class rule). The objectName is
    // kept for widget-tree selectors; the chrome-metrics handler resizes it.
    auto* header = new SectionHeaderBand(heading, plot_config_page_);
    header->setObjectName(header_object_name);
    header->onChromeMetricsChanged(chrome_metrics_);
    connect(this, &MainWindow::chromeMetricsChanged, header, &SectionHeaderBand::onChromeMetricsChanged);
    outer->addWidget(header);

    // Icon strip: FlowLayout, spacing 0 so icons sit flush with each
    // other and the chrome bands. Each button is the standard chrome
    // 24×24 with a 20×20 icon, matching every other icon in the app.
    //
    // hasHeightForWidth(true) on the size policy is what lets the strip
    // grow tall when it has to wrap — without it, the parent QVBoxLayout
    // asks for sizeHint().height() (one row's worth, 24 px), and the
    // wrapped rows render below the strip's bottom edge and get clipped.
    // Each strip wraps independently inside its own section, so icons
    // never cross a section header.
    auto* strip = new QWidget(plot_config_page_);
    QSizePolicy strip_policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    strip_policy.setHeightForWidth(true);
    strip->setSizePolicy(strip_policy);
    auto* flow = new FlowLayout(
        strip, /*margin=*/PJ::theme::space(theme::Space::None),
        /*h_spacing=*/PJ::theme::space(theme::Space::None),
        /*v_spacing=*/PJ::theme::space(theme::Space::None));
    for (const auto& spec : specs) {
      auto* btn = new SvgButton(strip);
      btn->setObjectName(QString::fromLatin1(spec.object_name));
      btn->setIconPath(QString::fromLatin1(spec.icon_path));
      btn->setExtent(chrome_metrics_.icon_size + chrome_metrics_.icon_padding, chrome_metrics_.icon_size);
      btn->setToolTip(tr(spec.tooltip));
      connect(btn, &QToolButton::clicked, this, spec.on_click);
      flow->addWidget(btn);
    }
    outer->addWidget(strip);
    return header;
  };

  curve_width_header_ = build_section(
      tr("Curve Width"), u"widgetLabelCurveWidth"_s,
      {
          {"globalWidth1_0", ":/resources/svg/line_width_1_0.svg", "Line width 1.0", on_width(1.0)},
          {"globalWidth1_5", ":/resources/svg/line_width_1_5.svg", "Line width 1.5", on_width(1.5)},
          {"globalWidth2_0", ":/resources/svg/line_width_2_0.svg", "Line width 2.0", on_width(2.0)},
          {"globalWidth3_0", ":/resources/svg/line_width_3_0.svg", "Line width 3.0", on_width(3.0)},
      });

  // Curve Width: same exclusive radio-group pattern as Curve Style.
  // Default is 1.0 (index 0); bindEditorToPlot resyncs the checked button to the
  // active plot's lineWidth(). The group's id is the LineWidth enum index (0..3);
  // the matching double is looked up from a parallel array so the click slot below
  // can call applyActivePlotWidth.
  width_button_group_ = new QButtonGroup(this);
  width_button_group_->setExclusive(true);
  const int initial_width_id = 0;
  for (int i = 0; i < static_cast<int>(kWidthButtonSpecs.size()); ++i) {
    auto* btn =
        curve_width_header_->parentWidget()->findChild<QToolButton*>(QString::fromLatin1(kWidthButtonSpecs[i].first));
    if (btn == nullptr) {
      continue;
    }
    btn->setCheckable(true);
    btn->setChecked(i == initial_width_id);
    width_button_group_->addButton(btn, i);
  }
  // Refresh any open preview panel when the width changes (the per-button slot
  // already applied it to the active plot via applyActivePlotWidth).
  connect(width_button_group_, &QButtonGroup::idClicked, this, [this](int /*width_id*/) {
    syncFilterEditorPreviewDisplay();
    syncPanelPreviewDisplay();
  });

  curve_style_header_ = build_section(
      tr("Curve Style"), u"widgetLabelCurveStyle"_s,
      {
          {"globalStyleLines", ":/resources/svg/style_lines.svg", "Lines",
           on_style(static_cast<int>(PlotWidgetBase::kLines))},
          {"globalStyleDots", ":/resources/svg/style_dots.svg", "Dots",
           on_style(static_cast<int>(PlotWidgetBase::kDots))},
          {"globalStyleLinesAndDots", ":/resources/svg/style_lines_and_dots.svg", "Lines and Dots",
           on_style(static_cast<int>(PlotWidgetBase::kLinesAndDots))},
          {"globalStyleSticks", ":/resources/svg/style_sticks.svg", "Sticks",
           on_style(static_cast<int>(PlotWidgetBase::kSticks))},
          {"globalStyleSteps", ":/resources/svg/style_steps.svg", "Steps (pre)",
           on_style(static_cast<int>(PlotWidgetBase::kSteps))},
          {"globalStyleStepsInverted", ":/resources/svg/style_steps_inverted.svg", "Steps (post)",
           on_style(static_cast<int>(PlotWidgetBase::kStepsInverted))},
      });

  // Curve Style buttons form an exclusive radio-style group: exactly one
  // is checked at any time. Default is "Lines" (kLines); bindEditorToPlot
  // resyncs the checked button to the active plot's defaultCurveStyle().
  // QButtonGroup with exclusive=true uses Qt's button-group semantics —
  // clicking the checked button is a no-op, clicking another checks it and
  // unchecks the previous.
  style_button_group_ = new QButtonGroup(this);
  style_button_group_->setExclusive(true);
  const int initial_style = static_cast<int>(PlotWidgetBase::kLines);
  const std::array<std::pair<const char*, int>, 6> style_button_specs{{
      {"globalStyleLines", static_cast<int>(PlotWidgetBase::kLines)},
      {"globalStyleDots", static_cast<int>(PlotWidgetBase::kDots)},
      {"globalStyleLinesAndDots", static_cast<int>(PlotWidgetBase::kLinesAndDots)},
      {"globalStyleSticks", static_cast<int>(PlotWidgetBase::kSticks)},
      {"globalStyleSteps", static_cast<int>(PlotWidgetBase::kSteps)},
      {"globalStyleStepsInverted", static_cast<int>(PlotWidgetBase::kStepsInverted)},
  }};
  for (const auto& [object_name, style_value] : style_button_specs) {
    auto* btn = curve_style_header_->parentWidget()->findChild<QToolButton*>(QString::fromLatin1(object_name));
    if (btn == nullptr) {
      continue;
    }
    btn->setCheckable(true);
    btn->setChecked(style_value == initial_style);
    style_button_group_->addButton(btn, style_value);
  }
  // Refresh any open preview panel when the style changes (the per-button slot
  // already applied it to the active plot via applyActivePlotStyle).
  connect(style_button_group_, &QButtonGroup::idClicked, this, [this](int /*style_value*/) {
    syncFilterEditorPreviewDisplay();
    syncPanelPreviewDisplay();
  });

  // Local-panel toolbar: each button stays button_extent square, the
  // two "Curve Width" / "Curve Style" header bands grow to band_extent
  // tall, and the section's flow-layout / strip gains layout_padding on
  // every edge so the icon strip doesn't sit flush with the panel edge.
  connect(this, &MainWindow::chromeMetricsChanged, ui_->localToolbarWidget, [this](const ChromeMetrics& metrics) {
    const int button_extent = metrics.icon_size + metrics.icon_padding;
    const int band_extent = button_extent + (2 * metrics.layout_padding);
    for (auto* btn : ui_->localToolbarWidget->findChildren<QToolButton*>()) {
      btn->setIconSize(QSize(metrics.icon_size, metrics.icon_size));
      btn->setFixedSize(button_extent, button_extent);
    }
    const QMargins band_margins(
        metrics.layout_padding, metrics.layout_padding, metrics.layout_padding, metrics.layout_padding);
    if (curve_width_header_ != nullptr) {
      curve_width_header_->setFixedHeight(band_extent);
      if (auto* layout = curve_width_header_->layout()) {
        layout->setContentsMargins(band_margins);
        layout->setSpacing(metrics.layout_spacing);
      }
    }
    if (curve_style_header_ != nullptr) {
      curve_style_header_->setFixedHeight(band_extent);
      if (auto* layout = curve_style_header_->layout()) {
        layout->setContentsMargins(band_margins);
        layout->setSpacing(metrics.layout_spacing);
      }
    }
    // Outer layout stays at 0 margins / 0 spacing (set once in
    // buildLocalToolbar). Each header band already applies layout_pad
    // internally; pushing it onto the outer too would double-pad the
    // bars — the LHS rootLayout follows the same rule.
  });
}

void MainWindow::applyActivePlotWidth(double width) {
  PlotWidget* plot = curve_editor_ != nullptr ? curve_editor_->plot() : nullptr;
  if (plot == nullptr) {
    return;
  }
  // Curve width is a plot-level property: setLineWidth re-pens every curve and
  // becomes the width any curve added later inherits. Map the toolbar's pixel
  // value to the LineWidth enum it mirrors (kWidthButtonSpecs index == enum).
  for (int i = 0; i < static_cast<int>(kWidthButtonSpecs.size()); ++i) {
    if (qFuzzyCompare(kWidthButtonSpecs[i].second, width)) {
      plot->setLineWidth(static_cast<LineWidth>(i));
      onUndoableChange();
      break;
    }
  }
}

void MainWindow::applyActivePlotStyle(int style) {
  PlotWidget* plot = curve_editor_ != nullptr ? curve_editor_->plot() : nullptr;
  if (plot == nullptr) {
    return;
  }
  // Curve style is a plot-level property: setDefaultStyle restyles every curve
  // and becomes the style any curve added later inherits (addCurve applies
  // curveStyle()).
  plot->setDefaultStyle(static_cast<PlotWidgetBase::CurveStyle>(style));
  onUndoableChange();
}

MainWindow::WrappedToolboxPanel MainWindow::wrapToolboxPanel(
    QWidget* content, const QString& title, const std::function<void()>& on_close,
    const std::function<void()>& on_migrate, std::function<bool()> has_work_in_flight,
    const std::function<void()>& on_fold_busy) {
  auto* container = new QWidget;
  container->setObjectName(QStringLiteral("toolboxPanelContainer"));
  auto* column = new QVBoxLayout(container);
  column->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  column->setSpacing(PJ::theme::space(theme::Space::None));

  // Banner header (Surface::Banner): title far-left, close far-right — mirrors
  // the PJ::Dialog title bar so a docked toolbox reads like every app dialog.
  auto* banner = new QWidget(container);
  banner->setObjectName(QStringLiteral("toolboxBanner"));
  auto* row = new QHBoxLayout(banner);
  // No left inset: the title leads via toolboxBannerTitle's own canonical
  // padding-left (Tight), matching every other section band's leading.
  row->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  row->setSpacing(PJ::theme::space(theme::Space::None));

  auto* title_label = new QLabel(title, banner);
  title_label->setObjectName(QStringLiteral("toolboxBannerTitle"));
  row->addWidget(title_label);
  row->addStretch(1);

  // "Migrate to tab" pins the toolbox as a persistent central tab instead of
  // the ephemeral chart-area takeover. Every toolbox gets it — the button is
  // host chrome, so plugins need no awareness of the gesture. SvgButton
  // self-retints on theme change, no wiring needed.
  auto* migrate_button = new SvgButton(u":/resources/svg/tab_move.svg"_s, SvgButton::Size::kDefault, banner);
  migrate_button->setObjectName(u"buttonMigrateTab"_s);
  migrate_button->setCursor(Qt::PointingHandCursor);
  migrate_button->setToolTip(tr("Move to a tab"));

  auto* close_button = new QToolButton(banner);
  close_button->setObjectName(QStringLiteral("buttonClose"));

  row->addWidget(migrate_button);

  // A tagged plugin action stays in the content tree for its existing routing
  // while the host gives it a domain-neutral place in takeover chrome.
  std::vector<QPointer<SvgButton>> chrome_action_proxies;
  for (auto* src : content->findChildren<QAbstractButton*>()) {
    if (src == nullptr || !src->property("pjToolboxChromeAction").toBool()) {
      continue;
    }
    QString icon_path = src->property("chromeActionIcon").toString();
    if (icon_path.isEmpty()) {
      icon_path = u":/resources/svg/help.svg"_s;
    }
    auto* proxy = new SvgButton(icon_path, SvgButton::Size::kDefault, banner);
    proxy->setToolTip(src->toolTip());
    proxy->setCursor(Qt::PointingHandCursor);
    chrome_action_proxies.emplace_back(proxy);
    row->insertWidget(row->indexOf(migrate_button), proxy);
    connect(proxy, &QAbstractButton::clicked, this, [src = QPointer<QAbstractButton>(src)]() {
      if (!src.isNull()) {
        src->click();
      }
    });
    src->hide();
  }

  // Banner + buttons ride the canonical band height so a docked toolbox
  // reads at the same height as every section band and every chrome button,
  // and rescales with the icon size. Seed from the current metrics, then keep
  // in step via the chromeMetricsChanged broadcast.
  const auto size_banner = [banner, migrate_button, close_button, chrome_action_proxies](const ChromeMetrics& metrics) {
    banner->setFixedHeight(metrics.bandHeight());
    migrate_button->setExtent(metrics.bandHeight(), metrics.icon_size);
    for (const auto& proxy : chrome_action_proxies) {
      if (!proxy.isNull()) {
        proxy->setExtent(metrics.bandHeight(), metrics.icon_size);
      }
    }
    close_button->setFixedSize(metrics.bandHeight(), metrics.bandHeight());
    close_button->setIconSize(QSize(metrics.icon_size, metrics.icon_size));
  };
  size_banner(chrome_metrics_);
  connect(this, &MainWindow::chromeMetricsChanged, banner, size_banner);

  // Pinning strips the takeover-only banner buttons — the tab frame provides
  // name + close. Owned here because only this function knows which banner
  // widgets are takeover chrome.
  const auto enter_pinned_chrome = [migrate_button, close_button, chrome_action_proxies]() {
    migrate_button->hide();
    for (const auto& proxy : chrome_action_proxies) {
      if (!proxy.isNull()) {
        proxy->hide();
      }
    }
    close_button->hide();
  };
  connect(migrate_button, &QToolButton::clicked, this, [enter_pinned_chrome, on_migrate]() {
    enter_pinned_chrome();
    on_migrate();
  });

  close_button->setAutoRaise(true);
  close_button->setFocusPolicy(Qt::NoFocus);
  close_button->setCursor(Qt::PointingHandCursor);
  close_button->setToolTip(tr("Close"));
  const auto tint_close = [close_button](const QString& theme) {
    close_button->setIcon(loadSvg(QStringLiteral(":/resources/svg/close_windows_light.svg"), theme));
  };
  tint_close(theme_->currentTheme());
  connect(this, &MainWindow::stylesheetChanged, close_button, tint_close);
  // Rules 1-2: with work in flight the X folds the panel into a background tab
  // (uncovering the chart area for the series the job is publishing) instead of
  // tearing it down, which would cancel that job. The fold takes `on_fold_busy`
  // — a host-chosen disposition (transient, kept out of layout save), because a
  // user who pressed CLOSE chose a pin even less than an auto-fold did — not
  // the migrate button's user-pin path.
  connect(
      close_button, &QToolButton::clicked, this,
      [on_close, on_migrate, on_fold_busy, enter_pinned_chrome, has_work_in_flight = std::move(has_work_in_flight)]() {
        if (has_work_in_flight && has_work_in_flight()) {
          enter_pinned_chrome();
          (on_fold_busy ? on_fold_busy : on_migrate)();
          return;
        }
        on_close();
      });
  row->addWidget(close_button);

  column->addWidget(banner);
  column->addWidget(content, /*stretch=*/1);
  return {.container = container, .enter_pinned_chrome = enter_pinned_chrome};
}

bool MainWindow::presentPanel(QWidget* panel) {
  if (panel == nullptr) {
    return false;
  }
  // A panel is already presented: dismiss it so launching a new toolbox/panel
  // replaces the open one instead of refusing.
  dismissTakeoverPanel();
  // The incoming panel owns the takeover from here; whoever launches it
  // registers its own fold.
  takeover_fold_ = {};

  // The chart area (ui_->tabbedPlotWidget) lives as a direct child of a
  // QSplitter in MainWindow.ui. Swap the panel into the chart's splitter slot
  // and remember the slot so restoreCentralArea() can put the chart back.
  QWidget* chart = ui_->tabbedPlotWidget;
  panel_parent_ = chart->parentWidget();
  auto* splitter = qobject_cast<QSplitter*>(panel_parent_);
  if (splitter == nullptr) {
    qWarning("MainWindow::presentPanel: tabbedPlotWidget is not in a QSplitter");
    panel_parent_ = nullptr;
    return false;
  }
  panel_layout_index_ = splitter->indexOf(chart);
  if (panel_layout_index_ < 0) {
    qWarning("MainWindow::presentPanel: chart not in splitter");
    panel_parent_ = nullptr;
    return false;
  }
  // Swap the panel into the chart's exact splitter slot via replaceWidget so the
  // pane count stays constant and the saved size list still lines up (an
  // insert+hide would leave N+1 panes against an N-entry size list). replaceWidget
  // hands the chart back to us, reparented out of the splitter; keep it hidden so
  // restoreCentralArea can swap it back into the same slot.
  const QList<int> saved_sizes = splitter->sizes();
  QWidget* removed = splitter->replaceWidget(panel_layout_index_, panel);
  if (removed != chart) {
    qWarning("MainWindow::presentPanel: unexpected widget at chart slot; aborting swap");
    if (removed != nullptr) {
      splitter->replaceWidget(panel_layout_index_, removed);
    }
    panel_parent_ = nullptr;
    panel_layout_index_ = -1;
    return false;
  }
  chart->hide();
  panel->show();
  // The incoming panel inherits the chart's role as the pane that absorbs every
  // width change, not just its slot. Without this the sidebar starts growing
  // again the moment the window is resized with a panel open — replaceWidget
  // carries no stretch factor over from the widget it displaced.
  splitter->setStretchFactor(panel_layout_index_, 1);
  splitter->setSizes(saved_sizes);
  current_panel_ = panel;
  return true;
}

QWidget* MainWindow::releaseCentralPanel() {
  // The panel stops being the takeover here, whichever exit it takes, so its
  // fold registration must not outlive this call. foldTakeoverPanelIfOwnedBy
  // moves the closure out before invoking it, so this never destroys a
  // std::function that is mid-execution.
  takeover_fold_ = {};
  if (current_panel_ == nullptr) {
    return nullptr;
  }
  auto* splitter = qobject_cast<QSplitter*>(panel_parent_);
  if (splitter == nullptr || panel_layout_index_ < 0) {
    qWarning("MainWindow::releaseCentralPanel: panel_parent_ is no longer a splitter; chart not restored to slot");
    ui_->tabbedPlotWidget->show();
  } else {
    // Swap the chart back into its slot; replaceWidget removes the panel and
    // hands it back reparented out of the splitter.
    const QList<int> saved_sizes = splitter->sizes();
    splitter->replaceWidget(panel_layout_index_, ui_->tabbedPlotWidget);
    // Show the chart before restoring the sizes: a hidden pane is left out of
    // the splitter's layout, so sizes applied while the chart is still hidden
    // are recomputed the instant it appears — and the sidebar keeps the slack.
    ui_->tabbedPlotWidget->show();
    splitter->setSizes(saved_sizes);
  }
  QWidget* released = current_panel_;
  released->hide();
  released->setParent(nullptr);
  current_panel_ = nullptr;
  filter_editor_origin_ = nullptr;  // no Filter Editor preview to drive once the panel is gone
  // The engine (when this was a toolbox panel) is owned by whoever tore the
  // panel down (the onCloseRequested handler, presentPanel's replace path, or
  // the migrate gesture); here we only drop our non-owning reference.
  current_panel_engine_ = nullptr;
  panel_layout_index_ = -1;
  panel_parent_ = nullptr;
  return released;
}

void MainWindow::restoreCentralArea() {
  if (QWidget* released = releaseCentralPanel()) {
    released->deleteLater();
  }
}

void MainWindow::dismissTakeoverPanel() {
  if (current_panel_ == nullptr) {
    return;
  }
  // Rule 2's other door: launching any other panel goes through here, and for a
  // panel with a job in flight the teardown below is that job's kill switch.
  // Fold instead. If the fold does not take (no registration, or the panel is
  // already gone) fall through so the takeover is never left stuck on screen.
  if (takeoverHasWorkInFlight()) {
    foldTakeoverPanelIfOwnedBy(takeover_fold_.owner, /*transient=*/true);
    if (current_panel_ == nullptr) {
      return;
    }
  }
  // For a toolbox panel, close its PanelEngine first (same teardown as the
  // reject path) so its host/handle are released; restoreCentralArea() then
  // swaps the chart back and clears the panel state. Non-toolbox panels
  // (null engine) just restore.
  PanelEngine* previous_engine = current_panel_engine_;
  if (previous_engine != nullptr) {
    previous_engine->close();
  }
  restoreCentralArea();
  if (previous_engine != nullptr) {
    previous_engine->deleteLater();
  }
}

void MainWindow::setTakeoverFold(
    const void* owner, std::function<void(bool transient)> fold, std::function<bool()> has_work_in_flight) {
  takeover_fold_ = {.owner = owner, .fold = std::move(fold), .has_work_in_flight = std::move(has_work_in_flight)};
}

bool MainWindow::takeoverHasWorkInFlight() const {
  return takeover_fold_.has_work_in_flight && takeover_fold_.has_work_in_flight();
}

void MainWindow::foldTakeoverPanelIfOwnedBy(const void* owner, bool transient) {
  if (current_panel_ == nullptr || owner == nullptr || takeover_fold_.owner != owner || !takeover_fold_.fold) {
    return;
  }
  // Move the closure out first: folding reaches releaseCentralPanel(), which
  // drops the registration and would otherwise destroy the very std::function
  // running here.
  const std::function<void(bool)> fold = std::move(takeover_fold_.fold);
  takeover_fold_ = {};
  fold(transient);
}

bool MainWindow::hostHasWorkInFlight(ToolboxRuntimeHost* host) const {
  if (host_work_in_flight_) {
    return host_work_in_flight_(host);
  }
  return host != nullptr && host->hasActiveIngests();
}

void MainWindow::stopHostWork(ToolboxRuntimeHost* host) {
  if (stop_host_work_) {
    stop_host_work_(host);
    return;
  }
  if (host != nullptr) {
    host->requestStopActiveIngests();
  }
}

bool MainWindow::confirmCancelRunningJob(const QString& label) {
  if (confirm_running_job_) {
    return confirm_running_job_(label);
  }
  // No default button: closing here cancels a transfer the user may have been
  // waiting on for minutes, so an accidental Enter must not do it. Esc /
  // dismiss keeps downloading.
  constexpr int kCancelAndClose = 0;
  const int choice = MessageBox::question(
      this, label.isEmpty() ? tr("Cancel download?") : label, tr("A download is in progress. Cancel it and close?"),
      // "&&" so Qt renders a literal ampersand instead of eating it as a mnemonic.
      {{tr("Cancel download && close"), MessageBox::kDestructiveRole},
       {tr("Keep downloading"), MessageBox::kCancelRole}});
  return choice == kCancelAndClose;
}

void MainWindow::openEmbeddedConsole() {
#ifdef PJ_TARGET_WASM
  qWarning("MainWindow::openEmbeddedConsole: external helper processes are unavailable in the browser");
#else
  auto* view = new RasterStreamView(this);
  view->setKeyTranslator(&engineKeyForQtKey);
  if (!presentPanel(view)) {
    view->deleteLater();
    return;
  }
  connect(view, &RasterStreamView::sessionEnded, this, [this]() { restoreCentralArea(); });
  const QString dir = QCoreApplication::applicationDirPath() + u"/3rdparty/retro/"_s;
  QString helper = QStandardPaths::findExecutable(u"pj-raster-helper"_s, {dir});
  if (helper.isEmpty()) {
    helper = dir + u"pj-raster-helper"_s;
  }
  view->start(helper, dir + u"base.wad"_s);
#endif
}

void MainWindow::launchToolbox(
    const QString& plugin_id, const QString& initial_config, ToolboxLaunchTarget target, const QString& pin_tab_name) {
  // Surface every failure on the diagnostic channel (the same sink the toolbox's
  // own on_message uses below), not just stderr, so a user-initiated launch that
  // fails is visible in the UI instead of silently doing nothing.
  auto report_error = [this](const QString& source, const QString& detail) {
    if (diagnostic_history_ != nullptr) {
      diagnostic_history_->record(DiagnosticLevel::kError, source, u"toolbox"_s, detail);
    }
    qWarning("MainWindow::launchToolbox: %s", qPrintable(detail));
  };

  // 0. One live instance per toolbox id: launching an already-pinned toolbox
  //    focuses its tab — dismissing any open takeover first, which would
  //    otherwise hide the tab strip the focus lands in. (Takeover relaunches
  //    keep the presentPanel replace semantics: the open panel is torn down
  //    and the toolbox starts fresh.)
  if (auto pinned_it = pinned_toolboxes_.constFind(plugin_id); pinned_it != pinned_toolboxes_.constEnd()) {
    if (!pinned_it->container.isNull()) {
      if (initial_config.isEmpty()) {
        dismissTakeoverPanel();
        ui_->tabbedPlotWidget->focusWidgetTab(pinned_it->container);
        return;
      }
      // An in-place edit (non-empty initial_config) must reach loadConfig()
      // BEFORE the dialog is built, which only a fresh instance can do:
      // relaunch the pinned toolbox with the config, keeping its tab surface
      // and (possibly renamed) label. Closing erases the registry entry, so
      // the recursive call takes the normal build path.
      const QString pinned_name = ui_->tabbedPlotWidget->widgetTabName(pinned_it->container);
      ui_->tabbedPlotWidget->closeWidgetTab(pinned_it->container);
      // A close the tab declined keeps the live instance, so relaunching would
      // breach the one-instance-per-toolbox rule; the surviving registry entry
      // (erased by a completed teardown) is the signal.
      if (pinned_toolboxes_.contains(plugin_id)) {
        return;
      }
      launchToolbox(plugin_id, initial_config, ToolboxLaunchTarget::kPinnedTab, pinned_name);
      return;
    }
    pinned_toolboxes_.remove(plugin_id);
  }
  if (target == ToolboxLaunchTarget::kPinnedTab) {
    // Pinning directly (layout restore): the takeover surface must not
    // survive — it hides the tab strip the new tab lives in, and it may BE
    // this same plugin, which must not end up with two live instances.
    dismissTakeoverPanel();
  }

  // 1. Find the toolbox in the catalog.
  const auto& toolboxes = session_->extensionCatalog().toolboxes();
  auto it = std::find_if(toolboxes.begin(), toolboxes.end(), [&plugin_id](const RuntimeToolboxPlugin& tb) {
    return QString::fromStdString(tb.id) == plugin_id;
  });
  if (it == toolboxes.end()) {
    report_error(plugin_id, tr("Cloud toolbox '%1' not found in catalog").arg(plugin_id));
    return;
  }

  // 2. Assemble the host services + toolbox handle into one owner whose member
  //    order *guarantees* teardown order. The plugin's destructor persists its
  //    state through the SettingsStoreHost, so the handle (declared last ->
  //    destroyed first) must be torn down while the host + settings backend it
  //    persists through are still alive. Lambda capture-destruction order is
  //    unspecified, so a struct (reverse-declaration destruction) is required.
  //
  //    KEEP IN SYNC: HeadlessDescriptorProviderSession::create mirrors this
  //    whole service-assembly block for the dialog-free layout-import path (a
  //    provider plugin must see an identical host environment headless vs
  //    interactive). Any service newly registered here — or any change to the
  //    callbacks' host-side semantics — must be applied there too; the
  //    verbatim-shared snippets live in ToolboxHostWiring.h.
  struct PanelSession {
    std::unique_ptr<QSettingsBackend> settings;
    std::unique_ptr<ServiceRegistryBuilder> builder;
    std::unique_ptr<ToolboxRuntimeHost> host;
    std::unique_ptr<MarkersRuntimeHost> markers_host;
    std::unique_ptr<DataProcessorsRuntimeHost> dp_host;
    std::unique_ptr<DataProcessorsKindRouter> dp_router;
    std::unique_ptr<SourcePromotionHost> promotion_host;
    std::shared_ptr<ToolboxHandle> handle;

    // Teardown order is load-bearing, so make it explicit here rather than relying
    // on member-declaration order alone. Two-phase promotion teardown (Codex
    // r1 F5): shutdown() FIRST — it fails every accepted-but-unfinished
    // promotion callback, which reaches plugin code, so the plugin
    // instance/DSO (handle) must still be alive — but the host OBJECT stays
    // alive until handle.reset() has destroyed the plugin instance (which
    // joins its workers): unlike the headless session, the interactive path
    // has no job registry to join, so a straggler provider worker may still
    // call the raw service pointer in that window and must hit a LIVE mutex
    // and the safe synchronous shutting_down_ rejection, not a
    // use-after-free. Then the plugin (handle), which persists its state
    // through the settings backend in its destructor; only now the promotion
    // host; then the service views (builder) into host/dp_host/settings,
    // then the bridge + host. dp_host holds only an external
    // DataProcessorService& (session-owned, outlives this), so it just has
    // to outlive the builder views into it; host holds a SettingsBackend&,
    // so settings goes last. This survives a future member reorder; the
    // implicit reverse-declaration destruction that follows only resets
    // already-null pointers.
    ~PanelSession() {
      if (promotion_host != nullptr) {
        promotion_host->shutdown();
      }
      handle.reset();
      promotion_host.reset();
      builder.reset();       // drops the service views into host + the routed bridges first
      dp_router.reset();     // holds fat pointers into markers_host + dp_host, so goes before them
      markers_host.reset();  // bridge only; the generators live on in MarkerService
      dp_host.reset();
      host.reset();
      settings.reset();
    }
  };
  auto session = std::make_shared<PanelSession>();
  session->settings = std::make_unique<QSettingsBackend>();
  session->builder = std::make_unique<ServiceRegistryBuilder>();

  const QString source = it->name.empty() ? plugin_id : QString::fromStdString(it->name);
  ToolboxRuntimeHost::Callbacks callbacks;
  const std::string plugin_id_std = plugin_id.toStdString();
  callbacks.on_data_changed = [this, plugin_id_std](std::vector<DatasetId> ingested_datasets) {
    session_->catalogModel().rebuildFromDatastore();
    // FileLoader parity: re-scan each ingested dataset's time reference. Without
    // this the "use time offset" origin memo can stay pinned at the 0 it
    // acquired before any data existed (setUseTimeOffset queries
    // globalTimeReference() eagerly), so cloud-imported series keep an ABSOLUTE
    // epoch axis and the t0 toggle looks dead — files never hit it because the
    // file loader always refreshes.
    for (const DatasetId id : ingested_datasets) {
      session_->sessionManager().refreshDatasetTimeReference(id);
    }
    // Publish worker-fed toolbox TF after the catalog exposes its object topics.
#ifdef PJ_WITH_SCENE3D
    if (transform_service_ != nullptr) {
      for (const DatasetId id : ingested_datasets) {
        transform_service_->publishTransforms(id);
      }
    }
#else
    Q_UNUSED(ingested_datasets);
#endif
    // A parser-ingest import (the cloud connector's fetch) gets FOCUS
    // semantics: the timeline snaps to the imported data so a 10s snippet
    // plays back as 10s — the monotonic union would bury it inside whatever
    // range earlier fetches/loads established. Plain write-API toolboxes
    // (empty list) keep the union seeding.
    if (ingested_datasets.empty() || !session_->focusPlaybackOnDatasets(ingested_datasets)) {
      session_->seedPlaybackFromSession();
    }
    // A toolbox write may have added/removed markers; repaint plot overlays.
    session_->sessionManager().notifyMarkersChanged();
    // Surface this plugin's transform outputs in the Custom Series panel (flat,
    // not in the main data tree).
    auto& dps = session_->sessionManager().dataProcessorService();
    for (const auto& recipe : dps.transformRecipes()) {
      if (recipe.owner_plugin != plugin_id_std) {
        continue;
      }
      for (const auto& output_name : recipe.outputs) {
        const QString out = QString::fromStdString(output_name);
        for (const auto& item : session_->catalogModel().items()) {
          if (item.topic_name == out) {
            ui_->curveListPanel->addCustomCurve(item.key, out);
            break;
          }
        }
      }
    }
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
    qInfo(
        "PJ_WASM_TOOLBOX_TRANSFORM_READY plugin=%s recipes=%llu", plugin_id_std.c_str(),
        static_cast<unsigned long long>(dps.transformRecipes().size()));
#endif
  };
  callbacks.on_message = [this, source](PJ_toolbox_message_level_t level, std::string message) {
    if (diagnostic_history_ == nullptr) {
      return;
    }
    diagnostic_history_->record(toolboxDiagnosticLevel(level), source, u"toolbox"_s, QString::fromStdString(message));
  };

  // Progressive bulk-import surface (all GUI-thread, marshalled by the host).
  // Lifecycle BOOKKEEPING lives in SessionManager (begin/update/endIngest —
  // #470 hoist); these callbacks forward to it and keep only presentation:
  // the strip widgets, FileLoader-vs-toolbox arbitration, and stop routing.
  // Weak capture: these callbacks are owned by the host inside the very
  // PanelSession they reference, so a strong capture would leak the session;
  // an expired owner (panel closed mid-import) simply drops the tick.
  const std::weak_ptr<void> session_weak = session;
  callbacks.on_ingest_started = [this, session_weak](
                                    DatasetId dataset, std::string label, uint64_t total, bool cancellable) {
    const auto owner = session_weak.lock();
    if (owner == nullptr) {
      return;
    }
    const QString import_label = QString::fromStdString(label);
    // The plugin declares whether its import can be stopped, and the row's stop
    // affordance follows that declaration. Registered as kStopOnly because the
    // stop is cooperative with no host-side rollback: the import always keeps
    // what arrived, so the discard affordance is presented as unavailable
    // rather than silently downgraded into a keep.
    // The outcome is always kUnknown since the plugin C-ABI reports no outcome.
    const std::weak_ptr<void> stop_owner = session_weak;
    const auto token = session_->sessionManager().beginIngest(
        dataset, import_label, total, cancellable,
        [this, dataset, stop_owner](bool) {
          if (stop_owner.lock() == nullptr) {
            return;  // the panel that owned this import is gone
          }
          const auto import_it = toolbox_active_imports_.constFind(dataset);
          if (import_it != toolbox_active_imports_.constEnd() && import_it->host != nullptr) {
            // ONLY this dataset's context. One panel commonly imports several
            // datasets through the same host, and stopping the host's whole
            // ingest set ended every sibling too — each reported kUnknown from
            // a row its user never touched.
            import_it->host->requestStopIngestForDataset(dataset);
          }
        },
        IngestStop::kStopOnly);

    // Lifecycle state (which datasets are importing, label/total) is the
    // session's; recorded even when FileLoader owns the strip so
    // started/finished stay paired and the import can re-adopt the strip later.
    // Store the token per dataset for progress and termination.
    toolbox_active_imports_.insert(
        dataset, ToolboxIngestRef{token, owner, std::static_pointer_cast<PanelSession>(owner)->host.get()});
    toolbox_ingest_label_ = import_label;
    toolbox_ingest_dataset_ = dataset;
    // The panel that just started importing must stop covering the chart area:
    // it publishes rows mid-batch and those series have to be reachable while
    // the batch runs. Folding keeps the instance — and the download — alive.
    // Ahead of the strip arbitration below, which is about presentation only.
    foldTakeoverPanelIfOwnedBy(owner.get(), /*transient=*/true);
    if (file_loader_->isBusy()) {
      return;  // FileLoader wins the strip; the import still runs, just undisplayed
    }
    adoptToolboxIngestStrip();
  };
  callbacks.on_ingest_progress = [this](DatasetId dataset, uint64_t current, uint64_t total) {
    // Toolbox analog of FileLoader::publishIngestProgress — the host
    // flushed pending rows before this fired. updateIngest publishes them to
    // plots/playback through notifyIngest (the catalog tree grows via
    // samplesIngested -> rebuildIfChanged) and records progress; the worker-fed
    // TF revision is published below. The full catalog rebuild + playback focus
    // stay on notify_data_changed.

    // Publish first, and for the dataset the host named — not for whatever the
    // imports map happens to hold. A miss here is a bookkeeping gap (ordering, a
    // panel closed mid-import), never evidence that the rows the host just
    // flushed are not real; returning early on it dropped both the data publish
    // and the scene's TF refresh for a dataset that was genuinely importing.
    // By VALUE, never a held iterator: both publishes below emit synchronously
    // into arbitrary observers, and one of them starting or finishing an import
    // rehashes toolbox_active_imports_ under us.
    const auto tracked_import = toolbox_active_imports_.constFind(dataset);
    const std::optional<IngestToken> tracked_token = tracked_import != toolbox_active_imports_.constEnd()
                                                         ? std::optional<IngestToken>(tracked_import->token)
                                                         : std::nullopt;
    if (tracked_token.has_value()) {
      session_->sessionManager().updateIngest(*tracked_token, current, total);
    } else {
      const auto ids = session_->sessionManager().dataEngine().listTopics(dataset);
      session_->sessionManager().notifyIngest(QVector<TopicId>(ids.begin(), ids.end()), /*live=*/false);
    }
#ifdef PJ_WITH_SCENE3D
    if (transform_service_ != nullptr) {
      transform_service_->publishTransforms(dataset);
    }
#endif
    // Only the strip presentation below needs a tracked import, and it must be
    // re-checked: the publishes above may have retired this one.
    if (!tracked_token.has_value() || !toolbox_active_imports_.contains(dataset)) {
      return;
    }
    if (file_loader_->isBusy() || !session_->sessionManager().hasActiveIngests()) {
      return;
    }
    if (ingest_strip_owner_kind_ != IngestStripOwnerKind::kInteractiveToolbox &&
        ingest_strip_owner_kind_ != IngestStripOwnerKind::kLayoutBatch) {
      adoptToolboxIngestStrip();  // the file load that owned the strip has drained
    }
    if (ingest_strip_owner_dataset_ != dataset) {
      return;  // another producer owns the display (D8); its ticks drive the bar
    }
    setIngestStripProgress(current, total);
  };
  callbacks.on_ingest_finished = [this](DatasetId dataset) {
    // Look up the token for this dataset and end with kUnknown outcome
    // (the plugin C-ABI reports no outcome).
    auto active_import_it = toolbox_active_imports_.find(dataset);
    if (active_import_it != toolbox_active_imports_.end()) {
      session_->sessionManager().endIngest(active_import_it->token, IngestOutcome::kUnknown);
    }
    toolbox_active_imports_.remove(dataset);
    if (toolbox_ingest_dataset_ == dataset) {
      // The last-started identity is gone; survivor picks fall back to the
      // lowest surviving id.
      toolbox_ingest_dataset_ = 0;
      toolbox_ingest_label_.clear();
    }
    // D8: a displayed import that ends hands the strip to a survivor (its own
    // label/progress) or runs the linger-hide; an undisplayed one changes
    // nothing on screen.
    releaseIngestStripOwner(dataset);
  };

  // Parser-ingest deps: catalog lookup plus synchronous worker-safe parser
  // registration shared with the headless path (see ToolboxHostWiring.h).
  ToolboxRuntimeHost::ParserIngestDeps ingest_deps;
  ingest_deps.catalog = &session_->extensionCatalog();
  ingest_deps.ingest_taps = session_->sessionManager().ingestTapsShared();
  ingest_deps.register_object_parser = makeObjectParserRegistrar(session_->sessionManager());
#ifndef __EMSCRIPTEN__
  // M3: capture this binding's delegated downloads into the source cache and
  // attach the SourceRecord layout save persists (plugin_id is the catalog's
  // stable toolbox id — the unspoofable provider identity).
  wireSourceCapture(ingest_deps, source_capture_service_.get(), plugin_id.toStdString(), session_->sessionManager());
#endif

  session->host = std::make_unique<ToolboxRuntimeHost>(
      session_->sessionManager().dataEngine(), session_->sessionManager().objectStore(), *session->settings,
      std::move(callbacks), std::move(ingest_deps));
  if (auto status = session->host->registerServices(*session->builder); !status) {
    report_error(
        source,
        tr("Failed to publish toolbox services for '%1': %2").arg(source, QString::fromStdString(status.error())));
    return;
  }

  // Host-driven data processors (pj.data_processors.v1, kind=markers): expose the marker
  // service so a toolbox can submit whole-series generators the HOST runs + recomputes
  // (the anomaly-detector path) instead of executing the script in-process. The service's
  // catalog-backed resolver/lister are installed once at construction, not here — see
  // the MainWindow constructor.
  session->markers_host = std::make_unique<MarkersRuntimeHost>(
      session_->sessionManager().markerService(), plugin_id.toStdString(),
      [this](const std::vector<std::string>& inputs) -> PJ::DatasetId {
        // Bind the generator to the dataset that actually holds its input series,
        // not just the first-loaded one (which stranded generators on the wrong
        // dataset when several are open). Fall back to the first dataset when the
        // inputs name no known series or are ambiguous across datasets.
        for (const std::string& key : inputs) {
          std::optional<PJ::DatasetId> found;
          bool ambiguous = false;
          for (const PJ::CatalogItem& item : session_->catalogModel().items()) {
            if (markerSeriesField(item, key) == nullptr) {
              continue;
            }
            if (found.has_value() && *found != item.dataset_id) {
              ambiguous = true;
              break;
            }
            found = item.dataset_id;
          }
          if (found.has_value() && !ambiguous) {
            return *found;
          }
        }
        const std::vector<std::pair<PJ::DatasetId, QString>> datasets = session_->catalogModel().datasets();
        return datasets.empty() ? PJ::DatasetId{0} : datasets.front().first;
      });
  session->dp_host = std::make_unique<DataProcessorsRuntimeHost>(
      session_->sessionManager().dataProcessorService(), plugin_id.toStdString());
  // ONE pj.data_processors.v1 registration, routed by kind: the registry rejects
  // duplicate names, so registering both bridges directly would silently drop the
  // second one and break that kind's toolboxes.
  session->dp_router =
      std::make_unique<DataProcessorsKindRouter>(session->markers_host->raw(), session->dp_host->raw());
  if (auto status = session->dp_router->registerServices(*session->builder); !status) {
    report_error(
        source, tr("Failed to publish data-processor services for '%1': %2")
                    .arg(source, QString::fromStdString(status.error())));
    return;
  }

  // Source promotion ("pj.source_promotion.v1"), bound per toolbox instance:
  // the provider identity is THIS binding's stable manifest id (host-derived,
  // unspoofable) and the ownership predicate is this instance's own ingest
  // bookkeeping — a promotion can only ever target a dataset this plugin's
  // ingest produced. `host` outlives promotion_host (see ~PanelSession), so
  // the captured raw pointer stays valid for the predicate's whole life.
  session->promotion_host = std::make_unique<SourcePromotionHost>(
      *file_loader_, session_->sessionManager(), QString::fromStdString(it->id),
      [host = session->host.get()](DatasetId dataset_id) { return host->hasIngestForDataset(dataset_id); });
  if (auto status = session->promotion_host->registerServices(*session->builder); !status) {
    report_error(
        source, tr("Failed to publish the source-promotion service for '%1': %2")
                    .arg(source, QString::fromStdString(status.error())));
    return;
  }

  // 3. Create the toolbox instance and bind it to the assembled services.
  session->handle = std::make_shared<ToolboxHandle>(it->library.createHandle());
  if (auto status = session->handle->bind(session->builder->view()); !status) {
    report_error(source, tr("Failed to bind toolbox '%1': %2").arg(source, QString::fromStdString(status.error())));
    return;
  }
  // Pre-populate for an in-place edit (Transform Editor pencil button): hand the
  // saved editor state to the toolbox before its dialog is built.
  if (!initial_config.isEmpty()) {
    (void)session->handle->loadConfig(initial_config.toStdString());
  }

  // 4. Host the toolbox's dialog in a PanelEngine.
  const PJ_borrowed_dialog_t borrowed = session->handle->getDialog();
  if (borrowed.vtable == nullptr || borrowed.ctx == nullptr) {
    report_error(source, tr("Toolbox '%1' returned no dialog").arg(source));
    return;
  }
  // The curve tree drags opaque catalog keys ("dataset:N/topic:M/column:K"); the
  // toolbox expects human field names ("topic/field"). CatalogModel owns that
  // mapping, so resolve dropped keys to names before they reach onItemsDropped.
  PanelEngineConfig panel_config;
#ifdef Q_OS_WASM
  // The Transform Editor defers its eager preview rebuild to on_tick. Restarting
  // the deadline after each browser-delivered edit both debounces typing and
  // guarantees one post-edit wake-up on Qt's WASM event dispatcher. Desktop
  // retains PanelEngine's existing fixed periodic cadence.
  panel_config.restart_tick_timer_on_event = plugin_id_std == "toolbox-transform-editor";
#endif
  // Hand the panel a session + catalog so its chart_series previews render with the
  // full PlotWidget (grid/zoom/tracker/legend) — matching the native editor — instead
  // of falling back to the bare ChartPreviewWidget.
  panel_config.session = session_.get();
  panel_config.catalog = &session_->catalogModel();
  panel_config.catalog_key_resolver = [this](const std::string& key) -> std::string {
    auto descriptor = session_->catalogModel().curveDescriptor(QString::fromStdString(key));
    if (!descriptor) {
      return {};
    }
    // Shared key builder so the producer and the plot overlay agree (and avoid a
    // doubled "//" when the field path already starts with '/').
    return sdk::markerSeriesKey(descriptor->topic_name.toStdString(), descriptor->field_name.toStdString());
  };
  auto* engine = new PanelEngine(DialogHandle::fromBorrowed(borrowed), panel_config, this);
  QWidget* panel = engine->openPanel();
  if (panel == nullptr) {
    report_error(source, tr("Failed to build the panel UI for '%1'").arg(source));
    delete engine;
    return;
  }

  // QLineEdit (and friends) accept drops by default, so a drop landing on a field
  // gets delivered there and never bubbles to the panel-root DropEventFilter.
  // Clear acceptDrops on every descendant so any drop inside the panel reaches
  // the root filter, which maps it to the declared drop target.
  for (QWidget* child : panel->findChildren<QWidget*>()) {
    child->setAcceptDrops(false);
  }

  // A plugin .ui's SectionHeaderBand (e.g. the FFT "Frequencies" band) is
  // inflated by QUiLoader at the widget default height and has no wiring to the
  // app. Route the chrome-metrics broadcast into it so it matches every other
  // section band and the toolbox banner above, and rescales with the icon size.
  for (auto* band : panel->findChildren<SectionHeaderBand*>()) {
    band->onChromeMetricsChanged(chrome_metrics_);
    connect(this, &MainWindow::chromeMetricsChanged, band, &SectionHeaderBand::onChromeMetricsChanged);
  }

  // 5. Close -> restore + teardown. The captured session keeps the services +
  //    plugin alive until the panel is gone; deleteLater defers the teardown
  //    (incl. the handle's worker-thread join) past any in-flight signals, and
  //    PanelSession's member order destroys the plugin before the settings host
  //    it persists through.
  engine->onCloseRequested([this, engine, session](const std::string& /*reason*/) {
    (void)session;
    restoreCentralArea();
    engine->deleteLater();
    return true;  // the takeover is gone; let the engine tear itself down
  });

  // 6. Wrap the panel in the canonical Banner header (title left, migrate +
  //    close right) and present it in the chart area. The banner close runs
  //    the same host-initiated teardown as presentPanel's replace path; the
  //    migrate button lifts the live panel out of the takeover and pins it as
  //    a central tab. save_config keeps the plugin session alive while pinned
  //    (the registry holds it) and reads its config for layout save.
  auto save_config = [session]() -> QString {
    std::string config_json;
    if (session->handle == nullptr || !session->handle->saveConfig(config_json)) {
      return {};
    }
    return QString::fromStdString(config_json);
  };
  // Lifting the live panel out of the takeover and into a tab: shared by the
  // migrate button, the banner X while work is in flight, and the auto-fold on
  // ingest start. `transient` distinguishes a fold the host chose from a pin the
  // user asked for.
  auto migrate_to_tab = [this, engine, plugin_id, source, save_config, host = session->host.get()](bool transient) {
    QWidget* released = releaseCentralPanel();
    if (released == nullptr) {
      return;
    }
    pinToolboxPanel(released, plugin_id, source, engine, save_config, host, transient);
  };
  // "This panel has work in flight" — the one predicate every teardown door
  // consults. Domain-neutral: it asks the panel's own host whether it currently
  // owns a live ingest context, not what the plugin is.
  std::function<bool()> has_work_in_flight = [session]() {
    return session->host != nullptr && session->host->hasActiveIngests();
  };
  const WrappedToolboxPanel wrapped = wrapToolboxPanel(
      panel, source,
      /*on_close=*/
      [this, engine]() {
        engine->close();
        restoreCentralArea();
        engine->deleteLater();
      },
      /*on_migrate=*/[migrate_to_tab]() { migrate_to_tab(/*transient=*/false); },
      /*has_work_in_flight=*/has_work_in_flight,
      /*on_fold_busy=*/[migrate_to_tab]() { migrate_to_tab(/*transient=*/true); });

  if (target == ToolboxLaunchTarget::kPinnedTab) {
    wrapped.enter_pinned_chrome();
    pinToolboxPanel(
        wrapped.container, plugin_id, pin_tab_name.isEmpty() ? source : pin_tab_name, engine, save_config,
        session->host.get(), /*transient=*/false);
    QTimer::singleShot(250, this, [this]() { syncPanelPreviewDisplay(); });
    return;
  }
  if (!presentPanel(wrapped.container)) {
    report_error(source, tr("Cannot show '%1': another panel is already open").arg(source));
    // presentPanel did not parent the container on the reject path, and the engine
    // keeps only a non-owning QPointer to the inner panel, so delete the wrapper
    // (which owns `panel`) here to avoid a leak.
    engine->close();
    wrapped.container->deleteLater();
    engine->deleteLater();
    return;
  }
  // presentPanel() succeeded; remember the engine so launching another toolbox
  // (or any panel) tears this one down first instead of being refused.
  current_panel_engine_ = engine;
  // Key the fold on the launch session's address — the identity the ingest
  // callbacks above report as their owner — so only this panel's own import
  // folds it.
  setTakeoverFold(
      static_cast<const void*>(session.get()),
      [migrate_to_tab, enter_pinned_chrome = wrapped.enter_pinned_chrome](bool transient) {
        enter_pinned_chrome();
        migrate_to_tab(transient);
      },
      has_work_in_flight);
  // Apply the app's grid/curve-style/width to the panel's embedded PlotWidget once
  // the panel engine has built it (deferred: the plot is created on the first tick).
  QTimer::singleShot(250, this, [this]() { syncPanelPreviewDisplay(); });
}

void MainWindow::pinToolboxPanel(
    QWidget* container, const QString& plugin_id, const QString& title, PanelEngine* engine,
    std::function<QString()> save_config, ToolboxRuntimeHost* host, bool transient) {
  pinned_toolboxes_.insert(
      plugin_id, PinnedToolbox{
                     .container = container,
                     .engine = engine,
                     .save_config = std::move(save_config),
                     .host = host,
                     .label = title,
                     .transient = transient});

  // A plugin-initiated requestClose (e.g. the toolbox's own Close button)
  // must now close the TAB, funnelling into the same on_close teardown as
  // the tab's X — not the takeover restore path this engine was wired with
  // at launch. Re-entrant double-teardown is harmless: PanelEngine::close()
  // is idempotent, deleteLater coalesces, and the registry erase is a no-op
  // the second time.
  QPointer<QWidget> container_guard(container);
  if (engine != nullptr) {
    engine->onCloseRequested([this, container_guard](const std::string& reason) {
      return onPinnedPanelCloseRequested(container_guard, reason);
    });
  }

  QPointer<PanelEngine> engine_guard(engine);
  ui_->tabbedPlotWidget->addWidgetTab(title, container, [this, plugin_id, engine_guard]() {
    // Quiesce the plugin BEFORE dropping the registry entry: the entry's
    // save_config owns the PanelSession, and close() reaches plugin code
    // through a borrowed handle — the session must still be alive here, not
    // kept so only incidentally by the migrate-button connection's capture.
    if (!engine_guard.isNull()) {
      engine_guard->close();
      engine_guard->deleteLater();
    }
    pinned_toolboxes_.remove(plugin_id);
  });

  // Rules 3-4: an idle folded panel closes like any tab; one with a job in
  // flight confirms first, because the teardown above cancels that job. The
  // entry's own host is what makes the cancellation land on THIS panel and
  // leave another folded import running.
  ui_->tabbedPlotWidget->setWidgetTabPreClose(container, [this, plugin_id]() {
    const auto it = pinned_toolboxes_.constFind(plugin_id);
    if (it == pinned_toolboxes_.constEnd() || !hostHasWorkInFlight(it->host)) {
      return true;
    }
    if (!confirmCancelRunningJob(it->label)) {
      return false;
    }
    // The confirmation ran a modal event loop, so `it` may be stale: re-find
    // the entry rather than stopping through a dangling host.
    if (const auto confirmed = pinned_toolboxes_.constFind(plugin_id); confirmed != pinned_toolboxes_.constEnd()) {
      stopHostWork(confirmed->host);
    }
    return true;
  });
}

bool MainWindow::onPinnedPanelCloseRequested(QWidget* container, const std::string& reason) {
  // A pinned panel survives its own batch completion: the tab is the user's
  // surface now and stays connected so another job can be queued into it. Only
  // a user-driven request closes it.
  //
  // Returning false is load-bearing, not cosmetic: the engine tears itself down
  // unless the owner declines, and a torn-down engine stops ticking and rejects
  // the plugin while the tab stays on screen — an inert panel whose every click
  // is silently dropped, unrecoverable without reopening the plugin.
  if (reason == "import_complete") {
    return false;
  }
  if (container != nullptr) {
    ui_->tabbedPlotWidget->closeWidgetTab(container);
  }
  return true;
}

QDomElement MainWindow::savePinnedToolboxes(QDomDocument& doc) const {
  QDomElement root = doc.createElement(u"pinned_toolboxes"_s);
  // Deterministic order (QHash iteration is not) so identical workspaces
  // produce identical layout files.
  QStringList plugin_ids = pinned_toolboxes_.keys();
  plugin_ids.sort();
  for (const QString& plugin_id : plugin_ids) {
    const auto toolbox_it = pinned_toolboxes_.constFind(plugin_id);
    if (toolbox_it == pinned_toolboxes_.constEnd() || toolbox_it->container.isNull()) {
      continue;
    }
    // A background fold is the host uncovering the chart area for a running
    // job, not a workspace the user arranged — persisting it would resurrect a
    // panel they never pinned (and cannot resume the job it was doing).
    if (toolbox_it->transient) {
      continue;
    }
    QDomElement element = doc.createElement(u"toolbox"_s);
    element.setAttribute(u"plugin_id"_s, plugin_id);
    // The tab strip's label is the sole store of a user rename (same
    // in-place rename plot tabs have), so capture it here.
    const QString tab_name = ui_->tabbedPlotWidget->widgetTabName(toolbox_it->container);
    if (!tab_name.isEmpty()) {
      element.setAttribute(u"tab_name"_s, tab_name);
    }
    if (toolbox_it->save_config) {
      const QString config = toolbox_it->save_config();
      if (!config.isEmpty()) {
        // CDATA (with ]]> splitting), matching every other plugin-JSON-in-
        // layout site — a plain text node entity-escapes on each round trip.
        layout_xml::appendJsonAsCdata(doc, element, config);
      }
    }
    root.appendChild(element);
  }
  return root;
}

bool MainWindow::restorePinnedToolboxes(const QDomElement& root, MissingCurvePolicy policy) {
  // Replacing the pinned set destroys every live pinned engine, and for a panel
  // with a job in flight that teardown is the job's kill switch. Ask ONCE
  // (naming one busy panel) before taking any of them down; declining leaves the
  // live set — and the transfer — exactly as it was.
  QString busy_label;
  bool any_busy = false;
  for (const auto& toolbox : std::as_const(pinned_toolboxes_)) {
    if (hostHasWorkInFlight(toolbox.host)) {
      any_busy = true;
      busy_label = toolbox.label;
      break;
    }
  }
  if (any_busy) {
    // D5's dialog-vs-diagnostic fork: a non-interactive restore (a batch
    // drain can land minutes after the layout open, mid-anything) must never
    // raise the confirmation modal. Its resolution is the same as a decline —
    // the live panels and their jobs are retained, the layout's pinned set is
    // not applied — reported through the diagnostic sink.
    if (policy == MissingCurvePolicy::kRetainAndDiagnose) {
      reportLayoutRestoreIssue(
          policy, "pinned-toolbox-busy",
          tr("Layout restore kept the current pinned panels: '%1' has a job still running.").arg(busy_label));
      return false;
    }
    if (!confirmCancelRunningJob(busy_label)) {
      return false;
    }
    for (const auto& toolbox : std::as_const(pinned_toolboxes_)) {
      if (hostHasWorkInFlight(toolbox.host)) {
        stopHostWork(toolbox.host);
      }
    }
  }

  // The layout's pinned set REPLACES the live one — a layout saved without
  // pinned toolboxes restores to none.
  closeAllPinnedToolboxTabs();
  const QDomElement pinned = root.firstChildElement(u"pinned_toolboxes"_s);
  for (QDomElement element = pinned.firstChildElement(u"toolbox"_s); !element.isNull();
       element = element.nextSiblingElement(u"toolbox"_s)) {
    const QString plugin_id = element.attribute(u"plugin_id"_s);
    if (plugin_id.isEmpty()) {
      continue;
    }
    // A plugin missing from the catalog surfaces launchToolbox's own
    // diagnostic and the tab is simply dropped (no placeholder). The saved
    // rename rides along so the tab is born with it (directCdataText, not
    // QDomElement::text(): the latter recurses into any future child
    // elements).
    launchToolbox(
        plugin_id, layout_xml::directCdataText(element), ToolboxLaunchTarget::kPinnedTab,
        element.attribute(u"tab_name"_s));
  }
  return true;
}

void MainWindow::closeAllPinnedToolboxTabs() {
  // Iterate a copy: each close mutates the registry through its on_close.
  const auto pinned = pinned_toolboxes_;
  for (const PinnedToolbox& toolbox : pinned) {
    QWidget* container = toolbox.container.data();
    if (container != nullptr) {
      // Forced: shutdown and layout replacement are not refusable, and the
      // pinned tab's own pre-close would otherwise raise a confirmation the
      // callers here have already resolved (or, at shutdown, cannot honor).
      ui_->tabbedPlotWidget->closeWidgetTabForced(container);
    }
    // Bulk closes (layout replace, shutdown) finish the teardown
    // SYNCHRONOUSLY instead of via the deferred deletes closeWidgetTab
    // scheduled: the engine must be gone and the container's captured
    // PanelSession released before a relaunch of the same plugin binds a
    // fresh instance (exclusive plugin resources), and before ~MainWindow
    // destroys the DataEngine the session writes into. The pending
    // deleteLater events are cancelled by the direct deletes.
    delete toolbox.engine.data();
    delete container;
  }
  pinned_toolboxes_.clear();  // drop any stale entries whose widget died
}

}  // namespace PJ
