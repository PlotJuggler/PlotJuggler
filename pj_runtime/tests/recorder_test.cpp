// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
// Recorder: ordering, channels opened by their first message, the overflow drop
// policy (the largest message loses) and the stop/truncate lifecycle — all
// against a fake sink, no files. Every wait is a handshake with the fake sink
// rather than a sleep.
#include "pj_runtime/Recorder.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

/// Guard on every handshake: long enough that a correct recorder never hits it,
/// short enough that a wrong expectation fails the test instead of hanging it.
constexpr auto kHandshakeTimeout = 10s;

struct FakeSink : PJ::RecordingSink {
  struct Written {
    uint16_t channel;
    int64_t ts;
    std::vector<uint8_t> bytes;
  };

  std::mutex mu;
  std::condition_variable cv;  ///< carries both the progress handshakes and release()
  std::vector<PJ::RecordedBinding> channels;
  std::vector<Written> written;
  PJ::RecordingSummary closed;
  /// Where close() also copies the summary, for the tests whose recorder — and
  /// with it this sink — is gone by the time they assert.
  std::shared_ptr<PJ::RecordingSummary> closed_mirror;
  int open_calls = 0;
  int close_calls = 0;
  int writes_entered = 0;     ///< write() calls that reached the sink, parked ones included
  bool block_writes = false;  ///< write() parks after announcing itself, until release()
  bool released = false;
  std::string fail_open_with;  // non-empty: the matching call fails with this message
  std::string fail_add_channel_with;
  std::string fail_write_with;
  std::string fail_close_with;
  bool throw_on_write = false;  ///< write() throws instead of returning: the writer thread must contain it
  bool throw_on_open = false;   ///< open() throws instead of returning: start() must contain it
  bool throw_on_close = false;  ///< close() throws instead of returning: stop() must contain it

  PJ::Status open(const std::filesystem::path&, const PJ::RecordingInfo&) override {
    std::lock_guard lock(mu);
    ++open_calls;
    if (throw_on_open) {
      throw std::runtime_error("open exploded");
    }
    if (!fail_open_with.empty()) {
      return PJ::unexpected(fail_open_with);
    }
    return PJ::okStatus();
  }

  PJ::Expected<uint16_t> addChannel(const PJ::RecordedBinding& binding) override {
    std::lock_guard lock(mu);
    if (!fail_add_channel_with.empty()) {
      return PJ::unexpected(fail_add_channel_with);
    }
    channels.push_back(binding);
    return static_cast<uint16_t>(channels.size());  // 1-based like mcap
  }

  PJ::Status write(uint16_t channel, int64_t ts, PJ::Span<const uint8_t> bytes) override {
    std::unique_lock lock(mu);
    ++writes_entered;
    cv.notify_all();  // the writer thread is provably inside write() from here on
    cv.wait(lock, [this] { return !block_writes || released; });
    if (throw_on_write) {
      throw std::runtime_error("sink exploded");
    }
    if (!fail_write_with.empty()) {
      return PJ::unexpected(fail_write_with);
    }
    written.push_back({channel, ts, std::vector<uint8_t>(bytes.begin(), bytes.end())});
    return PJ::okStatus();
  }

  PJ::Status close(const PJ::RecordingSummary& summary) override {
    std::lock_guard lock(mu);
    ++close_calls;
    closed = summary;
    if (closed_mirror != nullptr) {
      *closed_mirror = summary;
    }
    if (throw_on_close) {
      throw std::runtime_error("close exploded");
    }
    if (!fail_close_with.empty()) {
      return PJ::unexpected(fail_close_with);
    }
    return PJ::okStatus();
  }

  void blockWrites() {
    std::lock_guard lock(mu);
    block_writes = true;
  }

  void failWritesWith(std::string reason) {
    std::lock_guard lock(mu);
    fail_write_with = std::move(reason);
  }

  void release() {
    {
      std::lock_guard lock(mu);
      released = true;
    }
    cv.notify_all();
  }

  /// Returns once `count` write() calls have been entered (parked ones count).
  [[nodiscard]] bool waitForWritesEntered(int count) {
    std::unique_lock lock(mu);
    // GCC 11 TSan does not intercept the CLOCK_MONOTONIC wait used by wait_for (GCC PR 101978).
    return cv.wait_until(
        lock, std::chrono::system_clock::now() + kHandshakeTimeout, [this, count] { return writes_entered >= count; });
  }
};

/// The binding view a runtime host carries on each message.
PJ::RecordedBindingView bindingView(std::string_view topic) {
  return PJ::RecordedBindingView{.topic = topic, .encoding = "json", .type_name = "T", .schema_bytes = "{}"};
}

