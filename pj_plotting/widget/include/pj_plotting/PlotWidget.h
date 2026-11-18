#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_axis_id.h>

#include <QAction>
#include <QDomDocument>
#include <QDomElement>
#include <QMetaObject>
#include <QRectF>
#include <QStringList>
#include <QTimer>
#include <cstddef>
#include <cstdint>
#ifdef PJ_TARGET_WASM
#include <functional>
#endif
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "pj_plotting/CurveTracker.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_runtime/CurveDescriptor.h"
#include "pj_runtime/MarkerTopics.h"
#include "pj_runtime/Time.h"

class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;
class QMimeData;

namespace PJ {

class CatalogModel;
class SessionManager;
class PlotMarkersItem;
struct MarkerTarget;  // pj_plotting/PlotMarkersItem.h

/// The run state of one marker rule, as the plot's host reports it.
struct MarkerRuleStatus {
  QString rule_id;
  bool computing = false;
  QString error;  ///< why the rule's last run failed; empty when it did not

  bool operator==(const MarkerRuleStatus&) const = default;
};

/// An on-demand series that is not computed (yet), as the plot's host reports it.
struct SeriesCurveStatus {
  DatasetId dataset_id = 0;
  QString topic_name;
  std::optional<int> computing_percent;  ///< its replay's progress; nullopt: it does not compute

  bool operator==(const SeriesCurveStatus&) const = default;
};

class PlotWidget : public PlotWidgetBase {
  Q_OBJECT
 public:
  explicit PlotWidget(SessionManager* session = nullptr, CatalogModel* catalog = nullptr, QWidget* parent = nullptr);
  ~PlotWidget() override;

  void setDataServices(SessionManager* session, CatalogModel* catalog);
  using PlotWidgetBase::addCurve;
  CurveInfo* addCurve(const QString& name, QColor color = Qt::transparent);
  // Adds an XY (scatter) curve pairing the x_name and y_name series. `alias` is the curve's display title (legend and
  // identity key); empty falls back to an auto "Y vs X" title. Style and width come from the plot (Dots: entering XY
  // mode sets that default), not per curve.
  CurveInfo* addCurveXY(
      const QString& x_name, const QString& y_name, const QString& alias, QColor color = Qt::transparent);

#ifndef PJ_TARGET_WASM
  // Desktop XY creation: pops the XYCurveDialog (Swap + alias) for the two series, then adds the curve. Returns nullptr
  // if the user cancels. Layout and undo load never use it.
  CurveInfo* createCurveXYInteractive(const QString& x_key, const QString& y_key);
#else
  // Browser counterpart of createCurveXYInteractive(): Qt/WASM builds cannot nest QDialog::exec() event loops. The
  // callback runs after the nonblocking dialog (and any duplicate-name warning) closes, with nullptr on cancel or
  // validation failure.
  void createCurveXYInteractiveAsync(
      const QString& x_key, const QString& y_key, std::function<void(CurveInfo*)> on_finished);
#endif

  // Adds, or finds if already present, the curve a layout `<curve>` element describes and applies its saved style
  // (color, line_width, style, visible). Time-series or XY follows the plot's mode and the element's curve_x / curve_y
  // attributes. Idempotent: re-applying the same element rebinds in place. It is the per-curve half of xmlLoadState,
  // exposed so a progressive restore can bind a curve once its topic loads without re-running xmlLoadState, whose
  // remove pass would drop the live curves. Null when the element's key is empty or unresolvable.
  CurveInfo* applyCurveElement(const QDomElement& curve_element, bool preserve_viewport = false);

  /// Adds a curve, optionally without the curve-list auto-fit, so a viewport that was restored or must stay fixed holds
  /// while a late curve materializes (pending completion).
  CurveInfo* addCurveFromPending(const QString& name, bool preserve_viewport);

  // Replaces the curve plotting `source_key` with one plotting `output_key`, which inherits its color and place: the
  // Filter Editor's "filtered series replaces the input". If `source_key` is absent, `output_key` is simply added.
  // No-op if data services are unset or `output_key` is not in the catalog.
  void replaceCurve(const QString& source_key, const QString& output_key);

  // Drops curves whose source key left the catalog (an XY curve when either source did), keeps the rest, replots once
  // and returns whether anything changed. Symmetric with IObjectViewer::revalidateObjects.
  bool revalidate();

