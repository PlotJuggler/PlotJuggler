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

/// Implementation struct — defined in incremental_engine.cpp, hidden from callers.
struct IncrementalEngineImpl;

// ---------------------------------------------------------------------------
// VarValue — universal column value type for transform I/O
// ---------------------------------------------------------------------------
// Engine storage kinds map as follows:
//   kFloat32, kFloat64        → double   (float32 widens losslessly)
//   kInt8 … kInt64, kBool     → int64_t  (sign-extend; bool → 0/1)
//   kUint64                   → uint64_t (lossless)
//   kString                   → std::string
using VarValue = std::variant<int64_t, uint64_t, double, std::string>;

// ---------------------------------------------------------------------------
// ISISOTransform — single-input / single-output transform
// ---------------------------------------------------------------------------
// SEQUENTIAL CONTRACT (fundamental):
//   The engine calls calculate() once per sample, strictly in ascending
//   timestamp order. Implementations may therefore accumulate state freely
//   in member variables between calls (e.g. previous value for derivative,
//   ring buffer for moving average, running sum for integral).
//   State persists across chunk boundaries — the engine never resets it
//   during incremental scheduling.
//   reset() is the only path that clears state; the engine calls it
//   exclusively before a full batch recompute.
class ISISOTransform {
 public:
  virtual ~ISISOTransform() = default;

  /// Clear all accumulated state. Called by IncrementalEngine before batch recompute.
  /// After reset(), the next calculate() call must behave as if no data has
  /// been seen (same as a freshly constructed instance).
  virtual void reset() {}

  /// Declare the StorageKind of the output column. Called once at registration.
  /// Default: kFloat64 (suitable for most numeric filters).
  /// Override to preserve integer types or produce strings.
  virtual StorageKind outputKind(StorageKind input_kind) const {
    (void)input_kind;
    return StorageKind::kFloat64;
  }

  /// Process one sample. Called in strictly ascending timestamp order.
  ///   time:      sample timestamp (nanoseconds since epoch)
  ///   input:     sample value decoded as VarValue
  ///   out_time:  output timestamp (written by callee; read by engine only when true)
  ///   out_value: output value   (written by callee; read by engine only when true)
  ///
  /// Returns true to emit a row, false to suppress (e.g. first row of derivative).
  ///
  /// out_time MAY differ from `time` — time-offset transforms and interpolation
  /// may produce output on a different time grid than their input.
  /// When true is returned, out_time must be >= all previously returned out_times.
  virtual bool calculate(PJ::Timestamp time, const VarValue& input, PJ::Timestamp& out_time, VarValue& out_value) = 0;

  /// True once the op has hit an unrecoverable error (e.g. a script runtime error)
  /// so a returned `false` from calculate() means FAILED, not "row suppressed". The
  /// engine checks this after a batch and surfaces it as a Status error instead of
  /// silently committing a truncated/empty output. Default: never fails.
  [[nodiscard]] virtual bool failed() const {
    return false;
  }
  /// Human-readable reason for `failed()`, or empty.
  [[nodiscard]] virtual const std::string& error() const {
    static const std::string kNone;
    return kNone;
  }
};

// ---------------------------------------------------------------------------
// IMIMOTransform — multi-input / multi-output transform
// ---------------------------------------------------------------------------
// SEQUENTIAL CONTRACT (fundamental, same as ISISOTransform):
//   The engine calls calculate() once per joined sample, strictly in ascending
//   timestamp order. State may be accumulated in member variables between calls.
//   reset() clears all state; called exclusively before batch recompute.
class IMIMOTransform {
 public:
  virtual ~IMIMOTransform() = default;

  /// Clear all accumulated state. Called by IncrementalEngine before batch recompute.
  virtual void reset() {}

  /// Declare output StorageKind for each output topic.
  /// Called once at registration with the input kinds (one per input topic).
  /// Return one StorageKind per output topic name passed to add_mimo_transform.
  virtual std::vector<StorageKind> outputKinds(PJ::Span<const StorageKind> input_kinds) const = 0;

