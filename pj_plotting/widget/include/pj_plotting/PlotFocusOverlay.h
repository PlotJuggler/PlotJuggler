#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QPointer>
#include <QString>
#include <QWidget>

namespace ads {
class CDockAreaWidget;
class CDockContainerWidget;
}  // namespace ads

namespace PJ {

// Click-through overlay that frames the focused (blue) and hovered
// (light_blue) dock areas. The container must outlive this overlay,
// which parents itself to the container and installs an event filter
// on it to track layout changes.
class PlotFocusOverlay final : public QWidget {
  Q_OBJECT
 public:
  explicit PlotFocusOverlay(ads::CDockContainerWidget& container);

  // Both setters silently ignore areas not inside the bound container.
  void setFocusedArea(ads::CDockAreaWidget* area);
  void setHoveredArea(ads::CDockAreaWidget* area);

  /// A permanent watermark painted inside the bottom-right corner of every
  /// plot canvas in the container, empty for an unmarked tab. Drawn beneath the
  /// focus and hover frames so it never covers them.
  void setWatermarkText(const QString& text);

 protected:
  void paintEvent(QPaintEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void syncGeometryToContainer();
  [[nodiscard]] bool ownsArea(ads::CDockAreaWidget* area) const;

  ads::CDockContainerWidget* container_;
  QPointer<ads::CDockAreaWidget> focused_area_;
  QPointer<ads::CDockAreaWidget> hovered_area_;
  QString watermark_;
};

}  // namespace PJ
