// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_custom_widgets/CustomWidgetDockWidget.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QScopedValueRollback>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <exception>
#include <set>

#include "pj_custom_widgets/MissingWidgetPlaceholder.h"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/ActiveWidgetsMarker.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DiagnosticEmit.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/XmlCdata.h"
#include "pj_widgets/Fnv1a.h"
#include "pj_widgets/MessageBox.h"

using namespace Qt::StringLiterals;

namespace PJ {

namespace {

constexpr auto kTag = "CustomWidget"_L1;
/// The settings of a widget nobody configured, which is also what an empty `<config>` means.
constexpr auto kNoSettings = "{}"_L1;

QString normalizedConfig(const QString& json) {
  return json.isEmpty() ? QString(kNoSettings) : json;
}

DiagnosticLevel toDiagnosticLevel(WidgetMessageLevel level) {
  switch (level) {
    case WidgetMessageLevel::kInfo:
      return DiagnosticLevel::kInfo;
    case WidgetMessageLevel::kWarning:
      return DiagnosticLevel::kWarning;
    case WidgetMessageLevel::kError:
      return DiagnosticLevel::kError;
  }
  return DiagnosticLevel::kInfo;
}

std::optional<QJsonObject> parseObject(const QString& json) {
  const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
  if (!document.isObject()) {
    return std::nullopt;
  }
  return document.object();
}

const WidgetTypeDescriptor* findType(const LoadedWidget& plugin, const QString& type) {
  const std::string utf8 = type.toStdString();
  const auto it = std::ranges::find_if(plugin.widgets, [&](const auto& w) { return w.type == utf8; });
  return it == plugin.widgets.end() ? nullptr : &*it;
}

/// Stands in for the target of a pending wire, whose NONE value carries no payload.
const WireTarget kNoTarget{};

}  // namespace

template <typename Call>
std::optional<QString> CustomWidgetDockWidget::callPlugin(Call&& call) const {
  const QScopedValueRollback depth(plugin_call_depth_, plugin_call_depth_ + 1);
  try {
    std::forward<Call>(call)();
    return std::nullopt;
  } catch (const std::exception& e) {
    return QString::fromUtf8(e.what());
  } catch (...) {
    return tr("unknown exception");
  }
}

template <typename Call>
bool CustomWidgetDockWidget::callOrDisable(Call&& call) {
  if (const auto error = callPlugin(std::forward<Call>(call))) {
    disablePlugin(*error);
    return false;
  }
  return true;
}

CustomWidgetDockWidget::CustomWidgetDockWidget(
    SessionManager* session, CatalogModel* catalog, ExtensionCatalogService* extensions, PlaybackEngine* playback,
    QWidget* parent)
    : QWidget(parent),
      session_(session),
      catalog_(catalog),
      extensions_(extensions),
      playback_(playback),
      bindings_(session, catalog),
      reader_(session->createReader()) {
  layout_ = new QVBoxLayout(this);
  layout_->setContentsMargins(0, 0, 0, 0);
  layout_->setSpacing(0);

  if (catalog_ != nullptr) {
    // Wires saved against a dataset that had not loaded yet bind the moment its items appear.
    connect(catalog_, &CatalogModel::itemsAdded, this, [this]() { afterCatalogChange(bindings_.retryPending()); });
    // A pending wire's series may also surface through a CHANGE (a field reclassified on its first
    // data adds no key), so retry there too, as the state-transitions strip does.
    connect(catalog_, &CatalogModel::itemsChanged, this, [this]() { afterCatalogChange(bindings_.refresh()); });
    connect(catalog_, &CatalogModel::itemsRemoved, this, [this]() { afterCatalogChange(bindings_.revalidate()); });
    connect(catalog_, &CatalogModel::cleared, this, [this]() { afterCatalogChange(bindings_.revalidate()); });
  }
  const auto on_offset_changed = [this]() { pushValues(currentPushTime(), /*force=*/false); };
  connect(session_, qOverload<>(&SessionManager::displayOffsetChanged), this, on_offset_changed);
  connect(session_, qOverload<DatasetId>(&SessionManager::displayOffsetChanged), this, on_offset_changed);
}

CustomWidgetDockWidget::~CustomWidgetDockWidget() {
  releasePlugin();
}

void CustomWidgetDockWidget::setDiagnosticSink(DiagnosticSink sink) {
  sink_ = std::move(sink);
}

void CustomWidgetDockWidget::setActiveWidgetsMarker(std::shared_ptr<ActiveWidgetsMarker> marker) {
  marker_ = std::move(marker);
}

QString CustomWidgetDockWidget::widgetName() const {
  const LoadedWidget* plugin = extensions_ != nullptr ? extensions_->findWidget(plugin_id_) : nullptr;
  const WidgetTypeDescriptor* descriptor = plugin != nullptr ? findType(*plugin, type_) : nullptr;
  if (descriptor != nullptr && !descriptor->name.empty()) {
    return QString::fromStdString(descriptor->name);
  }
  return plugin_id_.isEmpty() ? type_ : plugin_id_ + u'/' + type_;
}

void CustomWidgetDockWidget::reportMessage(DiagnosticLevel level, const QString& message) const {
  emitDiagnosticTo(sink_, level, "CustomWidget", plugin_id_.toStdString(), message.toStdString());
}

bool CustomWidgetDockWidget::showPlaceholder(const QString& reason, DiagnosticLevel level, const QString& diagnostic) {
  missing_reason_ = reason;
  placeholder_ = new MissingWidgetPlaceholder(reason, this);
  layout_->addWidget(placeholder_);
  reportMessage(level, diagnostic.isEmpty() ? reason : diagnostic);
  return false;
}

bool CustomWidgetDockWidget::showDisabled(const QString& error) {
  return showPlaceholder(
      tr("This widget was disabled after an error: %1").arg(error), DiagnosticLevel::kError,
      tr("The widget %1/%2 was disabled after an error: %3").arg(plugin_id_, type_, error));
}

void CustomWidgetDockWidget::resetBindings() {
  bindings_.clearWires();
  bindings_.setInputs({});
  invalidateRenderKey();
}

void CustomWidgetDockWidget::releasePlugin() {
  if (plugin_widget_ != nullptr) {
    layout_->removeWidget(plugin_widget_);
    // Out of the parent's child list BEFORE the plugin deletes it: no parent may free an object
    // whose destructor lives in the plugin's library.
    plugin_widget_->setParent(nullptr);
  }
  handle_.reset();  // destroy(): the plugin deletes its widget here
  plugin_widget_.clear();
  if (marker_ != nullptr) {
    marker_->remove(this);
  }
  registry_.reset();
  host_.reset();
  delete placeholder_.data();
  placeholder_.clear();
}

void CustomWidgetDockWidget::aboutToBeDiscarded() {
  releasePlugin();
}

void CustomWidgetDockWidget::disablePlugin(const QString& error) {
  if (!handle_) {
    return;
  }
  releasePlugin();
  if (preserved_.isNull()) {
    // The panel as it was, read from the dock alone (handle_ is gone, so no plugin call).
    QDomDocument doc;
    doc.appendChild(xmlSaveState(doc));
    preserved_ = doc;
  }
  resetBindings();
  showDisabled(error);
  emit inputsChanged();
  emit bindingsChanged();
}

void CustomWidgetDockWidget::scheduleDisable(const QString& error) const {
  auto* self = const_cast<CustomWidgetDockWidget*>(this);
  QTimer::singleShot(0, self, [self, error, generation = bind_generation_]() {
    if (self->bind_generation_ == generation) {
      self->disablePlugin(error);
    }
  });
}

bool CustomWidgetDockWidget::bindToWidget(const QString& plugin_id, const QString& type) {
  settings_json_ = kNoSettings;
  const bool bound = bindWidget(plugin_id, type);
  if (bound) {
    refresh();
  }
  return bound;
}

bool CustomWidgetDockWidget::bindWidget(const QString& plugin_id, const QString& type, const QDomElement& saved_wires) {
  releasePlugin();
  ++bind_generation_;
  plugin_id_ = plugin_id;
  type_ = type;
  plugin_version_.clear();
  settings_decls_.clear();
  missing_reason_.clear();
  preserved_ = QDomDocument();
  resetBindings();

  const QString what = plugin_id + u'/' + type;
  const LoadedWidget* plugin = extensions_ != nullptr ? extensions_->findWidget(plugin_id) : nullptr;
  if (plugin == nullptr) {
    return showPlaceholder(
        tr("The widget %1 is missing: its plugin is not installed.").arg(what), DiagnosticLevel::kWarning);
  }
  plugin_version_ = QString::fromStdString(plugin->version);
  if (!plugin->qt_compatible) {
    return showPlaceholder(
        tr("The widget %1 cannot run here: %2").arg(what, QString::fromStdString(plugin->qt_mismatch_reason)),
        DiagnosticLevel::kWarning);
  }
  const WidgetTypeDescriptor* descriptor = findType(*plugin, type);
  if (descriptor == nullptr) {
    return showPlaceholder(
        tr("The widget %1 is missing: the installed plugin has no such widget.").arg(what), DiagnosticLevel::kWarning);
  }
  settings_decls_ = parseSettingsSchema(QString::fromStdString(descriptor->settings_json));

  // Recorded before the first call into the plugin: a crash from here on names this widget at the next start.
  if (marker_ != nullptr) {
    marker_->add(this, plugin_id, type);
  }
  std::optional<Expected<WidgetHandle>> created;
  const auto create_error = callPlugin([&]() { created.emplace(plugin->library.createHandle(descriptor->type)); });
  if (create_error || !*created) {
    if (marker_ != nullptr) {
      marker_->remove(this);
    }
    return create_error
               ? showDisabled(*create_error)
               : showPlaceholder(
                     tr("The widget %1 could not be created: %2").arg(what, QString::fromStdString(created->error())),
                     DiagnosticLevel::kWarning);
  }
  handle_.emplace(std::move(**created));

  const auto fail = [&](const QString& reason) {
    releasePlugin();
    return showPlaceholder(tr("The widget %1 could not be created: %2").arg(what, reason), DiagnosticLevel::kWarning);
  };

  host_ = std::make_unique<WidgetRuntimeHost>(
      reader_, session_->objectStore(),
      [this](std::uint32_t input, std::uint32_t slot) -> std::optional<WidgetRuntimeHost::ResolvedWire> {
        for (const WidgetWire& wire : bindings_.wires()) {
          if (wire.input_index == input && wire.slot == slot) {
            const std::optional<WireTarget> target = bindings_.currentTarget(wire);
            if (!target) {
              return std::nullopt;
            }
            return WidgetRuntimeHost::ResolvedWire{*target, wire.label};
          }
        }
        return std::nullopt;
      },
      WidgetRuntimeHost::Callbacks{
          .inputs_changed = [this]() { onInputsChangedFromPlugin(); },
          .report_message =
              [this](WidgetMessageLevel level, std::string_view message) {
                reportMessage(
                    toDiagnosticLevel(level),
                    QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())));
              },
      });
  registry_ = std::make_unique<ServiceRegistryBuilder>();
  if (const Status status = host_->registerServices(*registry_); !status) {
    return fail(QString::fromStdString(status.error()));
  }
  Status bound = okStatus();
  if (!callOrDisable([&]() { bound = handle_->bind(registry_->view()); })) {
    return false;
  }
  if (!bound) {
    return fail(QString::fromStdString(bound.error()));
  }
  // A brand-new panel too: the manifest defaults are the widget's first configuration.
  if (const QString config = withDefaults(settings_json_); config != kNoSettings && !loadConfigInto(config)) {
    if (!handle_) {
      return false;  // load_config threw: disabled
    }
    reportMessage(DiagnosticLevel::kWarning, tr("The widget %1 rejected its saved settings.").arg(what));
  }
  std::optional<std::vector<WidgetInput>> inputs = readInputs();
  if (!inputs) {
    return false;
  }
  bindings_.setInputs(std::move(*inputs));
  if (!saved_wires.isNull()) {
    bindings_.load(saved_wires);
  }

  std::optional<Expected<void*>> raw;
  if (!callOrDisable([&]() { raw.emplace(handle_->createQWidget(static_cast<void*>(static_cast<QWidget*>(this)))); })) {
    return false;
  }
  if (!*raw) {
    return fail(QString::fromStdString(raw->error()));
  }
  plugin_widget_ = static_cast<QWidget*>(**raw);
  layout_->addWidget(plugin_widget_);
  return true;
}