  /// Process one joined sample. Called in strictly ascending timestamp order,
  /// only when ALL input topics have a sample at exactly `time`.
  ///   inputs[i]  = value from input topic i (in add_mimo_transform order).
  ///   out_time   = output timestamp (written by callee; read only when true).
  ///   output     = pre-allocated buffer (size == num output topics); fill in-place.
  ///                output[k] corresponds to output_topic_names[k] from add_mimo_transform.
  ///
  /// Returns true to emit a row; false to suppress.
  /// out_time MAY differ from `time`. When true is returned, out_time must be
  /// >= all previously returned out_times. All M output topics share this timestamp.
  virtual bool calculate(
      PJ::Timestamp time, PJ::Span<const VarValue> inputs, PJ::Timestamp& out_time, std::vector<VarValue>& output) = 0;

  /// Distinguish ordinary row suppression from an unrecoverable transform
  /// failure that invalidates the current staged batch.
  [[nodiscard]] virtual bool failed() const {
    return false;
  }

  /// Human-readable reason for failed(), or empty for a healthy transform.
  [[nodiscard]] virtual const std::string& error() const {
    static const std::string kNone;
    return kNone;
  }
};

// ---------------------------------------------------------------------------
// IncrementalEngine
// ---------------------------------------------------------------------------
class IncrementalEngine {
 public:
  /// Captured input-column metadata for one node. Kinds are stored with the
  /// columns because a reload can replace the schema behind a stable TopicId.
  struct InputBindingState {
    std::vector<std::size_t> columns;
    std::vector<StorageKind> kinds;
  };

  explicit IncrementalEngine(DataEngine& engine);
  ~IncrementalEngine();
  IncrementalEngine(const IncrementalEngine&) = delete;
  IncrementalEngine& operator=(const IncrementalEngine&) = delete;

  // ---- SISO ----------------------------------------------------------------
  // Creates one scalar output topic (StorageKind from op->outputKind()).
  // Returns error if:
  //   - input_topic_id does not exist
  //   - input_column_index is out of range (>= the input topic's column count)
  //   - output_topic_name already registered within output_dataset_id
  //
  // Topics created via DataWriter::registerScalarSeries (schema_id == 0)
  // are supported even before any data has been committed: the column layout
  // is stored in TopicStorage at registration time. Topics created via
  // DataWriter::register_topic with schema_id != 0 are always supported.
  // Returns error if the column layout cannot be determined (e.g. a topic
  // created with schema_id==0 via the low-level register_topic API with no
  // committed chunks and no stored column descriptors).
  //
  // input_column_index selects which leaf column of a (possibly multi-column)
  // input topic feeds the transform. Default 0 = the first/only column. The
  // index counts flattened leaf columns in DFS order (fixed arrays expanded
  // element-wise) — the same order DataReader and the chunk column layout use.
  [[nodiscard]] PJ::Expected<PJ::NodeId> addSisoTransform(
      PJ::TopicId input_topic_id, std::string output_topic_name, PJ::DatasetId output_dataset_id,
      std::unique_ptr<ISISOTransform> op, std::size_t input_column_index = 0);

  // ---- MIMO -----------------------------------------------------------------
  // A row is emitted only when ALL input topics share the exact same timestamp.
  // Creates output_topic_names.size() new topics (kinds from op->outputKinds()).
  //
  // input_columns selects which leaf column of each (possibly multi-column) input
  // topic feeds the transform — same flattened-leaf DFS order as the SISO
  // input_column_index. Empty (the default) means column 0 for every input; when
  // non-empty it MUST have exactly one entry per input topic. This is what lets a
  // single multi-column topic (e.g. a quaternion's x/y/z/w) feed several inputs.
  //
  // output_topic_group selects the output SHAPE:
  //   - empty (default): each output_topic_names[k] is its own scalar topic with a
  //     single "value" column — M topics.
  //   - non-empty: ONE topic named output_topic_group with M named columns, where
  //     output_topic_names[k] is the FIELD name of column k (so a quaternion->RPY
  //     transform materializes one "rpy" topic with roll/pitch/yaw fields instead of
  //     three separate topics). The calculate() contract is unchanged — its k-th
  //     result fills column k. outputTopics() then returns the single topic id.
  [[nodiscard]] PJ::Expected<PJ::NodeId> addMimoTransform(
      std::vector<PJ::TopicId> input_topic_ids, std::vector<std::string> output_topic_names,
      PJ::DatasetId output_dataset_id, std::unique_ptr<IMIMOTransform> op, std::vector<std::size_t> input_columns = {},
      std::string output_topic_group = {});