std::vector<uint8_t> bytesOf(const std::string& text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

/// A payload of `size` bytes, `fill` making it identifiable at the sink.
std::vector<uint8_t> payloadOf(size_t size, uint8_t fill) {
  return std::vector<uint8_t>(size, fill);
}

/// One pushed message, as a runtime host delivers it.
PJ::TapVerdict push(
    const std::shared_ptr<PJ::RecordTap>& tap, uint32_t binding_id, std::string_view topic, int64_t log_time_ns,
    const std::vector<uint8_t>& bytes) {
  return tap->onMessage(
      binding_id, bindingView(topic), log_time_ns, PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
}

PJ::RecorderOptions options(uint64_t budget = 1 << 20) {
  PJ::RecorderOptions opts;
  opts.path = "/unused/by/fake/sink.mcap";
  opts.queue_budget_bytes = budget;
  opts.info.source_display_name = "src";
  opts.info.capture_ordinal = 1;
  return opts;
}

/// Starts `recorder` with its writer parked inside write() holding `primer`,
/// which leaves the queue empty: every message a test pushes afterwards meets
/// the byte budget with nothing else in flight. Returns the tap to push through.
/// The budgets below are sized in whole payload bytes with room to spare, so
/// the per-item framing queuedCost() adds — which differs between 64-bit and
/// wasm builds — never decides an outcome.
std::shared_ptr<PJ::RecordTap> startWithParkedWriter(
    const std::shared_ptr<PJ::Recorder>& recorder, FakeSink* sink, const std::vector<uint8_t>& primer) {
  sink->blockWrites();
  EXPECT_TRUE(recorder->start().has_value());
  auto tap = recorder->tapFor();
  EXPECT_EQ(push(tap, 1, "/a", 1, primer), PJ::TapVerdict::kContinue);
  EXPECT_TRUE(sink->waitForWritesEntered(1));
  return tap;
}

/// Joins its threads when it goes out of scope, so a failed assertion above the
/// explicit join() unwinds cleanly instead of terminating the binary.
struct ScopedThreads {
  std::vector<std::thread> threads;

  void join() {
    for (auto& thread : threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

  ~ScopedThreads() {
    join();
  }
};

/// A recorder whose payload copy throws, standing in for an allocation failure
/// on the push thread: the one exception the tap path has to contain.
struct ThrowingCopyRecorder final : PJ::Recorder {
  using PJ::Recorder::Recorder;

 protected:
  std::vector<uint8_t> copyPayload(PJ::Span<const uint8_t>) override {
    throw std::runtime_error("copy exploded");
  }
};

TEST(Recorder, WritesChannelsThenMessagesInOrderAndClosesClean) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options());
  ASSERT_TRUE(recorder->start().has_value());
  auto tap = recorder->tapFor();

  const auto a1 = bytesOf("a1");
  EXPECT_EQ(push(tap, 7, "/a", 10, a1), PJ::TapVerdict::kContinue);
  const auto b1 = bytesOf("b1");
  EXPECT_EQ(push(tap, 9, "/b", 20, b1), PJ::TapVerdict::kContinue);  // a binding first seen mid-recording
  const auto a2 = bytesOf("a2");
  EXPECT_EQ(push(tap, 7, "/a", 30, a2), PJ::TapVerdict::kContinue);

  const auto summary = recorder->stop();
  EXPECT_FALSE(summary.truncated);
  EXPECT_FALSE(summary.file_incomplete);
  EXPECT_EQ(summary.messages, 3u);
  EXPECT_EQ(summary.payload_bytes, 6u);
  EXPECT_EQ(recorder->state(), PJ::Recorder::State::kFinished);

  std::lock_guard lock(sink->mu);
  ASSERT_EQ(sink->channels.size(), 2u);
  EXPECT_EQ(sink->channels[0].topic, "/a");
  EXPECT_EQ(sink->channels[1].topic, "/b");
  ASSERT_EQ(sink->written.size(), 3u);
  EXPECT_EQ(sink->written[0].channel, 1);
  EXPECT_EQ(sink->written[0].ts, 10);
  EXPECT_EQ(sink->written[1].channel, 2);
  EXPECT_EQ(sink->written[2].channel, 1);
  EXPECT_EQ(sink->written[2].bytes, a2);
  EXPECT_EQ(sink->close_calls, 1);
}

// Nothing queued is bigger, so the newcomer is the largest member of the set
// and it is the one discarded. The queue is left untouched.
TEST(Recorder, AnIncomingMessageBiggerThanEveryQueuedOneIsTheOneDropped) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options(/*budget=*/5000));
  const auto primer = payloadOf(8, 0xAA);
  auto tap = startWithParkedWriter(recorder, sink, primer);

  const auto small = payloadOf(2000, 0xC2);
  const auto big = payloadOf(4000, 0xB1);
  EXPECT_EQ(push(tap, 1, "/a", 2, small), PJ::TapVerdict::kContinue);
  EXPECT_EQ(push(tap, 1, "/a", 3, big), PJ::TapVerdict::kContinue);

  sink->release();
  const auto summary = recorder->stop();
  EXPECT_FALSE(summary.truncated);
  EXPECT_EQ(summary.messages, 2u);
  EXPECT_EQ(summary.dropped_messages, 1u);

  std::lock_guard lock(sink->mu);
  ASSERT_EQ(sink->written.size(), 2u);
  EXPECT_EQ(sink->written[1].bytes, small);
}

// Which message loses is decided by size alone: the biggest goes even when it
// sits in the middle of the queue, and the survivors keep their FIFO order, so
// a drop is a hole and never a reordering.
TEST(Recorder, EvictionTakesTheBiggestMessageWhereverItSitsInTheQueue) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options(/*budget=*/8000));
  const auto primer = payloadOf(8, 0xAA);
  auto tap = startWithParkedWriter(recorder, sink, primer);

  const auto first = payloadOf(1000, 0x11);
  const auto biggest = payloadOf(4000, 0x22);
  const auto last = payloadOf(1500, 0x33);
  EXPECT_EQ(push(tap, 1, "/a", 2, first), PJ::TapVerdict::kContinue);
  EXPECT_EQ(push(tap, 1, "/a", 3, biggest), PJ::TapVerdict::kContinue);
  EXPECT_EQ(push(tap, 1, "/a", 4, last), PJ::TapVerdict::kContinue);
  const auto newcomer = payloadOf(2000, 0x44);  // 6500 + 2000 exceeds the budget
  EXPECT_EQ(push(tap, 1, "/a", 5, newcomer), PJ::TapVerdict::kContinue);

  sink->release();
  const auto summary = recorder->stop();
  EXPECT_EQ(summary.messages, 4u);
  EXPECT_EQ(summary.dropped_messages, 1u);

  std::lock_guard lock(sink->mu);
  ASSERT_EQ(sink->written.size(), 4u);
  EXPECT_EQ(sink->written[0].bytes, primer);
  EXPECT_EQ(sink->written[1].bytes, first);
  EXPECT_EQ(sink->written[2].bytes, last);
  EXPECT_EQ(sink->written[3].bytes, newcomer);
}

