#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
#include <memory>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_widgets/ChromeMetrics.h"

class QStackedWidget;
class QWidget;

namespace PJ {

class ActiveWidgetsMarker;
class AppSession;
class CustomWidgetConfigPanel;
class CustomWidgetDockWidget;
class CustomWidgetPickerPanel;
class DockWidget;

/// The shell side of Custom Widget panels: the picker page (which widget an empty panel becomes),
/// the config page of the focused panel, both in the right side panel's stack, and the crash loop
/// breaker (the active-widgets record every Custom Widget dock writes, cleared at a clean exit and
/// reviewed at the next start). The dock itself is built by the shell through `Hooks::create_dock`.
/// GUI thread only.
class CustomWidgetController : public QObject {
  Q_OBJECT
 public:
  /// What the controller asks of the shell.
  struct Hooks {
    /// Opens the right side panel when it is folded.
    std::function<void()> reveal_side_panel;
    /// Builds widget `type` of `plugin_id` into the empty `dock`.
    std::function<void(DockWidget* dock, const QString& plugin_id, const QString& type)> create_dock;
    /// Routes the focus to `dock` again, so the side panel shows its page.
    std::function<void(DockWidget* dock)> focus_dock;
  };

  /// Adds both pages to `pages`, which also owns the controller (QObject parent). `dialog_parent`
  /// parents the startup question. The record is the default one on desktop; the browser build has
  /// none.
  CustomWidgetController(
      QStackedWidget& pages, AppSession& session, DiagnosticSink sink, Hooks hooks, QWidget* dialog_parent);
  ~CustomWidgetController() override;

  CustomWidgetController(const CustomWidgetController&) = delete;
  CustomWidgetController& operator=(const CustomWidgetController&) = delete;

  /// The fifth placeholder icon of the empty `dock`: remembers it as the panel the choice applies
  /// to and shows the picker. A pick builds the widget there, unless the panel was closed or filled
  /// meanwhile.
  void showPicker(DockWidget* dock);

  /// Focus routing: binds the config page to `dock`'s Custom Widget (unbinds it for any other dock,
  /// or null) and returns the page to show for `dock`: the config page, the picker while the empty
  /// panel waiting for a choice has the focus, or nullptr when neither applies.
  [[nodiscard]] QWidget* pageForFocusedDock(DockWidget* dock);

  /// Hands the active-widgets record to a newly built Custom Widget dock.
  void attachDock(CustomWidgetDockWidget& dock) const;

  /// This process's record, shared with every Custom Widget dock (null in the browser build). A test
  /// points it at a directory of its own before any dock exists.
  [[nodiscard]] std::shared_ptr<ActiveWidgetsMarker>& activeWidgetsMarker() {
    return marker_;
  }

  /// Startup step, before any layout restore: consumes the records of the runs that are no longer
  /// alive (ActiveWidgetsMarker::take; another instance still running keeps its own). A record means
  /// that run ended while those plugins were live: one warning names the widgets of every such
  /// record, and when `interactive` the user is asked whether to disable them (Marketplace disabled
  /// list + catalog reload, so their panels restore as placeholders). No-op in the browser build.
  void reviewUncleanShutdown(bool interactive);

  /// The clean exit: call once every Custom Widget dock is destroyed.
  void clearActiveWidgets();

 public slots:
  void onStylesheetChanged(const QString& theme);
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 signals:
  /// The picker's "Open Marketplace" (shown when nothing is installed).
  void marketplaceRequested();

 private:
  void onWidgetChosen(const QString& plugin_id, const QString& type);

  QStackedWidget& pages_;
  AppSession& session_;
  DiagnosticSink sink_;
  Hooks hooks_;
  QWidget* dialog_parent_ = nullptr;
  CustomWidgetPickerPanel* picker_ = nullptr;       ///< owned by `pages_`
  CustomWidgetConfigPanel* config_page_ = nullptr;  ///< owned by `pages_`
  QPointer<DockWidget> target_;                     ///< the empty panel the picker was opened for
  std::shared_ptr<ActiveWidgetsMarker> marker_;
};

}  // namespace PJ