  // ---- Node management -----------------------------------------------------
  PJ::Status removeNode(PJ::NodeId id);
  [[nodiscard]] bool hasNode(PJ::NodeId id) const noexcept;

  // Returns output topic IDs: 1 for SISO, M for MIMO.
  [[nodiscard]] std::vector<PJ::TopicId> outputTopics(PJ::NodeId id) const;

  // Kahn's topological order (upstream → downstream).
  [[nodiscard]] std::vector<PJ::NodeId> topologicalOrder() const;

  // ---- Commit-cycle hook ---------------------------------------------------
  // Call after DataEngine::commitChunks() with the set of changed topic IDs.
  // Marks directly dependent nodes dirty.
  void onSourceCommitted(PJ::Span<const PJ::TopicId> changed_topics);

  // ---- Scheduling ----------------------------------------------------------
  // Run all dirty nodes in topological order (incremental path). A dirty node whose
  // inputs hold no chunk beyond its watermark is skipped without reading them.
  // Use for file/batch playback and tests. Equivalent to passing every
  // registered node to scheduleActive().
  PJ::Status scheduleAll();

  // Run only the specified nodes (and their transitive upstream dependencies)
  // that are dirty. Pass the set of nodes whose output topics are currently
  // visible in the UI to implement display-lazy scheduling.
  PJ::Status scheduleActive(const std::unordered_set<PJ::NodeId>& active_nodes);

  /// Time-sliced catch-up: advance one node over its not-yet-processed input for
  /// about `budget` of wall time, commit what it produced, and return. True once
  /// the node has consumed every committed input row, false while work remains.
  ///
  /// A step that leaves work behind makes the node *deferred* (see deferredNodes()):
  /// scheduleAll() and scheduleActive() skip it, so the caller owns it until a step
  /// returns true or an error, or the node is removed/replaced/recomputed (each of
  /// which finishes or drops the replay). Input committed meanwhile is consumed by
  /// later steps; each step's rows are committed at once, so readers see the output
  /// grow in time order, and the final output equals the synchronous replay's. A
  /// node that is neither dirty nor deferred (a scheduling pass would skip it,
  /// including one whose op already failed) reports true at once.
  ///
  /// The clock is read every few samples, so a step overshoots `budget` by at most
  /// a handful of calculate() calls. Only a SISO node over time-ordered input is
  /// sliced; a MIMO node, input whose chunks overlap in time, or a node awaiting a
  /// full recompute finishes in one (unbounded) step. On success, downstream nodes
  /// are marked dirty so the next pass (or a step of theirs) carries them along.
  [[nodiscard]] PJ::Expected<bool> materializeStep(PJ::NodeId node_id, std::chrono::nanoseconds budget);

  /// The deferred nodes (a materializeStep() stopped inside their input), in
  /// creation order: node ids are monotonic, so a producer precedes its consumers.
  [[nodiscard]] std::vector<PJ::NodeId> deferredNodes() const;

  /// The error of the node's own last run by any scheduling pass, step or recompute;
  /// empty while it succeeds (or for an unknown node). A failure downstream of it in
  /// a recompute cascade is recorded on the failing node only. A sticky-failed op keeps
  /// reporting it until a successful replay (e.g. after its op is replaced).
  [[nodiscard]] std::string nodeError(PJ::NodeId node_id) const;

  /// Input rows a node never consumed were evicted by retention (a straddling chunk's
  /// sub-floor rows count): the node resets and replays only what is retained, from `to`
  /// on. `from` is the node's feed watermark, the last input timestamp it processed
  /// (Timestamp::min when it had fed nothing: unbounded below); `to` is the first
  /// retained timestamp of the input it fell behind on (the retention floor when nothing
  /// is retained). Eviction of rows the node already consumed is no gap, nor is an
  /// input without retention (a file dataset). A time-sliced node stopped inside a chunk
  /// has consumed that chunk's rows up to its resume row only. A node whose op failed
  /// consumes nothing, but input arriving for it counts as consumed.
  struct Gap {
    PJ::Timestamp from = 0;
    PJ::Timestamp to = 0;
  };

