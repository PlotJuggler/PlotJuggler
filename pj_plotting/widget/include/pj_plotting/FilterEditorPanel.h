#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QHash>
#include <QList>
#include <QPair>
#include <QPointF>
#include <QString>
#include <QVector>
#include <QWidget>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_plotting/PlotWidgetBase.h"  // PlotWidgetBase::CurveStyle / LineWidth for the preview API
#include "pj_runtime/CurveDescriptor.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_scripting/filter_class.h"

class QTimer;
class QToolButton;
class QwtPlotCurve;

namespace Ui {
class FilterEditorPanel;
}

namespace PJ {

class SessionManager;
class CatalogModel;
class PlotWidget;
class ParameterForm;
namespace proc {
class DataProcessor;
}
namespace scripting {
class ScriptEngine;
}

/// Host Filter Editor, a full chart-area panel (not a modal dialog) shown through `MainWindow::presentPanel()`, as the
/// toolboxes are. The user picks source curves and a filter, tunes its parameters against a live before/after preview,
/// then Applies (materializes the filtered output and replaces each source curve in place) or Closes.
///
/// The panel is plot-agnostic: it owns the apply against `DataProcessorService` and reports the (source key -> output
/// key) replacements through `applied()`; the host (`MainWindow`) picks the plot and calls `PlotWidget::replaceCurve()`
/// so each filtered series takes the source's slot and colour. In EDIT mode (reopened on an existing filter output) the
/// update is in place, so `applied()` carries an empty list (the host just replots).
class FilterEditorPanel : public QWidget {
  Q_OBJECT
 public:
  /// `source_colors` maps a source catalog key to its colour on the originating plot, so the preview draws the
  /// before/after curves in the colour the result will have. Optional; a missing key gets a palette colour.
  FilterEditorPanel(
      SessionManager* session, CatalogModel* catalog, std::vector<CurveDescriptor> sources,
      QHash<QString, QColor> source_colors = {}, QWidget* parent = nullptr);
  ~FilterEditorPanel() override;

  /// Adopts the display settings of the plot the editor was opened on, so the preview mirrors it (the host calls this
  /// on open and whenever the grid, curve-style or curve-width toolbar changes while the panel is up). `style` and
  /// `width` are the origin plot's `defaultCurveStyle()` / `lineWidth()`; the preview applies them plot-level (so dot
  /// and line pen widths match) and re-dashes the ghost afterwards. Re-applied to rebuilt preview curves.
  void setPreviewDisplay(bool grid_visible, PlotWidgetBase::CurveStyle style, LineWidth width);

 signals:
  /// Emitted on Apply: (source key -> output key) pairs the host applies on the originating plot through
  /// PlotWidget::replaceCurve. Empty in EDIT mode (the existing output was updated in place; just replot).
  void applied(QList<QPair<QString, QString>> replacements);

  /// Emitted on Close (the user dismisses the panel without applying).
  void closed();

  /// A non-fatal warning for the host's diagnostics (one of N sources failed to apply), so it survives the panel
  /// closing.
  void diagnostic(QString message);

