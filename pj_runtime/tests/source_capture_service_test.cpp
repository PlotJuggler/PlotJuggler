// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// End-to-end tests of the M3 SourceCaptureService over a REAL
// DataSourceRuntimeHost and a real cache directory: capture raw pushed bytes
// through the completion contract's gate, publish into SourceCacheStore, and
// resolve the same request back — plus every gate condition that must refuse.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "pj_runtime/McapRecordingWriter.h"
#include "pj_runtime/RecordingFormat.h"
#include "pj_runtime/RecordingSink.h"
#include "pj_runtime/SourceCaptureService.h"
#include "runtime_host_test_fixture.h"

namespace fs = std::filesystem;

namespace PJ::test {
namespace {

constexpr const char* kProvider = "test.provider";
constexpr const char* kDescriptor = R"({"kind":"capture-test-request","request":{},"v":1})";

using namespace std::chrono_literals;
constexpr auto kHandshakeTimeout = 10s;  // recorder_test's handshake guard

/// Injectable capture sink (a subset of recorder_test's FakeSink): write()
/// can park until release(), close() can fail on command. Lets the blocked-
/// cancellation and close-failure tests drive the capture through arm()/
/// finalize() with no real file involved.
struct FakeCaptureSink final : RecordingSink {
  std::mutex mu;
  std::condition_variable cv;
  int writes_entered = 0;
  bool block_writes = false;
  bool released = false;
  bool close_called = false;
  std::string fail_close_with;

  Status open(const std::filesystem::path&, const RecordingInfo&) override {
    return okStatus();
  }
  Expected<uint16_t> addChannel(const RecordedBinding&) override {
    return static_cast<uint16_t>(1);
  }
  Status write(uint16_t, int64_t, Span<const uint8_t>) override {
    std::unique_lock lock(mu);
    ++writes_entered;
    cv.notify_all();
    cv.wait(lock, [this] { return !block_writes || released; });
    return okStatus();
  }
  Status close(const RecordingSummary&) override {
    std::lock_guard lock(mu);
    close_called = true;
    if (!fail_close_with.empty()) {
      return PJ::unexpected(fail_close_with);
    }
    return okStatus();
  }
  void release() {
    {
      std::lock_guard lock(mu);
      released = true;
    }
    cv.notify_all();
  }
  [[nodiscard]] bool waitForWritesEntered(int count) {
    std::unique_lock lock(mu);
    return cv.wait_until(
        lock, std::chrono::system_clock::now() + kHandshakeTimeout, [this, count] { return writes_entered >= count; });
  }
};

[[nodiscard]] std::vector<char> fileBytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Sink factory over a SHARED FakeCaptureSink: the recorder owns a forwarding
/// shim, the test keeps its handle to park/fail the sink.
[[nodiscard]] SourceCaptureService::SinkFactory shimFactory(std::shared_ptr<FakeCaptureSink> sink) {
  return [sink]() -> std::unique_ptr<RecordingSink> {
    struct Shim final : RecordingSink {
      std::shared_ptr<FakeCaptureSink> inner;
      explicit Shim(std::shared_ptr<FakeCaptureSink> sink_ptr) : inner(std::move(sink_ptr)) {}
      Status open(const std::filesystem::path& path, const RecordingInfo& info) override {
        return inner->open(path, info);
      }
      Expected<uint16_t> addChannel(const RecordedBinding& binding) override {
        return inner->addChannel(binding);
      }
      Status write(uint16_t channel, int64_t ts, Span<const uint8_t> bytes) override {
        return inner->write(channel, ts, bytes);
      }
      Status close(const RecordingSummary& summary) override {
        return inner->close(summary);
      }
    };
    return std::make_unique<Shim>(sink);
  };
}

class SourceCaptureServiceTest : public RuntimeHostFixture {
 protected:
  SourceCaptureServiceTest() : RuntimeHostFixture("capture-test-source") {}

  void SetUp() override {
    RuntimeHostFixture::SetUp();
    std::random_device entropy;
    cache_root_ = fs::temp_directory_path() / ("source_capture_test-" + std::to_string(entropy()));
    store_.emplace(cache_root_);
    service_.emplace(*store_);
  }

  void TearDown() override {
    RuntimeHostFixture::TearDown();
    std::error_code ec;
    fs::remove_all(cache_root_, ec);
  }

