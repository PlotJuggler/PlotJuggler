// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QObject>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_runtime/GridRunner.h"
#include "pj_runtime/RecipeGraph.h"

namespace PJ {

/// The plottable series of the on-demand nodes (REQUIREMENTS D10; docs/derived_objects/ARCHITECTURE.md §15): a node
/// that is not pinned, reads an object (a topic or an upstream's output) and declares a number output writes one
/// `DataEngine` topic named like its key, with one float64 column per number output, in declaration order. Its rows
/// sit at the node's object clock (`GridRunner::submitRows`): computed in chunks of `kInstantsPerChunk` instants, each
/// one `GridRunner` job (one reservation in the budget's shared line, so the cursor's runs pass between chunks), and
/// written at the chunk's end, the first chunk of a replay after the topic was emptied.
///
/// At most `kMaxRunningChunks` chunks of all series are in the runner at once, a cancelled one counted until its run
/// lets the pool thread go; the other wanted series wait their turn in a FIFO here, outside the budget's line, so a
/// playhead request waits for at most that many chunks (each `GridRunner::kRowsMaxMillis` long at most) however many
/// series compute (D30). A series whose chunk ended goes to the back, and the next chunk starts on a later event-loop
/// turn, so a chunk whose every instant is a gap (staged and skipped without a run) never chains into the next one
/// inside the same turn.
/// ponytail: one chunk at a time session-wide, so series never compute in parallel on idle pool threads; reserving per
/// run in `GridRunner` would let them.
///
/// Demand-gated: a series computes only while it is wanted, i.e. its topic is displayed (`setDemand`), a request is
/// open (`request`, until its replay completes) or it is a preview (`RecipeGraph::Recipe::ephemeral`, its plugin's
/// trial); otherwise it stops, keeping its rows, and resumes from its watermark when wanted again. An edit, an upstream
/// change or a dataset storage change (`invalidate`, `suspend`) empties the topic at once and replays from the start
/// when wanted; a live tick (`appendLive`) continues after the watermark, up to the live edge while the stream lasts,
/// unless an object input entry arrived before the watermark (out of order), which empties the topic and replays it. A
/// removal retires the topic; a restore parks it (`park`), and the same key reinstalled with the same columns in the
/// same dataset adopts it.
///
/// Visible range first (`setVisibleStart`): when a plot of the dataset starts its view after the watermark, the series
/// computes [visible start, end] first, then backfills its clock up to the visible start; at most these two ranges,
/// each with its own watermark, and one split per replay. While it has both, a view starting at or before the
/// backfill's end computes the backfill first, else the forward range, which a live tick continues; a later pan only
/// reorders what is left.
///
/// THREADING: GUI thread only; what runs on the pool is `GridRunner`'s. The engine, the graph and the runner must
/// outlive this object, whose destruction cancels its chunks.
class SeriesRunner {
 public:
  static constexpr std::size_t kInstantsPerChunk = 64;
  static constexpr std::size_t kMaxRunningChunks = 1;
  /// The shortest time between two announcements of a replay's new rows; its last chunk always announces.
  static constexpr std::chrono::milliseconds kAnnounceInterval{66};

  struct State {
    TopicId topic = 0;
    uint64_t rows = 0;    ///< rows written since the topic was last emptied
    uint64_t failed = 0;  ///< gaps among the instants evaluated
    uint64_t done = 0;    ///< instants evaluated since the topic was last emptied, rows and gaps
    /// `done` plus the clock instants left when the last chunk was submitted: exact for one object input, an upper
    /// bound when several share stamps.
    uint64_t total = 0;
    /// A chunk reached the end of the clock since the topic was emptied; a live append keeps it (`replaying` then
    /// reports the append), so a replay is `replaying && !complete`.
    bool complete = false;
    bool replaying = false;   ///< a chunk computes, or waits for its turn or its room
    bool stopped = false;     ///< `stop` ended it: its rows stay; a request, an edit or a reload starts it again
    std::string first_error;  ///< the first gap's reason, or why a chunk could not run

