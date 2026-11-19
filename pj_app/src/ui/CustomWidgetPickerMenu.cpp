// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "ui/CustomWidgetPickerMenu.h"

#include <QAction>
#include <QByteArray>
#include <QIcon>
#include <QPixmap>
#include <QStyle>
#include <algorithm>
#include <tuple>
#include <vector>

#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_widgets/SvgUtil.h"

namespace PJ {

namespace {

constexpr auto kFallbackIcon = ":/resources/svg/custom_widget.svg";

/// The manifest's inline SVG tinted with the theme ink; the generic Custom Widget glyph when the manifest has no icon
/// or the markup does not render.
QIcon widgetIcon(const QByteArray& svg, const QString& theme, const QSize& size, qreal dpr) {
  if (!svg.isEmpty()) {
    if (const QPixmap pixmap = renderSvgDataPixmap(svg, theme, size, dpr); !pixmap.isNull()) {
      return QIcon(pixmap);
    }
  }
  return QIcon(renderSvgPixmap(QString::fromLatin1(kFallbackIcon), theme, size, dpr));
}

}  // namespace

CustomWidgetPickerMenu::CustomWidgetPickerMenu(QWidget* parent) : QMenu(parent) {
  setObjectName(QStringLiteral("CustomWidgetPickerMenu"));
}

void CustomWidgetPickerMenu::rebuild(ExtensionCatalogService& extensions) {
  clear();

  struct Row {
    QString name;
    QString plugin_name;
    QString plugin_id;
    QString type;
    QByteArray icon;
    bool enabled = true;
    QString reason;
  };
  std::vector<Row> rows;
  for (const LoadedWidget& library : extensions.widgets()) {
    const QString plugin_name = QString::fromStdString(library.name.empty() ? library.id : library.name);
    for (const WidgetTypeDescriptor& widget : library.widgets) {
      Row row;
      row.name = QString::fromStdString(widget.name.empty() ? widget.type : widget.name);
      row.plugin_name = plugin_name;
      row.plugin_id = QString::fromStdString(library.id);
      row.type = QString::fromStdString(widget.type);
      row.icon = QByteArray::fromStdString(widget.icon);
      row.enabled = library.qt_compatible;
      row.reason = QString::fromStdString(library.qt_mismatch_reason);
      rows.push_back(std::move(row));
    }
  }
  std::ranges::sort(rows, [](const Row& lhs, const Row& rhs) {
    const int by_name = QString::localeAwareCompare(lhs.name, rhs.name);
    if (by_name != 0) {
      return by_name < 0;
    }
    return std::tie(lhs.plugin_name, lhs.type) < std::tie(rhs.plugin_name, rhs.type);
  });

  if (rows.empty()) {
    // The Custom Widget button is disabled without a widget, so this only shows if the catalog emptied meanwhile.
    addAction(tr("No custom widgets installed."))->setEnabled(false);
    return;
  }
  const int side = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
  const QSize icon_size(side, side);
  for (const Row& row : rows) {
    QAction* action = addAction(tr("%1 (from %2)").arg(row.name, row.plugin_name));
    action->setIcon(widgetIcon(row.icon, theme_, icon_size, devicePixelRatioF()));
    if (!row.enabled) {
      action->setEnabled(false);
      action->setToolTip(row.reason.toHtmlEscaped());
      continue;
    }
    action->setToolTip(tr("%1/%2").arg(row.plugin_id, row.type).toHtmlEscaped());
    connect(action, &QAction::triggered, this, [this, plugin_id = row.plugin_id, type = row.type]() {
      emit widgetChosen(plugin_id, type);
    });
  }
}

void CustomWidgetPickerMenu::onStylesheetChanged(QString theme) {
  theme_ = std::move(theme);
}

}  // namespace PJ
