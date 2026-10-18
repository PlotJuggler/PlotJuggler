// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "point_cloud_ops.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "pj_base/builtin/point_cloud_fields.hpp"
#include "record_walk.h"

namespace PJ::scripting::native {
namespace {

using sdk::PointCloud;
using sdk::PointField;
using sdk::Vector3;

Expected<ChannelRef> findChannel(const PointCloud& cloud, std::string_view name) {
  for (const auto& field : cloud.fields) {
    if (field.name != name) {
      continue;
    }
    if (field.count != 1) {
      return unexpected("channel '" + std::string(name) + "' is not a scalar channel (count != 1)");
    }
    const uint32_t size = sdk::bytesPerElement(field.datatype);
    if (size == 0 || size > cloud.point_step || field.offset > cloud.point_step - size) {
      return unexpected("channel '" + std::string(name) + "' does not fit within point_step");
    }
    return ChannelRef{field.offset, field.datatype};
  }
  return unexpected("point cloud has no channel named '" + std::string(name) + "'");
}

template <class T>
double readAs(const uint8_t* bytes) {
  T value;
  std::memcpy(&value, bytes, sizeof(value));
  return static_cast<double>(value);
}

double read(const uint8_t* record, const ChannelRef& channel) {
  const uint8_t* bytes = record + channel.offset;
  switch (channel.datatype) {
    case PointField::Datatype::kInt8:
      return readAs<int8_t>(bytes);
    case PointField::Datatype::kUint8:
      return readAs<uint8_t>(bytes);
    case PointField::Datatype::kInt16:
      return readAs<int16_t>(bytes);
    case PointField::Datatype::kUint16:
      return readAs<uint16_t>(bytes);
    case PointField::Datatype::kInt32:
      return readAs<int32_t>(bytes);
    case PointField::Datatype::kUint32:
      return readAs<uint32_t>(bytes);
    case PointField::Datatype::kFloat32:
      return readAs<float>(bytes);
    case PointField::Datatype::kFloat64:
      return readAs<double>(bytes);
    case PointField::Datatype::kUnknown:
      return 0.0;
  }
  return 0.0;
}

/// Calls `visit(std::type_identity<T>{})`, T the C++ type of `datatype`, so the caller's loop is compiled
/// once per type, with no switch inside. Does nothing for kUnknown, which `findChannel` never returns.
template <class Visit>
void dispatchDatatype(PointField::Datatype datatype, Visit&& visit) {
  switch (datatype) {
    case PointField::Datatype::kInt8:
      visit(std::type_identity<int8_t>{});
      return;
    case PointField::Datatype::kUint8:
      visit(std::type_identity<uint8_t>{});
      return;
    case PointField::Datatype::kInt16:
      visit(std::type_identity<int16_t>{});
      return;
    case PointField::Datatype::kUint16:
      visit(std::type_identity<uint16_t>{});
      return;
    case PointField::Datatype::kInt32:
      visit(std::type_identity<int32_t>{});
      return;
    case PointField::Datatype::kUint32:
      visit(std::type_identity<uint32_t>{});
      return;
    case PointField::Datatype::kFloat32:
      visit(std::type_identity<float>{});
      return;
    case PointField::Datatype::kFloat64:
      visit(std::type_identity<double>{});
      return;
    case PointField::Datatype::kUnknown:
      return;
  }
}

/// The x/y/z coordinate type when the three channels differ: every access switches on its channel's type.
struct MixedXyz {};

/// Calls `visit(std::type_identity<T>{})`: T is float or double when x/y/z share that type (the common
/// case), else MixedXyz.
template <class Visit>
decltype(auto) dispatchXyz(const XyzAccess& access, Visit&& visit) {
  const PointField::Datatype type = access.x.datatype;
  if (access.y.datatype != type || access.z.datatype != type) {
    return visit(std::type_identity<MixedXyz>{});
  }
  if (type == PointField::Datatype::kFloat32) {
    return visit(std::type_identity<float>{});
  }
  return visit(std::type_identity<double>{});
}

template <class T>
double readCoordinate(const uint8_t* record, const ChannelRef& channel) {
  if constexpr (std::is_same_v<T, MixedXyz>) {
    return read(record, channel);
  } else {
    return readAs<T>(record + channel.offset);
  }
}

/// Inverse of `readCoordinate`, for the two datatypes `resolveXyz` accepts.
template <class T>
void writeCoordinate(uint8_t* record, const ChannelRef& channel, double value) {
  if constexpr (std::is_same_v<T, MixedXyz>) {
    if (channel.datatype == PointField::Datatype::kFloat32) {
      writeCoordinate<float>(record, channel, value);
    } else {
      writeCoordinate<double>(record, channel, value);
    }
  } else {
    const auto narrow = static_cast<T>(value);
    std::memcpy(record + channel.offset, &narrow, sizeof(narrow));
  }
}

template <class T = MixedXyz>
Vector3 readXyz(const uint8_t* record, const XyzAccess& access) {
  return Vector3{
      readCoordinate<T>(record, access.x), readCoordinate<T>(record, access.y), readCoordinate<T>(record, access.z)};
}

bool isFinite(const Vector3& point) {
  return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

/// One record's byte offset, for a sparse access; a loop over every record walks them with forEachRecord.
std::size_t recordByteOffset(const PointCloud& cloud, std::size_t index) noexcept {
  if (cloud.row_step == 0 || cloud.height <= 1) {
    return index * cloud.point_step;
  }
  return (index / cloud.width) * static_cast<std::size_t>(cloud.row_step) +
         (index % cloud.width) * static_cast<std::size_t>(cloud.point_step);
}

/// Rotates `point` by unit quaternion `rotation` (p + 2w(u x p) + 2u x (u x p), u = rotation.xyz).
Vector3 rotate(const sdk::Quaternion& rotation, const Vector3& point) {
  const double tx = 2.0 * (rotation.y * point.z - rotation.z * point.y);
  const double ty = 2.0 * (rotation.z * point.x - rotation.x * point.z);
  const double tz = 2.0 * (rotation.x * point.y - rotation.y * point.x);
  return Vector3{
      point.x + rotation.w * tx + (rotation.y * tz - rotation.z * ty),
      point.y + rotation.w * ty + (rotation.z * tx - rotation.x * tz),
      point.z + rotation.w * tz + (rotation.x * ty - rotation.y * tx)};
}

/// The layout checks every operation shares; returns the record count, which then satisfies
/// `records * point_step <= data.size()`, so no later offset product can overflow. The extent check is the
/// SDK field table's, which never overflows on a hostile header.
Expected<std::size_t> checkLayout(const PointCloud& cloud) {
  if (cloud.is_bigendian) {
    return unexpected("big-endian point clouds are not supported");
  }
  if (cloud.height > 1 && cloud.row_step != 0 &&
      cloud.row_step < static_cast<uint64_t>(cloud.width) * cloud.point_step) {
    return unexpected("point cloud rows overlap (row_step < width * point_step)");
  }
  const uint64_t declared = countPoints(cloud);
  if (sdk::pointCloudDataField("data").buffer(&cloud).record_count != declared) {
    return unexpected("point cloud data buffer is smaller than its declared layout");
  }
  return static_cast<std::size_t>(declared);
}

/// An output buffer and the reservation that pays for it; the output cloud's anchor owns both, so the
/// bytes stay charged exactly as long as some copy of the cloud is alive.
struct ChargedBytes {
  MemoryReservation charge;
  std::vector<uint8_t> bytes;
};

/// Reserves `size` bytes plus the header copied from `cloud`, then allocates the bytes (capacity only:
/// the caller fills them).
Expected<std::shared_ptr<ChargedBytes>> allocateOutput(
    const PointCloud& cloud, std::size_t size, const NativeLimits& limits) {
  std::size_t header = sizeof(PointCloud) + cloud.frame_id.size();
  for (const auto& field : cloud.fields) {
    header += sizeof(PointField) + field.name.size();
  }
  MemoryReservation charge(limits.budget);
  if (Status reserved = charge.grow(size + header); !reserved) {
    return unexpected(reserved.error());
  }
  auto owner = std::make_shared<ChargedBytes>(ChargedBytes{std::move(charge), {}});
  owner->bytes.reserve(size);
  return owner;
}

/// `cloud`'s header (frame, stamp, fields) over `owner`'s bytes.
PointCloud withBytes(const PointCloud& cloud, std::shared_ptr<ChargedBytes> owner) {
  PointCloud out = cloud;
  out.data = Span<const uint8_t>(owner->bytes.data(), owner->bytes.size());
  out.anchor = std::move(owner);
  return out;
}

/// filterByField's predicate for one channel type and comparison, so its loop holds no switch.
template <class T, class Compare>
Expected<PointCloud> filterWhere(
    const PointCloud& cloud, std::size_t records, uint32_t offset, double threshold, const NativeLimits& limits) {
  return compactWhere(cloud, records, limits, [offset, threshold](const uint8_t* record) {
    const double value = readAs<T>(record + offset);
    if constexpr (std::is_floating_point_v<T>) {
      if (!std::isfinite(value)) {
        return false;
      }
    }
    return Compare{}(value, threshold);
  });
}

/// extremePoint's scan for one channel type. x/y/z are read only when a record beats the best so far, so
/// they keep the generic read.
template <class T>
std::optional<ExtremePoint> extremeOf(
    const PointCloud& cloud, const XyzAccess& access, uint32_t offset, Extreme which) {
  const bool want_max = which == Extreme::kMax;
  std::optional<ExtremePoint> best;
  forEachRecord(cloud, cloud.data.data(), access.records, {}, [&](const uint8_t* record, std::size_t index) {
    const double value = readAs<T>(record + offset);
    if (!std::isfinite(value) || (best && (want_max ? value <= best->value : value >= best->value))) {
      return;  // ties keep the first index
    }
    const Vector3 position = readXyz(record, access);
    if (isFinite(position)) {
      best = ExtremePoint{Point{index, position}, value};
    }
  });
  return best;
}

}  // namespace

Expected<PointCloud> copyMarked(
    const PointCloud& cloud, const std::vector<uint64_t>& mask, std::size_t kept, const NativeLimits& limits) {
  const std::size_t size = kept * cloud.point_step;
  auto owner = allocateOutput(cloud, size, limits);
  if (!owner) {
    return unexpected(owner.error());
  }
  std::vector<uint8_t>& out = (*owner)->bytes;
  out.resize(size);
  uint8_t* next = out.data();
  const uint8_t* const end = next + size;
  for (std::size_t word = 0; word < mask.size() && next != end; ++word) {
    if (cancelledAt(limits, word * 64)) {
      return unexpected("cancelled");
    }
    for (uint64_t bits = mask[word]; bits != 0 && next != end; bits &= bits - 1) {
      const std::size_t index = word * 64 + static_cast<std::size_t>(std::countr_zero(bits));
      std::memcpy(next, cloud.data.data() + recordByteOffset(cloud, index), cloud.point_step);
      next += cloud.point_step;
    }
  }
  // ponytail: width/row_step are uint32, so more than 4 GiB of kept records would wrap them; any
  // evaluation budget stops far below that, only an unbounded (null) budget could get there.
  PointCloud result = withBytes(cloud, std::move(*owner));
  result.width = static_cast<uint32_t>(kept);
  result.height = 1;
  result.row_step = cloud.point_step * static_cast<uint32_t>(kept);
  return result;
}

uint64_t countPoints(const PointCloud& cloud) noexcept {
  if (cloud.point_step == 0) {
    return 0;
  }
  const uint32_t height = cloud.height == 0 ? 1 : cloud.height;
  return static_cast<uint64_t>(cloud.width) * height;
}

Expected<XyzAccess> resolveXyz(const PointCloud& cloud) {
  Expected<std::size_t> records = checkLayout(cloud);
  if (!records) {
    return unexpected(records.error());
  }
  XyzAccess access;
  access.records = *records;
  ChannelRef* axes[3] = {&access.x, &access.y, &access.z};
  const char* names[3] = {"x", "y", "z"};
  for (int axis = 0; axis < 3; ++axis) {
    Expected<ChannelRef> channel = findChannel(cloud, names[axis]);
    if (!channel) {
      return unexpected(channel.error());
    }
    if (channel->datatype != PointField::Datatype::kFloat32 && channel->datatype != PointField::Datatype::kFloat64) {
      return unexpected("x/y/z channels must be float32 or float64");
    }
    *axes[axis] = *channel;
  }
  return access;
}

std::optional<Point> nextFinitePoint(const PointCloud& cloud, const XyzAccess& access, std::size_t& index) {
  while (index < access.records) {
    const std::size_t current = index++;
    const Vector3 position = readXyz(cloud.data.data() + recordByteOffset(cloud, current), access);
    if (isFinite(position)) {
      return Point{current, position};
    }
  }
  return std::nullopt;
}

Expected<Bounds> bounds(const PointCloud& cloud) {
  Expected<XyzAccess> access = resolveXyz(cloud);
  if (!access) {
    return unexpected(access.error());
  }
  return dispatchXyz(*access, [&]<class T>(std::type_identity<T>) {
    constexpr double kInf = std::numeric_limits<double>::infinity();
    Bounds box{Vector3{kInf, kInf, kInf}, Vector3{-kInf, -kInf, -kInf}, 0};
    forEachRecord(cloud, cloud.data.data(), access->records, {}, [&](const uint8_t* record, std::size_t) {
      const Vector3 position = readXyz<T>(record, *access);
      if (!isFinite(position)) {
        return;
      }
      box.min =
          Vector3{std::min(box.min.x, position.x), std::min(box.min.y, position.y), std::min(box.min.z, position.z)};
      box.max =
          Vector3{std::max(box.max.x, position.x), std::max(box.max.y, position.y), std::max(box.max.z, position.z)};
      ++box.count;
    });
    return box;
  });
}

Expected<std::optional<ExtremePoint>> extremePoint(const PointCloud& cloud, std::string_view channel, Extreme which) {
  Expected<XyzAccess> access = resolveXyz(cloud);
  if (!access) {
    return unexpected(access.error());
  }
  Expected<ChannelRef> ref = findChannel(cloud, channel);
  if (!ref) {
    return unexpected(ref.error());
  }
  std::optional<ExtremePoint> best;
  dispatchDatatype(
      ref->datatype, [&]<class T>(std::type_identity<T>) { best = extremeOf<T>(cloud, *access, ref->offset, which); });
  return best;
}

Expected<PointCloud> cropBox(
    const PointCloud& cloud, const Vector3& min, const Vector3& max, const NativeLimits& limits) {
  Expected<XyzAccess> access = resolveXyz(cloud);
  if (!access) {
    return unexpected(access.error());
  }
  Expected<PointCloud> out = dispatchXyz(*access, [&]<class T>(std::type_identity<T>) {
    return compactWhere(cloud, access->records, limits, [&](const uint8_t* record) {
      const Vector3 position = readXyz<T>(record, *access);
      return isFinite(position) && position.x >= min.x && position.x <= max.x && position.y >= min.y &&
             position.y <= max.y && position.z >= min.z && position.z <= max.z;
    });
  });
  if (out) {
    out->is_dense = true;  // every kept record has finite coordinates
  }
  return out;
}

Expected<PointCloud> filterByField(
    const PointCloud& cloud, std::string_view channel, CompareOp op, double threshold, const NativeLimits& limits) {
  Expected<std::size_t> records = checkLayout(cloud);
  if (!records) {
    return unexpected(records.error());
  }
  Expected<ChannelRef> ref = findChannel(cloud, channel);
  if (!ref) {
    return unexpected(ref.error());
  }
  Expected<PointCloud> out = unexpected("unsupported channel datatype");
  dispatchDatatype(ref->datatype, [&]<class T>(std::type_identity<T>) {
    switch (op) {
      case CompareOp::kLt:
        out = filterWhere<T, std::less<>>(cloud, *records, ref->offset, threshold, limits);
        return;
      case CompareOp::kLe:
        out = filterWhere<T, std::less_equal<>>(cloud, *records, ref->offset, threshold, limits);
        return;
      case CompareOp::kGt:
        out = filterWhere<T, std::greater<>>(cloud, *records, ref->offset, threshold, limits);
        return;
      case CompareOp::kGe:
        out = filterWhere<T, std::greater_equal<>>(cloud, *records, ref->offset, threshold, limits);
        return;
      case CompareOp::kEq:
        out = filterWhere<T, std::equal_to<>>(cloud, *records, ref->offset, threshold, limits);
        return;
      case CompareOp::kNe:
        out = filterWhere<T, std::not_equal_to<>>(cloud, *records, ref->offset, threshold, limits);
        return;
    }
  });
  return out;
}

Expected<PointCloud> transformXyz(const PointCloud& cloud, const sdk::FrameTransform& tf, const NativeLimits& limits) {
  const sdk::Quaternion& raw = tf.rotation;
  const double norm = std::sqrt(raw.x * raw.x + raw.y * raw.y + raw.z * raw.z + raw.w * raw.w);
  if (!isFinite(tf.translation) || !std::isfinite(norm) || norm == 0.0) {
    return unexpected("transform must be finite, with a non-zero rotation");
  }
  const sdk::Quaternion rotation{raw.x / norm, raw.y / norm, raw.z / norm, raw.w / norm};
  Expected<XyzAccess> access = resolveXyz(cloud);
  if (!access) {
    return unexpected(access.error());
  }
  auto owner = allocateOutput(cloud, cloud.data.size(), limits);
  if (!owner) {
    return unexpected(owner.error());
  }
  std::vector<uint8_t>& out = (*owner)->bytes;
  out.assign(cloud.data.begin(), cloud.data.end());
  const bool finished = dispatchXyz(*access, [&]<class T>(std::type_identity<T>) {
    return forEachRecord(cloud, out.data(), access->records, limits, [&](uint8_t* record, std::size_t) {
      const Vector3 position = readXyz<T>(record, *access);
      if (!isFinite(position)) {
        return;
      }
      const Vector3 moved = rotate(rotation, position);
      writeCoordinate<T>(record, access->x, moved.x + tf.translation.x);
      writeCoordinate<T>(record, access->y, moved.y + tf.translation.y);
      writeCoordinate<T>(record, access->z, moved.z + tf.translation.z);
    });
  });
  if (!finished) {
    return unexpected("cancelled");
  }
  PointCloud result = withBytes(cloud, std::move(*owner));
  if (!tf.parent_frame_id.empty()) {
    result.frame_id = tf.parent_frame_id;
  }
  return result;
}

}  // namespace PJ::scripting::native
