// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/SeriesRunner.h"

#include <QMetaObject>
#include <algorithm>
#include <limits>
#include <utility>

#include "pj_datastore/writer.hpp"

namespace PJ {
namespace {

/// The names of `node`'s number outputs, in declaration order: its series' columns.
std::vector<std::string> columnsOf(const RecipeGraph::Node& node) {
  std::vector<std::string> columns;
  for (const scripting::OutputSpec& output : node.recipe.outputs) {
    if (output.type == scripting::OutputType::kNumber) {
      columns.push_back(output.name);
    }
  }
  return columns;
}

/// The schema name of a series: its key and its columns, each length-prefixed, so no name passes for two.
std::string schemaName(std::string_view key, const std::vector<std::string>& columns) {
  std::string name = "derived_on_demand_";
  const auto add = [&name](std::string_view part) {
    name += std::to_string(part.size());
    name += ':';
    name += part;
  };
  add(key);
  for (const std::string& column : columns) {
    add(column);
  }
  return name;
}

}  // namespace

SeriesRunner::SeriesRunner(DataEngine& engine, const RecipeGraph& graph, GridRunner& runner, Hooks hooks)
    : engine_(engine), graph_(graph), runner_(runner), hooks_(std::move(hooks)) {}

SeriesRunner::~SeriesRunner() {
  for (auto& [key, series] : series_) {
    runner_.cancel(series.chunk);
  }
}

bool SeriesRunner::isSeries(const RecipeGraph::Node& node) {
  const auto reads_object = [](const RecipeGraph::Binding& binding) {
    return binding.object_type != sdk::BuiltinObjectType::kNone || !binding.upstream.empty();
  };
  return !node.recipe.pinned_t && std::ranges::any_of(node.bindings, reads_object) && !columnsOf(node).empty();
}

Status SeriesRunner::check(const RecipeGraph::Node& node) const {
  if (!isSeries(node)) {
    return okStatus();
  }
  const std::string key = node.recipe.key();
  std::optional<TopicId> own;
  for (const auto* held : {&series_, &parked_}) {
    if (const auto found = held->find(key); found != held->end() && found->second.dataset == node.dataset) {
      own = found->second.state.topic;
    }
  }
  const auto lock = engine_.lockEngine();
  for (const TopicId topic : engine_.listTopics(node.dataset)) {
    const TopicStorage* storage = engine_.getTopicStorage(topic);
    if (storage != nullptr && storage->descriptor().name == key && topic != own) {
      return unexpected(
          "on-demand: '" + key +
          "' would write a series topic, but a topic with that name already exists in the dataset");
    }
  }
  return okStatus();
}

void SeriesRunner::install(const RecipeGraph::Node& node, bool demanded) {
  const std::string key = node.recipe.key();
  std::optional<Series> previous;
  bool listed = false;
  for (auto* held : {&series_, &parked_}) {
    if (auto found = held->find(key); found != held->end()) {
      cancel(found->second);
      previous = std::move(found->second);
      held->erase(found);
      listed = listed || held == &series_;
    }
  }
  if (!isSeries(node)) {
    if (previous) {
      retire(previous->state.topic);
    }
    if (listed) {
      gone_.push_back(key);
    }
    schedule();  // its chunk's turn passes on
    return;
  }
  Series series;
  series.dataset = node.dataset;
  series.columns = columnsOf(node);
  series.ephemeral = node.recipe.ephemeral;
  series.demanded = demanded;
  series.requested = previous && previous->requested;  // the new revision computes to the end
  if (previous && previous->dataset == series.dataset && previous->columns == series.columns) {
    series.state.topic = previous->state.topic;  // the curves bound to it survive the edit
  } else {
    if (previous) {
      retire(previous->state.topic);
    }
    std::vector<DataEngine::GroupColumn> columns;
    for (const std::string& column : series.columns) {
      columns.push_back({column, PrimitiveType::kFloat64});
    }
    Expected<TopicId> topic =
        engine_.createColumnGroupTopic(series.dataset, key, schemaName(key, series.columns), columns);
    if (!topic) {
      series.state.first_error = "on-demand: " + topic.error();
    } else {
      series.state.topic = *topic;
    }
  }
  Series& installed = series_.insert_or_assign(key, std::move(series)).first->second;
  reset(installed);
  pump(key);
}

void SeriesRunner::remove(std::string_view key) {
  for (auto* held : {&series_, &parked_}) {
    if (const auto found = held->find(key); found != held->end()) {
      cancel(found->second);
      const TopicId topic = found->second.state.topic;
      if (held == &series_) {
        gone_.push_back(found->first);
      }
      held->erase(found);
      retire(topic);
    }
  }
  schedule();
}

void SeriesRunner::park(std::string_view key) {
  if (const auto found = series_.find(key); found != series_.end()) {
    cancel(found->second);
    gone_.push_back(found->first);
    parked_.insert_or_assign(found->first, std::move(found->second));
    series_.erase(found);
  }
  schedule();
}

void SeriesRunner::sweepParked() {
  for (auto& [key, series] : std::exchange(parked_, {})) {
    retire(series.state.topic);
  }
}

void SeriesRunner::setDemand(DatasetId dataset, const std::unordered_set<std::string>& names) {
  for (auto& [key, series] : series_) {
    if (series.dataset != dataset) {
      continue;
    }
    const bool before = wanted(series);
    series.demanded = names.contains(key);
    if (before && !wanted(series)) {
      cancel(series);  // the rows written stay; the next demand resumes after them
    } else if (!before && wanted(series)) {
      pump(key);
    }
  }
  schedule();
}

bool SeriesRunner::isDemanded(std::string_view key) const {
  const auto found = series_.find(key);
  return found != series_.end() && found->second.demanded;
}

void SeriesRunner::request(std::string_view key) {
  if (const auto found = series_.find(key); found != series_.end()) {
    found->second.requested = true;
    found->second.state.stopped = false;
    pump(found->first);
  }
}

bool SeriesRunner::stop(std::string_view key) {
  const auto found = series_.find(key);
  if (found == series_.end() || !computing(found->first, found->second)) {
    return false;
  }
  cancel(found->second);
  found->second.requested = false;
  found->second.state.stopped = true;
  schedule();
  return true;
}

void SeriesRunner::invalidate(std::string_view key) {
  if (const auto found = series_.find(key); found != series_.end()) {
    reset(found->second);
    pump(found->first);
  }
}

void SeriesRunner::suspend(DatasetId dataset) {
  ++holds_[dataset];
  for (auto& [key, series] : series_) {
    if (series.dataset == dataset) {
      reset(series);
    }
  }
  schedule();
}

void SeriesRunner::resume(DatasetId dataset) {
  const auto held = holds_.find(dataset);
  if (held == holds_.end() || --held->second > 0) {
    return;
  }
  holds_.erase(held);
  for (const auto& [key, series] : series_) {
    if (series.dataset == dataset) {
      pump(key);
    }
  }
}

void SeriesRunner::endLive(DatasetId dataset) {
  if (live_.erase(dataset) == 0) {
    return;
  }
  for (const auto& [key, series] : series_) {
    if (series.dataset == dataset) {
      pump(key);
    }
  }
}

void SeriesRunner::appendLive(DatasetId dataset) {
  live_.insert(dataset);
  for (const auto& [key, series] : series_) {
    if (series.dataset == dataset) {
      pump(key);
    }
  }
}

void SeriesRunner::setVisibleStart(DatasetId dataset, Timestamp start) {
  visible_start_.insert_or_assign(dataset, start);
}

void SeriesRunner::requestTopic(TopicId topic) {
  const auto found =
      std::ranges::find_if(series_, [topic](const auto& entry) { return entry.second.state.topic == topic; });
  if (found != series_.end()) {
    request(found->first);
  }
}

std::optional<SeriesRunner::State> SeriesRunner::state(std::string_view key) const {
  const auto found = series_.find(key);
  if (found == series_.end()) {
    return std::nullopt;
  }
  State state = found->second.state;
  state.replaying = computing(found->first, found->second);
  return state;
}

std::unordered_set<TopicId> SeriesRunner::topics(bool ephemeral_only) const {
  std::unordered_set<TopicId> topics;
  for (const auto* held : {&series_, &parked_}) {
    for (const auto& [key, series] : *held) {
      if (series.state.topic != 0 && (!ephemeral_only || series.ephemeral)) {
        topics.insert(series.state.topic);
      }
    }
  }
  return topics;
}

bool SeriesRunner::wanted(const Series& series) const {
  return (series.demanded || series.requested || series.ephemeral) && !series.state.stopped &&
         !holds_.contains(series.dataset);
}

bool SeriesRunner::computing(std::string_view key, const Series& series) const {
  return series.chunk != nullptr || (wanted(series) && queued(key));
}

bool SeriesRunner::queued(std::string_view key) const {
  return std::ranges::find(waiting_, key) != waiting_.end();
}

void SeriesRunner::pump(const std::string& key) {
  const auto found = series_.find(key);
  if (found != series_.end() && found->second.chunk == nullptr && found->second.state.topic != 0 &&
      wanted(found->second) && !queued(key)) {
    waiting_.push_back(key);
  }
  schedule();
}

void SeriesRunner::schedule() {
  const auto running = [this] {
    return draining_ + static_cast<std::size_t>(std::ranges::count_if(
                           series_, [](const auto& entry) { return entry.second.chunk != nullptr; }));
  };
  while (!waiting_.empty() && running() < kMaxRunningChunks) {
    const std::string key = std::move(waiting_.front());
    waiting_.pop_front();
    submit(key);
  }
  notify();
}

void SeriesRunner::submit(const std::string& key) {
  const auto found = series_.find(key);
  const RecipeGraph::Node* node = graph_.find(key);
  if (found == series_.end() || node == nullptr) {
    return;
  }
  Series& series = found->second;
  if (series.chunk != nullptr || series.state.topic == 0 || !wanted(series)) {
    return;
  }
  // Live, the clock ends where every input has arrived: an input lagging the others then lands after the watermark.
  const bool live = live_.contains(series.dataset) && hooks_.live_edge;
  const std::optional<Timestamp> until = live ? hooks_.live_edge(*node) : std::nullopt;
  if (!live || until) {  // else an input has no sample yet
    split(series, until);
    const auto start = visible_start_.find(series.dataset);
    const bool backfill_first =
        series.backfill && start != visible_start_.end() && start->second <= series.backfill->end;
    for (const bool backfill : {backfill_first, !backfill_first}) {
      if (backfill && !series.backfill) {
        continue;
      }
      Expected<std::shared_ptr<GridRunner::Job>> chunk = runner_.submitRows(
          *node, backfill ? series.backfill->watermark : series.watermark,
          backfill ? std::optional(series.backfill->end) : until, backfill ? series.backfill->seen : series.seen,
          kInstantsPerChunk, [this, key, backfill](GridRunner::Rows rows) { collect(key, backfill, std::move(rows)); });
      if (!chunk) {
        series.state.first_error = std::move(chunk.error());  // an upstream removed: its reinstall invalidates this one
        series.requested = false;
        return;
      }
      series.chunk = std::move(*chunk);
      if (series.chunk != nullptr) {
        // A complete series stays so through a live append, not through a restart from the first instant.
        series.state.complete = series.state.complete && !GridRunner::restarts(*series.chunk);
        return;
      }
      if (backfill) {
        if (series.watermark == series.backfill->end) {
          // The forward range evaluated nothing (the view starts past the last instant): it resumes from the backfill.
          series.watermark = series.backfill->watermark;
          series.seen = series.backfill->seen;
        }
        series.backfill.reset();  // backfilled: one computed range again
      }
    }
  }
  series.state.complete = true;
  series.requested = false;  // the request is served: everything up to the end of the clock is computed
  series.state.total = series.state.done;
}

void SeriesRunner::split(Series& series, std::optional<Timestamp> live_edge) const {
  const auto start = visible_start_.find(series.dataset);
  if (series.was_split || start == visible_start_.end() || start->second == std::numeric_limits<Timestamp>::min()) {
    return;
  }
  const Timestamp end = start->second - 1;
  if ((series.watermark && end <= *series.watermark) || (live_edge && start->second > *live_edge)) {
    return;  // the view starts inside the computed rows, or past what arrived
  }
  series.backfill = Backfill{.watermark = series.watermark, .seen = series.seen, .end = end};
  series.was_split = true;
  series.watermark = end;
  series.seen = {};  // what arrived before the visible start is the backfill's to find
}

void SeriesRunner::collect(const std::string& key, bool backfill, GridRunner::Rows rows) {
  // A chunk whose every instant is a gap ends inside the budget's drain, which would grant the next chunk at once.
  QMetaObject::invokeMethod(&context_, [this] { schedule(); }, Qt::QueuedConnection);
  const auto found = series_.find(key);
  if (found == series_.end()) {
    return;
  }
  Series& series = found->second;
  series.chunk = nullptr;
  if (!rows.error.empty()) {
    series.state.first_error = std::move(rows.error);  // replaced under it: the change that did so starts it again
    notify();
    return;
  }
  if (rows.restarted) {
    reset(series);  // an entry arrived before the watermark: these rows replace the topic's, from one range
  }
  const TopicId topic = series.state.topic;
  if (!rows.times.empty()) {
    const auto lock = engine_.lockEngine();
    if (engine_.getTopicStorage(topic) != nullptr) {
      DataWriter writer = engine_.createWriter();
      const std::size_t width = series.columns.size();
      const double* value = rows.values.data();
      for (const Timestamp t : rows.times) {
        bool written = writer.beginRow(topic, t).has_value();
        for (std::size_t column = 0; column < width; ++column, ++value) {
          writer.set<double>(topic, column, *value);
        }
        written = written && writer.finishRow(topic).has_value();
        series.state.rows += written ? 1 : 0;
      }
      (void)engine_.commitChunksLocked(writer.flushAll());
    }
  }
  const uint64_t evaluated = rows.times.size() + rows.gaps;
  series.state.done += evaluated;
  series.state.failed += rows.gaps;
  if (!series.backfill) {
    series.state.total = series.state.done + rows.left;
  } else if (!backfill) {
    series.state.total = rows.before + evaluated + rows.left;  // the forward range's clock is the whole one
  } else {
    series.state.total = std::max(series.state.total, series.state.done + rows.left);
  }
  if (series.state.first_error.empty()) {
    series.state.first_error = std::move(rows.first_gap);
  }
  if (rows.through && backfill && series.backfill) {
    series.backfill->watermark = rows.through;
    series.backfill->seen = rows.seen;
  } else if (rows.through) {
    series.watermark = rows.through;
    series.seen = rows.seen;
  }
  const bool again = wanted(series);
  if (again && !queued(key)) {
    waiting_.push_back(key);  // to the back: the other series' turns come first
  }
  const auto now = std::chrono::steady_clock::now();
  if (hooks_.topics_changed && (rows.left == 0 || !again || now - series.announced >= kAnnounceInterval)) {
    series.announced = now;
    hooks_.topics_changed({topic});
  }
  notify();
}

void SeriesRunner::reset(Series& series) {
  cancel(series);
  series.watermark.reset();
  series.seen = {};
  series.backfill.reset();
  series.was_split = false;
  State emptied;
  emptied.topic = series.state.topic;
  if (emptied.topic == 0) {
    emptied.first_error = std::move(series.state.first_error);  // why it has no topic
  }
  series.state = std::move(emptied);
  if (series.state.topic != 0) {
    engine_.clearTopicHistory(series.state.topic);
    if (hooks_.topics_changed) {
      hooks_.topics_changed({series.state.topic});  // the old rows are gone: plots redraw from empty
    }
  }
}

void SeriesRunner::cancel(Series& series) {
  const bool draining = runner_.cancel(series.chunk, [this, alive = std::weak_ptr<const int>(lifetime_)] {
    if (!alive.expired()) {
      --draining_;
      schedule();
    }
  });
  draining_ += draining ? 1 : 0;
  series.chunk = nullptr;
}

void SeriesRunner::notify() {
  std::vector<std::string> changed = std::exchange(gone_, {});
  if (!hooks_.state_changed) {
    return;
  }
  for (auto& [key, series] : series_) {
    State now = series.state;
    now.replaying = computing(key, series);
    if (series.notified != now) {
      series.notified = std::move(now);
      changed.push_back(key);
    }
  }
  for (const std::string& key : changed) {  // a callback may call back in: the reports are settled first
    hooks_.state_changed(key);
  }
}

void SeriesRunner::retire(TopicId topic) {
  if (topic == 0) {
    return;
  }
  engine_.retireTopic(topic);
  if (hooks_.topics_changed) {
    hooks_.topics_changed({topic});
  }
}

}  // namespace PJ
