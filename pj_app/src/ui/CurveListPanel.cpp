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
#include <QLocale>
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
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/TopicDemandTracker.h"
#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/OwnedMenu.h"
#include "pj_widgets/Search.h"
#include "pj_widgets/SvgUtil.h"
#include "scene_object_classification.h"
#include "ui_CurveListPanel.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

constexpr auto kPreserveTopicNameKey = "CurveListPanel/show_topics";
/// A series row's key in the custom view is `(series engine topic << kSeriesRowKeyColumnBits) | column`.
constexpr int kSeriesRowKeyColumnBits = 16;
constexpr auto kShowValuesKey = "CurveListPanel/show_values";

// The "omitted" variant of a type-filter glyph: dimmed, with a haloed slash in the direction, ink and weight of the
// visibility_off eye, so a hidden kind reads as struck through like a hidden curve.
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
  const qreal ink_width = qMax<qreal>(1.0, width * 0.05);
  const qreal halo_width = ink_width + qMax<qreal>(1.0, width * 0.045);  // background gap around the slash
  painter.setPen(QPen(halo, halo_width, Qt::SolidLine, Qt::RoundCap));
  painter.drawLine(slash);
  painter.setPen(QPen(ink, ink_width, Qt::SolidLine, Qt::RoundCap));
  painter.drawLine(slash);
  return out;
}

// The value column only needs to track the eye: this keeps the per-tick scalar reads off the playback path.
constexpr int kValueRefreshIntervalMs = 100;

// Once per pass, not tracker->activeTopics per item, which copies and sorts the vector for every catalog item. A
// dataset absent from the map is not pause-capable, so its topics are never unsubscribed. Empty without a tracker.
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

// A topic of a pause-capable dataset that nothing references.
bool isTopicUnsubscribed(
    const QHash<DatasetId, QSet<QString>>& active_sets, DatasetId dataset_id, const QString& topic_name) {
  const auto it = active_sets.constFind(dataset_id);
  return it != active_sets.constEnd() && !it->contains(topic_name);
}

// Shown as the row's tooltip, so a drag that never starts or a refused drop does not read as a bug.
QString undisplayableObjectTooltip(sdk::BuiltinObjectType type) {
  switch (type) {
    case sdk::BuiltinObjectType::kCameraInfo:
      return QObject::tr(
          "Camera calibration topic, used automatically by 2D and 3D views. "
          "It cannot be displayed on its own, but a plugin panel can read it.");
    case sdk::BuiltinObjectType::kOccupancyGridUpdate:
      return QObject::tr(
          "Incremental map update, applied automatically to its occupancy grid. "
          "It cannot be displayed on its own, but a Custom Widget panel can read it.");
    case sdk::BuiltinObjectType::kMeshMapUpdate:
      return QObject::tr(
          "Incremental mesh map update, applied automatically to its mesh map. "
          "It cannot be displayed on its own, but a Custom Widget panel can read it.");
    default:
      return isCustomWidgetObjectType(type) ? QObject::tr("Drop it on a Custom Widget panel.")
                                            : QObject::tr("No view can display this topic type.");
  }
}

// Schema-style spelling of a scalar column's declared primitive, for the tooltip.
QString primitiveTypeName(PrimitiveType type) {
  switch (type) {
    case PrimitiveType::kFloat32:
      return u"float32"_s;
    case PrimitiveType::kFloat64:
      return u"float64"_s;
    case PrimitiveType::kInt8:
      return u"int8"_s;
    case PrimitiveType::kInt16:
      return u"int16"_s;
    case PrimitiveType::kInt32:
      return u"int32"_s;
    case PrimitiveType::kInt64:
      return u"int64"_s;
    case PrimitiveType::kUint8:
      return u"uint8"_s;
    case PrimitiveType::kUint16:
      return u"uint16"_s;
    case PrimitiveType::kUint32:
      return u"uint32"_s;
    case PrimitiveType::kUint64:
      return u"uint64"_s;
    case PrimitiveType::kBool:
      return u"bool"_s;
    case PrimitiveType::kString:
      return u"string"_s;
    case PrimitiveType::kUnspecified:
      break;
  }
  return {};
}

// "~1,234" while the dataset is still ingesting: the physical row count is a
// moving number there and may include rows below the streaming retention floor.
QString formatCount(const TopicStats& stats) {
  const QString digits = QLocale().toString(static_cast<qulonglong>(stats.count));
  return stats.live ? u"~"_s + digits : digits;
}