    bool operator==(const State&) const = default;
  };
  /// Rows were written to, or the topic emptied or retired, on these topics: plots and the catalog re-read them.
  using TopicsChanged = std::function<void(std::vector<TopicId>)>;
  /// `state(key)` changed, or the series is gone (nullopt). Called after the change, from inside the call that made
  /// it or a later event-loop turn; it may call back into the runner.
  using StateChanged = std::function<void(const std::string& key)>;
  /// The end of a live dataset's clock for `node`: the oldest of the newest samples of the raw inputs (object and
  /// scalar) of `node` and of its upstreams; nullopt while one of them has none.
  using LiveEdge = std::function<std::optional<Timestamp>(const RecipeGraph::Node& node)>;

  /// What the owner wires in; each may be empty (no live edge: a live clock is not capped).
  struct Hooks {
    TopicsChanged topics_changed;
    StateChanged state_changed;
    LiveEdge live_edge;
  };

  SeriesRunner(DataEngine& engine, const RecipeGraph& graph, GridRunner& runner, Hooks hooks);
  ~SeriesRunner();

  SeriesRunner(const SeriesRunner&) = delete;
  SeriesRunner& operator=(const SeriesRunner&) = delete;

  /// Whether `node` has a series: not pinned, an object input, a number output.
  [[nodiscard]] static bool isSeries(const RecipeGraph::Node& node);
  /// Why installing `node` would fail: a topic of its key's name in its dataset that is not its own series' (SER-12).
  /// Changes nothing; ok for a node with no series.
  [[nodiscard]] PJ::Status check(const RecipeGraph::Node& node) const;
  /// After `node`'s new revision, `demanded` when its topic name is displayed already: its series (or the parked one)
  /// keeps its topic when the dataset and the columns match, else retires it and creates one; the topic is emptied
  /// and replayed when wanted. An open request carries over, so the new revision computes to the end. A node with no
  /// series retires the one it had. A topic it cannot create is the state's `first_error`.
  void install(const RecipeGraph::Node& node, bool demanded);
  /// The node is gone: cancels its chunk and retires its topic.
  void remove(std::string_view key);
  /// A restore removed the node and may install it again: cancels its chunk and keeps its topic for `install`.
  void park(std::string_view key);
  /// Retires the parked topics no install adopted.
  void sweepParked();

  /// The topic names of `dataset` something displays: its series whose key is among them are demanded.
  void setDemand(DatasetId dataset, const std::unordered_set<std::string>& names);
  [[nodiscard]] bool isDemanded(std::string_view key) const;
  /// An explicit request: computes to the end of the clock from the watermark even while not displayed, then lapses.
  /// Ends a stop.
  void request(std::string_view key);
  /// Ends the replay, keeping the rows written; false when nothing computes.
  bool stop(std::string_view key);
  /// What the series' rows mean changed (an upstream's revision, a re-created input): empties the topic and replays.
  void invalidate(std::string_view key);
  /// Before a change of `dataset`'s storage: cancels its chunks and empties its topics. Pair with `resume`; the last
  /// one replays the wanted series.
  void suspend(DatasetId dataset);
  void resume(DatasetId dataset);
  /// After a live tick of `dataset`: each wanted series that is not computing continues after its watermark, up to
  /// its live edge (`Hooks::live_edge`) from now on, so an input that lags the others appends instead of replaying.
  void appendLive(DatasetId dataset);
  /// `dataset`'s stream ended: its wanted series compute the tail past their live edge.
  void endLive(DatasetId dataset);
  /// Where the plots of `dataset` start their view (raw): read when the series' next chunk is chosen. While the dataset
  /// is live, a start past the live edge splits nothing.
  void setVisibleStart(DatasetId dataset, Timestamp start);
  /// A whole-series read of `topic` (a plugin's `read_series_arrow`): an explicit request of its series (decision 2),
  /// which the reader sees grow from the rows so far.
  void requestTopic(TopicId topic);

