// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/DockWidget.h"

#include <DockAreaWidget.h>
#include <DockManager.h>

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QContextMenuEvent>
#include <QDomDocument>
#include <QDomElement>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMetaObject>
#include <QMimeData>
#include <QPushButton>
#include <QScopedValueRollback>
#ifdef PJ_TARGET_WASM
#include <QPointer>
#include <QTimer>
#endif
#include <QUuid>
#include <QWidget>
#include <utility>

#include "WidgetClipboard.h"
#include "pj_plotting/DockToolbar.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SvgUtil.h"
#include "pj_widgets/VisualizationPlaceholderWidget.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

QString newStateId() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool isContentWidgetOrChild(QObject* watched, QWidget* content_widget) {
  auto* widget = qobject_cast<QWidget*>(watched);
  return widget != nullptr && content_widget != nullptr &&
         (widget == content_widget || content_widget->isAncestorOf(widget));
}

void addActionCategorySeparator(QMenu& menu) {
  const QList<QAction*> actions = menu.actions();
  if (!actions.isEmpty() && !actions.constLast()->isSeparator()) {
    menu.addSeparator();
  }
}

QDomElement clipboardWidgetElement(QDomDocument& doc) {
  const QString payload = widget_clipboard::xml();
  if (payload.isEmpty() || !doc.setContent(payload)) {
    return {};
  }
  return doc.documentElement();
}

bool objectElementSeedsWidget(const QDomElement& element) {
  return !element.firstChildElement(u"layer"_s).isNull() || !element.firstChildElement(u"config_topic"_s).isNull();
}

}  // namespace

DockWidget::DockWidget(SessionManager* session, CatalogModel* catalog, ads::CDockManager* manager, QWidget* parent)
    : DockWidget(nullptr, session, catalog, manager, parent, false) {}

DockWidget::DockWidget(
    PlotWidget* plot, SessionManager* session, CatalogModel* catalog, ads::CDockManager* manager, QWidget* parent,
    bool create_plot_when_null)
    : ads::CDockWidget(manager, "Plot", parent != nullptr ? parent : manager),
      session_(session),
      catalog_(catalog),
      state_id_(newStateId()) {
  setFrameShape(QFrame::NoFrame);

  setFeature(ads::CDockWidget::DockWidgetFloatable, false);
  setFeature(ads::CDockWidget::DockWidgetDeleteOnClose, true);

  toolbar_ = new DockToolbar(this);
  toolbar_->label()->setText("...");
  qobject_cast<QBoxLayout*>(layout())->insertWidget(0, toolbar_);

  connect(toolbar_->buttonSplitHorizontal(), &QPushButton::clicked, this, [this]() { splitHorizontal(); });
  connect(toolbar_->buttonSplitVertical(), &QPushButton::clicked, this, [this]() { splitVertical(); });

  auto fullscreen_action = [this]() {
    auto* parent_docker = qobject_cast<PlotDocker*>(dockManager());
    if (!parent_docker) {
      return;
    }
    parent_docker->toggleFullscreen(this);
    toolbar_->setFullscreen(parent_docker->fullscreenDock() == this);
  };
  connect(toolbar_->buttonFullscreen(), &QPushButton::clicked, this, fullscreen_action);

  connect(toolbar_->buttonClose(), &QPushButton::pressed, this, [this]() {
    dockAreaWidget()->closeArea();
    clearCurrentContent(true);
    emit undoableChange();
  });

  layout()->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  if (plot != nullptr || create_plot_when_null) {
    setPlotWidget(plot != nullptr ? plot : new PlotWidget(session_, catalog_, this));
  } else {
    setPlaceholderWidget();
  }
}

DockWidget::~DockWidget() = default;

void DockWidget::setDataServices(SessionManager* session, CatalogModel* catalog) {
  session_ = session;
  catalog_ = catalog;
  if (plot_widget_ != nullptr) {
    plot_widget_->setDataServices(session_, catalog_);
  }
}

void DockWidget::setObjectWidgetFactory(ObjectWidgetFactory factory) {
  object_widget_factory_ = std::move(factory);
  updatePlaceholderPasteAction();
}

PlotWidget* DockWidget::plotWidget() {
  return plot_widget_;
}

IDataWidget* DockWidget::objectWidget() {
  return object_widget_;
}

