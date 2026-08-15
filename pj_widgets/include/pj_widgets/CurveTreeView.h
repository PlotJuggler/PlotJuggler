#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QHash>
#include <QMetaType>
#include <QMimeData>
#include <QPersistentModelIndex>
#include <QPixmap>
#ifdef PJ_TARGET_WASM
#include <QPointer>
#endif
#include <QSet>
#include <QStringList>
#include <QTreeWidget>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QTimer;

namespace PJ {

class HeaderResizePolicy;

// Hierarchical tree of curves with two columns (name, value-at-tracker).
// Drag source emits "curveslist/add_curve" (left drag) or
// "curveslist/new_XY_axis" (right drag of exactly two curves).
class CurveTreeView : public QTreeWidget {
  Q_OBJECT
 public:
  using DragSelectionProvider = std::function<std::vector<QString>()>;

  struct CurvePath {
    QString key;
    QString dataset;
    QString topic;
    QString field;
    bool selectable = true;
    // false = the row never starts a drag and is excluded from any drag payload.
    // Curve leaves: a value-only leaf (string fields today — they can't be
    // plotted) keeps its Value cell but is drag-inert. Object topics: a type no
    // view can display (the caller's policy) — pair with `tooltip` to say why.
    bool draggable = true;
    // Optional name-cell tooltip. Empty = none.
    QString tooltip = {};
    bool is_image_topic = false;
    bool is_3d_object_topic = false;
    // An advertised-but-unsubscribed placeholder (a streaming source's topic with
    // no data yet, no storage id) — rendered "paused/ghost" (dimmed) by
    // CurveTreeItemDelegate, still selectable/draggable so it can be dropped to
    // register demand (see the pj_app-side TopicDemandController).
    bool is_placeholder = false;
    // A topic that HAS data but is not currently referenced by any displayed
    // widget on a per-topic-pause-capable dataset — dimmed the same way as
    // is_placeholder. Computed at construction time from
    // TopicDemandTracker::activeTopics(); kept live afterward via
    // setUnsubscribedKeys() rather than a rebuild, so a subscribe/unsubscribe
    // toggle only repaints, never restructures.
    bool is_unsubscribed = false;
  };

  // Plain-typed, per-dataset load progress retained by the tree view. The
  // state is stored on dataset rows only; topic and scalar-field rows are
  // never decorated.
  struct DatasetProgress {
    // The current controller-owned lifecycle state for the row decoration.
    // kStopping is latched the moment the user's stop is ACCEPTED, not when the
    // producer finishes: a cooperative stop can take anywhere from a few ms to
    // seconds, and without this the row would keep advancing as though the
    // click had been ignored.
    enum class State { kLoading, kStopping, kCompleted, kCancelled, kFailed };

    // Fallback label used only when no matching dataset node exists and the
    // view must create a top-level ghost row.
    QString display_name;
    // Determinate completion in the inclusive 0..1 range. Ignored while
    // indeterminate is true.
    double fraction = 0.0;
    // Whether the loading state has no determinate fraction.
    bool indeterminate = false;
    // Whether a loading row may be stopped at all. A non-cancellable row
    // exposes no affordance.
    bool cancellable = false;
    // Whether stopping can also THROW AWAY what has arrived. Some producers can
    // only stop, so the bin is still drawn — losing a cell mid-load reads as a
    // glitch — but greyed and inert, which says "not available here" rather
    // than silently keeping the data it offered to discard. Ignored when
    // cancellable is false.
    bool discardable = true;
    // Lifecycle state currently presented for the dataset.
    State state = State::kLoading;
    // Controller-driven phase for a failed-row flash; the view only retains
    // and presents the supplied phase.
    bool flash_on = true;
  };

  // Hierarchical: split dataset/topic/field on every '/' (after '.' → '/').
  // ShowTopics: dataset and topic stay as literal nodes (topic shown
  // verbatim, e.g. "/camera/image"); the field still splits on '/' so
  // nested struct fields show up as sub-folders below the topic.
  enum class ViewMode { kHierarchical, kShowTopics };

  explicit CurveTreeView(QWidget* parent = nullptr);

  void setViewMode(ViewMode mode);
  [[nodiscard]] ViewMode viewMode() const {
    return view_mode_;
  }

  [[nodiscard]] static QString catalogItemsMimeType();
  // Mime format set on a right-drag of exactly two curves — the "create XY plot"
  // gesture. Present alongside catalogItemsMimeType() so drop sites can detect it.
  [[nodiscard]] static QString newXyAxisMimeType();
  [[nodiscard]] static QByteArray encodeCatalogKeys(const QStringList& keys);
  [[nodiscard]] static QStringList decodeCatalogKeys(const QMimeData* mime_data);

