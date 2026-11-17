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

// The main window's top row: menus on the left, panel toggles and the notification bell on the right. In
// custom-frame mode (a frameless QMainWindow) it is the title bar itself: app icon, window controls, and its
// empty regions are the move handle (double-click toggles maximize).
class TitleBar : public QWidget {
  Q_OBJECT
 public:
  explicit TitleBar(QWidget* parent = nullptr);
  ~TitleBar() override;

  // Owned here, populated by MainWindow.
  [[nodiscard]] QMenu* fileMenu() const;
  [[nodiscard]] QMenu* toolboxMenu() const;
  [[nodiscard]] QMenu* helpMenu() const;

  // Between the bell and the window controls; repeated calls append left to right.
  void addRightClusterWidget(QWidget* widget);

  // nullptr clears it. The caller owns the widget: a replaced one is reparented out, not deleted.
  void setCenterWidget(QWidget* widget);

  // Paints the bell in the status-error colour; the owner decides what "seen" means.
  void setUnseenError(bool unseen);

  // With the system frame the bar hides the icon and window controls and is no move handle. Constructed custom.
  void setSystemFrame(bool system_frame);

  // count <= 0 hides the "Update" button.
  void setExtensionUpdateCount(int count);

 signals:
  void notificationsClicked();
  void extensionUpdateRequested();

 public slots:
  void onStylesheetChanged(QString theme);

  // Height ChromeMetrics::titleBarHeight(), chrome buttons (icon_size + icon_padding) square.
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
  QWidget* center_widget_ = nullptr;  // Not owned.
  QMenu* file_menu_ = nullptr;
  QMenu* toolbox_menu_ = nullptr;
  QMenu* help_menu_ = nullptr;
  bool unseen_error_ = false;
  bool system_frame_ = false;
  ChromeMetrics chrome_metrics_;
};

}  // namespace PJ
