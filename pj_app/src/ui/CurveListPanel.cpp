// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/CurveListPanel.h"

#include <QAction>
#include <QCheckBox>
#include <QDomDocument>
#include <QDomElement>
#include <QEvent>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QLineF>
#include <QMargins>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPoint>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidgetItem>
#include <QWidgetAction>
#include <algorithm>
#include <array>
#include <optional>

#include "TopicDemandController.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/TopicDemandTracker.h"
#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/Search.h"
#include "pj_widgets/SvgUtil.h"
#include "scene_object_classification.h"
#include "ui_CurveListPanel.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

constexpr auto kPreserveTopicNameKey = "CurveListPanel/show_topics";
constexpr auto kShowValuesKey = "CurveListPanel/show_values";

// Composite an "omitted" variant of a chrome glyph: the base dimmed to ~40% with
// a top-left → bottom-right diagonal slash drawn over it — same direction, ink,
// and weight as the app's visibility_off eye-slash — haloed for a clean cut on
// any glyph. Used for the UNCHECKED (hidden) state of the type-filter toggles so
// a disabled kind reads as struck-through, consistent with the visibility eye.
QPixmap makeOmittedGlyph(const QPixmap& base, const QColor& ink, const QColor& halo) {
  if (base.isNull()) {
    return base;
  }
  QPixmap out(base.size());
  out.setDevicePixelRatio(base.devicePixelRatio());
  out.fill(Qt::transparent);
  QPainter painter(&out);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setOpacity(0.40);
  painter.drawPixmap(0, 0, base);
  painter.setOpacity(1.0);
  const qreal width = base.width();
  const qreal height = base.height();
  const qreal inset = qMax<qreal>(2.0, width * 0.12);
  const QLineF slash(inset, inset, width - inset, height - inset);  // top-left → bottom-right
  // Thin stroke to match the visibility_off eye-slash weight (~1px at 20px icon).
  const qreal ink_width = qMax<qreal>(1.0, width * 0.05);
  const qreal halo_width = ink_width + qMax<qreal>(1.0, width * 0.045);  // background gap around the slash
  painter.setPen(QPen(halo, halo_width, Qt::SolidLine, Qt::RoundCap));
  painter.drawLine(slash);
  painter.setPen(QPen(ink, ink_width, Qt::SolidLine, Qt::RoundCap));
  painter.drawLine(slash);
  return out;
}

// Value-column refresh cap. Playback drives tracker updates up to ~60 Hz; 10 Hz
// (100 ms) is plenty for reading numbers and keeps the per-tick scalar reads off
// the hot path.
constexpr int kValueRefreshIntervalMs = 100;

// Per-dataset active-topic sets for every per-topic-pause-capable dataset,
// computed ONCE per pass — the per-item alternative (tracker->activeTopics per
// catalog item) copies and re-sorts the active vector hundreds of times per
// rebuild. A dataset absent from the map is not pause-capable, so its topics
// are never "unsubscribed". Empty whenever `tracker` is null.
QHash<DatasetId, QSet<QString>> buildActiveSets(const CatalogModel& catalog, const TopicDemandTracker* tracker) {
  QHash<DatasetId, QSet<QString>> sets;
  if (tracker == nullptr) {
    return sets;
  }
  for (const auto& [dataset_id, dataset_name] : catalog.datasets()) {
    if (!catalog.isPerTopicPauseCapable(dataset_id)) {
      continue;
    }
    const std::vector<QString> active = tracker->activeTopics(dataset_id);
    sets.insert(dataset_id, QSet<QString>(active.begin(), active.end()));
  }
  return sets;
}

// See buildActiveSets: a topic on a pause-capable dataset that nothing
// currently references.
bool isTopicUnsubscribed(
    const QHash<DatasetId, QSet<QString>>& active_sets, DatasetId dataset_id, const QString& topic_name) {
  const auto it = active_sets.constFind(dataset_id);
  return it != active_sets.constEnd() && !it->contains(topic_name);
}

// Why an object topic of `type` cannot be dragged into a view — shown as the
// row's tooltip so a drag that never starts doesn't read as a bug.
QString undisplayableObjectTooltip(sdk::BuiltinObjectType type) {
  switch (type) {
    case sdk::BuiltinObjectType::kCameraInfo:
      return QObject::tr(
          "Camera calibration topic, used automatically by 2D and 3D views. "
          "It cannot be displayed on its own.");
    case sdk::BuiltinObjectType::kOccupancyGridUpdate:
      return QObject::tr(
          "Incremental map update, applied automatically to its occupancy grid. "
          "It cannot be displayed on its own.");
    default:
      return QObject::tr("No view can display this topic type.");
  }
}

CurveTreeView::CurvePath treePathFromCatalogItem(
    const CatalogItem& item, const QHash<DatasetId, QSet<QString>>& active_sets) {
  const auto* scalar = asScalarField(item);
  const auto* object_topic = asObjectTopic(item);
  const auto* advertised = asAdvertisedTopic(item);
  // An advertised placeholder classified kNone is scalar-shaped (no data yet, so
  // it's shown as a flat, draggable leaf at the topic itself — placeholders carry
  // no field breakdown); any other classification is object-shaped, shown as a
  // non-selectable terminal node exactly like a real ObjectTopicPayload.
  const bool placeholder_is_object =
      advertised != nullptr && advertised->classification != sdk::BuiltinObjectType::kNone;
  const auto object_type = object_topic != nullptr ? object_topic->object_type
                           : placeholder_is_object ? advertised->classification
                                                   : sdk::BuiltinObjectType::kNone;
  // An object topic no dock family claims (kCameraInfo, ...) must not start a
  // drag — the drop could only end in a "cannot display" dead end.
  const bool object_undisplayable =
      (object_topic != nullptr || placeholder_is_object) && !isDroppableObjectType(object_type);
  return CurveTreeView::CurvePath{
      .key = item.key,
      .dataset = item.dataset_name,
      .topic = item.topic_name,
      .field = scalar != nullptr ? scalar->field_name : QString{},
      .selectable = scalar != nullptr || (advertised != nullptr && !placeholder_is_object),
      // String fields drag onto the State Transitions strip (plots keep
      // refusing them via the curveDescriptor gate, so a plot drop is a no-op).
      .draggable = !object_undisplayable,
      .tooltip = object_undisplayable ? undisplayableObjectTooltip(object_type) : QString{},
      .is_image_topic = isImageFamilyObjectType(object_type),
      .is_3d_object_topic = is3dSceneObjectType(object_type),
      .is_placeholder = advertised != nullptr,
      .is_unsubscribed = isTopicUnsubscribed(active_sets, item.dataset_id, item.topic_name),
  };
}

