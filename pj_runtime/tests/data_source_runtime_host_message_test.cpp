// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// DataSourceRuntimeHost::setDiagnosticSink: the plugin's report_message
// WARNING/ERROR reach the sink with the configured source and subject, INFO
// never does, and the sink runs on the thread that reported.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/sdk/data_source_host_views.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"

namespace {

class DataSourceRuntimeHostMessageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto dataset_or = engine_.createDataset(PJ::DatasetDescriptor{.source_name = "test", .time_domain_id = 0});
    ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();
    dataset_id_ = static_cast<PJ::DatasetId>(*dataset_or);
    source_handle_ = PJ_data_source_handle_t{static_cast<uint32_t>(*dataset_or)};
    host_ = std::make_unique<PJ::DataSourceRuntimeHost>(
        engine_, catalog_, dataset_id_, source_handle_, object_store_, "message_test_source",
        /*parser_registrar=*/nullptr, /*secondary_object_store=*/nullptr, /*secondary_data_engine=*/nullptr,
        /*library_keepalive=*/nullptr);
  }

  void registerServices() {
    EXPECT_TRUE(host_->registerServices(registry_builder_).has_value());
  }

  [[nodiscard]] PJ::DataSourceRuntimeHostView runtime() {
    PJ::sdk::ServiceRegistry services(registry_builder_.view());
    auto runtime_or = services.require<PJ::sdk::DataSourceRuntimeHostService>();
    EXPECT_TRUE(runtime_or.has_value()) << runtime_or.error();
    return runtime_or.has_value() ? *runtime_or : PJ::DataSourceRuntimeHostView{};
  }

  [[nodiscard]] PJ::DiagnosticSink collector() {
    return [this](const PJ::Diagnostic& d) {
      std::lock_guard<std::mutex> lock(mutex_);
      entries_.emplace_back(d, std::this_thread::get_id());
    };
  }

  std::vector<std::pair<PJ::Diagnostic, std::thread::id>> entries() {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
  }

  PJ::ExtensionCatalogService catalog_;
  PJ::DataEngine engine_;
  PJ::ObjectStore object_store_;
  PJ::DatasetId dataset_id_{0};
  PJ_data_source_handle_t source_handle_{};
  std::unique_ptr<PJ::DataSourceRuntimeHost> host_;
  PJ::ServiceRegistryBuilder registry_builder_;
  std::mutex mutex_;
  std::vector<std::pair<PJ::Diagnostic, std::thread::id>> entries_;
};

TEST_F(DataSourceRuntimeHostMessageTest, WarningAndErrorReachTheSinkWithSourceAndSubject) {
  host_->setDiagnosticSink(collector(), "Probe Loader", "data.csv");
  registerServices();
  runtime().reportMessage(PJ::DataSourceMessageLevel::kWarning, "w");
  runtime().reportMessage(PJ::DataSourceMessageLevel::kError, "e");

  const auto got = entries();
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].first.level, PJ::DiagnosticLevel::kWarning);
  EXPECT_EQ(got[0].first.source, "Probe Loader");
  EXPECT_EQ(got[0].first.id, "data-source-message");
  EXPECT_EQ(got[0].first.message, "data.csv: w");
  EXPECT_EQ(got[1].first.level, PJ::DiagnosticLevel::kError);
  EXPECT_EQ(got[1].first.source, "Probe Loader");
  EXPECT_EQ(got[1].first.id, "data-source-message");
  EXPECT_EQ(got[1].first.message, "data.csv: e");
}

TEST_F(DataSourceRuntimeHostMessageTest, InfoIsNotForwarded) {
  host_->setDiagnosticSink(collector(), "Probe Loader", "data.csv");
  registerServices();
  runtime().reportMessage(PJ::DataSourceMessageLevel::kInfo, "i");
  runtime().reportMessage(PJ::DataSourceMessageLevel::kWarning, "w");

  const auto got = entries();
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].first.level, PJ::DiagnosticLevel::kWarning);
  EXPECT_EQ(got[0].first.message, "data.csv: w");
}

TEST_F(DataSourceRuntimeHostMessageTest, ForwardingStopsAtTheLimitWithOneNotice) {
  host_->setDiagnosticSink(collector(), "Probe Loader", "data.csv");
  registerServices();
  for (int i = 0; i < 25; ++i) {
    runtime().reportMessage(PJ::DataSourceMessageLevel::kWarning, "w");
  }
  auto count = [&]() {
    std::size_t messages = 0;
    std::size_t limits = 0;
    for (const auto& entry : entries()) {
      messages += entry.first.id == "data-source-message" ? 1 : 0;
      limits += entry.first.id == "data-source-message-limit" ? 1 : 0;
    }
    return std::pair{messages, limits};
  };
  EXPECT_EQ(count(), (std::pair<std::size_t, std::size_t>{20, 1}));
  const auto got = entries();
  ASSERT_EQ(got.size(), 21u);
  EXPECT_EQ(got.back().first.level, PJ::DiagnosticLevel::kWarning);
  EXPECT_EQ(got.back().first.source, "Probe Loader");
  EXPECT_EQ(got.back().first.message, "data.csv: further plugin messages are in the log only");

  runtime().reportMessage(PJ::DataSourceMessageLevel::kError, "e");
  EXPECT_EQ(entries().size(), 21u);
}

TEST_F(DataSourceRuntimeHostMessageTest, NoSinkIsHarmless) {
  registerServices();
  runtime().reportMessage(PJ::DataSourceMessageLevel::kError, "e");
  host_->setDiagnosticSink(PJ::DiagnosticSink{}, "Probe Loader", "data.csv");
  runtime().reportMessage(PJ::DataSourceMessageLevel::kError, "e");
  EXPECT_TRUE(entries().empty());
}

TEST_F(DataSourceRuntimeHostMessageTest, ReportFromAnotherThreadRunsTheSinkOnThatThread) {
  host_->setDiagnosticSink(collector(), "Probe Loader", "data.csv");
  registerServices();
  std::thread::id worker_id{};
  std::thread worker([&]() {
    worker_id = std::this_thread::get_id();
    runtime().reportMessage(PJ::DataSourceMessageLevel::kWarning, "w");
  });
  worker.join();

  const auto got = entries();
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].second, worker_id);
  EXPECT_NE(got[0].second, std::this_thread::get_id());
}

}  // namespace
