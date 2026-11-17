// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Streaming pause/resume two-engine lockstep — runtime-host INTEGRATION level.
//
// Background. PJ4 streaming double-buffers the DataEngine: live messages land
// in the primary (A); on pause, incoming messages are rerouted into a
// secondary (B) so the frozen view stays stable and bounded; on resume, B
// flushes back into A. The reroute is DataSourceRuntimeHost::setDataEngineTarget.
//
// The bug PR #200 fixes: a streaming plugin registers its topic/fields ONCE
// (data_stream_dummy in onStart(); parser_protobuf on first message) and caches
// the returned TopicHandle / FieldHandle, then reuses them on every subsequent
// sample via appendBoundRecord. A handle is just an integer id into ONE engine.
// After setDataEngineTarget(B) the cached id must resolve to the SAME topic/field
// in B — but on origin/main the source-level write path mirrors NOTHING to the
// secondary, and the parser path mirrors topics but not fields. So the first
// post-pause write fails ("topic N not found" / field lookup fails) and the
// streaming worker dies. That is the reported "data_stream_dummy dies on first
// pause/resume" crash.
//
// These are the runtime-INTEGRATION companion to the datastore-unit tests in
// pj_datastore/tests/streaming_mirror_test.cpp. The unit tests verify the
// createTopicField / setSecondaryEngine MECHANISM in isolation (driving
// DatastoreSourceWriteHost directly). The tests here verify that
// DataSourceRuntimeHost actually WIRES that mechanism — calling
// setSecondaryEngine() on the source write host and on every parser binding —
// and that a plugin's cached handles survive a real setDataEngineTarget swap
// end to end.
//
// They touch only the host's PUBLIC surface (the secondary_data_engine ctor arg
// + setDataEngineTarget), which the fix leaves unchanged, so:
//   * without the fix they FAIL — the host never mirrors source/parser writes
//     to the secondary engine, so the cached handle goes stale across the swap;
//     i.e. they reproduce the reported "data_stream_dummy dies on first
//     pause/resume" bug (source path) and its parser-path variant,
//   * with the fix they PASS, and
//   * they guard the WIRING itself: deleting the host's setSecondaryEngine call
//     re-breaks streaming while the datastore-unit tests stay green.

#include <gtest/gtest.h>

#include <QFileInfo>
#include <QString>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "hermetic_catalog.h"
#include "pj_base/sdk/data_source_host_views.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"
using namespace Qt::StringLiterals;

#ifndef PJ_STREAMING_CACHING_PARSER_PATH
#error "PJ_STREAMING_CACHING_PARSER_PATH must be defined"
#endif

namespace {

using PJ::PrimitiveType;
using PJ::sdk::BoundFieldValue;
using PJ::sdk::FieldHandle;
using PJ::sdk::SourceWriteHostView;
using PJ::sdk::TopicHandle;

// Committed rows for `topic_id` on `engine`.
[[nodiscard]] uint64_t rowCount(PJ::DataEngine& engine, PJ::TopicId topic_id) {
  PJ::DataReader reader(engine);
  const auto meta = reader.getMetadata(topic_id);
  return meta.has_value() ? meta->total_row_count : 0;
}

// Every engine topic under `dataset_id` whose descriptor carries `name`.
// createTopic mints a fresh TopicId per call with NO name dedup, so a
// non-idempotent rebind shows up here as a duplicate topic.
[[nodiscard]] std::size_t countTopicsNamed(
    const PJ::DataEngine& engine, PJ::DatasetId dataset_id, const std::string& name) {
  std::size_t count = 0;
  for (const PJ::TopicId topic_id : engine.listTopics(dataset_id)) {
    const auto* storage = engine.getTopicStorage(topic_id);
    if (storage != nullptr && storage->descriptor().name == name) {
      ++count;
    }
  }
  return count;
}

// Two lockstep engines + a DataSourceRuntimeHost wired for streaming
// pause/resume, exercising the SOURCE-LEVEL scalar write path
// (SourceWriteHostService) — the path a plugin like data_stream_dummy uses
// when it caches handles in onStart() and writes bound records per sample.
class StreamEngineSwapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Primary dataset (id 0 on a fresh engine).
    auto primary_dataset =
        primary_engine_.createDataset(PJ::DatasetDescriptor{.source_name = "stream", .time_domain_id = 0});
    ASSERT_TRUE(primary_dataset.has_value()) << primary_dataset.error();
    dataset_id_ = static_cast<PJ::DatasetId>(*primary_dataset);

