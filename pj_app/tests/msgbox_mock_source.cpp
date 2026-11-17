// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Test-only DataSource plugin that exercises the runtime host's message-box path from both plugin lifecycle
// contexts used by FileLoader:
//
//  * "ask_msgbox" makes importData() ask from the import worker thread.
//  * "ask_msgbox_in_load_config" makes loadConfig() ask synchronously while FileLoader is still on the GUI thread.
//
// file_loader_test uses them to enforce both halves of the synchronous show_message_box contract: the host marshals
// worker calls to the GUI thread, and a GUI-thread caller receives the button the user clicked.
//
// Claims ".msgboxmock"; writes topic "mock/file_data" with a single row so a successful (Continue) askContinue
// produces a loadable dataset.

#include <fstream>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_base/sdk/platform.hpp>
#include <string>

namespace {

// Appends one line to the file named by PJ_MSGBOX_PROBE_FILE (flushed), so a test can observe the worker's
// askContinue outcome across the DSO boundary, e.g. that the shutdown gate answered -1. No-op when unset.
void recordMsgboxProbe(const char* line) {
  const auto path = PJ::sdk::getEnv("PJ_MSGBOX_PROBE_FILE");
  if (!path.has_value() || path->empty()) {
    return;
  }
  std::ofstream file(*path, std::ios::app);
  if (file) {
    file << line << '\n';
    file.flush();
  }
}

class MsgBoxMockSource : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest;
  }

  std::string saveConfig() const override {
    return config_;
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    config_ = std::string(config_json);
    if (config_.find("ask_msgbox_in_load_config") != std::string::npos) {
      if (!runtimeHost().askContinue("Regression", "Continue the GUI-thread loadConfig?")) {
        recordMsgboxProbe("load_config_aborted");
        return PJ::unexpected("loadConfig did not receive Continue");
      }
      recordMsgboxProbe("load_config_continued");
    }
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    // config_ is opaque here (no JSON dependency); the marker is matched by substring, safe because only the test
    // sets this config.
    if (config_.find("\"ask_msgbox\":true") != std::string::npos) {
      // Runs on the import worker thread. The host must marshal the dialog to the GUI thread (data_source_protocol.h:
      // show_message_box is [main-thread]); building the QWidget here directly would violate that.
      if (!runtimeHost().askContinue("Regression", "Continue the worker-thread load?")) {
        // askContinue == false: the host answered anything but Continue; at shutdown the message gate answers -1.
        recordMsgboxProbe("aborted");
        return PJ::unexpected("aborted by user");
      }
      recordMsgboxProbe("continued");
    }

    auto topic = writeHost().ensureTopic("mock/file_data");
    if (!topic) {
      return PJ::unexpected(topic.error());
    }
    auto status = writeHost().appendRecord(*topic, PJ::Timestamp{100}, {{.name = "value", .value = 1.0}});
    if (!status) {
      return PJ::unexpected(status.error());
    }
    return PJ::okStatus();
  }

 private:
  std::string config_ = "{}";
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(
    MsgBoxMockSource, R"({"id":"msgbox-mock-source","name":"Msgbox Mock Source",)"
                      R"("version":"1.0.0","description":"Test runtime-host message box from a worker thread",)"
                      R"("file_extensions":[".msgboxmock"]})")
