// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotWidget.h"

#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_item.h>
#include <qwt_plot_marker.h>
#include <qwt_scale_map.h>
#include <qwt_scale_widget.h>
#include <qwt_symbol.h>
#include <qwt_text.h>

#include <QApplication>
#include <QDataStream>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QIODevice>
#include <QIcon>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPalette>
#include <QPen>
#include <QScopedValueRollback>
#include <QSettings>
#include <QStringView>
#include <QTextDocument>
#include <QToolTip>
#include <QUuid>
#include <QVector>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <ranges>
#include <set>

#include "ReportedXml.h"
#include "TimeAxisWidth.h"
#include "WidgetClipboard.h"
#include "pj_plotting/CurveTracker.h"
#include "pj_plotting/DatastoreCurveAdapter.h"
#include "pj_plotting/PlotCurve.h"
#include "pj_plotting/PlotLegend.h"
#include "pj_plotting/PlotMarkersItem.h"
#include "pj_plotting/PlotXml.h"
#include "pj_plotting/PointSeriesXY.h"
#include "pj_plotting/XYCurveDialog.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/CurveColorRegistry.h"
#include "pj_runtime/CurveDescriptor.h"
#include "pj_runtime/CurveDisplayName.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/XmlStateOptions.h"
#include "pj_widgets/AppSettings.h"
#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/MessageBox.h"
#include "pj_widgets/OwnedMenu.h"
#include "pj_widgets/SvgUtil.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

constexpr int kHoverHitRadiusPx = 40;

void addActionCategorySeparator(QMenu& menu) {
  const QList<QAction*> actions = menu.actions();
  if (!actions.isEmpty() && !actions.constLast()->isSeparator()) {
    menu.addSeparator();
  }
}

QString newStateId() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// Continuous gestures (wheel ticks, pan moves) coalesce into one history snapshot this long after the last event.
constexpr int kGestureUndoDebounceMs = 200;

QString curveKey(const QString& source_name, const QString& x_name = {}, const QString& y_name = {}) {
  if (!x_name.isEmpty() || !y_name.isEmpty()) {
    return u"xy:"_s + x_name + u"\n"_s + y_name;
  }
  return u"ts:"_s + source_name;
}

QString pendingCurveIdentity(const QDomElement& curve) {
  static const QStringList attributes{
      u"topic"_s,   u"field"_s,   u"dataset_id"_s,   u"dataset_source"_s,   u"dataset_path"_s,
      u"x_topic"_s, u"x_field"_s, u"x_dataset_id"_s, u"x_dataset_source"_s, u"x_dataset_path"_s,
      u"y_topic"_s, u"y_field"_s, u"y_dataset_id"_s, u"y_dataset_source"_s, u"y_dataset_path"_s,
  };
  QStringList fields;
  fields.reserve(attributes.size() + 1);
  for (const QString& attribute : attributes) {
    fields.push_back(curve.attribute(attribute));
  }
  if (!curve.attribute(u"x_topic"_s).isEmpty()) {
    fields.push_back(curve.attribute(u"name"_s));
  }
  return fields.join(QLatin1Char('\x1f'));
}

using plotting_detail::readReported;

const QStringList kTrueFalse{u"true"_s, u"false"_s};

// Refusals report to the active XmlOptionsRecorder, so a refused set_widget_state names the attribute.
bool isPendingCurveIntentValid(const QDomElement& curve, bool xy_plot) {
  if (curve.tagName() != "curve"_L1 || !plot_xml::isPendingIntent(curve)) {
    XmlOptionsRecorder::noteRejection(curve, plot_xml::kPendingIntentAttribute, u"not a pending <curve>"_s);
    return false;
  }
  const bool time_series = !curve.attribute(u"topic"_s).isEmpty();
  const bool xy = !curve.attribute(u"x_topic"_s).isEmpty() && !curve.attribute(u"y_topic"_s).isEmpty();
  if (time_series == xy || xy != xy_plot) {
    XmlOptionsRecorder::noteRejection(
        curve, xy_plot ? u"x_topic"_s : u"topic"_s,
        xy_plot ? u"a pending curve of an XY plot names x_topic and y_topic, and no topic"_s
                : u"a pending curve of a time-series plot names topic, and no x_topic or y_topic"_s);
    return false;
  }
  const QStringList id_attributes =
      xy ? QStringList{u"x_dataset_id"_s, u"y_dataset_id"_s} : QStringList{u"dataset_id"_s};
  for (const QString& attribute : id_attributes) {
    if (!curve.hasAttribute(attribute)) {
      continue;
    }
    bool ok = false;
    const qulonglong raw = curve.attribute(attribute).toULongLong(&ok);
    if (!ok || raw == 0 || raw > std::numeric_limits<DatasetId>::max()) {
      XmlOptionsRecorder::noteRejection(
          curve, attribute, u"expected a dataset id from 1 to %1"_s.arg(std::numeric_limits<DatasetId>::max()));
      return false;
    }
  }
  return true;
}

// The session-scoped curve-colour registry, reached through the SessionManager the widget holds. nullptr when no
// session is wired (a service-less PlotWidget in a unit test); the base class's per-widget nextColor() rotation is the
// fallback.
CurveColorRegistry* colorRegistryOf(SessionManager* session) {
  return session != nullptr ? &session->curveColorRegistry() : nullptr;
}

// The `<prefix>dataset_id/_source/_path` qualifier triple as saved (PlotWidget::stampDatasetIdentity); an absent
// attribute reads as 0 / empty.
struct SavedDatasetIdentity {
  DatasetId id = 0;
  QString source;
  QString path;
};

SavedDatasetIdentity readDatasetIdentity(const QDomElement& element, const QString& prefix = {}) {
  SavedDatasetIdentity saved{
      .source = readXmlText(element, prefix + "dataset_source"_L1),
      .path = readXmlText(element, prefix + "dataset_path"_L1)};
  static_cast<void>(readXmlInt<DatasetId>(
      element, prefix + "dataset_id"_L1, std::nullopt, 1, std::numeric_limits<DatasetId>::max(), saved.id,
      XmlBadValue::kTolerate));
  return saved;
}

QString markerScopeName(MarkerScope scope) {
  return scope == MarkerScope::kDataset ? QString(plot_xml::kMarkerScopeDataset)
                                        : QString(plot_xml::kMarkerScopeAllDatasets);
}

// An absent or unrecognized `name` maps to nullopt: the entry is ignored, not guessed at.
std::optional<MarkerScope> markerScopeFromName(QStringView name) {
  if (name == plot_xml::kMarkerScopeDataset) {
    return MarkerScope::kDataset;
  }
  if (name == plot_xml::kMarkerScopeAllDatasets) {
    return MarkerScope::kAllDatasets;
  }
  return std::nullopt;
}

// One hidden <marker_scope>, as loadHiddenMarkerScopes applies it; nullopt for an entry it skips (not hidden, unknown
// scope, no dataset id), which is ignored, never refused.
struct HiddenMarkerScope {
  SavedDatasetIdentity dataset;
  MarkerScope scope = MarkerScope::kDataset;
};

std::optional<HiddenMarkerScope> parseHiddenMarkerScope(const QDomElement& marker_scope_element) {
  bool visible = true;
  static_cast<void>(readXmlBool(marker_scope_element, u"visible"_s, true, visible, XmlBadValue::kTolerate));
  QString scope_name;
  static_cast<void>(readXmlEnum(
      marker_scope_element, plot_xml::kMarkerScopeAttribute,
      {plot_xml::kMarkerScopeDataset, plot_xml::kMarkerScopeAllDatasets}, {}, scope_name, XmlBadValue::kTolerate));
  const std::optional<MarkerScope> scope = markerScopeFromName(scope_name);
  const SavedDatasetIdentity saved = readDatasetIdentity(marker_scope_element);
  if (visible || !scope.has_value() || saved.id == 0) {
    return std::nullopt;
  }
  return HiddenMarkerScope{.dataset = saved, .scope = *scope};
}

}  // namespace

PlotWidget::ParsedCurve PlotWidget::parseCurveElement(const QDomElement& curve) {
  if (XmlOptionsRecorder::active()) {
    for (const QString& prefix : {QString(), u"x_"_s, u"y_"_s}) {
      readXmlText(curve, prefix + "topic"_L1);
      readXmlText(curve, prefix + "field"_L1);
      readDatasetIdentity(curve, prefix);
    }
    bool pending = false;
    static_cast<void>(readXmlBool(curve, plot_xml::kPendingIntentAttribute, false, pending, XmlBadValue::kTolerate));
  }
  // Raw: a layout load reads anything but "true" as false.
  const auto read_bool = [&curve](const QString& attribute) {
    return readReported(
               curve, {.attribute = attribute, .type = u"bool"_s, .values = kTrueFalse, .fallback = u"true"_s}) ==
           "true"_L1;
  };
  return ParsedCurve{
      .color = QColor(readReported(curve, {.attribute = u"color"_s, .type = u"color"_s})),
      .visible = read_bool(u"visible"_s),
      .markers_visible = read_bool(u"markers_visible"_s),
      .name = readXmlText(curve, u"name"_s),
      .curve_x = readXmlText(curve, u"curve_x"_s),
      .curve_y = readXmlText(curve, u"curve_y"_s),
      .has_xy_keys = curve.hasAttribute(u"curve_x"_s) && curve.hasAttribute(u"curve_y"_s),
  };
}

PlotWidget::PlotWidget(SessionManager* session, CatalogModel* catalog, QWidget* parent)
    : PlotWidgetBase(parent), session_(session), catalog_(catalog) {
  state_id_ = newStateId();
  setAcceptDrops(true);
  const auto fw_theme = theme::themeFor(QGuiApplication::palette().color(QPalette::Window).lightness() >= 128);
  // Highlight playback tracker, matching the Source Timeline's playhead needle; the reference tracker uses Accent, as
  // the timeline's reference line.
  tracker_ =
      new CurveTracker(qwtPlot(), theme::interaction(theme::Variant::Highlight, theme::State::Checked, fw_theme));
  reference_tracker_ =
      new CurveTracker(qwtPlot(), theme::interaction(theme::Variant::Accent, theme::State::Checked, fw_theme));
  reference_tracker_->setParameter(CurveTracker::kLineOnly);
  reference_tracker_->setEnabled(false);
  connect(tracker_, &CurveTracker::valueBoxMoved, this, &PlotWidgetBase::replotOverlays);

  // Marker overlay: draws the findings (regions, events, value bands, labels) read from the session ObjectStore
  // (serialized PlotMarkers topics) for the series on this plot plus each dataset's Dataset and Global marker scopes.
  // Targets are recomputed from the current curves on each paint; repaint is driven by SessionManager::markersChanged.
  markers_item_ = new PlotMarkersItem(session_);
  markers_item_->setTargetsProvider([this]() { return markerTargets(); });
  markers_item_->attach(qwtPlot());

  // Mouse-hover inspector: a snap-to-curve dot and value tooltip wherever the mouse points, gated by show_points_.
  // Independent of the playback tracker_, whose line always shows the playback time.
  show_point_marker_ = new QwtPlotMarker();
  show_point_marker_->setSymbol(new QwtSymbol(
      QwtSymbol::Ellipse, theme::interaction(theme::Variant::Emphasis, theme::State::Nominal, fw_theme),
      QPen(theme::onFill(theme::Variant::Emphasis, theme::State::Nominal, fw_theme)), QSize(8, 8)));
  show_point_marker_->setVisible(false);
  show_point_marker_->attach(qwtPlot());

  show_point_text_ = new QwtPlotMarker();
  show_point_text_->setVisible(false);
  show_point_text_->attach(qwtPlot());

  connect(this, &PlotWidgetBase::viewResized, this, &PlotWidget::onExternallyResized);
  connect(this, &PlotWidgetBase::curveListChanged, this, &PlotWidget::applySeriesStatus);
  connect(
      qwtPlot()->axisWidget(QwtPlot::xBottom), &QwtScaleWidget::scaleDivChanged, this,
      &PlotWidget::visibleXRangeChanged);
  // A tracker hides the samples outside the view only when it is positioned: re-sample after every axis change, or a
  // zoom back over the tracker leaves its box hidden until the next seek. Not here: the curves still serve the
  // previous window when the signal fires (see beforeCanvasReplot).
  for (const QwtAxisId axis : {QwtAxisId(QwtPlot::xBottom), QwtAxisId(QwtPlot::yLeft)}) {
    connect(qwtPlot()->axisWidget(axis), &QwtScaleWidget::scaleDivChanged, this, [this]() { trackers_stale_ = true; });
  }
  gesture_undo_debounce_.setSingleShot(true);
  gesture_undo_debounce_.setInterval(kGestureUndoDebounceMs);
  connect(&gesture_undo_debounce_, &QTimer::timeout, this, [this]() {
    if (!loading_state_) {
      emit undoableChange();
    }
  });
  connect(this, &PlotWidgetBase::curveListChanged, this, [this]() {
    if (curveList().empty()) {
      pending_fit_ = PendingFit::kNone;
    }
    if (!restoring_curves_) {
      updateMaximumZoomArea();
    }
    autoZoomPlotVertically();
    pruneHiddenMarkerScopes();
    (void)refreshMarkerScopeRowSet();  // curveListChanged itself is the rebuild trigger
    emit markerTargetsChanged();
    // A removed curve's row and dot leave the tracker at the next replot, not the next seek. Not now: a curve just
    // added has no window yet, and sampling it would copy its whole series.
    trackers_stale_ = true;
  });
  connect(this, &PlotWidgetBase::dragEnterSignal, this, &PlotWidget::onDragEnterEvent);
  connect(this, &PlotWidgetBase::dragLeaveSignal, this, &PlotWidget::onDragLeaveEvent);
  connect(this, &PlotWidgetBase::dropSignal, this, &PlotWidget::onDropEvent);
  buildActions();
  reconnectDataSignals();
}