    // Lockstep dataset on the secondary with the SAME DatasetId — mirrors the
    // real streaming manager, which creates the dataset on both engines so a
    // later topic mirror can address it by id on either side.
    auto secondary_dataset =
        secondary_engine_.createDataset(PJ::DatasetDescriptor{.source_name = "stream", .time_domain_id = 0});
    ASSERT_TRUE(secondary_dataset.has_value()) << secondary_dataset.error();
    ASSERT_EQ(static_cast<PJ::DatasetId>(*secondary_dataset), dataset_id_);

    source_handle_ = PJ_data_source_handle_t{static_cast<uint32_t>(dataset_id_)};
    host_ = std::make_unique<PJ::DataSourceRuntimeHost>(
        primary_engine_, catalog_, dataset_id_, source_handle_, primary_object_store_, "stream_swap_source",
        /*parser_registrar=*/nullptr, /*secondary_object_store=*/nullptr, /*secondary_data_engine=*/&secondary_engine_,
        /*library_keepalive=*/nullptr);
    EXPECT_TRUE(host_->registerServices(registry_builder_).has_value());
  }

  // The SourceWriteHostView a source plugin would receive from bind().
  [[nodiscard]] SourceWriteHostView sourceWriter() {
    PJ::sdk::ServiceRegistry services(registry_builder_.view());
    auto writer_or = services.require<PJ::sdk::SourceWriteHostService>();
    EXPECT_TRUE(writer_or.has_value()) << writer_or.error();
    return writer_or.has_value() ? *writer_or : SourceWriteHostView{};
  }

  PJ::ExtensionCatalogService catalog_{QString{}};
  PJ::DataEngine primary_engine_;
  PJ::DataEngine secondary_engine_;
  PJ::ObjectStore primary_object_store_;
  PJ::DatasetId dataset_id_{0};
  PJ_data_source_handle_t source_handle_{};
  std::unique_ptr<PJ::DataSourceRuntimeHost> host_;
  PJ::ServiceRegistryBuilder registry_builder_;
};

// The first ensureTopic/ensureField on the source path must lockstep-mirror the
// new topic + columns onto the secondary engine with the SAME ids — otherwise a
// later setDataEngineTarget(secondary) lands on an engine that has no topic for
// the plugin's cached handle. On origin/main the source write host mirrors
// nothing, so the secondary has no "imu" topic at all → RED here.
TEST_F(StreamEngineSwapTest, FirstEnsureLockstepsTopicAndFieldIdsOntoSecondaryEngine) {
  SourceWriteHostView writer = sourceWriter();

  const auto topic = *writer.ensureTopic("imu");
  const auto field_ax = *writer.ensureField(topic, "ax", PrimitiveType::kFloat32);
  const auto field_ay = *writer.ensureField(topic, "ay", PrimitiveType::kFloat32);

  // FieldIds are dense from 0 — the first field of a topic legitimately gets
  // id 0. (PR #200's std::optional sentinel exists precisely so a forced id 0
  // is not confused with "auto-assign".)
  EXPECT_EQ(field_ax.id, 0U);
  EXPECT_EQ(field_ay.id, 1U);

  ASSERT_TRUE(writer.appendBoundRecord(topic, 10, {{.field = field_ax, .value = 1.0F}}).has_value());
  host_->flushPending();

  // The crux: the secondary engine must hold the same topic id and the same
  // (name, id) columns.
  const auto* sec_storage = secondary_engine_.getTopicStorage(topic.id);
  ASSERT_NE(sec_storage, nullptr) << "secondary engine never received the lockstep topic mirror";
  const auto& cols = sec_storage->columnDescriptors();
  ASSERT_GE(cols.size(), 2U) << "secondary topic is missing the lockstep field columns";
  EXPECT_EQ(cols[0].field_path, "ax");
  EXPECT_EQ(cols[0].field_id, field_ax.id);
  EXPECT_EQ(cols[1].field_path, "ay");
  EXPECT_EQ(cols[1].field_id, field_ay.id);
}

