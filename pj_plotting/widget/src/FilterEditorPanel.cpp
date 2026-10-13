// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/FilterEditorPanel.h"

#include <qwt_plot_curve.h>
#include <qwt_point_data.h>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPen>
#include <QPlainTextEdit>
#include <QPointF>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>
#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

#include "pj_datastore/engine.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_plotting/FilteredCurveAdapter.h"
#include "pj_plotting/ParameterForm.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/CurveDisplayName.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_scripting/lua_siso_transform.h"
#include "pj_scripting/script_engine.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/Style.h"
#include "pj_widgets/SvgUtil.h"
#include "ui_FilterEditorPanel.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

// Best human-readable label for a source curve: prefer the field display fields,
// falling back to the opaque catalog key only when those are empty.
//
// A materialized filter output is a single-column topic whose column is the
// generic "value" (incremental_engine.cpp:413) — its identity lives in the topic name
// (the alias, e.g. "/angular_velocity/y[Absolute]"), so a bare "value" field is
// skipped in favour of the topic name rather than shown verbatim.
QString readableName(const CurveDescriptor& descriptor) {
  if (!descriptor.field_name.isEmpty() && descriptor.field_name != QLatin1String("value")) {
    return curveDisplayName(descriptor);  // full "topic/field" path, as the legend shows it
  }
  if (!descriptor.topic_name.isEmpty()) {
    return descriptor.topic_name;
  }
  if (!descriptor.field_path.isEmpty()) {
    return descriptor.field_path;
  }
  return descriptor.name;
}

// The catalog key identifying one column of a topic in a dataset — the canonical
// "dataset:.../topic:.../column:..." string the catalog and host plots key curves by.
QString curveKey(DatasetId dataset_id, TopicId topic_id, std::size_t column) {
  return u"dataset:%1/topic:%2/column:%3"_s.arg(dataset_id).arg(topic_id).arg(column);
}

// Synthetic curve key/title for the in-memory filtered preview curve — never a
// catalog key, so it lives only inside the preview PlotWidget.
const QString kFilteredPreviewTitle = u"__filter_preview__"_s;

// Process-global "copied filter" (id + params JSON) — survives across panel
// instances so a filter can be copied from one plot and pasted onto another.
struct ClipboardFilter {
  std::string id;
  std::string params;
};
ClipboardFilter& filterClipboard() {
  static ClipboardFilter clip;
  return clip;
}

}  // namespace