PlotWidget* DockWidget::releasePlotWidget() {
  if (plot_widget_ == nullptr) {
    return nullptr;
  }
  disconnect(plot_widget_, nullptr, this, nullptr);
  auto* plot = plot_widget_;
  takeWidget();
  content_widget_ = nullptr;
  plot_widget_ = nullptr;
  return plot;
}

void DockWidget::setPlotWidget(PlotWidget* plot) {
  if (plot_widget_ == plot) {
    return;
  }
  clearCurrentContent(true);
  plot_widget_ = plot;
  content_widget_ = plot_widget_;
  if (plot_widget_ == nullptr) {
    return;
  }
  plot_widget_->setDataServices(session_, catalog_);
  setWidget(plot_widget_);
  connect(plot_widget_, &PlotWidget::splitHorizontal, this, [this]() { splitHorizontal(); });
  connect(plot_widget_, &PlotWidget::splitVertical, this, [this]() { splitVertical(); });
  connect(plot_widget_, &PlotWidget::undoableChange, this, &DockWidget::undoableChange);
  emit plotWidgetCreated(plot_widget_);
}

void DockWidget::setObjectWidget(IDataWidget* widget) {
  if (object_widget_ == widget) {
    return;
  }
  clearCurrentContent(true);
  object_widget_ = widget;
  content_widget_ = object_widget_ != nullptr ? object_widget_->widget() : nullptr;
  if (content_widget_ == nullptr) {
    return;
  }
  installObjectContextMenuFilter(content_widget_);
  // ForceNoScrollArea: object widgets manage their own viewport — wrapping
  // them in ADS' QScrollArea adds a visible frame. Mirrors the drop path.
  setWidget(content_widget_, ads::CDockWidget::ForceNoScrollArea);
}

IDataWidget* DockWidget::releaseObjectWidget() {
  if (object_widget_ == nullptr) {
    return nullptr;
  }
  if (content_widget_ != nullptr) {
    removeObjectContextMenuFilter(content_widget_);
  }
  auto* obj = object_widget_;
  takeWidget();
  content_widget_ = nullptr;
  object_widget_ = nullptr;
  return obj;
}

void DockWidget::setPlaceholderWidget() {
  clearCurrentContent(true);
  placeholder_widget_ = new VisualizationPlaceholderWidget(this);
  content_widget_ = placeholder_widget_;
  setWidget(placeholder_widget_);
  setName(u"..."_s);
  connect(
      placeholder_widget_, &VisualizationPlaceholderWidget::catalogItemsDropped, this,
      &DockWidget::onCatalogItemsDropped);
  connect(
      placeholder_widget_, &VisualizationPlaceholderWidget::catalogItemsXyRequested, this,
      &DockWidget::onCatalogItemsXyRequested);
  connect(placeholder_widget_, &VisualizationPlaceholderWidget::splitHorizontalRequested, this, [this]() {
    splitHorizontal();
  });
  connect(placeholder_widget_, &VisualizationPlaceholderWidget::splitVerticalRequested, this, [this]() {
    splitVertical();
  });
  connect(
      placeholder_widget_, &VisualizationPlaceholderWidget::visualizationRequested, this,
      &DockWidget::onVisualizationRequested);
  connect(
      placeholder_widget_, &VisualizationPlaceholderWidget::pasteRequested, this,
      &DockWidget::pastePlaceholderWidgetFromClipboard);
  connect(
      placeholder_widget_, &VisualizationPlaceholderWidget::contextMenuAboutToShow, this,
      &DockWidget::updatePlaceholderPasteAction);
  updatePlaceholderPasteAction();
}

void DockWidget::onVisualizationRequested(VisualizationKind kind) {
  if (kind == VisualizationKind::kPlot) {
    // This dock owns plot widgets, so it builds the empty plot itself.
    ensurePlotWidget();
    emit undoableChange();
    focusSelf();
    return;
  }
  // Scene families: the shell owns the family→kind mapping and dock construction.
  // It builds the empty widget and hands it back via adoptObjectWidget().
  emit objectFamilyRequested(this, kind);
}

