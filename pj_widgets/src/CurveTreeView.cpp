// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/CurveTreeView.h"

#include <QApplication>
#include <QDataStream>
#include <QDateTime>
#include <QDrag>
#ifdef PJ_TARGET_WASM
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#endif
#include <QFontDatabase>
#include <QHeaderView>
#include <QHelpEvent>
#include <QIcon>
#ifdef PJ_TARGET_WASM
#include <QKeyEvent>
#endif
#include <QItemSelectionModel>
#include <QLoggingCategory>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterStateGuard>
#include <QPalette>
#ifdef PJ_TARGET_WASM
#include <QScopeGuard>
#endif
#include <QScrollBar>
#include <QSet>
#include <QSize>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolTip>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/HeaderDividerHighlight.h"
#include "pj_widgets/HeaderResizePolicy.h"
#include "pj_widgets/SvgUtil.h"
using namespace Qt::StringLiterals;

namespace PJ {

// One full left-to-right sweep of the indeterminate band.
static constexpr qint64 kMarqueePeriodMs = 1200;

// Off by default; pairs with pj.runtime.ingest.cancel to time a stop from
// the click to the producer's terminal.
Q_LOGGING_CATEGORY(lcCurveTreeCancel, "pj.widgets.curvetree.cancel", QtWarningMsg)

namespace {
constexpr int kNameColumn = 0;
constexpr int kValueColumn = 1;
// Narrow by default — the numeric column shouldn't dominate the panel.
constexpr int kValueColumnWidth = 60;
constexpr int kSearchRole = Qt::UserRole + 1;
constexpr int kObjectTopicRole = Qt::UserRole + 2;
constexpr int kCatalogItemRole = Qt::UserRole + 3;
constexpr int kImageTopicRole = Qt::UserRole + 4;
constexpr int k3dObjectTopicRole = Qt::UserRole + 5;
constexpr int kSortKeyRole = Qt::UserRole + 6;
// Marks a leaf that shows a Value cell but must not be dragged or enter the drag
// payload (string fields — not plottable). See CurvePath::draggable.
constexpr int kValueOnlyRole = Qt::UserRole + 7;
// Marks an advertised-but-unsubscribed placeholder row, dimmed by the delegate.
// See CurvePath::is_placeholder.
constexpr int kPlaceholderRole = Qt::UserRole + 8;
// Marks a has-data-but-not-currently-referenced row, dimmed the same way as
// kPlaceholderRole. Set at construction from CurvePath::is_unsubscribed and
// kept live by setUnsubscribedKeys() without a rebuild.
constexpr int kUnsubscribedRole = Qt::UserRole + 9;
// Marks a topic row whose streaming is user-forced (context menu) — the
// delegate paints its name in the accent blue so a successful "Force topic
// streaming" is visible even when nothing else about the row changes. Set by
// setForcedTopicPaths() on the TOPIC node (group or leaf), never on fields.
constexpr int kForcedRole = Qt::UserRole + 10;
// Marks the managed empty-filter placeholder child row (see
// CurveTreeView::updateEmptyMessageChild) so the filter skips it and it is never
// mistaken for a real data row.
constexpr int kEmptyMessageRole = Qt::UserRole + 11;
// Marks an object-topic row the caller marked not-draggable
// (CurvePath::draggable=false) — see applyObjectTopicSelectability. Read by the
// drag-payload collector and the not-draggable-notice arming in mousePressEvent.
constexpr int kNotDraggableRole = Qt::UserRole + 12;
// Stores CurveTreeView::DatasetProgress on dataset rows and managed ghosts.
constexpr int kDatasetProgressRole = Qt::UserRole + 13;
// Marks top-level progress-only rows created when their dataset is absent.
constexpr int kDatasetGhostRole = Qt::UserRole + 14;
// Stores the opaque key needed to route a row-local cancel request. Keeping it
// beside the progress payload also makes loading ghosts unambiguous.
constexpr int kDatasetRowKeyRole = Qt::UserRole + 15;
constexpr int kAnimationIntervalMs = 1000 / 30;

QStringList splitPath(const QString& name) {
  return name.split('/', Qt::SkipEmptyParts);
}

QString normalizedPathSegment(QString path) {
  path.replace('.', '/');
  while (path.startsWith('/')) {
    path.remove(0, 1);
  }
  return path;
}

void setItemName(QTreeWidgetItem* item, const QString& name) {
  item->setText(kNameColumn, name);
  item->setData(kNameColumn, kSortKeyRole, name.toCaseFolded());
}

// The Value column renders monospace + right-aligned so the space-padded,
// fixed-precision numbers (see formatScalarForColumn) keep their decimal points
// in the same place from row to row. Applied to every curve leaf at creation.
// Keeps `base_font`'s size/weight (the tree's font) and only swaps to a
// monospace family — a raw FixedFont renders noticeably larger than the Name
// column.
void styleValueCell(QTreeWidgetItem* item, const QFont& base_font) {
  QFont mono = base_font;
  mono.setFamily(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
  mono.setStyleHint(QFont::Monospace);
  item->setFont(kValueColumn, mono);
  item->setTextAlignment(kValueColumn, Qt::AlignRight | Qt::AlignVCenter);
}

QString sortKeyForItem(const QTreeWidgetItem& item) {
  const QString cached_key = item.data(kNameColumn, kSortKeyRole).toString();
  if (!cached_key.isEmpty() || item.text(kNameColumn).isEmpty()) {
    return cached_key;
  }
  return item.text(kNameColumn).toCaseFolded();
}

class CurveTreeItem : public QTreeWidgetItem {
 public:
  explicit CurveTreeItem(QTreeWidgetItem* parent) : QTreeWidgetItem(parent) {}

  bool operator<(const QTreeWidgetItem& other) const override {
    const QString lhs_key = sortKeyForItem(*this);
    const QString rhs_key = sortKeyForItem(other);
    const int folded_compare = QString::localeAwareCompare(lhs_key, rhs_key);
    if (folded_compare != 0) {
      return folded_compare < 0;
    }
    return QString::localeAwareCompare(text(kNameColumn), other.text(kNameColumn)) < 0;
  }
};

class CurveTreeItemDelegate : public QStyledItemDelegate {
 public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    // Never draw the per-CELL focus decoration (the grey pill around the
    // current item's text). It renders on top of — and independently of — the
    // row selection, so a right-clicked row keeps a floating grey outline after
    // its selection is gone. Selection is the only per-row highlight here;
    // keyboard focus stays on the widget, it just is not drawn per cell.
    opt.state &= ~QStyle::State_HasFocus;

    // Placeholder/unsubscribed flags live on the Name column's item data (see
    // addCatalogItem); fetch via the row's Name-column sibling so every column
    // of a dimmed row (Name AND Value) dims, not just whichever column is
    // being painted.
    const QModelIndex name_index = index.sibling(index.row(), kNameColumn);
    const bool dimmed = name_index.data(kPlaceholderRole).toBool() || name_index.data(kUnsubscribedRole).toBool();
    if (dimmed) {
      painter->save();
      painter->setOpacity(painter->opacity() * 0.5);
    }
    if (index.column() == kNameColumn && name_index.data(kForcedRole).toBool()) {
      // Forced streaming: accent the topic's name so the state is visible —
      // brightness alone can't distinguish "subscribed because displayed" from
      // "subscribed because forced". Name column only: the mark lives on topic
      // rows, which have no Value text, so the extra lookup is skipped on the
      // (paint-hot) Value cells.
      const QColor accent = theme::gradient(theme::Gradient::Brand, theme::Theme::Dark).first;
      opt.palette.setColor(QPalette::Text, accent);
      opt.palette.setColor(QPalette::HighlightedText, accent);
    }

    // A topic row's builtin-family badge (image.svg / cube.svg, set as the
    // item's DecorationRole icon) is painted by the default control as a normal
    // left-side decoration, before the text; its size is the view's iconSize().
    const QWidget* widget = opt.widget;
    const QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    if (dimmed) {
      painter->restore();
    }
  }
};

void normalizeCurveNames(std::vector<QString>& names) {
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
}

QString curveNameForItem(const QTreeWidgetItem* item) {
  if (item == nullptr) {
    return {};
  }
  return item->data(kNameColumn, Qt::UserRole).toString();
}

bool isObjectTopicItem(const QTreeWidgetItem* item) {
  return item != nullptr && !item->data(kNameColumn, kObjectTopicRole).toString().isEmpty();
}

// A value-only leaf (string field): has a Value cell but is excluded from drags.
bool isValueOnlyItem(const QTreeWidgetItem* item) {
  return item != nullptr && item->data(kNameColumn, kValueOnlyRole).toBool();
}

// An object-topic row the caller marked CurvePath::draggable=false: it
// must neither start a drag nor ride inside a drag payload.
bool isNotDraggableItem(const QTreeWidgetItem* item) {
  return item != nullptr && item->data(kNameColumn, kNotDraggableRole).toBool();
}

// Flags for a curve leaf. A draggable leaf is a normal drag source; a value-only
// leaf (string field) stays selectable/highlightable but is never dragged and is
// tagged so the selection collectors leave it out of the drag payload.
void applyLeafSelectability(QTreeWidgetItem* item, bool draggable) {
  if (draggable) {
    item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable);
    return;
  }
  item->setData(kNameColumn, kValueOnlyRole, true);
  item->setFlags((item->flags() | Qt::ItemIsSelectable) & ~Qt::ItemIsDragEnabled);
}

// Flags for an object-topic terminal. A non-draggable one (a type no view can
// display) stays selectable — context menu, force-streaming — but never starts
// a drag and is tagged kNotDraggableRole so the drag-payload collectors and the
// not-draggable-notice arming recognize it. Unlike leaves, it is NOT tagged
// kValueOnlyRole (that role means "string field with a Value cell").
void applyObjectTopicSelectability(QTreeWidgetItem* item, bool draggable) {
  const Qt::ItemFlags base = item->flags() | Qt::ItemIsSelectable;
  item->setFlags(draggable ? (base | Qt::ItemIsDragEnabled) : (base & ~Qt::ItemIsDragEnabled));
  item->setData(kNameColumn, kNotDraggableRole, !draggable);
}

QString catalogKeyForItem(const QTreeWidgetItem* item) {
  if (item == nullptr) {
    return {};
  }
  QString key = item->data(kNameColumn, kCatalogItemRole).toString();
  if (!key.isEmpty()) {
    return key;
  }
  key = item->data(kNameColumn, Qt::UserRole).toString();
  if (!key.isEmpty()) {
    return key;
  }
  return item->data(kNameColumn, kObjectTopicRole).toString();
}

void setTopicIconDecoration(QTreeWidgetItem* item, bool is_image_topic, bool is_3d_object_topic, const QString& theme) {
  if (item == nullptr) {
    return;
  }
  item->setData(kNameColumn, kImageTopicRole, is_image_topic);
  item->setData(kNameColumn, k3dObjectTopicRole, is_3d_object_topic);
  QIcon icon;
  if (is_image_topic) {
    icon = QIcon(loadSvg(u":/resources/svg/image.svg"_s, theme));
  } else if (is_3d_object_topic) {
    icon = QIcon(loadSvg(u":/resources/svg/cube.svg"_s, theme));
  }
  item->setIcon(kNameColumn, icon);
}

void refreshTopicIcons(QTreeWidgetItem* item, const QString& theme) {
  if (item == nullptr) {
    return;
  }
  if (item->data(kNameColumn, kImageTopicRole).toBool()) {
    item->setIcon(kNameColumn, QIcon(loadSvg(u":/resources/svg/image.svg"_s, theme)));
  } else if (item->data(kNameColumn, k3dObjectTopicRole).toBool()) {
    item->setIcon(kNameColumn, QIcon(loadSvg(u":/resources/svg/cube.svg"_s, theme)));
  }
  for (int i = 0; i < item->childCount(); ++i) {
    refreshTopicIcons(item->child(i), theme);
  }
}
}  // namespace