PlotWidget::~PlotWidget() {
  if (markers_item_ != nullptr) {
    markers_item_->detach();
    delete markers_item_;
  }
  delete tracker_;
  delete reference_tracker_;
  if (show_point_marker_ != nullptr) {
    show_point_marker_->detach();
    delete show_point_marker_;
  }
  if (show_point_text_ != nullptr) {
    show_point_text_->detach();
    delete show_point_text_;
  }
  delete action_split_horizontal_;
  delete action_split_vertical_;
  delete action_remove_all_curves_;
  delete action_zoom_out_;
  delete action_zoom_out_horizontal_;
  delete action_zoom_out_vertical_;
}

void PlotWidget::setDataServices(SessionManager* session, CatalogModel* catalog) {
  if (session_ == session && catalog_ == catalog) {
    return;
  }
  session_ = session;
  catalog_ = catalog;
  if (markers_item_ != nullptr) {
    markers_item_->setSession(session_);
  }
  reconnectDataSignals();
}

PlotWidget::CurveInfo* PlotWidget::addCurve(const QString& name, QColor color) {
  if (session_ == nullptr || catalog_ == nullptr) {
    return nullptr;
  }

  const auto descriptor = catalog_->curveDescriptor(name);
  if (!descriptor.has_value()) {
    return nullptr;
  }

  // Auto-assignment: reuse the curve's remembered colour so it stays consistent across plots; otherwise take the next
  // colour from the configured sequence that this plot does not use yet (freePaletteIndex has the rule, including what
  // happens once the palette is exhausted) and remember it. An explicit (non-transparent) colour, from a layout file
  // for example, is honored as-is. session_ is non-null here (guarded above).
  if (color == Qt::transparent) {
    CurveColorRegistry& registry = session_->curveColorRegistry();
    if (const auto remembered = registry.color(name); remembered.has_value()) {
      color = QColor(*remembered);
    } else {
      // New curve: "global" draws from the session-wide registry counter (one continuous sequence across every plot);
      // "per plot" starts at the curve's positional index in this plot, so each plot restarts at colour 0. Either way
      // the colour is remembered by name, so the curve keeps it on re-add and across plots. The counters know nothing
      // of colours a layout restore or the user put on this plot, which freePaletteIndex reads from the plot.
      QSettings settings;
      const bool global = settings.value(u"Preferences::curve_color_global"_s, true).toBool();
      int per_plot_index = static_cast<int>(curveList().size());
      color = PlotWidgetBase::paletteColor(
          freePaletteIndex([&] { return global ? registry.nextPaletteIndex() : per_plot_index++; }));
      registry.setColor(name, color.name());
    }
  }

  auto* adapter = new DatastoreCurveAdapter(session_, *descriptor);
  auto* info = PlotWidgetBase::addCurve(name, adapter, color, curveDisplayName(*descriptor));
  if (info == nullptr) {
    return nullptr;
  }
  if (tracker_ != nullptr) {
    tracker_->setEnabled(tracker_enabled_);
  }
  replot();
  return info;
}

PlotWidget::CurveInfo* PlotWidget::addCurveFromPending(const QString& name, bool preserve_viewport) {
  const QScopedValueRollback guard(loading_state_, loading_state_ || preserve_viewport);
  return addCurve(name);
}

void PlotWidget::autoZoomPlotVertically() {
  // Skip during layout restore (the saved range wins), with nothing to fit, and for XY plots (their X axis is data, not
  // the shared time axis).
  if (loading_state_ || curveList().empty() || isXYPlot()) {
    return;
  }
  if (!QSettings().value(u"Preferences::auto_zoom_plots"_s, true).toBool()) {
    return;
  }
  // Rescale only Y, over the current X window, so the shared time axis and the other plots stay put.
  const Range<double> range_x = getVisualizationRangeX();
  const Range<double> range_y = getVisualizationRangeY(range_x);
  // Guard a non-finite or degenerate range (a curve that is all NaN/inf over the window): setAxisScale would give a
  // blank axis.
  if (!std::isfinite(range_y.min) || !std::isfinite(range_y.max) || range_y.min >= range_y.max) {
    return;
  }
  setAxisScale(QwtPlot::yLeft, range_y.min, range_y.max);
  replot();
}

void PlotWidget::replaceCurve(const QString& source_key, const QString& output_key) {
  // No-op (header contract) if services are unset or the output is not in the catalog: never drop the existing source
  // curve when the replacement could not be added.
  if (session_ == nullptr || catalog_ == nullptr || !catalog_->curveDescriptor(output_key).has_value()) {
    return;
  }
  // Inherit the source curve's colour, if plotted, so the filtered output takes its place with the same colour.
  QColor color = Qt::transparent;
  for (const CurveInfo& info : curveList()) {
    if (info.source_name == source_key && info.curve != nullptr) {
      color = info.curve->pen().color();
      break;
    }
  }
  // Removing a plot's only curve reads as emptying it, which would drop a fit the output still needs.
  const PendingFit pending_fit = pending_fit_;
  removeCurve(source_key);      // no-op if the source is not currently plotted
  addCurve(output_key, color);  // explicit colour honoured as-is; transparent => palette
  pending_fit_ = pending_fit;
  replot();
}

PlotWidget::CurveInfo* PlotWidget::addCurveXY(
    const QString& x_name, const QString& y_name, const QString& alias, QColor color) {
  if (session_ == nullptr || catalog_ == nullptr) {
    return nullptr;
  }

  const auto x_descriptor = catalog_->curveDescriptor(x_name);
  const auto y_descriptor = catalog_->curveDescriptor(y_name);
  if (!x_descriptor.has_value() || !y_descriptor.has_value()) {
    return nullptr;
  }

  // The alias is the curve's display title (the legend and the title curveFromTitle() keys on). An empty alias gives an
  // auto "Y vs X" title (non-interactive callers).
  const QString title =
      alias.isEmpty() ? tr("%1 vs %2").arg(curveDisplayName(*y_descriptor), curveDisplayName(*x_descriptor)) : alias;
  auto* series = new PointSeriesXY(session_, *x_descriptor, *y_descriptor);
  auto* info = PlotWidgetBase::addCurve(title, series, color);
  if (info == nullptr) {
    return nullptr;
  }
  if (tracker_ != nullptr) {
    tracker_->setEnabled(false);
  }
  // Clears the blue tracker and the red tracker's reference_pos_: otherwise returning to time-series mode would revive
  // stale deltas without a blue line.
  setReferenceLine(std::nullopt);
  // Style and width are plot-level: addCurve() above applied the plot's current ones. XY plots are not forced to Dots;
  // they inherit the plot-level style, which the Curve Style toolbar controls. The curveListChanged handler has
  // already recomputed the max zoom area.
  // Reveal the ride-along marker at the current cursor at once, not on the next seek or tick (setTrackerPosition
  // replots).
  setTrackerPosition(last_tracker_time_sec_);
  return info;
}

#ifndef PJ_TARGET_WASM
PlotWidget::CurveInfo* PlotWidget::createCurveXYInteractive(const QString& x_key, const QString& y_key) {
  if (catalog_ == nullptr) {
    return nullptr;
  }
  const auto x_descriptor = catalog_->curveDescriptor(x_key);
  const auto y_descriptor = catalog_->curveDescriptor(y_key);
  if (!x_descriptor.has_value() || !y_descriptor.has_value()) {
    return nullptr;
  }

  XYCurveDialog dialog(curveDisplayName(*x_descriptor), curveDisplayName(*y_descriptor), this);
  while (dialog.exec() == QDialog::Accepted) {
    const QString alias = dialog.alias();
    if (curveFromTitle(alias) != nullptr) {
      MessageBox::warning(
          this, tr("Duplicate name"), tr("A curve named \"%1\" already exists in this plot.").arg(alias));
      continue;
    }
    // The dialog reports whether the user swapped X and Y relative to the arguments.
    const QString final_x = dialog.swapped() ? y_key : x_key;
    const QString final_y = dialog.swapped() ? x_key : y_key;
    return addCurveXY(final_x, final_y, alias);
  }
  return nullptr;  // cancelled
}
#else
void PlotWidget::createCurveXYInteractiveAsync(
    const QString& x_key, const QString& y_key, std::function<void(CurveInfo*)> on_finished) {
  if (catalog_ == nullptr) {
    if (on_finished) {
      on_finished(nullptr);
    }
    return;
  }
  const auto x_descriptor = catalog_->curveDescriptor(x_key);
  const auto y_descriptor = catalog_->curveDescriptor(y_key);
  if (!x_descriptor.has_value() || !y_descriptor.has_value()) {
    if (on_finished) {
      on_finished(nullptr);
    }
    return;
  }

  // Heap-owned and opened asynchronously: QDialog::exec() enters a nested event loop, which an Asyncify-free Qt/WASM
  // build cannot suspend. The dialog stays alive across duplicate-name retries so the user's Swap and alias edits
  // survive, as in the desktop while(exec()) loop.
  auto* dialog = new XYCurveDialog(curveDisplayName(*x_descriptor), curveDisplayName(*y_descriptor), this);
  connect(
      dialog, &QDialog::finished, this,
      [this, dialog, x_key, y_key, on_finished = std::move(on_finished)](int result) mutable {
        if (result != QDialog::Accepted) {
          dialog->deleteLater();
          if (on_finished) {
            auto completion = std::move(on_finished);
            completion(nullptr);
          }
          return;
        }

        const QString alias = dialog->alias();
        if (curveFromTitle(alias) != nullptr) {
          // MessageBox::warning() is synchronous too: reproduce it with the same instance UI and reopen the XY dialog
          // only after dismissal.
          auto* warning = new MessageBox(this);
          warning->setAttribute(Qt::WA_DeleteOnClose);
          warning->setTitle(tr("Duplicate name"));
          warning->setText(tr("A curve named \"%1\" already exists in this plot.").arg(alias));
          warning->addButton(MessageBox::tr("OK"), MessageBox::kPrimaryRole);
          connect(warning, &QDialog::finished, dialog, [dialog](int) {
            dialog->setWindowModality(Qt::ApplicationModal);
            dialog->show();
          });
          // QDialog::open() downgrades ApplicationModal to WindowModal; show() keeps desktop exec()'s app-wide modality
          // without nesting.
          warning->setWindowModality(Qt::ApplicationModal);
          warning->show();
          return;
        }

        const QString final_x = dialog->swapped() ? y_key : x_key;
        const QString final_y = dialog->swapped() ? x_key : y_key;
        CurveInfo* created = addCurveXY(final_x, final_y, alias);
        dialog->deleteLater();
        if (on_finished) {
          auto completion = std::move(on_finished);
          completion(created);
        }
      });
  dialog->setWindowModality(Qt::ApplicationModal);
  dialog->show();
}
#endif

void PlotWidget::setVisibleXRange(double x0, double x1) {
  // An empty window (a linked peer still waiting for rows) imposes nothing to keep.
  if (pending_fit_ == PendingFit::kFull && x0 < x1) {
    pending_fit_ = PendingFit::kVertical;
  }
  QRectF rect = currentBoundingRect();
  rect.setLeft(x0);
  rect.setRight(x1);
  setZoomRectangle(rect, false);
  replot();
}

void PlotWidget::setSeriesStatus(std::vector<SeriesCurveStatus> statuses) {
  if (statuses != series_status_) {
    series_status_ = std::move(statuses);
    applySeriesStatus();
  }
}

void PlotWidget::applySeriesStatus() {
  bool changed = false;
  for (const CurveInfo& info : curveList()) {
    auto* curve = dynamic_cast<PlotCurve*>(info.curve);
    if (curve == nullptr) {
      continue;
    }
    QString suffix;
    const std::optional<CatalogItem> item =
        series_status_.empty() || catalog_ == nullptr ? std::nullopt : catalog_->itemDescriptor(info.source_name);
    for (const SeriesCurveStatus& status : series_status_) {
      if (item && status.dataset_id == item->dataset_id && status.topic_name == item->topic_name) {
        suffix = status.computing_percent ? tr("computing %1%").arg(*status.computing_percent) : tr("not computed");
      }
    }
    changed = curve->setLegendSuffix(suffix) || changed;
  }
  if (changed) {
    replot();
  }
}

void PlotWidget::setZoomRectangle(QRectF rect, bool emit_signal) {
  if (isXYPlot() && keepRatioXY()) {
    // Every programmatic "return to original zoom" funnels through here (zoomOut, the H/V zoom-outs, XML restore): keep
    // the requested zoom level and re-impose the canvas aspect ratio so the XY shape stays 1:1.
    applyRectKeepingRatio(rect);
  } else {
    setAxisScale(QwtPlot::yLeft, rect.bottom(), rect.top());
    setAxisScale(QwtPlot::xBottom, rect.left(), rect.right());
    qwtPlot()->updateAxes();
  }

  if (emit_signal) {
    if (isXYPlot()) {
      emit undoableChange();
    } else {
      emit rectChanged(this, rect);
    }
  }
}