void DockWidget::adoptObjectWidget(IDataWidget* widget) {
  if (widget == nullptr) {
    // Build failed (unknown kind / factory refusal): keep a usable placeholder.
    setPlaceholderWidget();
    return;
  }
  setObjectWidget(widget);
  // setObjectWidget cleared the gate via clearCurrentContent; arm it now so the
  // first topic dropped into this empty widget seeds streaming playback.
  object_widget_awaiting_first_topic_ = true;
  setName(u"..."_s);
  emit undoableChange();
  focusSelf();
}

DockToolbar* DockWidget::toolBar() {
  return toolbar_;
}

QString DockWidget::name() const {
  return toolbar_->label()->text();
}

void DockWidget::setName(const QString& name) {
  toolbar_->label()->setText(name);
}

QString DockWidget::stateId() const {
  return state_id_;
}

void DockWidget::setStateId(QString id) {
  if (!id.isEmpty()) {
    state_id_ = std::move(id);
  }
}

void DockWidget::onTrackerTime(double time) {
  if (plot_widget_ != nullptr) {
    plot_widget_->setTrackerPosition(time);
  }
  if (object_widget_ != nullptr) {
    object_widget_->onTrackerTime(time);
  }
}

void DockWidget::onStylesheetChanged(QString theme) {
  if (toolbar_ != nullptr) {
    toolbar_->onStylesheetChanged(theme);
  }
  if (placeholder_widget_ != nullptr) {
    placeholder_widget_->onStylesheetChanged(theme);
  }
}

bool DockWidget::eventFilter(QObject* watched, QEvent* event) {
  if (object_widget_ != nullptr && content_widget_ != nullptr && isContentWidgetOrChild(watched, content_widget_) &&
      event != nullptr) {
    switch (event->type()) {
      case QEvent::ContextMenu: {
        if (forwarding_context_menu_) {
          break;  // pass-through: the re-sent event must reach the widget's own handler
        }
        auto* context_event = static_cast<QContextMenuEvent*>(event);
        // First shot goes to the content widget's own contextMenuEvent (e.g.
        // the 3D view's frame-gizmo menu). Only an IGNORED event falls back to
        // the dock's standard menu — same fall-through contract as plain Qt
        // parent-chain propagation, which this filter would otherwise defeat.
        {
          QScopedValueRollback<bool> guard(forwarding_context_menu_, true);
          QContextMenuEvent forwarded(
              context_event->reason(), context_event->pos(), context_event->globalPos(), context_event->modifiers());
          forwarded.ignore();
          QApplication::sendEvent(watched, &forwarded);
          if (forwarded.isAccepted()) {
            event->accept();
            return true;  // the widget showed its own menu; swallow the original
          }
        }
        showObjectContextMenu(context_event->globalPos());
        event->accept();
        return true;
      }
      // Catalog drops on the *live* content widget — once the placeholder
      // is gone, this filter is the only thing that hears the drop. Used by
      // multi-topic widgets (Scene3D) to absorb additional topics; a topic the
      // committed widget can't host is rejected by onCatalogItemsDropped (no
      // family-switching replacement — that only happens from the placeholder).
      case QEvent::DragEnter:
      case QEvent::DragMove: {
        auto* drag = static_cast<QDropEvent*>(event);
        if (drag->mimeData() != nullptr && drag->mimeData()->hasFormat(CurveTreeView::catalogItemsMimeType())) {
          drag->acceptProposedAction();
          return true;
        }
        return false;
      }
      case QEvent::Drop: {
        auto* drop = static_cast<QDropEvent*>(event);
        const QStringList keys = CurveTreeView::decodeCatalogKeys(drop->mimeData());
        if (keys.isEmpty()) {
          return false;
        }
        drop->acceptProposedAction();
        onCatalogItemsDropped(keys);
        return true;
      }
      default:
        break;
    }
  }
  return ads::CDockWidget::eventFilter(watched, event);
}

DockWidget* DockWidget::splitHorizontal() {
  return splitHorizontal(nullptr);
}

DockWidget* DockWidget::splitVertical() {
  return splitVertical(nullptr);
}

DockWidget* DockWidget::splitHorizontal(PlotWidget* plot) {
  return splitInto(ads::RightDockWidgetArea, plot);
}

DockWidget* DockWidget::splitVertical(PlotWidget* plot) {
  return splitInto(ads::BottomDockWidgetArea, plot);
}

