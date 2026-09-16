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

// Identifies one dataset's marker scope — the CurveEditor pseudo-row unit and
// the hidden-flag key.
using MarkerScopeKey = std::pair<DatasetId, MarkerScope>;

class PlotWidget : public PlotWidgetBase {
  Q_OBJECT
 public:
  explicit PlotWidget(SessionManager* session = nullptr, CatalogModel* catalog = nullptr, QWidget* parent = nullptr);
  ~PlotWidget() override;

  void setDataServices(SessionManager* session, CatalogModel* catalog);
  using PlotWidgetBase::addCurve;
  CurveInfo* addCurve(const QString& name, QColor color = Qt::transparent);
  // Adds an XY (scatter) curve pairing the x_name/y_name series. `alias` is the
  // curve's display title (legend + identity key); an empty alias falls back to an
  // auto "Y vs X" title. Style/width are inherited from the plot (Dots, because
  // entering XY mode defaults the plot style) — not forced per-curve.
  CurveInfo* addCurveXY(
      const QString& x_name, const QString& y_name, const QString& alias, QColor color = Qt::transparent);

#ifndef PJ_TARGET_WASM
  // Desktop interactive XY creation: pops the XYCurveDialog (Swap + alias) for
  // the two series, then adds the curve with the chosen orientation/alias.
  // Returns nullptr if the user cancels. Never used by layout/undo load.
  CurveInfo* createCurveXYInteractive(const QString& x_key, const QString& y_key);
#else
  // Browser counterpart to createCurveXYInteractive(). Qt/WASM builds are
  // Asyncify-free, so nested QDialog::exec() event loops are unavailable. The
  // callback runs after the nonblocking dialog (and any duplicate-name warning)
  // closes; it receives nullptr on cancel or validation failure.
  void createCurveXYInteractiveAsync(
      const QString& x_key, const QString& y_key, std::function<void(CurveInfo*)> on_finished);
#endif

  // Add (or look up, if already present) the curve described by a layout `<curve>`
  // element and apply its saved style (color, line_width, style, visible). Picks
  // time-series vs XY from the plot's mode + the element's curve_x/curve_y attrs,
  // and is idempotent — re-applying the same element rebinds in place rather than
  // duplicating. This is the per-curve half of xmlLoadState, exposed so progressive
  // layout restore can bind a curve later (once its topic finishes loading) WITHOUT
  // re-running xmlLoadState, whose remove-pass would drop the already-live curves.
  // Returns the curve, or null if the element's key attribute is empty/unresolvable.
  CurveInfo* applyCurveElement(const QDomElement& curve_element, bool preserve_viewport = false);

  /// Adds a curve while optionally suppressing the normal curve-list auto-fit.
  /// Pending completion uses this when an existing or layout-restored viewport
  /// must stay fixed as the late curve materializes.
  CurveInfo* addCurveFromPending(const QString& name, bool preserve_viewport);

  // Replace the curve plotting `source_key` with one plotting `output_key`, the
  // new curve inheriting the source's color and taking its place (PJ3 in-place
  // transform semantics — the Filter Editor's "filtered series replaces the
  // input, same colour"). If `source_key` is absent, `output_key` is simply
  // added. No-op if data services are unset or `output_key` is not in the catalog.
  void replaceCurve(const QString& source_key, const QString& output_key);

  // Drops curves whose source key is gone from the catalog (XY drops if either X
  // or Y source is gone), keeps the rest. One replot; returns whether anything
  // changed. Symmetric with IObjectViewer::revalidateObjects.
  bool revalidate();

