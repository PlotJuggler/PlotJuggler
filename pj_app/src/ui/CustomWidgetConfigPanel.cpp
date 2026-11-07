// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "ui/CustomWidgetConfigPanel.h"

#include <QColor>
#include <QDomDocument>
#include <QDomElement>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

#include "pj_custom_widgets/CustomWidgetDockWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_widgets/ColorPickerWidget.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/ElidingLabel.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/Search.h"
#include "pj_widgets/SectionHeaderBand.h"
#include "pj_widgets/SvgButton.h"
#include "pj_widgets/SvgUtil.h"
#include "pj_widgets/ToggleSwitch.h"
#include "ui_CustomWidgetConfigPanel.h"

using namespace Qt::StringLiterals;

namespace PJ {

namespace {

// Dynamic properties the tests and the stylesheet key on.
constexpr const char* kInputNameProperty = "pjInputName";
constexpr const char* kSlotProperty = "pjSlot";
constexpr const char* kSettingKeyProperty = "pjSettingKey";
constexpr const char* kMutedProperty = "pjMuted";

constexpr double kUnboundedNumber = 1e9;

QLabel* mutedLabel(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  label->setTextFormat(Qt::PlainText);  // units and names come from the plugin
  label->setProperty(kMutedProperty, true);
  return label;
}

/// The name of a wired series, elided to the row's width.
ElidingLabel* makeSeriesLabel(QWidget* parent, const QString& text, const QString& tooltip) {
  auto* label = new ElidingLabel(parent);
  label->setObjectName(u"labelWireSeries"_s);
  label->setTextFormat(Qt::PlainText);
  label->setFullText(text);
  label->setToolTip(tooltip.toHtmlEscaped());
  return label;
}

QString colorString(const QColor& color) {
  return color.name(color.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb);
}

}  // namespace

CustomWidgetConfigPanel::CustomWidgetConfigPanel(QWidget* parent)
    : QWidget(parent), ui_(std::make_unique<Ui::CustomWidgetConfigPanel>()) {
  ui_->setupUi(this);
  // Both quote plugin- or layout-provided strings; QLabel's auto-detection must not treat them as markup.
  ui_->labelPluginName->setTextFormat(Qt::PlainText);
  ui_->labelMissingReason->setTextFormat(Qt::PlainText);
  const int comfortable = theme::space(theme::Space::Comfortable);
  const int snug = theme::space(theme::Space::Snug);
  for (QBoxLayout* inset : {ui_->headerBodyLayout, ui_->inputsLayout, ui_->settingsHostLayout}) {
    inset->setContentsMargins(comfortable, snug, comfortable, snug);
    inset->setSpacing(snug);
  }
  ui_->chooserFooterLayout->setContentsMargins(comfortable, snug, comfortable, snug);
  ui_->chooserFooterLayout->setSpacing(snug);
  ui_->chooserFooterLayout->setStretch(0, 1);
  ui_->settingsForm->setHorizontalSpacing(comfortable);
  ui_->settingsForm->setVerticalSpacing(snug);
  ui_->labelMissingReason->hide();

  ui_->chooserTree->setViewMode(CurveTreeView::ViewMode::kShowTopics);
  ui_->chooserTree->setValuesColumnHidden(true);
  ui_->chooserTree->setSelectionMode(QAbstractItemView::SingleSelection);
  if (Search* search = ui_->chooserBand->filterSearch(); search != nullptr) {
    connect(search, &Search::textChanged, ui_->chooserTree, &CurveTreeView::applyFilter);
  }
  connect(ui_->chooserTree, &QTreeWidget::itemActivated, this, &CustomWidgetConfigPanel::onChooserActivated);
  connect(ui_->buttonCancelChooser, &QPushButton::clicked, this, &CustomWidgetConfigPanel::closeChooser);

  settings_debounce_.setSingleShot(true);
  settings_debounce_.setInterval(kSettingsDebounceMs);
  connect(&settings_debounce_, &QTimer::timeout, this, &CustomWidgetConfigPanel::flushPendingSettings);

  rebuild();
}

CustomWidgetConfigPanel::~CustomWidgetConfigPanel() = default;

void CustomWidgetConfigPanel::setServices(CatalogModel* catalog, ExtensionCatalogService* extensions) {
  catalog_ = catalog;
  extensions_ = extensions;
}

void CustomWidgetConfigPanel::bindDock(CustomWidgetDockWidget* dock) {
  if (dock_.data() == dock) {
    return;
  }
  flushPendingSettings();
  if (dock_ != nullptr) {
    disconnect(dock_.data(), nullptr, this, nullptr);
  }
  dock_ = dock;
  closeChooser();
  rebuild();
  if (dock == nullptr) {
    return;
  }
  connect(dock, &CustomWidgetDockWidget::inputsChanged, this, [this]() { scheduleRebuild(/*with_header=*/true); });
  connect(dock, &CustomWidgetDockWidget::bindingsChanged, this, [this]() { scheduleRebuild(/*with_header=*/false); });
  // QWidget emits destroyed() after the dock's own destructor ran, while the QPointer still holds it.
  connect(dock, &QObject::destroyed, this, [this]() {
    dock_ = nullptr;
    pending_settings_ = {};
    settings_debounce_.stop();
    closeChooser();
    rebuild();
  });
}

CustomWidgetDockWidget* CustomWidgetConfigPanel::dock() const {
  return dock_.data();
}

void CustomWidgetConfigPanel::rebuild() {
  rebuildHeader();
  rebuildInputs();
  rebuildSettings();
}

void CustomWidgetConfigPanel::scheduleRebuild(bool with_header) {
  rebuild_header_ = rebuild_header_ || with_header;
  if (rebuild_scheduled_) {
    return;
  }
  rebuild_scheduled_ = true;
  QTimer::singleShot(0, this, &CustomWidgetConfigPanel::runScheduledRebuild);
}

void CustomWidgetConfigPanel::runScheduledRebuild() {
  rebuild_scheduled_ = false;
  const bool with_header = std::exchange(rebuild_header_, false);
  if (with_header) {
    rebuildHeader();
  }
  rebuildInputs();
  if (!with_header) {
    return;
  }
  if (settingsFormMatchesDock()) {
    syncSettingValues();
  } else {
    rebuildSettings();
  }
}

bool CustomWidgetConfigPanel::settingsFormMatchesDock() const {
  if (dock_ == nullptr || dock_->isMissing()) {
    return ui_->settingsHost->isHidden();
  }
  return !ui_->settingsHost->isHidden() &&
         std::ranges::equal(
             setting_controls_, dock_->settingsDeclarations(), {}, &SettingControl::key, &SettingDecl::key);
}

void CustomWidgetConfigPanel::rebuildHeader() {
  if (dock_ == nullptr) {
    ui_->headerBand->setText(tr("Custom Widget"));
    ui_->labelPluginName->clear();
    ui_->labelMissingReason->hide();
    return;
  }
  ui_->headerBand->setText(dock_->widgetName());
  const QString id = dock_->pluginId() + u'/' + dock_->widgetType();
  const LoadedWidget* library = extensions_ != nullptr ? extensions_->findWidget(dock_->pluginId()) : nullptr;
  const QString library_name =
      library != nullptr && !library->name.empty() ? QString::fromStdString(library->name) : dock_->pluginId();
  ui_->labelPluginName->setText(tr("from %1").arg(library_name));
  ui_->labelPluginName->setToolTip(id.toHtmlEscaped());
  ui_->labelMissingReason->setText(dock_->missingReason());
  ui_->labelMissingReason->setVisible(dock_->isMissing());
  applyMissingReasonStyle();
}

void CustomWidgetConfigPanel::rebuildInputs() {
  clearLayout(ui_->inputsLayout);
  if (dock_ == nullptr) {
    return;
  }
  if (dock_->isMissing()) {
    rebuildMissingWires();
    return;
  }
  const std::vector<WidgetInput>& inputs = dock_->inputs();
  if (inputs.empty()) {
    ui_->inputsLayout->addWidget(mutedLabel(tr("This widget has no inputs."), ui_->inputsHost));
    return;
  }
  const std::vector<WidgetWire>& wires = dock_->bindings();
  for (const WidgetInput& input : inputs) {
    const QString name = QString::fromStdString(input.name);
    auto* row = new QWidget(ui_->inputsHost);
    row->setObjectName(u"inputRow"_s);
    row->setProperty(kInputNameProperty, name);
    auto* row_layout = new QVBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(theme::space(theme::Space::Tight));

    auto* header = new QHBoxLayout();
    header->setSpacing(theme::space(theme::Space::Snug));
    auto* title = new QLabel(input.label.empty() ? name : QString::fromStdString(input.label), row);
    title->setObjectName(u"labelInputTitle"_s);
    title->setTextFormat(Qt::PlainText);  // the plugin's label, never markup
    title->setToolTip(name.toHtmlEscaped());
    header->addWidget(title);
    QString meta = CustomWidgetDockWidget::kindName(input.kind);
    if (!input.unit.empty()) {
      meta = QString::fromStdString(input.unit) + u" · "_s + meta;
    }
    header->addWidget(mutedLabel(meta, row));
    header->addStretch(1);
    auto* add = new QPushButton(input.multiple ? tr("Add series") : tr("Choose series"), row);
    add->setObjectName(input.multiple ? u"buttonAddSeries"_s : u"buttonChooseSeries"_s);
    add->setProperty(kInputNameProperty, name);
    add->setProperty("pjCompact", true);
    add->setCursor(Qt::PointingHandCursor);
    const std::uint32_t target_slot = input.multiple ? WidgetBindings::kNewSlot : 0U;
    connect(add, &QPushButton::clicked, this, [this, name, target_slot]() { openChooser(name, target_slot); });
    header->addWidget(add);
    row_layout->addLayout(header);

    std::vector<const WidgetWire*> own;
    for (const WidgetWire& wire : wires) {
      if (wire.input == name) {
        own.push_back(&wire);
      }
    }
    std::ranges::sort(own, {}, &WidgetWire::slot);
    if (own.empty()) {
      row_layout->addWidget(mutedLabel(tr("No series"), row));
    }
    for (const WidgetWire* wire : own) {
      auto* wire_layout = new QHBoxLayout();
      wire_layout->setSpacing(theme::space(theme::Space::Snug));
      const QString label = QString::fromStdString(wire->label);
      wire_layout->addWidget(
          makeSeriesLabel(
              row, label,
              wire->identity.dataset_source.isEmpty() ? label : wire->identity.dataset_source + u": "_s + label),
          1);
      if (!wire->resolved) {
        QLabel* pending = mutedLabel(tr("pending"), row);
        pending->setObjectName(u"labelWirePending"_s);
        pending->setToolTip(tr("Waiting for %1 to be loaded").arg(wire->identity.dataset_source.toHtmlEscaped()));
        wire_layout->addWidget(pending);
      }
      auto* remove = new SvgButton(u":/resources/svg/trash.svg"_s, SvgButton::Size::kSmaller, row);
      remove->setObjectName(u"buttonRemoveWire"_s);
      remove->setToolTip(tr("Remove this series"));
      remove->setProperty(kInputNameProperty, name);
      remove->setProperty(kSlotProperty, wire->slot);
      const std::uint32_t slot = wire->slot;
      connect(remove, &QToolButton::clicked, this, [this, name, slot]() {
        if (dock_ != nullptr) {
          dock_->removeBinding(name, slot);
        }
      });
      wire_layout->addWidget(remove);
      row_layout->addLayout(wire_layout);
    }
    ui_->inputsLayout->addWidget(row);
  }
}

void CustomWidgetConfigPanel::rebuildMissingWires() {
  // The dock keeps the layout element of a widget it could not create; its wires are only there.
  QDomDocument doc;
  const QDomElement element = dock_->xmlSaveState(doc);
  bool any = false;
  for (QDomElement input = element.firstChildElement(u"input"_s); !input.isNull();
       input = input.nextSiblingElement(u"input"_s)) {
    QString series = input.attribute(u"topic"_s);
    const QString field = input.attribute(u"field"_s);
    if (!field.isEmpty()) {
      series += u'/' + field;
    }
    auto* row = new QWidget(ui_->inputsHost);
    row->setObjectName(u"storedWireRow"_s);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::space(theme::Space::Snug));
    auto* input_name = new QLabel(input.attribute(u"name"_s), row);
    input_name->setTextFormat(Qt::PlainText);  // from the layout file, never markup
    layout->addWidget(input_name);
    layout->addWidget(makeSeriesLabel(row, series, input.attribute(u"dataset_source"_s) + u": "_s + series), 1);
    ui_->inputsLayout->addWidget(row);
    any = true;
  }
  if (!any) {
    ui_->inputsLayout->addWidget(mutedLabel(tr("No stored series."), ui_->inputsHost));
  }
}