// The reported bug, end to end: cache handles, write to A, pause (swap to B),
// then write THROUGH THE SAME CACHED HANDLES. On origin/main the post-pause
// write fails because B has no matching topic/field → RED. Resume (swap back to
// A) must keep the handles valid too.
TEST_F(StreamEngineSwapTest, CachedSourceHandlesResolveAcrossPauseAndResumeSwap) {
  SourceWriteHostView writer = sourceWriter();

  // Plugin registers + caches its handles once (data_stream_dummy::onStart).
  const auto topic = *writer.ensureTopic("imu");
  const auto field_ax = *writer.ensureField(topic, "ax", PrimitiveType::kFloat32);
  const auto field_ay = *writer.ensureField(topic, "ay", PrimitiveType::kFloat32);

  // Live phase — writes land in the primary.
  ASSERT_TRUE(
      writer.appendBoundRecord(topic, 10, {{.field = field_ax, .value = 1.0F}, {.field = field_ay, .value = 2.0F}})
          .has_value());
  host_->flushPending();

  // Pause: reroute scalar writes to the secondary engine.
  host_->setDataEngineTarget(&secondary_engine_);

  // Write through the cached handles — this is the exact call that died before
  // the fix. The secondary must already know the topic+fields at matching ids.
  const auto paused_write =
      writer.appendBoundRecord(topic, 20, {{.field = field_ax, .value = 3.0F}, {.field = field_ay, .value = 4.0F}});
  ASSERT_TRUE(paused_write.has_value()) << "cached FieldHandle did not resolve on the secondary engine after pause: "
                                        << paused_write.error();
  host_->flushPending();

  // Resume: swap back to the primary. Bidirectional lockstep keeps both valid.
  host_->setDataEngineTarget(&primary_engine_);
  const auto resumed_write =
      writer.appendBoundRecord(topic, 30, {{.field = field_ax, .value = 5.0F}, {.field = field_ay, .value = 6.0F}});
  ASSERT_TRUE(resumed_write.has_value()) << "cached FieldHandle did not resolve on the primary engine after resume: "
                                         << resumed_write.error();
  host_->flushPending();

  // Rows split across the two engines exactly as the pause window dictates:
  // ts 10 + ts 30 on the primary, ts 20 on the secondary.
  EXPECT_EQ(rowCount(primary_engine_, topic.id), 2U);
  EXPECT_EQ(rowCount(secondary_engine_, topic.id), 1U);
}

// Source-level (non-parser) analogue of the mid-pause new-topic case — the path
// data_stream_dummy uses. Here the write host is a SINGLE shared host retargeted
// at pause (not a fresh per-binding host), and WriteCore always creates the
// topic on the active engine AND mirrors it to the other, so this path is
// expected to already be safe: a topic first ensured mid-pause should land its
// sample on the secondary and leave the frozen primary empty. This pins that
// second, independent case so the fix is not argued from the parser path alone.
TEST_F(StreamEngineSwapTest, SourceTopicCreatedWhilePausedWritesToSecondaryEngine) {
  SourceWriteHostView writer = sourceWriter();

  // Enter the pause window BEFORE the topic has ever been ensured.
  host_->setDataEngineTarget(&secondary_engine_);

  const auto topic = *writer.ensureTopic("late_imu");
  const auto field = *writer.ensureField(topic, "ax", PrimitiveType::kFloat32);
  ASSERT_TRUE(writer.appendBoundRecord(topic, 20, {{.field = field, .value = 3.0F}}).has_value());
  host_->flushPending();

  EXPECT_EQ(rowCount(secondary_engine_, topic.id), 1U)
      << "a mid-pause source topic's sample must land on the secondary engine";
  EXPECT_EQ(rowCount(primary_engine_, topic.id), 0U)
      << "the frozen primary gained a row during pause — the timeline would advance";

  // Resume: the mirrored topic is on the primary, so the tail flushes cleanly.
  host_->setDataEngineTarget(&primary_engine_);
  ASSERT_TRUE(secondary_engine_.flushTo(primary_engine_).has_value())
      << "resume flush failed — the mid-pause topic was not mirrored onto the primary";
  EXPECT_EQ(rowCount(primary_engine_, topic.id), 1U) << "the paused tail did not flush back on resume";
}