  void setZoomRectangle(QRectF rect, bool emit_signal);
  // Applies [x0, x1] as the visible X window, leaving Y exactly as it was, and
  // replots. Never emits the zoom-changed signal: the linked-X paths that use
  // it are already propagating a peer's change or an explicit command.
  void setVisibleXRange(double x0, double x1);
  [[nodiscard]] bool isZoomLinkEnabled() const noexcept;
  void setTrackerEnabled(bool enabled);
  [[nodiscard]] bool trackerEnabled() const noexcept;
  void setTrackerParameter(CurveTracker::Parameter parameter);
  // The tracker's current display level (line only / line + value / + name).
  [[nodiscard]] CurveTracker::Parameter trackerParameter() const noexcept;
  // Whether the playback tracker's floating value box is currently visible
  // (always hidden in line-only mode).
  [[nodiscard]] bool trackerValueBoxVisible() const noexcept;
  // Sets (or clears, when nullopt) a blue reference line. While set, the red
  // playback tracker renders values as deltas from this X. No-op on XY plots.
  void setReferenceLine(std::optional<double> reference_x_sec);
  // Mouse-hover inspector. Independent from the playback tracker_; toggling
  // this off does not hide the playback red line.
  void setShowPoints(bool show);
  [[nodiscard]] bool showPoints() const noexcept;
  [[nodiscard]] bool pointInspectorVisible() const noexcept;
  // Hidden inspectors expose no stale sample state.
  [[nodiscard]] QPointF pointInspectorPosition() const noexcept;
  [[nodiscard]] QString pointInspectorLabel() const;
  // Enable/disable the canvas right-click context menu (off for read-only
  // previews, e.g. the Filter Editor's preview plot). Default on.
  void setContextMenuEnabled(bool enabled) noexcept {
    context_menu_enabled_ = enabled;
  }
  [[nodiscard]] QString stateId() const;
  void setStateId(QString id);
  [[nodiscard]] QDomElement xmlSaveState(QDomDocument& doc) const;
  bool xmlLoadState(const QDomElement& plot_element, bool autozoom = true);

  /// Retains one unresolved ordinary <curve pending_intent="true"> element as
  /// real workspace state until the pending binder fulfills it.
  bool rememberPendingCurveIntent(const QDomElement& curve_element, bool notify = true);
  void forgetPendingCurveIntent(const QDomElement& curve_element, bool notify = false);
  [[nodiscard]] std::size_t pendingCurveIntentCount() const noexcept {
    return pending_curve_intents_.size();
  }
  [[nodiscard]] bool hasSavedViewport() const noexcept {
    return saved_viewport_.has_value();
  }

  // Re-frame to the viewport stashed by the last xmlLoadState, converting any
  // absolute-time X with the display offset in effect NOW. xmlLoadState applies it
  // once, but during a PROGRESSIVE restore the catalog is still empty then (offset
  // 0), so the conversion is wrong until the dataset binds — progressive restore
  // calls this again per curve-bind and at drain to frame the plot to its final
  // (layout-saved) window up front and keep it pinned while data streams in, instead
  // of auto-fitting to the partial data. Falls back to zoomOut() when there is no
  // usable saved viewport (fresh load, or a degenerate/missing <range>). clear_after
  // drops the stash (pass true on the final drain pass).
  void applySavedViewportOrZoom(bool clear_after = false);

  // Datasets with >=1 DatastoreCurveAdapter curve on this plot (XY curves don't
  // count — they have no single owning dataset). Sorted, deduped; the CurveEditor
  // uses it to decide whether a marker-scope row's label needs the dataset name.
  [[nodiscard]] std::vector<DatasetId> curveDatasets() const;
  // The (dataset, scope) rows the CurveEditor's footer lists: every curveDatasets()
  // entry (ascending) × the scopes publishedMarkerScopes reports for it.
  // Refreshed on curveListChanged and markersChanged; datasetMarkerScopesChanged()
  // announces a change.
  [[nodiscard]] const std::vector<MarkerScopeKey>& markerScopeRows() const noexcept {
    return marker_scope_rows_;
  }
  // dataset_name of the first curve on this plot backed by `dataset_id`, or a
  // generic fallback if none is currently plotted.
  [[nodiscard]] QString datasetDisplayName(DatasetId dataset_id) const;
  // Whether `dataset_id`'s marker set for `scope` is currently drawn on this
  // plot. Default true; independent of every curve's own show_markers flag and
  // of the other scope's visibility.
  [[nodiscard]] bool datasetMarkerScopeVisible(DatasetId dataset_id, MarkerScope scope) const;
  // The (dataset, topic) targets PlotMarkersItem draws, recomputed from the
  // current curves + the per-dataset per-scope visibility flags. Exposed as the
  // test seam.
  [[nodiscard]] std::vector<MarkerTarget> markerTargets() const;