CurveTreeView::CurvePath treePathFromCatalogItem(
    const CatalogItem& item, const QHash<DatasetId, QSet<QString>>& active_sets) {
  const auto* scalar = asScalarField(item);
  const auto* object_topic = asObjectTopic(item);
  const auto* advertised = asAdvertisedTopic(item);
  // A kNone placeholder is scalar-shaped: a flat, draggable leaf at the topic (no fields yet). Any other
  // classification is object-shaped, a terminal node like a real ObjectTopicPayload.
  const bool placeholder_is_object =
      advertised != nullptr && advertised->classification != sdk::BuiltinObjectType::kNone;
  const auto object_type = object_topic != nullptr ? object_topic->object_type
                           : placeholder_is_object ? advertised->classification
                                                   : sdk::BuiltinObjectType::kNone;
  // An object topic no dock family claims says so in its tooltip.
  const bool object_undisplayable =
      (object_topic != nullptr || placeholder_is_object) && !isDroppableObjectType(object_type);
  return CurveTreeView::CurvePath{
      .key = item.key,
      .dataset = item.dataset_name,
      .topic = item.topic_name,
      .field = scalar != nullptr ? scalar->field_name : QString{},
      .selectable = scalar != nullptr || (advertised != nullptr && !placeholder_is_object),
      // String fields drag onto the State Transitions strip; a plot refuses them (curveDescriptor).
      .draggable = !object_undisplayable || isPanelOnlyObjectType(object_type) || isCustomWidgetObjectType(object_type),
      .tooltip = object_undisplayable ? undisplayableObjectTooltip(object_type) : QString{},
      .is_image_topic = isImageFamilyObjectType(object_type),
      .is_3d_object_topic = is3dSceneObjectType(object_type),
      .object_icon = objectTypeBadgeIcon(object_type),
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
    const QHash<QString, CurveListPanel::CustomTopicRow>& custom_rows) {
  // A rebuild runs on every catalog removal, including a placeholder superseded by its first sample, so it keeps the
  // expansion and scroll state.
  const QStringList expanded = tree_view->expandedGroupPaths();
  QScrollBar* scroll_bar = tree_view->verticalScrollBar();
  const int scroll = scroll_bar != nullptr ? scroll_bar->value() : 0;
  tree_view->clearCurves();
  // The custom view is not touched: its rows belong to setCustomTopics/refillCustomView.
  if (catalog == nullptr) {
    return;
  }
  // Exclude keys routed to the Custom Topics panel so they never appear in both, and
  // preview topics, which are not user-facing at all.
  const QSet<QString> preview_keys = catalog->ephemeralPreviewKeys();
  std::vector<CatalogItem> tree_items;
  for (const auto& item : catalog->items()) {
    if (!custom_rows.contains(item.key) && !preview_keys.contains(item.key)) {
      tree_items.push_back(item);
    }
  }
  addCatalogItems(tree_view, catalog, tracker, tree_items);
  tree_view->restoreExpandedGroupPaths(expanded);
  // On the next event-loop turn: the scrollbar's range is still the old tree's until relayout, and setValue() now
  // would clamp against it (jumping to the bottom when a removal shrinks the tree).
  if (scroll_bar != nullptr) {
    QTimer::singleShot(0, scroll_bar, [scroll_bar, scroll]() { scroll_bar->setValue(scroll); });
  }
}

// A QPushButton inside a QWidgetAction: a plain QAction cannot carry the themed leading icon and destructive styling.
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

  ui_->verticalSplitter->setStretchFactor(0, 5);
  ui_->verticalSplitter->setStretchFactor(1, 1);

  auto* datasets_menu = new QMenu(this);
  datasets_menu->setObjectName(u"PJMenu"_s);

  show_values_check_ = new QCheckBox(tr("Show Values"), datasets_menu);
  auto* show_values_action = new QWidgetAction(datasets_menu);
  show_values_action->setDefaultWidget(show_values_check_);
  datasets_menu->addAction(show_values_action);
  // Seeded before the signal is wired, so the initial state does not write the key back.
  {
    QSettings show_values_settings;
    show_values_check_->setChecked(show_values_settings.value(QLatin1String(kShowValuesKey), false).toBool());
  }
  connect(show_values_check_, &QCheckBox::toggled, this, &CurveListPanel::onShowValuesToggled);

  preserve_topic_name_check_ = new QCheckBox(tr("Preserve Topic Name"), datasets_menu);
  // The view mode too, so setCatalog's first rebuild already uses the saved mode.
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

  // A QPushButton, because QMenu's item painter has no per-action colour. The QSS rule
  // `QMenu#PJMenu QPushButton[destructive="true"]` styles it.
  clear_all_button_ = new QPushButton(tr("Remove all Datasets"), datasets_menu);
  clear_all_button_->setFlat(true);
  clear_all_button_->setProperty("destructive", true);
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

  connect(ui_->filterTimeseries, &Search::textChanged, this, &CurveListPanel::onFilterChanged);
  connect(ui_->filterCustom, &Search::textChanged, this, &CurveListPanel::onCustomFilterChanged);

  // All start checked in the .ui, matching the tree's default, so no initial push.
  connect(ui_->buttonFilterPlot, &QToolButton::toggled, this, &CurveListPanel::onTypeFilterToggled);
  connect(ui_->buttonFilterScene2D, &QToolButton::toggled, this, &CurveListPanel::onTypeFilterToggled);
  connect(ui_->buttonFilterScene3D, &QToolButton::toggled, this, &CurveListPanel::onTypeFilterToggled);
  tree_view_->setEmptyFilterMessage(tr("No series match the current search or type filters."));

  // Enter drops the focus, which restores the hidden header buttons (eventFilter).
  connect(ui_->filterTimeseries, &Search::returnPressed, ui_->filterTimeseries->lineEdit(), &QLineEdit::clearFocus);
  connect(ui_->filterCustom, &Search::returnPressed, ui_->filterCustom->lineEdit(), &QLineEdit::clearFocus);

  // Only the Custom Topics filter shares its band; the Datasets filter has its own row.
  ui_->filterCustom->lineEdit()->installEventFilter(this);

  // Fixed heights: hiding the tallest sibling would shrink the band, and QHBoxLayout's vertical centring would move
  // the line edit on focus.
  const auto fix_band_height = [](QWidget* band) {
    band->layout()->activate();
    band->setFixedHeight(band->layout()->sizeHint().height());
  };
  fix_band_height(ui_->widgetLabelTimeseries);
  fix_band_height(ui_->widgetSearchTimeseries);
  fix_band_height(ui_->widgetLabelCustom);

  connect(ui_->buttonAddCustom, &QToolButton::clicked, this, &CurveListPanel::createCustomTopicRequested);
  ui_->buttonAddCustom->setVisible(false);  // until setEditorPluginId names an editor to open

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
    // Recursive: a group row carries no catalog key, so only the walk deletes what is beneath it. By catalog key,
    // not curve name, so object-topic rows (no curve name) are reached too.
    const std::vector<QString> keys = custom_view_->selectedCatalogKeysRecursive();
    emit deleteCustomTopicsRequested(QStringList(keys.begin(), keys.end()));
  });
  connect(ui_->buttonCustomMenu, &QToolButton::clicked, this, [this, custom_menu]() {
    const QPoint anchor = ui_->buttonCustomMenu->mapToGlobal(QPoint(0, ui_->buttonCustomMenu->height()));
    custom_menu->popup(anchor);
  });

  connect(ui_->buttonEditCustom, &QToolButton::clicked, this, [this]() {
    // Enabled for one selected row the editor owns, or any one row when the editor shows views
    // (updateCustomEditButton).
    if (const QStringList keys = selectedCustomRowKeys(); keys.size() == 1) {
      emit editCustomTopicRequested(keys.front());
    }
  });
  connect(custom_view_, &QTreeWidget::itemSelectionChanged, this, &CurveListPanel::updateCustomEditButton);
  // The custom view holds object rows too: an undisplayable pull or a partial drag says why, as in the Datasets tree.
  connect(custom_view_, &CurveTreeView::dragAttemptedOnNotDraggableRow, this, [this](const QString& reason) {
    emit toastRequested(reason.isEmpty() ? undisplayableObjectTooltip(sdk::BuiltinObjectType::kNone) : reason);
  });
  connect(custom_view_, &CurveTreeView::dragPayloadKeysSkipped, this, &CurveListPanel::onDragPayloadKeysSkipped);

  const bool show_values = show_values_check_->isChecked();
  tree_view_->setValuesColumnHidden(!show_values);
  custom_view_->setValuesColumnHidden(!show_values);

  auto drag_selection_provider = [this]() { return selectedCurveNamesForDrag(); };
  tree_view_->setDragSelectionProvider(drag_selection_provider);
  // Its own selection only, never tree_view_'s.
  auto custom_drag_provider = [this]() { return custom_view_->selectedCurveNamesRecursive(); };
  custom_view_->setDragSelectionProvider(custom_drag_provider);

  tree_view_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(tree_view_, &QWidget::customContextMenuRequested, this, &CurveListPanel::onTreeContextMenu);

  tree_view_->setTooltipProvider(
      [this](const QString& catalog_key, const QString& tree_path) { return rowTooltip(catalog_key, tree_path); });

  connect(tree_view_, &CurveTreeView::placeholderPeekRequested, this, &CurveListPanel::onPlaceholderPeekRequested);

  // The row's tooltip is the toast text: one wording for both channels.
  connect(tree_view_, &CurveTreeView::dragAttemptedOnNotDraggableRow, this, [this](const QString& reason) {
    emit toastRequested(reason.isEmpty() ? undisplayableObjectTooltip(sdk::BuiltinObjectType::kNone) : reason);
  });
  connect(tree_view_, &CurveTreeView::dragPayloadKeysSkipped, this, &CurveListPanel::onDragPayloadKeysSkipped);

  connect(tree_view_, &CurveTreeView::cancelRequested, this, &CurveListPanel::cancelRequested);
  // A series row's key packs its topic above the column: the stop names the series.
  connect(custom_view_, &CurveTreeView::cancelRequested, this, [this](quint64 row_key, bool) {
    emit seriesStopRequested(row_key >> kSeriesRowKeyColumnBits);
  });

  // The trailing edge: an update during the window fills once more and re-arms, so playback settles into a steady
  // cadence.
  value_throttle_timer_ = new QTimer(this);
  value_throttle_timer_->setSingleShot(true);
  value_throttle_timer_->setInterval(kValueRefreshIntervalMs);
  connect(value_throttle_timer_, &QTimer::timeout, this, [this]() {
    if (!value_refresh_pending_) {
      return;
    }
    value_refresh_pending_ = false;
    if (valuesColumnActive()) {
      fillValuesNow();
    }
    value_throttle_timer_->start();
  });
}