void CustomWidgetConfigPanel::rebuildSettings() {
  setting_controls_.clear();
  while (ui_->settingsForm->rowCount() > 0) {
    ui_->settingsForm->removeRow(0);
  }
  const bool missing = dock_ == nullptr || dock_->isMissing();
  ui_->settingsBand->setVisible(!missing);
  ui_->settingsHost->setVisible(!missing);
  if (missing) {
    return;
  }

  for (const SettingDecl& setting : dock_->settingsDeclarations()) {
    SettingControl control;
    control.key = setting.key;
    const QString& key = setting.key;
    const QString& type = setting.type;
    QWidget* editor = nullptr;
    if (type == u"number"_s || type == u"integer"_s) {
      // An `integer` is the same scrubber without decimals, stored as a JSON integer: a plugin
      // reading the key as an int must not see 3.0.
      const bool whole = type == u"integer"_s;
      auto* scrubber = new DoubleScrubber(ui_->settingsHost);
      const double min = setting.min.value_or(-kUnboundedNumber);
      const double max = setting.max.value_or(kUnboundedNumber);
      scrubber->setRange(min, max);
      if (whole) {
        scrubber->setDecimals(0);
        scrubber->setSingleStep(1.0);
      } else if (setting.min && setting.max && max > min) {
        scrubber->setSingleStep((max - min) / 100.0);
      }
      connect(scrubber, &DoubleScrubber::valueChanged, this, [this, key, whole](double value) {
        editSetting(key, whole ? QJsonValue(qRound(value)) : QJsonValue(value), /*debounce=*/true);
      });
      control.set_value = [scrubber, whole](const QJsonValue& value) {
        const double v = value.toDouble(0.0);
        scrubber->setValue(whole ? double(qRound(v)) : v);
      };
      editor = scrubber;
    } else if (type == u"boolean"_s) {
      auto* toggle = new ToggleSwitch(ui_->settingsHost);
      connect(toggle, &ToggleSwitch::toggled, this, [this, key](bool checked) {
        editSetting(key, checked, /*debounce=*/false);
      });
      control.set_value = [toggle](const QJsonValue& value) {
        toggle->setChecked(value.toBool(false), /*animate=*/false);
      };
      editor = toggle;
    } else if (type == u"text"_s) {
      auto* line = new QLineEdit(ui_->settingsHost);
      connect(line, &QLineEdit::editingFinished, this, [this, key, line]() {
        const QJsonObject current = currentSettings();
        if (!current.contains(key) || current.value(key).toString() != line->text()) {
          editSetting(key, line->text(), /*debounce=*/false);
        }
      });
      control.set_value = [line](const QJsonValue& value) { line->setText(value.toVariant().toString()); };
      editor = line;
    } else if (type == u"color"_s) {
      auto* swatch = new ColorPickerWidget(ui_->settingsHost);
      connect(swatch, &ColorPickerWidget::colorChanged, this, [this, key](const QColor& color) {
        editSetting(key, colorString(color), /*debounce=*/false);
      });
      control.set_value = [swatch](const QJsonValue& value) {
        const QColor color(value.toString());
        swatch->setColor(color.isValid() ? color : QColor(Qt::black));
      };
      editor = swatch;
    } else if (type == u"enum"_s) {
      auto* combo = new ComboBox(ui_->settingsHost);
      for (const QJsonValue& option : setting.options) {
        combo->addItem(option.toVariant().toString(), option.toVariant());
      }
      connect(combo, &QComboBox::currentIndexChanged, this, [this, key, combo](int index) {
        if (index >= 0) {
          editSetting(key, QJsonValue::fromVariant(combo->itemData(index)), /*debounce=*/false);
        }
      });
      control.set_value = [combo](const QJsonValue& value) {
        combo->setCurrentIndex(std::max(0, combo->findData(value.toVariant())));
      };
      editor = combo;
    } else {
      qWarning("Custom Widget settings: key '%s' has unsupported type '%s'", qPrintable(key), qPrintable(type));
      continue;
    }
    editor->setObjectName(u"setting_"_s + key);
    editor->setProperty(kSettingKeyProperty, key);
    ui_->settingsForm->addRow(setting.label.isEmpty() ? key : setting.label, editor);
    control.editor = editor;
    setting_controls_.push_back(std::move(control));
  }
  ui_->labelNoSettings->setVisible(setting_controls_.empty());
  syncSettingValues();
}

