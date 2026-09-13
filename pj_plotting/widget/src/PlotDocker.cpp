// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotDocker.h"

#include <DockAreaTitleBar.h>
#include <DockAreaWidget.h>
#include <DockComponentsFactory.h>
#include <DockContainerWidget.h>
#include <DockSplitter.h>
#include <ads_globals.h>

#include <QDomDocument>
#include <QEvent>
#include <QHash>
#include <QSet>
#include <QSplitter>
#include <QStringList>
#include <QUuid>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotFocusOverlay.h"
#include "pj_plotting/PlotWidget.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

// We own the title via DockToolbar — the built-in ADS title bar is always
// hidden.
class HiddenTitleBar : public ads::CDockAreaTitleBar {
 public:
  using ads::CDockAreaTitleBar::CDockAreaTitleBar;
  void setVisible(bool /*visible*/) override {
    QWidget::setVisible(false);
  }
};

class SplittableComponentsFactory : public ads::CDockComponentsFactory {
 public:
  ads::CDockAreaTitleBar* createDockAreaTitleBar(ads::CDockAreaWidget* dock_area) const override {
    auto* title_bar = new HiddenTitleBar(dock_area);
    title_bar->setVisible(false);
    return title_bar;
  }
};

// A splitter drag emits splitterMoved per mouse move; coalesce into one
// history snapshot after the drag goes quiet.
constexpr int kSplitterUndoDebounceMs = 200;

QString newStateId() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QDomElement saveChildNodesState(QDomDocument& doc, QWidget* widget) {
  if (widget == nullptr) {
    return {};
  }

  if (auto* splitter = qobject_cast<QSplitter*>(widget)) {
    QDomElement splitter_element = doc.createElement(u"DockSplitter"_s);
    splitter_element.setAttribute(u"orientation"_s, splitter->orientation() == Qt::Horizontal ? u"|"_s : u"-"_s);
    splitter_element.setAttribute(u"count"_s, QString::number(splitter->count()));

    QStringList normalized_sizes;
    int total_size = 0;
    for (int size : splitter->sizes()) {
      total_size += size;
    }
    for (int index = 0; index < splitter->count(); ++index) {
      const int size = splitter->sizes().value(index, 0);
      const double normalized = total_size > 0 ? static_cast<double>(size) / static_cast<double>(total_size)
                                               : 1.0 / static_cast<double>(std::max(1, splitter->count()));
      normalized_sizes.push_back(QString::number(normalized, 'f', 8));
    }
    splitter_element.setAttribute(u"sizes"_s, normalized_sizes.join(u";"_s));

    for (int index = 0; index < splitter->count(); ++index) {
      QDomElement child = saveChildNodesState(doc, splitter->widget(index));
      if (!child.isNull()) {
        splitter_element.appendChild(child);
      }
    }
    return splitter_element;
  }

  auto* dock_area = qobject_cast<ads::CDockAreaWidget*>(widget);
  if (dock_area == nullptr) {
    return {};
  }

  QDomElement area_element = doc.createElement(u"DockArea"_s);
  for (int index = 0; index < dock_area->dockWidgetsCount(); ++index) {
    auto* dock_widget = dynamic_cast<DockWidget*>(dock_area->dockWidget(index));
    if (dock_widget == nullptr) {
      continue;
    }
    // Plot widgets emit <plot>; object widgets (Scene3DDockWidget, …)
    // emit whatever their xmlSaveState returns (e.g. <scene3d>). A null
    // QDomElement from an object widget means "I don't persist state yet",
    // so we skip it honestly rather than writing an empty stub.
    QDomElement payload;
    if (auto* plot = dock_widget->plotWidget()) {
      payload = plot->xmlSaveState(doc);
    } else if (auto* obj = dock_widget->objectWidget()) {
      payload = obj->xmlSaveState(doc);
      if (payload.isNull()) {
        continue;
      }
    } else {
      // The chooser is workspace state: omitting it makes undo of a visualization
      // choice restore an empty plot instead of the chooser.
      payload = doc.createElement(u"placeholder"_s);
    }
    area_element.setAttribute(u"id"_s, dock_widget->stateId());
    area_element.setAttribute(u"name"_s, dock_widget->name());
    area_element.appendChild(payload);
  }
  return area_element;
}

struct LayoutNode {
  enum class Type { kArea, kSplitter };

