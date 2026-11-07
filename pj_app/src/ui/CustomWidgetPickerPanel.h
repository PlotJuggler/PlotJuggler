// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>
#include <memory>

#include "pj_widgets/ChromeMetrics.h"

class QListWidgetItem;

namespace Ui {
class CustomWidgetPickerPanel;
}

namespace PJ {

class ExtensionCatalogService;

/// Right-side-panel page that lists the installed Custom Widget types (every type of every loaded
/// Widget library) so the user can choose what an empty panel shows. Each row carries the type's
/// icon (the manifest's inline SVG, tinted with the theme ink), its name and the library it comes
/// from. A library built against another Qt is listed disabled with the reason as tooltip. With no
/// Widget library installed the page says so and offers the Marketplace. A filter field appears
/// once the list is long enough to need one.
///
/// The page only reports the choice (widgetChosen); the shell owns the panel it applies to.
class CustomWidgetPickerPanel : public QWidget {
  Q_OBJECT
 public:
  /// Rows beyond this count show the filter field.
  static constexpr int kFilterThreshold = 8;

  explicit CustomWidgetPickerPanel(QWidget* parent = nullptr);
  ~CustomWidgetPickerPanel() override;

  /// Source of the list; the page refreshes itself on its catalogChanged. Null empties the list.
  void setCatalog(ExtensionCatalogService* extensions);
  /// Hides the Marketplace button where the build has no Marketplace.
  void setMarketplaceAvailable(bool available);
  /// Rebuilds the rows from the catalog and clears the filter.
  void refresh();

 public slots:
  void onStylesheetChanged(QString theme);
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 signals:
  /// The user picked widget `type` of library `plugin_id`.
  void widgetChosen(QString plugin_id, QString type);
  void marketplaceRequested();

 private:
  void applyFilter(const QString& text);
  void onItemTriggered(QListWidgetItem* item);
  void retintIcons();

  std::unique_ptr<Ui::CustomWidgetPickerPanel> ui_;
  QPointer<ExtensionCatalogService> extensions_;
  QString theme_ = QStringLiteral("light");
};

}  // namespace PJ