FilterEditorPanel::FilterEditorPanel(
    SessionManager* session, CatalogModel* catalog, std::vector<CurveDescriptor> sources,
    QHash<QString, QColor> source_colors, QWidget* parent)
    : QWidget(parent),
      ui_(std::make_unique<Ui::FilterEditorPanel>()),
      session_(session),
      catalog_(catalog),
      sources_(std::move(sources)),
      source_colors_(std::move(source_colors)) {
  ui_->setupUi(this);

  populateSources();
  populateTransforms();

  connect(ui_->transform_list, &QListWidget::currentRowChanged, this, [this](int) { onTransformChanged(); });

  // Primary action: Apply (materialize + replace in place). "Generate time
  // series" (an additive new series) is deferred under the replace-in-place
  // model, so its button is hidden for now.
  ui_->save_btn->setText(tr("Apply"));
  connect(ui_->save_btn, &QPushButton::clicked, this, [this]() { applyToSelected(); });
  ui_->generate_btn->setVisible(false);

  ui_->cancel_btn->setText(tr("Close"));
  connect(ui_->cancel_btn, &QPushButton::clicked, this, [this]() { emit closed(); });

  // The generated parameter editor replaces the 7 hardcoded panel_* widgets. Its
  // schema comes from the bundled Luau filter classes (read once from the app
  // resource); M5 routes this through the FilterCatalogue instead.
  filter_engine_ = scripting::makeLuauEngine();
  if (QFile f(u":/filters/builtin_filters.luau"_s); f.open(QIODevice::ReadOnly)) {
    if (auto classes = filter_engine_->inspectModule(f.readAll().toStdString(), "bundled"); classes.has_value()) {
      filter_classes_ = std::move(classes.value());
    }
  }
  if (filter_classes_.empty()) {  // the resource is embedded, so this is defensive
    ui_->status_label->setText(tr("Warning: built-in filter definitions failed to load"));
  }
  param_form_ = new ParameterForm(this);
  ui_->paramsLayout->insertWidget(0, param_form_);
  connect(param_form_, &ParameterForm::changed, this, [this]() { scheduleRefresh(); });

  // Copy / paste / apply-to-all of the visible filter parameters — icon buttons
  // in the parameters-column header (the params are what is copied), matching the
  // scene3d settings toolbar's SVGs and 20px sizing.
  const auto make_icon_button = [this](const QString& svg, const QString& tip) {
    auto* button = new QToolButton(this);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setIconSize(QSize(Style::kInputHeight, Style::kInputHeight));
    button->setFixedSize(Style::kInputHeight, Style::kInputHeight);  // 20x20, like every icon button
    button->setIcon(QIcon(loadSvg(svg, currentTheme())));
    button->setToolTip(tip);
    return button;
  };
  copy_button_ = make_icon_button(u":/resources/svg/copy.svg"_s, tr("Copy parameters"));
  paste_button_ = make_icon_button(u":/resources/svg/paste.svg"_s, tr("Paste parameters"));
  apply_all_button_ =
      make_icon_button(u":/resources/svg/format_paint.svg"_s, tr("Copy these parameters into all series"));
  copy_button_->setObjectName(u"filter_copy_btn"_s);
  paste_button_->setObjectName(u"filter_paste_btn"_s);
  apply_all_button_->setObjectName(u"filter_apply_all_btn"_s);
  ui_->autozoomRow->addWidget(copy_button_);
  ui_->autozoomRow->addWidget(paste_button_);
  ui_->autozoomRow->addWidget(apply_all_button_);
  connect(copy_button_, &QToolButton::clicked, this, &FilterEditorPanel::onCopy);
  connect(paste_button_, &QToolButton::clicked, this, &FilterEditorPanel::onPaste);
  connect(apply_all_button_, &QToolButton::clicked, this, &FilterEditorPanel::onApplyToAll);

  // Once the user types an alias, stop auto-overwriting it (textEdited fires only
  // on user input, never on our programmatic setText).
  connect(ui_->alias_edit, &QLineEdit::textEdited, this, [this]() { alias_user_edited_ = true; });

  // Preview vs controls split ~50/50 by default; the user can drag the handle.
  ui_->previewSplitter->setStretchFactor(0, 1);
  ui_->previewSplitter->setStretchFactor(1, 1);
  ui_->previewSplitter->setSizes({10000, 10000});

  // The first source is pre-selected (populateSources), but its selection signal
  // fired before our handler was connected; seed active_source_key_ so its
  // transform is remembered when the user switches away.
  active_source_key_ = sources_.empty() ? QString() : sources_.front().name;

  setupPreview();

  // The filtered preview curve bakes the display offset into its x-values at compute
  // time (previewFilteredPoints), while the datastore-backed ghost reads the offset
  // live — PlotWidget re-reads only its DatastoreCurveAdapter curves in its own
  // displayOffsetChanged handler. So when the t0 toggle flips the frame, the ghost
  // moves but the filtered curve is stranded in the old frame. Recompute it here, then
  // re-fit so both curves frame correctly regardless of slot order.
  if (session_ != nullptr) {
    connect(session_, qOverload<>(&SessionManager::displayOffsetChanged), this, [this]() {
      refreshPreview();
      if (preview_plot_ != nullptr) {
        preview_plot_->zoomOut(false);
      }
    });
  }

  onTransformChanged();
  // Process the pre-selected source explicitly (its selection signal fired before
  // setupPreview() connected our handler), so opening on a single already-filtered
  // curve enters EDIT mode and shows its transform instead of "No Transform".
  if (!sources_.empty()) {
    onSourceSelectionChanged();
  }
}

FilterEditorPanel::~FilterEditorPanel() = default;

QColor FilterEditorPanel::colorForSource(const CurveDescriptor& descriptor) const {
  if (const auto it = source_colors_.constFind(descriptor.name); it != source_colors_.constEnd() && it->isValid()) {
    return *it;
  }
  // Deterministic palette colour by the source's position in the list.
  int index = 0;
  for (std::size_t i = 0; i < sources_.size(); ++i) {
    if (sources_[i].name == descriptor.name) {
      index = static_cast<int>(i);
      break;
    }
  }
  return PlotWidgetBase::paletteColor(index);
}

CurveDescriptor FilterEditorPanel::originalInputOf(const CurveDescriptor& descriptor) const {
  if (session_ != nullptr && catalog_ != nullptr) {
    if (const auto* recipe = session_->dataProcessorService().filterConfig(descriptor.topic_id)) {
      const QString input_key = curveKey(recipe->dataset_id, recipe->input_topic_id, recipe->input_column_index);
      if (const auto input = catalog_->curveDescriptor(input_key); input.has_value()) {
        return *input;
      }
    }
  }
  return descriptor;
}

