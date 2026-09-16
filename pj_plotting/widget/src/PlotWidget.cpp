// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotWidget.h"

#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_item.h>
#include <qwt_plot_marker.h>
#include <qwt_scale_map.h>
#include <qwt_symbol.h>
#include <qwt_text.h>

#include <QDataStream>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFontDatabase>
#include <QGuiApplication>
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
#include <QUuid>
#include <QVector>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <set>

#include "TimeAxisWidth.h"
#include "WidgetClipboard.h"
#include "pj_plotting/CurveTracker.h"
#include "pj_plotting/DatastoreCurveAdapter.h"
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
#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/MessageBox.h"
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

// Continuous gestures (wheel ticks, pan moves) coalesce into one history
// snapshot this long after the last event.
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

bool isPendingCurveIntentValid(const QDomElement& curve, bool xy_plot) {
  if (curve.tagName() != "curve"_L1 || !plot_xml::isPendingIntent(curve)) {
    return false;
  }
  const bool time_series = !curve.attribute(u"topic"_s).isEmpty();
  const bool xy = !curve.attribute(u"x_topic"_s).isEmpty() && !curve.attribute(u"y_topic"_s).isEmpty();
  if (time_series == xy || xy != xy_plot) {
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
      return false;
    }
  }
  return true;
}

// The session-scoped curve-color registry (issue #68), reached through the
// SessionManager the widget already holds. Returns nullptr when no session is
// wired (e.g. a service-less PlotWidget in a unit test), in which case the base
// class's per-widget nextColor() rotation is the fallback.
CurveColorRegistry* colorRegistryOf(SessionManager* session) {
  return session != nullptr ? &session->curveColorRegistry() : nullptr;
}

// The `<prefix>dataset_id/_source/_path` qualifier triple as saved (see
// PlotWidget::stampDatasetIdentity); an absent attribute reads as 0 / empty.
struct SavedDatasetIdentity {
  DatasetId id = 0;
  QString source;
  QString path;
};

SavedDatasetIdentity readDatasetIdentity(const QDomElement& element, const QString& prefix = {}) {
  return SavedDatasetIdentity{
      .id = element.attribute(prefix + "dataset_id"_L1).toUInt(),
      .source = element.attribute(prefix + "dataset_source"_L1),
      .path = element.attribute(prefix + "dataset_path"_L1)};
}

QString markerScopeName(MarkerScope scope) {
  return scope == MarkerScope::kDataset ? QString(plot_xml::kMarkerScopeDataset)
                                        : QString(plot_xml::kMarkerScopeAllDatasets);
}

// An absent or unrecognized `name` maps to nullopt — the entry is ignored rather
// than guessed at.
std::optional<MarkerScope> markerScopeFromName(QStringView name) {
  if (name == plot_xml::kMarkerScopeDataset) {
    return MarkerScope::kDataset;
  }
  if (name == plot_xml::kMarkerScopeAllDatasets) {
    return MarkerScope::kAllDatasets;
  }
  return std::nullopt;
}

}  // namespace