CurveTreeView::CurveTreeView(QWidget* parent) : QTreeWidget(parent) {
  animation_timer_ = new QTimer(this);
  animation_timer_->setObjectName(u"datasetProgressAnimationTimer"_s);
  animation_timer_->setInterval(kAnimationIntervalMs);
  animation_timer_->setTimerType(Qt::PreciseTimer);
  connect(animation_timer_, &QTimer::timeout, this, &CurveTreeView::updateAnimatingRows);

  // Buttonless moves must reach mouseMoveEvent, or the row stop affordances
  // could only track hover while a button was held.
  viewport()->setMouseTracking(true);

  setColumnCount(2);
  setHeaderLabels({tr("Name"), tr("Value")});
  setItemDelegate(new CurveTreeItemDelegate(this));
  // Topic-badge icons (image / 3D-object) render as the default left-side
  // decoration before the name. Size them above the 16-px small-icon default
  // so the type badge reads clearly at the left margin.
  constexpr int kTopicIconExtent = 22;
  setIconSize(QSize(kTopicIconExtent, kTopicIconExtent));
  // Curve names share long common prefixes (e.g. /robot/state_estimator/...),
  // so eliding the tail would hide exactly the part that tells two rows apart.
  // Elide the prefix instead: "…state_estimator/contact_lf". Propagates into the
  // delegate, which elides icon rows via opt.textElideMode.
  setTextElideMode(Qt::ElideLeft);
  // Splitter-style divider: dragging it resizes Name and gives/takes the same
  // amount from Value, so the two sections always fill the viewport. Name is
  // the fill section, so it absorbs the slack when the panel is resized or
  // Value is hidden. Default Value width is narrow — the numeric column
  // shouldn't dominate the panel.
  header_policy_ = HeaderResizePolicy::install(header(), kNameColumn, 20);
  header_policy_->setSectionWidth(kValueColumn, kValueColumnWidth);
  header()->setSectionsClickable(false);
  // Tints the Name|Value boundary while it is grabbable, the way a QSplitter
  // handle reacts.
  HeaderDividerHighlight::install(header());
  setEditTriggers(QAbstractItemView::NoEditTriggers);
  setSelectionMode(QAbstractItemView::ExtendedSelection);
  setSelectionBehavior(QAbstractItemView::SelectRows);
  setFocusPolicy(Qt::ClickFocus);
  setRootIsDecorated(true);
  setUniformRowHeights(true);
  setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
  setExpandsOnDoubleClick(false);
  // Drag handled manually in mouseMoveEvent because left vs right button
  // emit different mime types.
  setDragEnabled(false);
  setDragDropMode(QAbstractItemView::NoDragDrop);

  connect(this, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
    if (item == nullptr || column != kNameColumn) {
      return;
    }
    if (item->childCount() == 0) {
      // Double-clicking a childless leaf is otherwise inert. A peek-eligible
      // scalar placeholder (advertised, and neither an image nor a 3D-object
      // terminal) instead requests a bounded preview so the host can land one
      // real sample and promote the row to per-field children. The expand
      // toggle below is for group nodes only, so return either way.
      const bool peek_eligible = item->data(kNameColumn, kPlaceholderRole).toBool() &&
                                 !item->data(kNameColumn, kImageTopicRole).toBool() &&
                                 !item->data(kNameColumn, k3dObjectTopicRole).toBool();
      const QString catalog_key = catalogKeyForItem(item);
      if (peek_eligible && !catalog_key.isEmpty()) {
        emit placeholderPeekRequested(catalog_key);
      }
      return;
    }
    const bool expanded = !item->isExpanded();
    item->setExpanded(expanded);
    if (item->parent() == nullptr) {
      return;
    }
    setDescendantsExpanded(item, expanded);
  });

  // The value column is filled per-tracker-tick over the *visible* rows only;
  // expanding, collapsing, or scrolling changes which rows are visible, so
  // re-apply the retained provider (deferred) to fill the newly-exposed cells.
  connect(this, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem*) { scheduleValueRefresh(); });
  connect(this, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem*) { scheduleValueRefresh(); });
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { scheduleValueRefresh(); });
}

QString CurveTreeView::catalogItemsMimeType() {
  return u"plotjuggler/catalog-items"_s;
}

QString CurveTreeView::newXyAxisMimeType() {
  return u"curveslist/new_XY_axis"_s;
}

QByteArray CurveTreeView::encodeCatalogKeys(const QStringList& keys) {
  QByteArray encoded;
  QDataStream stream(&encoded, QIODevice::WriteOnly);
  for (const QString& key : keys) {
    if (!key.isEmpty()) {
      stream << key;
    }
  }
  return encoded;
}

QStringList CurveTreeView::decodeCatalogKeys(const QMimeData* mime_data) {
  QStringList keys;
  if (mime_data == nullptr || !mime_data->hasFormat(catalogItemsMimeType())) {
    return keys;
  }

  QByteArray encoded = mime_data->data(catalogItemsMimeType());
  QDataStream stream(&encoded, QIODevice::ReadOnly);
  while (!stream.atEnd()) {
    QString key;
    stream >> key;
    if (!key.isEmpty()) {
      keys.push_back(key);
    }
  }
  keys.removeDuplicates();
  return keys;
}

QTreeWidgetItem* CurveTreeView::ensureGroupSegments(const QStringList& segments) {
  QTreeWidgetItem* parent = invisibleRootItem();
  for (const QString& part : segments) {
    if (part.isEmpty()) {
      continue;
    }
    QTreeWidgetItem* found = nullptr;
    for (int i = 0; i < parent->childCount(); ++i) {
      auto* child = parent->child(i);
      if (child->text(kNameColumn) == part) {
        found = child;
        break;
      }
    }
    if (!found) {
      found = new CurveTreeItem(parent);
      setItemName(found, part);
      // Top-level groups are the datasets: keep them selectable so they can be
      // multi-selected for the dataset context menu (merge / remove). Intermediate
      // topic-path folders stay non-selectable. Neither is a drag source.
      const bool is_dataset = (parent == invisibleRootItem());
      Qt::ItemFlags flags = found->flags() & ~Qt::ItemIsDragEnabled;
      if (!is_dataset) {
        flags &= ~Qt::ItemIsSelectable;
      }
      found->setFlags(flags);
    }
    parent = found;
  }
  return parent;
}

QTreeWidgetItem* CurveTreeView::ensureGroup(const QString& path) {
  return ensureGroupSegments(splitPath(path));
}

void CurveTreeView::mutateStructure(const std::function<void()>& mutate) {
  // Ghosts are rebuilt from the retained progress set at the end; dropping them
  // first stops the mutation from matching a stale ghost as a real row. The row
  // cache goes with them: a structural change is exactly what can move a row out
  // from under its key.
  removeDatasetGhostItems();
  progress_row_cache_.clear();
  mutate();
  // Every retained decoration, re-applied once and in ONE place. Which of them
  // survive a structural change must not depend on which overload the caller
  // reached for — forced marks used to come back only via addCatalogItems. The
  // filter is re-applied by rebuildProgressDecorations' tail (see its contract).
  expandPendingGroups();
  applyForcedTopicMarks();
  rebuildProgressDecorations();
}

void CurveTreeView::addCurve(const QString& name) {
  mutateStructure([&]() { addCurve(name, SortMode::kImmediate); });
}

void CurveTreeView::addCurves(const std::vector<QString>& names) {
  if (names.empty()) {
    return;
  }
  mutateStructure([&]() {
    const bool updates_were_enabled = updatesEnabled();
    setUpdatesEnabled(false);
    for (const QString& name : names) {
      addCurve(name, SortMode::kDeferred);
    }
    sortTree();
    setUpdatesEnabled(updates_were_enabled);
  });
}

void CurveTreeView::addCurve(const QString& name, SortMode sort_mode) {
  const int last_sep = name.lastIndexOf('/');
  QTreeWidgetItem* parent = invisibleRootItem();
  QString leaf_name = name;
  if (last_sep >= 0) {
    parent = ensureGroup(name.left(last_sep));
    leaf_name = name.mid(last_sep + 1);
  }
  auto* item = new CurveTreeItem(parent);
  setItemName(item, leaf_name);
  styleValueCell(item, font());
  item->setData(kNameColumn, Qt::UserRole, name);
  item->setData(kNameColumn, kSearchRole, name);
  item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable);
  if (sort_mode == SortMode::kImmediate) {
    sortTree();
  }
}

QString CurveTreeView::treePathFromCurvePath(const CurvePath& path) {
  QString tree_path = path.dataset;
  const QString topic = normalizedPathSegment(path.topic);
  const QString field = normalizedPathSegment(path.field);
  if (!topic.isEmpty()) {
    tree_path += u"/"_s + topic;
  }
  if (!field.isEmpty()) {
    tree_path += u"/"_s + field;
  }
  return tree_path;
}

void CurveTreeView::addCurve(const CurvePath& path) {
  mutateStructure([&]() {
    addCatalogItem(
        CurvePath{
            .key = path.key,
            .dataset = path.dataset,
            .topic = path.topic,
            .field = path.field,
            .selectable = true,
            .is_image_topic = false,
            .is_3d_object_topic = false,
        },
        SortMode::kImmediate);
  });
}

void CurveTreeView::addCatalogItem(const CurvePath& path) {
  mutateStructure([&]() { addCatalogItem(path, SortMode::kImmediate); });
}

void CurveTreeView::addCatalogItems(const std::vector<CurvePath>& paths) {
  if (paths.empty()) {
    return;
  }
  mutateStructure([&]() {
    const bool updates_were_enabled = updatesEnabled();
    setUpdatesEnabled(false);
    for (const CurvePath& path : paths) {
      addCatalogItem(path, SortMode::kDeferred);
    }
    sortTree();
    setUpdatesEnabled(updates_were_enabled);
  });
}

void CurveTreeView::addCatalogItem(const CurvePath& path, SortMode sort_mode) {
  const QString tree_path = treePathFromCurvePath(path);
  QTreeWidgetItem* item = nullptr;
  if (view_mode_ == ViewMode::kShowTopics) {
    // Dataset and topic are atomic (topic shown verbatim). The field still
    // splits on '/' after '.' → '/' so nested struct fields fan out as
    // sub-folders under the topic node.
    QStringList segments;
    if (!path.dataset.isEmpty()) {
      segments << path.dataset;
    }
    if (!path.topic.isEmpty()) {
      segments << path.topic;
    }
    if (path.selectable) {
      segments += splitPath(normalizedPathSegment(path.field));
      QString leaf_name = segments.isEmpty() ? tree_path : segments.takeLast();
      QTreeWidgetItem* parent = ensureGroupSegments(segments);
      item = new CurveTreeItem(parent);
      setItemName(item, leaf_name);
      styleValueCell(item, font());
      item->setData(kNameColumn, Qt::UserRole, path.key);
      item->setData(kNameColumn, kCatalogItemRole, path.key);
      applyLeafSelectability(item, path.draggable);
    } else {
      // Object topic: terminal at dataset ▸ topic.
      item = ensureGroupSegments(segments);
      item->setData(kNameColumn, kObjectTopicRole, path.key);
      item->setData(kNameColumn, kCatalogItemRole, path.key);
      applyObjectTopicSelectability(item, path.draggable);
      setTopicIconDecoration(item, path.is_image_topic, path.is_3d_object_topic, currentTheme());
    }
  } else if (path.selectable) {
    const int last_sep = tree_path.lastIndexOf('/');
    QTreeWidgetItem* parent = invisibleRootItem();
    QString leaf_name = tree_path;
    if (last_sep >= 0) {
      parent = ensureGroup(tree_path.left(last_sep));
      leaf_name = tree_path.mid(last_sep + 1);
    }
    item = new CurveTreeItem(parent);
    setItemName(item, leaf_name);
    styleValueCell(item, font());
    item->setData(kNameColumn, Qt::UserRole, path.key);
    item->setData(kNameColumn, kCatalogItemRole, path.key);
    applyLeafSelectability(item, path.draggable);
  } else {
    item = ensureGroup(tree_path);
    item->setData(kNameColumn, kObjectTopicRole, path.key);
    item->setData(kNameColumn, kCatalogItemRole, path.key);
    applyObjectTopicSelectability(item, path.draggable);
    setTopicIconDecoration(item, path.is_image_topic, path.is_3d_object_topic, currentTheme());
  }
  // Guarded: an unconditional write would add a setData/dataChanged per row on
  // every rebuild; the second clause clears a stale tooltip on a reused row.
  if (!path.tooltip.isEmpty() || !item->toolTip(kNameColumn).isEmpty()) {
    item->setToolTip(kNameColumn, path.tooltip);
  }
  item->setData(kNameColumn, kSearchRole, tree_path);
  item->setData(kNameColumn, kPlaceholderRole, path.is_placeholder);
  item->setData(kNameColumn, kUnsubscribedRole, path.is_unsubscribed);
  if (sort_mode == SortMode::kImmediate) {
    sortTree();
  }
}

void CurveTreeView::setViewMode(ViewMode mode) {
  if (view_mode_ == mode) {
    return;
  }
  view_mode_ = mode;
  // Force the next applyFilter() call to actually re-run, otherwise the
  // text-equality short-circuit at the top of applyFilter() would skip the
  // re-filter that the post-rebuild caller expects.
  last_filter_.clear();
}

void CurveTreeView::clearCurves() {
  mutateStructure([this]() { clear(); });
}