DockWidget* DockWidget::splitInto(ads::DockWidgetArea dock_area, PlotWidget* plot) {
  auto* parent_docker = qobject_cast<PlotDocker*>(dockManager());
  if (!parent_docker) {
    return nullptr;
  }
  auto* new_widget = new DockWidget(plot, session_, catalog_, parent_docker, nullptr, false);
  new_widget->setObjectWidgetFactory(object_widget_factory_);
  auto* area = parent_docker->addDockWidget(dock_area, new_widget, dockAreaWidget());
  area->setAllowedAreas(ads::OuterDockAreas);

  connect(new_widget, &DockWidget::undoableChange, parent_docker, &PlotDocker::undoableChange);
  connect(new_widget, &DockWidget::plotWidgetCreated, parent_docker, &PlotDocker::plotWidgetAdded);
  connect(new_widget, &DockWidget::objectFamilyRequested, parent_docker, &PlotDocker::objectFamilyRequested);
  connect(new_widget, &DockWidget::firstObjectTopicAdded, parent_docker, &PlotDocker::firstObjectTopicAdded);
  connect(new_widget, &DockWidget::placeholderTopicDropped, parent_docker, &PlotDocker::placeholderTopicDropped);
  emit undoableChange();
  emit parent_docker->dockAdded(new_widget);
  if (new_widget->plotWidget() != nullptr) {
    emit parent_docker->plotWidgetAdded(new_widget->plotWidget());
  }
  return new_widget;
}

PlotWidget* DockWidget::ensurePlotWidget() {
  if (plot_widget_ == nullptr) {
    setPlotWidget(new PlotWidget(session_, catalog_, this));
    setName(u"..."_s);
  }
  return plot_widget_;
}

