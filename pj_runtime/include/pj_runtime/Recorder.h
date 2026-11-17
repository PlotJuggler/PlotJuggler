// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/span.hpp"
#include "pj_runtime/RecordTap.h"
#include "pj_runtime/RecordingFormat.h"
#include "pj_runtime/RecordingSink.h"
#include "pj_runtime/RecordingTypes.h"

namespace PJ {

struct RecorderOptions {
  enum class OverflowPolicy { kDropLargest, kBlock };

  std::filesystem::path path;
  /// Queued bytes held before overflow handling: each payload plus its queue item. Not an RSS cap (channel-add strings,
  /// allocator overhead and the writer's in-flight message are uncounted). kBlock admits one oversized message when
  /// nothing else is queued or being copied, so a large frame cannot deadlock.
  uint64_t queue_budget_bytes = 64ull << 20;
  RecordingInfo info;
  OverflowPolicy overflow_policy = OverflowPolicy::kDropLargest;
};

/// One recording: producers (source push threads, via the taps from tapFor) enqueue channel-adds and messages; ONE
/// writer thread drains them FIFO into the sink. The default live policy never waits: when a message does not fit the
/// byte budget, the largest of {queued messages, incoming message} is dropped until it does, so a slow sink costs
/// fidelity (counted in RecordingStats::dropped_messages; holes, never reorderings), never ingest speed. Download
/// captures use kBlock: producers wait for space; requestStop(), stop() and sink failures wake them.
///
/// All shared state sits under mu_, never held across a sink call, the payload copy or the writer join; the sink is
/// reached by the writer thread alone between open() and close(). Links no Qt and no file format (builds for wasm,
/// runs under TSan).
///
/// Must be owned by a std::shared_ptr (tapFor() needs it). Taps co-own the recorder, so the last reference may be
/// released on a push thread; by then stop() has joined the writer.
///
/// Truncation is for failures the recording cannot survive (a sink write/addChannel/close error, id exhaustion, an
/// exception escaping a tap or the writer), never for a slow writer. No exception leaves start(), stop() or the
/// destructor on the sink's account: a throwing open() is start()'s error Status, a throwing close() is a truncated
/// file in the summary. A truncation ends the DATA, not the recording: the writer exits, the sink stays open and no
/// summary exists until stop(). There is no truncation callback (it would run owner code on a push or writer
/// thread), so the owner polls state() and calls stop() once it sees kTruncated.
class Recorder : public std::enable_shared_from_this<Recorder> {
 public:
  /// kFinished and kTruncated are terminal, and kTruncated survives stop(): the
  /// state alone tells a clean recording from one that lost data.
  enum class State { kIdle, kRunning, kStopping, kFinished, kTruncated };

  Recorder(std::unique_ptr<RecordingSink> sink, RecorderOptions options);
  /// stop()s a running recording with the `shutdown` cause (joins the writer).
  /// Virtual only for the copyPayload() seam below.
  virtual ~Recorder();
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  /// Opens the sink and starts the writer. Call once, before handing out a tap. A failed open (returned or thrown)
  /// leaves the recorder kIdle with the sink untouched; a failed writer start closes the sink into a complete,
  /// truncated file.
  [[nodiscard]] Status start();

  /// Queue a file-level metadata record {name, JSON body} for the sink to
  /// write on close (RecordingSummary::extra_metadata). Call BEFORE stop();
  /// last write per name wins. Any thread.
  void setExtraMetadata(std::string name, std::string json);

  /// A tap for this recording's one source: it co-owns the recorder and forwards messages; the binding-to-channel
  /// table lives here (binding ids are per host, a recorder holds one host's source). Valid after stop(): calls
  /// become no-ops returning kStopRecording. Throws std::bad_weak_ptr unless owned by a std::shared_ptr.
  [[nodiscard]] std::shared_ptr<RecordTap> tapFor();