void FilterEditorPanel::populateSources() {
  ui_->series_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
  for (std::size_t i = 0; i < sources_.size(); ++i) {
    // A source that is itself a filter output is labelled by its ORIGINAL input
    // series (not the output alias), so reopening reads as "edit <input>".
    auto* item = new QListWidgetItem(readableName(originalInputOf(sources_[i])), ui_->series_list);
    item->setData(Qt::UserRole, static_cast<int>(i));
  }
  if (!sources_.empty()) {
    ui_->series_list->setCurrentRow(0);
  }
}

void FilterEditorPanel::populateTransforms() {
  ui_->transform_list->clear();
  auto* none_item = new QListWidgetItem(tr("-- No Transform --"), ui_->transform_list);
  none_item->setData(Qt::UserRole, QString("none"));

  for (const auto& [id, label] : session_->dataProcessorService().availableFilters()) {
    if (id == "none") {
      continue;
    }
    auto* item = new QListWidgetItem(QString::fromStdString(label), ui_->transform_list);
    item->setData(Qt::UserRole, QString::fromStdString(id));
  }

  // (The freeform "Lua Function" custom-function entry returns once it ships as a
  // Luau filter class — until then it would be an inert, empty-form selection.)

  // Default to "-- No Transform --" (row 0): a freshly-opened source shows its
  // original signal, not a filter the user didn't ask for.
  ui_->transform_list->setCurrentRow(0);
}

std::string FilterEditorPanel::currentFilterId() const {
  const QListWidgetItem* item = ui_->transform_list->currentItem();
  return item ? item->data(Qt::UserRole).toString().toStdString() : std::string("none");
}

void FilterEditorPanel::onTransformChanged() {
  const std::string id = currentFilterId();

  // Generate the parameter editor for this filter's schema (empty for
  // parameterless filters: none/absolute/time_since_previous).
  param_form_->setSchema(schemaForId(id));

  updateAlias();
  updateApplyEnabled();
  scheduleRefresh();
}

void FilterEditorPanel::updateApplyEnabled() {
  // In EDIT mode Apply is always live: a transform updates the filter, "No
  // Transform" removes it (reverting to the input). Otherwise Apply lights up when
  // any source is configured (the visible transform or a remembered per-source one).
  ui_->save_btn->setEnabled(edit_recipe_.has_value() || currentFilterId() != "none" || !source_filters_.isEmpty());
}

void FilterEditorPanel::updateAlias() {
  // The alias is editable; once the user has typed, never overwrite it.
  if (alias_user_edited_) {
    return;
  }
  const std::string id = currentFilterId();
  if (id == "none") {
    ui_->alias_edit->clear();
    return;
  }

  // Primary source = the first selected curve, else the first available curve.
  // Resolve a filter-output source to its original input so the alias reads
  // "<input>[Transform]", never "<input>[Old][New]".
  QString primary;
  const QList<QListWidgetItem*> selected = ui_->series_list->selectedItems();
  if (!selected.isEmpty()) {
    const int idx = selected.front()->data(Qt::UserRole).toInt();
    primary = readableName(originalInputOf(sources_[static_cast<std::size_t>(idx)]));
  } else if (!sources_.empty()) {
    primary = readableName(originalInputOf(sources_[0]));
  }

  const QListWidgetItem* transform_item = ui_->transform_list->currentItem();
  const QString label = transform_item ? transform_item->text() : QString();
  ui_->alias_edit->setText(primary + "[" + label + "]");
}

void FilterEditorPanel::setPreviewDisplay(bool grid_visible, PlotWidgetBase::CurveStyle style, LineWidth width) {
  preview_grid_ = grid_visible;
  preview_curve_style_ = style;
  preview_line_width_ = width;
  applyPreviewDisplay();
  applyGhostPens();  // the plot-level restyle re-penned every curve; restore the ghost dash
  if (preview_plot_ != nullptr) {
    preview_plot_->replot();
  }
}

void FilterEditorPanel::applyPreviewDisplay() {
  if (preview_plot_ == nullptr) {
    return;
  }
  // Plot-level setters mirror how a real plot pens its curves — correct dot vs line
  // pen widths, inherited by curves added later — so the preview matches the origin
  // plot faithfully. They re-pen every curve, so ghost dashes must be re-applied after.
  preview_plot_->setGridVisible(preview_grid_);
  preview_plot_->setDefaultStyle(preview_curve_style_);
  preview_plot_->setLineWidth(preview_line_width_);
}