  Type type = Type::kArea;
  Qt::Orientation orientation = Qt::Horizontal;
  QVector<double> size_ratios;
  QString area_id;
  QString area_name;
  QVector<QDomElement> plots;
  QVector<LayoutNode> children;
  bool valid = false;
};

LayoutNode parseLayoutNode(const QDomElement& element) {
  LayoutNode node;
  if (element.isNull()) {
    return node;
  }

  if (element.tagName() == "DockSplitter"_L1) {
    node.type = LayoutNode::Type::kSplitter;
    const QString orientation = element.attribute(u"orientation"_s);
    if (orientation == "|"_L1) {
      node.orientation = Qt::Horizontal;
    } else if (orientation == "-"_L1) {
      node.orientation = Qt::Vertical;
    } else {
      return node;
    }

    if (element.hasAttribute(u"sizes"_s)) {
      const QString sizes = element.attribute(u"sizes"_s);
      if (sizes.isEmpty()) {
        return node;
      }
      double total_ratio = 0.0;
      for (const QString& size : sizes.split(';', Qt::KeepEmptyParts)) {
        bool ok = false;
        const double value = size.toDouble(&ok);
        if (!ok || !std::isfinite(value) || value < 0.0) {
          return node;
        }
        node.size_ratios.push_back(value);
        total_ratio += value;
      }
      if (!std::isfinite(total_ratio) || !(total_ratio > 0.0)) {
        return node;
      }
    }
    for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
      LayoutNode child_node = parseLayoutNode(child);
      if (!child_node.valid) {
        // A splitter is one structural unit. Skipping one malformed branch
        // would silently delete its plots while applying the valid siblings.
        return node;
      }
      node.children.push_back(std::move(child_node));
    }
    if (node.children.isEmpty()) {
      return node;
    }
    if (element.hasAttribute(u"count"_s)) {
      bool ok = false;
      const int declared_count = element.attribute(u"count"_s).toInt(&ok);
      if (!ok || declared_count != node.children.size()) {
        return node;
      }
    }
    if (!node.size_ratios.isEmpty() && node.size_ratios.size() != node.children.size()) {
      return node;
    }
    node.valid = true;
    return node;
  }

  if (element.tagName() == "DockArea"_L1) {
    node.type = LayoutNode::Type::kArea;
    node.area_id = element.attribute(u"id"_s);
    node.area_name = element.attribute(u"name"_s);
    // One PJ dock has exactly one payload. Picking the first of several would
    // turn a malformed document into a successful but lossy restore.
    const QDomElement payload = element.firstChildElement();
    if (payload.isNull() || !payload.nextSiblingElement().isNull()) {
      return node;
    }
    const QString payload_tag = payload.tagName();
    if (payload_tag == "Tab"_L1 || payload_tag == "Container"_L1 || payload_tag == "DockSplitter"_L1 ||
        payload_tag == "DockArea"_L1) {
      return node;
    }
    if (payload_tag == "placeholder"_L1 && !payload.firstChildElement().isNull()) {
      return node;
    }
    node.plots.push_back(payload);
    node.valid = true;
    return node;
  }

  return node;
}

// First leaf-area dock element reachable from `node`, descending into the
// first child of each splitter. Used to decide between plot-pool and
// empty-object-widget construction at the top of the restore tree.
QDomElement firstLeafElement(const LayoutNode& node) {
  const LayoutNode* cur = &node;
  while (cur->type == LayoutNode::Type::kSplitter && !cur->children.isEmpty()) {
    cur = &cur->children.front();
  }
  if (cur->type == LayoutNode::Type::kArea && !cur->plots.isEmpty()) {
    return cur->plots.front();
  }
  return {};
}

// A dock's content element is an object widget (Scene3DDockWidget,
// Scene2DDockWidget, …) when it is present and is not the PlotWidget's own
// <plot> element. Restore hands its tagName() to the object-widget factory as
// the "kind", so pj_plotting stays agnostic to specific scene families.
bool isObjectWidgetElement(const QDomElement& element) {
  return !element.isNull() && element.tagName() != "plot"_L1 && element.tagName() != "placeholder"_L1;
}

bool isPlaceholderElement(const QDomElement& element) {
  return !element.isNull() && element.tagName() == "placeholder"_L1;
}