bool CustomWidgetDockWidget::loadConfigInto(const QString& json) {
  const std::string config = json.toStdString();
  Status status = okStatus();
  if (!callOrDisable([&]() { status = handle_->loadConfig(config); })) {
    return false;
  }
  if (!status) {
    reportMessage(DiagnosticLevel::kWarning, tr("Settings rejected: %1").arg(QString::fromStdString(status.error())));
  }
  return status.has_value();
}

QJsonObject CustomWidgetDockWidget::settingsDefaults() const {
  QJsonObject defaults;
  for (const SettingDecl& setting : settings_decls_) {
    if (!setting.default_value.isUndefined()) {
      defaults.insert(setting.key, setting.default_value);
    }
  }
  return defaults;
}

std::optional<QJsonObject> CustomWidgetDockWidget::mergedWithDefaults(const QString& json) const {
  const auto user = parseObject(json);
  if (!user) {
    return std::nullopt;
  }
  QJsonObject merged = settingsDefaults();
  for (auto it = user->begin(); it != user->end(); ++it) {
    merged.insert(it.key(), it.value());
  }
  return merged;
}

QString CustomWidgetDockWidget::withDefaults(const QString& json) const {
  const bool has_defaults = std::ranges::any_of(
      settings_decls_, [](const SettingDecl& setting) { return !setting.default_value.isUndefined(); });
  if (!has_defaults) {
    return json;
  }
  const auto merged = mergedWithDefaults(json);
  if (!merged) {
    return json;  // not an object: handed over as stored, the widget rejects it
  }
  return QString::fromUtf8(QJsonDocument(*merged).toJson(QJsonDocument::Compact));
}