  void setZoomRectangle(QRectF rect, bool emit_signal);
  // Sets [x0, x1] as the visible X window and replots; Y stays as it was until a fit still waiting for first rows runs.
  // Never emits the zoom-changed signal: the linked-X paths that use it already propagate a peer's change or an
  // explicit command.
  void setVisibleXRange(double x0, double x1);
  using PlotWidgetBase::hasUsableCurveData;
  [[nodiscard]] bool isZoomLinkEnabled() const noexcept;
  void setTrackerEnabled(bool enabled);
  [[nodiscard]] bool trackerEnabled() const noexcept;
  void setTrackerParameter(CurveTracker::Parameter parameter);
  // The tracker's display level (line only, line + value, + name).
  [[nodiscard]] CurveTracker::Parameter trackerParameter() const noexcept;
  // Whether the playback tracker's floating value box is visible (always hidden in line-only mode).
  [[nodiscard]] bool trackerValueBoxVisible() const noexcept;
  [[nodiscard]] QString trackerValueBoxText() const;
  // Sets, or clears with nullopt, a blue reference line; while set, the red playback tracker shows values as deltas
  // from this X. No-op on XY plots.
  void setReferenceLine(std::optional<double> reference_x_sec);
  // Mouse-hover inspector, independent of the playback tracker: toggling it off does not hide the red line.
  void setShowPoints(bool show);
  [[nodiscard]] bool showPoints() const noexcept;
  [[nodiscard]] bool pointInspectorVisible() const noexcept;
  // Hidden inspectors expose no stale sample state.
  [[nodiscard]] QPointF pointInspectorPosition() const noexcept;
  [[nodiscard]] QString pointInspectorLabel() const;
  // Enables the canvas right-click context menu (off for read-only previews such as the Filter Editor's). Default on.
  void setContextMenuEnabled(bool enabled) noexcept {
    context_menu_enabled_ = enabled;
  }
  [[nodiscard]] QString stateId() const;
  void setStateId(QString id);
  [[nodiscard]] QDomElement xmlSaveState(QDomDocument& doc) const;
  bool xmlLoadState(const QDomElement& plot_element, bool autozoom = true);
  /// Adds this session's opaque curve keys (and dataset paths) to `plot_element`, this plot's xmlSaveState output, so
  /// xmlLoadState binds its curves without the shell's stable-path rebind: a same-session copy (clipboard, duplicated
  /// tab).
  void stampLiveCurveKeys(QDomElement& plot_element) const;

  /// Retains one unresolved ordinary <curve pending_intent="true"> element as workspace state until the pending binder
  /// fulfills it.
  bool rememberPendingCurveIntent(const QDomElement& curve_element, bool notify = true);
  void forgetPendingCurveIntent(const QDomElement& curve_element, bool notify = false);
  [[nodiscard]] std::size_t pendingCurveIntentCount() const noexcept {
    return pending_curve_intents_.size();
  }
  [[nodiscard]] bool hasSavedViewport() const noexcept {
    return saved_viewport_.has_value();
  }

  // Re-frames to the viewport the last xmlLoadState stashed, converting an absolute-time X with the display offset in
  // effect now. xmlLoadState applies it once, but during a progressive restore the catalog is still empty (offset 0),
  // so the conversion is wrong until the dataset binds: the restore calls this again per curve bind and at drain, to
  // frame the plot to its saved window up front instead of auto-fitting partial data. Falls back to zoomOut() when
  // there is no usable saved viewport (fresh load, degenerate or missing <range>). `clear_after` drops the stash (true
  // on the final drain pass).
  void applySavedViewportOrZoom(bool clear_after = false);

  // Datasets with at least one DatastoreCurveAdapter curve on this plot (XY curves have no single owning dataset),
  // sorted and deduped; the CurveEditor uses it to decide whether a marker-scope row's label needs the dataset name.
  [[nodiscard]] std::vector<DatasetId> curveDatasets() const;
  // The (dataset, scope) rows of the CurveEditor footer: every curveDatasets() entry (ascending) times the scopes
  // publishedMarkerScopes reports for it. Refreshed on curveListChanged and markersChanged;
  // datasetMarkerScopesChanged() announces a change.
  [[nodiscard]] const std::vector<MarkerScopeKey>& markerScopeRows() const noexcept {
    return marker_scope_rows_;
  }
  // dataset_name of the first curve backed by `dataset_id`, or a generic fallback if none is plotted.
  [[nodiscard]] QString datasetDisplayName(DatasetId dataset_id) const;
  // Whether `dataset_id`'s marker set for `scope` is drawn on this plot (default true); independent of every curve's
  // show_markers and of the other scope.
  [[nodiscard]] bool datasetMarkerScopeVisible(DatasetId dataset_id, MarkerScope scope) const;
  // The (dataset, topic) targets PlotMarkersItem draws, from the current curves and the per-dataset per-scope
  // visibility flags. A test seam.
  [[nodiscard]] std::vector<MarkerTarget> markerTargets() const;