// The contract ingest depends on: onMessage is memory-only. With the writer
// parked and the queue at its cap, both overflow paths — evicting to make room
// and discarding the newcomer — must return at once, and keep the tap attached.
TEST(Recorder, PushingIntoAFullQueueNeverWaitsForTheWriter) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options(/*budget=*/5000));
  const auto primer = payloadOf(8, 0xAA);
  auto tap = startWithParkedWriter(recorder, sink, primer);

  EXPECT_EQ(push(tap, 1, "/a", 2, payloadOf(4000, 0xB1)), PJ::TapVerdict::kContinue);  // fills the budget

  const auto before_evicting = std::chrono::steady_clock::now();
  const auto evicting = push(tap, 1, "/a", 3, payloadOf(2000, 0xC2));
  const auto evicting_wait = std::chrono::steady_clock::now() - before_evicting;

  const auto before_discarding = std::chrono::steady_clock::now();
  const auto discarding = push(tap, 1, "/a", 4, payloadOf(9000, 0xD3));  // over the whole budget
  const auto discarding_wait = std::chrono::steady_clock::now() - before_discarding;

  EXPECT_EQ(evicting, PJ::TapVerdict::kContinue);
  EXPECT_EQ(discarding, PJ::TapVerdict::kContinue);
  // Generous by orders of magnitude: these are a few memory operations under
  // one mutex, while the writer is parked for the whole test.
  EXPECT_LT(evicting_wait, 100ms);
  EXPECT_LT(discarding_wait, 100ms);
  EXPECT_EQ(recorder->state(), PJ::Recorder::State::kRunning);

  sink->release();
  const auto summary = recorder->stop();
  EXPECT_FALSE(summary.truncated);
  EXPECT_EQ(summary.dropped_messages, 2u);
}

TEST(Recorder, SinkWriteErrorTruncatesWithTheSinkMessage) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  sink->fail_write_with = "disk full";
  sink->fail_close_with = "recording finalize failed: disk full";  // a real sink fails close() too
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options());
  ASSERT_TRUE(recorder->start().has_value());
  auto tap = recorder->tapFor();
  const auto payload = bytesOf("x");
  (void)push(tap, 1, "/a", 1, payload);
  ASSERT_TRUE(sink->waitForWritesEntered(1));
  const auto summary = recorder->stop();
  EXPECT_TRUE(summary.truncated);
  EXPECT_EQ(summary.truncated_reason, "disk full");
  EXPECT_TRUE(summary.file_incomplete);
  EXPECT_EQ(recorder->state(), PJ::Recorder::State::kTruncated);
  // Stopping a truncated recording twice must not re-finalize the sink.
  const auto again = recorder->stop();
  EXPECT_EQ(again.truncated_reason, summary.truncated_reason);
  std::lock_guard lock(sink->mu);
  EXPECT_EQ(sink->close_calls, 1);
}

TEST(Recorder, AddChannelFailureTruncatesEvenWhenStopRacesTheWriter) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  sink->fail_add_channel_with = "channel rejected";
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options());
  ASSERT_TRUE(recorder->start().has_value());
  auto tap = recorder->tapFor();
  const auto payload = bytesOf("x");
  (void)push(tap, 1, "/a", 1, payload);
  // No handshake on purpose: stop() usually reaches kStopping before the writer
  // pops the add, and the rejection must be reported either way.
  const auto summary = recorder->stop();
  EXPECT_TRUE(summary.truncated);
  EXPECT_EQ(summary.truncated_reason, "channel rejected");
  EXPECT_EQ(recorder->state(), PJ::Recorder::State::kTruncated);
  std::lock_guard lock(sink->mu);
  EXPECT_TRUE(sink->closed.truncated);
}

TEST(Recorder, WriteRejectedDuringTheFinalDrainStillTruncates) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  sink->blockWrites();
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options());
  ASSERT_TRUE(recorder->start().has_value());
  auto tap = recorder->tapFor();
  const auto payload = bytesOf("x");
  EXPECT_EQ(push(tap, 1, "/a", 1, payload), PJ::TapVerdict::kContinue);
  ASSERT_TRUE(sink->waitForWritesEntered(1));  // the writer is parked inside write()
  sink->failWritesWith("no space left on device");
  // Release only once stop() has driven the recorder to kStopping, so the
  // rejection lands in the final drain rather than while the recording still runs.
  std::thread releaser([sink, &recorder] {
    while (recorder->state() != PJ::Recorder::State::kStopping) {
      std::this_thread::yield();  // stop() is not waiting on us until it joins
    }
    sink->release();
  });
  const auto summary = recorder->stop();
  releaser.join();
  EXPECT_TRUE(summary.truncated);
  EXPECT_EQ(summary.truncated_reason, "no space left on device");
  EXPECT_EQ(recorder->state(), PJ::Recorder::State::kTruncated);
  std::lock_guard lock(sink->mu);
  EXPECT_TRUE(sink->written.empty());
  EXPECT_TRUE(sink->closed.truncated);
}