  void addCurve(const QString& name);
  void addCurves(const std::vector<QString>& names);
  void addCurve(const CurvePath& path);
  void addCatalogItem(const CurvePath& path);
  void addCatalogItems(const std::vector<CurvePath>& paths);
  // Builds the hierarchical tree-path a row is filed under and searched by
  // (dataset/topic/field, '.'→'/' normalized) — the exact string addCatalogItem
  // stores in the row's search role. Exposed so a caller can name a row by its
  // tree location, e.g. to pre-arm requestExpansionWhenPromoted for a
  // placeholder that will later promote to a field-bearing group.
  [[nodiscard]] static QString treePathFromCurvePath(const CurvePath& path);
  void clearCurves();
  // Expanded-group snapshot for rebuild survival: a full rebuild (clearCurves +
  // re-add, e.g. after a catalog item removal) would otherwise collapse the
  // whole tree. Capture before, restore after. Paths are name chains joined
  // with a control character (names may contain '/'); restore only ADDS
  // expansions — vanished paths are skipped and nothing is collapsed, so it
  // composes with an active filter's own auto-expansion.
  [[nodiscard]] QStringList expandedGroupPaths() const;
  void restoreExpandedGroupPaths(const QStringList& paths);
  // Updates the "unsubscribed" (dimmed) flag on every on-screen row whose
  // catalog key is in `unsubscribed_keys`, clearing it on every other row — a
  // full-set replace, mirroring TopicDemandTracker's own declarative active-set
  // semantics. No structural change (no rebuild), so it is cheap to call after
  // every TopicDemandTracker::activeTopicsChanged.
  void setUnsubscribedKeys(const QSet<QString>& unsubscribed_keys);
  // Reads back what setUnsubscribedKeys (or CurvePath::is_unsubscribed at
  // construction) set for `key`; false for an unknown key.
  [[nodiscard]] bool isKeyUnsubscribed(const QString& key) const;
  // The catalog key a row resolves to (curve leaf, object-topic terminal, or
  // placeholder), or empty for pure group/folder rows — lets a context-menu
  // host map the clicked row back to a CatalogModel item.
  [[nodiscard]] static QString catalogKeyOf(const QTreeWidgetItem* item);
  // Every catalog key in `item`'s subtree, including `item`'s own (depth-first;
  // keyless group rows contribute nothing). Lets a context-menu host act on a
  // GROUP row — e.g. a promoted scalar topic, whose keys live on its field
  // leaves, not the topic node itself.
  [[nodiscard]] static QStringList catalogKeysUnder(const QTreeWidgetItem* item);
  // The sweeping band an indeterminate row draws, for `elapsed_ms` since the
  // animation started (NOT since the epoch — a band phased off absolute
  // wall-clock time opens wherever the clock happens to land, so a load can
  // start with the band already near the right edge instead of entering from
  // the left). Exposed as a pure function so the sweep is testable without
  // painting. `period_ms` must be > 0.
  [[nodiscard]] static QRect marqueeBandRect(const QRect& fill_rect, qint64 elapsed_ms, qint64 period_ms);

  // Dev-time geometry overlay for tree ROWS, which a widget-walking inspector
  // (pj_app's DebugUi pesticide overlay) can never reach: rows are model items
  // painted by the view, not child widgets. When on, every row is outlined
  // along with the rects drawRow actually consumes — the progress fill, the
  // percentage caption, and the cancel hit-test box — so a geometry fault is
  // visible rather than inferred.
  //
  // Process-wide and off by default; the host toggles it for every instance at
  // once. Callers must repaint affected viewports themselves.
  static void setGeometryDebugEnabled(bool enabled);
  [[nodiscard]] static bool geometryDebugEnabled();

  // Viewport-space centres of the stop affordances currently offered by a
  // loading row, or invalid points (-1,-1) where none is. `discard` stays
  // invalid for a producer that can stop but not discard, exactly as the
  // hit-test refuses that glyph.
  //
  // For an automated driver (the browser acceptance probe) that clicks the
  // affordances the way a user does: the points are validated THROUGH the
  // hit-test, so a driver can never aim at a rect a click would ignore.
  struct StopButtonPoints {
    QPoint keep{-1, -1};     // ✕ — stop and keep what arrived
    QPoint discard{-1, -1};  // bin — stop and discard it
  };
  [[nodiscard]] StopButtonPoints activeStopButtonPoints() const;

