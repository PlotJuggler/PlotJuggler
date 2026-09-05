// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/h264_utils.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <string>

#include "test_mp4_demux.h"

namespace PJ {
namespace {

const std::string kTestVideo = "pj_scene2D/testdata/test_480p.mp4";

class H264UtilsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!std::filesystem::exists(kTestVideo)) {
      GTEST_SKIP() << "UNTRACKED FIXTURE test_480p.mp4 — not in the repository, so this suite does NOT run on a clean "
                      "clone. Generate it per pj_scene2D/testdata/README.md.";
    }
    packets_ = test::extractAnnexBPackets(kTestVideo);
    ASSERT_GT(packets_.size(), 10u) << "need at least 10 packets for meaningful tests";
  }

  std::vector<test::AnnexBPacket> packets_;
};

TEST_F(H264UtilsTest, KeyframeDetectionAgreesWithDemuxer) {
  int keyframes_found = 0;
  int total_checked = 0;

  for (const auto& pkt : packets_) {
    bool detected = isH264Keyframe(pkt.data.data(), pkt.data.size());
    EXPECT_EQ(detected, pkt.keyframe) << "mismatch at packet ts=" << pkt.timestamp;
    if (detected) {
      ++keyframes_found;
    }
    ++total_checked;
  }

  EXPECT_GT(keyframes_found, 0) << "should find at least one keyframe";
  EXPECT_GT(total_checked, keyframes_found) << "should have some non-keyframes too";
}

TEST_F(H264UtilsTest, PFrameReturnsFalse) {
  for (const auto& pkt : packets_) {
    if (!pkt.keyframe) {
      EXPECT_FALSE(isH264Keyframe(pkt.data.data(), pkt.data.size()));
      return;
    }
  }
  FAIL() << "no P-frame found in test video";
}

TEST_F(H264UtilsTest, EmptyDataReturnsFalse) {
  EXPECT_FALSE(isH264Keyframe(nullptr, 0));
  EXPECT_FALSE(isH264Keyframe(nullptr, 100));

  std::array<uint8_t, 4> empty = {0, 0, 0, 0};
  EXPECT_FALSE(isH264Keyframe(empty.data(), 0));
  EXPECT_FALSE(isH264Keyframe(empty.data(), 3));
}

TEST_F(H264UtilsTest, AnnexBStartCodePresent) {
  for (const auto& pkt : packets_) {
    if (pkt.keyframe) {
      ASSERT_GE(pkt.data.size(), 5u);
      bool has_start_code =
          (pkt.data[0] == 0x00 && pkt.data[1] == 0x00 &&
           ((pkt.data[2] == 0x01) || (pkt.data[2] == 0x00 && pkt.data[3] == 0x01)));
      EXPECT_TRUE(has_start_code) << "keyframe should have annex-B start code";
      return;
    }
  }
}

}  // namespace
}  // namespace PJ