void CustomWidgetConfigPanel::syncSettingValues() {
  if (dock_ == nullptr || setting_controls_.empty()) {
    return;
  }
  const QJsonObject current = currentSettings();
  for (const SettingControl& control : setting_controls_) {
    if (pending_settings_.contains(control.key)) {
      continue;  // the user's own edit, not applied yet
    }
    const QSignalBlocker blocker(control.editor);
    // The dock's settings hold every declared key with its manifest default; a key with no default
    // that the user never set reads as undefined and the control shows its own empty value.
    control.set_value(current.value(control.key));
  }
}

QJsonObject CustomWidgetConfigPanel::currentSettings() const {
  if (dock_ == nullptr) {
    return {};
  }
  return QJsonDocument::fromJson(dock_->settingsJson().toUtf8()).object();
}

void CustomWidgetConfigPanel::editSetting(const QString& key, const QJsonValue& value, bool debounce) {
  pending_settings_.insert(key, value);
  if (debounce) {
    settings_debounce_.start();
  } else {
    flushPendingSettings();
  }
}

void CustomWidgetConfigPanel::flushPendingSettings() {
  settings_debounce_.stop();
  if (pending_settings_.isEmpty()) {
    return;
  }
  const QJsonObject edits = std::exchange(pending_settings_, {});
  if (dock_ == nullptr) {
    return;
  }
  QJsonObject settings = currentSettings();
  for (auto it = edits.begin(); it != edits.end(); ++it) {
    settings.insert(it.key(), it.value());
  }
  dock_->applySettings(QString::fromUtf8(QJsonDocument(settings).toJson(QJsonDocument::Compact)));
}