// Two lockstep engines + a DataSourceRuntimeHost driving the PARSER write path:
// a real parser plugin (streaming_caching_parser_plugin) that caches a
// FieldHandle on its first parse and reuses it — the parser_protobuf pattern.
// PR #200's commit message claims it "closes the latent FieldHandle-stale bug
// for parser plugins (parser_protobuf et al.)", yet ships no parser-path test.
// This fixture is that missing coverage.
class StreamParserSwapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NE(catalog_.findParserByEncoding(u"streaming_caching"_s), nullptr)
        << "streaming_caching_parser_plugin was not discovered in " << plugin_dir_.toStdString();

    auto primary_dataset =
        primary_engine_.createDataset(PJ::DatasetDescriptor{.source_name = "stream", .time_domain_id = 0});
    ASSERT_TRUE(primary_dataset.has_value()) << primary_dataset.error();
    dataset_id_ = static_cast<PJ::DatasetId>(*primary_dataset);

    auto secondary_dataset =
        secondary_engine_.createDataset(PJ::DatasetDescriptor{.source_name = "stream", .time_domain_id = 0});
    ASSERT_TRUE(secondary_dataset.has_value()) << secondary_dataset.error();
    ASSERT_EQ(static_cast<PJ::DatasetId>(*secondary_dataset), dataset_id_);

    source_handle_ = PJ_data_source_handle_t{static_cast<uint32_t>(dataset_id_)};
    host_ = std::make_unique<PJ::DataSourceRuntimeHost>(
        primary_engine_, catalog_, dataset_id_, source_handle_, primary_object_store_, "stream_parser_swap_source",
        /*parser_registrar=*/nullptr, /*secondary_object_store=*/nullptr, /*secondary_data_engine=*/&secondary_engine_,
        /*library_keepalive=*/nullptr);
    EXPECT_TRUE(host_->registerServices(registry_builder_).has_value());
  }

  [[nodiscard]] PJ::DataSourceRuntimeHostView runtime() {
    PJ::sdk::ServiceRegistry services(registry_builder_.view());
    auto runtime_or = services.require<PJ::sdk::DataSourceRuntimeHostService>();
    EXPECT_TRUE(runtime_or.has_value()) << runtime_or.error();
    return runtime_or.has_value() ? *runtime_or : PJ::DataSourceRuntimeHostView{};
  }

  // Push one message carrying a float32 payload through the bound parser.
  [[nodiscard]] PJ::Status pushFloat(PJ::ParserBindingHandle binding, PJ::Timestamp timestamp, float value) {
    std::vector<uint8_t> payload(sizeof(float));
    std::memcpy(payload.data(), &value, sizeof(float));
    return runtime().pushMessage(binding, timestamp, [payload]() -> std::vector<uint8_t> { return payload; });
  }

  QFileInfo plugin_file_{QString::fromUtf8(PJ_STREAMING_CACHING_PARSER_PATH)};
  QString plugin_dir_{plugin_file_.absolutePath()};
  PJ::test::HermeticCatalog catalog_box_{plugin_dir_};
  PJ::ExtensionCatalogService& catalog_{catalog_box_.service};
  PJ::DataEngine primary_engine_;
  PJ::DataEngine secondary_engine_;
  PJ::ObjectStore primary_object_store_;
  PJ::DatasetId dataset_id_{0};
  PJ_data_source_handle_t source_handle_{};
  std::unique_ptr<PJ::DataSourceRuntimeHost> host_;
  PJ::ServiceRegistryBuilder registry_builder_;
};

