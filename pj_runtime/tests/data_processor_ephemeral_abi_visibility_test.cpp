// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Pins an empirical claim about the ephemeral-transform preview path: a
// transform created with the EPHEMERAL flag is hidden from the GUI
// CatalogModel and from DataProcessorService::transformRecipes(), but IS
// visible through the plugin-facing ABI catalog snapshot (which enumerates
// straight from the DataEngine, unlike CatalogModel's exclusion filter) and
// its samples are readable through the ABI's read_series_arrow.

#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/settings_store_host.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/ToolboxRuntimeHost.h"

namespace PJ {
namespace {

constexpr const char* kNegate = R"LUAU(-- pj-script: luau
return { id="negate", name="Negate", output="same",
  create = function(p) return { calculate = function(t, v) return -v end } end }
)LUAU";

TEST(EphemeralTransformAbiVisibilityTest, EphemeralTransformVisibleToAbiNotToGui) {
  // A scoped extensions dir keeps this headless AppSession from scanning the
  // real (possibly plugin-laden) user extensions directory.
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  AppSession app_session(extensions_dir.path());
  SessionManager& session = app_session.sessionManager();
  DataEngine& engine = session.dataEngine();
  DataProcessorService& service = session.dataProcessorService();

  const auto ds = engine.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});
  ASSERT_TRUE(ds.has_value()) << ds.error();

  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(*ds, "x", NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  writer.appendScalar(*handle, 0, 2.0);
  writer.appendScalar(*handle, 1'000'000'000LL, 4.0);
  ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
  app_session.catalogModel().rebuildFromDatastore();

  int catalog_mutations = 0;
  QObject::connect(&app_session.catalogModel(), &CatalogModel::itemsAdded, [&catalog_mutations](const auto&) {
    ++catalog_mutations;
  });
  QObject::connect(&app_session.catalogModel(), &CatalogModel::itemsRemoved, [&catalog_mutations](const auto&) {
    ++catalog_mutations;
  });
  QObject::connect(
      &app_session.catalogModel(), &CatalogModel::cleared, [&catalog_mutations]() { ++catalog_mutations; });

  // Create the ephemeral transform through DataProcessorService directly because
  // DataProcessorsRuntimeHost is not independently wired in this fixture.
  // Same install path (installTransform) and same `ephemeral` recipe bit a
  // plugin's `PJ_DATA_PROCESSOR_FLAG_EPHEMERAL` create() call would produce.
  const auto created =
      service.upsertTransform("abi-visibility-test", "negate", {"x"}, {"x_negated"}, kNegate, "{}", /*ephemeral=*/true);
  ASSERT_TRUE(created.has_value()) << created.error();
  ASSERT_EQ(created->outputs.size(), 1u);
  ASSERT_EQ(created->output_topic_ids.size(), 1u);
  const std::string output_name = created->outputs.front();  // the resolved output name
  EXPECT_EQ(catalog_mutations, 0);
  app_session.catalogModel().rebuildFromDatastore();
  catalog_mutations = 0;  // exclude the explicit validation rebuild below

  // The plugin-facing ABI surface: a ToolboxRuntimeHost over the SAME engine +
  // object store, exactly as a toolbox plugin's bind() would receive.
  sdk::InMemorySettingsBackend settings;
  ToolboxRuntimeHost toolbox_host(engine, session.objectStore(), settings, ToolboxRuntimeHost::Callbacks{});
  ServiceRegistryBuilder builder;
  ASSERT_TRUE(toolbox_host.registerServices(builder).has_value());
  sdk::ServiceRegistry registry(builder.view());
  auto toolbox_or = registry.require<sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  sdk::ToolboxHostView toolbox(*toolbox_or);

  // (1) the ABI catalog snapshot lists a field whose topic is the ephemeral output.
  auto snapshot = toolbox.catalogSnapshot();
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error();
  std::optional<sdk::FieldHandle> field;
  for (const auto& topic : snapshot->topics()) {
    if (sdk::toStringView(topic.name) == output_name) {
      ASSERT_GT(topic.field_count, 0u);
      field = snapshot->fields()[topic.first_field].handle;
      break;
    }
  }
  ASSERT_TRUE(field.has_value()) << "ephemeral output '" << output_name << "' missing from the ABI catalog snapshot";

  // (2) readSeries(handle) on it returns the negated samples.
  auto series = toolbox.readSeries(*field);
  ASSERT_TRUE(series.has_value()) << series.error();
  ASSERT_EQ(series->rowCount(), 2u);
  const double* values = series->valuesAsFloat64();
  ASSERT_NE(values, nullptr);
  EXPECT_DOUBLE_EQ(values[0], -2.0);
  EXPECT_DOUBLE_EQ(values[1], -4.0);

  // (3) the GUI CatalogModel::items() does NOT list it.
  const std::vector<CatalogItem> items = app_session.catalogModel().items();
  const bool listed_in_gui = std::any_of(items.begin(), items.end(), [&](const CatalogItem& item) {
    return item.topic_name.toStdString() == output_name;
  });
  EXPECT_FALSE(listed_in_gui);

  // (4) DataProcessorService::transformRecipes() does NOT list it.
  const std::vector<DataProcessorService::TransformRecipe> recipes = service.transformRecipes();
  EXPECT_TRUE(std::none_of(recipes.begin(), recipes.end(), [&](const DataProcessorService::TransformRecipe& recipe) {
    return recipe.key == created->key;
  }));

  EXPECT_FALSE(service.transformOwning(created->output_topic_ids.front()).has_value());

  // (5) after remove, the ABI snapshot no longer lists it.
  ASSERT_TRUE(service.removeTransform(created->key).has_value());
  EXPECT_EQ(catalog_mutations, 0);
  auto snapshot_after = toolbox.catalogSnapshot();
  ASSERT_TRUE(snapshot_after.has_value()) << snapshot_after.error();
  const bool still_listed = std::any_of(
      snapshot_after->topics().begin(), snapshot_after->topics().end(),
      [&](const auto& topic) { return sdk::toStringView(topic.name) == output_name; });
  EXPECT_FALSE(still_listed);
}

// An exporter (any toolbox snapshot read through read_series_arrow) never sees a
// transform output that is still replaying in slices: the read finishes that
// replay first. Reading an unrelated series leaves the replay to the event loop.
// A replay failing after its create returned reaches the session's diagnostics.
TEST(TimeSlicedTransformAbiTest, SnapshotReadFinishesThePendingReplayAndFailuresReachDiagnostics) {
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  std::vector<Diagnostic> reported;
  AppSession app_session(extensions_dir.path(), [&reported](const Diagnostic& d) {
    if (d.source == "Data Processors") {
      reported.push_back(d);
    }
  });
  SessionManager& session = app_session.sessionManager();
  DataEngine& engine = session.dataEngine();
  DataProcessorService& service = session.dataProcessorService();

  constexpr std::size_t kRows = 200'000;
  const auto ds = engine.createDataset(DatasetDescriptor{.source_name = "s", .time_domain_id = 0});
  ASSERT_TRUE(ds.has_value()) << ds.error();
  DataWriter writer = engine.createWriter();
  auto handle = writer.registerScalarSeries(*ds, "x", NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  for (std::size_t i = 0; i < kRows; ++i) {
    writer.appendScalar(*handle, static_cast<Timestamp>(i) * 1'000'000LL, 1.0);
  }
  ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());

  const auto created = service.upsertTransform("exporter-test", "negate", {"x"}, {"x_negated"}, kNegate, "{}");
  ASSERT_TRUE(created.has_value()) << created.error();
  ASSERT_TRUE(service.materializationPending());

  sdk::InMemorySettingsBackend settings;
  ToolboxRuntimeHost toolbox_host(engine, session.objectStore(), settings, ToolboxRuntimeHost::Callbacks{});
  ServiceRegistryBuilder builder;
  ASSERT_TRUE(toolbox_host.registerServices(builder).has_value());
  sdk::ServiceRegistry registry(builder.view());
  auto toolbox_or = registry.require<sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  sdk::ToolboxHostView toolbox(*toolbox_or);
  auto snapshot = toolbox.catalogSnapshot();
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error();
  std::optional<sdk::FieldHandle> source_field;
  std::optional<sdk::FieldHandle> output_field;
  for (const auto& topic : snapshot->topics()) {
    if (sdk::toStringView(topic.name) == "x") {
      source_field = snapshot->fields()[topic.first_field].handle;
    } else if (sdk::toStringView(topic.name) == "x_negated") {
      output_field = snapshot->fields()[topic.first_field].handle;
    }
  }
  ASSERT_TRUE(source_field.has_value());
  ASSERT_TRUE(output_field.has_value());

  auto source = toolbox.readSeries(*source_field);
  ASSERT_TRUE(source.has_value()) << source.error();
  EXPECT_EQ(source->rowCount(), kRows);
  EXPECT_TRUE(service.materializationPending());

  auto exported = toolbox.readSeries(*output_field);
  ASSERT_TRUE(exported.has_value()) << exported.error();
  EXPECT_FALSE(service.materializationPending());
  ASSERT_EQ(exported->rowCount(), kRows);
  const double* values = exported->valuesAsFloat64();
  ASSERT_NE(values, nullptr);
  EXPECT_DOUBLE_EQ(values[0], -1.0);
  EXPECT_DOUBLE_EQ(values[kRows - 1], -1.0);

  // The session wires late replay failures to its diagnostic sink.
  const std::string late_boom = R"LUAU(-- pj-script: luau
return { id="late", name="Late", output="same",
  create = function(p) local n = 0 return { calculate = function(t, v)
    n = n + 1 if n > 199000 then error("late boom") end return v end } end }
)LUAU";
  const auto failing = service.upsertTransform("exporter-test", "late", {"x"}, {"x_late"}, late_boom, "{}");
  ASSERT_TRUE(failing.has_value()) << failing.error();
  ASSERT_TRUE(service.materializationPending());
  while (!service.stepMaterialization()) {}
  ASSERT_EQ(reported.size(), 1u);
  EXPECT_EQ(reported.front().level, DiagnosticLevel::kError);
  EXPECT_EQ(reported.front().id, "exporter-test/late");
}

}  // namespace
}  // namespace PJ
