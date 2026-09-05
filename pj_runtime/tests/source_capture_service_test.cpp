// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// End-to-end tests of the M3 SourceCaptureService over a REAL
// DataSourceRuntimeHost and a real cache directory: capture raw pushed bytes
// through the completion contract's gate, publish into SourceCacheStore, and
// resolve the same request back — plus every gate condition that must refuse.

#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "pj_runtime/McapRecordingWriter.h"
#include "pj_runtime/RecordingFormat.h"
#include "pj_runtime/SourceCaptureService.h"
#include "runtime_host_test_fixture.h"

namespace fs = std::filesystem;

namespace PJ::test {
namespace {

constexpr const char* kProvider = "test.provider";
constexpr const char* kDescriptor = R"({"kind":"capture-test-request","request":{},"v":1})";

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

TEST_F(SourceCaptureServiceTest, EnvelopeRefusesUnknownAndCredentialFields) {
  auto capture = service_->arm(*host, kProvider);
  // Unknown top-level field: refused (allowlist), NOT latched, NOT vetoed.
  EXPECT_FALSE(attach(R"({"kind":"k","request":{},"v":1,"extra":1})").has_value());
  // Credential-shaped key at depth: refused.
  EXPECT_FALSE(attach(R"({"kind":"k","request":{"api_key":"x"},"v":1})").has_value());
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

}  // namespace
}  // namespace PJ::test