// The parser-path analogue of the source-path repro. The host mirrors the bound
// TOPIC onto the secondary engine, but on origin/main the parser write host
// never mirrors the FIELD. So after the pause swap, the parser's cached
// FieldHandle points at a column the secondary topic does not have, and the
// post-pause push fails → RED. PR #200 wires setSecondaryEngine() onto the
// parser binding's write host, mirroring the field at a matching id → GREEN.
TEST_F(StreamParserSwapTest, CachedParserFieldHandleResolvesAfterPauseSwap) {
  auto binding_or = runtime().ensureParserBinding(
      PJ::ParserBindingRequest{
          .topic_name = "/imu/value",
          .parser_encoding = "streaming_caching",
          .type_name = "streaming/cached_scalar",
          .schema = PJ::Span<const uint8_t>{},
          .parser_config_json = "{}",
      });
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();
  const auto binding = *binding_or;

  // Live phase: first parse caches the FieldHandle and writes to the primary.
  ASSERT_TRUE(pushFloat(binding, 10, 1.0F).has_value());
  host_->flushPending();

  // Pause: reroute parser scalar writes to the secondary engine.
  host_->setDataEngineTarget(&secondary_engine_);

  // The parser reuses its cached FieldHandle. On origin/main the field was
  // never mirrored to the secondary → this push fails.
  const auto paused_push = pushFloat(binding, 20, 2.0F);
  ASSERT_TRUE(paused_push.has_value())
      << "parser's cached FieldHandle did not resolve on the secondary engine after pause: " << paused_push.error();
  host_->flushPending();

  // Resume keeps it valid too.
  host_->setDataEngineTarget(&primary_engine_);
  ASSERT_TRUE(pushFloat(binding, 30, 3.0F).has_value());
  host_->flushPending();
}

// Mid-session new topic while PAUSED. A topic first observed AFTER the pause
// swap gets its parser binding minted mid-pause. setDataEngineTarget only
// retargeted the bindings that existed at swap time, so the new binding's write
// host initialised against the PRIMARY (frozen) engine — its first push landed
// on the frozen primary and dragged the global timeline, even though every
// pre-existing binding respected the pause. The dummy streamer never hit this
// (it declares all topics in onStart, before any pause could apply); real
// streamers publishing a brand-new topic mid-session (ZMQ, MQTT) did.
//
// The fix initialises a freshly-minted binding's write host to the ACTIVE
// target, so a binding born during pause writes into the secondary like every
// other paused write. Pre-fix the sample lands on the primary → RED here; with
// the fix it lands on the secondary and the frozen primary stays at zero rows.
TEST_F(StreamParserSwapTest, BindingCreatedWhilePausedWritesToSecondaryEngine) {
  // Enter the pause window BEFORE the topic has ever been seen.
  host_->setDataEngineTarget(&secondary_engine_);

  // First observation of the topic happens now, mid-pause: the binding (and its
  // write host) is minted while the swap is already active.
  auto binding_or = runtime().ensureParserBinding(
      PJ::ParserBindingRequest{
          .topic_name = "/late/value",
          .parser_encoding = "streaming_caching",
          .type_name = "streaming/cached_scalar",
          .schema = PJ::Span<const uint8_t>{},
          .parser_config_json = "{}",
      });
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  // The empty topic descriptor is minted on both engines at bind time (that is
  // harmless — no samples, no time bounds); the SAMPLE is what must not touch
  // the frozen primary.
  const auto topic_ids = primary_engine_.listTopics(dataset_id_);
  ASSERT_EQ(topic_ids.size(), 1U);
  const PJ::TopicId topic_id = topic_ids.front();

  ASSERT_TRUE(pushFloat(*binding_or, 20, 2.0F).has_value());
  host_->flushPending();

  EXPECT_EQ(rowCount(secondary_engine_, topic_id), 1U)
      << "a mid-pause topic's sample must land on the secondary (live/tail) engine";
  EXPECT_EQ(rowCount(primary_engine_, topic_id), 0U)
      << "the frozen primary gained a row during pause — the timeline would advance";
}

