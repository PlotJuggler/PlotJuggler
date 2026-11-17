// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Round-trip, dispatch and adversarial tests for the CompressedPointCloud decoders: each test encodes a known cloud
// with the REAL Cloudini / Draco encoder, wraps it in a canonical CompressedPointCloud and checks the decoder
// reconstructs it.

#include "pj_scene3d_core/pointcloud_codecs.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "cloudini_lib/cloudini.hpp"
#include "draco/compression/encode.h"
#include "draco/core/draco_types.h"
#include "draco/core/encoder_buffer.h"
#include "draco/metadata/geometry_metadata.h"
#include "draco/point_cloud/point_cloud_builder.h"

namespace {

using PJ::Span;
using PJ::sdk::CompressedPointCloud;
using PJ::sdk::PointField;

struct Pt {
  float x, y, z, intensity;
};

std::vector<Pt> makePoints(int n) {
  std::vector<Pt> pts;
  pts.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    pts.push_back(
        {static_cast<float>(i) * 0.1f, static_cast<float>(i) * 0.2f, static_cast<float>(i) * 0.3f,
         static_cast<float>(i)});
  }
  return pts;
}

// Cloudini-encode with the lossless (NONE first-stage + ZSTD) path so the round-trip is exact: this validates our
// reconstruction, not Cloudini's compression quality. width == 0 means "unorganized": pts.size() x 1.
std::vector<uint8_t> encodeCloudini(const std::vector<Pt>& pts, uint32_t width = 0, uint32_t height = 1) {
  Cloudini::EncodingInfo info;
  info.width = width != 0 ? width : static_cast<uint32_t>(pts.size());
  info.height = height;
  info.point_step = sizeof(Pt);
  info.encoding_opt = Cloudini::EncodingOptions::NONE;
  info.compression_opt = Cloudini::CompressionOption::ZSTD;
  info.fields = {
      {"x", 0, Cloudini::FieldType::FLOAT32, std::nullopt},
      {"y", 4, Cloudini::FieldType::FLOAT32, std::nullopt},
      {"z", 8, Cloudini::FieldType::FLOAT32, std::nullopt},
      {"intensity", 12, Cloudini::FieldType::FLOAT32, std::nullopt},
  };
  Cloudini::PointcloudEncoder encoder(info);
  Cloudini::ConstBufferView input(reinterpret_cast<const uint8_t*>(pts.data()), pts.size() * sizeof(Pt));
  std::vector<uint8_t> out;
  encoder.encode(input, out);
  return out;
}

// Draco-encode POSITION (x,y,z) + one GENERIC (intensity) attribute, sequential
// (lossless float) so positions round-trip closely. When generic_meta_name is given,
// the GENERIC attribute is tagged with that name in Draco attribute metadata (the
// Foxglove / draco_point_cloud_transport convention).
std::vector<uint8_t> encodeDraco(const std::vector<Pt>& pts, const char* generic_meta_name = nullptr) {
  draco::PointCloudBuilder builder;
  builder.Start(static_cast<draco::PointIndex::ValueType>(pts.size()));
  const int pos_att = builder.AddAttribute(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
  const int gen_att = builder.AddAttribute(draco::GeometryAttribute::GENERIC, 1, draco::DT_FLOAT32);
  for (size_t i = 0; i < pts.size(); ++i) {
    const float xyz[3] = {pts[i].x, pts[i].y, pts[i].z};
    builder.SetAttributeValueForPoint(pos_att, draco::PointIndex(static_cast<uint32_t>(i)), xyz);
    builder.SetAttributeValueForPoint(gen_att, draco::PointIndex(static_cast<uint32_t>(i)), &pts[i].intensity);
  }
  if (generic_meta_name != nullptr) {
    auto meta = std::make_unique<draco::AttributeMetadata>();
    meta->AddEntryString("name", generic_meta_name);
    builder.AddAttributeMetadata(gen_att, std::move(meta));
  }
  std::unique_ptr<draco::PointCloud> pc = builder.Finalize(/*deduplicate_points=*/false);
  draco::Encoder encoder;
  encoder.SetEncodingMethod(draco::POINT_CLOUD_SEQUENTIAL_ENCODING);
  draco::EncoderBuffer buffer;
  const draco::Status status = encoder.EncodePointCloudToBuffer(*pc, &buffer);
  EXPECT_TRUE(status.ok()) << status.error_msg();
  return std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size());
}