CurveListPanel::~CurveListPanel() {
  delete ui_;
}

QString CurveListPanel::rowTooltip(const QString& catalog_key, const QString& tree_path) const {
  if (catalog_ == nullptr) {
    return {};
  }
  // Rich text: semi-bold labels, names HTML-escaped (a topic can contain '<').
  QStringList lines;
  const auto describe_topic = [&](const CatalogItem& item) {
    if (isAdvertisedTopic(item)) {
      lines << tr("Not subscribed. Drop it into a view to start streaming.");
      return;
    }
    const std::optional<TopicStats> stats = catalog_->topicStats(item.key);
    if (!stats.has_value()) {
      return;
    }
    lines << item.topic_name.toHtmlEscaped();
    if (!stats->schema_name.isEmpty()) {
      lines << tr("<span style=\"font-weight:600\">Type:</span> %1").arg(stats->schema_name.toHtmlEscaped());
    }
    lines << tr("<span style=\"font-weight:600\">Messages:</span> %1").arg(formatCount(*stats));
    // Plugin-supplied text, untrusted: escaped, with line breaks kept. "description" is the canonical metadata key.
    if (const QString description = catalog_->topicMetadata(item.key).value(u"description"_s).toString();
        !description.isEmpty()) {
      lines << tr("<span style=\"font-weight:600\">Description:</span> %1")
                   .arg(description.toHtmlEscaped().replace(u'\n', u"<br>"_s));
    }
  };

  if (!catalog_key.isEmpty()) {
    const std::optional<CatalogItem> item = catalog_->itemDescriptor(catalog_key);
    if (!item.has_value()) {
      return {};
    }
    if (const ScalarFieldPayload* scalar = asScalarField(*item)) {
      const std::optional<TopicStats> stats = catalog_->topicStats(item->key);
      if (!stats.has_value()) {
        return {};
      }
      lines << scalar->field_path.toHtmlEscaped();
      const QString type_name = primitiveTypeName(scalar->logical_type);
      if (!type_name.isEmpty()) {
        lines << tr("<span style=\"font-weight:600\">Type:</span> %1").arg(type_name);
      }
      lines << tr("<span style=\"font-weight:600\">Samples:</span> %1").arg(formatCount(*stats));
      // Plugin-supplied text, untrusted: escaped, with line breaks kept. "description" is the canonical metadata key.
      if (const QString description = catalog_->fieldMetadata(item->key).value(u"description"_s).toString();
          !description.isEmpty()) {
        lines << tr("<span style=\"font-weight:600\">Description:</span> %1")
                     .arg(description.toHtmlEscaped().replace(u'\n', u"<br>"_s));
      }
    } else {
      describe_topic(*item);
    }
    return lines.join(u"<br>"_s);
  }

  // A keyless group row: the dataset itself, a scalar topic's node, or a plain
  // struct folder. Resolve by tree path; a folder that is neither stays silent.
  // ponytail: O(catalog) scan per hover; index by tree path if it ever shows.
  const std::vector<CatalogItem> items = catalog_->items();
  QSet<QString> dataset_topics;
  DatasetId tooltip_dataset_id = 0;
  for (const CatalogItem& item : items) {
    const CurveTreeView::CurvePath topic_path{
        .key = {}, .dataset = item.dataset_name, .topic = item.topic_name, .field = {}};
    const QString topic_tree_path = CurveTreeView::treePathFromCurvePath(topic_path);
    if (topic_tree_path == tree_path) {
      describe_topic(item);
      return lines.join(u"<br>"_s);
    }
    if (item.dataset_name == tree_path) {
      dataset_topics.insert(item.topic_name);
      tooltip_dataset_id = item.dataset_id;
    }
  }
  if (!dataset_topics.isEmpty()) {
    lines << tr("<span style=\"font-weight:600\">Topics:</span> %1").arg(dataset_topics.size());
    // Attribution only, the full document is in the Info dialog. Metadata is untrusted: everything is escaped.
    if (const auto metadata = catalog_->datasetMetadata(tooltip_dataset_id); metadata.has_value()) {
      lines
          << tr("<span style=\"font-weight:600\">Loader metadata:</span> %1").arg(metadata->plugin_id.toHtmlEscaped());
    }
    lines << tr("Right-click for Info…");
  }
  return lines.join(u"<br>"_s);
}

