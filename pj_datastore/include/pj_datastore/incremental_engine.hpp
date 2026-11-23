#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/span.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/column_buffer.hpp"

namespace PJ {

class DataEngine;

struct IncrementalEngineImpl;

/// A transform input or output value. Storage kinds map as: float32/float64 → double, int8..int64 and bool → int64_t
/// (bool as 0/1), uint64 → uint64_t, string → std::string; all lossless.
using VarValue = std::variant<int64_t, uint64_t, double, std::string>;

/// Single-input, single-output transform. calculate() sees every sample once, in ascending timestamp order, and its
/// state carries across chunks: the engine clears it only through reset(), right before a full replay (a late input
/// commit, retention evicting unconsumed input, a recompute).
class ISISOTransform {
 public:
  virtual ~ISISOTransform() = default;

  /// Afterwards calculate() must behave as on a fresh instance.
  virtual void reset() {}

  /// Called once at registration.
  virtual StorageKind outputKind(StorageKind input_kind) const {
    (void)input_kind;
    return StorageKind::kFloat64;
  }

  /// `time` in nanoseconds. Returns true to emit `out_time`/`out_value`, false to emit nothing (a derivative's first
  /// sample). `out_time` may differ from `time` but must never go below an earlier emitted one.
  virtual bool calculate(PJ::Timestamp time, const VarValue& input, PJ::Timestamp& out_time, VarValue& out_value) = 0;

  /// True after an unrecoverable error (a script runtime error): calculate()'s false then means failed, and the
  /// engine reports an error instead of committing a truncated output.
  [[nodiscard]] virtual bool failed() const {
    return false;
  }
  [[nodiscard]] virtual const std::string& error() const {
    static const std::string kNone;
    return kNone;
  }
};

/// Multi-input, multi-output transform, with ISISOTransform's ordering and reset contract.
class IMIMOTransform {
 public:
  virtual ~IMIMOTransform() = default;

  virtual void reset() {}

  /// Called once at registration, one input kind per input topic; returns one kind per output name given to
  /// addMimoTransform().
  virtual std::vector<StorageKind> outputKinds(PJ::Span<const StorageKind> input_kinds) const = 0;

  /// Called only when every input has a sample at exactly `time`; `inputs[i]` is input topic i. Fill `output`
  /// (pre-sized, `output[k]` is output k) and return true to emit; all outputs share `out_time`, which must never go
  /// below an earlier emitted one.
  virtual bool calculate(
      PJ::Timestamp time, PJ::Span<const VarValue> inputs, PJ::Timestamp& out_time, std::vector<VarValue>& output) = 0;

  /// As ISISOTransform::failed().
  [[nodiscard]] virtual bool failed() const {
    return false;
  }

  [[nodiscard]] virtual const std::string& error() const {
    static const std::string kNone;
    return kNone;
  }
};

/// The derived-series DAG over a DataEngine: nodes run transforms on committed input and commit their output topics.
/// Design: docs/ARCHITECTURE.md "Derived series (IncrementalEngine)".
class IncrementalEngine {
 public:
  /// A node's input columns. The kinds are kept because a reload can change the schema behind a stable TopicId.
  struct InputBindingState {
    std::vector<std::size_t> columns;
    std::vector<StorageKind> kinds;
  };

  explicit IncrementalEngine(DataEngine& engine);
  ~IncrementalEngine();
  IncrementalEngine(const IncrementalEngine&) = delete;
  IncrementalEngine& operator=(const IncrementalEngine&) = delete;

  // ---- SISO ----------------------------------------------------------------
  /// Creates one scalar output topic. `input_column_index` counts flattened leaf columns in DFS order (fixed arrays
  /// expanded), the order DataReader and chunks use. Fails for an unknown input, an out-of-range column, an output
  /// name already taken in the dataset, or an input whose column layout is unknown: a schema-less topic needs
  /// committed chunks or stored column descriptors (registerScalarSeries() stores them).
  [[nodiscard]] PJ::Expected<PJ::NodeId> addSisoTransform(
      PJ::TopicId input_topic_id, std::string output_topic_name, PJ::DatasetId output_dataset_id,
      std::unique_ptr<ISISOTransform> op, std::size_t input_column_index = 0);

  // ---- MIMO -----------------------------------------------------------------
  /// Emits a row only when all inputs share a timestamp. `input_columns` picks each input's leaf column (as in
  /// addSisoTransform()); empty means column 0 everywhere, otherwise one entry per input, so one multi-column topic
  /// (a quaternion's x/y/z/w) can feed several inputs. With `output_topic_group` empty, each output name is its own
  /// scalar topic; otherwise there is one topic of that name whose column k is named `output_topic_names[k]`, and
  /// outputTopics() returns that one id.
  [[nodiscard]] PJ::Expected<PJ::NodeId> addMimoTransform(
      std::vector<PJ::TopicId> input_topic_ids, std::vector<std::string> output_topic_names,
      PJ::DatasetId output_dataset_id, std::unique_ptr<IMIMOTransform> op, std::vector<std::size_t> input_columns = {},
      std::string output_topic_group = {});

  // ---- Node management -----------------------------------------------------
  PJ::Status removeNode(PJ::NodeId id);
  [[nodiscard]] bool hasNode(PJ::NodeId id) const noexcept;

  /// 1 id for SISO, M for MIMO (1 with an output group).
  [[nodiscard]] std::vector<PJ::TopicId> outputTopics(PJ::NodeId id) const;

  /// Upstream before downstream.
  [[nodiscard]] std::vector<PJ::NodeId> topologicalOrder() const;