QString CustomWidgetDockWidget::settingsJson() const {
  if (!handle_) {
    return withDefaults(settings_json_);
  }
  std::string state;
  Status saved = okStatus();
  if (const auto error = callPlugin([&]() { saved = handle_->saveConfig(state); })) {
    scheduleDisable(*error);
    return withDefaults(settings_json_);
  }
  if (!saved) {
    return withDefaults(settings_json_);
  }
  auto merged = mergedWithDefaults(settings_json_);
  const auto own = parseObject(QString::fromStdString(state));
  if (!merged || !own) {
    return withDefaults(settings_json_);
  }
  for (auto it = own->begin(); it != own->end(); ++it) {
    merged->insert(it.key(), it.value());
  }
  return QString::fromUtf8(QJsonDocument(*merged).toJson(QJsonDocument::Compact));
}

bool CustomWidgetDockWidget::applySettings(const QString& json) {
  settings_json_ = normalizedConfig(json);
  bool accepted = true;
  if (handle_) {
    accepted = loadConfigInto(withDefaults(settings_json_));
    reconcileInputs();  // no-op when load_config threw and the panel was disabled
    invalidateRenderKey();
    refresh();
  }
  emit workspaceChanged();
  return accepted;
}

std::optional<std::vector<WidgetInput>> CustomWidgetDockWidget::readInputs() {
  std::optional<Expected<std::vector<WidgetInput>>> inputs;
  if (!callOrDisable([&]() { inputs.emplace(handle_->inputs()); })) {
    return std::nullopt;
  }
  if (!*inputs) {
    // Never reconciled against: an empty list would drop every wire. The disabled panel keeps them
    // in its layout element, so the wiring comes back with the widget.
    disablePlugin(tr("its inputs could not be read: %1").arg(QString::fromStdString(inputs->error())));
    return std::nullopt;
  }
  return std::move(**inputs);
}