void CurveListPanel::setCatalog(CatalogModel* catalog) {
  if (catalog_ == catalog) {
    return;
  }
  if (catalog_) {
    disconnect(catalog_, nullptr, this, nullptr);
  }
  catalog_ = catalog;
  rebuildTree(tree_view_, catalog_, tracker_, custom_rows_);
  if (!catalog_) {
    return;
  }
  connect(catalog_, &CatalogModel::itemsAdded, this, &CurveListPanel::onCatalogItemsAdded);
  connect(catalog_, &CatalogModel::itemsRemoved, this, &CurveListPanel::onCatalogItemsRemoved);
  connect(catalog_, &CatalogModel::cleared, this, &CurveListPanel::onCatalogCleared);
  // A key kept with new catalog data (a merge relabels its dataset): the rows built from it are stale, whatever
  // setCustomTopics says next.
  connect(catalog_, &CatalogModel::itemsChanged, this, [this](const QStringList& keys) {
    if (std::any_of(keys.begin(), keys.end(), [this](const QString& key) { return custom_rows_.contains(key); })) {
      refillCustomView();
    }
  });
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
  if (controller_ == controller) {
    return;
  }
  if (controller_ != nullptr) {
    disconnect(controller_, nullptr, this, nullptr);
  }
  controller_ = controller;
  if (controller_ != nullptr) {
    connect(
        controller_, &TopicDemandController::displayedSeriesChanged, this,
        &CurveListPanel::scheduleDisplayedKeysRefresh);
  }
  scheduleDisplayedKeysRefresh();
}

void CurveListPanel::setDisplayedScope(std::function<const QWidget*()> scope) {
  displayed_scope_ = std::move(scope);
  scheduleDisplayedKeysRefresh();
}

void CurveListPanel::scheduleDisplayedKeysRefresh() {
  if (displayed_refresh_scheduled_) {
    return;
  }
  displayed_refresh_scheduled_ = true;
  QTimer::singleShot(0, this, [this]() {
    displayed_refresh_scheduled_ = false;
    refreshDisplayedKeys();
  });
}

void CurveListPanel::refreshDisplayedKeys() {
  const QSet<QString> keys =
      controller_ != nullptr && displayed_scope_ ? controller_->displayedKeys(displayed_scope_()) : QSet<QString>{};
  tree_view_->setDisplayedKeys(keys);
  custom_view_->setDisplayedKeys(keys);
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
  // The path is derived as addCatalogItem files the row, so it matches the promoted fields' group node; the
  // unsubscribed flag does not affect it.
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
  // A key that vanished mid-drag shows raw rather than dropping the notice. Escaped: ToastNotification renders rich
  // text.
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
  // Without a tracker the sets are empty, so every flag clears instead of leaving stale dimming.
  const QHash<DatasetId, QSet<QString>> active_sets = buildActiveSets(*catalog_, tracker_);
  QSet<QString> unsubscribed;
  if (!active_sets.isEmpty()) {
    catalog_->forEachItem([&](const CatalogItem& item) {
      if (isTopicUnsubscribed(active_sets, item.dataset_id, item.topic_name)) {
        unsubscribed.insert(item.key);
      }
    });
  }
  tree_view_->setUnsubscribedKeys(unsubscribed);
  custom_view_->setUnsubscribedKeys(unsubscribed);
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
  last_tracker_time_ = tracker_time;
  if (!valuesColumnActive()) {
    return;
  }
  if (value_throttle_timer_->isActive()) {
    value_refresh_pending_ = true;
    return;
  }
  fillValuesNow();  // leading edge
  value_throttle_timer_->start();
}