void DockWidget::onCatalogItemsDropped(const QStringList& keys) {
  if (catalog_ == nullptr || keys.empty()) {
    return;
  }

  const auto first_item = catalog_->itemDescriptor(keys.front());
  if (!first_item.has_value()) {
    return;
  }

  if (const auto* advertised = asAdvertisedTopic(*first_item); advertised != nullptr) {
    // Multi-select drop: the FIRST placeholder decides the branch (scalar vs
    // object); every other advertised key of the same shape rides along, so a
    // two-cloud drop pends both. Keys of the other shape are dropped silently
    // (exactly like non-plottables in a mixed real-topic drop).
    const auto for_each_advertised = [&](sdk::BuiltinObjectType wanted, auto&& emit_drop) {
      for (const QString& key : keys) {
        const auto item = catalog_->itemDescriptor(key);
        if (!item.has_value()) {
          continue;
        }
        const auto* adv = asAdvertisedTopic(*item);
        if (adv == nullptr) {
          continue;
        }
        const bool is_scalar_shaped = adv->classification == sdk::BuiltinObjectType::kNone;
        if (is_scalar_shaped == (wanted == sdk::BuiltinObjectType::kNone)) {
          emit_drop(*item, adv->classification);
        }
      }
    };

    if (advertised->classification == sdk::BuiltinObjectType::kNone) {
      // Scalar-shaped placeholder: materialize the plot lazily (mirrors the real
      // scalar branch below) so the pending bind has somewhere to land, but let
      // the shell register demand instead of fabricating a curve.
      if (object_widget_ != nullptr) {
        return;  // an object dock hosts no scalar curves
      }
      PlotWidget* plot = ensurePlotWidget();
      if (plot == nullptr) {
        return;
      }
      for_each_advertised(sdk::BuiltinObjectType::kNone, [&](const auto& item, sdk::BuiltinObjectType type) {
        emit placeholderTopicDropped(this, item.dataset_id, item.topic_name, type);
      });
      focusSelf();
      return;
    }
    // Object-shaped placeholder: a committed plot dock hosts no object topics —
    // reject rather than replacing the plot (and its curves) with an object view.
    if (plot_widget_ != nullptr) {
      return;
    }
    if (object_widget_ == nullptr) {
      // Empty tile: materialize a dock of the family the shell resolves from the
      // classification. The seed carries a null storage id — the topic has no
      // data (and thus no ObjectTopicId) until the demand registered by the drop
      // below starts the subscription; the pending drop then completes against
      // the real id (TopicDemandController::handleSceneDockPlaceholderDrop).
      if (!object_widget_factory_) {
        return;
      }
      const ObjectDropSeed seed{ObjectTopicId{}, advertised->classification, first_item->topic_name};
      IDataWidget* widget = object_widget_factory_(QString(), &seed, this);
      if (widget == nullptr) {
        return;  // family not hostable — keep the placeholder affordance
      }
      setObjectWidget(widget);
      // Arm the gate so the first real topic (a later drop into this widget)
      // seeds streaming playback, mirroring the click-create path.
      object_widget_awaiting_first_topic_ = true;
      setName(u"..."_s);
      emit undoableChange();
      focusSelf();
    }
    for_each_advertised(advertised->classification, [&](const auto& item, sdk::BuiltinObjectType type) {
      emit placeholderTopicDropped(this, item.dataset_id, item.topic_name, type);
    });
    return;
  }

  if (isScalarField(*first_item)) {
    // A committed object dock hosts no plot curves — but a widget that consumes
    // scalar series itself (the state-transitions strip) may absorb the keys in
    // place. Refused ⇒ reject the drop rather than replacing the object view.
    if (object_widget_ != nullptr) {
      if (object_widget_->tryAcceptSeriesKeys(keys)) {
        emit undoableChange();
        focusSelf();
      }
      return;
    }
    // Materialize the plot lazily — only once a dropped key is actually a
    // plottable curve. String fields are catalog scalars (isScalarField) but have
    // no curveDescriptor, so a drop of only string fields must NOT convert a
    // placeholder into a blank, curveless plot.
    PlotWidget* plot = nullptr;
    const bool was_empty = plotWidget() == nullptr || plotWidget()->isEmpty();
    bool changed = false;
    for (const QString& key : keys) {
      if (!catalog_->curveDescriptor(key).has_value()) {
        continue;
      }
      if (plot == nullptr) {
        plot = ensurePlotWidget();
      }
      changed = plot->addCurve(key) != nullptr || changed;
    }
    if (plot == nullptr) {
      // Nothing plottable — but a discrete-only drop on an empty tile
      // materializes the state-transitions strip instead of dying. (Plottable
      // wins the tie: an integer/bool key creates a plot above and never
      // reaches here; only strings are discrete-and-not-plottable today.)
      maybeCreateStateTransitionsFromDrop(keys);
      return;
    }
    if (changed) {
      plot->zoomOut(false);
      emit plot->curvesDropped(was_empty);
      emit undoableChange();
    }
    focusSelf();
    return;
  }

  const auto* object_payload = asObjectTopic(*first_item);
  if (object_payload == nullptr || !object_widget_factory_) {
    return;
  }

  // Helper: build the title string the factory expects from a catalog item.
  // With a single dataset loaded the dataset name is redundant noise in the
  // object lists, so show just the topic; with multiple datasets keep the
  // "dataset/topic" qualifier to disambiguate.
  const bool single_dataset = (catalog_ != nullptr) && catalog_->datasets().size() <= 1;
  const auto title_for = [single_dataset](const auto& descriptor) {
    return (single_dataset || descriptor.dataset_name.isEmpty())
               ? descriptor.topic_name
               : u"%1/%2"_s.arg(descriptor.dataset_name, descriptor.topic_name);
  };
  // Helper: walk all keys and offer each one to the given widget via
  // IDataWidget::tryAcceptObjectTopic. Returns the count accepted.
  const auto offer_keys_to = [&](IDataWidget* target, int start_index) {
    int accepted = 0;
    for (int i = start_index; i < keys.size(); ++i) {
      const auto desc = catalog_->itemDescriptor(keys[i]);
      if (!desc.has_value()) {
        continue;
      }
      const auto* payload = asObjectTopic(*desc);
      if (payload == nullptr) {
        continue;
      }
      if (target->tryAcceptObjectTopic(payload->object_topic_id, payload->object_type, title_for(*desc))) {
        ++accepted;
      }
    }
    return accepted;
  };

  // First: if a widget is already mounted, try to add the dropped topics
  // *into* it (Scene3DDockWidget consumes pointcloud / TF this way). On
  // success we don't replace the widget — only the topic list grows.
  if (object_widget_ != nullptr) {
    if (offer_keys_to(object_widget_, /*start_index=*/0) > 0) {
      if (object_widget_awaiting_first_topic_) {
        // First topic into an empty click-created dock: seed streaming playback,
        // which the placeholder→drop path otherwise does at creation. Fires once
        // per dock. The dock keeps its "..." name — dragging a topic does NOT
        // rename the dock (the user renames it explicitly via the title if wanted).
        object_widget_awaiting_first_topic_ = false;
        emit firstObjectTopicAdded();
      }
      emit undoableChange();
      focusSelf();
      return;
    }
    // A committed object dock refused the dropped topic(s) — a different family
    // (e.g. an image dropped on a 3D view, or a pointcloud on a 2D view). Reject
    // the drop rather than silently replacing the dock with another family; the
    // user switches families via Clear or the placeholder icons, not by drop.
    return;
  }

  // A committed plot dock hosts no object topics — reject rather than replacing
  // the plot (and its curves) with an object view.
  if (plot_widget_ != nullptr) {
    return;
  }

  // Placeholder state only: the factory classifies the object type and builds the
  // matching dock. A null return ("I can't render this") reverts to the
  // placeholder so the user sees an explicit "not supported" affordance.
  const QString title = title_for(*first_item);
  clearCurrentContent(true);
  // Drop path: empty kind + a seed for the first topic. The factory classifies
  // the object type, constructs the matching dock, and populates the seed.
  const ObjectDropSeed seed{object_payload->object_topic_id, object_payload->object_type, title};
  object_widget_ = object_widget_factory_(QString(), &seed, this);
  content_widget_ = object_widget_ != nullptr ? object_widget_->widget() : nullptr;
  if (content_widget_ == nullptr) {
    setPlaceholderWidget();
    return;
  }
  installObjectContextMenuFilter(content_widget_);
  // ForceNoScrollArea: object widgets (Scene3DDockWidget, Scene2DDockWidget,
  // …) manage their own viewport — wrapping them in ADS' QScrollArea adds a
  // visible frame around the content. The object widget is responsible for
  // its own sizing/scrolling if any is needed.
  setWidget(content_widget_, ads::CDockWidget::ForceNoScrollArea);
  // The dock keeps its default "..." name — dragging a topic does NOT rename it.
  // Multi-select drop: hand the remaining keys to the new widget so it
  // can absorb the rest of the selection (Scene3D / future multi-topic
  // viewers benefit; single-topic widgets refuse and we drop them
  // silently — the user still got their first topic shown).
  if (object_widget_ != nullptr && keys.size() > 1) {
    offer_keys_to(object_widget_, /*start_index=*/1);
  }
  emit undoableChange();
  focusSelf();
}