void CustomWidgetDockWidget::reconcileInputs() {
  if (!handle_) {
    return;
  }
  std::optional<std::vector<WidgetInput>> inputs = readInputs();
  if (!inputs) {
    return;
  }
  const WidgetBindings::InputsDelta delta = bindings_.setInputs(std::move(*inputs));
  invalidateRenderKey();
  if (delta.inputs_changed) {
    emit inputsChanged();
  }
  if (delta.wires_changed) {
    emit bindingsChanged();
    emit workspaceChanged();
  }
  if (delta.inputs_changed || delta.wires_changed) {
    refresh();
  }
}

void CustomWidgetDockWidget::onInputsChangedFromPlugin() {
  if (plugin_call_depth_ == 0) {
    reconcileInputs();
    return;
  }
  // The widget announced it from inside one of its own calls (on_update, load_config, ...): the
  // list must not change under that call, so reconcile once it returned.
  if (reconcile_scheduled_) {
    return;
  }
  reconcile_scheduled_ = true;
  QTimer::singleShot(0, this, [this]() {
    reconcile_scheduled_ = false;
    reconcileInputs();
  });
}

void CustomWidgetDockWidget::afterBindingsEdited() {
  invalidateRenderKey();
  emit bindingsChanged();
  emit workspaceChanged();
  refresh();
}