 public slots:
  void zoomOut(bool emit_signal = true);
  void onZoomOutHorizontalTriggered(bool emit_signal = true);
  void onZoomOutVerticalTriggered(bool emit_signal = true);
  void setTrackerPosition(double display_time_sec);
  void onChangeCurveColor(const QString& curve_name, QColor new_color);
  // Per-curve mutators (the plot-wide ones are setLineWidth / overrideCurvesStyle).
  void setCurveLineWidth(const QString& curve_name, double width);
  void setCurveStyle(const QString& curve_name, CurveStyle style);
  // No-op, with no replot or undo step, if the visibility does not change.
  void setCurveVisible(const QString& curve_name, bool visible) override;
  // Whether this curve contributes plot markers to the overlay (CurveInfo::show_markers); independent of
  // setCurveVisible.
  void setCurveShowMarkers(const QString& curve_name, bool show);
  // Shows or hides `dataset_id`'s marker set for `scope` on this plot (the CurveEditor's "Dataset markers" / "Global
  // markers" row toggle), independent of every curve's show_markers and of the other scope. No-op, with no replot or
  // undo step, if the value does not change.
  void setDatasetMarkerScopeVisible(DatasetId dataset_id, MarkerScope scope, bool visible);
  /// Replaces the run state of the marker rules this plot shows (the host decides which; the plot knows no engine).
  /// While a rule computes the plot draws "Computing markers…" in its top-left corner (top-right when the legend is
  /// there), and "Marker rule failed" when only failures remain; the note's tooltip lists the errors. The markers stay
  /// drawn. An entry that neither computes nor failed is ignored. Repaints only when the note's text changes, so a host
  /// may call it on every engine event.
  void setMarkerStatus(std::vector<MarkerRuleStatus> statuses);
  /// Replaces the status of the series this plot may show (the host's, as for setMarkerStatus): a curve on one of
  /// these topics reads "(not computed)" or "(computing N%)" after its legend title, which stays its identity. Replots
  /// only when a legend changes, so a host may call it on every progress event.
  void setSeriesStatus(std::vector<SeriesCurveStatus> statuses);
  /// The text of the note `setMarkerStatus` put up; empty when none.
  [[nodiscard]] QString markerStatusNote() const;
  /// Where the note sits, in canvas coordinates; empty when there is none. A hover on it shows the errors as a tooltip.
  [[nodiscard]] QRectF markerStatusNoteRect() const;
  // Re-resolves the hidden <marker_scope> entries whose dataset was not loaded at xmlLoadState time (kept pending and
  // visible meanwhile) and moves those that now name a live dataset into the hidden set, possibly under a reminted id.
  // The pending binder calls it for each plot it bound a curve on, since a dataset's scopes exist only once its curves
  // do. Ambiguous entries stay pending. Also rebuilds markerScopeRows(), because a generator replayed after this plot's
  // curves resolved can leave its footer row unbuilt; emits datasetMarkerScopesChanged on either change.
  void resolvePendingMarkerScopes();
  void removeAllCurves() override;

