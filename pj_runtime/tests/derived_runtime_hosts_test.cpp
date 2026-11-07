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

#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/builtin/point_cloud.hpp"
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
#include "pj_runtime/OnDemandService.h"
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

// The on-demand recipe `id` through `view`, reading the cloud topic `cloud`.
sdk::DataProcessorRequest onDemand(std::string id, uint32_t flags = 0) {
  sdk::DataProcessorRequest made;
  made.id = std::move(id);
  made.kind = "on_demand";
  made.language = "luau";
  made.script = "local inputs = ...\nreturn { n = inputs.cloud:count() }\n";
  made.inputs = {"cloud"};
  made.outputs = {{"n", "number"}};
  made.flags = flags;
  return made;
}

/// A session with the dataset `run.mcap`: the scalar series `/sensor` and the point-cloud topic `cloud`.
struct Session {
  Session() {
    dataset = *session.dataEngine().createDataset(DatasetDescriptor{.source_name = "run.mcap"});
    DataWriter writer = session.dataEngine().createWriter();
    auto handle = writer.registerScalarSeries(dataset, "/sensor", NumericType::kFloat64);
    EXPECT_TRUE(handle.has_value());
    writer.appendScalar(*handle, 100, 1.0);
    EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
    const ObjectTopicId cloud =
        session.objectStore().registerTopic({dataset, "cloud", R"({"builtin_object_type":"kPointCloud"})"}).value();
    sdk::PointCloud points;
    EXPECT_TRUE(session.objectStore()
                    .pushOwned(cloud, 100, serializeBuiltinObject(sdk::BuiltinObject(std::move(points))).value())
                    .has_value());
    app.catalogModel().rebuildFromDatastore();
  }
  [[nodiscard]] const RecipeGraph::Node* onDemandNode(const std::string& key) const {
    return session.onDemandService().graph().find(key);
  }
  /// Whether a preview with the local id `id` of plugin `tool` is installed, whichever instance made it.
  [[nodiscard]] bool hasPreview(std::string_view id) const {
    const RecipeGraph& graph = session.onDemandService().graph();
    return std::ranges::any_of(graph.keys(), [&](const std::string& key) {
      return key.starts_with("tool/~preview/") && key.ends_with("/" + std::string(id)) &&
             graph.find(key)->recipe.ephemeral;
    });
  }

  QTemporaryDir extensions_dir;
  AppSession app{extensions_dir.path()};
  SessionManager& session = app.sessionManager();
  DatasetId dataset = 0;
};

// Two instances of one plugin (a toolbox panel and a headless binding, say): ending
// one ends the previews it created and still holds, in both engines. Another
// instance's previews stay, including one reusing an id the ended instance removed
// itself, and so do the plugin's persistent recipes.
TEST(DerivedRuntimeHostsTest, EndingOneInstanceKeepsAnotherInstancesPreviews) {
  Session fx;
  SessionManager& session = fx.session;
  AppSession& app = fx.app;

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
  ASSERT_TRUE(panel_view.createV2(onDemand("panel_probe", PJ_DATA_PROCESSOR_FLAG_EPHEMERAL)).has_value());
  ASSERT_TRUE(headless_view.createV2(onDemand("headless_probe", PJ_DATA_PROCESSOR_FLAG_EPHEMERAL)).has_value());
  ASSERT_TRUE(panel_view.createV2(onDemand("shared_probe", PJ_DATA_PROCESSOR_FLAG_EPHEMERAL)).has_value());
  ASSERT_TRUE(headless_view.createV2(onDemand("shared_probe", PJ_DATA_PROCESSOR_FLAG_EPHEMERAL)).has_value());
  ASSERT_TRUE(panel_view.createV2(onDemand("kept_probe")).has_value());

  panel.reset();

  std::vector<std::string> transforms = session.dataProcessorService().transformIdsForPlugin("tool");
  std::sort(transforms.begin(), transforms.end());
  EXPECT_EQ(transforms, (std::vector<std::string>{"headless_preview", "kept", "reused"}));
  const BatchEngine& markers = session.batchEngine();
  EXPECT_EQ(markers.firstBinding("tool/panel_rule"), nullptr);
  EXPECT_NE(markers.firstBinding("tool/headless_rule"), nullptr);
  EXPECT_NE(markers.firstBinding("tool/kept_rule"), nullptr);
  EXPECT_FALSE(fx.hasPreview("panel_probe"));
  EXPECT_TRUE(fx.hasPreview("headless_probe"));
  EXPECT_TRUE(fx.hasPreview("shared_probe")) << "one id in two instances is two previews";
  const auto shared = headless_view.recipeOf("shared_probe");
  ASSERT_TRUE(shared.has_value()) << shared.error();
  EXPECT_NE(fx.onDemandNode("tool/kept_probe"), nullptr);
}

// OWN-05: a plugin removes its own preview of any kind through the kind-less remove of the real router.
TEST(DataProcessorsKindRouterRealBackendsTest, RemovesAnEphemeralOfEveryKind) {
  Session fx;
  DerivedRuntimeHosts hosts(fx.session, fx.app.catalogModel(), "tool");
  ServiceRegistryBuilder services;
  const sdk::DataProcessorsHostView view = bind(hosts, services);
  createTransformPreview(view, "transform_probe");
  createMarkerPreview(view, "marker_probe");
  const auto on_demand_topics = view.createV2(onDemand("on_demand_probe", PJ_DATA_PROCESSOR_FLAG_EPHEMERAL));
  ASSERT_TRUE(on_demand_topics.has_value()) << on_demand_topics.error();
  ASSERT_TRUE(fx.hasPreview("on_demand_probe"));

  for (const char* id : {"transform_probe", "marker_probe", "on_demand_probe"}) {
    EXPECT_TRUE(view.remove(id).has_value()) << id;
    EXPECT_FALSE(view.remove(id).has_value()) << id;
  }
  EXPECT_TRUE(fx.session.dataProcessorService().transformIdsForPlugin("tool").empty());
  EXPECT_EQ(fx.session.batchEngine().firstBinding("tool/marker_probe"), nullptr);
  EXPECT_FALSE(fx.hasPreview("on_demand_probe"));
  EXPECT_FALSE(fx.session.objectStore().findTopic(fx.dataset, (*on_demand_topics)[0]).has_value());
}

// OWN-06: list hides an on-demand preview; config by its id answers its owner; an absent id is refused.
TEST(DataProcessorsKindRouterRealBackendsTest, ListHidesEphemeralRecipesButConfigAnswersTheOwner) {
  Session fx;
  DerivedRuntimeHosts hosts(fx.session, fx.app.catalogModel(), "tool");
  ServiceRegistryBuilder services;
  const sdk::DataProcessorsHostView view = bind(hosts, services);
  ASSERT_TRUE(view.createV2(onDemand("probe", PJ_DATA_PROCESSOR_FLAG_EPHEMERAL)).has_value());
  ASSERT_TRUE(view.createV2(onDemand("kept")).has_value());
  EXPECT_EQ(*view.list(), std::vector<std::string>{"kept"});
  const auto config = view.recipeOf("probe");
  ASSERT_TRUE(config.has_value()) << config.error();
  EXPECT_NE(config->find("\"kind\":\"on_demand\""), std::string::npos) << *config;
  EXPECT_FALSE(view.recipeOf("absent").has_value());
}

}  // namespace
}  // namespace PJ