void addCatalogItems(
    CurveTreeView* tree_view, const CatalogModel* catalog, const TopicDemandTracker* tracker,
    const std::vector<CatalogItem>& items) {
  if (tree_view == nullptr || catalog == nullptr || items.empty()) {
    return;
  }
  const QHash<DatasetId, QSet<QString>> active_sets = buildActiveSets(*catalog, tracker);
  std::vector<CurveTreeView::CurvePath> paths;
  paths.reserve(items.size());
  for (const CatalogItem& item : items) {
    paths.push_back(treePathFromCatalogItem(item, active_sets));
  }
  tree_view->addCatalogItems(paths);
}

void rebuildTree(
    CurveTreeView* tree_view, CatalogModel* catalog, const TopicDemandTracker* tracker,
    const QSet<QString>& custom_keys) {
  // A rebuild fires on every catalog removal — including the routine
  // placeholder-supersede when a demand-subscribed topic's first sample
  // arrives — so it must not cost the user their expand/scroll state.
  const QStringList expanded = tree_view->expandedGroupPaths();
  QScrollBar* scroll_bar = tree_view->verticalScrollBar();
  const int scroll = scroll_bar != nullptr ? scroll_bar->value() : 0;
  tree_view->clearCurves();
  // Note: custom_view is NOT cleared here — custom series are managed separately
  // via addCustomCurve/removeCustomCurve in MainWindow.
  if (catalog == nullptr) {
    return;
  }
  // Exclude keys routed to the Custom Series panel so they never appear in both.
  std::vector<CatalogItem> tree_items;
  for (const auto& item : catalog->items()) {
    if (!custom_keys.contains(item.key)) {
      tree_items.push_back(item);
    }
  }
  addCatalogItems(tree_view, catalog, tracker, tree_items);
  tree_view->restoreExpandedGroupPaths(expanded);
  // Restore scroll AFTER the tree has relaid out. Re-adding rows and expanding
  // groups only schedules a geometry update, so the scrollbar's range is still
  // stale right now — setValue() here would clamp against it (jumping to the
  // bottom when a rebuild shrinks the tree, e.g. a dataset removal). Defer to
  // the next event-loop turn, once the range reflects the rebuilt tree.
  if (scroll_bar != nullptr) {
    QTimer::singleShot(0, scroll_bar, [scroll_bar, scroll]() { scroll_bar->setValue(scroll); });
  }
}

// One themed flat-button row in a PJMenu context menu — the shared
// QPushButton-inside-QWidgetAction pattern both tree context menus use (a
// plain QAction cannot carry the themed leading icon + destructive styling).
QPushButton* addMenuButton(QMenu* menu, const QString& icon, const QString& theme, const QString& text) {
  auto* button = new QPushButton(loadSvg(icon, theme), text, menu);
  button->setFlat(true);
  auto* action = new QWidgetAction(menu);
  action->setDefaultWidget(button);
  menu->addAction(action);
  return button;
}

}  // namespace

