// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "CustomWidgetController.h"

#include <QCursor>
#include <QPoint>
#include <QStackedWidget>
#include <QStringList>
#include <QToolButton>
#include <utility>

#include "pj_custom_widgets/CustomWidgetDockWidget.h"
#include "pj_marketplace/extension_manager.hpp"
#include "pj_plotting/DockWidget.h"
#include "pj_runtime/ActiveWidgetsMarker.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/DiagnosticEmit.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_widgets/MessageBox.h"
#include "ui/CustomWidgetConfigPanel.h"
#include "ui/CustomWidgetPickerMenu.h"

using namespace Qt::StringLiterals;

namespace PJ {

CustomWidgetController::CustomWidgetController(
    QStackedWidget& pages, AppSession& session, DiagnosticSink sink, Hooks hooks, QWidget* dialog_parent)
    : QObject(&pages),
      pages_(pages),
      session_(session),
      sink_(std::move(sink)),
      hooks_(std::move(hooks)),
      dialog_parent_(dialog_parent) {
#ifndef PJ_TARGET_WASM
  marker_ = std::make_shared<ActiveWidgetsMarker>(ActiveWidgetsMarker::defaultDirectory());
#endif
  picker_ = new CustomWidgetPickerMenu(dialog_parent_);
  connect(picker_, &CustomWidgetPickerMenu::widgetChosen, this, &CustomWidgetController::onWidgetChosen);
  config_page_ = new CustomWidgetConfigPanel(&pages_);
  config_page_->setServices(&session_.catalogModel(), &session_.extensionCatalog());
  pages_.addWidget(config_page_);
}

CustomWidgetController::~CustomWidgetController() = default;

void CustomWidgetController::onStylesheetChanged(const QString& theme) {
  picker_->onStylesheetChanged(theme);
  config_page_->onStylesheetChanged(theme);
}

void CustomWidgetController::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  config_page_->onChromeMetricsChanged(metrics);
}

void CustomWidgetController::showPicker(DockWidget* dock) {
  target_ = dock;
  picker_->rebuild(session_.extensionCatalog());
  const QToolButton* button = dock->findChild<QToolButton*>(u"buttonVizCustomWidget"_s);
  const QPoint anchor = button != nullptr ? button->mapToGlobal(QPoint(0, button->height())) : QCursor::pos();
  picker_->popup(anchor);
}

void CustomWidgetController::onWidgetChosen(const QString& plugin_id, const QString& type) {
  DockWidget* dock = target_.data();
  // The panel may have been closed, or filled some other way, while the picker was open.
  if (dock == nullptr || dock->plotWidget() != nullptr || dock->objectWidget() != nullptr) {
    target_ = nullptr;
    return;
  }
  target_ = nullptr;
  if (hooks_.create_dock) {
    hooks_.create_dock(dock, plugin_id, type);
  }
  // Adopting focuses the dock, which normally switches the page; a dock that already had the
  // focus emits nothing new, so route it explicitly.
  if (hooks_.focus_dock) {
    hooks_.focus_dock(dock);
  }
}

QWidget* CustomWidgetController::pageForFocusedDock(DockWidget* dock) {
  CustomWidgetDockWidget* custom = nullptr;
  if (dock != nullptr && dock->plotWidget() == nullptr && dock->objectWidget() != nullptr) {
    custom = qobject_cast<CustomWidgetDockWidget*>(dock->objectWidget()->widget());
  }
  config_page_->bindDock(custom);
  return custom != nullptr ? config_page_ : nullptr;
}

void CustomWidgetController::attachDock(CustomWidgetDockWidget& dock) const {
  dock.setActiveWidgetsMarker(marker_);
}

void CustomWidgetController::clearActiveWidgets() {
  if (marker_ != nullptr) {
    marker_->clear();
  }
}

void CustomWidgetController::reviewUncleanShutdown(bool interactive) {
#ifdef PJ_TARGET_WASM
  Q_UNUSED(interactive);
#else
  if (marker_ == nullptr) {
    return;
  }
  const auto report = [this](DiagnosticLevel level, const QString& id, const QString& message) {
    emitDiagnosticTo(sink_, level, "CustomWidget", id.toStdString(), message.toStdString());
  };
  const ActiveWidgetsMarker::ReadResult previous = ActiveWidgetsMarker::take(marker_->directory());
  if (!previous.error.isEmpty()) {
    report(
        DiagnosticLevel::kWarning, {},
        tr("The record of the custom widgets active in a previous run could not be read: %1").arg(previous.error));
    return;
  }
  if (previous.entries.empty()) {
    return;
  }
  QStringList widgets;
  QStringList plugin_ids;
  for (const ActiveWidgetsMarker::Entry& entry : previous.entries) {
    widgets << entry.plugin_id + u'/' + entry.type;
    if (!plugin_ids.contains(entry.plugin_id)) {
      plugin_ids << entry.plugin_id;
    }
  }
  const QString list = widgets.join(u", "_s);
  report(
      DiagnosticLevel::kWarning, plugin_ids.join(u","_s),
      tr("PlotJuggler did not shut down cleanly last time while these custom widgets were active: %1.").arg(list));
  if (!interactive) {
    return;
  }
  const int choice = MessageBox::question(
      dialog_parent_, tr("Custom widgets"),
      tr("PlotJuggler did not shut down cleanly last time while these custom widgets were active:\n\n%1\n\n"
         "Disable them? Their panels will show a placeholder, and they can be enabled again in the Marketplace.")
          .arg(list.toHtmlEscaped()),  // ids and types come from a file on disk; the body label auto-detects markup
      {{tr("Disable"), MessageBox::kPrimaryRole}, {tr("Keep"), MessageBox::kCancelRole}});
  if (choice != 0) {
    return;
  }
  ExtensionCatalogService& extensions = session_.extensionCatalog();
  for (const QString& id : plugin_ids) {
    extensions.extensionManager().setEnabled(id, false);
  }
  // The reload withdraws the disabled Widget plugins from the catalog; their libraries stay mapped.
  extensions.reload();
  report(
      DiagnosticLevel::kInfo, plugin_ids.join(u","_s),
      tr("Disabled custom widget plugins: %1").arg(plugin_ids.join(u", "_s)));
#endif
}

}  // namespace PJ
