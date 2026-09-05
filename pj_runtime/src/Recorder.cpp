// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_runtime/Recorder.h"

#include <cassert>
#include <chrono>
#include <ctime>
#include <exception>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>

namespace PJ {

// The RecordTap a runtime host holds: co-owns the recorder, so an in-flight
// onMessage can never find it destroyed, and forwards each message. The
// binding→channel table is the recorder's, so the tap has no state of its own
// and no lock: the recorder's mu_ is the only mutex on the push path.
class Recorder::SourceTap final : public RecordTap {
 public:
  explicit SourceTap(std::shared_ptr<Recorder> recorder) : recorder_(std::move(recorder)) {}

  TapVerdict onMessage(
      uint32_t binding_id, const RecordedBindingView& binding, int64_t log_time_ns,
      Span<const uint8_t> bytes) override {
    try {
      return recorder_->enqueueMessage(binding_id, binding, log_time_ns, bytes);
    } catch (...) {
      recorder_->truncate("recorder exception", activeExceptionWhat());
      return TapVerdict::kStopRecording;
    }
  }

 private:
  std::shared_ptr<Recorder> recorder_;
};

// A queue-budget charge that is given back unless it reaches the queue. The
// charge is taken under mu_ and the payload copied outside it; releasing it
// through one destructor is what keeps the two ways out of that window — the
// recording ending, and the copy throwing — from drifting apart.
class Recorder::BudgetReservation {
 public:
  BudgetReservation(Recorder& recorder, uint64_t cost) : recorder_(recorder), cost_(cost) {}
  BudgetReservation(const BudgetReservation&) = delete;
  BudgetReservation& operator=(const BudgetReservation&) = delete;

  ~BudgetReservation() {
    if (committed_) {
      return;
    }
    std::lock_guard lock(recorder_.mu_);
    recorder_.queued_bytes_ -= cost_;
    recorder_.space_cv_.notify_all();
  }

  /// The item is in the queue: the writer refunds the charge when it drains it.
  /// Call while holding mu_, with the item already queued.
  void commit() noexcept {
    committed_ = true;
  }

 private:
  Recorder& recorder_;
  uint64_t cost_;
  bool committed_ = false;
};

Recorder::Recorder(std::unique_ptr<RecordingSink> sink, RecorderOptions options)
    : sink_(std::move(sink)), options_(std::move(options)) {}

Recorder::~Recorder() {
  try {
    (void)stop(std::string(kTerminalCauseShutdown));
  } catch (...) {
    // stop() contains the sink's failures itself; only an allocation failure in
    // the summary's strings can land here, and a destructor has nobody to tell.
  }
  assert(!writer_.joinable());
}

std::string Recorder::nowUtcIso() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  char text[sizeof("YYYY-MM-DDTHH:MM:SSZ")];
  const std::size_t length = std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return std::string(text, length);
}

const char* Recorder::activeExceptionWhat() noexcept {
  try {
    throw;
  } catch (const std::exception& error) {
    return error.what();
  } catch (...) {
    return "unknown exception";
  }
}

