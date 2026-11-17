// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A deliberately tiny end-to-end fixture for the browser ingress path. Compiled only with
// PJ_WASM_ENABLE_INGRESS_PROBE: the browser picker stages bytes into MEMFS, FileLoader injects that backing path
// into config, and this statically registered source must reopen and consume the file.

#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <pj_base/builtin/scene_entities_codec.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kProbeDialogManifest =
    R"({"id":"wasm-ingress-probe-dialog","name":"WASM Ingress Probe","version":"1.0.0"})";

constexpr const char* kProbeDialogUi = R"(<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <class>WasmIngressProbeDialog</class>
 <widget class="QWidget" name="WasmIngressProbeDialog">
  <property name="windowTitle"><string>Confirm browser file</string></property>
  <layout class="QVBoxLayout">
   <item><widget class="QLabel" name="probe_label"/></item>
   <item><widget class="QDialogButtonBox" name="buttonBox">
    <property name="standardButtons"><set>QDialogButtonBox::Cancel|QDialogButtonBox::Ok</set></property>
   </widget></item>
  </layout>
 </widget>
</ui>)";

class WasmIngressProbeDialog : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override {
    return kProbeDialogManifest;
  }
  std::string ui_content() const override {
    return kProbeDialogUi;
  }
  std::string widget_data() override {
    PJ::WidgetData data;
    data.setLabel("probe_label", "The browser upload is staged. Continue importing it?");
    data.setOkEnabled(true);
    return data.toJson();
  }
  void onAccepted(std::string_view) override {}
  void onRejected() override {}
  std::string saveConfig() const override {
    return nlohmann::json{{"filepath", filepath_}}.dump();
  }
  bool loadConfig(std::string_view config_json) override {
    const auto config = nlohmann::json::parse(config_json, nullptr, false);
    if (config.is_discarded() || !config.is_object()) {
      return false;
    }
    filepath_ = config.value("filepath", std::string{});
    return !filepath_.empty();
  }

 private:
  std::string filepath_;
};

}  // namespace

class WasmIngressProbeSource : public PJ::FileSourceBase {
 public:
  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest | PJ::kCapabilityHasDialog;
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    auto config = nlohmann::json::parse(config_json, nullptr, false);
    if (config.is_discarded() || !config.is_object()) {
      return PJ::unexpected("ingress probe: invalid config JSON");
    }
    filepath_ = config.value("filepath", std::string{});
    if (filepath_.empty()) {
      return PJ::unexpected("ingress probe: missing filepath");
    }
    if (!dialog_.loadConfig(config_json)) {
      return PJ::unexpected("ingress probe: dialog rejected config");
    }
    return PJ::okStatus();
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status importData() override {
    std::ifstream input(filepath_, std::ios::binary);
    if (!input) {
      return PJ::unexpected("ingress probe: staged file is not readable");
    }

    uint64_t byte_count = 0;
    uint64_t byte_sum = 0;
    char buffer[4096];
    while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
      const auto count = static_cast<std::size_t>(input.gcount());
      byte_count += count;
      for (std::size_t i = 0; i < count; ++i) {
        byte_sum += static_cast<unsigned char>(buffer[i]);
      }
    }
    if (!input.eof()) {
      return PJ::unexpected("ingress probe: failed while reading staged file");
    }

    if (filepath_.size() >= 10 && filepath_.compare(filepath_.size() - 10, 10, ".pjmarkers") == 0) {
      const auto* object_host = objectWriteHost();
      if (object_host == nullptr) {
        return PJ::unexpected("marker axes probe: object write host is unavailable");
      }
      PJ::sdk::SceneEntities scene;
      PJ::sdk::SceneEntity entity;
      entity.frame_id = "map";
      entity.id = "axes-probe";
      PJ::sdk::AxesPrimitive axes;
      axes.pose.position = {.x = 0.0, .y = 0.0, .z = 0.0};
      axes.length = 1.25;
      axes.thickness = 0.12;
      entity.axes.push_back(axes);
      scene.entities.push_back(std::move(entity));
      const std::vector<std::uint8_t> bytes = PJ::serializeSceneEntities(scene);
      auto topic = object_host->registerTopic("/marker_axes", R"({"builtin_object_type":"kSceneEntities"})");
      if (!topic) {
        return PJ::unexpected(topic.error());
      }
      auto status =
          object_host->pushOwned(*topic, PJ::Timestamp{0}, PJ::Span<const std::uint8_t>(bytes.data(), bytes.size()));
      if (!status) {
        return status;
      }
      std::fprintf(stderr, "PJ_WASM_MARKER_AXES_PROBE_OK arms=3\n");
      return PJ::okStatus();
    }

    auto topic = writeHost().ensureTopic("wasm/ingress_probe");
    if (!topic) {
      return PJ::unexpected(topic.error());
    }
    auto status = writeHost().appendRecord(
        *topic, PJ::Timestamp{0},
        {{.name = "byte_count", .value = static_cast<double>(byte_count)},
         {.name = "byte_sum", .value = static_cast<double>(byte_sum)}});
    if (!status) {
      return status;
    }

    std::fprintf(
        stderr, "PJ_WASM_INGRESS_PROBE_OK bytes=%llu sum=%llu\n", static_cast<unsigned long long>(byte_count),
        static_cast<unsigned long long>(byte_sum));
    return PJ::okStatus();
  }

 private:
  WasmIngressProbeDialog dialog_;
  std::string filepath_;
};

PJ_DATA_SOURCE_PLUGIN(
    WasmIngressProbeSource,
    R"({"id":"wasm-ingress-probe","name":"WASM Ingress Probe","version":"1.0.0",)"
    R"("description":"Internal browser file-ingress fixture","file_extensions":[".pjprobe",".pjmarkers"]})")

PJ_DIALOG_PLUGIN(WasmIngressProbeDialog, kProbeDialogManifest)
