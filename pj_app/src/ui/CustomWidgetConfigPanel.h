// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "pj_widgets/ChromeMetrics.h"

class QLayout;
class QTreeWidgetItem;

namespace Ui {
class CustomWidgetConfigPanel;
}

namespace PJ {

class CatalogModel;
class CustomWidgetDockWidget;
class ExtensionCatalogService;

/// Right-side-panel page for the focused Custom Widget panel: the widget's name and library, its
/// inputs with the series wired to each (add, replace, remove), and the settings form the host
/// paints from the type's declared `settings` list. The dock owns every piece of state; this page
/// reads it and turns gestures into dock calls.
///
/// Wiring: a normal input offers "Choose series" (replaces its wire), a `multiple` input offers
/// "Add series" (a new slot). Both open a chooser in place of the form that lists only the catalog
/// series the input accepts (CustomWidgetDockWidget::canBind).
///
/// Settings: one control per declared entry (`number`, `integer`, `boolean`, `text`, `color`, `enum`),
/// showing the value of that key in the dock's settingsJson() (which carries the manifest defaults). A change
/// edits that one key of the dock's current settingsJson() and hands the whole object back through
/// applySettings(), once per change; number edits are coalesced for kSettingsDebounceMs so a drag
/// produces one call. The form is built when a dock is bound, not on every dock signal, so a control
/// is never destroyed under the user's hand. The dock's signals rebuild the inputs (and, after an
/// input change, the header) once, from the event loop, however many of them arrived together.
///
/// A dock whose widget is missing shows the reason and its stored wires, read-only.
class CustomWidgetConfigPanel : public QWidget {
  Q_OBJECT
 public:
  static constexpr int kSettingsDebounceMs = 150;

  explicit CustomWidgetConfigPanel(QWidget* parent = nullptr);
  ~CustomWidgetConfigPanel() override;

  /// The catalog the series chooser lists and the extension catalog the header names libraries from.
  void setServices(CatalogModel* catalog, ExtensionCatalogService* extensions);

  /// Attaches to a dock; nullptr detaches. A pending number edit is applied to the previous dock first.
  void bindDock(CustomWidgetDockWidget* dock);
  [[nodiscard]] CustomWidgetDockWidget* dock() const;

  /// Applies a debounced settings edit now (no-op when none is pending).
  void flushPendingSettings();

 public slots:
  void onStylesheetChanged(QString theme);
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 private:
  /// One control of the settings form, kept to re-read its value after an outside change.
  struct SettingControl {
    QString key;
    QWidget* editor = nullptr;
    std::function<void(const QJsonValue&)> set_value;  ///< shows a value in `editor`; signals stay blocked around it
  };

  void rebuild();
  void rebuildHeader();
  void rebuildInputs();
  void rebuildMissingWires();
  void rebuildSettings();
  /// Re-reads every control's value from settingsJson() without emitting edits.
  void syncSettingValues();
  /// Coalesces the dock's signals into one rebuild from the event loop; `with_header` also rebuilds
  /// the header and re-reads the settings (an input change may come from a settings change).
  void scheduleRebuild(bool with_header);
  void runScheduledRebuild();
  /// Whether the settings form shows what the dock declares. Rebuilding it takes the focus from the
  /// setting being edited, so inputs changing under an unchanged form only refreshes its values.
  [[nodiscard]] bool settingsFormMatchesDock() const;

  void openChooser(const QString& input, std::uint32_t slot);
  void closeChooser();
  void onChooserActivated(QTreeWidgetItem* item);

  void editSetting(const QString& key, const QJsonValue& value, bool debounce);
  [[nodiscard]] QJsonObject currentSettings() const;
  void applyMissingReasonStyle();
  /// Removes and deletes every item of `layout`. Widgets are detached first and deleted later: the
  /// rebuild can run from inside one of their own signals.
  static void clearLayout(QLayout* layout);

  std::unique_ptr<Ui::CustomWidgetConfigPanel> ui_;
  QPointer<CustomWidgetDockWidget> dock_;
  QPointer<CatalogModel> catalog_;
  QPointer<ExtensionCatalogService> extensions_;
  QString theme_ = QStringLiteral("light");

  std::vector<SettingControl> setting_controls_;
  QJsonObject pending_settings_;
  QTimer settings_debounce_;

  QString chooser_input_;
  std::uint32_t chooser_slot_ = 0;

  bool rebuild_scheduled_ = false;
  bool rebuild_header_ = false;
};

}  // namespace PJ
