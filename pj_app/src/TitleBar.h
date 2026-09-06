#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QWidget>

#include "pj_widgets/ChromeMetrics.h"

class QMenu;
class QMouseEvent;

namespace Ui {
class TitleBar;
}

namespace PJ {

// Custom title bar for a frameless QMainWindow. Hosts the app icon and
// a traditional QMenuBar (File / Toolbox / Help) on the left, and the
// relocated panel toggles, the notification bell and minimize /
// maximize / close on the right. Empty regions act as the system-move
// handle; double-click on empty regions toggles maximize.
class TitleBar : public QWidget {
  Q_OBJECT
 public:
  explicit TitleBar(QWidget* parent = nullptr);
  ~TitleBar() override;

  // The TitleBar owns the QMenuBar's three popup menus; MainWindow
  // populates them. fileMenu() holds layout load/save + marketplace +
  // preferences + quit. toolboxMenu() and the Help "Installed
  // Extensions" submenu are rebuilt lazily by the caller on
  // aboutToShow, so they track the live extension catalog.
  [[nodiscard]] QMenu* fileMenu() const;
  [[nodiscard]] QMenu* toolboxMenu() const;
  [[nodiscard]] QMenu* helpMenu() const;

  // Inserts a widget into the right-side cluster, between the
  // notification bell and the window controls. Repeated calls append
  // left-to-right. Used by the shell to relocate the three
  // panel-toggle buttons created by TabbedPlotWidget.
  void addRightClusterWidget(QWidget* widget);

  // Place a widget in the center region (where the old horizontalSpacer lived),
  // horizontally centered between two stretches. Passing nullptr clears it and
  // leaves the stretches, so the bar looks identical to having no center widget.
  // The caller owns the widget's lifetime (on replacement/clear it is reparented
  // out, not deleted). The empty center area stays a window-drag handle, because
  // centerContainer is transparent for mouse events.
  void setCenterWidget(QWidget* widget);

  // Paint the bell in the theme's status-error colour while errors have
  // been recorded that the user has not yet opened the diagnostics view
  // for; plain glyph otherwise. The owner decides what "seen" means.
  void setUnseenError(bool unseen);

  // Shows/hides the magenta "Update" affordance sitting left of the bell.
  // count > 0 reveals the button and sets its tooltip to the pluralized
  // count; count <= 0 hides it. Clicking it emits extensionUpdateRequested().
  void setExtensionUpdateCount(int count);

 signals:
  // Bell click. The owner (MainWindow) opens the DiagnosticsDialog.
  void notificationsClicked();

  // Emitted when the user clicks the "Update" button. The owner
  // (MainWindow) opens the Marketplace and hides the button in response.
  void extensionUpdateRequested();

 public slots:
  void onStylesheetChanged(QString theme);

  // Rebinds Chrome metrics from MainWindow. Re-sizes the title bar to
  // (icon_size + icon_padding) + 2 * layout_padding tall, sets each
  // chrome button to (icon_size + icon_padding) square, pushes
  // layout_padding as contentsMargins on the horizontal layout, and
  // layout_spacing as the gap between adjacent chrome buttons.
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void changeEvent(QEvent* event) override;

 private slots:

 private:
  void applyIcons(const QString& theme);
  void applyIconMetrics();
  void onMaximizeClicked();
  [[nodiscard]] bool isOnMoveHandle(const QPoint& pos) const;

  Ui::TitleBar* ui_;
  // Widget currently placed in centerContainer via setCenterWidget (not owned;
  // reparented out on replacement). nullptr when the center region is empty.
  QWidget* center_widget_ = nullptr;
  QMenu* file_menu_ = nullptr;
  QMenu* toolbox_menu_ = nullptr;
  QMenu* help_menu_ = nullptr;
  // Stored so applyIcons() re-tints the bell on a theme change.
  bool unseen_error_ = false;

  // Chrome metrics broadcast from MainWindow::chromeMetricsChanged.
  ChromeMetrics chrome_metrics_;
};

}  // namespace PJ
