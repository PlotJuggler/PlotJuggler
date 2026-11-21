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
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#if QT_CONFIG(process)
#include <QProcess>
#endif
#include <QDomDocument>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
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
#include <QRhiWidget>
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
#include <QTextDocument>
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
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <set>
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

#include "BrowserFileStore.h"
#include "BrowserPersistence.h"
#include "BuiltinPlugins.h"
#include "DatasetMergeActions.h"
#include "DebugUi.h"
#include "FanoutConfig.h"
#include "FileLoader.h"
#include "IngestProgressController.h"
#include "IngestProgressWiring.h"
#ifdef PJ_TARGET_WASM
#include "FileSelectionService.h"
#endif
#include "CustomWidgetController.h"
#include "LayoutImportBatch.h"
#include "LayoutReplayHints.h"
#include "LayoutXml.h"
#include "PendingDisplayBinder.h"
#include "PluginPlotTabsController.h"
#include "PreferencesDialog.h"
#include "PresetController.h"
#include "RasterKeyMap.h"
#include "SeriesProgressController.h"
#include "SourcePromotionHost.h"
#include "SourceTimelineController.h"
#include "StreamingSourceManager.h"
#include "Theme.h"
#include "TitleBar.h"
#include "ToolboxHostWiring.h"
#include "TopicDemandController.h"
#include "mcp/McpBrowserLink.h"
#include "mcp/McpServer.h"
#ifdef PJ_TARGET_WASM
#include "WelcomePresetsDialog.h"
#endif
#include "pj_base/builtin/image.hpp"
#include "pj_base/builtin/image_codec.hpp"
#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/dataset.hpp"
#include "pj_base/types.hpp"
#include "pj_custom_widgets/CustomWidgetDockWidget.h"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_marketplace/extension_manager.hpp"
#ifndef PJ_TARGET_WASM
#include "pj_base/sdk/service_traits.hpp"
#include "pj_marketplace/marketplace_window.hpp"
#include "pj_marketplace/platform_utils.hpp"
#include "pj_runtime/FeatureCatalogHost.h"
#include "pj_runtime/ServiceRegistration.h"
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
#include "pj_log/LogDockWidget.h"
#include "pj_plotting/PlotMarkersItem.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/PlotWidgetBase.h"
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
#include "pj_plugins/host_qt/widget_binding.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/CurveColorRegistry.h"
#include "pj_runtime/CustomTopicsPolicy.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/DerivedObjects.h"
#include "pj_runtime/DerivedRuntimeHosts.h"
#include "pj_runtime/DerivedTopics.h"
#include "pj_runtime/DiagnosticHistory.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/IObjectViewer.h"
#include "pj_runtime/MarkerTopics.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/PlaybackControlHost.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/PlotTabsRuntimeHost.h"
#include "pj_runtime/PresetCatalog.h"
#include "pj_runtime/QSettingsBackend.h"
#include "pj_runtime/RecordingService.h"
#include "pj_runtime/SessionManager.h"
#ifndef __EMSCRIPTEN__
#include "pj_runtime/SourceCacheStore.h"
#include "pj_runtime/SourceCaptureService.h"
#endif
#include "pj_runtime/SourceConfigVariables.h"
#include "pj_runtime/TelemetryPing.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/ToolboxRuntimeHost.h"
#include "pj_runtime/ToolboxTags.h"
#include "pj_runtime/TopicDemandTracker.h"
#include "pj_runtime/UpdateChecker.h"
#include "pj_runtime/UsageCounters.h"
#include "pj_runtime/UsageReporter.h"
#include "pj_runtime/ViewportRuntimeHost.h"
#include "pj_runtime/XmlCdata.h"
#ifdef PJ_WITH_SCENE2D
#include "SceneImageDecoder.h"
#include "SceneVideoFrameSource.h"
#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#endif
#ifdef PJ_WITH_SCENE3D
#include "pj_runtime/TransformService.h"
#include "pj_scene3d_widgets/FixedFrameMemory.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/layout_resources.h"
#endif
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_widgets/CoalescingTrigger.h"
#include "pj_widgets/Dialog.h"
#include "pj_widgets/FileDialog.h"
#include "pj_widgets/FlowLayout.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/GraphicsApi.h"
#include "pj_widgets/IngestProgressWidget.h"
#include "pj_widgets/MessageBox.h"
#ifndef PJ_TARGET_WASM
#include "pj_widgets/RasterStreamView.h"
#endif
#include "ToolboxChrome.h"
#include "pj_widgets/Scrollbar.h"
#include "pj_widgets/SectionHeaderBand.h"
#include "pj_widgets/SideDrawer.h"
#include "pj_widgets/SvgButton.h"
#include "pj_widgets/SvgUtil.h"
#include "pj_widgets/Timeline.h"
#include "pj_widgets/ToastManager.h"
#include "scene_object_classification.h"
#include "ui/AboutDialog.h"
#include "ui/BatchLoadDialog.h"
#include "ui/CurveListPanel.h"
#include "ui/DatasetInfoDialog.h"
#include "ui/DiagnosticsDialog.h"
#include "ui/LayoutDatasetDialog.h"
#include "ui/LeftPanel.h"
#include "ui/McpJobsDialog.h"
#ifdef PJ_WITH_SCENE2D
#include "ui/Scene2DConfigPanel.h"
#endif
#ifdef PJ_WITH_SCENE3D
#include "ui/Scene3DConfigPanel.h"
#endif
#include "ui/TimelineWidget.h"
#include "ui_MainWindow.h"
#include "ui_ToolboxBanner.h"
using namespace Qt::StringLiterals;

#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
#include "../tests/wasm_acceptance_probes.inc"
#ifdef PJ_WITH_SCENE3D
#include "../tests/wasm_scene3d_acceptance_probes.h"
#endif
#endif

namespace PJ {

namespace {

// A stream-bound layout: it records streaming sources a load restarts (an invalid block records none).
bool recordsStreams(const QDomDocument& doc) {
  return !layout_xml::extractStreamingSources(doc, nullptr).isEmpty();
}

// The attribute prefix of the dataset a filter or generator entry is bound to: those bind once per dataset, so the
// dataset is part of what they are. The other kinds are one recipe whatever their inputs read.
QString boundDatasetPrefix(const QDomElement& entry) {
  if (entry.tagName() == "processor"_L1) {
    return u"input_dataset_"_s;
  }
  if (entry.tagName() == "generator"_L1) {
    return u"dataset_"_s;
  }
  return {};
}

// Whether two <data_processors> entries name the same recipe, the bound dataset aside: a filter by its output, the
// other kinds by owner and id.
bool sameRecipeName(const QDomElement& a, const QDomElement& b) {
  return a.tagName() == b.tagName() && a.attribute(u"owner_plugin"_s) == b.attribute(u"owner_plugin"_s) &&
         a.attribute(u"id"_s) == b.attribute(u"id"_s) && a.attribute(u"output_name"_s) == b.attribute(u"output_name"_s);
}

// The <output> names of every `tag` entry of `block`.
QSet<QString> outputNames(const QDomElement& block, const QString& tag) {
  QSet<QString> names;
  for (QDomElement entry = block.firstChildElement(tag); !entry.isNull(); entry = entry.nextSiblingElement(tag)) {
    for (QDomElement out = entry.firstChildElement(u"output"_s); !out.isNull();
         out = out.nextSiblingElement(u"output"_s)) {
      names.insert(out.attribute(u"name"_s));
    }
  }
  return names;
}

// A dataset id names a dataset of one session only: kept past its load, it would make the save stamp the path of
// whatever dataset holds that id now. A qualifier the saved path or source still identifies just loses its id; one
// that only the id names was bound to a stream at load (bindStreamQualifiers), and gets back its pj-stream:// identity.
void keepSessionFreeQualifiers(QDomElement element, const QHash<std::uint32_t, QString>& stream_identity_for_dataset) {
  const QDomNamedNodeMap attributes = element.attributes();
  QStringList ids;
  for (int index = 0; index < attributes.count(); ++index) {
    const QString name = attributes.item(index).nodeName();
    if (name == "dataset_id"_L1 || name.endsWith("_dataset_id"_L1)) {
      ids.push_back(name);
    }
  }
  for (const QString& name : ids) {
    const QString prefix = name.chopped(2);  // "<prefix>dataset_"
    if (!element.hasAttribute(prefix + u"path"_s) && !element.hasAttribute(prefix + u"source"_s)) {
      if (const auto stream = stream_identity_for_dataset.constFind(element.attribute(name).toUInt());
          stream != stream_identity_for_dataset.cend()) {
        element.setAttribute(prefix + u"path"_s, stream.value());
      }
    }
    element.removeAttribute(name);
  }
  for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
    keepSessionFreeQualifiers(child, stream_identity_for_dataset);
  }
}

#ifndef PJ_TARGET_WASM
// pj_runtime links no resources, so FeatureCatalogHost is handed the bytes.
QByteArray featureCatalogResource() {
  QFile file(QStringLiteral(":/resources/feature_catalog.json"));
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
#endif

Q_LOGGING_CATEGORY(lcMain, "pj.app.main")

// Arms the 3D view's first-content framing (Scene3DDockWidget::frameInitialContent) for a view a user drop or a
// plugin panel fills: the default fixed frame can sit far from the data. Any other widget, or none, is left alone.
void frameDroppedScene3dContent(IDataWidget* widget) {
#ifdef PJ_WITH_SCENE3D
  if (auto* scene3d = widget != nullptr ? qobject_cast<Scene3DDockWidget*>(widget->widget()) : nullptr) {
    scene3d->frameInitialContent();
  }
#else
  Q_UNUSED(widget);
#endif
}

// An embedded scene view keeps one dynamic property per (dataset, topic) request it
// attached, named by embeddedAttachedKey and holding the ObjectTopicId it attached, or
// 0 once that topic was evicted (the request stands; a later load re-attaches it).
constexpr char kEmbeddedAttachedPrefix[] = "_pj_attached:";

[[maybe_unused]] QByteArray embeddedAttachedKey(const QString& topic, const QString& dataset) {
  return QByteArray(kEmbeddedAttachedPrefix) + dataset.toUtf8() + '\n' + topic.toUtf8();
}

QList<QByteArray> embeddedAttachedKeys(const QObject& view) {
  QList<QByteArray> keys = view.dynamicPropertyNames();
  keys.removeIf([](const QByteArray& key) { return !key.startsWith(kEmbeddedAttachedPrefix); });
  return keys;
}

uint32_t embeddedAttachedId(const QObject& view, const QByteArray& key) {
  return view.property(key.constData()).value<uint32_t>();
}

// Passive resync with the group's signals blocked: the Curve Width/Style groups' click handlers would refresh the
// open preview panels.
void checkGroupButton(QButtonGroup* group, int id) {
  if (group == nullptr) {
    return;
  }
  if (auto* btn = group->button(id)) {
    const QSignalBlocker blocker(group);
    btn->setChecked(true);
  }
}

// Relative when the data lives at or beneath `layout_dir`, else absolute: a ../.. path would break when the layout
// file moves. `startsWith("..")` alone would misclassify names like "..foo". Both sides are canonical, the form the
// session stores source paths in, so a layout reached through a symlinked folder (macOS /var -> /private/var) still
// contains its data.
[[nodiscard]] QString relocatableSubpath(const QString& absolute_path, const QDir& layout_dir) {
  const QString canonical_dir = layout_dir.canonicalPath();
  const QDir base(canonical_dir.isEmpty() ? layout_dir.absolutePath() : canonical_dir);
  const QString relative = base.relativeFilePath(SessionManager::normalizedSourcePath(absolute_path));
  const bool is_subpath = relative != ".."_L1 && !relative.startsWith("../"_L1);
  return is_subpath ? relative : absolute_path;
}

constexpr auto kDefaultRegistryUrl =
    "https://raw.githubusercontent.com/PlotJuggler/pj-plugin-registry/"
    "refs/heads/development/registry.json";
constexpr auto kRegistryUrlSettingsKey = "Marketplace/registryUrl";
constexpr auto kPanelBottomExpandedKey = "MainWindow.panelBottomExpandedHeight";
// Source Timeline strip floor (px) when the bottom panel opens: the ruler plus a few source bars.
constexpr int kMinTimelineStripHeight = 150;

// Persisted key, shared with PlotJuggler 3 so an upgrade keeps the last layout directory.
#ifndef PJ_TARGET_WASM
constexpr auto kLastLayoutDirKey = "MainWindow.lastLayoutDirectory";
#endif

// The root's pj4_version. Bumped only on incompatible changes: the loader skips unknown elements and attributes,
// and a layout from a newer build loads best-effort with a warning.
// v2: curves by topic+field path, rebound per dataset on load; <root binding=...> (generic vs source-bound).
// v3: <range> X axis in absolute seconds (the display offset is applied at load, never stored); <fileInfo>
//     display_offset_ns + timeline_order; <source_timeline> view state.
// v4: one <dataset> per fan-out member under <fileInfo>; offsets are sourceDisplayOffset() only, so the loader
//     subtracts the current global reference from a <=v3 offset.
// v5: <previouslyLoaded_StreamingSources>. layout_xml::writeStreamingSources stamps v5 only on a layout that
//     carries it, so an older build warns about exactly the layouts whose stream records it would drop.
constexpr int kLayoutSchemaVersion = 4;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr int kTestSampleCount = 1000;
constexpr double kTestDurationSeconds = 10.0;
constexpr int kResizeMargin = 6;
// Tracker-time fan-out cap, equal to PlaybackEngine's kTickIntervalMs so playback passes 1:1; it throttles the
// faster drivers (scrubbing, programmatic seeks).
constexpr int kTrackerBroadcastIntervalMs = 33;
// Loads that finish inside this delay never flash the title-bar ingest strip. The WASM acceptance probes
// (PJ_WASM_ENABLE_INGRESS_PROBE) shorten it for deterministic cancellation fixtures.
constexpr int kIngestStripShowDelayMs = 500;
[[maybe_unused]] constexpr int kIngestStripShowDelayWasmMs = 50;
// Linger after the last load/import drains before the strip hides.
constexpr int kIngestStripHideLingerMs = 1000;
// Byte counts exceed QProgressBar's int range, so the bar counts permille.
constexpr int kIngestProgressResolution = 1000;
// Per section of the recent popup (Layouts and Files are capped independently).
constexpr int kMaxRecentEntries = 8;
constexpr auto kRecentLayoutsKey = "Layout/recent";
constexpr auto kLayoutFilter = "PlotJuggler 4 Layout (*.pj4.xml)";
// The filter pattern must match layout_xml::kLayoutExtension.
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

// `text` is plain text: it often quotes a downloaded layout or index, whose
// markup must be shown, never rendered.
void showBrowserLayoutWarning(QWidget* parent, const QString& title, const QString& text) {
  // The browser main thread cannot enter MessageBox::warning's nested exec().
  auto* dialog = new MessageBox(parent);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowModality(Qt::ApplicationModal);
  dialog->setTitle(title);
  dialog->setText(Qt::convertFromPlainText(text, Qt::WhiteSpaceNormal));
  dialog->addButton(QObject::tr("OK"), MessageBox::kPrimaryRole);
  dialog->show();
}

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
  // Material's Dock-to-Left/Right glyphs fill the side opposite their name, hence the swapped assets.
  return {{
      {ui->tabbedPlotWidget->leftPanelButton(), ui->leftColumn, ":/resources/svg/panel_right.svg",
       ":/resources/svg/panel_right_off.svg"},
      // timelineStrip, not bottomPanel: the playback bar above it in the same panel stays visible.
      {ui->tabbedPlotWidget->bottomPanelButton(), ui->timelineStrip, ":/resources/svg/panel_bottom.svg",
       ":/resources/svg/panel_bottom_off.svg"},
      {ui->tabbedPlotWidget->rightPanelButton(), ui->localToolbarWidget, ":/resources/svg/panel_left.svg",
       ":/resources/svg/panel_left_off.svg"},
  }};
}

// MEMFS in the browser; a temporary directory on the desktop, reached only by tests (presets are browser-only),
// where a crashed process's leased files can linger.
QString presetStagingRoot() {
#ifdef PJ_TARGET_WASM
  return u"/pj_uploads"_s;
#else
  return QDir::tempPath() + u"/pj_uploads"_s;
#endif
}

// Layouts persist the width value, not the button, and map it back on load.
inline constexpr std::array<std::pair<const char*, double>, 4> kWidthButtonSpecs{{
    {"globalWidth1_0", 1.0},
    {"globalWidth1_5", 1.5},
    {"globalWidth2_0", 2.0},
    {"globalWidth3_0", 3.0},
}};

// The on-demand node whose object output the catalog item `catalog_key` is; null otherwise.
const RecipeGraph::Node* onDemandNodeOfCatalogKey(AppSession& session, const QString& catalog_key) {
  const std::optional<CatalogItem> item = session.catalogModel().itemDescriptor(catalog_key);
  if (!item.has_value()) {
    return nullptr;
  }
  const OnDemandService& on_demand = session.sessionManager().onDemandService();
  if (const ScalarFieldPayload* field = asScalarField(*item)) {
    // A series column: its topic is named like its node's key, and only that node's series owns it.
    const RecipeGraph::Node* node = on_demand.graph().find(item->topic_name.toStdString());
    const std::optional<SeriesRunner::State> series =
        node != nullptr ? on_demand.series().state(node->recipe.key()) : std::nullopt;
    return series.has_value() && series->topic == field->topic_id ? node : nullptr;
  }
  const ObjectTopicPayload* object = asObjectTopic(*item);
  if (object == nullptr) {
    return nullptr;
  }
  const std::optional<std::string> key = on_demand.keyOfTopic(object->object_topic_id);
  return key.has_value() ? on_demand.graph().find(*key) : nullptr;
}

// Spans one workspace restore (apply, rollback, toolbox relaunch). Its steps hide or delete the widget holding the
// keyboard focus, or switch the shown tab, and Qt then moves focus along the tab chain, possibly into a line edit
// that takes Space from playback. On exit focus goes back to the widget that held it at the start if it is still
// shown, else to the window, where the window shortcuts still reach.
class RestoreFocusGuard {
 public:
  explicit RestoreFocusGuard(QWidget* window) : window_(window), had_focus_(window->focusWidget()) {}
  ~RestoreFocusGuard() {
    if (had_focus_ != nullptr && had_focus_->isVisible()) {
      had_focus_->setFocus(Qt::OtherFocusReason);
    } else {
      window_->setFocus(Qt::OtherFocusReason);
    }
  }
  RestoreFocusGuard(const RestoreFocusGuard&) = delete;
  RestoreFocusGuard& operator=(const RestoreFocusGuard&) = delete;

 private:
  QWidget* window_;
  QPointer<QWidget> had_focus_;
};

}  // namespace

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

  // A stream record and its configuration filled in for this page.
  struct ReplayStream {
    layout_xml::StreamingSourceRef record;
    QString expanded_config;
  };

  // Each stream start follows its stream and plays from its end, so a rollback restores this.
  struct Transport {
    DatasetId followed_stream = 0;
    bool seeded = false;
    bool playing = false;
    DisplaySeconds current_time;
  };

  // One file a replay stages: a source's data file, or a side file its plugin reads (layout_xml::SideFileRef).
  struct ReplayFile {
    int source_index = 0;
    bool side = false;
    QString logical_path;  // the <fileInfo> or <sideFile> filename
    QString content_sha256;
    bool content_sha256_malformed = false;
  };

  struct Replay {
    QDomDocument doc;
    QString layout_name;
    QList<layout_xml::DataSourceRef> sources;
    std::vector<LoadInput> staged_inputs;
    QStringList selected_identities;
    int selection_index = 0;
    bool load_failed = false;
    QString load_failure;
    CapturedWorkspace previous_workspace;
    QPointer<QPushButton> selection_button;
    // The one selection (picker, or download + staging) this replay waits for; 0 when none.
    quint64 active_picker_generation = 0;
    // A downloaded layout's file URLs, in `files` order; empty for an upload. Their bytes share one preset cap.
    QList<QUrl> source_urls;
    qint64 staged_bytes = 0;
    // This replay's accepted imports whose loadFinished has not arrived.
    QSet<quint64> pending_tickets;
    bool cancelled = false;
    // Context of this replay's FileLoader connections, so a terminal arriving after the replay is delivered nowhere.
    std::unique_ptr<QObject> receiver = std::make_unique<QObject>();
    // Started after the file imports validate, just before the layout applies.
    std::vector<ReplayStream> streams;
    // The streams this replay started so far: its rollback stops and removes them.
    std::vector<DatasetId> stream_datasets;
    // Captured just before the first stream starts; empty while none has.
    std::optional<Transport> transport_before_streams;
    // Every file this replay stages, in staging order: each source's data file (files[i] is sources[i]'s), then
    // every side file. selection_index walks it; a downloaded layout's source_urls follow it.
    QList<ReplayFile> files = {};
    // The staged side-file copies, in the order their files appear.
    std::vector<LoadInput> staged_side_files = {};
  };

  // What one preset load brought: its files' source identities (save-ledger entries) and the datasets the next
  // preset load removes. A merge takes its datasets out: a merge result may hold data no preset brought.
  struct PresetData {
    QSet<QString> file_identities;
    std::vector<DatasetId> datasets;
  };

  QHash<QString, SourceRecord> sources;
  std::optional<Replay> replay;
  QPointer<MessageBox> decision_dialog;
  QString pending_recipe_name;
  bool pending_generic_recipe = false;
  quint64 next_picker_generation = 0;
  // loadLayoutFromUrl's downloads; created on first use. Its completions never outlive it.
  std::unique_ptr<PresetController> downloads;
  // The loadLayoutFromUrl in flight until its terminal; empty for any other load. finishBrowserLayoutLoad reports
  // its end through layoutUrlLoadFinished.
  QUrl loading_url;
  BrowserFileStore store{presetStagingRoot()};
  // The last loadLayoutFromUrl that loaded: the next one that loads removes it.
  PresetData preset_data;
};

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
              session_->sessionManager().dataEngine(),
              [this](DatasetId dataset_id, DisplaySeconds start) {
                session_->sessionManager().setSeriesVisibleStart(dataset_id, start);
              })),
      ingest_progress_controller_(std::make_unique<IngestProgressController>(this)),
      theme_(std::make_unique<Theme>()) {
  browser_layout_runtime_ = std::make_unique<BrowserLayoutRuntime>();
#ifdef PJ_WITH_SCENE3D
  transform_service_ = std::make_unique<PJ::TransformService>(session_->sessionManager());
  transform_service_->activateIngestTap();
  session_->sessionManager().setTransformService(transform_service_.get());  // on-demand runs read TF here
  fixed_frame_memory_ = std::make_unique<pj::scene3d::FixedFrameMemory>(session_->sessionManager());
#endif
#ifdef PJ_WITH_SCENE2D
  session_->sessionManager().setImageDecoder(std::make_shared<SceneImageDecoder>());  // on-demand scripts' decode()
#ifdef PJ_HAS_FFMPEG
  // On-demand scripts' frame_at: a decoder per topic and parser, made again when the topic's parser changes.
  {
    SessionManager& session = session_->sessionManager();
    session.setVideoFrameSource(
        std::make_shared<SceneVideoFrameSource>(
            session.objectStore(),
            [&session](PJ::ObjectTopicId topic) {
              const SessionManager::ParserBinding binding = session.parserBindingForObjectTopic(topic);
              return binding ? SceneVideoFrameSource::parserExtractor(binding.parser, binding.mutex, binding.keepalive)
                             : SceneVideoFrameSource::canonicalExtractor();
            },
            [&session](PJ::ObjectTopicId topic) -> const void* {
              return session.parserBindingForObjectTopic(topic).parser;
            }));
  }
#endif
#endif
  // Composite through QRhi from the first show: otherwise the first plot's QRhiWidget switches the window from
  // raster and Qt recreates its native window (a flash on Wayland). Not in the browser: a WebGL context created
  // before the first file pick can be lost across the picker round trip (pj_scene2D/docs/TECHNICAL_NOTES.md).
#ifndef PJ_TARGET_WASM
  if (PlotWidgetBase::gpuCanvasEnabled()) {
    auto* rhi_compositing_anchor = new QRhiWidget(this);
    rhi_compositing_anchor->setApi(preferredGraphicsApi());
    rhi_compositing_anchor->hide();
  }
  // Not in the browser, whose drops carry no local paths.
  setAcceptDrops(true);
#endif
  FileDialog::setUseSystemDialog(QSettings().value(kSystemFileDialogKey, false).toBool());
  // Before setupUi, so the build*Toolbar() calls paint with them; .ui widgets get them from the re-broadcast at the
  // end of the constructor.
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

  // Custom frame (opt-in): TitleBar owns the top of the window and edge resize is the application event filter
  // installed below. Otherwise TitleBar is the toolbar row under the window manager's title bar.
  setWindowFlag(Qt::FramelessWindowHint, custom_frame_);
  setMouseTracking(custom_frame_);

  ui_->setupUi(this);

  // Every container down to the chrome rows gets zero margins: the platform style can otherwise leave a visible gap
  // below the TitleBar. The window's own margins reserve the custom frame's border band, which paintEvent() draws
  // and children painted later would cover.
  const int chrome_border_width = custom_frame_ ? PJ::theme::stroke(theme::Stroke::Hairline) : 0;
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

  ui_->mainSplitter->setHandleWidth(1);
  ui_->timelineSplitter->setHandleWidth(1);
  ui_->rightToolbarSplitter->setHandleWidth(1);
  // A plain QWidget paints its QSS border (#timelineStrip's border-top) only with this attribute.
  ui_->timelineStrip->setAttribute(Qt::WA_StyledBackground, true);

  // Extra height goes to the upper area; the bottom pane starts at exactly the playback bar's height.
  ui_->timelineSplitter->setStretchFactor(0, 1);
  ui_->timelineSplitter->setStretchFactor(1, 0);
  ui_->timelineSplitter->setSizes({1000, ui_->timelineWidget->minimumHeight()});

  // Width changes go to the chart pane alone; shared, they would creep the sidebar out toward its maximum width.
  ui_->mainSplitter->setStretchFactor(0, 0);
  ui_->mainSplitter->setStretchFactor(1, 1);

  ui_->rightToolbarSplitter->setStretchFactor(0, 1);
  ui_->rightToolbarSplitter->setStretchFactor(1, 0);
  // Wide enough for the scene config panels' label columns, and for the Curve Style strip on one FlowLayout row.
  ui_->localToolbarWidget->setMinimumWidth(250);
  ui_->rightToolbarSplitter->setSizes({2000, 240});
  ui_->rightToolbarSplitter->setOpaqueResize(true);

  // With the splitter's children not collapsible, this floor keeps a drag from clipping the playback controls.
  ui_->timelineWidget->setMinimumHeight(ui_->timelineWidget->sizeHint().height());

  // TitleBar owns the menus; the window only adds actions.
  title_bar_ = new TitleBar(this);
  title_bar_->setSystemFrame(!custom_frame_);
  connect(title_bar_, &TitleBar::notificationsClicked, this, [this]() {
    if (diagnostics_dialog_ == nullptr) {
      diagnostics_dialog_ = new DiagnosticsDialog(diagnostic_history_, this);
      diagnostics_dialog_->setChromeMetrics(chrome_metrics_);
      connect(this, &MainWindow::chromeMetricsChanged, diagnostics_dialog_, &Dialog::setChromeMetrics);
    }
    diagnostics_dialog_->show();
    diagnostics_dialog_->raise();
    diagnostics_dialog_->activateWindow();
    title_bar_->setUnseenError(false);
  });

  QMenu* file_menu = title_bar_->fileMenu();
  // Duplicates the LeftPanel load button: many users look for "open" here first.
  action_open_data_ = file_menu->addAction(tr("Open Data File..."), this, &MainWindow::onLoadDataRequested);
  action_open_data_->setShortcut(QKeySequence::Open);
#ifdef PJ_TARGET_WASM
  // Hidden until startBrowserPresets finds presets to offer.
  file_menu->addAction(ui_->actionPresets);
  connect(ui_->actionPresets, &QAction::triggered, this, [this]() {
    if (presets_dialog_ != nullptr) {
      presets_dialog_->show();
      presets_dialog_->raise();
      presets_dialog_->activateWindow();
    }
  });
#endif
  file_menu->addSeparator();
#ifndef PJ_TARGET_WASM
  action_load_layout_ = file_menu->addAction(tr("Load Layout..."), this, &MainWindow::onLoadLayout);
  action_save_layout_ = file_menu->addAction(tr("Save Layout..."), this, &MainWindow::onSaveLayout);
  file_menu->addSeparator();
  file_menu->addAction(ui_->actionMarketplace);
  action_preferences_ = file_menu->addAction(tr("Preferences..."), this, &MainWindow::onShowPreferencesDialog);
#else
  // Content-based open and browser download: no native paths.
  action_load_layout_ = file_menu->addAction(tr("Load Layout..."), this, &MainWindow::onLoadLayout);
  action_load_layout_->setObjectName(u"actionLoadLayout"_s);
  action_save_layout_ = file_menu->addAction(tr("Save Layout..."), this, &MainWindow::onSaveLayout);
  action_save_layout_->setObjectName(u"actionSaveLayout"_s);
  action_save_source_layout_ =
      file_menu->addAction(tr("Save Layout with Sources..."), this, &MainWindow::onSaveSourceLayout);
  action_save_source_layout_->setObjectName(u"actionSaveSourceLayout"_s);
  action_save_source_layout_->setEnabled(false);
  ui_->actionMarketplace->setVisible(false);
  file_menu->addSeparator();
  action_preferences_ = file_menu->addAction(tr("Preferences..."), this, &MainWindow::onShowPreferencesDialog);
  action_preferences_->setObjectName(u"actionPreferences"_s);
#endif
  file_menu->addSeparator();
  file_menu->addAction(ui_->actionExit);

  // Rebuilt on show, from what the catalog has loaded now.
  connect(title_bar_->toolboxMenu(), &QMenu::aboutToShow, this, &MainWindow::onRebuildToolboxMenu);

  QMenu* help_menu = title_bar_->helpMenu();
  help_menu->addAction(tr("About PlotJuggler..."), this, &MainWindow::onShowAboutDialog);
#ifndef PJ_TARGET_WASM
  // Not in the browser: it cannot install a release, and the GitHub API refuses the cross-origin check.
  help_menu->addAction(tr("Check for Updates..."), this, &MainWindow::onCheckForUpdates)
      ->setObjectName(u"actionCheckForUpdates"_s);
#endif
  help_menu->addAction(
      tr("Documentation"), this, []() { QDesktopServices::openUrl(QUrl(u"https://plotjuggler.io"_s)); });
  help_menu->addAction(tr("Report an Issue"), this, []() {
    QDesktopServices::openUrl(QUrl(u"https://github.com/PlotJuggler/PlotJuggler/issues"_s));
  });
  help_menu->addSeparator();
  installed_extensions_menu_ = help_menu->addMenu(tr("Installed Extensions"));
  installed_extensions_menu_->setObjectName(u"PJMenu"_s);
  connect(installed_extensions_menu_, &QMenu::aboutToShow, this, &MainWindow::onRebuildExtensionsMenu);

  setMenuWidget(title_bar_);

  // TabbedPlotWidget creates the panel toggles; the title bar only hosts them.
  title_bar_->addRightClusterWidget(ui_->tabbedPlotWidget->leftPanelButton());
  title_bar_->addRightClusterWidget(ui_->tabbedPlotWidget->bottomPanelButton());
  title_bar_->addRightClusterWidget(ui_->tabbedPlotWidget->rightPanelButton());

  // Application-wide, so a press on a descendant at the window edge (the TimelineWidget's bottom margin) still
  // starts a resize.
  qApp->installEventFilter(this);

  diagnostic_history_ = new DiagnosticHistory(this);
  diagnostic_history_->connectBridge(diagnostic_bridge_);
  // An error counts as seen once the diagnostics dialog is (or gets) opened.
  connect(diagnostic_history_, &DiagnosticHistory::recorded, this, [this](const DiagnosticRecord& record) {
    const bool dialog_open = diagnostics_dialog_ != nullptr && diagnostics_dialog_->isVisible();
    if (record.level == DiagnosticLevel::kError && !dialog_open) {
      title_bar_->setUnseenError(true);
    }
  });
  // One toast per session for a plugin that failed to load. The bridge delivers queued, so this still catches the
  // scan that ran in the AppSession constructor.
  connect(
      diagnostic_bridge_, &QtDiagnosticBridge::diagnosticReported, this,
      [this](int level, const QString& source, const QString& id, const QString&) {
        if (plugin_failure_toast_shown_ || level != static_cast<int>(DiagnosticLevel::kError) ||
            source != "ExtensionCatalogService"_L1) {
          return;
        }
        plugin_failure_toast_shown_ = true;
        // `id` is the manifest id; the loader error is too long for a toast.
        showToast(
            (id.isEmpty() ? tr("A plugin failed to load.")
                          : tr("Plugin '%1' failed to load.").arg(id.toHtmlEscaped())) +
                u"<br>"_s + tr("Details are in the notifications."),
            QPixmap(u":/resources/crying_cat.png"_s));
      });

  ui_->tabbedPlotWidget->setDataServices(&session_->sessionManager(), &session_->catalogModel());
  ui_->tabbedPlotWidget->setObjectWidgetFactory(
      [this](const QString& kind, const ObjectDropSeed* seed, QWidget* dock_parent) -> IDataWidget* {
        // Layout restore passes the saved XML tag as `kind` with a null seed (the dock reloads its own state); a
        // catalog drop passes an empty kind with a seed.
        QString resolved_kind = kind;
        if (resolved_kind.isEmpty() && seed != nullptr) {
          // The same routing the curve list's drag veto uses (scene_object_classification.h).
          resolved_kind = panelKindForObjectType(seed->object_type);
        }
        if (seed == nullptr) {
          // Restore, click-create and undo/redo: the playhead is seeded before PlotDocker's xmlLoadState(), so
          // restored layers come up at the current time (currentTimeChanged fires only on changes). An unknown kind
          // returns nullptr silently: "not my kind".
          return makeSeededEmptyObjectDock(resolved_kind, dock_parent);
        }

        if (seed->topic_id == ObjectTopicId{}) {
          // Placeholder drop: an advertised topic with no storage id until the drop's demand delivers the first
          // sample (TopicDemandController::handleSceneDockPlaceholderDrop). The content arriving later is framed once.
          IDataWidget* empty_dock = makeSeededEmptyObjectDock(resolved_kind, dock_parent);
          frameDroppedScene3dContent(empty_dock);
          return empty_dock;
        }

        // Failures toast (no modal: WASM-safe). The unknown-kind branch is defensive (the drag veto and
        // CustomWidgetSupport filter types first), but the per-dock refusals below are reachable: the veto is
        // type-level and cannot see whether a parser is registered.
        IDataWidget* widget = makeSceneDock(resolved_kind, dock_parent);
        if (widget == nullptr) {
          showToast(tr("This object topic cannot be displayed (type %1).")
                        .arg(QString::fromUtf8(sdk::name(seed->object_type))));
          return nullptr;
        }
        [[maybe_unused]] QWidget* qwidget = widget->widget();
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
        } else
#endif
        {
          // Any other family takes the topic through IDataWidget.
          if (!widget->tryAcceptObjectTopic(seed->topic_id, seed->object_type, seed->title)) {
            widget->widget()->deleteLater();
            showToast(tr("This object topic cannot be displayed (type %1).")
                          .arg(QString::fromUtf8(sdk::name(seed->object_type))));
            return nullptr;
          }
          seedStreamingPlaybackFromDrop();
        }
        // currentTimeChanged fires only on changes, so a new widget is seeded here; a 3D view frames its first content
        // at that time.
        widget->onTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
        frameDroppedScene3dContent(widget);
        return widget;
      });
  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::tabAdded, this, &MainWindow::onPlotTabAdded);
  wireExistingPlots();
  ui_->curveListPanel->setCatalog(&session_->catalogModel());
  ui_->curveListPanel->setTopicDemandTracker(&session_->topicDemandTracker());
  ui_->curveListPanel->setTopicDemandController(topic_demand_controller_.get());
  // The curve list marks what the selected widget shows; onDockFocused re-schedules it.
  ui_->curveListPanel->setDisplayedScope([this]() -> const QWidget* { return activeFocusedDock(); });

  // A row's two buttons are the choice (✕ keeps, bin discards), so they go straight to requestCancel; only the
  // title-bar strip's single stop button (hidden, kShowTitleBarIngestStrip) prompts.
  wireIngestProgress(
      session_->sessionManager(), session_->catalogModel(), *ingest_progress_controller_, *ui_->curveListPanel, this);

  applyDerivedBatchPreferences();
  wireMarkerPlotStatus();
  connect(
      &session_->sessionManager(), &SessionManager::seriesStateChanged, this, [this] { pushSeriesStatus(nullptr); });

  // Every catalog removal, whoever triggers it, prunes the data widgets.
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
    if (browser_layout_runtime_ != nullptr) {
      std::erase_if(browser_layout_runtime_->preset_data.datasets, [anchor, &consumed](DatasetId dataset_id) {
        return dataset_id == anchor || consumed.contains(dataset_id);
      });
    }
    syncWidgetsToCatalog();
    resetUndoHistory();
  });

  connect(ui_->curveListPanel, &CurveListPanel::trashRequested, this, &MainWindow::onCatalogTrashRequested);
  connect(ui_->curveListPanel, &CurveListPanel::toastRequested, this, [this](const QString& message) {
    showToast(message);
  });
  connect(ui_->curveListPanel, &CurveListPanel::removeDatasetsRequested, this, &MainWindow::onRemoveDatasetsRequested);
  connect(ui_->curveListPanel, &CurveListPanel::mergeDatasetsRequested, this, &MainWindow::onMergeDatasetsRequested);
  connect(ui_->curveListPanel, &CurveListPanel::refreshDerivedRequested, this, [this](const QList<DatasetId>& ids) {
    for (const DatasetId dataset_id : ids) {
      session_->sessionManager().batchEngine().refreshAll(dataset_id);
    }
  });
  // The "+" opens the toolbox whose manifest declares `custom_topics_editor`, like any toolbox.
  connect(ui_->curveListPanel, &CurveListPanel::createCustomTopicRequested, this, [this]() {
    if (const QString editor = session_->extensionCatalog().customTopicsEditorId(); !editor.isEmpty()) {
      launchToolbox(editor);
    }
  });
  // Opens the editor on the recipe's params_json. The panel shows the pencil only on the editor's own rows; the owner
  // is checked again here.
  connect(ui_->curveListPanel, &CurveListPanel::editCustomTopicRequested, this, [this](const QString& catalog_key) {
    const ExtensionCatalogService& extensions = session_->extensionCatalog();
    QString initial_config;
    if (const RecipeGraph::Node* node = onDemandNodeOfCatalogKey(*session_, catalog_key)) {
      if (!extensions.isCustomTopicsEditor(QString::fromStdString(node->recipe.owner_plugin))) {
        return;
      }
      initial_config = QString::fromStdString(node->recipe.params_json);
    } else {
      const std::optional<CurveDescriptor> output = session_->catalogModel().curveDescriptor(catalog_key);
      const auto transform = output.has_value()
                                 ? session_->sessionManager().dataProcessorService().transformOwning(output->topic_id)
                                 : std::nullopt;
      if (!transform.has_value() || !extensions.isCustomTopicsEditor(QString::fromStdString(transform->owner_plugin))) {
        return;
      }
      initial_config = QString::fromStdString(transform->params_json);
    }
    launchToolbox(extensions.customTopicsEditorId(), initial_config);
  });
  connect(ui_->curveListPanel, &CurveListPanel::deleteCustomTopicsRequested, this, &MainWindow::deleteCustomTopics);
  // A series replay is a stoppable bar on its Custom Topics rows.
  wireSeriesProgress(
      session_->sessionManager(), *new SeriesProgressController(this), *ui_->curveListPanel, /*context=*/this);
  ui_->curveListPanel->setOwnerResolver([this](const QString& plugin_id) {
    const ExtensionCatalogService& extensions = session_->extensionCatalog();
    return CurveListPanel::OwnerInfo{extensions.toolboxDisplayName(plugin_id), extensions.toolboxBadge(plugin_id)};
  });
  ui_->curveListPanel->setDisplayTimeResolver([this](DatasetId dataset_id, Timestamp raw_ns) {
    return displayTimeForSource({static_cast<uint32_t>(dataset_id)}, raw_ns);
  });
  // Connected after the catalog's own slot, so the new topics already have catalog keys. A removal needs nothing:
  // the panel prunes on itemsRemoved.
  connect(
      &session_->sessionManager(), &SessionManager::onDemandRecipeInstalled, this, &MainWindow::syncCustomTopicsPanel);
  // A transform row is matched to the catalog by name, so the refresh reuses the matches until the catalog changes.
  const auto transform_rows_stale = [this]() { custom_topic_transform_rows_stale_ = true; };
  connect(&session_->catalogModel(), &CatalogModel::itemsAdded, this, transform_rows_stale);
  connect(&session_->catalogModel(), &CatalogModel::itemsRemoved, this, transform_rows_stale);
  connect(&session_->catalogModel(), &CatalogModel::cleared, this, transform_rows_stale);
  // A recipe created after a load replaces the kept entry of its identity for good, even if deleted before a save.
  connect(&session_->catalogModel(), &CatalogModel::itemsAdded, this, &MainWindow::retireReplacedKeptDerived);
  connect(&session_->sessionManager(), &SessionManager::markersChanged, this, &MainWindow::retireReplacedKeptDerived);
  // A node's state changes on every request, a finding's pin moves on the display axis
  // with the offsets: both only repaint, once per event-loop turn.
  connect(
      &session_->sessionManager(), &SessionManager::derivedStateChanged, this, &MainWindow::scheduleCustomTopicsSync);
  connect(
      &session_->sessionManager(), qOverload<>(&SessionManager::displayOffsetChanged), this,
      &MainWindow::scheduleCustomTopicsSync);
  connect(
      &session_->sessionManager(), qOverload<DatasetId>(&SessionManager::displayOffsetChanged), this,
      &MainWindow::scheduleCustomTopicsSync);
  connect(ui_->curveListPanel, &CurveListPanel::clearAllCurvesRequested, this, [this]() {
    if (streaming_manager_ != nullptr &&
        (streaming_manager_->hasActiveSession() || streaming_manager_->hasPendingStart())) {
      streaming_manager_->stopAllAndWait(tr("dataset removed"));
      active_streaming_dataset_id_ = 0;
      streaming_playback_seeded_ = false;
    }
    // Objects go before the catalog changes, so cleared() subscribers see the topics gone. Eviction stays out of
    // clearAll(), which speculative callers use. lastLoadedSource is kept for quick reload. An all_datasets generator
    // is bound to the session, not a dataset: removeDataset() would leave it publishing onto whatever loads next.
    kept_derived_ = QDomDocument();
    session_->sessionManager().batchEngine().clearAllGenerators();
    session_->sessionManager().clearAllObjects();
#ifdef PJ_WITH_SCENE3D
    if (transform_service_ != nullptr) {
      transform_service_->invalidateAll();
    }
#endif
    // Objects go before the catalog, so its cleared() finds them gone and resets the 2D viewers. The catalog (no
    // tombstone) goes before the engine erase, so widgets drop their curve adapters on cleared() before the storage
    // goes (DataEngine::removeDataset's invalidate-first contract). Without the erase a reload would reattach to the
    // emptied shells.
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
  // Read before buildGlobalToolbar() creates the buttons.
  show_points_ = settings.value(u"MainWindow.buttonShowpoint"_s, true).toBool();
  activate_grid_ = settings.value(u"MainWindow.buttonActivateGrid"_s, false).toBool();
  dots_ = settings.value(u"MainWindow.buttonDots"_s, false).toBool();
  tracker_info_ = static_cast<CurveTracker::Parameter>(
      settings.value(u"MainWindow.timeTrackerSetting"_s, static_cast<int>(CurveTracker::kValue)).toInt());
  keep_ratio_ = settings.value(u"MainWindow.buttonRatio"_s, true).toBool();
  legend_status_ = static_cast<LegendStatus>(
      settings.value(u"MainWindow.legendStatus"_s, static_cast<int>(LegendStatus::kHidden)).toInt());

  // Plots from wireExistingPlots(); later plots get them in onPlotAdded.
  forEachPlot([this](PlotWidget* plot) { applyGlobalToggles(plot); });
  applyShowPointsTo2DWidgets();

  // Not checkable: the glyph shows the state, through the "iconPath" property applyIcons() tints. Not persisted:
  // every launch opens with only the left panel.
  for (const PanelToggle& toggle : panelToggles(ui_)) {
    const bool visible = (toggle.target == ui_->leftColumn);
    toggle.target->setVisible(visible);
    toggle.button->setCheckable(false);
    toggle.button->setProperty("iconPath", QString::fromLatin1(visible ? toggle.icon_path_on : toggle.icon_path_off));
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
      // The strip's expanded height survives fold cycles; applyBottomPanelConstraints owns the open floor and the
      // closed lock.
      if (target == ui_->timelineStrip) {
        const QList<int> sizes = ui_->timelineSplitter->sizes();
        const int total = sizes[0] + sizes[1];
        if (now_visible) {
          applyBottomPanelConstraints();  // lifts the cap, pins the open minimum
          const int floor = ui_->bottomPanel->minimumHeight();
          const int expanded = std::max(QSettings().value(kPanelBottomExpandedKey, sizes[1]).toInt(), floor);
          ui_->timelineSplitter->setSizes({total - expanded, expanded});
          // Deferred a tick, until the view is sized.
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

  // QSS alone can leave the platform's tooltip palette in place (a yellow box), so the palettes follow the theme too.
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
    // QToolTip's own palette, not QApplication's, styles the tooltip widget.
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

  connect(this, &MainWindow::chromeMetricsChanged, ui_->leftPanel, &LeftPanel::onChromeMetricsChanged);
  connect(this, &MainWindow::chromeMetricsChanged, ui_->curveListPanel, &CurveListPanel::onChromeMetricsChanged);
  connect(this, &MainWindow::chromeMetricsChanged, ui_->timelineWidget, &TimelineWidget::onChromeMetricsChanged);
  connect(this, &MainWindow::chromeMetricsChanged, ui_->tabbedPlotWidget, &TabbedPlotWidget::onChromeMetricsChanged);
  connect(this, &MainWindow::chromeMetricsChanged, title_bar_, &TitleBar::onChromeMetricsChanged);

  apply_theme_chrome();

  // Connected last, so the slots above have updated sizeHint(). QSplitter does not re-layout on a child's minimum
  // change and the closed bottom panel's cap holds the old height, so both are pushed explicitly; otherwise the
  // playback icons clip.
  connect(this, &MainWindow::chromeMetricsChanged, this, [this](const ChromeMetrics& metrics) {
    // The playback bar's height follows its buttons; this overrides both height bounds the .ui sets.
    const int playback_height = ui_->timelineWidget->sizeHint().height();
    ui_->timelineWidget->setMinimumHeight(playback_height);
    ui_->timelineWidget->setMaximumHeight(playback_height);

    const int band_extent = (metrics.icon_size + metrics.icon_padding) + (2 * metrics.layout_padding);
    // Six buttons on one FlowLayout row, never under the side-panel floor.
    ui_->localToolbarWidget->setMinimumWidth(qMax((6 * band_extent) + 6, 250));

    applyBottomPanelConstraints();
    // No-op until the playback bar is laid out (after show).
    alignNameColumnToPlayback();
  });

  // Dev-only widget inspector: Ctrl+Shift+D outline overlay, Ctrl+Shift+Q QSS debug borders.
  DebugUi::installInto(this, theme_.get());

  auto& playback = session_->playbackEngine();
  // Empty range: the transport stays disabled until the first dataset, as after a full removal.
  playback.setRangeAndCurrentTime(displayRange(0.0, 0.0), displaySeconds(0.0));
  ui_->timelineWidget->setPlaybackEngine(&playback);
  // Leading and trailing edge with the latest time; intermediate cursor times are dropped.
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

  // timelineStrip is empty in the .ui; the bottom-panel toggle and splitter already act on it.
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
  // alignNameColumnToPlayback re-applies it over the playback-aligned floor; layouts persist it.
  connect(source_timeline, &PJ::Timeline::nameColumnWidthChanged, this, [this](int width_px) {
    timeline_name_column_width_ = width_px;
  });
  source_timeline->setAutoZoomEnabled(true);
  // Needs source_timeline_controller_.
  buildTimelineAlignRail();
  connect(source_timeline, &PJ::Timeline::referenceLineMoved, this, [this](double display_seconds) {
    // Display seconds in the playback frame; stored as an absolute instant through the representative's full
    // displayOffset (source + global), so it survives offset changes.
    reference_instant_ = toAbsolute(
        fromAxisDouble(display_seconds), session_->sessionManager().displayOffset(representativeDatasetId()));
    const std::optional<double> ref_sec = referenceDisplaySeconds();
    forEachPlot([ref_sec](PlotWidget* plot) { plot->setReferenceLine(ref_sec); });
  });

  streaming_manager_ = std::make_unique<StreamingSourceManager>(
      session_->sessionManager(), session_->extensionCatalog(), session_->catalogModel(),
      session_->topicDemandTracker(), this, this);
  // So the stream dialog's SectionHeaderBands match the toolbox panels' band height.
  streaming_manager_->setChromeMetricsProvider([this] { return chrome_metrics_; });
  // Interactive placeholder drops' pending binds, outside a layout restore (which has its own connection).
  connect(&session_->catalogModel(), &CatalogModel::itemsAdded, this, [this](const std::vector<CatalogItem>& items) {
    // Scenes first: the binder then sees the completed queue and releases its demand reference.
    retryPendingSceneRestores(items);
    flushPendingCurveBindings(items);
  });
  connect(
      ui_->leftPanel, &LeftPanel::streamingBufferChanged, streaming_manager_.get(),
      &StreamingSourceManager::onBufferChanged);
  connect(
      ui_->leftPanel, &LeftPanel::streamingStartRequested, streaming_manager_.get(),
      &StreamingSourceManager::onStartRequested);
  connect(
      ui_->leftPanel, &LeftPanel::streamingPauseToggled, streaming_manager_.get(),
      &StreamingSourceManager::onPauseToggled);

  // Wired on every platform; only the Record affordances follow isSupported().
  ui_->leftPanel->setRecordingSupported(RecordingService::isSupported());
  recording_service_ = std::make_unique<RecordingService>(this);
  recording_service_->setSettings(RecordingService::loadSettings());

#ifndef __EMSCRIPTEN__
  // Folder and budget apply at the next launch (member doc); the capture toggle applies live
  // (bindRecordingPreferences).
  const SourceCacheStore::Settings cache_settings = SourceCacheStore::loadSettings();
  source_cache_store_ = SourceCacheStore::fromSettings(cache_settings);
  source_capture_service_ = std::make_unique<SourceCaptureService>(*source_cache_store_);
  source_capture_service_->setCaptureEnabled(cache_settings.capture_enabled);
#endif
  connect(ui_->leftPanel, &LeftPanel::streamingRecordToggled, this, &MainWindow::onRecordToggled);
  connect(recording_service_.get(), &RecordingService::started, this, &MainWindow::onRecordingStarted);
  connect(recording_service_.get(), &RecordingService::stopped, this, &MainWindow::onRecordingStopped);
  connect(recording_service_.get(), &RecordingService::progress, this, &MainWindow::onRecordingProgress);

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

  // While streaming, the slider spans the active stream only: the union of all datasets (what file loads use) would
  // shrink the streamed window to a sliver. The first live ingest seeds it (a drop may seed earlier; data alone
  // must be enough). Playing is following live: the engine holds at rangeMax between ingests instead of pausing;
  // pause unlocks scrubbing within the retained window, play snaps back to the newest sample.
  connect(streaming_manager_.get(), &StreamingSourceManager::streamStarted, this, [this](DatasetId id) {
    streams_in_launch_order_.push_back(id);
    active_streaming_dataset_id_ = id;
    session_->setActiveStreamingDataset(id);  // scope recomputeRange to the live tip while playing
    streaming_playback_seeded_ = false;
    auto& engine = session_->playbackEngine();
    engine.setHoldAtRangeMax(true);
    engine.setCurrentTime(engine.rangeMax());
    engine.play();
    refreshStreamingSeekLock();
  });
  connect(streaming_manager_.get(), &StreamingSourceManager::setupError, this, [this](const QString& message) {
    emitDiagnostic(DiagnosticLevel::kError, "Streaming", "stream-setup-failed", message);
    MessageBox::warning(this, tr("Start Streaming"), tr("The stream could not be started:\n\n%1").arg(message));
  });
  connect(
      streaming_manager_.get(), &StreamingSourceManager::streamError, this, [this](DatasetId, const QString& message) {
        emitDiagnostic(DiagnosticLevel::kError, "Streaming", "stream-failed", message);
        MessageBox::warning(this, tr("Streaming"), tr("The stream failed:\n\n%1").arg(message));
      });
  connect(
      streaming_manager_.get(), &StreamingSourceManager::streamStopped, this,
      [this, snap_streaming_playback_to_live_edge](DatasetId id, const QString& reason) {
        session_->sessionManager().notifyStreamEnded(id);
        std::erase(streams_in_launch_order_, id);
        if (active_streaming_dataset_id_ == id) {
          // The last stream launched that has a live edge takes over, as a start would have handed it; with none,
          // live-follow ends, since following a stream with no data would hold a frozen cursor.
          const auto has_data = [this](DatasetId stream) {
            return session_->sessionManager().datasetDisplayRange(stream).has_value();
          };
          const auto found = std::find_if(streams_in_launch_order_.rbegin(), streams_in_launch_order_.rend(), has_data);
          const DatasetId next = found == streams_in_launch_order_.rend() ? 0 : *found;
          active_streaming_dataset_id_ = next;
          session_->setActiveStreamingDataset(next);
          streaming_playback_seeded_ = false;
          if (next == 0) {
            session_->playbackEngine().setHoldAtRangeMax(false);
          } else if (session_->playbackEngine().isPlaying()) {
            (void)snap_streaming_playback_to_live_edge();
          }
          refreshStreamingSeekLock();
        }
        // Finalizes only that source's file; the capture ends with its last live source. Taps attach at start()
        // only, so a stream started later is never recorded.
        if (recording_service_ != nullptr && recording_service_->isRecording()) {
          recording_service_->onSourceEnded(static_cast<quint64>(id), reason);
        }
      });
  connect(&playback, &PlaybackEngine::playingChanged, this, [this, snap_streaming_playback_to_live_edge](bool playing) {
    if (active_streaming_dataset_id_ != 0 && playing) {
      session_->playbackEngine().setHoldAtRangeMax(true);
      (void)snap_streaming_playback_to_live_edge();
    }
    refreshStreamingSeekLock();
  });
  // A live stream's TF history follows the ObjectStore retention window (file loads keep everything). The headroom
  // covers the two trimming on independent ticks: a TF lookup must not fail for an object still stored.
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
      &session_->sessionManager(), &SessionManager::samplesIngested, this, [this](const QVector<TopicId>&, bool live) {
        auto& engine = session_->playbackEngine();
        if (live) {
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
            refreshStreamingSeekLock();
          }
          return;
        }
        // File ingest grows the union of every dataset: one file's range would clamp the cursor forward to each
        // new file's start in a multi-file load. The cursor is set on completion by seedPlaybackFromSession.
        session_->recomputeRange();
      });

  refreshStreamingCombo();
  connect(
      &session_->extensionCatalog(), &ExtensionCatalogService::catalogChanged, this,
      &MainWindow::refreshStreamingCombo);

  plugin_plot_tabs_ = std::make_unique<PluginPlotTabsController>(
      *ui_->tabbedPlotWidget, session_->catalogModel(), session_->sessionManager(), session_->extensionCatalog(),
      [this](const QString& kind, QWidget* dock_parent) { return makeSeededEmptyObjectDock(kind, dock_parent); });
  plugin_plot_tabs_->setOwnedTabCreatedHook(
      [this](const QString& plugin_id, PlotDocker* docker) { revealOwnedTabOverTakeover(plugin_id, docker); });

#ifndef PJ_TARGET_WASM
  // Every source is read when a plugin asks, so the catalog always describes
  // the current plugin set, installed records and registry URL.
  FeatureCatalogHost::Sources catalog_sources;
  catalog_sources.loaded = [this]() { return FeatureCatalogHost::loadedPlugins(session_->extensionCatalog()); };
  catalog_sources.installed = [this]() {
    return FeatureCatalogHost::installedRecords(session_->extensionCatalog().extensionManager());
  };
  catalog_sources.installable = [this](const Extension& ext) {
    return FeatureCatalogHost::installable(session_->extensionCatalog().extensionManager(), ext);
  };
  catalog_sources.registry_url = [this]() { return effectiveRegistryUrl(); };
  feature_catalog_ = std::make_unique<FeatureCatalogHost>(
      std::move(catalog_sources), featureCatalogResource(), QCoreApplication::applicationVersion().toStdString(),
      PlatformUtils::currentPlatform().toStdString());
#endif

  ui_->leftPanel->populateCloudToolboxes(session_->extensionCatalog().toolboxes());
  connect(&session_->extensionCatalog(), &ExtensionCatalogService::catalogChanged, this, [this]() {
    ui_->leftPanel->populateCloudToolboxes(session_->extensionCatalog().toolboxes());
    plugin_plot_tabs_->refreshAvailability();
    syncCustomTopicsEditor();
    refreshCustomWidgetsAvailability();
  });
  syncCustomTopicsEditor();
  refreshCustomWidgetsAvailability();

  file_loader_ = std::make_unique<FileLoader>(
      session_->sessionManager(), session_->extensionCatalog(), session_->catalogModel(), this);
#ifdef PJ_WITH_SCENE3D
  file_loader_->setTransformService(transform_service_.get());
#endif
  file_loader_->setDiagnosticSink(diagnosticSink());
  // Injected so FileLoader does not depend on MainWindow; the window is the dialog's chrome-metrics source.
  file_loader_->setFilePicker(
      [](QWidget* dialog_parent, const QString& caption, const QString& dir, const QString& filter, bool multi) {
        auto* metrics_source = dialog_parent != nullptr ? qobject_cast<MainWindow*>(dialog_parent->window()) : nullptr;
        if (multi) {
          return FileDialog::getOpenFileNames(dialog_parent, caption, dir, filter, metrics_source);
        }
        const QString path = FileDialog::getOpenFileName(dialog_parent, caption, dir, filter, metrics_source);
        return path.isEmpty() ? QStringList{} : QStringList{path};
      });
  file_loader_->setBatchAsker([](QWidget* dialog_parent, const QStringList& paths, auto done) {
    BatchLoadDialog::ask(
        dialog_parent, paths, /*ask_mode=*/true, /*list_files=*/false,
        [done = std::move(done)](std::optional<BatchLoadDialog::Answer> answer) {
          done(answer.has_value() ? answer->batch : nullptr);
        });
  });
  // Reload/Replace only for file-backed datasets.
  ui_->curveListPanel->setDatasetSourcePathResolver(
      [this](DatasetId dataset_id) { return file_loader_->sourcePathForDataset(dataset_id); });
  connect(ui_->curveListPanel, &CurveListPanel::reloadDatasetRequested, this, &MainWindow::onReloadDatasetRequested);
  connect(ui_->curveListPanel, &CurveListPanel::replaceDatasetRequested, this, &MainWindow::onReplaceDatasetRequested);
  connect(ui_->curveListPanel, &CurveListPanel::datasetInfoRequested, this, &MainWindow::onDatasetInfoRequested);
  connect(ui_->leftPanel, &LeftPanel::loadDataRequested, this, &MainWindow::onLoadDataRequested);
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  // Acceptance builds only: browser callbacks that invoke a slot or button directly, because Chromium can drop a
  // synthetic canvas gesture after many fresh WASM contexts and the canvas has no DOM element to target.
  pj_wasm_install_browser_test_probes();
#ifdef PJ_WITH_SCENE3D
  installWasmScene3dAcceptanceProbes();
#endif
  installWasmPlotAcceptanceProbes();
#endif
  connect(ui_->leftPanel, &LeftPanel::reloadDataRequested, this, &MainWindow::onReloadDataRequested);
  connect(ui_->leftPanel, &LeftPanel::cloudToolboxRequested, this, [this](const QString& id) { launchToolbox(id); });
  scene_undo_debounce_.setSingleShot(true);
  scene_undo_debounce_.setInterval(kSceneUndoDebounceMs);
  connect(&scene_undo_debounce_, &QTimer::timeout, this, [this]() { onUndoableChange(); });
  plugin_history_step_.setSingleShot(true);
  plugin_history_step_.setInterval(0);
  connect(&plugin_history_step_, &QTimer::timeout, this, [this]() { onUndoableChange(/*force_new_state=*/true); });

  connect(
      file_loader_.get(), &FileLoader::sourceReplacementAboutToCommit, this,
      [this](quint64 ticket, const QString& path, DatasetId replaced_id) {
        if (progressive_layout_in_flight_) {
          return;
        }
        // The incoming path (Replace points at another file), so the post-load rebind maps onto the replacement.
        const QString incoming = QFileInfo(path).absoluteFilePath();
        pending_source_replacement_ = PendingSourceReplacement{
            .workspace = capturePortableWorkspace(replaced_id, incoming), .path = incoming, .ticket = ticket};
      });
  connect(file_loader_.get(), &FileLoader::fileLoaded, this, &MainWindow::onFileLoaded);
  // loadFinished is the one terminal every accepted request reaches, including the exits fileLoadFailed stays silent
  // on (a rejected config dialog, a queued cancel, shutdown). It is queued, so onFileLoaded has already run.
  connect(file_loader_.get(), &FileLoader::loadFinished, this, &MainWindow::refreshReloadEnabled);
  // Disarms a replacement snapshot whose load rolled back: armed, a later load of the same path would rebind onto
  // an obsolete picture. By ticket, not path: a stale same-path terminal (a cancelled duplicate, a failed
  // predecessor) would disarm a live load's capture.
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
  connect(
      file_loader_.get(), &FileLoader::fileLoaded, this,
      [this](
          const QString& path, const QString& /*prefix*/, const QString& /*plugin_id*/,
          const QString& /*plugin_config_json*/) {
        // An upload identity expires with the page.
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
  // onLoadRecentLayout checks existence and prunes dead entries.
  connect(ui_->leftPanel, &LeftPanel::recentLayoutSelected, this, &MainWindow::onLoadRecentLayout);
#ifdef PJ_TARGET_WASM
  connect(ui_->leftPanel, &LeftPanel::clearRecentLayoutsRequested, this, [this]() {
    if (BrowserPersistence::instance() != nullptr) {
      BrowserPersistence::instance()->clearRecentLayouts();
    }
    ui_->leftPanel->setRecentEnabled(false);
  });
#endif
#ifdef PJ_TARGET_WASM
  const bool had_recent =
      BrowserPersistence::instance() != nullptr && !BrowserPersistence::instance()->recentLayouts().isEmpty();
#else
  const bool had_recent = !settings.value(u"File/recent"_s).toStringList().isEmpty() ||
                          !settings.value(u"Layout/recent"_s).toStringList().isEmpty();
#endif
  ui_->leftPanel->setRecentEnabled(had_recent);

  // Title-bar load strip. Its stop button opens a keep/discard/resume dialog that routes to
  // FileLoader::cancelCurrent.
  ingest_progress_ = new IngestProgressWidget(this);
  ingest_progress_->setActive(false);
  ingest_progress_->setPrimaryButton({}, u":/resources/svg/cancel.svg"_s, tr("Stop loading…"));
  title_bar_->setCenterWidget(ingest_progress_);
  ingest_show_timer_ = new QTimer(this);
  ingest_show_timer_->setSingleShot(true);
  // Progress shows on the curve-tree rows, so the strip stays hidden; everything behind it (displayed-owner
  // arbitration, stop dialog, counters) is still fed, and true brings it back. static: clang rejects odr-use of an
  // automatic constexpr in a lambda without a capture-default (GCC accepts it).
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
    // A file load or toolbox import may have taken the strip since the linger began.
    if (!session_->sessionManager().hasActiveIngests() && !file_loader_->isBusy()) {
      ingest_progress_->setActive(false);
      ingest_strip_owner_kind_ = IngestStripOwnerKind::kNone;
      ingest_strip_owner_dataset_ = 0;
    }
  });
  connect(ingest_progress_, &IngestProgressWidget::actionRequested, this, [this](IngestProgressWidget::Action) {
    // The load keeps running while the dialog is up.
    if (!file_loader_->isBusy()) {
      // No file load: Stop routes by the displayed owner. A layout batch's job takes the nonblocking keep-partial
      // per-job cancel: the job ends kCancelled, the batch continues with its next source, nothing rolls back.
      // Never batch->cancel(), and never a faked ToolboxIngestRef for a headless job.
      if (ingest_strip_owner_kind_ == IngestStripOwnerKind::kLayoutBatch && layoutImportBatchActive()) {
        layout_import_batch_->cancelActiveImportJob();
        return;  // the strip hands off when the job's ingest ends
      }
      // Interactive toolbox imports: a cooperative cancel of every live importing host, no dialog. The batch never
      // registers a ToolboxIngestRef, so this cannot reach it.
      if (session_->sessionManager().hasActiveIngests()) {
        stopAllToolboxImports();
        return;  // strip hides when on_ingest_finished drains the import set
      }
      // Clicked during the post-completion linger.
      ingest_progress_->setActive(false);
      return;
    }
    if (!ingest_stop_dialog_.isNull()) {
      ingest_stop_dialog_->raise();
      ingest_stop_dialog_->activateWindow();
      return;
    }

    // The browser main thread cannot enter a nested QDialog event loop: continue from finished.
    auto* dialog = new MessageBox(this);
    ingest_stop_dialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    // Window-modality would leave floating ADS dock tool-windows interactive.
    dialog->setWindowModality(Qt::ApplicationModal);
    dialog->setTitle(tr("Stop loading?"));
    dialog->setText(tr("This data is still loading. Keep what has loaded so far, remove all of it, or keep loading?"));
    dialog->addButton(tr("Remove All"), MessageBox::kDestructiveRole);  // index 0 — stop and discard
    dialog->addButton(tr("Stop and Keep"), MessageBox::kPrimaryRole);   // index 1 — stop, keep partial
    dialog->addButton(tr("Cancel"), MessageBox::kCancelRole);           // index 2 — resume loading

    // Bound to the load it was raised for: dismissed when the queue drains or the next load takes over, so it can
    // never act on the wrong load.
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
    // Not open(): it downgrades ApplicationModal to WindowModal.
    dialog->show();
  });
  connect(
      file_loader_.get(), &FileLoader::ingestStarted, this,
      [this](const QString& title, int index, int total, bool /*determinate*/) {
        // A file load takes the strip even from a displayed toolbox or batch ingest.
        ingest_strip_owner_kind_ = IngestStripOwnerKind::kFile;
        ingest_strip_owner_dataset_ = 0;
        ingest_progress_->setTitle(title);
        ingest_progress_->setCounterText(total > 1 ? u"%1/%2"_s.arg(index).arg(total) : QString());
        ingest_progress_->setRange(0, 0);  // busy until the first determinate progress tick
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
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
    // A toolbox or batch ingest still running takes the strip over, with its own label and progress.
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

  // Hides the badge for this session; the next launch's scan decides again.
  connect(title_bar_, &TitleBar::extensionUpdateRequested, this, [this]() {
    title_bar_->setExtensionUpdateCount(0);
    onOpenMarketplace();
  });

  // Keyboard-only, in no menu. Added to the window so the shortcuts fire window-wide: an action that lives only in a
  // popup menu does not fire while the menu is closed.
  undo_action_ = new QAction(tr("Undo"), this);
  undo_action_->setShortcuts(QKeySequence::Undo);
  connect(undo_action_, &QAction::triggered, this, &MainWindow::onUndo);
  addAction(undo_action_);
  redo_action_ = new QAction(tr("Redo"), this);
  // QKeySequence::Redo depends on the desktop theme, so both keys are added. Each key once: Qt treats a key bound
  // twice to one action as ambiguous and fires nothing.
  QList<QKeySequence> redo_shortcuts = QKeySequence::keyBindings(QKeySequence::Redo);
  for (const QKeySequence& key : {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), QKeySequence(Qt::CTRL | Qt::Key_Y)}) {
    if (!redo_shortcuts.contains(key)) {
      redo_shortcuts.append(key);
    }
  }
  redo_action_->setShortcuts(redo_shortcuts);
  connect(redo_action_, &QAction::triggered, this, &MainWindow::onRedo);
  addAction(redo_action_);

  // A lambda: a function-pointer connect rejects onUndoableChange's extra (defaulted) argument.
  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::undoableChange, this, [this] { onUndoableChange(); });

  // "Align starts", off by default so the Source Timeline shows the datasets' natural offsets. Toggling it later
  // rewrites every dataset's offset.
  session_->sessionManager().setUseTimeOffset(QSettings().value(u"MainWindow.useTimeOffset"_s, false).toBool());
  ui_->timelineWidget->setUseTimeOffset(session_->sessionManager().useTimeOffset());
  connect(ui_->timelineWidget, &TimelineWidget::useTimeOffsetToggled, this, [this](bool checked) {
    QSettings().setValue(u"MainWindow.useTimeOffset"_s, checked);
    onUseTimeOffsetToggled(checked);
  });

  ui_->timelineWidget->setUseDateTime(QSettings().value(kDateTimeDisplayKey, false).toBool());
  connect(ui_->timelineWidget, &TimelineWidget::useDateTimeToggled, this, [this](bool checked) {
    QSettings().setValue(kDateTimeDisplayKey, checked);
    applyTimeLabelFormat();
  });
  applyTimeLabelFormat();

  buildGlobalToolbar();

  // One page per focused widget type; onDockFocused() picks it.
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
  custom_widgets_ = new CustomWidgetController(
      *right_panel_stack_, *session_, diagnosticSink(),
      CustomWidgetController::Hooks{
          .create_dock = [this](
                             DockWidget* dock, const QString& plugin_id,
                             const QString& type) { createCustomWidgetDock(dock, plugin_id, type); },
          .focus_dock = [this](DockWidget* dock) { onDockFocused(dock); },
      },
      this);
  custom_widgets_->onStylesheetChanged(theme_->currentTheme());
  connect(this, &MainWindow::stylesheetChanged, custom_widgets_, &CustomWidgetController::onStylesheetChanged);
  connect(this, &MainWindow::chromeMetricsChanged, custom_widgets_, &CustomWidgetController::onChromeMetricsChanged);
  right_panel_stack_->setCurrentWidget(plot_config_page_);

  buildLocalToolbar();

  // After the build* calls, which connect their own listeners: everything built with .ui defaults re-renders.
  emit chromeMetricsChanged(chrome_metrics_);

  curve_editor_ = new CurveEditor(plot_config_page_);
  plot_config_layout->addWidget(curve_editor_, /*stretch=*/1);
  // Anchors the strips at the top while the editor is hidden; the editor's stretch wins while it is shown.
  plot_config_layout->addStretch(0);
  curve_editor_->onStylesheetChanged(theme_->currentTheme());
  curve_editor_->onChromeMetricsChanged(chrome_metrics_);
  connect(this, &MainWindow::stylesheetChanged, curve_editor_, &CurveEditor::onStylesheetChanged);
  connect(this, &MainWindow::chromeMetricsChanged, curve_editor_, &CurveEditor::onChromeMetricsChanged);
  connect(ui_->tabbedPlotWidget, &TabbedPlotWidget::currentTabChanged, this, [this](PlotDocker* /*docker*/) {
    // A tab switch emits no ADS focus.
    onDockFocused(activeFocusedDock());
    // The fan-out skips hidden tabs, so the newly shown one missed every cursor move.
    if (session_ != nullptr) {
      broadcastTrackerTimeToVisible(toAxisDouble(session_->playbackEngine().currentTime()));
    }
  });
  bindEditorToPlot(firstPlotOfActiveTab());

  pushInitialUndoState();
  updateUndoRedoActions();

  // Scroll areas that exist now; panels created later attach their own.
  attachPillScrollbars(this);
}

IDataWidget* MainWindow::makeSceneDock(const QString& kind, QWidget* parent) {
  // The one construct-and-wire site for object docks, shared by drop and restore.
#ifdef PJ_WITH_SCENE3D
  if (kind == "scene3d"_L1) {
    usageCounters().count(UsageFeature::kPanelScene3D);
    auto* widget = new Scene3DDockWidget(parent);
    connect(widget, &SceneDockWidget::pendingRestoresChanged, this, &MainWindow::schedulePendingDisplayBindingRebuild);
    connect(widget, &SceneDockWidget::topicRefused, this, [this](const QString& reason) { showToast(reason); });
    connect(widget, &SceneDockWidget::workspaceChanged, &scene_undo_debounce_, qOverload<>(&QTimer::start));
    widget->setSessionManager(&session_->sessionManager());
    widget->setTransformService(transform_service_.get());
    widget->setFixedFrameMemory(fixed_frame_memory_.get());
    widget->setSettings(app_settings_.get());
    topic_demand_controller_->registerSceneDock(widget);
    // Approximation: the most recent load, which may not be this dock's dataset in a multi-file session.
    if (const auto src = session_->sessionManager().lastLoadedSource(); src.has_value()) {
      widget->setSourcePath(src->path);
    }
    // Once the lazy view exists it is seeded with the panel's current control values (the panel binds only the
    // focused dock); a layout's per-dock <scene_controls> then override the seed (Scene3DDockWidget::xmlLoadState).
    if (scene3d_config_panel_ != nullptr) {
      auto* panel = scene3d_config_panel_;
      connect(widget, &Scene3DDockWidget::sceneViewReady, panel, [panel, widget]() {
        panel->applySceneControlsTo(widget);
      });
      if (widget->sceneView() != nullptr) {
        panel->applySceneControlsTo(widget);
      }
    }
    // No theme push: SceneViewWidget follows its palette (QEvent::PaletteChange).
    return widget;
  }
#endif
#ifdef PJ_WITH_SCENE2D
  if (kind == "scene2d"_L1) {
    usageCounters().count(UsageFeature::kPanelScene2D);
    auto* widget = new Scene2DDockWidget(parent);
    connect(widget, &SceneDockWidget::pendingRestoresChanged, this, &MainWindow::schedulePendingDisplayBindingRebuild);
    connect(widget, &SceneDockWidget::topicRefused, this, [this](const QString& reason) { showToast(reason); });
    connect(widget, &SceneDockWidget::workspaceChanged, &scene_undo_debounce_, qOverload<>(&QTimer::start));
    widget->setSessionManager(&session_->sessionManager());
    topic_demand_controller_->registerSceneDock(widget);
    return widget;
  }
#endif
  if (kind == "CustomWidget"_L1) {
    auto* widget = new CustomWidgetDockWidget(
        &session_->sessionManager(), &session_->catalogModel(), &session_->extensionCatalog(),
        &session_->playbackEngine(), parent);
    widget->setDiagnosticSink(diagnosticSink());
    widget->setPlaybackTimeMappers(
        std::bind_front(&MainWindow::displayTimeForTopic, this),
        std::bind_front(&MainWindow::displayTimeForSource, this));
    if (custom_widgets_ != nullptr) {
      custom_widgets_->attachDock(*widget);
    }
    connect(widget, &CustomWidgetDockWidget::workspaceChanged, &scene_undo_debounce_, qOverload<>(&QTimer::start));
    connect(widget, &CustomWidgetDockWidget::bindingsChanged, this, &MainWindow::schedulePendingDisplayBindingRebuild);
    topic_demand_controller_->registerCustomWidgetDock(widget);
    return widget;
  }
  if (kind == "state_transitions"_L1) {
    auto* widget = new StateTransitionsDockWidget(
        &session_->sessionManager(), &session_->catalogModel(), &session_->playbackEngine(), parent);
    connect(widget, &StateTransitionsDockWidget::workspaceChanged, &scene_undo_debounce_, qOverload<>(&QTimer::start));
    connect(
        widget->view(), &StateTransitionsView::visibleRangeChanged, this,
        [this, widget](double t_min, double t_max) { onStateTransitionsRangeChanged(widget, t_min, t_max); });
    topic_demand_controller_->registerStateTransitionsDock(widget);
    return widget;
  }
  if (kind == "log"_L1) {
    usageCounters().count(UsageFeature::kPanelLog);
    auto* widget =
        new LogDockWidget(&session_->sessionManager(), &session_->catalogModel(), &session_->playbackEngine(), parent);
    widget->setTimeLabelFormat(time_label_format_);
    connect(widget, &LogDockWidget::workspaceChanged, &scene_undo_debounce_, qOverload<>(&QTimer::start));
    connect(widget, &LogDockWidget::topicsChanged, this, &MainWindow::schedulePendingDisplayBindingRebuild);
    topic_demand_controller_->registerLogDock(widget);
    return widget;
  }
  return nullptr;
}

EmbeddedViewHost MainWindow::makeEmbeddedViewHost() {
  EmbeddedViewHost host;
#if defined(PJ_WITH_SCENE3D) || defined(PJ_WITH_SCENE2D)
  host.create = [this](const QString& kind, QWidget* parent) -> QWidget* {
    IDataWidget* widget = makeSeededEmptyObjectDock(sceneWidgetKind(kind), parent);
    auto* view = widget == nullptr ? nullptr : qobject_cast<SceneDockWidget*>(widget->widget());
    if (view == nullptr) {
      return nullptr;
    }
    // The plugin owns the view: its changes snapshot no workspace, and a refusal
    // reaches the plugin as scene_view_error instead of a toast.
    disconnect(view, &SceneDockWidget::workspaceChanged, &scene_undo_debounce_, nullptr);
    disconnect(view, &SceneDockWidget::topicRefused, this, nullptr);
    embedded_scene_views_.append(view);
    frameDroppedScene3dContent(widget);
    return view;
  };
  // Every toolbox panel is torn down before the window resets the controller.
  PluginSceneViewsController& scene_views = plugin_plot_tabs_->sceneViews();
  // A detach undoes the topic its attach resolved: by then the name may resolve to
  // another dataset's topic, or to none.
  host.attach = [&scene_views](QWidget* view, const QString& topic, const QString& dataset, std::string* error) {
    const Expected<ObjectTopicId> attached =
        scene_views.attachEmbeddedTopic(qobject_cast<SceneDockWidget*>(view), topic, dataset);
    if (!attached) {
      if (error != nullptr) {
        *error = attached.error();
      }
      return false;
    }
    view->setProperty(embeddedAttachedKey(topic, dataset).constData(), attached->id);
    return true;
  };
  // Two requests may name one topic (by bare name and by dataset): its layer goes
  // with the last of them.
  host.detach = [](QWidget* view, const QString& topic, const QString& dataset) {
    const QByteArray key = embeddedAttachedKey(topic, dataset);
    const uint32_t id = embeddedAttachedId(*view, key);
    view->setProperty(key.constData(), QVariant());
    const QList<QByteArray> others = embeddedAttachedKeys(*view);
    const bool held = std::any_of(others.begin(), others.end(), [view, id](const QByteArray& other) {
      return embeddedAttachedId(*view, other) == id;
    });
    if (auto* scene = qobject_cast<SceneDockWidget*>(view); scene != nullptr && id != 0 && !held) {
      (void)scene->detachTopic(ObjectTopicId{id});
    }
  };
#endif
  return host;
}

void MainWindow::reattachEmbeddedTopics(SceneDockWidget& view) {
  for (const QByteArray& key : embeddedAttachedKeys(view)) {
    const uint32_t id = embeddedAttachedId(view, key);
    if (id != 0 && view.layerFor(ObjectTopicId{id}) != nullptr) {
      continue;
    }
    const QByteArray entry = key.mid(QByteArrayView(kEmbeddedAttachedPrefix).size());
    const qsizetype split = entry.indexOf('\n');
    const Expected<ObjectTopicId> attached = plugin_plot_tabs_->sceneViews().attachEmbeddedTopic(
        &view, QString::fromUtf8(entry.mid(split + 1)), QString::fromUtf8(entry.left(split)));
    view.setProperty(key.constData(), attached ? attached->id : 0U);
  }
}

void MainWindow::tickEmbeddedViews(double display_seconds, bool visible_only) {
  embedded_scene_views_.removeIf([](const QPointer<SceneDockWidget>& view) { return view.isNull(); });
  for (const QPointer<SceneDockWidget>& view : embedded_scene_views_) {
    if (!visible_only || view->isVisible()) {
      view->onTrackerTime(display_seconds);
    }
  }
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
  // No default: a new enumerator must be a compile error here, the one site that maps a family to a kind.
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
    case VisualizationKind::kLog:
      kind = u"log"_s;
      break;
    case VisualizationKind::kCustomWidget:
      // Chosen in the picker menu first: building before the choice would leave an unbound panel behind.
      if (custom_widgets_ != nullptr) {
        custom_widgets_->showPicker(dock);
      }
      return;
    case VisualizationKind::kPlot:  // DockWidget builds plots itself
      return;
  }
  IDataWidget* widget = makeSeededEmptyObjectDock(kind, dock);
  if (widget == nullptr) {
    qCWarning(lcMain) << "onObjectFamilyRequested: could not build scene dock for kind" << kind;
  }
  dock->adoptObjectWidget(widget);  // null still reverts to a usable placeholder
}

bool MainWindow::createCustomWidgetDock(DockWidget* dock, const QString& plugin_id, const QString& type) {
  if (dock == nullptr) {
    return false;
  }
  IDataWidget* widget = makeSeededEmptyObjectDock(u"CustomWidget"_s, dock);
  auto* custom = widget != nullptr ? qobject_cast<CustomWidgetDockWidget*>(widget->widget()) : nullptr;
  if (custom == nullptr) {
    qCWarning(lcMain) << "createCustomWidgetDock: could not build the Custom Widget dock";
    dock->adoptObjectWidget(nullptr);
    return false;
  }
  // Bound before the dock mounts it, so the mount sees the plugin's widget tree.
  const bool bound = custom->bindToWidget(plugin_id, type);
  dock->adoptObjectWidget(widget);
  return bound;
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
  } else if (
      auto* log = qobject_cast<LogDockWidget*>(object_widget != nullptr ? object_widget->widget() : nullptr);
      log != nullptr && object_type == sdk::BuiltinObjectType::kLog) {
    topic_demand_controller_->handleLogDockPlaceholderDrop(log, dataset_id, topic_name);
  }
}

MainWindow::~MainWindow() {
  QDesktopServices::unsetUrlHandler(u"pj-update"_s);
  // Before `delete ui_`: stop() emits stopped(), whose slot touches the left panel, and a quit that never ran
  // closeEvent (--screenshot, --exit-after-layout) reaches here with recording live.
  if (recording_service_ != nullptr) {
    recording_service_->stop();
  }
  // A stream start still at its dialog discards its dataset when withdrawn, which the catalog
  // announces to slots that touch the widgets: before they go, not from the member's destructor.
  if (streaming_manager_ != nullptr) {
    streaming_manager_->cancelPendingStarts();
  }
  usageCounters().flush();  // a no-op unless the usage report is on
  // Before the toolboxes: a running batch's host teardown fires on_ingest_finished -> endIngest, which would reach
  // a still-connected observer.
  teardownLayoutBatchObservation();
  // Every toolbox (takeover, tab, floating window) holds a plugin session writing into the AppSession's engine:
  // torn down while the session, tab strip and engines live. A floating window is a QObject child and the
  // base-class child sweep runs after the members die.
  teardownAllToolboxes();
  // The MCP server goes right after the toolboxes, whose endpoints revoked their tokens as they
  // went: no plugin is left to start it again, and its agent sessions point into the services below.
  mcp_server_.reset();
  // Only a ToolboxPresenter sets current_panel_engine_ and releasing its panel clears it, so after
  // teardownAllToolboxes this delete is a guard. A remaining takeover (marketplace, filter editor) has no engine; it
  // is deleted now, not deferred: no event loop runs after this.
  delete current_panel_engine_;
  current_panel_engine_ = nullptr;
  if (current_panel_ != nullptr) {
    delete releaseCentralPanel();
  }
  // Docks go before session_ unloads the plugin libraries they may depend on (a Scene2D video layer keeps
  // parser-owned records and a decode thread). ~ads::CDockManager deletes its docks in place.
  plugin_plot_tabs_.reset();  // its tab-strip reference must not outlive the strip
  delete ui_->tabbedPlotWidget;
  // Every Custom Widget dock died above, each removing its entry; clearing here also covers one
  // that never got the chance. Reaching this line is the clean exit the record is about.
  if (custom_widgets_ != nullptr) {
    custom_widgets_->clearActiveWidgets();
  }
  // So no late signal reaches the engines session_ destroys.
  ui_->timelineWidget->setPlaybackEngine(nullptr);
  ui_->curveListPanel->setCatalog(nullptr);
  // The loader member dies after this body and its destructor delivers queued terminals (loadFinished)
  // synchronously, into slots that touch the widgets.
  file_loader_->disconnect(this);
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

  // The packaging smokes' fixture layout (packaging/smoke/) shows this image in a Scene2D view.
  constexpr uint32_t kImageSide = 64;
  std::vector<uint8_t> pixels(static_cast<size_t>(kImageSide) * kImageSide * 4);
  for (uint32_t y = 0; y < kImageSide; ++y) {
    for (uint32_t x = 0; x < kImageSide; ++x) {
      uint8_t* px = pixels.data() + ((static_cast<size_t>(y) * kImageSide) + x) * 4;
      px[0] = static_cast<uint8_t>(x * 4);
      px[1] = static_cast<uint8_t>(y * 4);
      px[2] = 128;
      px[3] = 255;
    }
  }
  sdk::Image image;
  image.width = kImageSide;
  image.height = kImageSide;
  image.encoding = "rgba8";
  image.data = Span<const uint8_t>(pixels.data(), pixels.size());
  ObjectStore& objects = session_->sessionManager().objectStore();
  const auto image_topic = objects.registerTopic(
      ObjectTopicDescriptor{
          .dataset_id = *dataset_or,
          .topic_name = "test/image",
          .metadata_json = R"({"builtin_object_type":"kImage","image_codec":"pj_image_v1"})"});
  if (!image_topic.has_value() || !objects.pushOwned(*image_topic, 0, serializeImage(image)).has_value()) {
    qCWarning(lcMain) << "test image topic could not be written";
    return false;
  }
  session_->catalogModel().rebuildFromDatastore();
  session_->playbackEngine().setRange(displayRange(0.0, kTestDurationSeconds));
  emitDiagnostic(DiagnosticLevel::kInfo, "TestData", "loaded", tr("Loaded test sin/cos data"));
  return true;
}

QDomDocument MainWindow::browserGenericLayoutDocument() const {
  QDomDocument doc = xmlSaveState();
  doc.documentElement().setAttribute(u"binding"_s, u"generic"_s);
  layout_xml::removeDatasetQualifiersForGenericLayout(doc);
  doc.documentElement().appendChild(saveRightPanelState(doc));
  doc.documentElement().appendChild(ui_->leftPanel->saveSourcesState(doc, recordsStreams(doc)));
  doc.documentElement().appendChild(ui_->curveListPanel->saveListState(doc));
  doc.documentElement().appendChild(saveChromeState(doc));
  doc.documentElement().appendChild(saveSourceTimelineViewState(doc));
  return doc;
}

QList<layout_xml::StreamingSourceRef> MainWindow::liveStreamRecords(
    QHash<std::uint32_t, QString>& identity_for_dataset) const {
  if (streaming_manager_ == nullptr) {
    return {};
  }
  QList<layout_xml::StreamingSourceRef> records = streaming_manager_->layoutSources();
  // A stream started without a configuration (a plugin with no dialog, an agent's bare
  // start) cannot start again from a file: it is left out, and what it draws saves unbound.
  records.removeIf(
      [](const layout_xml::StreamingSourceRef& record) { return record.plugin_config_json.trimmed().isEmpty(); });
  if (records.isEmpty()) {
    return records;
  }
  QHash<QString, DatasetId> dataset_for_id;
  for (const auto& [dataset_id, id] : streaming_manager_->layoutIds()) {
    dataset_for_id.insert(id, dataset_id);
  }
  const QHash<DatasetId, int> slot_of = timelineSlots();
  for (layout_xml::StreamingSourceRef& record : records) {
    const DatasetId dataset_id = dataset_for_id.value(record.id);
    identity_for_dataset.insert(dataset_id, layout_xml::streamSourceIdentity(record.id));
    record.dataset.timeline_order = slot_of.value(dataset_id, -1);
  }
  return records;
}

QHash<DatasetId, int> MainWindow::timelineSlots() const {
  QHash<DatasetId, int> slot_of;
  if (source_timeline_controller_ != nullptr) {
    const std::vector<DatasetId> order = source_timeline_controller_->currentTrackOrder();
    for (int slot = 0; slot < static_cast<int>(order.size()); ++slot) {
      slot_of.insert(order[static_cast<std::size_t>(slot)], slot);
    }
  }
  return slot_of;
}

std::optional<QDomDocument> MainWindow::browserSourceLayoutDocument(QString& error) const {
  error.clear();
  if (browser_layout_runtime_ == nullptr) {
    error = tr("Browser source state is unavailable.");
    return std::nullopt;
  }

  // Every live stream is saved as its record; the datasets it writes into are
  // named by that record, never by a file.
  QHash<std::uint32_t, QString> stream_identity_for_dataset;
  const QList<layout_xml::StreamingSourceRef> stream_records = liveStreamRecords(stream_identity_for_dataset);
  const std::vector<std::pair<DatasetId, QString>> live_datasets = session_->catalogModel().datasets();
  if (live_datasets.empty() && stream_records.isEmpty()) {
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
    if (stream_identity_for_dataset.contains(dataset_id)) {
      continue;
    }
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

  // Replayable only while its whole original fan-out is live: ids reject a look-alike sibling, names catch order
  // drift.
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
  const QHash<DatasetId, int> timeline_order = timelineSlots();

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
    // The live fingerprint, not the record's (taken when the load finished): a picked file is fingerprinted while it
    // loads, and loses its fingerprint when a read of it fails.
    const QString content_sha256 = file_loader_->browserContentSha256(source.identity);
    if (content_sha256.isEmpty()) {
      error = tr("'%1' has no content fingerprint yet: it is still being computed, or the file changed since it was "
                 "picked. Save the layout again in a moment, or open the file again.")
                  .arg(basename);
      return std::nullopt;
    }
    // Several sources get an ordinal directory each, so duplicate basenames never alias in fileInfo, curve,
    // processor or scene qualifiers.
    const QString logical_path =
        sources.size() == 1 ? basename : u"pj4-sources/%1/%2"_s.arg(source_index).arg(basename);
    logical_path_for_identity.insert(source.identity, logical_path);

    QDomElement file_info = doc.createElement(u"fileInfo"_s);
    file_info.setAttribute(u"filename"_s, logical_path);
    file_info.setAttribute(u"prefix"_s, record.prefix);
    file_info.setAttribute(u"content_sha256"_s, content_sha256);
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
      // Both identities, like the desktop writer: readers match manifest_id first, older ones read ID.
      if (!record.plugin_manifest_id.isEmpty()) {
        plugin.setAttribute(u"manifest_id"_s, record.plugin_manifest_id);
      }
      plugin.setAttribute(u"filepath_mode"_s, u"source"_s);
      // Files the plugin reads besides the data file (one picked in its dialog) are saved like the data file: a
      // logical name and a fingerprint, reselected on replay.
      QString side_error;
      const std::optional<layout_xml::LogicalSideFiles> side_files = layout_xml::logicalBrowserSideFiles(
          record.plugin_config_json.toStdString(), file_loader_->browserPathForIdentity(source.identity), logical_path,
          [this](const QString& path) -> std::optional<layout_xml::StagedFileInfo> {
            const QString identity = file_loader_->browserIdentityForPath(path);
            if (identity.isEmpty()) {
              return std::nullopt;
            }
            return layout_xml::StagedFileInfo{
                .name = BrowserFileStore::sanitizedBasename(BrowserFileStore::displayNameForIdentity(identity)),
                .content_sha256 = file_loader_->browserContentSha256(identity),
            };
          },
          &side_error);
      if (!side_files.has_value()) {
        error = side_error;
        return std::nullopt;
      }
      for (const layout_xml::SideFileRef& side_file : side_files->side_files) {
        QDomElement side = doc.createElement(u"sideFile"_s);
        side.setAttribute(u"filename"_s, side_file.filename);
        side.setAttribute(u"content_sha256"_s, side_file.content_sha256);
        file_info.appendChild(side);
      }
      // A config that is not a JSON object cannot be rebound; fall back to the
      // minimal `{"filepath": logical}` rather than persist it unrewritten.
      std::optional<std::string> logical_config =
          detail::rewriteReplayFilepaths(side_files->config, logical_path, detail::SavedFilepath::kOmit);
      if (!logical_config.has_value()) {
        logical_config = detail::rewriteReplayFilepaths({}, logical_path, detail::SavedFilepath::kOmit);
      }
      appendJsonAsCdata(doc, plugin, QString::fromStdString(*logical_config));
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
  layout_xml::stampDatasetIdentities(doc, stream_identity_for_dataset);
  layout_xml::removeUnvalidatedDatasetIds(doc);
  if (!layout_xml::writeStreamingSources(doc, stream_records, &error)) {
    return std::nullopt;
  }

  root.appendChild(saveRightPanelState(doc));
  root.appendChild(ui_->leftPanel->saveSourcesState(doc, recordsStreams(doc)));
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

#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
QByteArray MainWindow::wasmProbeLayoutBytes() const {
  // The same serialized document as the browser download.
  const QDomDocument doc = browserGenericLayoutDocument();
  return doc.toByteArray(2);
}

qsizetype MainWindow::wasmProbeDatasetCount() const {
  return static_cast<qsizetype>(session_->catalogModel().datasets().size());
}

std::size_t MainWindow::wasmProbeResidentPoolCapacity() const {
  return session_->sessionManager().residentPayloadPool()->stats().capacity_bytes;
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

void MainWindow::wasmProbeLaunchToolbox(const QString& plugin_id) {
  launchToolbox(plugin_id);
}

void MainWindow::wasmProbeReportToolboxTransformResult() const {
  const DataProcessorService& processors = session_->sessionManager().dataProcessorService();
  const auto recipes = processors.transformRecipes();
  qInfo(
      "PJ_WASM_TOOLBOX_TRANSFORMS count=%llu materializing=%d", static_cast<unsigned long long>(recipes.size()),
      processors.materializationPending() ? 1 : 0);

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

  // Already the central panel: re-activate, keeping scroll and filter and not re-reading the registry. A
  // marketplace migrated into a tab keeps its controller but frees the central area, which gets a fresh instance.
  if (!marketplace_panel_.isNull() && current_panel_ == marketplace_container_.data() && current_panel_ != nullptr) {
    marketplace_panel_->activateEmbedded();
    return;
  }

  // MarketplaceWindow stays a hidden controller that owns the UI; its content widget is lifted into a takeover
  // panel that can be closed or migrated to a tab.
  auto* mkt = new MarketplaceWindow(&catalog.extensionManager(), effectiveRegistryUrl(), this);
  mkt->setChromeMetrics(chrome_metrics_);
  trackRegistryFetches(mkt->registryManager());
  QWidget* content = mkt->contentWidget();

  // Deferred, so the dialog does not run inside the panel's click handling.
  connect(mkt, &MarketplaceWindow::pluginPreferencesRequested, this, [this]() {
    QTimer::singleShot(0, this, [this]() {
      PreferencesDialog prefs(*theme_, this);
      prefs.setChromeMetrics(chrome_metrics_);
      bindRecordingPreferences(prefs);
      bindPlottingPreferences(prefs);
      bindScenePreferences(prefs);
      prefs.showPage(PreferencesDialog::kPluginsPage);
      prefs.exec();
    });
  });

  // Guarded: on the migrate exit this runs when the tab closes, and the container may have taken the controller
  // with it.
  const auto reload_if_changed = [this, mkt = QPointer<MarketplaceWindow>(mkt)]() {
    if (!mkt.isNull() && mkt->installationsChanged()) {
      session_->extensionCatalog().reload();
    }
  };

  // No presenter routes this panel, so its close finds where it is: a tab closes through the strip (whose on_close
  // reloads), a takeover restores the chart.
  const WrappedToolboxPanel wrapped = wrapToolboxPanel(
      content, tr("Marketplace"),
      ToolboxChromeHooks{
          .on_close =
              [this, reload_if_changed]() {
                if (current_panel_ != nullptr && current_panel_ == marketplace_container_.data()) {
                  reload_if_changed();
                  restoreCentralArea();
                } else if (!marketplace_container_.isNull()) {
                  ui_->tabbedPlotWidget->closeWidgetTab(marketplace_container_.data());
                }
              },
          .on_migrate =
              [this, reload_if_changed]() {
                QWidget* released = releaseCentralPanel();
                if (released == nullptr) {
                  return;
                }
                ui_->tabbedPlotWidget->addWidgetTab(
                    tr("Marketplace"), released, [reload_if_changed]() { reload_if_changed(); });
              },
          .on_float = {},
          .on_dock = {}});

  // The controller must not outlive the container, which owns every widget its ui_ points at: presentPanel() can
  // delete the container without running either callback, and a surviving controller's ExtensionManager
  // install/uninstall slots would write into freed widgets. The controller as context drops the connection when
  // it dies first (the migrate exit).
  connect(wrapped.container, &QObject::destroyed, mkt, &QObject::deleteLater);

  presentPanel(wrapped.container);
  // QPointers: they clear themselves on every exit path.
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

LoadRequestId MainWindow::reloadSource(
    const QString& path, const QString& plugin_id, const QString& plugin_config_json,
    std::optional<DialogPolicy> policy, std::function<bool()> still_allowed) {
  // Re-enabled only by refreshReloadEnabled (loadFinished, or the refusal below).
  ui_->leftPanel->setReloadEnabled(false);
  LoadHints hints{
      .expected_manifest_id = QString(),  // interactive reload: the display name is the recorded identity
      .expected_plugin_id = plugin_id,
      .preset_config_json = plugin_config_json,
      .dialog_policy = policy.value_or(
          !plugin_id.isEmpty() && !plugin_config_json.isEmpty() ? DialogPolicy::kPreferPreset
                                                                : DialogPolicy::kInteractive),
      .still_allowed = std::move(still_allowed),
  };
  const LoadRequestId request = file_loader_->loadFileTicketed(LoadInput::fromNativePath(path), this, hints);
  if (request == 0) {
    refreshReloadEnabled();  // refused outright: no ticket, so no loadFinished
  }
  return request;
}

void MainWindow::refreshReloadEnabled() {
  // Browser uploads are never recorded as reloadable (onFileLoaded): rereading needs a fresh pick gesture.
  ui_->leftPanel->setReloadEnabled(session_->sessionManager().lastLoadedSource().has_value());
}

void MainWindow::onReloadDatasetRequested(DatasetId dataset_id) {
  reloadDataset(dataset_id);
}

LoadRequestId MainWindow::reloadDataset(
    DatasetId dataset_id, std::optional<DialogPolicy> policy, std::function<bool()> still_allowed) {
  const QString path = file_loader_->sourcePathForDataset(dataset_id);
  if (path.isEmpty()) {
    return 0;  // not file-backed; the menu item should have been disabled
  }
  // This path's own plugin and config, not the latest load's; an unrecorded path reloads through the dialog.
  const auto& sources = session_->sessionManager().loadedSources();
  const auto recorded = std::find_if(sources.begin(), sources.end(), [&path](const auto& src) {
    return FileLoader::sameSourceIdentity(src.path, path);
  });
  if (recorded != sources.end()) {
    return reloadSource(
        recorded->path, recorded->plugin_id, recorded->plugin_config_json, policy, std::move(still_allowed));
  }
  return reloadSource(path, QString(), QString(), policy, std::move(still_allowed));
}

void MainWindow::onReplaceDatasetRequested(DatasetId dataset_id) {
  file_loader_->replaceFromDialog(dataset_id, this);
}

void MainWindow::onDatasetInfoRequested(DatasetId dataset_id) {
  QString name;
  for (const auto& [id, dataset_name] : session_->catalogModel().datasets()) {
    if (id == dataset_id) {
      name = dataset_name;
      break;
    }
  }
  const SessionManager& manager = session_->sessionManager();
  auto* dialog = new DatasetInfoDialog(
      name, manager.datasetSourcePath(dataset_id), manager.sourceRecord(dataset_id),
      manager.datasetMetadata(dataset_id), this);
  dialog->show();
}

QSet<QString> MainWindow::captureHistoryDataUniverse() const {
  QSet<QString> universe;
  const QChar separator(QLatin1Char('\x1f'));
  const auto signature = [separator](const QStringList& fields) { return fields.join(separator); };

  // Processor outputs and on-demand series belong to workspace state and may be recreated by undo.
  // Only raw inputs constrain whether an older snapshot remains restorable.
  const std::unordered_set<TopicId> processor_outputs =
      session_->sessionManager().derivedScalarTopics(/*ephemeral_only=*/false);

  // Datastore identities, hidden catalog rows included: a processor can keep a raw dependency the curve list no
  // longer shows.
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
          // A column a reload lost leaves the universe as a lost topic does, so history plotting it is dropped.
          if (storage->isColumnRetired(column_descriptor.field_id)) {
            continue;
          }
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

  // ObjectStore's lock is never taken under DataEngine's. A marker or derived topic is a processor output too: an
  // undo recreates it.
  const ObjectStore& objects = session_manager.objectStore();
  for (const ObjectTopicId topic_id : objects.listTopics()) {
    const ObjectTopicDescriptor object_descriptor = objects.descriptor(topic_id);
    if (isOffClockTopic(object_descriptor)) {
      continue;
    }
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
  // `prefix` is always empty: the load dialog has no prefix input. A browser upload's MEMFS file dies with the
  // page, so it is never recorded as reloadable; its datasets still carry the identity within the session.
  if (!isBrowserUploadIdentity(path)) {
    session_->sessionManager().recordLoadedSource(path, prefix, plugin_id, plugin_config_json, plugin_manifest_id);
  }
  usageCounters().countPlugin(PluginUse::kFileLoad, plugin_manifest_id.toStdString());
  // Uploads, and on every platform a preset's staged downloads: what a
  // source-bound browser save describes them by.
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
#ifdef PJ_WITH_SCENE3D
  // For the docks' URDF package resolver; docks created later take lastLoadedSource() in makeSceneDock.
  forEachDock([&path](DockWidget* dock) {
    if (dock->objectWidget() == nullptr) {
      return;
    }
    if (auto* scene3d = qobject_cast<Scene3DDockWidget*>(dock->objectWidget()->widget())) {
      scene3d->setSourcePath(path);
    }
  });
#endif
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
  // A same-source reload evicts the old objects after removeDataset was signalled; a 2D viewer may still be bound
  // to one.
  syncWidgetsToCatalog();
  if (!progressive_layout_in_flight_) {
    reconcileHistoryWithDataUniverse();
  }
  // currentTimeChanged fires only on a change: a load landing the cursor where it was would leave the Value column
  // blank until the first scrub.
  broadcastTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
#ifdef PJ_TARGET_WASM
  updateBrowserLayoutActions();
#endif
}

void MainWindow::confirmAndRemoveDependentTransforms(
    const std::vector<TopicId>& removed_topics, std::function<void()> proceed, std::function<bool()> still_allowed,
    std::function<void()> cancelled, dialog_presenter::DialogWatch on_dialog) {
  auto& dp = session_->sessionManager().dataProcessorService();
  const std::vector<TopicId> outputs = dp.dependentProcessorOutputs(removed_topics);
  if (outputs.empty()) {
    proceed();  // nothing depends on it
    return;
  }
  const std::unordered_set<TopicId> dependent_outputs(outputs.begin(), outputs.end());

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
  MessageBox::questionAsync(
      this, tr("Delete derived series?"),
      tr("These derived series depend on what you are deleting and will also be removed:\n\n• %1")
          .arg(names.join(u"\n• "_s)),
      {{tr("Delete"), MessageBox::kDestructiveRole}, {tr("Cancel"), MessageBox::kCancelRole}},
      [this, removed_topics, dependent_curve_keys, proceed = std::move(proceed),
       still_allowed = std::move(still_allowed), cancelled = std::move(cancelled), on_dialog](int choice, bool) {
        if (on_dialog) {
          on_dialog({});
        }
        if (choice != 0 || (still_allowed && !still_allowed())) {
          if (cancelled) {
            cancelled();
          }
          return;
        }
        for (const QString& key : dependent_curve_keys) {
          ui_->curveListPanel->removeCustomCurve(key);
        }
        auto& processors = session_->sessionManager().dataProcessorService();
        if (const Status removed = processors.removeProcessorsDependingOn(removed_topics); !removed.has_value()) {
          emitDiagnostic(
              DiagnosticLevel::kError, "Processors", "dependent-remove-failed",
              QString::fromStdString(removed.error()));
          if (cancelled) {
            cancelled();
          }
          return;
        }
        session_->catalogModel().rebuildFromDatastore();
        proceed();
      },
      /*show_dont_show_again=*/false,
      [on_dialog](QDialog& prompt) {
        if (on_dialog) {
          on_dialog([prompt = QPointer<QDialog>(&prompt)] {
            if (prompt != nullptr) {
              prompt->reject();
            }
          });
        }
      });
}

void MainWindow::onCatalogTrashRequested(QStringList keys, bool covers_all) {
  CatalogModel& catalog = session_->catalogModel();
  if (covers_all) {
    if (streaming_manager_ != nullptr &&
        (streaming_manager_->hasActiveSession() || streaming_manager_->hasPendingStart())) {
      streaming_manager_->stopAllAndWait(tr("dataset removed"));
      active_streaming_dataset_id_ = 0;
      streaming_playback_seeded_ = false;
    }
    if (pending_binder_ != nullptr) {
      pending_binder_->clear();
    }
    clearPendingSceneRestores();
    // Same order as the "Remove all Datasets" wipe in the constructor.
    kept_derived_ = QDomDocument();
    session_->sessionManager().batchEngine().clearAllGenerators();
    session_->sessionManager().clearAllObjects();
#ifdef PJ_WITH_SCENE3D
    if (transform_service_ != nullptr) {
      transform_service_->invalidateAll();
    }
#endif
    // AppSession's cleared() hook also resets the transport to empty.
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
  std::vector<TopicId> removed_topics;
  for (const QString& key : keys) {
    if (const auto d = catalog.curveDescriptor(key)) {
      removed_topics.push_back(d->topic_id);
    }
  }
  confirmAndRemoveDependentTransforms(removed_topics, [this, keys]() {
    CatalogModel& model = session_->catalogModel();
    // Objects go before the catalog changes: itemsRemoved's revalidateObjects() checks the ObjectStore, not the
    // catalog.
    std::vector<ObjectTopicId> trashed_objects;
    for (const QString& key : keys) {
      if (const auto item = model.itemDescriptor(key); item.has_value()) {
        forEachSceneDock([&item](SceneDockWidget* scene_dock) {
          scene_dock->discardPendingRestoresForTopic(item->dataset_id, item->topic_name);
        });
        forEachLogPanel([&item](LogDockWidget* log) { log->discardTopic(item->dataset_id, item->topic_name); });
        if (const ObjectTopicPayload* obj = asObjectTopic(*item)) {
          trashed_objects.push_back(obj->object_topic_id);
        }
      }
    }
    session_->sessionManager().evictObjectTopics(trashed_objects);
    model.removeItems(std::vector<QString>(keys.begin(), keys.end()));
    if (model.items().empty()) {
      kept_derived_ = QDomDocument();  // nothing left for a kept entry to come back to, as after Remove all
    }
    if (pending_binder_ != nullptr) {
      static_cast<void>(pending_binder_->flush({}));
    }
    // Not while streaming: the slider is scoped to the active stream, and a catalog-wide recompute would stomp it.
    if (active_streaming_dataset_id_ == 0) {
      session_->seedPlaybackFromSession();
    }
    resetUndoHistory();
#ifdef PJ_TARGET_WASM
    updateBrowserLayoutActions();
#endif
  });
}

void MainWindow::removeDatasetData(DatasetId dataset_id) {
  forEachSceneDock(
      [dataset_id](SceneDockWidget* scene_dock) { scene_dock->discardPendingRestoresForDataset(dataset_id); });
  forEachLogPanel([dataset_id](LogDockWidget* log) { log->discardDataset(dataset_id); });
  if (pending_binder_ != nullptr) {
    static_cast<void>(pending_binder_->flush({}));
  }
  // Any stream, not only the active one: its worker must stop writing before the storage is erased.
  if (streaming_manager_ != nullptr) {
    static_cast<void>(streaming_manager_->stopDatasetAndWait(dataset_id, tr("dataset removed")));
    // A stream that ended on its own had no session to stop, and may have left paused rows.
    streaming_manager_->removeSecondaryDataset(dataset_id);
  }
  if (dataset_id == active_streaming_dataset_id_) {
    active_streaming_dataset_id_ = 0;
    streaming_playback_seeded_ = false;
  }
  // No tombstone: a later load of the same source mints a new DatasetId instead of reattaching to an empty shell.
  // Order: TF first (invalidateDataset walks the object topics eviction removes), then objects (so itemsRemoved
  // prunes the scene layers), then the catalog (tearing down every curve adapter), and only then the engine:
  // DataEngine::removeDataset requires every reader invalidated first.
#ifdef PJ_WITH_SCENE3D
  if (transform_service_ != nullptr) {
    transform_service_->invalidateDataset(dataset_id);
  }
#endif
  session_->sessionManager().evictDatasetObjects(dataset_id);
  session_->catalogModel().removeDataset(dataset_id, /*tombstone=*/false);
  if (session_->sessionManager().removeDataset(dataset_id)) {
    session_->catalogModel().rebuildIfChanged();
  }
  // loadedSources() stays whole for quick reload; appendDataSourceElement's liveness filter keeps the removed
  // source out of a saved layout.
  file_loader_->untrackDataset(dataset_id);
#ifdef PJ_TARGET_WASM
  updateBrowserLayoutActions();
#endif
}

void MainWindow::onRemoveDatasetsRequested(const QList<DatasetId>& dataset_ids) {
  if (dataset_ids.isEmpty()) {
    return;
  }
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
  MessageBox::questionAsync(
      this, dataset_ids.size() == 1 ? tr("Remove dataset") : tr("Remove datasets"), message,
      {{tr("Remove"), MessageBox::kDestructiveRole}, {tr("Cancel"), MessageBox::kCancelRole}},
      [this, dataset_ids](int choice, bool) {
        if (choice != 0) {
          return;  // Cancel / Esc
        }
        // A derived series' output usually lives in another dataset, so removeDataset alone would orphan it.
        // Cancelling the cascade prompt aborts the whole removal.
        std::vector<TopicId> removed_topics;
        for (const DatasetId dataset_id : dataset_ids) {
          const std::vector<TopicId> dataset_topics = session_->sessionManager().datasetTopicIds(dataset_id);
          removed_topics.insert(removed_topics.end(), dataset_topics.begin(), dataset_topics.end());
        }
        confirmAndRemoveDependentTransforms(removed_topics, [this, dataset_ids]() { removeDatasets(dataset_ids); });
      });
}

void MainWindow::removeDatasets(const QList<DatasetId>& dataset_ids) {
  for (const DatasetId id : dataset_ids) {
    removeDatasetData(id);
  }
  // A live stream keeps its tip-scoped slider and keeps following live.
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
  confirmAndMergeDatasets(this, *session_, ids, [this](DatasetId anchor) {
    if (source_timeline_controller_ != nullptr) {
      source_timeline_controller_->markDatasetMerged(anchor);
    }
#ifdef PJ_TARGET_WASM
    // A merge result cannot be recreated by replaying one browser file.
    if (browser_layout_runtime_ != nullptr) {
      browser_layout_runtime_->sources.remove(file_loader_->sourcePathForDataset(anchor));
    }
    updateBrowserLayoutActions();
#endif
  });
}

void MainWindow::onShowPreferencesDialog() {
#ifdef PJ_TARGET_WASM
  // No nested event loop in the Asyncify-free browser build.
  auto* dlg = new PreferencesDialog(*theme_, this);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setChromeMetrics(chrome_metrics_);
  bindRecordingPreferences(*dlg);
  bindPlottingPreferences(*dlg);
  bindScenePreferences(*dlg);
  dlg->open();
#else
  PreferencesDialog dlg(*theme_, this);
  dlg.setChromeMetrics(chrome_metrics_);
  bindRecordingPreferences(dlg);
  bindPlottingPreferences(dlg);
  bindScenePreferences(dlg);
  dlg.exec();
#endif
}

void MainWindow::bindRecordingPreferences(PreferencesDialog& dialog) {
  connect(&dialog, &PreferencesDialog::recordingSettingsChanged, this, [this]() {
    recording_service_->setSettings(RecordingService::loadSettings());
#ifndef __EMSCRIPTEN__
    // The capture toggle gates arming only, so it applies now; folder and budget wait for the next launch.
    if (source_capture_service_ != nullptr) {
      source_capture_service_->setCaptureEnabled(SourceCacheStore::loadSettings().capture_enabled);
    }
#endif
  });
}

void MainWindow::applyDerivedBatchPreferences() {
  session_->sessionManager().batchEngine().setSettings(BatchEngine::loadSettings());
}

namespace {

// A quicker run never flickers a "computing" note.
constexpr std::chrono::milliseconds kMarkerStatusShowDelay{300};

// The rule behind a status entry, and the marker set it publishes.
struct RuleStatusEntry {
  MarkerRuleStatus status;
  MarkerTopicInfo info;
  bool all_datasets = false;
  DatasetId dataset_id = 0;
};

// A target names the rule's scope and key on the rule's dataset (any dataset for an all_datasets rule).
bool plotDrawsRule(const std::vector<MarkerTarget>& targets, const RuleStatusEntry& entry) {
  return std::ranges::any_of(targets, [&](const MarkerTarget& target) {
    return target.marker == entry.info && (entry.all_datasets || target.dataset == entry.dataset_id);
  });
}

}  // namespace

void MainWindow::wireMarkerPlotStatus() {
  BatchEngine* batch_engine = &session_->sessionManager().batchEngine();
  auto* delay = new QTimer(this);
  delay->setSingleShot(true);
  using Clock = std::chrono::steady_clock;
  using BindingId = std::pair<std::string, DatasetId>;
  auto running_since = std::make_shared<std::map<BindingId, Clock::time_point>>();
  // Reports the standing status to `only`, or to every plot when null.
  push_marker_status_ = [this, batch_engine, delay, running_since](PlotWidget* only) {
    const Clock::time_point now = Clock::now();
    std::map<BindingId, Clock::time_point> still_running;
    std::optional<Clock::time_point> next_due;
    std::vector<RuleStatusEntry> entries;  // only the rules a plot would report
    for (const BatchEngine::GeneratorRecipe& recipe : batch_engine->recipes()) {
      bool computing = false;
      if (batch_engine->isBindingRunning(recipe)) {
        const BindingId binding{recipe.id, recipe.all_datasets ? BatchEngine::kAnyDataset : recipe.dataset_id};
        const auto known = running_since->find(binding);
        const Clock::time_point since = known != running_since->end() ? known->second : now;
        still_running.emplace(binding, since);
        computing = now - since >= kMarkerStatusShowDelay;
        if (!computing && (!next_due.has_value() || since + kMarkerStatusShowDelay < *next_due)) {
          next_due = since + kMarkerStatusShowDelay;
        }
      }
      QString error = QString::fromStdString(batch_engine->bindingError(recipe));
      if (recipe.outputs.empty() || (!computing && error.isEmpty())) {
        continue;
      }
      entries.push_back(
          RuleStatusEntry{
              .status =
                  {.rule_id = QString::fromStdString(recipe.id), .computing = computing, .error = std::move(error)},
              .info = markerInfoForKey(recipe.outputs.front()),
              .all_datasets = recipe.all_datasets,
              .dataset_id = recipe.dataset_id});
    }
    *running_since = std::move(still_running);
    if (next_due.has_value()) {
      delay->start(
          std::max(std::chrono::milliseconds(1), std::chrono::ceil<std::chrono::milliseconds>(*next_due - now)));
    } else {
      delay->stop();
    }
    const auto report = [&entries](PlotWidget* plot) {
      const std::vector<MarkerTarget> targets = plot->markerTargets();
      std::vector<MarkerRuleStatus> statuses;
      for (const RuleStatusEntry& entry : entries) {
        if (plotDrawsRule(targets, entry)) {
          statuses.push_back(entry.status);
        }
      }
      plot->setMarkerStatus(std::move(statuses));
    };
    if (only != nullptr) {
      report(only);
    } else {
      forEachPlot(report);
    }
  };
  connect(delay, &QTimer::timeout, this, [this] { push_marker_status_(nullptr); });
  connect(batch_engine, &BatchEngine::runningChanged, this, [this] { push_marker_status_(nullptr); });
}

void MainWindow::pushSeriesStatus(PlotWidget* only) {
  const OnDemandService& service = session_->sessionManager().onDemandService();
  std::vector<SeriesCurveStatus> statuses;
  for (const std::string& key : service.graph().keys()) {
    const std::optional<SeriesRunner::State> state = service.series().state(key);
    if (!state || state->complete) {
      continue;  // computed: a live append says nothing
    }
    std::optional<int> percent;
    if (state->replaying) {
      percent = state->total == 0 ? 0 : static_cast<int>(state->done * 100 / state->total);
    }
    statuses.push_back(
        SeriesCurveStatus{
            .dataset_id = service.graph().find(key)->dataset,
            .topic_name = QString::fromStdString(key),
            .computing_percent = percent});
  }
  if (only != nullptr) {
    only->setSeriesStatus(std::move(statuses));
  } else {
    forEachPlot([&statuses](PlotWidget* plot) { plot->setSeriesStatus(statuses); });
  }
}

void MainWindow::bindPlottingPreferences(PreferencesDialog& dialog) {
  connect(&dialog, &PreferencesDialog::derivedBatchSettingsChanged, this, &MainWindow::applyDerivedBatchPreferences);
  connect(&dialog, &PreferencesDialog::timeLabelSettingsChanged, this, &MainWindow::applyTimeLabelFormat);
  connect(&dialog, &PreferencesDialog::valuePrecisionChanged, this, [this]() {
    broadcastReferenceLine();
    broadcastTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
  });
}

void MainWindow::bindScenePreferences([[maybe_unused]] PreferencesDialog& dialog) {
#ifdef PJ_WITH_SCENE3D
  connect(&dialog, &PreferencesDialog::packageSearchFoldersChanged, this, [this]() {
    forEachSceneDock([](SceneDockWidget* scene) {
      if (auto* scene3d = qobject_cast<Scene3DDockWidget*>(scene)) {
        scene3d->reloadPackageSearchFolders();
      }
    });
  });
#endif
}

void MainWindow::onShowAboutDialog() {
#ifdef PJ_TARGET_WASM
  auto* dialog = new AboutDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setChromeMetrics(chrome_metrics_);
  dialog->open();
#else
  AboutDialog dialog(this);
  dialog.setChromeMetrics(chrome_metrics_);
  dialog.exec();
#endif
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
    // The toast's "Update now" link routes back here, not to a browser.
    QDesktopServices::setUrlHandler(u"pj-update"_s, this, "onUpdateLinkActivated");
  }
  update_checker_->setInstallation(installation_);

  // Rebound per call for its `interactive`; checkLatestRelease() aborts a check in flight, so no toast reaches the
  // wrong request.
  disconnect(update_available_conn_);
  disconnect(up_to_date_conn_);
  disconnect(check_failed_conn_);

  update_available_conn_ =
      connect(update_checker_, &UpdateChecker::updateAvailable, this, [this](const ReleaseInfo& release) {
        QString message =
            tr("New release available: <span style='font-weight:600;'>%1</span>").arg(release.name.toHtmlEscaped());
        switch (release.route) {
          case UpdateRoute::AptUpgrade:
            message += u"<br>"_s + tr("Update with <code>sudo apt update &amp;&amp; sudo apt upgrade</code>");
            break;
          case UpdateRoute::AptAddRepository:
            message += u"<br>"_s + tr("Get it, and future releases, through apt:<br>"
                                      "<code>curl -fsSL https://apt.plotjuggler.io/pj4_install.sh | sudo sh</code>");
            break;
          case UpdateRoute::MaintenanceTool:
            message += u"<br>"_s + tr("<a href=\"pj-update:maintenance-tool\">Update now</a> (PlotJuggler will close)");
            break;
          case UpdateRoute::Download:
            break;
        }
        if (!release.html_url.isEmpty()) {
          message += u"<br>"_s + (release.route == UpdateRoute::Download ? tr("<a href=\"%1\">View on GitHub</a>")
                                                                         : tr("<a href=\"%1\">Release notes</a>"))
                                     .arg(release.html_url);
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

void MainWindow::setInstallation(const QString& installation) {
  installation_ = installation;
}

void MainWindow::onUpdateLinkActivated(const QUrl& /*url*/) {
#if QT_CONFIG(process)
  if (update_checker_ && QProcess::startDetached(update_checker_->maintenanceToolPath(), {u"--start-updater"_s})) {
    close();
    return;
  }
#endif
  showToast(tr("Could not start the PlotJuggler maintenance tool."));
}

void MainWindow::checkExtensionUpdates() {
#ifndef PJ_TARGET_WASM
  if (update_scan_registry_ == nullptr) {
    update_scan_registry_ = new RegistryManager(this);
    // A failed fetch leaves the badge hidden, silently.
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
    trackRegistryFetches(update_scan_registry_);
  }
  update_scan_registry_->fetchRegistry(effectiveRegistryUrl());
#endif  // the browser installs no extensions
}

#ifndef PJ_TARGET_WASM
void MainWindow::noteRegistryScanSkipped() {
  feature_catalog_->setStartupScanSkipped();
}

void MainWindow::trackRegistryFetches(RegistryManager* manager) {
  connect(manager, &RegistryManager::fetchStarted, this, [this, manager]() {
    feature_catalog_->registryFetchStarted(manager->url());
  });
  connect(manager, &RegistryManager::fetchFinished, this, [this, manager](bool ok) {
    feature_catalog_->registryFetchFinished(manager->url(), ok, manager->extensions());
  });
}
#endif

void MainWindow::sendTelemetryPing(const QString& installation) {
  if (!telemetry_ping_) {
    telemetry_ping_ = new TelemetryPing(this);
  }
  telemetry_ping_->send(installation);
}

#ifndef PJ_TARGET_WASM
void MainWindow::startUsageReporting(const QString& installation, bool may_report) {
  UsageCounters& counters = usageCounters();
  counters.configure(
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + u"/usage_counters.json"_s,
      QCoreApplication::applicationVersion());
  if (!QSettings().value(kUsageStatsKey, true).toBool()) {
    counters.setEnabled(false);  // deletes a file a concurrent instance wrote back after the opt-out
    return;
  }
  if (!may_report || !UsageReporter::canUpload(installation)) {
    return;
  }
  counters.setEnabled(true);

  const auto take_inventory = [this]() {
    usageCounters().setInstalledPlugins(installedPluginInventory(session_->extensionCatalog()));
  };
  take_inventory();
  connect(&session_->extensionCatalog(), &ExtensionCatalogService::catalogChanged, this, take_inventory);

  // Counting never touches the disk; this bounds what a crash can lose.
  auto* flush_timer = new QTimer(this);
  connect(flush_timer, &QTimer::timeout, this, []() { usageCounters().flush(); });
  flush_timer->start(std::chrono::minutes(5));

  if (counters.completedDays().empty()) {
    return;  // nothing to upload this launch: skip the registry fetch too
  }
  auto* reporter = new UsageReporter(counters, this);
  // A developer's own collector, e.g. a local `wrangler dev` (docs/TELEMETRY.md, "Testing the usage report").
  if (const QString url = qEnvironmentVariable("PJ_USAGE_REPORT_URL"); !url.isEmpty()) {
    reporter->setEndpointUrl(QUrl(url));
  }
  // Its own fetch rather than the update scan's: that one is skipped when update checks are off, and
  // it follows the user's registry setting. Only the official registry defines "public": a company's own
  // registry lists exactly the in-house ids a report must never name.
  auto* registry = new RegistryManager(this);
  connect(registry, &RegistryManager::fetchFinished, this, [this, registry, reporter, installation](bool ok) {
    registry->deleteLater();
    if (!ok) {
      return;  // without the public list no plugin could be named: keep the days for the next launch
    }
    std::set<std::string> public_ids = session_->extensionCatalog().shippedPluginIds();
    for (const Extension& extension : registry->extensions()) {
      public_ids.insert(extension.id.toStdString());
    }
    reporter->send(installation, public_ids);
  });
  registry->fetchRegistry(QUrl(defaultRegistryUrl()));
}
#endif

void MainWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  if (toast_manager_) {
    toast_manager_->updatePosition();
  }
}

// Copy, never the proposed Move: a source may delete a file whose move was accepted.
void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
  if (event->mimeData() != nullptr && !FileLoader::droppedFilePaths(*event->mimeData()).isEmpty()) {
    event->setDropAction(Qt::CopyAction);
    event->accept();
  }
}

void MainWindow::dragMoveEvent(QDragMoveEvent* event) {
  if (event->mimeData() != nullptr && event->mimeData()->hasUrls()) {
    event->setDropAction(Qt::CopyAction);
    event->accept();
  }
}

void MainWindow::dropEvent(QDropEvent* event) {
  const QStringList paths = FileLoader::droppedFilePaths(*event->mimeData());
  if (paths.isEmpty()) {
    return;
  }
  event->setDropAction(Qt::CopyAction);
  event->accept();
#if defined(PJ_WITH_SCENE3D) && !defined(PJ_TARGET_WASM)
  constexpr bool kDropsRobotModels = true;
#else
  constexpr bool kDropsRobotModels = false;
#endif
  QStringList data_paths;
  QStringList urdf_paths;
  for (const QString& path : paths) {
    const bool urdf = kDropsRobotModels && QFileInfo(path).suffix().compare("urdf"_L1, Qt::CaseInsensitive) == 0;
    (urdf ? urdf_paths : data_paths).append(path);
  }
  const QPoint pos = event->position().toPoint();
  // After the drop returns: the file manager waits inside the OS drag loop until then, and a load can open dialogs.
  QTimer::singleShot(0, this, [this, data_paths, urdf_paths, pos] {
    if (!urdf_paths.isEmpty()) {
      addDroppedRobotModels(urdf_paths, pos);
    }
    if (!data_paths.isEmpty()) {
      file_loader_->loadFiles(data_paths, this);
    }
  });
}

void MainWindow::addDroppedRobotModels([[maybe_unused]] const QStringList& urdf_paths, [[maybe_unused]] QPoint pos) {
#ifdef PJ_WITH_SCENE3D
  Scene3DDockWidget* target = nullptr;
  for (QWidget* widget = childAt(pos); widget != nullptr && target == nullptr; widget = widget->parentWidget()) {
    target = qobject_cast<Scene3DDockWidget*>(widget);
  }
  if (target == nullptr) {
    std::vector<Scene3DDockWidget*> views;
    forEachSceneDock([&views](SceneDockWidget* scene) {
      if (auto* scene3d = qobject_cast<Scene3DDockWidget*>(scene)) {
        views.push_back(scene3d);
      }
    });
    if (views.size() != 1) {
      showToast(tr("Drop the URDF file onto a 3D view."));
      return;
    }
    target = views.front();
  }
  for (const QString& path : urdf_paths) {
    target->addRobotModelLayer(path);
  }
#endif
}

void MainWindow::paintEvent(QPaintEvent* event) {
  QMainWindow::paintEvent(event);
  if (!custom_frame_ || isMaximized() || isFullScreen()) {
    return;  // The system frame outlines itself; a maximized window has no edge.
  }

  const theme::Theme t = theme::appTheme();
  const int border_width = PJ::theme::stroke(theme::Stroke::Hairline);

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  QPen border_pen(PJ::theme::outline(theme::OutlineRole::Default, theme::OutlineState::Rest, t));
  border_pen.setWidthF(border_width);
  painter.setPen(border_pen);
  const qreal half_border = border_width / 2.0;
  painter.drawRect(QRectF(rect()).adjusted(half_border, half_border, -half_border, -half_border));
}

void MainWindow::onThemeChanged(const QString& theme) {
  // Queued (see the constructor): run synchronously inside Theme::setTheme, emitted from a PreferencesDialog
  // widget event, this work crashed. The QSS and tooltip palette are not re-applied here: the qssChanged handler
  // already did, and running last would overwrite what a later qssChanged listener (DebugUi) layered on top.
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
  // One QSettings: one INI rewrite for all four keys.
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
    // Scoped: flushed before effectiveRegistryUrl() reads it back.
    QSettings settings;
    if (url.isEmpty()) {
      settings.remove(QLatin1String(kRegistryUrlSettingsKey));
    } else {
      settings.setValue(QLatin1String(kRegistryUrlSettingsKey), url);
    }
  }
#ifndef PJ_TARGET_WASM
  // An open marketplace captured the URL when it was built. This is the key's only writer.
  if (!marketplace_panel_.isNull()) {
    marketplace_panel_->setRegistryUrl(effectiveRegistryUrl());
  }
#endif
}

bool MainWindow::customFrameSelectable() {
#if defined(PJ_TARGET_WASM) || defined(Q_OS_MACOS)
  return false;
#else
  return true;
#endif
}

bool MainWindow::usesCustomFrame() {
#ifdef PJ_TARGET_WASM
  return true;  // a browser tab has no window manager to supply a frame
#else
  return customFrameSelectable() && QSettings().value(kCustomTitleBarKey, false).toBool();
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
  // The build*Toolbar() buttons re-tint through their own stylesheetChanged hooks.
  for (const PanelToggle& toggle : panelToggles(ui_)) {
    // "iconPath" holds the variant shown; empty before the first toggle.
    const QString icon_path = toggle.button->property("iconPath").toString();
    const QString resolved = icon_path.isEmpty() ? QString::fromLatin1(toggle.icon_path_on) : icon_path;
    toggle.button->setIcon(loadSvg(resolved, theme));
  }
  ui_->actionExit->setIcon(QIcon(loadSvg(":/resources/svg/logout.svg", theme)));
  if (action_open_data_ != nullptr) {
    action_open_data_->setIcon(QIcon(loadSvg(":/resources/svg/upload_file.svg", theme)));
  }
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

void MainWindow::onSettingsPanelToggleRequested(DockWidget* dock) {
  onDockFocused(dock);
  ui_->tabbedPlotWidget->rightPanelButton()->click();
}

void MainWindow::onPlotTabAdded(PlotDocker* docker) {
  if (docker == nullptr) {
    return;
  }
  docker->setConfigurationPanelVisibilityQuery([this]() { return !ui_->localToolbarWidget->isHidden(); });
  connect(docker, &PlotDocker::plotWidgetAdded, this, &MainWindow::onPlotAdded, Qt::UniqueConnection);
  // ADS focus decides the active widget; PlotDocker re-emits it without ADS types.
  connect(docker, &PlotDocker::dockFocused, this, &MainWindow::onDockFocused, Qt::UniqueConnection);
  connect(
      docker, &PlotDocker::settingsPanelToggleRequested, this, &MainWindow::onSettingsPanelToggleRequested,
      Qt::UniqueConnection);
  connect(docker, &PlotDocker::objectFamilyRequested, this, &MainWindow::onObjectFamilyRequested, Qt::UniqueConnection);
  connect(docker, &PlotDocker::firstObjectTopicAdded, this, &MainWindow::onFirstObjectTopicAdded, Qt::UniqueConnection);
  connect(
      docker, &PlotDocker::placeholderTopicDropped, this, &MainWindow::onPlaceholderTopicDropped, Qt::UniqueConnection);
  connect(docker, &PlotDocker::topicRefused, this, &MainWindow::onTopicRefused, Qt::UniqueConnection);
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
  // Like an object drop into a scene dock (PlotDocker::firstObjectTopicAdded). A no-op unless a stream is active,
  // and once per stream; without it a scalar-only stream never seeds playback and the slider keeps its placeholder
  // range.
  connect(plot, &PlotWidget::curvesDropped, this, &MainWindow::seedStreamingPlaybackFromDrop, Qt::UniqueConnection);
  connect(plot, &PlotWidget::curvesDropped, this, &MainWindow::onPlotCurvesDropped, Qt::UniqueConnection);
  connect(plot, &PlotWidget::filterEditorRequested, this, &MainWindow::openFilterEditor, Qt::UniqueConnection);
  connect(
      plot, &PlotWidget::pendingCurveIntentsChanged, this, &MainWindow::schedulePendingDisplayBindingRebuild,
      Qt::UniqueConnection);
  connect(plot, &PlotWidget::statusMessageRequested, this, &MainWindow::onPlotStatusMessage, Qt::UniqueConnection);
  // Idempotent: layout load and undo re-run onPlotAdded for live plots.
  topic_demand_controller_->registerPlot(plot);
  plot->setTrackerPosition(toAxisDouble(session_->playbackEngine().currentTime()));
  applyGlobalToggles(plot);
  connect(plot, &PlotWidget::markerTargetsChanged, this, &MainWindow::onPlotMarkerTargetsChanged, Qt::UniqueConnection);
  reportMarkerStatus(plot);
  pushSeriesStatus(plot);
  if (curve_editor_ != nullptr && curve_editor_->plot() == nullptr) {
    bindEditorToPlot(plot);
  }
}

void MainWindow::onPlotStatusMessage(const QString& message) {
  emitDiagnostic(DiagnosticLevel::kInfo, "Plot", "status", message);
}

void MainWindow::onTopicRefused(const QString& reason) {
  showToast(reason);
}

void MainWindow::onPlotMarkerTargetsChanged() {
  reportMarkerStatus(qobject_cast<PlotWidget*>(sender()));
}

void MainWindow::reportMarkerStatus(PlotWidget* plot) {
  if (push_marker_status_ && plot != nullptr) {
    push_marker_status_(plot);
  }
}

void MainWindow::openFilterEditor(std::vector<CurveDescriptor> sources, PlotWidget* origin) {
  if (origin == nullptr || session_ == nullptr) {
    return;
  }
  // The filtered output inherits its source's colour; the preview shows the same.
  QHash<QString, QColor> source_colors;
  for (const auto& [key, color] : origin->curveColors()) {
    source_colors.insert(key, color);
  }

  auto* panel = new FilterEditorPanel(
      &session_->sessionManager(), &session_->catalogModel(), std::move(sources), std::move(source_colors));

  // Replaces each source curve in place on the originating plot, whose dock may close while the panel is open.
  const QPointer<PlotWidget> origin_guard(origin);
  connect(
      panel, &FilterEditorPanel::applied, this,
      [this, origin_guard](const QList<QPair<QString, QString>>& replacements) {
        if (origin_guard != nullptr) {
          for (const auto& replacement : replacements) {
            origin_guard->replaceCurve(replacement.first, replacement.second);
          }
          origin_guard->replot();
          // Its own undo step, never coalesced into a preceding edit.
          onUndoableChange(/*force_new_state=*/true);
        }
        restoreCentralArea();
      });
  connect(panel, &FilterEditorPanel::closed, this, [this]() { restoreCentralArea(); });
  connect(panel, &FilterEditorPanel::diagnostic, this, [this](const QString& message) {
    emitDiagnostic(DiagnosticLevel::kWarning, "Filter", "apply", message);
  });

  presentPanel(panel);
  filter_editor_origin_ = origin;  // the preview mirrors this plot's grid/style/width
  syncFilterEditorPreviewDisplay();
}

void MainWindow::syncFilterEditorPreviewDisplay() {
  auto* panel = qobject_cast<FilterEditorPanel*>(current_panel_);
  if (panel == nullptr || filter_editor_origin_ == nullptr) {
    return;
  }
  // The grid is global, so activate_grid_ is the origin's.
  panel->setPreviewDisplay(
      activate_grid_, filter_editor_origin_->defaultCurveStyle(), filter_editor_origin_->lineWidth());
}

void MainWindow::syncPanelPreviewDisplay() {
  // A toolbox panel's preview PlotWidget (in the presented panel and in every pinned tab) has no plot to mirror;
  // only the global grid is pushed.
  const auto apply_to_panel = [this](QWidget* panel_root) {
    for (auto* plot : panel_root->findChildren<PlotWidget*>()) {
      // Read by the dialog-host binding, which re-applies it whenever the preview's curves are rebuilt.
      if (QWidget* frame = plot->parentWidget()) {
        frame->setProperty("_pj_view_set", true);
        frame->setProperty("_pj_view_grid", activate_grid_);
      }
      plot->setGridVisible(activate_grid_);
      plot->replot();
    }
  };
  if (current_panel_ != nullptr) {
    apply_to_panel(current_panel_);
  }
  for (const auto& [plugin_id, toolbox] : toolboxes_) {
    if (QWidget* container = toolbox->container(); container != nullptr && container != current_panel_) {
      apply_to_panel(container);
    }
  }
}

namespace {
// Callers pass the saved corner (previous_legend_corner_) for a hidden legend.
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

// TR -> TL -> BL -> BR. The click handler owns the wrap to hidden and back.
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
  // The hidden legend's icon and the next show use the last corner.
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
  // BR hides and resets the saved corner to TR, so the hidden button shows TR and the next click re-enters there;
  // a right-click hide keeps the last corner instead.
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
  if (!applying_state_) {
    applyDots(plot);
  }
  plot->setReferenceLine(referenceDisplaySeconds());
  plot->setKeepRatioXY(keep_ratio_);
  applyLegendStatus(plot);
  plot->setTrackerParameter(tracker_info_);
  plot->setTimeLabelFormat(time_label_format_);
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
  // setIconPath, not setIcon: the SvgButton re-tints it on theme switches.
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
  // Flips only Lines <-> Lines+Dots; another style chosen in the Curve Style panel stays.
  const auto from = dots_ ? PlotWidgetBase::kLines : PlotWidgetBase::kLinesAndDots;
  const auto to = dots_ ? PlotWidgetBase::kLinesAndDots : PlotWidgetBase::kLines;
  if (plot->defaultCurveStyle() == from) {
    plot->setDefaultStyle(to);
  }
}

void MainWindow::onPlotCurvesDropped(bool was_empty) {
  auto* plot = qobject_cast<PlotWidget*>(sender());
  if (!was_empty || plot == nullptr || !button_link_->isChecked() || plot->isEmpty() || plot->isXYPlot()) {
    return;
  }
  std::optional<QRectF> linked_range;
  forEachPlot([&](PlotWidget* peer) {
    if (!linked_range && peer != plot && !peer->isEmpty() && !peer->isXYPlot() && peer->isZoomLinkEnabled()) {
      // A peer still waiting for its first rows has no window to share.
      if (const QRectF rect = peer->currentBoundingRect(); rect.left() < rect.right()) {
        linked_range = rect;
      }
    }
  });
  if (linked_range) {
    plot->setVisibleXRange(linked_range->left(), linked_range->right());
  }
}

void MainWindow::onPlotZoomChanged(PlotWidget* modified, QRectF rect) {
  if (!button_link_->isChecked()) {
    return;
  }
  if (modified == nullptr || modified->isEmpty() || modified->isXYPlot() || !modified->isZoomLinkEnabled()) {
    return;
  }
  // A plot without rows has no X window to share; this also refuses a gesture's NaN one.
  if (!(rect.left() < rect.right())) {
    return;
  }

  forEachPlot([modified, rect](PlotWidget* plot) {
    if (plot == modified || plot->isEmpty() || plot->isXYPlot() || !plot->isZoomLinkEnabled()) {
      return;
    }
    // X only: each peer keeps its vertical range.
    plot->setVisibleXRange(rect.left(), rect.right());
  });
  // setVisibleRange never echoes back, so no feedback loop.
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
    plot->setVisibleXRange(t_min, t_max);
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
  // The same projection as a curve adapter's, so the line never drifts off its data.
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

void MainWindow::applyTimeLabelFormat() {
  const QSettings settings;
  time_label_format_ =
      clockFormatFor(settings.value(kDateTimeDisplayKey, false).toBool(), settings.value(kClockUtcKey, false).toBool());
  forEachPlot([this](PlotWidget* plot) { plot->setTimeLabelFormat(time_label_format_); });
  forEachLogPanel([this](LogDockWidget* log) { log->setTimeLabelFormat(time_label_format_); });
  ui_->timelineWidget->setTimeLabelFormat(time_label_format_);
  if (source_timeline_ != nullptr) {
    source_timeline_->setAbsoluteClockFormat(time_label_format_);
  }
}

void MainWindow::onUseTimeOffsetToggled(bool checked) {
  // The global displayOffsetChanged handler translates the range and playhead.
  session_->sessionManager().setUseTimeOffset(checked);
}

void MainWindow::onFirstObjectTopicAdded(DockWidget* dock) {
  seedStreamingPlaybackFromDrop();
  // A layout restore never gets here: it repopulates layers without a drop.
  frameDroppedScene3dContent(dock != nullptr ? dock->objectWidget() : nullptr);
}

void MainWindow::seedStreamingPlaybackFromDrop() {
  if (active_streaming_dataset_id_ == 0) {
    return;
  }
  // Once per stream: a later drop must not re-snap a paused scrub.
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

void MainWindow::refreshStreamingSeekLock() {
  if (session_ == nullptr) {
    return;
  }
  // Live and playing: the cursor is held at the tip (PlaybackEngine::onTick and each live ingest). Pausing unlocks
  // scrubbing.
  const bool lock = active_streaming_dataset_id_ != 0 && session_->playbackEngine().isPlaying();
  ui_->timelineWidget->setSeekLocked(lock);
  setSourceTimelineStreamingLock(lock);
}

void MainWindow::setSourceTimelineStreamingLock(bool locked) {
  if (source_timeline_ != nullptr) {
    source_timeline_->setInteractionLocked(locked);
  }
  // The rail calls the controller directly, past the widget lock.
  if (ui_->timelineAlignRail != nullptr) {
    ui_->timelineAlignRail->setEnabled(!locked);
  }
}

DiagnosticSink MainWindow::diagnosticSink() const {
  return diagnostic_bridge_->sink();
}

void MainWindow::emitDiagnostic(DiagnosticLevel level, const char* source, const char* id, const QString& message) {
  if (layout_issue_capture_ != nullptr && source != nullptr && qstrcmp(source, "Layout") == 0) {
    layout_issue_capture_->push_back(message);
  }
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
  // pj.recording metadata; main.cpp seeds it from pj_version.h, generated from versions.env.
  const auto directory =
      recording_service_->start(std::move(recordable.targets), QCoreApplication::applicationVersion().toStdString());
  if (!directory) {
    ui_->leftPanel->setRecordingActive(false);
    showToast(tr("Recording could not start: %1").arg(QString::fromStdString(directory.error())));
    return;
  }
  if (!recordable.excluded.isEmpty()) {
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
  // Per-source detail goes to the log: a toast line per file is unreadable past a couple of sources.
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
    // At most two names; the log has the rest.
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
    // A complete file does not record its own holes.
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
  // The budget sheds the biggest messages while the sink lags; the recording continues with holes.
  if (dropped > 0) {
    text += tr(" | %1 dropped").arg(dropped);
  }
  ui_->leftPanel->setRecordingStatusText(text);
}

void MainWindow::wireExistingPlots() {
  ui_->tabbedPlotWidget->forEachDocker([this](PlotDocker* docker) { onPlotTabAdded(docker); });
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

void MainWindow::forEachCustomWidgetDock(const std::function<void(CustomWidgetDockWidget*)>& operation) {
  forEachDock([&operation](DockWidget* dock) {
    if (dock->objectWidget() == nullptr) {
      return;
    }
    if (auto* custom = qobject_cast<CustomWidgetDockWidget*>(dock->objectWidget()->widget())) {
      operation(custom);
    }
  });
}

void MainWindow::forEachLogPanel(const std::function<void(LogDockWidget*)>& operation) {
  forEachDock([&operation](DockWidget* dock) {
    if (dock->objectWidget() == nullptr) {
      return;
    }
    if (auto* log = qobject_cast<LogDockWidget*>(dock->objectWidget()->widget())) {
      operation(log);
    }
  });
}

void MainWindow::refreshCustomWidgetsAvailability() {
  const auto& widgets = session_->extensionCatalog().widgets();
  ui_->tabbedPlotWidget->setCustomWidgetSupport({
      .available = std::ranges::any_of(widgets, [](const LoadedWidget& library) { return !library.widgets.empty(); }),
      .shows_only = &isCustomWidgetObjectType,
  });
}

void MainWindow::forEachDock(const std::function<void(DockWidget*)>& operation) {
  ui_->tabbedPlotWidget->forEachDocker([&operation](PlotDocker* docker) {
    for (int index = 0; index < docker->plotCount(); ++index) {
      DockWidget* dock = docker->plotAt(index);
      if (dock != nullptr) {
        operation(dock);
      }
    }
  });
}

void MainWindow::forEachVisibleDock(const std::function<void(DockWidget*)>& operation) {
  // Per tab: a dock hidden by a maximized sibling is still visited. plotAt() returns each area's current dock, so a
  // dock tabbed behind another in one area would be skipped and never re-seeded; the docking allows only
  // OuterDockAreas, so no inner tabs exist. Enabling them needs a re-seed on CDockAreaWidget::currentChanged.
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
  tickEmbeddedViews(display_seconds, /*visible_only=*/false);
  ui_->curveListPanel->refreshValues(display_seconds);
}

void MainWindow::broadcastTrackerTimeToVisible(double display_seconds) {
  // Per-tick fan-out: a hidden tab's replot would still do the full canvas work. Structural seeds use
  // broadcastTrackerTime() (all tabs), and currentTabChanged re-seeds the tab that becomes active.
  forEachVisibleDock([display_seconds](DockWidget* dock) { dock->onTrackerTime(display_seconds); });
  tickEmbeddedViews(display_seconds, /*visible_only=*/true);
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
  // An emptied object viewer becomes a placeholder, except in a plugin's own tab: the plugin chose its kind.
  forEachPlot([](PlotWidget* plot) { plot->revalidate(); });
  // An embedded view keeps its emptied widget too. The binding never re-applies an unchanged request, so the shell
  // re-attaches what it still asks for, and forgets an evicted topic's id (clear() reuses ids).
  for (const QPointer<SceneDockWidget>& view : embedded_scene_views_) {
    if (view != nullptr) {
      (void)view->revalidateObjects();
      reattachEmbeddedTopics(*view);
    }
  }
  ui_->tabbedPlotWidget->forEachDocker([](PlotDocker* docker) {
    const bool plugin_owned = !docker->ownerPlugin().isEmpty();
    for (int index = 0; index < docker->plotCount(); ++index) {
      DockWidget* dock = docker->plotAt(index);
      auto* viewer = dock == nullptr ? nullptr : dynamic_cast<IObjectViewer*>(dock->objectWidget());
      if (viewer != nullptr && !viewer->revalidateObjects() && !plugin_owned) {
        dock->clearToPlaceholder();
      }
    }
  });
  // Clearing to a placeholder changes no ADS focus. The active tab's dock, not a remembered pointer, so a hidden
  // tab's dock is never shown.
  onDockFocused(activeFocusedDock());
}

std::optional<double> MainWindow::displayTimeForSource(PJ_data_source_handle_t source, int64_t absolute_ns) const {
  const auto seconds = session_->sessionManager().displayTimeForSource(source.id, absolute_ns);
  return seconds.has_value() ? std::optional<double>(toAxisDouble(*seconds)) : std::nullopt;
}

std::optional<double> MainWindow::displayTimeForTopic(std::string_view topic, int64_t absolute_ns) const {
  const DatasetId dataset_id = topic.empty()
                                   ? representativeDatasetId()
                                   : session_->catalogModel().datasetForTopic(
                                         QString::fromUtf8(topic.data(), static_cast<qsizetype>(topic.size())));
  return displayTimeForSource({static_cast<uint32_t>(dataset_id)}, absolute_ns);
}

void MainWindow::linkedZoomOut() {
  if (!button_link_->isChecked()) {
    forEachPlot([](PlotWidget* plot) { plot->zoomOut(false); });
    return;
  }
  ui_->tabbedPlotWidget->forEachDocker([](PlotDocker* docker) {
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

    // The union over plots and strips is complete before any widget moves; a strip-only tab zooms to its content.
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
      // A plot still waiting for rows would fold its [0, 0] into the union.
      if (PlotWidget* plot = plot_at(index); plot != nullptr && !plot->isXYPlot() && plot->hasUsableCurveData()) {
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
          plot->zoomOut(false);  // keeps a plot waiting for rows armed
          plot->setVisibleXRange(x_union->min, x_union->max);
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
  // First: the import batch's provider jobs are cancelled and joined and its tickets discarded, so no provider
  // worker calls into the loader or session while they tear down.
  supersedeActiveRestore();
  // Before the session tears down: a worker would write into a freed engine or signal a half-destroyed window.
  if (file_loader_ != nullptr) {
    file_loader_->joinForShutdown();
  }
  // ~MainWindow joins these workers; stopping now lets the downloads wind down instead of the join waiting for a
  // full fetch.
  stopAllToolboxImports();
  // Before the streaming sources go: stop() detaches the taps from live hosts and writes the summary section.
  if (recording_service_ != nullptr) {
    recording_service_->stop();
  }
#ifdef PJ_TARGET_WASM
  // Emscripten may keep the runtime alive after the last window closes instead of unwinding main(), so the
  // destructor cannot be relied on to stop worker threads.
  if (streaming_manager_ != nullptr &&
      (streaming_manager_->hasActiveSession() || streaming_manager_->hasPendingStart())) {
    streaming_manager_->stopAllAndWait(tr("application shutdown"));
    active_streaming_dataset_id_ = 0;
    streaming_playback_seeded_ = false;
  }
#endif
  QSettings settings;
  settings.setValue(u"MainWindow.buttonLink"_s, button_link_->isChecked());
  settings.setValue(u"MainWindow.mainSplitterState"_s, ui_->mainSplitter->saveState());
#ifndef PJ_TARGET_WASM
  // Restored by main() before the first show. A Wayland compositor picks the screen itself, so there only the size
  // and maximized state come back.
  settings.setValue(u"MainWindow.geometry"_s, saveGeometry());
#endif
  QMainWindow::closeEvent(event);
}

void MainWindow::showEvent(QShowEvent* event) {
  QMainWindow::showEvent(event);

#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  if (!property("pjWasmReadyReported").toBool()) {
    setProperty("pjWasmReadyReported", true);
    QTimer::singleShot(0, this, []() { qInfo("PJ_WASM_APP_READY"); });
  }
#endif

  // Here, not in the constructor, so the first layout pass does not overwrite the saved sizes; a --layout load runs
  // later and wins (restoreChromeState).
  if (!left_splitter_restored_) {
    left_splitter_restored_ = true;
    const QByteArray state = QSettings().value(u"MainWindow.mainSplitterState"_s).toByteArray();
    if (state.isEmpty() || !ui_->mainSplitter->restoreState(state)) {
      // Otherwise the splitter collapses the panel toward its minimum width.
      constexpr int kDefaultLeftPanelWidth = 400;
      const int total = ui_->mainSplitter->width();
      if (total > kDefaultLeftPanelWidth) {
        ui_->mainSplitter->setSizes({kDefaultLeftPanelWidth, total - kDefaultLeftPanelWidth});
      }
    }
  }

  // After the first layout pass: only then does an absolute setSizes() stick (earlier it leaves a ratio that
  // resolves to a sliver).
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
  // The working directory on first run: layout files commonly live in project directories.
  const QString start_dir = QSettings().value(kLastLayoutDirKey, QDir::currentPath()).toString();
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
  // setDefaultSuffix takes no leading dot.
  const QString default_suffix = QString::fromLatin1(layout_xml::kLayoutExtension).mid(1);

  // Checked: source-bound, the layout reloads this exact file. Unchecked: generic, curves bind by topic+field to
  // whatever is loaded. A bound layout stores each live stream's configuration verbatim, credentials included.
  const bool streaming = streaming_manager_ != nullptr && streaming_manager_->hasActiveSession();
  const FileDialog::ExtraOption save_source_opt{
      streaming ? tr("Bind to this data source (stores the stream settings, credentials included)")
                : tr("Bind to this data source"),
      /*default_checked=*/true};
  const auto result = FileDialog::getSaveFileNameWithOptions(
      this, tr("Save Layout"), start_dir, tr(kLayoutFilter), default_suffix, {save_source_opt}, this);

  if (result.path.isEmpty()) {
    return;
  }
  // A platform dialog may skip defaultSuffix.
  const QString save_path = layout_xml::ensureLayoutExtension(result.path);
  QSettings().setValue(kLastLayoutDirKey, QFileInfo(save_path).absolutePath());
  const bool include_data_source = !result.option_states.empty() && result.option_states[0];
  usageCounters().count(UsageFeature::kLayoutSave);
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
      action->setEnabled(false);  // informational; the Marketplace manages them
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
  // Permille (kIngestProgressResolution); total 0 keeps the busy bar.
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
  // Interactive survivors outrank the batch job's ingest; among them the last-started one, else the lowest id.
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

  bool added_any = false;
  for (const auto& toolbox : session_->extensionCatalog().toolboxes()) {
    // A load failure, already reported.
    const auto* vtable = toolbox.library.vtable();
    if (vtable == nullptr || vtable->manifest_json == nullptr) {
      continue;
    }
    const auto manifest = nlohmann::json::parse(vtable->manifest_json, nullptr, /*allow_exceptions=*/false);
    // "cloud" toolboxes open from the Sources panel, "hidden" ones from elsewhere (the "+" in Custom Topics).
    if (manifestHasTag(manifest, kToolboxTagCloud) || manifestHasTag(manifest, kToolboxTagHidden)) {
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

void MainWindow::loadLayoutFromPath(
    const QString& path, LayoutLoadInteractivity interactivity, const std::optional<QStringList>& current_data_files,
    const QStringList& chosen_files) {
  snapshotStreamWindowAtLayoutOpen();
  const bool interactive = interactivity == LayoutLoadInteractivity::kInteractive;
  // Governs every failure surface of this load, entry failures included: an automated (--layout) run must never
  // open a modal.
  const MissingCurvePolicy restore_policy =
      interactive ? MissingCurvePolicy::kPrompt : MissingCurvePolicy::kRetainAndDiagnose;
  // Every terminal outcome (entry failures, user cancels, both restore legs' ends) emits layoutRestoreSettled
  // exactly once: --exit-after-layout waits for it.
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
  if (interactive) {
    usageCounters().count(UsageFeature::kLayoutLoad);  // the user's own load: an automated --layout run is not a use
  }
#ifdef PJ_WITH_SCENE3D
  // Robot resources may be named relative to the layout file: anchor them
  // before anything restores from the document.
  if (QString resource_error; !pj::scene3d::resolveLayoutResourceUrls(
          doc, QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()), &resource_error)) {
    reportLayoutRestoreIssue(
        restore_policy, "layout-resources-invalid", tr("'%1' cannot be loaded: %2").arg(path, resource_error));
    emit layoutRestoreSettled(false);
    return;
  }
#endif

  // A newer layout warns and loads best-effort, never fails: unknown elements are ignored downstream.
  const QDomElement root = doc.documentElement();
  bool version_ok = false;
  const int version = root.attribute(u"pj4_version"_s, u"0"_s).toInt(&version_ok);
  if (version_ok && version > layout_xml::kStreamingLayoutSchemaVersion) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "schema-newer",
        tr("Layout '%1' was saved by a newer PJ4 (pj4_version=%2 > %3); loading best-effort.")
            .arg(QFileInfo(path).fileName())
            .arg(version)
            .arg(layout_xml::kStreamingLayoutSchemaVersion));
  }
  const QDir layout_dir(QFileInfo(path).absoluteDir());
  // Qualifiers may be stored relative to the layout; every binder compares normalized absolute paths.
  layout_xml::resolveDatasetSourcePaths(doc, layout_dir);
  const QList<layout_xml::DataSourceRef> replays = layout_xml::extractDataSource(doc, layout_dir);
  // Before the rewrites below (explicit files, stream qualifiers): the dataset choice weighs what the file named.
  const QList<layout_xml::DatasetIdentity> saved_identities = layout_xml::datasetIdentities(doc);
  // `binding` records the save-time intent; current_data_files overrides it at load.
  if (current_data_files.has_value()) {
    doc.documentElement().setAttribute(u"binding"_s, u"generic"_s);
    QStringList source_order;
    for (const layout_xml::DataSourceRef& replay : replays) {
      source_order.push_back(replay.resolved_path);
    }
    const QList<layout_xml::DatasetFileMatch> matches =
        layout_xml::matchDatasetsToFiles(doc, *current_data_files, source_order);
    for (const layout_xml::DatasetFileMatch& match : matches) {
      using Rule = layout_xml::DatasetFileMatch::Rule;
      if (match.rule == Rule::kAlias) {
        continue;  // its dataset's own line covers it
      }
      const QString how = match.rule == Rule::kPath   ? tr("same file")
                          : match.rule == Rule::kName ? tr("by name")
                                                      : tr("positional");
      emitDiagnostic(
          DiagnosticLevel::kInfo, "Layout", "dataset-mapped",
          tr("Layout dataset \"%1\" %2 %3 (%4)").arg(match.label, QString(QChar(0x2192)), match.file, how));
    }
    layout_xml::removeDatasetQualifiersForGenericLayout(doc, matches);
  }
  const QString binding = root.attribute(u"binding"_s, u"source"_s);
  // A layout may carry a numeric dataset id with a basename-like source; the id means nothing outside the session
  // that minted it. Stripped on read as on write; the source alone binds only when unique.
  layout_xml::removeUnvalidatedDatasetIds(doc);
  // A <range> without x_basis gets it explicitly before any widget restore, so PlotWidget::xmlLoadState never infers
  // it from the plot mode. No value changes.
  layout_xml::normalizePlotRangeBasis(doc);
  // A restore in flight is superseded only at this load's commit points (the do_reload branch and
  // applyRestoredLayout): a Cancel at the reload or trust prompt must leave it draining. Classification and
  // prepare() touch only state and the new document, so they need not wait.
  // Set on "Reload": the loads may run on a worker, so the apply waits for the queue to drain.
  bool reload_requested = false;
  // "path: why" per source this load could not bring back, for the no-data dialog.
  QStringList source_failures;
  // A stream already running with the same plugin and configuration is bound as it is; the others start at the
  // commit point, from their saved configuration without their plugin's dialog.
  const bool source_bound = binding != "generic"_L1;
  LayoutStreamBinding streams;
  QList<layout_xml::StreamingSourceRef> pending_streams;
  QStringList stream_failures;
  if (source_bound && streaming_manager_ != nullptr) {
    QString stream_error;
    for (const layout_xml::StreamingSourceRef& record : layout_xml::extractStreamingSources(doc, &stream_error)) {
      const QSet<DatasetId> taken(streams.dataset_for_identity.cbegin(), streams.dataset_for_identity.cend());
      if (const DatasetId live = streaming_manager_->liveSessionFor(record, taken); live != 0) {
        streams.add(record, live);
      } else {
        pending_streams.push_back(record);
      }
    }
    if (!stream_error.isEmpty()) {
      stream_failures.push_back(tr("Streaming sources: %1").arg(stream_error));
    }
  }
  if (source_bound && (!replays.empty() || !pending_streams.empty())) {
    // Sources with a materialize record belong to the batch, which rewrites `doc` and classifies them against
    // their provider's effective paths; plain sources are classified below. The browser has no batch.
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
      for (const LayoutImportBatch::SourceResult& result : batch->result().sources) {
        if (result.outcome == LayoutImportBatch::SourceOutcome::kFailed) {
          source_failures.push_back(u"%1: %2"_s.arg(result.effective_path, result.message));
        }
      }
    }
#else
    const QList<layout_xml::DataSourceRef>& plain_refs = replays;
#endif
    // Already loaded counts only while the catalog has data: a remembered but cleared source must reload.
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
        source_failures.push_back(u"%1: %2"_s.arg(replay.resolved_path, tr("does not exist on disk")));
        continue;
      }
      pending.push_back(replay);
    }

    if (!pending.empty() || (batch != nullptr && batch->hasPendingWork()) || !pending_streams.empty()) {
      // --layout reloads and starts streams without asking; interactively one prompt covers the whole set.
      bool do_reload = !interactive;
      if (interactive) {
        QStringList source_lines;
        source_lines.reserve(pending.size() + pending_streams.size());
        for (const auto& replay : pending) {
          source_lines.push_back(u"  %1"_s.arg(replay.resolved_path));
        }
        if (batch != nullptr) {
          for (const QString& line : batch->pendingSourceLines()) {
            source_lines.push_back(u"  %1"_s.arg(line));
          }
        }
        const bool reloads_files = !source_lines.isEmpty();
        for (const layout_xml::StreamingSourceRef& stream : pending_streams) {
          const QString& plugin = stream.plugin_id.isEmpty() ? stream.plugin_manifest_id : stream.plugin_id;
          source_lines.push_back(
              stream.source_name.isEmpty() ? tr("  %1 (streaming)").arg(plugin)
                                           : tr("  %1 (streaming, %2)").arg(stream.source_name, plugin));
        }
        const QString reload_label = pending_streams.isEmpty() ? tr("Reload source file")
                                     : reloads_files           ? tr("Reload and start streaming")
                                                               : tr("Start streaming");
        // Button order is the returned index; Esc maps to kCancelRole.
        constexpr int kReloadOriginal = 0;
        constexpr int kCancel = 2;
        const int choice = MessageBox::question(
            this, tr("Load Layout"),
            tr("This layout was saved with %n data source(s):\n\n%1", nullptr, static_cast<int>(source_lines.size()))
                .arg(source_lines.join(QLatin1Char('\n'))),
            {{reload_label, MessageBox::kPrimaryRole},
             {tr("Load Layout only"), MessageBox::kNeutralRole},
             {tr("Cancel"), MessageBox::kCancelRole}});
        if (choice == kCancel || choice < 0) {
          emit layoutRestoreSettled(false);
          return;
        }
        do_reload = (choice == kReloadOriginal);
      }
      if (do_reload) {
        reload_requested = true;
        LayoutImportBatch::StartResult batch_result = LayoutImportBatch::StartResult::kNoAsyncWork;
        if (batch != nullptr) {
          // Trust gate, then the batch's stock loads and first import job; before the plain loads, so a Cancel at
          // the gate aborts with nothing enqueued.
          batch_result = batch->start();
          if (batch_result == LayoutImportBatch::StartResult::kCancelledByUser) {
            // start() enqueued and captured nothing (the batch tests pin it): the old restore keeps draining.
            emit layoutRestoreSettled(false);
            return;
          }
        }
        // Commit point: after every cancel return, before the batch handover and the plain enqueues. The batch's
        // enqueues above and this teardown share one synchronous stretch, so no stale waiter can observe them.
        supersedeActiveRestore();
        if (batch_result == LayoutImportBatch::StartResult::kRunning) {
          layout_import_batch_ = std::move(batch);
          // The observers exist exactly while a handed-over batch does.
          installLayoutBatchObservers();
        }
        // An automated load passes a null dialog parent, which suppresses the load-failure dialog and the plugin
        // message-box service: an unattended load must not open modals.
        for (const auto& replay : pending) {
          file_loader_->loadFile(
              replay.resolved_path, interactive ? this : nullptr, layoutReplayHints(replay, /*prefer_reuse=*/true));
        }
        stream_failures.append(startLayoutStreams(pending_streams, streams));
      }
      // "Load Layout only": a prepared batch dies unstarted.
    }
  }

  // Stream-bound items bind to their session's fresh DatasetIds and wait for topics arriving after the apply.
  layout_xml::bindStreamQualifiers(doc, streams.dataset_for_identity);
  source_failures.append(stream_failures);

  // Data still arriving (a worker load, or a batch import with FileLoader idle) restores progressively, binding
  // curves as topics arrive; otherwise the layout applies now. Both legs take restore_policy, never batch survival:
  // a mixed layout can lose its batch and still go progressive, and the sync leg is reachable unattended too.
  if (!source_failures.empty() && session_->catalogModel().datasets().empty() && streams.tracks.empty() &&
      !restoreHasPendingAsyncWork(reload_requested)) {
    reportLayoutRestoreIssue(
        restore_policy, "layout-apply-no-data",
        tr("No data is loaded, and the layout's data sources could not be restored:\n\n%1")
            .arg(source_failures.join(u"\n\n"_s)));
    emit layoutRestoreSettled(false);
    return;
  }
  if (!stream_failures.empty()) {
    reportLayoutRestoreIssue(
        restore_policy, "layout-streams-not-started",
        tr("Some of the layout's streaming sources could not be started:\n\n%1").arg(stream_failures.join(u"\n\n"_s)));
  }
  // Asked only now: the report above can be a modal box, and a reload that finishes
  // under it signals no waiter, so a restore begun after it would wait forever.
  if (restoreHasPendingAsyncWork(reload_requested)) {
    beginProgressiveLayoutRestore(doc, path, restore_policy, streams.tracks);
    return;
  }
  chooseLayoutDatasets(
      std::move(doc), saved_identities, interactivity != LayoutLoadInteractivity::kAutomated,
      QFileInfo(path).fileName(), chosen_files,
      [this, path, restore_policy, tracks = std::move(streams.tracks)](ChosenLayout chosen) {
        if (!chosen.removed.isEmpty()) {
          reportLayoutRestoreIssue(restore_policy, "layout-datasets-removed", chosen.removed);
        }
        if (!chosen.doc.has_value()) {
          emit layoutRestoreSettled(false);  // nothing was applied yet
          return;
        }
        applyRestoredLayout(*chosen.doc, path, restore_policy, tracks, chosen.compared);
      });
}

void MainWindow::chooseLayoutDatasets(
    QDomDocument doc, const std::optional<QList<layout_xml::DatasetIdentity>>& saved, bool can_prompt,
    const QString& layout_name, const QStringList& chosen_files, std::function<void(ChosenLayout)> then) {
  const CatalogModel& catalog = session_->catalogModel();
  layout_xml::LoadedDatasets loaded;
  QHash<DatasetId, qsizetype> index_of;
  for (const auto& [id, label] : catalog.datasets()) {
    index_of.insert(id, loaded.datasets.size());
    loaded.datasets.push_back(
        {.id = id,
         .source = catalog.datasetSourceName(id).value_or(label),
         .path = catalog.datasetSourcePath(id),
         .label = catalog.datasetLabel(id).value_or(label),
         .topics = {}});
  }
  catalog.forEachItem([&loaded, &index_of](const CatalogItem& item) {
    if (const auto found = index_of.constFind(item.dataset_id); found != index_of.cend()) {
      loaded.datasets[*found].topics.insert(item.topic_name);
    }
  });
  // A loaded file named under another label still names it: the curve binding falls back to the path alone. A path
  // shared by a fan-out file's entries names none of them, so those stay per entry.
  loaded.names_one = [&catalog](const layout_xml::DatasetIdentity& identity) {
    return catalog.resolveDatasetIdentity(0, identity.source, identity.path).id.has_value() ||
           (!identity.path.isEmpty() && catalog.resolveDatasetIdentity(0, {}, identity.path).id.has_value());
  };
  // The rules and their reasons: ARCHITECTURE "One layout over several datasets".
  layout_xml::DatasetChoice choice = layout_xml::planDatasetChoice(doc, saved, loaded);
  switch (choice.kind) {
    case layout_xml::DatasetChoice::Kind::kKeep:
      then({.doc = std::move(doc), .removed = {}, .compared = {}});
      return;
    case layout_xml::DatasetChoice::Kind::kPin:
      layout_xml::pinLayoutToDataset(doc, choice.datasets.front(), choice.loaded_topics);
      then({.doc = std::move(doc), .removed = {}, .compared = {}});
      return;
    case layout_xml::DatasetChoice::Kind::kUnbind:
      layout_xml::removeDatasetQualifiersForGenericLayout(doc);
      then({.doc = std::move(doc), .removed = {}, .compared = {}});
      return;
    case layout_xml::DatasetChoice::Kind::kAsk:
      break;
  }
  if (!can_prompt) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "layout-dataset-chosen",
        tr("Layout '%1' fits %2 loaded datasets; it shows the first loaded one, \"%3\".")
            .arg(layout_name)
            .arg(choice.datasets.size())
            .arg(choice.datasets.front().label));
    layout_xml::pinLayoutToDatasets(doc, {choice.datasets.front()}, choice.loaded_topics);
    then({.doc = std::move(doc), .removed = {}, .compared = {}});
    return;
  }
  const QString single_only =
      choice.blockers.isEmpty()
          ? QString()
          : tr("This layout shows one dataset at a time: %1 cannot be copied for a second dataset.")
                .arg(choice.blockers.join(u", "_s));
  QStringList labels;
  for (const layout_xml::DatasetPin& pin : std::as_const(choice.datasets)) {
    labels.push_back(pin.label);
  }
  QPointer<MainWindow> self(this);
  // Files chosen before they loaded answer the question: every candidate dataset from one of them (a fan-out file's
  // entries share its path). When the layout cannot be copied and several were chosen, the question still asks, so
  // it can say why only one can be shown.
  QList<int> preanswered;
  for (qsizetype index = 0; index < choice.datasets.size(); ++index) {
    const QString& path = choice.datasets[index].path;
    if (std::any_of(chosen_files.begin(), chosen_files.end(), [&path](const QString& chosen) {
          return layout_xml::isSamePath(path, chosen);
        })) {
      preanswered.push_back(static_cast<int>(index));
    }
  }
  if (!choice.blockers.isEmpty() && preanswered.size() > 1) {
    preanswered.clear();
  }
  auto apply_choice = [self, doc, pins = std::move(choice.datasets), loaded_topics = std::move(choice.loaded_topics),
                       then = std::move(then)](std::optional<QList<int>> chosen) mutable {
    if (self.isNull()) {
      return;
    }
    if (!chosen.has_value()) {
      then({});
      return;
    }
    QList<layout_xml::DatasetPin> picked;
    for (const int index : std::as_const(*chosen)) {
      // The browser's question does not block: a dataset may have been removed meanwhile.
      if (self->session_->catalogModel().datasetLabel(pins[index].id).has_value()) {
        picked.push_back(pins[index]);
      }
    }
    if (picked.isEmpty()) {
      then(
          {.doc = std::nullopt,
           .removed = tr("The chosen datasets were removed while the question was open; nothing was loaded."),
           .compared = {}});
      return;
    }
    const CatalogModel& live_catalog = self->session_->catalogModel();
    const CurveColorRegistry& colors = self->session_->curveColorRegistry();
    layout_xml::pinLayoutToDatasets(doc, picked, loaded_topics, [&live_catalog, &colors](const QDomElement& curve) {
      const std::optional<QString> key = live_catalog.resolveCurveKey(
          curve.attribute(u"dataset_id"_s).toUInt(), curve.attribute(u"dataset_source"_s),
          curve.attribute(u"dataset_path"_s), curve.attribute(u"topic"_s), curve.attribute(u"field"_s));
      const std::optional<QString> hex = key.has_value() ? colors.color(*key) : std::nullopt;
      return hex.has_value() ? std::optional<QColor>(QColor(*hex)) : std::nullopt;
    });
    const QString removed =
        picked.size() < chosen->size()
            ? tr("Some of the chosen datasets were removed while the question was open; the layout shows the "
                 "others.")
            : QString();
    std::vector<DatasetId> compared;
    if (picked.size() > 1) {
      for (const layout_xml::DatasetPin& pin : std::as_const(picked)) {
        compared.push_back(pin.id);
      }
    }
    then({.doc = std::move(doc), .removed = removed, .compared = std::move(compared)});
  };
  if (!preanswered.isEmpty()) {
    apply_choice(preanswered);
    return;
  }
  LayoutDatasetDialog::ask(this, layout_name, labels, single_only, std::move(apply_choice));
}

QStringList MainWindow::startLayoutStreams(
    const QList<layout_xml::StreamingSourceRef>& streams, LayoutStreamBinding& binding) {
  QStringList failures;
  for (const layout_xml::StreamingSourceRef& stream : streams) {
    QString error;
    const QString config = expandSourceConfigVariables(stream.plugin_config_json, deployment_page_url_, &error);
    const DatasetId dataset_id =
        error.isEmpty() ? streaming_manager_->startFromLayout(stream, config, error) : DatasetId{0};
    if (dataset_id == 0) {
      failures.push_back(tr("Stream '%1': %2").arg(stream.id, error));
      continue;
    }
    binding.add(stream, dataset_id);
  }
  return failures;
}

void MainWindow::loadLayoutFromBytes(const QByteArray& bytes, const QString& browser_name, const QUrl& layout_url) {
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
#ifdef PJ_WITH_SCENE3D
  // An upload has no location, so only its absolute robot-resource references resolve.
  if (QString resource_error; !pj::scene3d::resolveLayoutResourceUrls(doc, layout_url, &resource_error)) {
    showBrowserLayoutWarning(
        this, tr("Load Layout"), tr("'%1' cannot be loaded: %2").arg(browser_name, resource_error));
    finishBrowserLayoutLoad(browser_name, false, u"resources"_s);
    return;
  }
#endif
  const bool source_bound = layout_xml::isSourceBoundLayout(doc);
  if (browser_layout_runtime_ != nullptr) {
    browser_layout_runtime_->pending_recipe_name = browser_name;
    // Only a source-free document may enter browser storage: a source-bound one can carry plugin configuration.
    browser_layout_runtime_->pending_generic_recipe = !source_bound;
  }
  loadBrowserParsedLayout(std::move(doc), browser_name, layout_url);
}

void MainWindow::loadBrowserParsedLayout(QDomDocument doc, const QString& browser_name, const QUrl& layout_url) {
  snapshotStreamWindowAtLayoutOpen();
  const QDomElement root = doc.documentElement();
  bool version_ok = false;
  const int version = root.attribute(u"pj4_version"_s, u"0"_s).toInt(&version_ok);
  if (version_ok && version > layout_xml::kStreamingLayoutSchemaVersion) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "schema-newer",
        tr("Layout '%1' was saved by a newer PJ4 (pj4_version=%2 > %3); loading best-effort.")
            .arg(browser_name)
            .arg(version)
            .arg(layout_xml::kStreamingLayoutSchemaVersion));
  }

  // No durable directory: source paths are reselected (an upload) or downloaded beside `layout_url`, never resolved
  // as files.
  layout_xml::removeUnvalidatedDatasetIds(doc);
  layout_xml::normalizePlotRangeBasis(doc);
  if (layout_xml::isSourceBoundLayout(doc)) {
    QList<layout_xml::DataSourceRef> sources = layout_xml::extractDataSource(doc, QDir());
    beginBrowserSourceReplay(std::move(doc), browser_name, std::move(sources), layout_url);
    return;
  }
  // The dataset question does not block in the browser, so its answer finishes the load, before or after this returns.
  chooseLayoutDatasets(
      std::move(doc), std::nullopt, /*can_prompt=*/true, browser_name, {}, [this, browser_name](ChosenLayout chosen) {
        if (!chosen.removed.isEmpty()) {
          showBrowserLayoutWarning(this, tr("Load Layout"), chosen.removed);
        }
        if (!chosen.doc.has_value()) {
          finishBrowserLayoutLoad(browser_name, false, chosen.removed.isEmpty() ? u"cancelled"_s : u"removed"_s);
          return;
        }
        const bool applied = applyBrowserRestoredLayout(std::move(*chosen.doc), browser_name, {}, chosen.compared);
        finishBrowserLayoutLoad(browser_name, applied, applied ? QString() : u"apply"_s);
      });
}

void MainWindow::beginBrowserSourceReplay(
    QDomDocument doc, const QString& browser_name, QList<layout_xml::DataSourceRef> sources, const QUrl& layout_url) {
  if (browser_layout_runtime_ == nullptr || browser_layout_runtime_->replay.has_value()) {
    finishBrowserLayoutLoad(browser_name, false, u"busy"_s);
    return;
  }
  // Their provider plugins are desktop-only, and the cache file to reselect only ever existed on a desktop.
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
  // Streams are checked and filled in for this page before anything is fetched.
  QString stream_error;
  const QList<layout_xml::StreamingSourceRef> stream_records = layout_xml::extractStreamingSources(doc, &stream_error);
  if (!stream_error.isEmpty()) {
    showBrowserLayoutWarning(this, tr("Load source-bound layout"), stream_error);
    finishBrowserLayoutLoad(browser_name, false, u"stream-records"_s);
    return;
  }
  std::vector<BrowserLayoutRuntime::ReplayStream> streams;
  for (const layout_xml::StreamingSourceRef& record : stream_records) {
    QString config_error;
    QString expanded = expandSourceConfigVariables(record.plugin_config_json, deployment_page_url_, &config_error);
    if (!config_error.isEmpty()) {
      showBrowserLayoutWarning(
          this, tr("Load source-bound layout"), tr("Stream '%1' cannot start: %2").arg(record.id, config_error));
      finishBrowserLayoutLoad(browser_name, false, u"stream-config"_s);
      return;
    }
    streams.push_back({.record = record, .expanded_config = std::move(expanded)});
  }
  browser_layout_runtime_->replay.emplace(
      BrowserLayoutRuntime::Replay{
          .doc = std::move(doc),
          .layout_name = browser_name,
          .sources = std::move(sources),
          .staged_inputs = {},
          .selected_identities = {},
          .selection_index = 0,
          .load_failed = false,
          .load_failure = {},
          .previous_workspace = captureWorkspace(),
          .selection_button = {},
          .active_picker_generation = 0,
          .source_urls = {},
          .staged_bytes = 0,
          .pending_tickets = {},
          .cancelled = false,
          .receiver = std::make_unique<QObject>(),
          .streams = std::move(streams),
          .stream_datasets = {},
          .transport_before_streams = std::nullopt,
      });

  BrowserLayoutRuntime::Replay& replay = *browser_layout_runtime_->replay;
  for (int index = 0; index < replay.sources.size(); ++index) {
    const layout_xml::DataSourceRef& source = replay.sources[index];
    replay.files.push_back({
        .source_index = index,
        .side = false,
        .logical_path = source.serialized_path,
        .content_sha256 = source.content_sha256,
        .content_sha256_malformed = source.content_sha256_malformed,
    });
  }
  for (int index = 0; index < replay.sources.size(); ++index) {
    for (const layout_xml::SideFileRef& side_file : replay.sources[index].side_files) {
      replay.files.push_back({
          .source_index = index,
          .side = true,
          .logical_path = side_file.filename,
          .content_sha256 = side_file.content_sha256,
          .content_sha256_malformed = side_file.content_sha256_malformed,
      });
    }
  }
  if (replay.sources.isEmpty()) {
    if (replay.streams.empty()) {
      finishBrowserLayoutLoad(browser_name, false, u"source-list"_s);
      return;
    }
    // Streams only: nothing to download or reselect.
    startBrowserReplayImports();
    return;
  }
  QSet<QString> logical_paths;
  for (const layout_xml::DataSourceRef& source : replay.sources) {
    if (source.serialized_path.isEmpty() || logical_paths.contains(source.serialized_path)) {
      showBrowserLayoutWarning(
          this, tr("Load source-bound layout"), tr("The layout does not uniquely identify every saved data source."));
      finishBrowserLayoutLoad(browser_name, false, u"source-list"_s);
      return;
    }
    logical_paths.insert(source.serialized_path);
  }
  if (layout_url.isValid()) {
    // Downloaded: every file is named by its URL, so equal basenames are not ambiguous. A data file resolves
    // against the layout, a side file against its data file. Refuse what cannot be fetched or checked before any
    // request.
    for (const BrowserLayoutRuntime::ReplayFile& file : replay.files) {
      QString error;
      const QUrl base = file.side ? replay.source_urls[file.source_index] : layout_url;
      const QUrl url = layout_xml::resolveFileSourceUrl(file.logical_path, base, &error);
      if (url.isEmpty() || file.content_sha256_malformed) {
        showBrowserLayoutWarning(
            this, tr("Load Layout"),
            url.isEmpty() ? error : tr("'%1' records a malformed content fingerprint.").arg(file.logical_path));
        finishBrowserLayoutLoad(browser_name, false, url.isEmpty() ? u"source-url"_s : u"fingerprint"_s);
        return;
      }
      replay.source_urls.push_back(url);
    }
    downloadNextReplaySource();
    return;
  }
  // Reselected: the name says which file to pick, and a fingerprint must tell same-named files apart.
  for (const BrowserLayoutRuntime::ReplayFile& file : replay.files) {
    const QString basename = BrowserFileStore::sanitizedBasename(file.logical_path);
    if (isAmbiguousReplayName(basename) && file.content_sha256.isEmpty()) {
      showBrowserLayoutWarning(
          this, tr("Load source-bound layout"),
          tr("This layout contains more than one file named '%1', but no valid content fingerprints to tell them "
             "apart safely. Open the sources separately and use a newer source-bound layout.")
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
  const int side_count = static_cast<int>(replay.files.size() - replay.sources.size());
  QString decision_text;
  if (replay.sources.size() == 1) {
    decision_text =
        side_count == 0
            ? tr("This layout was saved with data source '%1'. Reselect that file to reload it, or apply the layout "
                 "to compatible data already open in this page.")
                  .arg(first_name)
            : tr("This layout was saved with data source '%1', whose plugin also reads %2 more file(s) you select "
                 "next. Reselect that file to reload it, or apply the layout to compatible data already open in "
                 "this page.")
                  .arg(first_name, QString::number(side_count));
  } else {
    decision_text =
        side_count == 0
            ? tr("This layout was saved with %1 data sources. Select source 1 of %1: '%2' for saved slot '%3'. "
                 "Every file will be staged before any import begins.")
                  .arg(QString::number(replay.sources.size()), first_name, first_context)
            : tr("This layout was saved with %1 data sources, whose plugins also read %4 more file(s) you select "
                 "after them. Select source 1 of %1: '%2' for saved slot '%3'. Every file will be staged before any "
                 "import begins.")
                  .arg(QString::number(replay.sources.size()), first_name, first_context, QString::number(side_count));
  }
  dialog->setText(decision_text);
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
      // The dialog stays open until the picker calls back: a browser can drop a picker request without a callback,
      // and the user can then click again.
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
      // Reselect never finishes the dialog: an external close after the click falls through to the cancel below,
      // so a vanished dialog cannot lock the layout actions.
    }
    if (choice == 1) {
      BrowserLayoutRuntime::Replay& active = *self->browser_layout_runtime_->replay;
      QDomDocument apply_doc = active.doc;
      QHash<QString, QString> identity_for_logical_path;
      QSet<QString> consumed_identities;
      QStringList matched_identities;
      const auto live_datasets = self->session_->catalogModel().datasets();
      for (const layout_xml::DataSourceRef& source : active.sources) {
        const QString expected_name = BrowserFileStore::sanitizedBasename(source.serialized_path);
        const bool basename_is_ambiguous =
            std::count_if(
                active.sources.cbegin(), active.sources.cend(),
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
          const int expected_count = source.datasets.isEmpty() ? 1 : static_cast<int>(source.datasets.size());
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
          const QString layout_name = active.layout_name;
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
      self->applyBrowserReplay(std::move(apply_doc), u"apply"_s);
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
  // Supersedes any picker the browser left pending. Page-wide, not per replay, so a late callback cannot attach to
  // a later layout load.
  ++browser_layout_runtime_->next_picker_generation;
  if (browser_layout_runtime_->next_picker_generation == 0) {
    ++browser_layout_runtime_->next_picker_generation;
  }
  const quint64 generation = browser_layout_runtime_->next_picker_generation;
  browser_layout_runtime_->replay->active_picker_generation = generation;
  return generation;
}

void MainWindow::selectBrowserReplaySource() {
#ifdef PJ_TARGET_WASM
  const quint64 generation = beginBrowserReplayPickerGeneration();
  if (generation == 0) {
    return;
  }
  // A side file is picked with a filter for its own extension: the data-file filter would hide it.
  QString filter;
  const BrowserLayoutRuntime::Replay& replay = *browser_layout_runtime_->replay;
  if (replay.selection_index < replay.files.size() && replay.files[replay.selection_index].side) {
    const QString name = BrowserFileStore::sanitizedBasename(replay.files[replay.selection_index].logical_path);
    const QString suffix = QFileInfo(name).suffix();
    filter = suffix.isEmpty() ? tr("All files (*)") : u"%1 (*.%2)"_s.arg(name, suffix);
  }
  QPointer<MainWindow> self(this);
  file_loader_->selectBrowserInput(
      this,
      [self, generation](FileLoader::BrowserSelectionResult selection) mutable {
        if (self.isNull()) {
          return;
        }
        self->loadBrowserReplaySource(
            generation, std::move(selection.browser_name), std::move(selection.input), std::move(selection.error));
      },
      FileSelectionService::Fingerprint::kBeforeSelection, filter);
#endif
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
  // Consumed once: a duplicate delivery is stale even before the next click.
  replay.active_picker_generation = 0;
  const QString layout_name = replay.layout_name;
  if (replay.selection_index < 0 || replay.selection_index >= replay.files.size()) {
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

  const BrowserLayoutRuntime::ReplayFile& file = replay.files[replay.selection_index];
  const QString expected_name = BrowserFileStore::sanitizedBasename(file.logical_path);
  const QString selected_name = BrowserFileStore::sanitizedBasename(browser_name);
  if (selected_name != expected_name) {
    showBrowserLayoutWarning(
        this, tr("Load source-bound layout"),
        tr("Select '%1' to replay this layout (selected '%2').").arg(expected_name, selected_name));
    qInfo("PJ_WASM_SOURCE_LAYOUT_SELECTION_REJECTED index=%d reason=name", replay.selection_index);
    return;
  }
  if (isAmbiguousReplayName(expected_name) && input->content_sha256 != file.content_sha256) {
    showBrowserLayoutWarning(
        this, tr("Load source-bound layout"),
        file.side ? tr("The selected '%1' is not the file saved as '%2'. Select the original file.")
                        .arg(expected_name, file.logical_path)
                  : tr("The selected '%1' is not the file saved for source %2 of %3 ('%4'). Select the original "
                       "file for that saved slot.")
                        .arg(
                            expected_name, QString::number(replay.selection_index + 1),
                            QString::number(replay.sources.size()), file.logical_path));
    qInfo("PJ_WASM_SOURCE_LAYOUT_SELECTION_REJECTED index=%d reason=content", replay.selection_index);
    return;
  }

  stageReplayInput(std::move(*input), selected_name);
  if (replay.selection_index < replay.files.size()) {
    promptBrowserReplayFile();
    return;
  }
  if (!browser_layout_runtime_->decision_dialog.isNull()) {
    QPointer<MessageBox> decision = browser_layout_runtime_->decision_dialog;
    browser_layout_runtime_->decision_dialog.clear();
    decision->accept();
  }
  startBrowserReplayImports();
}

bool MainWindow::isAmbiguousReplayName(const QString& basename) const {
  const BrowserLayoutRuntime::Replay& replay = *browser_layout_runtime_->replay;
  return std::count_if(replay.files.cbegin(), replay.files.cend(), [&basename](const auto& file) {
           return BrowserFileStore::sanitizedBasename(file.logical_path) == basename;
         }) > 1;
}

QString MainWindow::browserReplaySideFilePrompt(
    const QString& name, const QString& source_path, int number, int total) {
  return tr("Every data source is staged. Select '%1', which data source '%2' reads (file %3 of %4). No import has "
            "started yet.")
      .arg(name, source_path, QString::number(number), QString::number(total));
}

void MainWindow::promptBrowserReplayFile() {
  BrowserLayoutRuntime::Replay& replay = *browser_layout_runtime_->replay;
  const BrowserLayoutRuntime::ReplayFile& file = replay.files[replay.selection_index];
  const QString name = BrowserFileStore::sanitizedBasename(file.logical_path);
  QString text;
  QString button;
  if (file.side) {
    const int sources = static_cast<int>(replay.sources.size());
    text = browserReplaySideFilePrompt(
        name, replay.sources[file.source_index].serialized_path, replay.selection_index - sources + 1,
        static_cast<int>(replay.files.size()) - sources);
    button = tr("Select '%1'").arg(name);
  } else {
    const layout_xml::DataSourceRef& source = replay.sources[file.source_index];
    const QString context = source.plugin_id.isEmpty()
                                ? source.serialized_path
                                : tr("%1 (plugin: %2)").arg(source.serialized_path, source.plugin_id);
    const QString total = QString::number(replay.sources.size());
    text = tr("Staged %1 of %2 data sources. Select source %3 of %2: '%4' for saved slot '%5'. No import has "
              "started yet.")
               .arg(
                   QString::number(replay.selection_index), total, QString::number(replay.selection_index + 1), name,
                   context);
    button = tr("Select source %1 of %2").arg(QString::number(replay.selection_index + 1), total);
  }
  if (!browser_layout_runtime_->decision_dialog.isNull()) {
    browser_layout_runtime_->decision_dialog->setText(text);
  }
  if (!replay.selection_button.isNull()) {
    replay.selection_button->setText(button);
  }
}

void MainWindow::stageReplayInput(LoadInput input, const QString& name) {
  BrowserLayoutRuntime::Replay& replay = *browser_layout_runtime_->replay;
  if (replay.files[replay.selection_index].side) {
    // A later "Save Layout with Sources" finds this copy's name and fingerprint by its path, as for a dialog pick.
    file_loader_->rememberBrowserUpload(input);
    replay.staged_side_files.push_back(std::move(input));
  } else {
    replay.selected_identities.push_back(input.source_identity);
    replay.staged_inputs.push_back(std::move(input));
  }
  ++replay.selection_index;
  qInfo(
      "PJ_WASM_SOURCE_LAYOUT_STAGED index=%d total=%d name=%s", replay.selection_index,
      static_cast<int>(replay.files.size()), qPrintable(name));
}

void MainWindow::downloadNextReplaySource() {
  BrowserLayoutRuntime::Replay& replay = *browser_layout_runtime_->replay;
  const quint64 generation = beginBrowserReplayPickerGeneration();
  const QUrl url = replay.source_urls[replay.selection_index];
  // Streamed into its staged file (in the browser, never through the wasm heap) and stopped at what is left of the
  // preset's cap. A dropped completion drops `input`, whose lease removes the partial file.
  BrowserFileStore::StageResult staged = browser_layout_runtime_->store.reserve(url.fileName());
  if (!staged.input.has_value()) {
    addDownloadedReplaySource(generation, std::nullopt, staged.error);
    return;
  }
  const QString path = staged.input->backing_path;
  const QUrl layout_url = browser_layout_runtime_->loading_url;
  browser_layout_runtime_->downloads->downloadToFile(
      url, path, kMaxPresetDataBytes - replay.staged_bytes,
      [this, generation, layout_url,
       input = std::move(*staged.input)](const PresetController::FileDownload& file) mutable {
        if (!isActiveReplayDownload(generation)) {
          return;
        }
        if (file.limit_exceeded) {
          failReplayOverCap();
          return;
        }
        if (!file.error.isEmpty()) {
          failReplayDownload(file.error);
          return;
        }
        // Ends at received == total, also when the size was not declared.
        emit layoutUrlLoadProgress(layout_url, file.size, file.size);
        browser_layout_runtime_->replay->staged_bytes += file.size;
        input.content_sha256 = file.sha256;
        addDownloadedReplaySource(generation, std::move(input), QString());
      },
      [this, generation, layout_url](qint64 received, qint64 total) {
        if (isActiveReplayDownload(generation)) {
          emit layoutUrlLoadProgress(layout_url, received, total);
        }
      },
      replay.files[replay.selection_index].content_sha256);
}

bool MainWindow::isActiveReplayDownload(quint64 generation) const {
  return browser_layout_runtime_->replay.has_value() &&
         browser_layout_runtime_->replay->active_picker_generation == generation;
}

void MainWindow::failReplayDownload(const QString& error) {
  const BrowserLayoutRuntime::Replay& replay = *browser_layout_runtime_->replay;
  const QString layout_name = replay.layout_name;  // finishing ends the replay
  showBrowserLayoutWarning(
      this, tr("Load Layout"),
      tr("'%1' could not be downloaded: %2").arg(replay.source_urls[replay.selection_index].toString(), error));
  finishBrowserLayoutLoad(layout_name, false, u"download"_s);
}

void MainWindow::failReplayOverCap() {
  const QString layout_name = browser_layout_runtime_->replay->layout_name;  // finishing ends the replay
  showBrowserLayoutWarning(
      this, tr("Load Layout"),
      tr("The layout's data sources exceed the %1 MiB a downloaded layout may load.")
          .arg(kMaxPresetDataBytes / (1024 * 1024)));
  finishBrowserLayoutLoad(layout_name, false, u"size"_s);
}

void MainWindow::addDownloadedReplaySource(quint64 generation, std::optional<LoadInput> input, const QString& error) {
  if (!isActiveReplayDownload(generation)) {
    return;
  }
  BrowserLayoutRuntime::Replay& replay = *browser_layout_runtime_->replay;
  replay.active_picker_generation = 0;
  const QString layout_name = replay.layout_name;  // finishing ends the replay
  const BrowserLayoutRuntime::ReplayFile& file = replay.files[replay.selection_index];
  if (!input.has_value()) {
    showBrowserLayoutWarning(this, tr("Load Layout"), error);
    finishBrowserLayoutLoad(layout_name, false, u"staging"_s);
    return;
  }
  if (!file.content_sha256.isEmpty() && input->content_sha256 != file.content_sha256) {
    showBrowserLayoutWarning(
        this, tr("Load Layout"),
        tr("'%1' is not the file its layout was saved with (its content fingerprint differs).").arg(file.logical_path));
    finishBrowserLayoutLoad(layout_name, false, u"fingerprint"_s);
    return;
  }
  stageReplayInput(std::move(*input), replay.source_urls[replay.selection_index].fileName());
  if (replay.selection_index < replay.files.size()) {
    downloadNextReplaySource();
    return;
  }
  startBrowserReplayImports();
}

void MainWindow::cancelBrowserReplayImports() {
  // A copy: a queued cancel resolves later, never inside this loop.
  const QSet<quint64> tickets = browser_layout_runtime_->replay->pending_tickets;
  for (const quint64 ticket : tickets) {
    file_loader_->cancelLoad(ticket, /*keep_partial=*/false);
  }
}

void MainWindow::startBrowserReplayImports() {
  if (browser_layout_runtime_ == nullptr || !browser_layout_runtime_->replay.has_value()) {
    return;
  }
  BrowserLayoutRuntime::Replay& replay = *browser_layout_runtime_->replay;
  const QString layout_name = replay.layout_name;
  if (replay.sources.size() != replay.selected_identities.size() ||
      replay.sources.size() != static_cast<int>(replay.staged_inputs.size()) ||
      replay.files.size() - replay.sources.size() != static_cast<qsizetype>(replay.staged_side_files.size())) {
    finishBrowserLayoutLoad(layout_name, false, u"staging-shape"_s);
    return;
  }

  // Both die with the replay's receiver, so `replay` exists inside them. Other loads may run meanwhile.
  connect(
      file_loader_.get(), &FileLoader::fileLoadFailed, replay.receiver.get(),
      [this](const QString& path, const QString& reason) {
        BrowserLayoutRuntime::Replay& active = *browser_layout_runtime_->replay;
        if (active.selected_identities.contains(path) && active.load_failure.isEmpty()) {
          active.load_failure = reason;
        }
      });
  connect(
      file_loader_.get(), &FileLoader::loadFinished, replay.receiver.get(),
      [this](quint64 ticket, LoadOutcome outcome, const QString&, DatasetId, const QVector<DatasetId>&) {
        BrowserLayoutRuntime::Replay& active = *browser_layout_runtime_->replay;
        if (!active.pending_tickets.remove(ticket)) {
          return;
        }
        if (outcome != LoadOutcome::kLoaded && !active.load_failed) {
          // The transaction already failed: stop the rest instead of importing it.
          active.load_failed = true;
          cancelBrowserReplayImports();
        }
        if (active.pending_tickets.isEmpty()) {
          finishBrowserReplayImports();
        }
      });

  for (int index = 0; index < replay.sources.size() && !replay.load_failed; ++index) {
    const layout_xml::DataSourceRef& source = replay.sources[index];
    LoadHints hints = layoutReplayHints(source, /*prefer_reuse=*/false);
    LoadInput& input = replay.staged_inputs[static_cast<std::size_t>(index)];
    // Point the saved side-file names at their staged copies, which then live as long as the data file's.
    std::vector<StorageLease> side_leases;
    for (qsizetype file = replay.sources.size(), side = 0; file < replay.files.size(); ++file, ++side) {
      if (replay.files[file].source_index == index) {
        LoadInput& staged = replay.staged_side_files[static_cast<std::size_t>(side)];
        hints.side_file_paths.insert(replay.files[file].logical_path, staged.backing_path);
        side_leases.push_back(std::move(staged.lease));
      }
    }
    if (!side_leases.empty()) {
      side_leases.push_back(std::move(input.lease));
      input.lease = std::make_shared<std::vector<StorageLease>>(std::move(side_leases));
    }
    const quint64 ticket = file_loader_->loadFileTicketed(std::move(input), this, hints);
    if (ticket == 0) {
      replay.load_failed = true;
      replay.load_failure = tr("A staged source could not be queued for import.");
      cancelBrowserReplayImports();
    } else {
      replay.pending_tickets.insert(ticket);
    }
  }
  if (replay.pending_tickets.isEmpty()) {
    finishBrowserReplayImports();
  }
}

void MainWindow::finishBrowserReplayImports() {
  if (browser_layout_runtime_ == nullptr || !browser_layout_runtime_->replay.has_value()) {
    return;
  }
  BrowserLayoutRuntime::Replay& replay = *browser_layout_runtime_->replay;
  const QString layout_name = replay.layout_name;
  if (replay.cancelled) {
    rollbackBrowserReplayImports();
    finishBrowserLayoutLoad(layout_name, false, u"cancelled"_s);
    return;
  }
  QString validation_error;
  if (replay.load_failed) {
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
    const int expected_count = source.datasets.isEmpty() ? 1 : static_cast<int>(source.datasets.size());
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
  // Streams share the timeline's slots with the files.
  for (const BrowserLayoutRuntime::ReplayStream& stream : replay.streams) {
    const int slot = stream.record.dataset.timeline_order;
    if (validation_error.isEmpty() && slot >= 0 && timeline_slots.contains(slot)) {
      validation_error = tr("The saved Source Timeline contains duplicate track positions.");
    }
    if (slot >= 0) {
      timeline_slots.insert(slot);
    }
  }
  for (int slot = 0; validation_error.isEmpty() && slot < timeline_slots.size(); ++slot) {
    if (!timeline_slots.contains(slot)) {
      validation_error = tr("The saved Source Timeline contains invalid track positions.");
    }
  }

  if (!validation_error.isEmpty()) {
    const QString reason = replay.load_failed ? u"import"_s : u"source-shape"_s;
    showBrowserLayoutWarning(this, tr("Load source-bound layout"), validation_error);
    rollbackBrowserReplayImports();
    finishBrowserLayoutLoad(layout_name, false, reason);
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
  applyBrowserReplay(std::move(apply_doc), u"apply-after-source-load"_s);
}

void MainWindow::applyBrowserReplay(QDomDocument apply_doc, const QString& apply_failure) {
  const QString layout_name = browser_layout_runtime_->replay->layout_name;  // finishing ends the replay
  const QString failure = startReplayStreamsAndApply(std::move(apply_doc), apply_failure);
  if (!failure.isEmpty()) {
    // All or nothing: sources imported or started for a layout that cannot apply go too.
    rollbackBrowserReplayImports();
  } else if (!browser_layout_runtime_->loading_url.isEmpty()) {
    replacePreviousPresetData();
  }
  finishBrowserLayoutLoad(layout_name, failure.isEmpty(), failure);
}

QString MainWindow::startReplayStreamsAndApply(QDomDocument apply_doc, const QString& apply_failure) {
  BrowserLayoutRuntime::Replay& replay = *browser_layout_runtime_->replay;
  if (!replay.streams.empty()) {
    const PlaybackEngine& engine = session_->playbackEngine();
    replay.transport_before_streams = BrowserLayoutRuntime::Transport{
        .followed_stream = active_streaming_dataset_id_,
        .seeded = streaming_playback_seeded_,
        .playing = engine.isPlaying(),
        .current_time = engine.currentTime(),
    };
  }
  LayoutStreamBinding streams;
  for (const BrowserLayoutRuntime::ReplayStream& stream : replay.streams) {
    QString error;
    const DatasetId dataset_id = streaming_manager_->startFromLayout(stream.record, stream.expanded_config, error);
    if (dataset_id == 0) {
      showBrowserLayoutWarning(
          this, tr("Load source-bound layout"), tr("Stream '%1' could not start: %2").arg(stream.record.id, error));
      return u"stream"_s;
    }
    replay.stream_datasets.push_back(dataset_id);
    streams.add(stream.record, dataset_id);
  }
  // Stream-bound items bind to the fresh sessions' DatasetIds and wait for their topics.
  layout_xml::bindStreamQualifiers(apply_doc, streams.dataset_for_identity);
  return applyBrowserRestoredLayout(std::move(apply_doc), replay.layout_name, streams.tracks) ? QString{}
                                                                                              : apply_failure;
}

void MainWindow::replacePreviousPresetData() {
  BrowserLayoutRuntime& runtime = *browser_layout_runtime_;
  const BrowserLayoutRuntime::Replay& replay = *runtime.replay;
  BrowserLayoutRuntime::PresetData loaded{
      .file_identities = QSet<QString>(replay.selected_identities.cbegin(), replay.selected_identities.cend()),
      .datasets = replay.stream_datasets,
  };
  for (const auto& [dataset_id, unused_name] : session_->catalogModel().datasets()) {
    (void)unused_name;
    if (loaded.file_identities.contains(file_loader_->sourcePathForDataset(dataset_id))) {
      loaded.datasets.push_back(dataset_id);
    }
  }
  const BrowserLayoutRuntime::PresetData previous = std::exchange(runtime.preset_data, std::move(loaded));
  std::vector<DatasetId> remove_ids;
  for (const DatasetId dataset_id : previous.datasets) {
    // A stream that ended keeps its dataset; one the user removed is gone already.
    if (session_->sessionManager().dataEngine().getDataset(dataset_id) != nullptr) {
      remove_ids.push_back(dataset_id);
    }
  }
  if (remove_ids.empty()) {
    return;
  }
  for (const DatasetId dataset_id : remove_ids) {
    removeDatasetData(dataset_id);
  }
  for (const QString& identity : previous.file_identities) {
    runtime.sources.remove(identity);
  }
  // The timeline spans what is left, not the previous preset's recordings too.
  if (active_streaming_dataset_id_ == 0) {
    session_->seedPlaybackFromSession();
  }
  resetUndoHistory();
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
  for (const DatasetId dataset_id : replay.stream_datasets) {
    remove_ids.push_back(dataset_id);
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
  const std::optional<BrowserLayoutRuntime::Transport> transport = replay.transport_before_streams;
  PlaybackEngine& engine = session_->playbackEngine();
  // Removing the replay's streams handed live-follow to another stream, or ended it; follow the previous stream
  // again first, so the range below is its window.
  if (transport.has_value() && transport->followed_stream != 0 &&
      !streaming_manager_->layoutSourceId(transport->followed_stream).isEmpty()) {
    active_streaming_dataset_id_ = transport->followed_stream;
    session_->setActiveStreamingDataset(active_streaming_dataset_id_);
    engine.setHoldAtRangeMax(true);
  }
  session_->seedPlaybackFromSession();
  if (transport.has_value()) {
    streaming_playback_seeded_ = transport->seeded;
    engine.setCurrentTime(transport->current_time);
    if (transport->playing) {
      engine.play();
    } else {
      engine.pause();
    }
    // play() may not emit playingChanged here, since the started stream already played.
    refreshStreamingSeekLock();
  }
}

void MainWindow::finishBrowserLayoutLoad(const QString& browser_name, bool applied, const QString& reason) {
  QUrl loading_url;
  if (browser_layout_runtime_ != nullptr) {
    if (browser_layout_runtime_->downloads != nullptr) {
      browser_layout_runtime_->downloads->cancel();
    }
    loading_url = std::exchange(browser_layout_runtime_->loading_url, QUrl());
  }
  if (browser_layout_runtime_ != nullptr && browser_layout_runtime_->replay.has_value()) {
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
  const QString failure = applied ? QString() : (reason.isEmpty() ? u"apply"_s : reason);
  if (applied) {
    qInfo("PJ_WASM_LAYOUT_LOAD_OK name=%s", qPrintable(browser_name));
  } else if (failure == "cancelled"_L1) {
    qInfo("PJ_WASM_SOURCE_LAYOUT_CANCELLED name=%s", qPrintable(browser_name));
  } else {
    qWarning("PJ_WASM_LAYOUT_LOAD_FAILED name=%s reason=%s", qPrintable(browser_name), qPrintable(failure));
  }
  if (!loading_url.isEmpty()) {
    emit layoutUrlLoadFinished(loading_url, applied, failure);
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

void MainWindow::loadLayoutFromUrl(const QUrl& url) {
  const QString name = url.toString();
  if (layout_selection_pending_) {
    qWarning("PJ_WASM_LAYOUT_LOAD_FAILED name=%s reason=busy", qPrintable(name));
    emit layoutUrlLoadFinished(url, false, u"busy"_s);
    return;
  }
  layout_selection_pending_ = true;
  updateBrowserLayoutActions();
  BrowserLayoutRuntime& runtime = *browser_layout_runtime_;
  if (runtime.downloads == nullptr) {
    runtime.downloads = std::make_unique<PresetController>();
  }
  runtime.loading_url = url;
  runtime.downloads->download(url, [this, name](QByteArray bytes, const QUrl& final_url, QString error) {
    if (!error.isEmpty()) {
      showBrowserLayoutWarning(this, tr("Load Layout"), tr("'%1' could not be downloaded: %2").arg(name, error));
      finishBrowserLayoutLoad(name, false, u"download"_s);
      return;
    }
    // Relative references resolve against where the layout was fetched from.
    // The browser reports the requested URL here even after a redirect.
    loadLayoutFromBytes(bytes, name, final_url);
  });
}

void MainWindow::cancelLayoutFromUrl() {
  BrowserLayoutRuntime& runtime = *browser_layout_runtime_;
  // The URL's layout file itself is still downloading: once it arrives, the load either
  // ends synchronously or becomes a replay.
  if (!runtime.loading_url.isEmpty() && !runtime.replay.has_value()) {
    finishBrowserLayoutLoad(runtime.loading_url.toString(), false, u"cancelled"_s);
    return;
  }
  if (!runtime.replay.has_value() || runtime.replay->source_urls.isEmpty() || runtime.replay->cancelled) {
    return;
  }
  if (runtime.replay->pending_tickets.isEmpty()) {
    // Still downloading or staging: nothing was imported yet.
    const QString name = runtime.replay->layout_name;  // finishing ends the replay
    finishBrowserLayoutLoad(name, false, u"cancelled"_s);
    return;
  }
  // The last terminal of its own imports rolls back (finishBrowserReplayImports).
  runtime.replay->cancelled = true;
  cancelBrowserReplayImports();
}

#ifdef PJ_TARGET_WASM
void MainWindow::startBrowserPresets(const QUrl& page_url) {
  auto* discovery = new PresetController(this);
  connect(
      discovery, &PresetController::discovered, this,
      [this, discovery](const QList<PresetEntry>& entries, const QString& deep_link, const QString& deep_link_error) {
        discovery->deleteLater();
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
        qInfo(
            "PJ_WASM_PRESETS_DISCOVERED count=%lld deep_link=%s error=%s", static_cast<long long>(entries.size()),
            qPrintable(deep_link), qPrintable(deep_link_error));
#endif
        if (entries.isEmpty()) {
          // No dialog without presets: the page looks as if it had none.
          if (!deep_link_error.isEmpty()) {
            showBrowserLayoutWarning(this, tr("Open Preset"), deep_link_error);
          }
          return;
        }
        presets_dialog_ = new WelcomePresetsDialog(entries, this);
        presets_dialog_->setWindowModality(Qt::ApplicationModal);
        presets_dialog_->setChromeMetrics(chrome_metrics_);
        connect(this, &MainWindow::chromeMetricsChanged, presets_dialog_, &Dialog::setChromeMetrics);
        connect(
            presets_dialog_, &WelcomePresetsDialog::presetSelected, this,
            [this](const QString&, const QUrl& layout_url) { loadLayoutFromUrl(layout_url); });
        connect(presets_dialog_, &WelcomePresetsDialog::uploadRequested, action_open_data_, &QAction::trigger);
        connect(presets_dialog_, &WelcomePresetsDialog::cancelRequested, this, &MainWindow::cancelLayoutFromUrl);
        connect(this, &MainWindow::layoutUrlLoadFinished, presets_dialog_, &WelcomePresetsDialog::layoutLoadFinished);
        connect(this, &MainWindow::layoutUrlLoadProgress, presets_dialog_, &WelcomePresetsDialog::layoutLoadProgress);
        ui_->actionPresets->setVisible(true);
        // A page an agent opened (?mcp=) is the agent's: an application-modal welcome would hold every mutating tool
        // until the user closed it.
        if (mcp_relay_link_ != nullptr && deep_link.isEmpty() && deep_link_error.isEmpty()) {
          return;
        }
        presets_dialog_->show();
        if (!deep_link_error.isEmpty()) {
          presets_dialog_->showError(deep_link_error);
        } else if (!deep_link.isEmpty()) {
          presets_dialog_->selectPreset(deep_link);
        }
      });
  discovery->start(page_url);
}
#endif

void MainWindow::reviewUncleanShutdown(bool interactive) {
  custom_widgets_->reviewUncleanShutdown(interactive);
}

void MainWindow::loadAtStartup(const QStringList& data_files, const QString& layout_path, bool interactive) {
  const LayoutLoadInteractivity layout_interactivity =
      interactive ? LayoutLoadInteractivity::kStartupAttended : LayoutLoadInteractivity::kAutomated;
  if (data_files.isEmpty()) {
    if (!layout_path.isEmpty()) {
      loadLayoutFromPath(layout_path, layout_interactivity);
    }
    return;
  }

  QStringList absolute_files;
  for (const QString& path : data_files) {
    absolute_files.push_back(QFileInfo(path).absoluteFilePath());
  }
  // The file list only for a layout naming at most one dataset: one naming several keeps its own bindings, mapped to
  // the files ("Command-line startup"), and never asks which datasets to show.
  bool list_files = false;
  if (!layout_path.isEmpty()) {
    QFile file(layout_path);
    QDomDocument doc;
    list_files =
        file.open(QIODevice::ReadOnly) && doc.setContent(&file) && layout_xml::datasetIdentities(doc).size() <= 1;
  }
  const bool ask_mode = file_loader_->batchRepeatsADialog(absolute_files);
  if (interactive && absolute_files.size() >= 2 && (ask_mode || list_files)) {
    QPointer<MainWindow> self(this);
    BatchLoadDialog::ask(
        this, absolute_files, ask_mode, list_files,
        [self, absolute_files, layout_path](std::optional<BatchLoadDialog::Answer> answer) {
          if (self.isNull()) {
            return;
          }
          if (!answer.has_value()) {
            if (!layout_path.isEmpty()) {
              emit self->layoutRestoreSettled(false);
            }
            return;
          }
          self->loadStartupFiles(absolute_files, layout_path, /*interactive=*/true, answer->batch, answer->compared);
        });
    return;
  }
  loadStartupFiles(absolute_files, layout_path, interactive, nullptr, {});
}

void MainWindow::loadStartupFiles(
    const QStringList& absolute_files, const QString& layout_path, bool interactive,
    const std::shared_ptr<LoadBatch>& load_batch, const QStringList& compared) {
  const LayoutLoadInteractivity layout_interactivity =
      interactive ? LayoutLoadInteractivity::kStartupAttended : LayoutLoadInteractivity::kAutomated;
  // By ticket: configuration dialogs can deliver other completions through nested loops.
  struct StartupBatch {
    QSet<LoadRequestId> pending;
    bool enqueuing = true;
    bool failed = false;
    bool completed = false;
  };
  const auto batch = std::make_shared<StartupBatch>();
  auto* receiver = new QObject(this);
  const auto finish = [this, batch, receiver, layout_path, absolute_files, layout_interactivity, compared]() {
    if (batch->enqueuing || !batch->pending.isEmpty() || batch->completed) {
      return;
    }
    batch->completed = true;
    receiver->deleteLater();
    // A window-owned timer: shutdown flushes file terminals synchronously, into a dying window.
    QTimer::singleShot(0, this, [this, batch, layout_path, absolute_files, layout_interactivity, compared]() {
      if (!layout_path.isEmpty()) {
        if (batch->failed) {
          emit layoutRestoreSettled(false);
        } else {
          loadLayoutFromPath(layout_path, layout_interactivity, absolute_files, compared);
        }
      }
    });
  };
  connect(
      file_loader_.get(), &FileLoader::loadFinished, receiver,
      [batch, finish](quint64 ticket, LoadOutcome outcome, const QString&, DatasetId, const QVector<DatasetId>&) {
        if (batch->pending.remove(ticket)) {
          batch->failed |= outcome != LoadOutcome::kLoaded;
          finish();
        }
      });
  for (const QString& path : absolute_files) {
    LoadHints hints;
    hints.dialog_policy = interactive ? DialogPolicy::kInteractive : DialogPolicy::kNever;
    hints.batch = load_batch;
    const auto ticket =
        file_loader_->loadFileTicketed(LoadInput::fromNativePath(path), interactive ? this : nullptr, hints);
    if (ticket == 0) {
      batch->failed = true;
    } else {
      batch->pending.insert(ticket);
    }
  }
  batch->enqueuing = false;
  finish();
}

void MainWindow::enableAutoplay() {
  autoplay_pending_ = true;
  // Once, at the first non-empty range (--test-data sets it synchronously, a --layout load later); a later manual
  // pause or seek is left alone.
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

bool MainWindow::applyBrowserRestoredLayout(
    QDomDocument doc, const QString& browser_name, const LayoutTimelinePlan& stream_tracks,
    const std::vector<DatasetId>& compared) {
  if (session_->catalogModel().datasets().empty() && stream_tracks.empty()) {
    showBrowserLayoutWarning(
        this, tr("Load Layout"), tr("No data is loaded. Open a data source before applying this layout."));
    return false;
  }

  // kExact: an unresolved curve or scene fails and rolls back, never dropped silently. The duration log feeds
  // tests/wasm/presets.spec.js.
  // Aligned before the restore builds and fits the plots, so each copy's view opens on its run's aligned data; a
  // failed restore rolls back to the offsets from before, and one that refuses is not rolled back, so it aligns
  // nothing.
  const CapturedWorkspace rollback = captureWorkspace();
  if (source_timeline_controller_ != nullptr && !restoreRefuses(doc)) {
    source_timeline_controller_->alignStartsOf(compared);
  }
  QElapsedTimer restore_timer;
  restore_timer.start();
  const RestoreResult restored = restoreWorkspaceState(doc, MissingCurvePolicy::kExact, &rollback);
  qInfo("PJ_WASM_LAYOUT_RESTORE_MS ms=%lld", static_cast<long long>(restore_timer.elapsed()));
  if (restored != RestoreResult::kApplied) {
    showBrowserLayoutWarning(
        this, tr("Load Layout"),
        tr("Layout was parsed but could not be applied to the currently loaded data. Open matching data sources "
           "and try again."));
    return false;
  }

  // After the workspace succeeds, so a failure cannot leave a partly moved timeline.
  const LayoutTimelinePlan timeline =
      resolveLayoutTimelineTracks(layout_xml::extractDataSource(doc, QDir()), stream_tracks);
  static_cast<void>(applyLayoutTimelineOffsets(timeline));
  applyLayoutTimelineOrder(timeline);
  restoreBrowserChromeAndPanels(doc, browser_name);
  resetUndoHistory();
  return true;
}

void MainWindow::restoreBrowserChromeAndPanels(const QDomDocument& doc, const QString& browser_name) {
  ui_->curveListPanel->restoreListState(doc.documentElement().firstChildElement(u"curve_list_state"_s));
  restoreRightPanelState(doc.documentElement().firstChildElement(u"right_panel_state"_s));
  restoreLeftPanelState(doc);
  restoreChromeState(doc.documentElement().firstChildElement(u"chrome_state"_s));

  restoreSourceTimelineViewState(doc.documentElement().firstChildElement(u"source_timeline"_s));
  emitDiagnostic(DiagnosticLevel::kInfo, "Layout", "loaded", tr("Loaded layout: %1").arg(browser_name));
}

void MainWindow::applyRestoredLayout(
    QDomDocument doc, const QString& path, MissingCurvePolicy policy, const LayoutTimelinePlan& stream_tracks,
    const std::vector<DatasetId>& compared) {
  // Each curve binds to the first loaded dataset (load order) holding its topic+field, so a multi-file layout
  // restores per source and a layout reuses on a similar recording; no "which dataset?" prompt. Never reached with
  // async work pending (that goes progressive).
  if (session_->catalogModel().datasets().empty() && stream_tracks.empty()) {
    // A no-op: an in-flight previous restore keeps draining.
    reportLayoutRestoreIssue(
        policy, "layout-apply-no-data", tr("No data is loaded. Open a data source before applying this layout."));
    emit layoutRestoreSettled(false);
    return;
  }
  // Commit point (a no-op when the do_reload branch already superseded).
  supersedeActiveRestore();
  // Offsets before processors: a generator replays under the saved offsets. The rollback is captured before they
  // move.
  const CapturedWorkspace rollback = captureWorkspace();
  const QDir layout_dir(QFileInfo(path).absoluteDir());
  const LayoutTimelinePlan timeline_plan =
      resolveLayoutTimelineTracks(layout_xml::extractDataSource(doc, layout_dir), stream_tracks);
  // A restore that refuses (below, reported as a failure) is not rolled back, so the offsets stay put for it.
  if (!restoreRefuses(doc)) {
    static_cast<void>(applyLayoutTimelineOffsets(timeline_plan));
    // Before the restore builds and fits the plots, so each copy's view opens on its run's aligned data.
    if (source_timeline_controller_ != nullptr) {
      source_timeline_controller_->alignStartsOf(compared);
    }
  }
  // The restore path shared with undo/redo. Processors are recreated before the curve rebind, so derived outputs
  // are in the catalog.
  QStringList issues;
  layout_issue_capture_ = &issues;
  const RestoreResult restore_result = restoreWorkspaceState(doc, policy, &rollback);
  layout_issue_capture_ = nullptr;
  switch (restore_result) {
    case RestoreResult::kCancelled:
      emit layoutRestoreSettled(false);
      return;  // the user aborted at the missing-curve prompt
    case RestoreResult::kFailed:
      // A data source reloaded above stays loaded: FileLoader has no unload and DataEngine no transactional commit.
      reportLayoutRestoreIssue(
          policy, "layout-apply-failed",
          tr("Layout was parsed but could not be applied. If a data source was reloaded, it is still loaded.") +
              (issues.isEmpty() ? QString{} : u"\n\n"_s + issues.join(u"\n\n"_s)));
      emit layoutRestoreSettled(false);
      return;
    case RestoreResult::kApplied:
      break;
  }

  restoreChromeAndPanels(doc, path, &timeline_plan);
  commitRestoredLayout(doc, policy);
  emit layoutRestoreSettled(true);
}

void MainWindow::restoreChromeAndPanels(
    const QDomDocument& doc, const QString& path, const LayoutTimelinePlan* resolved_timeline) {
  ui_->curveListPanel->restoreListState(doc.documentElement().firstChildElement(u"curve_list_state"_s));
  restoreRightPanelState(doc.documentElement().firstChildElement(u"right_panel_state"_s));
  restoreChromeState(doc.documentElement().firstChildElement(u"chrome_state"_s));

  // The synchronous path already resolved its sources and applied the offsets.
  if (resolved_timeline != nullptr) {
    applyLayoutTimelineOrder(*resolved_timeline);
  } else {
    // A progressive reload may not have registered every dataset path yet: re-resolved at drain, before the marker
    // generators replay.
    const QDir layout_dir(QFileInfo(path).absoluteDir());
    const QList<layout_xml::DataSourceRef> sources = layout_xml::extractDataSource(doc, layout_dir);
    static_cast<void>(applyTimelineStateFromLayout(sources));
    if (progressive_layout_in_flight_) {
      pending_timeline_sources_ = sources;
    }
  }

  // After the track order, so zoom and scroll map onto the offset-adjusted scene.
  restoreSourceTimelineViewState(doc.documentElement().firstChildElement(u"source_timeline"_s));

  // Pinned toolbox tabs are restored at each leg's commit point, not here: the progressive leg runs this while it
  // can still roll back, and a cancelled restore would lose the user's live toolboxes.
  recordRecentLayout(path);
  emitDiagnostic(DiagnosticLevel::kInfo, "Layout", "loaded", tr("Loaded layout: %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::beginProgressiveLayoutRestore(
    QDomDocument doc, const QString& path, MissingCurvePolicy policy, const LayoutTimelinePlan& stream_tracks) {
  cancelProgressiveLayoutRestore();
  progressive_stream_tracks_ = stream_tracks;
  if (layoutImportBatchActive() && batch_workspace_checkpoint_ != nullptr) {
    // The checkpoint start() took: only async enqueues happened since, so it is the same state.
    progressive_previous_workspace_ = *batch_workspace_checkpoint_;
  } else {
    progressive_previous_workspace_ = capturePortableWorkspace();
  }
  batch_workspace_checkpoint_.reset();
  // The load's interactivity, never inferred from batch survival; consumed at the drain, after every caller has
  // returned.
  progressive_missing_curve_policy_ = policy;
  progressive_layout_in_flight_ = true;
  progressive_layout_doc_ = doc;

  bool applied = false;
  {
    QScopedValueRollback guard(applying_state_, true);
    const RestoreFocusGuard focus_guard(this);
    // Topics are still arriving: the previous graph goes now, nothing replays until the drain, when a missing input
    // means something.
    static_cast<void>(applyDataProcessors(DerivedSnapshot{}));
    // Rewrites doc in place; the progressive binder reports unresolved paths at drain.
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
  // Each waiter names a reason to keep waiting and the signal that may clear it. Plain connections the gate clears
  // when it settles: queueDrained can fire several times between sequential batch children.
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
  progressive_stream_tracks_.clear();
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
  // A wire restored before its stream sends the topic waits for it; the reference makes a demand-driven source
  // subscribe. The panel resolves the wire itself.
  forEachCustomWidgetDock([this](CustomWidgetDockWidget* custom) {
    for (const PendingWireDemand& demand : custom->pendingDemands()) {
      pending_binder_->addPendingCustomWidgetWire(custom, demand.topic_name, demand.preferred_dataset);
    }
  });
  forEachLogPanel([this](LogDockWidget* log) {
    for (const LogDockWidget::PendingDemand& demand : log->pendingDemands()) {
      pending_binder_->addPendingLogTopic(log, demand.topic_name, demand.preferred_dataset);
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
    // Skips the full-workspace serialization when nothing waits.
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
    if (!any_intents) {
      forEachCustomWidgetDock(
          [&any_intents](CustomWidgetDockWidget* custom) { any_intents = any_intents || custom->hasPendingWires(); });
    }
    if (!any_intents) {
      forEachLogPanel(
          [&any_intents](LogDockWidget* log) { any_intents = any_intents || !log->pendingDemands().empty(); });
    }
    if (any_intents) {
      rebuildPendingDisplayBindings(xmlSaveState());
    }
  });
}

MainWindow::SceneRestoreVerdict MainWindow::settleSceneRestores() {
  // Each dock makes a final retry, so a deferred element that became resolvable is validated now; the verdict is
  // folded here because a dock cannot see its siblings.
  forEachSceneDock([](SceneDockWidget* scene_dock) { scene_dock->finishRestore(); });
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
  // An unattended restore must never open a dialog from a late continuation.
  if (policy == MissingCurvePolicy::kRetainAndDiagnose) {
    emitDiagnostic(DiagnosticLevel::kWarning, "Layout", id, message);
  } else {
    // Plain text: messages quote paths and layout content.
    MessageBox::warning(this, tr("Load Layout"), Qt::convertFromPlainText(message, Qt::WhiteSpaceNormal));
  }
}

bool MainWindow::finalizeUnresolvedRestoreState(MissingCurvePolicy policy, const QStringList& skipped_derived) {
  QStringList shown;
  if (pending_binder_ != nullptr) {
    QSet<QString> seen;
    for (const layout_xml::SeriesPath& path : pending_binder_->unresolved()) {
      const QString display = path.display();
      if (!display.isEmpty() && !seen.contains(display)) {
        seen.insert(display);
        shown.push_back(display);
      }
    }
    // Blocking scene references join the prompt: Remove drops them, so the committed baseline carries none and an
    // exact undo back to it can succeed.
    for (const QString& topic : unresolvedPendingSceneRestores()) {
      if (!topic.isEmpty() && !seen.contains(topic)) {
        seen.insert(topic);
        shown.push_back(topic);
      }
    }
  }
  const bool retain_unresolved_intents = !shown.isEmpty() && policy == MissingCurvePolicy::kRetainAndDiagnose;
  if (retain_unresolved_intents) {
    // Retained, so a later binding pass resolves them when their data arrives.
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "layout-import-unresolved-curves",
        tr("%n layout reference(s) could not be bound after the import: %1", nullptr, static_cast<int>(shown.size()))
            .arg(shown.join(u", "_s)));
  } else if (policy != MissingCurvePolicy::kRetainAndDiagnose && (!shown.isEmpty() || !skipped_derived.isEmpty())) {
    switch (promptMissingCurves(shown, layoutImportFailureLines(), skipped_derived)) {
      case MissingCurveChoice::kRemove:
        // Unbound curves are in no widget; blocking scene pends would poison later exact snapshots.
        forEachSceneDock([](SceneDockWidget* scene_dock) { scene_dock->clearBlockingPendingRestores(); });
        break;
      case MissingCurveChoice::kCancel:
        static_cast<void>(abortProgressiveRestore());
        return false;
    }
  }
  if (pending_binder_ != nullptr && !retain_unresolved_intents) {
    pending_binder_->clear();
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
  // Captured at begin, never read from live batch state.
  const MissingCurvePolicy policy = progressive_missing_curve_policy_;
  // kRetainAndDiagnose keeps itemsAdded connected past the drain, so retained intents still bind when their data
  // arrives (main_window_layout_import_binder_test).
  if (policy != MissingCurvePolicy::kRetainAndDiagnose) {
    QObject::disconnect(pending_items_added_conn_);
    pending_items_added_conn_ = {};
  }

  // Offsets before generators: an all-datasets rule reads every dataset in the display frame. Processors before the
  // binder's last pass, so derived curves resolve to fresh outputs.
  static_cast<void>(applyPendingTimelineState());
  // Only the preflight refuses here: a derived object that cannot be restored is skipped (applyDataProcessors).
  QString refusal;
  QStringList skipped_derived;
  if (!progressive_layout_doc_.isNull() &&
      !restoreDataProcessors(
          progressive_layout_doc_.documentElement(), RestoreIntent::kReplace, &refusal, policy, &skipped_derived)) {
    const bool rolled_back = abortProgressiveRestore();
    reportLayoutRestoreIssue(
        policy, "processor-restore-refused",
        rolled_back
            ? tr("The layout's derived objects could not be restored: %1 The previous workspace was restored.")
                  .arg(refusal)
            : tr("The layout's derived objects could not be restored: %1 The previous workspace could not be fully "
                 "restored against the reloaded data. The partial layout was kept as the new undo baseline.")
                  .arg(refusal));
    emit layoutRestoreSettled(false);
    return;
  }

  // A rejected element fails the transaction, not merely warns: otherwise the partial scene becomes the baseline.
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
    // The saved X is absolute and converts with the now-settled offset. Still under the in-flight gate, so
    // onUndoableChange stays suppressed until the snapshot below.
    forEachPlot([](PlotWidget* plot) { plot->applySavedViewportOrZoom(/*clear_after=*/true); });
  }

  if (!finalizeUnresolvedRestoreState(policy, skipped_derived)) {
    emit layoutRestoreSettled(false);
    return;  // the user cancelled at the missing-curve prompt
  }

  progressive_layout_in_flight_ = false;
  commitRestoredLayout(progressive_layout_doc_, policy);
  progressive_layout_doc_.clear();
  progressive_previous_workspace_.reset();
  // After the commit: every waiter is cleared and any batch finished, so a subscriber quitting here cannot tear down
  // an active batch. The caller retires the batch right after.
  emit layoutRestoreSettled(true);
}

void MainWindow::commitRestoredLayout(const QDomDocument& doc, MissingCurvePolicy policy) {
  // A relaunched pinned toolbox becomes the shown tab, and Qt moves focus into its first field.
  const RestoreFocusGuard focus_guard(this);
  kept_derived_ = std::exchange(staged_kept_derived_, QDomDocument());
  // A kept busy panel leaves the live pinned toolboxes in place; the baseline resets either way.
  static_cast<void>(restorePinnedToolboxes(doc.documentElement(), policy));
  restoreLeftPanelState(doc);
  resetUndoHistory();
}

void MainWindow::snapshotStreamWindowAtLayoutOpen() {
  stream_window_at_layout_open_s_ = streaming_manager_->hasActiveSession()
                                        ? std::optional<int>(streaming_manager_->retentionSeconds())
                                        : std::nullopt;
}

void MainWindow::restoreLeftPanelState(const QDomDocument& doc) {
  const QDomElement state = doc.documentElement().firstChildElement(u"left_panel_state"_s);
  std::optional<int> buffer_s;
  bool ok = false;
  const int seconds = state.attribute(u"streaming_buffer"_s).toInt(&ok);
  if (ok && recordsStreams(doc)) {
    buffer_s = std::max(seconds, stream_window_at_layout_open_s_.value_or(seconds));
  }
  ui_->leftPanel->restoreSourcesState(state, buffer_s);
}

bool MainWindow::layoutImportBatchActive() const {
  return layout_import_batch_ != nullptr && layout_import_batch_->isActive();
}

void MainWindow::installLayoutBatchObservers() {
  // Events for activeImportDataset() display as kLayoutBatch; the delivery order guarantees the correlation exists
  // before that dataset's began reaches the GUI thread. Every other ingest takes the same arbitration; its own
  // host callbacks produce the same result, so the double handling is idempotent.
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
  // Before the batch dies, so no lifecycle event or strip Stop click reaches a dangling batch. Its SessionManager
  // entry may outlive this until its producer's teardown ends it, as a plain ingest.
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
  // A running batch takes its destructor's hard shutdown: children cancelled and joined, no rollback.
  teardownLayoutBatchObservation();
  layout_import_batch_.reset();
  batch_workspace_checkpoint_.reset();
  if (!progressive_layout_in_flight_) {
    // Only a kRetainAndDiagnose drain's retention channel can still be up. A restore in flight owns its own.
    QObject::disconnect(pending_items_added_conn_);
    pending_items_added_conn_ = {};
  }
}

void MainWindow::supersedeActiveRestore() {
  // On every new layout load and at close: a superseding load on the sync path never reaches
  // beginProgressiveLayoutRestore, and a stale waiter settled by the old load's late queueDrained would commit the
  // old document over the new workspace.
  if (progressive_layout_in_flight_) {
    cancelProgressiveLayoutRestore();
  }
  resetLayoutImportBatch();
}

bool MainWindow::restoreHasPendingAsyncWork(bool reload_requested) const {
  // A provider job is invisible to FileLoader's queue, so the batch counts on its own. Both halves need
  // reload_requested: only the do_reload branch supersedes before enqueuing, so only there do the loader and
  // layout_import_batch_ belong to this load; otherwise they may be a predecessor's, and this load takes the sync
  // apply.
  return reload_requested && (file_loader_->isBusy() || layoutImportBatchActive());
}

void MainWindow::maybeSettleProgressiveRestore() {
  if (!progressive_layout_in_flight_) {
    return;
  }
  // Each waiter's signal re-runs this check.
  for (const RestoreWaiter& waiter : restore_waiters_) {
    if (waiter.pending && waiter.pending()) {
      return;
    }
  }
  // The batch's own rollback already removed its datasets and restored the pre-layout workspace: no commit, no
  // second rollback.
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
  // Observers go before the batch. Not resetLayoutImportBatch(): a kRetainAndDiagnose drain's retention channel
  // (pending_items_added_conn_) must survive.
  teardownLayoutBatchObservation();
  layout_import_batch_.reset();
  batch_workspace_checkpoint_.reset();
}

std::unique_ptr<LayoutImportBatch> MainWindow::makeLayoutImportBatch(bool interactive) {
  LayoutImportBatch::Hooks hooks;
  hooks.begin_workspace_checkpoint = [this]() -> std::function<bool()> {
    // One capture serves the batch's cancel rollback and beginProgressiveLayoutRestore, which would otherwise
    // rebuild and serialize the whole DOM again. The batch cannot name the private CapturedWorkspace type.
    auto captured = std::make_shared<const CapturedWorkspace>(capturePortableWorkspace());
    batch_workspace_checkpoint_ = captured;
    return [this, captured]() {
      return restoreWorkspaceState(*captured, MissingCurvePolicy::kExact, TimelineRestoreMode::kExact) ==
             RestoreResult::kApplied;
    };
  };
  hooks.remove_dataset = [this](DatasetId dataset_id) {
    // A provider-reported id that never materialized, or is already gone, is a no-op.
    if (session_->catalogModel().datasetSourceName(dataset_id).has_value()) {
      removeDatasetData(dataset_id);
    }
  };
  if (interactive) {
    hooks.confirm_import = [this](const QStringList& lines) {
      // One prompt for the whole batch.
      constexpr int kTrust = 0;
      constexpr int kSkip = 1;
      QStringList shown;
      shown.reserve(lines.size());
      for (const QString& line : lines) {
        shown.push_back(u"  %1"_s.arg(line));
      }
      // No primary role, so no Enter default: an untrusted origin needs explicit confirmation, and Enter on the
      // preceding reload prompt would chain straight into granting a credentialed network fetch. Esc cancels.
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
  // Per-machine download ceiling; 0 = none. The batch honours any non-zero value (refuse or confirm at query time,
  // and each job's max_transfer_bytes).
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

void MainWindow::retireReplacedKeptDerived() {
  QDomElement kept = kept_derived_.documentElement();
  if (kept.firstChildElement().isNull() || applying_state_ || progressive_layout_in_flight_) {
    return;  // mid-restore, the live recipes may be rolled back
  }
  QDomDocument live_doc;
  const QDomElement live = derivedObjects().save(live_doc, SnapshotScope::kFull);
  // A filter or generator binding is the same recipe only on the same dataset: by path when both sides know it (a
  // live entry may carry only its session id), else by source name when the kept entry names one.
  const auto bound_dataset = [this](const QDomElement& entry, const QString& prefix) {
    QString path = entry.attribute(prefix + u"path"_s);
    bool ok = false;
    if (const std::uint32_t id = entry.attribute(prefix + u"id"_s).toUInt(&ok); path.isEmpty() && ok) {
      path = session_->sessionManager().datasetSourcePath(id);
    }
    return std::make_pair(path.isEmpty() ? path : QDir::cleanPath(path), entry.attribute(prefix + u"source"_s));
  };
  const auto replaces = [&](const QDomElement& live_entry, const QDomElement& kept_entry) {
    if (!sameRecipeName(live_entry, kept_entry)) {
      return false;
    }
    const QString prefix = boundDatasetPrefix(kept_entry);
    if (prefix.isEmpty()) {
      return true;
    }
    const auto [kept_path, kept_source] = bound_dataset(kept_entry, prefix);
    const auto [live_path, live_source] = bound_dataset(live_entry, prefix);
    if (!kept_path.isEmpty() && !live_path.isEmpty()) {
      return kept_path == live_path;
    }
    return kept_source.isEmpty() || kept_source == live_source;
  };
  for (QDomElement entry = kept.firstChildElement(); !entry.isNull();) {
    const QDomElement next = entry.nextSiblingElement();
    for (QDomElement live_entry = live.firstChildElement(); !live_entry.isNull();
         live_entry = live_entry.nextSiblingElement()) {
      if (replaces(live_entry, entry)) {
        kept.removeChild(entry);
        break;
      }
    }
    entry = next;
  }
}

void MainWindow::appendKeptDerived(QDomDocument& doc) {
  retireReplacedKeptDerived();
  const QDomElement kept = kept_derived_.documentElement();
  if (kept.firstChildElement().isNull()) {
    return;
  }
  QDomElement block = doc.documentElement().firstChildElement(u"data_processors"_s);
  if (block.isNull()) {
    block = doc.createElement(u"data_processors"_s);
    doc.documentElement().appendChild(block);
  }
  // An entry whose output a live recipe now writes would fail the next load ("output topic name already in use").
  const QSet<QString> transform_outputs = outputNames(block, u"transform"_s);
  const QSet<QString> on_demand_outputs = outputNames(block, u"on_demand"_s);
  for (QDomElement entry = kept.firstChildElement(); !entry.isNull(); entry = entry.nextSiblingElement()) {
    const QSet<QString>& taken = entry.tagName() == "transform"_L1 ? transform_outputs : on_demand_outputs;
    bool collides = false;
    for (QDomElement out = entry.firstChildElement(u"output"_s); !out.isNull() && !collides;
         out = out.nextSiblingElement(u"output"_s)) {
      collides = taken.contains(out.attribute(u"name"_s));
    }
    if (!collides) {
      block.appendChild(doc.importNode(entry, true));
    }
  }
}

void MainWindow::saveLayoutToPath(const QString& path, bool include_data_source) {
  QDomDocument doc = xmlSaveState();
  // Before the binding rewrites below, so a kept entry is qualified or relativized like a live one.
  appendKeptDerived(doc);
  doc.documentElement().setAttribute(u"binding"_s, include_data_source ? u"source"_s : u"generic"_s);
  const QDir layout_dir(QFileInfo(path).absoluteDir());
  if (include_data_source) {
    QDomElement ds = appendDataSourceElement(doc, layout_dir);
    if (!ds.isNull()) {
      doc.documentElement().appendChild(ds);
    }
    // Widget serializers know nothing of FileLoader, so the paths are stamped here, then made layout-relative.
    layout_xml::stampDatasetSourcePaths(doc, [this](std::uint32_t dataset_id) {
      const QString source_path = file_loader_->sourcePathForDataset(dataset_id);
      if (source_path.isEmpty()) {
        return QString{};
      }
      return QFileInfo(source_path).absoluteFilePath();
    });
    layout_xml::relativizeDatasetSourcePaths(
        doc, [&layout_dir](const QString& absolute_path) { return relocatableSubpath(absolute_path, layout_dir); });
    // A live stream is named by its record (pj-stream://<id>), with the configuration its session started from.
    QHash<std::uint32_t, QString> stream_identity_for_dataset;
    const QList<layout_xml::StreamingSourceRef> stream_records = liveStreamRecords(stream_identity_for_dataset);
    layout_xml::stampDatasetIdentities(doc, stream_identity_for_dataset);
    // A non-file dataset has no path to validate its numeric id: the id goes, so a duplicate basename cannot
    // capture the layout.
    layout_xml::removeUnvalidatedDatasetIds(doc);
    if (QString stream_error; !layout_xml::writeStreamingSources(doc, stream_records, &stream_error)) {
      MessageBox::warning(
          this, tr("Save Layout"), tr("The layout's streaming sources cannot be saved: %1").arg(stream_error));
      return;
    }
  } else {
    // Undo and source-bound documents keep the exact qualifiers; a generic file binds topic+field paths, when
    // unambiguous, to whatever is loaded.
    layout_xml::removeDatasetQualifiersForGenericLayout(doc);
  }
#ifdef PJ_WITH_SCENE3D
  // In both bindings; the live layer and undo snapshots keep the absolute path.
  pj::scene3d::relocateLayoutResources(
      doc, [&layout_dir](const QString& absolute_path) { return relocatableSubpath(absolute_path, layout_dir); });
#endif
  // UI state, saved in both bindings.
  doc.documentElement().appendChild(saveRightPanelState(doc));
  doc.documentElement().appendChild(ui_->leftPanel->saveSourcesState(doc, recordsStreams(doc)));
  doc.documentElement().appendChild(ui_->curveListPanel->saveListState(doc));
  doc.documentElement().appendChild(saveChromeState(doc));
  // <data_processors> comes from xmlSaveState(); appending it here would write two copies. Per-source offsets and
  // order ride <fileInfo>.
  doc.documentElement().appendChild(saveSourceTimelineViewState(doc));
  // Layout file only: undo snapshots exclude pinned toolbox tabs (TabbedPlotWidget::xmlSaveState).
  doc.documentElement().appendChild(savePinnedToolboxes(doc));
  // A partial write (disk full, broken NFS) leaves the previous layout untouched.
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

bool MainWindow::restoreDataProcessors(
    const QDomElement& root, RestoreIntent intent, QString* out_reason, MissingCurvePolicy policy,
    QStringList* skipped) {
  DerivedSnapshot snapshot = derivedObjects().parse(root, intent);
  if (!checkDataProcessorDependencies(snapshot, out_reason)) {
    return false;
  }
  return applyDataProcessors(std::move(snapshot), policy, skipped);
}

DerivedObjects MainWindow::derivedObjects() const {
  return DerivedObjects(session_->sessionManager(), session_->catalogModel());
}

void MainWindow::reportDerived(const std::vector<DerivedDiagnostic>& diagnostics) {
  for (const DerivedDiagnostic& diagnostic : diagnostics) {
    emitDiagnostic(DiagnosticLevel::kWarning, "Layout", diagnostic.id, diagnostic.message);
  }
}

bool MainWindow::checkDataProcessorDependencies(const DerivedSnapshot& snapshot, QString* out_reason) {
  if (out_reason != nullptr) {
    out_reason->clear();
  }
  const std::optional<QString> reason = derivedObjects().refusal(snapshot);
  if (!reason.has_value()) {
    return true;
  }
  if (out_reason != nullptr) {
    *out_reason = *reason;
  }
  emitDiagnostic(DiagnosticLevel::kWarning, "Layout", "history-exempt-blocked", *reason);
  return false;
}

bool MainWindow::applyDataProcessors(DerivedSnapshot snapshot, MissingCurvePolicy policy, QStringList* skipped) {
  const DerivedRestoreReport report = derivedObjects().restore(std::move(snapshot));
  reportDerived(report.diagnostics);
  // An unattended load keeps what it skipped, exactly those entries, for the next layout save (commitRestoredLayout).
  staged_kept_derived_ = QDomDocument();
  if (policy == MissingCurvePolicy::kRetainAndDiagnose && !report.skipped.empty()) {
    QHash<std::uint32_t, QString> stream_identity_for_dataset;
    static_cast<void>(liveStreamRecords(stream_identity_for_dataset));
    QDomElement block = staged_kept_derived_.createElement(u"data_processors"_s);
    staged_kept_derived_.appendChild(block);
    for (const QDomElement& entry : report.skipped) {
      QDomElement copy = staged_kept_derived_.importNode(entry, true).toElement();
      keepSessionFreeQualifiers(copy, stream_identity_for_dataset);
      block.appendChild(copy);
    }
  }
  // An empty snapshot (a progressive restore's teardown pass) empties the panel.
  syncCustomTopicsPanel();
  if (report.restored_all) {
    return true;
  }
  switch (policy) {
    case MissingCurvePolicy::kExact:
      return false;
    case MissingCurvePolicy::kPrompt:
      if (skipped != nullptr) {
        for (const DerivedDiagnostic& diagnostic : report.diagnostics) {
          skipped->push_back(diagnostic.message);
        }
      }
      break;
    case MissingCurvePolicy::kRetainAndDiagnose:
      showToast(
          tr("A derived series saved in the layout could not be restored and was skipped.") + u"<br>"_s +
          tr("Details are in the notifications."));
      break;
    case MissingCurvePolicy::kSilentDrop:
      break;
  }
  return true;
}

void MainWindow::syncCustomTopicsPanel() {
  custom_topic_transform_rows_stale_ = true;
  refreshCustomTopicsPanel();
}

void MainWindow::refreshCustomTopicsPanel() {
  custom_topics_sync_pending_ = false;
  ++custom_topics_syncs_;
  if (session_ == nullptr) {
    return;
  }
  const CatalogModel& catalog = session_->catalogModel();
  if (custom_topic_transform_rows_stale_) {
    custom_topic_transform_rows_stale_ = false;
    ++custom_topics_transform_scans_;
    custom_topic_transform_rows_.clear();
    // Not filtered by owner: a restored transform's plugin may not be loaded (the layout carries the script).
    const auto transforms = session_->sessionManager().dataProcessorService().transformRecipes();
    if (!transforms.empty()) {
      // First key wins for a repeated topic name, as a plotted curve resolves it.
      QHash<QString, QString> key_by_topic;
      for (const auto& item : catalog.items()) {
        if (!key_by_topic.contains(item.topic_name)) {
          key_by_topic.insert(item.topic_name, item.key);
        }
      }
      for (const auto& recipe : transforms) {
        for (const auto& output_name : recipe.outputs) {
          if (const auto found = key_by_topic.constFind(QString::fromStdString(output_name));
              found != key_by_topic.constEnd()) {
            custom_topic_transform_rows_.push_back(
                {.key = found.value(), .owner = QString::fromStdString(recipe.owner_plugin)});
          }
        }
      }
    }
  }
  std::vector<CurveListPanel::CustomTopicRow> rows;
  for (const TransformTopicRow& transform_row : custom_topic_transform_rows_) {
    rows.push_back({.key = transform_row.key, .owner = transform_row.owner});
  }
  // Persistent on-demand recipes; a preview is never a row. One row per object output, named by the recipe id, or
  // `id/output` when it has several.
  const OnDemandService& on_demand = session_->sessionManager().onDemandService();
  for (const DatasetId dataset : session_->sessionManager().dataEngine().listDatasets()) {
    for (const std::string& key : on_demand.graph().keysOf(dataset)) {
      const RecipeGraph::Node* node = on_demand.graph().find(key);
      if (node == nullptr || node->recipe.ephemeral) {
        continue;
      }
      const std::vector<ObjectTopicId> topics = on_demand.history().topicsOf(key);
      const std::optional<NodeScheduler::NodeState> state = on_demand.state(key);
      size_t object_output = 0;
      for (const scripting::OutputSpec& output : node->recipe.outputs) {
        if (output.type != scripting::OutputType::kObject || object_output >= topics.size()) {
          continue;
        }
        const std::optional<QString> catalog_key = catalog.objectTopicKey(dataset, topics[object_output++]);
        if (!catalog_key.has_value()) {
          continue;
        }
        QString name = QString::fromStdString(node->recipe.user_id);
        if (topics.size() > 1) {
          name += u'/' + QString::fromStdString(output.name);
        }
        rows.push_back(
            {.key = *catalog_key,
             .owner = QString::fromStdString(node->recipe.owner_plugin),
             .name = std::move(name),
             .pinned_t = node->recipe.pinned_t,
             .state = state});
      }
      // A series writes one engine topic with a column per number output, in declaration order: a flat row each,
      // always `id/output`, since a recipe with an object output already lists the bare id.
      if (const std::optional<SeriesRunner::State> series = on_demand.series().state(key)) {
        size_t column = 0;
        for (const scripting::OutputSpec& output : node->recipe.outputs) {
          if (output.type != scripting::OutputType::kNumber) {
            continue;
          }
          if (const std::optional<QString> catalog_key = catalog.scalarColumnKey(dataset, series->topic, column++)) {
            rows.push_back(
                {.key = *catalog_key,
                 .owner = QString::fromStdString(node->recipe.owner_plugin),
                 .name = QString::fromStdString(node->recipe.user_id) + u'/' + QString::fromStdString(output.name)});
          }
        }
      }
    }
  }
  ui_->curveListPanel->setCustomTopics(rows);
}

void MainWindow::scheduleCustomTopicsSync() {
  if (custom_topics_sync_pending_) {
    return;
  }
  custom_topics_sync_pending_ = true;
  QTimer::singleShot(0, this, [this]() { refreshCustomTopicsPanel(); });
}

void MainWindow::syncCustomTopicsEditor() {
  ui_->curveListPanel->setEditorPluginId(session_->extensionCatalog().customTopicsEditorId());
}

void MainWindow::reportDeleteRefusal(const CustomTopicDeleteRefusal& refusal) {
  using Reason = CustomTopicDeleteRefusal::Reason;
  const QString user_id = QString::fromStdString(refusal.user_id);
  const auto owner_name = [this, &refusal]() {
    return session_->extensionCatalog().toolboxDisplayName(QString::fromStdString(refusal.owner_plugin));
  };
  switch (refusal.reason) {
    case Reason::kGone:
      syncCustomTopicsPanel();  // the row outlived its recipe: clearing it is all there is to do
      return;
    case Reason::kReadOnly:
      emitDiagnostic(
          DiagnosticLevel::kWarning, "CustomTopics", "delete-read-only",
          refusal.owner_plugin.empty()
              ? tr("'%1' has no owner and cannot be deleted from here").arg(user_id)
              : tr("'%1' was created by %2 and cannot be deleted from here").arg(user_id, owner_name()));
      return;
    case Reason::kForeignReader:
      emitDiagnostic(
          DiagnosticLevel::kWarning, "CustomTopics", "delete-cascade-read-only",
          tr("'%1' cannot be deleted: '%2' of %3 reads its output")
              .arg(user_id, QString::fromStdString(refusal.reader), owner_name()));
      return;
    case Reason::kExemptReader:
      emitDiagnostic(
          DiagnosticLevel::kWarning, "CustomTopics", "delete-exempt-reader",
          tr("'%1' cannot be deleted: the protected '%2' reads its output")
              .arg(user_id, QString::fromStdString(refusal.reader)));
      return;
  }
}

void MainWindow::deleteCustomTopic(const QString& catalog_key) {
  deleteCustomTopics({catalog_key});
}

void MainWindow::deleteCustomTopics(const QStringList& catalog_keys) {
  // Resolved first: a delete relists the catalog, and a cancelled prompt must not come back for the recipe's next
  // row.
  std::vector<std::string> recipes;
  QStringList outputs;
  for (const QString& catalog_key : catalog_keys) {
    if (const RecipeGraph::Node* node = onDemandNodeOfCatalogKey(*session_, catalog_key)) {
      if (std::find(recipes.begin(), recipes.end(), node->recipe.key()) == recipes.end()) {
        recipes.push_back(node->recipe.key());
      }
    } else {
      outputs.push_back(catalog_key);
    }
  }
  for (const std::string& key : recipes) {
    deleteOnDemandRecipe(key);
  }
  for (const QString& output : outputs) {
    deleteTransformOutput(output);
  }
}

void MainWindow::deleteOnDemandRecipe(const std::string& key) {
  // Asked again once a prompt is answered: the editor may have been unloaded, or a reader installed, meanwhile.
  const auto refused = [this, key]() {
    const std::optional<CustomTopicDeleteRefusal> refusal = onDemandDeleteRefusal(
        session_->sessionManager().onDemandService().graph(), key, session_->extensionCatalog().customTopicsEditorId());
    if (refusal.has_value()) {
      reportDeleteRefusal(*refusal);
    }
    return refusal.has_value();
  };
  if (refused()) {
    return;
  }
  const auto remove = [this, key, refused]() {
    if (refused()) {
      return;
    }
    OnDemandService& on_demand = session_->sessionManager().onDemandService();
    // An ordinary recipe is an undo step, its baseline refreshed first: an install made directly on the service,
    // not through a plugin's pj.data_processors.v1, pushes no snapshot.
    const bool history_exempt = on_demand.graph().find(key)->recipe.history_exempt;
    if (!history_exempt) {
      hydrateCurrentUndoState(/*refresh_data_universe=*/false);
    }
    // The announcement relists the catalog, which drops the rows and any view bound to the topics.
    if (!on_demand.remove(key).empty()) {
      session_->sessionManager().notifyObjectTopicsChanged();
    }
    if (!history_exempt) {
      onUndoableChange(/*force_new_state=*/true);
    }
  };
  const std::vector<std::string> dependents = session_->sessionManager().onDemandService().graph().dependentsOf({key});
  if (dependents.empty()) {
    remove();
    return;
  }
  QStringList names;
  for (const std::string& dependent : dependents) {
    names << QString::fromStdString(dependent);
  }
  MessageBox::questionAsync(
      this, tr("Delete derived topic?"),
      tr("These derived topics read what you are deleting. They stay, but report a missing input:\n\n• %1")
          .arg(names.join(u"\n• "_s)),
      {{tr("Delete"), MessageBox::kDestructiveRole}, {tr("Cancel"), MessageBox::kCancelRole}},
      [remove](int choice, bool) {
        if (choice == 0) {
          remove();
        }
      });
}

bool MainWindow::mayDeleteTransformWriting(TopicId output_topic) {
  SessionManager& session_manager = session_->sessionManager();
  const std::optional<CustomTopicDeleteRefusal> refusal = transformDeleteRefusal(
      session_manager.dataProcessorService(), session_manager.onDemandService().graph(), session_manager.batchEngine(),
      output_topic, session_->extensionCatalog().customTopicsEditorId());
  if (refusal.has_value()) {
    reportDeleteRefusal(*refusal);
  }
  return !refusal.has_value();
}

void MainWindow::deleteTransformOutput(const QString& catalog_key) {
  const std::optional<CurveDescriptor> output = session_->catalogModel().curveDescriptor(catalog_key);
  if (!output.has_value()) {
    syncCustomTopicsPanel();  // the row outlived its catalog item: clearing it is all there is to do
    return;
  }
  const TopicId output_topic = output->topic_id;
  if (!mayDeleteTransformWriting(output_topic)) {
    return;
  }
  confirmAndRemoveDependentTransforms(
      {output_topic},
      [this, output_topic]() {
        // Looked up again: the confirmation may have removed transforms.
        DataProcessorService& live = session_->sessionManager().dataProcessorService();
        const auto transform = live.transformOwning(output_topic);
        if (!transform.has_value() || !live.removeTransform(transform->key).has_value()) {
          syncCustomTopicsPanel();
          return;
        }
        // itemsRemoved fires synchronously and drops the row.
        session_->catalogModel().rebuildFromDatastore();
      },
      // The editor may have been unloaded, or a foreign reader added, while the prompt was open.
      [this, output_topic]() { return mayDeleteTransformWriting(output_topic); });
}

void MainWindow::recordRecentLayout(const QString& path) {
  QStringList recent = recentLayouts();
  recent.removeAll(path);  // dedupe — most-recent-first
  recent.prepend(path);
  while (recent.size() > kMaxRecentEntries) {
    recent.removeLast();
  }
  QSettings().setValue(kRecentLayoutsKey, recent);
  // Even with no data file loaded: the popup has a Layouts section.
  ui_->leftPanel->setRecentEnabled(true);
}

QStringList MainWindow::recentLayouts() const {
  return QSettings().value(kRecentLayoutsKey).toStringList();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  const QEvent::Type type = event->type();
  // A hidden embedded scene view skips the per-tick cursor moves (tickEmbeddedViews);
  // revealed, it catches up to the playhead, which may since have stopped moving.
  if (type == QEvent::Show && !embedded_scene_views_.isEmpty()) {
    if (auto* scene = qobject_cast<SceneDockWidget*>(watched);
        scene != nullptr && embedded_scene_views_.contains(scene)) {
      scene->onTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
    }
  }
  // Qt's internal tooltip widget, reused for every tooltip: one Polish covers them all. KWin and Mutter would draw
  // a drop shadow around it.
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
  if (!custom_frame_ || (type != QEvent::MouseMove && type != QEvent::MouseButtonPress)) {
    return false;
  }
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
  return layout_xml::rebindCurveKeys(
      doc, [this](const layout_xml::SeriesPath& p) { return session_->catalogModel().resolveCurveKey(p); });
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

  const QHash<DatasetId, int> order_slots = timelineSlots();
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

MainWindow::CapturedWorkspace MainWindow::captureWorkspace(SnapshotScope scope) const {
  return CapturedWorkspace{.xml = xmlSaveState(scope).toByteArray(2), .timeline = captureTimelineState()};
}

MainWindow::CapturedWorkspace MainWindow::captureWorkspace() const {
  return captureWorkspace(SnapshotScope::kFull);
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

bool MainWindow::applyTimelineOffsets(const TimelineState& state, const TimelineResolutionPlan& plan) {
  if (plan.size() != state.tracks.size()) {
    return false;
  }
  for (std::size_t index = 0; index < state.tracks.size(); ++index) {
    const TimelineTrackState& track = state.tracks[index];
    const DatasetId dataset_id = plan[index];
    session_->sessionManager().setDisplayOffset(dataset_id, DisplayOffset{Duration{track.display_offset_ns}});
  }
  return true;
}

bool MainWindow::applyTimelineState(const TimelineState& state, const TimelineResolutionPlan& plan) {
  if (plan.size() != state.tracks.size()) {
    return false;
  }

  std::vector<std::pair<int, DatasetId>> ordered;
  for (std::size_t index = 0; index < state.tracks.size(); ++index) {
    const TimelineTrackState& track = state.tracks[index];
    const DatasetId dataset_id = plan[index];
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
    const TimelineResolutionPlan* timeline_plan, RestoreIntent intent, QString* out_reason) {
  const QDomElement root = doc.documentElement();
  // Order: preflight, display offsets, processors, curves. A generator reading the display frame (all_datasets
  // rules) must see the settled offset; a preflight after any mutation could no longer refuse cleanly.
  // 1. Read-only: refuse if dropping this snapshot's processors would strand an exempt transform or generator.
  DerivedSnapshot processors = derivedObjects().parse(root, intent);
  if (!checkDataProcessorDependencies(processors, out_reason)) {
    return RestoreResult::kFailed;
  }
  // 2. Display offsets.
  if (timeline_state != nullptr &&
      (timeline_plan == nullptr || !applyTimelineOffsets(*timeline_state, *timeline_plan))) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "layout-timeline-failed",
        tr("The saved per-source time offsets could not be applied."));
    return RestoreResult::kFailed;
  }
  // 3. Processors, so derived output topics are in the catalog before the rebind.
  QStringList skipped_derived;
  if (!applyDataProcessors(std::move(processors), policy, &skipped_derived)) {
    return RestoreResult::kFailed;
  }
  // 4. Curve rebind.
  const QList<layout_xml::SeriesPath> unresolved = rebindCurvesToLoadedDatasets(doc);
  if (policy == MissingCurvePolicy::kExact && !unresolved.isEmpty()) {
    QStringList names;
    for (const layout_xml::SeriesPath& sp : unresolved) {
      names.push_back(sp.display());
    }
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "layout-curves-unbound",
        tr("%n curve(s) have no matching data: %1", nullptr, static_cast<int>(names.size())).arg(names.join(u", "_s)));
    return RestoreResult::kFailed;
  }
  if (policy == MissingCurvePolicy::kPrompt && (!unresolved.isEmpty() || !skipped_derived.isEmpty())) {
    QStringList shown;
    shown.reserve(unresolved.size());
    for (const layout_xml::SeriesPath& sp : unresolved) {
      shown.push_back(sp.display());
    }
    switch (promptMissingCurves(shown, {}, skipped_derived)) {
      case MissingCurveChoice::kCancel:
        return RestoreResult::kCancelled;
      case MissingCurveChoice::kRemove:
        layout_xml::stripUnresolvedCurves(doc);
        break;
    }
  }
  if (policy == MissingCurvePolicy::kRetainAndDiagnose && !unresolved.isEmpty()) {
    // Never a dialog. The curves stay in the document and rebuildPendingDisplayBindings below retains them for later
    // binding. The id differs from the drain-time one.
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
  // kSilentDrop leaves unresolved curves for xmlLoadState to discard.
  if (!xmlLoadState(doc, intent)) {
    return RestoreResult::kFailed;
  }
  // Track order and view chrome only: the offsets moved in step 2.
  if (timeline_state != nullptr && (timeline_plan == nullptr || !applyTimelineState(*timeline_state, *timeline_plan))) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "layout-timeline-failed",
        tr("The saved per-source time offsets could not be applied."));
    return RestoreResult::kFailed;
  }
  rebuildPendingDisplayBindings(doc);
  // No drain follows a blocking load or a history replay: a rejected scene element, or an unresolved blocking one
  // in an exact snapshot, fails the transaction rather than committing a partial scene.
  const SceneRestoreVerdict scenes = settleSceneRestores();
  if (scenes.failed || (policy == MissingCurvePolicy::kExact && !scenes.blocking_topics.isEmpty())) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "layout-scene-failed",
        scenes.failed ? tr("A 2D/3D scene could not restore its layers (the element is named in the log).")
                      : tr("Scene layers have no matching data: %1").arg(scenes.blocking_topics.join(u", "_s)));
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
  // A history restore rebuilds only the non-exempt dockers; an exempt one was
  // preserved live and has no saved viewport, so re-framing it would zoom it out.
  ui_->tabbedPlotWidget->forEachDocker([intent](PlotDocker* docker) {
    if (intent == RestoreIntent::kHistory && docker->isHistoryExempt()) {
      return;
    }
    for (int index = 0; index < docker->plotCount(); ++index) {
      DockWidget* dock = docker->plotAt(index);
      if (PlotWidget* plot = dock != nullptr ? dock->plotWidget() : nullptr) {
        plot->applySavedViewportOrZoom(/*clear_after=*/true);
      }
    }
  });
  // currentTimeChanged fires only on a change: without this seed, restored scene docks render blank until the next
  // scrub.
  broadcastTrackerTime(toAxisDouble(session_->playbackEngine().currentTime()));
  // dockFocused fires during a rebuild only when a kept plot gets the keyboard focus back, not for new docks: without
  // this a scene-only layout would show curve options in the right panel.
  onDockFocused(activeFocusedDock());
  return RestoreResult::kApplied;
}

MainWindow::RestoreResult MainWindow::restoreWorkspaceStateImpl(
    QDomDocument& doc, MissingCurvePolicy policy, const TimelineState* timeline_state,
    TimelineRestoreMode timeline_mode, const CapturedWorkspace* rollback_to, RestoreIntent intent,
    QString* out_reason) {
  std::optional<TimelineResolutionPlan> timeline_plan;
  if (timeline_state != nullptr) {
    timeline_plan = validateTimelineState(*timeline_state, timeline_mode);
    if (!timeline_plan.has_value()) {
      return RestoreResult::kFailed;
    }
  }

  QScopedValueRollback applying_guard(applying_state_, true);
  const RestoreFocusGuard focus_guard(this);
  const CapturedWorkspace previous =
      rollback_to != nullptr
          ? *rollback_to
          : captureWorkspace(intent == RestoreIntent::kHistory ? SnapshotScope::kHistory : SnapshotScope::kFull);
  QString internal_reason;
  QString* reason = out_reason != nullptr ? out_reason : &internal_reason;
  const RestoreResult result = applyWorkspace(
      doc, policy, timeline_state, timeline_plan.has_value() ? &*timeline_plan : nullptr, intent, reason);
  if (result == RestoreResult::kApplied) {
    return result;
  }
  // A preflight refusal changed nothing: a rollback would only repeat it.
  if (!reason->isEmpty()) {
    return result;
  }

  QDomDocument previous_doc;
  if (previous_doc.setContent(previous.xml)) {
    const std::optional<TimelineResolutionPlan> previous_plan =
        validateTimelineState(previous.timeline, TimelineRestoreMode::kExact);
    if (previous_plan.has_value()) {
      // The same intent as the failed attempt: a kHistory rollback leaves the exempt processors untouched.
      static_cast<void>(
          applyWorkspace(previous_doc, MissingCurvePolicy::kSilentDrop, &previous.timeline, &*previous_plan, intent));
    }
  }
  return result;
}

bool MainWindow::restoreRefuses(const QDomDocument& doc) const {
  const DerivedObjects derived = derivedObjects();
  return derived.refusal(derived.parse(doc.documentElement(), RestoreIntent::kReplace)).has_value();
}

MainWindow::RestoreResult MainWindow::restoreWorkspaceState(
    QDomDocument& doc, MissingCurvePolicy policy, const CapturedWorkspace* rollback_to, RestoreIntent intent,
    QString* out_reason) {
  return restoreWorkspaceStateImpl(doc, policy, nullptr, TimelineRestoreMode::kExact, rollback_to, intent, out_reason);
}

MainWindow::RestoreResult MainWindow::restoreWorkspaceState(
    const CapturedWorkspace& target, MissingCurvePolicy policy, TimelineRestoreMode timeline_mode,
    const CapturedWorkspace* rollback_to, RestoreIntent intent, QString* out_reason) {
  QDomDocument doc;
  if (!doc.setContent(target.xml)) {
    return RestoreResult::kFailed;
  }
  return restoreWorkspaceStateImpl(doc, policy, &target.timeline, timeline_mode, rollback_to, intent, out_reason);
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
  // At the target's scope, so a rollback also leaves every exempt processor and tab untouched.
  const CapturedWorkspace current_state = captureWorkspace(SnapshotScope::kHistory);
  QString reason;
  const bool loaded = restoreWorkspaceState(
                          target, MissingCurvePolicy::kExact, TimelineRestoreMode::kExact, &current_state,
                          RestoreIntent::kHistory, &reason) == RestoreResult::kApplied;
  if (!loaded) {
    statusBar()->showMessage(
        !reason.isEmpty() ? reason : (undo ? tr("Unable to restore undo state") : tr("Unable to restore redo state")),
        3000);
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
  if (sources.empty() || session_->catalogModel().isEmpty()) {
    return QDomElement();
  }

  // Only sources that still back a live dataset, so a removed one is not resurrected on reload (the quick-reload
  // button keys off lastLoadedSource, not this list). One file may fan out into several datasets, each with its own
  // timeline track.
  const std::vector<std::pair<DatasetId, QString>> live_datasets = session_->catalogModel().datasets();

  // Absent from the map: load order on restore.
  const QHash<DatasetId, int> timeline_order = timelineSlots();

  QDomElement wrapper = doc.createElement(u"previouslyLoaded_Datafiles"_s);

  // One <fileInfo> per file, in load order; a reader of only the first child degrades to the first file.
  for (const auto& src : sources) {
    // source_index is the position here: two fan-out members with the same name must not swap offsets on reload.
    std::vector<DatasetId> datasets_for_file;
    for (const auto& [id, name] : live_datasets) {
      (void)name;
      const QString source_path = file_loader_->sourcePathForDataset(id);
      if (!source_path.isEmpty() && layout_xml::isSamePath(source_path, src.path)) {
        datasets_for_file.push_back(id);
      }
    }
    if (datasets_for_file.empty()) {
      continue;
    }
    QDomElement file_info = doc.createElement(u"fileInfo"_s);

    const QString abs = QFileInfo(src.path).absoluteFilePath();
    file_info.setAttribute(u"filename"_s, relocatableSubpath(abs, layout_dir));
    file_info.setAttribute(u"prefix"_s, src.prefix);

    // The offset is sourceDisplayOffset(), without the global reference (schema v4). The first child is mirrored
    // onto <fileInfo> for v3 and older readers.
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

    // Even with an empty config: a plugin with no settings would otherwise re-prompt on every reload.
    if (!src.plugin_id.isEmpty()) {
      QDomElement plugin = doc.createElement(u"plugin"_s);
      plugin.setAttribute(u"ID"_s, src.plugin_id);
      // Both identities: old readers match the display name in ID; the manifest id survives a rename.
      if (!src.plugin_manifest_id.isEmpty()) {
        plugin.setAttribute(u"manifest_id"_s, src.plugin_manifest_id);
      }
      // Older 4.x builds rewrite the preset's filepath only on this marker.
      plugin.setAttribute(u"filepath_mode"_s, u"source"_s);
      // Split across CDATA sections around a literal "]]>".
      appendJsonAsCdata(doc, plugin, src.plugin_config_json);
      file_info.appendChild(plugin);
    }

    // Provider provenance as a <materialize> sibling of <plugin>, at most one per fileInfo, primary dataset first.
    // The descriptor JSON is written verbatim: its bytes are a cross-repo identity contract, never re-serialized.
    for (const DatasetId id : datasets_for_file) {
      const SourceRecord* record = session_->sessionManager().sourceRecord(id);
      if (record == nullptr || record->provider_id.isEmpty()) {
        continue;  // no provenance, or unroutable without a provider id
      }
      QDomElement materialize = doc.createElement(u"materialize"_s);
      materialize.setAttribute(u"provider"_s, record->provider_id);
      materialize.setAttribute(u"identity"_s, record->source_identity);
      appendJsonAsCdata(doc, materialize, record->descriptor_json);
      file_info.appendChild(materialize);
      break;
    }

    wrapper.appendChild(file_info);
  }
  if (!wrapper.hasChildNodes()) {
    return QDomElement();
  }
  return wrapper;
}

MainWindow::LayoutTimelinePlan MainWindow::resolveLayoutTimelineTracks(
    const QList<layout_xml::DataSourceRef>& sources, const LayoutTimelinePlan& streams) const {
  LayoutTimelinePlan plan;
  const SessionManager& mgr = session_->sessionManager();
  // DatasetIds are minted per session: path + (source_name, source_index) is the stable identity.
  QSet<DatasetId> matched_ids;
  const auto add_track = [this, &mgr, &plan, &matched_ids](
                             DatasetId matched, qint64 offset_ns, bool has_offset, bool includes_global_reference,
                             int order) {
    // Several aliases can resolve to one dataset: the first wins, never two offsets or two slots.
    if (matched_ids.contains(matched)) {
      return;
    }
    matched_ids.insert(matched);
    if (has_offset) {
      // v3 wrote displayOffset(), which includes the global reference; v4 writes sourceDisplayOffset().
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
    }
    plan.push_back(LayoutTimelineTrack{matched, offset_ns, has_offset, order});
  };

  // datasets() copies the catalog list on each call.
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
      continue;  // not loaded
    }

    if (ref.datasets.isEmpty()) {
      // v3 and older: one state on <fileInfo>, applied to the file's first dataset.
      add_track(
          candidates.front(), ref.display_offset_ns, ref.has_display_offset,
          ref.display_offset_includes_global_reference, ref.timeline_order);
      continue;
    }

    const std::vector<std::uint32_t> matches = layout_xml::matchFanoutDatasets(
        ref.datasets, std::vector<std::uint32_t>(candidates.begin(), candidates.end()), [this](std::uint32_t id) {
          return session_->catalogModel().datasetSourceName(static_cast<DatasetId>(id)).value_or(QString{});
        });
    for (qsizetype child_index = 0; child_index < ref.datasets.size(); ++child_index) {
      const DatasetId matched = static_cast<DatasetId>(matches[static_cast<std::size_t>(child_index)]);
      if (matched == 0) {
        continue;  // missing or ambiguous: never shift a sibling
      }
      const layout_xml::DataSourceDatasetRef& saved = ref.datasets[child_index];
      add_track(
          matched, saved.display_offset_ns, saved.has_display_offset, saved.display_offset_includes_global_reference,
          saved.timeline_order);
    }
  }

  for (const LayoutTimelineTrack& stream : streams) {
    add_track(
        stream.dataset_id, stream.display_offset_ns, stream.has_display_offset,
        /*includes_global_reference=*/false, stream.timeline_order);
  }
  return plan;
}

bool MainWindow::applyLayoutTimelineOffsets(const LayoutTimelinePlan& plan) {
  bool changed = false;
  SessionManager& mgr = session_->sessionManager();
  for (const LayoutTimelineTrack& track : plan) {
    if (!track.has_display_offset) {
      continue;
    }
    changed |= mgr.sourceDisplayOffset(track.dataset_id).value.count() != track.display_offset_ns;
    mgr.setDisplayOffset(track.dataset_id, DisplayOffset{Duration{track.display_offset_ns}});
  }
  return changed;
}

void MainWindow::applyLayoutTimelineOrder(const LayoutTimelinePlan& plan) {
  if (source_timeline_controller_ == nullptr) {
    return;
  }
  std::vector<std::pair<int, DatasetId>> ordered;
  for (const LayoutTimelineTrack& track : plan) {
    if (track.timeline_order >= 0) {
      ordered.emplace_back(track.timeline_order, track.dataset_id);
    }
  }
  if (ordered.empty()) {
    return;
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<DatasetId> order;
  order.reserve(ordered.size());
  for (const auto& [slot, id] : ordered) {
    (void)slot;
    order.push_back(id);
  }
  source_timeline_controller_->setDisplayOrder(std::move(order));
}

bool MainWindow::applyTimelineStateFromLayout(const QList<layout_xml::DataSourceRef>& sources) {
  const LayoutTimelinePlan plan = resolveLayoutTimelineTracks(sources, progressive_stream_tracks_);
  const bool offset_changed = applyLayoutTimelineOffsets(plan);
  applyLayoutTimelineOrder(plan);
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

  // The panel's open/closed state is a QSettings preference, not document state.

  // The value, not the button index, which depends on declaration order.
  if (width_button_group_ != nullptr) {
    const int id = width_button_group_->checkedId();
    if (id >= 0 && id < static_cast<int>(kWidthButtonSpecs.size())) {
      element.setAttribute(u"width"_s, QString::number(kWidthButtonSpecs[id].second, 'g'));
    }
  }

  // The button id is the CurveStyle value.
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
  // The playback bar is fixed-height, so its minimum is its height.
  const int playback_height = ui_->timelineWidget->minimumHeight();
  // Not isVisible(): before the first show it is false for an open strip, which would pin the panel to the playback
  // bar.
  const bool strip_visible = !ui_->timelineStrip->isHidden();
  if (ui_->timelineAlignRail != nullptr) {
    ui_->timelineAlignRail->setVisible(strip_visible);
  }
  if (ui_->timelineAlignBorder != nullptr) {
    ui_->timelineAlignBorder->setVisible(strip_visible);
  }
  if (ui_->timelinePlaybackBorder != nullptr) {
    ui_->timelinePlaybackBorder->setVisible(strip_visible);
  }
  if (strip_visible) {
    const int min_panel = playback_height + kMinTimelineStripHeight;
    ui_->bottomPanel->setMaximumHeight(QWIDGETSIZE_MAX);
    ui_->bottomPanel->setMinimumHeight(min_panel);
    // QSplitter caches slot sizes and does not re-honour a new minimum by itself.
    const QList<int> sizes = ui_->timelineSplitter->sizes();
    if (sizes.size() == 2 && sizes[1] < min_panel) {
      ui_->timelineSplitter->setSizes({(sizes[0] + sizes[1]) - min_panel, min_panel});
    }
  } else {
    // min == max: the splitter handle cannot drag the strip open, only the toggle can.
    ui_->bottomPanel->setMinimumHeight(playback_height);
    ui_->bottomPanel->setMaximumHeight(playback_height);
    // QSplitter caches sizes and does not shrink the slot from the maximum alone.
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
  // The groove has margin 0 (QSS), so the slider's x is where the track starts, and the name column shares the
  // playback bar's left origin.
  auto* slider = ui_->timelineWidget->findChild<QWidget*>(u"timeSlider"_s);
  if (slider == nullptr) {
    return;
  }
  const int slider_x = slider->mapTo(ui_->timelineWidget, QPoint(0, 0)).x();
  if (slider_x <= 0) {
    return;  // not laid out before the first show
  }
  // slider_x is the floor; a wider chosen width is kept.
  source_timeline_->setNameColumnWidth(slider_x);
  if (timeline_name_column_width_ > slider_x) {
    source_timeline_->resizeNameColumn(timeline_name_column_width_);
  }
}

void MainWindow::restoreRightPanelState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "right_panel_state"_L1) {
    return;
  }

  // checkGroupButton blocks the group's signals: a passive resync must not refresh the previews.
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

  if (element.hasAttribute(u"style"_s) && style_button_group_ != nullptr) {
    bool ok = false;
    const int wanted = element.attribute(u"style"_s).toInt(&ok);
    if (ok) {
      checkGroupButton(style_button_group_, wanted);
    }
  }

  // A count mismatch means the splitter changed shape between versions: skipped.
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

  // Only the main splitter: panel visibility and the timeline-strip height are launch state.
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

  // A count mismatch is a silent no-op.
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

QStringList MainWindow::layoutImportFailureLines() const {
  QStringList lines;
  if (layout_import_batch_ != nullptr) {
    for (const LayoutImportBatch::SourceResult& result : layout_import_batch_->result().sources) {
      if (result.outcome == LayoutImportBatch::SourceOutcome::kFailed) {
        lines.push_back(u"%1: %2"_s.arg(result.effective_path, result.message));
      }
    }
  }
  return lines;
}

MainWindow::MissingCurveChoice MainWindow::promptMissingCurves(
    const QStringList& names, const QStringList& import_failures, const QStringList& skipped_derived) {
  static constexpr int kMaxShown = 10;
  const int name_count = static_cast<int>(names.size());
  QString body;
  if (name_count > 0) {
    body += tr("The layout references %n curve(s) not present in the current data:", "", name_count);
    body += u"\n\n"_s;
    const int shown = std::min(name_count, kMaxShown);
    for (int i = 0; i < shown; ++i) {
      body += u"  • "_s + names[i] + u"\n"_s;
    }
    if (name_count > kMaxShown) {
      body += tr("  … and %n more\n", "", name_count - kMaxShown);
    }
  }
  if (!skipped_derived.isEmpty()) {
    if (!body.isEmpty()) {
      body += u"\n"_s;
    }
    body += tr("These derived objects of the layout cannot be restored over the current data and are skipped:");
    body += u"\n\n"_s;
    for (const QString& line : skipped_derived) {
      body += u"  • "_s + line + u"\n"_s;
    }
  }
  if (!import_failures.isEmpty()) {
    body += u"\n"_s;
    body += tr("The layout's data sources could not be restored:");
    body += u"\n\n"_s;
    for (const QString& line : import_failures) {
      body += u"  "_s + line + u"\n"_s;
    }
  }
  body += u"\n"_s;
  body += tr("Choose how to handle them:");

  // No default button: Remove drops every missing curve from every plot (and the skipped derived objects from the
  // workspace and its next save), and an accidental Enter must not trigger it.
  const bool curves_only = skipped_derived.isEmpty();
  constexpr int kRemove = 0;
  const int choice = MessageBox::question(
      this, curves_only ? tr("Missing curves") : tr("Missing layout data"), body,
      {{curves_only ? tr("Remove from plots") : tr("Continue without them"), MessageBox::kDestructiveRole},
       {tr("Cancel"), MessageBox::kCancelRole}});
  return choice == kRemove ? MissingCurveChoice::kRemove : MissingCurveChoice::kCancel;
}

QDomDocument MainWindow::xmlSaveState(SnapshotScope scope) const {
  QDomDocument doc;
  doc.appendChild(doc.createProcessingInstruction(u"xml"_s, u"version='1.0' encoding='UTF-8'"_s));

  QDomElement root = doc.createElement(u"root"_s);
  root.setAttribute(u"format"_s, u"PlotJuggler"_s);
  root.setAttribute(u"pj4_version"_s, QString::number(kLayoutSchemaVersion));
  doc.appendChild(root);

  root.appendChild(ui_->tabbedPlotWidget->xmlSaveState(doc, scope));

  // The global toolbar toggles (link X, legend, grid, "Use time offset", ...) are QSettings preferences, not
  // document state. That is why a plot's <range> can store absolute time: it frames the right instant whatever the
  // toggle says at load.

  // Processors are part of the undo snapshot: undoing across a filter's creation would otherwise drop its derived
  // curve. This serializer is the only writer, layout files included.
  root.appendChild(derivedObjects().save(doc, scope));
  return doc;
}

QDomDocument MainWindow::xmlSaveState() const {
  return xmlSaveState(SnapshotScope::kFull);
}

bool MainWindow::xmlLoadState(const QDomDocument& state_document, RestoreIntent intent) {
  const QDomElement root = state_document.documentElement();
  if (root.isNull() || root.tagName() != "root"_L1) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "layout-xml-invalid", tr("No <root> element at the top of the layout."));
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
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "layout-xml-invalid", tr("No <tabbed_widget> element in the layout."));
    return false;
  }

  const bool loaded = ui_->tabbedPlotWidget->xmlLoadState(main_tabbed_widget, intent);
  if (!loaded) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "Layout", "layout-plots-invalid",
        tr("The plot tabs could not be reconstructed from the layout."));
    return false;
  }
  plugin_plot_tabs_->refreshAvailability();
  wireExistingPlots();

  // The global toggles are QSettings preferences, not in the document (xmlSaveState): restored plots adopt the
  // current ones, except Dots, whose effect is the plot's saved style.
  forEachPlot([this](PlotWidget* plot) { applyGlobalToggles(plot); });
  applyShowPointsTo2DWidgets();
  return true;
}

void MainWindow::pushInitialUndoState() {
  undo_states_.clear();
  redo_states_.clear();
  undo_states_.push_back(captureWorkspace(SnapshotScope::kHistory));
  history_data_universe_ = captureHistoryDataUniverse();
  undo_timer_.invalidate();
  updateUndoRedoActions();
}

void MainWindow::resetUndoHistory() {
  // After a removal the synchronous catalog subscriptions have already pruned the widgets, so the snapshot is clean.
  pushInitialUndoState();
}

void MainWindow::pushUndoState(bool force_new_state) {
  const CapturedWorkspace state = captureWorkspace(SnapshotScope::kHistory);
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
    // The next ordinary edit must not coalesce into a forced one.
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
  undo_states_.back() = captureWorkspace(SnapshotScope::kHistory);
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
    // A restore keeps a plot and re-binds it: setPlot ignores the plot it already holds, so refresh the rows, which the
    // restore may have changed without a curve-list signal (colour, visibility, markers).
    if (plot != nullptr && curve_editor_->plot() == plot) {
      curve_editor_->refresh();
    } else {
      curve_editor_->setPlot(plot);
    }
  }
  const bool enable = plot != nullptr;
  // The group ids are the LineWidth index and the CurveStyle value; every bind resyncs them.
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
  ui_->curveListPanel->scheduleDisplayedKeysRefresh();

  // Placeholder docks and unknown widget kinds get the empty page.
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
        // The strip reuses the plot page's curves table; width and style are hidden below.
        target = plot_config_page_;
        strip_controller = strip->controller();
      }
    }
  }
  if (curve_editor_ != nullptr) {
    curve_editor_->setStateTransitions(strip_controller);  // null releases the binding
  }
  // Everything in the plot page except the CurveEditor is a width or style section.
  if (plot_config_page_ != nullptr && plot_config_page_->layout() != nullptr) {
    QLayout* page_layout = plot_config_page_->layout();
    for (int index = 0; index < page_layout->count(); ++index) {
      QWidget* section = page_layout->itemAt(index)->widget();
      if (section != nullptr && section != curve_editor_) {
        section->setVisible(strip_controller == nullptr);
      }
    }
  }
  // Before switching the stack, so the page is populated when it shows; nullptr unbinds.
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
  if (custom_widgets_ != nullptr) {
    if (QWidget* page = custom_widgets_->pageForFocusedDock(dock)) {
      target = page;
    }
  }
  if (right_panel_stack_ != nullptr) {
    right_panel_stack_->setCurrentWidget(target);
  }
}

void MainWindow::buildGlobalToolbar() {
  // Visible whatever the right panel's toggle says.
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

  // Global view toggles, persisted to QSettings; applying_state_ no-ops them during a restore.
  button_link_ = add_button("buttonLink", ":/resources/svg/link.svg", "Link X axis");
  button_zoom_out_ = add_button("buttonZoomOut", ":/resources/svg/zoom_max.svg", "Zoom Out All");
  button_grid_ = add_button("buttonActivateGrid", ":/resources/svg/grid.svg", "Show/Hide the grid");
  button_time_tracker_ = add_button(
      "buttonTimeTracker", ":/resources/svg/line_tracker.svg",
      "Cycle TimeTracker display: line only / line + value / line + value + name");
  updateTimeTrackerIcon();
  connect(button_time_tracker_, &QToolButton::clicked, this, &MainWindow::onTimeTrackerButtonClicked);

  // Left-click shows a hidden legend at the saved corner, else walks TR → TL → BL → BR and hides after BR
  // (onLegendButtonClicked). Right-click toggles at the current corner. Unchecked, the icon shows the saved corner.
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
  // customContextMenuRequested is only the right-click hook; no menu is shown.
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
  // Session-only. Stores the absolute instant, so a display-frame change re-projects it.
  connect(button_reference_point_, &QToolButton::toggled, this, [this](bool checked) {
    if (applying_state_) {
      return;
    }
    reference_instant_ = checked ? std::optional<Timepoint>{toAbsolute(
                                       session_->playbackEngine().currentTime(),
                                       session_->sessionManager().displayOffset(representativeDatasetId()))}
                                 : std::nullopt;
    broadcastReferenceLine();
  });

  outer->addStretch(1);

  // The padding goes into the layout margins too, so the buttons absorb it instead of clipping.
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
  // Continues the global toolbar above it when the right panel is closed; shown with the strip
  // (applyBottomPanelConstraints).
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

  QToolButton* zoom_fit =
      add_button("buttonTimelineFit", ":/resources/svg/zoom_horizontal.svg", "Zoom out to fit all sources");

  QToolButton* snap_toggle = add_button(
      "buttonTimelineSnap", ":/resources/svg/transition_push.svg", "Snap to neighbouring dataset edges while dragging");
  snap_toggle->setCheckable(true);
  snap_toggle->setChecked(true);

  // The reset button sits at the bottom of the rail.
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

  // The global toolbar's sizing, so the two rails line up.
  connect(this, &MainWindow::chromeMetricsChanged, ui_->timelineAlignRail, [this](const ChromeMetrics& metrics) {
    const int button_extent = metrics.icon_size + metrics.icon_padding;
    const int column_width = button_extent + (2 * metrics.layout_padding);
    ui_->timelineAlignRail->setMinimumWidth(column_width);
    ui_->timelineAlignRail->setMaximumWidth(column_width);
    if (auto* layout = ui_->timelineAlignRail->layout()) {
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
  // The plot-config page: Curve Width and Curve Style sections; the caller adds the CurveEditor.
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
    // The objectName is a widget-tree selector.
    auto* header = new SectionHeaderBand(heading, plot_config_page_);
    header->setObjectName(header_object_name);
    header->onChromeMetricsChanged(chrome_metrics_);
    connect(this, &MainWindow::chromeMetricsChanged, header, &SectionHeaderBand::onChromeMetricsChanged);
    outer->addWidget(header);

    // Height-for-width lets the strip grow when it wraps; without it the parent asks for one row and the wrapped
    // rows are clipped.
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

  // The id is the LineWidth index; bindEditorToPlot resyncs the checked button to the active plot.
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
  // The per-button slot already applied it to the active plot.
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

  // The id is the CurveStyle value; bindEditorToPlot resyncs the checked button to the active plot.
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
  // The per-button slot already applied it to the active plot.
  connect(style_button_group_, &QButtonGroup::idClicked, this, [this](int /*style_value*/) {
    syncFilterEditorPreviewDisplay();
    syncPanelPreviewDisplay();
  });

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
    // The outer layout keeps zero margins: the bands already pad, and padding both would double it.
  });
}

void MainWindow::applyActivePlotWidth(double width) {
  PlotWidget* plot = curve_editor_ != nullptr ? curve_editor_->plot() : nullptr;
  if (plot == nullptr) {
    return;
  }
  // Plot-level: later curves inherit it. The kWidthButtonSpecs index is the LineWidth value.
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
  // Plot-level: later curves inherit it.
  plot->setDefaultStyle(static_cast<PlotWidgetBase::CurveStyle>(style));
  onUndoableChange();
}

MainWindow::WrappedToolboxPanel MainWindow::wrapToolboxPanel(
    QWidget* content, const QString& title, ToolboxChromeHooks hooks, const QString& persist_key) {
  const std::function<void()>& on_close = hooks.on_close;
  const std::function<void()>& on_migrate = hooks.on_migrate;
  const std::function<void()>& on_float = hooks.on_float;
  const std::function<void()>& on_dock = hooks.on_dock;
  auto* container = new QWidget;
  container->setObjectName(QStringLiteral("toolboxPanelContainer"));
  // [side drawer][body: banner over content], splitter panes when the plugin declares a drawer: the drawer runs
  // beside the banner, top to bottom.
  auto* outer = new QHBoxLayout(container);
  outer->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  outer->setSpacing(PJ::theme::space(theme::Space::None));
  auto* body = new QWidget(container);
  body->setObjectName(QStringLiteral("toolboxPanelBody"));
  auto* column = new QVBoxLayout(body);
  column->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  column->setSpacing(PJ::theme::space(theme::Space::None));

  // The migrate buttons are host chrome: plugins need no awareness of either gesture.
  auto* banner = new QWidget(body);
  Ui::ToolboxBanner banner_ui;
  banner_ui.setupUi(banner);
  QHBoxLayout* row = banner_ui.bannerRow;
  QLabel* title_label = banner_ui.toolboxBannerTitle;
  title_label->setText(title);
  SvgButton* float_button = banner_ui.buttonMigrateFloat;
  float_button->setIconPath(u":/resources/svg/float_window.svg"_s);
  SvgButton* migrate_button = banner_ui.buttonMigrateTab;
  migrate_button->setIconPath(u":/resources/svg/tab_move.svg"_s);
  SvgButton* dock_button = banner_ui.buttonMigrateDock;
  dock_button->setIconPath(u":/resources/svg/dock_window.svg"_s);
  SvgButton* close_button = banner_ui.buttonClose;
  close_button->setIconPath(u":/resources/svg/close_windows_light.svg"_s);

  // A tagged plugin action stays in the content tree for its existing routing
  // while the host gives it a domain-neutral place in takeover chrome.
  std::vector<QPointer<SvgButton>> chrome_action_proxies;
  int leading_proxies = 0;
  for (QAbstractButton* src : taggedChromeActions(content)) {
    SvgButton* proxy = makeChromeActionProxy(src, banner);
    chrome_action_proxies.emplace_back(proxy);
    if (isLeadingChromeAction(src)) {
      row->insertWidget(leading_proxies, proxy);
      ++leading_proxies;
    } else {
      row->insertWidget(row->indexOf(dock_button), proxy);
    }
    src->hide();
  }
  if (leading_proxies > 0) {
    // Controls on both sides: centre the title, as a window title.
    row->insertStretch(row->indexOf(title_label), 1);
    title_label->setAlignment(Qt::AlignCenter);
  }

  // The drawer leaves the content tree for a splitter pane; the hoisted-widgets property keeps its by-name data
  // binding, so the plugin drives it as if it had never moved.
  QPointer<QWidget> side_drawer;
  for (auto* candidate : content->findChildren<QWidget*>()) {
    if (candidate != nullptr && candidate->property(kToolboxSideDrawerProperty).toBool()) {
      side_drawer = candidate;
      break;
    }
  }
  if (!side_drawer.isNull()) {
    QVariantList hoisted = content->property(PJ::kHoistedWidgetsProperty).toList();
    hoisted.push_back(QVariant::fromValue<QObject*>(side_drawer.data()));
    content->setProperty(PJ::kHoistedWidgetsProperty, hoisted);
  }

  const auto size_banner = [banner, float_button, migrate_button, dock_button, close_button,
                            chrome_action_proxies](const ChromeMetrics& metrics) {
    banner->setFixedHeight(metrics.bandHeight());
    dock_button->setExtent(metrics.bandHeight(), metrics.icon_size);
    float_button->setExtent(metrics.bandHeight(), metrics.icon_size);
    migrate_button->setExtent(metrics.bandHeight(), metrics.icon_size);
    for (const auto& proxy : chrome_action_proxies) {
      if (!proxy.isNull()) {
        proxy->setExtent(metrics.bandHeight(), metrics.icon_size);
      }
    }
    close_button->setExtent(metrics.bandHeight(), metrics.icon_size);
  };
  size_banner(chrome_metrics_);
  connect(this, &MainWindow::chromeMetricsChanged, banner, size_banner);

  // Pinned shows the dock button, docked the tab one; also re-shows the banner a floating window hid.
  const std::function<void(bool)> apply_chrome = [banner, float_button, migrate_button, dock_button, close_button,
                                                  chrome_action_proxies, can_float = bool(on_float),
                                                  can_dock = bool(on_dock)](bool pinned_presentation) {
    banner->show();
    dock_button->setVisible(pinned_presentation && can_dock);
    migrate_button->setVisible(!pinned_presentation);
    float_button->setVisible(can_float);
    // The plugin's own actions belong to the panel, not to the takeover presentation.
    for (const auto& proxy : chrome_action_proxies) {
      if (!proxy.isNull()) {
        proxy->show();
      }
    }
    close_button->show();
  };
  if (on_dock) {
    connect(dock_button, &QToolButton::clicked, this, on_dock);
  }
  // Here as well as in the presenter's tab attach: the marketplace panel pins through on_migrate alone.
  connect(migrate_button, &QToolButton::clicked, this, [apply_chrome, on_migrate]() {
    apply_chrome(true);
    on_migrate();
  });
  // The floating window's own title bar is the header.
  if (on_float) {
    connect(float_button, &QToolButton::clicked, this, [banner, on_float]() {
      banner->hide();
      on_float();
    });
  } else {
    float_button->hide();
  }

  connect(close_button, &QToolButton::clicked, this, on_close);

  column->addWidget(banner);
  column->addWidget(content, /*stretch=*/1);
  if (side_drawer.isNull()) {
    outer->addWidget(body, /*stretch=*/1);
  } else {
    // The drawer's own minimumWidth is the drag floor; nothing is hardcoded here.
    QSplitter* splitter = makeSideDrawerSplitter(side_drawer, body, toolboxDrawerWidthSettingsKey(persist_key));
    outer->addWidget(splitter, /*stretch=*/1);
  }
  return {.container = container, .apply_chrome = apply_chrome};
}

void MainWindow::presentPanel(QWidget* panel) {
  Q_ASSERT(panel);
  // A new panel replaces the open one.
  dismissTakeoverPanel();

  ui_->centralAreaStack->addWidget(panel);
  ui_->centralAreaStack->setCurrentWidget(panel);
  current_panel_ = panel;
}

QWidget* MainWindow::releaseCentralPanel() {
  if (current_panel_ == nullptr) {
    return nullptr;
  }
  QWidget* released = current_panel_;
  ui_->centralAreaStack->setCurrentWidget(ui_->tabbedPlotWidget);
  ui_->centralAreaStack->removeWidget(released);
  released->hide();
  current_panel_ = nullptr;
  filter_editor_origin_ = nullptr;  // no Filter Editor preview to drive once the panel is gone
  // Non-owning: a toolbox's engine belongs to its ToolboxPresenter.
  current_panel_engine_ = nullptr;
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
  // Launching any other panel comes here, and the teardown below would kill a job in flight: fold into a tab
  // instead. If the fold does not take, fall through so the takeover is never stuck on screen.
  if (ToolboxPresenter* toolbox = takeoverToolbox(); toolbox != nullptr && toolbox->hasWorkInFlight()) {
    toolbox->moveToTab(/*transient=*/true);
    if (current_panel_ == nullptr) {
      return;
    }
  }
  // A registered toolbox tears down through its presenter. Other panels (marketplace, filter editor) have no
  // PanelEngine and just restore; the engine branch is a guard, since only a ToolboxPresenter sets one.
  if (const ToolboxPresenter* toolbox = takeoverToolbox(); toolbox != nullptr) {
    teardownToolbox(toolbox->pluginId(), ToolboxPresenter::Teardown::kDeferred);
    return;
  }
  PanelEngine* previous_engine = current_panel_engine_;
  if (previous_engine != nullptr) {
    previous_engine->close();
  }
  restoreCentralArea();
  if (previous_engine != nullptr) {
    previous_engine->deleteLater();
  }
}

ToolboxPresenter* MainWindow::takeoverToolbox() const {
  return toolboxForContainer(current_panel_);
}

void MainWindow::foldTakeoverPanelIfOwnedBy(const void* owner, bool transient) {
  if (ToolboxPresenter* toolbox = takeoverToolbox(); toolbox != nullptr && toolbox->sessionOwner() == owner) {
    toolbox->moveToTab(transient);
  }
}

void MainWindow::revealOwnedTabOverTakeover(const QString& plugin_id, PlotDocker* docker) {
  ToolboxPresenter* toolbox = takeoverToolbox();
  if (toolbox == nullptr || toolbox->pluginId() != plugin_id) {
    return;
  }
  // The fold leaves the panel's own tab current, so the new tab is selected after it.
  toolbox->moveToTab(/*transient=*/true);
  ui_->tabbedPlotWidget->focusTab(docker);
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
  // The desktop prompt is modal, so the continuation has run on return. Browser callers use the async form.
  bool confirmed = false;
  confirmCancelRunningJobAsync(label, [&confirmed]() { confirmed = true; });
  return confirmed;
}

void MainWindow::confirmCancelRunningJobAsync(const QString& label, std::function<void()> on_confirmed) {
  if (confirm_running_job_) {
    if (confirm_running_job_(label)) {
      on_confirmed();
    }
    return;
  }
  // No default button: an accidental Enter must not cancel a long transfer.
  static constexpr int kCancelAndClose = 0;
  MessageBox::questionAsync(
      this, label.isEmpty() ? tr("Cancel download?") : label, tr("A download is in progress. Cancel it and close?"),
      // "&&" renders a literal ampersand, not a mnemonic.
      {{tr("Cancel download && close"), MessageBox::kDestructiveRole},
       {tr("Keep downloading"), MessageBox::kCancelRole}},
      [on_confirmed = std::move(on_confirmed)](int choice, bool) {
        if (choice == kCancelAndClose) {
          on_confirmed();
        }
      });
}

void MainWindow::openEmbeddedConsole() {
#ifdef PJ_TARGET_WASM
  qWarning("MainWindow::openEmbeddedConsole: external helper processes are unavailable in the browser");
#else
  auto* view = new RasterStreamView(this);
  view->setKeyTranslator(&engineKeyForQtKey);
  presentPanel(view);
  connect(view, &RasterStreamView::sessionEnded, this, [this]() { restoreCentralArea(); });
  const QString dir = QCoreApplication::applicationDirPath() + u"/3rdparty/retro/"_s;
  QString helper = QStandardPaths::findExecutable(u"pj-raster-helper"_s, {dir});
  if (helper.isEmpty()) {
    helper = dir + u"pj-raster-helper"_s;
  }
  view->start(helper, dir + u"base.wad"_s);
#endif
}

// One interactive toolbox's host services and plugin handle. A struct, not lambda captures, because capture
// destruction order is unspecified and the destructor below fixes the order.
struct ToolboxPanelSession {
  std::unique_ptr<QSettingsBackend> settings;
  std::unique_ptr<ServiceRegistryBuilder> builder;
  std::unique_ptr<ToolboxRuntimeHost> host;
  std::unique_ptr<DerivedRuntimeHosts> derived_hosts;
  std::unique_ptr<SourcePromotionHost> promotion_host;
  std::unique_ptr<PlaybackControlHost> playback_host;
  std::unique_ptr<ViewportRuntimeHost> viewport_host;
  std::unique_ptr<PlotTabsRuntimeHost> plot_tabs_host;
#ifndef PJ_TARGET_WASM
  std::unique_ptr<McpEndpointHost> mcp_endpoint;
#endif
  std::shared_ptr<ToolboxHandle> handle;

  // Explicit, so a member reorder cannot break it:
  // 1. promotion_host->shutdown() fails every pending promotion callback, which reaches plugin code: the plugin
  //    must be alive.
  // 2. handle: the plugin persists its state through the settings backend and joins its workers. The promotion host
  //    object outlives it, because the interactive path has no job registry to join and a straggler worker must
  //    hit a live mutex and the shutting-down rejection, not freed memory.
  // 3. The promotion host, then the service views (builder), then derived_hosts (it ends the plugin's previews,
  //    which no plugin call may re-create), the bridges and host. host holds a SettingsBackend&: settings goes last.
  ~ToolboxPanelSession() {
    if (promotion_host != nullptr) {
      promotion_host->shutdown();
    }
    handle.reset();
#ifndef PJ_TARGET_WASM
    // Revokes the plugin's MCP token, held or not (the plugin may have released it from its
    // destructor just now): its queued tool results point into the services below.
    mcp_endpoint.reset();
#endif
    promotion_host.reset();
    builder.reset();        // drops the service views into host + the routed bridges first
    derived_hosts.reset();  // ends this plugin's previews; its persistent recipes live on in the engines
    // Adapters over services and lambdas that outlive every ToolboxPanelSession.
    playback_host.reset();
    viewport_host.reset();
    // The tabs it made stay, the user's to keep, as a derived series outlives its plugin.
    plot_tabs_host.reset();
    host.reset();
    settings.reset();
  }
};
std::shared_ptr<ToolboxPanelSession> MainWindow::buildToolboxPanelSession(
    const RuntimeToolboxPlugin& toolbox, const QString& plugin_id, const QString& source,
    const std::function<void(const QString&, const QString&)>& report_error) {
  // HeadlessDescriptorProviderSession::create builds the toolbox host, DerivedRuntimeHosts and source promotion the
  // same way for the layout import (shared snippets in ToolboxHostWiring.h): keep their semantics in step.
  auto session = std::make_shared<ToolboxPanelSession>();
  session->settings = std::make_unique<QSettingsBackend>();
  session->builder = std::make_unique<ServiceRegistryBuilder>();

  ToolboxRuntimeHost::Callbacks callbacks;
  const std::string plugin_id_std = plugin_id.toStdString();
  callbacks.on_data_changed = [this, plugin_id_std](std::vector<DatasetId> ingested_datasets) {
    session_->catalogModel().rebuildFromDatastore();
    // As FileLoader does: otherwise the "use time offset" origin stays at the 0 it memoized before any data, and
    // imported series keep an absolute epoch axis.
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
    // A parser-ingest import focuses the timeline on its data, which the union of earlier loads would bury; plain
    // write-API toolboxes (empty list) keep the union.
    if (ingested_datasets.empty() || !session_->focusPlaybackOnDatasets(ingested_datasets)) {
      session_->seedPlaybackFromSession();
    }
    // Markers may have changed.
    session_->sessionManager().notifyMarkersChanged();
    // Re-derived, not announced: only a derivation drops the row of a withdrawn transform.
    syncCustomTopicsPanel();
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
    auto& dps = session_->sessionManager().dataProcessorService();
    qInfo(
        "PJ_WASM_TOOLBOX_TRANSFORM_READY plugin=%s recipes=%llu", plugin_id_std.c_str(),
        static_cast<unsigned long long>(dps.transformRecipes().size()));
#else
    Q_UNUSED(plugin_id_std);
#endif
  };
  callbacks.on_message = [this, source](PJ_toolbox_message_level_t level, std::string message) {
    if (diagnostic_history_ == nullptr) {
      return;
    }
    diagnostic_history_->record(toolboxDiagnosticLevel(level), source, u"toolbox"_s, QString::fromStdString(message));
  };

  // GUI thread (the host marshals). Lifecycle bookkeeping is SessionManager's; these keep the presentation. Weak:
  // the host inside the session owns these callbacks, so a strong capture would leak it.
  const std::weak_ptr<ToolboxPanelSession> session_weak = session;
  callbacks.on_ingest_started = [this, session_weak](
                                    DatasetId dataset, std::string label, uint64_t total, bool cancellable) {
    const auto owner = session_weak.lock();
    if (owner == nullptr) {
      return;
    }
    const QString import_label = QString::fromStdString(label);
    // kStopOnly: the stop is cooperative with no host rollback, so discard shows as unavailable rather than silently
    // keeping. The C ABI reports no outcome: always kUnknown.
    const std::weak_ptr<void> stop_owner = session_weak;
    const auto token = session_->sessionManager().beginIngest(
        dataset, import_label, total, cancellable,
        [this, dataset, stop_owner](bool) {
          if (stop_owner.lock() == nullptr) {
            return;  // the panel is gone
          }
          const auto import_it = toolbox_active_imports_.constFind(dataset);
          if (import_it != toolbox_active_imports_.constEnd() && import_it->host != nullptr) {
            // This dataset only: one panel often imports several through the same host.
            import_it->host->requestStopIngestForDataset(dataset);
          }
        },
        IngestStop::kStopOnly);

    // Recorded even while FileLoader owns the strip, so started/finished stay paired and the import can re-adopt it.
    toolbox_active_imports_.insert(dataset, ToolboxIngestRef{token, owner, owner->host.get()});
    toolbox_ingest_label_ = import_label;
    toolbox_ingest_dataset_ = dataset;
    // Its series must be reachable while the batch runs; folding keeps the instance and the download alive.
    foldTakeoverPanelIfOwnedBy(owner.get(), /*transient=*/true);
    if (file_loader_->isBusy()) {
      return;  // FileLoader wins the strip; the import runs undisplayed
    }
    adoptToolboxIngestStrip();
  };
  callbacks.on_ingest_progress = [this](DatasetId dataset, uint64_t current, uint64_t total) {
    // The host flushed its rows before this fired. They are published for the dataset the host named even when the
    // map misses it (a bookkeeping gap, not unreal rows). The token is copied, never held as an iterator: the
    // publishes emit synchronously, and an observer starting or finishing an import rehashes the map.
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
    // Re-checked: the publishes above may have retired it.
    if (!tracked_token.has_value() || !toolbox_active_imports_.contains(dataset)) {
      return;
    }
    if (file_loader_->isBusy() || !session_->sessionManager().hasActiveIngests()) {
      return;
    }
    if (ingest_strip_owner_kind_ != IngestStripOwnerKind::kInteractiveToolbox &&
        ingest_strip_owner_kind_ != IngestStripOwnerKind::kLayoutBatch) {
      adoptToolboxIngestStrip();  // the file load that owned the strip drained
    }
    if (ingest_strip_owner_dataset_ != dataset) {
      return;  // another producer owns the display
    }
    setIngestStripProgress(current, total);
  };
  callbacks.on_ingest_finished = [this](DatasetId dataset) {
    // The C ABI reports no outcome.
    auto active_import_it = toolbox_active_imports_.find(dataset);
    if (active_import_it != toolbox_active_imports_.end()) {
      session_->sessionManager().endIngest(active_import_it->token, IngestOutcome::kUnknown);
    }
    toolbox_active_imports_.remove(dataset);
    if (toolbox_ingest_dataset_ == dataset) {
      // Survivor picks fall back to the lowest surviving id.
      toolbox_ingest_dataset_ = 0;
      toolbox_ingest_label_.clear();
    }
    releaseIngestStripOwner(dataset);
  };

  // Shared with the headless path (ToolboxHostWiring.h).
  ToolboxRuntimeHost::ParserIngestDeps ingest_deps;
  ingest_deps.catalog = &session_->extensionCatalog();
  ingest_deps.ingest_taps = session_->sessionManager().ingestTapsShared();
  ingest_deps.register_object_parser = makeObjectParserRegistrar(session_->sessionManager());
#ifndef __EMSCRIPTEN__
  // Delegated downloads go to the source cache with the SourceRecord a layout save persists. plugin_id is the
  // catalog's toolbox id, which a plugin cannot spoof.
  wireSourceCapture(ingest_deps, source_capture_service_.get(), plugin_id.toStdString(), session_->sessionManager());
#endif

  session->host = std::make_unique<ToolboxRuntimeHost>(
      session_->sessionManager().dataEngine(), session_->sessionManager().objectStore(), *session->settings,
      std::move(callbacks), std::move(ingest_deps));
  if (auto status = session->host->registerServices(*session->builder); !status) {
    report_error(
        source,
        tr("Failed to publish toolbox services for '%1': %2").arg(source, QString::fromStdString(status.error())));
    return nullptr;
  }

  // pj.data_processors.v1: the host runs and recomputes the transforms and marker rules a toolbox submits.
  // A persistent node the plugin creates, replaces or removes is its own undo step, as a Transform Editor apply is.
  // A plugin re-creating its nodes while a restore applies is part of that restore.
  session->derived_hosts = std::make_unique<DerivedRuntimeHosts>(
      session_->sessionManager(), session_->catalogModel(), plugin_id.toStdString(), [this] {
        if (!applying_state_ && !progressive_layout_in_flight_) {
          plugin_history_step_.start();
        }
      });
  if (auto status = session->derived_hosts->registerServices(*session->builder); !status) {
    report_error(
        source, tr("Failed to publish data-processor services for '%1': %2")
                    .arg(source, QString::fromStdString(status.error())));
    return nullptr;
  }

  // Per instance: the provider identity is the host-derived manifest id, and a promotion can target only a dataset
  // this plugin's ingest produced. `host` outlives promotion_host (~ToolboxPanelSession).
  session->promotion_host = std::make_unique<SourcePromotionHost>(
      *file_loader_, session_->sessionManager(), QString::fromStdString(toolbox.id),
      [host = session->host.get()](DatasetId dataset_id) { return host->hasIngestForDataset(dataset_id); });
  if (auto status = session->promotion_host->registerServices(*session->builder); !status) {
    report_error(
        source, tr("Failed to publish the source-promotion service for '%1': %2")
                    .arg(source, QString::fromStdString(status.error())));
    return nullptr;
  }

  // pj.playback.v1 and pj.viewport.v1 slots are main-thread per the ABI: the lambdas touch widgets directly.
  session->playback_host = std::make_unique<PlaybackControlHost>(
      session_->playbackEngine(), std::bind_front(&MainWindow::displayTimeForTopic, this),
      std::bind_front(&MainWindow::displayTimeForSource, this));
  if (auto status = session->playback_host->registerServices(*session->builder); !status) {
    report_error(
        source,
        tr("Failed to publish the playback service for '%1': %2").arg(source, QString::fromStdString(status.error())));
    return nullptr;
  }

  // Scoped to `plugin_id`: the plugin reaches only the tabs it composed.
  session->viewport_host = std::make_unique<ViewportRuntimeHost>(plugin_plot_tabs_->viewportCallbacks(plugin_id));
  if (auto status = session->viewport_host->registerServices(*session->builder); !status) {
    report_error(
        source,
        tr("Failed to publish the viewport service for '%1': %2").arg(source, QString::fromStdString(status.error())));
    return nullptr;
  }

  session->plot_tabs_host = std::make_unique<PlotTabsRuntimeHost>(plugin_plot_tabs_->plotTabsCallbacks(plugin_id));
  if (auto status = session->plot_tabs_host->registerServices(*session->builder); !status) {
    report_error(
        source,
        tr("Failed to publish the plot-tab service for '%1': %2").arg(source, QString::fromStdString(status.error())));
    return nullptr;
  }

#ifndef PJ_TARGET_WASM
  // pj.feature_catalog.v1: one read-only host shared by every session; optional, so a failure only warns.
  registerOptionalService<sdk::FeatureCatalogService>(*session->builder, feature_catalog_->view());
  // pj.mcp_endpoint.v1: an agent the plugin launches runs its tools as this plugin, against these services.
  session->mcp_endpoint =
      std::make_unique<McpEndpointHost>([this] { return ensureMcpServer(); }, session->builder->view(), plugin_id);
  registerOptionalService<sdk::McpEndpointHostService>(*session->builder, session->mcp_endpoint->raw());
#endif
  return session;
}

std::shared_ptr<ToolboxPanelSession> MainWindow::buildAgentSession(const QString& owner, const QString& label) {
  RuntimeToolboxPlugin stand_in;  // no library: nothing is loaded, only the services are built
  stand_in.id = owner.toStdString();
  stand_in.name = label.toStdString();
  return buildToolboxPanelSession(stand_in, owner, label, [this](const QString&, const QString& error) {
    emitDiagnostic(DiagnosticLevel::kError, "MCP", "agent-services", error);
  });
}

PJ_service_registry_t MainWindow::toolboxSessionServices(ToolboxPanelSession& session) {
  return session.builder->view();
}

void MainWindow::refreshMcpIndicator() {
  const std::vector<McpServer::JobInfo> jobs =
      mcp_server_ != nullptr ? mcp_server_->unfinishedJobs() : std::vector<McpServer::JobInfo>();
  if (mcp_jobs_dialog_ != nullptr && mcp_jobs_dialog_->isVisible()) {
    std::vector<McpJobsDialog::Job> rows;
    for (const McpServer::JobInfo& job : jobs) {
      rows.push_back(
          {.id = job.id, .client = job.client, .what = job.summary, .state = job.state, .elapsed_s = job.elapsed_s});
    }
    mcp_jobs_dialog_->setJobs(rows);
  }
  if (mcp_agent_button_ == nullptr) {
    if (mcp_active_client_.isEmpty() && jobs.empty()) {
      return;
    }
    mcp_agent_button_ = new QToolButton(this);
    mcp_agent_button_->setObjectName(u"mcpAgentButton"_s);
    mcp_agent_button_->setAutoRaise(true);
#ifdef PJ_TARGET_WASM
    mcp_agent_button_->setToolTip(
        tr("An AI agent is controlling PlotJuggler through MCP. Click to see what it is running and stop it; closing "
           "this page stops it too."));
#else
    mcp_agent_button_->setToolTip(
        tr("An AI agent is controlling PlotJuggler through MCP. Click to see what it is running and stop it. Turn "
           "this off in Preferences → Appearance."));
#endif
    statusBar()->addPermanentWidget(mcp_agent_button_);
    connect(mcp_agent_button_, &QToolButton::clicked, this, &MainWindow::showMcpJobs);
  }
  const QString who = mcp_active_client_.isEmpty() ? tr("AI agents") : tr("AI agent: %1").arg(mcp_active_client_);
  mcp_agent_button_->setText(
      jobs.empty() ? who : tr("%1 · %n running", nullptr, static_cast<int>(jobs.size())).arg(who));
  // Only the button: the bar also carries other messages (undo failures). It stays while anything runs.
  mcp_agent_button_->setVisible(!mcp_active_client_.isEmpty() || !jobs.empty());
}

void MainWindow::showMcpJobs() {
  if (mcp_jobs_dialog_ == nullptr) {
    mcp_jobs_dialog_ = new McpJobsDialog(this);
    connect(mcp_jobs_dialog_, &McpJobsDialog::stopRequested, this, [this](quint64 id) {
      if (mcp_server_ != nullptr) {
        mcp_server_->stopJob(id);
      }
    });
    // Elapsed times and states move while it is open, and only then.
    auto* refresh = new QTimer(mcp_jobs_dialog_);
    refresh->setInterval(1000);
    connect(refresh, &QTimer::timeout, this, [this, refresh] {
      if (mcp_jobs_dialog_->isVisible()) {
        refreshMcpIndicator();
      } else {
        refresh->stop();
      }
    });
    mcp_jobs_refresh_ = refresh;
  }
  mcp_jobs_dialog_->show();
  mcp_jobs_dialog_->raise();
  mcp_jobs_refresh_->start();
  refreshMcpIndicator();
}

McpServer* MainWindow::ensureMcpServer() {
  if (mcp_server_ != nullptr) {
    return mcp_server_.get();
  }
  auto server = std::make_unique<McpServer>(*this);
#ifndef PJ_TARGET_WASM
  if (!server->listening()) {
    // Every acquire retries (one per Assistant turn): one report until a start succeeds.
    if (!std::exchange(mcp_listen_failure_reported_, true)) {
      emitDiagnostic(
          DiagnosticLevel::kError, "MCP", "listen-failed",
          tr("The MCP server could not listen on 127.0.0.1, so AI agents cannot reach PlotJuggler."));
    }
    return nullptr;
  }
#endif
  mcp_listen_failure_reported_ = false;
  connect(server.get(), &McpServer::activeClientChanged, this, [this](const QString& client) {
    mcp_active_client_ = client;
    refreshMcpIndicator();
  });
  connect(server.get(), &McpServer::jobsChanged, this, &MainWindow::refreshMcpIndicator);
  mcp_server_ = std::move(server);
  return mcp_server_.get();
}

void MainWindow::openMcpPublicAccess() {
  if (McpServer* server = ensureMcpServer(); server != nullptr) {
    server->openPublicAccess();
  }
}

void MainWindow::closeMcpPublicAccess() {
  if (mcp_server_ != nullptr) {
    mcp_server_->closePublicAccess();
  }
}

bool MainWindow::mcpPublicAccess() const {
  return mcp_server_ != nullptr && mcp_server_->publicAccess();
}

bool MainWindow::connectMcpRelay(const QUrl& relay) {
  if (!McpBrowserLink::acceptableRelay(relay)) {
    emitDiagnostic(
        DiagnosticLevel::kError, "MCP", "relay-refused",
        tr("Not connecting to the AI agent relay %1: it must be an http address on this computer (127.0.0.1).")
            .arg(relay.toDisplayString(QUrl::RemoveQuery)));
    return false;
  }
  McpServer* server = ensureMcpServer();
  if (server == nullptr) {
    return false;
  }
  server->openPublicAccess();
  delete mcp_relay_link_.data();
  mcp_relay_link_ = new McpBrowserLink(*server, relay, windowTitle(), server);
  return true;
}

void MainWindow::launchToolbox(
    const QString& plugin_id, const QString& initial_config, ToolboxLaunchTarget target, const QString& pin_tab_name) {
  auto report_error = [this](const QString& source, const QString& detail) {
    if (diagnostic_history_ != nullptr) {
      diagnostic_history_->record(DiagnosticLevel::kError, source, u"toolbox"_s, detail);
    }
    qWarning("MainWindow::launchToolbox: %s", qPrintable(detail));
    MessageBox::warning(this, tr("Toolbox"), detail);  // a click that did nothing needs an answer on screen
  };

  // 0. One live instance per toolbox id. An idle takeover is torn down and relaunched fresh; a busy one folds into a
  //    tab (its job survives) and is treated as pinned.
  ToolboxPresenter* live = findToolbox(plugin_id);
  if (live != nullptr && live->presentation() == ToolboxPresenter::Presentation::kTakeover) {
    dismissTakeoverPanel();
    live = findToolbox(plugin_id);
  }
  if (live != nullptr && live->presentation() == ToolboxPresenter::Presentation::kTab) {
    // Focus its tab, after dismissing a takeover that would hide the tab strip.
    if (initial_config.isEmpty()) {
      dismissTakeoverPanel();
      ui_->tabbedPlotWidget->focusWidgetTab(live->container());
      return;
    }
    // An in-place edit must reach loadConfig() before the dialog is built, so only a fresh instance can take it:
    // relaunch pinned, keeping the label.
    const QString pinned_name = live->title();
    ui_->tabbedPlotWidget->closeWidgetTab(live->container());
    // A declined close keeps the entry; relaunching would make a second instance.
    if (findToolbox(plugin_id) != nullptr) {
      return;
    }
    launchToolbox(plugin_id, initial_config, ToolboxLaunchTarget::kPinnedTab, pinned_name);
    return;
  }
  if (live != nullptr && live->presentation() == ToolboxPresenter::Presentation::kFloating) {
    // A top-level needs nothing uncovered. showNormal(): raise() cannot bring back a hidden or minimized window.
    QWidget* existing = live->floatingWindow();
    if (existing != nullptr && target == ToolboxLaunchTarget::kTakeover && initial_config.isEmpty()) {
      existing->showNormal();
      existing->raise();
      existing->activateWindow();
      return;
    }
    // An in-place edit or a pinned-tab restore needs a fresh instance: the close runs the full teardown.
    live = nullptr;
    if (existing != nullptr) {
      existing->close();
    }
  }
  if (findToolbox(plugin_id) != nullptr) {
    // A remnant nothing above could take apart (a floating window already gone, a detached one).
    teardownToolbox(plugin_id, ToolboxPresenter::Teardown::kDeferred);
  }
  if (target == ToolboxLaunchTarget::kPinnedTab) {
    // The takeover hides the tab strip, and it may be this same plugin, which must not get two instances.
    dismissTakeoverPanel();
  }

  const LoadedToolbox* toolbox = session_->extensionCatalog().findToolbox(plugin_id);
  if (toolbox == nullptr) {
    report_error(plugin_id, tr("Toolbox '%1' not found in catalog").arg(plugin_id));
    return;
  }

  const QString source = toolbox->name.empty() ? plugin_id : QString::fromStdString(toolbox->name);
  const std::shared_ptr<ToolboxPanelSession> session =
      buildToolboxPanelSession(*toolbox, plugin_id, source, report_error);
  if (session == nullptr) {
    return;
  }

  session->handle = std::make_shared<ToolboxHandle>(toolbox->library.createHandle());
  if (auto status = session->handle->bind(session->builder->view()); !status) {
    report_error(source, tr("Failed to bind toolbox '%1': %2").arg(source, QString::fromStdString(status.error())));
    return;
  }
  usageCounters().countPlugin(PluginUse::kToolboxOpen, toolbox->id);
  // An in-place edit: before the dialog is built.
  if (!initial_config.isEmpty()) {
    (void)session->handle->loadConfig(initial_config.toStdString());
  }

  const PJ_borrowed_dialog_t borrowed = session->handle->getDialog();
  if (borrowed.vtable == nullptr || borrowed.ctx == nullptr) {
    report_error(source, tr("Toolbox '%1' returned no dialog").arg(source));
    return;
  }
  // The curve tree drags opaque catalog keys; the toolbox expects topic or series names.
  PanelEngineConfig panel_config;
#ifdef Q_OS_WASM
  const std::string plugin_id_std = plugin_id.toStdString();
  // The Transform Editor rebuilds its preview on_tick: restarting the deadline per edit debounces typing and
  // guarantees one wake-up after the edit on Qt's WASM dispatcher.
  panel_config.restart_tick_timer_on_event = plugin_id_std == "toolbox-transform-editor";
#endif
  // Without them chart_series previews fall back to the bare ChartPreviewWidget.
  panel_config.session = session_.get();
  panel_config.catalog = &session_->catalogModel();
  panel_config.embedded_views = makeEmbeddedViewHost();
  panel_config.catalog_key_resolver = [this](const std::string& key) -> std::string {
    return session_->catalogModel().pluginDropName(QString::fromStdString(key)).value_or(std::string{});
  };
  // The catalog snapshot's data source handles are the dataset ids.
  panel_config.catalog_key_data_source = [this](const std::string& key) -> uint32_t {
    const auto item = session_->catalogModel().itemDescriptor(QString::fromStdString(key));
    return item.has_value() ? item->dataset_id : 0;
  };
  auto* engine = new PanelEngine(DialogHandle::fromBorrowed(borrowed), panel_config, this);
  QWidget* panel = engine->openPanel();
  if (panel == nullptr) {
    report_error(source, tr("Failed to build the panel UI for '%1'").arg(source));
    delete engine;
    return;
  }

  // A QLineEdit accepts drops, which would never reach the panel-root DropEventFilter.
  for (QWidget* child : panel->findChildren<QWidget*>()) {
    child->setAcceptDrops(false);
  }

  // QUiLoader inflates a plugin's SectionHeaderBand with no wiring to the chrome metrics.
  for (auto* band : panel->findChildren<SectionHeaderBand*>()) {
    band->onChromeMetricsChanged(chrome_metrics_);
    connect(this, &MainWindow::chromeMetricsChanged, band, &SectionHeaderBand::onChromeMetricsChanged);
  }

  // The presenter owns the session, and so the plugin, from here. The chrome hooks look it up by id: a click queued
  // across a teardown must find nothing, not a dangling presenter.
  WrappedToolboxPanel wrapped = wrapToolboxPanel(
      panel, source,
      ToolboxChromeHooks{
          .on_close = [this,
                       plugin_id]() { withToolbox(plugin_id, [](ToolboxPresenter& shown) { shown.requestClose(); }); },
          .on_migrate =
              [this, plugin_id]() {
                withToolbox(plugin_id, [](ToolboxPresenter& shown) { shown.moveToTab(/*transient=*/false); });
              },
          .on_float =
              [this, plugin_id]() { withToolbox(plugin_id, [](ToolboxPresenter& shown) { shown.moveToFloating(); }); },
          .on_dock = [this,
                      plugin_id]() { withToolbox(plugin_id, [](ToolboxPresenter& shown) { shown.moveToDock(); }); }},
      /*persist_key=*/plugin_id);
  ToolboxPresenter& presenter = registerToolbox(
      plugin_id, ToolboxPresenter::Spec{
                     .container = wrapped.container,
                     .engine = engine,
                     .session = session,
                     .host = session->host.get(),
                     .title = pin_tab_name.isEmpty() ? source : pin_tab_name,
                     // The presenter owns `session`, so its raw pointer outlives this closure.
                     .save_config = [session = session.get()]() -> QString {
                       std::string config_json;
                       if (session->handle == nullptr || !session->handle->saveConfig(config_json)) {
                         return {};
                       }
                       return QString::fromStdString(config_json);
                     },
                     .apply_chrome = std::move(wrapped.apply_chrome)});

  if (target == ToolboxLaunchTarget::kPinnedTab) {
    presenter.moveToTab(/*transient=*/false);
  } else {
    presenter.moveToDock();
  }
  schedulePanelPreviewSync();
}

void MainWindow::schedulePanelPreviewSync() {
  QTimer::singleShot(250, this, [this]() { syncPanelPreviewDisplay(); });
}

ToolboxPresenter* MainWindow::findToolbox(const QString& plugin_id) const {
  const auto it = toolboxes_.find(plugin_id);
  return it != toolboxes_.end() ? it->second.get() : nullptr;
}

ToolboxPresenter* MainWindow::toolboxForContainer(const QWidget* container) const {
  if (container == nullptr) {
    return nullptr;
  }
  for (const auto& [plugin_id, toolbox] : toolboxes_) {
    if (toolbox->container() == container) {
      return toolbox.get();
    }
  }
  return nullptr;
}

ToolboxPresenter& MainWindow::registerToolbox(const QString& plugin_id, ToolboxPresenter::Spec spec) {
  Q_ASSERT(!toolboxes_.contains(plugin_id));
  auto& slot = toolboxes_[plugin_id];
  slot = std::make_unique<ToolboxPresenter>(*this, plugin_id, std::move(spec));
  return *slot;
}

void MainWindow::teardownToolbox(const QString& plugin_id, ToolboxPresenter::Teardown mode) {
  // Extracted first, so a callback landing mid-teardown finds nothing.
  auto node = toolboxes_.extract(plugin_id);
  if (node.empty()) {
    return;
  }
  node.mapped()->teardown(mode);
  // `node` dies here: the presenter, then the session it owns.
}

void MainWindow::teardownAllToolboxes() {
  while (!toolboxes_.empty()) {
    teardownToolbox(toolboxes_.begin()->first, ToolboxPresenter::Teardown::kImmediate);
  }
}

QDomElement MainWindow::savePinnedToolboxes(QDomDocument& doc) const {
  QDomElement root = doc.createElement(u"pinned_toolboxes"_s);
  const auto append_toolbox = [&doc, &root](const QString& plugin_id, const ToolboxPresenter& toolbox) {
    QDomElement element = doc.createElement(u"toolbox"_s);
    element.setAttribute(u"plugin_id"_s, plugin_id);
    if (const QString tab_name = toolbox.title(); !tab_name.isEmpty()) {
      element.setAttribute(u"tab_name"_s, tab_name);
    }
    if (const QString config = toolbox.saveConfig(); !config.isEmpty()) {
      // A plain text node would entity-escape on each round trip.
      appendJsonAsCdata(doc, element, config);
    }
    root.appendChild(element);
  };

  // std::map order: identical workspaces write identical files.
  for (const auto& [plugin_id, toolbox] : toolboxes_) {
    switch (toolbox->presentation()) {
      case ToolboxPresenter::Presentation::kTab:
        // A transient fold uncovered the chart for a running job; the user never pinned it.
        if (!toolbox->transient()) {
          append_toolbox(plugin_id, *toolbox);
        }
        break;
      case ToolboxPresenter::Presentation::kFloating:
        // As a pinned tab: floating has no layout representation.
        append_toolbox(plugin_id, *toolbox);
        break;
      case ToolboxPresenter::Presentation::kTakeover:
      case ToolboxPresenter::Presentation::kDetached:
        break;  // the chart-area takeover is not part of the layout
    }
  }
  return root;
}

bool MainWindow::restorePinnedToolboxes(const QDomElement& root, MissingCurvePolicy policy) {
  // Replacing the pinned set kills any job in flight: ask once, naming one busy panel; declining leaves the live set
  // and its transfers as they were. Replaced: every tab and floating window, plus a takeover the layout names (its
  // relaunch as a tab would otherwise fold it and raise the tab's own cancel prompt on a path that must not prompt).
  const QDomElement pinned = root.firstChildElement(u"pinned_toolboxes"_s);
  QSet<QString> layout_ids;
  for (QDomElement element = pinned.firstChildElement(u"toolbox"_s); !element.isNull();
       element = element.nextSiblingElement(u"toolbox"_s)) {
    layout_ids.insert(element.attribute(u"plugin_id"_s));
  }
  const auto collect_replaced_ids = [this, &layout_ids]() {
    QStringList replaced_ids;
    for (const auto& [plugin_id, toolbox] : toolboxes_) {
      if (toolbox->presentation() != ToolboxPresenter::Presentation::kTakeover || layout_ids.contains(plugin_id)) {
        replaced_ids.push_back(plugin_id);
      }
    }
    return replaced_ids;
  };
  QStringList replaced_ids = collect_replaced_ids();
  QString busy_label;
  for (const QString& plugin_id : replaced_ids) {
    if (const ToolboxPresenter* toolbox = findToolbox(plugin_id); toolbox != nullptr && toolbox->hasWorkInFlight()) {
      busy_label = toolbox->title();
      break;
    }
  }
  if (!busy_label.isEmpty()) {
    // An unattended restore (a batch drain can land minutes later, mid-anything) and the browser (no modal loop)
    // never ask: resolved as a decline, reported as a diagnostic.
    bool can_prompt = policy != MissingCurvePolicy::kRetainAndDiagnose;
#ifdef PJ_TARGET_WASM
    can_prompt = false;
#endif
    if (!can_prompt) {
      reportLayoutRestoreIssue(
          policy, "pinned-toolbox-busy",
          tr("Layout restore kept the current pinned panels: '%1' has a job still running.").arg(busy_label));
      return false;
    }
    if (!confirmCancelRunningJob(busy_label)) {
      return false;
    }
    // The modal loop may have folded a takeover into a tab (its ingest-start callback), joining the replaced set.
    replaced_ids = collect_replaced_ids();
    for (const QString& plugin_id : replaced_ids) {
      if (const ToolboxPresenter* toolbox = findToolbox(plugin_id); toolbox != nullptr && toolbox->hasWorkInFlight()) {
        stopHostWork(toolbox->host());
      }
    }
  }

  // A layout without pinned toolboxes restores to none. Immediate: relaunching the same plugin needs the engine gone.
  for (const QString& plugin_id : replaced_ids) {
    teardownToolbox(plugin_id, ToolboxPresenter::Teardown::kImmediate);
  }
  for (QDomElement element = pinned.firstChildElement(u"toolbox"_s); !element.isNull();
       element = element.nextSiblingElement(u"toolbox"_s)) {
    const QString plugin_id = element.attribute(u"plugin_id"_s);
    if (plugin_id.isEmpty()) {
      continue;
    }
    // A plugin missing from the catalog is dropped, reported by launchToolbox. directCdataText, not text(), which
    // would recurse into child elements.
    launchToolbox(
        plugin_id, directCdataText(element), ToolboxLaunchTarget::kPinnedTab, element.attribute(u"tab_name"_s));
  }
  return true;
}

}  // namespace PJ