CurveListPanel::CurveListPanel(QWidget* parent) : QWidget(parent), ui_(new Ui::CurveListPanel) {
  ui_->setupUi(this);

  tree_view_ = ui_->treeView;
  custom_view_ = ui_->customView;

  // Top tree takes more room than the custom series panel.
  ui_->verticalSplitter->setStretchFactor(0, 5);
  ui_->verticalSplitter->setStretchFactor(1, 1);

  // Datasets header overflow menu — view toggles + Clear All
  // (destructive, so styled red).
  auto* datasets_menu = new QMenu(this);
  datasets_menu->setObjectName(u"PJMenu"_s);

  show_values_check_ = new QCheckBox(tr("Show Values"), datasets_menu);
  auto* show_values_action = new QWidgetAction(datasets_menu);
  show_values_action->setDefaultWidget(show_values_check_);
  datasets_menu->addAction(show_values_action);
  // Seed from QSettings before wiring the signal so the initial state doesn't
  // write the key back (parity with Preserve Topic Name below).
  {
    QSettings show_values_settings;
    show_values_check_->setChecked(show_values_settings.value(QLatin1String(kShowValuesKey), false).toBool());
  }
  connect(show_values_check_, &QCheckBox::toggled, this, &CurveListPanel::onShowValuesToggled);

  preserve_topic_name_check_ = new QCheckBox(tr("Preserve Topic Name"), datasets_menu);
  // Seed the checkbox AND the tree view mode from QSettings before wiring
  // the signal — that way the first rebuildTree() driven by setCatalog()
  // already lays out under the saved mode (no rebuild thrash on startup).
  QSettings settings;
  const bool preserve_topic_name = settings.value(QLatin1String(kPreserveTopicNameKey), true).toBool();
  preserve_topic_name_check_->setChecked(preserve_topic_name);
  tree_view_->setViewMode(
      preserve_topic_name ? CurveTreeView::ViewMode::kShowTopics : CurveTreeView::ViewMode::kHierarchical);
  auto* preserve_topic_name_action = new QWidgetAction(datasets_menu);
  preserve_topic_name_action->setDefaultWidget(preserve_topic_name_check_);
  datasets_menu->addAction(preserve_topic_name_action);
  connect(preserve_topic_name_check_, &QCheckBox::toggled, this, &CurveListPanel::onPreserveTopicNameToggled);

  datasets_menu->addSeparator();

  // QWidgetAction wraps a flat QPushButton so we can colour the text
  // red — QMenu's default item painter doesn't expose a per-action
  // colour the way QSS would for a regular QPushButton.
  clear_all_button_ = new QPushButton(tr("Remove all Datasets"), datasets_menu);
  clear_all_button_->setFlat(true);
  clear_all_button_->setProperty("destructive", true);
  // Padding, text-align, AND the destructive ${purple} colour
  // are handled centrally in stylesheet_*.qss under
  // `QMenu#PJMenu QPushButton[destructive="true"]` — no per-button
  // stylesheet needed here.
  auto* clear_all_action = new QWidgetAction(datasets_menu);
  clear_all_action->setDefaultWidget(clear_all_button_);
  datasets_menu->addAction(clear_all_action);
  connect(clear_all_button_, &QPushButton::clicked, this, [this, datasets_menu]() {
    datasets_menu->hide();
    emit clearAllCurvesRequested();
  });

  connect(ui_->buttonDatasetsMenu, &QToolButton::clicked, this, [this, datasets_menu]() {
    const QPoint anchor = ui_->buttonDatasetsMenu->mapToGlobal(QPoint(0, ui_->buttonDatasetsMenu->height()));
    datasets_menu->popup(anchor);
  });

  applyIcons(currentTheme());

  // Both filters are canonical Search controls inline in their header bands.
  connect(ui_->filterTimeseries, &Search::textChanged, this, &CurveListPanel::onFilterChanged);
  connect(ui_->filterCustom, &Search::textChanged, this, &CurveListPanel::onCustomFilterChanged);

  // Datasets type-filter toggles (plot / 2D / 3D). All start checked (see the
  // .ui), so no initial push is needed — the tree defaults to all kinds shown.
  connect(ui_->buttonFilterPlot, &QToolButton::toggled, this, &CurveListPanel::onTypeFilterToggled);
  connect(ui_->buttonFilterScene2D, &QToolButton::toggled, this, &CurveListPanel::onTypeFilterToggled);
  connect(ui_->buttonFilterScene3D, &QToolButton::toggled, this, &CurveListPanel::onTypeFilterToggled);
  tree_view_->setEmptyFilterMessage(tr("No series match the current search or type filters."));

  // Enter while typing drops focus back to the panel — restores the
  // sibling label + action buttons (via the focus-out branch of
  // eventFilter) without forcing the user to click elsewhere.
  connect(ui_->filterTimeseries, &Search::returnPressed, ui_->filterTimeseries->lineEdit(), &QLineEdit::clearFocus);
  connect(ui_->filterCustom, &Search::returnPressed, ui_->filterCustom->lineEdit(), &QLineEdit::clearFocus);

  // While the Custom Series filter has focus, hide its sibling label + buttons
  // so the input takes the full header width. Restored on focus loss. The
  // Datasets filter lives on its own dedicated row (widgetSearchTimeseries),
  // so it never competes with the label for width and needs no such expansion.
  ui_->filterCustom->lineEdit()->installEventFilter(this);

  // Lock each header band to its natural height so hiding the Custom Series
  // siblings can't shrink the row and shift the line edit's vertical centre.
  // QHBoxLayout vertically centres items, so even a 1-2px drop in the
  // row's preferred height (when the tallest sibling hides) was enough
  // to nudge the line edit upwards on focus.
  const auto fix_band_height = [](QWidget* band) {
    band->layout()->activate();
    band->setFixedHeight(band->layout()->sizeHint().height());
  };
  fix_band_height(ui_->widgetLabelTimeseries);
  fix_band_height(ui_->widgetSearchTimeseries);
  fix_band_height(ui_->widgetLabelCustom);

  connect(ui_->buttonAddCustom, &QToolButton::clicked, this, &CurveListPanel::createCustomSeriesRequested);

  // Custom-series header overflow menu — mirrors the Datasets menu.
  // Delete is destructive so it gets the same red treatment.
  auto* custom_menu = new QMenu(this);
  custom_menu->setObjectName(u"PJMenu"_s);
  delete_custom_button_ = new QPushButton(tr("Delete"), custom_menu);
  delete_custom_button_->setFlat(true);
  delete_custom_button_->setProperty("destructive", true);
  auto* delete_custom_action = new QWidgetAction(custom_menu);
  delete_custom_action->setDefaultWidget(delete_custom_button_);
  custom_menu->addAction(delete_custom_action);
  connect(delete_custom_button_, &QPushButton::clicked, this, [this, custom_menu]() {
    custom_menu->hide();
    // Recursive so selecting a GROUP row (a slashed name like "test/sin/valueasda"
    // nests under "test") deletes every custom curve beneath it. A group row itself
    // carries no catalog key, so the non-recursive selection would emit only the
    // group label, which the delete handler can't resolve — the delete silently
    // no-ops. This mirrors the custom-series drag provider.
    const auto names = custom_view_->selectedCurveNamesRecursive();
    for (const auto& name : names) {
      emit deleteCustomSeriesRequested(name);
    }
  });
  connect(ui_->buttonCustomMenu, &QToolButton::clicked, this, [this, custom_menu]() {
    const QPoint anchor = ui_->buttonCustomMenu->mapToGlobal(QPoint(0, ui_->buttonCustomMenu->height()));
    custom_menu->popup(anchor);
  });

  // Edit (pencil): enabled only when exactly one custom series is selected; opens
  // the Transform Editor pre-populated for an in-place Modify (PJ3 parity).
  connect(ui_->buttonEditCustom, &QToolButton::clicked, this, [this]() {
    const auto names = custom_view_->selectedCurveNames();
    if (names.size() == 1) {
      emit editCustomSeriesRequested(names.front());
    }
  });
  connect(custom_view_, &QTreeWidget::itemSelectionChanged, this, [this]() {
    ui_->buttonEditCustom->setEnabled(custom_view_->selectedCurveNames().size() == 1);
  });

  const bool show_values = show_values_check_->isChecked();
  tree_view_->setValuesColumnHidden(!show_values);
  custom_view_->setValuesColumnHidden(!show_values);

  auto drag_selection_provider = [this]() { return selectedCurveNamesForDrag(); };
  tree_view_->setDragSelectionProvider(drag_selection_provider);
  // custom_view_ uses its own selection only — avoids including tree_view_ selection in drag
  auto custom_drag_provider = [this]() { return custom_view_->selectedCurveNamesRecursive(); };
  custom_view_->setDragSelectionProvider(custom_drag_provider);

  tree_view_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(tree_view_, &QWidget::customContextMenuRequested, this, &CurveListPanel::onTreeContextMenu);

  // Double-click on a scalar placeholder leaf → bounded field preview + one-shot
  // auto-expand. The handler no-ops until setTopicDemandController wires it.
  connect(tree_view_, &CurveTreeView::placeholderPeekRequested, this, &CurveListPanel::onPlaceholderPeekRequested);

  // Not-draggable drag feedback → shell toasts. A pull on an undisplayable topic reuses
  // the row's tooltip as the toast text (one wording for both channels); a row
  // without one falls back to the generic undisplayable wording.
  connect(tree_view_, &CurveTreeView::dragAttemptedOnNotDraggableRow, this, [this](const QString& reason) {
    emit toastRequested(reason.isEmpty() ? undisplayableObjectTooltip(sdk::BuiltinObjectType::kNone) : reason);
  });
  connect(tree_view_, &CurveTreeView::dragPayloadKeysSkipped, this, &CurveListPanel::onDragPayloadKeysSkipped);

  // Forward the tree's cancel request to the controller (MainWindow).
  connect(tree_view_, &CurveTreeView::cancelRequested, this, &CurveListPanel::cancelRequested);

  // 10 Hz throttle for the value column (see refreshValues). The timeout is the
  // trailing edge: if a tracker update arrived during the window, fill once more
  // and re-arm so a continuous playback stream settles into a steady 10 Hz.
  value_throttle_timer_ = new QTimer(this);
  value_throttle_timer_->setSingleShot(true);
  value_throttle_timer_->setInterval(kValueRefreshIntervalMs);
  connect(value_throttle_timer_, &QTimer::timeout, this, [this]() {
    if (!value_refresh_pending_) {
      return;  // no update during the window — let the timer rest
    }
    value_refresh_pending_ = false;
    if (valuesColumnActive()) {
      fillValuesNow();
    }
    value_throttle_timer_->start();  // keep the cadence while updates keep arriving
  });
}