CompressedPointCloud wrap(std::vector<uint8_t> blob, std::string format, std::string frame_id, int64_t ts_ns) {
  auto owned = std::make_shared<std::vector<uint8_t>>(std::move(blob));
  CompressedPointCloud c;
  c.timestamp_ns = ts_ns;
  c.frame_id = std::move(frame_id);
  c.format = std::move(format);
  c.data = Span<const uint8_t>(owned->data(), owned->size());
  c.anchor = owned;
  return c;
}

const PointField* findField(const PJ::sdk::PointCloud& pc, std::string_view name) {
  for (const auto& f : pc.fields) {
    if (f.name == name) {
      return &f;
    }
  }
  return nullptr;
}

float readFloat(const PJ::sdk::PointCloud& pc, int point, const PointField& field) {
  float v = 0.0f;
  std::memcpy(&v, pc.data.data() + static_cast<size_t>(point) * pc.point_step + field.offset, sizeof(float));
  return v;
}

}  // namespace

TEST(PointcloudCodecs, DecodeCloudiniRoundTrip) {
  const auto pts = makePoints(16);
  const auto cloud = wrap(encodeCloudini(pts), "cloudini", "lidar_link", 123456789);

  const auto result = pj::scene3d::decodeCloudini(cloud);
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto& pc = result.value();

  EXPECT_EQ(pc.width, 16u);
  EXPECT_EQ(pc.height, 1u);
  EXPECT_EQ(pc.frame_id, "lidar_link");
  EXPECT_EQ(pc.timestamp_ns, 123456789);
  ASSERT_EQ(pc.fields.size(), 4u);
  ASSERT_GE(pc.data.size(), static_cast<size_t>(pc.width) * pc.point_step);

  const PointField* fx = findField(pc, "x");
  const PointField* fz = findField(pc, "z");
  const PointField* fi = findField(pc, "intensity");
  ASSERT_NE(fx, nullptr);
  ASSERT_NE(fz, nullptr);
  ASSERT_NE(fi, nullptr);
  for (int i = 0; i < 16; ++i) {
    EXPECT_FLOAT_EQ(readFloat(pc, i, *fx), static_cast<float>(i) * 0.1f);
    EXPECT_FLOAT_EQ(readFloat(pc, i, *fz), static_cast<float>(i) * 0.3f);
    EXPECT_FLOAT_EQ(readFloat(pc, i, *fi), static_cast<float>(i));
  }
}

TEST(PointcloudCodecs, DecodeDracoRoundTrip) {
  const auto pts = makePoints(16);
  const auto cloud = wrap(encodeDraco(pts), "draco", "camera_depth", 42);

  const auto result = pj::scene3d::decodeDraco(cloud);
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto& pc = result.value();

  EXPECT_EQ(pc.width, 16u);
  EXPECT_EQ(pc.frame_id, "camera_depth");
  EXPECT_EQ(pc.timestamp_ns, 42);
  EXPECT_GE(pc.fields.size(), 3u);

  const PointField* fx = findField(pc, "x");
  const PointField* fy = findField(pc, "y");
  const PointField* fz = findField(pc, "z");
  ASSERT_NE(fx, nullptr);
  ASSERT_NE(fy, nullptr);
  ASSERT_NE(fz, nullptr);
  for (int i = 0; i < 16; ++i) {
    EXPECT_NEAR(readFloat(pc, i, *fx), static_cast<float>(i) * 0.1f, 1e-3);
    EXPECT_NEAR(readFloat(pc, i, *fy), static_cast<float>(i) * 0.2f, 1e-3);
    EXPECT_NEAR(readFloat(pc, i, *fz), static_cast<float>(i) * 0.3f, 1e-3);
  }
}