bool PlotWidget::isZoomLinkEnabled() const noexcept {
  return true;
}

void PlotWidget::setTrackerEnabled(bool enabled) {
  tracker_enabled_ = enabled;
  if (tracker_ != nullptr) {
    tracker_->setEnabled(enabled && !isXYPlot());
  }
  replot();
}

void PlotWidget::setReferenceLine(std::optional<double> reference_x_sec) {
  if (reference_tracker_ == nullptr || tracker_ == nullptr) {
    return;
  }
  if (isXYPlot() || !reference_x_sec.has_value()) {
    reference_tracker_->setEnabled(false);
    tracker_->setReferencePosition(std::nullopt);
  } else {
    const QPointF reference_point(*reference_x_sec, 0.0);
    reference_tracker_->setEnabled(true);
    reference_tracker_->setPosition(reference_point);
    tracker_->setReferencePosition(reference_point);
  }
  replot();
}

bool PlotWidget::trackerEnabled() const noexcept {
  return tracker_enabled_;
}

void PlotWidget::setTrackerParameter(CurveTracker::Parameter parameter) {
  if (tracker_ != nullptr) {
    tracker_->setParameter(parameter);
  }
}

CurveTracker::Parameter PlotWidget::trackerParameter() const noexcept {
  return tracker_ != nullptr ? tracker_->parameter() : CurveTracker::kValue;
}

bool PlotWidget::trackerValueBoxVisible() const noexcept {
  return tracker_ != nullptr && tracker_->valueBoxVisible();
}

bool PlotWidget::trackerValueBoxGliding() const noexcept {
  return tracker_ != nullptr && tracker_->valueBoxGliding();
}

void PlotWidget::beforeCanvasReplot() {
  if (trackers_stale_) {
    trackers_stale_ = false;
    redrawTrackers();
  }
}

void PlotWidget::changeEvent(QEvent* event) {
  PlotWidgetBase::changeEvent(event);  // re-derives the canvas colour first
  if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange) {
    redrawTrackers();
  }
}

void PlotWidget::redrawTrackers() {
  if (isXYPlot()) {
    updateXYMarkers(last_tracker_time_sec_);
  }
  if (tracker_ != nullptr) {
    tracker_->redraw();
  }
  if (reference_tracker_ != nullptr) {
    reference_tracker_->redraw();
  }
}

std::vector<ValueBoxRow> PlotWidget::trackerValueBoxRows() const {
  return tracker_ != nullptr ? tracker_->valueBoxRows() : std::vector<ValueBoxRow>{};
}

QString PlotWidget::trackerValueBoxText() const {
  return tracker_ != nullptr ? tracker_->valueBoxText() : QString();
}

void PlotWidget::setShowPoints(bool show) {
  show_points_ = show;
  if (!show) {
    if (show_point_marker_ != nullptr) {
      show_point_marker_->setVisible(false);
    }
    if (show_point_text_ != nullptr) {
      show_point_text_->setVisible(false);
    }
    replot();
  }
}

bool PlotWidget::showPoints() const noexcept {
  return show_points_;
}

bool PlotWidget::pointInspectorVisible() const noexcept {
  return show_point_marker_ != nullptr && show_point_text_ != nullptr && show_point_marker_->isVisible() &&
         show_point_text_->isVisible();
}

QPointF PlotWidget::pointInspectorPosition() const noexcept {
  return pointInspectorVisible() ? show_point_marker_->value() : QPointF{};
}

QString PlotWidget::pointInspectorLabel() const {
  return pointInspectorVisible() ? show_point_text_->label().text() : QString{};
}

void PlotWidget::showPointValues(QPoint paint_point) {
  if (!show_points_ || show_point_marker_ == nullptr || show_point_text_ == nullptr) {
    return;
  }

  auto paint_to_plot = [this](QPoint p) {
    return QPointF(qwtPlot()->invTransform(QwtPlot::xBottom, p.x()), qwtPlot()->invTransform(QwtPlot::yLeft, p.y()));
  };
  auto plot_to_paint = [this](QPointF p) {
    return QPoint(qwtPlot()->transform(QwtPlot::xBottom, p.x()), qwtPlot()->transform(QwtPlot::yLeft, p.y()));
  };

  const QPointF mouse_in_plot = paint_to_plot(paint_point);
  const int precision = valuePrecision();

  QString text;
  int min_distance_sqr = kHoverHitRadiusPx * kHoverHitRadiusPx;
  bool updated = false;
  QPointF marker_point;
  const QwtPlotItemList curves = qwtPlot()->itemList(QwtPlotItem::Rtti_PlotCurve);
  for (auto* item : curves) {
    auto* curve = dynamic_cast<QwtPlotCurve*>(item);
    if (curve == nullptr || !curve->isVisible()) {
      continue;
    }
    const auto maybe_point = curvePointAt(curve, mouse_in_plot.x());
    if (!maybe_point) {
      continue;
    }
    const QPoint sample_paint = plot_to_paint(*maybe_point);
    const QPoint diff = sample_paint - paint_point;
    const int dist_sqr = diff.x() * diff.x() + diff.y() * diff.y();
    if (dist_sqr < min_distance_sqr) {
      updated = true;
      min_distance_sqr = dist_sqr;
      marker_point = *maybe_point;
      text =
          QString("<font color=%1>%2<br>x: %3<br>y: %4</font>")
              .arg(
                  curve->pen().color().name(), curve->title().text(), QString::number(maybe_point->x(), 'f', precision),
                  QString::number(maybe_point->y(), 'f', precision));
    }
  }

  const bool was_visible = show_point_marker_->isVisible();
  show_point_marker_->setVisible(updated);
  show_point_text_->setVisible(updated);

  if (updated) {
    show_point_marker_->setValue(marker_point);

    QwtText label;
    label.setText(text);
    label.setBorderPen(QPen(Qt::NoPen));
    QColor background = qwtPlot()->canvasBackground().color();
    background.setAlpha(200);
    label.setBackgroundBrush(background);
    QFont font = QApplication::font();
    font.setPointSize(9);
    label.setFont(font);
    label.setRenderFlags(Qt::AlignLeft);
    show_point_text_->setLabel(label);
    show_point_text_->setLabelAlignment(Qt::AlignRight);

    const QPoint marker_paint = plot_to_paint(marker_point);
    QPoint text_anchor = marker_paint + QPoint(15, -20);
    const double text_width = label.textSize().width();
    const double canvas_width = qwtPlot()->canvas()->width();
    if (marker_paint.x() > canvas_width * 0.5 && (text_anchor.x() + text_width) > canvas_width) {
      text_anchor = marker_paint + QPoint(-15 - static_cast<int>(text_width), -20);
    }
    show_point_text_->setValue(paint_to_plot(text_anchor));
  }

  // Replot only when the steady state changed: visibility flipped, a different sample was snapped, or the tooltip text
  // differs (a curve appeared or disappeared at the same x).
  const bool visibility_changed = updated != was_visible;
  const bool position_or_text_changed =
      updated && (marker_point != show_point_last_pos_ || text != show_point_last_text_);
  if (visibility_changed || position_or_text_changed) {
    replot();
  }
  show_point_last_pos_ = updated ? marker_point : QPointF{};
  show_point_last_text_ = updated ? text : QString{};
}

QString PlotWidget::stateId() const {
  return state_id_;
}

void PlotWidget::setStateId(QString id) {
  if (!id.isEmpty()) {
    state_id_ = std::move(id);
  }
}

QDomElement PlotWidget::xmlSaveState(QDomDocument& doc) const {
  QDomElement plot_element = doc.createElement(u"plot"_s);
  plot_element.setAttribute(u"id"_s, state_id_);
  plot_element.setAttribute(u"mode"_s, isXYPlot() ? u"XYPlot"_s : u"TimeSeries"_s);
  plot_element.setAttribute(u"line_width"_s, lineWidthToString(lineWidth()));
  // Style and width are plot-level (every curve shares them; only colour is per curve), so they are saved once on the
  // <plot>, not per <curve>.
  plot_element.setAttribute(u"style"_s, curveStyleToString(curveStyle()));
  plot_element.setAttribute(u"title"_s, qwtPlot()->title().text());
  plot_element.setAttribute(u"tracker_enabled"_s, tracker_enabled_ ? u"true"_s : u"false"_s);

  // Skip <range> when the canvas has not computed a real viewport (a drop right before save): a degenerate rect would
  // restore as a zero-width window and hide everything. Without <range>, xmlLoadState falls back to zoomOut(), which
  // auto-fits.
  const QRectF rect = requestedView();
  if (rect.left() != rect.right() && rect.top() != rect.bottom()) {
    QDomElement range_element = doc.createElement(u"range"_s);
    range_element.setAttribute(u"bottom"_s, QString::number(rect.bottom(), 'f', 6));
    range_element.setAttribute(u"top"_s, QString::number(rect.top(), 'f', 6));
    // An explicit x_basis marker records what the X coordinates mean, so load never infers it from the plot mode:
    // "absolute" = time-series time, "value" = an XY plot's raw data value.
    if (isXYPlot()) {
      // XY X is a data value, not time: offset-independent, stored verbatim.
      range_element.setAttribute(u"left"_s, QString::number(rect.left(), 'f', 6));
      range_element.setAttribute(u"right"_s, QString::number(rect.right(), 'f', 6));
      range_element.setAttribute(plot_xml::kXBasisAttribute, plot_xml::kXBasisValue);
    } else {
      // A time-series X axis is persisted in absolute seconds, not the display-relative seconds the Qwt axis speaks
      // (display = absolute - offset; pj_runtime/Time.h). The display offset is per dataset and live, so a display
      // range frames the right instant only for the offset present at save time; absolute stays correct whatever the
      // offset state at load (the "Use time offset" toggle, reloaded data, a layout shared between machines).
      // The authoritative edges are integer nanoseconds: quantize the small display value (rect.left()/right(), a few
      // seconds) to ns first, then add the epoch-scale offset in the integer domain. Adding the offset as a double
      // first would round away a deeply zoomed window (a double's ULP at ~1.6e9 s is ~238 ns).
      const qint64 offset_ns = displayOffsetNanoseconds();
      const qint64 absolute_left_ns =
          static_cast<qint64>(std::llround(rect.left() * kNanosecondsPerSecond)) + offset_ns;
      const qint64 absolute_right_ns =
          static_cast<qint64>(std::llround(rect.right() * kNanosecondsPerSecond)) + offset_ns;
      // The decimal left/right stay for readability and as the v3 fallback; they carry the same (lossy) absolute
      // seconds.
      range_element.setAttribute(
          u"left"_s, QString::number(static_cast<double>(absolute_left_ns) / kNanosecondsPerSecond, 'f', 6));
      range_element.setAttribute(
          u"right"_s, QString::number(static_cast<double>(absolute_right_ns) / kNanosecondsPerSecond, 'f', 6));
      range_element.setAttribute(u"left_ns"_s, QString::number(absolute_left_ns));
      range_element.setAttribute(u"right_ns"_s, QString::number(absolute_right_ns));
      range_element.setAttribute(plot_xml::kXBasisAttribute, plot_xml::kXBasisAbsolute);
    }
    plot_element.appendChild(range_element);
  }

  // A curve is identified by its stable topic + field path (PJ::LayoutXml), not the engine's opaque per-load catalog
  // key. On load (layout file or undo/redo snapshot) the path is re-resolved against the current dataset(s) to the
  // concrete key (PJ::LayoutXml::rebindCurveKeys), so the saved form stays valid across reloads and similar datasets.
  const auto write_stable_path = [&](QDomElement& element, const QString& prefix, const QString& key) {
    if (catalog_ == nullptr) {
      return;
    }
    if (const auto descriptor = catalog_->curveDescriptor(key); descriptor.has_value()) {
      element.setAttribute(prefix + "topic"_L1, descriptor->topic_name);
      element.setAttribute(prefix + "field"_L1, descriptor->field_path);
      stampDatasetIdentity(element, descriptor->dataset_id, prefix);
    }
  };

  for (const CurveInfo& info : curveList()) {
    if (info.curve == nullptr) {
      continue;
    }
    QDomElement curve_element = doc.createElement(u"curve"_s);
    curve_element.setAttribute(u"color"_s, info.curve->pen().color().name());
    curve_element.setAttribute(u"visible"_s, info.curve->isVisible() ? u"true"_s : u"false"_s);
    curve_element.setAttribute(u"markers_visible"_s, info.show_markers ? u"true"_s : u"false"_s);
    if (auto* xy_series = dynamic_cast<PointSeriesXY*>(info.curve->data())) {
      // An XY curve's title is the user alias (not derivable from x/y), so it is persisted.
      curve_element.setAttribute(u"name"_s, info.source_name);
      write_stable_path(curve_element, u"x_"_s, xy_series->xSource().name);
      write_stable_path(curve_element, u"y_"_s, xy_series->ySource().name);
    } else {
      write_stable_path(curve_element, {}, info.source_name);
    }
    plot_element.appendChild(curve_element);
  }

  saveHiddenMarkerScopes(doc, plot_element);

  for (const QDomDocument& pending_doc : pending_curve_intents_) {
    const QDomElement pending = pending_doc.documentElement();
    if (!pending.isNull()) {
      plot_element.appendChild(doc.importNode(pending, /*deep=*/true));
    }
  }

  return plot_element;
}