class RestorePlotPool {
 public:
  RestorePlotPool(QVector<DockWidget*> docks, SessionManager* session, CatalogModel* catalog, QWidget* restored_parent)
      : session_(session), catalog_(catalog), restored_parent_(restored_parent) {
    for (DockWidget* dock : docks) {
      if (dock == nullptr) {
        continue;
      }
      PlotWidget* plot = dock->releasePlotWidget();
      if (plot == nullptr) {
        continue;
      }
      plot->setDataServices(session_, catalog_);
      plots_by_position_.push_back(plot);
      const QString id = plot->stateId();
      if (!id.isEmpty() && !plots_by_id_.contains(id)) {
        plots_by_id_.insert(id, plot);
      }
    }
  }

  PlotWidget* takeFirstForNode(const LayoutNode& node) {
    if (node.type == LayoutNode::Type::kArea) {
      return takeForArea(node);
    }
    for (const LayoutNode& child : node.children) {
      return takeFirstForNode(child);
    }
    return createPlot({});
  }

  PlotWidget* takeForArea(const LayoutNode& node) {
    const QDomElement plot_element = node.plots.isEmpty() ? QDomElement{} : node.plots.front();
    const QString plot_id = plot_element.attribute(u"id"_s);
    if (!plot_id.isEmpty()) {
      auto it = plots_by_id_.find(plot_id);
      if (it != plots_by_id_.end() && !used_.contains(it.value())) {
        PlotWidget* plot = it.value();
        used_.insert(plot);
        return plot;
      }
    }

    while (next_position_ < plots_by_position_.size()) {
      PlotWidget* plot = plots_by_position_.at(next_position_++);
      if (plot != nullptr && !used_.contains(plot)) {
        used_.insert(plot);
        return plot;
      }
    }

    return createPlot(plot_id);
  }

  void deleteUnused() {
    for (PlotWidget* plot : plots_by_position_) {
      if (plot != nullptr && !used_.contains(plot)) {
        plot->deleteLater();
      }
    }
  }

 private:
  PlotWidget* createPlot(const QString& state_id) {
    auto* plot = new PlotWidget(session_, catalog_, restored_parent_);
    plot->setStateId(state_id);
    used_.insert(plot);
    plots_by_position_.push_back(plot);
    if (!plot->stateId().isEmpty() && !plots_by_id_.contains(plot->stateId())) {
      plots_by_id_.insert(plot->stateId(), plot);
    }
    return plot;
  }

  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  QWidget* restored_parent_ = nullptr;
  QVector<PlotWidget*> plots_by_position_;
  QHash<QString, PlotWidget*> plots_by_id_;
  QSet<PlotWidget*> used_;
  qsizetype next_position_ = 0;
};

void applySplitterSizes(const LayoutNode& node, const QVector<DockWidget*>& widgets) {
  if (node.size_ratios.size() != widgets.size() || widgets.isEmpty()) {
    return;
  }
  auto* splitter = ads::internal::findParent<ads::CDockSplitter*>(widgets.back());
  if (splitter == nullptr) {
    return;
  }

  // Express the saved ratios as large relative weights instead of scaling them by
  // the widgets' *current* pixel sizes. During layout restore the docker is not
  // laid out at its final size yet — TabbedPlotWidget builds a brand-new PlotDocker
  // per tab and restores into it before it is shown — so widget->width()/height()
  // report placeholder geometry that distorts the proportions (and the splitter
  // later "grows" to fit, pulling the split toward 50/50). QSplitter::setSizes only
  // stores relative weights: when the splitter is first laid out it shrinks these
  // oversized hints to fit, distributing space proportionally, so a large common
  // scale reproduces the saved ratios exactly regardless of when the dock area
  // becomes visible.
  constexpr double kRelativeScale = 1'000'000.0;
  const double total_ratio = std::accumulate(node.size_ratios.cbegin(), node.size_ratios.cend(), 0.0);
  QList<int> sizes;
  for (double ratio : node.size_ratios) {
    sizes.push_back(std::max(1, static_cast<int>((ratio / total_ratio) * kRelativeScale)));
  }
  splitter->setSizes(sizes);
}

