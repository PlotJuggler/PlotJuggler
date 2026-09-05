#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Test support for the recording tests that drive a REAL DataSourceRuntimeHost:
// a host bound to the `runtime_host_object` parser plugin through a hermetic
// catalog, plus the bind/push/read helpers that exercise it. Requires the
// including target to define PJ_RUNTIME_HOST_OBJECT_PARSER_PATH and to depend
// on the runtime_host_object_parser_plugin target.

#include <gtest/gtest.h>

#include <QFileInfo>
#include <QString>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "hermetic_catalog.h"
#include "pj_base/sdk/data_source_host_views.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/ObjectIngestTap.h"

#ifndef PJ_RUNTIME_HOST_OBJECT_PARSER_PATH
#error "PJ_RUNTIME_HOST_OBJECT_PARSER_PATH must be defined"
#endif

namespace PJ::test {

/// One live source's worth of host: its dataset, its DataSourceRuntimeHost over
/// the mock object parser, and the service registry a plugin would reach it
/// through. A second recorded source is a second box on the same engine and
/// catalog, which is why this is not folded into the fixture below.
struct HostBox {
  /// The one parser signature these tests bind topics with. All three fields
  /// reach the recorder verbatim, so tests assert on them.
  static constexpr const char* kEncoding = "runtime_host_object";
  static constexpr const char* kTypeName = "mock/image";
  static constexpr const char* kParserConfig = R"({"k":1})";

  DatasetId dataset_id = 0;
  PJ_data_source_handle_t handle{};
  ServiceRegistryBuilder registry;
  std::unique_ptr<DataSourceRuntimeHost> host;

  /// Creates this box's dataset on `engine` and stands its host up over
  /// `catalog`, with the eager ingest policy every recording test starts from
  /// (a pure-lazy push fetches no bytes on the push thread and so records
  /// nothing). Leaves `host` null if the dataset cannot be created, so callers
  /// assert on it before using the box.
  void open(
      DataEngine& engine, ExtensionCatalogService& catalog, ObjectStore& object_store,
      std::shared_ptr<ObjectIngestTapRegistry> ingest_taps, const std::string& dataset_name,
      const std::string& source_id) {
    auto dataset_or = engine.createDataset(DatasetDescriptor{.source_name = dataset_name, .time_domain_id = 0});
    ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();
    dataset_id = static_cast<DatasetId>(*dataset_or);
    handle = PJ_data_source_handle_t{static_cast<uint32_t>(*dataset_or)};
    host = std::make_unique<DataSourceRuntimeHost>(
        engine, catalog, dataset_id, handle, object_store, source_id,
        /*parser_registrar=*/nullptr, /*secondary_object_store=*/nullptr, /*secondary_data_engine=*/nullptr,
        /*library_keepalive=*/nullptr, std::move(ingest_taps));
    ASSERT_TRUE(host->registerServices(registry).has_value());
    host->policyResolver().setDefault(sdk::ObjectIngestPolicy::kEager);
  }

  /// The delegated-ingest view a plugin would hold — the only way in to the host.
  [[nodiscard]] DataSourceRuntimeHostView runtime() {
    sdk::ServiceRegistry services(registry.view());
    auto runtime_or = services.require<sdk::DataSourceRuntimeHostService>();
    EXPECT_TRUE(runtime_or.has_value()) << runtime_or.error();
    return runtime_or.has_value() ? *runtime_or : DataSourceRuntimeHostView{};
  }

  [[nodiscard]] Expected<ParserBindingHandle> bind(const std::string& topic) {
    return runtime().ensureParserBinding(
        ParserBindingRequest{
            .topic_name = topic,
            .parser_encoding = kEncoding,
            .type_name = kTypeName,
            .schema = Span<const uint8_t>{},
            .parser_config_json = kParserConfig,
        });
  }

  /// bind() for the tests that only care about the handle: a failure fails the
  /// test rather than being handed back.
  [[nodiscard]] ParserBindingHandle bindTopic(const std::string& topic) {
    auto binding = bind(topic);
    EXPECT_TRUE(binding.has_value()) << binding.error();
    return binding.has_value() ? *binding : ParserBindingHandle{};
  }

  void push(ParserBindingHandle binding, Timestamp timestamp, std::vector<uint8_t> payload) {
    auto status = runtime().pushMessage(binding, timestamp, [payload]() -> std::vector<uint8_t> { return payload; });
    ASSERT_TRUE(status.has_value()) << status.error();
  }
};

/// The fixture's own HostBox over a hermetic catalog and a fresh engine.
/// Derived fixtures pass the source id the host reports and may extend SetUp —
/// call this one first.
class RuntimeHostFixture : public ::testing::Test, protected HostBox {
 protected:
  explicit RuntimeHostFixture(std::string source_id) : source_id_(std::move(source_id)) {}

  void SetUp() override {
    ASSERT_NE(catalog_.findParserByEncoding(QString::fromLatin1(kEncoding)), nullptr);
    open(engine_, catalog_, object_store_, ingest_taps_, "test", source_id_);
    ASSERT_NE(host, nullptr);
  }

  /// The host points into the engine, object store and catalog declared below
  /// it, and as a base subobject it would otherwise be destroyed after them.
  /// A fixture that adds its own TearDown must chain to this one.
  void TearDown() override {
    host.reset();
  }

  /// Objects stored for `topic`, flushed first so a pending batch counts.
  [[nodiscard]] uint64_t objectEntryCount(const std::string& topic) {
    host->flushAll();
    auto topic_id = object_store_.findTopic(dataset_id, topic);
    EXPECT_TRUE(topic_id.has_value()) << topic;
    return topic_id.has_value() ? object_store_.entryCount(*topic_id) : 0;
  }

  std::string source_id_;
  QFileInfo plugin_file_{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  HermeticCatalog catalog_box_{plugin_file_.absolutePath()};
  ExtensionCatalogService& catalog_{catalog_box_.service};
  DataEngine engine_;
  ObjectStore object_store_;
  std::shared_ptr<ObjectIngestTapRegistry> ingest_taps_ = std::make_shared<ObjectIngestTapRegistry>();
};

}  // namespace PJ::test