  /// Stops admission and wakes blocked producers without waiting for disk I/O. On cancellation, call it before joining
  /// a download's producer thread (detaching the tap cannot wake a call already in progress); stop() must still drain
  /// and close the sink. A stopped recorder alone proves no source coverage: cache publication also requires a
  /// successful, uncancelled source completion.
  void requestStop();

  /// Drains the queue, closes the sink, joins the writer and returns the final summary. Required after a truncation
  /// too (that is what closes the sink). Idempotent: later calls re-return the same summary, and a call racing the
  /// first blocks until it exists. Meant for the owning thread.
  ///
  /// `terminal_cause` goes into the summary so the file records why the recording ended; a recording that lost data
  /// overrides it with `truncated`.
  RecordingSummary stop(std::string terminal_cause = std::string(kTerminalCauseStopped));

  [[nodiscard]] State state() const;
  /// One consistent snapshot. The written counters trail the sink by at most
  /// the message being written: the writer folds them in on its next dequeue.
  [[nodiscard]] RecordingStats stats() const;

 protected:
  /// The one allocation a message makes on the push thread, and so the one
  /// place a test can make the tap path throw. Production never overrides it.
  [[nodiscard]] virtual std::vector<uint8_t> copyPayload(Span<const uint8_t> bytes);

 private:
  class SourceTap;
  class BudgetReservation;

  struct ChannelAdd {
    uint32_t logical_channel = 0;
    RecordedBinding binding;
  };
  struct MessageItem {
    uint32_t logical_channel = 0;
    int64_t log_time_ns = 0;
    uint64_t sequence = 0;  ///< only to make this message's size-index key unique
    std::vector<uint8_t> bytes;
  };
  /// The channel-add arm is boxed: its strings would otherwise set the size of
  /// every queued message, and messages outnumber adds by orders of magnitude.
  using Item = std::variant<std::unique_ptr<ChannelAdd>, MessageItem>;
  /// A list, not a deque: eviction removes from the MIDDLE while the writer
  /// drains the front, and only list iterators survive both.
  using Queue = std::list<Item>;
  /// Queued messages keyed by (cost, sequence): the largest is the last entry and any message erases in O(log n).
  /// Scanning the queue per eviction would make every message pay once the sink is saturated.
  using SizeIndex = std::map<std::pair<uint64_t, uint64_t>, Queue::iterator>;

  /// What one message costs the budget: payload plus queue item; charged at enqueue, refunded at dequeue or eviction
  /// through this one definition.
  [[nodiscard]] static constexpr uint64_t queuedCost(uint64_t payload_bytes) {
    return payload_bytes + sizeof(MessageItem);
  }

