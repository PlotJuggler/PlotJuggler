// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// DataSourceRuntimeHost::bind with a settings store: the plugin reaches it as pj.settings.v1 up to its destructor,
// since the host owns the store and outlives the handle; without one the plugin sees no store.

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/sdk/data_source_plugin_base.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/settings_store_host.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host/data_source_handle.hpp"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"

namespace {

// What the plugin's destructor found: "stored", "failed" or "unbound".
std::string g_destroy_outcome;

class SettingsProbeSource final : public PJ::DataSourcePluginBase {
 public:
  ~SettingsProbeSource() override {
    const auto settings = services().get<PJ::sdk::SettingsStoreService>();
    if (!settings.has_value()) {
      g_destroy_outcome = "unbound";
      return;
    }
    g_destroy_outcome = settings->setValue("last_words", "goodbye") ? "stored" : "failed";
  }
  uint64_t capabilities() const override {
    return PJ::kCapabilityFiniteImport | PJ::kCapabilityDirectIngest;
  }
  PJ::Status start() override {
    return PJ::okStatus();
  }
  void stop() override {}
  PJ::DataSourceState currentState() const override {
    return PJ::DataSourceState::kIdle;
  }
};

const PJ_data_source_vtable_t* probeVtable() {
  static const PJ_data_source_vtable_t* vtable = PJ::DataSourcePluginBase::vtableWithCreate(
      []() noexcept -> void* { return new SettingsProbeSource(); },
      R"({"id":"settings-probe","name":"Settings Probe","version":"1.0.0"})");
  return vtable;
}

// Writes into a map the test owns, so the writes stay readable after the host is gone.
class RecordingBackend final : public PJ::sdk::SettingsBackend {
 public:
  explicit RecordingBackend(std::map<std::string, std::string>& stored) : stored_(stored) {}
  std::optional<std::string> getString(std::string_view key) override {
    const auto it = stored_.find(std::string(key));
    return it == stored_.end() ? std::nullopt : std::optional<std::string>(it->second);
  }
  void setString(std::string_view key, std::string_view value) override {
    stored_[std::string(key)] = std::string(value);
  }
  std::optional<std::vector<std::string>> getStringList(std::string_view) override {
    return std::nullopt;
  }
  void setStringList(std::string_view, const std::vector<std::string>&) override {}
  bool contains(std::string_view key) override {
    return stored_.contains(std::string(key));
  }
  void remove(std::string_view key) override {
    stored_.erase(std::string(key));
  }

 private:
  std::map<std::string, std::string>& stored_;
};

class DataSourceRuntimeHostSettingsTest : public ::testing::Test {
 protected:
  // Binds a probe plugin with `settings`, then destroys the plugin before its host, as every owner does.
  void bindAndDestroy(std::unique_ptr<PJ::sdk::SettingsBackend> settings) {
    g_destroy_outcome.clear();
    auto dataset_or = engine_.createDataset(PJ::DatasetDescriptor{.source_name = "test", .time_domain_id = 0});
    ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();
    auto host = std::make_unique<PJ::DataSourceRuntimeHost>(
        engine_, catalog_, static_cast<PJ::DatasetId>(*dataset_or),
        PJ_data_source_handle_t{static_cast<uint32_t>(*dataset_or)}, object_store_, "settings-probe",
        /*parser_registrar=*/nullptr, /*secondary_object_store=*/nullptr, /*secondary_data_engine=*/nullptr,
        /*library_keepalive=*/nullptr);
    auto handle = std::make_unique<PJ::DataSourceHandle>(probeVtable());
    ASSERT_TRUE(handle->valid());
    ASSERT_TRUE(host->bind(*handle, std::move(settings)).has_value());
    handle.reset();
    host.reset();
  }

  PJ::ExtensionCatalogService catalog_;
  PJ::DataEngine engine_;
  PJ::ObjectStore object_store_;
};

TEST_F(DataSourceRuntimeHostSettingsTest, APluginWritesItsStoreFromItsDestructor) {
  std::map<std::string, std::string> stored;
  bindAndDestroy(std::make_unique<RecordingBackend>(stored));
  EXPECT_EQ(g_destroy_outcome, "stored");
  EXPECT_EQ(stored, (std::map<std::string, std::string>{{"last_words", "goodbye"}}));
}

TEST_F(DataSourceRuntimeHostSettingsTest, WithoutAStoreThePluginSeesNone) {
  bindAndDestroy(nullptr);
  EXPECT_EQ(g_destroy_outcome, "unbound");
}

}  // namespace