CurveListPanel::~CurveListPanel() {
  delete ui_;
}

void CurveListPanel::setCatalog(CatalogModel* catalog) {
  if (catalog_ == catalog) {
    return;
  }
  if (catalog_) {
    disconnect(catalog_, nullptr, this, nullptr);
  }
  catalog_ = catalog;
  rebuildTree(tree_view_, catalog_, tracker_, custom_keys_);
  if (!catalog_) {
    return;
  }
  connect(catalog_, &CatalogModel::itemsAdded, this, &CurveListPanel::onCatalogItemsAdded);
  connect(catalog_, &CatalogModel::itemsRemoved, this, &CurveListPanel::onCatalogItemsRemoved);
  connect(catalog_, &CatalogModel::cleared, this, &CurveListPanel::onCatalogCleared);
}

void CurveListPanel::setTopicDemandTracker(TopicDemandTracker* tracker) {
  if (tracker_ == tracker) {
    return;
  }
  if (tracker_ != nullptr) {
    disconnect(tracker_, nullptr, this, nullptr);
  }
  tracker_ = tracker;
  if (tracker_ != nullptr) {
    connect(tracker_, &TopicDemandTracker::activeTopicsChanged, this, &CurveListPanel::onActiveTopicsChanged);
    connect(tracker_, &TopicDemandTracker::forcedTopicsChanged, this, [this](DatasetId) { refreshForcedMarks(); });
  }
  refreshUnsubscribedFlags();
  refreshForcedMarks();
}

void CurveListPanel::setTopicDemandController(TopicDemandController* controller) {
  controller_ = controller;
}

void CurveListPanel::onPlaceholderPeekRequested(const QString& catalog_key) {
  if (controller_ == nullptr || catalog_ == nullptr) {
    return;
  }
  const auto item = catalog_->itemDescriptor(catalog_key);
  if (!item.has_value()) {
    return;
  }
  controller_->requestFieldPreview(item->dataset_id, item->topic_name);
  // Arm the one-shot auto-expand at the topic's tree location, derived the same
  // way addCatalogItem files the row (so the promoted fields' group node
  // matches).
  // The unsubscribed flag is irrelevant for path derivation — pass empty sets.
  const CurveTreeView::CurvePath path = treePathFromCatalogItem(*item, {});
  tree_view_->requestExpansionWhenPromoted(CurveTreeView::treePathFromCurvePath(path));
}

void CurveListPanel::onDragPayloadKeysSkipped(const QStringList& catalog_keys) {
  if (catalog_keys.isEmpty()) {
    return;
  }
  if (catalog_keys.size() > 1) {
    emit toastRequested(tr("%1 topics were left out of the drag: no view can display them.").arg(catalog_keys.size()));
    return;
  }
  // Single skipped topic: name it. The catalog resolves the key to its topic
  // name; a key that vanished mid-drag (dataset removal) falls back to the raw
  // key rather than dropping the notice. ToastNotification renders rich text,
  // so the name must be escaped or an HTML-looking channel name would render
  // as markup.
  QString topic_name = catalog_keys.front();
  if (catalog_ != nullptr) {
    if (const auto item = catalog_->itemDescriptor(topic_name); item.has_value()) {
      topic_name = item->topic_name;
    }
  }
  emit toastRequested(tr("\"%1\" was left out of the drag: no view can display it.").arg(topic_name.toHtmlEscaped()));
}

void CurveListPanel::onActiveTopicsChanged(DatasetId /*dataset_id*/, const std::vector<QString>& /*active_topics*/) {
  refreshUnsubscribedFlags();
}

void CurveListPanel::refreshUnsubscribedFlags() {
  if (catalog_ == nullptr) {
    return;
  }
  // buildActiveSets is empty when tracker_ is null, so a detached tracker
  // correctly clears every row's flag here instead of leaving stale dimming.
  const QHash<DatasetId, QSet<QString>> active_sets = buildActiveSets(*catalog_, tracker_);
  QSet<QString> unsubscribed;
  for (const CatalogItem& item : catalog_->items()) {
    if (isTopicUnsubscribed(active_sets, item.dataset_id, item.topic_name)) {
      unsubscribed.insert(item.key);
    }
  }
  tree_view_->setUnsubscribedKeys(unsubscribed);
}