 public slots:
  void zoomOut(bool emit_signal = true);
  void onZoomOutHorizontalTriggered(bool emit_signal = true);
  void onZoomOutVerticalTriggered(bool emit_signal = true);
  void setTrackerPosition(double display_time_sec);
  void onChangeCurveColor(const QString& curve_name, QColor new_color);
  // Per-curve mutators (vs the plot-wide setLineWidth / overrideCurvesStyle).
  void setCurveLineWidth(const QString& curve_name, double width);
  void setCurveStyle(const QString& curve_name, CurveStyle style);
  void setCurveVisible(const QString& curve_name, bool visible);
  // Toggle whether this curve contributes plot markers to the overlay (see
  // CurveInfo::show_markers). Independent from setCurveVisible.
  void setCurveShowMarkers(const QString& curve_name, bool show);
  // Shows/hides `dataset_id`'s marker set for `scope` on this plot (the
  // CurveEditor's "Dataset markers" / "Global markers" pseudo-row toggle).
  // Independent of every curve's own show_markers and of the other scope.
  // No-op (no replot/undo) if the value doesn't change.
  void setDatasetMarkerScopeVisible(DatasetId dataset_id, MarkerScope scope, bool visible);
  // Re-resolves the hidden <marker_scope> entries whose dataset was not loaded
  // at xmlLoadState time (kept as pending saved identities, visible meanwhile)
  // and moves the ones that now name a live dataset into the hidden set —
  // possibly under a reminted id. The pending binder calls it for each plot it
  // bound a curve on, since a dataset's scopes only exist once its curves do.
  // Ambiguous entries stay pending. Also rebuilds markerScopeRows(), since a
  // generator replayed after this plot's curves resolved can leave its footer
  // row unbuilt; emits datasetMarkerScopesChanged on either change.
  void resolvePendingMarkerScopes();
  void removeAllCurves() override;