bool PlotWidget::rememberPendingCurveIntent(const QDomElement& curve_element, bool notify) {
  if (!isPendingCurveIntentValid(curve_element, isXYPlot())) {
    return false;
  }
  const QString identity = pendingCurveIdentity(curve_element);
  for (const QDomDocument& existing : pending_curve_intents_) {
    if (pendingCurveIdentity(existing.documentElement()) == identity) {
      return false;
    }
  }
  QDomDocument pending_doc;
  QDomElement pending = pending_doc.importNode(curve_element, /*deep=*/true).toElement();
  pending.setTagName(u"curve"_s);
  plot_xml::markPendingIntent(pending);
  if (pending.attribute(u"x_topic"_s).isEmpty()) {
    pending.removeAttribute(u"name"_s);
  }
  pending.removeAttribute(u"curve_x"_s);
  pending.removeAttribute(u"curve_y"_s);
  pending_doc.appendChild(pending);
  pending_curve_intents_.push_back(std::move(pending_doc));
  emit pendingCurveIntentsChanged();
  if (notify && !loading_state_) {
    emit undoableChange();
  }
  return true;
}

void PlotWidget::forgetPendingCurveIntent(const QDomElement& curve_element, bool notify) {
  const QString identity = pendingCurveIdentity(curve_element);
  const std::size_t old_size = pending_curve_intents_.size();
  pending_curve_intents_.erase(
      std::remove_if(
          pending_curve_intents_.begin(), pending_curve_intents_.end(),
          [&](const QDomDocument& pending_doc) {
            return pendingCurveIdentity(pending_doc.documentElement()) == identity;
          }),
      pending_curve_intents_.end());
  if (pending_curve_intents_.size() == old_size) {
    return;
  }
  emit pendingCurveIntentsChanged();
  if (notify && !loading_state_) {
    emit undoableChange();
  }
}

bool PlotWidget::parseState(const QDomElement& plot_element, ParsedState& parsed) {
  if (plot_element.isNull() || plot_element.tagName() != "plot"_L1) {
    XmlOptionsRecorder::noteRejection(plot_element, {}, u"expected a <plot> element"_s);
    return false;
  }
  parsed.id = readXmlText(plot_element, u"id"_s);
  QString mode;
  static_cast<void>(readXmlEnum(
      plot_element, u"mode"_s, {u"TimeSeries"_s, u"XYPlot"_s}, u"TimeSeries"_s, mode, XmlBadValue::kTolerate));
  parsed.xy = mode == "XYPlot"_L1;

  std::set<QString> pending_identities;
  for (QDomElement curve = plot_element.firstChildElement(u"curve"_s); !curve.isNull();
       curve = curve.nextSiblingElement(u"curve"_s)) {
    parsed.curves.push_back(parseCurveElement(curve));
    if (!plot_xml::isPendingIntent(curve)) {
      // A layout load keeps a curve of the other shape, which the plot then draws wrong or drops.
      const bool xy_curve = curve.hasAttribute(u"x_topic"_s) || curve.hasAttribute(u"y_topic"_s) ||
                            curve.hasAttribute(u"curve_x"_s) || curve.hasAttribute(u"curve_y"_s);
      if (xy_curve != parsed.xy) {
        XmlOptionsRecorder::noteRejection(
            plot_element, u"mode"_s,
            parsed.xy ? u"a time-series curve in an XYPlot"_s : u"an XY curve in a TimeSeries plot"_s);
      }
      continue;
    }
    const bool resolved = parsed.xy
                              ? !curve.attribute(u"curve_x"_s).isEmpty() && !curve.attribute(u"curve_y"_s).isEmpty()
                              : !curve.attribute(u"name"_s).isEmpty();
    if (resolved) {
      continue;
    }
    if (!isPendingCurveIntentValid(curve, parsed.xy)) {
      return false;
    }
    if (!pending_identities.insert(pendingCurveIdentity(curve)).second) {
      XmlOptionsRecorder::noteRejection(curve, {}, u"duplicates an earlier pending curve"_s);
      return false;
    }
    QDomDocument pending_doc;
    pending_doc.appendChild(pending_doc.importNode(curve, /*deep=*/true));
    parsed.pending_curves.push_back(std::move(pending_doc));
  }

  // Line width is plot-level and stored on the <plot>. A layout whose plot value is absent or the default ("1.0") and
  // that stored the width per <curve> falls back to the first saved curve's pixel width. The per-curve legacy
  // attributes are read unreported: no saved state carries them, and advertising them would invite per-curve values
  // only the first curve's can set.
  static const QStringList width_tokens = [] {
    QStringList tokens;
    for (int width = static_cast<int>(LineWidth::kPoints10); width <= static_cast<int>(LineWidth::kPoints30); ++width) {
      tokens.push_back(lineWidthToString(static_cast<LineWidth>(width)));
    }
    return tokens;
  }();
  const QString width = readReported(
      plot_element, {.attribute = u"line_width"_s,
                     .type = u"enum"_s,
                     .values = width_tokens,
                     .fallback = lineWidthToString(LineWidth::kPoints10)});
  const QDomElement first_curve = plot_element.firstChildElement(u"curve"_s);
  if ((width.isEmpty() || width == lineWidthToString(LineWidth::kPoints10)) &&
      first_curve.hasAttribute(u"line_width"_s)) {
    parsed.line_width = lineWidthFromPixels(first_curve.attribute(u"line_width"_s).toDouble());
  } else {
    parsed.line_width = lineWidthFromString(width);
  }
  // Style is plot-level and stored on the <plot>; a layout that stored it per <curve> falls back to the first saved
  // curve.
  static const QStringList style_tokens = [] {
    QStringList tokens;
    for (int style = kLines; style <= kStepsInverted; ++style) {
      tokens.push_back(curveStyleToString(static_cast<CurveStyle>(style)));
    }
    return tokens;
  }();
  const QString legacy_style = first_curve.attribute(u"style"_s, curveStyleToString(kLines));
  QString style = readReported(
      plot_element, {.attribute = u"style"_s, .type = u"enum"_s, .values = style_tokens, .fallback = legacy_style});
  parsed.style = curveStyleFromString(style.isEmpty() ? legacy_style : style);
  // Raw: a layout load reads anything but "true" as false.
  parsed.tracker_enabled =
      readReported(
          plot_element,
          {.attribute = u"tracker_enabled"_s, .type = u"bool"_s, .values = kTrueFalse, .fallback = u"true"_s}) ==
      "true"_L1;
  parsed.title = readXmlText(plot_element, u"title"_s);

  if (const QDomElement range = plot_element.firstChildElement(u"range"_s); !range.isNull()) {
    // Raw: a layout load reads an unparsable edge as 0 and keeps a non-finite one.
    const auto edge = [&range](const QString& attribute) {
      return readReported(range, {.attribute = attribute, .type = u"float"_s, .fallback = u"0"_s}).toDouble();
    };
    SavedViewport view;
    view.bottom = edge(u"bottom"_s);
    view.top = edge(u"top"_s);
    view.left = edge(u"left"_s);
    view.right = edge(u"right"_s);
    // x_basis is authoritative: "value" = XY data value, anything else (a v3 layout with no marker is annotated
    // "absolute" by normalizePlotRangeBasis on load) = absolute time. Never re-infer it from the plot mode.
    QString x_basis;
    static_cast<void>(readXmlEnum(
        range, plot_xml::kXBasisAttribute, {plot_xml::kXBasisAbsolute, plot_xml::kXBasisValue},
        plot_xml::kXBasisAbsolute, x_basis, XmlBadValue::kTolerate));
    view.x_is_absolute = x_basis != plot_xml::kXBasisValue;
    // Prefer the integer-ns edges (schema v4+): they round-trip an epoch-scale value exactly, which the decimal
    // left/right of a v3 layout cannot. They are read on any plot; an XY plot's value axis ignores them.
    bool left_ok = false;
    bool right_ok = false;
    const qint64 left_ns = readReported(range, {.attribute = u"left_ns"_s, .type = u"int"_s}).toLongLong(&left_ok);
    const qint64 right_ns = readReported(range, {.attribute = u"right_ns"_s, .type = u"int"_s}).toLongLong(&right_ok);
    if (view.x_is_absolute && left_ok && right_ok) {
      view.left_ns = left_ns;
      view.right_ns = right_ns;
    }
    parsed.viewport = view;
  }

  // Report-only: loadHiddenMarkerScopes parses them against the catalog.
  if (XmlOptionsRecorder::active()) {
    for (QDomElement marker_scope = plot_element.firstChildElement(u"marker_scope"_s); !marker_scope.isNull();
         marker_scope = marker_scope.nextSiblingElement(u"marker_scope"_s)) {
      parseHiddenMarkerScope(marker_scope);
    }
  }
  return true;
}

bool PlotWidget::xmlValidateState(const QDomElement& plot_element) const {
  ParsedState parsed;
  return parseState(plot_element, parsed);
}

bool PlotWidget::xmlLoadState(const QDomElement& plot_element, bool autozoom) {
  ParsedState parsed;
  if (!parseState(plot_element, parsed)) {
    return false;
  }
  setStateId(parsed.id);
  setModeXY(parsed.xy);
  setLineWidth(parsed.line_width);
  // setDefaultStyle restyles the plot and is inherited by curves added after the load.
  setDefaultStyle(parsed.style);
  setTrackerEnabled(parsed.tracker_enabled);
  qwtPlot()->setTitle(parsed.title);

  const bool was_loading_state = loading_state_;
  loading_state_ = true;
  // A wheel or pan armed just before the restore must not publish the restored state as a new undo step.
  gesture_undo_debounce_.stop();
  const bool intents_changing = !pending_curve_intents_.empty() || !parsed.pending_curves.empty();
  pending_curve_intents_ = std::move(parsed.pending_curves);
  if (intents_changing) {
    emit pendingCurveIntentsChanged();
  }

  std::vector<QString> desired_order;
  for (const ParsedCurve& curve : parsed.curves) {
    desired_order.push_back(
        isXYPlot() && curve.has_xy_keys ? curveKey(curve.name, curve.curve_x, curve.curve_y) : curveKey(curve.name));
  }
  const std::set<QString> desired_keys(desired_order.begin(), desired_order.end());

  QStringList remove_titles;
  QStringList kept_titles;
  std::vector<QString> kept_keys;
  for (const CurveInfo& info : curveList()) {
    if (info.curve == nullptr) {
      continue;
    }
    QString existing_key = curveKey(info.source_name);
    if (auto* xy_series = dynamic_cast<PointSeriesXY*>(info.curve->data())) {
      existing_key = curveKey(info.source_name, xy_series->xSource().name, xy_series->ySource().name);
    }
    if (desired_keys.find(existing_key) == desired_keys.end()) {
      remove_titles.push_back(info.curve->title().text());
    } else {
      kept_titles.push_back(info.curve->title().text());
      kept_keys.push_back(existing_key);
    }
  }
  // Missing curves are appended after the kept ones, so the saved order (legend, editor rows, draw order) holds only
  // when the kept curves are its leading entries; otherwise rebuild this plot's curves from the XML.
  if (kept_keys.size() > desired_order.size() ||
      !std::equal(kept_keys.begin(), kept_keys.end(), desired_order.begin())) {
    remove_titles += kept_titles;
  }
  {
    // Each recompute reads every sample of every curve: once per plot, not once per curve change.
    const QScopedValueRollback defer(restoring_curves_, true);
    for (const QString& title : remove_titles) {
      removeCurve(title);
    }
    for (const ParsedCurve& curve : parsed.curves) {
      applyParsedCurve(curve, /*preserve_viewport=*/false);
    }
  }
  updateMaximumZoomArea();

  loadHiddenMarkerScopes(plot_element);

  // Stash the layout-saved viewport (raw, before the offset conversion) and frame to it through the shared helper.
  // During a progressive restore the catalog is still empty here, so the absolute -> display conversion is wrong until
  // the dataset binds; the progressive path re-applies the stash per curve bind and at drain (applySavedViewportOrZoom,
  // PendingCurveBinder). A blocking load resolves correctly on this first call, the offset being known.
  saved_viewport_ = autozoom ? parsed.viewport : std::nullopt;
  applySavedViewportOrZoom(/*clear_after=*/false);
  replot();
  loading_state_ = was_loading_state;
  return true;
}