void CurveListPanel::refreshForcedMarks() {
  if (catalog_ == nullptr || tracker_ == nullptr || tree_view_ == nullptr) {
    return;
  }
  QSet<QString> paths;
  for (const auto& [dataset_id, dataset_name] : catalog_->datasets()) {
    for (const QString& topic : tracker_->forcedTopics(dataset_id)) {
      paths.insert(
          CurveTreeView::treePathFromCurvePath(
              CurveTreeView::CurvePath{.key = {}, .dataset = dataset_name, .topic = topic, .field = {}}));
    }
  }
  tree_view_->setForcedTopicPaths(paths);
}

void CurveListPanel::refreshValues(double tracker_time) {
  last_tracker_time_ = tracker_time;  // remembered for non-tracker refreshes (Show Values toggle)
  if (!valuesColumnActive()) {
    return;  // nothing shown — skip even the throttle bookkeeping
  }
  if (value_throttle_timer_->isActive()) {
    value_refresh_pending_ = true;  // coalesce; the trailing fill will use last_tracker_time_
    return;
  }
  fillValuesNow();                 // leading edge: first update lands immediately
  value_throttle_timer_->start();  // then at most one fill per window
}

bool CurveListPanel::valuesColumnActive() const {
  return catalog_ != nullptr && !(tree_view_->valuesColumnHidden() && custom_view_->valuesColumnHidden());
}

void CurveListPanel::fillValuesNow() {
  QSettings settings;
  const int precision = settings.value(u"Preferences::precision"_s, 3).toInt();
  const double tracker_time = last_tracker_time_;
  auto provider = [this, tracker_time, precision](const QString& key) -> QString {
    // String fields show their text value at the cursor (zero-order hold), "-"
    // when no sample exists yet — read on their own seam since they aren't numeric.
    // MUST be checked before the numeric path: isScalarKey is true for strings
    // too, so reversing this order would render every string field as "-".
    if (catalog_->isStringKey(key)) {
      return catalog_->stringValueAt(key, tracker_time).value_or(u"-"_s);
    }
    const std::optional<double> value = catalog_->scalarValueAt(key, tracker_time);
    if (value.has_value()) {
      return formatScalarForColumn(*value, precision);
    }
    // No sample at or before the cursor: show "-" for a numeric scalar (PJ3
    // parity) and leave non-scalar rows (object topics) blank.
    return catalog_->isScalarKey(key) ? u"-"_s : QString();
  };
  tree_view_->refreshVisibleValues(provider);
  custom_view_->refreshVisibleValues(provider);
}

QDomElement CurveListPanel::saveListState(QDomDocument& doc) const {
  QDomElement element = doc.createElement(u"curve_list_state"_s);

  if (preserve_topic_name_check_ != nullptr) {
    element.setAttribute(u"show_topics"_s, preserve_topic_name_check_->isChecked() ? u"true"_s : u"false"_s);
  }
  if (show_values_check_ != nullptr) {
    element.setAttribute(u"show_values"_s, show_values_check_->isChecked() ? u"true"_s : u"false"_s);
  }
  element.setAttribute(QStringLiteral("datasets_filter"), ui_->filterTimeseries->text());
  element.setAttribute(QStringLiteral("custom_filter"), ui_->filterCustom->text());
  return element;
}

void CurveListPanel::restoreListState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "curve_list_state"_L1) {
    return;
  }

  // Bracket the toggles with applying_state_ so onPreserveTopicNameToggled
  // doesn't write its QSettings key. setViewMode + rebuildTree still run.
  // QScopedValueRollback gives exception-safety: a throw inside any slot
  // restores the flag instead of leaving it stuck-true (which would
  // silently disable QSettings writes from later user-initiated toggles).
  // Matches MainWindow.cpp's pattern at the xmlLoadState path.
  {
    QScopedValueRollback guard(applying_state_, true);

    if (element.hasAttribute(u"show_topics"_s) && preserve_topic_name_check_ != nullptr) {
      const bool wanted = element.attribute(u"show_topics"_s) == "true"_L1;
      if (preserve_topic_name_check_->isChecked() != wanted) {
        preserve_topic_name_check_->setChecked(wanted);  // emits toggled -> slot runs (rebuilds tree)
      }
    }

    if (element.hasAttribute(u"show_values"_s) && show_values_check_ != nullptr) {
      const bool wanted = element.attribute(u"show_values"_s) == "true"_L1;
      if (show_values_check_->isChecked() != wanted) {
        show_values_check_->setChecked(wanted);  // emits toggled -> slot runs (no QSettings write)
      }
    }
  }

  // Filter texts: setText emits textChanged, which the connected slots
  // forward to tree_view_->applyFilter — that's exactly what we want.
  // Do NOT block signals here.
  if (element.hasAttribute(QStringLiteral("datasets_filter"))) {
    ui_->filterTimeseries->setText(element.attribute(QStringLiteral("datasets_filter")));
  }
  if (element.hasAttribute(QStringLiteral("custom_filter"))) {
    ui_->filterCustom->setText(element.attribute(QStringLiteral("custom_filter")));
  }
}

void CurveListPanel::onFilterChanged(const QString& text) {
  tree_view_->applyFilter(text);
}

void CurveListPanel::onCustomFilterChanged(const QString& text) {
  custom_view_->applyFilter(text);
}

void CurveListPanel::onTypeFilterToggled() {
  const bool show_plot = ui_->buttonFilterPlot->isChecked();
  const bool show_scene2d = ui_->buttonFilterScene2D->isChecked();
  const bool show_scene3d = ui_->buttonFilterScene3D->isChecked();
  tree_view_->setVisibleCurveKinds(show_plot, show_scene2d, show_scene3d);
}

void CurveListPanel::onShowValuesToggled(bool show) {
  if (!applying_state_) {
    QSettings settings;
    settings.setValue(QLatin1String(kShowValuesKey), show);
  }
  tree_view_->setValuesColumnHidden(!show);
  custom_view_->setValuesColumnHidden(!show);
  if (show) {
    // Fill the freshly-shown column at the current cursor instead of waiting for
    // the next playback tick.
    refreshValues(last_tracker_time_);
  }
}