std::vector<uint8_t> Recorder::copyPayload(Span<const uint8_t> bytes) {
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

Status Recorder::start() {
  {
    std::lock_guard lock(mu_);
    if (state_ != State::kIdle) {
      return unexpected("recorder already started");
    }
  }
  options_.info.started_utc = nowUtcIso();
  try {
    if (auto opened = sink_->open(options_.path, options_.info); !opened) {
      return opened;  // still kIdle: stop() must not close a sink that never opened
    }
  } catch (...) {
    return unexpected(std::string("recording open: ") + activeExceptionWhat());
  }
  try {
    // Started before the state flips: the writer only waits for items, and a
    // failure here must leave a recorder nothing can be pushed into.
    writer_ = std::thread([this] { writerLoop(); });
  } catch (const std::system_error& error) {
    return unexpected(finalizeWithoutWriter(std::string("writer thread: ") + error.what()));
  }
  std::lock_guard lock(mu_);
  state_ = State::kRunning;
  return okStatus();
}

std::string Recorder::finalizeWithoutWriter(std::string reason) {
  // Nothing will ever drain this recording, so close the file here: leaving it
  // open would abandon a footer-less file that needs `mcap recover`.
  RecordingSummary summary;
  summary.stopped_utc = nowUtcIso();
  summary.terminal_cause = std::string(kTerminalCauseTruncated);
  summary.truncated = true;
  summary.truncated_reason = reason;
  if (auto closed = closeSink(summary); !closed) {
    summary.file_incomplete = true;
  }
  std::lock_guard lock(mu_);
  // Published as stop() would: the owner's stop() (or the destructor's)
  // re-returns this summary instead of touching the sink again.
  stop_entered_ = true;
  summary_frozen_ = true;
  summary_published_ = true;
  truncated_reason_ = std::move(reason);
  summary_ = std::move(summary);
  state_ = State::kTruncated;
  return summary_.truncated_reason;
}

Status Recorder::closeSink(const RecordingSummary& summary) {
  try {
    return sink_->close(summary);
  } catch (...) {
    return unexpected(std::string("recording close: ") + activeExceptionWhat());
  }
}

std::shared_ptr<RecordTap> Recorder::tapFor() {
  return std::make_shared<SourceTap>(shared_from_this());
}

uint32_t Recorder::channelFor(uint32_t binding_id, const RecordedBindingView& binding) {
  if (const auto found = channels_.find(binding_id); found != channels_.end()) {
    return found->second;
  }
  const uint32_t logical = next_logical_channel_++;
  auto add = std::make_unique<ChannelAdd>();
  add->logical_channel = logical;
  add->binding.topic = binding.topic;
  add->binding.encoding = binding.encoding;
  add->binding.type_name = binding.type_name;
  add->binding.schema_bytes = binding.schema_bytes;
  queue_.emplace_back(std::move(add));
  channels_.emplace(binding_id, logical);
  items_cv_.notify_one();
  return logical;
}

bool Recorder::makeRoomFor(uint64_t incoming_cost) {
  while (queued_bytes_ + incoming_cost > options_.queue_budget_bytes) {
    // Nothing queued is bigger than the newcomer, so the newcomer is the
    // largest member of the set and loses. A message costing more than the
    // whole budget takes this exit against an empty queue: it can never fit.
    if (size_index_.empty() || size_index_.rbegin()->first.first <= incoming_cost) {
      return false;
    }
    evictLargest();
  }
  return true;
}

void Recorder::evictLargest() {
  const auto largest = std::prev(size_index_.end());
  const auto& message = std::get<MessageItem>(*largest->second);
  ++dropped_messages_;
  queued_bytes_ -= queuedCost(message.bytes.size());
  queue_.erase(largest->second);
  size_index_.erase(largest);
}

void Recorder::discardQueue() noexcept {
  for (const Item& item : queue_) {
    if (const auto* message = std::get_if<MessageItem>(&item)) {
      queued_bytes_ -= queuedCost(message->bytes.size());
    }
  }
  size_index_.clear();
  queue_.clear();
}

TapVerdict Recorder::enqueueMessage(
    uint32_t binding_id, const RecordedBindingView& binding, int64_t log_time_ns, Span<const uint8_t> bytes) {
  const uint64_t queued_size = queuedCost(bytes.size());
  uint32_t logical_channel = 0;
  {
    std::unique_lock lock(mu_);
    if (state_ != State::kRunning) {
      return TapVerdict::kStopRecording;  // never started, or stopped/truncated under us
    }
    if (options_.overflow_policy == RecorderOptions::OverflowPolicy::kBlock) {
      space_cv_.wait(lock, [this, queued_size] {
        return state_ != State::kRunning || queued_bytes_ == 0 ||
               (queued_size <= options_.queue_budget_bytes &&
                queued_bytes_ <= options_.queue_budget_bytes - queued_size);
      });
      if (state_ != State::kRunning) {
        return TapVerdict::kStopRecording;
      }
    }
    // The channel opens before the message is weighed, so a binding whose
    // every message is too big still names its topic in the file.
    logical_channel = channelFor(binding_id, binding);
    if (options_.overflow_policy == RecorderOptions::OverflowPolicy::kDropLargest && !makeRoomFor(queued_size)) {
      ++dropped_messages_;
      // The recording lost a message, not its tap: dropping IS the policy
      // working, so the source keeps feeding the ones that follow.
      return TapVerdict::kContinue;
    }
    // Charge the budget here and copy below, outside the lock: holding mu_
    // across the copy would park the writer and every other producer behind one
    // memcpy, while reserving first stops concurrent producers from all
    // admitting themselves into the same free space.
    queued_bytes_ += queued_size;
  }

  // Declared before the copy, so an allocation that throws unwinds through its
  // destructor and the charge does not outlive the message it was for.
  BudgetReservation reservation(*this, queued_size);
  MessageItem item{logical_channel, log_time_ns, 0, copyPayload(bytes)};

  std::lock_guard lock(mu_);
  if (state_ != State::kRunning) {
    return TapVerdict::kStopRecording;  // the recording ended while the payload was being copied
  }
  item.sequence = next_message_sequence_++;
  const std::pair<uint64_t, uint64_t> key{queued_size, item.sequence};
  queue_.push_back(std::move(item));
  reservation.commit();
  // Indexed after the commit: an allocation failure here can then only cost
  // this one message its evictability, never the byte accounting.
  size_index_.emplace(key, std::prev(queue_.end()));
  items_cv_.notify_one();
  return TapVerdict::kContinue;
}

void Recorder::truncate(std::string_view reason, const char* detail) noexcept {
  {
    std::lock_guard lock(mu_);
    if (summary_frozen_ || (state_ != State::kRunning && state_ != State::kStopping)) {
      // Past the freeze nothing can reach the file any more, and before it the
      // first reason is the real one.
      return;
    }
    state_ = State::kTruncated;
    drain_and_exit_ = true;  // the writer drains what is queued, then leaves
    try {
      truncated_reason_.assign(reason);
      if (detail != nullptr) {
        truncated_reason_ += ": ";
        truncated_reason_ += detail;
      }
    } catch (...) {
      // Out of memory: the truncation stands, its reason reads as far as it got.
    }
  }
  items_cv_.notify_all();
  space_cv_.notify_all();
}

void Recorder::writerLoop() noexcept {
  try {
    drainQueue();
  } catch (...) {
    truncate("recorder writer exception", activeExceptionWhat());
  }
  // Nothing drains the queue once this thread is gone, and nothing enters it
  // either — every exit above is preceded by a state no producer is admitted in.
  std::lock_guard lock(mu_);
  discardQueue();
}

void Recorder::drainQueue() {
  // Written since the writer last held mu_: folded into the counters on the
  // next dequeue, which takes the lock anyway, so a write costs no lock of its own.
  uint64_t pending_messages = 0;
  uint64_t pending_payload_bytes = 0;
  for (;;) {
    std::optional<Item> item;
    {
      std::unique_lock lock(mu_);
      written_messages_ += pending_messages;
      written_payload_bytes_ += pending_payload_bytes;
      pending_messages = 0;
      pending_payload_bytes = 0;
      items_cv_.wait(lock, [this] { return !queue_.empty() || drain_and_exit_; });
      if (queue_.empty()) {
        return;  // told to leave, and drained
      }
      item = std::move(queue_.front());
      if (const auto* message = std::get_if<MessageItem>(&*item)) {
        const uint64_t cost = queuedCost(message->bytes.size());
        queued_bytes_ -= cost;
        size_index_.erase({cost, message->sequence});
      }
      queue_.pop_front();
    }
    space_cv_.notify_all();

    if (auto* add = std::get_if<std::unique_ptr<ChannelAdd>>(&*item)) {
      auto channel = sink_->addChannel((*add)->binding);
      if (!channel) {
        truncate(channel.error());
        continue;
      }
      sink_channels_[(*add)->logical_channel] = *channel;
      continue;
    }
    auto& message = std::get<MessageItem>(*item);
    const auto found = sink_channels_.find(message.logical_channel);
    if (found == sink_channels_.end()) {
      continue;  // its channel add failed; the recording is already truncating
    }
    if (auto status = sink_->write(
            found->second, message.log_time_ns, Span<const uint8_t>(message.bytes.data(), message.bytes.size()));
        !status) {
      truncate(status.error());
      return;  // a sink that rejected a write is left alone: nothing queued can land
    }
    ++pending_messages;
    pending_payload_bytes += message.bytes.size();
  }
}

void Recorder::requestStop() {
  {
    std::lock_guard lock(mu_);
    if (state_ != State::kRunning) {
      return;
    }
    state_ = State::kStopping;
    drain_and_exit_ = true;
  }
  items_cv_.notify_all();
  space_cv_.notify_all();
}

void Recorder::setExtraMetadata(std::string name, std::string json) {
  std::lock_guard lock(mu_);
  for (auto& [existing_name, body] : extra_metadata_) {
    if (existing_name == name) {
      body = std::move(json);
      return;
    }
  }
  extra_metadata_.emplace_back(std::move(name), std::move(json));
}

RecordingSummary Recorder::stop(std::string terminal_cause) {
  requestStop();
  {
    std::unique_lock lock(mu_);
    if (state_ == State::kIdle) {
      return summary_;  // never started: nothing to drain, close or report
    }
    if (stop_entered_) {
      // The first caller owns the join and the close; this one gets its summary
      // once it exists. Sharing items_cv_ with the writer steals no wake-up:
      // drain_and_exit_ is set, so the writer never waits on it again.
      items_cv_.wait(lock, [this] { return summary_published_; });
      return summary_;
    }
    stop_entered_ = true;
  }
  if (writer_.joinable()) {
    writer_.join();
  }

  RecordingSummary summary;
  summary.stopped_utc = nowUtcIso();
  summary.terminal_cause = std::move(terminal_cause);
  {
    // Freeze the facts and read them under one lock: a producer that timed out
    // just before the join must not turn this summary into a truncation, while
    // a truncation raised during the final drain still has to count.
    std::lock_guard lock(mu_);
    summary_frozen_ = true;
    summary.messages = written_messages_;
    summary.payload_bytes = written_payload_bytes_;
    summary.dropped_messages = dropped_messages_;
    summary.truncated = state_ == State::kTruncated;
    summary.truncated_reason = truncated_reason_;
    summary.extra_metadata = extra_metadata_;
  }
  if (summary.truncated) {
    // A recording that lost data ended for that reason, whatever the caller
    // asked to record as the cause.
    summary.terminal_cause = std::string(kTerminalCauseTruncated);
    if (summary.truncated_reason.empty()) {
      summary.truncated_reason = "reason lost: out of memory";  // truncate() could not store it
    }
  }
  // A failed close means the sink could not finalize: the file has no footer.
  if (auto closed = closeSink(summary); !closed) {
    summary.file_incomplete = true;
    if (!summary.truncated) {
      summary.truncated = true;
      summary.truncated_reason = closed.error();
      summary.terminal_cause = std::string(kTerminalCauseTruncated);
    }
  }
  {
    std::lock_guard lock(mu_);
    if (summary.truncated) {
      state_ = State::kTruncated;  // a close() failure is data loss too
      truncated_reason_ = summary.truncated_reason;
    } else {
      state_ = State::kFinished;
    }
    summary_ = summary;
    summary_published_ = true;
  }
  items_cv_.notify_all();
  return summary;
}

Recorder::State Recorder::state() const {
  std::lock_guard lock(mu_);
  return state_;
}

RecordingStats Recorder::stats() const {
  std::lock_guard lock(mu_);
  return RecordingStats{written_messages_, written_payload_bytes_, dropped_messages_};
}

}  // namespace PJ
