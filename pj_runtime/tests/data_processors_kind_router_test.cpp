// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// DataProcessorsKindRouter fans ONE pj.data_processors.v1 registration out to three
// backends (markers, transforms, on_demand) by the create-time `kind`, and — since the
// wire carries no kind on remove/config/list — routes those to whichever backend owns
// the id (its ownership probe). These tests drive it against fake backends, proving
// cross-kind id collisions are rejected at create, kind-less ops reach the right
// backend, typed requests are decoded before any field is read, and evaluation handles
// live in the router's own namespace.
//
// Note what is deliberately NOT guaranteed: `list` does not de-duplicate. Prevention
// lives at create; filtering the output would report one processor where two exist and
// leave the second unreachable after a remove. The back-door case below pins that.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/plugin_data_api.h"
#include "pj_base/sdk/data_processors_host.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DataProcessorsKindRouter.h"

namespace {

using PJ::DataProcessorsKindRouter;

std::string_view sv(PJ_string_view_t v) {
  return {v.data, v.size};
}

PJ_string_view_t s(std::string_view v) {
  return PJ_string_view_t{v.data(), v.size()};
}

// An in-memory backend: it tracks its live ids so the routing can be observed. A
// preview is owned but hidden from list and config, as the markers bridge hides it.
// `evaluates` gives it the submit/poll/release slots and the three-way check (the on-demand
// backend's shape); without it, it has create_v2 only, whatever the real transform and markers
// bridges fill, so the router's fallback for a backend without the check is exercised.
struct FakeBackend {
  std::set<std::string> ids;
  std::set<std::string> previews;
  // When set, create() rejects any other kind, as a real bridge does.
  std::optional<std::string> accepted_kind;
  bool evaluates = false;
  int calls = 0;  ///< every vtable call and ownership probe
  int config_calls = 0;
  bool validate_called = false;
  std::string validate_error;  ///< when set, validate() refuses with it
  bool check_called = false;
  uint32_t check_status = PJ_SCRIPT_CHECK_UNCHECKED;  ///< what check() answers, with the message "fake: not decided"
  uint32_t last_v2_struct_size = 0;
  std::string last_v2_kind;
  std::map<uint64_t, std::string> evaluations;
  uint64_t next_eval_handle = 100;  ///< its own numbering, distinct from the router's

  [[nodiscard]] bool owns(std::string_view id) {
    ++calls;
    return ids.count(std::string(id)) > 0 || previews.count(std::string(id)) > 0;
  }

  static FakeBackend& self(void* ctx) {
    auto& backend = *static_cast<FakeBackend*>(ctx);
    ++backend.calls;
    return backend;
  }

  static bool create(
      void* ctx, PJ_string_view_t id, PJ_string_view_t kind, PJ_string_view_t, const PJ_string_view_t*, uint64_t,
      const PJ_string_view_t*, uint64_t, PJ_string_view_t, PJ_string_view_t, uint32_t flags, PJ_string_view_t*,
      uint64_t, uint64_t* out_topics_count, PJ_error_t* out_error) noexcept {
    FakeBackend& backend = self(ctx);
    if (backend.accepted_kind.has_value() && sv(kind) != *backend.accepted_kind) {
      PJ::sdk::fillError(out_error, PJ_ERROR_CODE_REJECTED, "fake_backend", "fake backend does not accept this kind");
      return false;
    }
    ((flags & PJ_DATA_PROCESSOR_FLAG_EPHEMERAL) != 0 ? backend.previews : backend.ids).insert(std::string(sv(id)));
    if (out_topics_count != nullptr) {
      *out_topics_count = 0;
    }
    return true;
  }
  static bool remove(void* ctx, PJ_string_view_t id, PJ_error_t*) noexcept {
    FakeBackend& backend = self(ctx);
    return backend.ids.erase(std::string(sv(id))) + backend.previews.erase(std::string(sv(id))) > 0;
  }
  static bool list(void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count, PJ_error_t*) noexcept {
    FakeBackend& backend = self(ctx);
    if (out_count != nullptr) {
      *out_count = backend.ids.size();
    }
    if (out_ids != nullptr) {
      uint64_t index = 0;
      for (const std::string& id : backend.ids) {
        if (index >= capacity) {
          break;
        }
        out_ids[index++] = s(id);
      }
    }
    return true;
  }
  static bool config(void* ctx, PJ_string_view_t id, PJ_string_view_t*, PJ_error_t*) noexcept {
    FakeBackend& backend = self(ctx);
    ++backend.config_calls;
    return backend.ids.count(std::string(sv(id))) > 0;
  }
  static bool validate(
      void* ctx, PJ_string_view_t, PJ_string_view_t, PJ_string_view_t, PJ_string_view_t,
      PJ_error_t* out_error) noexcept {
    FakeBackend& backend = self(ctx);
    backend.validate_called = true;
    if (!backend.validate_error.empty()) {
      PJ::sdk::fillError(out_error, PJ_ERROR_CODE_REJECTED, "fake_backend", backend.validate_error);
      return false;
    }
    return true;
  }
  static bool check(
      void* ctx, PJ_string_view_t, PJ_string_view_t, PJ_string_view_t, PJ_string_view_t, uint32_t* out_status,
      PJ_error_t* out_error) noexcept {
    FakeBackend& backend = self(ctx);
    backend.check_called = true;
    *out_status = backend.check_status;
    PJ::sdk::fillError(out_error, PJ_ERROR_CODE_REJECTED, "fake_backend", "fake: not decided");
    return true;
  }
  static bool createV2(
      void* ctx, const PJ_data_processor_request_t* request, PJ_string_view_t*, uint64_t, uint64_t* out_topics_count,
      PJ_error_t*) noexcept {
    FakeBackend& backend = self(ctx);
    backend.last_v2_struct_size = request->struct_size;
    backend.last_v2_kind = std::string(sv(request->kind));
    backend.ids.insert(std::string(sv(request->id)));
    if (out_topics_count != nullptr) {
      *out_topics_count = 0;
    }
    return true;
  }
  static bool submit(
      void* ctx, const PJ_data_processor_request_t* request, const PJ_evaluation_budget_t*, uint64_t* out_handle,
      PJ_error_t*) noexcept {
    FakeBackend& backend = self(ctx);
    const uint64_t handle = backend.next_eval_handle++;
    backend.evaluations[handle] = R"({"id":")" + std::string(sv(request->id)) + R"("})";
    *out_handle = handle;
    return true;
  }
  static bool poll(
      void* ctx, uint64_t handle, uint32_t* out_state, PJ_string_view_t* out_json, PJ_error_t* out_error) noexcept {
    FakeBackend& backend = self(ctx);
    const auto it = backend.evaluations.find(handle);
    if (it == backend.evaluations.end()) {
      PJ::sdk::fillError(out_error, PJ_ERROR_CODE_REJECTED, "fake_backend", "unknown handle");
      return false;
    }
    *out_state = PJ_EVALUATION_STATE_COMPLETED;
    *out_json = s(it->second);
    return true;
  }
  static bool release(void* ctx, uint64_t handle, PJ_error_t* out_error) noexcept {
    FakeBackend& backend = self(ctx);
    if (backend.evaluations.erase(handle) == 0) {
      PJ::sdk::fillError(out_error, PJ_ERROR_CODE_REJECTED, "fake_backend", "unknown handle");
      return false;
    }
    return true;
  }

