// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Test-only streaming DataSource for StreamingPresetTest. It carries a dialog
// (kCapabilityHasDialog) so an interactive start would open one, and its
// configuration steers what it does, so a test can tell which configuration
// reached it:
//   "topic":"<name>"  the topic it writes (default "layout/stream")
//   "value":<number>  the value of every sample (default 1)
//   "gate_file":"<path>"  publish nothing until that file exists (default: no gate)
//   "reject":true     loadConfig() refuses the configuration
//   "fail_start":true start() fails
// Each loadConfig/start/stop appends one line ("load <config>", "start",
// "stop") to the file PJ_LAYOUT_STREAM_PROBE_FILE names, when set.

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_base/sdk/platform.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <string_view>

namespace {

constexpr const char* kDialogManifestJson = R"({
  "id": "mock-layout-stream-dialog",
  "name": "Mock Layout Stream",
  "version": "1.0.0",
  "description": "Dialog of the layout-replay test stream"
})";

constexpr const char* kUiContent = R"(<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <class>MockLayoutStream</class>
 <widget class="QWidget" name="MockLayoutStream">
  <layout class="QVBoxLayout">
   <item><widget class="QLineEdit" name="topic_input"/></item>
   <item><widget class="QDialogButtonBox" name="buttonBox">
    <property name="standardButtons"><set>QDialogButtonBox::Cancel|QDialogButtonBox::Ok</set></property>
   </widget></item>
  </layout>
 </widget>
</ui>
)";

void recordProbe(const std::string& line) {
  const auto path = PJ::sdk::getEnv("PJ_LAYOUT_STREAM_PROBE_FILE");
  if (!path.has_value()) {
    return;
  }
  // Binary: Windows text mode would end lines with "\r\n", which the test's line split keeps.
  std::ofstream file(*path, std::ios::app | std::ios::binary);
  file << line << '\n';
}

// The text after `"key":` in a flat JSON object (substring match: test configs only).
std::string_view valueAfter(std::string_view json, std::string_view key) {
  const std::string pattern = "\"" + std::string(key) + "\":";
  const auto at = json.find(pattern);
  return at == std::string_view::npos ? std::string_view{} : json.substr(at + pattern.size());
}

std::string stringValue(std::string_view json, std::string_view key, std::string fallback) {
  std::string_view rest = valueAfter(json, key);
  if (rest.empty() || rest.front() != '"') {
    return fallback;
  }
  rest.remove_prefix(1);
  return std::string(rest.substr(0, rest.find('"')));
}

double numberValue(std::string_view json, std::string_view key, double fallback) {
  const std::string rest(valueAfter(json, key));
  return rest.empty() ? fallback : std::strtod(rest.c_str(), nullptr);
}

class MockLayoutStreamDialog : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override {
    return kDialogManifestJson;
  }
  std::string ui_content() const override {
    return kUiContent;
  }
  std::string widget_data() override {
    PJ::WidgetData data;
    data.setOkEnabled(true);
    return data.toJson();
  }
  std::string saveConfig() const override {
    return "{}";
  }
  bool loadConfig(std::string_view) override {
    return true;
  }
};

class MockLayoutStream : public PJ::StreamSourceBase {
 public:
  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest | PJ::kCapabilityHasDialog;
  }

  std::string saveConfig() const override {
    return config_;
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    recordProbe("load " + std::string(config_json));
    if (config_json.find("\"reject\":true") != std::string_view::npos) {
      return PJ::unexpected("mock_layout_stream: configuration refused");
    }
    config_ = std::string(config_json);
    return PJ::okStatus();
  }

  PJ::Status onStart() override {
    recordProbe("start");
    if (config_.find("\"fail_start\":true") != std::string::npos) {
      return PJ::unexpected("mock_layout_stream: configured start failure");
    }
    topic_name_ = stringValue(config_, "topic", "layout/stream");
    value_ = numberValue(config_, "value", 1.0);
    gate_file_ = stringValue(config_, "gate_file", "");
    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    if (!gate_file_.empty() && !std::ifstream(gate_file_).good()) {
      return PJ::okStatus();
    }
    ++polls_;
    if (!topic_.has_value()) {
      auto topic = writeHost().ensureTopic(topic_name_);
      if (!topic) {
        return PJ::unexpected(topic.error());
      }
      topic_ = *topic;
    }
    const PJ::Timestamp stamp{1'000'000'000LL + static_cast<int64_t>(polls_) * 1'000'000LL};
    auto status = writeHost().appendRecord(*topic_, stamp, {{.name = "value", .value = value_}});
    return status ? PJ::okStatus() : PJ::unexpected(status.error());
  }

  void onStop() override {
    recordProbe("stop");
  }

 private:
  MockLayoutStreamDialog dialog_;
  std::string config_ = "{}";
  std::string topic_name_;
  double value_ = 1.0;
  std::string gate_file_;
  int polls_ = 0;
  std::optional<PJ::sdk::TopicHandle> topic_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(
    MockLayoutStream, R"({"id":"mock-layout-stream","name":"Mock Layout Stream","version":"1.0.0",)"
                      R"("description":"Streaming source driven by its saved configuration"})")

PJ_DIALOG_PLUGIN(MockLayoutStreamDialog, kDialogManifestJson)