  /// The retention gaps recorded for `node_id`, oldest first, bounded to the most
  /// recent 16 (a diagnostic trail, not an audit log). Empty for an unknown node or
  /// one that never gapped. Like nodeError(), does not take lockEngine() itself.
  [[nodiscard]] std::vector<Gap> gaps(PJ::NodeId node_id) const;

  /// Full history recompute of `node_id` and its downstream: detach the outputs, reset the
  /// transform and replay all input; a failed replay restores the detached outputs.
  PJ::Status recomputeBatch(PJ::NodeId node_id);

  /// Reset and replay the union of every root's downstream graph once in
  /// global topological order. Invalid roots are rejected before any output is
  /// changed; an empty root span is a successful no-op.
  PJ::Status recomputeBatch(PJ::Span<const PJ::NodeId> root_node_ids);

  // Replace a SISO node's transform op IN PLACE (same node id, same output topic
  // id) and fully recompute it plus every downstream node. The swap is
  // transactional: failure restores the old op and replays the old graph before
  // returning. Errors if the node is unknown, is MIMO, or op is null. The output
  // StorageKind is unchanged; the new op's values are coerced to it.
  [[nodiscard]] PJ::Status replaceSisoTransform(PJ::NodeId node_id, std::unique_ptr<ISISOTransform> op);

  /// MIMO counterpart of replaceSisoTransform, with the same stable topology,
  /// downstream replay, and rollback guarantees.
  [[nodiscard]] PJ::Status replaceMimoTransform(PJ::NodeId node_id, std::unique_ptr<IMIMOTransform> op);

  /// Capture the binding metadata currently installed on a node.
  [[nodiscard]] PJ::Expected<InputBindingState> inputBindingState(PJ::NodeId node_id) const;

  /// Validate replacement column indices against the node's current input
  /// topics and return a complete state without mutating the node.
  [[nodiscard]] PJ::Expected<InputBindingState> resolvedInputBindingState(
      PJ::NodeId node_id, const std::vector<std::size_t>& columns) const;

  /// Restore previously captured or resolved metadata without validating it
  /// against storage and without recomputing. The node is reset to dirty so the
  /// caller can replay after the surrounding datastore transaction is settled.
  [[nodiscard]] PJ::Status restoreInputBindingState(PJ::NodeId node_id, const InputBindingState& state);

 private:
  // *Locked workers assume the caller already holds engine_.lockEngine(). Public
  // scheduling, recompute, and binding-state methods acquire that lock internally;
  // callers must not nest them while holding a different DataEngine's lock.
  // Keeping the workers separate also avoids redundant recursive re-locks while
  // one transaction reads inputs, runs transforms, and commits derived outputs.
  PJ::Status scheduleActiveLocked(const std::unordered_set<PJ::NodeId>& active_nodes);
  PJ::Status recomputeBatchLocked(PJ::NodeId node_id);
  PJ::Status recomputeBatchLocked(PJ::Span<const PJ::NodeId> root_node_ids);
  /// One scheduling step of a node, shared by the scheduling passes and
  /// materializeStep(): a reset + full replay when its output or input needs one,
  /// else the incremental path, which stops at `deadline` (null: run to the end).
  /// A failed node counts as processed and is not deferred; a successful one marks
  /// its downstream nodes dirty. A node with no input chunk beyond its watermark (and
  /// nothing pending) returns at once, leaving its downstream alone.
  PJ::Status runNodeLocked(PJ::NodeId node_id, const std::chrono::steady_clock::time_point* deadline);
  /// Shared rollback contract for in-place node mutations: replay the mutated
  /// node's graph; on failure run `revert` and replay again, composing
  /// "<op>: <err>; rollback failed: <err2>" if even the rollback replay fails.
  PJ::Status recomputeOrRevertLocked(PJ::NodeId node_id, std::string_view op_name, const std::function<void()>& revert);

  DataEngine& engine_;
  PJ::NodeId next_node_id_ = 1;
  std::unique_ptr<IncrementalEngineImpl> impl_;
};

}  // namespace PJ
