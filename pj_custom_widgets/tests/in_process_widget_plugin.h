#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// An in-process Widget plugin for the Custom Widget tests: a static vtable built with the SDK base
// class that creates a real QLabel and records every call it receives in `g`. Widget types:
// "gauge" (value + status), "table" (one `multiple` number input), "viewer" (one object input).
// Config tokens switch behaviours: "dynamic" (inputs value, extra), "grow_on_update" (announces a
// new input from inside on_update), "reject" (load_config fails), "history" (queries history),
// "throw_on_update" (onUpdate throws; the SDK trampoline contains it), "fail_inputs" (inputs()
// throws, so the host receives a failed inputs call), "own_state" (save_config returns its own
// state, whose `unit` collides with the setting of that name). `g.null_widget` makes
// createWidget return null. A second vtable claims another Qt minor, to exercise the admission
// refusal; a third ("faulty_widgets") hands the host a string whose length no std::string can hold
// (an input name once `g.corrupt_inputs` is set, the saved config once `g.corrupt_saved_config` is
// set), so the host's own conversion throws.

// The Qt header must precede the SDK widget header: the build-info macros read QT_VERSION_*.
#include <QLabel>
#include <QPointer>
#include <QtGlobal>
#include <cstddef>
#include <functional>
#include <limits>
#include <pj_plugins/sdk/widget_plugin_base.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pj_cw_test {

using namespace PJ;

struct ValueCopy {
  uint32_t input = 0;
  uint32_t slot = 0;
  PJ_widget_value_kind_t kind = PJ_WIDGET_VALUE_NONE;
  double time = 0.0;
  double number = 0.0;
  std::string text;
  std::vector<uint8_t> bytes;
  std::string type_name;
  std::string label;
};

// What the plugin saw, reset by every test fixture.
struct PluginStats {
  int instances = 0;
  int destroyed = 0;
  int widgets_created = 0;
  int widgets_deleted_by_plugin = 0;
  int destroyed_while_parented = 0;
  int updates = 0;
  int load_configs = 0;
  int history_samples = -1;
  uint32_t history_total = 0;
  bool history_ok = false;
  std::string last_config;
  std::vector<ValueCopy> last_values;
  bool null_widget = false;                ///< createWidget returns null
  bool corrupt_inputs = false;             ///< "faulty_widgets" reports an impossible input name
  bool corrupt_saved_config = false;       ///< "faulty_widgets" reports an impossible save_config length
  std::function<void()> on_create_widget;  ///< runs at the start of createWidget
};
inline PluginStats g;

inline constexpr const char* kManifest =
    R"({"id":"test_widgets","name":"Test Widgets","version":"1.2.3","widgets":[)"
    R"({"type":"gauge","name":"Gauge","settings":[{"key":"unit","type":"text","default":"m"},)"
    R"({"key":"scale","type":"number","default":2}]},)"
    R"({"type":"table","name":"Table"},{"type":"viewer","name":"Viewer"}]})";
inline constexpr const char* kFaultyManifest =
    R"({"id":"faulty_widgets","name":"Faulty Widgets","version":"0.1.0","widgets":[)"
    R"({"type":"gauge","name":"Gauge"}]})";
inline constexpr const char* kOtherQtManifest =
    R"({"id":"test_widgets_other_qt","name":"Other Qt","version":"1.0.0","widgets":[)"
    R"({"type":"gauge","name":"Gauge"}]})";

class TestPlugin : public WidgetPluginBase {
 public:
  explicit TestPlugin(std::string_view type) : type_(type) {
    if (type_ != "gauge" && type_ != "table" && type_ != "viewer") {
      throw std::invalid_argument("unknown widget type");
    }
    ++g.instances;
  }

  ~TestPlugin() override {
    ++g.destroyed;
    if (label_ != nullptr) {
      // The host must have taken the widget out of the tree before this runs.
      if (label_->parent() != nullptr) {
        ++g.destroyed_while_parented;
      }
      delete label_.data();
      ++g.widgets_deleted_by_plugin;
    }
  }

  std::vector<WidgetInput> inputs() const override {
    if (has("fail_inputs")) {
      throw std::runtime_error("inputs unavailable");
    }
    std::vector<WidgetInput> result;
    if (type_ == "table") {
      result.emplace_back("rows", "Rows", "", WidgetInputKind::kNumber, true);
    } else if (type_ == "viewer") {
      result.emplace_back("frame", "Frame", "", WidgetInputKind::kObject);
    } else if (has("dynamic")) {
      result.emplace_back("value", "Value", "V");
      result.emplace_back("extra", "Extra");
    } else {
      result.emplace_back("value", "Value", "V");
      result.emplace_back("status", "Status", "", WidgetInputKind::kText);
    }
    if (grown_) {
      result.emplace_back("grown", "Grown");
    }
    return result;
  }

  void* createWidget(void* parent) override {
    if (g.on_create_widget) {
      g.on_create_widget();
    }
    if (has("read_past_at_create")) {
      readHistory();
    }
    if (g.null_widget) {
      return nullptr;
    }
    ++g.widgets_created;
    label_ = new QLabel(static_cast<QWidget*>(parent));
    return label_.data();
  }

