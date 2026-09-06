// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Lifecycle-composition tests of the M3 owner wiring: ToolboxRuntimeHost's
// parser-ingest create/release/discard driving SourceCaptureService end to
// end over a real DataSourceRuntimeHost and cache directory. What the
// service-level tests cannot see: that RELEASE is the commit, DISCARD and a
// host stop never publish, and a re-created context is a fresh capture
// generation.

#include <gtest/gtest.h>

#include <QFileInfo>
#include <QString>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "hermetic_catalog.h"
#include "pj_base/sdk/data_source_host_views.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/settings_store_host.hpp"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/SourceCaptureService.h"
#include "pj_runtime/ToolboxRuntimeHost.h"
using namespace Qt::StringLiterals;

#ifndef PJ_RUNTIME_HOST_OBJECT_PARSER_PATH
#error "PJ_RUNTIME_HOST_OBJECT_PARSER_PATH must be defined"
#endif

namespace fs = std::filesystem;

namespace PJ::test {
namespace {

constexpr const char* kProvider = "toolbox.capture.provider";
constexpr const char* kDescriptor = R"({"kind":"toolbox-capture-request","request":{},"v":1})";

/// One record-worthy release the wiring reported through on_capture_finalized.
struct FinalizedEvent {
  DatasetId dataset = 0;
  std::string provider_id;
  std::string descriptor_json;
  std::string source_identity;
  bool published = false;
  std::string refusal_reason;
  fs::path artifact_path;
};

class ToolboxCaptureWiringTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NE(catalog_.findParserByEncoding(u"runtime_host_object"_s), nullptr);
    std::random_device entropy;
    cache_root_ = fs::temp_directory_path() / ("toolbox_capture_test-" + std::to_string(entropy()));
    store_.emplace(cache_root_);
    service_.emplace(*store_);

    ToolboxRuntimeHost::ParserIngestDeps deps;
    deps.catalog = &catalog_;
    deps.capture_service = &*service_;
    deps.capture_provider_id = kProvider;
    deps.on_capture_finalized = [this](
                                    DatasetId dataset, const std::string& provider_id,
                                    const std::string& descriptor_json, const std::string& source_identity,
                                    bool published, const std::string& refusal_reason, const fs::path& artifact_path) {
      finalized_.push_back(
          {dataset, provider_id, descriptor_json, source_identity, published, refusal_reason, artifact_path});
    };
    host_.emplace(engine_, object_store_, settings_, ToolboxRuntimeHost::Callbacks{}, std::move(deps));
    ASSERT_TRUE(host_->registerServices(builder_).has_value());
    sdk::ServiceRegistry services(builder_.view());
    auto toolbox = services.require<sdk::ToolboxHostService>();
    ASSERT_TRUE(toolbox.has_value());
    toolbox_.emplace(*toolbox);
    auto runtime = services.require<sdk::ToolboxRuntimeHostService>();
    ASSERT_TRUE(runtime.has_value());
    runtime_.emplace(*runtime);
  }

  void TearDown() override {
    host_.reset();
    std::error_code ec;
    fs::remove_all(cache_root_, ec);
  }

  /// createDataSource + createDatasetIngest — one delegated download.
  [[nodiscard]] std::pair<uint32_t, DatasetIngestHostView> openIngest(const std::string& name) {
    auto ds = toolbox_->createDataSource(name);
    EXPECT_TRUE(ds.has_value()) << (ds.has_value() ? "" : ds.error());
    auto ingest = runtime_->createDatasetIngest(ds->id);
    EXPECT_TRUE(ingest.has_value()) << (ingest.has_value() ? "" : ingest.error());
    return {ds->id, *ingest};
  }

  [[nodiscard]] static ParserBindingHandle bindTopic(const DatasetIngestHostView& ingest, const std::string& topic) {
    auto binding = ingest.ensureParserBinding(
        ParserBindingRequest{
            .topic_name = topic,
            .parser_encoding = "runtime_host_object",
            .type_name = "mock/image",
            .schema = Span<const uint8_t>{},
            .parser_config_json = R"({"k":1})",
        });
    EXPECT_TRUE(binding.has_value()) << (binding.has_value() ? "" : binding.error());
    return binding.has_value() ? *binding : ParserBindingHandle{};
  }

  static void push(
      const DatasetIngestHostView& ingest, ParserBindingHandle binding, int64_t time_ns, std::vector<uint8_t> payload) {
    auto status =
        ingest.pushMessage(binding, Timestamp{time_ns}, [payload]() -> std::vector<uint8_t> { return payload; });
    ASSERT_TRUE(status.has_value()) << (status.has_value() ? "" : status.error());
  }