void FilterEditorPanel::applyGhostPens() {
  for (const PreviewSeries& entry : preview_series_) {
    if (entry.ghost == nullptr) {
      continue;
    }
    // Active source -> faded dashed "before"; inactive ("No Transform") -> plain solid.
    // entry.active is resolved once per refreshPreview() (see there); reused here so a
    // reactive display change doesn't rebuild a Luau VM per ghost.
    QColor ghost_color = entry.color;
    QPen pen = entry.ghost->pen();
    if (entry.active) {
      ghost_color.setAlpha(90);
      pen.setStyle(Qt::DashLine);
    } else {
      pen.setStyle(Qt::SolidLine);
    }
    pen.setColor(ghost_color);
    entry.ghost->setPen(pen);
  }
}

void FilterEditorPanel::setupPreview() {
  auto* layout = new QVBoxLayout(ui_->chart_preview);
  // Keep a snug amount of breathing room below the plot.
  layout->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::Snug));

  // A real PlotWidget (zoom / legend / tracker), not a bare QwtPlot. It reads the
  // same datastore, so the ghost (source) curve renders natively; the filtered
  // curve is an in-memory series fed below.
  preview_plot_ = new PlotWidget(session_, catalog_, ui_->chart_preview);
  preview_plot_->setContextMenuEnabled(false);  // read-only preview: no right-click menu
  applyPreviewDisplay();                        // honor any display settings the host pushed pre-setup
  layout->addWidget(preview_plot_);

  preview_timer_ = new QTimer(this);
  preview_timer_->setSingleShot(true);
  preview_timer_->setInterval(150);
  connect(preview_timer_, &QTimer::timeout, this, &FilterEditorPanel::refreshPreview);

  // Recompute whenever any parameter / source / transform changes (debounced).
  for (QLineEdit* widget : findChildren<QLineEdit*>()) {
    connect(widget, &QLineEdit::textChanged, this, [this]() { scheduleRefresh(); });
  }
  for (QSpinBox* widget : findChildren<QSpinBox*>()) {
    connect(widget, qOverload<int>(&QSpinBox::valueChanged), this, [this]() { scheduleRefresh(); });
  }
  for (QDoubleSpinBox* widget : findChildren<QDoubleSpinBox*>()) {
    connect(widget, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this]() { scheduleRefresh(); });
  }
  for (QCheckBox* widget : findChildren<QCheckBox*>()) {
    connect(widget, &QCheckBox::toggled, this, [this]() { scheduleRefresh(); });
  }
  for (QRadioButton* widget : findChildren<QRadioButton*>()) {
    connect(widget, &QRadioButton::toggled, this, [this]() { scheduleRefresh(); });
  }
  for (QPlainTextEdit* widget : findChildren<QPlainTextEdit*>()) {
    connect(widget, &QPlainTextEdit::textChanged, this, [this]() { scheduleRefresh(); });
  }
  connect(ui_->series_list, &QListWidget::itemSelectionChanged, this, &FilterEditorPanel::onSourceSelectionChanged);
}

void FilterEditorPanel::onSourceSelectionChanged() {
  // Remember the transform of the source we are leaving, then restore the one we
  // are entering — so each series keeps its own transform instead of inheriting
  // whatever was last configured.
  saveActiveSourceFilter();
  alias_user_edited_ = false;  // the incoming source gets a fresh auto-alias

  edit_recipe_.reset();
  const QList<QListWidgetItem*> selected = ui_->series_list->selectedItems();
  const CurveDescriptor* primary =
      selected.isEmpty() ? nullptr : &sources_[static_cast<std::size_t>(selected.front()->data(Qt::UserRole).toInt())];
  const QString new_key = primary ? primary->name : QString();

  // A single selected source that is itself a filter OUTPUT → edit that filter.
  bool loaded_from_recipe = false;
  if (selected.size() == 1 && primary != nullptr && session_ != nullptr) {
    if (const auto* recipe = session_->dataProcessorService().filterConfig(primary->topic_id)) {
      if (selectTransformById(recipe->processor_id)) {  // shows the matching panel
        edit_recipe_ = *recipe;
        if (recipe->processor) {
          // The recipe has a LIVE processor; drive the form from its params JSON.
          setWidgetsFromParams(recipe->processor_id, recipe->processor->saveParams());
        }
      } else {
        // The restored filter's transform is not available in this build: show "No
        // Transform" + a notice rather than silently editing an unrelated filter.
        ui_->transform_list->setCurrentRow(0);
        ui_->status_label->setText(
            tr("Filter '%1' is not available").arg(QString::fromStdString(recipe->processor_id)));
      }
      loaded_from_recipe = true;
    }
  }
  // Otherwise restore this source's remembered transform (or "No Transform").
  if (!loaded_from_recipe && primary != nullptr) {
    loadSourceFilter(new_key);
  }

  active_source_key_ = new_key;
  updateAlias();
  updateApplyEnabled();
  // The button always reads "Apply" (even in EDIT mode, where it updates in place).
  ui_->save_btn->setText(tr("Apply"));
  scheduleRefresh();
}