namespace {

// Separator for joined name chains: names may contain '/', so use a control
// character that never appears in topic/dataset names.
constexpr QChar kPathJoin = QChar(0x1F);

void collectExpandedPaths(const QTreeWidgetItem* item, const QString& prefix, QStringList& out) {
  for (int i = 0; i < item->childCount(); ++i) {
    const QTreeWidgetItem* child = item->child(i);
    if (child->childCount() == 0) {
      continue;
    }
    const QString path = prefix.isEmpty() ? child->text(0) : prefix + kPathJoin + child->text(0);
    if (child->isExpanded()) {
      out.push_back(path);
    }
    collectExpandedPaths(child, path, out);
  }
}

void applyExpandedPaths(QTreeWidgetItem* item, const QString& prefix, const QSet<QString>& paths) {
  for (int i = 0; i < item->childCount(); ++i) {
    QTreeWidgetItem* child = item->child(i);
    if (child->childCount() == 0) {
      continue;
    }
    const QString path = prefix.isEmpty() ? child->text(0) : prefix + kPathJoin + child->text(0);
    if (paths.contains(path)) {
      child->setExpanded(true);
    }
    applyExpandedPaths(child, path, paths);
  }
}

// Every row carrying a catalog key (leaf or object-topic terminal — see
// addCatalogItem, both set kCatalogItemRole) gets its unsubscribed flag set
// from membership in `unsubscribed_keys`; group/folder rows have no key and
// are left untouched.
void applyUnsubscribedFlag(QTreeWidgetItem* item, const QSet<QString>& unsubscribed_keys) {
  for (int i = 0; i < item->childCount(); ++i) {
    QTreeWidgetItem* child = item->child(i);
    const QString key = child->data(kNameColumn, kCatalogItemRole).toString();
    if (!key.isEmpty()) {
      child->setData(kNameColumn, kUnsubscribedRole, unsubscribed_keys.contains(key));
    }
    applyUnsubscribedFlag(child, unsubscribed_keys);
  }
}

const QTreeWidgetItem* findItemByCatalogKey(const QTreeWidgetItem* item, const QString& key) {
  for (int i = 0; i < item->childCount(); ++i) {
    const QTreeWidgetItem* child = item->child(i);
    if (child->data(kNameColumn, kCatalogItemRole).toString() == key) {
      return child;
    }
    if (const QTreeWidgetItem* found = findItemByCatalogKey(child, key); found != nullptr) {
      return found;
    }
  }
  return nullptr;
}

}  // namespace

QStringList CurveTreeView::expandedGroupPaths() const {
  QStringList out;
  // invisibleRootItem() is non-const in QTreeWidget; the walk only reads.
  collectExpandedPaths(const_cast<CurveTreeView*>(this)->invisibleRootItem(), QString(), out);
  return out;
}

void CurveTreeView::restoreExpandedGroupPaths(const QStringList& paths) {
  if (paths.isEmpty()) {
    return;
  }
  const QSet<QString> set(paths.begin(), paths.end());
  applyExpandedPaths(invisibleRootItem(), QString(), set);
}

void CurveTreeView::setUnsubscribedKeys(const QSet<QString>& unsubscribed_keys) {
  applyUnsubscribedFlag(invisibleRootItem(), unsubscribed_keys);
}

QString CurveTreeView::catalogKeyOf(const QTreeWidgetItem* item) {
  return catalogKeyForItem(item);
}

QStringList CurveTreeView::catalogKeysUnder(const QTreeWidgetItem* item) {
  QStringList keys;
  if (item == nullptr) {
    return keys;
  }
  const QString own = catalogKeyForItem(item);
  if (!own.isEmpty()) {
    keys.push_back(own);
  }
  for (int i = 0; i < item->childCount(); ++i) {
    keys += catalogKeysUnder(item->child(i));
  }
  return keys;
}

bool CurveTreeView::isKeyUnsubscribed(const QString& key) const {
  // invisibleRootItem() is non-const in QTreeWidget; the walk only reads.
  const QTreeWidgetItem* item = findItemByCatalogKey(const_cast<CurveTreeView*>(this)->invisibleRootItem(), key);
  return item != nullptr && item->data(kNameColumn, kUnsubscribedRole).toBool();
}

void CurveTreeView::requestExpansionWhenPromoted(const QString& tree_path) {
  if (!tree_path.isEmpty()) {
    pending_expand_paths_.insert(tree_path);
  }
}

// The topic node that a field leaf hangs under, `field_depth` tree levels above
// it, or nullptr if the walk runs off the top.
QTreeWidgetItem* topicNodeAbove(QTreeWidgetItem* leaf, int field_depth) {
  QTreeWidgetItem* node = leaf;
  for (int up = 0; up < field_depth && node != nullptr; ++up) {
    node = node->parent();
  }
  return node;
}

QTreeWidgetItem* CurveTreeView::findTopicNode(const QString& topic_path) {
  // A topic's node is either a row that IS the topic (a placeholder leaf or an
  // object-topic terminal — its search role equals the path exactly), or — for
  // a promoted scalar topic — the keyless GROUP above its field leaves: locate
  // any field leaf whose search role lives under the path, then step up the
  // field sub-path's segment count. Keying on the always-normalized search
  // role — not node text — works in both view modes.
  // INVARIANT the step-up relies on: a field leaf nests exactly one tree level
  // per '/'-segment of its field sub-path (addCatalogItem splits fields on '/'
  // in both view modes). If the tree ever groups fields differently, store an
  // explicit topic-node link on the leaves instead of counting segments.
  // KNOWN AMBIGUITY: the normalized search path cannot distinguish topic "/a"
  // with field "b/c" from topic "/a/b" with field "c" — colliding names across
  // topics can badge/expand the wrong node. Distinguishing them needs separate
  // topic/field roles on the rows; not worth it until a real source hits it.
  const QString prefix = topic_path + QLatin1Char('/');
  QTreeWidgetItem* topic_node = nullptr;
  std::function<bool(QTreeWidgetItem*)> find = [&](QTreeWidgetItem* item) {
    for (int i = 0; i < item->childCount(); ++i) {
      QTreeWidgetItem* child = item->child(i);
      const QString search = child->data(kNameColumn, kSearchRole).toString();
      if (search == topic_path) {
        topic_node = child;
        return true;
      }
      if (child->childCount() != 0) {
        if (find(child)) {
          return true;
        }
        continue;
      }
      if (search.startsWith(prefix)) {
        const int field_depth = static_cast<int>(search.mid(prefix.size()).count(QLatin1Char('/'))) + 1;
        topic_node = topicNodeAbove(child, field_depth);
        return topic_node != nullptr;
      }
    }
    return false;
  };
  find(invisibleRootItem());
  return topic_node;
}

void CurveTreeView::expandPendingGroups() {
  if (pending_expand_paths_.isEmpty()) {
    return;
  }
  // A pending intent is honored once the placeholder promotes to a topic
  // bearing field children — a childless node (the placeholder itself) reveals
  // nothing and keeps the intent armed.
  for (const QString& topic_path : pending_expand_paths_.values()) {
    QTreeWidgetItem* topic_node = findTopicNode(topic_path);
    if (topic_node == nullptr || topic_node->childCount() == 0) {
      continue;  // not promoted yet (or single unnamed field — nothing to reveal)
    }
    for (QTreeWidgetItem* node = topic_node; node != nullptr; node = node->parent()) {
      node->setExpanded(true);
    }
    pending_expand_paths_.remove(topic_path);
  }
}

void CurveTreeView::setForcedTopicPaths(const QSet<QString>& topic_paths) {
  forced_topic_paths_ = topic_paths;
  applyForcedTopicMarks();
}

namespace {
// Same rows AND same row identities: which rows carry progress decides whether a
// ghost has to be built or retired, and the display NAME decides what a ghost is
// called and therefore where it sorts. Anything else in the payload (fraction,
// state, flash) is a value the fast path can stamp in place.
bool sameRowIdentities(
    const QHash<quint64, CurveTreeView::DatasetProgress>& lhs,
    const QHash<quint64, CurveTreeView::DatasetProgress>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (auto it = lhs.cbegin(); it != lhs.cend(); ++it) {
    const auto other = rhs.constFind(it.key());
    if (other == rhs.cend() || other->display_name != it.value().display_name) {
      return false;
    }
  }
  return true;
}
}  // namespace

void CurveTreeView::setDatasetProgress(const QHash<quint64, DatasetProgress>& by_row_key) {
  for (auto it = by_row_key.cbegin(); it != by_row_key.cend(); ++it) {
    qCInfo(lcCurveTreeCancel).nospace() << "[view] SET row_key=" << it.key()
                                        << " state=" << static_cast<int>(it.value().state)
                                        << " fraction=" << it.value().fraction << " indet=" << it.value().indeterminate;
  }
  if (by_row_key.isEmpty()) {
    qCInfo(lcCurveTreeCancel) << "[view] SET (empty)";
  }

  // A fraction-only tick (the common case: ~20 Hz during a load) must not tear
  // the tree down and rebuild it. Only a change in the row IDENTITIES — which
  // rows carry progress, and what each is called — can add, retire, rename or
  // re-sort a ghost; that goes through the full pass, which already does all
  // four. A name resolution happens once per ingest, so paying a full pass for
  // it costs nothing per tick.
  const bool membership_changed = !sameRowIdentities(dataset_progress_, by_row_key);
  if (membership_changed) {
    // Retire the mappings whose key just left. A mapping exists to file a LIVE
    // row under its tree path, so it outlives its purpose the moment its key
    // does — and a surviving one both grows the map for the session's lifetime
    // and lets a recycled path resolve to a dead key. Only a key that WAS
    // published and is now gone prunes: a mapping legitimately arrives before
    // its key is ever published (the controller resolves the dataset's path
    // first, then emits the progress set).
    row_key_to_tree_path_.removeIf([this, &by_row_key](const auto& mapping) {
      return dataset_progress_.contains(mapping.key()) && !by_row_key.contains(mapping.key());
    });
  }
  dataset_progress_ = by_row_key;
  if (membership_changed) {
    rebuildProgressDecorations();
  } else {
    refreshDatasetProgressValues();
  }
}

void CurveTreeView::setDatasetRowKey(const QString& dataset_tree_path, quint64 row_key) {
  qCInfo(lcCurveTreeCancel).nospace() << "[view] MAP path=\"" << dataset_tree_path << "\" -> row_key=" << row_key;
  // The last registration owns the path: a key that was filed under it loses the
  // real row and falls back to a ghost. This is the reload window — a new
  // ingest's key resolves to the dataset a finishing one still decorates — and a
  // row cannot carry two decorations, nor route its stop click to two ingests.
  row_key_to_tree_path_.removeIf(
      [&dataset_tree_path](const auto& mapping) { return mapping.value() == dataset_tree_path; });
  row_key_to_tree_path_.insert(row_key, dataset_tree_path);
  rebuildProgressDecorations();
}

QTreeWidgetItem* CurveTreeView::findDatasetNode(const QString& dataset_tree_path) {
  if (dataset_tree_path.isEmpty()) {
    return nullptr;
  }

  const auto subtree_contains_path = [&dataset_tree_path](QTreeWidgetItem* root) {
    std::function<bool(QTreeWidgetItem*)> contains = [&](QTreeWidgetItem* item) {
      if (item->data(kNameColumn, kSearchRole).toString() == dataset_tree_path) {
        return true;
      }
      for (int i = 0; i < item->childCount(); ++i) {
        if (contains(item->child(i))) {
          return true;
        }
      }
      return false;
    };
    return contains(root);
  };

  for (int i = 0; i < topLevelItemCount(); ++i) {
    QTreeWidgetItem* dataset_node = topLevelItem(i);
    if (dataset_node->data(kNameColumn, kDatasetGhostRole).toBool()) {
      continue;
    }
    // Group-only dataset rows do not carry kSearchRole, so a dataset-only path
    // also matches their displayed name. Full catalog paths resolve through a
    // descendant and still return this top-level ancestor.
    if (dataset_node->text(kNameColumn) == dataset_tree_path || subtree_contains_path(dataset_node)) {
      return dataset_node;
    }
  }
  return nullptr;
}

void CurveTreeView::removeDatasetGhostItems() {
  // Scan instead of dereferencing the registry: inherited QTreeWidget::clear()
  // may have deleted its raw pointers without going through clearCurves().
  for (int i = topLevelItemCount() - 1; i >= 0; --i) {
    if (topLevelItem(i)->data(kNameColumn, kDatasetGhostRole).toBool()) {
      delete takeTopLevelItem(i);
    }
  }
  ghost_items_.clear();
}