// A demand-driven source unsubscribes and later RE-subscribes a topic; the
// plugin calls ensure_parser_binding again with the identical request (its own
// binding cache was dropped with the subscription). The host must hand back
// the EXISTING binding — a second createTopic would register a second engine
// topic with the same name, doubling every field in the catalog and curve tree.
TEST_F(StreamParserSwapTest, IdenticalRebindReusesBindingInsteadOfDuplicatingTopic) {
  const PJ::ParserBindingRequest request{
      .topic_name = "/imu/value",
      .parser_encoding = "streaming_caching",
      .type_name = "streaming/cached_scalar",
      .schema = PJ::Span<const uint8_t>{},
      .parser_config_json = "{}",
  };
  auto first = runtime().ensureParserBinding(request);
  ASSERT_TRUE(first.has_value()) << first.error();
  ASSERT_TRUE(pushFloat(*first, 10, 1.0F).has_value());
  host_->flushPending();

  auto second = runtime().ensureParserBinding(request);
  ASSERT_TRUE(second.has_value()) << second.error();
  EXPECT_EQ(second->id, first->id) << "identical rebind must reuse the existing binding";
  EXPECT_EQ(countTopicsNamed(primary_engine_, dataset_id_, "/imu/value"), 1U)
      << "rebind must not register a duplicate engine topic";

  // The reused handle keeps ingesting into the SAME topic.
  ASSERT_TRUE(pushFloat(*second, 20, 2.0F).has_value());
  host_->flushPending();
  EXPECT_EQ(countTopicsNamed(primary_engine_, dataset_id_, "/imu/value"), 1U);
}

// A topic re-advertised with a different type/schema is a genuine retype — the
// old binding's parser and columns don't apply, so the host must mint a fresh
// binding rather than reuse one bound to the stale schema.
TEST_F(StreamParserSwapTest, RetypedRebindMintsFreshBinding) {
  const PJ::ParserBindingRequest request{
      .topic_name = "/imu/value",
      .parser_encoding = "streaming_caching",
      .type_name = "streaming/cached_scalar",
      .schema = PJ::Span<const uint8_t>{},
      .parser_config_json = "{}",
  };
  auto first = runtime().ensureParserBinding(request);
  ASSERT_TRUE(first.has_value()) << first.error();

  PJ::ParserBindingRequest retyped = request;
  retyped.type_name = "streaming/other_type";
  auto second = runtime().ensureParserBinding(retyped);
  ASSERT_TRUE(second.has_value()) << second.error();
  EXPECT_NE(second->id, first->id) << "a retyped topic must get a fresh binding";
}

// The offline pin for the delegated-ingest TopicId-stability contract.
//
// SessionManager::beginRefill keeps a reloading source's TopicIds REGISTERED so
// the refill writes back into the same ids and every curve key survives. That
// only holds if binding a name the dataset ALREADY carries resolves to the
// existing topic instead of minting a new one — which the direct-write API
// (WriteCore::ensureTopic) and the object route have always done, and which
// this scalar parser-binding path did NOT do until the fix this test pins.
//
// The gap was invisible to every other offline test because the test/mock
// loaders write through the direct-write API (which reuses by name); only a
// REAL delegated-ingest loader (data_load_mcap — i.e. every cloud promotion)
// took the broken path, so the bug surfaced first in the live layout-import
// E2E. Pre-fix this fails on the first assertion: the bind mints a second
// same-named topic and the dataset carries two.
TEST_F(StreamParserSwapTest, ParserBindingReusesTheDatasetsExistingSameNamedTopic) {
  // A topic the dataset already carries — the shape a refill/reload presents.
  auto existing_or = primary_engine_.createTopic(dataset_id_, PJ::TopicDescriptor{.name = "/reused"});
  ASSERT_TRUE(existing_or.has_value()) << existing_or.error();
  const PJ::TopicId existing_id = *existing_or;

  auto binding_or = runtime().ensureParserBinding(
      PJ::ParserBindingRequest{
          .topic_name = "/reused",
          .parser_encoding = "streaming_caching",
          .type_name = "streaming/cached_scalar",
          .schema = PJ::Span<const uint8_t>{},
          .parser_config_json = "{}",
      });
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  // No duplicate minted, and the id the refill contract depends on is intact.
  ASSERT_EQ(countTopicsNamed(primary_engine_, dataset_id_, "/reused"), 1U)
      << "binding minted a duplicate same-named topic";
  EXPECT_EQ(primary_engine_.listTopics(dataset_id_).front(), existing_id);

  // And the binding actually writes into that same topic.
  ASSERT_TRUE(pushFloat(*binding_or, 1000, 1.0F).has_value());
  host_->flushPending();
  EXPECT_GT(rowCount(primary_engine_, existing_id), 0U) << "rows landed somewhere other than the reused topic";
}

