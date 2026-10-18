// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// What the on-demand tests (ResultHistory, InputStager, NodeScheduler) share: point clouds, recipes whose
// scripts never run, a parser over raw points, and readers of the store.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "pj_base/buffer_anchor.hpp"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/RecipeGraph.h"

namespace PJ::test {

inline constexpr size_t kPointStride = 16;

/// A cloud of 16-byte float points (x, y, z, intensity) over `bytes`, aliasing them, in `frame`, carrying
/// the stamp its script left.
inline sdk::PointCloud cloudOver(sdk::PayloadView bytes, Timestamp stamp = 0, std::string frame = "base_link") {
  sdk::PointCloud cloud;
  cloud.width = static_cast<uint32_t>(bytes.bytes.size() / kPointStride);
  cloud.point_step = kPointStride;
  cloud.row_step = cloud.point_step * cloud.width;
  cloud.frame_id = std::move(frame);
  cloud.timestamp_ns = stamp;
  using Datatype = sdk::PointField::Datatype;
  cloud.fields = {
      {"x", 0, Datatype::kFloat32, 1},
      {"y", 4, Datatype::kFloat32, 1},
      {"z", 8, Datatype::kFloat32, 1},
      {"intensity", 12, Datatype::kFloat32, 1}};
  cloud.data = bytes.bytes;
  cloud.anchor = bytes.anchor;
  return cloud;
}

/// `points` zeroed points.
inline sdk::PayloadView rawPoints(size_t points) {
  return sdk::makePayloadView(std::vector<uint8_t>(points * kPointStride, 0));
}

inline sdk::PointCloud makeCloud(size_t points, Timestamp stamp = 0, std::string frame = "base_link") {
  return cloudOver(rawPoints(points), stamp, std::move(frame));
}

/// `owner/<id>` reading `inputs`, declaring `outputs`.
inline RecipeGraph::Recipe onDemandRecipe(
    std::string id, std::vector<std::string> inputs, std::vector<scripting::OutputSpec> outputs) {
  RecipeGraph::Recipe recipe;
  recipe.owner_plugin = "owner";
  recipe.user_id = std::move(id);
  recipe.inputs = std::move(inputs);
  recipe.outputs = std::move(outputs);
  recipe.script = "local inputs = ...\nreturn {}\n";
  return recipe;
}

/// What the fake parser saw.
struct ParseProbe {
  std::shared_ptr<std::mutex> topic_lock = std::make_shared<std::mutex>();
  std::atomic<int> parses{0};
  std::thread::id thread;
  bool lock_held = false;
};

/// A parser of the `raw_points` schema: a payload of raw 16-byte points becomes a cloud aliasing it.
class RawPointsParser : public MessageParserPluginBase {
 public:
  explicit RawPointsParser(ParseProbe& probe) {
    sdk::SchemaHandler handler;
    handler.object_type = sdk::BuiltinObjectType::kPointCloud;
    handler.parse_object = [&probe](Timestamp ts, sdk::PayloadView payload) -> Expected<sdk::ObjectRecord> {
      probe.thread = std::this_thread::get_id();
      // Another thread can take the topic lock only when the parse's caller does not hold it.
      probe.lock_held = !std::async(std::launch::async, [&probe] {
                           const bool taken = probe.topic_lock->try_lock();
                           if (taken) {
                             probe.topic_lock->unlock();
                           }
                           return taken;
                         }).get();
      ++probe.parses;
      return sdk::ObjectRecord{ts, sdk::BuiltinObject(cloudOver(std::move(payload)))};
    };
    registerSchemaHandler("raw_points", std::move(handler));
  }
};

/// Removes every entry of `topic`.
inline void prune(ObjectStore& store, ObjectTopicId topic) {
  OwnedObjectWrite write;
  write.topic = topic;
  write.append = false;
  write.drop = [](Timestamp, SequentialUID) { return true; };
  std::vector<OwnedObjectWrite> writes;
  writes.push_back(std::move(write));
  ASSERT_TRUE(store.pushOwnedBundle(std::move(writes)).has_value());
}

/// The median, in µs, of `runs` calls of `work`.
template <class Work>
double medianMicros(int runs, const Work& work) {
  std::vector<double> times;
  for (int run = 0; run < runs; ++run) {
    const auto begin = std::chrono::steady_clock::now();
    work();
    times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count());
  }
  std::nth_element(times.begin(), times.begin() + runs / 2, times.end());
  return times[runs / 2];
}

/// The object of type `T` the store shows at `t` (its newest entry at or before), decoded.
template <class T>
T decodeAt(const ObjectStore& store, ObjectTopicId topic, Timestamp t) {
  const auto entry = store.latestAt(topic, t);
  if (!entry) {
    ADD_FAILURE() << "no entry at or before " << t;
    return {};
  }
  const auto type = sdk::BuiltinObjectTraits<T>::kType;
  auto object = deserializeBuiltinObject(type, entry->payload.bytes.data(), entry->payload.bytes.size());
  if (!object || object->template get<T>() == nullptr) {
    ADD_FAILURE() << "undecodable entry at " << t;
    return {};
  }
  return *object->template get<T>();
}

inline std::vector<Timestamp> timestampsOf(const ObjectStore& store, ObjectTopicId topic) {
  const EntryTimestampsView view = store.entryTimestamps(topic);
  return {view.begin(), view.end()};
}

}  // namespace PJ::test
