#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QList>
#include <QMetaObject>
#include <QString>
#include <QWidget>
#include <functional>

#include "pj_base/types.hpp"
#include "pj_runtime/MarkerTopics.h"
#include "pj_widgets/ChromeMetrics.h"

class QListWidgetItem;
class QPushButton;
class QToolButton;

namespace Ui {
class CurveEditor;
}

namespace PJ {

class ColorPickerPopup;
class PlotWidget;
class StateTransitionsController;

// Side panel listing the curves of one PlotWidget. One row per curve: colour swatch (click for the picker) | name |
// markers toggle | visibility eye | trash. Under the list a pinned footer (1-px separator above it) holds up to two
// rows per dataset shown on the plot, one per marker scope (Dataset / Global) whose object topic exists: dataset glyph
// | bold name | markers toggle, which shows or hides that scope's plot markers independently of any curve's toggle and
// of the dataset's other scope; the footer is hidden when it has no rows. The header strip has a Datasets-style filter
// and a kebab popup with "Clear all curves".
//
// Bind with setPlot(plot) / setPlot(nullptr). The panel rebuilds its rows on the plot's curveListChanged() and
// datasetMarkerScopesChanged(), re-checks one footer toggle on markerScopeVisibilityChanged(), and detects a destroyed
// plot through a QObject::destroyed connection. Width and line style controls live in MainWindow's right-toolbar strip.
class CurveEditor : public QWidget {
  Q_OBJECT
 public:
  explicit CurveEditor(QWidget* parent = nullptr);
  ~CurveEditor() override;

  void setPlot(PlotWidget* plot);

  /// Alternative binding: the same table lists a State Transitions strip's series (name | eye | trash, no colour
  /// swatch: state colours are hash-derived). A non-null bind releases the plot binding and vice versa; a null releases
  /// only the strip binding.
  void setStateTransitions(StateTransitionsController* controller);

  [[nodiscard]] PlotWidget* plot() const noexcept {
    return plot_;
  }

 public slots:
  void refresh();
  // Updates the per-row visibility, trash and header icons to the new theme's ink.
  void onStylesheetChanged(QString theme);
  // Rebinds the header band's chrome dimensions (search, filter, kebab) so they follow the global chrome-metrics
  // setting: the band is (icon_size + icon_padding) + 2 * layout_padding tall with layout_padding as contentsMargins,
  // and layout_spacing goes to QListWidget::setSpacing so the rows gain gaps. Per-row eye and trash icons keep their
  // compact 20-px row height (a deliberate dense list).
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 private slots:
  void onFilterChanged(const QString& text);

 protected:
  // Watches the curves-filter QLineEdit so its FocusIn / FocusOut events swap label visibility (as the Datasets
  // header), and the listWidget viewport so row names re-elide on resize.
  bool eventFilter(QObject* watched, QEvent* event) override;

  // Drives the progressive collapse of the Curves header (search, filter and label hide when too narrow; the kebab
  // stays). Driven off the panel's own width, because the header band's QWidget cannot shrink below the sum of its
  // contents' natural sizes.
  void resizeEvent(QResizeEvent* event) override;

 private:
  // Re-applies the filter text to the list items (case-insensitive substring of the curve name); called after
  // refresh().
  void applyFilter();
  // Hides search, filter and label when the header band is too narrow, keeping the kebab reachable.
  void updateHeaderForWidth();
  void appendRow(
      const QString& curve_key, const QString& display_name, QColor color, bool visible, bool markers_visible);
  // A marker-scope row for (`dataset_id`, `scope`) in the pinned footer: scope glyph, bold name, markers toggle (no
  // swatch, eye or trash).
  void appendMarkerScopeRow(DatasetId dataset_id, MarkerScope scope, const QString& label, bool visible);
  // Deletes every footer row and hides the footer (its separator goes with it).
  void clearMarkerScopeRows();
  // The footer's row widgets in display order; each carries its (dataset, scope) key as dynamic properties.
  [[nodiscard]] QList<QWidget*> markerScopeRows() const;
  // Re-checks the footer toggle of (`dataset_id`, `scope`) without a rebuild.
  void onMarkerScopeVisibilityChanged(DatasetId dataset_id, MarkerScope scope, bool visible);
  // Visits every list row, then every footer row (all CurveRowWidgets).
  void forEachRow(const std::function<void(QWidget*)>& visit) const;
  // Shared builder of a per-row checkable icon toggle (visibility eye, markers): a flat ink glyph whose 2-state SVG
  // swaps on toggle. The objectName drives the QSS rule that strips QToolButton's default hover and checked background;
  // `property_key` tags the button for the theme pass.
  QToolButton* makeToggleButton(
      const QString& object_name, const char* property_key, const char* on_path, const char* off_path, bool checked,
      const QString& tooltip, std::function<void(bool)> on_toggled);
  void onSwatchClicked(const QString& curve_name, QPushButton* swatch);
  void onPickerColorChanged(QColor color);
  void onCurveColorChanged(const QString& curve_name, QColor color);
  // Re-checks the eye of `curve_name`'s row without a rebuild.
  void onCurveVisibilityChanged(const QString& curve_name, bool visible);
  void onVisibilityToggled(const QString& curve_name, bool visible);
  void onMarkersToggled(const QString& curve_name, bool show);
  void clearActivePicker();

  Ui::CurveEditor* ui_;
  PlotWidget* plot_ = nullptr;
  // The strip binding (setStateTransitions); mutually exclusive with plot_.
  StateTransitionsController* state_controller_ = nullptr;
  QMetaObject::Connection state_series_connection_;
  QMetaObject::Connection state_destroyed_connection_;
  // The active theme, so per-row icons render in the right ink at row creation without walking up to qApp / Theme.
  // Updated through onStylesheetChanged.
  QString current_theme_ = QStringLiteral("light");
  // Row height in pixels for curve rows; tracks icon_size so larger icons do not clip (onChromeMetricsChanged). The
  // default matches MainWindow's first-launch icon size.
  int row_height_ = 20;
  QMetaObject::Connection curve_list_connection_;
  QMetaObject::Connection curve_color_connection_;
  QMetaObject::Connection curve_visibility_connection_;
  QMetaObject::Connection marker_scopes_connection_;
  QMetaObject::Connection marker_visibility_connection_;
  QMetaObject::Connection plot_destroyed_connection_;

  // Created on the first swatch click and reused, so the persistent colorChanged connection survives.
  ColorPickerPopup* color_picker_ = nullptr;
  // Which row's swatch the popup is editing; cleared when the row is destroyed (refresh, setPlot, plot teardown).
  QString active_color_curve_;
};

}  // namespace PJ
