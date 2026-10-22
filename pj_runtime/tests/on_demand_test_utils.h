// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// What the on-demand tests (ResultHistory, InputStager, NodeScheduler, the session wiring) share: point clouds,
// recipes whose scripts never run, a fake script run, a parser over raw points, and readers of the store.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pj_base/buffer_anchor.hpp"
#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/builtin/scene_entities.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/NodeScheduler.h"
#include "pj_runtime/OnDemandService.h"
#include "pj_runtime/RecipeGraph.h"

namespace PJ::test {

inline constexpr size_t kPointStride = 16;

/// Registers in `dataset` a topic `name` whose one field, `state`, is a string, as a parser's string field is.
inline TopicId registerStringTopic(DataWriter& writer, DatasetId dataset, const std::string& name) {
  const auto schema =
      writer.registerSchema(name + "/schema", makeStruct("state", {makePrimitive("state", PrimitiveType::kString)}));
  EXPECT_TRUE(schema.has_value()) << schema.error();
  TopicDescriptor descriptor;
  descriptor.name = name;
  descriptor.schema_id = schema ? *schema : SchemaId{};
  const auto topic = writer.registerTopic(dataset, descriptor);
  EXPECT_TRUE(topic.has_value()) << topic.error();
  return topic ? *topic : TopicId{};
}
/// Writes `value` into `topic`'s string field at `t`; the caller commits.
inline void appendString(DataWriter& writer, TopicId topic, Timestamp t, std::string_view value) {
  EXPECT_TRUE(writer.beginRow(topic, t).has_value());
  writer.set(topic, 0, value);
  EXPECT_TRUE(writer.finishRow(topic).has_value());
}
/// Writes a row at `t` whose string field is null; the caller commits.
inline void appendNullString(DataWriter& writer, TopicId topic, Timestamp t) {
  EXPECT_TRUE(writer.beginRow(topic, t).has_value());
  writer.setNull(topic, 0);
  EXPECT_TRUE(writer.finishRow(topic).has_value());
}

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

/// `onDemandRecipe` in Python: its script defines `evaluate` and returns nothing (a fake run ignores it).
inline RecipeGraph::Recipe pythonRecipe(
    std::string id, std::vector<std::string> inputs, std::vector<scripting::OutputSpec> outputs) {
  RecipeGraph::Recipe recipe = onDemandRecipe(std::move(id), std::move(inputs), std::move(outputs));
  recipe.language = "python";
  recipe.script = "def evaluate(inputs, params):\n    return {}\n";
  return recipe;
}

/// The thread of `pool`'s Python lane, where a task submitted to it runs; publishes everything submitted (GUI thread).
inline std::thread::id pythonLaneThread(EnginePool& pool) {
  auto seen = std::make_shared<std::thread::id>();
  EnginePool::Task task;
  task.compute = [seen](const EnginePool::CancelToken&) { *seen = std::this_thread::get_id(); };
  task.publish = [](const EnginePool::Outcome&) {};
  pool.submit(std::move(task), EnginePool::Lane::kPython);
  pool.waitIdle();
  return *seen;
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

/// A lazy fetch of four raw points whose payload deleter (plugin code in production) counts itself in `late_deletes`
/// when it runs after `library`, the parser's, has unloaded.
inline LazyCallback lazyPointsNotingUnload(std::weak_ptr<int> library, std::shared_ptr<std::atomic<int>> late_deletes) {
  auto bytes = std::make_shared<const std::vector<uint8_t>>(4 * kPointStride, 0);
  return [bytes, library, late_deletes]() -> std::optional<sdk::PayloadView> {
    return sdk::PayloadView{
        {bytes->data(), bytes->size()}, sdk::BufferAnchor(bytes.get(), [bytes, library, late_deletes](const void*) {
          *late_deletes += library.expired() ? 1 : 0;
        })};
  };
}

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
  const auto middle = times.begin() + runs / 2;
  std::nth_element(times.begin(), middle, times.end());
  return *middle;
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

/// The first point-cloud input, or null.
inline const sdk::PointCloud* firstCloud(Span<const scripting::NamedValue> inputs) {
  for (const scripting::NamedValue& input : inputs) {
    if (const auto* object = std::get_if<sdk::BuiltinObject>(&input.value)) {
      if (const auto* cloud = object->get<sdk::PointCloud>()) {
        return cloud;
      }
    }
  }
  return nullptr;
}

/// A script run that records where and what it ran. Each declared point-cloud output is its first cloud input
/// sharing its points (a pass-through), its frame named after the run's query time; a SceneEntities output holds
/// `entities(query time)`; a number output is that time. Set the knobs while nothing runs.
class FakeRun {
 public:
  struct Record {
    Timestamp query_time = 0;
    std::thread::id thread;
    const uint8_t* input_points = nullptr;  ///< the first cloud input's points
    std::shared_ptr<const pj::tf::TransformSnapshot> tf;
    bool cancelled = false;            ///< seen once the run was let through
    bool slow = false;                 ///< its params mention "slow"
    std::vector<std::string> strings;  ///< its string inputs, in input order
    bool has_image_decoder = false;    ///< the run was given an image decoder
  };

  bool gated = false;                       ///< each run waits for `allow`
  std::chrono::milliseconds delay{0};       ///< how long a run computes, polling its cancellation
  std::chrono::milliseconds slow_delay{0};  ///< ... instead, for a run whose params mention "slow"
  bool observe_clock = false;               ///< the run reports it read the query clock
  bool observe_tf = false;                  ///< ... and the TF tree
  std::string fail;                         ///< non-empty: the run fails with this message
  std::optional<double> fail_on_number;     ///< the run fails ("boom") when a number input equals this
  std::vector<Timestamp> fail_at;           ///< ... and at these query times
  std::vector<std::string> unavailable;     ///< outputs it marks unavailable ("no points")
  bool empty_clouds = false;                ///< its cloud outputs are empty (valid, D25)
  /// What its SceneEntities outputs hold at a query time; empty: no entity.
  std::function<std::vector<sdk::SceneEntity>(Timestamp)> entities;
  std::string text = "ok";  ///< its string outputs' value
  bool real = false;        ///< runs the script for real (`scripting::runOnDemand`), recording nothing

  NodeScheduler::RunFunction function() {
    return
        [this](
            const scripting::CompiledOnDemandScript& script, Span<const scripting::NamedValue> inputs,
            std::string_view params, Span<const scripting::OutputSpec> outputs, const scripting::RunOptions& options) {
          return real ? scripting::runOnDemand(script, inputs, params, outputs, options)
                      : run(inputs, params, outputs, options);
        };
  }

  void allow(int runs) {
    const std::lock_guard<std::mutex> lock(mutex_);
    permits_ += runs;
    cv_.notify_all();
  }
  /// Waits until `runs` runs have started; false after 5 s.
  [[nodiscard]] bool waitStarted(int runs) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(5), [&] { return started_ >= runs; });
  }
  [[nodiscard]] int started() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return started_;
  }
  [[nodiscard]] int finished() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(records_.size());
  }
  [[nodiscard]] std::vector<Record> records() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return records_;
  }
  /// The query times of the runs, in the order they started; `records` keeps the order they finished, which two runs
  /// let through by one `allow` take in any order.
  [[nodiscard]] std::vector<Timestamp> startedQueryTimes() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return started_query_times_;
  }

 private:
  Expected<scripting::ScriptBundle, scripting::ScriptError> run(
      Span<const scripting::NamedValue> inputs, std::string_view params, Span<const scripting::OutputSpec> outputs,
      const scripting::RunOptions& options) {
    const bool slow = params.find("slow") != std::string_view::npos;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ++started_;
      started_query_times_.push_back(options.query_time);
      cv_.notify_all();
      // A test that forgets to let a run through fails instead of hanging.
      if (gated && cv_.wait_for(lock, std::chrono::seconds(5), [this] { return permits_ > 0; })) {
        --permits_;
      }
    }
    const auto cancelled = [&options] { return options.cancelled && options.cancelled(); };
    for (const auto until = std::chrono::steady_clock::now() + (slow ? slow_delay : delay);
         std::chrono::steady_clock::now() < until && !cancelled();) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const sdk::PointCloud* input = firstCloud(inputs);
    Record record{
        options.query_time,
        std::this_thread::get_id(),
        input != nullptr ? input->data.data() : nullptr,
        options.tf,
        cancelled(),
        slow,
        {},
        options.image_decoder != nullptr};
    for (const scripting::NamedValue& value : inputs) {
      if (const auto* string = std::get_if<std::string>(&value.value)) {
        record.strings.push_back(*string);
      }
    }
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      records_.push_back(record);
      cv_.notify_all();
    }
    if (record.cancelled) {
      return unexpected(scripting::ScriptError{"script cancelled"});
    }
    if (!fail.empty()) {
      return unexpected(scripting::ScriptError{fail, observe_clock, observe_tf});
    }
    if (std::find(fail_at.begin(), fail_at.end(), options.query_time) != fail_at.end()) {
      return unexpected(scripting::ScriptError{"boom"});
    }
    for (const scripting::NamedValue& value : inputs) {
      if (const auto* number = std::get_if<double>(&value.value); number != nullptr && *number == fail_on_number) {
        return unexpected(scripting::ScriptError{"boom"});
      }
    }
    scripting::ScriptBundle bundle;
    bundle.observed_query_clock = observe_clock;
    bundle.observed_tf = observe_tf;
    for (const scripting::OutputSpec& output : outputs) {
      if (std::find(unavailable.begin(), unavailable.end(), output.name) != unavailable.end()) {
        bundle.outputs.push_back({.name = output.name, .value = scripting::Unavailable{"no points"}});
      } else if (output.type == scripting::OutputType::kNumber) {
        bundle.outputs.push_back({.name = output.name, .value = static_cast<double>(options.query_time)});
      } else if (output.type == scripting::OutputType::kString) {
        bundle.outputs.push_back({.name = output.name, .value = text});
      } else if (output.object_type == sdk::BuiltinObjectType::kSceneEntities) {
        sdk::SceneEntities scene;
        if (entities) {
          scene.entities = entities(options.query_time);
        }
        bundle.outputs.push_back({.name = output.name, .value = sdk::BuiltinObject(std::move(scene))});
      } else {
        sdk::PointCloud cloud = input != nullptr && !empty_clouds ? *input : makeCloud(0);
        cloud.frame_id = std::to_string(options.query_time);
        bundle.outputs.push_back({.name = output.name, .value = sdk::BuiltinObject(std::move(cloud))});
      }
    }
    return bundle;
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  int permits_ = 0;
  int started_ = 0;
  std::vector<Timestamp> started_query_times_;
  std::vector<Record> records_;
};

/// Sets the run of every on-demand service built while it lives (`OnDemandService::setRunForTest`).
struct ScopedRun {
  explicit ScopedRun(FakeRun& fake) {
    OnDemandService::setRunForTest(fake.function());
  }
  ~ScopedRun() {
    OnDemandService::setRunForTest({});
  }
  ScopedRun(const ScopedRun&) = delete;
  ScopedRun& operator=(const ScopedRun&) = delete;
};

inline std::vector<Timestamp> timestampsOf(const ObjectStore& store, ObjectTopicId topic) {
  const EntryTimestampsView view = store.entryTimestamps(topic);
  return {view.begin(), view.end()};
}

}  // namespace PJ::test