TEST(PointcloudCodecs, DispatchRoutesByFormat) {
  const auto pts = makePoints(8);
  const auto cloudini = wrap(encodeCloudini(pts), "cloudini", "f", 1);
  const auto draco = wrap(encodeDraco(pts), "draco", "f", 1);

  EXPECT_TRUE(pj::scene3d::decodeCompressedPointCloud(cloudini).has_value());
  EXPECT_TRUE(pj::scene3d::decodeCompressedPointCloud(draco).has_value());
}

TEST(PointcloudCodecs, BoundedDispatchRejectsPointCountBeforePublishingOutput) {
  const auto pts = makePoints(16);
  const auto cloudini = wrap(encodeCloudini(pts), "cloudini", "f", 1);
  const auto draco = wrap(encodeDraco(pts, "intensity"), "draco", "f", 1);
  const pj::scene3d::PointCloudDecodeLimits limits{.max_points = 15, .max_decoded_bytes = 1024};

  const auto cloudini_result = pj::scene3d::decodeCompressedPointCloud(cloudini, limits);
  const auto draco_result = pj::scene3d::decodeCompressedPointCloud(draco, limits);
  ASSERT_FALSE(cloudini_result.has_value());
  ASSERT_FALSE(draco_result.has_value());
  EXPECT_NE(cloudini_result.error().find("decoded point count 16 exceeds limit 15"), std::string::npos);
  EXPECT_NE(draco_result.error().find("decoded point count 16 exceeds limit 15"), std::string::npos);
}

TEST(PointcloudCodecs, BoundedDispatchRejectsPackedOutputBytes) {
  const auto pts = makePoints(16);
  const auto cloudini = wrap(encodeCloudini(pts), "cloudini", "f", 1);
  const auto draco = wrap(encodeDraco(pts, "intensity"), "draco", "f", 1);
  const pj::scene3d::PointCloudDecodeLimits limits{.max_points = 16, .max_decoded_bytes = 255};

  const auto cloudini_result = pj::scene3d::decodeCompressedPointCloud(cloudini, limits);
  const auto draco_result = pj::scene3d::decodeCompressedPointCloud(draco, limits);
  ASSERT_FALSE(cloudini_result.has_value());
  ASSERT_FALSE(draco_result.has_value());
  EXPECT_NE(cloudini_result.error().find("decoded payload 256 bytes exceeds limit 255"), std::string::npos);
  EXPECT_NE(draco_result.error().find("decoded payload 256 bytes exceeds limit 255"), std::string::npos);
}

TEST(PointcloudCodecs, DispatchIsCaseInsensitive) {
  const auto pts = makePoints(8);
  const auto cloud = wrap(encodeCloudini(pts), "Cloudini", "f", 1);
  EXPECT_TRUE(pj::scene3d::decodeCompressedPointCloud(cloud).has_value());
}

TEST(PointcloudCodecs, UnknownFormatReturnsError) {
  const auto pts = makePoints(8);
  const auto cloud = wrap(encodeCloudini(pts), "zlib", "f", 1);
  const auto result = pj::scene3d::decodeCompressedPointCloud(cloud);
  EXPECT_FALSE(result.has_value());
}

TEST(PointcloudCodecs, EmptyDataReturnsError) {
  CompressedPointCloud cloud;
  cloud.format = "draco";
  EXPECT_FALSE(pj::scene3d::decodeDraco(cloud).has_value());
  cloud.format = "cloudini";
  EXPECT_FALSE(pj::scene3d::decodeCloudini(cloud).has_value());
}