  PJ_data_processors_host_t host() {
    static const PJ_data_processors_host_vtable_t kBridge = {
        .protocol_version = 1,
        .struct_size = sizeof(PJ_data_processors_host_vtable_t),
        .create_data_processor = &FakeBackend::create,
        .remove_data_processor = &FakeBackend::remove,
        .list_data_processor_ids = &FakeBackend::list,
        .data_processor_config = &FakeBackend::config,
        .validate_data_processor_script = &FakeBackend::validate,
        .create_data_processor_v2 = &FakeBackend::createV2,
    };
    static const PJ_data_processors_host_vtable_t kEvaluating = {
        .protocol_version = 1,
        .struct_size = sizeof(PJ_data_processors_host_vtable_t),
        .create_data_processor = &FakeBackend::create,
        .remove_data_processor = &FakeBackend::remove,
        .list_data_processor_ids = &FakeBackend::list,
        .data_processor_config = &FakeBackend::config,
        .validate_data_processor_script = &FakeBackend::validate,
        .create_data_processor_v2 = &FakeBackend::createV2,
        .submit_evaluation = &FakeBackend::submit,
        .poll_evaluation = &FakeBackend::poll,
        .release_evaluation = &FakeBackend::release,
        .check_data_processor_script = &FakeBackend::check,
    };
    return PJ_data_processors_host_t{this, evaluates ? &kEvaluating : &kBridge};
  }
};

// The three fakes and the router over them; `bind_on_demand = false` leaves the third
// backend unbound.
struct Fixture {
  FakeBackend markers;
  FakeBackend transforms;
  FakeBackend on_demand;
  DataProcessorsKindRouter router;

  explicit Fixture(bool bind_on_demand = true)
      : router(
            markers.host(), transforms.host(), bind_on_demand ? onDemandHost() : PJ_data_processors_host_t{},
            {.transforms = [this](std::string_view id) { return transforms.owns(id); },
             .markers = [this](std::string_view id) { return markers.owns(id); },
             .on_demand = [this](std::string_view id) { return on_demand.owns(id); }}) {}