void PlotWidget::applySavedViewportOrZoom(bool clear_after) {
  QRectF rect;
  // The layout stores a time axis in absolute seconds; the per-dataset display offset is applied here, never persisted:
  // the saved X converts back to the current display coordinate (display = absolute - offset). An XY plot's X is a data
  // value, offset-independent, used verbatim. Re-running this as the offset settles is what lets a progressive restore
  // pin the saved window up front and keep it framed while data streams in.
  if (saved_viewport_.has_value()) {
    const SavedViewport& view = *saved_viewport_;
    double left = 0.0;
    double right = 0.0;
    if (view.x_is_absolute && view.left_ns.has_value() && view.right_ns.has_value()) {
      // Authoritative path (schema v4+): subtract the display offset in the integer ns domain, then convert the small
      // display value to seconds. Converting the epoch-scale absolute value to a double first would round away a few-ns
      // window (a double's ULP at ~1.6e9 s is ~238 ns) and a deep-zoom viewport would drift.
      const qint64 left_ns = *view.left_ns;
      const qint64 right_ns = *view.right_ns;
      const bool degenerate = left_ns == right_ns;
      const qint64 offset_ns = displayOffsetNanoseconds();
      left = static_cast<double>(left_ns - offset_ns) / kNanosecondsPerSecond;
      right = static_cast<double>(right_ns - offset_ns) / kNanosecondsPerSecond;
      // Degenerate-range tolerance: a window saved zoomed below 1 ns rounds to equal ns edges, and falling through to
      // auto-fit would wedge undo/redo after a deep zoom (the snapshot could never restore). Nudge the collapsed edges
      // by +-0.5 ns so a real, if sub-pixel, window is restored. With the offset on, the display value is near zero and
      // that yields two distinct doubles; with "Use time offset" off it sits at epoch scale (~1.6e9 s), where 0.5 ns is
      // far below the double ULP (~2.4e-7 s) and the edges collapse again, so a still-degenerate window widens to the
      // ULP-aware minimum around its center. The magnifier's floor keeps live zooming out of this regime; this only
      // rescues an already persisted degenerate range.
      if (degenerate) {
        const double center = 0.5 * (left + right);
        left = center - 0.5e-9;
        right = center + 0.5e-9;
        if (left == right) {
          const double half_width = 0.5 * plotting_detail::ulpAwareMinTimeXWidthSec(center);
          left = center - half_width;
          right = center + half_width;
        }
      }
    } else {
      // Fallback: a v3 decimal-only layout, or an XY value axis. The decimal seconds are the only representation; an XY
      // axis is offset-independent (used verbatim).
      left = view.left;
      right = view.right;
      if (view.x_is_absolute) {
        const double offset_sec = displayOffsetSeconds();
        left -= offset_sec;
        right -= offset_sec;
      }
    }
    rect.setBottom(view.bottom);
    rect.setTop(view.top);
    rect.setLeft(left);
    rect.setRight(right);
  }
  // A degenerate or missing saved range auto-fits (fresh load, or a layout saved before the canvas had a viewport).
  if (rect.left() == rect.right() || rect.top() == rect.bottom()) {
    zoomOut(/*emit_signal=*/false);
  } else {
    pending_fit_ = PendingFit::kNone;
    setZoomRectangle(rect, /*emit_signal=*/false);
  }
  if (clear_after) {
    saved_viewport_.reset();
  }
}

PlotWidget::CurveInfo* PlotWidget::applyCurveElement(const QDomElement& curve_element, bool preserve_viewport) {
  return applyParsedCurve(parseCurveElement(curve_element), preserve_viewport);
}

PlotWidget::CurveInfo* PlotWidget::applyParsedCurve(const ParsedCurve& parsed, bool preserve_viewport) {
  CurveInfo* loaded_curve = nullptr;
  if (isXYPlot() && parsed.has_xy_keys) {
    for (CurveInfo& info : curveList()) {
      if (auto* xy_series = info.curve != nullptr ? dynamic_cast<PointSeriesXY*>(info.curve->data()) : nullptr) {
        if (curveKey(info.source_name, xy_series->xSource().name, xy_series->ySource().name) ==
            curveKey(parsed.name, parsed.curve_x, parsed.curve_y)) {
          loaded_curve = &info;
          break;
        }
      }
    }
    if (loaded_curve == nullptr) {
      // Restore the saved alias as the title; no dialog on load.
      const QScopedValueRollback guard(loading_state_, loading_state_ || preserve_viewport);
      loaded_curve = addCurveXY(
          parsed.curve_x, parsed.curve_y, parsed.name, parsed.color.isValid() ? parsed.color : Qt::transparent);
    }
  } else {
    loaded_curve = curveFromTitle(parsed.name);
    if (loaded_curve == nullptr) {
      const QScopedValueRollback guard(loading_state_, loading_state_ || preserve_viewport);
      loaded_curve = addCurve(parsed.name, parsed.color.isValid() ? parsed.color : Qt::transparent);
    }
  }
  if (loaded_curve != nullptr && loaded_curve->curve != nullptr && parsed.color.isValid()) {
    loaded_curve->curve->setPen(parsed.color, loaded_curve->curve->pen().widthF());
    // Seed the session colour memory so this curve keeps its saved colour when later dragged into another plot.
    // Time-series only, as in onChangeCurveColor.
    if (CurveColorRegistry* registry = colorRegistryOf(session_); registry != nullptr && !isXYPlot()) {
      registry->setColor(loaded_curve->source_name, parsed.color.name());
    }
  }
  // Style and width are plot-level (restored once in xmlLoadState); only colour and visibility are per curve.
  if (loaded_curve != nullptr) {
    loaded_curve->curve->setVisible(parsed.visible);
    trackers_stale_ = true;  // a kept curve's colour or visibility may have changed; the caller replots
    loaded_curve->show_markers = parsed.markers_visible;
  }
  return loaded_curve;
}

void PlotWidget::zoomOut(bool emit_signal) {
  if (curveList().empty()) {
    setZoomRectangle(QRectF(0, 1, 1, -1), false);
    return;
  }

  pending_fit_ = hasUsableCurveData() ? PendingFit::kNone : PendingFit::kFull;
  pending_fit_unlinked_ = false;
  updateMaximumZoomArea();
  setZoomRectangle(maxZoomRect(), emit_signal);
  replot();
}

void PlotWidget::zoomOutUnlinked() {
  zoomOut(false);
  pending_fit_unlinked_ = true;
}

void PlotWidget::onZoomOutHorizontalTriggered(bool emit_signal) {
  updateMaximumZoomArea();
  QRectF rect = currentBoundingRect();
  const Range<double> range_x = getVisualizationRangeX();
  rect.setLeft(range_x.min);
  rect.setRight(range_x.max);
  setZoomRectangle(rect, emit_signal);
  replot();
}

void PlotWidget::onZoomOutVerticalTriggered(bool emit_signal) {
  updateMaximumZoomArea();
  QRectF rect = currentBoundingRect();
  const Range<double> range_y = getVisualizationRangeY(Range<double>{.min = rect.left(), .max = rect.right()});
  rect.setBottom(range_y.min);
  rect.setTop(range_y.max);
  setZoomRectangle(rect, emit_signal);
  replot();
}

void PlotWidget::setTrackerPosition(double display_time_sec) {
  if (tracker_ == nullptr) {
    return;
  }
  last_tracker_time_sec_ = display_time_sec;
  tracker_->setEnabled(tracker_enabled_ && !isXYPlot());
  if (isXYPlot()) {
    updateXYMarkers(display_time_sec);
    replotOverlays();
    return;
  }
  tracker_->setPosition(QPointF(display_time_sec, 0.0));
  replotOverlays();
}

void PlotWidget::updateXYMarkers(double display_time_sec) {
  // An XY plot has no time axis, so the vertical-line tracker is meaningless: each curve's marker rides along the
  // parametric curve, on the (x, y) sample at the current time.
  for (CurveInfo& info : curveList()) {
    if (info.marker == nullptr || info.curve == nullptr) {
      continue;
    }
    syncMarkerColor(info);
    const auto* series = dynamic_cast<const PointSeriesXY*>(info.curve->data());
    std::optional<QPointF> point;
    if (series != nullptr) {
      point = series->sampleFromTime(display_time_sec);
    }
    if (point.has_value() && info.curve->isVisible()) {
      info.marker->setValue(*point);
      info.marker->setVisible(true);
    } else {
      info.marker->setVisible(false);
    }
  }
}

void PlotWidget::onChangeCurveColor(const QString& curve_name, QColor new_color) {
  CurveInfo* info = curveFromTitle(curve_name);
  if (info != nullptr && info->curve != nullptr) {
    info->curve->setPen(new_color, info->curve->pen().widthF());
    // Remember the override so the curve keeps this colour when re-dragged into another plot, keyed by source_name as
    // addCurve does. Time-series only: XY curves are keyed by a composed title and the registry is never consulted for
    // them, so an entry would be read by nothing.
    if (CurveColorRegistry* registry = colorRegistryOf(session_); registry != nullptr && !isXYPlot()) {
      registry->setColor(info->source_name, new_color.name());
    }
    emit curveColorChanged(info->source_name, new_color);
    trackers_stale_ = true;
    replot();
  }
}

void PlotWidget::setCurveLineWidth(const QString& curve_name, double width) {
  CurveInfo* info = curveFromTitle(curve_name);
  if (info == nullptr || info->curve == nullptr) {
    return;
  }
  info->curve->setPen(info->curve->pen().color(), width);
  replot();
  emit undoableChange();
}

void PlotWidget::setCurveStyle(const QString& curve_name, CurveStyle style) {
  CurveInfo* info = curveFromTitle(curve_name);
  if (info == nullptr || info->curve == nullptr) {
    return;
  }
  // The style-to-Qwt mapping of PlotWidgetBase::setStyle() without the pen-width assignment, so a per-curve width set
  // through setCurveLineWidth() survives a style toggle.
  applyStyleToCurve(info->curve, style);
  replot();
  if (!loading_state_) {
    emit undoableChange();
  }
}

void PlotWidget::setCurveVisible(const QString& curve_name, bool visible) {
  const CurveInfo* info = curveFromTitle(curve_name);
  if (info == nullptr || info->curve == nullptr || info->curve->isVisible() == visible) {
    return;
  }
  const QString source_name = info->source_name;
  trackers_stale_ = true;
  PlotWidgetBase::setCurveVisible(curve_name, visible);
  emit curveVisibilityChanged(source_name, visible);
  emit undoableChange();
}

void PlotWidget::setCurveShowMarkers(const QString& curve_name, bool show) {
  CurveInfo* info = curveFromTitle(curve_name);
  if (info == nullptr || info->curve == nullptr) {
    return;
  }
  info->show_markers = show;
  // markers_item_'s targets are recomputed on each paint from show_markers, so a replot re-evaluates the overlay.
  replot();
  emit markerTargetsChanged();
  emit undoableChange();
}

void PlotWidget::setDatasetMarkerScopeVisible(DatasetId dataset_id, MarkerScope scope, bool visible) {
  const MarkerScopeKey key{dataset_id, scope};
  const bool currently_visible = !hidden_marker_scopes_.contains(key);
  if (currently_visible == visible) {
    return;
  }
  if (visible) {
    hidden_marker_scopes_.erase(key);
  } else {
    hidden_marker_scopes_.insert(key);
  }
  // markers_item_'s targets are recomputed on each paint from hidden_marker_scopes_, so a replot re-evaluates the
  // overlay.
  replot();
  emit markerTargetsChanged();
  emit markerScopeVisibilityChanged(dataset_id, scope, visible);
  if (!loading_state_) {
    emit undoableChange();
  }
}

std::vector<DatasetId> PlotWidget::curveDatasets() const {
  std::set<DatasetId> datasets;
  for (const CurveInfo& info : curveList()) {
    if (info.curve == nullptr) {
      continue;
    }
    if (const auto* adapter = dynamic_cast<const DatastoreCurveAdapter*>(info.curve->data())) {
      datasets.insert(adapter->source().dataset_id);
    }
  }
  return {datasets.begin(), datasets.end()};
}

QString PlotWidget::datasetDisplayName(DatasetId dataset_id) const {
  for (const CurveInfo& info : curveList()) {
    if (info.curve == nullptr) {
      continue;
    }
    if (const auto* adapter = dynamic_cast<const DatastoreCurveAdapter*>(info.curve->data())) {
      if (adapter->source().dataset_id == dataset_id) {
        return adapter->source().dataset_name;
      }
    }
  }
  return tr("dataset %1").arg(dataset_id);
}

bool PlotWidget::datasetMarkerScopeVisible(DatasetId dataset_id, MarkerScope scope) const {
  return !hidden_marker_scopes_.contains(MarkerScopeKey{dataset_id, scope});
}

void PlotWidget::saveHiddenMarkerScopes(QDomDocument& doc, QDomElement& plot_element) const {
  // Only the hidden (dataset, scope) pairs get a child: with every scope visible nothing is written, and an absent
  // child means visible on load.
  for (const DatasetId dataset_id : curveDatasets()) {
    for (const MarkerScope scope : {MarkerScope::kDataset, MarkerScope::kAllDatasets}) {
      if (!hidden_marker_scopes_.contains(MarkerScopeKey{dataset_id, scope})) {
        continue;
      }
      QDomElement marker_scope_element = doc.createElement(u"marker_scope"_s);
      stampDatasetIdentity(marker_scope_element, dataset_id);
      marker_scope_element.setAttribute(plot_xml::kMarkerScopeAttribute, markerScopeName(scope));
      marker_scope_element.setAttribute(u"visible"_s, u"false"_s);
      plot_element.appendChild(marker_scope_element);
    }
  }
  for (const PendingMarkerScope& pending : pending_marker_scopes_) {
    QDomElement marker_scope_element = doc.createElement(u"marker_scope"_s);
    marker_scope_element.setAttribute(u"dataset_id"_s, pending.id);
    marker_scope_element.setAttribute(u"dataset_source"_s, pending.source);
    if (!pending.path.isEmpty()) {
      marker_scope_element.setAttribute(u"dataset_path"_s, pending.path);
    }
    marker_scope_element.setAttribute(plot_xml::kMarkerScopeAttribute, markerScopeName(pending.scope));
    marker_scope_element.setAttribute(u"visible"_s, u"false"_s);
    plot_element.appendChild(marker_scope_element);
  }
}

