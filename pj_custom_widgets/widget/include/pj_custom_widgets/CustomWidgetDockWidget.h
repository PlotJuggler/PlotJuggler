#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_custom_widgets/SettingsSchema.h"
#include "pj_custom_widgets/WidgetBindings.h"
#include "pj_custom_widgets/WireSample.h"
#include "pj_datastore/reader.hpp"
#include "pj_plugins/host/widget_handle.hpp"
#include "pj_runtime/IDataWidget.h"
#include "pj_runtime/WidgetRuntimeHost.h"

class QVBoxLayout;

namespace PJ {

class ActiveWidgetsMarker;
class CatalogModel;
class ExtensionCatalogService;
class MissingWidgetPlaceholder;
class PlaybackEngine;
class ServiceRegistryBuilder;
class SessionManager;

/// The "Custom Widget" panel: a dock whose content is a Qt widget created by a Widget plugin. The
/// user wires series to the widget's named inputs (drag and drop, or setBinding from the side
/// panel) and the dock pushes their values at the tracker time. Persisted as one `<CustomWidget>`
/// element. A widget that cannot be created, or that was disabled after an error, shows a
/// placeholder with the reason (isMissing(), missingReason()) and keeps its layout element as it
/// was. GUI thread only. Lifetime and error policy: pj_custom_widgets/CLAUDE.md.
class CustomWidgetDockWidget : public QWidget, public IDataWidget {
  Q_OBJECT
 public:
  CustomWidgetDockWidget(
      SessionManager* session, CatalogModel* catalog, ExtensionCatalogService* extensions, PlaybackEngine* playback,
      QWidget* parent = nullptr);
  ~CustomWidgetDockWidget() override;
  /// The user-facing word for an input kind ("number", "text", "object"), shared with the config panel.
  static QString kindName(WidgetInputKind kind);

  /// Receives the widget's own messages (report_message) and the dock's warnings.
  void setDiagnosticSink(DiagnosticSink sink);
  /// Where the dock records that it runs plugin code: added before the first call into a plugin
  /// instance, removed once that instance is destroyed. Null (the default) records nothing.
  void setActiveWidgetsMarker(std::shared_ptr<ActiveWidgetsMarker> marker);

  /// Creates the widget `type` of plugin `plugin_id` and mounts it. Returns false when it cannot
  /// (library not installed, no such type, Qt build mismatch, creation failed or threw): the dock then
  /// shows the placeholder, isMissing() is true and the reason is in missingReason(). Replaces
  /// whatever the dock held before, wires and settings of the previous widget included. The new
  /// widget starts with its default settings.
  bool bindToWidget(const QString& plugin_id, const QString& type);

  // --- IDataWidget ---
  QWidget* widget() override {
    return this;
  }
  /// Pushes the values at `time` (display-axis seconds) to the widget when a wired sample changed.
  void onTrackerTime(double time) override;
  /// Wires a dropped object topic to the first compatible input (see WidgetBindings::pickInput).
  bool tryAcceptObjectTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) override;
  /// Wires dropped scalar keys the same way; a key that fits no input is ignored. A drop of known
  /// series that wires nothing tells the user why (showDropRejected); unknown keys stay silent.
  bool tryAcceptSeriesKeys(const QStringList& catalog_keys) override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  /// Always true for a `<CustomWidget>` element, including one whose plugin is missing.
  bool xmlLoadState(const QDomElement& element) override;
  /// Destroys the plugin instance and its widget now; the dock itself is deleted later.
  void aboutToBeDiscarded() override;