void FilterEditorPanel::saveActiveSourceFilter() {
  if (active_source_key_.isEmpty()) {
    return;
  }
  // Record the active source's visible config — INCLUDING for an existing filter
  // output in EDIT mode, so a pasted/edited filter survives a selection change and
  // Apply re-edits it in place. (No early-return on edit_recipe_: that previously
  // dropped the edited config.)
  if (std::unique_ptr<proc::DataProcessor> processor = buildConfiguredProcessor()) {
    source_filters_[active_source_key_] = {
        QString::fromStdString(processor->id()), QString::fromStdString(processor->saveParams())};
  } else {
    source_filters_.remove(active_source_key_);  // "No Transform" -> no entry
  }
}

void FilterEditorPanel::loadSourceFilter(const QString& source_key) {
  const auto it = source_filters_.constFind(source_key);
  if (it == source_filters_.constEnd()) {
    ui_->transform_list->setCurrentRow(0);  // No Transform
    return;
  }
  const std::string id = it.value().first.toStdString();
  if (!selectTransformById(id)) {
    ui_->transform_list->setCurrentRow(0);  // remembered filter no longer available
    return;
  }
  setWidgetsFromParams(id, it.value().second.toStdString());
}

bool FilterEditorPanel::selectTransformById(const std::string& id) {
  for (int row = 0; row < ui_->transform_list->count(); ++row) {
    if (ui_->transform_list->item(row)->data(Qt::UserRole).toString().toStdString() == id) {
      ui_->transform_list->setCurrentRow(row);
      return true;
    }
  }
  return false;
}

void FilterEditorPanel::setWidgetsFromParams(const std::string& id, const std::string& params_json) {
  // Generic inverse of buildConfiguredProcessor(): rebuild the form for this
  // filter's schema, then drive it from the params JSON (the keys are the
  // parameter names) — no per-id static_cast, no throwaway processor.
  param_form_->setSchema(schemaForId(id));
  // allow_exceptions=false returns a DISCARDED value on malformed JSON; setValues
  // would then silently skip every row, leaving stale params. Detect and surface it.
  const nlohmann::json values = nlohmann::json::parse(params_json, nullptr, /*allow_exceptions=*/false);
  if (values.is_discarded()) {
    ui_->status_label->setText(tr("Could not read filter parameters; using defaults"));
  } else {
    param_form_->setValues(values);
  }
  // setValues is silent (no changed()), so refresh the preview explicitly — e.g.
  // pasting onto the already-selected filter wouldn't otherwise re-render.
  scheduleRefresh();
}

const std::vector<scripting::ParamSpec>& FilterEditorPanel::schemaForId(const std::string& id) const {
  static const std::vector<scripting::ParamSpec> kEmpty;
  for (const scripting::FilterClass& c : filter_classes_) {
    if (c.id == id) {
      return c.parameters;
    }
  }
  return kEmpty;
}

void FilterEditorPanel::onCopy() {
  std::unique_ptr<proc::DataProcessor> processor = buildConfiguredProcessor();
  if (!processor) {
    ui_->status_label->setText(tr("Pick a filter to copy"));
    return;
  }
  filterClipboard() = {processor->id(), processor->saveParams()};
  ui_->status_label->setText(tr("Filter copied"));
}

void FilterEditorPanel::onPaste() {
  const ClipboardFilter& clip = filterClipboard();
  if (clip.id.empty()) {
    ui_->status_label->setText(tr("Nothing to paste"));
    return;
  }
  // Select the transform row FIRST; only drive the form for clip.id once it is the
  // actually-selected transform, or the form schema would mismatch currentFilterId().
  if (!selectTransformById(clip.id)) {
    ui_->status_label->setText(tr("Cannot paste filter '%1'").arg(QString::fromStdString(clip.id)));
    return;
  }
  setWidgetsFromParams(clip.id, clip.params);
  ui_->status_label->setText(tr("Filter pasted"));
}

void FilterEditorPanel::onApplyToAll() {
  // "Copy into all others": stamp the visible filter onto every source's
  // remembered transform, so switching to any series shows (and Apply applies) it.
  std::unique_ptr<proc::DataProcessor> processor = buildConfiguredProcessor();
  if (!processor) {
    ui_->status_label->setText(tr("Pick a filter to copy to all series"));
    return;
  }
  const QPair<QString, QString> config{
      QString::fromStdString(processor->id()), QString::fromStdString(processor->saveParams())};
  for (const CurveDescriptor& source : sources_) {
    source_filters_[source.name] = config;
  }
  ui_->status_label->setText(tr("Copied to all %1 series").arg(sources_.size()));
}