void PlotWidget::loadHiddenMarkerScopes(const QDomElement& plot_element) {
  // Identity resolves as a curve's dataset_id / dataset_source does (a layout from another session may have reminted
  // ids); ambiguous resolution defaults to visible (skipped). An unloaded dataset (resolution.id nullopt) is parked in
  // pending_marker_scopes_ with its full saved identity, so the entry can follow a reminted id once the pending binder
  // lands its curves. `scope` is required: an absent or unrecognized value leaves the entry out (visible).
  std::set<MarkerScopeKey> new_hidden_scopes;
  pending_marker_scopes_.clear();
  if (catalog_ != nullptr) {
    for (QDomElement marker_scope_element = plot_element.firstChildElement(u"marker_scope"_s);
         !marker_scope_element.isNull();
         marker_scope_element = marker_scope_element.nextSiblingElement(u"marker_scope"_s)) {
      const std::optional<HiddenMarkerScope> hidden = parseHiddenMarkerScope(marker_scope_element);
      if (!hidden.has_value()) {
        continue;
      }
      const SavedDatasetIdentity& saved = hidden->dataset;
      const auto resolution = catalog_->resolveDatasetIdentity(saved.id, saved.source, saved.path);
      if (resolution.ambiguous) {
        continue;
      }
      if (!resolution.id.has_value()) {
        pending_marker_scopes_.push_back(PendingMarkerScope{saved.id, saved.source, saved.path, hidden->scope});
        continue;
      }
      new_hidden_scopes.insert(MarkerScopeKey{*resolution.id, hidden->scope});
    }
  }
  if (new_hidden_scopes != hidden_marker_scopes_) {
    hidden_marker_scopes_ = std::move(new_hidden_scopes);
    emit markerTargetsChanged();
    emit datasetMarkerScopesChanged();
  }
}

void PlotWidget::resolvePendingMarkerScopes() {
  if (catalog_ == nullptr || pending_marker_scopes_.empty()) {
    return;
  }
  bool moved = false;
  std::erase_if(pending_marker_scopes_, [&](const PendingMarkerScope& pending) {
    const auto resolution = catalog_->resolveDatasetIdentity(pending.id, pending.source, pending.path);
    if (!resolution.id.has_value()) {
      return false;
    }
    moved = true;
    hidden_marker_scopes_.insert(MarkerScopeKey{*resolution.id, pending.scope});
    return true;
  });
  if (!moved) {
    return;
  }
  // A dataset resolved here can already carry curves whose footer row was never built (curveListChanged fired before
  // its marker object topics existed, e.g. a generator replayed after the pending binder ran): refresh the row set too,
  // not just the hidden flag.
  refreshMarkerScopeRowSet();
  emit markerTargetsChanged();
  emit datasetMarkerScopesChanged();
}

void PlotWidget::setMarkerStatus(std::vector<MarkerRuleStatus> statuses) {
  std::erase_if(statuses, [](const MarkerRuleStatus& status) { return !status.computing && status.error.isEmpty(); });
  std::ranges::sort(statuses, {}, &MarkerRuleStatus::rule_id);
  if (statuses == marker_status_) {
    return;
  }
  marker_status_ = std::move(statuses);
  const bool computing = std::ranges::any_of(marker_status_, &MarkerRuleStatus::computing);
  const QString note = marker_status_.empty() ? QString()
                       : computing            ? tr("Computing markers…")
                                              : tr("Marker rule failed");
  if (markers_item_->setStatusNote(note)) {
    replot();
  }
}

QString PlotWidget::markerStatusNote() const {
  return markers_item_->statusNote();
}

QRectF PlotWidget::markerStatusNoteRect() const {
  return markers_item_->statusNoteRect(qwtPlot()->canvas()->contentsRect());
}

QString PlotWidget::markerStatusToolTip() const {
  QStringList lines;
  for (const MarkerRuleStatus& status : marker_status_) {
    if (!status.error.isEmpty()) {
      lines.push_back(status.rule_id + u": "_s + status.error);
    }
  }
  return lines.isEmpty() ? QString() : Qt::convertFromPlainText(lines.join(u'\n'));
}

std::vector<MarkerTarget> PlotWidget::markerTargets() const {
  std::vector<MarkerTarget> targets;
  std::set<DatasetId> datasets;
  for (const CurveInfo& info : curveList()) {
    const auto* adapter =
        info.curve != nullptr ? dynamic_cast<const DatastoreCurveAdapter*>(info.curve->data()) : nullptr;
    if (adapter == nullptr) {
      continue;
    }
    const auto& src = adapter->source();
    // The dataset counts as present whatever show_markers says: that flag gates only this curve's own series target;
    // each scope's target is gated solely by hidden_marker_scopes_ below.
    datasets.insert(src.dataset_id);
    if (!info.show_markers) {
      continue;
    }
    // The per-series marker topic key is the human path "topic_name/field_name": the string a producer (the markers
    // toolbox) receives when a series is dropped on it (through catalog_key_resolver), so both sides agree on the
    // marker object-topic name.
    targets.push_back(
        MarkerTarget{
            src.dataset_id,
            markerInfoForKey(sdk::markerSeriesKey(src.topic_name.toStdString(), src.field_name.toStdString()))});
  }
  for (const DatasetId dataset : datasets) {
    for (const MarkerScope scope : {MarkerScope::kDataset, MarkerScope::kAllDatasets}) {
      if (hidden_marker_scopes_.contains(MarkerScopeKey{dataset, scope})) {
        continue;
      }
      targets.push_back(MarkerTarget{dataset, markerInfoForScope(scope)});
    }
  }
  return targets;
}

void PlotWidget::pruneHiddenMarkerScopes() {
  if (loading_state_ || !pending_curve_intents_.empty()) {
    return;
  }
  const std::vector<DatasetId> live = curveDatasets();  // sorted; pending entries are left alone
  std::erase_if(hidden_marker_scopes_, [&live](const MarkerScopeKey& key) {
    return !std::ranges::binary_search(live, key.first);
  });
}

bool PlotWidget::refreshMarkerScopeRowSet() {
  std::vector<MarkerScopeKey> current;
  if (session_ != nullptr) {
    const std::vector<DatasetId> datasets = curveDatasets();
    current = publishedMarkerScopes(session_->objectStore(), datasets);
  }
  if (current == marker_scope_rows_) {
    return false;
  }
  marker_scope_rows_ = std::move(current);
  return true;
}

void PlotWidget::removeAllCurves() {
  PlotWidgetBase::removeAllCurves();
  setModeXY(false);
  if (tracker_ != nullptr) {
    tracker_->setEnabled(tracker_enabled_);
    tracker_->redraw();
  }
}

bool PlotWidget::revalidate() {
  if (catalog_ == nullptr) {
    return false;
  }
  // A curve is stale once its source key is gone from the catalog. Collect first, then remove: removeCurve() mutates
  // curve_list, so removing while iterating would invalidate the iterator.
  QStringList to_remove;
  for (const CurveInfo& info : curveList()) {
    if (info.curve == nullptr) {
      continue;
    }
    if (const auto* adapter = dynamic_cast<const DatastoreCurveAdapter*>(info.curve->data())) {
      if (!catalog_->curveDescriptor(adapter->source().name).has_value()) {
        to_remove.push_back(info.source_name);
      }
    } else if (const auto* xy_series = dynamic_cast<const PointSeriesXY*>(info.curve->data())) {
      if (!catalog_->curveDescriptor(xy_series->xSource().name).has_value() ||
          !catalog_->curveDescriptor(xy_series->ySource().name).has_value()) {
        to_remove.push_back(info.source_name);
      }
    }
  }
  if (to_remove.isEmpty()) {
    return false;
  }
  for (const QString& source_name : to_remove) {
    removeCurve(source_name);
  }
  // If revalidation drained every curve, mirror removeAllCurves()'s reset out of XY mode: onDragEnterEvent() accepts an
  // add_curve drop only when !isXYPlot(), so a stuck-XY empty plot would reject every new curve.
  if (curveList().empty()) {
    setModeXY(false);
    if (tracker_ != nullptr) {
      tracker_->setEnabled(tracker_enabled_);
      tracker_->redraw();
    }
  }
  updateMaximumZoomArea();
  replot();
  return true;
}

bool PlotWidget::eventFilter(QObject* obj, QEvent* event) {
  if (PlotWidgetBase::eventFilter(obj, event)) {
    return true;
  }
  if (event->type() == QEvent::Destroy || obj != qwtPlot()->canvas()) {
    return false;
  }

  if (event->type() == QEvent::MouseButtonPress) {
    auto* mouse_event = static_cast<QMouseEvent*>(event);
    if (mouse_event->button() == Qt::LeftButton && mouse_event->modifiers() == Qt::ShiftModifier && !isXYPlot()) {
      const QwtScaleMap x_map = qwtPlot()->canvasMap(QwtPlot::xBottom);
      const QwtScaleMap y_map = qwtPlot()->canvasMap(QwtPlot::yLeft);
      emit trackerMoved(
          QPointF(x_map.invTransform(mouse_event->pos().x()), y_map.invTransform(mouse_event->pos().y())));
      return true;
    }
    if (mouse_event->button() == Qt::RightButton && mouse_event->modifiers() == Qt::NoModifier) {
      canvasContextMenuTriggered(mouse_event->pos());
      return true;
    }
  }
  if (event->type() == QEvent::MouseMove) {
    auto* mouse_event = static_cast<QMouseEvent*>(event);
    if (mouse_event->buttons() == Qt::LeftButton && mouse_event->modifiers() == Qt::ShiftModifier && !isXYPlot()) {
      const QwtScaleMap x_map = qwtPlot()->canvasMap(QwtPlot::xBottom);
      const QwtScaleMap y_map = qwtPlot()->canvasMap(QwtPlot::yLeft);
      emit trackerMoved(
          QPointF(x_map.invTransform(mouse_event->pos().x()), y_map.invTransform(mouse_event->pos().y())));
      return true;
    }
    // Mouse hover inspector (buttonShowpoint). Does not consume the event, so panning and zooming keep working
    // underneath. Gated by show_points_ so the hot path is one bool read when the toggle is off.
    if (show_points_) {
      showPointValues(mouse_event->pos());
    }
  }
  if (event->type() == QEvent::ToolTip) {
    const auto* help = static_cast<QHelpEvent*>(event);
    const QString tip = markerStatusToolTip();
    if (!tip.isEmpty() && markerStatusNoteRect().contains(help->pos())) {
      QToolTip::showText(help->globalPos(), tip, qwtPlot()->canvas());
      return true;
    }
  }
  if (event->type() == QEvent::Leave && obj == qwtPlot()->canvas()) {
    if (show_point_marker_ != nullptr && show_point_marker_->isVisible()) {
      show_point_marker_->setVisible(false);
      show_point_text_->setVisible(false);
      replot();
    }
  }
  return false;
}

void PlotWidget::onExternallyResized(const QRectF& rect) {
  // The user framed the view, unless the gesture had no X extent to scale (it reports an empty or NaN window).
  if (rect.left() < rect.right()) {
    pending_fit_ = PendingFit::kNone;
  }
  if (curveList().empty()) {
    return;
  }
  if (isXYPlot()) {
    if (keepRatioXY()) {
      // Wheel zoom (magnifier) and pan bypass the drag-zoom keep-ratio path, and the magnifier clamps each axis
      // independently at the data bounds; that asymmetry skews a 1:1 circle into an ellipse. Re-impose the canvas
      // aspect ratio on the event's rect, the exact proposed gesture viewport, immune to canvas-map round-off.
      applyRectKeepingRatio(rect);
      replot();
    }
    gesture_undo_debounce_.start();
    return;  // an XY plot never emits rectChanged
  }
  if (isZoomLinkEnabled()) {
    emit rectChanged(this, rect);
  }
  // rectChanged reaches linked peers at once; the history snapshot is debounced so a continuous gesture publishes once,
  // after its last event.
  gesture_undo_debounce_.start();
}

void PlotWidget::onDragEnterEvent(QDragEnterEvent* event) {
  dragging_ = {};
  if (catalog_ == nullptr || event == nullptr || event->mimeData() == nullptr) {
    return;
  }

  const QMimeData* mime_data = event->mimeData();
  if (mime_data->hasFormat(u"curveslist/add_curve"_s)) {
    const QStringList curves = decodeCurveDrop(mime_data, u"curveslist/add_curve"_s);
    if (!curves.empty() && allCurvesDroppable(curves) && !isXYPlot()) {
      dragging_.mode = DragMode::kCurves;
      dragging_.curves = curves;
      event->acceptProposedAction();
    }
    return;
  }

  if (mime_data->hasFormat(CurveTreeView::newXyAxisMimeType())) {
    const QStringList curves = decodeCurveDrop(mime_data, CurveTreeView::newXyAxisMimeType());
    // Accept a new XY pair on an empty plot or one that is already XY (a plot can hold several XY curves), never mixed
    // with time-series curves.
    if (curves.size() == 2 && allCurvesKnown(curves) && (curveList().empty() || isXYPlot())) {
      dragging_.mode = DragMode::kNewXY;
      dragging_.curves = curves;
      event->acceptProposedAction();
    }
  }
}

void PlotWidget::onDragLeaveEvent(QDragLeaveEvent* /*event*/) {
  dragging_ = {};
}