void CurveListPanel::onPreserveTopicNameToggled(bool checked) {
  if (!applying_state_) {
    QSettings settings;
    settings.setValue(QLatin1String(kPreserveTopicNameKey), checked);
  }
  tree_view_->setViewMode(checked ? CurveTreeView::ViewMode::kShowTopics : CurveTreeView::ViewMode::kHierarchical);
  rebuildTree(tree_view_, catalog_, tracker_, custom_keys_);
  tree_view_->applyFilter(ui_->filterTimeseries->text());
}

void CurveListPanel::onTrashClicked() {
  const auto selected = tree_view_->selectedCatalogKeysRecursive();
  const std::size_t total = catalog_ != nullptr ? catalog_->items().size() : 0;
  const bool covers_all = selected.empty() || (total > 0 && selected.size() >= total);
  emit trashRequested(QStringList(selected.begin(), selected.end()), covers_all);
}

void CurveListPanel::onTreeContextMenu(const QPoint& pos) {
  if (catalog_ == nullptr) {
    return;
  }
  // Dataset nodes (top-level groups) get the Merge/Remove menu; any other row
  // that resolves to a catalog item gets the force-streaming toggle.
  const auto is_dataset_node = [](QTreeWidgetItem* node) {
    return node != nullptr && node->parent() == nullptr && node->childCount() > 0;
  };
  QTreeWidgetItem* clicked = tree_view_->itemAt(pos);
  if (!is_dataset_node(clicked)) {
    showTopicContextMenu(clicked, pos);
    return;
  }

  QHash<QString, DatasetId> id_by_name;
  for (const auto& [id, name] : catalog_->datasets()) {
    id_by_name.insert(name, id);
  }
  QList<DatasetId> dataset_ids;
  const auto add_dataset = [&](QTreeWidgetItem* node) {
    if (!is_dataset_node(node)) {
      return;
    }
    const auto found = id_by_name.constFind(node->text(0));
    if (found != id_by_name.constEnd() && !dataset_ids.contains(found.value())) {
      dataset_ids.push_back(found.value());
    }
  };
  // Always include the right-clicked dataset; if it is part of a multi-selection,
  // include every other selected dataset too. Right-clicking a dataset outside the
  // selection targets just that one (standard list behavior). Does not rely on the
  // clicked row being selected, so the menu works even on a fresh right-click.
  add_dataset(clicked);
  if (clicked->isSelected()) {
    for (QTreeWidgetItem* item : tree_view_->selectedItems()) {
      add_dataset(item);
    }
  }
  if (dataset_ids.isEmpty()) {
    return;  // clicked name no longer resolves to a catalog dataset
  }

  // Build the menu from flat QPushButtons wrapped in QWidgetActions — the same
  // pattern as the "Remove all Datasets" item in the datasets popup — so each item
  // carries a themed leading icon and the destructive ones paint in the shared
  // ${purple} via the central `QMenu#PJMenu QPushButton[destructive="true"]` rule.
  QMenu menu(this);
  menu.setObjectName(u"PJMenu"_s);
  const QString theme = currentTheme();
  const auto add_item = [&](const QString& icon, const QString& text, bool destructive, bool enabled) {
    QPushButton* button = addMenuButton(&menu, icon, theme, text);
    button->setEnabled(enabled);
    if (destructive) {
      button->setProperty("destructive", true);
    }
    return button;
  };

  // Merge first (needs ≥2 datasets), then Reload/Replace (exactly one dataset,
  // and only one loaded from a file — a streaming/test dataset has no source to
  // re-read), then Remove (destructive → purple). The menu targets a dataset
  // selection, so the labels drop the noun.
  const bool single_file_backed = dataset_ids.size() == 1 && source_path_resolver_ != nullptr &&
                                  !source_path_resolver_(dataset_ids.front()).isEmpty();
  QPushButton* merge_button = add_item(u":/resources/svg/merge.svg"_s, tr("Merge"), false, dataset_ids.size() >= 2);
  QPushButton* reload_button = add_item(u":/resources/svg/replay.svg"_s, tr("Reload"), false, single_file_backed);
  QPushButton* replace_button =
      add_item(u":/resources/svg/compare_arrows.svg"_s, tr("Replace"), false, single_file_backed);
  QPushButton* remove_button =
      add_item(u":/resources/svg/trash.svg"_s, tr("Remove"), /*destructive=*/true, /*enabled=*/true);

  // QWidgetAction buttons don't dismiss the menu on click — close it ourselves and
  // record the choice (exec blocks, so capturing by reference is safe).
  bool do_merge = false;
  bool do_reload = false;
  bool do_replace = false;
  bool do_remove = false;
  connect(merge_button, &QPushButton::clicked, &menu, [&]() {
    do_merge = true;
    menu.close();
  });
  connect(reload_button, &QPushButton::clicked, &menu, [&]() {
    do_reload = true;
    menu.close();
  });
  connect(replace_button, &QPushButton::clicked, &menu, [&]() {
    do_replace = true;
    menu.close();
  });
  connect(remove_button, &QPushButton::clicked, &menu, [&]() {
    do_remove = true;
    menu.close();
  });

  menu.exec(tree_view_->viewport()->mapToGlobal(pos));
  if (do_merge) {
    emit mergeDatasetsRequested(dataset_ids);
  } else if (do_reload) {
    emit reloadDatasetRequested(dataset_ids.front());
  } else if (do_replace) {
    emit replaceDatasetRequested(dataset_ids.front());
  } else if (do_remove) {
    emit removeDatasetsRequested(dataset_ids);
  }
}

