#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>

#include "pj_base/builtin/compressed_point_cloud.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/expected.hpp"

// Decode adapters: turn a canonical CompressedPointCloud (Draco / Cloudini) into a
// canonical PointCloud the existing render path understands. SELF-DESCRIBING formats
// only — the point layout is recovered from the blob. CPU-heavy; widgets run it off
// the UI thread. See pj_scene3D/CLAUDE.md "Invariants" (the codec carve-out).
namespace pj::scene3d {

struct PointCloudDecodeLimits {
  std::uint64_t max_points = 0;
  std::uint64_t max_decoded_bytes = 0;
};

// Dispatch on `cloud.format` ("draco" | "cloudini", case-insensitive). Returns an
// error for an unsupported format, empty data, or a malformed/corrupt blob. Never
// throws. `frame_id` and `timestamp_ns` are copied through from the input.
[[nodiscard]] PJ::Expected<PJ::sdk::PointCloud> decodeCompressedPointCloud(const PJ::sdk::CompressedPointCloud& cloud);

// Bounded variant for constrained products. A zero limit disables that one
// check. Cloudini dimensions are rejected before its output allocation; Draco's
// point count is read from the bitstream before the full decoder, and its packed
// canonical output size is rejected before the second/output allocation. The
// unlimited overload above preserves the desktop contract verbatim.
[[nodiscard]] PJ::Expected<PJ::sdk::PointCloud> decodeCompressedPointCloud(
    const PJ::sdk::CompressedPointCloud& cloud, PointCloudDecodeLimits limits);

// Format-specific decoders. Exposed for focused testing; production code goes
// through decodeCompressedPointCloud().
[[nodiscard]] PJ::Expected<PJ::sdk::PointCloud> decodeCloudini(const PJ::sdk::CompressedPointCloud& cloud);
[[nodiscard]] PJ::Expected<PJ::sdk::PointCloud> decodeDraco(const PJ::sdk::CompressedPointCloud& cloud);

}  // namespace pj::scene3d