bool CurveListPanel::valuesColumnActive() const {
  return catalog_ != nullptr && !(tree_view_->valuesColumnHidden() && custom_view_->valuesColumnHidden());
}

void CurveListPanel::fillValuesNow() {
  const int precision = valuePrecision();
  const double tracker_time = last_tracker_time_;
  auto provider = [this, tracker_time, precision](const QString& key) -> QString {
    // Before the numeric path: isScalarKey is true for strings too, which would then all show "-".
    if (catalog_->isStringKey(key)) {
      return catalog_->stringValueAt(key, tracker_time).value_or(u"-"_s);
    }
    const std::optional<double> value = catalog_->scalarValueAt(key, tracker_time);
    if (value.has_value()) {
      return formatScalarForColumn(*value, precision);
    }
    // No sample at or before the cursor: "-" for a numeric scalar, blank for an object topic.
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

  // The toggle slots still run but skip their QSettings writes. A rollback, so a throw cannot leave the flag stuck
  // and silently disable later user writes.
  {
    QScopedValueRollback guard(applying_state_, true);

    if (element.hasAttribute(u"show_topics"_s) && preserve_topic_name_check_ != nullptr) {
      const bool wanted = element.attribute(u"show_topics"_s) == "true"_L1;
      if (preserve_topic_name_check_->isChecked() != wanted) {
        preserve_topic_name_check_->setChecked(wanted);
      }
    }

    if (element.hasAttribute(u"show_values"_s) && show_values_check_ != nullptr) {
      const bool wanted = element.attribute(u"show_values"_s) == "true"_L1;
      if (show_values_check_->isChecked() != wanted) {
        show_values_check_->setChecked(wanted);
      }
    }
  }

  // Not blocked: textChanged applies the filter.
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
    refreshValues(last_tracker_time_);
  }
}

void CurveListPanel::onPreserveTopicNameToggled(bool checked) {
  if (!applying_state_) {
    QSettings settings;
    settings.setValue(QLatin1String(kPreserveTopicNameKey), checked);
  }
  tree_view_->setViewMode(checked ? CurveTreeView::ViewMode::kShowTopics : CurveTreeView::ViewMode::kHierarchical);
  rebuildTree(tree_view_, catalog_, tracker_, custom_rows_);
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
  // The clicked dataset, plus the rest of the selection when it is part of it; it need not be selected itself.
  add_dataset(clicked);
  if (clicked->isSelected()) {
    for (QTreeWidgetItem* item : tree_view_->selectedItems()) {
      add_dataset(item);
    }
  }
  if (dataset_ids.isEmpty()) {
    return;
  }

  auto* menu = new QMenu(this);
  menu->setObjectName(u"PJMenu"_s);
  const QString theme = currentTheme();
  const auto add_item = [&](const QString& icon, const QString& text, bool destructive, bool enabled) {
    QPushButton* button = addMenuButton(menu, icon, theme, text);
    button->setEnabled(enabled);
    if (destructive) {
      button->setProperty("destructive", true);
    }
    return button;
  };

  // Reload/Replace need exactly one file-backed dataset: a stream has no source to re-read.
  const bool single_file_backed = dataset_ids.size() == 1 && source_path_resolver_ != nullptr &&
                                  !source_path_resolver_(dataset_ids.front()).isEmpty();
  QPushButton* info_button = add_item(u":/resources/svg/diag_info.svg"_s, tr("Info"), false, dataset_ids.size() == 1);
  QPushButton* merge_button = add_item(u":/resources/svg/merge.svg"_s, tr("Merge"), false, dataset_ids.size() >= 2);
  QPushButton* reload_button = add_item(u":/resources/svg/replay.svg"_s, tr("Reload"), false, single_file_backed);
  QPushButton* replace_button =
      add_item(u":/resources/svg/compare_arrows.svg"_s, tr("Replace"), false, single_file_backed);
  // Always enabled: a dataset without marker rules makes it a no-op, and Manual mode
  // has no other way to run a stale rule.
  QPushButton* refresh_derived_button =
      add_item(u":/resources/svg/refresh.svg"_s, tr("Refresh derived"), false, /*enabled=*/true);
  QPushButton* remove_button =
      add_item(u":/resources/svg/trash.svg"_s, tr("Remove"), /*destructive=*/true, /*enabled=*/true);

  // A QWidgetAction button does not dismiss the menu: close it, then act from a queued call, after the menu's modal
  // loop has returned.
  const auto on_click = [this, menu](QPushButton* button, auto action) {
    connect(button, &QPushButton::clicked, menu, [this, menu, action]() {
      menu->close();
      QTimer::singleShot(0, this, action);
    });
  };
  const DatasetId first = dataset_ids.front();
  on_click(info_button, [this, first]() { emit datasetInfoRequested(first); });
  on_click(merge_button, [this, dataset_ids]() { emit mergeDatasetsRequested(dataset_ids); });
  on_click(reload_button, [this, first]() { emit reloadDatasetRequested(first); });
  on_click(replace_button, [this, first]() { emit replaceDatasetRequested(first); });
  on_click(refresh_derived_button, [this, dataset_ids]() { emit refreshDerivedRequested(dataset_ids); });
  on_click(remove_button, [this, dataset_ids]() { emit removeDatasetsRequested(dataset_ids); });

  showOwnedMenu(menu, tree_view_->viewport()->mapToGlobal(pos));
}