  [[nodiscard]] Status attach(std::string_view descriptor = kDescriptor) {
    return runtime().attachSourceRecord(descriptor);
  }

  [[nodiscard]] Status complete(
      sdk::IngestOutcome outcome, const std::vector<std::string_view>& topics,
      PJ_ingest_completion_flags_t flags = PJ_INGEST_COMPLETION_FLAG_NONE) {
    return runtime().completeIngest(outcome, {topics.data(), topics.size()}, flags);
  }

  /// arm() then attach: recording arms on the attach, per the service design.
  [[nodiscard]] std::unique_ptr<SourceCaptureService::Capture> armAndAttach(std::string_view descriptor = kDescriptor) {
    auto capture = service_->arm(*host, kProvider);
    EXPECT_TRUE(attach(descriptor).has_value());
    return capture;
  }

  [[nodiscard]] SourceCaptureService::FinalizeResult finalize(
      std::unique_ptr<SourceCaptureService::Capture> capture, bool committed = true, bool cancelled = false) {
    return service_->finalize(std::move(capture), committed, cancelled);
  }

  /// Rebuild the service around an injected sink factory (P3's test seam).
  void remakeService(SourceCaptureService::SinkFactory factory) {
    service_.emplace(*store_, std::move(factory));
  }

  fs::path cache_root_;
  std::optional<SourceCacheStore> store_;
  std::optional<SourceCaptureService> service_;
};

TEST_F(SourceCaptureServiceTest, HappyPathPublishesAndResolves) {
  auto capture = armAndAttach();
  ASSERT_NE(capture, nullptr);

  const auto imu = bindTopic("/imu");
  const auto camera = bindTopic("/camera");
  push(imu, 100, {1, 2, 3});
  push(imu, 200, {4, 5, 6});
  push(camera, 150, {7, 8});
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu", "/camera"}));

  auto result = finalize(std::move(capture));
  ASSERT_TRUE(result.published) << result.reason;
  ASSERT_TRUE(result.artifact.has_value());
  EXPECT_TRUE(fs::exists(result.artifact->path));

  // The artifact is self-describing.
  auto manifest = SourceCaptureService::readManifest(result.artifact->path);
  ASSERT_TRUE(manifest.has_value()) << manifest.error();
  EXPECT_EQ(manifest->version, kCaptureManifestVersion);
  EXPECT_EQ(manifest->provider_id, kProvider);
  EXPECT_EQ(manifest->total_messages, 3u);
  EXPECT_EQ(manifest->requested_topic_messages.at("/imu"), 2u);
  EXPECT_EQ(manifest->requested_topic_messages.at("/camera"), 1u);

  // Release the publish pin, then the same request resolves from disk alone.
  result.artifact.reset();
  std::string miss;
  auto resolved = service_->resolve(kProvider, kDescriptor, &miss);
  ASSERT_TRUE(resolved.has_value()) << miss;
  EXPECT_EQ(resolved->manifest.total_messages, 3u);

  // A different provider or descriptor is a different identity: absent miss.
  EXPECT_FALSE(service_->resolve("other.provider", kDescriptor, &miss).has_value());
  EXPECT_FALSE(service_->resolve(kProvider, R"({"kind":"other","request":{},"v":1})", &miss).has_value());
}

TEST_F(SourceCaptureServiceTest, EmptyRequestedTopicNeedsTheAttestation) {
  auto capture = armAndAttach();
  const auto imu = bindTopic("/imu");
  push(imu, 100, {1});
  // "/empty" was requested but produced nothing; without the flag: refuse.
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu", "/empty"}));
  auto refused = finalize(std::move(capture));
  EXPECT_FALSE(refused.published);
  EXPECT_NE(refused.reason.find("emptiness was not attested"), std::string::npos);
}

TEST_F(SourceCaptureServiceTest, AttestedEmptyTopicPublishes) {
  auto capture = armAndAttach();
  const auto imu = bindTopic("/imu");
  push(imu, 100, {1});
  ASSERT_TRUE(
      complete(sdk::IngestOutcome::kCompleted, {"/imu", "/empty"}, PJ_INGEST_COMPLETION_FLAG_ATTESTS_EMPTY_TOPICS));
  auto result = finalize(std::move(capture));
  ASSERT_TRUE(result.published) << result.reason;
  auto manifest = SourceCaptureService::readManifest(result.artifact->path);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_TRUE(manifest->attests_empty_topics);
  EXPECT_EQ(manifest->requested_topic_messages.at("/empty"), 0u);
}

TEST_F(SourceCaptureServiceTest, CancellationNeverPublishes) {
  auto capture = armAndAttach();
  push(bindTopic("/imu"), 100, {1});
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
  SourceCaptureService::cancel(*capture);
  auto result = finalize(std::move(capture), /*committed=*/true, /*cancelled=*/true);
  EXPECT_FALSE(result.published);
  EXPECT_NE(result.reason.find("cancelled"), std::string::npos);
  // The aborted transaction removed its partial (lock/stamp files remain).
  for (const auto& entry : fs::directory_iterator(cache_root_)) {
    EXPECT_TRUE(entry.path().string().find(".partial") == std::string::npos) << entry.path();
  }
}

TEST_F(SourceCaptureServiceTest, MissingTerminalRefuses) {
  auto capture = armAndAttach();
  push(bindTopic("/imu"), 100, {1});
  auto no_terminal = finalize(std::move(capture));
  EXPECT_FALSE(no_terminal.published);
  EXPECT_NE(no_terminal.reason.find("no ingest terminal"), std::string::npos);
}

TEST_F(SourceCaptureServiceTest, NonCompletedTerminalRefuses) {
  auto capture = armAndAttach();
  push(bindTopic("/imu"), 200, {2});
  ASSERT_TRUE(complete(sdk::IngestOutcome::kFailed, {"/imu"}));
  auto failed = finalize(std::move(capture));
  EXPECT_FALSE(failed.published);
  EXPECT_NE(failed.reason.find("not COMPLETED"), std::string::npos);
}

TEST_F(SourceCaptureServiceTest, UncommittedTransactionRefuses) {
  auto capture = armAndAttach();
  push(bindTopic("/imu"), 100, {1});
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
  auto result = finalize(std::move(capture), /*committed=*/false);
  EXPECT_FALSE(result.published);
  EXPECT_NE(result.reason.find("did not commit"), std::string::npos);
}

TEST_F(SourceCaptureServiceTest, ConflictingTerminalVetoesForGood) {
  auto capture = armAndAttach();
  push(bindTopic("/imu"), 100, {1});
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
  // An identical repeat is idempotent...
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
  // ...a conflicting one is rejected AND permanently vetoes.
  EXPECT_FALSE(complete(sdk::IngestOutcome::kFailed, {"/imu"}));
  auto result = finalize(std::move(capture));
  EXPECT_FALSE(result.published);
  EXPECT_NE(result.reason.find("veto"), std::string::npos);
}

TEST_F(SourceCaptureServiceTest, AttachAfterPushVetoes) {
  auto capture = service_->arm(*host, kProvider);
  push(bindTopic("/imu"), 100, {1});
  EXPECT_FALSE(attach());  // too late, and not byte-identical to anything
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
  auto result = finalize(std::move(capture));
  EXPECT_FALSE(result.published);
  EXPECT_NE(result.reason.find("veto"), std::string::npos);
}

TEST_F(SourceCaptureServiceTest, PushAfterCompletionIsRejectedAndLatched) {
  auto capture = armAndAttach();
  const auto imu = bindTopic("/imu");
  push(imu, 100, {1});
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
  auto late = runtime().pushMessage(imu, 200, []() -> std::vector<uint8_t> { return {9}; });
  EXPECT_FALSE(late.has_value());  // sealed
  auto result = finalize(std::move(capture));
  EXPECT_FALSE(result.published);  // the rejected push latched a callback failure
  EXPECT_NE(result.reason.find("callback failure"), std::string::npos);
}

TEST_F(SourceCaptureServiceTest, UndeclaredRecordedTopicRefuses) {
  auto capture = armAndAttach();
  push(bindTopic("/imu"), 100, {1});
  push(bindTopic("/rogue"), 150, {2});
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
  auto result = finalize(std::move(capture));
  EXPECT_FALSE(result.published);
  EXPECT_NE(result.reason.find("outside the declared requested set"), std::string::npos);
}

TEST_F(SourceCaptureServiceTest, MissingAttachmentRefuses) {
  auto capture = service_->arm(*host, kProvider);
  push(bindTopic("/imu"), 100, {1});
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
  auto result = finalize(std::move(capture));
  EXPECT_FALSE(result.published);
  EXPECT_NE(result.reason.find("no source record"), std::string::npos);
}

// A malformed C-ABI call — null data pointer, nonzero size — must be refused by
// the empty-descriptor guard. Nothing may build a string_view over that size.
TEST_F(SourceCaptureServiceTest, RawAbiNullDescriptorWithNonZeroSizeRefuses) {
  const PJ_service_registry_t services = registry.view();
  const char* const name = sdk::DataSourceRuntimeHostService::kName;
  PJ_service_t service{};
  PJ_error_t lookup_error{};
  ASSERT_TRUE(services.vtable->get_service(
      services.ctx, PJ_string_view_t{name, std::strlen(name)}, sdk::DataSourceRuntimeHostService::kMinVersion, &service,
      &lookup_error))
      << lookup_error.message;
  const auto* vtable = static_cast<const PJ_data_source_runtime_host_vtable_t*>(service.vtable);
  ASSERT_NE(vtable->attach_source_record, nullptr);

  PJ_error_t error{};
  EXPECT_FALSE(vtable->attach_source_record(service.ctx, PJ_string_view_t{nullptr, 1}, &error));
  EXPECT_STREQ(error.message, "source record descriptor is empty");
  EXPECT_FALSE(host->sourceRecordDescriptor().has_value());
  EXPECT_EQ(host->ingestCallbackFailures(), 0u);
}

TEST_F(SourceCaptureServiceTest, EnvelopeRefusesUnknownAndCredentialFields) {
  auto capture = service_->arm(*host, kProvider);
  // Unknown top-level field: refused (allowlist), NOT latched, NOT vetoed.
  EXPECT_FALSE(attach(R"({"kind":"k","request":{},"v":1,"extra":1})").has_value());
  // Credential-shaped key at depth: refused.
  EXPECT_FALSE(attach(R"({"kind":"k","request":{"api_key":"x"},"v":1})").has_value());
  // 'label' is allowlisted but still typed: a non-string one is refused.
  EXPECT_FALSE(attach(R"({"kind":"k","request":{},"v":1,"label":7})").has_value());
  // Precedence, pinned: the SDK denies credential keys during traversal, so a
  // record that is BOTH credential-carrying and mistyped reports the credential.
  // Either way it is refused; this asserts which message wins so an SDK
  // precedence change lands here as a test diff instead of a surprise.
  const auto both_wrong = attach(R"({"kind":"","request":{"api_key":"x"},"v":1})");
  ASSERT_FALSE(both_wrong.has_value());
  EXPECT_EQ(both_wrong.error(), "pj.runtime.ingest: source record carries credential-shaped key: api_key");
  EXPECT_EQ(host->ingestCallbackFailures(), 0u);
  EXPECT_TRUE(host->captureVetoReason().empty());
  // A corrected attach before the first push still arms and publishes.
  ASSERT_TRUE(attach().has_value());
  push(bindTopic("/imu"), 100, {1});
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
  auto result = finalize(std::move(capture));
  EXPECT_TRUE(result.published) << result.reason;
}

TEST_F(SourceCaptureServiceTest, CancelLatchOutlivesTheArgument) {
  auto capture = armAndAttach();
  push(bindTopic("/imu"), 100, {1});
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
  SourceCaptureService::cancel(*capture);
  // Even a caller claiming "not cancelled" cannot publish past the latch.
  auto result = finalize(std::move(capture), /*committed=*/true, /*cancelled=*/false);
  EXPECT_FALSE(result.published);
  EXPECT_NE(result.reason.find("cancelled"), std::string::npos);
}

TEST_F(SourceCaptureServiceTest, EntirelyEmptyCaptureRefuses) {
  auto capture = armAndAttach();
  ASSERT_TRUE(
      complete(sdk::IngestOutcome::kCompleted, {"/only_empty"}, PJ_INGEST_COMPLETION_FLAG_ATTESTS_EMPTY_TOPICS));
  auto result = finalize(std::move(capture));
  EXPECT_FALSE(result.published);
  EXPECT_NE(result.reason.find("entirely empty"), std::string::npos);
}

TEST_F(SourceCaptureServiceTest, ReplacementAttachRearmsUnderTheNewIdentity) {
  auto capture = service_->arm(*host, kProvider);
  ASSERT_TRUE(attach(R"({"kind":"first","request":{},"v":1})").has_value());
  ASSERT_TRUE(attach(kDescriptor).has_value());  // last attach before ingest wins
  push(bindTopic("/imu"), 100, {1});
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
  auto result = finalize(std::move(capture));
  ASSERT_TRUE(result.published) << result.reason;
  result.artifact.reset();
  // Published under the FINAL descriptor's identity, not the first one.
  std::string miss;
  EXPECT_TRUE(service_->resolve(kProvider, kDescriptor, &miss).has_value()) << miss;
  EXPECT_FALSE(service_->resolve(kProvider, R"({"kind":"first","request":{},"v":1})", &miss).has_value());
}

TEST_F(SourceCaptureServiceTest, ArtifactWithoutManifestIsQuarantinedOnResolve) {
  auto capture = armAndAttach();
  push(bindTopic("/imu"), 100, {1});
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
  auto result = finalize(std::move(capture));
  ASSERT_TRUE(result.published) << result.reason;
  const fs::path artifact_path = result.artifact->path;
  result.artifact.reset();  // release the pin so quarantine can act

  // Swap the artifact for a valid MCAP that carries no pj.capture manifest —
  // structurally sound (the store's validator accepts it), semantically bare.
  {
    std::error_code ec;
    fs::remove(artifact_path, ec);
    McapRecordingWriter bare;
    ASSERT_TRUE(bare.open(artifact_path, RecordingInfo{}).has_value());
    ASSERT_TRUE(bare.close(RecordingSummary{}).has_value());
  }

  std::string miss;
  EXPECT_FALSE(service_->resolve(kProvider, kDescriptor, &miss).has_value());
  EXPECT_NE(miss.find("quarantined"), std::string::npos);
  // The heal removed it: the next resolve is a plain absent miss.
  EXPECT_FALSE(service_->resolve(kProvider, kDescriptor, &miss).has_value());
  EXPECT_FALSE(fs::exists(artifact_path));
}

// T1 (acceptance row 4): a published artifact is immutable — store cleanup and
// repeated resolve round-trips must not change a single byte.
TEST_F(SourceCaptureServiceTest, PublishedArtifactBytesAreImmutableAcrossCleanupAndResolves) {
  auto capture = armAndAttach();
  const auto imu = bindTopic("/imu");
  push(imu, 100, {1, 2, 3});
  push(imu, 200, {4, 5, 6});
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
  auto result = finalize(std::move(capture));
  ASSERT_TRUE(result.published) << result.reason;
  const fs::path artifact_path = result.artifact->path;
  result.artifact.reset();  // release the publish pin

  const std::vector<char> original = fileBytes(artifact_path);
  ASSERT_FALSE(original.empty());

  static_cast<void>(store_->cleanup());
  for (int round = 0; round < 3; ++round) {
    std::string miss;
    auto resolved = service_->resolve(kProvider, kDescriptor, &miss);
    ASSERT_TRUE(resolved.has_value()) << miss;
    EXPECT_EQ(resolved->artifact.path, artifact_path);
  }
  EXPECT_EQ(fileBytes(artifact_path), original) << "cleanup/resolve must never rewrite a published artifact";
}

// T2 (acceptance row 5): a producer blocked on the lossless capture queue is
// released by BOTH cancellation paths — the host's stop request and the
// service's cancel() — strictly BEFORE disk finalization, and nothing ever
// publishes. One TEST_F per path: each needs a fresh host generation.
class SourceCaptureServiceBlockedTest : public SourceCaptureServiceTest {
 protected:
  void runBlockedCancellation(bool via_host_stop) {
    auto sink = std::make_shared<FakeCaptureSink>();
    sink->block_writes = true;
    remakeService(shimFactory(sink));

    // A tiny queue: the first push parks inside the sink, the second fills the
    // budget, the third blocks the producer.
    auto capture = service_->arm(*host, kProvider, /*queue_budget_bytes=*/5000);
    // A fatal assertion below must not leave the writer parked — the capture's
    // and the producer future's destructors would hang on it. Released on
    // every exit path (release() is idempotent); destroyed before `capture`.
    const auto release_on_exit = std::unique_ptr<FakeCaptureSink, void (*)(FakeCaptureSink*)>(
        sink.get(), [](FakeCaptureSink* parked) { parked->release(); });
    ASSERT_TRUE(attach().has_value());
    const auto imu = bindTopic("/imu");
    push(imu, 100, std::vector<uint8_t>(64, 0x01));
    ASSERT_TRUE(sink->waitForWritesEntered(1)) << "the writer must be parked inside write()";
    push(imu, 200, std::vector<uint8_t>(4000, 0x02));

    auto runtime_view = runtime();  // resolved on the main thread; the view is thread-safe to use
    std::promise<void> entered;
    auto producer = std::async(std::launch::async, [&, runtime_view] {
      entered.set_value();
      return runtime_view.pushMessage(
          imu, Timestamp{300}, []() -> std::vector<uint8_t> { return std::vector<uint8_t>(9000, 0x03); });
    });
    entered.get_future().wait();
    EXPECT_EQ(producer.wait_for(50ms), std::future_status::timeout) << "the oversized push must block at budget";

    if (via_host_stop) {
      host->requestStop();  // fires the capture's stop-requested hook
    } else {
      SourceCaptureService::cancel(*capture);
    }
    // The producer must return while the sink is STILL parked — unblocking
    // must never wait for disk. Non-fatal on purpose: past this point every
    // check must still reach the release below.
    EXPECT_EQ(producer.wait_for(kHandshakeTimeout), std::future_status::ready);
    sink->release();
    static_cast<void>(producer.get());

    ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
    auto result = finalize(std::move(capture), /*committed=*/true, /*cancelled=*/true);
    EXPECT_FALSE(result.published);
    std::string miss;
    EXPECT_FALSE(service_->resolve(kProvider, kDescriptor, &miss).has_value()) << "nothing may publish";
  }
};

TEST_F(SourceCaptureServiceBlockedTest, ServiceCancelReleasesBlockedProducerBeforeDiskFinalization) {
  runBlockedCancellation(/*via_host_stop=*/false);
}

TEST_F(SourceCaptureServiceBlockedTest, HostStopReleasesBlockedProducerBeforeDiskFinalization) {
  runBlockedCancellation(/*via_host_stop=*/true);
}

// F3: a non-null sink factory returning nullptr is an arming failure surfaced
// at finalize, never a crash inside Recorder::start.
TEST_F(SourceCaptureServiceTest, NullSinkFromFactoryIsAnArmingFailureNotACrash) {
  remakeService([]() -> std::unique_ptr<RecordingSink> { return nullptr; });
  auto capture = armAndAttach();
  push(bindTopic("/imu"), 100, {1});
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
  auto result = finalize(std::move(capture));
  EXPECT_FALSE(result.published);
  EXPECT_NE(result.reason.find("never armed"), std::string::npos) << result.reason;
  EXPECT_NE(result.reason.find("no sink"), std::string::npos) << result.reason;
  std::string miss;
  EXPECT_FALSE(service_->resolve(kProvider, kDescriptor, &miss).has_value());
  // The aborted transaction left no partial behind.
  for (const auto& entry : fs::directory_iterator(cache_root_)) {
    EXPECT_TRUE(entry.path().string().find(".partial") == std::string::npos) << entry.path();
  }
}

// T3 (acceptance row 6): a pure-lazy push bypasses the tap, so the capture has
// a hole — publication must refuse even on a clean COMPLETED terminal.
TEST_F(SourceCaptureServiceTest, PureLazySkipRefusesPublication) {
  auto capture = armAndAttach();
  const auto imu = bindTopic("/imu");
  push(imu, 100, {1});
  host->policyResolver().setDefault(sdk::ObjectIngestPolicy::kPureLazy);
  push(imu, 200, {2});  // fetch deferred: never reaches the tap
  ASSERT_EQ(host->recordTapSkippedLazy(), 1u);
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
  auto result = finalize(std::move(capture));
  EXPECT_FALSE(result.published);
  EXPECT_NE(result.reason.find("pure-lazy"), std::string::npos) << result.reason;
  std::string miss;
  EXPECT_FALSE(service_->resolve(kProvider, kDescriptor, &miss).has_value());
}

// T4 (acceptance row 6): a sink whose close() fails leaves the file
// unfinalized — publication must refuse and no artifact may exist.
TEST_F(SourceCaptureServiceTest, SinkCloseFailureRefusesPublication) {
  auto sink = std::make_shared<FakeCaptureSink>();
  sink->fail_close_with = "disk full at close";
  remakeService(shimFactory(sink));

  auto capture = armAndAttach();
  push(bindTopic("/imu"), 100, {1});
  ASSERT_TRUE(complete(sdk::IngestOutcome::kCompleted, {"/imu"}));
  auto result = finalize(std::move(capture));
  EXPECT_FALSE(result.published);
  // A close failure surfaces as a truncation carrying the sink's own message.
  EXPECT_NE(result.reason.find("disk full at close"), std::string::npos) << result.reason;
  EXPECT_TRUE(sink->close_called);
  std::string miss;
  EXPECT_FALSE(service_->resolve(kProvider, kDescriptor, &miss).has_value());
}

// T5 (acceptance row 6): first topic delivered, second failed mid-download —
// the FAILED terminal refuses publication, and the reason is the terminal
// verdict, distinct from the empty-topic attestation case.
TEST_F(SourceCaptureServiceTest, SuccessThenFetchFailureRefusesOnTheFailedTerminal) {
  auto capture = armAndAttach();
  push(bindTopic("/imu"), 100, {1, 2});
  // "/camera" never delivered; the source reports its own fetch failure.
  ASSERT_TRUE(complete(sdk::IngestOutcome::kFailed, {"/imu", "/camera"}));
  auto result = finalize(std::move(capture));
  EXPECT_FALSE(result.published);
  EXPECT_NE(result.reason.find("not COMPLETED"), std::string::npos) << result.reason;
  EXPECT_EQ(result.reason.find("attested"), std::string::npos)
      << "a failed terminal must not be reported as the empty-attestation case";
  std::string miss;
  EXPECT_FALSE(service_->resolve(kProvider, kDescriptor, &miss).has_value());
}

// T6 (acceptance row 7): an artifact whose per-topic counts disagree with its
// embedded pj.capture manifest — MCAP magic and footer fully intact — must be
// quarantined by resolve()'s strict-completeness check, never served as a hit.
TEST_F(SourceCaptureServiceTest, BodyManifestCountMismatchIsQuarantinedOnResolve) {
  const std::string identity = sourceCacheIdentity(kProvider, kDescriptor);
  auto txn = store_->beginPublish(identity);
  ASSERT_TRUE(txn.has_value());
  {
    // ONE recorded message, but a manifest claiming TWO: a structurally valid
    // MCAP whose body disagrees with its own completion manifest.
    McapRecordingWriter writer;
    ASSERT_TRUE(writer.open(txn->partialPath(), RecordingInfo{}).has_value());
    auto channel =
        writer.addChannel(RecordedBinding{.topic = "/imu", .encoding = "raw", .type_name = "t", .schema_bytes = {}});
    ASSERT_TRUE(channel.has_value());
    const uint8_t payload[] = {1, 2, 3};
    ASSERT_TRUE(writer.write(*channel, 100, {payload, 3}).has_value());
    const nlohmann::json manifest{
        {"version", kCaptureManifestVersion},
        {"provider_id", kProvider},
        {"identity", identity},  // JSON-escaped: the identity embeds the descriptor's quotes
        {"attests_empty_topics", false},
        {"requested_topic_messages", {{"/imu", 2}}},
        {"total_messages", 2},
    };
    RecordingSummary summary;
    summary.extra_metadata.emplace_back(std::string(kCaptureMetadataName), manifest.dump());
    ASSERT_TRUE(writer.close(summary).has_value());
  }
  auto published = store_->publish(identity, std::move(*txn));
  ASSERT_TRUE(published.has_value());
  const fs::path artifact_path = published->path;
  published->pin.release();

  std::string miss;
  EXPECT_FALSE(service_->resolve(kProvider, kDescriptor, &miss).has_value());
  EXPECT_NE(miss.find("disagrees with the manifest"), std::string::npos) << miss;
  EXPECT_NE(miss.find("quarantined"), std::string::npos) << miss;
  EXPECT_FALSE(fs::exists(artifact_path));
}

}  // namespace
}  // namespace PJ::test