TEST(PointcloudCodecs, CloudiniDropsInt64FieldKeepingOffsets) {
  // Layout per point (point_step 16): x@0 (f32), big@4 (int64), y@12 (f32). INT64 has no
  // PJ datatype, so the decoder must DROP "big" while leaving x@0 and y@12 readable at
  // their original offsets — the byte still occupies point_step.
  constexpr int kN = 10;
  constexpr uint32_t kStep = 16;
  std::vector<uint8_t> raw(static_cast<size_t>(kN) * kStep);
  for (int i = 0; i < kN; ++i) {
    const float x = static_cast<float>(i) * 0.5f;
    const float y = static_cast<float>(i) * 2.0f;
    const int64_t big = 1000 + i;
    std::memcpy(&raw[static_cast<size_t>(i) * kStep + 0], &x, 4);
    std::memcpy(&raw[static_cast<size_t>(i) * kStep + 4], &big, 8);
    std::memcpy(&raw[static_cast<size_t>(i) * kStep + 12], &y, 4);
  }
  Cloudini::EncodingInfo info;
  info.width = kN;
  info.height = 1;
  info.point_step = kStep;
  info.encoding_opt = Cloudini::EncodingOptions::NONE;
  info.compression_opt = Cloudini::CompressionOption::ZSTD;
  info.fields = {
      {"x", 0, Cloudini::FieldType::FLOAT32, std::nullopt},
      {"big", 4, Cloudini::FieldType::INT64, std::nullopt},
      {"y", 12, Cloudini::FieldType::FLOAT32, std::nullopt},
  };
  Cloudini::PointcloudEncoder encoder(info);
  std::vector<uint8_t> blob;
  encoder.encode(Cloudini::ConstBufferView(raw.data(), raw.size()), blob);

  const auto result = pj::scene3d::decodeCloudini(wrap(blob, "cloudini", "f", 1));
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto& pc = result.value();
  EXPECT_EQ(pc.point_step, 16u);
  EXPECT_EQ(findField(pc, "big"), nullptr) << "INT64 field must be dropped";
  const PointField* fx = findField(pc, "x");
  const PointField* fy = findField(pc, "y");
  ASSERT_NE(fx, nullptr);
  ASSERT_NE(fy, nullptr);
  EXPECT_EQ(fx->offset, 0u);
  EXPECT_EQ(fy->offset, 12u) << "surviving field offset must be preserved";
  for (int i = 0; i < kN; ++i) {
    EXPECT_FLOAT_EQ(readFloat(pc, i, *fx), static_cast<float>(i) * 0.5f);
    EXPECT_FLOAT_EQ(readFloat(pc, i, *fy), static_cast<float>(i) * 2.0f);
  }
}

TEST(PointcloudCodecs, CloudiniOrganizedCloudHeightGreaterThanOne) {
  constexpr uint32_t kW = 4;
  constexpr uint32_t kH = 3;  // organized cloud: 12 points in a 4x3 grid
  const auto pts = makePoints(static_cast<int>(kW * kH));
  const auto blob = encodeCloudini(pts, kW, kH);

  const auto result = pj::scene3d::decodeCloudini(wrap(blob, "cloudini", "f", 1));
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto& pc = result.value();
  EXPECT_EQ(pc.width, kW);
  EXPECT_EQ(pc.height, kH);
  ASSERT_GE(pc.data.size(), static_cast<size_t>(kW) * kH * pc.point_step) << "full organized buffer";
  const PointField* fx = findField(pc, "x");
  ASSERT_NE(fx, nullptr);
  EXPECT_FLOAT_EQ(readFloat(pc, static_cast<int>(kW), *fx), pts[kW].x);
}

TEST(PointcloudCodecs, DracoRecoversAttributeNameFromMetadata) {
  // The GENERIC attribute is tagged with metadata name "intensity" (the convention
  // Foxglove / draco_point_cloud_transport use). Decoder must recover "intensity",
  // not "generic_N".
  const auto cloud = wrap(encodeDraco(makePoints(12), "intensity"), "draco", "f", 1);
  const auto result = pj::scene3d::decodeDraco(cloud);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_NE(findField(result.value(), "intensity"), nullptr) << "metadata attribute name not recovered";
}

TEST(PointcloudCodecs, GarbageBufferReturnsErrorNotCrash) {
  std::vector<uint8_t> garbage(64, 0xAB);
  const auto draco = wrap(garbage, "draco", "f", 1);
  const auto cloudini = wrap(garbage, "cloudini", "f", 1);
  EXPECT_FALSE(pj::scene3d::decodeDraco(draco).has_value());
  EXPECT_FALSE(pj::scene3d::decodeCloudini(cloudini).has_value());
}