void CurveListPanel::showTopicContextMenu(QTreeWidgetItem* clicked, const QPoint& pos) {
  if (tracker_ == nullptr || catalog_ == nullptr) {
    return;
  }
  // On Linux the menu arrives on press, before a right-drag can start: a multi-selection (even across both trees)
  // gets no menu, so that gesture stays available.
  if (tree_view_->selectedItems().size() + custom_view_->selectedItems().size() > 1) {
    return;
  }
  // A promoted scalar topic is a group row whose keys live on its leaves: act only when every keyed row below the
  // click belongs to one topic.
  std::optional<CatalogItem> item;
  for (const QString& key : CurveTreeView::catalogKeysUnder(clicked)) {
    const auto resolved = catalog_->itemDescriptor(key);
    if (!resolved.has_value()) {
      continue;
    }
    if (item.has_value() && (item->dataset_id != resolved->dataset_id || item->topic_name != resolved->topic_name)) {
      return;
    }
    item = resolved;
  }
  if (!item.has_value() || !catalog_->isPerTopicPauseCapable(item->dataset_id)) {
    return;  // no topic, or a dataset without per-topic pause
  }

  // Forcing is a hold on top of display references (TopicDemandTracker::setTopicForced): stopping it never pauses a
  // topic something still displays.
  const bool forced = tracker_->isTopicForced(item->dataset_id, item->topic_name);
  auto* menu = new QMenu(this);
  menu->setObjectName(u"PJMenu"_s);
  QPushButton* button = addMenuButton(
      menu, u":/resources/svg/cast.svg"_s, currentTheme(),
      forced ? tr("Stop forced streaming") : tr("Force topic streaming"));

  connect(
      button, &QPushButton::clicked, menu,
      [this, menu, dataset_id = item->dataset_id, topic_name = item->topic_name, forced]() {
        menu->close();
        // tracker_ is re-read: the panel can be rebound while the menu is open.
        if (tracker_ != nullptr) {
          tracker_->setTopicForced(dataset_id, topic_name, !forced);
        }
      });
  showOwnedMenu(menu, tree_view_->viewport()->mapToGlobal(pos));
}

void CurveListPanel::onCatalogItemsAdded(const std::vector<CatalogItem>& items) {
  const QSet<QString> preview_keys = catalog_ != nullptr ? catalog_->ephemeralPreviewKeys() : QSet<QString>{};
  std::vector<CatalogItem> tree_items;
  for (const auto& item : items) {
    if (!custom_rows_.contains(item.key) && !preview_keys.contains(item.key)) {
      tree_items.push_back(item);
    }
  }
  addCatalogItems(tree_view_, catalog_, tracker_, tree_items);
  refreshValues(last_tracker_time_);
}

void CurveListPanel::setCustomTopics(const std::vector<CustomTopicRow>& rows) {
  QHash<QString, CustomTopicRow> wanted;
  wanted.reserve(static_cast<qsizetype>(rows.size()));
  for (const CustomTopicRow& row : rows) {
    wanted.insert(row.key, row);
  }
  const auto same_presentation = [](const CustomTopicRow& a, const CustomTopicRow& b) {
    return a.owner == b.owner && a.name == b.name && a.pinned_t == b.pinned_t;
  };
  const bool keys_changed = wanted.size() != custom_rows_.size() ||
                            std::any_of(wanted.keyValueBegin(), wanted.keyValueEnd(), [this](const auto& entry) {
                              return !custom_rows_.contains(entry.first);
                            });
  const bool presentation_changed =
      keys_changed || std::any_of(wanted.keyValueBegin(), wanted.keyValueEnd(), [&](const auto& entry) {
        return !same_presentation(custom_rows_.value(entry.first), entry.second);
      });
  custom_rows_ = std::move(wanted);
  if (!presentation_changed) {
    applyCustomRowStates();
    return;
  }
  refillCustomView();
  if (keys_changed) {
    // The keys are the main tree's exclusion set: rebuild, or a topic shows in both lists or in neither.
    rebuildTree(tree_view_, catalog_, tracker_, custom_rows_);
  }
}

void CurveListPanel::setEditorPluginId(const QString& plugin_id) {
  if (plugin_id == editor_plugin_id_) {
    return;
  }
  editor_plugin_id_ = plugin_id;
  ui_->buttonAddCustom->setVisible(!plugin_id.isEmpty());
  refillCustomView();  // which rows are read-only, so which carry a badge
}

void CurveListPanel::setEditorViewsOthers(bool views) {
  editor_views_others_ = views;
  updateCustomEditButton();
}

QStringList CurveListPanel::customTopicNames() const {
  QStringList names;
  if (catalog_ == nullptr) {
    return names;
  }
  for (const QString& key : custom_rows_.keys()) {
    if (const std::optional<CatalogItem> item = catalog_->itemDescriptor(key); item.has_value()) {
      names.push_back(item->topic_name);
    }
  }
  names.sort();
  return names;
}