bool restoreNode(
    const LayoutNode& node, DockWidget* widget, RestorePlotPool& pool,
    const PlotDocker::ObjectWidgetFactory& object_factory) {
  if (widget == nullptr) {
    return false;
  }

  if (node.type == LayoutNode::Type::kArea) {
    widget->setStateId(node.area_id);
    widget->setName(node.area_name.isEmpty() ? u"..."_s : node.area_name);

    const QDomElement dock_element = node.plots.isEmpty() ? QDomElement{} : node.plots.front();

    if (isPlaceholderElement(dock_element)) {
      widget->setPlaceholderWidget();
      return true;
    }

    if (isObjectWidgetElement(dock_element)) {
      // Object-widget path: build an empty dock by kind (the XML tag), then ask
      // it to load its own payload. Unknown kind (no factory / null result) →
      // leave the dock as-is rather than mis-parsing a scene element as a plot.
      IDataWidget* obj = object_factory ? object_factory(dock_element.tagName(), nullptr, widget) : nullptr;
      if (obj == nullptr) {
        return false;
      }
      widget->setObjectWidget(obj);
      return obj->xmlLoadState(dock_element);
    }

    // Plot path
    PlotWidget* plot = widget->plotWidget();
    if (plot == nullptr) {
      plot = pool.takeForArea(node);
      widget->setPlotWidget(plot);
    }
    if (!dock_element.isNull()) {
      return plot->xmlLoadState(dock_element);
    } else {
      plot->removeAllCurves();
    }
    return true;
  }

  if (node.children.isEmpty()) {
    return false;
  }

  QVector<DockWidget*> widgets;
  widgets.push_back(widget);
  DockWidget* split_anchor = widget;
  for (qsizetype index = 1; index < node.children.size(); ++index) {
    const LayoutNode& child = node.children.at(index);
    const bool child_is_object = isObjectWidgetElement(firstLeafElement(child));
    const bool child_is_placeholder = isPlaceholderElement(firstLeafElement(child));
    if (child_is_object || child_is_placeholder) {
      // No plot needed — the leaf will install an object widget itself.
      split_anchor =
          node.orientation == Qt::Horizontal ? split_anchor->splitHorizontal() : split_anchor->splitVertical();
    } else {
      PlotWidget* child_plot = pool.takeFirstForNode(child);
      split_anchor = node.orientation == Qt::Horizontal ? split_anchor->splitHorizontal(child_plot)
                                                        : split_anchor->splitVertical(child_plot);
    }
    if (split_anchor == nullptr) {
      return false;
    }
    widgets.push_back(split_anchor);
  }
  applySplitterSizes(node, widgets);

  bool restored = widgets.size() == node.children.size();
  for (qsizetype index = 0; index < node.children.size() && index < widgets.size(); ++index) {
    restored = restoreNode(node.children.at(index), widgets.at(index), pool, object_factory) && restored;
  }
  return restored;
}

}  // namespace

PlotDocker::PlotDocker(QString name, SessionManager* session, CatalogModel* catalog, QWidget* parent)
    : ads::CDockManager(parent), state_id_(newStateId()), name_(std::move(name)), session_(session), catalog_(catalog) {
  setStyleSheet("");  // Disable ADS's built-in stylesheet.
  setComponentsFactory(new SplittableComponentsFactory());

  connect(this, &ads::CDockManager::dockWidgetRemoved, this, [this](ads::CDockWidget* removed) {
    auto* removed_dock = qobject_cast<DockWidget*>(removed);
    const bool removed_was_focused = (removed_dock != nullptr && removed_dock == focused_dock_);
    ensureAtLeastOneWidget();
    // ADS doesn't move focus to a survivor when the focused dock is closed, so
    // do it here: the right panel mirrors the focused dock, and leaving focus
    // on a deleted dock would strand stale settings on screen.
    if (removed_was_focused && !restoring_state_) {
      refocusAfterRemoval(removed_dock);
    }
  });
  connect(this, &ads::CDockManager::dockAreasAdded, this, [this]() {
    watchSplitters();
    if (!restoring_state_) {
      emit undoableChange();
    }
  });

  focus_overlay_ = new PlotFocusOverlay(*this);
  connect(
      this, &ads::CDockManager::focusedDockWidgetChanged, this,
      [this](ads::CDockWidget* /*old*/, ads::CDockWidget* now) {
        auto* now_dock = qobject_cast<DockWidget*>(now);
        if (now_dock != focused_dock_) {
          previous_dock_ = focused_dock_;
          focused_dock_ = now_dock;
        }
        focus_overlay_->setFocusedArea(now != nullptr ? now->dockAreaWidget() : nullptr);
        emit dockFocused(now_dock);
      });
  connect(this, &PlotDocker::plotWidgetAdded, this, &PlotDocker::watchPlotForHover);

  splitter_undo_debounce_.setSingleShot(true);
  splitter_undo_debounce_.setInterval(kSplitterUndoDebounceMs);
  connect(&splitter_undo_debounce_, &QTimer::timeout, this, [this]() {
    // Re-check at fire time: a debounce armed just before a restore must not
    // publish into it.
    if (!restoring_state_) {
      emit undoableChange();
    }
  });

  ensureAtLeastOneWidget();
  watchSplitters();
}