  PJ_data_processors_host_t onDemandHost() {
    on_demand.evaluates = true;
    return on_demand.host();
  }
  [[nodiscard]] PJ_data_processors_host_t r() const {
    return router.raw();
  }
  [[nodiscard]] PJ::sdk::DataProcessorsHostView view() const {
    return PJ::sdk::DataProcessorsHostView(router.raw());
  }
  [[nodiscard]] int backendCalls() const {
    return markers.calls + transforms.calls + on_demand.calls;
  }
};

// Drive create through the router's public vtable.
bool routerCreate(
    const PJ_data_processors_host_t& router, std::string_view id, std::string_view kind, PJ_error_t* err = nullptr,
    uint32_t flags = 0) {
  uint64_t topics = 0;
  PJ_error_t local{};
  return router.vtable->create_data_processor(
      router.ctx, s(id), s(kind), s("luau"), nullptr, 0, nullptr, 0, s(""), s("{}"), flags, nullptr, 0, &topics,
      err != nullptr ? err : &local);
}

PJ::sdk::DataProcessorRequest typed(std::string_view id, std::string_view kind) {
  PJ::sdk::DataProcessorRequest request;
  request.id = std::string(id);
  request.kind = std::string(kind);
  request.language = "luau";
  request.script = "return 1";
  return request;
}

// A raw request with only the id and kind set, struct_size = sizeof.
PJ_data_processor_request_t rawRequest(std::string_view id, std::string_view kind) {
  PJ_data_processor_request_t request{};
  request.struct_size = sizeof(PJ_data_processor_request_t);
  request.id = s(id);
  request.kind = s(kind);
  request.language = s("luau");
  request.script = s("return 1");
  return request;
}

bool routerCreateV2(
    const PJ_data_processors_host_t& router, const PJ_data_processor_request_t& request, PJ_error_t& err) {
  uint64_t topics = 0;
  return router.vtable->create_data_processor_v2(router.ctx, &request, nullptr, 0, &topics, &err);
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

bool contains(const PJ_error_t& err, std::string_view needle) {
  return std::string_view(err.message).find(needle) != std::string_view::npos;
}

TEST(DataProcessorsKindRouterTest, CreateRoutesByKind) {
  Fixture fx;
  EXPECT_TRUE(routerCreate(fx.r(), "m1", "markers"));
  EXPECT_TRUE(routerCreate(fx.r(), "t1", "transform"));
  EXPECT_EQ(fx.markers.ids, (std::set<std::string>{"m1"}));
  EXPECT_EQ(fx.transforms.ids, (std::set<std::string>{"t1"}));
}

TEST(DataProcessorsKindRouterTest, RejectsCrossKindIdCollision) {
  Fixture fx;
  EXPECT_TRUE(routerCreate(fx.r(), "shared", "markers"));
  // Same id on the other backend is rejected: it would spawn a second node and make
  // kind-less remove/config ambiguous.
  EXPECT_FALSE(routerCreate(fx.r(), "shared", "transform"));
  EXPECT_EQ(fx.transforms.ids.count("shared"), 0u);
}

TEST(DataProcessorsKindRouterTest, RemoveRoutesToOwningBackend) {
  Fixture fx;
  ASSERT_TRUE(routerCreate(fx.r(), "m1", "markers"));
  PJ_error_t err{};
  EXPECT_TRUE(fx.r().vtable->remove_data_processor(fx.r().ctx, s("m1"), &err));
  EXPECT_EQ(fx.markers.ids.count("m1"), 0u);

  // An unknown id is a clean error, not a probe that swallows the wrong backend's.
  err = PJ_error_t{};
  EXPECT_FALSE(fx.r().vtable->remove_data_processor(fx.r().ctx, s("nope"), &err));
  EXPECT_TRUE(contains(err, "unknown")) << err.message;

  // A preview is owned even though its backend's config hides it, so it can be removed.
  ASSERT_TRUE(routerCreate(fx.r(), "preview", "markers", nullptr, PJ_DATA_PROCESSOR_FLAG_EPHEMERAL));
  EXPECT_TRUE(fx.r().vtable->remove_data_processor(fx.r().ctx, s("preview"), &err)) << err.message;
  EXPECT_TRUE(fx.markers.previews.empty());
}

TEST(DataProcessorsKindRouterTest, ListUnionsBothBackends) {
  Fixture fx(/*bind_on_demand=*/false);
  ASSERT_TRUE(routerCreate(fx.r(), "m1", "markers"));
  ASSERT_TRUE(routerCreate(fx.r(), "t1", "transform"));

  const std::vector<std::string> ids = routerList(fx.r());
  EXPECT_EQ(std::set<std::string>(ids.begin(), ids.end()), (std::set<std::string>{"m1", "t1"}));
}

// Layout restore writes to BatchEngine and DataProcessorService directly, bypassing
// the router, so onCreate's cross-kind rejection never sees a hand-edited layout that
// carries one id as both a <transform> and a <generator>. This pins what the router
// does with that state: the id is listed twice, and kind-less ops resolve to the
// transform (probed first), leaving the marker generator unreachable through this API.
TEST(DataProcessorsKindRouterTest, ListSurfacesDuplicateIdsCreatedBehindTheRouter) {
  Fixture fx;
  fx.markers.ids.insert("shared");
  fx.transforms.ids.insert("shared");

  const std::vector<std::string> ids = routerList(fx.r());
  EXPECT_EQ(ids.size(), 2u) << "the duplicate is surfaced, not filtered away";
  EXPECT_EQ(std::count(ids.begin(), ids.end(), "shared"), 2);

  PJ_error_t err{};
  EXPECT_TRUE(fx.r().vtable->remove_data_processor(fx.r().ctx, s("shared"), &err));
  EXPECT_TRUE(fx.transforms.ids.empty());
  EXPECT_EQ(fx.markers.ids.count("shared"), 1u);
}

// The router exists precisely because pj.data_processors.v1 can be registered only
// once. So a name that is already taken must fail registration and name the cause —
// silently dropping it would leave the whole kind's toolboxes wired to someone else's
// backend.
TEST(DataProcessorsKindRouterTest, RegisterServicesFailsWhenTheServiceNameIsTaken) {
  Fixture fx;
  PJ::ServiceRegistryBuilder builder;
  ASSERT_TRUE(fx.router.registerServices(builder).has_value());
  ASSERT_EQ(builder.size(), 1u);

  const PJ::Status status = fx.router.registerServices(builder);

  ASSERT_FALSE(status.has_value());
  EXPECT_NE(status.error().find(PJ::sdk::DataProcessorsHostService::kName), std::string::npos) << status.error();
  EXPECT_NE(status.error().find("duplicate name"), std::string::npos) << status.error();
}

// GAP-34: a request shorter than the v1 prefix is refused before `kind` is read, so no
// backend sees it — not even through an ownership probe — on either typed slot.
TEST(DataProcessorsKindRouterTest, KindRouterChecksStructSizeFirst) {
  Fixture fx;
  PJ_data_processor_request_t request = rawRequest("m1", "markers");
  request.struct_size = 8;

  PJ_error_t err{};
  EXPECT_FALSE(routerCreateV2(fx.r(), request, err));
  EXPECT_EQ(err.code, PJ_ERROR_CODE_REJECTED);
  EXPECT_TRUE(contains(err, "struct_size")) << err.message;

  request.kind = s("on_demand");
  request.time_flags = PJ_DATA_PROCESSOR_TIME_FLAG_INSTANT;
  err = PJ_error_t{};
  uint64_t handle = 0;
  EXPECT_FALSE(fx.r().vtable->submit_evaluation(fx.r().ctx, &request, nullptr, &handle, &err));
  EXPECT_TRUE(contains(err, "struct_size")) << err.message;

  EXPECT_EQ(fx.backendCalls(), 0);
  EXPECT_TRUE(fx.markers.ids.empty());
}

// The read-prefix rule: a request from a newer SDK (larger struct_size) is accepted, and
// the backend gets the known prefix with struct_size = sizeof; one byte short of the v1
// minimum is refused.
TEST(DataProcessorsKindRouterTest, LargerRequestPrefixIsAcceptedAndShortPrefixRejected) {
  Fixture fx;
  struct Newer {
    PJ_data_processor_request_t known;
    uint64_t appended[2];
  } newer{};
  newer.known = rawRequest("m1", "markers");
  newer.known.struct_size = sizeof(Newer);

  PJ_error_t err{};
  ASSERT_TRUE(routerCreateV2(fx.r(), newer.known, err)) << err.message;
  EXPECT_EQ(fx.markers.ids, (std::set<std::string>{"m1"}));
  EXPECT_EQ(fx.markers.last_v2_struct_size, sizeof(PJ_data_processor_request_t));

  PJ_data_processor_request_t short_request = rawRequest("m2", "markers");
  short_request.struct_size = static_cast<uint32_t>(PJ_DATA_PROCESSOR_REQUEST_V1_MIN_SIZE - 1);
  err = PJ_error_t{};
  EXPECT_FALSE(routerCreateV2(fx.r(), short_request, err));
  EXPECT_TRUE(contains(err, "struct_size")) << err.message;
  EXPECT_EQ(fx.markers.ids.count("m2"), 0u);
}

// ABI-24.
TEST(DataProcessorsKindRouterTest, CreateV2RoutesByKind) {
  Fixture fx;
  const PJ::sdk::DataProcessorsHostView view = fx.view();
  ASSERT_TRUE(view.hasTypedRequests());

  EXPECT_TRUE(view.createV2(typed("m1", "markers")).has_value());
  EXPECT_TRUE(view.createV2(typed("t1", "transform")).has_value());
  EXPECT_TRUE(view.createV2(typed("od1", "on_demand")).has_value());
  EXPECT_EQ(fx.markers.ids, (std::set<std::string>{"m1"}));
  EXPECT_EQ(fx.transforms.ids, (std::set<std::string>{"t1"}));
  EXPECT_EQ(fx.on_demand.ids, (std::set<std::string>{"od1"}));
  EXPECT_EQ(fx.on_demand.last_v2_kind, "on_demand");

  // The v1 shape of the decoder's rejections reaches no backend.
  PJ::sdk::DataProcessorRequest both = typed("m2", "markers");
  both.window = std::pair<int64_t, int64_t>{1, 2};
  both.instant_ns = 1;
  const auto refused = view.createV2(both);
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find("mutually exclusive"), std::string::npos) << refused.error();
  EXPECT_EQ(fx.markers.ids.count("m2"), 0u);
}

// ABI-21: kind="on_demand" with a bound third backend routes create/validate to THAT
// backend exclusively; config/remove (no kind) resolve to it by ownership.
TEST(DataProcessorsKindRouterTest, OnDemandKindRoutesToThirdBackend) {
  Fixture fx;
  EXPECT_TRUE(fx.view().createV2(typed("od1", "on_demand")).has_value());
  EXPECT_EQ(fx.on_demand.ids, (std::set<std::string>{"od1"}));
  EXPECT_TRUE(fx.markers.ids.empty());
  EXPECT_TRUE(fx.transforms.ids.empty());

  PJ_error_t err{};
  EXPECT_TRUE(
      fx.r().vtable->validate_data_processor_script(fx.r().ctx, s("on_demand"), s("luau"), s(""), s("{}"), &err));
  EXPECT_TRUE(fx.on_demand.validate_called);
  EXPECT_FALSE(fx.markers.validate_called);
  EXPECT_FALSE(fx.transforms.validate_called);

  PJ_string_view_t recipe{};
  EXPECT_TRUE(fx.r().vtable->data_processor_config(fx.r().ctx, s("od1"), &recipe, &err));
  EXPECT_EQ(fx.on_demand.config_calls, 1);
  EXPECT_EQ(fx.transforms.config_calls + fx.markers.config_calls, 0);

  EXPECT_TRUE(fx.r().vtable->remove_data_processor(fx.r().ctx, s("od1"), &err));
  EXPECT_TRUE(fx.on_demand.ids.empty());
}

// ABI-22: with no third backend bound, kind="on_demand" is an unknown kind on every
// slot that carries a kind, and nothing is created anywhere.
TEST(DataProcessorsKindRouterTest, OnDemandKindWithoutBackendIsRejected) {
  Fixture fx(/*bind_on_demand=*/false);

  PJ_error_t err{};
  EXPECT_FALSE(routerCreate(fx.r(), "od1", "on_demand", &err));
  EXPECT_TRUE(contains(err, "create_data_processor_v2")) << err.message;

  const auto created = fx.view().createV2(typed("od1", "on_demand"));
  ASSERT_FALSE(created.has_value());
  EXPECT_NE(created.error().find("unknown data processor kind 'on_demand'"), std::string::npos) << created.error();

  PJ::sdk::DataProcessorRequest submit = typed("od1", "on_demand");
  submit.instant_ns = 0;
  const auto handle = fx.view().submitEvaluation(submit);
  ASSERT_FALSE(handle.has_value());
  EXPECT_NE(handle.error().find("kind 'on_demand'"), std::string::npos) << handle.error();

  EXPECT_TRUE(fx.transforms.ids.empty());
  EXPECT_TRUE(fx.markers.ids.empty());
  EXPECT_TRUE(fx.on_demand.ids.empty());
  EXPECT_EQ(fx.on_demand.calls, 0);
}

// The contract's v1 rule: kind "on_demand" has typed outputs, so the v1 create refuses
// it naming create_data_processor_v2, before any backend (or ownership probe) is asked.
TEST(DataProcessorsKindRouterTest, V1CreateOfOnDemandNamesTheV2Slot) {
  Fixture fx;
  PJ_error_t err{};
  EXPECT_FALSE(routerCreate(fx.r(), "od1", "on_demand", &err));
  EXPECT_EQ(err.code, PJ_ERROR_CODE_REJECTED);
  EXPECT_TRUE(contains(err, "create_data_processor_v2")) << err.message;
  EXPECT_EQ(fx.backendCalls(), 0);
  EXPECT_TRUE(fx.on_demand.ids.empty());
}

TEST(DataProcessorsKindRouterTest, ListUnionsThreeBackends) {
  Fixture fx;
  ASSERT_TRUE(routerCreate(fx.r(), "m1", "markers"));
  ASSERT_TRUE(routerCreate(fx.r(), "t1", "transform"));
  ASSERT_TRUE(fx.view().createV2(typed("od1", "on_demand")).has_value());

  const std::vector<std::string> ids = routerList(fx.r());
  EXPECT_EQ(std::set<std::string>(ids.begin(), ids.end()), (std::set<std::string>{"m1", "t1", "od1"}));
}

// ABI-23: the cross-kind collision guard holds pairwise across all three backends, on
// both create slots, and sees a preview its backend's config hides. Probing ownership
// never goes through config (which rewrites the backend's borrowed storage).
TEST(DataProcessorsKindRouterTest, IdCollisionAcrossThreeBackends) {
  Fixture fx;
  const PJ::sdk::DataProcessorsHostView view = fx.view();

  ASSERT_TRUE(routerCreate(fx.r(), "shared", "transform"));
  const auto clash = view.createV2(typed("shared", "on_demand"));
  ASSERT_FALSE(clash.has_value());
  EXPECT_NE(clash.error().find("different kind"), std::string::npos) << clash.error();
  EXPECT_FALSE(view.createV2(typed("shared", "markers")).has_value());
  EXPECT_TRUE(fx.on_demand.ids.empty());
  EXPECT_TRUE(fx.markers.ids.empty());

  ASSERT_TRUE(view.createV2(typed("shared2", "on_demand")).has_value());
  EXPECT_FALSE(routerCreate(fx.r(), "shared2", "transform"));
  EXPECT_FALSE(view.createV2(typed("shared2", "markers")).has_value());
  EXPECT_EQ(fx.transforms.ids.count("shared2"), 0u);
  EXPECT_EQ(fx.markers.ids.count("shared2"), 0u);

  ASSERT_TRUE(routerCreate(fx.r(), "preview", "markers", nullptr, PJ_DATA_PROCESSOR_FLAG_EPHEMERAL));
  EXPECT_FALSE(view.createV2(typed("preview", "transform")).has_value());
  EXPECT_FALSE(view.createV2(typed("preview", "on_demand")).has_value());
  EXPECT_TRUE(fx.transforms.ids.empty() || fx.transforms.ids == std::set<std::string>{"shared"});
  EXPECT_EQ(fx.on_demand.ids.count("preview"), 0u);

  // Same-kind ids fall through to the backend's own upsert.
  EXPECT_TRUE(view.createV2(typed("shared", "transform")).has_value());

  EXPECT_EQ(fx.markers.config_calls + fx.transforms.config_calls + fx.on_demand.config_calls, 0);
}

// ABI-25: submit goes to the kind's backend and returns a router handle; poll and
// release reach the issuing backend through it.
TEST(DataProcessorsKindRouterTest, SubmitPollReleaseRouteThroughRouterHandles) {
  Fixture fx;
  const PJ::sdk::DataProcessorsHostView view = fx.view();
  PJ::sdk::DataProcessorRequest request = typed("od1", "on_demand");
  request.instant_ns = 5;

  const auto handle = view.submitEvaluation(request);
  ASSERT_TRUE(handle.has_value()) << handle.error();
  EXPECT_EQ(fx.on_demand.evaluations.size(), 1u);

  const auto polled = view.pollEvaluation(*handle);
  ASSERT_TRUE(polled.has_value()) << polled.error();
  EXPECT_EQ(polled->state, PJ::sdk::EvaluationState::kCompleted);
  EXPECT_EQ(polled->json, R"({"id":"od1"})");

  EXPECT_FALSE(view.pollEvaluation(*handle + 1).has_value());

  EXPECT_TRUE(view.releaseEvaluation(*handle).has_value());
  EXPECT_TRUE(fx.on_demand.evaluations.empty());
  const PJ::Status twice = view.releaseEvaluation(*handle);
  ASSERT_FALSE(twice.has_value());
  EXPECT_NE(twice.error().find("unknown"), std::string::npos) << twice.error();
  EXPECT_FALSE(view.pollEvaluation(*handle).has_value());

  // A kind whose backend has no evaluation is refused naming the kind (ABI-34).
  PJ::sdk::DataProcessorRequest on_transform = typed("t1", "transform");
  on_transform.instant_ns = 5;
  const auto refused = view.submitEvaluation(on_transform);
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find("no on-demand evaluation for kind 'transform'"), std::string::npos) << refused.error();