  // Full-set replacement of all per-dataset progress. The hash is retained and
  // re-applied after every tree rebuild, mirroring setForcedTopicPaths(). An
  // empty hash clears every progress mark. Entries with no mapped node become
  // top-level progress-only ghost rows: they use DatasetProgress::display_name,
  // carry no catalog key, and are neither selectable nor draggable.
  //
  // A decorated row is also exempt from the active filter for as long as it
  // carries progress — see applyFilter.
  void setDatasetProgress(const QHash<quint64, DatasetProgress>& by_row_key);
  // Associates a normalized catalog tree path (dataset/topic/field, with topic
  // and field '.' separators normalized to '/') with the opaque row key used by
  // setDatasetProgress(). Call this before adding the corresponding dataset so
  // its first inserted row can replace any ghost immediately.
  //
  // The mapping is one-to-one in BOTH directions, and the last registration
  // wins. A key names one path because a dataset's path changes under a stable
  // key (a sibling's removal relabels "foo (2)" back to "foo"), and a leftover
  // path would let the key decorate — and take the stop click of — another
  // dataset's row. A path names one key because a row carries ONE decoration and
  // ONE click target: when a reload's ingest resolves to the path a finishing
  // one still holds, the newcomer takes the real row and the key it displaces
  // falls back to a ghost, rather than the two sharing a row and leaving the
  // winner to hash order.
  void setDatasetRowKey(const QString& dataset_tree_path, quint64 row_key);
  // Full-set replace of the topics whose streaming is user-forced, each named
  // by its topic tree-path (dataset/topic, '.'→'/' — see treePathFromCurvePath
  // with an empty field). The TOPIC node's name paints in the accent blue.
  // The set is retained and re-applied across rebuilds.
  void setForcedTopicPaths(const QSet<QString>& topic_paths);
  // Test read-back: whether the topic node at `topic_path` carries the forced
  // mark right now.
  [[nodiscard]] bool isTopicPathForced(const QString& topic_path);
  // Arms a one-shot intent: once a placeholder leaf at `tree_path` promotes to a
  // field-bearing topic (its fields materialize as child rows), expand that
  // topic's node and its ancestors so the field breakdown is revealed, then
  // forget the intent. Fires at most once, so a later manual collapse survives
  // subsequent rebuilds; the intent itself survives rebuilds until honored.
  // `tree_path` is the row's normalized search path (dataset/topic/field, '.'→'/'
  // — see treePathFromCurvePath); matching keys on that search role, so it works
  // in both the hierarchical and show-topics views. A promotion to a single
  // unnamed field (no sub-path) reveals nothing to expand and is a no-op.
  void requestExpansionWhenPromoted(const QString& tree_path);
  // Hides every row that does not match `filter` (space-separated tokens, all of
  // which must appear, case-insensitive), ANDed with the kind filter below. A
  // row showing load progress is exempt: it reports work happening right now and
  // carries the affordances that stop it, so filtering it away would take the
  // stop button with it — and a ghost row matches no text at all. The exemption
  // lasts exactly as long as the decoration does.
  void applyFilter(const QString& filter);
  // Restrict which topic KINDS the tree shows, ANDed with the text filter.
  // Classification is per TOPIC and governs the topic's whole subtree: a Scene2D
  // (image-family, kImageTopicRole) or Scene3D (3D-object, k3dObjectTopicRole)
  // topic — and every scalar field nested under it — is hidden together when
  // that kind is off; those fields count as the topic's scene kind, never as
  // Plot. Plot is the "by exclusion" bucket: a topic carrying no scene marker
  // anywhere above the row (a plain numeric topic and its fields). All three
  // default to true (no type restriction).
  void setVisibleCurveKinds(bool show_plot, bool show_scene2d, bool show_scene3d);
  // Message shown as a muted, column-spanning child row under each dataset node
  // whose topics are ALL filtered out (by text and/or setVisibleCurveKinds): the
  // dataset name stays visible and the row explains the blank instead of the
  // whole panel going empty. Empty string (default) disables it; a dataset with
  // no topics at all shows nothing.
  void setEmptyFilterMessage(const QString& message);
  void refreshIcons(const QString& theme);
  std::vector<QString> selectedCurveNames() const;
  // selectedCurveNames() returns only directly-selected leaves; this variant
  // expands selected group nodes to all their leaf descendants. Result is
  // sorted and deduplicated.
  std::vector<QString> selectedCurveNamesRecursive() const;
  // Returns catalog item keys for selected nodes, including object-topic
  // branch nodes. Scalar-only curve selection remains available through
  // selectedCurveNamesRecursive(). COMPLETE: not-draggable object rows are
  // included — deletion and selection-size logic must see the whole selection
  // (an empty result means "nothing selected", which some callers widen to
  // "everything"). Drag payloads use selectedCatalogKeysForDrag() instead.
  std::vector<QString> selectedCatalogKeysRecursive() const;
  // Drag-payload variant of selectedCatalogKeysRecursive(): not-draggable object
  // rows (CurvePath::draggable=false) are excluded; when `skipped_keys` is
  // given, their keys are appended to it so the caller can tell the user what
  // was left out.
  std::vector<QString> selectedCatalogKeysForDrag(QStringList* skipped_keys = nullptr) const;