PlotDocker::~PlotDocker() = default;

void PlotDocker::watchPlotForHover(PlotWidget* plot) {
  if (plot != nullptr) {
    plot->installHoverFilter(this);
  }
}

void PlotDocker::watchSplitters() {
  for (QSplitter* splitter : findChildren<QSplitter*>()) {
    connect(splitter, &QSplitter::splitterMoved, this, &PlotDocker::onSplitterMoved, Qt::UniqueConnection);
  }
}

void PlotDocker::onSplitterMoved(int /*position*/, int /*index*/) {
  if (!restoring_state_) {
    splitter_undo_debounce_.start();
  }
}

bool PlotDocker::eventFilter(QObject* watched, QEvent* event) {
  const QEvent::Type type = event->type();
  if (focus_overlay_ != nullptr && (type == QEvent::Enter || type == QEvent::Leave)) {
    // Walk up from the watched child (canvas or axis widget) to its
    // containing DockWidget so we can hand the overlay its CDockAreaWidget.
    for (QObject* node = watched; node != nullptr; node = node->parent()) {
      auto* dock_widget = qobject_cast<ads::CDockWidget*>(node);
      if (dock_widget == nullptr) {
        continue;
      }
      focus_overlay_->setHoveredArea(type == QEvent::Enter ? dock_widget->dockAreaWidget() : nullptr);
      break;
    }
  }
  return ads::CDockManager::eventFilter(watched, event);
}

void PlotDocker::setDataServices(SessionManager* session, CatalogModel* catalog) {
  session_ = session;
  catalog_ = catalog;
  for (int index = 0; index < plotCount(); ++index) {
    if (auto* dock = plotAt(index)) {
      dock->setDataServices(session_, catalog_);
    }
  }
}

void PlotDocker::setObjectWidgetFactory(ObjectWidgetFactory factory) {
  object_widget_factory_ = std::move(factory);
  for (int index = 0; index < plotCount(); ++index) {
    if (auto* dock = plotAt(index)) {
      dock->setObjectWidgetFactory(object_widget_factory_);
    }
  }
}

QString PlotDocker::stateId() const {
  return state_id_;
}

void PlotDocker::setStateId(QString id) {
  if (!id.isEmpty()) {
    state_id_ = std::move(id);
  }
}

bool PlotDocker::isHistoryExempt() const {
  return history_exempt_;
}

void PlotDocker::setHistoryExempt(bool history_exempt) {
  history_exempt_ = history_exempt;
}

bool PlotDocker::isSerializable(SnapshotScope scope) const {
  return !(scope == SnapshotScope::kHistory && history_exempt_);
}

QString PlotDocker::ownerPlugin() const {
  return owner_plugin_;
}

QString PlotDocker::ownerTabId() const {
  return owner_tab_id_;
}

void PlotDocker::setOwnerMetadata(QString plugin_id, QString tab_id) {
  owner_plugin_ = std::move(plugin_id);
  owner_tab_id_ = std::move(tab_id);
  refreshOwnerWatermark();
}

bool PlotDocker::isOwnerPluginAvailable() const {
  return !owner_badge_.isEmpty();
}

void PlotDocker::setOwnerBadge(QString badge) {
  owner_badge_ = std::move(badge);
  refreshOwnerWatermark();
}

void PlotDocker::refreshOwnerWatermark() {
  if (focus_overlay_ == nullptr) {
    return;
  }
  if (owner_plugin_.isEmpty()) {
    focus_overlay_->setWatermarkText({});
    setToolTip({});
    return;
  }
  if (isOwnerPluginAvailable()) {
    focus_overlay_->setWatermarkText(owner_badge_);
    setToolTip({});
    return;
  }
  focus_overlay_->setWatermarkText(tr("Plugin unavailable"));
  setToolTip(tr("Plugin unavailable: %1").arg(owner_plugin_));
}

