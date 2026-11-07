// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/NodeScheduler.h"

#include <algorithm>
#include <utility>

#include "OnDemandRun.h"
#include "pj_datastore/saturating_time.hpp"

namespace PJ {

using NodeStatus = NodeScheduler::NodeState::Status;

/// One run: what the worker reads, written on the GUI thread before the submission, and what it writes back,
/// read on the GUI thread at the publication. Destroyed on the GUI thread (the pool's closures die there).
struct NodeScheduler::Job {
  std::string key;
  uint64_t revision = 0;
  uint64_t epoch = 0;
  Timestamp requested_t = 0;
  bool query_time = true;                           ///< the recipe's: the run's clock is the requested time
  EnginePool::CancelToken token;                    ///< GUI thread
  std::optional<InFlightBudget::Reservation> room;  ///< GUI thread: returned at the publication
  InputStager::Staged staged;
  uint64_t tf_revision = 0;
  std::shared_ptr<const scripting::CompiledOnDemandScript> script;
  std::string params_json;
  std::vector<scripting::OutputSpec> outputs;
  scripting::RunOptions options;
  // Written by the worker. The keepalives come first, so they outlive `result`, which may pass an input through.
  std::vector<InputStager::Keepalive> keepalives;
  Expected<scripting::ScriptBundle, scripting::ScriptError> result{unexpected(scripting::ScriptError{"not run"})};
  ResultPayloads payloads;     ///< `result`'s object outputs, serialized
  bool decode_failed = false;  ///< `result` is an input's fetch or decode error: the script did not run
};

NodeScheduler::NodeScheduler(
    const ObjectStore& store, const RecipeGraph& graph, ResultHistory& history, const InputStager& stager,
    EnginePool& pool, InFlightBudget& budget, Hooks hooks)
    : store_(store),
      graph_(graph),
      history_(history),
      stager_(stager),
      pool_(pool),
      budget_(budget),
      hooks_(std::move(hooks)) {
  if (!hooks_.run) {
    hooks_.run = &scripting::runOnDemand;
  }
}

NodeScheduler::~NodeScheduler() {
  for (auto& [key, slot] : slots_) {
    if (slot.running) {
      slot.running->token.cancel();
    }
  }
}

void NodeScheduler::requestAt(std::string_view key, Timestamp t) {
  const RecipeGraph::Node* node = graph_.find(key);
  if (node == nullptr) {
    return;
  }
  auto found = slots_.find(key);
  if (found == slots_.end()) {
    found = slots_.emplace(std::string(key), Slot{}).first;
  }
  Entry& entry = *found;
  Slot& slot = entry.second;
  slot.requested = true;
  slot.cursor = node->recipe.pinned_t.value_or(t);
  history_.noteRequest(entry.first, slot.cursor);
  if (slot.failure && slot.failure->decode) {
    slot.failure.reset();  // the source may read its inputs now; a script fails the same inputs again
  }
  if (slot.running && slot.running->epoch != epochOf(node->dataset)) {
    slot.running->token.cancel();  // staged under another epoch; its publication drops it
  }
  // Upstream first, at the same time: the reader stages the result the upstream shows there.
  // ponytail: a diamond's shared ancestor is requested once per path, with no memo; fine at depth 3, memoize per
  // request if recipe graphs grow deep.
  for (const RecipeGraph::Binding& binding : node->bindings) {
    if (binding.upstream.empty()) {
      continue;
    }
    requestAt(binding.upstream, slot.cursor);
    if (const auto upstream = slots_.find(binding.upstream); upstream != slots_.end()) {
      std::vector<std::string>& readers = upstream->second.readers;
      if (std::find(readers.begin(), readers.end(), entry.first) == readers.end()) {
        readers.push_back(entry.first);
      }
    }
  }
  serve(entry, *node, Trigger::kRequest);
}

void NodeScheduler::serve(
    Entry& entry, const RecipeGraph::Node& node, Trigger trigger, std::optional<InFlightBudget::Reservation> room) {
  const std::string& key = entry.first;
  Slot& slot = entry.second;
  if (!slot.requested) {
    return;  // withdrawn: nothing starts until its next request
  }
  if (held_.contains(node.dataset)) {
    setWait(entry, Wait::kNone);  // its storage changes: `release` serves it
    setState(entry, NodeStatus::kPending);
    return;
  }
  const Timestamp at = slot.cursor;
  const uint64_t epoch = epochOf(node.dataset);
  if (std::optional<std::string> why = graph_.missingInput(key)) {
    settleMissing(entry, node, epoch, std::move(*why), /*keep_finding=*/false);
    return;
  }
  bool waiting = false;
  for (size_t index = 0; index < node.bindings.size(); ++index) {
    const RecipeGraph::Binding& binding = node.bindings[index];
    const auto upstream = binding.upstream.empty() ? slots_.end() : slots_.find(binding.upstream);
    if (upstream == slots_.end() || upstream->second.cursor != at) {
      continue;  // a topic input, or an upstream requested at another time: staged as it shows here
    }
    const Slot& up = upstream->second;
    if (up.status == NodeStatus::kMissingInput || up.status == NodeStatus::kError) {
      // An upstream that failed clears a finding (D6); one missing its inputs (they left the window) does not.
      settleMissing(
          entry, node, epoch, "on-demand: missing input '" + node.recipe.inputs[index] + "': " + up.detail,
          /*keep_finding=*/up.status == NodeStatus::kMissingInput);
      return;
    }
    if (up.status == NodeStatus::kPending && !waiting) {
      // A request waits for its upstream; once the upstream published, the reader takes what it shows.
      const auto shown =
          trigger == Trigger::kRequest ? std::nullopt : history_.resultAt(binding.upstream, binding.name, at);
      waiting = !shown || shown->unavailable;
    }
  }
  if (waiting) {
    setWait(entry, Wait::kUpstream);
    setState(entry, NodeStatus::kPending);
    return;
  }
  Expected<InputStager::Staged> staged = stager_.stage(node, at, epoch);
  if (!staged) {
    settleMissing(entry, node, epoch, std::move(staged.error()));
    return;
  }
  const uint64_t tf_revision = tfRevisionOf(node.dataset);
  if (slot.failure && slot.failure->key.serves(staged->key, at, tf_revision)) {
    setWait(entry, Wait::kNone);
    clearShown(key, node, at, staged->stamp, epoch, slot.failure->message);
    setState(entry, NodeStatus::kError, slot.failure->message);
    return;
  }
  if (std::optional<ResultHistory::Hit> hit = history_.lookup(key, staged->key, staged->stamp, at, tf_revision)) {
    setWait(entry, Wait::kNone);
    bool shown_changed = false;
    if (!hit->in_ring) {
      // A copy of the bundle shares its objects; `hit->bundle` holds the libraries they need until it is gone.
      if (Status republished = history_.publish(node, at, hit->stamp, *hit->key, *hit->bundle); !republished) {
        setState(entry, NodeStatus::kError, republished.error());
        return;
      }
      if (hooks_.published) {
        hooks_.published(key);
      }
    }
    settleServed(entry, hit->stamp);
    return;
  }
  const std::optional<Timestamp> lag = lagBehind(entry, staged->stamp);
  if (slot.running) {
    setWait(entry, Wait::kRun);
    setState(entry, NodeStatus::kPending, {}, lag);
    return;
  }
  if (!room) {
    room = budget_.tryReserve(InFlightBudget::kRunBytes);  // behind the waiters, whichever engine's
  }
  if (!room) {
    setWait(entry, Wait::kRoom);
    setState(entry, NodeStatus::kPending, {}, lag);
    return;
  }
  setWait(entry, Wait::kNone);
  submit(entry, node, epoch, std::move(*staged), std::move(*room));
  setState(entry, NodeStatus::kPending, {}, lag);
}

void NodeScheduler::submit(
    Entry& entry, const RecipeGraph::Node& node, uint64_t epoch, InputStager::Staged staged,
    InFlightBudget::Reservation room) {
  auto job = std::make_shared<Job>();
  job->key = entry.first;
  job->revision = node.revision;
  job->epoch = epoch;
  job->requested_t = entry.second.cursor;
  job->query_time = node.recipe.query_time;
  job->room.emplace(std::move(room));
  std::shared_ptr<const pj::tf::TransformSnapshot> tf = hooks_.tf ? hooks_.tf(node.dataset) : nullptr;
  job->tf_revision = tf != nullptr ? tf->revision() : 0;
  job->options.tf = std::move(tf);
  job->options.query_time = node.recipe.query_time ? job->requested_t : staged.stamp;
  job->options.image_decoder = hooks_.image_decoder ? hooks_.image_decoder() : nullptr;
  job->options.video_source = hooks_.video_source ? hooks_.video_source() : nullptr;
  job->options.result_stamp = staged.stamp;
  job->staged = std::move(staged);
  job->script = node.compiled;
  job->params_json = node.recipe.params_json;
  job->outputs = node.recipe.outputs;

  EnginePool::Task task;
  task.compute = [store = &store_, run = hooks_.run, job](const EnginePool::CancelToken& token) {
    StagedRun ran =
        runStaged(*store, job->staged, run, *job->script, job->params_json, job->outputs, job->options, token);
    job->keepalives = std::move(ran.keepalives);
    job->result = std::move(ran.result);
    job->decode_failed = ran.decode_failed;
    if (job->result && !token.cancelled()) {
      job->payloads = ResultHistory::serializeOutputs(job->outputs, *job->result);
    }
  };
  task.publish = [this, alive = std::weak_ptr<const int>(lifetime_), job](const EnginePool::Outcome& outcome) {
    if (!alive.expired()) {
      finish(*job, outcome);
    }
  };
  job->token = pool_.submit(std::move(task), laneOf(node));
  entry.second.running = std::move(job);
}

void NodeScheduler::finish(Job& job, const EnginePool::Outcome& outcome) {
  job.room.reset();
  const auto found = slots_.find(job.key);
  Entry* entry = nullptr;
  const RecipeGraph::Node* node = nullptr;
  bool current = false;
  bool served = false;
  if (found != slots_.end() && found->second.running.get() == &job) {
    entry = &*found;
    entry->second.running.reset();  // the publish closure still holds the job
    node = graph_.find(job.key);
    current = outcome.status != EnginePool::Outcome::Status::kCancelled && node != nullptr &&
              node->revision == job.revision && epochOf(node->dataset) == job.epoch;
    if (current) {
      if (outcome.status == EnginePool::Outcome::Status::kFailed) {
        job.result = unexpected(scripting::ScriptError{outcome.error});
      }
      served = apply(*entry, *node, job);
    }
  }
  if (current) {
    serveReaders(*entry, /*waiting_only=*/false);
  }
  if (served) {
    settleServed(*entry, job.staged.stamp);  // no lookup: the history may keep nothing to find (no object output)
  } else if (entry != nullptr && node != nullptr) {
    serve(*entry, *node, Trigger::kRequest);
  }
  // Last, since a grant may re-enter either engine: the room goes to those waiting for it first, and the readers,
  // which can take this result, and then this node's own request queued behind them in that order.
  budget_.drain();
}

bool NodeScheduler::apply(Entry& entry, const RecipeGraph::Node& node, Job& job) {
  const std::string& key = entry.first;
  Slot& slot = entry.second;
  ResultHistory::CacheKey stored = job.staged.key;
  const auto observed = [&](bool clock, bool tf) {
    if (clock && job.query_time) {
      stored.query_clock = job.requested_t;
    }
    if (tf) {
      stored.tf_revision = job.tf_revision;
    }
  };
  if (job.result) {
    observed(job.result->observed_query_clock, job.result->observed_tf);
  } else {
    observed(job.result.error().observed_query_clock, job.result.error().observed_tf);
  }
  // A late run that does not serve the current request is only cached, and its failure clears nothing: the ring
  // keeps what serves it. A video packet arriving during the run does not count (`completes`): under a live stream
  // every run would otherwise end cached and be run again, the result never shown.
  // ponytail: a packet changed or appended mid-run then shows the frame the run decoded until the next request.
  // Upgrade: record the frames a run queried and validate them here.
  const Expected<InputStager::Staged> current = stager_.stage(node, slot.cursor, job.epoch);
  const bool serves = current && stored.completes(current->key, slot.cursor, tfRevisionOf(node.dataset));
  std::string failure;
  if (job.result) {
    const Status published = history_.publish(
        node, job.requested_t, job.staged.stamp, stored, std::move(*job.result), std::move(job.keepalives),
        {.cache_only = !serves, .payloads = std::move(job.payloads)});
    if (published) {
      if (hooks_.published) {
        hooks_.published(key);
      }
      return serves;
    }
    failure = published.error();
  } else {
    failure = job.result.error().message;
  }
  if (serves) {
    clearShown(key, node, job.requested_t, job.staged.stamp, job.epoch, failure);
  }
  slot.failure = Slot::Failure{std::move(stored), std::move(failure), job.decode_failed};
  return false;
}

void NodeScheduler::settleServed(Entry& entry, Timestamp stamp) {
  setWait(entry, Wait::kNone);
  if (history_.supersedeTombstones(entry.first, stamp, entry.second.cursor) && hooks_.published) {
    hooks_.published(entry.first);
  }
  setState(entry, NodeStatus::kReady, {}, lagBehind(entry, stamp));
}

void NodeScheduler::settleMissing(
    Entry& entry, const RecipeGraph::Node& node, uint64_t epoch, std::string reason, bool keep_finding) {
  if (keep_finding && node.recipe.pinned_t) {
    if (const auto shown = history_.shownAt(entry.first, entry.second.cursor); shown && shown->available) {
      settleServed(entry, shown->stamp);
      return;
    }
  }
  setWait(entry, Wait::kNone);
  clearShown(entry.first, node, entry.second.cursor, entry.second.cursor, epoch, reason);
  setState(entry, NodeStatus::kMissingInput, std::move(reason));
}

void NodeScheduler::clearShown(
    const std::string& key, const RecipeGraph::Node& node, Timestamp requested_t, Timestamp stamp, uint64_t epoch,
    const std::string& reason) {
  const std::optional<ResultHistory::Shown> shown = history_.shownAt(key, stamp);
  if (shown ? !shown->available : !history_.resultsDropped(key)) {
    return;  // nothing drawn to clear
  }
  scripting::ScriptBundle cleared;
  for (const scripting::OutputSpec& output : node.recipe.outputs) {
    cleared.outputs.push_back({.name = output.name, .value = scripting::Unavailable{reason}});
  }
  // No staged key has an empty input list, so this key never serves a request.
  const ResultHistory::CacheKey tombstone{.revision = node.revision, .epoch = epoch};
  if (history_.publish(node, requested_t, stamp, tombstone, std::move(cleared)) && hooks_.published) {
    hooks_.published(key);
  }
}

std::optional<Timestamp> NodeScheduler::lagBehind(const Entry& entry, Timestamp target) const {
  const std::optional<ResultHistory::Shown> shown = history_.shownAt(entry.first, entry.second.cursor);
  if (!shown || !shown->available) {
    return std::nullopt;
  }
  return shown->stamp < target ? saturatingSub(target, shown->stamp) : 0;
}

void NodeScheduler::setState(Entry& entry, NodeStatus status, std::string detail, std::optional<Timestamp> lag) {
  Slot& slot = entry.second;
  if (status != slot.status || detail != slot.detail || lag != slot.lag) {
    slot.status = status;
    slot.detail = std::move(detail);
    slot.lag = lag;
    if (hooks_.state_changed) {
      hooks_.state_changed(entry.first);
    }
  }
  if (status != NodeStatus::kPending) {
    serveReaders(entry, /*waiting_only=*/true);  // a settlement: the readers waiting on it go on
  }
}

void NodeScheduler::setWait(Entry& entry, Wait wait) {
  Slot& slot = entry.second;
  if (wait != Wait::kRoom) {
    slot.room = {};  // leaves the line, if in it
  } else if (slot.wait != Wait::kRoom) {
    // The slot owns the ticket, so the grant never outlives the slot (map nodes do not move).
    slot.room = budget_.whenRoom(
        InFlightBudget::kRunBytes, [this, &entry](InFlightBudget::Reservation room) { admit(entry, std::move(room)); });
  }
  slot.wait = wait;
}

void NodeScheduler::serveReaders(const Entry& entry, bool waiting_only) {
  const std::vector<std::string> readers = entry.second.readers;  // serving one may register another
  for (const std::string& name : readers) {
    const auto reader = slots_.find(name);
    const RecipeGraph::Node* node = reader == slots_.end() ? nullptr : graph_.find(name);
    if (node == nullptr || reader->second.running) {
      continue;  // gone, or its run's end serves it
    }
    const Wait wait = reader->second.wait;
    if (waiting_only ? wait == Wait::kUpstream : wait != Wait::kRoom) {
      serve(*reader, *node, Trigger::kRetry);
    }
  }
}

void NodeScheduler::admit(Entry& entry, InFlightBudget::Reservation room) {
  setWait(entry, Wait::kNone);  // out of the line already
  if (const RecipeGraph::Node* node = graph_.find(entry.first)) {
    serve(entry, *node, Trigger::kRetry, std::move(room));
  }
}

void NodeScheduler::forget(std::string_view key) {
  const auto found = slots_.find(key);
  if (found == slots_.end()) {
    return;
  }
  if (found->second.running) {
    found->second.running->token.cancel();
  }
  slots_.erase(found);
}

bool NodeScheduler::withdraw(std::string_view key) {
  const auto found = slots_.find(key);
  if (found == slots_.end()) {
    return false;
  }
  for (const std::string& name : found->second.readers) {
    const auto reader = slots_.find(name);
    const RecipeGraph::Node* node = graph_.find(name);
    if (reader != slots_.end() && reader->second.requested && node != nullptr &&
        std::ranges::any_of(node->bindings, [&](const RecipeGraph::Binding& b) { return b.upstream == key; })) {
      return false;  // the reader's request waits on it
    }
  }
  found->second.requested = false;
  setWait(*found, Wait::kNone);
  setState(*found, NodeStatus::kIdle);
  return true;
}

void NodeScheduler::hold(DatasetId dataset) {
  ++held_[dataset];
  for (Entry& entry : slots_) {
    const RecipeGraph::Node* node = graph_.find(entry.first);
    if (node == nullptr || node->dataset != dataset) {
      continue;
    }
    if (entry.second.running) {
      entry.second.running->token.cancel();  // staged under the old epoch: never published
    }
    if (entry.second.requested) {
      setState(entry, NodeStatus::kPending);
    }
  }
}

void NodeScheduler::release(DatasetId dataset) {
  const auto held = held_.find(dataset);
  if (held == held_.end() || --held->second > 0) {
    return;
  }
  held_.erase(held);
  std::vector<std::pair<std::string, Timestamp>> requests;
  for (const auto& [key, slot] : slots_) {
    const RecipeGraph::Node* node = graph_.find(key);
    if (slot.requested && node != nullptr && node->dataset == dataset) {
      requests.emplace_back(key, slot.cursor);
    }
  }
  for (const auto& [key, cursor] : requests) {
    requestAt(key, cursor);  // a finding at its pin, which a merge may have moved
  }
}

void NodeScheduler::supersede(std::string_view key) {
  const auto found = slots_.find(key);
  if (found == slots_.end()) {
    return;
  }
  Slot& slot = found->second;
  if (slot.running) {
    slot.running->token.cancel();  // a reader's revision stays, so its publication would not drop it
  }
  slot.failure.reset();
  if (slot.requested) {
    requestAt(key, slot.cursor);
  }
}

std::optional<NodeScheduler::NodeState> NodeScheduler::state(std::string_view key) const {
  const auto found = slots_.find(key);
  if (found == slots_.end()) {
    return std::nullopt;
  }
  const Slot& slot = found->second;
  return NodeState{slot.status, slot.detail, slot.cursor, slot.lag};
}

uint64_t NodeScheduler::epochOf(DatasetId dataset) const {
  return hooks_.epoch ? hooks_.epoch(dataset) : 0;
}

uint64_t NodeScheduler::tfRevisionOf(DatasetId dataset) const {
  if (hooks_.tf_revision) {
    return hooks_.tf_revision(dataset);
  }
  const auto snapshot = hooks_.tf ? hooks_.tf(dataset) : nullptr;
  return snapshot != nullptr ? snapshot->revision() : 0;
}

}  // namespace PJ