  // ---- Commit-cycle hook ---------------------------------------------------
  /// Marks the nodes that read `changed_topics` dirty; pass DataEngine::commitChunks()'s result.
  void onSourceCommitted(PJ::Span<const PJ::TopicId> changed_topics);

  // ---- Scheduling ----------------------------------------------------------
  /// scheduleActive() over every node. A dirty node with no input chunk past its watermark is skipped unread.
  PJ::Status scheduleAll();

  /// Runs the dirty nodes among `active_nodes` and their upstream (display-lazy scheduling: pass the nodes whose
  /// outputs are visible).
  PJ::Status scheduleActive(const std::unordered_set<PJ::NodeId>& active_nodes);

  /// Advances one node for about `budget`, commits what it produced and returns true once all committed input is
  /// consumed. While it returns false the node is deferred: scheduling passes skip it and the caller owns it until a
  /// step returns true or an error, or the node is removed, replaced or recomputed. Each step's rows are committed at
  /// once, so readers watch the output grow in time order; the result equals a synchronous replay. A node that is
  /// neither dirty nor deferred returns true at once.
  ///
  /// The clock is read every few samples, so a step overshoots by a few calculate() calls. Only a SISO node over
  /// time-ordered input is sliced; a MIMO node, overlapping input chunks or a pending full recompute run in one
  /// unbounded step. Success marks downstream nodes dirty.
  [[nodiscard]] PJ::Expected<bool> materializeStep(PJ::NodeId node_id, std::chrono::nanoseconds budget);

  /// Nodes a materializeStep() left mid-input, in creation order (ids grow, so producers come before consumers).
  [[nodiscard]] std::vector<PJ::NodeId> deferredNodes() const;

  /// The error of the node's own last run (pass, step or recompute); empty after a success. In a recompute cascade
  /// only the failing node records it. A failed op keeps reporting until a successful replay. Does not lock.
  [[nodiscard]] std::string nodeError(PJ::NodeId node_id) const;

  /// Input the node never consumed was evicted by retention, so it reset and replayed only what is retained.
  /// `from`: the last input timestamp it processed (Timestamp::min if none). `to`: the first retained timestamp of
  /// that input (the retention floor if nothing is retained). Evicting rows already consumed is no gap; a time-sliced
  /// node stopped inside a chunk has consumed it only up to its resume row; input reaching a failed node counts as
  /// consumed.
  struct Gap {
    PJ::Timestamp from = 0;
    PJ::Timestamp to = 0;
  };

  /// The node's most recent 16 gaps, oldest first (a diagnostic trail). Does not lock.
  [[nodiscard]] std::vector<Gap> gaps(PJ::NodeId node_id) const;

  /// Resets and replays `node_id` and its downstream over all input; a failed replay restores the previous outputs.
  PJ::Status recomputeBatch(PJ::NodeId node_id);

  /// Replays the union of the roots' downstream graphs once, in topological order. Invalid roots are rejected
  /// before any output changes.
  PJ::Status recomputeBatch(PJ::Span<const PJ::NodeId> root_node_ids);

  /// Swaps the op in place (same node and output ids) and replays the node and its downstream; on failure the old op
  /// and outputs come back. The output kind stays; the new op's values are coerced to it, and an op whose kind
  /// switches between text and numbers is refused (the coercion would lose every value).
  [[nodiscard]] PJ::Status replaceSisoTransform(PJ::NodeId node_id, std::unique_ptr<ISISOTransform> op);

  /// As replaceSisoTransform().
  [[nodiscard]] PJ::Status replaceMimoTransform(PJ::NodeId node_id, std::unique_ptr<IMIMOTransform> op);

  [[nodiscard]] PJ::Expected<InputBindingState> inputBindingState(PJ::NodeId node_id) const;

  /// Validates `columns` against the node's current inputs without changing the node.
  [[nodiscard]] PJ::Expected<InputBindingState> resolvedInputBindingState(
      PJ::NodeId node_id, const std::vector<std::size_t>& columns) const;

  /// Installs `state` unvalidated and marks the node dirty, without replaying: the caller replays once the
  /// surrounding datastore transaction settles.
  [[nodiscard]] PJ::Status restoreInputBindingState(PJ::NodeId node_id, const InputBindingState& state);

 private:
  // The *Locked workers expect the caller to hold engine_.lockEngine(); the public methods take it. Calling a public
  // method while holding another DataEngine's lock risks a lock-order inversion.
  PJ::Status scheduleActiveLocked(const std::unordered_set<PJ::NodeId>& active_nodes);
  PJ::Status recomputeBatchLocked(PJ::NodeId node_id);
  PJ::Status recomputeBatchLocked(PJ::Span<const PJ::NodeId> root_node_ids);
  // A full replay when the output or input needs one, else the incremental path up to `deadline` (null: to the end).
  // A failed node counts as processed and is not deferred; success marks downstream dirty. With no input past the
  // watermark it returns at once and leaves downstream alone.
  PJ::Status runNodeLocked(PJ::NodeId node_id, const std::chrono::steady_clock::time_point* deadline);
  // Replays the node's graph; on failure runs `revert` and replays again ("<op>: <err>; rollback failed: <err2>" if
  // that fails too).
  PJ::Status recomputeOrRevertLocked(PJ::NodeId node_id, std::string_view op_name, const std::function<void()>& revert);

  DataEngine& engine_;
  PJ::NodeId next_node_id_ = 1;
  std::unique_ptr<IncrementalEngineImpl> impl_;
};

}  // namespace PJ