  // The budget is decoded before the backend sees the request.
  PJ_data_processor_request_t raw = rawRequest("od1", "on_demand");
  raw.time_flags = PJ_DATA_PROCESSOR_TIME_FLAG_INSTANT;
  PJ_evaluation_budget_t budget{};
  budget.struct_size = sizeof(budget);
  budget.reserved = 1;
  PJ_error_t err{};
  uint64_t raw_handle = 0;
  EXPECT_FALSE(fx.r().vtable->submit_evaluation(fx.r().ctx, &raw, &budget, &raw_handle, &err));
  EXPECT_TRUE(contains(err, "reserved")) << err.message;
  EXPECT_TRUE(fx.on_demand.evaluations.empty());

  // Without somewhere to put the handle, the evaluation could never be released.
  err = PJ_error_t{};
  EXPECT_FALSE(fx.r().vtable->submit_evaluation(fx.r().ctx, &raw, nullptr, nullptr, &err));
  EXPECT_TRUE(contains(err, "out_handle")) << err.message;
  EXPECT_TRUE(fx.on_demand.evaluations.empty());
}

// The router's handles are its own: increasing and never reused, distinct from the
// backend's numbering, and unknown to another plugin's router over the same backend.
TEST(DataProcessorsKindRouterTest, RouterHandlesDoNotAliasBackendHandlesOrAnotherPlugin) {
  Fixture fx;
  const PJ::sdk::DataProcessorsHostView view = fx.view();
  PJ::sdk::DataProcessorRequest request = typed("od1", "on_demand");
  request.instant_ns = 5;

  const auto first = view.submitEvaluation(request);
  ASSERT_TRUE(first.has_value()) << first.error();
  ASSERT_EQ(fx.on_demand.evaluations.size(), 1u);
  const uint64_t backend_handle = fx.on_demand.evaluations.begin()->first;
  EXPECT_NE(*first, backend_handle);
  EXPECT_FALSE(view.pollEvaluation(backend_handle).has_value()) << "a backend handle is not a router handle";

  ASSERT_TRUE(view.releaseEvaluation(*first).has_value());
  const auto second = view.submitEvaluation(request);
  ASSERT_TRUE(second.has_value()) << second.error();
  EXPECT_GT(*second, *first) << "a released handle is never reused";

  // Another plugin's router over the very same backend does not know this handle.
  DataProcessorsKindRouter other(
      PJ_data_processors_host_t{}, PJ_data_processors_host_t{}, fx.on_demand.host(),
      {.on_demand = [&fx](std::string_view id) { return fx.on_demand.owns(id); }});
  const PJ::sdk::DataProcessorsHostView other_view(other.raw());
  EXPECT_FALSE(other_view.pollEvaluation(*second).has_value());
  EXPECT_FALSE(other_view.releaseEvaluation(*second).has_value());
  EXPECT_EQ(fx.on_demand.evaluations.size(), 1u) << "the other router released nothing";
  EXPECT_TRUE(view.pollEvaluation(*second).has_value());
}

