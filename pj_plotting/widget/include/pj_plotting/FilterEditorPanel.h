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

/// Host Filter Editor, presented as a full chart-area **panel** (not a modal
/// dialog) via `MainWindow::presentPanel()` — the same takeover the toolboxes
/// use. The user picks source curve(s), a filter, tunes its parameters in the
/// matching panel against a live before/after preview, then **Apply** (materialize
/// the filtered output and replace each source curve in place) or **Close**.
///
/// The panel is plot-agnostic: it owns the apply against `DataProcessorService`
/// and reports the (source key → output key) replacements via `applied()`. The
/// host (`MainWindow`) decides which plot to mutate — it calls
/// `PlotWidget::replaceCurve()` so each filtered series takes the source's slot
/// and colour. In EDIT mode (reopened on an existing filter output) the update is
/// in place, so `applied()` carries an empty list (the host just replots).
class FilterEditorPanel : public QWidget {
  Q_OBJECT
 public:
  /// `source_colors` maps a source catalog key to the colour it currently has on
  /// the originating plot, so the preview can render the before/after curves in
  /// that colour (faithful to the post-Apply result). Optional; missing keys fall
  /// back to a palette colour.
  FilterEditorPanel(
      SessionManager* session, CatalogModel* catalog, std::vector<CurveDescriptor> sources,
      QHash<QString, QColor> source_colors = {}, QWidget* parent = nullptr);
  ~FilterEditorPanel() override;

  /// Adopt the display settings of the plot the editor was opened on, so the
  /// before/after preview mirrors that plot (the host calls this on open and
  /// whenever the grid / curve-style / curve-width toolbar changes while the panel
  /// is up). `style` and `width` are the origin plot's own `defaultCurveStyle()` /
  /// `lineWidth()`; the preview applies them plot-level (so dot vs line pen widths
  /// match) and re-dashes the ghost afterwards. Re-applied to rebuilt preview curves.
  void setPreviewDisplay(bool grid_visible, PlotWidgetBase::CurveStyle style, LineWidth width);

 signals:
  /// Emitted on Apply. `replacements` is a list of (source key → output key)
  /// pairs the host applies on the originating plot via PlotWidget::replaceCurve.
  /// Empty in EDIT mode (the existing output was updated in place; just replot).
  void applied(QList<QPair<QString, QString>> replacements);

  /// Emitted on Close (the user dismisses the panel without applying).
  void closed();

  /// A non-fatal warning the host should surface in its diagnostics (e.g. one of N
  /// sources failed to apply) — so it survives the panel closing.
  void diagnostic(QString message);

 private:
  void populateSources();
  void populateTransforms();
  void onTransformChanged();  // show the matching param panel + refresh the alias
  // Enable Apply when ANY source is configured — the visible transform OR a
  // remembered per-source one — so inspecting a "No Transform" source while others
  // are configured does not grey out Apply.
  void updateApplyEnabled();
  void updateAlias();
  // Live before/after preview of the primary-selected source, rendered in a real
  // PlotWidget (zoom/legend/tracker). The ghost is the datastore-backed source
  // curve (dashed); the filtered curve is an in-memory series from applyBatch —
  // NOTHING is materialized into the catalog until Apply.
  void setupPreview();
  // Push the stored grid/style/width onto the preview plot plot-level
  // (idempotent; called from setPreviewDisplay and after each preview rebuild).
  void applyPreviewDisplay();
  // Re-apply each ghost's faded/dashed (active) or solid (inactive) pen. MUST run
  // after applyPreviewDisplay() — the plot-level restyle re-pens every curve, so
  // the ghost's dash would otherwise be clobbered on a reactive display change.
  void applyGhostPens();
  void scheduleRefresh();  // debounced trigger; coalesces rapid parameter edits
  void refreshPreview();
  // When the single selected source is itself a filter output, switch to EDIT
  // mode: preselect its transform and repopulate the panels from its recipe, so
  // Apply updates that filter in place (mirrors PJ3's re-open-and-edit).
  void onSourceSelectionChanged();
  // Per-source transform memory: each source remembers the transform + params it
  // was last configured with, so switching sources doesn't bleed one source's
  // transform onto another (it defaults to "No Transform").
  void saveActiveSourceFilter();                     // capture the visible config for active_source_key_
  void loadSourceFilter(const QString& source_key);  // restore a source's config, or "No Transform"
  // Selects the transform-list row for `id`. Returns false if no such row exists
  // (the id is not an available filter) — callers must not then drive the form for
  // `id`, or the form's schema would mismatch the actually-selected transform.
  [[nodiscard]] bool selectTransformById(const std::string& id);
  /// Rebuild the parameter form for `id`'s schema and drive it from a params JSON
  /// string (keys = parameter names) — the data-only path used to repopulate the
  /// editor from a remembered/clipboard config without constructing a processor.
  void setWidgetsFromParams(const std::string& id, const std::string& params_json);
  /// Presentation-only copy of the bundled schema, with localized labels but
  /// unchanged parameter keys and values; empty for parameterless / unknown ids.
  [[nodiscard]] std::vector<scripting::ParamSpec> schemaForId(const std::string& id) const;
  void onCopy();        // serialize the visible filter (id + params JSON) to a process-global clipboard
  void onPaste();       // restore a copied filter into the panels
  void onApplyToAll();  // copy the visible filter into every source's remembered transform
  [[nodiscard]] std::string currentFilterId() const;
  // Construct + configure the selected builtin from the visible param panel.
  [[nodiscard]] std::unique_ptr<proc::DataProcessor> buildConfiguredProcessor() const;
  // By-id twin of buildConfiguredProcessor(): build a processor from an explicit
  // (filter id, params JSON) — lets applyToSelected() materialize each source's own
  // remembered transform. nullptr for "none"/unknown id.
  [[nodiscard]] std::unique_ptr<proc::DataProcessor> buildProcessorFor(
      const std::string& id, const std::string& params_json) const;
  // The processor a given source previews/applies through: its own remembered transform
  // (source_filters_) if configured, else the visible form (buildConfiguredProcessor). This is
  // the SAME resolution applyToSelected uses, so a multi-source preview matches Apply. A source
  // not in source_filters_ falls back to the visible form (what Apply also assigns it). nullptr
  // for "No Transform".
  [[nodiscard]] std::unique_ptr<proc::DataProcessor> processorForSource(const QString& source_key) const;
  // The canonical, untranslated class name used to derive persisted output names.
  [[nodiscard]] QString labelForId(const std::string& id) const;
  // Materialize a filtered output for EVERY source that has a configured transform
  // (its own remembered one, or the visible one for a plain multi-selection), then
  // emit applied() with the (source -> output) replacements.
  void applyToSelected();
  // Colour to draw the preview / result curves for `descriptor` (from
  // source_colors_, else a palette colour by source index).
  [[nodiscard]] QColor colorForSource(const CurveDescriptor& descriptor) const;
  // If `descriptor` is itself a filter OUTPUT, the original input series it was
  // derived from (resolved via the recipe) — so reopening the editor on a filtered
  // curve labels/previews the input, not the output alias. Else `descriptor`.
  [[nodiscard]] CurveDescriptor originalInputOf(const CurveDescriptor& descriptor) const;