void CurveTreeView::deselectProgressRow(QTreeWidgetItem* row, DatasetProgress::State state) {
  // A LOADING row must not stay selected: the selection highlight spans the
  // name column and drowns the translucent bar wash, so the highlight's own
  // right edge (~2/3 of the row) reads as a stuck progress bar — during the
  // load AND after it, since the selection outlives the decoration. The
  // reload gesture itself is what selected the row; drop it.
  if ((state != DatasetProgress::State::kLoading && state != DatasetProgress::State::kStopping) ||
      selectionModel() == nullptr) {
    return;
  }
  const QModelIndex node_index = indexFromItem(row);
  if (selectionModel()->isSelected(node_index)) {
    selectionModel()->select(node_index, QItemSelectionModel::Deselect | QItemSelectionModel::Rows);
  }
  if (selectionModel()->currentIndex() == node_index) {
    selectionModel()->clearCurrentIndex();
  }
}

void CurveTreeView::refreshAnimationTimer() {
  bool has_animating_row = false;
  for (int i = 0; i < topLevelItemCount(); ++i) {
    if (rowNeedsAnimation(topLevelItem(i))) {
      has_animating_row = true;
      break;
    }
  }
  if (has_animating_row) {
    if (!animation_timer_->isActive()) {
      animation_epoch_ms_ = QDateTime::currentMSecsSinceEpoch();
      animation_timer_->start();
    }
  } else {
    animation_timer_->stop();
  }
}

void CurveTreeView::refreshDatasetProgressValues() {
  // Fast path for a fraction-only tick: the rows and their names are unchanged
  // and so is the tree, so every key still resolves through the cache filled by
  // the last full apply, and no row can have moved. A miss — including an index
  // invalidated by a removal this class never saw — means the row moved or died;
  // fall back rather than guess.
  for (auto it = dataset_progress_.cbegin(); it != dataset_progress_.cend(); ++it) {
    const QPersistentModelIndex cached_index = progress_row_cache_.value(it.key());
    QTreeWidgetItem* row = cached_index.isValid() ? itemFromIndex(cached_index) : nullptr;
    if (row == nullptr) {
      rebuildProgressDecorations();
      return;
    }
    row->setData(kNameColumn, kDatasetProgressRole, QVariant::fromValue(it.value()));
    deselectProgressRow(row, it.value().state);
    updateProgressRowRegion(row);
  }
  refreshAnimationTimer();
}

void CurveTreeView::rebuildProgressDecorations() {
  removeDatasetGhostItems();
  progress_row_cache_.clear();

  // Every row whose decoration changed this pass, repainted full-width at the
  // end — see updateProgressRowRegion for why the implicit invalidation is not
  // enough.
  std::vector<QTreeWidgetItem*> touched_rows;

  // Full-set replace: clear stale data from every surviving real row before
  // applying the current hash. This also makes an empty hash a complete clear.
  // TOP-LEVEL ONLY: progress lives on dataset rows, and findDatasetNode never
  // returns anything deeper (pinned by DatasetProgressNeverMarksScalarLeaves),
  // so recursing the whole tree only paid to visit every curve leaf.
  for (int i = 0; i < topLevelItemCount(); ++i) {
    QTreeWidgetItem* row = topLevelItem(i);
    const bool had_progress = row->data(kNameColumn, kDatasetProgressRole).isValid();
    if (had_progress) {
      row->setData(kNameColumn, kDatasetProgressRole, QVariant());
      touched_rows.push_back(row);  // its decoration must be erased, not left behind
    }
    if (row->data(kNameColumn, kDatasetRowKeyRole).isValid()) {
      row->setData(kNameColumn, kDatasetRowKeyRole, QVariant());
    }
  }

  bool created_ghost = false;
  for (auto progress_it = dataset_progress_.cbegin(); progress_it != dataset_progress_.cend(); ++progress_it) {
    const quint64 row_key = progress_it.key();
    QTreeWidgetItem* dataset_node = nullptr;
    if (const auto path_it = row_key_to_tree_path_.constFind(row_key); path_it != row_key_to_tree_path_.cend()) {
      dataset_node = findDatasetNode(path_it.value());
    }

    qCInfo(lcCurveTreeCancel).nospace() << "[view] APPLY row_key=" << row_key
                                        << (dataset_node != nullptr ? " -> REAL node" : " -> NO node (ghost)")
                                        << " item=" << static_cast<const void*>(dataset_node)
                                        << " top0=" << static_cast<const void*>(topLevelItem(0))
                                        << " visRect=" << (dataset_node ? visualItemRect(dataset_node).y() : -1);
    if (dataset_node != nullptr) {
      dataset_node->setData(kNameColumn, kDatasetProgressRole, QVariant::fromValue(progress_it.value()));
      dataset_node->setData(kNameColumn, kDatasetRowKeyRole, QVariant::fromValue(row_key));
      deselectProgressRow(dataset_node, progress_it.value().state);
      progress_row_cache_.insert(row_key, QPersistentModelIndex(indexFromItem(dataset_node, kNameColumn)));
      touched_rows.push_back(dataset_node);
      continue;
    }

    auto* ghost = new CurveTreeItem(invisibleRootItem());
    setItemName(ghost, progress_it.value().display_name);
    ghost->setFlags(Qt::ItemIsEnabled);
    ghost->setData(kNameColumn, kDatasetProgressRole, QVariant::fromValue(progress_it.value()));
    ghost->setData(kNameColumn, kDatasetRowKeyRole, QVariant::fromValue(row_key));
    ghost->setData(kNameColumn, kDatasetGhostRole, true);
    ghost_items_.insert(row_key, ghost);
    progress_row_cache_.insert(row_key, QPersistentModelIndex(indexFromItem(ghost, kNameColumn)));
    touched_rows.push_back(ghost);
    created_ghost = true;
  }

  if (created_ghost) {
    sortTree();
  }

  // Last, because the filter exempts rows that carry progress and the ghosts
  // above only exist now: a row that just gained (or lost) its decoration has to
  // be re-tested before the rects below are read. This is also the ONE filter
  // re-application after a structural change — mutateStructure ends here.
  reapplyFilter();

  // After any sort or hide, so the row rects resolved here are the final ones.
  for (QTreeWidgetItem* row : touched_rows) {
    updateProgressRowRegion(row);
  }

  refreshAnimationTimer();
}

bool CurveTreeView::rowNeedsAnimation(QTreeWidgetItem* item) const {
  if (item == nullptr) {
    return false;
  }
  const QVariant progress_data = item->data(kNameColumn, kDatasetProgressRole);
  if (!progress_data.isValid()) {
    return false;
  }
  const DatasetProgress progress = progress_data.value<DatasetProgress>();
  const bool indeterminate_loading = progress.state == DatasetProgress::State::kLoading && progress.indeterminate;
  const bool flashing = progress.state == DatasetProgress::State::kFailed && progress.flash_on;
  return indeterminate_loading || flashing;
}

namespace {
// Process-wide dev toggle; see CurveTreeView::setGeometryDebugEnabled.
bool g_geometry_debug_enabled = false;

// Outline a rect with a labelled dashed border. Labels are drawn INSIDE the
// rect's top-left so they cannot be confused with a neighbouring rect's.
void outlineRect(QPainter* painter, const QRect& rect, const QColor& color, const QString& label) {
  if (rect.isEmpty()) {
    return;
  }
  QPen pen(color);
  pen.setStyle(Qt::DashLine);
  pen.setWidth(1);
  painter->setPen(pen);
  painter->setBrush(Qt::NoBrush);
  // adjusted(): a QRect's right/bottom edge is inclusive, so drawing it raw
  // paints one pixel outside the region it describes.
  painter->drawRect(rect.adjusted(0, 0, -1, -1));
  if (!label.isEmpty()) {
    QFont font = painter->font();
    font.setPointSize(7);
    painter->setFont(font);
    painter->drawText(rect.adjusted(2, 1, -1, -1), Qt::AlignLeft | Qt::AlignTop, label);
  }
}
}  // namespace

void CurveTreeView::setGeometryDebugEnabled(bool enabled) {
  g_geometry_debug_enabled = enabled;
}

bool CurveTreeView::geometryDebugEnabled() {
  return g_geometry_debug_enabled;
}

void CurveTreeView::paintGeometryDebug(
    QPainter* painter, const QRect& row_rect, const QModelIndex& index, const ProgressGeometry* geom) const {
  const QPainterStateGuard guard(painter);
  painter->setRenderHint(QPainter::Antialiasing, false);

  outlineRect(painter, row_rect, QColor(0, 200, 255), QStringLiteral("row"));

  // Per-column cell rects: where the VIEW thinks each column lives, which is
  // what the row-spanning progress decoration has to coexist with.
  if (index.isValid() && index.model() != nullptr) {
    for (int column = 0; column < index.model()->columnCount(); ++column) {
      const QRect cell = visualRect(index.sibling(index.row(), column));
      outlineRect(painter, cell, QColor(120, 120, 120), QStringLiteral("c%1").arg(column));
    }
  }

  if (geom != nullptr) {
    outlineRect(painter, geom->fill_rect, QColor(0, 220, 0), QStringLiteral("fill"));
    outlineRect(painter, geom->text_rect, QColor(255, 180, 0), QStringLiteral("text"));
    outlineRect(painter, geom->discard_button_rect, QColor(255, 0, 255), QString());
    outlineRect(painter, geom->keep_button_rect, QColor(255, 0, 0), QString());
  }
}

const CurveTreeView::ProgressPalette& CurveTreeView::progressPalette() const {
  const bool is_light = palette().window().color().lightness() >= 128;
  if (progress_palette_.valid && progress_palette_.light == is_light) {
    return progress_palette_;
  }
  // Every token the row decoration paints with, resolved in one pass. Each
  // theme lookup builds a QString key, hashes it and may reparse CSS, and
  // drawRow wants eight of them PER ROW PER FRAME once a marquee or flash runs
  // the animation timer; the theme itself changes about never.
  const auto fw_theme = theme::themeFor(is_light);
  progress_palette_.light = is_light;
  progress_palette_.indicator = theme::interaction(theme::Variant::Accent, theme::State::CheckedPressed, fw_theme);
  progress_palette_.backdrop = theme::surface(theme::Surface::DataBackdrop, fw_theme);
  progress_palette_.selection_fill = theme::selectionFill(theme::Selection::Item, fw_theme);
  progress_palette_.loading_backdrop = theme::interaction(theme::Variant::Accent, theme::State::Nominal, fw_theme);
  progress_palette_.failed_bar = theme::interaction(theme::Variant::Highlight, theme::State::Nominal, fw_theme);
  progress_palette_.text = theme::onProgress(fw_theme);
  progress_palette_.hover_layer = theme::overlay(theme::Overlay::Hover, fw_theme);
  progress_palette_.icon_ink = theme::iconInk(fw_theme);
  progress_palette_.icon_ink_disabled = theme::iconInkDisabled(fw_theme);
  progress_palette_.valid = true;
  return progress_palette_;
}

void CurveTreeView::changeEvent(QEvent* event) {
  // A palette/style/theme switch is the only thing that moves the resolved
  // tokens, and the bin glyph is rasterized in one of them.
  if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange ||
      event->type() == QEvent::ThemeChange) {
    progress_palette_.valid = false;
    discard_icons_.clear();
  }
  QTreeWidget::changeEvent(event);
}

QRect CurveTreeView::fullWidthRowRect(const QRect& row_rect) const {
  // A row rect from visualItemRect / option.rect covers only the name column,
  // but the progress decoration spans the whole row. Paint, hit-test and
  // invalidation all widen it HERE so they cannot disagree: the right edge is
  // rect().right() (the last painted pixel), never width(), which would place
  // the right-aligned cluster one pixel off from the hit-test.
  const QRect viewport_rect = viewport()->rect();
  QRect full = row_rect;
  full.setLeft(viewport_rect.left());
  full.setRight(viewport_rect.right());
  return full;
}

CurveTreeView::StopButtonHit CurveTreeView::stopButtonAt(const QPoint& viewport_pos) const {
  QTreeWidgetItem* item = itemAt(viewport_pos);
  if (item == nullptr) {
    return {};
  }
  const QVariant progress_data = item->data(kNameColumn, kDatasetProgressRole);
  if (!progress_data.isValid()) {
    return {};
  }
  const DatasetProgress progress = progress_data.value<DatasetProgress>();
  // A row that was never stoppable paints no cluster at all, so there is
  // nothing to hit — not even an inert glyph.
  if (!progress.cancellable) {
    return {};
  }
  const ProgressGeometry geom = progressGeometry(fullWidthRowRect(visualItemRect(item)));

  // Mirrors drawRow's enablement exactly: only a live load acts, and the bin
  // additionally needs a producer that can roll back.
  const bool live = progress.state == DatasetProgress::State::kLoading;
  if (geom.keep_button_rect.contains(viewport_pos)) {
    return {.item = item, .button = StopButton::kKeep, .progress = progress, .actionable = live};
  }
  if (geom.discard_button_rect.contains(viewport_pos)) {
    return {
        .item = item, .button = StopButton::kDiscard, .progress = progress, .actionable = live && progress.discardable};
  }
  return {};
}