 private:
  void populateSources();
  void populateTransforms();
  void onTransformChanged();  // show the matching param panel + refresh the alias
  // Enables Apply when any source is configured, the visible transform or a remembered per-source one, so inspecting a
  // "No Transform" source while others are configured does not grey Apply out.
  void updateApplyEnabled();
  void updateAlias();
  // Live before/after preview of the primary-selected source in a real PlotWidget (zoom, legend, tracker). The ghost is
  // the datastore-backed source curve (dashed); the filtered curve is an in-memory series from applyBatch. Nothing is
  // materialized into the catalog until Apply.
  void setupPreview();
  // Pushes the stored grid, style and width onto the preview plot plot-level (idempotent; from setPreviewDisplay and
  // after each preview rebuild).
  void applyPreviewDisplay();
  // Re-applies each ghost's faded/dashed (active) or solid (inactive) pen. Must run after applyPreviewDisplay(): the
  // plot-level restyle re-pens every curve and would clobber the ghost's dash.
  void applyGhostPens();
  void scheduleRefresh();  // debounced trigger; coalesces rapid parameter edits
  void refreshPreview();
  // When the single selected source is itself a filter output, switches to EDIT mode: preselects its transform and
  // repopulates the panels from its recipe, so Apply updates that filter in place.
  void onSourceSelectionChanged();
  // Per-source transform memory: each source remembers the transform and params it was last configured with (default
  // "No Transform"), so switching sources does not carry one source's transform onto another.
  void saveActiveSourceFilter();                     // capture the visible config for active_source_key_
  void loadSourceFilter(const QString& source_key);  // restore a source's config, or "No Transform"
  // Selects the transform-list row for `id`; false if there is none (the id is not an available filter). Callers must
  // not then drive the form for `id`: its schema would mismatch the selected transform.
  [[nodiscard]] bool selectTransformById(const std::string& id);
  /// Rebuilds the parameter form for `id`'s schema and drives it from a params JSON string (keys = parameter names):
  /// the data-only path to repopulate the editor from a remembered or clipboard config without a processor.
  void setWidgetsFromParams(const std::string& id, const std::string& params_json);
  /// The parameter schema for a filter id (from the bundled Luau classes) with localized labels; keys and values are
  /// unchanged. Empty for a parameterless or unknown id.
  [[nodiscard]] std::vector<scripting::ParamSpec> schemaForId(const std::string& id) const;
  void onCopy();        // serialize the visible filter (id + params JSON) to a process-global clipboard
  void onPaste();       // restore a copied filter into the panels
  void onApplyToAll();  // copy the visible filter into every source's remembered transform
  [[nodiscard]] std::string currentFilterId() const;
  // Constructs and configures the selected builtin from the visible parameter panel.
  [[nodiscard]] std::unique_ptr<proc::DataProcessor> buildConfiguredProcessor() const;
  // By-id twin of buildConfiguredProcessor(): a processor from an explicit (filter id, params JSON), so
  // applyToSelected() can materialize each source's own remembered transform. nullptr for "none" or an unknown id.
  [[nodiscard]] std::unique_ptr<proc::DataProcessor> buildProcessorFor(
      const std::string& id, const std::string& params_json) const;
  // The processor a source previews and applies through: its remembered transform (source_filters_) if configured, else
  // the visible form (buildConfiguredProcessor), the same resolution applyToSelected uses, so a multi-source preview
  // matches Apply. nullptr for "No Transform".
  [[nodiscard]] std::unique_ptr<proc::DataProcessor> processorForSource(const QString& source_key) const;
  // The canonical, untranslated class name used to derive persisted output names.
  [[nodiscard]] QString labelForId(const std::string& id) const;
  // Materializes a filtered output for every source with a configured transform (its own, or the visible one for a
  // plain multi-selection), then emits applied() with the (source -> output) replacements.
  void applyToSelected();
  // Colour of the preview and result curves for `descriptor` (source_colors_, else a palette colour by source index).
  [[nodiscard]] QColor colorForSource(const CurveDescriptor& descriptor) const;
  // If `descriptor` is itself a filter output, the original input series it derives from (through the recipe), so
  // reopening the editor on a filtered curve labels and previews the input, not the output alias; else `descriptor`.
  [[nodiscard]] CurveDescriptor originalInputOf(const CurveDescriptor& descriptor) const;

  std::unique_ptr<Ui::FilterEditorPanel> ui_;
  // The generated parameter editor, driven by the selected filter's schema (the bundled Luau filter classes).
  ParameterForm* param_form_ = nullptr;
  std::shared_ptr<scripting::ScriptEngine> filter_engine_;
  std::vector<scripting::FilterClass> filter_classes_;

  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  std::vector<CurveDescriptor> sources_;
  QHash<QString, QColor> source_colors_;
  // Set when the single selected source is an existing filter output: the panel edits that filter in place.
  std::optional<DataProcessorService::FilterRecipe> edit_recipe_;

  // Per-source remembered transform: source catalog key -> (processor id, params JSON); absent = "No Transform".
  // active_source_key_ is the source whose config the widgets show (saved before switching away).
  QHash<QString, QPair<QString, QString>> source_filters_;
  QString active_source_key_;
  // True once the user typed in the alias field, so the auto-alias stops overwriting it.
  bool alias_user_edited_ = false;

  // Copy, paste and apply-to-all icon buttons in the parameters column header (theme-aware QToolButtons, created in C++
  // to match the scene3d toolbar).
  QToolButton* copy_button_ = nullptr;
  QToolButton* paste_button_ = nullptr;
  QToolButton* apply_all_button_ = nullptr;

  // The origin plot's display settings mirrored onto the preview (setPreviewDisplay; no grid, Lines, 1.0 px until the
  // host pushes them).
  bool preview_grid_ = false;
  PlotWidgetBase::CurveStyle preview_curve_style_ = PlotWidgetBase::kLines;
  LineWidth preview_line_width_ = LineWidth::kPoints10;

  // Live preview embedded in the `chart_preview` frame.
  PlotWidget* preview_plot_ = nullptr;
  QTimer* preview_timer_ = nullptr;

  // One ghost + filtered pair per selected source, so a multi-select edit previews every series Apply will touch. The
  // ghost is the datastore-backed original input, dashed and faded when a transform is active; the filtered curve is
  // in-memory, refreshed through setSamples.
  struct PreviewSeries {
    CurveDescriptor input;             // effective (original) input drawn as the ghost
    QString source_key;                // the selected source's catalog key -> its OWN configured filter
    QColor color;                      // the selected curve's plot colour
    QwtPlotCurve* ghost = nullptr;     // datastore-backed source curve
    QwtPlotCurve* filtered = nullptr;  // in-memory filtered result curve
    // Whether this source has an active transform (dashed ghost, visible filtered curve). Resolved once per
    // refreshPreview(), since processorForSource() builds a Luau VM and must not run per redraw; reused by
    // applyGhostPens().
    bool active = false;
  };
  std::vector<PreviewSeries> preview_series_;
  QString preview_set_key_;  // identity of the current ghost set; rebuild when it changes
};

}  // namespace PJ