 signals:
  void rectChanged(PlotWidget* modified, QRectF rect);
  /// The time (x) axis' range changed, whatever moved it: a gesture, a linked or programmatic zoom, an autoscale.
  /// Unlike rectChanged, it does not drive the zoom link.
  void visibleXRangeChanged();
  void undoableChange();
  void trackerMoved(QPointF point);
  /// A successful curve drop; was_empty identifies the first curves in this plot.
  void curvesDropped(bool was_empty);
  void statusMessageRequested(QString message);
  /// The serialized unresolved-curve set changed: binder registrations must be rebuilt from the plot's XML state.
  void pendingCurveIntentsChanged();
  // markerScopeRows() changed because a marker topic appeared or was retired (markersChanged) or a layout restore
  // replaced the hidden flags wholesale. Not emitted for a curve-list change: curveListChanged already reaches every
  // listener that rebuilds rows, and both would rebuild twice.
  void datasetMarkerScopesChanged();
  /// What `markerTargets()` returns may have changed (a curve added or removed, a curve's markers or a dataset's marker
  /// scope shown or hidden). The host re-derives the plot's marker status.
  void markerTargetsChanged();
  // One (dataset, scope) row's visibility flipped (a toggle, undo or redo); only that row's check state changes.
  void markerScopeVisibilityChanged(DatasetId dataset_id, MarkerScope scope, bool visible);
  void splitHorizontal();
  void splitVertical();
  void curveColorChanged(QString curve_name, QColor color);
  // A curve was shown or hidden through setCurveVisible (`curve_name` is its source_name): the curve editor's eye
  // follows without a rebuild.
  void curveVisibilityChanged(QString curve_name, bool visible);
  // "Apply Filter…" was chosen for this plot's curves: the host opens the Filter Editor panel scoped to `sources` and,
  // on Apply, calls `origin->replaceCurve()` so each filtered output replaces its source in place.
  void filterEditorRequested(std::vector<CurveDescriptor> sources, PlotWidget* origin);
  // A dropped catalog key named an advertised (not yet subscribed) placeholder topic, not resolved data, so no curve is
  // created (there is no storage id yet); the shell resolves demand and a pending bind (pj_app's
  // TopicDemandController).
  void placeholderCurveDropped(QString catalog_key);

 protected:
  bool eventFilter(QObject* obj, QEvent* event) override;

 private slots:
  // Copies this plot's XML state to the widget clipboard.
  void copyWidgetToClipboard();
  // Replaces this plot's state from compatible clipboard XML.
  void pasteWidgetFromClipboard();
  void onExternallyResized(const QRectF& rect);
  void onDragEnterEvent(QDragEnterEvent* event);
  void onDragLeaveEvent(QDragLeaveEvent* event);
  void onDropEvent(QDropEvent* event);

 private:
  // Re-samples both trackers at their current positions against the current curves and view; on an XY plot, which has
  // no tracker line, the markers at the last tracker time.
  void redrawTrackers();
  // XY plots: puts each curve's marker on its sample at `display_time_sec`, shown only for a visible curve and filled
  // with the curve's current colour. Does not replot.
  void updateXYMarkers(double display_time_sec);
  void beforeCanvasReplot() override;
  // Vertical-only auto-fit when a curve is added or removed, gated by Preferences::auto_zoom_plots: rescales Y to the
  // data over the current X window; the shared time axis (and sibling plots) is never touched. No-op during layout
  // restore (the saved range wins), when empty and for XY plots (no shared time axis).
  void autoZoomPlotVertically();

  // No-op when show_points_ is false.
  void showPointValues(QPoint paint_point);
  enum class DragMode { kNone, kCurves, kNewXY };

  struct DragInfo {
    DragMode mode = DragMode::kNone;
    QStringList curves;
  };