TEST(Recorder, StopIsIdempotent) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options());
  ASSERT_TRUE(recorder->start().has_value());
  auto tap = recorder->tapFor();
  const auto payload = bytesOf("x");
  EXPECT_EQ(push(tap, 1, "/a", 1, payload), PJ::TapVerdict::kContinue);

  const auto first = recorder->stop();
  const auto second = recorder->stop();
  EXPECT_EQ(second.stopped_utc, first.stopped_utc);
  EXPECT_EQ(second.truncated, first.truncated);
  EXPECT_EQ(second.truncated_reason, first.truncated_reason);
  EXPECT_EQ(second.file_incomplete, first.file_incomplete);
  EXPECT_EQ(second.messages, first.messages);
  EXPECT_EQ(second.payload_bytes, first.payload_bytes);
  EXPECT_EQ(recorder->state(), PJ::Recorder::State::kFinished);
  std::lock_guard lock(sink->mu);
  EXPECT_EQ(sink->close_calls, 1);
}

TEST(Recorder, FailedOpenLeavesTheRecorderIdleAndNeverClosesTheSink) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  sink->fail_open_with = "permission denied";
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options());
  const auto started = recorder->start();
  ASSERT_FALSE(started.has_value());
  EXPECT_EQ(started.error(), "permission denied");
  EXPECT_EQ(recorder->state(), PJ::Recorder::State::kIdle);

  const auto summary = recorder->stop();
  EXPECT_FALSE(summary.truncated);
  EXPECT_EQ(summary.messages, 0u);
  EXPECT_TRUE(summary.stopped_utc.empty());
  std::lock_guard lock(sink->mu);
  EXPECT_EQ(sink->close_calls, 0);
}

// A message that cannot fit the budget even alone is never recorded: no queue
// state can make room for it, so it is dropped against an empty queue and the
// counters are the only trace of it.
TEST(Recorder, AMessageLargerThanTheWholeBudgetIsNeverRecorded) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options(/*budget=*/4));
  ASSERT_TRUE(recorder->start().has_value());
  auto tap = recorder->tapFor();
  const auto oversized = bytesOf("0123456789");  // 10 bytes against a 4-byte budget
  EXPECT_EQ(push(tap, 1, "/a", 1, oversized), PJ::TapVerdict::kContinue);

  const auto summary = recorder->stop();
  EXPECT_FALSE(summary.truncated);
  EXPECT_EQ(summary.messages, 0u);
  EXPECT_EQ(summary.payload_bytes, 0u);
  EXPECT_EQ(summary.dropped_messages, 1u);
  std::lock_guard lock(sink->mu);
  EXPECT_TRUE(sink->written.empty());
  EXPECT_EQ(sink->channels.size(), 1u);  // its channel was opened before the message was weighed
}

// Four push threads on one recorder, stopped while their pushes are in flight.
// No wait here depends on the scheduler reaching a count: the writer is parked
// inside the sink, the producers park on a latch after their first half, and
// the test releases both itself before stop() overlaps the second half. Every
// producer is joined on every path, so a failed expectation cannot terminate
// the binary.
TEST(Recorder, ConcurrentTapsSurviveAStopMidFlight) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options(/*budget=*/1024));
  const auto primer = bytesOf("primer");
  auto primer_tap = startWithParkedWriter(recorder, sink, primer);  // the writer is provably mid-flight

  constexpr int kTapCount = 4;
  constexpr int kMessagesPerTap = 200;
  std::atomic<uint64_t> accepted{1};  ///< pushes the recorder took on, dropped ones included; the primer is one
  std::atomic<int> refused{0};        ///< producers that met kStopRecording and detached
  std::latch first_half_pushed(kTapCount);
  std::latch resume(1);
  ScopedThreads producers;  // declared after the latches: joined before they die
  for (int index = 0; index < kTapCount; ++index) {
    producers.threads.emplace_back([recorder, &accepted, &refused, &first_half_pushed, &resume, index] {
      const std::string suffix = std::to_string(index);
      auto tap = recorder->tapFor();  // one source, several push threads
      const uint32_t binding_id = 10u + static_cast<uint32_t>(index);
      const std::string topic = "/topic" + suffix;
      const auto payload = bytesOf("payload" + suffix);
      // False once the tap is detached, which is what the runtime host does on
      // kStopRecording: no later message is pushed through it.
      const auto pushRange = [&](int from, int to) {
        for (int seq = from; seq < to; ++seq) {
          if (push(tap, binding_id, topic, seq, payload) == PJ::TapVerdict::kStopRecording) {
            refused.fetch_add(1);
            return false;
          }
          accepted.fetch_add(1);
        }
        return true;
      };
      const bool attached = pushRange(0, kMessagesPerTap / 2);
      // Unconditional, so the test thread's waits below are released whatever
      // the pushes returned.
      first_half_pushed.count_down();
      resume.wait();
      if (attached) {
        pushRange(kMessagesPerTap / 2, kMessagesPerTap);
      }
    });
  }
  // Only EXPECTs from here to the joins: an early return would strand the
  // producers on `resume`.
  first_half_pushed.wait();  // every producer is parked with half its pushes in; the writer is still inside write()
  sink->release();
  resume.count_down();  // the second half of every producer now races the stop
  const auto summary = recorder->stop();
  producers.join();

  EXPECT_FALSE(summary.truncated);
  EXPECT_LE(refused.load(), kTapCount);  // a producer detaches at most once, and only after the stop
  std::lock_guard lock(sink->mu);
  EXPECT_EQ(summary.messages, sink->written.size());
  EXPECT_FALSE(sink->written.empty());
  // Every message the recorder answered kContinue to is accounted for: written,
  // or dropped by the budget. Nothing may go missing unreported.
  EXPECT_EQ(summary.messages + summary.dropped_messages, accepted.load());
  for (const auto& entry : sink->written) {
    EXPECT_GE(static_cast<std::size_t>(entry.channel), 1u);
    EXPECT_LE(static_cast<std::size_t>(entry.channel), sink->channels.size());
  }
}