CurveTreeView::StopButtonPoints CurveTreeView::activeStopButtonPoints() const {
  for (int i = 0; i < topLevelItemCount(); ++i) {
    const QRect row_rect = visualItemRect(topLevelItem(i));
    if (row_rect.isEmpty()) {
      continue;  // scrolled out or collapsed: nothing clickable on screen
    }
    const ProgressGeometry geom = progressGeometry(fullWidthRowRect(row_rect));
    // Report a centre only where the hit-test really resolves to that glyph AND
    // would act on it, so the reported point and the click path cannot diverge.
    StopButtonPoints points;
    if (const StopButtonHit hit = stopButtonAt(geom.keep_button_rect.center());
        hit.actionable && hit.button == StopButton::kKeep) {
      points.keep = geom.keep_button_rect.center();
    }
    if (const StopButtonHit hit = stopButtonAt(geom.discard_button_rect.center());
        hit.actionable && hit.button == StopButton::kDiscard) {
      points.discard = geom.discard_button_rect.center();
    }
    if (points.keep.x() >= 0 || points.discard.x() >= 0) {
      return points;
    }
  }
  return {};
}

QString CurveTreeView::stopButtonTooltip(const QPoint& viewport_pos) const {
  const StopButtonHit hit = stopButtonAt(viewport_pos);
  if (hit.button == StopButton::kNone) {
    return {};
  }
  // The glyphs stay painted through the terminal linger, but that load is over:
  // nothing left to stop, and nothing worth explaining about a row that is
  // about to retire.
  if (hit.progress.state != DatasetProgress::State::kLoading) {
    return {};
  }
  if (hit.button == StopButton::kKeep) {
    return tr("Stop loading and keep the data received so far");
  }
  // A greyed bin is the affordance that most needs a tooltip: it looks like a
  // choice and answers to nothing, so say why rather than leaving the user
  // clicking it.
  return hit.actionable ? tr("Stop loading and discard the partial data")
                        : tr("This source can stop but cannot discard partial data");
}

bool CurveTreeView::viewportEvent(QEvent* event) {
  if (event->type() == QEvent::ToolTip) {
    auto* help_event = static_cast<QHelpEvent*>(event);
    if (stopButtonAt(help_event->pos()).button != StopButton::kNone) {
      // Over a painted glyph the affordance answers for itself — or, when it is
      // inert, for nothing. Either way the row's own tooltip must not stand in
      // for it, so this branch always consumes the event.
      const QString tooltip = stopButtonTooltip(help_event->pos());
      if (tooltip.isEmpty()) {
        QToolTip::hideText();
      } else {
        QToolTip::showText(help_event->globalPos(), tooltip, viewport());
      }
      event->accept();
      return true;
    }
  }
  return QTreeWidget::viewportEvent(event);
}

void CurveTreeView::refreshStopButtonHover(const QPoint& viewport_pos) {
  // Only an actionable glyph lights up: a disabled one offers no click, so
  // highlighting it would promise one.
  const StopButtonHit hit = stopButtonAt(viewport_pos);
  QTreeWidgetItem* const hovered_item = hit.actionable ? hit.item : nullptr;
  const StopButton button = hit.actionable ? hit.button : StopButton::kNone;

  const QPersistentModelIndex hovered_index = hovered_item != nullptr
                                                  ? QPersistentModelIndex(indexFromItem(hovered_item, kNameColumn))
                                                  : QPersistentModelIndex();
  if (hovered_index == hovered_stop_row_ && button == hovered_stop_button_) {
    return;  // no transition: nothing to repaint
  }

  QTreeWidgetItem* previous = hovered_stop_row_.isValid() ? itemFromIndex(hovered_stop_row_) : nullptr;
  hovered_stop_row_ = hovered_index;
  hovered_stop_button_ = button;
  if (previous != nullptr && previous != hovered_item) {
    updateProgressRowRegion(previous);
  }
  updateProgressRowRegion(hovered_item);
}

void CurveTreeView::leaveEvent(QEvent* event) {
  refreshStopButtonHover(QPoint(-1, -1));
  QTreeWidget::leaveEvent(event);
}

const QPixmap& CurveTreeView::discardIcon(int size, const QColor& ink) const {
  // Keyed by size AND ink: a frame that paints one live bin beside one greyed
  // bin needs both tints at once, and a single slot re-rasterized the SVG twice
  // per frame as the two rows took turns evicting each other.
  const quint64 key = (static_cast<quint64>(static_cast<quint32>(size)) << 32U) | ink.rgba();
  if (const auto cached = discard_icons_.constFind(key); cached != discard_icons_.cend()) {
    return *cached;
  }
  // The stylesheet-driven recolor only knows light/dark ink, so tint the
  // rasterized glyph directly: SourceIn keeps the glyph's alpha and replaces
  // its color, which is what lets the bin track the ✕ through the enabled and
  // disabled inks.
  QPixmap icon = renderSvgPixmap(u":/resources/svg/trash.svg"_s, u"light"_s, QSize(size, size), devicePixelRatioF());
  if (!icon.isNull()) {
    QPainter tint(&icon);
    tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
    tint.fillRect(icon.rect(), ink);
  }
  return *discard_icons_.insert(key, std::move(icon));
}

void CurveTreeView::updateProgressRowRegion(QTreeWidgetItem* item) {
  if (item == nullptr) {
    return;
  }
  const QRect item_rect = visualItemRect(item);
  if (item_rect.isEmpty()) {
    return;  // scrolled out or collapsed: nothing on screen to invalidate
  }
  const QRect visible_row_rect = fullWidthRowRect(item_rect).intersected(viewport()->rect());
  if (!visible_row_rect.isEmpty()) {
    viewport()->update(visible_row_rect);
  }
}

void CurveTreeView::updateAnimatingRows() {
  bool has_animating_row = false;
  for (int i = 0; i < topLevelItemCount(); ++i) {
    QTreeWidgetItem* item = topLevelItem(i);
    if (!rowNeedsAnimation(item)) {
      continue;
    }
    has_animating_row = true;
    updateProgressRowRegion(item);
  }
  if (!has_animating_row) {
    animation_timer_->stop();
  }
}

QRect CurveTreeView::marqueeBandRect(const QRect& fill_rect, qint64 elapsed_ms, qint64 period_ms) {
  if (period_ms <= 0 || fill_rect.isEmpty()) {
    return {};
  }
  // A quarter-width band that enters from the left, crosses, and exits right.
  const int total_width = fill_rect.width();
  const int band_width = total_width / 4;
  const double phase = static_cast<double>(elapsed_ms % period_ms) / static_cast<double>(period_ms);
  // Starts one band-width OFF the left edge so elapsed 0 shows nothing yet and
  // the band slides in, rather than appearing mid-track.
  const int band_pos = static_cast<int>(phase * (total_width + band_width)) - band_width;
  QRect band = fill_rect;
  band.setLeft(fill_rect.left() + band_pos);
  band.setRight(band.left() + band_width);
  return band.intersected(fill_rect);
}

CurveTreeView::ProgressGeometry CurveTreeView::progressGeometry(const QRect& row_rect) const {
  // Layout: [progress bar, FULL row width ..................................]
  //         [ .......................... | text | bin | ✕ ]  <- drawn over it
  //
  // The bar spans the whole row and the right-edge cluster is composited on top
  // of it, so the bar reads as one continuous track rather than stopping short
  // to make room. The cluster's three cells are contiguous — no gaps — and run
  // flush to the row's right edge.
  constexpr int kCancelButtonSize = 24;
  constexpr int kCancelButtonMinSize = 10;
  constexpr int kTextAreaWidth = 50;

  ProgressGeometry geom;
  geom.fill_rect = row_rect;

  // Stop affordances: two squares flush against the row's right edge and its
  // full height, so they read as part of the row rather than as controls
  // floating inside it. Rightmost is the ✕ (stop and keep) — the safe one, and
  // the outer edge is the easiest target; the destructive bin sits inboard.
  const int button_size = std::clamp(row_rect.height(), kCancelButtonMinSize, kCancelButtonSize);
  const int button_top = row_rect.top() + ((row_rect.height() - button_size) / 2);
  // +1: right() is inclusive, so a rect of width `button_size` ending flush on
  // it starts at right() - button_size + 1.
  const int keep_left = row_rect.right() - button_size + 1;
  geom.keep_button_rect = QRect(keep_left, button_top, button_size, button_size);
  const int discard_left = keep_left - button_size;
  geom.discard_button_rect = QRect(discard_left, button_top, button_size, button_size);
  // Butts directly against the bin, so the caption and the glyphs read as one
  // cluster instead of a number floating away from its controls.
  geom.text_rect = QRect(discard_left - kTextAreaWidth, row_rect.top(), kTextAreaWidth, row_rect.height());

  return geom;
}

