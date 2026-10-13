// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// DataProcessorsKindRouter fans ONE pj.data_processors.v1 registration out to two
// backends (markers vs transforms) by the create-time `kind`, and — since the wire
// carries no kind on remove/config/list — routes those to whichever backend owns the
// id (probed via config). These tests drive it against two fake backends, proving
// cross-kind id collisions are rejected at create and that kind-less ops reach the
// right backend.
//
// Note what is deliberately NOT guaranteed: `list` does not de-duplicate. Prevention
// lives at create; filtering the output would report one processor where two exist and
// leave the second unreachable after a remove. The back-door case below pins that.

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/plugin_data_api.h"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DataProcessorsKindRouter.h"

namespace {

using PJ::DataProcessorsKindRouter;

std::string_view sv(PJ_string_view_t v) {
  return {v.data, v.size};
}

// A minimal in-memory data-processors backend: it just tracks the set of live ids so
// the router's routing can be observed.
struct FakeBackend {
  std::set<std::string> ids;

  static bool create(
      void* ctx, PJ_string_view_t id, PJ_string_view_t, PJ_string_view_t, const PJ_string_view_t*, uint64_t,
      const PJ_string_view_t*, uint64_t, PJ_string_view_t, PJ_string_view_t, uint32_t, PJ_string_view_t*, uint64_t,
      uint64_t* out_topics_count, PJ_error_t*) noexcept {
    static_cast<FakeBackend*>(ctx)->ids.insert(std::string(sv(id)));
    if (out_topics_count != nullptr) {
      *out_topics_count = 0;
    }
    return true;
  }
  static bool remove(void* ctx, PJ_string_view_t id, PJ_error_t*) noexcept {
    return static_cast<FakeBackend*>(ctx)->ids.erase(std::string(sv(id))) > 0;
  }
  static bool list(void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count, PJ_error_t*) noexcept {
    auto& ids = static_cast<FakeBackend*>(ctx)->ids;
    if (out_count != nullptr) {
      *out_count = ids.size();
    }
    if (out_ids != nullptr) {
      uint64_t i = 0;
      for (const std::string& s : ids) {
        if (i >= capacity) {
          break;
        }
        out_ids[i++] = PJ_string_view_t{s.data(), s.size()};
      }
    }
    return true;
  }
  static bool config(void* ctx, PJ_string_view_t id, PJ_string_view_t*, PJ_error_t*) noexcept {
    return static_cast<FakeBackend*>(ctx)->ids.count(std::string(sv(id))) > 0;
  }
  static bool validate(
      void*, PJ_string_view_t, PJ_string_view_t, PJ_string_view_t, PJ_string_view_t, PJ_error_t*) noexcept {
    return true;
  }