void PlotWidget::onDropEvent(QDropEvent* event) {
  if (event == nullptr || dragging_.mode == DragMode::kNone) {
    return;
  }

  const bool was_empty = curveList().empty();
  bool curves_changed = false;
  if (dragging_.mode == DragMode::kCurves) {
    if (isXYPlot()) {
      emit statusMessageRequested(tr("Timeseries curves can not be dropped on an XY plot."));
    } else {
      setModeXY(false);
      for (const QString& curve_name : dragging_.curves) {
        if (isPlaceholderCurveName(curve_name)) {
          // No fabricated curve: the shell (TopicDemandController) registers demand and a pending bind that completes
          // once real data arrives.
          emit placeholderCurveDropped(curve_name);
          curves_changed = true;
          continue;
        }
        curves_changed = addCurve(curve_name) != nullptr || curves_changed;
      }
    }
  } else if (dragging_.mode == DragMode::kNewXY && dragging_.curves.size() == 2) {
    if (!isXYPlot() && !curveList().empty()) {
      emit statusMessageRequested(tr("Create XY plots by dropping two curves on an empty plot."));
    } else {
      const bool was_xy = isXYPlot();
      setModeXY(true);
#ifdef PJ_TARGET_WASM
      // The source-side WASM drag loop must finish unwinding before any dialog result mutates the plot: accept and
      // clear the transient drag state now; the completion reproduces the synchronous desktop finalization later.
      const QString x_key = dragging_.curves[0];
      const QString y_key = dragging_.curves[1];
      dragging_ = {};
      event->acceptProposedAction();
      createCurveXYInteractiveAsync(x_key, y_key, [this, was_empty, was_xy](CurveInfo* created) {
        if (created == nullptr) {
          if (!was_xy && curveList().empty()) {
            // A cancelled first XY drop must not strand the empty plot in XY mode.
            setModeXY(false);
          }
          return;
        }
        emit curvesDropped(was_empty);
        if (was_empty) {
          zoomOut(true);
        } else {
          replot();
        }
        emit undoableChange();
      });
      return;
#else
      curves_changed = createCurveXYInteractive(dragging_.curves[0], dragging_.curves[1]) != nullptr;
      if (!curves_changed && !was_xy) {
        // A cancelled dialog on a plot that was not already XY must not strand the empty plot in XY mode, where it
        // would refuse normal drops.
        setModeXY(false);
      }
#endif
    }
  }

  if (curves_changed) {
    event->acceptProposedAction();
    if (was_empty) {
      zoomOut(false);
    } else {
      replot();
    }
    emit curvesDropped(was_empty);
    emit undoableChange();
  }
  dragging_ = {};
}

void PlotWidget::buildActions() {
  // Icons are re-applied with the current theme each time the context menu opens (canvasContextMenuTriggered), so they
  // need not be set here.
  action_split_horizontal_ = new QAction(tr("Split &Horizontally"), this);
  connect(action_split_horizontal_, &QAction::triggered, this, &PlotWidget::splitHorizontal);

  action_split_vertical_ = new QAction(tr("Split &Vertically"), this);
  connect(action_split_vertical_, &QAction::triggered, this, &PlotWidget::splitVertical);

  action_remove_all_curves_ = new QAction(tr("&Remove ALL curves"), this);
  connect(action_remove_all_curves_, &QAction::triggered, this, &PlotWidget::removeAllCurves);
  connect(action_remove_all_curves_, &QAction::triggered, this, &PlotWidget::undoableChange);

  action_zoom_out_ = new QAction(tr("&Zoom Out"), this);
  connect(action_zoom_out_, &QAction::triggered, this, [this]() {
    zoomOut(true);
    emit undoableChange();
  });

  action_zoom_out_horizontal_ = new QAction(tr("Zoom Out Horizontally"), this);
  connect(action_zoom_out_horizontal_, &QAction::triggered, this, [this]() {
    onZoomOutHorizontalTriggered(true);
    emit undoableChange();
  });

  action_zoom_out_vertical_ = new QAction(tr("Zoom Out Vertically"), this);
  connect(action_zoom_out_vertical_, &QAction::triggered, this, [this]() {
    onZoomOutVerticalTriggered(true);
    emit undoableChange();
  });
}

void PlotWidget::copyWidgetToClipboard() {
  QDomDocument doc(u"plotjuggler_widget"_s);
  QDomElement plot_element = xmlSaveState(doc);
  if (plot_element.isNull()) {
    return;
  }
  stampLiveCurveKeys(plot_element);
  doc.appendChild(plot_element);
  widget_clipboard::setXml(doc.toString(2));
}

void PlotWidget::pasteWidgetFromClipboard() {
  QDomDocument doc;
  if (!widget_clipboard::parse(doc, u"plot"_s)) {
    return;
  }
  QDomElement plot_element = doc.documentElement();
  plot_element.setAttribute(u"id"_s, state_id_);
  rebindClipboardCurveKeys(plot_element);
  if (xmlLoadState(plot_element)) {
    emit undoableChange();
  }
}

bool PlotWidget::canPasteWidgetFromClipboard() const {
  QDomDocument doc;
  return widget_clipboard::parse(doc, u"plot"_s);
}

void PlotWidget::stampDatasetIdentity(QDomElement& element, DatasetId dataset_id, const QString& prefix) const {
  if (catalog_ == nullptr) {
    return;
  }
  // Exact-id and raw-source qualifiers, so a same-topic sibling dataset is never interchanged on restore. The raw
  // DatasetInfo::source_name is preferred over the deduped display label: it is the portable identity a later-session
  // reload compares against.
  element.setAttribute(prefix + "dataset_id"_L1, QString::number(dataset_id));
  element.setAttribute(
      prefix + "dataset_source"_L1, catalog_->datasetSourceName(dataset_id).value_or(datasetDisplayName(dataset_id)));
}

void PlotWidget::stampDatasetPath(QDomElement& element, DatasetId dataset_id, const QString& prefix) const {
  if (catalog_ == nullptr) {
    return;
  }
  const QString path = catalog_->datasetSourcePath(dataset_id);
  if (!path.isEmpty()) {
    element.setAttribute(prefix + "dataset_path"_L1, path);
  }
}

void PlotWidget::stampLiveCurveKeys(QDomElement& plot_element) const {
  // Full-path qualifier so a clipboard paste in a later session can validate a reminted id (rebindClipboardCurveKeys).
  // xmlSaveState already stamped dataset_id and dataset_source; the path is the tiebreak for same-basename files.
  QDomElement curve_element = plot_element.firstChildElement(u"curve"_s);
  for (const CurveInfo& info : curveList()) {
    if (info.curve == nullptr || curve_element.isNull()) {
      continue;
    }
    curve_element.setAttribute(u"name"_s, info.source_name);
    if (auto* xy_series = dynamic_cast<PointSeriesXY*>(info.curve->data())) {
      curve_element.setAttribute(u"curve_x"_s, xy_series->xSource().name);
      curve_element.setAttribute(u"curve_y"_s, xy_series->ySource().name);
      stampDatasetPath(curve_element, xy_series->xSource().dataset_id, u"x_"_s);
      stampDatasetPath(curve_element, xy_series->ySource().dataset_id, u"y_"_s);
    } else if (catalog_ != nullptr) {
      if (const auto descriptor = catalog_->curveDescriptor(info.source_name); descriptor.has_value()) {
        stampDatasetPath(curve_element, descriptor->dataset_id);
      }
    }
    curve_element = curve_element.nextSiblingElement(u"curve"_s);
  }
  for (QDomElement marker_scope_element = plot_element.firstChildElement(u"marker_scope"_s);
       !marker_scope_element.isNull();
       marker_scope_element = marker_scope_element.nextSiblingElement(u"marker_scope"_s)) {
    // A pending scope's saved id can name a live dataset of another source: its path would never resolve the scope.
    const SavedDatasetIdentity saved = readDatasetIdentity(marker_scope_element);
    if (catalog_ != nullptr && catalog_->resolveDatasetIdentity(saved.id, saved.source).id == saved.id) {
      stampDatasetPath(marker_scope_element, saved.id);
    }
  }
}

void PlotWidget::rebindClipboardCurveKeys(QDomElement& plot_element) const {
  if (catalog_ == nullptr) {
    return;
  }
  // Clipboard XML carries the dataset qualifiers of any snapshot (dataset_id, dataset_source, full dataset_path,
  // written by xmlSaveState / stampLiveCurveKeys), and the clipboard outlives undo-history epochs: a pasted id may
  // be stale or reminted. Three-way result:
  //   value   = resolved concrete key: set it;
  //   empty   = the curve was qualified but resolution failed: clear the concrete key, so a stale opaque key from
  //             another session never collides with a live series of a different dataset;
  //   nullopt = an unqualified copy that stays ambiguous: leave the copied key (xmlLoadState drops it if it no longer
  //             resolves).
  const auto resolve = [this](const QDomElement& curve, const QString& prefix) -> std::optional<QString> {
    const QString topic = curve.attribute(prefix + "topic"_L1);
    const QString field = curve.attribute(prefix + "field"_L1);
    if (topic.isEmpty() || field.isEmpty()) {
      return std::nullopt;
    }
    const auto [dataset_id, dataset_source, dataset_path] = readDatasetIdentity(curve, prefix);
    // The shared resolver (the same three tiers as layout and undo restore, full-path fallback included) returns a
    // concrete key on success. A miss is nullopt, so map it to the three-way apply below by re-deriving whether the
    // curve was qualified: a qualified miss clears the key, an unqualified one leaves it.
    const auto key = catalog_->resolveCurveKey(dataset_id, dataset_source, dataset_path, topic, field);
    if (key.has_value()) {
      return key;
    }
    const bool was_qualified = dataset_id != 0 || !dataset_source.isEmpty() || !dataset_path.isEmpty();
    return was_qualified ? std::optional<QString>{QString{}} : std::nullopt;
  };

  // Applies a resolve() result to one key attribute: overwrite on success, remove on a qualified failure (empty), leave
  // untouched on nullopt.
  const auto apply_key = [](QDomElement& curve, const QString& key_attr, const std::optional<QString>& result) {
    if (!result.has_value()) {
      return;
    }
    if (result->isEmpty()) {
      curve.removeAttribute(key_attr);
    } else {
      curve.setAttribute(key_attr, *result);
    }
  };

  for (QDomElement curve = plot_element.firstChildElement(u"curve"_s); !curve.isNull();
       curve = curve.nextSiblingElement(u"curve"_s)) {
    if (curve.hasAttribute(u"x_topic"_s)) {
      const std::optional<QString> x_key = resolve(curve, u"x_"_s);
      const std::optional<QString> y_key = resolve(curve, u"y_"_s);
      // An XY curve is undrawable unless both axes resolve: if either axis was qualified but failed, clear both keys so
      // no stale half survives.
      if (x_key.has_value() && !x_key->isEmpty() && y_key.has_value() && !y_key->isEmpty()) {
        curve.setAttribute(u"curve_x"_s, *x_key);
        curve.setAttribute(u"curve_y"_s, *y_key);
      } else if ((x_key.has_value() && x_key->isEmpty()) || (y_key.has_value() && y_key->isEmpty())) {
        curve.removeAttribute(u"curve_x"_s);
        curve.removeAttribute(u"curve_y"_s);
      }
      continue;
    }
    apply_key(curve, u"name"_s, resolve(curve, {}));
  }
}

void PlotWidget::canvasContextMenuTriggered(const QPoint& pos) {
  if (!context_menu_enabled_) {
    return;
  }

  auto* menu = new QMenu(qwtPlot());
  menu->setObjectName(u"PJMenu"_s);
  menu->setProperty("categorySeparators", true);
  // Refresh icons with the active theme on every popup so the glyphs stay tinted after a theme switch.
  const QString theme = currentTheme();
  action_split_horizontal_->setIcon(QIcon(loadSvg(":/resources/svg/add_column.svg", theme)));
  action_split_vertical_->setIcon(QIcon(loadSvg(":/resources/svg/add_row.svg", theme)));
  action_remove_all_curves_->setIcon(QIcon(loadSvg(":/resources/svg/delete_forever.svg", theme)));
  action_zoom_out_->setIcon(QIcon(loadSvg(":/resources/svg/zoom_max.svg", theme)));
  action_zoom_out_horizontal_->setIcon(QIcon(loadSvg(":/resources/svg/zoom_horizontal.svg", theme)));
  action_zoom_out_vertical_->setIcon(QIcon(loadSvg(":/resources/svg/zoom_vertical.svg", theme)));
  // Apply Filter...: open the Filter Editor scoped to this plot's curves; on Save it applies the chosen filter (through
  // DataProcessorService) and adds the filtered curve(s) to this plot. Filters are time-series transforms, so the
  // action is hidden on XY plots.
  if (session_ != nullptr && catalog_ != nullptr && !curveList().empty() && !isXYPlot()) {
    menu->addAction(QIcon(loadSvg(":/resources/svg/function.svg", theme)), tr("Apply Filter..."), this, [this]() {
      launchFilterEditor();
    });
    addActionCategorySeparator(*menu);
  }

  menu->addAction(
      QIcon(loadSvg(":/resources/svg/copy.svg", theme)), tr("Copy"), this, [this]() { copyWidgetToClipboard(); });
  QAction* paste_action = menu->addAction(
      QIcon(loadSvg(":/resources/svg/paste.svg", theme)), tr("Paste"), this, [this]() { pasteWidgetFromClipboard(); });
  paste_action->setEnabled(canPasteWidgetFromClipboard());
  addActionCategorySeparator(*menu);

  menu->addAction(action_split_horizontal_);
  menu->addAction(action_split_vertical_);
  addActionCategorySeparator(*menu);
  menu->addAction(action_zoom_out_);
  menu->addAction(action_zoom_out_horizontal_);
  menu->addAction(action_zoom_out_vertical_);
  addActionCategorySeparator(*menu);
  menu->addAction(action_remove_all_curves_);
  action_remove_all_curves_->setEnabled(!curveList().empty());
  showOwnedMenu(menu, qwtPlot()->canvas()->mapToGlobal(pos));
}