void CustomWidgetConfigPanel::openChooser(const QString& input, std::uint32_t slot) {
  if (dock_ == nullptr) {
    return;
  }
  chooser_input_ = input;
  chooser_slot_ = slot;
  const std::string name = input.toStdString();
  const std::vector<WidgetInput>& inputs = dock_->inputs();
  const auto declared = std::ranges::find(inputs, name, &WidgetInput::name);
  const QString label =
      declared != inputs.end() && !declared->label.empty() ? QString::fromStdString(declared->label) : input;
  ui_->chooserBand->setText(tr("Series for %1").arg(label));

  CurveTreeView* tree = ui_->chooserTree;
  tree->clearCurves();
  std::vector<CurveTreeView::CurvePath> paths;
  if (catalog_ != nullptr) {
    for (const CatalogItem& item : catalog_->items()) {
      if (!dock_->canBind(input, item.key)) {
        continue;
      }
      const ScalarFieldPayload* scalar = asScalarField(item);
      paths.push_back(
          CurveTreeView::CurvePath{
              .key = item.key,
              .dataset = item.dataset_name,
              .topic = item.topic_name,
              .field = scalar != nullptr ? scalar->field_name : QString{},
              .selectable = scalar != nullptr,
          });
    }
  }
  tree->addCatalogItems(paths);
  tree->expandAll();
  ui_->labelChooserHint->setText(
      paths.empty() ? tr("No loaded series fits this input.") : tr("Double-click a series to wire it."));
  if (Search* search = ui_->chooserBand->filterSearch(); search != nullptr) {
    search->clear();
  }
  ui_->bodyStack->setCurrentWidget(ui_->chooserPage);
}

