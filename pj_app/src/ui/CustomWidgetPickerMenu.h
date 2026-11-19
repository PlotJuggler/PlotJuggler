// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QMenu>
#include <QString>

namespace PJ {

class ExtensionCatalogService;

/// Context menu, opened from an empty panel's Custom Widget button, that lists the installed Custom Widget types
/// (every type of every loaded Widget library) so the user can choose what the panel shows. Each entry carries the
/// type's icon (the manifest's inline SVG, tinted with the theme ink), its name and the library it comes from. A
/// library built against another Qt is listed disabled with the reason as tooltip.
///
/// The menu only reports the choice (widgetChosen); the controller owns the panel it applies to.
class CustomWidgetPickerMenu : public QMenu {
  Q_OBJECT
 public:
  explicit CustomWidgetPickerMenu(QWidget* parent = nullptr);

  /// Rebuilds the entries from the catalog's current widget types. Called each time the menu is opened.
  void rebuild(ExtensionCatalogService& extensions);

 public slots:
  void onStylesheetChanged(QString theme);

 signals:
  /// The user picked widget `type` of library `plugin_id`.
  void widgetChosen(QString plugin_id, QString type);

 private:
  QString theme_ = QStringLiteral("light");
};

}  // namespace PJ