void CurveTreeView::drawRow(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
  QTreeWidgetItem* item = itemFromIndex(index);
  if (item == nullptr) {
    QTreeWidget::drawRow(painter, option, index);
    return;
  }

  const QVariant progress_data = item->data(kNameColumn, kDatasetProgressRole);
  if (!progress_data.isValid()) {
    QTreeWidget::drawRow(painter, option, index);
    if (g_geometry_debug_enabled) {
      paintGeometryDebug(painter, fullWidthRowRect(option.rect), index, nullptr);
    }
    return;
  }

  const DatasetProgress progress = progress_data.value<DatasetProgress>();
  const QRect row_rect = fullWidthRowRect(option.rect);

  const ProgressPalette& tokens = progressPalette();

  const ProgressGeometry geom = progressGeometry(row_rect);
  // The bar is the Accent family's checked tone, one step up the same ramp as the
  // loading row's backdrop below, so bar and backdrop are two rungs of one ramp
  // rather than two unrelated blues.
  // A DARK rung of the accent ramp. The lighter Checked tone (#99CCFF) sits one
  // step from selection_fill (#C2DCFF), so on a selected row the bar and the
  // row's own background were the same colour and the fill was invisible.
  const QColor indicator_color = tokens.indicator;
  // Backdrop behind the bar, in precedence order:
  //  - selected: the selection fill, which outranks any load state;
  //  - still loading: the Accent family's nominal tone — its lowest rung — so an
  //    in-flight row is legible as one at a glance even where the bar has not
  //    reached yet, and the unfilled track reads as the same hue as the bar;
  //  - otherwise (a terminal row seeing out its linger): the tree's own surface,
  //    so it sits flush with its neighbours.
  // NOT the progress track tone, which is darker than the tree surface in the
  // dark theme and banded the whole row a different shade.
  // Ask the selection model, not option.state: QTreeView::drawRow derives the
  // row's selected state internally and the option handed to this override does
  // NOT carry State_Selected. Reading the flag here reports "not selected" while
  // the base pass still paints the selection fill over the whole row.
  const bool row_selected = (option.state & QStyle::State_Selected) != 0 ||
                            (selectionModel() != nullptr && selectionModel()->isSelected(index));
  const bool row_loading =
      progress.state == DatasetProgress::State::kLoading || progress.state == DatasetProgress::State::kStopping;
  // While loading, the progress readout owns the row: a selection fill spans the
  // FULL width in the same blue family as the bar, so a selected row at 0% reads
  // as a finished one. Selection is transient information here and returns the
  // moment the load ends; the bar is the reason the row is decorated at all.
  QColor row_background = tokens.backdrop;
  if (row_selected && !row_loading) {
    row_background = tokens.selection_fill;
  } else if (row_loading) {
    // The unfilled TRACK: the Accent family's nominal rung, the lowest of the
    // same ramp the bar sits high on. The bar reads against it because it is a
    // far darker rung (checked-pressed), not because the track is a foreign hue.
    row_background = tokens.loading_backdrop;
  }

  // The bar as one (rect, color) pair, resolved once. Both paint passes below
  // consume it, so the band under the right-edge cluster cannot disagree with
  // the bar under the name.
  QRect bar_rect;
  QColor bar_color = indicator_color;
  switch (progress.state) {
    case DatasetProgress::State::kCompleted:
      bar_rect = geom.fill_rect;
      break;
    case DatasetProgress::State::kCancelled:
      bar_rect = geom.fill_rect;
      bar_color.setAlpha(128);
      break;
    case DatasetProgress::State::kFailed:
      bar_rect = geom.fill_rect;
      // A failure reads through the Highlight family, never the red status ink:
      // red is not this framework's failure signal. The flash carries the
      // alarm; the hue only has to stand apart from the normal indicator.
      bar_color = tokens.failed_bar;
      if (!progress.flash_on) {
        bar_color.setAlpha(100);
      }
      break;
    case DatasetProgress::State::kLoading:
    case DatasetProgress::State::kStopping:
      if (progress.indeterminate) {
        const qint64 elapsed_ms =
            animation_timer_->isActive() ? QDateTime::currentMSecsSinceEpoch() - animation_epoch_ms_ : 0;
        bar_rect = marqueeBandRect(geom.fill_rect, elapsed_ms, kMarqueePeriodMs);
      } else {
        bar_rect = geom.fill_rect;
        bar_rect.setWidth(static_cast<int>(geom.fill_rect.width() * progress.fraction));
      }
      break;
  }

  // Over a selection highlight the bar goes translucent so the highlight still
  // reads through it; one color for every pass, so the band under the cluster
  // cannot end up more saturated than the bar under the name.
  QColor bar_paint_color = bar_color;
  if (row_selected) {
    bar_paint_color.setAlpha(static_cast<int>(255 * 0.45));
  }

  // Row background then indicator, clipped to `region`.
  const auto paint_bar = [&](const QRect& region) {
    painter->fillRect(region, row_background);
    const QRect visible_bar = bar_rect.intersected(region);
    if (!visible_bar.isEmpty()) {
      painter->fillRect(visible_bar, bar_paint_color);
    }
  };

  paint_bar(row_rect);

  // Call base class to paint expander, icon, text
  QTreeWidget::drawRow(painter, option, index);

  // The base pass repaints the selection highlight over our fill, so re-apply
  // the bar on top of it (background untouched — that would erase the name).
  // Re-assert the bar over the base pass's selection fill. It MUST stay
  // translucent: this pass runs after the row's text was drawn, so an opaque
  // fill would paint over the dataset name.
  if (row_selected) {
    painter->fillRect(bar_rect, bar_paint_color);
  }

  // Re-assert the bar under the right-edge cluster, AFTER the base pass. The
  // base pass paints the value column and the column separator over our first
  // fill, so without this the cluster's glyphs would sit on top of foreign
  // content. Repainting the band makes the cluster opaque: whatever the view
  // drew there is hidden, separator included.
  const QRect cluster_rect = geom.text_rect.united(geom.discard_button_rect).united(geom.keep_button_rect);
  paint_bar(cluster_rect);

  // Paint right-edge cluster: percentage text and cancel button
  const QFont font = this->font();
  const QColor text_color = tokens.text;

  painter->setFont(font);
  painter->setPen(text_color);

  // The caption is ONLY ever a percentage — never a word. Prose in a data row
  // reads as tree content rather than chrome; the bar's color and the flash
  // carry the outcome. A terminal state therefore shows the percentage it
  // reached, not what happened to it.
  //
  // An indeterminate row shows nothing at all: it has no fraction to report.
  QString text;
  if (progress.state == DatasetProgress::State::kCompleted) {
    text = u"100%"_s;  // pinned, so a 0.999 fraction cannot render "99%" on success
  } else if (!progress.indeterminate) {
    text = QStringLiteral("%1%").arg(static_cast<int>(progress.fraction * 100));
  }

  if (!text.isEmpty()) {
    // Right-aligned so the caption sits against the bin rather than centred in
    // its cell with a gap on the icon side.
    painter->drawText(geom.text_rect, Qt::AlignRight | Qt::AlignVCenter, text);
  }

  // Stop affordances: ✕ stops and keeps what arrived, the bin stops and
  // discards it. Both are offered on the row so the choice is made by WHICH one
  // is clicked — the host never has to ask afterwards.
  //
  // They stay PAINTED through the terminal linger, greyed to the disabled ink
  // and inert, rather than disappearing: the row is about to retire, and having
  // the cluster lose two of its three cells for that last second reads as a
  // glitch. Only a live load accepts a click (see mousePressEvent).
  if (progress.cancellable) {
    const bool actionable = progress.state == DatasetProgress::State::kLoading;
    // Read the TRACKED hover, not QCursor: sampling the global cursor here made
    // the highlight depend on when the row happened to repaint.
    const bool row_hovered = hovered_stop_row_.isValid() && itemFromIndex(hovered_stop_row_) == item;

    // Both glyphs carry the framework's nominal icon ink — the same ink every
    // other icon in the app uses — dropping to the disabled ink once the load
    // has ended. Hover is the framework's translucent state layer behind the
    // glyph, NOT a recolor: a colored glyph here would read as a status, and
    // the row's state is already told by the bar.
    const QColor hover_layer = tokens.hover_layer;
    const QColor glyph_ink = actionable ? tokens.icon_ink : tokens.icon_ink_disabled;

    if (actionable && row_hovered && hovered_stop_button_ == StopButton::kKeep) {
      painter->fillRect(geom.keep_button_rect, hover_layer);
    }
    painter->setPen(QPen(glyph_ink, 2));
    painter->setRenderHint(QPainter::Antialiasing);
    const int inset = std::max(2, geom.keep_button_rect.height() / 4);
    const QRect inner = geom.keep_button_rect.adjusted(inset, inset, -inset, -inset);
    painter->drawLine(inner.topLeft(), inner.bottomRight());
    painter->drawLine(inner.topRight(), inner.bottomLeft());

    // A producer that cannot discard still gets the bin drawn, greyed: dropping
    // the cell mid-load reads as a glitch, and a live-looking bin that silently
    // kept the data would be worse than either.
    const bool discard_actionable = actionable && progress.discardable;
    const QColor bin_ink = discard_actionable ? glyph_ink : tokens.icon_ink_disabled;
    const QPixmap& bin = discardIcon(geom.discard_button_rect.height(), bin_ink);
    if (!bin.isNull()) {
      if (discard_actionable && row_hovered && hovered_stop_button_ == StopButton::kDiscard) {
        painter->fillRect(geom.discard_button_rect, hover_layer);
      }
      const QPainterStateGuard icon_guard(painter);
      painter->setRenderHint(QPainter::SmoothPixmapTransform);
      painter->drawPixmap(geom.discard_button_rect, bin);
    }
  }

  if (g_geometry_debug_enabled) {
    paintGeometryDebug(painter, row_rect, index, &geom);
  }
}

void CurveTreeView::applyForcedTopicMarks() {
  // Full-set replace: clear every mark, then set the current ones. The stored
  // set survives rebuilds (clearCurves + re-add), re-applied by addCatalogItems.
  std::function<void(QTreeWidgetItem*)> clear = [&](QTreeWidgetItem* item) {
    for (int i = 0; i < item->childCount(); ++i) {
      QTreeWidgetItem* child = item->child(i);
      if (child->data(kNameColumn, kForcedRole).toBool()) {
        child->setData(kNameColumn, kForcedRole, false);
      }
      clear(child);
    }
  };
  clear(invisibleRootItem());
  for (const QString& topic_path : forced_topic_paths_) {
    if (QTreeWidgetItem* node = findTopicNode(topic_path); node != nullptr) {
      node->setData(kNameColumn, kForcedRole, true);
    }
  }
}

bool CurveTreeView::isTopicPathForced(const QString& topic_path) {
  QTreeWidgetItem* node = findTopicNode(topic_path);
  return node != nullptr && node->data(kNameColumn, kForcedRole).toBool();
}

void CurveTreeView::refreshIcons(const QString& theme) {
  for (int i = 0; i < topLevelItemCount(); ++i) {
    refreshTopicIcons(topLevelItem(i), theme);
  }
}

void CurveTreeView::applyFilter(const QString& filter) {
  if (filter == last_filter_) {
    return;
  }
  last_filter_ = filter;
  refilterTree();
}

void CurveTreeView::setVisibleCurveKinds(bool show_plot, bool show_scene2d, bool show_scene3d) {
  if (show_plot_ == show_plot && show_scene2d_ == show_scene2d && show_scene3d_ == show_scene3d) {
    return;
  }
  show_plot_ = show_plot;
  show_scene2d_ = show_scene2d;
  show_scene3d_ = show_scene3d;
  refilterTree();
  viewport()->update();  // repaint rows for the new visibility (placeholder may show/hide)
}

void CurveTreeView::setEmptyFilterMessage(const QString& message) {
  if (empty_filter_message_ == message) {
    return;
  }
  empty_filter_message_ = message;
  refilterTree();  // cheap no-op on an empty tree; refreshes placeholders otherwise
}

void CurveTreeView::refilterTree() {
  const QStringList tokens = last_filter_.split(' ', Qt::SkipEmptyParts);

  // A topic's scene classification governs its ENTIRE subtree. `inherited_scene`
  // is the nearest scene-topic kind at or above `item` (kSceneNone / kScene2D /
  // kScene3D). Once the walk enters a 2D/3D topic every descendant inherits that
  // kind, so hiding the kind collapses the whole topic — its scalar fields
  // included — and those fields never fall into the Plot bucket. A row with no
  // scene ancestor is Plot ("by exclusion") when it bears a catalog key, or a
  // kind-less folder (revealed only by a visible descendant) when it doesn't.
  enum : int { kSceneNone = 0, kScene2D = 2, kScene3D = 3 };
  std::function<bool(QTreeWidgetItem*, int)> apply = [&](QTreeWidgetItem* item, int inherited_scene) {
    int scene = inherited_scene;
    if (scene == kSceneNone) {
      if (item->data(kNameColumn, kImageTopicRole).toBool()) {
        scene = kScene2D;
      } else if (item->data(kNameColumn, k3dObjectTopicRole).toBool()) {
        scene = kScene3D;
      }
    }
    bool any_child_visible = false;
    for (int i = 0; i < item->childCount(); ++i) {
      QTreeWidgetItem* child = item->child(i);
      if (child->data(kNameColumn, kEmptyMessageRole).toBool()) {
        continue;  // the managed empty-filter placeholder is not real data
      }
      any_child_visible = apply(child, scene) || any_child_visible;
    }
    bool type_ok = false;
    if (scene == kScene2D) {
      type_ok = show_scene2d_;
    } else if (scene == kScene3D) {
      type_ok = show_scene3d_;
    } else {
      // kCatalogItemRole is only ever set to a (non-empty) key, so its presence
      // marks a data-bearing row; a keyless folder rides on its children.
      const bool has_key = item->data(kNameColumn, kCatalogItemRole).isValid();
      type_ok = has_key && show_plot_;
    }
    QString haystack = item->data(kNameColumn, kSearchRole).toString();
    if (haystack.isEmpty()) {
      const QString full = item->data(kNameColumn, Qt::UserRole).toString();
      haystack = full.isEmpty() ? item->text(kNameColumn) : full;
    }
    const bool text_match = std::all_of(tokens.begin(), tokens.end(), [&](const QString& token) {
      return haystack.contains(token, Qt::CaseInsensitive);
    });
    // A row showing load progress is never filtered away. It is the feedback for
    // work happening right now — and it carries the stop affordances, so hiding
    // it would take away the only way to stop that load. Ghost rows (a dataset
    // with no catalog row yet) match no filter at all and would always vanish.
    // The exemption is inherently transient: it lasts as long as the decoration.
    const bool shows_progress = item->data(kNameColumn, kDatasetProgressRole).isValid();
    const bool visible = shows_progress || any_child_visible || (text_match && type_ok);
    item->setHidden(!visible);
    return visible;
  };

  for (int i = 0; i < topLevelItemCount(); ++i) {
    QTreeWidgetItem* dataset_node = topLevelItem(i);
    const bool visible = apply(dataset_node, kSceneNone);
    updateEmptyMessageChild(dataset_node, !visible);
  }
}

void CurveTreeView::reapplyFilter() {
  const bool type_restricted = !(show_plot_ && show_scene2d_ && show_scene3d_);
  if (last_filter_.isEmpty() && !type_restricted) {
    return;  // no active filter: freshly inserted rows are visible by default
  }
  refilterTree();
}