void PlotDocker::refocusAfterRemoval(DockWidget* removed) {
  DockWidget* target = nullptr;
  // Prefer the previously focused dock so the user lands back where they were.
  // QPointer keeps it null if it was deleted in the meantime.
  if (previous_dock_ != nullptr && previous_dock_ != removed) {
    target = previous_dock_;
  } else {
    // Otherwise focus the first surviving dock. ensureAtLeastOneWidget() has
    // already guaranteed at least one exists, so closing the last real widget
    // lands focus on the freshly created placeholder (an empty config panel).
    for (int i = 0; i < plotCount(); ++i) {
      if (DockWidget* dock = plotAt(i); dock != nullptr && dock != removed) {
        target = dock;
        break;
      }
    }
  }
  if (target != nullptr) {
    setDockWidgetFocused(target);
  }
}

void PlotDocker::ensureAtLeastOneWidget() {
  if (restoring_state_) {
    return;
  }
  if (dockAreaCount() != 0) {
    return;
  }
  addDockWithPlot(nullptr, ads::TopDockWidgetArea);
}

DockWidget* PlotDocker::addDockWithPlot(
    PlotWidget* plot, ads::DockWidgetArea dock_area, ads::CDockAreaWidget* relative_to) {
  auto* widget = new DockWidget(plot, session_, catalog_, this, nullptr, plot != nullptr);
  widget->setObjectWidgetFactory(object_widget_factory_);
  auto* area_widget = addDockWidget(dock_area, widget, relative_to);
  area_widget->setAllowedAreas(ads::OuterDockAreas);

  connect(widget, &DockWidget::undoableChange, this, &PlotDocker::undoableChange);
  connect(widget, &DockWidget::plotWidgetCreated, this, &PlotDocker::plotWidgetAdded);
  connect(widget, &DockWidget::objectFamilyRequested, this, &PlotDocker::objectFamilyRequested);
  connect(widget, &DockWidget::firstObjectTopicAdded, this, &PlotDocker::firstObjectTopicAdded);
  connect(widget, &DockWidget::placeholderTopicDropped, this, &PlotDocker::placeholderTopicDropped);
  emit dockAdded(widget);
  if (widget->plotWidget() != nullptr) {
    emit plotWidgetAdded(widget->plotWidget());
  }
  return widget;
}

QDomElement PlotDocker::xmlSaveState(QDomDocument& doc) const {
  QDomElement tab_element = doc.createElement(u"Tab"_s);
  tab_element.setAttribute(u"id"_s, state_id_);
  tab_element.setAttribute(u"containers"_s, dockContainers().count());
  if (history_exempt_) {
    tab_element.setAttribute(u"history_exempt"_s, u"1"_s);
  }
  if (!owner_plugin_.isEmpty()) {
    tab_element.setAttribute(u"owner_plugin"_s, owner_plugin_);
    tab_element.setAttribute(u"tab_id"_s, owner_tab_id_);
  }

  for (ads::CDockContainerWidget* container : dockContainers()) {
    QDomElement container_element = doc.createElement(u"Container"_s);
    QDomElement child =
        saveChildNodesState(doc, container->findChild<QSplitter*>(QString(), Qt::FindDirectChildrenOnly));
    if (!child.isNull()) {
      container_element.appendChild(child);
    }
    tab_element.appendChild(container_element);
  }
  return tab_element;
}