  void buildActions();
  void canvasContextMenuTriggered(const QPoint& pos);
  // True when the clipboard contains a plot XML payload.
  [[nodiscard]] bool canPasteWidgetFromClipboard() const;
  // Writes the `<prefix>dataset_id` and `<prefix>dataset_source` qualifiers that identify `dataset_id` across reloads
  // (<curve> uses "", "x_", "y_"; <marker_scope> uses ""). Nothing is written without a catalog.
  void stampDatasetIdentity(QDomElement& element, DatasetId dataset_id, const QString& prefix = {}) const;
  // Adds the `<prefix>dataset_path` tiebreak (same-basename files) when the catalog knows a file path: clipboard only,
  // layouts stay path-free.
  void stampDatasetPath(QDomElement& element, DatasetId dataset_id, const QString& prefix = {}) const;
  // Resolves copied stable topic/field paths to this session's curve keys.
  void rebindClipboardCurveKeys(QDomElement& plot_element) const;
  // Opens the Filter Editor scoped to this plot's curves; on Save the filtered curves are added to this plot.
  void launchFilterEditor();
  void setAxisScale(QwtAxisId axis_id, double min, double max);
  // The per-dataset display offset of this plot's time axis: that of the first datastore-backed curve's dataset. The
  // axis is shared across curves; usually they share a dataset, and when they do not the first one represents them, the
  // same rule at save and load so the absolute <-> display round trip is stable. nullopt without a session or a
  // datastore-backed curve (display == absolute). displayOffsetSeconds / Nanoseconds convert at the edge.
  [[nodiscard]] std::optional<DisplayOffset> representativeDisplayOffset() const;
  // representativeDisplayOffset() in seconds: converts the saved X range between display-relative and absolute time at
  // the layout save/load boundary. 0 when there is no offset.
  [[nodiscard]] double displayOffsetSeconds() const;
  // representativeDisplayOffset() in integer nanoseconds, the datastore's precision: lets an epoch-scale absolute range
  // be built and undone in the ns domain, where a double's ~238 ns ULP would round away a deeply zoomed window. 0 when
  // there is no offset.
  [[nodiscard]] qint64 displayOffsetNanoseconds() const;
  void reconnectDataSignals();
  // Drops the cached display offset of every bound DatastoreCurveAdapter on dataset `only` (all of them when nullopt),
  // so the next paint re-maps X into the new frame. Returns whether any adapter matched. PointSeriesXY ignores the
  // display offset and is skipped. The caller owns the post-action (re-fit or replot at the current zoom).
  bool invalidateAdapterOffsets(std::optional<DatasetId> only = std::nullopt);
  // Drops hidden-flag entries for datasets no longer in curveDatasets(), so a later re-add starts with every scope
  // visible. No-op during layout restore or while curves are pending a bind (pruning then would drop the flag before
  // the dataset re-links). Runs on curveListChanged only, which already rebuilds the rows.
  void pruneHiddenMarkerScopes();
  // The <marker_scope visible="false"> children of <plot>: one per hidden (dataset, scope) pair, with the dataset
  // identity a curve carries, plus the pending entries verbatim so a mid-restore snapshot keeps them.
  void saveHiddenMarkerScopes(QDomDocument& doc, QDomElement& plot_element) const;
  void loadHiddenMarkerScopes(const QDomElement& plot_element);
  // The errors of the failing rules, one per line, as the note's rich-text tooltip.
  [[nodiscard]] QString markerStatusToolTip() const;
  // Recomputes markerScopeRows(); true when it changed. The caller decides whether to announce it: markersChanged fires
  // on every streaming tick whether or not a topic appeared or disappeared.
  [[nodiscard]] bool refreshMarkerScopeRowSet();
  [[nodiscard]] QStringList decodeCurveDrop(const QMimeData* mime_data, const QString& format) const;
  [[nodiscard]] bool allCurvesKnown(const QStringList& curves) const;
  // Time-series drop gate (curveslist/add_curve): a name is droppable when it resolves to real data or is a
  // scalar-shaped (kNone) advertised placeholder; only the former materializes a curve at once (onDropEvent routes the
  // latter to placeholderCurveDropped). Unlike allCurvesKnown (real curves only), which the XY gesture uses.
  [[nodiscard]] bool allCurvesDroppable(const QStringList& curves) const;
  // True iff `name` is a scalar-shaped (kNone) advertised placeholder, a droppable name allCurvesKnown would reject.
  [[nodiscard]] bool isPlaceholderCurveName(const QString& name) const;
  [[nodiscard]] static QString lineWidthToString(LineWidth width);
  [[nodiscard]] static LineWidth lineWidthFromString(QString value);
  // Maps a raw pen-width in pixels (a per-curve width in older layouts) to the closest LineWidth, accepting both the
  // raw {1.0, 1.5, 2.0, 3.0} and the lineWidthValue()-scaled forms.
  [[nodiscard]] static LineWidth lineWidthFromPixels(double pixels);
  [[nodiscard]] static QString curveStyleToString(CurveStyle style);
  [[nodiscard]] static CurveStyle curveStyleFromString(QString value);

  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  QMetaObject::Connection samples_ingested_connection_;
  QMetaObject::Connection dataset_replace_connection_;
  QMetaObject::Connection markers_changed_connection_;
  QMetaObject::Connection display_offset_connection_;          // global "Use time offset" frame
  QMetaObject::Connection display_offset_dataset_connection_;  // per-source Timeline drag
  Timestamp last_global_time_reference_ = 0;
  DragInfo dragging_;
  PlotMarkersItem* markers_item_ = nullptr;
  CurveTracker* tracker_ = nullptr;
  CurveTracker* reference_tracker_ = nullptr;
  bool tracker_enabled_ = true;
  // Last display time pushed through setTrackerPosition, so a new XY curve can place its ride-along marker at the
  // cursor at once.
  double last_tracker_time_sec_ = 0.0;
  bool show_points_ = true;
  bool loading_state_ = false;
  // True while xmlLoadState swaps the curve set: the curveListChanged handler leaves the max zoom area to the single
  // recompute that follows.
  bool restoring_curves_ = false;
  // An axis scale or the curve list changed since the trackers last sampled the curves.
  bool trackers_stale_ = false;
  // A zoomOut that found no curve data (an on-demand series computes only once shown) stays armed and re-runs on every
  // non-live ingest, so a series computed in chunks stays framed whole until the user frames the view. An external X
  // window (zoom link) narrows it to Y. Never armed when data existed at fit time.
  enum class PendingFit : std::uint8_t { kNone, kFull, kVertical };
  PendingFit pending_fit_ = PendingFit::kNone;
  // Trailing-edge coalescer for gesture history snapshots: wheel zoom and pan emit per input event, and serializing the
  // workspace each time is wasteful, so one undoableChange fires when the gesture goes quiet.
  QTimer gesture_undo_debounce_;
  QwtPlotMarker* show_point_marker_ = nullptr;
  QwtPlotMarker* show_point_text_ = nullptr;
  // Skips the replot when the mouse drifts but the snapped sample is unchanged.
  QPointF show_point_last_pos_;
  QString show_point_last_text_;
  QString state_id_;