void DockWidget::maybeCreateStateTransitionsFromDrop(const QStringList& keys) {
  // Only from an empty tile, and only when the drop actually carries a discrete
  // series; the factory owns construction + wiring (the shell's one construct
  // site), seeded through the same tryAcceptSeriesKeys hook a mounted strip uses.
  if (plot_widget_ != nullptr || object_widget_ != nullptr || !object_widget_factory_ || catalog_ == nullptr) {
    return;
  }
  const bool any_discrete =
      std::ranges::any_of(keys, [this](const QString& key) { return catalog_->isDiscreteKey(key); });
  if (!any_discrete) {
    return;
  }
  IDataWidget* widget = object_widget_factory_(u"state_transitions"_s, nullptr, this);
  if (widget == nullptr || widget->widget() == nullptr) {
    return;  // family not hostable — keep the placeholder affordance
  }
  setObjectWidget(widget);
  setName(u"..."_s);
  widget->tryAcceptSeriesKeys(keys);
  emit undoableChange();
  focusSelf();
}

void DockWidget::onCatalogItemsXyRequested(const QStringList& keys) {
  // A right-drag of two curves onto an empty placeholder: build a plot and create
  // an XY (scatter) curve from the pair — the same gesture the live plot canvas
  // handles, here routed through the placeholder.
  if (catalog_ == nullptr || keys.size() != 2 || object_widget_ != nullptr) {
    return;
  }
  if (!catalog_->curveDescriptor(keys.front()).has_value() || !catalog_->curveDescriptor(keys.back()).has_value()) {
    return;
  }
#ifdef PJ_TARGET_WASM
  const bool created_plot = plot_widget_ == nullptr;
#endif
  PlotWidget* plot = ensurePlotWidget();
  if (plot == nullptr) {
    return;
  }
  plot->setModeXY(true);
#ifdef PJ_TARGET_WASM
  const QPointer<DockWidget> guard(this);
  const QPointer<PlotWidget> pending_plot(plot);
  plot->createCurveXYInteractiveAsync(
      keys.front(), keys.back(), [guard, pending_plot, created_plot](PlotWidget::CurveInfo* created) {
        if (guard.isNull() || pending_plot.isNull() || guard->plot_widget_ != pending_plot.data()) {
          return;
        }
        if (created != nullptr) {
          pending_plot->zoomOut(true);
          emit guard->undoableChange();
          guard->focusSelf();
          return;
        }
        // Do not schedule the just-cancelled plot for deletion while its dialog's
        // finished handler is still on that plot's stack.
        QTimer::singleShot(0, guard, [guard, pending_plot, created_plot]() {
          if (created_plot && !guard.isNull() && !pending_plot.isNull() && guard->plot_widget_ == pending_plot.data() &&
              pending_plot->curveList().empty()) {
            guard->setPlaceholderWidget();
          }
        });
      });
#else
  if (plot->createCurveXYInteractive(keys.front(), keys.back()) != nullptr) {
    plot->zoomOut(true);
    emit undoableChange();
    focusSelf();
  } else {
    // Cancelled in the dialog: drop the just-created empty plot back to a placeholder.
    setPlaceholderWidget();
  }
#endif
}