void CurveTreeView::updateEmptyMessageChild(QTreeWidgetItem* dataset_node, bool subtree_hidden) {
  // Find any existing placeholder + learn whether the dataset has real topics.
  QTreeWidgetItem* placeholder = nullptr;
  bool has_real_child = false;
  for (int i = 0; i < dataset_node->childCount(); ++i) {
    QTreeWidgetItem* child = dataset_node->child(i);
    if (child->data(kNameColumn, kEmptyMessageRole).toBool()) {
      placeholder = child;
    } else {
      has_real_child = true;
    }
  }

  // Only speak up when a dataset that HAS topics has them all filtered away.
  const bool want_message = subtree_hidden && has_real_child && !empty_filter_message_.isEmpty();
  if (!want_message) {
    if (placeholder != nullptr) {
      placeholder->setHidden(true);
    }
    return;
  }

  if (placeholder == nullptr) {
    placeholder = new QTreeWidgetItem(dataset_node);
    placeholder->setData(kNameColumn, kEmptyMessageRole, true);
    placeholder->setFlags(Qt::ItemIsEnabled);  // display-only: not selectable/draggable
    QFont message_font = font();
    message_font.setItalic(true);
    placeholder->setFont(kNameColumn, message_font);
    placeholder->setForeground(kNameColumn, palette().color(QPalette::Disabled, QPalette::Text));
  }
  placeholder->setText(kNameColumn, empty_filter_message_);
  placeholder->setHidden(false);
  // Span the message across both columns; re-assert each call since a re-sort can
  // move the placeholder to a different child row (spanning is keyed by row).
  setFirstColumnSpanned(dataset_node->indexOfChild(placeholder), indexFromItem(dataset_node), true);
  dataset_node->setHidden(false);   // keep the dataset name visible
  dataset_node->setExpanded(true);  // reveal the message row
}

std::vector<QString> CurveTreeView::selectedCurveNames() const {
  std::vector<QString> names;
  for (auto* item : selectedItems()) {
    if (isValueOnlyItem(item)) {
      continue;  // string fields are not draggable curves
    }
    const QString full = item->data(kNameColumn, Qt::UserRole).toString();
    if (!full.isEmpty()) {
      names.push_back(full);
    }
  }
  normalizeCurveNames(names);
  return names;
}

std::vector<QString> CurveTreeView::selectedCurveNamesRecursive() const {
  std::vector<QString> names;
  std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* item) {
    const QString full = curveNameForItem(item);
    if (!full.isEmpty() && !isValueOnlyItem(item)) {
      names.push_back(full);
    }
    if (isObjectTopicItem(item)) {
      return;
    }
    for (int i = 0; i < item->childCount(); ++i) {
      collect(item->child(i));
    }
  };
  for (auto* item : selectedItems()) {
    collect(item);
  }
  normalizeCurveNames(names);
  return names;
}

std::vector<QString> CurveTreeView::selectedCatalogKeysRecursive() const {
  return collectSelectedCatalogKeys(/*exclude_not_draggable=*/false, nullptr);
}

std::vector<QString> CurveTreeView::selectedCatalogKeysForDrag(QStringList* skipped_keys) const {
  return collectSelectedCatalogKeys(/*exclude_not_draggable=*/true, skipped_keys);
}

// The complete walk keeps not-draggable object rows — deletion and selection-size
// logic must see the whole selection (an empty result reads as "nothing
// selected", which some callers widen to "everything"). Only the drag-payload
// variant excludes them (a not-draggable row riding into a drop would hit the very
// dead end its exclusion exists to prevent), reporting each via `skipped_keys` so
// the host can say why fewer topics arrived.
std::vector<QString> CurveTreeView::collectSelectedCatalogKeys(
    bool exclude_not_draggable, QStringList* skipped_keys) const {
  std::vector<QString> keys;
  std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* item) {
    const QString key = catalogKeyForItem(item);
    if (!key.isEmpty()) {
      if (exclude_not_draggable && isNotDraggableItem(item)) {
        if (skipped_keys != nullptr && !skipped_keys->contains(key)) {
          skipped_keys->append(key);
        }
      } else if (!isValueOnlyItem(item)) {
        keys.push_back(key);
      }
    }
    if (isObjectTopicItem(item)) {
      return;
    }
    for (int i = 0; i < item->childCount(); ++i) {
      collect(item->child(i));
    }
  };
  for (auto* item : selectedItems()) {
    collect(item);
  }
  normalizeCurveNames(keys);
  return keys;
}

void CurveTreeView::setValuesColumnHidden(bool hidden) {
  setColumnHidden(kValueColumn, hidden);
  // Hiding a section frees its width; Name reclaims it (and gives it back when
  // Value returns) so the two always span the viewport exactly.
  header_policy_->rebalance();
  if (!hidden) {
    scheduleValueRefresh();  // re-show stale cells when the column reappears
  }
}

void CurveTreeView::refreshVisibleValues(const std::function<QString(const QString&)>& value_provider) {
  value_provider_ = value_provider;
  applyVisibleValues();
}

void CurveTreeView::applyVisibleValues() {
  if (isColumnHidden(kValueColumn) || !value_provider_) {
    return;
  }
  // Visit every item but cull each row independently by its rect — only the
  // on-screen leaves are written, so the expensive part (the per-leaf lookup
  // inside value_provider_) stays O(visible). The walk itself is O(N) cheap
  // geometry checks. Matches PlotJuggler 3's curve-list refresh: culling each
  // row (rather than breaking at the first off-screen row) is what keeps
  // scrolled-in rows correct in a deeply-nested tree.
  const int viewport_height = viewport()->height();
  for (int i = 0; i < topLevelItemCount(); ++i) {
    applyVisibleValuesToSubtree(topLevelItem(i), viewport_height);
  }
}

void CurveTreeView::applyVisibleValuesToSubtree(QTreeWidgetItem* item, int viewport_height) {
  if (item->childCount() != 0) {
    for (int i = 0; i < item->childCount(); ++i) {
      applyVisibleValuesToSubtree(item->child(i), viewport_height);
    }
    return;
  }
  const QString key = catalogKeyForItem(item);
  if (key.isEmpty()) {
    return;  // group placeholder / unkeyed row
  }
  const QRect rect = visualItemRect(item);
  if (rect.isNull() || rect.bottom() < 0 || rect.top() > viewport_height) {
    return;  // off-screen or collapsed — skip the value lookup entirely
  }
  const QString text = value_provider_(key);
  if (text == item->text(kValueColumn)) {
    return;  // value unchanged — skip setText so a stable cell emits no
             // dataChanged / repaint (the bulk of the value column's CPU at 10 Hz)
  }
  item->setText(kValueColumn, text);
  // Full value as a tooltip so a string left-elided in the narrow Value column
  // (or a long number) stays readable on hover; trimmed of the alignment padding.
  item->setToolTip(kValueColumn, text.trimmed());
}

void CurveTreeView::scheduleValueRefresh() {
  // This coalesces VISIBILITY-driven re-applies (expand/collapse/scroll/resize)
  // onto the next event-loop turn; the TRACKER-driven refresh rate is capped
  // separately by the owner (CurveListPanel's 10 Hz throttle). Deferring matters
  // because when this fires from an itemExpanded / scroll handler the freshly-
  // revealed rows have not been laid out yet, so reading visualItemRect now would
  // wrongly cull them.
  if (value_refresh_scheduled_ || !value_provider_ || isColumnHidden(kValueColumn)) {
    return;
  }
  value_refresh_scheduled_ = true;
  QTimer::singleShot(0, this, [this]() {
    value_refresh_scheduled_ = false;
    applyVisibleValues();
  });
}

void CurveTreeView::resizeEvent(QResizeEvent* event) {
  QTreeWidget::resizeEvent(event);
  scheduleValueRefresh();  // a taller viewport exposes more rows to fill
}

void CurveTreeView::setDragSelectionProvider(DragSelectionProvider provider) {
  drag_selection_provider_ = std::move(provider);
}

void CurveTreeView::sortTree() {
  std::function<void(QTreeWidgetItem*)> sort_children = [&](QTreeWidgetItem* item) {
    item->sortChildren(kNameColumn, Qt::AscendingOrder);
    for (int i = 0; i < item->childCount(); ++i) {
      sort_children(item->child(i));
    }
  };
  sort_children(invisibleRootItem());
}

void CurveTreeView::setDescendantsExpanded(QTreeWidgetItem* item, bool expanded) {
  for (int i = 0; i < item->childCount(); ++i) {
    QTreeWidgetItem* child = item->child(i);
    if (child->childCount() > 0) {
      child->setExpanded(expanded);
      setDescendantsExpanded(child, expanded);
    }
  }
}

std::vector<QString> CurveTreeView::selectedCurveNamesForDrag() const {
  std::vector<QString> names =
      drag_selection_provider_ != nullptr ? drag_selection_provider_() : selectedCurveNamesRecursive();
  normalizeCurveNames(names);
  return names;
}

#ifdef PJ_TARGET_WASM
bool CurveTreeView::event(QEvent* event) {
  if (wasm_drag_mime_ != nullptr) {
    const bool escape = event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape;
    if (escape || event->type() == QEvent::WindowDeactivate || event->type() == QEvent::UngrabMouse) {
      // The synchronous drop handler still owns wasm_drag_mime_. Do not let a
      // nested cancellation free it or balance the source press before the
      // outer drop returns and completes its normal cleanup.
      if (in_wasm_drop_) {
        event->accept();
        return true;
      }
      abortWasmDrag(QApplication::keyboardModifiers());
      event->accept();
      return true;
    }
  }
  return QTreeWidget::event(event);
}
#endif

bool CurveTreeView::pastDragThreshold(const QPoint& pos) const {
  return (pos - drag_start_pos_).manhattanLength() >= QApplication::startDragDistance();
}

void CurveTreeView::mousePressEvent(QMouseEvent* event) {
#ifdef PJ_TARGET_WASM
  if (in_wasm_drop_) {
    event->accept();
    return;
  }
  // A second press should not normally arrive before release, but cancelling
  // here keeps the synthetic drag state balanced if the browser interrupted a
  // gesture (lost focus, modal UI, or an injected automation event).
  cancelWasmDrag();
#endif
  drag_curve_names_.clear();
  drag_catalog_keys_.clear();
  suppress_next_release_ = false;
  drag_button_ = Qt::NoButton;
  not_draggable_reason_.reset();
  QTreeWidgetItem* const item = itemAt(event->pos());
  if (event->button() == Qt::LeftButton) {
    if (item != nullptr) {
      const QVariant row_key_data = item->data(kNameColumn, kDatasetRowKeyRole);
      // Only an actionable glyph reaches the host: a greyed bin is inert (the
      // producer cannot discard), so a click there must do nothing rather than
      // fall back to keeping.
      if (const StopButtonHit hit = stopButtonAt(event->pos()); hit.actionable && row_key_data.isValid()) {
        const bool keep = hit.button == StopButton::kKeep;
        const quint64 row_key = row_key_data.toULongLong();
        event->accept();
        qCInfo(lcCurveTreeCancel) << "stop affordance clicked: row" << row_key << "keep_partial" << keep;
        emit cancelRequested(row_key, /*keep_partial=*/keep);
        return;
      }
    }
  }
  if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton) {
    // Only draggable rows (curve leaves / object topics) initiate a drag or the
    // drag-the-whole-selection gesture. Non-draggable rows — notably the selectable
    // dataset groups — fall straight through to the base handler, so plain/Ctrl/Shift
    // selection and expand/collapse behave normally and a dataset never starts a drag.
    const bool draggable = item != nullptr && (item->flags() & Qt::ItemIsDragEnabled);
    if (draggable) {
      drag_start_pos_ = event->pos();
      drag_button_ = event->button();

      const Qt::KeyboardModifiers selection_modifiers = Qt::ControlModifier | Qt::ShiftModifier | Qt::MetaModifier;
      const QString item_catalog_key = catalogKeyForItem(item);
      if (!item_catalog_key.isEmpty()) {
        drag_catalog_keys_.push_back(item_catalog_key);
      }
      if (item->isSelected() && !(event->modifiers() & selection_modifiers)) {
        drag_curve_names_ = selectedCurveNamesForDrag();
        // A plain press on an already-selected row drags the WHOLE selection.
        // Preserve it (suppress the release that would otherwise collapse it to
        // the clicked row) whenever more than one row is selected — counting
        // object/image topics too, which contribute catalog keys but no scalar
        // curve names. The payload itself is read back from the live selection in
        // createDragMimeData(), so it stays correct as long as we keep it intact.
        if (drag_curve_names_.size() > 1 || selectedCatalogKeysRecursive().size() > 1) {
          suppress_next_release_ = true;
          event->accept();
          return;
        }
      }
    } else if (event->button() == Qt::LeftButton && isNotDraggableItem(item)) {
      // Pulling on a not-draggable row must not fail silently: arm a one-shot
      // notice that fires if this press turns into a drag gesture
      // (mouseMoveEvent). drag_start_pos_ is free here — such a row never
      // arms a real drag.
      drag_start_pos_ = event->pos();
      not_draggable_reason_ = item->toolTip(kNameColumn);
    }
  }
  QTreeWidget::mousePressEvent(event);
}