  // Builds the MIME payload for a drag of the current selection: the
  // "curveslist/add_curve" / "curveslist/new_XY_axis" curve-name format and
  // the catalog-key format, each covering EVERY selected row (not just the row
  // under the cursor). Returns nullptr — and transfers ownership otherwise —
  // when the selection has nothing draggable for `button`. Exposed for tests
  // because the desktop live drag path ends in a blocking QDrag::exec() that
  // cannot be driven from a unit test. WASM reuses this payload with a
  // non-blocking in-app dispatcher (see the wasmDrag* helpers below), which is
  // covered by the browser (Playwright) suite rather than these native tests.
  // `skipped_keys` (optional) collects the not-draggable object rows the payload
  // excluded — see selectedCatalogKeysForDrag.
  [[nodiscard]] QMimeData* createDragMimeData(Qt::MouseButton button, QStringList* skipped_keys = nullptr) const;

  void setValuesColumnHidden(bool hidden);
  bool valuesColumnHidden() const {
    return isColumnHidden(1);
  }

  // Install/refresh the value provider and repaint column 1 ("Value") for the
  // on-screen scalar leaves. Rows whose row rect falls outside the viewport are
  // skipped, so a huge catalog formats only the visible handful. No-op when the
  // value column is hidden. `value_provider` receives each visible leaf's catalog
  // key (see catalogKeyForItem) and returns the preformatted cell text (empty
  // string for rows with no value / non-scalar rows). Non-leaf group rows are
  // never touched. The provider is retained, so the view re-applies it on its own
  // whenever the visible set changes (expand/collapse/scroll/resize) — the caller
  // only re-invokes this when the underlying values change (e.g. tracker moved).
  // Lifetime: re-application is deferred (QTimer::singleShot), so anything the
  // provider captures must stay valid until a new provider is installed, the
  // value column is hidden, or the view is destroyed.
  void refreshVisibleValues(const std::function<QString(const QString& key)>& value_provider);

  void setDragSelectionProvider(DragSelectionProvider provider);

 signals:
  // Emitted when the user stops a loading, cancellable dataset row from the
  // row itself. `keep_partial` distinguishes the two affordances: the ✕ stops
  // and KEEPS what has arrived, the bin stops and DISCARDS it. The row offers
  // both, so the host must not re-ask which was meant.
  void cancelRequested(quint64 row_key, bool keep_partial);
  // Emitted on a double-click of a childless, peek-eligible scalar placeholder
  // leaf: an advertised-but-unsubscribed row that is NOT an image/3D-object
  // terminal. The host (pj_app) responds by starting a bounded preview
  // subscription so one real sample lands and the placeholder promotes to
  // per-field rows. Carries the row's catalog key.
  void placeholderPeekRequested(const QString& catalog_key);
  // Emitted at most once per press gesture when the user pulls past the drag
  // threshold on a not-draggable object-topic row (CurvePath::draggable=false) —
  // the drag never starts, and without this notice the gesture would fail
  // silently. `reason` is the row's tooltip (may be empty; the host supplies a
  // fallback wording).
  void dragAttemptedOnNotDraggableRow(const QString& reason);
  // Emitted when a drag that DID start had to exclude not-draggable object rows
  // from its multi-selection payload, so the host can tell the user why fewer
  // topics arrive than were selected. Fires when the drag gesture ENDS (after
  // the QDrag loop returns; on WASM when the drop is delivered) — a toast
  // raised mid-drag could sit over the drop target and steal its hit test.
  void dragPayloadKeysSkipped(const QStringList& catalog_keys);