  // Viewport stashed by xmlLoadState so a progressive restore can re-apply it once the per-dataset display offset is
  // known (applySavedViewportOrZoom). X bounds are absolute seconds for a time axis (converted to display on apply) or
  // raw values for an XY plot; Y is raw. Unset: no saved range, auto-fit.
  struct SavedViewport {
    double bottom = 0.0;
    double top = 0.0;
    double left = 0.0;
    double right = 0.0;
    // Authoritative integer-nanosecond absolute X edges of a time axis, present only when the loaded <range> carried
    // left_ns / right_ns (schema v4+); a decimal-only (v3) range leaves them unset and applySavedViewportOrZoom falls
    // back to the double `left` / `right`. An epoch-scale double (~1.6e9 s) loses precision, so the integer edges are
    // preferred to make a deep-zoom viewport round-trip exactly.
    std::optional<qint64> left_ns;
    std::optional<qint64> right_ns;
    // True for a time axis (X is absolute time, converted with the display offset on apply); false for an XY plot (X is
    // a data value used verbatim). Taken from the <range>'s explicit x_basis marker, never re-inferred from the mode.
    bool x_is_absolute = true;
  };
  std::optional<SavedViewport> saved_viewport_;
  std::vector<QDomDocument> pending_curve_intents_;

  // Marker scopes hidden on this plot (absent = visible). Independent of markerScopeRows() so a layout can restore
  // "hidden" before the producing rule has published the topic (pruneHiddenMarkerScopes, xmlLoadState).
  std::set<MarkerScopeKey> hidden_marker_scopes_;
  // Sorted by rule id (setMarkerStatus).
  std::vector<MarkerRuleStatus> marker_status_;
  // setSeriesStatus's state, applied again to the curves whenever they change.
  std::vector<SeriesCurveStatus> series_status_;
  void applySeriesStatus();
  // A hidden <marker_scope> whose dataset was not loaded when the layout was read: the saved identity, re-resolved by
  // resolvePendingMarkerScopes() once the dataset (possibly reminted) exists. Pending means visible.
  struct PendingMarkerScope {
    DatasetId id = 0;
    QString source;
    QString path;
    MarkerScope scope = MarkerScope::kDataset;
  };
  std::vector<PendingMarkerScope> pending_marker_scopes_;
  // See markerScopeRows().
  std::vector<MarkerScopeKey> marker_scope_rows_;

  QAction* action_split_horizontal_ = nullptr;
  QAction* action_split_vertical_ = nullptr;
  QAction* action_remove_all_curves_ = nullptr;
  QAction* action_zoom_out_ = nullptr;
  QAction* action_zoom_out_horizontal_ = nullptr;
  QAction* action_zoom_out_vertical_ = nullptr;
  bool context_menu_enabled_ = true;
};

}  // namespace PJ