void CurveTreeView::mouseMoveEvent(QMouseEvent* event) {
  refreshStopButtonHover(event->position().toPoint());
#ifdef PJ_TARGET_WASM
  if (in_wasm_drop_) {
    event->accept();
    return;
  }
  if (wasm_drag_mime_ != nullptr) {
    if (!event->buttons().testFlag(wasm_drag_button_)) {
      // Browser focus changes can lose the matching release. Do not leave a
      // stale accepted target receiving buttonless drag moves indefinitely.
      abortWasmDrag(event->modifiers());
      event->accept();
      return;
    }
    // Once the threshold starts our in-app drag, keep every grabbed move in
    // the DnD dispatcher. Passing these moves back to QAbstractItemView would
    // rubber-select each source row crossed on the way to the plot.
    updateWasmDrag(event->globalPosition().toPoint(), event->buttons(), event->modifiers());
    event->accept();
    return;
  }
#endif
  if (not_draggable_reason_.has_value()) {
    if (!event->buttons().testFlag(Qt::LeftButton)) {
      not_draggable_reason_.reset();
    } else if (pastDragThreshold(event->pos())) {
      // Same threshold as a real drag start, so the notice fires exactly when
      // the pull stops reading as a click. Selection handling continues below —
      // the notice explains the missing drag, it does not swallow the gesture.
      const QString reason = *not_draggable_reason_;
      not_draggable_reason_.reset();
      emit dragAttemptedOnNotDraggableRow(reason);
    }
  }
  if (drag_button_ == Qt::NoButton) {
    QTreeWidget::mouseMoveEvent(event);
    return;
  }
  if (!(event->buttons() & drag_button_)) {
    drag_button_ = Qt::NoButton;
    drag_curve_names_.clear();
    drag_catalog_keys_.clear();
    QTreeWidget::mouseMoveEvent(event);
    return;
  }
  if (!pastDragThreshold(event->pos())) {
    if (drag_curve_names_.empty() && drag_catalog_keys_.empty()) {
      QTreeWidget::mouseMoveEvent(event);
    } else {
      event->accept();
    }
    return;
  }

  QStringList skipped_keys;
  QMimeData* mime_data = createDragMimeData(drag_button_, &skipped_keys);
  drag_button_ = Qt::NoButton;
  drag_curve_names_.clear();
  drag_catalog_keys_.clear();
  if (mime_data == nullptr) {
    return;
  }

#ifdef PJ_TARGET_WASM
  beginWasmDrag(mime_data, event, std::move(skipped_keys));
#else
  auto* drag = new QDrag(this);
  drag->setMimeData(mime_data);
  drag->exec(Qt::CopyAction | Qt::MoveAction);
  // Only after the drag ends: the host answers this with a toast, and a toast
  // raised mid-drag would sit over drop targets in its corner of the window.
  if (!skipped_keys.isEmpty()) {
    emit dragPayloadKeysSkipped(skipped_keys);
  }
#endif
}

#ifdef PJ_TARGET_WASM
void CurveTreeView::beginWasmDrag(QMimeData* mime_data, QMouseEvent* event, QStringList skipped_keys) {
  // A drop handler running on our stack (in_wasm_drop_) may synthesize a press
  // that reaches here; do not start a fresh drag inside the in-flight one.
  if (in_wasm_drop_) {
    delete mime_data;
    return;
  }
  wasm_drag_mime_.reset(mime_data);
  wasm_drag_skipped_keys_ = std::move(skipped_keys);
  wasm_drag_button_ = event->buttons().testFlag(Qt::RightButton) ? Qt::RightButton : Qt::LeftButton;
  updateWasmDrag(event->globalPosition().toPoint(), event->buttons(), event->modifiers());
  event->accept();
}

void CurveTreeView::updateWasmDrag(
    const QPoint& global_pos, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers) {
  if (wasm_drag_mime_ == nullptr) {
    return;
  }

  QWidget* leaf = QApplication::widgetAt(global_pos);

  // If the pointer is still inside the accepted target (possibly over one of
  // its children), give it a move first. A target may reject only part of its
  // area; rejection falls through to the next accepting ancestor below.
  bool target_is_ancestor = false;
  for (QWidget* candidate = leaf; candidate != nullptr; candidate = candidate->parentWidget()) {
    if (candidate == wasm_drag_target_) {
      target_is_ancestor = true;
      break;
    }
  }
  if (wasm_drag_target_ != nullptr && target_is_ancestor) {
    QDragMoveEvent move_event(
        wasm_drag_target_->mapFromGlobal(global_pos), Qt::CopyAction | Qt::MoveAction, wasm_drag_mime_.get(), buttons,
        modifiers);
    QApplication::sendEvent(wasm_drag_target_, &move_event);
    if (move_event.isAccepted()) {
      return;
    }
  }

  if (wasm_drag_target_ != nullptr) {
    QDragLeaveEvent leave_event;
    QApplication::sendEvent(wasm_drag_target_, &leave_event);
    wasm_drag_target_.clear();
  }

  // Match Qt's normal deepest-widget-first routing. Event filters installed on
  // placeholder buttons and the live plot canvas still run through sendEvent;
  // an ignored child lets the nearest accepting parent try the same payload.
  QPointer<QWidget> candidate = leaf;
  while (candidate != nullptr) {
    QPointer<QWidget> current = candidate;
    // Capture the next hop before delivery: a drag-enter filter is arbitrary
    // application code and may synchronously delete its watched widget.
    QPointer<QWidget> parent = current->parentWidget();
    if (!current->acceptDrops()) {
      candidate = parent;
      continue;
    }
    QDragEnterEvent enter_event(
        current->mapFromGlobal(global_pos), Qt::CopyAction | Qt::MoveAction, wasm_drag_mime_.get(), buttons, modifiers);
    QApplication::sendEvent(current, &enter_event);
    if (enter_event.isAccepted() && current != nullptr) {
      wasm_drag_target_ = current;
      return;
    }
    candidate = parent;
  }
}

void CurveTreeView::finishWasmDrag(QMouseEvent* event) {
  // The drop handler rebuilds this tree synchronously (see in_wasm_drop_); a
  // re-entry on that stack must not dispatch a nested drop.
  if (wasm_drag_mime_ == nullptr || in_wasm_drop_) {
    return;
  }

  const QPoint global_pos = event->globalPosition().toPoint();
  // Drop straight to the target the last move already entered and accepted — do
  // NOT re-run updateWasmDrag here. Re-routing on release could leave the old
  // target and enter a new one (arbitrary app code) before the drop, turning a
  // single release into enter->leave->enter->drop. DnD drops on the last-entered
  // target, so any pointer drift since the last move is intentionally ignored.
  QPointer<QWidget> target = wasm_drag_target_;
  if (target != nullptr) {
    QDropEvent drop_event(
        QPointF(target->mapFromGlobal(global_pos)), Qt::CopyAction | Qt::MoveAction, wasm_drag_mime_.get(),
        wasm_drag_button_, event->modifiers());
    // RAII so the guard clears on every exit of this scope (including an
    // exception unwinding out of the drop handler).
    in_wasm_drop_ = true;
    const auto drop_guard = qScopeGuard([this] { in_wasm_drop_ = false; });
    QApplication::sendEvent(target, &drop_event);
  }

  // A drop ends the sequence without a leave event, matching native DnD.
  wasm_drag_target_.clear();
  wasm_drag_mime_.reset();
  wasm_drag_button_ = Qt::NoButton;
  event->accept();

  // The drop is delivered; the skipped-keys toast can no longer sit between
  // the pointer and the drop target's hit test.
  if (!wasm_drag_skipped_keys_.isEmpty()) {
    const QStringList skipped_keys = wasm_drag_skipped_keys_;
    wasm_drag_skipped_keys_.clear();
    emit dragPayloadKeysSkipped(skipped_keys);
  }
}

void CurveTreeView::cancelWasmDrag() {
  if (wasm_drag_target_ != nullptr) {
    QDragLeaveEvent leave_event;
    QApplication::sendEvent(wasm_drag_target_, &leave_event);
  }
  wasm_drag_target_.clear();
  wasm_drag_mime_.reset();
  wasm_drag_button_ = Qt::NoButton;
  wasm_drag_skipped_keys_.clear();
}

void CurveTreeView::balanceWasmSourcePress(Qt::MouseButton button, Qt::KeyboardModifiers modifiers) {
  if (suppress_next_release_) {
    // The corresponding press was consumed before the base class saw it.
    suppress_next_release_ = false;
    return;
  }
  // Balance the base-class press at its original position. Forwarding a real
  // release over the plot makes ExtendedSelection select the entire row span.
  QMouseEvent balanced_release(
      QEvent::MouseButtonRelease, QPointF(drag_start_pos_), QPointF(viewport()->mapToGlobal(drag_start_pos_)), button,
      Qt::NoButton, modifiers);
  QTreeWidget::mouseReleaseEvent(&balanced_release);
}

void CurveTreeView::abortWasmDrag(Qt::KeyboardModifiers modifiers) {
  const Qt::MouseButton button = wasm_drag_button_;
  cancelWasmDrag();
  balanceWasmSourcePress(button, modifiers);
}
#endif

QMimeData* CurveTreeView::createDragMimeData(Qt::MouseButton button, QStringList* skipped_keys) const {
  const std::vector<QString> names = selectedCurveNamesForDrag();

  // The catalog payload must carry EVERY selected item, not just the row under
  // the cursor when the drag began. Object/image topics come from the recursive
  // catalog walk; scalar curves (whose catalog key is their own name) are added
  // from `names`, which also folds in a cross-view selection supplied by a drag
  // selection provider.
  QStringList catalog_keys;
  for (const QString& key : selectedCatalogKeysForDrag(skipped_keys)) {
    catalog_keys.push_back(key);
  }
  for (const QString& name : names) {
    catalog_keys.push_back(name);
  }
  catalog_keys.removeDuplicates();

  // Left-button drag → add curve(s) to a plot; right-button drag of exactly two
  // curves → XY scatter plot. Anything else is not a drag we initiate.
  const bool left_add = button == Qt::LeftButton;
  const bool right_xy = button == Qt::RightButton && names.size() == 2;
  if ((!left_add && !right_xy) || (names.empty() && catalog_keys.empty())) {
    return nullptr;
  }

  QByteArray encoded;
  QDataStream stream(&encoded, QIODevice::WriteOnly);
  for (const QString& name : names) {
    stream << name;
  }

  auto* mime_data = new QMimeData();
  // Plot-widget and placeholder drop sites match on these mime keys exactly.
  if (!catalog_keys.empty()) {
    mime_data->setData(catalogItemsMimeType(), encodeCatalogKeys(catalog_keys));
  }
  if (left_add && !names.empty()) {
    mime_data->setData(u"curveslist/add_curve"_s, encoded);
  } else if (right_xy) {
    mime_data->setData(newXyAxisMimeType(), encoded);
  }
  return mime_data;
}

void CurveTreeView::mouseReleaseEvent(QMouseEvent* event) {
#ifdef PJ_TARGET_WASM
  if (in_wasm_drop_) {
    event->accept();
    return;
  }
  if (wasm_drag_mime_ != nullptr && event->button() == wasm_drag_button_) {
    const Qt::MouseButton button = wasm_drag_button_;
    finishWasmDrag(event);
    // The native QDrag loop leaves QAbstractItemView's pressed/selection state
    // balanced when it consumes the release. Our in-app loop must forward that
    // release explicitly; otherwise the next plain drag is interpreted as a
    // continuation and selects every row crossed between the two gestures.
    balanceWasmSourcePress(button, event->modifiers());
    return;
  }
#endif
  if (suppress_next_release_) {
    suppress_next_release_ = false;
    drag_button_ = Qt::NoButton;
    drag_curve_names_.clear();
    drag_catalog_keys_.clear();
    event->accept();
    return;
  }
  if (event->button() == drag_button_) {
    drag_button_ = Qt::NoButton;
    drag_curve_names_.clear();
    drag_catalog_keys_.clear();
  }
  QTreeWidget::mouseReleaseEvent(event);
}

QString formatScalarForColumn(double value, int precision) {
  if (!std::isfinite(value)) {
    return u"-"_s;
  }
  // Fixed precision keeps a constant fractional width; then overwrite trailing
  // zeros — and a bare trailing '.' once all decimals are blanked — with spaces.
  // The single appended space + a right-aligned monospace cell line up every
  // decimal point across rows (e.g. 1.2 -> "1.2   ", 5 -> "5     ").
  QString text = QString::number(value, 'f', precision);
  const int dot = text.indexOf(QLatin1Char('.'));
  if (dot >= 0) {
    int idx = text.size() - 1;
    while (idx > dot && text[idx] == QLatin1Char('0')) {
      text[idx] = QLatin1Char(' ');
      --idx;
    }
    if (idx == dot) {
      text[idx] = QLatin1Char(' ');
    }
  }
  return text + QLatin1Char(' ');
}

}  // namespace PJ