  // --- state, for the side panel ---
  [[nodiscard]] QString pluginId() const {
    return plugin_id_;
  }
  [[nodiscard]] QString widgetType() const {
    return type_;
  }
  /// Display name of the widget type (the id when the library is not loaded).
  [[nodiscard]] QString widgetName() const;
  [[nodiscard]] bool isMissing() const {
    return !handle_.has_value();
  }
  [[nodiscard]] const QString& missingReason() const {
    return missing_reason_;
  }
  /// Current inputs of the widget; empty while missing.
  [[nodiscard]] const std::vector<WidgetInput>& inputs() const {
    return bindings_.inputs();
  }
  [[nodiscard]] const std::vector<WidgetWire>& bindings() const {
    return bindings_.wires();
  }
  /// The user-editable settings of this widget type, parsed once per bind; empty while missing.
  [[nodiscard]] const std::vector<SettingDecl>& settingsDeclarations() const {
    return settings_decls_;
  }
  /// The full configuration as it is saved in the layout: every declared setting (the manifest
  /// `default` where the user set nothing), the user's values over them, and the widget's own
  /// save_config state laid over those (a colliding state key wins). Edit one key of THIS and pass
  /// it to applySettings. A setting the manifest gives no `default` appears once the user set it.
  [[nodiscard]] QString settingsJson() const;
  /// (dataset, topic) of every resolved wire: the demand this panel holds on the data sources.
  [[nodiscard]] std::vector<std::pair<DatasetId, QString>> displayedTopics() const {
    return bindings_.displayedTopics();
  }
  /// One entry per distinct (topic, preferred dataset) of the wires that did not resolve yet. The
  /// shell's PendingDisplayBinder holds a demand reference for each, so a demand-driven stream
  /// subscribes the topic the wire waits for; bindingsChanged() tells it a wire resolved or went.
  [[nodiscard]] std::vector<PendingWireDemand> pendingDemands() const {
    return bindings_.pendingDemands();
  }
  /// Whether a pending wire waits on `topic`; resolves no dataset (see WidgetBindings::hasPendingTopic).
  [[nodiscard]] bool hasPendingTopic(const QString& topic) const {
    return bindings_.hasPendingTopic(topic);
  }
  /// Whether pendingDemands() would list anything, without building it.
  [[nodiscard]] bool hasPendingWires() const {
    return bindings_.hasPendingWires();
  }

  // --- editing, for the side panel ---
  /// Wires catalog `key` to `slot` of input `input` (WidgetBindings::kNewSlot appends on a `multiple`
  /// input; a normal input replaces its wire). False when the key does not fit the input.
  bool setBinding(const QString& input, std::uint32_t slot, const QString& key);
  /// Whether catalog `key` fits input `input` (its kind matches): what a series chooser offers.
  [[nodiscard]] bool canBind(const QString& input, const QString& key) const;
  void removeBinding(const QString& input, std::uint32_t slot);
  /// Stores the full settings JSON and hands it whole to the widget (load_config), then
  /// reconciles the inputs it may have changed. False when the widget rejected it; the JSON is
  /// stored either way, so the layout keeps what the user wrote.
  bool applySettings(const QString& json);

 signals:
  /// The set of wires, or what they resolve to, changed. Demand follows this signal.
  void bindingsChanged();
  /// The widget's input list changed (after load_config or inputs_changed).
  void inputsChanged();
  /// Persisted state changed through a user gesture or a reconciliation that dropped wires; the
  /// shell debounces it into an undo snapshot.
  void workspaceChanged();

 private:
  /// Marks a call into the plugin; inputs_changed arriving meanwhile is deferred.
  /// Runs `call` (one call into the plugin) inside a PluginCallScope and a catch. Returns the
  /// message of an exception that escaped it, nullopt when it returned normally.
  template <typename Call>
  std::optional<QString> callPlugin(Call&& call) const;
  /// callPlugin that disables the panel when the call threw. False then: the handle is gone.
  template <typename Call>
  bool callOrDisable(Call&& call);
  /// The panel stops calling its plugin: the instance is destroyed now, the placeholder shows
  /// `error`, the layout element is kept as the panel was, a kError goes to the sink.
  void disablePlugin(const QString& error);
  /// disablePlugin from a const path (saving the layout): runs from the event loop, unless the
  /// dock was rebound or released in between.
  void scheduleDisable(const QString& error) const;