// Two push threads meeting a binding's first message at once must still open ONE
// channel: a second one would split the topic across two sink channels and cost
// a replay its ordering.
TEST(Recorder, ConcurrentFirstMessagesOnOneBindingOpenOneChannel) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options());
  ASSERT_TRUE(recorder->start().has_value());
  auto tap = recorder->tapFor();  // one tap: the binding table is the recorder's either way

  std::latch gate(2);
  std::vector<std::thread> producers;
  producers.reserve(2);
  for (int index = 0; index < 2; ++index) {
    producers.emplace_back([&tap, &gate, index] {
      const auto payload = bytesOf("p" + std::to_string(index));
      gate.arrive_and_wait();  // both threads enter onMessage together
      EXPECT_EQ(push(tap, 42, "/shared", index, payload), PJ::TapVerdict::kContinue);
    });
  }
  for (auto& producer : producers) {
    producer.join();
  }

  const auto summary = recorder->stop();
  EXPECT_FALSE(summary.truncated);
  EXPECT_EQ(summary.messages, 2u);

  std::lock_guard lock(sink->mu);
  ASSERT_EQ(sink->channels.size(), 1u);
  EXPECT_EQ(sink->channels[0].topic, "/shared");
  ASSERT_EQ(sink->written.size(), 2u);
  EXPECT_EQ(sink->written[0].channel, sink->written[1].channel);
}

// A sink that throws must end the recording, not the process: the writer thread
// has no caller to propagate to, so an escaping exception would std::terminate.
TEST(Recorder, ThrowingSinkTruncatesInsteadOfTerminating) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  sink->throw_on_write = true;
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options());
  ASSERT_TRUE(recorder->start().has_value());
  auto tap = recorder->tapFor();
  const auto payload = bytesOf("x");
  EXPECT_EQ(push(tap, 1, "/a", 1, payload), PJ::TapVerdict::kContinue);

  const auto summary = recorder->stop();
  EXPECT_TRUE(summary.truncated);
  EXPECT_NE(summary.truncated_reason.find("sink exploded"), std::string::npos) << summary.truncated_reason;
  EXPECT_EQ(recorder->state(), PJ::Recorder::State::kTruncated);
  EXPECT_EQ(summary.messages, 0u);
}

// The cause travels INSIDE the summary, so the sink writes it into the file:
// a reader must be able to tell a deliberate end from a lost one.
TEST(Recorder, TheTerminalCauseReachesTheSinkInsideTheClosingSummary) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options());
  ASSERT_TRUE(recorder->start().has_value());
  auto tap = recorder->tapFor();
  const auto payload = bytesOf("x");
  EXPECT_EQ(push(tap, 1, "/a", 1, payload), PJ::TapVerdict::kContinue);

  const auto summary = recorder->stop("source_ended");
  EXPECT_EQ(summary.terminal_cause, "source_ended");
  std::lock_guard lock(sink->mu);
  EXPECT_EQ(sink->closed.terminal_cause, "source_ended");
}

// A recording that lost data ended for THAT reason, whatever the owner asked
// to record: the cause a reader has to believe is the truncation.
TEST(Recorder, ATruncationOverridesTheRequestedTerminalCause) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  sink->fail_write_with = "disk full";
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options());
  ASSERT_TRUE(recorder->start().has_value());
  auto tap = recorder->tapFor();
  const auto payload = bytesOf("x");
  (void)push(tap, 1, "/a", 1, payload);
  ASSERT_TRUE(sink->waitForWritesEntered(1));

  const auto summary = recorder->stop("source_ended");
  EXPECT_TRUE(summary.truncated);
  EXPECT_EQ(summary.terminal_cause, "truncated");
  std::lock_guard lock(sink->mu);
  EXPECT_EQ(sink->closed.terminal_cause, "truncated");
}

TEST(Recorder, TheDestructorClosesWithTheShutdownCause) {
  auto closed = std::make_shared<PJ::RecordingSummary>();
  {
    auto owned = std::make_unique<FakeSink>();
    owned->closed_mirror = closed;
    auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options());
    ASSERT_TRUE(recorder->start().has_value());
    auto tap = recorder->tapFor();
    const auto payload = bytesOf("x");
    EXPECT_EQ(push(tap, 1, "/a", 1, payload), PJ::TapVerdict::kContinue);
  }
  EXPECT_EQ(closed->terminal_cause, "shutdown");
  EXPECT_FALSE(closed->truncated);
}