PJ::sdk::DataProcessorRequest instantOn(std::string_view id) {
  PJ::sdk::DataProcessorRequest request = typed(id, "on_demand");
  request.instant_ns = 5;
  return request;
}

// Two plugins' routers never hand out the same handle, so a handle from one is
// refused by the other instead of reaching the other's evaluation.
TEST(DataProcessorsKindRouterTest, RouterHandlesAreUniqueAcrossRouters) {
  Fixture first;
  Fixture second;
  const auto mine = first.view().submitEvaluation(instantOn("a"));
  const auto theirs = second.view().submitEvaluation(instantOn("b"));
  ASSERT_TRUE(mine.has_value() && theirs.has_value());
  EXPECT_NE(*mine, *theirs);

  EXPECT_FALSE(second.view().pollEvaluation(*mine).has_value());
  EXPECT_FALSE(second.view().releaseEvaluation(*mine).has_value());
  EXPECT_EQ(second.on_demand.evaluations.size(), 1u) << "the other plugin's evaluation is untouched";
  EXPECT_EQ(first.view().pollEvaluation(*mine)->json, R"({"id":"a"})");
}

// The handle counter never wraps: once exhausted, a submit is refused before the
// backend starts anything.
TEST(DataProcessorsKindRouterTest, HandleExhaustionIsRefusedBeforeTheBackend) {
  Fixture fx;
  const uint64_t restore = DataProcessorsKindRouter::setNextHandleForTest(UINT64_MAX);
  PJ_data_processor_request_t raw = rawRequest("od1", "on_demand");
  raw.time_flags = PJ_DATA_PROCESSOR_TIME_FLAG_INSTANT;
  PJ_error_t err{};
  uint64_t handle = 0;
  const bool ok = fx.r().vtable->submit_evaluation(fx.r().ctx, &raw, nullptr, &handle, &err);
  DataProcessorsKindRouter::setNextHandleForTest(restore);
  EXPECT_FALSE(ok);
  EXPECT_EQ(err.code, PJ_ERROR_CODE_REJECTED);
  EXPECT_TRUE(contains(err, "exhausted")) << err.message;
  EXPECT_TRUE(fx.on_demand.evaluations.empty());
}