 protected:
#ifdef PJ_TARGET_WASM
  bool event(QEvent* event) override;
#endif
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  // Clears the stop-affordance hover when the pointer leaves the viewport;
  // without it the last hovered glyph stays lit after the cursor is gone.
  void leaveEvent(QEvent* event) override;
  // Answers QEvent::ToolTip over the stop-affordance cluster. The glyphs are
  // painted, not child widgets, so there is nothing for Qt to read a tooltip
  // from; this resolves the pointer through the same hit-test the click path
  // uses and shows that affordance's text. Over a glyph the row's own tooltip
  // is deliberately suppressed: the pointer is on the icon, not the name.
  bool viewportEvent(QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  // Drops the cached theme tokens and tinted glyphs on a palette/style change.
  void changeEvent(QEvent* event) override;
  // Paint the progress decoration for a dataset row: track background, progress
  // fill (determinate or animated marquee), and right-edge percentage caption +
  // cancel button. Base QTreeView::drawRow is called after the fill so expander/
  // icon/name/value paint on top. Selected rows repaint the fill at ~45% alpha
  // over the selection highlight to preserve visibility.
  void drawRow(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

 private slots:
  void updateAnimatingRows();

 private:
  friend class CurveTreeViewTestPeer;

  enum class SortMode { kImmediate, kDeferred };

  // Which stop affordance the pointer is over, if any.
  enum class StopButton { kNone, kKeep, kDiscard };

  // Geometry for a dataset-row progress decoration: the background fill rect,
  // the reserved text area for the percentage, and the cancel button hit-test
  // rect. Used by paint, text layout, and the cancel affordance hit-test so
  // they all agree on the rects.
  struct ProgressGeometry {
    QRect fill_rect;            // track and indicator background
    QRect text_rect;            // reserved for percentage text
    QRect keep_button_rect;     // ✕ — stop loading, KEEP what arrived
    QRect discard_button_rect;  // bin — stop loading, DISCARD what arrived
  };

  // One source of truth for progress-row paint geometry: the background fill,
  // reserved text space, and cancel button hit-test. Called by drawRow (paint),
  // text formatting, and mousePressEvent (hit-test) so all three agree.
  [[nodiscard]] ProgressGeometry progressGeometry(const QRect& row_rect) const;

  // Row-decoration tokens, resolved once per theme instead of per row per
  // frame; see progressPalette. `light` guards a theme flip that arrived
  // without the changeEvent that normally invalidates this.
  struct ProgressPalette {
    bool valid = false;
    bool light = false;
    QColor indicator;
    QColor backdrop;
    QColor selection_fill;
    QColor loading_backdrop;
    QColor failed_bar;
    QColor text;
    QColor hover_layer;
    QColor icon_ink;
    QColor icon_ink_disabled;
  };

  // `row_rect` widened across every column to the viewport's full width, which
  // is what the progress decoration spans. THE definition of that widening:
  // paint, hit-test and invalidation share it so their right edges agree.
  [[nodiscard]] QRect fullWidthRowRect(const QRect& row_rect) const;

  // What the stop-affordance cluster offers at a viewport position.
  struct StopButtonHit {
    QTreeWidgetItem* item = nullptr;        // the row, null when no glyph is there
    StopButton button = StopButton::kNone;  // which painted glyph the position is over
    DatasetProgress progress;               // that row's progress, meaningless when button is kNone
    bool actionable = false;                // whether this glyph accepts a click
  };

  // THE hit-test for the cluster: hover, click and tooltip all resolve a
  // position through here, so the rects can never disagree between them.
  //
  // Reports a glyph whenever one is PAINTED (mirroring drawRow, which keeps the
  // cluster through the terminal linger) and carries the row's progress, so a
  // caller can tell a live affordance from one that is merely drawn. Only
  // `actionable` may be acted on: it is false during the linger, and false for
  // the bin of a producer that can stop but not discard.
  [[nodiscard]] StopButtonHit stopButtonAt(const QPoint& viewport_pos) const;

  // The tooltip for the affordance under `viewport_pos`, empty when that
  // position has none to offer. A greyed bin on a LIVE load explains why it
  // does nothing; every glyph on a row whose load already ended stays silent.
  [[nodiscard]] QString stopButtonTooltip(const QPoint& viewport_pos) const;

  // Runs `mutate` as a tree-structure change: ghosts cleared first, then every
  // retained decoration (filter, forced marks, pending expansions, dataset
  // progress) re-applied once. Every public add/clear goes through it, so a
  // decoration cannot survive one overload and be dropped by another.
  void mutateStructure(const std::function<void()>& mutate);

  void addCurve(const QString& name, SortMode sort_mode);
  void addCatalogItem(const CurvePath& path, SortMode sort_mode);
  // The node representing the topic at `topic_path`: the row whose search role
  // equals it (placeholder leaf / object terminal), or the group above its
  // field leaves (promoted scalar topic). Null when nothing matches.
  QTreeWidgetItem* findTopicNode(const QString& topic_path);
  // Re-stamps kForcedRole from forced_topic_paths_ (clear-all then set).
  void applyForcedTopicMarks();
  // The FULL progress pass, in four steps that callers depend on as one
  // contract: STAMP the retained progress onto the dataset rows that resolve,
  // BUILD a ghost for every entry whose tree path has no row (sorting them into
  // place, since a ghost's label is its sort key), RE-APPLY the filter (a row
  // that just gained or lost its decoration also gains or loses the filter
  // exemption — and mutateStructure relies on this as its one post-mutation
  // refilter), then REPAINT every row it touched.
  //
  // Use it whenever the row identities or the tree itself changed;
  // refreshDatasetProgressValues is the per-tick counterpart.
  void rebuildProgressDecorations();
  // Fraction-only counterpart: re-stamps the VALUES of rows whose identities (the
  // key set and each row's display name) are unchanged, through
  // progress_row_cache_ and touching only those rows. No ghost teardown, no
  // re-label and no re-sort, which a ~20 Hz load tick must not pay for. Falls
  // back to the full pass on a cache miss.
  void refreshDatasetProgressValues();
  // Drops a row's selection while it shows live progress; shared by both passes.
  void deselectProgressRow(QTreeWidgetItem* row, DatasetProgress::State state);
  // Starts or stops the marquee/flash timer from whether any row still animates.
  void refreshAnimationTimer();
  // True only for progress states whose paint changes without an incoming
  // controller update (an indeterminate marquee or an active flash phase).
  [[nodiscard]] bool rowNeedsAnimation(QTreeWidgetItem* item) const;
  // Recomputes which stop affordance is under `viewport_pos` and repaints only
  // on a transition.
  //
  // The hover state is TRACKED rather than sampled from QCursor at paint time:
  // sampling repaints nothing when the pointer moves, so the highlight only
  // appeared on the next unrelated repaint (a progress tick), which reads as
  // lag. Repainting on every move instead would repaint the row continuously.
  void refreshStopButtonHover(const QPoint& viewport_pos);
  // The bin glyph at `size`, tinted to `ink` so it matches the ✕ stroke exactly
  // in every state. Rasterized on demand and cached until the size or ink
  // changes.
  [[nodiscard]] const QPixmap& discardIcon(int size, const QColor& ink) const;
  // The row-decoration tokens for the current theme, resolving them on first use
  // and after a theme change.
  [[nodiscard]] const ProgressPalette& progressPalette() const;
  // Draws the geometry overlay over one row: the row rect, each column's cell
  // rect, and — when the row carries progress — the fill, caption and cancel
  // rects. `geom` is null for rows with no progress decoration.
  void paintGeometryDebug(
      QPainter* painter, const QRect& row_rect, const QModelIndex& index, const ProgressGeometry* geom) const;
  // Invalidates `item`'s FULL-WIDTH row rect.
  //
  // Required whenever progress data changes: setData() dirties only the changed
  // column's cell, so Qt would clip drawRow's painter to the name column and
  // leave the right-edge cluster (percentage caption, cancel glyph) showing
  // stale pixels from the previous frame.
  void updateProgressRowRegion(QTreeWidgetItem* item);
  // Removes every managed ghost without changing the retained progress set.
  void removeDatasetGhostItems();
  // Resolves a normalized catalog tree path and returns its top-level dataset
  // ancestor, never the matching topic or field row.
  QTreeWidgetItem* findDatasetNode(const QString& dataset_tree_path);
  QTreeWidgetItem* ensureGroupSegments(const QStringList& segments);
  QTreeWidgetItem* ensureGroup(const QString& path);
  // Walks the tree; for every pending_expand_paths_ entry whose group node now
  // exists, expands that node and its ancestors and drops the entry. No-op when
  // there are no pending intents.
  void expandPendingGroups();
  // Repaint visible value cells now, using the retained provider (no-op if none
  // / column hidden). scheduleValueRefresh() coalesces this onto the next event
  // loop turn — used after expand/collapse/scroll/resize so the freshly-laid-out
  // rows have valid rects before we read them.
  void applyVisibleValues();
  // Recursive worker for applyVisibleValues: writes the value cell for `item` if
  // it's an on-screen scalar leaf, else recurses into its children. A member
  // (not a per-call std::function) so the ≤10 Hz refresh allocates nothing.
  void applyVisibleValuesToSubtree(QTreeWidgetItem* item, int viewport_height);
  void scheduleValueRefresh();
  void sortTree();
  // Re-run the active filter (last_filter_) over the whole tree, hiding or showing
  // each row. Unlike applyFilter() it has no text-equality short-circuit, so it
  // also re-evaluates rows inserted since the filter was last set.
  void refilterTree();
  // Re-assert the active filter after rows are inserted, so a filter typed before
  // the data arrived still applies to it. No-op when no filter is active (freshly
  // inserted rows are visible by default).
  void reapplyFilter();
  // Show/hide the managed empty-filter placeholder under `dataset_node`: a single
  // italic, muted, column-spanning child row, displayed (with the node force-shown
  // and expanded) when `subtree_hidden` and the dataset actually has topics — so a
  // fully-filtered dataset keeps its name and explains the blank. See
  // setEmptyFilterMessage. No-op when the message is empty.
  void updateEmptyMessageChild(QTreeWidgetItem* dataset_node, bool subtree_hidden);
  void setDescendantsExpanded(QTreeWidgetItem* item, bool expanded);
  std::vector<QString> selectedCurveNamesForDrag() const;
  // Shared walk behind selectedCatalogKeysRecursive (complete) and
  // selectedCatalogKeysForDrag (not-draggable rows excluded and reported).
  std::vector<QString> collectSelectedCatalogKeys(bool exclude_not_draggable, QStringList* skipped_keys) const;
  // The one drag-start threshold, measured from drag_start_pos_ — used by both
  // the real drag arming and the not-draggable notice so they can never disagree.
  [[nodiscard]] bool pastDragThreshold(const QPoint& pos) const;
#ifdef PJ_TARGET_WASM
  // QDrag::exec() needs Qt WASM's Asyncify build because it enters a nested
  // event loop. The production browser build deliberately avoids Asyncify, so
  // keep the mouse grab established by the press and dispatch the ordinary Qt
  // DnD events to the widget under the pointer instead. Drop sites therefore
  // share their exact MIME validation and mutation paths with desktop. These
  // helpers are exercised by the browser (Playwright) suite, not native tests.
  void beginWasmDrag(QMimeData* mime_data, QMouseEvent* event, QStringList skipped_keys);
  void updateWasmDrag(const QPoint& global_pos, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers);
  void finishWasmDrag(QMouseEvent* event);
  void cancelWasmDrag();
  void abortWasmDrag(Qt::KeyboardModifiers modifiers);
  void balanceWasmSourcePress(Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
#endif

  QPoint drag_start_pos_;
  Qt::MouseButton drag_button_ = Qt::NoButton;
  std::vector<QString> drag_curve_names_;
  QStringList drag_catalog_keys_;
  bool suppress_next_release_ = false;
  // One-shot dragAttemptedOnNotDraggableRow state, armed (with the row's tooltip as the
  // reason) by a left press on a not-draggable object row and consumed by the
  // first past-threshold move of that gesture. Shares drag_start_pos_ with the
  // real drag arming — the two are mutually exclusive per press.
  std::optional<QString> not_draggable_reason_;
#ifdef PJ_TARGET_WASM
  std::unique_ptr<QMimeData> wasm_drag_mime_;
  QPointer<QWidget> wasm_drag_target_;
  Qt::MouseButton wasm_drag_button_ = Qt::NoButton;
  // Not-draggable keys excluded from the in-flight WASM drag's payload; emitted as
  // dragPayloadKeysSkipped when the drop is delivered (finishWasmDrag) and
  // dropped silently on cancel/abort.
  QStringList wasm_drag_skipped_keys_;
  // Reentrancy guard for the synchronous drop dispatch: sendEvent into the drop
  // target runs the drop site's handler inline (e.g. adding a curve rebuilds
  // this very tree via clearCurves), which can loop back into finishWasmDrag /
  // beginWasmDrag on the same stack. INVARIANT: while a drop is in flight, no
  // new drag starts and no nested drop dispatches — those re-entries early-out.
  bool in_wasm_drop_ = false;
#endif
  QString last_filter_;
  // setVisibleCurveKinds flags; all true = no type restriction (the default).
  bool show_plot_ = true;
  bool show_scene2d_ = true;
  bool show_scene3d_ = true;
  // See setEmptyFilterMessage. Empty string disables the overlay.
  QString empty_filter_message_;
  DragSelectionProvider drag_selection_provider_;
  // Owned by the header; borrowed here to rebalance after the Value column is
  // shown or hidden, which the policy cannot observe on its own.
  HeaderResizePolicy* header_policy_ = nullptr;
  ViewMode view_mode_ = ViewMode::kHierarchical;
  // Retained value-column provider (see refreshVisibleValues). Re-applied on
  // visibility changes so expanding/scrolling fills the newly-revealed rows.
  std::function<QString(const QString&)> value_provider_;
  // Coalesces the deferred re-apply so a burst of expand/scroll events schedules
  // a single refresh on the next event loop turn.
  bool value_refresh_scheduled_ = false;
  // Topics currently marked as force-streamed (see setForcedTopicPaths);
  // retained so rebuilds re-stamp the marks.
  QSet<QString> forced_topic_paths_;
  // Full progress set retained across clearCurves() + re-add rebuilds.
  QHash<quint64, DatasetProgress> dataset_progress_;
  // The catalog tree path each live row key is filed under — the direction every
  // lookup runs, and the direction that makes "one path per key" structural. A
  // mapping is pruned when its key leaves the progress set: it exists to file a
  // LIVE row, so keeping retired keys would grow the map for the session's
  // lifetime and let a recycled path resolve to a dead key.
  QHash<quint64, QString> row_key_to_tree_path_;
  // Managed top-level ghost items, keyed by row key for safe replacement and
  // removal on every progress re-apply.
  QHash<quint64, QTreeWidgetItem*> ghost_items_;
  // Independent of the deferred value-column refresh: this timer exists solely
  // to repaint visible rows whose progress decoration is animated.
  QTimer* animation_timer_ = nullptr;
  // Wall-clock ms when the marquee animation last started. The sweep phase is
  // measured from HERE, not from the absolute clock: phasing off
  // currentMSecsSinceEpoch() alone made every indeterminate bar begin its sweep
  // at whatever position the clock happened to land on, so a load could open
  // with the band already near the right edge instead of entering from the left.
  qint64 animation_epoch_ms_ = 0;
  // Tinted bin glyph, rasterized at the row's button size. Cached because
  // drawRow runs per row per frame; invalidated when either the size or the ink
  // changes. Mutable so the const paint path can fill it.
  // The bin glyph per (size, ink) — see discardIcon. Both inks are live in one
  // frame whenever an enabled bin and a greyed one are on screen together.
  mutable QHash<quint64, QPixmap> discard_icons_;

  mutable ProgressPalette progress_palette_;

  // row_key -> the row currently carrying its decoration (a real dataset row or
  // a ghost), filled by the full apply pass so a fraction-only tick needs no
  // lookup at all. Dropped by every structural change (mutateStructure) and by
  // the full pass itself. PERSISTENT indexes, not item pointers: the inherited,
  // non-virtual clear() / takeTopLevelItem() delete rows without passing through
  // this class, and a raw pointer would survive that as a dangling non-null the
  // next same-membership tick would happily dereference. An invalidated index is
  // a cache miss, which falls back to the full pass.
  QHash<quint64, QPersistentModelIndex> progress_row_cache_;
  // The row and affordance currently under the pointer. Persistent so a catalog
  // rebuild cannot leave it dangling.
  QPersistentModelIndex hovered_stop_row_;
  StopButton hovered_stop_button_ = StopButton::kNone;
  // One-shot auto-expand intents keyed by tree-path (see
  // requestExpansionWhenPromoted). Survives rebuilds; each entry is erased the
  // first time a matching group node is expanded.
  QSet<QString> pending_expand_paths_;
};

// Format a scalar for the curve-list "Value" column, PlotJuggler-3 style: fixed
// `precision` decimals, then trailing zeros (and a bare trailing '.') overwritten
// with spaces with one space appended — so a right-aligned monospace column keeps
// every decimal point in the same place (e.g. 1.2 -> "1.2   ", 5 -> "5     ",
// -0.001 -> "-0.001 "). Non-finite values (NaN/inf) render as "-".
[[nodiscard]] QString formatScalarForColumn(double value, int precision);

}  // namespace PJ

Q_DECLARE_METATYPE(PJ::CurveTreeView::DatasetProgress)