TEST(Recorder, DestructorStopsARunningRecordingAndTapsKeepItAlive) {
  // The recorder owns the sink, so nothing may be dereferenced afterwards: this
  // proves only that the tap co-owns the recorder and that the last release
  // joins the writer without hanging.
  std::shared_ptr<PJ::RecordTap> tap;
  {
    auto recorder = std::make_shared<PJ::Recorder>(std::make_unique<FakeSink>(), options());
    ASSERT_TRUE(recorder->start().has_value());
    tap = recorder->tapFor();
  }
  const auto payload = bytesOf("x");
  EXPECT_EQ(push(tap, 1, "/a", 1, payload), PJ::TapVerdict::kContinue);
  tap.reset();  // last reference: the recorder is destroyed here, and stops itself
  SUCCEED();
}

// A sink that rejected a write is left alone: nothing still queued can land, so
// the writer discards it instead of failing — and, for a real sink, compressing
// — every remaining message inside the owner's join.
TEST(Recorder, WriterStopsTouchingTheSinkAfterAWriteError) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options());
  const auto primer = bytesOf("x");
  auto tap = startWithParkedWriter(recorder, sink, primer);  // the first write() is parked

  constexpr int kQueuedBehindTheFailure = 49;
  for (int index = 0; index < kQueuedBehindTheFailure; ++index) {
    EXPECT_EQ(push(tap, 1, "/a", 2 + index, primer), PJ::TapVerdict::kContinue);
  }
  // The parked write fails once released, and so would every one after it.
  sink->failWritesWith("disk full");
  sink->release();

  const auto summary = recorder->stop();
  EXPECT_TRUE(summary.truncated);
  EXPECT_EQ(summary.truncated_reason, "disk full");
  EXPECT_EQ(summary.messages, 0u);
  EXPECT_EQ(recorder->state(), PJ::Recorder::State::kTruncated);
  std::lock_guard lock(sink->mu);
  EXPECT_TRUE(sink->written.empty());
  EXPECT_EQ(sink->writes_entered, 1) << "the writer kept calling write() on a sink that already reported an error";
}

// A close() that throws is a close() that failed: the summary reports an
// unfinalized, truncated file, stop() returns it, and every later stop()
// re-returns that same summary.
TEST(Recorder, AThrowingCloseIsReportedAsAnIncompleteFileNotThrownOutOfStop) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  sink->throw_on_close = true;
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options());
  ASSERT_TRUE(recorder->start().has_value());
  auto tap = recorder->tapFor();
  const auto payload = bytesOf("x");
  EXPECT_EQ(push(tap, 1, "/a", 1, payload), PJ::TapVerdict::kContinue);
  ASSERT_TRUE(sink->waitForWritesEntered(1));

  PJ::RecordingSummary summary;
  EXPECT_NO_THROW(summary = recorder->stop()) << "the sink's close() exception escaped Recorder::stop()";
  EXPECT_TRUE(summary.file_incomplete);
  EXPECT_TRUE(summary.truncated);
  EXPECT_EQ(summary.messages, 1u);
  EXPECT_EQ(recorder->state(), PJ::Recorder::State::kTruncated);

  // The stop() after it must re-return THAT summary, not a default one.
  const auto again = recorder->stop();
  EXPECT_EQ(again.messages, 1u) << "the second stop() returned a summary that was never published";
  EXPECT_EQ(again.stopped_utc, summary.stopped_utc);
  EXPECT_TRUE(again.file_incomplete);
  std::lock_guard lock(sink->mu);
  EXPECT_EQ(sink->close_calls, 1);  // never re-finalized
}

// An open() that throws comes back as the error Status the caller is written to
// handle, with the recorder left exactly as a returned error leaves it.
TEST(Recorder, AThrowingOpenFailsStartWithAStatusAndLeavesTheRecorderIdle) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  sink->throw_on_open = true;
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options());

  PJ::Status started = PJ::okStatus();
  EXPECT_NO_THROW(started = recorder->start()) << "the sink's open() exception escaped Recorder::start()";
  EXPECT_FALSE(started.has_value()) << "start() must report a throwing open() as an error";
  if (!started.has_value()) {
    EXPECT_NE(started.error().find("open exploded"), std::string::npos) << started.error();
  }
  EXPECT_EQ(recorder->state(), PJ::Recorder::State::kIdle);

  const auto summary = recorder->stop();
  EXPECT_TRUE(summary.stopped_utc.empty());
  std::lock_guard lock(sink->mu);
  EXPECT_EQ(sink->open_calls, 1);
  EXPECT_EQ(sink->close_calls, 0);  // a sink that never opened is never closed
}

// A stop() that races the first one waits for that one's summary — the join
// and the close happen once, and nobody is handed a default in the meantime.
TEST(Recorder, AStopRacingTheFirstStopReturnsTheSameSummary) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options());
  const auto primer = bytesOf("x");
  auto tap = startWithParkedWriter(recorder, sink, primer);  // the writer is parked inside write()

  // The owner's stop(): it reaches kStopping (set under mu_ before the join),
  // then blocks in the join for as long as the sink stays parked.
  PJ::RecordingSummary first;
  std::thread owner([&recorder, &first] { first = recorder->stop(); });
  const auto deadline = std::chrono::steady_clock::now() + kHandshakeTimeout;
  while (recorder->state() != PJ::Recorder::State::kStopping && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_EQ(recorder->state(), PJ::Recorder::State::kStopping);

  // The racing stop(), while the first is provably inside its join. The sink is
  // released only once the racer has entered stop(), so a stop() that waits for
  // the first one to finish cannot deadlock the test; the single window left —
  // the racer descheduled between the latch and its lock for the whole of the
  // owner's drain, join and close — could only turn this into a false pass.
  PJ::RecordingSummary second;
  std::latch entered(1);
  std::thread racer([&recorder, &second, &entered] {
    entered.count_down();
    second = recorder->stop();
  });
  entered.wait();
  sink->release();
  racer.join();  // every path joins both threads: the release above lets both stop() calls return
  owner.join();

  EXPECT_EQ(first.messages, 1u);
  EXPECT_FALSE(first.stopped_utc.empty());
  EXPECT_EQ(second.messages, first.messages) << "the racing stop() returned the unpublished default summary";
  EXPECT_FALSE(second.stopped_utc.empty()) << "the racing stop() returned the unpublished default summary";
  EXPECT_EQ(second.stopped_utc, first.stopped_utc);
  EXPECT_EQ(recorder->state(), PJ::Recorder::State::kFinished);
  std::lock_guard lock(sink->mu);
  EXPECT_EQ(sink->close_calls, 1);
}

