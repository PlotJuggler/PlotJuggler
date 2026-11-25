// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Test-only DataSource plugin for FileLoader's shutdown/progress race. It
// reports one progress update after the loader's flush throttle has elapsed,
// records that the host callback returned (and therefore queued its GUI
// metacall), then remains on the worker until joinForShutdown requests stop.

#include <chrono>
#include <fstream>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_base/sdk/platform.hpp>
#include <string>
#include <thread>

#include "FileLoadFlushInterval.h"

namespace {

void recordProbe(const char* marker) {
  const auto path = PJ::sdk::getEnv("PJ_PROGRESS_SHUTDOWN_PROBE_FILE");
  if (!path.has_value() || path->empty()) {
    return;
  }
  std::ofstream file(*path, std::ios::app);
  if (file) {
    file << marker << '\n';
  }
}

class ProgressShutdownSource : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest;
  }

  std::string saveConfig() const override {
    return "{}";
  }

  PJ::Status loadConfig(std::string_view) override {
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    auto progress = runtimeHost().progressStart("Shutdown progress probe", 1, true);
    if (!progress) {
      return progress;
    }

    auto topic = writeHost().ensureTopic("shutdown/progress");
    if (!topic) {
      return PJ::unexpected(topic.error());
    }
    auto append = writeHost().appendRecord(*topic, PJ::Timestamp{1}, {{.name = "value", .value = 1.0}});
    if (!append) {
      return append;
    }

    // FileLoader throttles worker-side flush/progress posting. Sleep past it so
    // progressUpdate necessarily posts its GUI metacall.
    std::this_thread::sleep_for(PJ::kFileLoadFlushInterval + std::chrono::milliseconds(50));
    if (!runtimeHost().progressUpdate(1)) {
      return PJ::unexpected("progress unexpectedly cancelled before probe handshake");
    }
    recordProbe("progress_callback_returned");

    // Keep joinForShutdown on the real running-worker path. Its requestStop()
    // releases this loop; the GUI thread is blocked in wait() and cannot drain
    // the already queued progress metacall before ctx_ is reset.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!runtimeHost().isStopRequested() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!runtimeHost().isStopRequested()) {
      return PJ::unexpected("timed out waiting for shutdown stop request");
    }
    recordProbe("stop_observed");
    return PJ::unexpected("stopped for shutdown probe");
  }
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(
    ProgressShutdownSource,
    R"({"id":"progress-shutdown-source","name":"Progress Shutdown Source","version":"1.0.0",)"
    R"("description":"Test queued progress delivery during shutdown","file_extensions":[".progressshutdown"]})")