void CustomWidgetConfigPanel::closeChooser() {
  chooser_input_.clear();
  ui_->chooserTree->clearCurves();
  ui_->bodyStack->setCurrentWidget(ui_->formPage);
}

void CustomWidgetConfigPanel::onChooserActivated(QTreeWidgetItem* item) {
  const QString key = CurveTreeView::catalogKeyOf(item);
  if (key.isEmpty() || dock_ == nullptr || chooser_input_.isEmpty()) {
    return;  // a group row: the tree expands it
  }
  const QString input = chooser_input_;
  const std::uint32_t slot = chooser_slot_;
  closeChooser();
  dock_->setBinding(input, slot, key);
}

void CustomWidgetConfigPanel::onStylesheetChanged(QString theme) {
  theme_ = std::move(theme);
  ui_->chooserTree->refreshIcons(theme_);
  applyMissingReasonStyle();
}

void CustomWidgetConfigPanel::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  for (SectionHeaderBand* band : {ui_->headerBand, ui_->inputsBand, ui_->settingsBand, ui_->chooserBand}) {
    band->onChromeMetricsChanged(metrics);
  }
}

void CustomWidgetConfigPanel::applyMissingReasonStyle() {
  const auto tokens = theme::themeFor(isLightTheme(theme_));
  ui_->labelMissingReason->setStyleSheet(u"QLabel { color: %1; background: %2; border-radius: %3px; padding: %4px; }"_s
                                             .arg(
                                                 theme::onStatus(theme::Status::Warning, tokens).name(),
                                                 theme::status(theme::Status::Warning, tokens).name())
                                             .arg(theme::radius(theme::Radius::Input, tokens))
                                             .arg(theme::space(theme::Space::Snug, tokens)));
}

void CustomWidgetConfigPanel::clearLayout(QLayout* layout) {
  while (QLayoutItem* item = layout->takeAt(0)) {
    if (QWidget* widget = item->widget()) {
      widget->hide();
      widget->setParent(nullptr);
      widget->deleteLater();
    } else if (QLayout* child = item->layout()) {
      clearLayout(child);
    }
    delete item;
  }
}

}  // namespace PJ