// A backend whose poll or release slot is absent could hand out a handle the router
// cannot read or free: the submit is refused before it reaches the backend.
TEST(DataProcessorsKindRouterTest, SubmitNeedsAllThreeEvaluationSlots) {
  FakeBackend backend;
  backend.evaluates = true;
  const PJ_data_processors_host_vtable_t full = *backend.host().vtable;
  for (int missing = 0; missing < 2; ++missing) {
    PJ_data_processors_host_vtable_t partial = full;
    if (missing == 0) {
      partial.poll_evaluation = nullptr;
    } else {
      partial.release_evaluation = nullptr;
    }
    DataProcessorsKindRouter router(
        PJ_data_processors_host_t{}, PJ_data_processors_host_t{}, PJ_data_processors_host_t{&backend, &partial},
        {.on_demand = [&backend](std::string_view id) { return backend.owns(id); }});
    const auto handle = PJ::sdk::DataProcessorsHostView(router.raw()).submitEvaluation(instantOn("od1"));
    ASSERT_FALSE(handle.has_value()) << missing;
    EXPECT_NE(handle.error().find("no on-demand evaluation for kind 'on_demand'"), std::string::npos) << handle.error();
    EXPECT_TRUE(backend.evaluations.empty()) << missing;
  }
}

// The router owns the handles it returned: destroying it releases the ones still
// held, on backends that outlive it.
TEST(DataProcessorsKindRouterTest, DestroyingTheRouterReleasesItsEvaluations) {
  FakeBackend backend;
  backend.evaluates = true;
  {
    DataProcessorsKindRouter router(
        PJ_data_processors_host_t{}, PJ_data_processors_host_t{}, backend.host(),
        {.on_demand = [&backend](std::string_view id) { return backend.owns(id); }});
    const PJ::sdk::DataProcessorsHostView view(router.raw());
    const auto released = view.submitEvaluation(instantOn("a"));
    ASSERT_TRUE(view.submitEvaluation(instantOn("b")).has_value());
    ASSERT_TRUE(released.has_value());
    ASSERT_TRUE(view.releaseEvaluation(*released).has_value());
    ASSERT_EQ(backend.evaluations.size(), 1u);
  }
  EXPECT_TRUE(backend.evaluations.empty());
}