 signals:
  void rectChanged(PlotWidget* modified, QRectF rect);
  void undoableChange();
  void trackerMoved(QPointF point);
  void curvesDropped();
  void statusMessageRequested(QString message);
  /// The serialized unresolved-curve set changed and binder registrations must
  /// be rebuilt from the plot's current XML state.
  void pendingCurveIntentsChanged();
  // markerScopeRows() changed because a marker topic appeared or was retired
  // (markersChanged), or a layout restore replaced the hidden flags wholesale.
  // NOT emitted for a curve-list change: curveListChanged already reaches every
  // listener that rebuilds rows, so emitting both would rebuild twice.
  void datasetMarkerScopesChanged();
  // One (dataset, scope) row's visibility flag flipped (a toggle, or undo/redo).
  // Only that row's check state changes — no row set change.
  void markerScopeVisibilityChanged(DatasetId dataset_id, MarkerScope scope, bool visible);
  void splitHorizontal();
  void splitVertical();
  void curveColorChanged(QString curve_name, QColor color);
  // "Apply Filter…" was chosen for this plot's curves. The host opens the Filter
  // Editor panel (chart-area takeover) scoped to `sources` and, on Apply, calls
  // `origin->replaceCurve()` so the filtered output replaces each source in place.
  void filterEditorRequested(std::vector<CurveDescriptor> sources, PlotWidget* origin);
  // A dropped catalog key named an ADVERTISED (not-yet-subscribed) placeholder
  // topic rather than resolved data — dropping it must not fabricate a curve
  // (there is no storage id yet). The shell resolves demand + a pending bind
  // (see pj_app's TopicDemandController) instead of this widget.
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
  // Vertical-only auto-fit triggered when a curve is added or removed, gated by
  // the Preferences::auto_zoom_plots setting. Rescales the Y axis to the data
  // over the current X window only — the shared time axis (and thus sibling
  // plots) is never touched. No-op during layout restore (the saved range wins),
  // when empty, and for XY plots (no shared time axis).
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
  // Writes the `<prefix>dataset_id` + `<prefix>dataset_source` qualifiers that
  // identify `dataset_id` across reloads (<curve> uses "", "x_", "y_";
  // <marker_scope> uses ""). Nothing is written without a catalog.
  void stampDatasetIdentity(QDomElement& element, DatasetId dataset_id, const QString& prefix = {}) const;
  // Adds the `<prefix>dataset_path` tiebreak (same-basename files) when the
  // catalog knows a file path — clipboard-only, layouts stay path-free.
  void stampDatasetPath(QDomElement& element, DatasetId dataset_id, const QString& prefix = {}) const;
  // Adds current opaque curve keys + dataset paths to copied XML for same-session paste.
  void stampClipboardCurveKeys(QDomElement& plot_element) const;
  // Resolves copied stable topic/field paths to this session's curve keys.
  void rebindClipboardCurveKeys(QDomElement& plot_element) const;
  // Open the Filter Editor scoped to this plot's curves; on Save, add the
  // resulting filtered curve(s) to this plot.
  void launchFilterEditor();
  void setAxisScale(QwtAxisId axis_id, double min, double max);
  // The representative per-dataset display offset for this plot's time axis: the
  // offset of the FIRST datastore-backed curve's dataset. The axis is shared
  // across curves; in the common case they share a dataset (one offset), and when
  // they don't the first datastore-backed curve is the representative — the same
  // rule must hold at save and load so the absolute<->display round-trip is
  // stable. nullopt when there is no session or no datastore-backed curve (then
  // display == absolute). displayOffsetSeconds/Nanoseconds convert at the edge.
  [[nodiscard]] std::optional<DisplayOffset> representativeDisplayOffset() const;
  // representativeDisplayOffset() in seconds. Used to convert the saved X-axis
  // range between display-relative and absolute time at the layout save/load
  // boundary (xmlSaveState/xmlLoadState). Returns 0 when there is no offset.
  [[nodiscard]] double displayOffsetSeconds() const;
  // representativeDisplayOffset() in integer nanoseconds — the datastore's native
  // precision. Used at the viewport save/load boundary so an epoch-scale absolute
  // range can be built and undone in the ns domain without a double's ~238 ns ULP
  // rounding away a deeply-zoomed window. Returns 0 when there is no offset.
  [[nodiscard]] qint64 displayOffsetNanoseconds() const;
  void reconnectDataSignals();
  // Drop the cached display offset on every bound DatastoreCurveAdapter whose
  // dataset matches `only` (or on all bound adapters when nullopt), so the next
  // paint re-maps the curve's X into the new frame. Returns whether any adapter
  // matched. PointSeriesXY ignores display offset (plan §12) and is skipped. The
  // caller owns the post-action (re-fit vs replot-at-current-zoom).
  bool invalidateAdapterOffsets(std::optional<DatasetId> only = std::nullopt);
  // Drops hidden-flag entries for datasets no longer in curveDatasets() (last
  // curve of that dataset removed), so a later re-add starts every scope
  // visible again. No-op during layout restore or while curves are still
  // pending a bind (pruning here would drop the loaded flag before the dataset
  // re-links). Runs on curveListChanged only, which already rebuilds the rows.
  void pruneHiddenMarkerScopes();
  // The <marker_scope visible="false"> children of <plot>: one per hidden
  // (dataset, scope) pair, carrying the dataset identity like a curve does, plus
  // the pending entries verbatim so a snapshot taken mid-restore keeps them.
  void saveHiddenMarkerScopes(QDomDocument& doc, QDomElement& plot_element) const;
  void loadHiddenMarkerScopes(const QDomElement& plot_element);
  // Recomputes markerScopeRows(); true when it differs from the previous set.
  // The caller decides whether to announce it — markersChanged fires on every
  // streaming tick regardless of whether a topic actually appeared/disappeared.
  [[nodiscard]] bool refreshMarkerScopeRowSet();
  [[nodiscard]] QStringList decodeCurveDrop(const QMimeData* mime_data, const QString& format) const;
  [[nodiscard]] bool allCurvesKnown(const QStringList& curves) const;
  // Time-series drop gate (curveslist/add_curve): a curve name is droppable when
  // it resolves to real data OR names a scalar-shaped advertised placeholder
  // (kNone classification) — both are legitimate drop targets even though only
  // the former can materialize a curve immediately (onDropEvent routes the
  // latter to placeholderCurveDropped instead of addCurve). Unlike
  // allCurvesKnown (real curves only), used by the XY gesture, which has no
  // placeholder analogue.
  [[nodiscard]] bool allCurvesDroppable(const QStringList& curves) const;
  // True iff `name` is a scalar-shaped (kNone) advertised placeholder — i.e. a
  // droppable name allCurvesKnown would reject.
  [[nodiscard]] bool isPlaceholderCurveName(const QString& name) const;
  [[nodiscard]] static QString lineWidthToString(LineWidth width);
  [[nodiscard]] static LineWidth lineWidthFromString(QString value);
  // Maps a raw pen-width pixel value (older layouts' per-curve width) to the
  // closest LineWidth, tolerating both the raw {1.0,1.5,2.0,3.0} and the
  // lineWidthValue()-scaled forms.
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
  // Last display time pushed through setTrackerPosition, so a newly-created XY
  // curve can place its ride-along marker at the current cursor immediately
  // instead of waiting for the next playback tick / seek.
  double last_tracker_time_sec_ = 0.0;
  bool show_points_ = true;
  bool loading_state_ = false;
  // Trailing-edge coalescer for gesture-driven history snapshots: wheel-zoom and
  // pan emit per input event, but serializing the whole workspace per event is
  // wasteful — one undoableChange fires when the gesture goes quiet.
  QTimer gesture_undo_debounce_;
  QwtPlotMarker* show_point_marker_ = nullptr;
  QwtPlotMarker* show_point_text_ = nullptr;
  // Used to skip replot when the mouse drifts but the snapped sample is unchanged.
  QPointF show_point_last_pos_;
  QString show_point_last_text_;
  QString state_id_;