  /// Called by taps. Channel lookup-or-open and admission are ONE critical section, so two push threads racing a
  /// binding's first message open a single channel. Returns kContinue while the recording runs, dropped or not.
  /// Admission compares the newcomer against the INDEXED (fully enqueued) messages, so a payload still being copied
  /// is not yet an eviction candidate; concurrent producers can at worst pick a suboptimal victim, never corrupt the
  /// accounting.
  [[nodiscard]] TapVerdict enqueueMessage(
      uint32_t binding_id, const RecordedBindingView& binding, int64_t log_time_ns, Span<const uint8_t> bytes);
  /// The logical channel of `binding_id`, opened from `binding` on its first message; the id is assigned
  /// synchronously so the following message can reference it before the writer processes the add. Adds are never
  /// charged to the budget. Call under mu_, in kRunning.
  [[nodiscard]] uint32_t channelFor(uint32_t binding_id, const RecordedBindingView& binding);
  /// Frees budget for a message costing `incoming_cost` by discarding the
  /// largest of {queued messages, that message} until it fits, counting each
  /// discard. False = the incoming message is the one discarded, because
  /// nothing queued is bigger. Call under mu_.
  [[nodiscard]] bool makeRoomFor(uint64_t incoming_cost);
  /// Discards the biggest queued message and counts it. Call under mu_ with a
  /// non-empty size index.
  void evictLargest();
  /// Refunds and forgets everything queued: data the truncation lost, neither written nor a budget drop. Call under
  /// mu_.
  void discardQueue() noexcept;
  /// The writer thread: runs drainQueue(), contains whatever it throws as a
  /// truncation, and refunds what is still queued once nothing will drain it.
  void writerLoop() noexcept;
  /// The writer thread's loop proper. Leaves when the queue is empty after drain_and_exit_, or at once after a sink
  /// write error (a sink that rejected a write is never called again). An addChannel rejection only truncates: the
  /// sink is healthy and other channels' messages still land.
  void drainQueue();
  /// Closes the sink with a truncated summary and publishes it, for the one
  /// failure that leaves a recording with no writer thread to drain it.
  /// Returns the reason.
  std::string finalizeWithoutWriter(std::string reason);
  /// sink_->close() with a throw turned into an error Status, so a sink failure
  /// of either kind reads the same to its callers.
  [[nodiscard]] Status closeSink(const RecordingSummary& summary);
  /// Ends the recording with data loss, from any thread. Accepted from kRunning and kStopping (a sink failure during
  /// stop()'s final drain must not be lost) but never from a terminal state or once stop() froze the summary, so the
  /// first reason wins. The state flips before the reason ("<reason>: <detail>") is stored, best-effort: this may run
  /// in a bad_alloc handler, where an allocation failure costs the reason its text, not the truncation.
  void truncate(std::string_view reason, const char* detail = nullptr) noexcept;
  /// what() of the exception a catch block is handling, "unknown exception"
  /// for one that is not a std::exception. Call from a catch block only; the
  /// pointer lives as long as that block does.
  [[nodiscard]] static const char* activeExceptionWhat() noexcept;
  /// Now as `YYYY-MM-DDTHH:MM:SSZ`, the shape the recording metadata carries.
  [[nodiscard]] static std::string nowUtcIso();

  std::unique_ptr<RecordingSink>
      sink_;                 ///< writer thread only, plus open() before it exists and close() after the join
  RecorderOptions options_;  ///< immutable after start(), except info.started_utc set there

  /// The one lock. Guards every member below it up to writer_, except those
  /// marked writer-thread only.
  mutable std::mutex mu_;
  /// The writer waits here for items or drain_and_exit_; a stop() that raced
  /// the first one waits here for summary_published_.
  std::condition_variable items_cv_;
  std::condition_variable space_cv_;  ///< kBlock producers wait for refunds or the end of admission
  Queue queue_;
  SizeIndex size_index_;       ///< queued messages only, since adds are never evicted
  uint64_t queued_bytes_ = 0;  ///< queuedCost() of every queued message plus outstanding reservations
  uint64_t next_message_sequence_ = 0;
  uint64_t dropped_messages_ = 0;       ///< budget drops only; a truncation's discards are not counted
  uint64_t written_messages_ = 0;       ///< folded in by the writer on its next dequeue, exact once it has left
  uint64_t written_payload_bytes_ = 0;  ///< same discipline as written_messages_
  State state_ = State::kIdle;
  bool drain_and_exit_ = false;     ///< the writer drains what is queued, then leaves
  bool stop_entered_ = false;       ///< the first stop() owns the join and the close; later ones wait
  bool summary_frozen_ = false;     ///< the summary's facts are read: a truncate() from here on is too late
  bool summary_published_ = false;  ///< summary_ is final and may be re-returned
  std::string truncated_reason_;
  uint32_t next_logical_channel_ = 1;
  std::unordered_map<uint32_t, uint32_t> channels_;       ///< the source's binding id → logical channel
  std::unordered_map<uint32_t, uint16_t> sink_channels_;  ///< writer thread only: logical → sink id
  RecordingSummary summary_;                              ///< published by stop() or finalizeWithoutWriter()
  std::vector<std::pair<std::string, std::string>>
      extra_metadata_;  ///< queued by setExtraMetadata(), copied into summary_ at stop()
  std::thread writer_;  ///< created by start(), joined by the first stop()
};

}  // namespace PJ
