// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "pj_base/builtin/point_cloud.hpp"
#include "pj_scene3d_core/camera/camera.h"  // AABB
#include "pj_scene3d_core/pointcloud.h"

namespace pj::scene3d {

// Result of convertCanonical(): the decoded render geometry plus the source-frame
// AABB of its finite points, accumulated in the same pass that fills `cloud`. The
// box is `valid == false` when no finite point was read (empty/all-NaN cloud), so a
// caller can use it directly as the layer's world bounds.
struct ConvertedPointCloud {
  DecodedPointCloud cloud;
  AABB bounds;
};

// How to read a packed per-point RGBA color from a cloud's point buffer, when it
// carries color. `valid == false` means no recognizable color field is present.
// Offsets are byte offsets within one point; convertCanonical validates them
// against point_step before any read.
struct ColorLayout {
  bool valid = false;
  uint32_t r_offset = 0;
  uint32_t g_offset = 0;
  uint32_t b_offset = 0;
  bool has_alpha = false;  // false -> alpha is forced to 255 (opaque)
  uint32_t a_offset = 0;   // meaningful only when has_alpha
};

// Recognize a cloud's per-point color by the canonical convention, in priority order:
//   1. a single packed field named "rgba" or "rgb" of datatype kUint32 — the 4 bytes at
//      its offset are R,G,B,A in increasing address (alpha present iff the name is "rgba");
//   2. separate uint8 channels "red"/"green"/"blue" (+ optional "alpha") — the raw,
//      un-normalized foxglove layout, recognized defensively so the host shows true color
//      even for sources the parser has not collapsed to a packed field yet.
// Returns {valid=false} when neither is present. Offsets are NOT range-checked here;
// convertCanonical validates them against point_step before reading.
[[nodiscard]] ColorLayout detectColorLayout(const PJ::sdk::PointCloud& src);

// Decode a canonical PointCloud into render-ready positions plus an optional
// colorize-by-field scalar, reading the raw little-endian point buffer per the
// cloud's own field layout. `scalar_field` selects the field to extract into
// `cloud.scalar` (empty -> no scalar). Positions/scalars are in the cloud's OWN
// source frame; the fixed-frame TF is applied later in the shader, never here.
//
// When `extract_rgba` is true, the cloud's color field (see detectColorLayout) is
// decoded into `cloud.rgba` (packed R,G,B,A; alpha 255 when the source has none) and
// the scalar is NOT extracted — RGB-direct mode bypasses the colormap. With no color
// field present, `cloud.rgba` stays empty.
//
// This is the renderer's last line of defense against a malformed/hostile cloud:
// the function validates the buffer size and every read field's offset against
// point_step BEFORE indexing, and returns an EMPTY result (empty positions, invalid
// bounds) on any rejection — big-endian, zero/oversized geometry, a too-small data
// buffer, a missing x/y/z field, or any field (including a color channel) whose
// `offset + bytesPerElement*count` exceeds `point_step`. The layer logs a generic
// warning when positions come back empty, so no logging happens here (Qt-free core).
ConvertedPointCloud convertCanonical(
    const PJ::sdk::PointCloud& src, std::string_view scalar_field, bool extract_rgba = false);

// What packPointVertices() reads from a cloud and writes, validated once by
// planPointVertices(). Its fields point into the cloud's `fields`: valid while that
// cloud is.
struct PointVertexPlan {
  // kXyzRgba when `extract_rgba` found a colour field, kXyzScalar when
  // `store_scalar` and `scalar_field` names a field of the cloud, else kXyz.
  PointVertexFormat format = PointVertexFormat::kXyz;
  std::size_t point_count = 0;  // width * height: the records `out` needs room for
  std::size_t step = 0;         // point_step
  const PJ::sdk::PointField* x = nullptr;
  const PJ::sdk::PointField* y = nullptr;
  const PJ::sdk::PointField* z = nullptr;
  const PJ::sdk::PointField* scalar = nullptr;  // null: no scalar read (RGB-direct, or no such field)
  ColorLayout color;                            // valid only when RGB-direct found a colour field
  // The bytes packPointVertices() may write: point_count records of `format`.
  [[nodiscard]] std::size_t maxBytes() const {
    return point_count * pointVertexStride(format);
  }
};

// Validates `src` by exactly convertCanonical()'s rules (nullopt where it would return
// no positions) and plans its packing for these arguments.
[[nodiscard]] std::optional<PointVertexPlan> planPointVertices(
    const PJ::sdk::PointCloud& src, std::string_view scalar_field, bool extract_rgba, bool store_scalar);

// Result of packPointVertices().
struct PackedPointVertices {
  PointVertexFormat format = PointVertexFormat::kXyz;  // how `out` was written
  std::size_t count = 0;                               // finite points written, in wire order
  AABB bounds;                                         // their extent
  // Finite min/max of the scalar field over EVERY point, including those dropped for
  // a non-finite position (convertCanonical's `cloud.scalar`); unclamped. nullopt
  // without a scalar field or a finite value (the scalars are then 0 or non-finite).
  std::optional<std::pair<float, float>> scalar_range;
};

// Runs body(0) .. body(count - 1), possibly concurrently, and returns when all have.
using ParallelFor = std::function<void(std::size_t count, const std::function<void(std::size_t)>& body)>;
// With a ParallelFor, packPointVertices() packs chunks of at least this many points,
// at most kMaxPackChunks of them, concurrently. Sized for total CPU, not only wall:
// every pool thread woken costs CPU, and smaller or more chunks bought little wall
// for much more process CPU (TECHNICAL_NOTES §9).
inline constexpr std::size_t kPackChunkPoints = 262144;
inline constexpr std::size_t kMaxPackChunks = 8;
// The cap for packing into a freshly allocated `out`: its first-touch page faults
// dominate and scale with the threads (10M points: 18.5 ms with 8 chunks, 10.4 ms
// with 16), where rewriting a reused array gains 5 ms of wall for 12 ms of CPU.
inline constexpr std::size_t kMaxFreshPackChunks = 16;

// convertCanonical() and the packing into retained points in one pass, with no
// intermediate: writes the finite points of `src` (the cloud `plan` was made for) to
// `out` in plan.format and returns their count and bounds; the same values as
// convertCanonical() followed by dropping every point with a non-finite coordinate.
// The scalar field is read for `scalar_range` even when it is not stored. `out` needs
// plan.maxBytes(); a smaller span writes nothing (count 0). With `parallel_for`, up to
// `max_chunks` chunks of the cloud pack concurrently; the result is the same.
[[nodiscard]] PackedPointVertices packPointVertices(
    const PJ::sdk::PointCloud& src, const PointVertexPlan& plan, std::span<std::byte> out,
    const ParallelFor& parallel_for = {}, std::size_t max_chunks = kMaxPackChunks);

// --- Raw byte readers (exposed for direct unit testing) -------------------------
// All assemble little-endian; the cloud's `is_bigendian` is rejected upstream in
// convertCanonical, so these never see big-endian input. `data` must point at a
// span with at least the datatype's width of readable bytes (callers in
// convertCanonical guarantee this via the offset validation).

// IEEE-754 float32 at `data` (4 bytes consumed).
[[nodiscard]] float readFloat32At(const uint8_t* data);

// IEEE-754 float64 at `data` (8 bytes consumed).
[[nodiscard]] double readFloat64At(const uint8_t* data);

// One field element at `data` widened to float, sign-extending the signed integer
// datatypes (int8/int16/int32 decode negative values correctly). kFloat64 is
// truncated to float; kUnknown yields 0.
[[nodiscard]] float readScalarAt(const uint8_t* data, PJ::sdk::PointField::Datatype datatype);

// First field named `name`, or nullptr if absent.
[[nodiscard]] const PJ::sdk::PointField* findField(
    const std::vector<PJ::sdk::PointField>& fields, std::string_view name);

// GL-agnostic description of how to bind the verbatim wire buffer as vertex attribs.
// scalar_gl_type carries the GL enum value as a plain uint32_t so core/ stays GL-free;
// widgets passes it straight to glVertexAttribPointer. xyz are float32 and contiguous
// (x_offset, +4, +8), guaranteed by checkFastPath(); position is one vec3.
struct AttribLayout {
  uint32_t stride{0};          // = point_step
  uint32_t xyz_offset{0};      // x field offset; y=+4, z=+8
  uint32_t scalar_offset{0};   // valid only when has_scalar
  uint32_t scalar_gl_type{0};  // GL_FLOAT/GL_UNSIGNED_INT/... as uint; 0 when no scalar
  bool has_scalar{false};      // false for solid colour, x/y/z axis colouring, and RGB-direct mode
  // RGB-direct: the canonical packed 'rgba'/'rgb' uint32 field is 4 contiguous bytes
  // (R,G,B,A in increasing address) — bind straight at color_offset as a normalized vec4
  // (the shader uses only .rgb), pixel-identical to the CPU rgba extraction. Set only when
  // checkFastPath() is called with want_rgba and finds a packed (contiguous) colour field.
  uint32_t color_offset{0};
  bool has_color{false};
};

// GL enum constants mirrored here so core needs no GL header. Values are the fixed OpenGL
// ABI constants; widgets static_asserts they match GL_* at the core<->GL seam (render pass).
inline constexpr uint32_t kGlByte = 0x1400;
inline constexpr uint32_t kGlUnsignedByte = 0x1401;
inline constexpr uint32_t kGlShort = 0x1402;
inline constexpr uint32_t kGlUnsignedShort = 0x1403;
inline constexpr uint32_t kGlInt = 0x1404;
inline constexpr uint32_t kGlUnsignedInt = 0x1405;
inline constexpr uint32_t kGlFloat = 0x1406;

// Returns a layout when src is fast-path eligible, else nullopt (-> fallback).
// scalar_field empty OR naming x/y/z (spatial-axis colouring) => has_scalar=false.
// When want_rgba is true (RGB-direct mode), scalar_field is ignored and the layout instead
// carries the packed colour field (has_color/color_offset); it returns nullopt unless the
// cloud has a packed, contiguous 'rgba'/'rgb' uint32 field (separate r/g/b channels -> fallback).
[[nodiscard]] std::optional<AttribLayout> checkFastPath(
    const PJ::sdk::PointCloud& src, std::string_view scalar_field, bool want_rgba = false);

// Allocation-free single strided pass over src.data: finite-point AABB (identical math to
// convertCanonical) and, when sf != nullptr, the finite RAW min/max of that scalar field
// (not clamped; the caller applies clampScalarRange). Precondition: checkFastPath passed.
// scalar_range is nullopt when sf == nullptr OR no finite scalar value exists.
struct BoundsScanResult {
  AABB bounds;                                          // valid==false if no finite point
  std::optional<std::pair<float, float>> scalar_range;  // raw min/max
};

[[nodiscard]] BoundsScanResult scanBoundsAndScalarRange(
    const PJ::sdk::PointCloud& src, const AttribLayout& layout, const PJ::sdk::PointField* sf);

}  // namespace pj::scene3d