void CurveListPanel::showTopicContextMenu(QTreeWidgetItem* clicked, const QPoint& pos) {
  if (tracker_ == nullptr || catalog_ == nullptr) {
    return;
  }
  // A promoted scalar topic renders as a GROUP row whose keys live on its field
  // leaves — resolve through the whole subtree and act only when every keyed
  // row below the click belongs to ONE topic (a folder spanning several topics
  // would make "force" ambiguous).
  std::optional<CatalogItem> item;
  for (const QString& key : CurveTreeView::catalogKeysUnder(clicked)) {
    const auto resolved = catalog_->itemDescriptor(key);
    if (!resolved.has_value()) {
      continue;
    }
    if (item.has_value() && (item->dataset_id != resolved->dataset_id || item->topic_name != resolved->topic_name)) {
      return;  // subtree spans multiple topics
    }
    item = resolved;
  }
  if (!item.has_value() || !catalog_->isPerTopicPauseCapable(item->dataset_id)) {
    return;  // no topic here, or a file dataset / non-demand source
  }

  // Forcing is a tier on top of display references (see TopicDemandTracker
  // ::setTopicForced): "Stop forced streaming" only drops the forced hold — it
  // can never pause a topic something still displays.
  const bool forced = tracker_->isTopicForced(item->dataset_id, item->topic_name);
  QMenu menu(this);
  menu.setObjectName(u"PJMenu"_s);
  QPushButton* button = addMenuButton(
      &menu, u":/resources/svg/cast.svg"_s, currentTheme(),
      forced ? tr("Stop forced streaming") : tr("Force topic streaming"));

  bool toggle = false;
  connect(button, &QPushButton::clicked, &menu, [&]() {
    toggle = true;
    menu.close();
  });
  menu.exec(tree_view_->viewport()->mapToGlobal(pos));
  if (toggle) {
    tracker_->setTopicForced(item->dataset_id, item->topic_name, !forced);
  }
}

void CurveListPanel::onCatalogItemsAdded(const std::vector<CatalogItem>& items) {
  // Custom-series keys are added flat via addCustomCurve from MainWindow; keep
  // them out of the main tree.
  std::vector<CatalogItem> tree_items;
  for (const auto& item : items) {
    if (!custom_keys_.contains(item.key)) {
      tree_items.push_back(item);
    }
  }
  addCatalogItems(tree_view_, catalog_, tracker_, tree_items);
  // Fill the Value cells of the just-added rows at the current cursor.
  refreshValues(last_tracker_time_);
}

void CurveListPanel::addCustomCurve(const QString& catalog_key, const QString& display_name) {
  if (custom_view_ == nullptr) {
    return;
  }
  // Idempotent BY NAME: a plugin toolbox re-announces ALL its transforms on every
  // data change. A plain Create re-announces the same key (skip it). A Modify mints
  // a NEW output topic id → new key for the SAME name; drop the stale entry first so
  // we replace it in place instead of appending a duplicate row.
  const auto existing = custom_name_to_key_.constFind(display_name);
  if (existing != custom_name_to_key_.constEnd()) {
    if (existing.value() == catalog_key) {
      return;  // same series, same key — nothing to do
    }
    removeCustomCurve(existing.value());  // stale key from a prior version — remove it
  }
  if (custom_keys_.contains(catalog_key)) {
    return;
  }
  custom_keys_.insert(catalog_key);
  custom_name_to_key_.insert(display_name, catalog_key);
  CurveTreeView::CurvePath path;
  path.key = catalog_key;
  path.dataset = display_name;
  path.topic = QString{};
  path.field = QString{};
  custom_view_->addCatalogItem(path);
  rebuildTree(tree_view_, catalog_, tracker_, custom_keys_);
}

void CurveListPanel::removeCustomCurve(const QString& catalog_key) {
  if (custom_view_ == nullptr) {
    return;
  }
  custom_keys_.remove(catalog_key);
  custom_name_to_key_.removeIf([&](const auto& it) { return it.value() == catalog_key; });
  custom_view_->clearCurves();
  if (catalog_ != nullptr) {
    for (const auto& item : catalog_->items()) {
      if (custom_keys_.contains(item.key)) {
        CurveTreeView::CurvePath path;
        path.key = item.key;
        path.dataset = item.topic_name;
        custom_view_->addCatalogItem(path);
      }
    }
  }
}

void CurveListPanel::removeCustomCurveByName(const QString& display_name) {
  const auto it = custom_name_to_key_.constFind(display_name);
  if (it == custom_name_to_key_.constEnd()) {
    return;
  }
  removeCustomCurve(it.value());  // also erases the name->key entry
}

void CurveListPanel::onCatalogItemsRemoved(const QStringList& /*keys*/) {
  // One rebuild per batch (a dataset / multi-key trash is a single itemsRemoved).
  // TODO: incremental CurveTreeView::removeCurve(name); linear rebuild wipes
  // scroll/expansion/selection.
  rebuildTree(tree_view_, catalog_, tracker_, custom_keys_);
}

void CurveListPanel::onCatalogCleared() {
  tree_view_->clearCurves();
}

void CurveListPanel::onStylesheetChanged(QString theme) {
  applyIcons(theme);
}

void CurveListPanel::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  chrome_metrics_ = metrics;
  applyIcons(currentTheme());
}

bool CurveListPanel::eventFilter(QObject* watched, QEvent* event) {
  const QEvent::Type type = event->type();
  if (type == QEvent::FocusIn || type == QEvent::FocusOut) {
    const bool focused = (type == QEvent::FocusIn);
    if (watched == ui_->filterCustom->lineEdit()) {
      ui_->labelCustom->setVisible(!focused);
      ui_->buttonAddCustom->setVisible(!focused);
      ui_->buttonCustomMenu->setVisible(!focused);
    }
  }
  return QWidget::eventFilter(watched, event);
}