// An exception on the push thread — an allocation failure in production — ends
// the recording with its reason and detaches the tap; it never escapes into
// the plugin's push. What was queued before it still lands: the sink is fine.
TEST(Recorder, AThrowingPushTruncatesWithTheReasonAndDetachesTheTap) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  auto recorder = std::make_shared<ThrowingCopyRecorder>(std::move(owned), options());
  ASSERT_TRUE(recorder->start().has_value());
  auto tap = recorder->tapFor();
  const auto payload = bytesOf("x");
  EXPECT_EQ(push(tap, 1, "/a", 1, payload), PJ::TapVerdict::kStopRecording);
  EXPECT_EQ(recorder->state(), PJ::Recorder::State::kTruncated);
  EXPECT_EQ(push(tap, 1, "/a", 2, payload), PJ::TapVerdict::kStopRecording);  // ended for good

  const auto summary = recorder->stop();
  EXPECT_TRUE(summary.truncated);
  EXPECT_NE(summary.truncated_reason.find("copy exploded"), std::string::npos) << summary.truncated_reason;
  EXPECT_FALSE(summary.file_incomplete);
  EXPECT_EQ(summary.messages, 0u);
  EXPECT_EQ(summary.dropped_messages, 0u);
  std::lock_guard lock(sink->mu);
  EXPECT_EQ(sink->channels.size(), 1u);  // opened before the copy, drained after the truncation
  EXPECT_TRUE(sink->written.empty());
  EXPECT_EQ(sink->close_calls, 1);
}

// What a write error leaves queued is the data the truncation lost: neither
// written nor a budget drop, so the counters keep their meaning, and stats()
// agrees with the summary once the recording has ended.
TEST(Recorder, MessagesDiscardedAfterAWriteErrorAreNeitherWrittenNorDropped) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), options(/*budget=*/5000));
  const auto primer = payloadOf(8, 0xAA);
  auto tap = startWithParkedWriter(recorder, sink, primer);

  const auto big = payloadOf(4000, 0xB1);
  const auto small = payloadOf(2000, 0xC2);
  EXPECT_EQ(push(tap, 1, "/a", 2, big), PJ::TapVerdict::kContinue);
  EXPECT_EQ(push(tap, 1, "/a", 3, small), PJ::TapVerdict::kContinue);  // evicts `big`: one budget drop
  sink->failWritesWith("disk full");
  sink->release();  // the parked primer write fails; `small` is discarded unwritten

  const auto summary = recorder->stop();
  EXPECT_TRUE(summary.truncated);
  EXPECT_EQ(summary.truncated_reason, "disk full");
  EXPECT_EQ(summary.messages, 0u);
  EXPECT_EQ(summary.payload_bytes, 0u);
  EXPECT_EQ(summary.dropped_messages, 1u);
  const auto stats = recorder->stats();
  EXPECT_EQ(stats.messages, summary.messages);
  EXPECT_EQ(stats.payload_bytes, summary.payload_bytes);
  EXPECT_EQ(stats.dropped_messages, summary.dropped_messages);
  EXPECT_EQ(push(tap, 1, "/a", 4, primer), PJ::TapVerdict::kStopRecording);
  std::lock_guard lock(sink->mu);
  EXPECT_EQ(sink->writes_entered, 1);
  EXPECT_TRUE(sink->written.empty());
}

TEST(Recorder, BlockingModeWaitsForSpaceAndPreservesOversizedMessages) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  auto opts = options(5000);
  opts.overflow_policy = PJ::RecorderOptions::OverflowPolicy::kBlock;
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), opts);
  auto tap = startWithParkedWriter(recorder, sink, bytesOf("primer"));
  const auto queued = payloadOf(4000, 0x11);
  const auto oversized = payloadOf(9000, 0x22);
  EXPECT_EQ(push(tap, 1, "/a", 2, queued), PJ::TapVerdict::kContinue);

  std::promise<void> entered;
  auto producer = std::async(std::launch::async, [&] {
    entered.set_value();
    return push(tap, 1, "/a", 3, oversized);
  });
  entered.get_future().wait();
  // A bounded negative check: the writer is parked, so no capacity can appear.
  EXPECT_EQ(producer.wait_for(50ms), std::future_status::timeout);
  sink->release();
  EXPECT_EQ(producer.wait_for(kHandshakeTimeout), std::future_status::ready);
  EXPECT_EQ(producer.get(), PJ::TapVerdict::kContinue);

  const auto summary = recorder->stop();
  EXPECT_FALSE(summary.truncated);
  EXPECT_EQ(summary.messages, 3u);
  EXPECT_EQ(summary.payload_bytes, 6u + queued.size() + oversized.size());
  EXPECT_EQ(summary.dropped_messages, 0u);
  ASSERT_EQ(sink->written.size(), 3u);
  EXPECT_EQ(sink->written[1].bytes, queued);
  EXPECT_EQ(sink->written[2].bytes, oversized);
  EXPECT_EQ(sink->written[2].ts, 3);
}