  /// A full clean download: attach, one topic, one message, COMPLETED.
  [[nodiscard]] uint32_t runCleanIngest(const std::string& name, const std::string& topic) {
    auto [dataset_id, ingest] = openIngest(name);
    EXPECT_TRUE(ingest.attachSourceRecord(kDescriptor).has_value());
    push(ingest, bindTopic(ingest, topic), 100, {1, 2, 3});
    const std::string_view requested[] = {topic};
    EXPECT_TRUE(ingest.completeIngest(sdk::IngestOutcome::kCompleted, {requested, 1}).has_value());
    return dataset_id;
  }

  [[nodiscard]] bool cacheHasArtifact() {
    std::string miss;
    return service_->resolve(kProvider, kDescriptor, &miss).has_value();
  }

  QFileInfo plugin_file_{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  HermeticCatalog catalog_box_{plugin_file_.absolutePath()};
  ExtensionCatalogService& catalog_{catalog_box_.service};
  DataEngine engine_;
  ObjectStore object_store_;
  sdk::InMemorySettingsBackend settings_;
  ServiceRegistryBuilder builder_;
  fs::path cache_root_;
  std::optional<SourceCacheStore> store_;
  std::optional<SourceCaptureService> service_;
  std::optional<ToolboxRuntimeHost> host_;
  std::optional<sdk::ToolboxHostView> toolbox_;
  std::optional<ToolboxRuntimeHostView> runtime_;
  std::vector<FinalizedEvent> finalized_;
};

TEST_F(ToolboxCaptureWiringTest, ReleasePublishesACleanCompletedIngest) {
  const uint32_t dataset_id = runCleanIngest("download", "/imu");
  EXPECT_FALSE(cacheHasArtifact());  // nothing published before the release
  ASSERT_TRUE(runtime_->releaseParserIngest(dataset_id).has_value());

  EXPECT_TRUE(cacheHasArtifact());
  ASSERT_EQ(finalized_.size(), 1u);
  const FinalizedEvent& event = finalized_.front();
  EXPECT_EQ(event.dataset, static_cast<DatasetId>(dataset_id));
  EXPECT_EQ(event.provider_id, kProvider);
  EXPECT_EQ(event.descriptor_json, kDescriptor);
  EXPECT_EQ(event.source_identity, sourceCacheIdentityDigest(kProvider, kDescriptor));
  EXPECT_TRUE(event.published) << event.refusal_reason;
  // The published artifact path travels with the event (the shell registers
  // it as the dataset's saveable backing file).
  EXPECT_FALSE(event.artifact_path.empty());
  EXPECT_TRUE(fs::exists(event.artifact_path));
}

TEST_F(ToolboxCaptureWiringTest, DiscardNeverPublishesAndReportsNothing) {
  const uint32_t dataset_id = runCleanIngest("download", "/imu");
  ASSERT_TRUE(runtime_->discardParserIngest(dataset_id).has_value());
  EXPECT_FALSE(cacheHasArtifact());
  EXPECT_TRUE(finalized_.empty());
  // No partial survives the abort.
  for (const auto& entry : fs::directory_iterator(cache_root_)) {
    EXPECT_TRUE(entry.path().string().find(".partial") == std::string::npos) << entry.path();
  }
}

TEST_F(ToolboxCaptureWiringTest, HostStopRefusesPublicationOnRelease) {
  const uint32_t dataset_id = runCleanIngest("download", "/imu");
  host_->requestStopActiveIngests();  // the shell's stop affordance
  ASSERT_TRUE(runtime_->releaseParserIngest(dataset_id).has_value());
  EXPECT_FALSE(cacheHasArtifact());
  EXPECT_TRUE(finalized_.empty());  // a stopped download is not record-worthy either
}

TEST_F(ToolboxCaptureWiringTest, IncompleteIngestReleasesWithoutPublishingButKeepsNoRecord) {
  auto [dataset_id, ingest] = openIngest("download");
  ASSERT_TRUE(ingest.attachSourceRecord(kDescriptor).has_value());
  push(ingest, bindTopic(ingest, "/imu"), 100, {1});
  // No completeIngest: the plugin vanished without a terminal.
  ASSERT_TRUE(runtime_->releaseParserIngest(dataset_id).has_value());
  EXPECT_FALSE(cacheHasArtifact());
  EXPECT_TRUE(finalized_.empty());
}

TEST_F(ToolboxCaptureWiringTest, RecreatedContextIsAFreshCaptureGeneration) {
  // Generation 1 releases WITHOUT a terminal: refused, nothing published.
  auto [dataset_id, first] = openIngest("download");
  ASSERT_TRUE(first.attachSourceRecord(kDescriptor).has_value());
  push(first, bindTopic(first, "/imu"), 100, {1});
  ASSERT_TRUE(runtime_->releaseParserIngest(dataset_id).has_value());
  EXPECT_FALSE(cacheHasArtifact());

  // Generation 2 on the SAME dataset id completes cleanly and publishes under
  // its own evidence — nothing of generation 1 leaks into it.
  auto second = runtime_->createDatasetIngest(dataset_id);
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(second->attachSourceRecord(kDescriptor).has_value());
  push(*second, bindTopic(*second, "/imu"), 200, {4, 5});
  const std::string_view requested[] = {std::string_view{"/imu"}};
  ASSERT_TRUE(second->completeIngest(sdk::IngestOutcome::kCompleted, {requested, 1}).has_value());
  ASSERT_TRUE(runtime_->releaseParserIngest(dataset_id).has_value());

  EXPECT_TRUE(cacheHasArtifact());
  ASSERT_EQ(finalized_.size(), 1u);
  EXPECT_TRUE(finalized_.front().published) << finalized_.front().refusal_reason;
}

// T10 (P1): the Preferences capture toggle gates ARMING only, evaluated at
// context CREATION, and applies immediately — a context created while off
// stays unarmed even when the toggle turns on before its attachment; a
// context created while on publishes even when the toggle turns off
// mid-flight; existing hits keep resolving while off.
TEST_F(ToolboxCaptureWiringTest, CaptureToggleGatesArmingOnlyAndAppliesImmediately) {
  constexpr const char* kDescOff = R"({"kind":"toggle-off","request":{},"v":1})";
  constexpr const char* kDescOn = R"({"kind":"toggle-on","request":{},"v":1})";
  const auto resolves = [this](const char* descriptor) {
    std::string miss;
    return service_->resolve(kProvider, descriptor, &miss).has_value();
  };
  const std::string_view requested[] = {std::string_view{"/imu"}};

  // Created while OFF: never arms — re-enabling BEFORE the attachment is
  // too late for this context, the gate is evaluated at creation.
  service_->setCaptureEnabled(false);
  auto [off_dataset, off_ingest] = openIngest("download-off");
  service_->setCaptureEnabled(true);
  ASSERT_TRUE(off_ingest.attachSourceRecord(kDescOff).has_value());
  push(off_ingest, bindTopic(off_ingest, "/imu"), 100, {1, 2, 3});
  ASSERT_TRUE(off_ingest.completeIngest(sdk::IngestOutcome::kCompleted, {requested, 1}).has_value());
  ASSERT_TRUE(runtime_->releaseParserIngest(off_dataset).has_value());
  EXPECT_FALSE(resolves(kDescOff)) << "a context created while capture is off must not publish";
  EXPECT_TRUE(finalized_.empty()) << "an unarmed context must report no capture event";

  // Created while ON: disabling MID-FLIGHT does not abort it — the armed
  // capture finishes and publishes normally (arming-only semantics).
  auto [on_dataset, on_ingest] = openIngest("download-on");
  ASSERT_TRUE(on_ingest.attachSourceRecord(kDescOn).has_value());
  push(on_ingest, bindTopic(on_ingest, "/imu"), 200, {4, 5});
  service_->setCaptureEnabled(false);
  ASSERT_TRUE(on_ingest.completeIngest(sdk::IngestOutcome::kCompleted, {requested, 1}).has_value());
  ASSERT_TRUE(runtime_->releaseParserIngest(on_dataset).has_value());
  EXPECT_TRUE(resolves(kDescOn)) << "an in-flight armed capture must finish normally after the flip";
  ASSERT_EQ(finalized_.size(), 1u);
  EXPECT_TRUE(finalized_.front().published) << finalized_.front().refusal_reason;

  // Disabling never breaks restore: the published hit still resolves.
  EXPECT_FALSE(service_->captureEnabled());
  EXPECT_TRUE(resolves(kDescOn));
}

TEST_F(ToolboxCaptureWiringTest, TeardownAbortsAnUnreleasedCapture) {
  const uint32_t dataset_id = runCleanIngest("download", "/imu");
  static_cast<void>(dataset_id);
  host_.reset();  // context never released: the capture aborts with it
  EXPECT_FALSE(cacheHasArtifact());
  EXPECT_TRUE(finalized_.empty());
}

}  // namespace
}  // namespace PJ::test