void CurveListPanel::refillCustomView() {
  if (custom_view_ == nullptr) {
    return;
  }
  const QStringList expanded = custom_view_->expandedGroupPaths();
  std::vector<CurveTreeView::CurvePath> paths;
  custom_object_rows_.clear();
  series_row_paths_.clear();
  if (catalog_ != nullptr) {
    const QHash<DatasetId, QSet<QString>> active_sets = buildActiveSets(*catalog_, tracker_);
    for (const auto& item : catalog_->items()) {
      const auto found = custom_rows_.constFind(item.key);
      if (found == custom_rows_.cend()) {
        continue;
      }
      const CustomTopicRow& row = *found;
      // Like its Datasets-tree twin but flat, named by its recipe; the search path stays the dataset/topic one, so
      // the glyph and the filter match.
      const auto flat_row = [&](const QString& label) {
        CurveTreeView::CurvePath flat = treePathFromCatalogItem(item, active_sets);
        flat.display_name = label;
        if (isReadOnlyCustomRow(item.key)) {
          flat.badge = ownerInfo(row.owner).badge;
        }
        const QString created_by =
            row.owner.isEmpty()
                ? tr("Dataset %1").arg(item.dataset_name)
                : tr("Created by %1 \u00B7 dataset %2").arg(ownerInfo(row.owner).name, item.dataset_name);
        flat.tooltip = flat.tooltip.isEmpty() ? created_by : created_by + u"\n"_s + flat.tooltip;
        return flat;
      };
      CurveTreeView::CurvePath path;
      const ScalarFieldPayload* field = asScalarField(item);
      if (asObjectTopic(item) != nullptr) {
        path = flat_row(row.name.isEmpty() ? item.topic_name : row.name);
        custom_object_rows_.insert(
            item.key, CustomObjectRow{CurveTreeView::treePathFromCurvePath(path), item.dataset_id, path.tooltip});
      } else if (field != nullptr && !row.name.isEmpty() && field->column_index < (1U << kSeriesRowKeyColumnBits)) {
        path = flat_row(row.name);  // a series column: a flat, plottable leaf
        series_row_paths_.insert(
            (static_cast<quint64>(field->topic_id) << kSeriesRowKeyColumnBits) | field->column_index,
            CurveTreeView::treePathFromCurvePath(path));
      } else {
        path.key = item.key;
        path.dataset = item.topic_name;
        if (!row.owner.isEmpty()) {
          path.tooltip = tr("Created by %1").arg(ownerInfo(row.owner).name);
        }
      }
      paths.push_back(std::move(path));
    }
  }
  custom_view_->clearCurves();
  custom_view_->addCatalogItems(paths);
  custom_view_->restoreExpandedGroupPaths(expanded);
  for (auto it = series_row_paths_.cbegin(); it != series_row_paths_.cend(); ++it) {
    custom_view_->setDatasetRowKey(it.value(), it.key());  // before any progress names the key
  }
  applySeriesProgress();
  applyCustomRowStates();
  updateCustomEditButton();
}

void CurveListPanel::setSeriesProgress(const QHash<quint64, CurveTreeView::DatasetProgress>& by_topic) {
  series_progress_ = by_topic;
  applySeriesProgress();
}

void CurveListPanel::applySeriesProgress() {
  if (custom_view_ == nullptr) {
    return;
  }
  // Only rows that exist are decorated: the view would turn an unmapped key into a ghost row.
  QHash<quint64, CurveTreeView::DatasetProgress> by_row_key;
  for (auto it = series_row_paths_.cbegin(); it != series_row_paths_.cend(); ++it) {
    const auto progress = series_progress_.constFind(it.key() >> kSeriesRowKeyColumnBits);
    if (progress != series_progress_.cend()) {
      by_row_key.insert(it.key(), *progress);
    }
  }
  custom_view_->setDatasetProgress(by_row_key);
}

namespace {

// Computing while an older result is shown keeps the computing glyph, with the lag.
QString stateText(const NodeScheduler::NodeState& state) {
  using NodeStatus = NodeScheduler::NodeState::Status;
  const QString detail = QString::fromStdString(state.detail);
  switch (state.status) {
    case NodeStatus::kPending:
      return state.lag.has_value() ? CurveListPanel::tr("Computing; the result shown is %1 s behind")
                                         .arg(static_cast<double>(*state.lag) / 1e9, 0, 'f', 3)
                                   : CurveListPanel::tr("Computing");
    case NodeStatus::kMissingInput:
      return detail.isEmpty() ? CurveListPanel::tr("Missing input")
                              : CurveListPanel::tr("Missing input: %1").arg(detail);
    case NodeStatus::kError:
      return detail.isEmpty() ? CurveListPanel::tr("Error") : CurveListPanel::tr("Error: %1").arg(detail);
    case NodeStatus::kIdle:
    case NodeStatus::kReady:
      break;
  }
  return {};
}

CurveTreeView::DerivedGlyphState glyphStateOf(NodeScheduler::NodeState::Status status) {
  using NodeStatus = NodeScheduler::NodeState::Status;
  switch (status) {
    case NodeStatus::kPending:
      return CurveTreeView::DerivedGlyphState::kPending;
    case NodeStatus::kReady:
      return CurveTreeView::DerivedGlyphState::kReady;
    case NodeStatus::kMissingInput:
      return CurveTreeView::DerivedGlyphState::kMissingInput;
    case NodeStatus::kError:
      return CurveTreeView::DerivedGlyphState::kError;
    case NodeStatus::kIdle:
      break;
  }
  return CurveTreeView::DerivedGlyphState::kIdle;
}

}  // namespace

void CurveListPanel::applyCustomRowStates() {
  if (custom_view_ == nullptr) {
    return;
  }
  // Each object row's whole tooltip is composed here and written with its glyph in one pass, so a row whose
  // state, pin and catalog data did not change sees no model change (a hover stays put).
  QHash<QString, CurveTreeView::DerivedGlyph> glyphs;
  for (auto it = custom_object_rows_.cbegin(); it != custom_object_rows_.cend(); ++it) {
    const CustomTopicRow& row = custom_rows_[it.key()];
    CurveTreeView::DerivedGlyph glyph{.state = CurveTreeView::DerivedGlyphState::kIdle, .tooltip = it->base_tooltip};
    if (row.state.has_value()) {
      glyph.state = glyphStateOf(row.state->status);
      if (const QString text = stateText(*row.state); !text.isEmpty()) {
        glyph.tooltip += u"\n"_s + text;
      }
    }
    // A finding names its computation instant on the axis the timeline shows.
    if (row.pinned_t.has_value() && display_time_resolver_) {
      if (const std::optional<double> seconds = display_time_resolver_(it->dataset_id, *row.pinned_t)) {
        glyph.tooltip += u"\n"_s + tr("Pinned at %1 s").arg(*seconds, 0, 'f', 3);
      }
    }
    glyphs.insert(it->tree_path, std::move(glyph));
  }
  custom_view_->setDerivedTopicGlyphs(glyphs);
}