  /// bindToWidget without resetting the stored settings (xmlLoadState sets them first). `saved_wires`
  /// (an element holding <input> children, or null) loads before create_qwidget, because a widget may
  /// query the history of its wires while building its view.
  bool bindWidget(const QString& plugin_id, const QString& type, const QDomElement& saved_wires = {});
  /// Shows the placeholder with `reason` and reports `diagnostic` (`reason` when empty) at
  /// `level`. Returns false, the result of a bind that ends here.
  bool showPlaceholder(const QString& reason, DiagnosticLevel level, const QString& diagnostic = {});
  /// showPlaceholder for a panel disabled after `error`.
  bool showDisabled(const QString& error);
  /// Destroys the plugin instance, its widget, the host service and the placeholder.
  void releasePlugin();
  /// Drops every wire and input; the next push is unconditional.
  void resetBindings();
  bool loadConfigInto(const QString& json);
  /// The manifest `default` of every declared setting that has one, keyed by `key`.
  [[nodiscard]] QJsonObject settingsDefaults() const;
  /// `json` (the stored settings) over settingsDefaults(); nullopt when `json` is not an object.
  [[nodiscard]] std::optional<QJsonObject> mergedWithDefaults(const QString& json) const;
  /// mergedWithDefaults serialised: what load_config receives. `json` is returned unchanged when
  /// the type declares no default or `json` is not an object.
  [[nodiscard]] QString withDefaults(const QString& json) const;
  /// The plugin's inputs, or nullopt once the panel was disabled: the call threw, or the plugin
  /// reported a failure or returned a list the host refuses. Wires are never reconciled then.
  std::optional<std::vector<WidgetInput>> readInputs();
  void reconcileInputs();
  void onInputsChangedFromPlugin();
  void reportMessage(DiagnosticLevel level, const QString& message) const;
  void afterBindingsEdited();
  /// For a drop that wired nothing: the app's message box naming the dropped kinds and the widget's
  /// inputs. Opened with show(), never exec(), so the drop handler does not nest an event loop.
  void showDropRejected(const std::vector<WidgetInputKind>& dropped_kinds);
  void afterCatalogChange(bool changed);
  /// The next push reaches the widget whatever it holds. Also forgets the samples kept from the
  /// previous tick, which belong to the wires as they were.
  void invalidateRenderKey() {
    last_render_key_.reset();
    samples_.clear();
  }
  /// The last tracker time, or the playhead before the first tick.
  [[nodiscard]] double currentPushTime() const;
  /// Pushes the values at currentPushTime() regardless of the render key.
  void refresh();
  void pushValues(double time, bool force);

  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  ExtensionCatalogService* extensions_ = nullptr;
  PlaybackEngine* playback_ = nullptr;
  DiagnosticSink sink_;
  std::shared_ptr<ActiveWidgetsMarker> marker_;

  QVBoxLayout* layout_ = nullptr;
  QPointer<MissingWidgetPlaceholder> placeholder_;
  QPointer<QWidget> plugin_widget_;

  QString plugin_id_;
  QString type_;
  QString plugin_version_;
  QString settings_json_ = QStringLiteral("{}");
  std::vector<SettingDecl> settings_decls_;
  QString missing_reason_;
  QDomDocument preserved_;  ///< verbatim element of a widget that could not be created

  WidgetBindings bindings_;

  // Destruction runs bottom-up: the handle (and with it the plugin's widget) first, then the
  // registry the plugin may still hold, then the host service it points to, then the reader.
  DataReader reader_;
  std::unique_ptr<WidgetRuntimeHost> host_;
  std::unique_ptr<ServiceRegistryBuilder> registry_;
  // mutable: saving the layout asks the plugin for its state (save_config), a non-const call.
  mutable std::optional<WidgetHandle> handle_;

  mutable int plugin_call_depth_ = 0;
  std::uint64_t bind_generation_ = 0;  ///< bumped by every bind; a scheduled disable checks it
  bool reconcile_scheduled_ = false;
  /// Identity of what the widget was last handed (see mixWireSample); unset = push unconditionally.
  std::optional<std::uint64_t> last_render_key_;
  double last_time_ = std::numeric_limits<double>::quiet_NaN();
  // Per-tick scratch of pushValues, one entry per wire, kept to reuse the capacity. samples_ also
  // carries each wire's previous sample into the next tick (refreshWireSample).
  std::vector<std::optional<WireTarget>> targets_;
  std::vector<WireSample> samples_;
  std::vector<PJ_widget_value_t> values_;
};

}  // namespace PJ
