// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/ffmpeg_decoder.h"

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace PJ {
namespace {

const std::string kTestVideo = "pj_scene2D/testdata/test_480p.mp4";

class FfmpegDecoderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!std::filesystem::exists(kTestVideo)) {
      GTEST_SKIP() << "UNTRACKED FIXTURE test_480p.mp4 — not in the repository, so this suite does NOT run on a clean "
                      "clone. Generate it per pj_scene2D/testdata/README.md.";
    }

    ASSERT_GE(avformat_open_input(&fmt_ctx_, kTestVideo.c_str(), nullptr, nullptr), 0);
    ASSERT_GE(avformat_find_stream_info(fmt_ctx_, nullptr), 0);

    for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
      if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        video_idx_ = static_cast<int>(i);
        break;
      }
    }
    ASSERT_GE(video_idx_, 0);
    ASSERT_TRUE(decoder_.open(fmt_ctx_->streams[video_idx_]->codecpar));
  }

  void TearDown() override {
    if (fmt_ctx_ != nullptr) {
      avformat_close_input(&fmt_ctx_);
    }
  }

  AVFormatContext* fmt_ctx_ = nullptr;
  int video_idx_ = -1;
  FfmpegDecoder decoder_;
};

TEST_F(FfmpegDecoderTest, DecodeFirstFrame) {
  // Read and decode until we get a frame
  AVPacket* pkt = av_packet_alloc();
  DecodedFrame frame;
  int packets_sent = 0;

  while (av_read_frame(fmt_ctx_, pkt) >= 0) {
    if (pkt->stream_index != video_idx_) {
      av_packet_unref(pkt);
      continue;
    }

    auto result = decoder_.decode(pkt->data, static_cast<size_t>(pkt->size), pkt->pts, pkt->dts);
    av_packet_unref(pkt);
    ++packets_sent;

    if (result.has_value() && !result->isNull()) {
      frame = std::move(*result);
      break;
    }
  }
  av_packet_free(&pkt);

  ASSERT_FALSE(frame.isNull()) << "no frame decoded after " << packets_sent << " packets";
  EXPECT_EQ(frame.width, 640);
  EXPECT_EQ(frame.height, 480);
  // Software decode yields planar YUV420P; hardware decode (VAAPI) yields native
  // NV12. Both are valid — accept either and size-check against the actual format.
  EXPECT_TRUE(frame.format == PixelFormat::kYUV420P || frame.format == PixelFormat::kNV12)
      << "format=" << static_cast<int>(frame.format);
  EXPECT_EQ(frame.pixels->size(), expectedBufferSize(640, 480, frame.format));
  EXPECT_TRUE(frame.isValid());
}

// C2 contract: decode() must never return has_value() with isNull() frame.
// Every successful decode must produce a valid frame.
TEST_F(FfmpegDecoderTest, SuccessfulDecodeNeverReturnsNullFrame) {
  AVPacket* pkt = av_packet_alloc();
  int total_results = 0;

  while (av_read_frame(fmt_ctx_, pkt) >= 0 && total_results < 30) {
    if (pkt->stream_index != video_idx_) {
      av_packet_unref(pkt);
      continue;
    }

    auto result = decoder_.decode(pkt->data, static_cast<size_t>(pkt->size), pkt->pts, pkt->dts);
    av_packet_unref(pkt);

    if (result.has_value()) {
      // Contract: if decode returns success, the frame must not be null
      EXPECT_FALSE(result->isNull()) << "decode() returned success but null frame at result #" << total_results;
      ++total_results;
    }
  }
  av_packet_free(&pkt);

  EXPECT_GT(total_results, 0) << "should have decoded at least one frame";
}

TEST_F(FfmpegDecoderTest, DecodeMultipleFrames) {
  AVPacket* pkt = av_packet_alloc();
  int decoded_count = 0;

  while (av_read_frame(fmt_ctx_, pkt) >= 0 && decoded_count < 10) {
    if (pkt->stream_index != video_idx_) {
      av_packet_unref(pkt);
      continue;
    }

    auto result = decoder_.decode(pkt->data, static_cast<size_t>(pkt->size), pkt->pts, pkt->dts);
    av_packet_unref(pkt);

    if (result.has_value() && !result->isNull()) {
      ++decoded_count;
    }
  }
  av_packet_free(&pkt);

  EXPECT_GE(decoded_count, 5) << "should decode at least 5 frames from the first packets";
}

TEST_F(FfmpegDecoderTest, FlushAndResume) {
  // Decode a few frames, then flush (simulating a seek)
  AVPacket* pkt = av_packet_alloc();
  int count = 0;
  while (av_read_frame(fmt_ctx_, pkt) >= 0 && count < 5) {
    if (pkt->stream_index == video_idx_) {
      decoder_.decode(pkt->data, static_cast<size_t>(pkt->size), pkt->pts, pkt->dts);
      ++count;
    }
    av_packet_unref(pkt);
  }

  decoder_.flush();

  // Continue decoding after flush — should not crash
  int post_flush = 0;
  while (av_read_frame(fmt_ctx_, pkt) >= 0 && post_flush < 5) {
    if (pkt->stream_index == video_idx_) {
      auto result = decoder_.decode(pkt->data, static_cast<size_t>(pkt->size), pkt->pts, pkt->dts);
      if (result.has_value() && !result->isNull()) {
        ++post_flush;
      }
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);

  EXPECT_GT(post_flush, 0) << "should decode frames after flush";
}

TEST_F(FfmpegDecoderTest, CancelStopsEarly) {
  auto token = makeCancelToken();
  token->cancel();

  AVPacket* pkt = av_packet_alloc();
  while (av_read_frame(fmt_ctx_, pkt) >= 0) {
    if (pkt->stream_index == video_idx_) {
      auto result = decoder_.decode(pkt->data, static_cast<size_t>(pkt->size), pkt->pts, pkt->dts, token);
      av_packet_unref(pkt);
      EXPECT_FALSE(result.has_value());
      break;
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
}

}  // namespace
}  // namespace PJ
