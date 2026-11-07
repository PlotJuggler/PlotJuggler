// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "ui/CustomWidgetPickerPanel.h"

#include <QByteArray>
#include <QIcon>
#include <QLineEdit>
#include <QListWidgetItem>
#include <QPixmap>
#include <algorithm>
#include <tuple>
#include <vector>

#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/Search.h"
#include "pj_widgets/SectionHeaderBand.h"
#include "pj_widgets/SvgUtil.h"
#include "ui_CustomWidgetPickerPanel.h"

namespace PJ {

namespace {

constexpr int kPluginIdRole = Qt::UserRole;
constexpr int kTypeRole = Qt::UserRole + 1;
constexpr int kIconSvgRole = Qt::UserRole + 2;
constexpr int kSearchRole = Qt::UserRole + 3;

constexpr auto kFallbackIcon = ":/resources/svg/custom_widget.svg";

/// The manifest's inline SVG tinted with the theme ink; the generic Custom Widget glyph when the
/// manifest has no icon or the markup does not render.
QIcon widgetIcon(const QByteArray& svg, const QString& theme, const QSize& size, qreal dpr) {
  if (!svg.isEmpty()) {
    if (const QPixmap pixmap = renderSvgDataPixmap(svg, theme, size, dpr); !pixmap.isNull()) {
      return QIcon(pixmap);
    }
  }
  return QIcon(renderSvgPixmap(QString::fromLatin1(kFallbackIcon), theme, size, dpr));
}

}  // namespace

CustomWidgetPickerPanel::CustomWidgetPickerPanel(QWidget* parent)
    : QWidget(parent), ui_(std::make_unique<Ui::CustomWidgetPickerPanel>()) {
  ui_->setupUi(this);
  const int comfortable = theme::space(theme::Space::Comfortable);
  const int snug = theme::space(theme::Space::Snug);
  ui_->pickerBodyLayout->setContentsMargins(comfortable, snug, comfortable, snug);
  ui_->pickerBodyLayout->setSpacing(snug);
  ui_->emptyStateLayout->setContentsMargins(0, 0, 0, 0);
  ui_->emptyStateLayout->setSpacing(snug);
  ui_->pickerBodyLayout->setStretchFactor(ui_->listCustomWidgets, 1);
  ui_->pickerBodyLayout->setStretchFactor(ui_->emptyState, 1);

  if (Search* search = ui_->pickerBand->filterSearch(); search != nullptr) {
    connect(search, &Search::textChanged, this, &CustomWidgetPickerPanel::applyFilter);
  }
  // A click is the choice; Return on the keyboard-current row is the same choice.
  connect(ui_->listCustomWidgets, &QListWidget::itemClicked, this, &CustomWidgetPickerPanel::onItemTriggered);
  connect(ui_->listCustomWidgets, &QListWidget::itemActivated, this, &CustomWidgetPickerPanel::onItemTriggered);
  connect(ui_->buttonOpenMarketplace, &QPushButton::clicked, this, &CustomWidgetPickerPanel::marketplaceRequested);
  refresh();
}

CustomWidgetPickerPanel::~CustomWidgetPickerPanel() = default;

void CustomWidgetPickerPanel::setCatalog(ExtensionCatalogService* extensions) {
  if (extensions_ != nullptr) {
    disconnect(extensions_.data(), nullptr, this, nullptr);
  }
  extensions_ = extensions;
  if (extensions_ != nullptr) {
    connect(extensions_.data(), &ExtensionCatalogService::catalogChanged, this, &CustomWidgetPickerPanel::refresh);
  }
  refresh();
}

void CustomWidgetPickerPanel::setMarketplaceAvailable(bool available) {
  ui_->buttonOpenMarketplace->setVisible(available);
  ui_->labelMarketplaceHint->setVisible(available);
}

void CustomWidgetPickerPanel::refresh() {
  QListWidget* list = ui_->listCustomWidgets;
  list->clear();

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
  if (extensions_ != nullptr) {
    for (const LoadedWidget& library : extensions_->widgets()) {
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
  }
  std::ranges::sort(rows, [](const Row& lhs, const Row& rhs) {
    const int by_name = QString::localeAwareCompare(lhs.name, rhs.name);
    if (by_name != 0) {
      return by_name < 0;
    }
    return std::tie(lhs.plugin_name, lhs.type) < std::tie(rhs.plugin_name, rhs.type);
  });

  const int row_height = list->iconSize().height() + (2 * theme::space(theme::Space::Snug));
  for (const Row& row : rows) {
    auto* item = new QListWidgetItem(tr("%1\nfrom %2").arg(row.name, row.plugin_name), list);
    item->setData(kPluginIdRole, row.plugin_id);
    item->setData(kTypeRole, row.type);
    item->setData(kIconSvgRole, row.icon);
    item->setData(kSearchRole, row.name + u'\n' + row.plugin_name);
    item->setSizeHint(QSize(0, row_height));
    if (!row.enabled) {
      item->setFlags(item->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
      item->setToolTip(row.reason.toHtmlEscaped());
    } else {
      item->setToolTip(tr("%1/%2").arg(row.plugin_id, row.type).toHtmlEscaped());
    }
  }
  retintIcons();

  const bool empty = rows.empty();
  list->setVisible(!empty);
  ui_->labelPickerHint->setVisible(!empty);
  ui_->emptyState->setVisible(empty);
  if (Search* search = ui_->pickerBand->filterSearch(); search != nullptr) {
    search->clear();
    search->setVisible(static_cast<int>(rows.size()) > kFilterThreshold);
  }
}

void CustomWidgetPickerPanel::applyFilter(const QString& text) {
  QListWidget* list = ui_->listCustomWidgets;
  const QString needle = text.trimmed();
  for (int row = 0; row < list->count(); ++row) {
    QListWidgetItem* item = list->item(row);
    item->setHidden(!needle.isEmpty() && !item->data(kSearchRole).toString().contains(needle, Qt::CaseInsensitive));
  }
}

void CustomWidgetPickerPanel::onItemTriggered(QListWidgetItem* item) {
  if (item == nullptr || !item->flags().testFlag(Qt::ItemIsEnabled)) {
    return;
  }
  emit widgetChosen(item->data(kPluginIdRole).toString(), item->data(kTypeRole).toString());
}

void CustomWidgetPickerPanel::retintIcons() {
  QListWidget* list = ui_->listCustomWidgets;
  const QSize size = list->iconSize();
  const qreal dpr = devicePixelRatioF();
  for (int row = 0; row < list->count(); ++row) {
    QListWidgetItem* item = list->item(row);
    item->setIcon(widgetIcon(item->data(kIconSvgRole).toByteArray(), theme_, size, dpr));
  }
}

void CustomWidgetPickerPanel::onStylesheetChanged(QString theme) {
  theme_ = std::move(theme);
  retintIcons();
}

void CustomWidgetPickerPanel::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  ui_->pickerBand->onChromeMetricsChanged(metrics);
}

}  // namespace PJ