void CustomWidgetDockWidget::afterCatalogChange(bool changed) {
  if (!changed) {
    return;
  }
  invalidateRenderKey();
  emit bindingsChanged();
  refresh();
}

bool CustomWidgetDockWidget::setBinding(const QString& input, std::uint32_t slot, const QString& key) {
  const auto index = bindings_.inputIndex(input);
  if (!handle_ || !index || !bindings_.bindKey(*index, slot, key)) {
    return false;
  }
  afterBindingsEdited();
  return true;
}

bool CustomWidgetDockWidget::canBind(const QString& input, const QString& key) const {
  const auto index = bindings_.inputIndex(input);
  return handle_.has_value() && index.has_value() && bindings_.canBind(*index, key);
}

void CustomWidgetDockWidget::removeBinding(const QString& input, std::uint32_t slot) {
  if (bindings_.unbind(input, slot)) {
    afterBindingsEdited();
  }
}

bool CustomWidgetDockWidget::tryAcceptSeriesKeys(const QStringList& catalog_keys) {
  if (!handle_) {
    return false;
  }
  // A normal input takes one series per drop: once filled by this drop it is not replaced by a
  // later key of the same drop. A `multiple` input keeps taking slots.
  std::set<std::size_t> filled;
  std::vector<WidgetInputKind> dropped_kinds;
  bool any = false;
  for (const QString& key : catalog_keys) {
    const auto kind = bindings_.kindOfKey(key);
    if (!kind) {
      continue;
    }
    dropped_kinds.push_back(*kind);
    const auto index = bindings_.pickInput(*kind, filled);
    if (!index || !bindings_.bindKey(*index, WidgetBindings::kNewSlot, key)) {
      continue;
    }
    if (!bindings_.inputs()[*index].multiple) {
      filled.insert(*index);
    }
    any = true;
  }
  if (any) {
    afterBindingsEdited();
  } else if (!dropped_kinds.empty()) {
    showDropRejected(dropped_kinds);
  }
  return any;
}

bool CustomWidgetDockWidget::tryAcceptObjectTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType /*object_type*/, const QString& /*title*/) {
  if (!handle_) {
    return false;
  }
  const auto index = bindings_.pickInput(WidgetInputKind::kObject);
  if (!index || !bindings_.bindObject(*index, WidgetBindings::kNewSlot, topic_id)) {
    showDropRejected({WidgetInputKind::kObject});
    return false;
  }
  afterBindingsEdited();
  return true;
}

QString CustomWidgetDockWidget::kindName(WidgetInputKind kind) {
  switch (kind) {
    case WidgetInputKind::kNumber:
      return tr("number");
    case WidgetInputKind::kText:
      return tr("text");
    case WidgetInputKind::kObject:
      return tr("object");
  }
  return {};
}

void CustomWidgetDockWidget::showDropRejected(const std::vector<WidgetInputKind>& dropped_kinds) {
  QStringList dropped;
  for (const WidgetInputKind kind : std::set<WidgetInputKind>(dropped_kinds.begin(), dropped_kinds.end())) {
    dropped << kindName(kind);
  }
  QStringList inputs;
  for (const WidgetInput& input : bindings_.inputs()) {
    const QString name = QString::fromStdString(input.label.empty() ? input.name : input.label);
    inputs << u"%1 (%2)"_s.arg(name, kindName(input.kind));
  }
  QString text = tr("No input of this widget takes a %1 series.").arg(dropped.join(u"/"_s));
  if (!inputs.isEmpty()) {
    text += u' ' + tr("Its inputs take: %1.").arg(inputs.join(u", "_s));
  }

  // Non-blocking, like PlotWidget's duplicate-name warning: a drop handler must not nest an event loop.
  auto* warning = new MessageBox(this);
  warning->setObjectName(u"customWidgetDropRejected"_s);
  warning->setAttribute(Qt::WA_DeleteOnClose);
  warning->setTitle(tr("Series not accepted"));
  warning->setText(text);
  warning->addButton(MessageBox::tr("OK"), MessageBox::kPrimaryRole);
  warning->setWindowModality(Qt::ApplicationModal);
  warning->show();
}