void PlotWidget::launchFilterEditor() {
  if (session_ == nullptr || catalog_ == nullptr) {
    return;
  }
  std::vector<CurveDescriptor> sources;
  for (const CurveInfo& info : curveList()) {
    if (const auto descriptor = catalog_->curveDescriptor(info.source_name)) {
      sources.push_back(*descriptor);
    }
  }
  if (sources.empty()) {
    return;
  }
  // The Filter Editor is a chart-area takeover panel owned by the host (it needs MainWindow::presentPanel), so request
  // it rather than construct it here; on Apply the host calls replaceCurve() on this plot for each result.
  emit filterEditorRequested(std::move(sources), this);
}

void PlotWidget::setAxisScale(QwtAxisId axis_id, double min, double max) {
  if (min > max) {
    std::swap(min, max);
  }
  qwtPlot()->setAxisScale(axis_id, min, max);
}

std::optional<DisplayOffset> PlotWidget::representativeDisplayOffset() const {
  if (session_ == nullptr) {
    return std::nullopt;
  }
  // The axis is shared across curves; usually they share a dataset (one offset), and when they do not the first
  // datastore-backed curve's dataset is the representative, the same rule at save and load so the absolute <-> display
  // round trip is stable.
  for (const CurveInfo& info : curveList()) {
    if (info.curve == nullptr) {
      continue;
    }
    if (const auto* adapter = dynamic_cast<const DatastoreCurveAdapter*>(info.curve->data())) {
      return session_->displayOffset(adapter->source().dataset_id);
    }
  }
  return std::nullopt;
}

double PlotWidget::displayOffsetSeconds() const {
  const std::optional<DisplayOffset> offset = representativeDisplayOffset();
  return offset.has_value() ? std::chrono::duration<double>(offset->value).count() : 0.0;
}

qint64 PlotWidget::displayOffsetNanoseconds() const {
  const std::optional<DisplayOffset> offset = representativeDisplayOffset();
  return offset.has_value() ? static_cast<qint64>(offset->value.count()) : 0;
}

QStringList PlotWidget::decodeCurveDrop(const QMimeData* mime_data, const QString& format) const {
  QStringList curves;
  if (mime_data == nullptr || !mime_data->hasFormat(format)) {
    return curves;
  }

  QByteArray encoded = mime_data->data(format);
  QDataStream stream(&encoded, QIODevice::ReadOnly);
  while (!stream.atEnd()) {
    QString curve_name;
    stream >> curve_name;
    if (!curve_name.isEmpty()) {
      curves.push_back(curve_name);
    }
  }
  return curves;
}

bool PlotWidget::allCurvesKnown(const QStringList& curves) const {
  if (catalog_ == nullptr) {
    return false;
  }
  return std::all_of(curves.begin(), curves.end(), [this](const QString& curve_name) {
    return catalog_->curveDescriptor(curve_name).has_value();
  });
}

bool PlotWidget::isPlaceholderCurveName(const QString& name) const {
  if (catalog_ == nullptr) {
    return false;
  }
  const auto item = catalog_->itemDescriptor(name);
  return item.has_value() && isAdvertisedTopic(*item) &&
         asAdvertisedTopic(*item)->classification == sdk::BuiltinObjectType::kNone;
}

bool PlotWidget::allCurvesDroppable(const QStringList& curves) const {
  if (catalog_ == nullptr) {
    return false;
  }
  return std::all_of(curves.begin(), curves.end(), [this](const QString& curve_name) {
    return catalog_->curveDescriptor(curve_name).has_value() || isPlaceholderCurveName(curve_name);
  });
}

QString PlotWidget::lineWidthToString(LineWidth width) {
  switch (width) {
    case LineWidth::kPoints10:
      return u"1.0"_s;
    case LineWidth::kPoints15:
      return u"1.5"_s;
    case LineWidth::kPoints20:
      return u"2.0"_s;
    case LineWidth::kPoints30:
      return u"3.0"_s;
  }
  return u"1.0"_s;
}

LineWidth PlotWidget::lineWidthFromString(QString value) {
  if (value == "1.5"_L1) {
    return LineWidth::kPoints15;
  }
  if (value == "2.0"_L1) {
    return LineWidth::kPoints20;
  }
  if (value == "3.0"_L1) {
    return LineWidth::kPoints30;
  }
  return LineWidth::kPoints10;
}

LineWidth PlotWidget::lineWidthFromPixels(double pixels) {
  // A per-curve line width in older layouts is a raw pen width, either the rendered lineWidthValue() or the raw toolbar
  // value (lineWidthValue is `scale` times the raw value). Pick the LineWidth closest across both scales, deriving
  // every candidate from lineWidthValue() so the width ladder lives in one place.
  const double scale = lineWidthValue(LineWidth::kPoints10);  // raw == lineWidthValue / scale
  int best = 0;
  double best_dist = std::numeric_limits<double>::max();
  for (int i = static_cast<int>(LineWidth::kPoints10); i <= static_cast<int>(LineWidth::kPoints30); ++i) {
    const double scaled = lineWidthValue(static_cast<LineWidth>(i));
    const double dist = std::min(std::abs(pixels - scaled), std::abs(pixels - scaled / scale));
    if (dist < best_dist) {
      best_dist = dist;
      best = i;
    }
  }
  return static_cast<LineWidth>(best);
}

QString PlotWidget::curveStyleToString(CurveStyle style) {
  switch (style) {
    case kLines:
      return u"Lines"_s;
    case kDots:
      return u"Dots"_s;
    case kLinesAndDots:
      return u"LinesAndDots"_s;
    case kSticks:
      return u"Sticks"_s;
    case kSteps:
      return u"Steps"_s;
    case kStepsInverted:
      return u"StepsInverted"_s;
  }
  return u"Lines"_s;
}

PlotWidgetBase::CurveStyle PlotWidget::curveStyleFromString(QString value) {
  if (value == "Dots"_L1) {
    return kDots;
  }
  if (value == "LinesAndDots"_L1) {
    return kLinesAndDots;
  }
  if (value == "Sticks"_L1) {
    return kSticks;
  }
  if (value == "Steps"_L1) {
    return kSteps;
  }
  if (value == "StepsInverted"_L1) {
    return kStepsInverted;
  }
  return kLines;
}

void PlotWidget::reconnectDataSignals() {
  if (samples_ingested_connection_) {
    disconnect(samples_ingested_connection_);
  }
  if (dataset_replace_connection_) {
    disconnect(dataset_replace_connection_);
  }
  if (markers_changed_connection_) {
    disconnect(markers_changed_connection_);
  }
  if (display_offset_connection_) {
    disconnect(display_offset_connection_);
  }
  if (display_offset_dataset_connection_) {
    disconnect(display_offset_dataset_connection_);
  }
  if (session_ == nullptr) {
    last_global_time_reference_ = 0;
    return;
  }
  last_global_time_reference_ = session_->globalTimeReference();

  // Markers changed (a producer republished or cleared a marker object topic): the overlay re-reads the ObjectStore on
  // each paint, so a replot suffices. The row-set signal fires only when a topic appeared or was retired, so a producer
  // re-publishing at up to 60 Hz does not rebuild the CurveEditor's rows every tick.
  markers_changed_connection_ = connect(session_, &SessionManager::markersChanged, this, [this]() {
    if (refreshMarkerScopeRowSet()) {
      emit datasetMarkerScopesChanged();
    }
    replot();
  });

  samples_ingested_connection_ =
      connect(session_, &SessionManager::samplesIngested, this, [this](const QVector<TopicId>& ids, bool live) {
        bool changed = false;
        for (auto& info : curveList()) {
          auto* adapter = dynamic_cast<DatastoreCurveAdapter*>(info.curve->data());
          if (adapter != nullptr) {
            if (std::find(ids.begin(), ids.end(), adapter->source().topic_id) != ids.end()) {
              adapter->onTopicCommitted();
              changed = true;
            }
            continue;
          }

          auto* xy_series = dynamic_cast<PointSeriesXY*>(info.curve->data());
          if (xy_series == nullptr) {
            continue;
          }
          if (std::find(ids.begin(), ids.end(), xy_series->xSource().topic_id) != ids.end() ||
              std::find(ids.begin(), ids.end(), xy_series->ySource().topic_id) != ids.end()) {
            xy_series->onTopicCommitted();
            changed = true;
          }
        }
        if (!changed) {
          return;
        }
        if (live) {
          // Follow-live: re-fit the axes so streaming samples beyond the drop-time range become visible; resetZoom
          // subsumes updateMaximumZoomArea and the replot.
          pending_fit_ = PendingFit::kNone;
          resetZoom();
        } else if (pending_fit_ != PendingFit::kNone && hasUsableCurveData()) {
          // Rows of a plot fitted before it had any; the fit stays armed so the
          // next chunk widens the frame too.
          if (pending_fit_ == PendingFit::kFull) {
            updateMaximumZoomArea();
            // Linked peers took this plot's empty or partial window; an XY plot's axes are its own.
            setZoomRectangle(maxZoomRect(), /*emit_signal=*/!pending_fit_unlinked_ && !isXYPlot());
            replot();
          } else {
            onZoomOutVerticalTriggered(/*emit_signal=*/false);
          }
        } else {
          // One-shot writers (file load) stay zoomed where the user left them.
          updateMaximumZoomArea();
          replot();
        }
      });

  // An in-place reload swaps a dataset's chunks. Drop the adapters' caches synchronously before the swap:
  // PointSeriesXY's pair index holds raw TopicChunk* that the swap frees (a use-after-free otherwise), and
  // DatastoreCurveAdapter's window cache (plain values) would be stale. Bindings (DatasetId / TopicIds) stay valid and
  // re-index on the next paint through the post-swap samplesIngested. DirectConnection so the clear runs inline within
  // the emit (same thread, never queued): no event-loop turn may pass between the clear and the swap.
  dataset_replace_connection_ = connect(
      session_, &SessionManager::datasetAboutToBeReplaced, this,
      [this](DatasetId dataset_id) {
        for (auto& info : curveList()) {
          if (auto* adapter = dynamic_cast<DatastoreCurveAdapter*>(info.curve->data())) {
            if (adapter->source().dataset_id == dataset_id) {
              adapter->onDataCleared();
            }
            continue;
          }
          if (auto* xy_series = dynamic_cast<PointSeriesXY*>(info.curve->data())) {
            if (xy_series->xSource().dataset_id == dataset_id || xy_series->ySource().dataset_id == dataset_id) {
              xy_series->onDataCleared();
            }
          }
        }
      },
      Qt::DirectConnection);

  // The global "Use time offset" toggled (or the frame was otherwise re-based): every curve's x shifts by a constant
  // and no topic changed, so a per-topic samplesIngested would skip them all. Translate the viewport by the same frame
  // delta so its absolute interval survives the global rebase.
  display_offset_connection_ = connect(session_, qOverload<>(&SessionManager::displayOffsetChanged), this, [this]() {
    const Timestamp new_reference = session_ != nullptr ? session_->globalTimeReference() : 0;
    const double delta_seconds = timestampDifferenceSeconds(last_global_time_reference_, new_reference);
    last_global_time_reference_ = new_reference;
    if (invalidateAdapterOffsets()) {
      QRectF viewport = currentBoundingRect();
      viewport.translate(delta_seconds, 0.0);
      updateMaximumZoomArea();
      setZoomRectangle(viewport, /*emit_signal=*/false);
    }
    // The curves just moved to the new frame, so each tracker's cached intersection markers (the circles) were sampled
    // against the old data and view rect: re-sample them at their positions against the re-fit curves.
    // Order-independent with the app's reference re-projection: whichever of {reposition, this redraw} runs last
    // re-samples against re-fit data, so the blue line keeps its dots across the toggle.
    redrawTrackers();
    replot();
  });

  // One source's display offset changed (a Timeline drag): the samples did not move, only their X mapping. Drop the
  // bound adapters' offset caches and replot at the current zoom so the curve slides into place without re-fitting.
  // Unlike datasetAboutToBeReplaced there is no use-after-free window (chunks stay alive), so a normal connection is
  // fine. PointSeriesXY ignores the display offset and is skipped.
  display_offset_dataset_connection_ = connect(
      session_, qOverload<PJ::DatasetId>(&SessionManager::displayOffsetChanged), this, [this](DatasetId dataset_id) {
        if (invalidateAdapterOffsets(dataset_id)) {
          // The max zoom-out rect (and the magnifier's wheel-out clamp) is view-independent data state: refresh it here
          // or it stays frozen at the pre-drag union of curve ranges.
          updateMaximumZoomArea();
          replot();  // current zoom; the source's curve slides into its new position
        }
      });
}

bool PlotWidget::invalidateAdapterOffsets(std::optional<DatasetId> only) {
  bool changed = false;
  for (auto& info : curveList()) {
    auto* adapter = dynamic_cast<DatastoreCurveAdapter*>(info.curve->data());
    if (adapter == nullptr) {
      continue;  // PointSeriesXY ignores the display offset
    }
    if (only.has_value() && adapter->source().dataset_id != *only) {
      continue;
    }
    adapter->onDisplayOffsetChanged();
    changed = true;
  }
  return changed;
}

}  // namespace PJ