QStringList CurveListPanel::selectedCustomRowKeys() const {
  QStringList keys;
  for (const QTreeWidgetItem* item : custom_view_->selectedItems()) {
    if (const QString key = CurveTreeView::catalogKeyOf(item); !key.isEmpty() && !keys.contains(key)) {
      keys.push_back(key);
    }
  }
  return keys;
}

bool CurveListPanel::isReadOnlyCustomRow(const QString& catalog_key) const {
  return !ExtensionCatalogService::isEditorOwner(editor_plugin_id_, custom_rows_.value(catalog_key).owner);
}

CurveListPanel::OwnerInfo CurveListPanel::ownerInfo(const QString& plugin_id) const {
  OwnerInfo info = owner_resolver_ ? owner_resolver_(plugin_id) : OwnerInfo{};
  // An owner that is not loaded keeps a visible mark: its id stands in for both.
  if (info.badge.isEmpty()) {
    info.badge = plugin_id;
  }
  if (info.name.isEmpty()) {
    info.name = plugin_id;
  }
  return info;
}

void CurveListPanel::updateCustomEditButton() {
  const QStringList keys = selectedCustomRowKeys();
  const auto read_only_tooltip = [this](const QString& key) {
    const QString owner = custom_rows_.value(key).owner;
    return owner.isEmpty() ? tr("Read-only") : tr("Created by %1; read-only").arg(ownerInfo(owner).name);
  };
  bool edit_enabled = false;
  QString edit_tooltip = tr("Edit the selected custom topic");
  if (keys.size() == 1) {
    if (!isReadOnlyCustomRow(keys.front())) {
      edit_enabled = true;
    } else if (editor_views_others_ && !editor_plugin_id_.isEmpty()) {
      edit_enabled = true;
      const QString owner = custom_rows_.value(keys.front()).owner;
      edit_tooltip = owner.isEmpty() ? tr("View the formula (read-only)")
                                     : tr("View the formula (created by %1; read-only)").arg(ownerInfo(owner).name);
    } else {
      edit_tooltip = read_only_tooltip(keys.front());
    }
  }
  ui_->buttonEditCustom->setEnabled(edit_enabled);
  ui_->buttonEditCustom->setToolTip(edit_tooltip);

  // Delete acts on every key under the selection (a group row included), so one
  // use-only row anywhere in it disables the whole action.
  bool delete_enabled = true;
  QString delete_tooltip;
  for (const QString& key : custom_view_->selectedCatalogKeysRecursive()) {
    if (isReadOnlyCustomRow(key)) {
      delete_enabled = false;
      delete_tooltip = read_only_tooltip(key);
      break;
    }
  }
  delete_custom_button_->setEnabled(delete_enabled);
  delete_custom_button_->setToolTip(delete_tooltip);
}

void CurveListPanel::removeCustomCurve(const QString& catalog_key) {
  if (custom_view_ == nullptr) {
    return;
  }
  custom_rows_.remove(catalog_key);
  refillCustomView();
}

void CurveListPanel::onCatalogItemsRemoved(const QStringList& keys) {
  // The delete action cannot clear a row whose item is gone (it removes the row only after removing the transform,
  // which no longer exists): drop it here.
  bool custom_changed = false;
  for (const QString& key : keys) {
    custom_changed = custom_rows_.remove(key) || custom_changed;
  }
  if (custom_changed) {
    refillCustomView();
  }
  // One rebuild per batch: a dataset or multi-key trash is a single itemsRemoved.
  rebuildTree(tree_view_, catalog_, tracker_, custom_rows_);
}

void CurveListPanel::onCatalogCleared() {
  tree_view_->clearCurves();
  custom_rows_.clear();
  custom_object_rows_.clear();
  series_row_paths_.clear();
  if (custom_view_ != nullptr) {
    custom_view_->clearCurves();
  }
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
      ui_->buttonAddCustom->setVisible(!focused && !editor_plugin_id_.isEmpty());
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
  // The placeholder-widget glyphs; unchecked (hidden) is the slashed variant. The slash ink copies loadSvg's ink per
  // theme, so it matches the visibility_off eye: change both together.
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
  ui_->filterTimeseries->setChromeMetrics(chrome_metrics_);
  ui_->filterCustom->setChromeMetrics(chrome_metrics_);

  // clear_all_button_ and delete_custom_button_ are full-width menu items, not square chrome.
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
  // band_extent includes the inner margins, so they grow the band instead of squeezing the chrome.
  ui_->widgetLabelTimeseries->setFixedHeight(band_extent);
  ui_->widgetSearchTimeseries->setFixedHeight(band_extent);
  ui_->widgetLabelCustom->setFixedHeight(band_extent);
  const QMargins margins(
      chrome_metrics_.layout_padding, chrome_metrics_.layout_padding, chrome_metrics_.layout_padding,
      chrome_metrics_.layout_padding);
  // Title bands get no left inset: their label's own padding leads, and both would double the leading of the other
  // bands. Search bands keep it: their field has no internal padding.
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
  // QTreeView has no setSpacing(): a per-instance stylesheet pads ::item. At zero spacing the empty stylesheet
  // clears the previous rule.
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