// The direct-write path stores "robot//imu" as "robot/imu"; a parser binding on the same name must reach that topic,
// not mint a second one under the raw name.
TEST_F(StreamParserSwapTest, ParserBindingCollapsesSlashesLikeTheDirectWritePath) {
  PJ::sdk::ServiceRegistry services(registry_builder_.view());
  auto writer_or = services.require<PJ::sdk::SourceWriteHostService>();
  ASSERT_TRUE(writer_or.has_value()) << writer_or.error();
  const auto topic = writer_or->ensureTopic("robot//imu");
  ASSERT_TRUE(topic.has_value()) << topic.error();

  auto binding_or = runtime().ensureParserBinding(
      PJ::ParserBindingRequest{
          .topic_name = "robot//imu",
          .parser_encoding = "streaming_caching",
          .type_name = "streaming/cached_scalar",
          .schema = PJ::Span<const uint8_t>{},
          .parser_config_json = "{}",
      });
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  EXPECT_EQ(primary_engine_.listTopics(dataset_id_).size(), 1U) << "the binding minted a second topic";
  ASSERT_TRUE(pushFloat(*binding_or, 1000, 1.0F).has_value());
  host_->flushPending();
  EXPECT_GT(rowCount(primary_engine_, topic->id), 0U)
      << "the binding wrote somewhere other than the direct-write topic";
}

// The same reuse path seen from the streaming pause/resume lockstep: the mirror
// stays idempotent — an id the secondary already holds is not re-created — and
// the two engines keep matching ids.
TEST_F(StreamParserSwapTest, ParserBindingReuseKeepsTheSecondaryEngineMirrorIdempotent) {
  // Both engines already carry the topic under the same id — the post-refill
  // state a rebind lands in. (SetUp already pinned the two datasets' ids.)
  auto primary_or = primary_engine_.createTopic(dataset_id_, PJ::TopicDescriptor{.name = "/mirrored"});
  ASSERT_TRUE(primary_or.has_value()) << primary_or.error();
  auto secondary_or = secondary_engine_.createTopic(dataset_id_, PJ::TopicDescriptor{.name = "/mirrored"}, *primary_or);
  ASSERT_TRUE(secondary_or.has_value()) << secondary_or.error();

  auto binding_or = runtime().ensureParserBinding(
      PJ::ParserBindingRequest{
          .topic_name = "/mirrored",
          .parser_encoding = "streaming_caching",
          .type_name = "streaming/cached_scalar",
          .schema = PJ::Span<const uint8_t>{},
          .parser_config_json = "{}",
      });
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  ASSERT_EQ(countTopicsNamed(primary_engine_, dataset_id_, "/mirrored"), 1U)
      << "binding minted a duplicate same-named topic on the primary";
  ASSERT_EQ(countTopicsNamed(secondary_engine_, dataset_id_, "/mirrored"), 1U)
      << "the mirror re-created an id the secondary already held";
  EXPECT_EQ(primary_engine_.listTopics(dataset_id_).front(), secondary_engine_.listTopics(dataset_id_).front());
}

}  // namespace