void FilterEditorPanel::scheduleRefresh() {
  if (preview_timer_) {
    preview_timer_->start();
  }
}

void FilterEditorPanel::refreshPreview() {
  if (!preview_plot_ || session_ == nullptr) {
    return;
  }

  // Sources to preview = every selected curve (in list order), or the first
  // available if nothing is selected. Each gets its own ghost + filtered pair, so a
  // multi-select edit previews EVERY series Apply will touch — not just the first.
  std::vector<int> indices;
  const QList<QListWidgetItem*> selected = ui_->series_list->selectedItems();
  if (!selected.isEmpty()) {
    indices.reserve(static_cast<std::size_t>(selected.size()));
    for (const QListWidgetItem* item : selected) {
      indices.push_back(item->data(Qt::UserRole).toInt());
    }
    std::sort(indices.begin(), indices.end());  // list order, not selection-click order
  } else if (!sources_.empty()) {
    indices.push_back(0);
  }

  // Resolve each to its EFFECTIVE input (a filter-output source previews its ORIGINAL
  // input, so re-editing reads as "input -> re-filtered output") and the SELECTED
  // curve's plot colour. De-duplicate by input so two outputs of the same input
  // collapse to one ghost.
  std::vector<PreviewSeries> wanted;
  QString set_key;
  for (const int idx : indices) {
    if (idx < 0 || static_cast<std::size_t>(idx) >= sources_.size()) {
      continue;
    }
    const CurveDescriptor& source = sources_[static_cast<std::size_t>(idx)];
    const CurveDescriptor input = originalInputOf(source);
    if (std::any_of(wanted.begin(), wanted.end(), [&](const PreviewSeries& s) { return s.input.name == input.name; })) {
      continue;
    }
    wanted.push_back(PreviewSeries{.input = input, .source_key = source.name, .color = colorForSource(source)});
    set_key += input.name;
    set_key += QChar('\n');
  }
  if (wanted.empty()) {
    return;
  }

  // Rebuild curves only when the SET of ghosts changes, so tuning a parameter keeps
  // the user's current zoom on the preview.
  const bool rebuilt = (set_key != preview_set_key_);
  if (rebuilt) {
    preview_plot_->removeAllCurves();
    preview_set_key_ = set_key;
    for (PreviewSeries& entry : wanted) {
      // Ghost = the datastore-backed source. Its dash/fade styling is set below.
      if (auto* ghost = preview_plot_->addCurve(entry.input.name, entry.color); ghost != nullptr) {
        entry.ghost = ghost->curve;
      }
      // Filtered = a lazy datastore-backed curve that runs THIS source's filter
      // over the input column on read. It reports the input topic as its source,
      // so PlotWidget's samplesIngested handler refreshes it on the SAME signal as
      // the ghost — the filtered "after" tracks streaming with no timer/extra
      // wiring here. The factory builds a FRESH processor each recompute from the
      // CURRENT config, so parameter edits take effect on the next invalidate().
      // Keyed uniquely PER source (the `__filter_preview__` prefix never collides
      // with a catalog topic). Legend title + visibility are set below.
      auto factory = [this, source_key = entry.source_key]() -> std::unique_ptr<proc::DataProcessor> {
        return processorForSource(source_key);
      };
      auto* series = new FilteredCurveAdapter(session_, entry.input, std::move(factory));
      const QString filtered_key = kFilteredPreviewTitle + entry.input.name;
      if (auto* info = preview_plot_->addCurve(filtered_key, series, entry.color, tr("filtered")); info != nullptr) {
        entry.filtered = info->curve;
      }
    }
    preview_series_ = std::move(wanted);
  } else {
    // Same ghost set: keep the curves, just refresh each remembered colour (a curve
    // may have been recoloured on the plot since the last preview).
    for (std::size_t i = 0; i < preview_series_.size() && i < wanted.size(); ++i) {
      preview_series_[i].color = wanted[i].color;
    }
  }

  // Resolve each source's active state ONCE — processorForSource() builds a Luau VM,
  // so it must not be re-derived per redraw. Each source previews through its OWN
  // filter (matching Apply): a source on "No Transform" stays plain even while another
  // in the multi-selection is filtered. Reused by applyGhostPens() and the loop below.
  for (PreviewSeries& entry : preview_series_) {
    entry.active = (processorForSource(entry.source_key) != nullptr);
  }

  // Adopt the origin plot's grid/style/width on the (possibly just-rebuilt) curves
  // BEFORE the ghost-pen styling, so the ghost's dash/fade is applied last and wins.
  applyPreviewDisplay();
  applyGhostPens();

  const QListWidgetItem* transform_item = ui_->transform_list->currentItem();
  const QString transform_label = transform_item ? transform_item->text() : QString();
  const QString alias = ui_->alias_edit->text();

  for (PreviewSeries& entry : preview_series_) {
    const bool entry_active = entry.active;

    if (entry.filtered != nullptr) {
      // Legend label: the single-source case keeps the user-editable alias; a
      // multi-select preview labels each filtered curve by its own output name so
      // the legend stays unambiguous.
      QString title;
      if (preview_series_.size() == 1 && !alias.isEmpty()) {
        title = alias;
      } else {
        title = readableName(entry.input) + "[" + transform_label + "]";
      }
      entry.filtered->setTitle(title.isEmpty() ? tr("filtered") : title);
      // The adapter computes its own samples lazily; just mark it stale so a
      // parameter edit (which keeps the same ghost set) re-runs the filter on the
      // next paint. Streaming ingest invalidates it independently via PlotWidget's
      // samplesIngested handler. When inactive ("No Transform") the factory returns
      // nullptr -> empty curve, so hiding it is enough.
      if (auto* adapter = dynamic_cast<FilteredCurveAdapter*>(entry.filtered->data()); adapter != nullptr) {
        adapter->invalidate();
      }
      entry.filtered->setVisible(entry_active);
    }
  }

  preview_plot_->replot();
  // Refit the view to the data: always when the ghost set changed, and on every
  // refresh while AutoZoom is on (so changing the transform — e.g. a Binary Filter
  // whose output reaches 1 — rescales the Y axis instead of clipping).
  if (rebuilt || ui_->autozoom_check->isChecked()) {
    preview_plot_->zoomOut(false);
  }
}