  void onUpdate(Span<const PJ_widget_value_t> values, double /*time*/) override {
    ++g.updates;
    if (has("throw_on_update")) {
      throw std::runtime_error("on_update failed");
    }
    g.last_values.clear();
    for (const PJ_widget_value_t& v : values) {
      ValueCopy copy;
      copy.input = v.input;
      copy.slot = v.slot;
      copy.kind = v.kind;
      copy.time = v.time;
      copy.number = v.number;
      if (v.text.data != nullptr) {
        copy.text.assign(v.text.data, v.text.size);
      }
      if (v.bytes.data != nullptr) {
        copy.bytes.assign(v.bytes.data, v.bytes.data + v.bytes.size);
      }
      if (v.type_name.data != nullptr) {
        copy.type_name.assign(v.type_name.data, v.type_name.size);
      }
      copy.label.assign(v.label.data == nullptr ? "" : v.label.data, v.label.size);
      g.last_values.push_back(std::move(copy));
    }
    if (has("history")) {
      readHistory();
    }
    if (has("grow_on_update") && !grown_) {
      grown_ = true;
      widgetHost().notifyInputsChanged();
    }
  }

  std::string saveConfig() const override {
    if (has("own_state")) {
      return R"({"unit":"from_state","own_state":true})";
    }
    return config_;
  }

  Status loadConfig(std::string_view config_json) override {
    ++g.load_configs;
    g.last_config = std::string(config_json);
    config_ = g.last_config;
    if (has("reject")) {
      return unexpected("rejected");
    }
    return okStatus();
  }

 private:
  void readHistory() {
    g.history_samples = 0;
    const Expected<uint32_t> total =
        widgetHost().queryHistory(0, 0, 0.0, 100.0, 0, [](const PJ_widget_value_t&) { ++g.history_samples; });
    g.history_ok = total.has_value();
    g.history_total = total ? *total : 0U;
  }

  [[nodiscard]] bool has(const char* token) const {
    return config_.find(token) != std::string::npos;
  }

  std::string type_;
  std::string config_ = "{}";
  bool grown_ = false;
  QPointer<QLabel> label_;
};

inline const PJ_widget_build_info_t* hostQtBuildInfo() noexcept {
  static const PJ_widget_build_info_t info =
      detail::makeWidgetBuildInfo(QT_VERSION_MAJOR, QT_VERSION_MINOR, QT_VERSION_PATCH, QT_VERSION_STR);
  return &info;
}

inline const PJ_widget_build_info_t* otherQtBuildInfo() noexcept {
  static const PJ_widget_build_info_t info =
      detail::makeWidgetBuildInfo(QT_VERSION_MAJOR, QT_VERSION_MINOR + 1, 0, "other");
  return &info;
}

inline const PJ_widget_vtable_t* goodVtable() {
  return WidgetPluginBase::vtableWithCreate(
      [](PJ_string_view_t type) noexcept -> void* { return WidgetPluginBase::instantiate<TestPlugin>(type); },
      kManifest, &hostQtBuildInfo);
}

// A string length past std::string::max_size(): copying such a view is the host's job, and the
// copy throws std::length_error before it reads a byte.
inline constexpr std::size_t kImpossibleLength = std::numeric_limits<std::size_t>::max() - 1;

// The good plugin's inputs(), except that once g.corrupt_inputs is set the first name is impossible
// (the host's handle refuses the array).
inline bool corruptingInputs(
    void* ctx, const PJ_widget_input_t** out_inputs, uint32_t* out_count, PJ_error_t* out_error) noexcept {
  if (!goodVtable()->inputs(ctx, out_inputs, out_count, out_error)) {
    return false;
  }
  if (!g.corrupt_inputs || *out_count == 0 || *out_inputs == nullptr) {
    return true;
  }
  static PJ_widget_input_t corrupt{};
  corrupt = (*out_inputs)[0];
  corrupt.name.size = kImpossibleLength;
  *out_inputs = &corrupt;
  *out_count = 1;
  return true;
}

// The good plugin's save_config(), except that once g.corrupt_saved_config is set its length is impossible.
inline bool corruptingSaveConfig(void* ctx, PJ_string_view_t* out_json, PJ_error_t* out_error) noexcept {
  const bool ok = goodVtable()->save_config(ctx, out_json, out_error);
  if (ok && g.corrupt_saved_config && out_json != nullptr) {
    out_json->size = kImpossibleLength;
  }
  return ok;
}

inline const PJ_widget_vtable_t* faultyVtable() {
  static const PJ_widget_vtable_t vtable = [] {
    PJ_widget_vtable_t copy = *goodVtable();
    copy.manifest_json = kFaultyManifest;
    copy.inputs = &corruptingInputs;
    copy.save_config = &corruptingSaveConfig;
    return copy;
  }();
  return &vtable;
}

inline const PJ_widget_vtable_t* otherQtVtable() {
  static const PJ_widget_vtable_t vtable = [] {
    PJ_widget_vtable_t copy = *goodVtable();
    copy.manifest_json = kOtherQtManifest;
    copy.build_info = &otherQtBuildInfo;
    return copy;
  }();
  return &vtable;
}

}  // namespace pj_cw_test