void DockWidget::clearToPlaceholder() {
  setPlaceholderWidget();
  emit undoableChange();
}

void DockWidget::focusSelf() {
  if (auto* docker = qobject_cast<PlotDocker*>(dockManager()); docker != nullptr) {
    docker->focusDock(this);
  }
}

QString DockWidget::objectWidgetClipboardTag() const {
  if (object_widget_ == nullptr) {
    return {};
  }
  QDomDocument doc(u"plotjuggler_widget"_s);
  const QDomElement element = object_widget_->xmlSaveState(doc);
  return element.isNull() ? QString() : element.tagName();
}

void DockWidget::copyObjectWidgetToClipboard() {
  if (object_widget_ == nullptr) {
    return;
  }
  QDomDocument doc(u"plotjuggler_widget"_s);
  QDomElement element = object_widget_->xmlSaveState(doc);
  if (element.isNull()) {
    return;
  }
  doc.appendChild(element);
  widget_clipboard::setXml(doc.toString(2));
}

void DockWidget::pasteObjectWidgetFromClipboard() {
  if (object_widget_ == nullptr) {
    return;
  }
  QDomDocument doc;
  if (!widget_clipboard::parse(doc, objectWidgetClipboardTag())) {
    return;
  }
  const QDomElement element = doc.documentElement();
  const bool seeds_empty_click_created_widget =
      object_widget_awaiting_first_topic_ &&
      (!element.firstChildElement(u"layer"_s).isNull() || !element.firstChildElement(u"config_topic"_s).isNull());
  if (object_widget_->xmlLoadState(element)) {
    if (seeds_empty_click_created_widget) {
      object_widget_awaiting_first_topic_ = false;
      emit firstObjectTopicAdded();
    }
    emit undoableChange();
    focusSelf();
  }
}

bool DockWidget::canPasteObjectWidgetFromClipboard() const {
  QDomDocument doc;
  return widget_clipboard::parse(doc, objectWidgetClipboardTag());
}

void DockWidget::pastePlaceholderWidgetFromClipboard() {
  if (placeholder_widget_ == nullptr) {
    return;
  }
  QDomDocument doc;
  const QDomElement element = clipboardWidgetElement(doc);
  if (element.isNull()) {
    return;
  }

  if (element.tagName() == "plot"_L1) {
    PlotWidget* plot = ensurePlotWidget();
    if (plot == nullptr) {
      return;
    }
    QMetaObject::invokeMethod(plot, "pasteWidgetFromClipboard", Qt::DirectConnection);
    focusSelf();
    return;
  }

  if (!object_widget_factory_) {
    return;
  }
  IDataWidget* widget = object_widget_factory_(element.tagName(), nullptr, this);
  if (widget == nullptr) {
    return;
  }
  setObjectWidget(widget);
  setName(u"..."_s);
  const bool seeded = objectElementSeedsWidget(element);
  if (!object_widget_->xmlLoadState(element)) {
    setPlaceholderWidget();
    return;
  }
  object_widget_awaiting_first_topic_ = !seeded;
  if (seeded) {
    emit firstObjectTopicAdded();
  }
  emit undoableChange();
  focusSelf();
}