bool PlotDocker::xmlLoadState(const QDomElement& tab_element) {
  if (tab_element.isNull() || tab_element.tagName() != "Tab"_L1) {
    return false;
  }

  // Parse and validate the complete structural tree before changing the live
  // docker. Floating containers are disabled, so exactly one is restorable.
  QDomElement container;
  int container_count = 0;
  for (QDomElement child = tab_element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
    if (child.tagName() != "Container"_L1) {
      return false;
    }
    container = child;
    ++container_count;
  }
  if (container_count != 1) {
    return false;
  }
  if (tab_element.hasAttribute(u"containers"_s)) {
    bool ok = false;
    const int declared_count = tab_element.attribute(u"containers"_s).toInt(&ok);
    if (!ok || declared_count != container_count) {
      return false;
    }
  }
  const QDomElement serialized_root = container.firstChildElement();
  if (serialized_root.isNull() || !serialized_root.nextSiblingElement().isNull()) {
    return false;
  }
  const LayoutNode root_node = parseLayoutNode(serialized_root);
  if (!root_node.valid) {
    return false;
  }

  setStateId(tab_element.attribute(u"id"_s));
  if (tab_element.hasAttribute(u"tab_name"_s)) {
    setName(tab_element.attribute(u"tab_name"_s));
  }

  const bool was_hidden = isHidden();
  if (!was_hidden) {
    hide();
  }

  restoring_state_ = true;
  QVector<DockWidget*> old_docks;
  for (int index = 0; index < plotCount(); ++index) {
    if (DockWidget* dock = plotAt(index)) {
      old_docks.push_back(dock);
    }
  }

  RestorePlotPool pool(old_docks, session_, catalog_, this);
  for (DockWidget* dock : old_docks) {
    removeDockWidget(dock);
    dock->deleteLater();
  }

  // Decide top-level dock kind by peeking the first leaf's content tag. An
  // object-widget root (<scene3d>, <scene2d>, …) needs a plot-less placeholder
  // DockWidget, into which restoreNode installs the dock via the factory.
  const QDomElement root_element = firstLeafElement(root_node);
  const bool root_is_object = isObjectWidgetElement(root_element);
  const bool root_is_placeholder = isPlaceholderElement(root_element);
  DockWidget* root_widget = nullptr;
  if (root_is_object || root_is_placeholder) {
    root_widget = addDockWithPlot(nullptr, ads::TopDockWidgetArea);
  } else {
    PlotWidget* root_plot = pool.takeFirstForNode(root_node);
    root_widget = addDockWithPlot(root_plot, ads::TopDockWidgetArea);
  }
  const bool restored = restoreNode(root_node, root_widget, pool, object_widget_factory_);
  watchSplitters();
  pool.deleteUnused();

  restoring_state_ = false;
  ensureAtLeastOneWidget();
  if (!was_hidden) {
    show();
  }
  return restored;
}

int PlotDocker::plotCount() const {
  return dockAreaCount();
}

DockWidget* PlotDocker::plotAt(int index) {
  return dynamic_cast<DockWidget*>(dockArea(index)->currentDockWidget());
}

DockWidget* PlotDocker::focusedDock() const {
  return focused_dock_.data();
}

void PlotDocker::focusDock(DockWidget* dock) {
  if (dock == nullptr) {
    return;
  }
  if (dock == focused_dock_) {
    // Already focused: ADS won't re-emit focusedDockWidgetChanged, but the
    // dock's content just changed (e.g. a drop populated a focused placeholder
    // after every widget was closed), so refresh listeners directly.
    emit dockFocused(dock);
  } else {
    setDockWidgetFocused(dock);
  }
}

DockWidget* PlotDocker::fullscreenDock() const {
  return fullscreen_dock_;
}

void PlotDocker::toggleFullscreen(DockWidget* dock) {
  if (fullscreen_dock_) {
    // Exit: re-show exactly the docks the maximize hid (those still alive).
    // toggleView(true) re-shows each dock and its collapsed parent splitters,
    // restoring the layout the maximize folded away.
    for (const QPointer<ads::CDockWidget>& hidden : hidden_by_fullscreen_) {
      if (hidden) {
        hidden->toggleView(true);
      }
    }
    hidden_by_fullscreen_.clear();
    fullscreen_dock_ = nullptr;
    return;
  }
  if (dock == nullptr) {
    return;
  }
  // Enter: hide every currently shown dock except the target. toggleView(false)
  // routes through ADS' hideDockWidget -> hideEmptyParentSplitters, collapsing
  // the emptied branches so the survivor reflows to fill the tab. (It only
  // deletes content under DeleteContentOnClose, not the DeleteOnClose flag the
  // docks carry, so the hidden docks survive to be restored.)
  // openedDockWidgets() (not dockWidgetsMap(), which is keyed by objectName —
  // every PJ4 dock is named "Plot", so the map collapses to a single entry and
  // only one sibling would hide).
  hidden_by_fullscreen_.clear();
  for (ads::CDockWidget* other : openedDockWidgets()) {
    if (other == dock) {
      continue;
    }
    other->toggleView(false);
    hidden_by_fullscreen_.append(other);
  }
  fullscreen_dock_ = dock;
}

void PlotDocker::onStylesheetChanged(QString theme) {
  for (int index = 0; index < plotCount(); ++index) {
    if (auto* dock = plotAt(index)) {
      dock->onStylesheetChanged(theme);
    }
  }
}

}  // namespace PJ