std::unique_ptr<proc::DataProcessor> FilterEditorPanel::buildConfiguredProcessor() const {
  return buildProcessorFor(currentFilterId(), param_form_->values().dump());
}

std::unique_ptr<proc::DataProcessor> FilterEditorPanel::processorForSource(const QString& source_key) const {
  // A source with its OWN remembered config previews/applies through that; an unconfigured one
  // falls back to the visible form (which is also what applyToSelected assigns it). This keeps a
  // multi-source preview consistent with Apply instead of routing every series through the
  // currently-visible filter.
  if (const auto it = source_filters_.constFind(source_key); it != source_filters_.constEnd()) {
    return buildProcessorFor(it->first.toStdString(), it->second.toStdString());
  }
  return buildConfiguredProcessor();
}

std::unique_ptr<proc::DataProcessor> FilterEditorPanel::buildProcessorFor(
    const std::string& id, const std::string& params_json) const {
  if (id.empty() || id == "none") {
    return nullptr;  // "No Transform"
  }
  // Produce the Luau filter for this id (configured from the params JSON, whose
  // keys are the parameter names). The bundled resource is the only source; an id
  // absent from it yields no processor (nullptr -> "Pick a filter").
  for (const scripting::FilterClass& cls : filter_classes_) {
    if (cls.id == id) {
      return std::make_unique<scripting::LuaSisoTransform>(filter_engine_, cls, params_json);
    }
  }
  return nullptr;
}

QString FilterEditorPanel::labelForId(const std::string& id) const {
  for (const scripting::FilterClass& cls : filter_classes_) {
    if (cls.id == id) {
      return QString::fromStdString(cls.name);
    }
  }
  return QString::fromStdString(id);
}