TEST(Recorder, StoppingReleasesBlockedProducersBeforeDiskFinishes) {
  for (const bool finalize_immediately : {false, true}) {
    SCOPED_TRACE(finalize_immediately);
    auto owned = std::make_unique<FakeSink>();
    FakeSink* sink = owned.get();
    auto opts = options(5000);
    opts.overflow_policy = PJ::RecorderOptions::OverflowPolicy::kBlock;
    auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), opts);
    auto tap = startWithParkedWriter(recorder, sink, bytesOf("primer"));
    const auto payload = payloadOf(4000, 0x11);
    EXPECT_EQ(push(tap, 1, "/a", 2, payload), PJ::TapVerdict::kContinue);
    std::promise<void> entered;
    auto producer = std::async(std::launch::async, [&] {
      entered.set_value();
      return push(tap, 1, "/a", 3, payload);
    });
    entered.get_future().wait();
    EXPECT_EQ(producer.wait_for(50ms), std::future_status::timeout);

    std::future<PJ::RecordingSummary> stopping;
    if (finalize_immediately) {
      stopping = std::async(std::launch::async, [&] { return recorder->stop("cancelled"); });
    } else {
      recorder->requestStop();
      recorder->requestStop();  // request and finalization are both idempotent
    }
    // The producer must leave while the sink remains parked, allowing its owner
    // to join it. Finalization still waits for the actual write to return.
    EXPECT_EQ(producer.wait_for(kHandshakeTimeout), std::future_status::ready);
    sink->release();
    EXPECT_EQ(producer.get(), PJ::TapVerdict::kStopRecording);
    const auto summary = finalize_immediately ? stopping.get() : recorder->stop("cancelled");
    EXPECT_FALSE(summary.truncated);
    EXPECT_EQ(summary.terminal_cause, "cancelled");
    EXPECT_EQ(summary.messages, 2u);
    EXPECT_EQ(summary.dropped_messages, 0u);
    EXPECT_EQ(sink->close_calls, 1);
  }
}

TEST(Recorder, SinkFailureReleasesBlockedProducersAndRejectsTheCapture) {
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  auto opts = options(5000);
  opts.overflow_policy = PJ::RecorderOptions::OverflowPolicy::kBlock;
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), opts);
  auto tap = startWithParkedWriter(recorder, sink, bytesOf("primer"));
  const auto payload = payloadOf(4000, 0x11);
  EXPECT_EQ(push(tap, 1, "/a", 2, payload), PJ::TapVerdict::kContinue);
  std::promise<void> entered;
  auto producer = std::async(std::launch::async, [&] {
    entered.set_value();
    return push(tap, 1, "/a", 3, payload);
  });
  entered.get_future().wait();
  EXPECT_EQ(producer.wait_for(50ms), std::future_status::timeout);
  sink->failWritesWith("disk full");
  sink->release();
  EXPECT_EQ(producer.wait_for(kHandshakeTimeout), std::future_status::ready);
  EXPECT_EQ(producer.get(), PJ::TapVerdict::kStopRecording);
  const auto summary = recorder->stop();
  EXPECT_TRUE(summary.truncated);
  EXPECT_EQ(summary.truncated_reason, "disk full");
  EXPECT_EQ(summary.messages, 0u);
  EXPECT_EQ(summary.dropped_messages, 0u);
}

TEST(Recorder, ConcurrentBlockingProducersPreserveEveryMessageAndChannelOrder) {
  // A zero budget still admits one message at a time, including its framing.
  auto opts = options(0);
  opts.overflow_policy = PJ::RecorderOptions::OverflowPolicy::kBlock;
  auto owned = std::make_unique<FakeSink>();
  FakeSink* sink = owned.get();
  auto recorder = std::make_shared<PJ::Recorder>(std::move(owned), opts);
  ASSERT_TRUE(recorder->start());
  constexpr int kProducers = 4;
  constexpr int kMessages = 250;
  ScopedThreads producers;
  for (uint32_t index = 0; index < kProducers; ++index) {
    producers.threads.emplace_back([recorder, index] {
      auto tap = recorder->tapFor();
      const auto payload = payloadOf(300, static_cast<uint8_t>(index));
      const auto topic = "/topic" + std::to_string(index);
      for (int sequence = 0; sequence < kMessages; ++sequence) {
        EXPECT_EQ(push(tap, index, topic, sequence, payload), PJ::TapVerdict::kContinue);
      }
    });
  }
  producers.join();
  const auto summary = recorder->stop();
  EXPECT_FALSE(summary.truncated);
  EXPECT_EQ(summary.messages, kProducers * kMessages);
  EXPECT_EQ(summary.payload_bytes, 300u * kProducers * kMessages);
  EXPECT_EQ(summary.dropped_messages, 0u);
  ASSERT_EQ(sink->channels.size(), kProducers);
  std::vector<int64_t> next_timestamp(kProducers, 0);
  for (const auto& message : sink->written) {
    ASSERT_GE(message.channel, 1u);
    ASSERT_LE(message.channel, kProducers);
    EXPECT_EQ(message.ts, next_timestamp[message.channel - 1]++);
  }
}

}  // namespace