void CustomWidgetDockWidget::onTrackerTime(double time) {
  last_time_ = time;
  pushValues(time, /*force=*/false);
}

double CustomWidgetDockWidget::currentPushTime() const {
  if (!std::isfinite(last_time_) && playback_ != nullptr) {
    return toAxisDouble(playback_->currentTime());
  }
  return last_time_;
}

void CustomWidgetDockWidget::refresh() {
  pushValues(currentPushTime(), /*force=*/true);
}

void CustomWidgetDockWidget::pushValues(double time, bool force) {
  if (!handle_ || !std::isfinite(time)) {
    return;
  }
  const std::vector<WidgetWire>& wires = bindings_.wires();
  const ObjectStore& objects = session_->objectStore();

  // First pass: identity of what each wire would show. Scalars are read in full (a text string only
  // when its row moved); an object only resolves its entry id, so a tick that changed nothing never
  // touches a lazy payload.
  targets_.clear();
  samples_.resize(wires.size());
  std::uint64_t key = fnv1aMix(kFnv1aOffsetBasis, wires.size());
  for (std::size_t i = 0; i < wires.size(); ++i) {
    const std::optional<WireTarget>& target = targets_.emplace_back(bindings_.currentTarget(wires[i]));
    if (target) {
      refreshWireSample(reader_, objects, *target, toAbsolute(displaySeconds(time), target->offset), samples_[i]);
      key = mixWireSample(key, samples_[i], target->offset);
    } else {
      samples_[i] = WireSample{};
      key = mixWireSample(key, samples_[i], DisplayOffset{});
    }
  }
  if (!force && last_render_key_ == key) {
    return;
  }
  last_render_key_ = key;

  values_.clear();
  for (std::size_t i = 0; i < wires.size(); ++i) {
    if (targets_[i]) {
      fetchObjectPayload(objects, *targets_[i], samples_[i]);
    }
    values_.push_back(makeWidgetValue(
        static_cast<std::uint32_t>(wires[i].input_index), wires[i].slot, targets_[i] ? *targets_[i] : kNoTarget,
        samples_[i], wires[i].label));
  }
  callOrDisable([&]() { handle_->onUpdate(Span<const PJ_widget_value_t>(values_.data(), values_.size()), time); });
}

QDomElement CustomWidgetDockWidget::xmlSaveState(QDomDocument& doc) const {
  if (!preserved_.isNull()) {
    return doc.importNode(preserved_.documentElement(), /*deep=*/true).toElement();
  }
  QDomElement root = doc.createElement(QString(kTag));
  root.setAttribute(u"plugin"_s, plugin_id_);
  root.setAttribute(u"type"_s, type_);
  if (!plugin_version_.isEmpty()) {
    root.setAttribute(u"plugin_version"_s, plugin_version_);
  }
  bindings_.save(doc, root);
  const QString config = settingsJson();
  if (config != kNoSettings) {
    QDomElement config_element = doc.createElement(u"config"_s);
    appendJsonAsCdata(doc, config_element, config);
    root.appendChild(config_element);
  }
  return root;
}

bool CustomWidgetDockWidget::xmlLoadState(const QDomElement& element) {
  if (element.tagName() != kTag) {
    return false;
  }
  const QDomElement config = element.firstChildElement(u"config"_s);
  settings_json_ = normalizedConfig(config.isNull() ? QString() : config.text().trimmed());

  const bool bound = bindWidget(element.attribute(u"plugin"_s), element.attribute(u"type"_s), element);
  if (!bound) {
    resetBindings();
    // Keep the saved element as it is: the layout round-trips without the plugin.
    preserved_ = QDomDocument();
    preserved_.appendChild(preserved_.importNode(element, /*deep=*/true));
  }
  emit inputsChanged();
  emit bindingsChanged();
  if (bound) {
    refresh();
  }
  return true;
}

}  // namespace PJ