  // Viewport stashed by xmlLoadState so progressive restore can re-apply it once the
  // per-dataset display offset is known (see applySavedViewportOrZoom). X bounds are
  // ABSOLUTE seconds for a time axis (converted to display on apply) or raw values for
  // an XY plot; Y (bottom/top) is raw. Unset means "no saved range" -> auto-fit.
  struct SavedViewport {
    double bottom = 0.0;
    double top = 0.0;
    double left = 0.0;
    double right = 0.0;
    // Authoritative integer-nanosecond absolute X edges for a time axis. Present
    // only when the loaded <range> carried left_ns/right_ns (schema v4+); a v3
    // decimal-only range leaves these unset and applySavedViewportOrZoom falls
    // back to the double `left`/`right`. Doubles lose precision once an
    // epoch-scale value (~1.6e9 s) is stored, so the integer edges are preferred
    // whenever available to make a deep-zoom viewport exactly round-trippable.
    std::optional<qint64> left_ns;
    std::optional<qint64> right_ns;
    // True for a time axis (X is absolute time, converted with the display offset
    // on apply); false for an XY plot (X is a data value used verbatim). Decided
    // from the <range>'s explicit x_basis marker, never re-inferred from plot mode.
    bool x_is_absolute = true;
  };
  std::optional<SavedViewport> saved_viewport_;
  std::vector<QDomDocument> pending_curve_intents_;

  // Marker scopes hidden on this plot (absent = visible). Lives independently
  // of markerScopeRows() so a layout can restore "hidden" before the producing
  // rule has published the topic (see pruneHiddenMarkerScopes / xmlLoadState).
  std::set<MarkerScopeKey> hidden_marker_scopes_;
  // A hidden <marker_scope> whose dataset was not loaded when the layout was
  // read: the saved identity, re-resolved by resolvePendingMarkerScopes() once
  // the dataset (possibly reminted) exists. Pending means visible.
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