  PJ_data_processors_host_t host() {
    static const PJ_data_processors_host_vtable_t vtable = {
        .protocol_version = 1,
        .struct_size = sizeof(PJ_data_processors_host_vtable_t),
        .create_data_processor = &FakeBackend::create,
        .remove_data_processor = &FakeBackend::remove,
        .list_data_processor_ids = &FakeBackend::list,
        .data_processor_config = &FakeBackend::config,
        .validate_data_processor_script = &FakeBackend::validate,
    };
    return PJ_data_processors_host_t{this, &vtable};
  }
};

PJ_string_view_t s(std::string_view v) {
  return PJ_string_view_t{v.data(), v.size()};
}

// Drive create through the router's public vtable.
bool routerCreate(const PJ_data_processors_host_t& router, std::string_view id, std::string_view kind) {
  uint64_t topics = 0;
  PJ_error_t err{};
  return router.vtable->create_data_processor(
      router.ctx, s(id), s(kind), s("luau"), nullptr, 0, nullptr, 0, s(""), s("{}"), 0, nullptr, 0, &topics, &err);
}

TEST(DataProcessorsKindRouterTest, CreateRoutesByKind) {
  FakeBackend markers;
  FakeBackend transforms;
  DataProcessorsKindRouter router(markers.host(), transforms.host());
  const PJ_data_processors_host_t r = router.raw();

  EXPECT_TRUE(routerCreate(r, "m1", "markers"));
  EXPECT_TRUE(routerCreate(r, "t1", "transform"));
  EXPECT_EQ(markers.ids, (std::set<std::string>{"m1"}));
  EXPECT_EQ(transforms.ids, (std::set<std::string>{"t1"}));
}

TEST(DataProcessorsKindRouterTest, RejectsCrossKindIdCollision) {
  FakeBackend markers;
  FakeBackend transforms;
  DataProcessorsKindRouter router(markers.host(), transforms.host());
  const PJ_data_processors_host_t r = router.raw();

  EXPECT_TRUE(routerCreate(r, "shared", "markers"));
  // Same id on the other backend is rejected: it would spawn a second node and make
  // kind-less remove/config ambiguous.
  EXPECT_FALSE(routerCreate(r, "shared", "transform"));
  EXPECT_EQ(transforms.ids.count("shared"), 0u);  // transform backend untouched
}

TEST(DataProcessorsKindRouterTest, RemoveRoutesToOwningBackend) {
  FakeBackend markers;
  FakeBackend transforms;
  DataProcessorsKindRouter router(markers.host(), transforms.host());
  const PJ_data_processors_host_t r = router.raw();

  ASSERT_TRUE(routerCreate(r, "m1", "markers"));
  PJ_error_t err{};
  EXPECT_TRUE(r.vtable->remove_data_processor(r.ctx, s("m1"), &err));
  EXPECT_EQ(markers.ids.count("m1"), 0u);  // removed from the markers backend

  // An unknown id is a clean error, not a probe that swallows the wrong backend's.
  err = PJ_error_t{};
  EXPECT_FALSE(r.vtable->remove_data_processor(r.ctx, s("nope"), &err));
  EXPECT_NE(std::string_view(err.message).find("unknown"), std::string_view::npos);
}

// Read the router's id list.
std::vector<std::string> routerList(const PJ_data_processors_host_t& r) {
  uint64_t count = 0;
  PJ_error_t err{};
  if (!r.vtable->list_data_processor_ids(r.ctx, nullptr, 0, &count, &err)) {
    return {};
  }
  std::vector<PJ_string_view_t> views(count);
  if (count > 0 && !r.vtable->list_data_processor_ids(r.ctx, views.data(), count, &count, &err)) {
    return {};
  }
  std::vector<std::string> ids;
  ids.reserve(count);
  for (uint64_t i = 0; i < count; ++i) {
    ids.emplace_back(sv(views[i]));
  }
  return ids;
}

TEST(DataProcessorsKindRouterTest, ListUnionsBothBackends) {
  FakeBackend markers;
  FakeBackend transforms;
  DataProcessorsKindRouter router(markers.host(), transforms.host());
  const PJ_data_processors_host_t r = router.raw();

  ASSERT_TRUE(routerCreate(r, "m1", "markers"));
  ASSERT_TRUE(routerCreate(r, "t1", "transform"));

  const std::vector<std::string> ids = routerList(r);
  EXPECT_EQ(std::set<std::string>(ids.begin(), ids.end()), (std::set<std::string>{"m1", "t1"}));
}

// Layout restore writes to BatchEngine and DataProcessorService directly, bypassing
// the router, so onCreate's cross-kind rejection never sees a hand-edited layout that
// carries one id as both a <transform> and a <generator>. This pins what the router
// does with that state: the id is listed twice, and kind-less ops resolve to the
// transform (probed first), leaving the marker generator unreachable through this API.
TEST(DataProcessorsKindRouterTest, ListSurfacesDuplicateIdsCreatedBehindTheRouter) {
  FakeBackend markers;
  FakeBackend transforms;
  DataProcessorsKindRouter router(markers.host(), transforms.host());
  const PJ_data_processors_host_t r = router.raw();

  // The back door: both backends written straight, as a layout restore does.
  markers.ids.insert("shared");
  transforms.ids.insert("shared");

  const std::vector<std::string> ids = routerList(r);
  EXPECT_EQ(ids.size(), 2u) << "the duplicate is surfaced, not filtered away";
  EXPECT_EQ(std::count(ids.begin(), ids.end(), "shared"), 2);

  // Remove reaches the transform side only; the marker generator survives.
  PJ_error_t err{};
  EXPECT_TRUE(r.vtable->remove_data_processor(r.ctx, s("shared"), &err));
  EXPECT_TRUE(transforms.ids.empty());
  EXPECT_EQ(markers.ids.count("shared"), 1u);
}

// The router exists precisely because pj.data_processors.v1 can be registered only
// once (MainWindow routes both bridges through it for that reason). So a name that
// is already taken must fail registration and name the cause — silently dropping it
// would leave the whole kind's toolboxes wired to someone else's backend.
TEST(DataProcessorsKindRouterTest, RegisterServicesFailsWhenTheServiceNameIsTaken) {
  FakeBackend markers;
  FakeBackend transforms;
  DataProcessorsKindRouter router(markers.host(), transforms.host());

  PJ::ServiceRegistryBuilder builder;
  ASSERT_TRUE(router.registerServices(builder).has_value());
  ASSERT_EQ(builder.size(), 1u);

  const PJ::Status status = router.registerServices(builder);

  ASSERT_FALSE(status.has_value());
  EXPECT_NE(status.error().find(PJ::sdk::DataProcessorsHostService::kName), std::string::npos) << status.error();
  EXPECT_NE(status.error().find("duplicate name"), std::string::npos) << status.error();
}

}  // namespace