TEST(PointcloudCodecs, CloudiniRejectsOutOfRangeFieldOffset) {
  // The header is untrusted: cloudini's own field decoders memcpy to dest+offset with
  // no destination bounds check, so a hostile offset must be rejected BEFORE decode
  // (heap OOB write otherwise). Craft a header-only blob with a field far outside
  // point_step using the library's own header encoder.
  Cloudini::EncodingInfo info;
  info.width = 1;
  info.height = 1;
  info.point_step = 16;
  info.encoding_opt = Cloudini::EncodingOptions::NONE;
  info.compression_opt = Cloudini::CompressionOption::ZSTD;
  info.fields = {
      {"x", 0, Cloudini::FieldType::FLOAT32, std::nullopt},
      {"evil", 1000000, Cloudini::FieldType::FLOAT64, std::nullopt},
  };
  std::vector<uint8_t> blob;
  Cloudini::EncodeHeader(info, blob);

  const auto result = pj::scene3d::decodeCloudini(wrap(blob, "cloudini", "f", 1));
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("offset"), std::string::npos) << result.error();

  // Boundary: a field that overlaps point_step's end by 2 bytes is just as invalid.
  info.fields = {{"x", 14, Cloudini::FieldType::FLOAT32, std::nullopt}};
  blob.clear();
  Cloudini::EncodeHeader(info, blob);
  EXPECT_FALSE(pj::scene3d::decodeCloudini(wrap(blob, "cloudini", "f", 1)).has_value());
}

TEST(PointcloudCodecs, DracoConvertsNonFloatColorAttribute) {
  // COLOR is uint8 in the bitstream; the all-float32 repack must go through the
  // ConvertValue fallback (the float32 memcpy fast-path doesn't apply) and yield
  // the raw 0..255 component values as floats.
  constexpr int kN = 8;
  draco::PointCloudBuilder builder;
  builder.Start(kN);
  const int pos_att = builder.AddAttribute(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
  const int col_att = builder.AddAttribute(draco::GeometryAttribute::COLOR, 4, draco::DT_UINT8);
  for (int i = 0; i < kN; ++i) {
    const float xyz[3] = {static_cast<float>(i), 0.0f, 0.0f};
    const uint8_t rgba[4] = {
        static_cast<uint8_t>(10 * i), static_cast<uint8_t>(5 * i), static_cast<uint8_t>(2 * i), 255};
    builder.SetAttributeValueForPoint(pos_att, draco::PointIndex(static_cast<uint32_t>(i)), xyz);
    builder.SetAttributeValueForPoint(col_att, draco::PointIndex(static_cast<uint32_t>(i)), rgba);
  }
  std::unique_ptr<draco::PointCloud> pc = builder.Finalize(/*deduplicate_points=*/false);
  draco::Encoder encoder;
  encoder.SetEncodingMethod(draco::POINT_CLOUD_SEQUENTIAL_ENCODING);
  draco::EncoderBuffer buffer;
  ASSERT_TRUE(encoder.EncodePointCloudToBuffer(*pc, &buffer).ok());

  const auto cloud = wrap(std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size()), "draco", "f", 1);
  const auto result = pj::scene3d::decodeDraco(cloud);
  ASSERT_TRUE(result.has_value()) << result.error();
  const auto& out = result.value();

  const PointField* fx = findField(out, "x");
  const PointField* fr = findField(out, "red");
  const PointField* fa = findField(out, "alpha");
  ASSERT_NE(fx, nullptr);
  ASSERT_NE(fr, nullptr);
  ASSERT_NE(fa, nullptr);
  EXPECT_EQ(fr->datatype, PointField::Datatype::kFloat32);
  for (int i = 0; i < kN; ++i) {
    EXPECT_NEAR(readFloat(out, i, *fx), static_cast<float>(i), 1e-3);
    EXPECT_FLOAT_EQ(readFloat(out, i, *fr), static_cast<float>(10 * i));
    EXPECT_FLOAT_EQ(readFloat(out, i, *fa), 255.0f);
  }
}