  std::unique_ptr<Ui::FilterEditorPanel> ui_;
  // The generated parameter editor (replaces the 7 hardcoded panel_* widgets),
  // driven by the selected filter's schema. The schema comes from the bundled
  // Luau filter classes (M5 routes this through the FilterCatalogue instead).
  ParameterForm* param_form_ = nullptr;
  std::shared_ptr<scripting::ScriptEngine> filter_engine_;
  std::vector<scripting::FilterClass> filter_classes_;

  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  std::vector<CurveDescriptor> sources_;
  QHash<QString, QColor> source_colors_;
  // Set when the single selected source is an existing filter output: the panel
  // edits that filter in place instead of creating a new one.
  std::optional<DataProcessorService::FilterRecipe> edit_recipe_;

  // Per-source remembered transform: source catalog key -> (processor id, params
  // JSON). Absent key == "No Transform". active_source_key_ is the source whose
  // config the widgets currently show (saved before switching away).
  QHash<QString, QPair<QString, QString>> source_filters_;
  QString active_source_key_;
  // True once the user typed in the alias field, so the auto-alias stops
  // overwriting their text (the field stays genuinely editable).
  bool alias_user_edited_ = false;

  // Copy / paste / apply-to-all icon buttons in the parameters column header
  // (theme-aware QToolButtons, created in C++ to match the scene3d toolbar).
  QToolButton* copy_button_ = nullptr;
  QToolButton* paste_button_ = nullptr;
  QToolButton* apply_all_button_ = nullptr;

  // Origin plot's display settings mirrored onto the preview (set by the host via
  // setPreviewDisplay; defaults = no grid, Lines, 1.0px until the host pushes them).
  bool preview_grid_ = false;
  PlotWidgetBase::CurveStyle preview_curve_style_ = PlotWidgetBase::kLines;
  LineWidth preview_line_width_ = LineWidth::kPoints10;

  // Live preview embedded in the `chart_preview` frame.
  PlotWidget* preview_plot_ = nullptr;
  QTimer* preview_timer_ = nullptr;

  // One ghost+filtered pair PER selected source, so a multi-select edit previews
  // every series Apply will touch — not just the first. The ghost is the
  // datastore-backed (original) input, dashed/faded when a transform is active;
  // the filtered curve is in-memory, refreshed via setSamples.
  struct PreviewSeries {
    CurveDescriptor input;             // effective (original) input drawn as the ghost
    QString source_key;                // the selected source's catalog key -> its OWN configured filter
    QColor color;                      // the selected curve's plot colour
    QwtPlotCurve* ghost = nullptr;     // datastore-backed source curve
    QwtPlotCurve* filtered = nullptr;  // in-memory filtered result curve
    // Whether this source has an active transform (dashed ghost + visible filtered).
    // Resolved once per refreshPreview() — processorForSource() builds a Luau VM, so it
    // must not be re-derived per redraw — and reused by applyGhostPens().
    bool active = false;
  };
  std::vector<PreviewSeries> preview_series_;
  QString preview_set_key_;  // identity of the current ghost set; rebuild when it changes
};

}  // namespace PJ