void CurveListPanel::applyIcons(QString theme) {
  if (tree_view_ != nullptr) {
    tree_view_->refreshIcons(theme);
  }
  ui_->buttonDatasetsMenu->setIcon(loadSvg(":/resources/svg/more_vert.svg", theme));
  ui_->buttonCustomMenu->setIcon(loadSvg(":/resources/svg/more_vert.svg", theme));
  ui_->buttonAddCustom->setIcon(loadSvg(":/resources/svg/add.svg", theme));
  ui_->buttonEditCustom->setIcon(loadSvg(":/resources/svg/pencil-edit.svg", theme));
  if (clear_all_button_ != nullptr) {
    clear_all_button_->setIcon(loadSvg(":/resources/svg/trash.svg", theme));
  }
  if (delete_custom_button_ != nullptr) {
    delete_custom_button_->setIcon(loadSvg(":/resources/svg/delete_forever.svg", theme));
  }
  // Type-filter toggles reuse the exact placeholder-widget glyphs (plot / 2D /
  // 3D). The On (checked = shown) state is the plain themed glyph; the Off
  // (unchecked = hidden) state is a dimmed, slashed variant so an omitted kind
  // reads as struck-through. The checked-background fill is suppressed for these
  // buttons in QSS, so the default all-shown state stays visually calm.
  // Match the app's visibility_off eye-slash ink exactly (loadSvg keys "light"
  // -> #3D3D3D, else #E0E0E0), so the two "hidden" affordances read the same.
  const QColor slash_ink = isLightTheme(theme) ? QColor(0x3D, 0x3D, 0x3D) : QColor(0xE0, 0xE0, 0xE0);
  const QColor slash_halo = palette().color(QPalette::Window);
  const auto make_toggle_icon = [&](const QString& path) {
    const QPixmap glyph = loadSvg(path, theme);
    QIcon icon;
    icon.addPixmap(glyph, QIcon::Normal, QIcon::On);
    icon.addPixmap(makeOmittedGlyph(glyph, slash_ink, slash_halo), QIcon::Normal, QIcon::Off);
    return icon;
  };
  ui_->buttonFilterPlot->setIcon(make_toggle_icon(":/resources/svg/line_axis.svg"));
  ui_->buttonFilterScene2D->setIcon(make_toggle_icon(":/resources/svg/image.svg"));
  ui_->buttonFilterScene3D->setIcon(make_toggle_icon(":/resources/svg/cube.svg"));
  // The Search filters own their glyph + sizing; keep them in lock-step with
  // the global icon metrics (height + glyph track the chrome).
  ui_->filterTimeseries->setChromeMetrics(chrome_metrics_);
  ui_->filterCustom->setChromeMetrics(chrome_metrics_);

  // Resize chrome buttons in lock-step with the global icon metrics.
  // clear_all_button_ and delete_custom_button_ are inline-action menu
  // items (full-width inside a popup), not square chrome — skip them.
  const QSize icon_sz(chrome_metrics_.icon_size, chrome_metrics_.icon_size);
  const int button_extent = chrome_metrics_.icon_size + chrome_metrics_.icon_padding;
  const int band_extent = chrome_metrics_.bandHeight();
  const std::array<QToolButton*, 6> chrome_buttons{ui_->buttonDatasetsMenu,  ui_->buttonCustomMenu,
                                                   ui_->buttonAddCustom,     ui_->buttonFilterPlot,
                                                   ui_->buttonFilterScene2D, ui_->buttonFilterScene3D};
  for (QToolButton* btn : chrome_buttons) {
    btn->setMinimumSize(button_extent, button_extent);
    btn->setMaximumSize(button_extent, button_extent);
    btn->setIconSize(icon_sz);
  }
  // Bands grow to band_extent so the contentsMargins applied to their
  // inner layouts (below) are absorbed by the band instead of squeezing
  // the chrome inside.
  ui_->widgetLabelTimeseries->setFixedHeight(band_extent);
  ui_->widgetSearchTimeseries->setFixedHeight(band_extent);
  ui_->widgetLabelCustom->setFixedHeight(band_extent);
  const QMargins margins(
      chrome_metrics_.layout_padding, chrome_metrics_.layout_padding, chrome_metrics_.layout_padding,
      chrome_metrics_.layout_padding);
  // Title bands lead via their label's own canonical padding-left (Tight), so
  // their layout adds no left inset — otherwise the two stack into a doubled
  // leading that no longer matches the SectionHeaderBand/Timeline reference.
  // Search bands keep the left inset: their field carries no internal padding.
  const QMargins title_band_margins(
      0, chrome_metrics_.layout_padding, chrome_metrics_.layout_padding, chrome_metrics_.layout_padding);
  if (auto* layout = ui_->timeseriesHeaderLayout) {
    layout->setContentsMargins(title_band_margins);
    layout->setSpacing(chrome_metrics_.layout_spacing);
  }
  if (auto* layout = ui_->searchTimeseriesLayout) {
    layout->setContentsMargins(margins);
    layout->setSpacing(chrome_metrics_.layout_spacing);
  }
  if (auto* layout = ui_->customHeaderLayout) {
    layout->setContentsMargins(title_band_margins);
    layout->setSpacing(chrome_metrics_.layout_spacing);
  }
  // Per-row padding on the Datasets / Custom Series trees. QTreeView
  // has no setSpacing() the way QListWidget does — instead, push a
  // per-instance stylesheet that pads ::item by layout_spacing on top
  // and bottom. Setting an empty stylesheet at zero spacing clears the
  // rule (otherwise the previous value would linger).
  const QString row_padding =
      chrome_metrics_.layout_spacing > 0
          ? u"QTreeView::item { padding-top: %1px; padding-bottom: %1px; }"_s.arg(chrome_metrics_.layout_spacing)
          : QString();
  if (tree_view_ != nullptr) {
    tree_view_->setStyleSheet(row_padding);
  }
  if (custom_view_ != nullptr) {
    custom_view_->setStyleSheet(row_padding);
  }
}

std::vector<QString> CurveListPanel::selectedCurveNamesForDrag() const {
  std::vector<QString> names = tree_view_->selectedCurveNamesRecursive();
  std::vector<QString> custom_names = custom_view_->selectedCurveNamesRecursive();
  names.insert(names.end(), custom_names.begin(), custom_names.end());
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

void CurveListPanel::setDatasetProgress(const QHash<quint64, CurveTreeView::DatasetProgress>& by_row_key) {
  if (tree_view_ != nullptr) {
    tree_view_->setDatasetProgress(by_row_key);
  }
}

void CurveListPanel::setDatasetRowKey(const QString& dataset_tree_path, quint64 row_key) {
  if (tree_view_ != nullptr) {
    tree_view_->setDatasetRowKey(dataset_tree_path, row_key);
  }
}

}  // namespace PJ