// A long or NUL-carrying kind never pushes the reason out of the bounded message: the
// reason comes first and the kind is cut and escaped.
TEST(DataProcessorsKindRouterTest, AHostileKindKeepsTheReasonInTheMessage) {
  Fixture fx;
  const std::string long_kind(300, 'k');
  PJ_data_processor_request_t raw = rawRequest("od1", long_kind);
  raw.time_flags = PJ_DATA_PROCESSOR_TIME_FLAG_INSTANT;
  PJ_error_t err{};
  uint64_t handle = 0;
  EXPECT_FALSE(fx.r().vtable->submit_evaluation(fx.r().ctx, &raw, nullptr, &handle, &err));
  EXPECT_TRUE(contains(err, "no on-demand evaluation for kind 'kkkk")) << err.message;
  EXPECT_TRUE(contains(err, "...")) << err.message;

  const std::string nul_kind("on\0demand", 9);
  raw.kind = s(nul_kind);
  err = PJ_error_t{};
  EXPECT_FALSE(routerCreateV2(fx.r(), raw, err));
  EXPECT_TRUE(contains(err, "unknown data processor kind 'on\\x00demand'")) << err.message;
}

// ABI-26: unbound backends are skipped without a null vtable dereference.
TEST(DataProcessorsKindRouterTest, UnboundBackendsAreSkipped) {
  FakeBackend transforms;
  transforms.accepted_kind = "transform";
  DataProcessorsKindRouter router(
      PJ_data_processors_host_t{}, transforms.host(), PJ_data_processors_host_t{},
      {.transforms = [&transforms](std::string_view id) { return transforms.owns(id); }});
  const PJ_data_processors_host_t r = router.raw();
  const PJ::sdk::DataProcessorsHostView view(r);

  PJ_error_t err{};
  EXPECT_FALSE(routerCreate(r, "m1", "markers", &err));
  EXPECT_TRUE(contains(err, "kind 'markers'")) << err.message;
  err = PJ_error_t{};
  EXPECT_FALSE(routerCreate(r, "od1", "on_demand", &err));
  EXPECT_TRUE(contains(err, "kind 'on_demand'")) << err.message;
  err = PJ_error_t{};
  EXPECT_FALSE(r.vtable->validate_data_processor_script(r.ctx, s("markers"), s("luau"), s(""), s("{}"), &err));
  EXPECT_TRUE(contains(err, "kind 'markers'")) << err.message;
  EXPECT_FALSE(view.createV2(typed("m1", "markers")).has_value());

  ASSERT_TRUE(routerCreate(r, "t1", "transform"));
  EXPECT_EQ(routerList(r), std::vector<std::string>{"t1"});
  err = PJ_error_t{};
  EXPECT_FALSE(r.vtable->remove_data_processor(r.ctx, s("nope"), &err));
  EXPECT_TRUE(contains(err, "unknown")) << err.message;
  EXPECT_TRUE(r.vtable->remove_data_processor(r.ctx, s("t1"), &err));
}

// The three-way check: a backend with the slot answers through it, so the on-demand backend's UNCHECKED and its reason
// reach the plugin instead of a pass; a backend without it is asked its validate, a pass reading VALID and a refusal
// INVALID with the reason. An unknown kind, a NULL out_status and a null ctx are calls that could not be made.
TEST(DataProcessorsKindRouterTest, CheckForwardsToTheBackendsCheckElseMapsItsValidate) {
  Fixture fx;
  const PJ::sdk::DataProcessorsHostView view = fx.view();
  const auto unchecked = view.checkScript("on_demand", "python", "x");
  ASSERT_TRUE(unchecked.has_value()) << unchecked.error();
  EXPECT_EQ(unchecked->status, PJ::sdk::ScriptCheckStatus::kUnchecked);
  EXPECT_NE(unchecked->message.find("fake: not decided"), std::string::npos) << unchecked->message;
  EXPECT_TRUE(fx.on_demand.check_called);
  EXPECT_FALSE(fx.on_demand.validate_called);

  const auto valid = view.checkScript("markers", "luau", "return 1");
  ASSERT_TRUE(valid.has_value()) << valid.error();
  EXPECT_EQ(valid->status, PJ::sdk::ScriptCheckStatus::kValid);
  EXPECT_TRUE(fx.markers.validate_called);
  fx.transforms.validate_error = "fake: syntax error";
  const auto invalid = view.checkScript("transform", "luau", "return {");
  ASSERT_TRUE(invalid.has_value()) << invalid.error();
  EXPECT_EQ(invalid->status, PJ::sdk::ScriptCheckStatus::kInvalid);
  EXPECT_NE(invalid->message.find("fake: syntax error"), std::string::npos) << invalid->message;

  const auto unknown = view.checkScript("nope", "luau", "return 1");
  ASSERT_FALSE(unknown.has_value());
  EXPECT_NE(unknown.error().find("unknown data processor kind"), std::string::npos) << unknown.error();
  const PJ_data_processors_host_t r = fx.r();
  ASSERT_TRUE(PJ_HAS_TAIL_SLOT(PJ_data_processors_host_vtable_t, r.vtable, check_data_processor_script));
  PJ_error_t err{};
  EXPECT_FALSE(r.vtable->check_data_processor_script(r.ctx, s("markers"), s("luau"), s(""), s("{}"), nullptr, &err));
  EXPECT_GT(std::strlen(err.message), 0u);
  err = PJ_error_t{};
  uint32_t status = PJ_SCRIPT_CHECK_VALID;
  EXPECT_FALSE(r.vtable->check_data_processor_script(nullptr, s("markers"), s("luau"), s(""), s("{}"), &status, &err));
  EXPECT_GT(std::strlen(err.message), 0u);
}