void FilterEditorPanel::applyToSelected() {
  const QList<QListWidgetItem*> selected = ui_->series_list->selectedItems();
  if (selected.isEmpty()) {
    ui_->status_label->setText(tr("Select at least one source curve"));
    return;
  }

  auto& service = session_->dataProcessorService();

  // Record the visible source's config into per-source memory FIRST, so a pasted /
  // edited filter on the source you are looking at (even an existing output) is
  // included — this is the single authority Apply reads, same as the preview.
  saveActiveSourceFilter();

  const std::string visible_id = currentFilterId();
  QList<QPair<QString, QString>> replacements;
  QStringList failures;

  // Removal: the active source is an existing filter OUTPUT set to "No Transform" ->
  // remove its filter and revert the plotted curve to its original input. (Collected,
  // not returned, so OTHER configured sources in the same Apply still run.)
  if (!active_source_key_.isEmpty() && visible_id == "none") {
    for (const CurveDescriptor& descriptor : sources_) {
      if (descriptor.name != active_source_key_) {
        continue;
      }
      if (const auto* recipe = service.filterConfig(descriptor.topic_id)) {
        const QString output_key = curveKey(recipe->dataset_id, recipe->output_topic_id, 0);
        const QString input_key = curveKey(recipe->dataset_id, recipe->input_topic_id, recipe->input_column_index);
        if (const auto removed = service.removeFilter(recipe->node_id); removed.has_value()) {
          // Drop the now-orphaned output topic (removeNode leaves the series in the
          // store); the trash blacklist keeps it off the next rebuild.
          catalog_->removeCurves({output_key});
          replacements.push_back({output_key, input_key});  // host swaps output -> input
        } else {
          failures << tr("remove %1: %2").arg(readableName(descriptor), QString::fromStdString(removed.error()));
        }
      }
      break;
    }
  }

  // Plain multi-selection convenience: a selected source with no remembered
  // transform of its own inherits the currently-visible one, so ctrl-selecting N
  // curves and picking a single filter applies it to all N.
  if (visible_id != "none") {
    const QString visible_params = QString::fromStdString(param_form_->values().dump());
    for (const QListWidgetItem* item : selected) {
      const QString key = sources_[static_cast<std::size_t>(item->data(Qt::UserRole).toInt())].name;
      if (!source_filters_.contains(key)) {
        source_filters_[key] = {QString::fromStdString(visible_id), visible_params};
      }
    }
  }

  if (source_filters_.isEmpty() && replacements.isEmpty() && failures.isEmpty()) {
    ui_->status_label->setText(tr("Pick a filter to apply"));
    return;
  }

  // Apply every configured source, in list order. A source that is itself a filter
  // OUTPUT is RE-EDITED IN PLACE (its recipe is updated, re-running on the original
  // input — consistent with the preview); a plain source materializes a NEW output
  // topic and is reported for in-place curve replacement on the host plot. Per-source
  // failures are collected (not silently skipped, not masked by a later success).
  bool updated_in_place = false;
  const bool single = source_filters_.size() == 1;
  for (const CurveDescriptor& descriptor : sources_) {
    const auto cfg = source_filters_.constFind(descriptor.name);
    if (cfg == source_filters_.constEnd()) {
      continue;  // this source is left on "No Transform"
    }
    const QString display_name = readableName(descriptor);
    const std::string id = cfg->first.toStdString();
    std::unique_ptr<proc::DataProcessor> processor = buildProcessorFor(id, cfg->second.toStdString());
    if (!processor) {
      failures << tr("%1: filter '%2' is unavailable").arg(display_name, QString::fromStdString(id));
      continue;
    }

    if (const auto* recipe = service.filterConfig(descriptor.topic_id)) {
      // Existing filter output -> re-edit its recipe in place (re-runs on its input).
      std::shared_ptr<proc::DataProcessor> shared = std::move(processor);
      if (const auto updated = service.updateFilter(recipe->node_id, shared); updated.has_value()) {
        // The output's data changed under existing adapters; notify so plots drop
        // their cache and repaint (a bare replot keeps the stale samples).
        session_->notifyIngest({recipe->output_topic_id});
        updated_in_place = true;
      } else {
        failures << tr("update %1: %2").arg(display_name, QString::fromStdString(updated.error()));
      }
      continue;
    }

    // Plain source -> materialize a new output and replace the curve in place. A lone
    // configured source honours the user's custom alias; with several, each output is
    // named per source so they stay distinct.
    QString out_name;
    if (single && !ui_->alias_edit->text().isEmpty()) {
      out_name = ui_->alias_edit->text();
    } else {
      out_name = display_name + "[" + labelForId(id) + "]";
    }
    auto result = service.applyFilter(
        descriptor.topic_id, descriptor.dataset_id, std::move(processor), out_name.toStdString(),
        descriptor.column_index);
    if (!result.has_value()) {
      failures << tr("%1: %2").arg(display_name, QString::fromStdString(result.error()));
      continue;
    }
    const QString output_key = curveKey(result->dataset_id, result->output_topic_id, 0);
    replacements.push_back({descriptor.name, output_key});
  }

  catalog_->rebuildFromDatastore();

  // Surface any failures both inline AND as an app diagnostic (which survives the
  // panel closing), so a partial failure is never masked by a later success.
  if (!failures.isEmpty()) {
    const QString summary = tr("%1 filter(s) failed: %2").arg(failures.size()).arg(failures.join(u"; "_s));
    ui_->status_label->setText(summary);
    emit diagnostic(summary);
  }

  if (!replacements.isEmpty() || updated_in_place) {
    // Non-empty -> the host swaps each source curve for its output; empty (only
    // in-place updates) -> the host just replots and restores the chart area.
    emit applied(replacements);
  } else if (failures.isEmpty()) {
    ui_->status_label->setText(tr("Nothing applied"));
  }
  // failures-only (no successes): no applied() -> the panel stays open with the
  // failure summary visible.
}

}  // namespace PJ