void DockWidget::updatePlaceholderPasteAction() {
  if (placeholder_widget_ != nullptr) {
    placeholder_widget_->setPasteActionEnabled(canPastePlaceholderWidgetFromClipboard());
  }
}

bool DockWidget::canPastePlaceholderWidgetFromClipboard() const {
  QDomDocument doc;
  const QDomElement element = clipboardWidgetElement(doc);
  if (element.isNull()) {
    return false;
  }
  if (element.tagName() == "plot"_L1) {
    return true;
  }
  return widget_clipboard::hasWidgetXml() && object_widget_factory_;
}

void DockWidget::clearCurrentContent(bool delete_content) {
  if (plot_widget_ != nullptr) {
    disconnect(plot_widget_, nullptr, this, nullptr);
  }
  if (placeholder_widget_ != nullptr) {
    disconnect(placeholder_widget_, nullptr, this, nullptr);
  }
  if (content_widget_ != nullptr) {
    removeObjectContextMenuFilter(content_widget_);
    takeWidget();
    if (delete_content) {
      content_widget_->deleteLater();
    }
  }
  content_widget_ = nullptr;
  placeholder_widget_ = nullptr;
  plot_widget_ = nullptr;
  object_widget_ = nullptr;
  object_widget_awaiting_first_topic_ = false;
}

void DockWidget::installObjectContextMenuFilter(QWidget* root) {
  if (root == nullptr) {
    return;
  }
  root->installEventFilter(this);
  // Enable drop-target status on the root content widget so subsequent
  // catalog drags surface DragEnter/Drop events here (eventFilter then
  // routes them to onCatalogItemsDropped). Children typically refuse drops
  // and Qt walks up the parent chain to find this accepting root, so we
  // don't need to flip every descendant — only the root.
  root->setAcceptDrops(true);
  const auto children = root->findChildren<QWidget*>();
  for (auto* child : children) {
    child->installEventFilter(this);
  }
}

void DockWidget::removeObjectContextMenuFilter(QWidget* root) {
  if (root == nullptr) {
    return;
  }
  root->removeEventFilter(this);
  const auto children = root->findChildren<QWidget*>();
  for (auto* child : children) {
    child->removeEventFilter(this);
  }
}

void DockWidget::showObjectContextMenu(const QPoint& global_pos) {
  const QString theme = currentTheme();
#ifdef PJ_TARGET_WASM
  // QMenu::exec() needs a nested event loop, which the Asyncify-free browser
  // build cannot enter. Retain the popup until it closes; action wiring stays
  // identical to the desktop menu below.
  auto* wasm_menu = new QMenu(this);
  wasm_menu->setAttribute(Qt::WA_DeleteOnClose);
  QMenu& menu = *wasm_menu;
#else
  QMenu menu(this);
#endif
  menu.setObjectName(u"PJMenu"_s);
  menu.setProperty("categorySeparators", true);
  QAction* copy_action = menu.addAction(
      QIcon(loadSvg(":/resources/svg/copy.svg", theme)), tr("Copy"), this, [this]() { copyObjectWidgetToClipboard(); });
  copy_action->setEnabled(!objectWidgetClipboardTag().isEmpty());
  QAction* paste_action = menu.addAction(
      QIcon(loadSvg(":/resources/svg/paste.svg", theme)), tr("Paste"), this,
      [this]() { pasteObjectWidgetFromClipboard(); });
  paste_action->setEnabled(canPasteObjectWidgetFromClipboard());
  addActionCategorySeparator(menu);

  menu.addAction(QIcon(loadSvg(":/resources/svg/add_column.svg", theme)), tr("Split Horizontally"), this, [this]() {
    splitHorizontal();
  });
  menu.addAction(QIcon(loadSvg(":/resources/svg/add_row.svg", theme)), tr("Split Vertically"), this, [this]() {
    splitVertical();
  });
  addActionCategorySeparator(menu);
  menu.addAction(
      QIcon(loadSvg(":/resources/svg/clear.svg", theme)), tr("Clear"), this, [this]() { clearToPlaceholder(); });
#ifdef PJ_TARGET_WASM
  menu.popup(global_pos);
#else
  menu.exec(global_pos);
#endif
}

}  // namespace PJ