// ABI-27: every router refusal is PJ_ERROR_CODE_REJECTED, and a null ctx still says why.
TEST(DataProcessorsKindRouterTest, RefusalsCarryTheRejectedCodeAndNullCtxFillsTheError) {
  Fixture fx;
  const PJ_data_processors_host_t r = fx.r();
  ASSERT_TRUE(routerCreate(r, "shared", "markers"));

  const auto expectRejected = [](bool ok, const PJ_error_t& err, const char* what) {
    EXPECT_FALSE(ok) << what;
    EXPECT_EQ(err.code, PJ_ERROR_CODE_REJECTED) << what;
    EXPECT_GT(std::strlen(err.message), 0u) << what;
  };
  PJ_error_t err{};
  expectRejected(routerCreate(r, "x", "nope", &err), err, "unknown kind");
  err = PJ_error_t{};
  expectRejected(routerCreate(r, "shared", "transform", &err), err, "collision");
  err = PJ_error_t{};
  expectRejected(r.vtable->remove_data_processor(r.ctx, s("nope"), &err), err, "unknown id on remove");
  err = PJ_error_t{};
  PJ_string_view_t recipe{};
  expectRejected(r.vtable->data_processor_config(r.ctx, s("nope"), &recipe, &err), err, "unknown id on config");
  err = PJ_error_t{};
  uint32_t state = 0;
  PJ_string_view_t json{};
  expectRejected(r.vtable->poll_evaluation(r.ctx, 42, &state, &json, &err), err, "unknown handle on poll");
  err = PJ_error_t{};
  expectRejected(r.vtable->release_evaluation(r.ctx, 42, &err), err, "unknown handle on release");
  err = PJ_error_t{};
  expectRejected(routerCreateV2(r, rawRequest("y", "nope"), err), err, "unknown kind on create_v2");
  err = PJ_error_t{};
  expectRejected(r.vtable->create_data_processor_v2(r.ctx, nullptr, nullptr, 0, nullptr, &err), err, "null request");

  const PJ_data_processor_request_t request = rawRequest("m1", "markers");
  uint64_t topics = 0;
  uint64_t handle = 0;
  uint64_t count = 0;
  const auto expectExplained = [](bool ok, const PJ_error_t& err, const char* slot) {
    EXPECT_FALSE(ok) << slot;
    EXPECT_GT(std::strlen(err.message), 0u) << slot;
  };
  const PJ_data_processors_host_vtable_t& vt = *r.vtable;
  err = PJ_error_t{};
  expectExplained(
      vt.create_data_processor(
          nullptr, s("a"), s("markers"), s("luau"), nullptr, 0, nullptr, 0, s(""), s("{}"), 0, nullptr, 0, &topics,
          &err),
      err, "create");
  err = PJ_error_t{};
  expectExplained(vt.remove_data_processor(nullptr, s("a"), &err), err, "remove");
  err = PJ_error_t{};
  expectExplained(vt.list_data_processor_ids(nullptr, nullptr, 0, &count, &err), err, "list");
  err = PJ_error_t{};
  expectExplained(vt.data_processor_config(nullptr, s("a"), &recipe, &err), err, "config");
  err = PJ_error_t{};
  expectExplained(
      vt.validate_data_processor_script(nullptr, s("markers"), s("luau"), s(""), s("{}"), &err), err, "validate");
  err = PJ_error_t{};
  expectExplained(vt.create_data_processor_v2(nullptr, &request, nullptr, 0, &topics, &err), err, "create_v2");
  err = PJ_error_t{};
  expectExplained(vt.submit_evaluation(nullptr, &request, nullptr, &handle, &err), err, "submit");
  err = PJ_error_t{};
  expectExplained(vt.poll_evaluation(nullptr, 1, &state, &json, &err), err, "poll");
  err = PJ_error_t{};
  expectExplained(vt.release_evaluation(nullptr, 1, &err), err, "release");
}

// The typed decode a request costs, per input and output: the decoder walks each array
// once to validate its views (no copy). Through the router the backend decodes again.
TEST(DataProcessorsKindRouterBench, TypedDecodeCost) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  constexpr std::size_t kElements = 1000;
  constexpr int kRuns = 2001;
  const std::vector<std::string> names = [] {
    std::vector<std::string> out;
    for (std::size_t index = 0; index < kElements; ++index) {
      out.push_back("topic/field_" + std::to_string(index));
    }
    return out;
  }();
  std::vector<PJ_string_view_t> inputs;
  std::vector<PJ_data_processor_output_t> outputs;
  for (const std::string& name : names) {
    inputs.push_back(s(name));
    outputs.push_back(PJ_data_processor_output_t{s(name), s(""), {0, 0}});
  }
  PJ_data_processor_request_t request = rawRequest("bench", "transform");
  request.inputs = inputs.data();
  request.input_count = inputs.size();
  request.outputs = outputs.data();
  request.output_count = outputs.size();

  std::vector<double> nanos;
  for (int run = 0; run < kRuns; ++run) {
    const auto begin = std::chrono::steady_clock::now();
    const auto decoded = PJ::sdk::readDataProcessorRequest(&request, PJ::sdk::DataProcessorSlot::kCreateV2);
    const auto end = std::chrono::steady_clock::now();
    ASSERT_TRUE(decoded.has_value());
    nanos.push_back(std::chrono::duration<double, std::nano>(end - begin).count());
  }
  std::nth_element(nanos.begin(), nanos.begin() + kRuns / 2, nanos.end());
  std::printf(
      "readDataProcessorRequest, %zu inputs + %zu outputs: median %.0f ns, %.2f ns per input/output\n", kElements,
      kElements, nanos[kRuns / 2], nanos[kRuns / 2] / (2.0 * kElements));
}

}  // namespace