  [[nodiscard]] std::optional<State> state(std::string_view key) const;
  /// The series topics, only the previews' when `ephemeral_only` (the catalog hides those).
  [[nodiscard]] std::unordered_set<TopicId> topics(bool ephemeral_only) const;

 private:
  /// The second computed range of a split series: its clock from `watermark` up to `end`.
  struct Backfill {
    std::optional<Timestamp> watermark;
    SequentialUID seen;
    Timestamp end = 0;  ///< the last instant it covers: the visible start of the split, minus one
  };
  struct Series {
    DatasetId dataset = 0;
    std::vector<std::string> columns;
    bool ephemeral = false;
    bool demanded = false;
    bool requested = false;
    std::optional<Timestamp> watermark;  ///< the last instant evaluated since the topic was emptied
    SequentialUID seen;                  ///< the `GridRunner::Rows::seen` of the chunk that evaluated it
    std::optional<Backfill> backfill;    ///< while split; the watermark above is then the forward range's
    bool was_split = false;              ///< since the topic was emptied: a later view start only reorders
    State state;
    std::shared_ptr<GridRunner::Job> chunk;
    std::chrono::steady_clock::time_point announced;
    std::optional<State> notified;  ///< the state `Hooks::state_changed` last reported
  };

  [[nodiscard]] bool wanted(const Series& series) const;
  /// A chunk computes, or the series waits its turn while wanted.
  [[nodiscard]] bool computing(std::string_view key, const Series& series) const;
  [[nodiscard]] bool queued(std::string_view key) const;
  /// Queues a wanted series that computes nothing for its next chunk, then `schedule`s.
  void pump(const std::string& key);
  /// Submits the queued series' chunks, in turn, while fewer than `kMaxRunningChunks` run.
  void schedule();
  /// Submits the next chunk of a wanted series that computes nothing.
  void submit(const std::string& key);
  /// Splits `series`, once per replay, at its dataset's visible start when that starts after its watermark (and, live,
  /// at or before `live_edge`): the watermark moves to the visible start, the backfill keeps the old one.
  void split(Series& series, std::optional<Timestamp> live_edge) const;
  /// GUI thread: writes a chunk's rows (the backfill's when `backfill`) and queues the series again; the next chunk
  /// starts on a later turn.
  void collect(const std::string& key, bool backfill, GridRunner::Rows rows);
  /// Cancels the chunk, empties the topic and resets the counts, the watermark and the split.
  void reset(Series& series);
  /// Cancels the chunk; one whose run still holds its pool thread counts in `draining_` until the pool lets it go.
  void cancel(Series& series);
  void retire(TopicId topic);
  /// Reports, through `Hooks::state_changed`, `gone_` and each series whose state differs from the one last reported.
  void notify();

  DataEngine& engine_;
  const RecipeGraph& graph_;
  GridRunner& runner_;
  Hooks hooks_;
  std::map<std::string, Series, std::less<>> series_;
  std::map<std::string, Series, std::less<>> parked_;
  std::map<DatasetId, int> holds_;      ///< `suspend` minus `resume`, per dataset
  std::unordered_set<DatasetId> live_;  ///< `appendLive` until `endLive`: their clocks end at the live edge
  /// ponytail: one visible start per dataset, the last plot to move wins; per plot if two plots of one dataset
  /// disagree.
  std::map<DatasetId, Timestamp> visible_start_;
  std::deque<std::string> waiting_;  ///< the series waiting their turn, each key once; checked again when it comes
  /// Cancelled chunks whose run still holds its pool thread and its room: they count against `kMaxRunningChunks`.
  std::size_t draining_ = 0;
  std::vector<std::string> gone_;  ///< the series removed since the last `notify`
  /// Expires with the runner: a cancelled chunk's release arriving later does nothing.
  std::shared_ptr<const int> lifetime_ = std::make_shared<const int>(0);
  /// The receiver of the posted `schedule`s, declared last: destroyed first, it drops one still posted.
  QObject context_;
};

}  // namespace PJ
