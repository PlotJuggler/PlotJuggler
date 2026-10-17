// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The derived runtime hosts of one plugin binding: a preview belongs to the
// instance that created it, not to its plugin id.

#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/plugin_data_api.h"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/DerivedRuntimeHosts.h"
#include "pj_runtime/SessionManager.h"

namespace PJ {
namespace {

// A transform class whose id is `id`, passing its input through.
std::string passThrough(std::string_view id) {
  const std::string name(id);
  return "-- pj-script: luau\nreturn { id=\"" + name + "\", name=\"" + name +
         "\", output=\"same\",\n"
         "  create = function(p) return { calculate = function(t, v) return v end } end }\n";
}

constexpr std::string_view kMarkerScript = "local s = series(\"/sensor/value\")\ncreateMarker(s:at(0).t)\n";

// The routed pj.data_processors.v1 a plugin bound to `hosts` would get. It refuses
// one id for two kinds, so a transform and a marker rule never share one.
sdk::DataProcessorsHostView bind(DerivedRuntimeHosts& hosts, ServiceRegistryBuilder& builder) {
  EXPECT_TRUE(hosts.registerServices(builder).has_value());
  const sdk::ServiceRegistry registry(builder.view());
  auto view = registry.require<sdk::DataProcessorsHostService>();
  EXPECT_TRUE(view.has_value());
  return view.has_value() ? *view : sdk::DataProcessorsHostView{};
}

// An ephemeral transform `id` through `view`, reading /sensor.
void createTransformPreview(const sdk::DataProcessorsHostView& view, std::string_view id) {
  const std::vector<std::string_view> inputs{"/sensor"};
  const std::string output = "out_" + std::string(id);
  const std::vector<std::string_view> outputs{output};
  EXPECT_TRUE(view.createEphemeralTransform(id, inputs, outputs, passThrough(id), "{}").has_value());
}

// An ephemeral marker rule `id` through `view`, reading /sensor/value.
void createMarkerPreview(const sdk::DataProcessorsHostView& view, std::string_view id) {
  const std::vector<std::string_view> inputs{"/sensor/value"};
  const auto created = view.createMarkers(id, inputs, "", kMarkerScript, "{}", PJ_DATA_PROCESSOR_FLAG_EPHEMERAL);
  EXPECT_TRUE(created.has_value()) << created.error();
}

// Two instances of one plugin (a toolbox panel and a headless binding, say): ending
// one ends the previews it created and still holds, in both engines. Another
// instance's previews stay, including one reusing an id the ended instance removed
// itself, and so do the plugin's persistent recipes.
TEST(DerivedRuntimeHostsTest, EndingOneInstanceKeepsAnotherInstancesPreviews) {
  QTemporaryDir extensions_dir;
  AppSession app(extensions_dir.path());
  SessionManager& session = app.sessionManager();
  const DatasetId dataset = *session.dataEngine().createDataset(DatasetDescriptor{.source_name = "run.mcap"});
  DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset, "/sensor", NumericType::kFloat64);
  ASSERT_TRUE(handle.has_value());
  writer.appendScalar(*handle, 100, 1.0);
  ASSERT_FALSE(session.commitChunks(writer.flushAll()).empty());
  app.catalogModel().rebuildFromDatastore();

  auto panel = std::make_unique<DerivedRuntimeHosts>(session, app.catalogModel(), "tool");
  DerivedRuntimeHosts headless(session, app.catalogModel(), "tool");
  ServiceRegistryBuilder panel_services;
  ServiceRegistryBuilder headless_services;
  const sdk::DataProcessorsHostView panel_view = bind(*panel, panel_services);
  const sdk::DataProcessorsHostView headless_view = bind(headless, headless_services);

  createTransformPreview(panel_view, "panel_preview");
  createMarkerPreview(panel_view, "panel_rule");
  createTransformPreview(panel_view, "reused");
  ASSERT_TRUE(panel_view.remove("reused").has_value());
  createTransformPreview(headless_view, "headless_preview");
  createMarkerPreview(headless_view, "headless_rule");
  createTransformPreview(headless_view, "reused");
  const std::vector<std::string_view> inputs{"/sensor"};
  const std::vector<std::string_view> outputs{"out_kept"};
  ASSERT_TRUE(panel_view.createTransform("kept", inputs, outputs, passThrough("kept"), "{}").has_value());
  const std::vector<std::string_view> marker_inputs{"/sensor/value"};
  ASSERT_TRUE(panel_view.createMarkers("kept_rule", marker_inputs, "__global__", kMarkerScript, "{}").has_value());

  panel.reset();

  std::vector<std::string> transforms = session.dataProcessorService().transformIdsForPlugin("tool");
  std::sort(transforms.begin(), transforms.end());
  EXPECT_EQ(transforms, (std::vector<std::string>{"headless_preview", "kept", "reused"}));
  const BatchEngine& markers = session.batchEngine();
  EXPECT_EQ(markers.firstBinding("tool/panel_rule"), nullptr);
  EXPECT_NE(markers.firstBinding("tool/headless_rule"), nullptr);
  EXPECT_NE(markers.firstBinding("tool/kept_rule"), nullptr);
}

}  // namespace
}  // namespace PJ