PlotWidget::PlotWidget(SessionManager* session, CatalogModel* catalog, QWidget* parent)
    : PlotWidgetBase(parent), session_(session), catalog_(catalog) {
  state_id_ = newStateId();
  setAcceptDrops(true);
  const auto fw_theme = theme::themeFor(QGuiApplication::palette().color(QPalette::Window).lightness() >= 128);
  // Highlight playback tracker, matching the Source Timeline's playhead needle
  // for cross-widget visual consistency. The reference tracker uses Accent,
  // matching the timeline's reference line.
  tracker_ =
      new CurveTracker(qwtPlot(), theme::interaction(theme::Variant::Highlight, theme::State::Checked, fw_theme));
  reference_tracker_ =
      new CurveTracker(qwtPlot(), theme::interaction(theme::Variant::Accent, theme::State::Checked, fw_theme));
  reference_tracker_->setParameter(CurveTracker::kLineOnly);
  reference_tracker_->setEnabled(false);

  // Marker overlay: draws findings (regions / events / value bands / labels)
  // read from the session ObjectStore (serialized PlotMarkers object topics) for
  // the series shown on this plot plus each dataset's Dataset/Global marker
  // scopes. Targets are recomputed from the current curves on each paint;
  // repaint is driven by SessionManager::markersChanged.
  markers_item_ = new PlotMarkersItem(session_);
  markers_item_->setTargetsProvider([this]() { return markerTargets(); });
  markers_item_->attach(qwtPlot());

  // Mouse-hover inspector. Shows a snap-to-curve dot + value tooltip wherever
  // the mouse points, gated by show_points_. Independent from the playback
  // tracker (tracker_): the playback line always shows the current
  // playback time and is not affected by this toggle.
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
  gesture_undo_debounce_.setSingleShot(true);
  gesture_undo_debounce_.setInterval(kGestureUndoDebounceMs);
  connect(&gesture_undo_debounce_, &QTimer::timeout, this, [this]() {
    if (!loading_state_) {
      emit undoableChange();
    }
  });
  connect(this, &PlotWidgetBase::curveListChanged, this, [this]() {
    updateMaximumZoomArea();
    autoZoomPlotVertically();
    pruneHiddenMarkerScopes();
    (void)refreshMarkerScopeRowSet();  // curveListChanged itself is the rebuild trigger
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

  // Auto-assignment (issue #68): reuse the curve's remembered color so it stays
  // consistent across plots; otherwise take the next color from the shared,
  // session-wide palette counter and remember it. An explicit (non-transparent)
  // color — e.g. from a layout file — is honored as-is. session_ is non-null here
  // (guarded above), so the session registry is always available.
  if (color == Qt::transparent) {
    CurveColorRegistry& registry = session_->curveColorRegistry();
    if (const auto remembered = registry.color(name); remembered.has_value()) {
      color = QColor(*remembered);
    } else {
      // New curve: choose the colour index from the configured sequence.
      // "global" advances the session-wide registry counter (one continuous
      // sequence across every plot); "per plot" uses this curve's positional
      // index within the plot, so each plot restarts at colour 0. Either way the
      // colour is then remembered by name (always-on), so the curve keeps it on
      // re-add and across plots.
      QSettings settings;
      const bool global = settings.value(u"Preferences::curve_color_global"_s, true).toBool();
      const int index = global ? registry.nextPaletteIndex() : static_cast<int>(curveList().size());
      color = PlotWidgetBase::paletteColor(index);
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
  updateMaximumZoomArea();
  replot();
  return info;
}

PlotWidget::CurveInfo* PlotWidget::addCurveFromPending(const QString& name, bool preserve_viewport) {
  const QScopedValueRollback guard(loading_state_, loading_state_ || preserve_viewport);
  return addCurve(name);
}

void PlotWidget::autoZoomPlotVertically() {
  // Skip during layout restore (the saved range wins), when there is nothing to
  // fit, and for XY plots (their X axis is data, not the shared time axis).
  if (loading_state_ || curveList().empty() || isXYPlot()) {
    return;
  }
  if (!QSettings().value(u"Preferences::auto_zoom_plots"_s, true).toBool()) {
    return;
  }
  // Rescale only Y, over the current X window, so the shared time axis — and
  // therefore the other plots — stay put.
  const Range<double> range_x = getVisualizationRangeX();
  const Range<double> range_y = getVisualizationRangeY(range_x);
  // Guard against a non-finite or degenerate range (e.g. a curve that is all
  // NaN/inf over the window) — feeding it to setAxisScale yields a blank axis.
  if (!std::isfinite(range_y.min) || !std::isfinite(range_y.max) || range_y.min >= range_y.max) {
    return;
  }
  setAxisScale(QwtPlot::yLeft, range_y.min, range_y.max);
  replot();
}

void PlotWidget::replaceCurve(const QString& source_key, const QString& output_key) {
  // No-op (header contract) if services are unset or the output is not in the catalog: never
  // drop the existing source curve when the replacement could not actually be added.
  if (session_ == nullptr || catalog_ == nullptr || !catalog_->curveDescriptor(output_key).has_value()) {
    return;
  }
  // Inherit the source curve's colour, if it is currently plotted, so the
  // filtered output takes its place with the same colour (PJ3 transform-in-place).
  QColor color = Qt::transparent;
  for (const CurveInfo& info : curveList()) {
    if (info.source_name == source_key && info.curve != nullptr) {
      color = info.curve->pen().color();
      break;
    }
  }
  removeCurve(source_key);      // no-op if the source is not currently plotted
  addCurve(output_key, color);  // explicit colour honoured as-is; transparent => palette
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

  // The alias is the curve's display title (legend + the title curveFromTitle()
  // keys on). Empty alias → auto "Y vs X" title (legacy / non-interactive callers).
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
  // Clears blue tracker + red tracker's reference_pos_. Otherwise, returning
  // to time-series mode later would resurrect stale Δ values without a blue line.
  setReferenceLine(std::nullopt);
  // Style/width are plot-level: addCurve() above already applied the plot's
  // current style and width. XY plots are not forced to Dots — they inherit the
  // plot-level style like any plot (PJ3 parity); the Curve Style toolbar controls it.
  updateMaximumZoomArea();
  // Reveal the ride-along marker at the current cursor immediately rather than on
  // the next seek/tick (setTrackerPosition replots).
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

  // Deliberately heap-owned and opened asynchronously: QDialog::exec() enters a
  // nested event loop, which an Asyncify-free Qt/WASM build cannot suspend.
  // Keep this one dialog alive across duplicate-name retries so the user's Swap
  // and alias edits are retained exactly like the desktop while(exec()) loop.
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
          // MessageBox::warning() is synchronous too, so reproduce it with the
          // same instance UI and reopen the XY dialog only after dismissal.
          auto* warning = new MessageBox(this);
          warning->setAttribute(Qt::WA_DeleteOnClose);
          warning->setTitle(tr("Duplicate name"));
          warning->setText(tr("A curve named \"%1\" already exists in this plot.").arg(alias));
          warning->addButton(MessageBox::tr("OK"), MessageBox::kPrimaryRole);
          connect(warning, &QDialog::finished, dialog, [dialog](int) {
            dialog->setWindowModality(Qt::ApplicationModal);
            dialog->show();
          });
          // QDialog::open() force-downgrades ApplicationModal to WindowModal.
          // show() preserves desktop exec()'s app-wide modality without nesting.
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
  QRectF rect = currentBoundingRect();
  rect.setLeft(x0);
  rect.setRight(x1);
  setZoomRectangle(rect, false);
  replot();
}

void PlotWidget::setZoomRectangle(QRectF rect, bool emit_signal) {
  if (isXYPlot() && keepRatioXY()) {
    // Every programmatic "return to original zoom" funnels through here
    // (zoomOut, the H/V zoom-outs, XML restore). We keep the requested zoom
    // level (PJ3 re-fits to the data extent) and re-impose the canvas aspect
    // ratio so the XY shape stays 1:1.
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
  const int precision = QSettings().value(u"Preferences::precision"_s, 3).toInt();

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
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
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

  // Replot only when the steady state actually changed: visibility flipped,
  // we snapped to a different sample, or the tooltip text differs (e.g. a
  // curve appeared/disappeared at the same x).
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
  // Style and width are plot-level properties (every curve shares them; only
  // colour is per-curve), so they are saved once on the <plot>, not per <curve>.
  plot_element.setAttribute(u"style"_s, curveStyleToString(curveStyle()));
  plot_element.setAttribute(u"title"_s, qwtPlot()->title().text());
  plot_element.setAttribute(u"tracker_enabled"_s, tracker_enabled_ ? u"true"_s : u"false"_s);

  // Skip the <range> element when the canvas has not yet computed a real
  // viewport (e.g. drop happened immediately before save) -- a degenerate
  // rect would restore as a zero-width window and hide everything. Without
  // <range>, xmlLoadState falls back to zoomOut(), which auto-fits.
  const QRectF rect = currentBoundingRect();
  if (rect.left() != rect.right() && rect.top() != rect.bottom()) {
    QDomElement range_element = doc.createElement(u"range"_s);
    range_element.setAttribute(u"bottom"_s, QString::number(rect.bottom(), 'f', 6));
    range_element.setAttribute(u"top"_s, QString::number(rect.top(), 'f', 6));
    // An explicit x_basis marker records what the X coordinates MEAN, so load never
    // has to infer it from plot mode: "absolute" = time-series wall-clock time,
    // "value" = an XY plot's raw data value.
    if (isXYPlot()) {
      // XY X is a data value, not time — offset-independent, stored verbatim.
      range_element.setAttribute(u"left"_s, QString::number(rect.left(), 'f', 6));
      range_element.setAttribute(u"right"_s, QString::number(rect.right(), 'f', 6));
      range_element.setAttribute(plot_xml::kXBasisAttribute, plot_xml::kXBasisValue);
    } else {
      // The X axis of a time-series plot is TIME: persist it in ABSOLUTE seconds,
      // not the display-relative seconds the Qwt axis speaks (display = absolute -
      // offset; see pj_runtime/Time.h). PJ4's display offset is per-dataset and
      // live, so a display-relative range only frames the right instant for the
      // offset present at save time — storing absolute makes the restored range
      // correct regardless of the offset state at load (the "Use time offset"
      // toggle, reloaded data, a layout shared between machines).
      // The authoritative edges are integer nanoseconds: quantize the SMALL display
      // value (rect.left()/right(), a few seconds at most) to ns first, then add the
      // epoch-scale offset in the INTEGER domain. Adding the offset as a double first
      // would round away a deeply-zoomed window (a double's ULP at ~1.6e9 s is ~238 ns).
      const qint64 offset_ns = displayOffsetNanoseconds();
      const qint64 absolute_left_ns =
          static_cast<qint64>(std::llround(rect.left() * kNanosecondsPerSecond)) + offset_ns;
      const qint64 absolute_right_ns =
          static_cast<qint64>(std::llround(rect.right() * kNanosecondsPerSecond)) + offset_ns;
      // The decimal left/right remain for human readability and as the v3 fallback;
      // they carry the same (lossy) absolute seconds older loaders would have read.
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

  // A curve is identified by its stable topic+field path (PJ::LayoutXml), not
  // the engine's opaque per-load catalog key. On load — layout file or undo/redo
  // snapshot alike — the path is re-resolved against the current dataset(s) to
  // the concrete key (PJ::LayoutXml::rebindCurveKeys), so the saved form stays
  // valid across reloads and similar datasets.
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
      // An XY curve's title is the user alias (not derivable from x/y), so persist it.
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

bool PlotWidget::xmlLoadState(const QDomElement& plot_element, bool autozoom) {
  if (plot_element.isNull() || plot_element.tagName() != "plot"_L1) {
    return false;
  }

  const bool saved_xy_mode = plot_element.attribute(u"mode"_s) == "XYPlot"_L1;
  std::vector<QDomDocument> parsed_pending_curves;
  std::set<QString> parsed_pending_identities;
  for (QDomElement curve = plot_element.firstChildElement(u"curve"_s); !curve.isNull();
       curve = curve.nextSiblingElement(u"curve"_s)) {
    if (!plot_xml::isPendingIntent(curve)) {
      continue;
    }
    const bool resolved = saved_xy_mode
                              ? !curve.attribute(u"curve_x"_s).isEmpty() && !curve.attribute(u"curve_y"_s).isEmpty()
                              : !curve.attribute(u"name"_s).isEmpty();
    if (resolved) {
      continue;
    }
    const QString identity = pendingCurveIdentity(curve);
    if (!isPendingCurveIntentValid(curve, saved_xy_mode) || !parsed_pending_identities.insert(identity).second) {
      return false;
    }
    QDomDocument pending_doc;
    pending_doc.appendChild(pending_doc.importNode(curve, /*deep=*/true));
    parsed_pending_curves.push_back(std::move(pending_doc));
  }

  setStateId(plot_element.attribute(u"id"_s));
  setModeXY(saved_xy_mode);
  // Line width is plot-level. New layouts store the chosen width on the <plot>.
  // Older layouts kept the plot-level value at the stale default ("1.0") and the
  // real width per-curve, so when the plot value is absent/default fall back to
  // the first saved curve's pixel width. New layouts no longer write a per-curve
  // line_width, so the fallback only fires for genuinely old files.
  const QString plot_line_width = plot_element.attribute(u"line_width"_s);
  const QDomElement first_curve = plot_element.firstChildElement(u"curve"_s);
  if ((plot_line_width.isEmpty() || plot_line_width == "1.0"_L1) && first_curve.hasAttribute(u"line_width"_s)) {
    setLineWidth(lineWidthFromPixels(first_curve.attribute(u"line_width"_s).toDouble()));
  } else {
    setLineWidth(lineWidthFromString(plot_line_width.isEmpty() ? u"1.0"_s : plot_line_width));
  }
  // Style is plot-level. New layouts store it on the <plot>; for older layouts
  // that stored it per <curve>, fall back to the first saved curve. setDefaultStyle
  // restyles the plot and is inherited by curves added after the load.
  QString style_attr = plot_element.attribute(u"style"_s);
  if (style_attr.isEmpty()) {
    style_attr = first_curve.attribute(u"style"_s, u"Lines"_s);
  }
  setDefaultStyle(curveStyleFromString(style_attr));
  setTrackerEnabled(plot_element.attribute(u"tracker_enabled"_s, u"true"_s) == "true"_L1);
  qwtPlot()->setTitle(plot_element.attribute(u"title"_s));

  const bool was_loading_state = loading_state_;
  loading_state_ = true;
  const bool intents_changing = !pending_curve_intents_.empty() || !parsed_pending_curves.empty();
  pending_curve_intents_ = std::move(parsed_pending_curves);
  if (intents_changing) {
    emit pendingCurveIntentsChanged();
  }

  std::set<QString> desired_keys;
  for (QDomElement curve_element = plot_element.firstChildElement(u"curve"_s); !curve_element.isNull();
       curve_element = curve_element.nextSiblingElement(u"curve"_s)) {
    if (isXYPlot() && curve_element.hasAttribute(u"curve_x"_s) && curve_element.hasAttribute(u"curve_y"_s)) {
      desired_keys.insert(curveKey(
          curve_element.attribute(u"name"_s), curve_element.attribute(u"curve_x"_s),
          curve_element.attribute(u"curve_y"_s)));
    } else {
      desired_keys.insert(curveKey(curve_element.attribute(u"name"_s)));
    }
  }

  QStringList remove_titles;
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
    }
  }
  for (const QString& title : remove_titles) {
    removeCurve(title);
  }

  for (QDomElement curve_element = plot_element.firstChildElement(u"curve"_s); !curve_element.isNull();
       curve_element = curve_element.nextSiblingElement(u"curve"_s)) {
    applyCurveElement(curve_element);
  }

  loadHiddenMarkerScopes(plot_element);

  // Stash the layout-saved viewport (raw, pre-offset-conversion) and frame to it via
  // the shared helper. During a progressive restore the catalog is still empty here,
  // so the absolute->display conversion is wrong until the dataset binds — the
  // progressive path re-applies the stash per curve-bind and at drain (see
  // applySavedViewportOrZoom / PendingCurveBinder). A blocking load resolves correctly
  // on this first call because the offset is already known.
  const QDomElement range_element = plot_element.firstChildElement(u"range"_s);
  if (!range_element.isNull() && autozoom) {
    SavedViewport view;
    view.bottom = range_element.attribute(u"bottom"_s).toDouble();
    view.top = range_element.attribute(u"top"_s).toDouble();
    view.left = range_element.attribute(u"left"_s).toDouble();
    view.right = range_element.attribute(u"right"_s).toDouble();
    // x_basis is authoritative: "value" = XY data value, anything else (incl. a
    // v3 layout with no marker, annotated to "absolute" by normalizePlotRangeBasis
    // on load) = absolute time. Never re-infer the basis from plot mode.
    view.x_is_absolute = range_element.attribute(plot_xml::kXBasisAttribute) != plot_xml::kXBasisValue;
    // Prefer the integer-ns edges (schema v4+) — they survive an epoch-scale round
    // trip exactly, unlike the decimal left/right which a v3 layout is limited to.
    if (view.x_is_absolute && range_element.hasAttribute(u"left_ns"_s) && range_element.hasAttribute(u"right_ns"_s)) {
      bool left_ok = false;
      bool right_ok = false;
      const qint64 left_ns = range_element.attribute(u"left_ns"_s).toLongLong(&left_ok);
      const qint64 right_ns = range_element.attribute(u"right_ns"_s).toLongLong(&right_ok);
      if (left_ok && right_ok) {
        view.left_ns = left_ns;
        view.right_ns = right_ns;
      }
    }
    saved_viewport_ = view;
  } else {
    saved_viewport_.reset();
  }
  applySavedViewportOrZoom(/*clear_after=*/false);
  replot();
  loading_state_ = was_loading_state;
  return true;
}

void PlotWidget::applySavedViewportOrZoom(bool clear_after) {
  QRectF rect;
  // The layout always stores a time axis in ABSOLUTE seconds; the per-dataset display
  // offset is a visualization concern applied HERE, never persisted. Convert the saved X
  // back to the CURRENT display coordinate (display = absolute - offset). An XY plot's X
  // is a data value, not time, so it is offset-independent and used verbatim. Re-running
  // this as the offset settles is what lets a progressive restore pin the saved window
  // up front and keep it framed while data streams in.
  if (saved_viewport_.has_value()) {
    const SavedViewport& view = *saved_viewport_;
    double left = 0.0;
    double right = 0.0;
    if (view.x_is_absolute && view.left_ns.has_value() && view.right_ns.has_value()) {
      // Authoritative path (schema v4+): subtract the display offset in the INTEGER
      // ns domain, THEN convert the small display value to seconds. Converting the
      // epoch-scale absolute value to a double first would round away a few-ns window
      // (a double's ULP at ~1.6e9 s is ~238 ns), so a deep-zoom viewport would drift.
      const qint64 left_ns = *view.left_ns;
      const qint64 right_ns = *view.right_ns;
      const bool degenerate = left_ns == right_ns;
      const qint64 offset_ns = displayOffsetNanoseconds();
      left = static_cast<double>(left_ns - offset_ns) / kNanosecondsPerSecond;
      right = static_cast<double>(right_ns - offset_ns) / kNanosecondsPerSecond;
      // Degenerate-range tolerance: a window saved while zoomed below 1 ns rounds to
      // equal ns edges. Falling through to auto-fit would wedge undo/redo after a deep
      // zoom -- the snapshot could never restore. Nudge the collapsed edges +-0.5 ns so a
      // real (if sub-pixel) window is restored. Near zero (offset ON) that already yields
      // two distinct doubles; with "Use time offset" OFF the display value sits at epoch
      // scale (~1.6e9 s) where 0.5 ns is far below the double ULP (~2.4e-7 s) and both
      // edges collapse back to one value -- so if the nudged window is STILL degenerate,
      // widen to the ULP-aware minimum around its center, which is guaranteed distinct.
      // The magnifier's floor keeps LIVE zooming out of this regime; this only rescues an
      // already-persisted degenerate range.
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
      // Fallback: a v3 decimal-only layout, or an XY value axis. The decimal seconds
      // are the only representation; an XY axis is offset-independent (used verbatim).
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
  // Degenerate or no saved range -> auto-fit (fresh load, or a layout saved before the
  // canvas computed a viewport).
  if (rect.left() == rect.right() || rect.top() == rect.bottom()) {
    zoomOut(/*emit_signal=*/false);
  } else {
    setZoomRectangle(rect, /*emit_signal=*/false);
  }
  if (clear_after) {
    saved_viewport_.reset();
  }
}

PlotWidget::CurveInfo* PlotWidget::applyCurveElement(const QDomElement& curve_element, bool preserve_viewport) {
  const QColor color(curve_element.attribute(u"color"_s));
  CurveInfo* loaded_curve = nullptr;
  if (isXYPlot() && curve_element.hasAttribute(u"curve_x"_s) && curve_element.hasAttribute(u"curve_y"_s)) {
    const QString x_name = curve_element.attribute(u"curve_x"_s);
    const QString y_name = curve_element.attribute(u"curve_y"_s);
    const QString source_name = curve_element.attribute(u"name"_s);
    for (CurveInfo& info : curveList()) {
      if (auto* xy_series = info.curve != nullptr ? dynamic_cast<PointSeriesXY*>(info.curve->data()) : nullptr) {
        if (curveKey(info.source_name, xy_series->xSource().name, xy_series->ySource().name) ==
            curveKey(source_name, x_name, y_name)) {
          loaded_curve = &info;
          break;
        }
      }
    }
    if (loaded_curve == nullptr) {
      // Restore the saved alias as the title; no dialog on load.
      const QScopedValueRollback guard(loading_state_, loading_state_ || preserve_viewport);
      loaded_curve = addCurveXY(x_name, y_name, source_name, color.isValid() ? color : Qt::transparent);
    }
  } else {
    const QString curve_name = curve_element.attribute(u"name"_s);
    loaded_curve = curveFromTitle(curve_name);
    if (loaded_curve == nullptr) {
      const QScopedValueRollback guard(loading_state_, loading_state_ || preserve_viewport);
      loaded_curve = addCurve(curve_name, color.isValid() ? color : Qt::transparent);
    }
  }
  if (loaded_curve != nullptr && loaded_curve->curve != nullptr && color.isValid()) {
    loaded_curve->curve->setPen(color, loaded_curve->curve->pen().widthF());
    // Seed the session color memory so this curve keeps its saved color when
    // later dragged into another plot (issue #68). Time-series only — see the
    // matching note in onChangeCurveColor; XY curves are out of scope here.
    if (CurveColorRegistry* registry = colorRegistryOf(session_); registry != nullptr && !isXYPlot()) {
      registry->setColor(loaded_curve->source_name, color.name());
    }
  }
  // Style and width are plot-level (restored once in xmlLoadState); only colour
  // and visibility are per-curve.
  if (loaded_curve != nullptr) {
    const QString visible_attr = curve_element.attribute(u"visible"_s, u"true"_s);
    loaded_curve->curve->setVisible(visible_attr == "true"_L1);
    const QString markers_attr = curve_element.attribute(u"markers_visible"_s, u"true"_s);
    loaded_curve->show_markers = (markers_attr == "true"_L1);
  }
  return loaded_curve;
}

void PlotWidget::zoomOut(bool emit_signal) {
  if (curveList().empty()) {
    setZoomRectangle(QRectF(0, 1, 1, -1), false);
    return;
  }

  updateMaximumZoomArea();
  setZoomRectangle(maxZoomRect(), emit_signal);
  replot();
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
    // An XY plot has no time axis, so the vertical-line tracker is meaningless.
    // Instead each curve's marker rides along the parametric curve, sitting on
    // the (x,y) sample at the current time (PJ3 parity).
    for (CurveInfo& info : curveList()) {
      if (info.marker == nullptr || info.curve == nullptr) {
        continue;
      }
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
    replot();
    return;
  }
  tracker_->setPosition(QPointF(display_time_sec, 0.0));
  replot();
}

void PlotWidget::onChangeCurveColor(const QString& curve_name, QColor new_color) {
  CurveInfo* info = curveFromTitle(curve_name);
  if (info != nullptr && info->curve != nullptr) {
    info->curve->setPen(new_color, info->curve->pen().widthF());
    // A curve symbol (the WASM Lines-and-Dots path) snapshots its color at
    // creation; recolor it too or the dots keep the old hue. The replacement
    // keeps the original's cache policy rather than resetting to AutoCache.
    if (const QwtSymbol* symbol = info->curve->symbol(); symbol != nullptr) {
      auto* recolored = new QwtSymbol(symbol->style(), new_color, QPen(new_color), symbol->size());
      recolored->setCachePolicy(symbol->cachePolicy());
      info->curve->setSymbol(recolored);
    }
    // Remember the override so the curve keeps this color when re-dragged into
    // another plot (issue #68). Keyed by source_name, the same key addCurve uses.
    // Time-series only: XY curves are keyed by a composed title and the registry
    // is never consulted for them (addCurveXY does not auto-assign from it), so
    // writing them here would only add entries nothing reads.
    if (CurveColorRegistry* registry = colorRegistryOf(session_); registry != nullptr && !isXYPlot()) {
      registry->setColor(info->source_name, new_color.name());
    }
    emit curveColorChanged(info->source_name, new_color);
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
  // Same style-to-Qwt mapping as PlotWidgetBase::setStyle(), minus the
  // pen-width assignment so per-curve width set via setCurveLineWidth()
  // survives a style toggle.
  applyStyleToCurve(info->curve, style);
  replot();
  if (!loading_state_) {
    emit undoableChange();
  }
}

void PlotWidget::setCurveVisible(const QString& curve_name, bool visible) {
  CurveInfo* info = curveFromTitle(curve_name);
  if (info == nullptr || info->curve == nullptr) {
    return;
  }
  info->curve->setVisible(visible);
  replot();
  emit undoableChange();
}

void PlotWidget::setCurveShowMarkers(const QString& curve_name, bool show) {
  CurveInfo* info = curveFromTitle(curve_name);
  if (info == nullptr || info->curve == nullptr) {
    return;
  }
  info->show_markers = show;
  // markers_item_'s targets are recomputed on each paint from show_markers,
  // so a replot is all that's needed to re-evaluate the overlay.
  replot();
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
  // markers_item_'s targets are recomputed on each paint from
  // hidden_marker_scopes_, so a replot is all that's needed to re-evaluate the
  // overlay.
  replot();
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
  // Only the HIDDEN (dataset, scope) pairs get a child — the common case (every
  // scope visible) writes nothing, and an absent child means visible on load.
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
  // Identity is resolved the same way a curve's dataset_id/dataset_source is (a
  // layout from another session may have reminted ids); ambiguous resolution
  // defaults to visible (skip). An unloaded dataset (resolution.id is nullopt)
  // is parked in pending_marker_scopes_ with its full saved identity, so the
  // entry can follow a reminted id once the pending binder lands its curves.
  // `scope` is required: an absent or unrecognized value leaves the entry out
  // (visible) rather than guessing.
  std::set<MarkerScopeKey> new_hidden_scopes;
  pending_marker_scopes_.clear();
  if (catalog_ != nullptr) {
    for (QDomElement marker_scope_element = plot_element.firstChildElement(u"marker_scope"_s);
         !marker_scope_element.isNull();
         marker_scope_element = marker_scope_element.nextSiblingElement(u"marker_scope"_s)) {
      if (marker_scope_element.attribute(u"visible"_s) != "false"_L1) {
        continue;
      }
      const std::optional<MarkerScope> scope =
          markerScopeFromName(marker_scope_element.attribute(plot_xml::kMarkerScopeAttribute));
      if (!scope.has_value()) {
        continue;
      }
      const SavedDatasetIdentity saved = readDatasetIdentity(marker_scope_element);
      if (saved.id == 0) {
        continue;
      }
      const auto resolution = catalog_->resolveDatasetIdentity(saved.id, saved.source, saved.path);
      if (resolution.ambiguous) {
        continue;
      }
      if (!resolution.id.has_value()) {
        pending_marker_scopes_.push_back(PendingMarkerScope{saved.id, saved.source, saved.path, *scope});
        continue;
      }
      new_hidden_scopes.insert(MarkerScopeKey{*resolution.id, *scope});
    }
  }
  if (new_hidden_scopes != hidden_marker_scopes_) {
    hidden_marker_scopes_ = std::move(new_hidden_scopes);
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
  // A dataset resolved here can already carry curves whose footer row never got
  // built (curveListChanged fired before this dataset's marker object topics
  // existed, e.g. a generator replayed after the pending binder ran) — refresh
  // the row set too, not just the hidden flag.
  refreshMarkerScopeRowSet();
  emit datasetMarkerScopesChanged();
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
    // The dataset counts as present regardless of show_markers — that flag
    // gates only this curve's own series target; each scope's target is gated
    // solely by hidden_marker_scopes_ below.
    datasets.insert(src.dataset_id);
    if (!info.show_markers) {
      continue;
    }
    // Per-series marker topic key is the human path "topic_name/field_name" —
    // the exact string a producer (e.g. the markers toolbox) receives when a
    // series is dropped on it (resolved via catalog_key_resolver), so the two
    // sides agree on the marker object-topic name.
    targets.push_back(
        MarkerTarget{
            src.dataset_id,
            QString::fromStdString(sdk::markerSeriesKey(src.topic_name.toStdString(), src.field_name.toStdString()))});
  }
  for (const DatasetId dataset : datasets) {
    for (const MarkerScope scope : {MarkerScope::kDataset, MarkerScope::kAllDatasets}) {
      if (hidden_marker_scopes_.contains(MarkerScopeKey{dataset, scope})) {
        continue;
      }
      const std::string_view topic = markerScopeTopic(scope);
      targets.push_back(MarkerTarget{dataset, QString::fromUtf8(topic.data(), static_cast<qsizetype>(topic.size()))});
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
    for (const DatasetId dataset_id : curveDatasets()) {
      for (const MarkerScope scope : publishedMarkerScopes(session_->objectStore(), dataset_id)) {
        current.emplace_back(dataset_id, scope);
      }
    }
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
  // A curve is stale once its source key is gone from the catalog. Collect first,
  // then remove: removeCurve() mutates curve_list, so removing while iterating
  // would invalidate the iterator.
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
  // If revalidation drained every curve, mirror removeAllCurves()'s reset out of
  // XY mode: onDragEnterEvent() only accepts an add_curve drop when !isXYPlot(),
  // so a stuck-XY empty plot would reject every new curve dropped onto it.
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
    // Mouse hover inspector (buttonShowpoint). Doesn't consume the event so
    // panning/zooming/etc keep working underneath. Gate by show_points_ so
    // the hot path stays a single bool read when the toggle is off.
    if (show_points_) {
      showPointValues(mouse_event->pos());
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
  if (curveList().empty()) {
    return;
  }
  if (isXYPlot()) {
    if (keepRatioXY()) {
      // Wheel-zoom (magnifier) and pan bypass the drag-zoom keep-ratio path,
      // and the magnifier clamps each axis independently at the data bounds —
      // that asymmetry skews a 1:1 circle into an ellipse. Re-impose the
      // canvas aspect ratio on the event's rect — the exact proposed gesture
      // viewport, immune to canvas-map round-off.
      applyRectKeepingRatio(rect);
      replot();
    }
    gesture_undo_debounce_.start();
    return;  // XY never emits rectChanged (PJ3 parity).
  }
  if (isZoomLinkEnabled()) {
    emit rectChanged(this, rect);
  }
  // rectChanged reaches linked peers immediately; the history snapshot is
  // debounced so a continuous gesture publishes once, after its last event.
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
    // Accept a new XY pair on an empty plot OR on a plot that is already XY (a plot
    // can hold several XY curves) — but never mixed with time-series curves.
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
          // No fabricated curve — the shell (TopicDemandController) registers
          // demand and a pending bind that completes once real data arrives.
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
      // The source-side WASM drag loop must finish unwinding before any dialog
      // result mutates the plot. Accept and clear the transient drag state now;
      // the completion reproduces the synchronous desktop finalization later.
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
        emit curvesDropped();
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
        // Cancelled the dialog on a plot that was not already XY: don't strand the
        // (still empty) plot in XY mode, or it would silently refuse normal drops.
        setModeXY(false);
      }
#endif
    }
  }

  if (curves_changed) {
    event->acceptProposedAction();
    emit curvesDropped();
    if (was_empty) {
      zoomOut(true);
    } else {
      replot();
    }
    emit undoableChange();
  }
  dragging_ = {};
}

void PlotWidget::buildActions() {
  // Icons are re-applied with the current theme each time the
  // context menu opens (see canvasContextMenuTriggered), so they
  // don't need to be set here.
  action_split_horizontal_ = new QAction(tr("&Split Horizontally"), this);
  connect(action_split_horizontal_, &QAction::triggered, this, &PlotWidget::splitHorizontal);

  action_split_vertical_ = new QAction(tr("&Split Vertically"), this);
  connect(action_split_vertical_, &QAction::triggered, this, &PlotWidget::splitVertical);

  action_remove_all_curves_ = new QAction(tr("&Remove ALL curves"), this);
  connect(action_remove_all_curves_, &QAction::triggered, this, &PlotWidget::removeAllCurves);
  connect(action_remove_all_curves_, &QAction::triggered, this, &PlotWidget::undoableChange);

  action_zoom_out_ = new QAction(tr("&Zoom Out"), this);
  connect(action_zoom_out_, &QAction::triggered, this, [this]() {
    zoomOut(true);
    emit undoableChange();
  });

  action_zoom_out_horizontal_ = new QAction(tr("&Zoom Out Horizontally"), this);
  connect(action_zoom_out_horizontal_, &QAction::triggered, this, [this]() {
    onZoomOutHorizontalTriggered(true);
    emit undoableChange();
  });

  action_zoom_out_vertical_ = new QAction(tr("&Zoom Out Vertically"), this);
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
  stampClipboardCurveKeys(plot_element);
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
  // Exact-id + raw-source qualifiers so a same-topic sibling dataset is never
  // interchanged on restore. The raw DatasetInfo::source_name is preferred over
  // the deduped display label because it is the portable identity a
  // later-session reload compares against.
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

void PlotWidget::stampClipboardCurveKeys(QDomElement& plot_element) const {
  // Full-path qualifier so a clipboard paste in a later session can validate a
  // reminted id (see rebindClipboardCurveKeys). xmlSaveState already stamped
  // dataset_id + dataset_source; the path is the tiebreak for same-basename files.
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
    stampDatasetPath(marker_scope_element, readDatasetIdentity(marker_scope_element).id);
  }
}

void PlotWidget::rebindClipboardCurveKeys(QDomElement& plot_element) const {
  if (catalog_ == nullptr) {
    return;
  }
  // Clipboard XML carries the same dataset qualifiers as any snapshot (dataset_id +
  // dataset_source + full dataset_path, written by xmlSaveState/stampClipboardCurveKeys),
  // and the clipboard outlives undo-history epochs: a pasted id may be stale or reminted.
  // Three-way result:
  //   value  = resolved concrete key -> set it;
  //   empty  = the curve WAS qualified but resolution failed -> CLEAR the concrete key,
  //            so a stale opaque key from another session can never collide with a live
  //            different-dataset series;
  //   nullopt = unqualified legacy copy that stays ambiguous -> leave the copied key
  //             intact (today's behavior; xmlLoadState drops it if it no longer resolves).
  const auto resolve = [this](const QDomElement& curve, const QString& prefix) -> std::optional<QString> {
    const QString topic = curve.attribute(prefix + "topic"_L1);
    const QString field = curve.attribute(prefix + "field"_L1);
    if (topic.isEmpty() || field.isEmpty()) {
      return std::nullopt;
    }
    const auto [dataset_id, dataset_source, dataset_path] = readDatasetIdentity(curve, prefix);
    // Shared resolver (same three tiers as layout/undo restore, incl. the
    // full-path-fallback leg the local scan used to lack) — a resolved key comes
    // back concrete. A miss is nullopt, so map it to the 3-way apply below by
    // re-deriving whether the curve WAS qualified: qualified-miss clears the key
    // (never let a stale opaque key collide with a live different-dataset series),
    // unqualified-miss leaves it untouched (today's behavior).
    const auto key = catalog_->resolveCurveKey(dataset_id, dataset_source, dataset_path, topic, field);
    if (key.has_value()) {
      return key;
    }
    const bool was_qualified = dataset_id != 0 || !dataset_source.isEmpty() || !dataset_path.isEmpty();
    return was_qualified ? std::optional<QString>{QString{}} : std::nullopt;
  };

  // Applies a resolve() result to one key attribute: overwrite on success, remove
  // on qualified failure (empty), leave untouched when the result is nullopt.
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
      // An XY curve is undrawable unless BOTH axes resolve. If either axis was
      // qualified-but-failed, clear both keys so no stale half survives.
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

#ifdef PJ_TARGET_WASM
  // QMenu::exec() enters a nested event loop, which requires Asyncify in Qt
  // WASM. Keep the production browser build Asyncify-free and retain the menu
  // until its asynchronous popup closes. Actions and their handlers are shared
  // unchanged with desktop.
  auto* wasm_menu = new QMenu(qwtPlot());
  wasm_menu->setAttribute(Qt::WA_DeleteOnClose);
  QMenu& menu = *wasm_menu;
#else
  QMenu menu(qwtPlot());
#endif
  menu.setObjectName(u"PJMenu"_s);
  menu.setProperty("categorySeparators", true);
  // Refresh icons with the active theme on every popup so the
  // glyphs stay correctly tinted after a theme switch.
  const QString theme = currentTheme();
  action_split_horizontal_->setIcon(QIcon(loadSvg(":/resources/svg/add_column.svg", theme)));
  action_split_vertical_->setIcon(QIcon(loadSvg(":/resources/svg/add_row.svg", theme)));
  action_remove_all_curves_->setIcon(QIcon(loadSvg(":/resources/svg/delete_forever.svg", theme)));
  action_zoom_out_->setIcon(QIcon(loadSvg(":/resources/svg/zoom_max.svg", theme)));
  action_zoom_out_horizontal_->setIcon(QIcon(loadSvg(":/resources/svg/zoom_horizontal.svg", theme)));
  action_zoom_out_vertical_->setIcon(QIcon(loadSvg(":/resources/svg/zoom_vertical.svg", theme)));
  // Apply Filter...: open the Filter Editor scoped to this plot's curves. On
  // Save it applies the chosen filter (via DataProcessorService) and adds the
  // resulting filtered curve(s) to this plot. Filters are time-series transforms,
  // so the action is hidden on XY (scatter) plots.
  if (session_ != nullptr && catalog_ != nullptr && !curveList().empty() && !isXYPlot()) {
    menu.addAction(QIcon(loadSvg(":/resources/svg/function.svg", theme)), tr("Apply Filter..."), this, [this]() {
      launchFilterEditor();
    });
    addActionCategorySeparator(menu);
  }

  menu.addAction(
      QIcon(loadSvg(":/resources/svg/copy.svg", theme)), tr("Copy"), this, [this]() { copyWidgetToClipboard(); });
  QAction* paste_action = menu.addAction(
      QIcon(loadSvg(":/resources/svg/paste.svg", theme)), tr("Paste"), this, [this]() { pasteWidgetFromClipboard(); });
  paste_action->setEnabled(canPasteWidgetFromClipboard());
  addActionCategorySeparator(menu);

  menu.addAction(action_split_horizontal_);
  menu.addAction(action_split_vertical_);
  addActionCategorySeparator(menu);
  menu.addAction(action_zoom_out_);
  menu.addAction(action_zoom_out_horizontal_);
  menu.addAction(action_zoom_out_vertical_);
  addActionCategorySeparator(menu);
  menu.addAction(action_remove_all_curves_);
  action_remove_all_curves_->setEnabled(!curveList().empty());
#ifdef PJ_TARGET_WASM
  menu.popup(qwtPlot()->canvas()->mapToGlobal(pos));
#else
  menu.exec(qwtPlot()->canvas()->mapToGlobal(pos));
#endif
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
  // The Filter Editor is a chart-area takeover panel owned by the host (it needs
  // MainWindow::presentPanel), so request it rather than constructing it here.
  // On Apply the host calls replaceCurve() on this plot for each result.
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
  // The axis is shared across curves; in the common case they share a dataset
  // (hence one offset). When they don't, the first datastore-backed curve's
  // dataset is the representative — the same rule must hold at save and load so
  // the absolute<->display round-trip is stable.
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
  // Older layouts stored the line width per-curve as a raw pen width, which could be
  // either the rendered lineWidthValue() or the raw toolbar value (lineWidthValue is
  // `scale` times the raw value). Pick the LineWidth closest across both scales,
  // deriving every candidate from lineWidthValue() so the width ladder lives in one place.
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

  // Markers changed (a producer republished or cleared a marker object topic):
  // the overlay re-reads the ObjectStore on each paint, so a plain replot suffices.
  // The row-set signal fires only when a topic actually appeared or was retired,
  // so a streaming producer re-publishing at up to 60 Hz doesn't rebuild the
  // CurveEditor's rows on every tick.
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
          // Follow-live: re-fit axes so streaming samples beyond the initial
          // drop-time range become visible. resetZoom subsumes
          // updateMaximumZoomArea + replot.
          resetZoom();
        } else {
          // One-shot writers (file load) stay zoomed where the user left them.
          updateMaximumZoomArea();
          replot();
        }
      });

  // In-place reload swaps a dataset's chunks under our adapters, freeing the raw
  // TopicChunk* they cache (sample_index_ / xy index). Drop those caches
  // synchronously BEFORE the swap to avoid a UAF; bindings (DatasetId/TopicIds)
  // stay valid and re-index on the next paint via the post-swap samplesIngested.
  // Forced DirectConnection so the clear runs inline within the emit (same
  // thread, never queued) — the no-event-loop UAF contract requires it.
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

  // Global "Use time offset" toggled (or otherwise re-based): every curve's x
  // shifts by a constant and NO topic changed, so a per-topic samplesIngested
  // would skip them all. Translate the current viewport by the same frame delta
  // so its absolute interval survives the automatic global rebase.
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
    // The curves just moved to the new frame, so each tracker's cached
    // intersection markers (the circles) were sampled against the OLD data and
    // view rect — re-sample them at their current positions against the re-fit
    // curves. Order-independent with the app's reference re-projection: whichever
    // of {reposition, this redraw} runs last re-samples against re-fit data, so
    // the blue line keeps its dots across the toggle.
    if (tracker_ != nullptr) {
      tracker_->redraw();
    }
    if (reference_tracker_ != nullptr) {
      reference_tracker_->redraw();
    }
    replot();
  });

  // ONE source's display offset changed (e.g. a Timeline drag). The samples did
  // not move — only their X mapping. Drop the bound adapters' offset caches and
  // replot at the CURRENT zoom so the curve slides into its new position without
  // re-fitting axes. Unlike datasetAboutToBeReplaced there is no UAF window
  // (chunks stay alive), so a normal (auto/queued) connection is fine.
  // PointSeriesXY ignores display_offset (plan §12) — skip it.
  display_offset_dataset_connection_ = connect(
      session_, qOverload<PJ::DatasetId>(&SessionManager::displayOffsetChanged), this, [this](DatasetId dataset_id) {
        if (invalidateAdapterOffsets(dataset_id)) {
          // The max zoom-out rect (and the magnifier's wheel-out clamp) is
          // view-independent data state: refresh it here or it stays frozen at
          // the pre-drag union of curve ranges.
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
      continue;  // PointSeriesXY ignores display offset (plan §12)
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
