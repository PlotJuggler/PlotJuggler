// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "point_cloud_builder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "field_checks.h"
#include "pj_base/buffer_anchor.hpp"

namespace PJ::scripting::native {
namespace {

using sdk::PointField;

/// The first growth: 1 KiB, so a small cloud reallocates a handful of times at most.
constexpr std::size_t kFirstRecords = 64;
constexpr std::size_t kMaxCloudBytes = kMaxCloudPoints * kCloudRecordBytes;

Status tooManyPoints() {
  return unexpected("cloud: more than " + std::to_string(kMaxCloudPoints) + " points");
}

PointField channel(const char* name, uint32_t offset, PointField::Datatype datatype = PointField::Datatype::kFloat32) {
  return PointField{.name = name, .offset = offset, .datatype = datatype, .count = 1};
}

std::vector<PointField> presetFields(CloudPreset preset) {
  std::vector<PointField> fields{channel("x", 0), channel("y", 4), channel("z", 8)};
  if (preset == CloudPreset::kXyzi) {
    fields.push_back(channel("intensity", 12));
  } else if (preset == CloudPreset::kXyzrgb) {
    fields.push_back(channel("rgb", 12, PointField::Datatype::kUint32));
  }
  return fields;
}

const char* presetValues(CloudPreset preset) {
  switch (preset) {
    case CloudPreset::kXyzi:
      return "a xyzi point takes 4 values (x, y, z, intensity)";
    case CloudPreset::kXyzrgb:
      return "a xyzrgb point takes 6 values (x, y, z, r, g, b)";
    case CloudPreset::kXyz:
      break;
  }
  return "a xyz point takes 3 values (x, y, z)";
}

}  // namespace

std::optional<CloudPreset> parseCloudPreset(std::string_view name) {
  if (name == "xyz") {
    return CloudPreset::kXyz;
  }
  if (name == "xyzi") {
    return CloudPreset::kXyzi;
  }
  if (name == "xyzrgb") {
    return CloudPreset::kXyzrgb;
  }
  return std::nullopt;
}

Expected<PointCloudBuilder> PointCloudBuilder::create(
    CloudPreset preset, std::string frame_id, std::shared_ptr<MemoryBudget> budget) {
  sdk::PointCloud header;
  header.fields = presetFields(preset);
  std::size_t header_bytes = sizeof(sdk::PointCloud) + frame_id.size();
  for (const PointField& field : header.fields) {
    header_bytes += sizeof(PointField) + field.name.size();
  }
  MemoryReservation charge(std::move(budget));
  if (Status reserved = charge.grow(header_bytes); !reserved) {
    return unexpected(reserved.error());
  }
  header.frame_id = std::move(frame_id);
  header.height = 1;
  header.point_step = kCloudRecordBytes;
  header.is_dense = true;
  return PointCloudBuilder(preset, std::move(header), std::move(charge));
}

std::size_t PointCloudBuilder::arity() const noexcept {
  return preset_ == CloudPreset::kXyz ? 3 : preset_ == CloudPreset::kXyzi ? 4 : 6;
}

Status PointCloudBuilder::acceptsArity(std::size_t count) const {
  if (count != arity()) {
    return unexpected(std::string("cloud: ") + presetValues(preset_));
  }
  return okStatus();
}

Status PointCloudBuilder::growTo(std::size_t capacity) {
  const std::size_t held = bytes_.capacity();
  if (capacity <= held) {
    return okStatus();
  }
  // The reallocation holds the old buffer beside the new one: charge the whole new capacity, then give the old back.
  if (Status reserved = charge_.grow(capacity); !reserved) {
    return reserved;
  }
  bytes_.reserve(capacity);
  charge_.shrink(held);
  return okStatus();
}

Status PointCloudBuilder::reserve(std::size_t points) {
  const std::size_t records = bytes_.size() / kCloudRecordBytes;
  if (points > kMaxCloudPoints - records) {
    return tooManyPoints();
  }
  return growTo((records + points) * kCloudRecordBytes);  // sized exactly: no doubling slack
}

Status PointCloudBuilder::add(std::span<const double> values) {
  if (Status fits = acceptsArity(values.size()); !fits) {
    return fits;
  }
  for (const double value : values) {
    if (Status valid = checkFloat32("cloud: every value of a point", value); !valid) {
      return valid;
    }
  }
  if (preset_ == CloudPreset::kXyzrgb) {
    for (std::size_t index = 3; index < 6; ++index) {
      if (!(values[index] >= 0.0 && values[index] <= 255.0)) {
        return unexpected("cloud: r, g and b must be numbers from 0 to 255");
      }
    }
  }
  const std::size_t offset = bytes_.size();
  if (offset / kCloudRecordBytes == kMaxCloudPoints) {
    return tooManyPoints();
  }
  if (offset == bytes_.capacity()) {
    const std::size_t doubled = offset > kMaxCloudBytes / 2 ? kMaxCloudBytes : offset * 2;
    if (Status grown = growTo(offset == 0 ? kFirstRecords * kCloudRecordBytes : doubled); !grown) {
      return grown;
    }
  }
  bytes_.resize(offset + kCloudRecordBytes);
  uint8_t* record = bytes_.data() + offset;
  const float xyz[3] = {static_cast<float>(values[0]), static_cast<float>(values[1]), static_cast<float>(values[2])};
  std::memcpy(record, xyz, sizeof(xyz));
  if (preset_ == CloudPreset::kXyzi) {
    const auto intensity = static_cast<float>(values[3]);
    std::memcpy(record + 12, &intensity, sizeof(intensity));
  } else if (preset_ == CloudPreset::kXyzrgb) {
    record[12] = static_cast<uint8_t>(values[3]);
    record[13] = static_cast<uint8_t>(values[4]);
    record[14] = static_cast<uint8_t>(values[5]);
    record[15] = 255;
  }
  return okStatus();
}

Charged<sdk::PointCloud> PointCloudBuilder::finish() && {
  sdk::PointCloud cloud = std::move(header_);
  const auto records = static_cast<uint32_t>(bytes_.size() / kCloudRecordBytes);
  cloud.width = records;
  cloud.row_step = records * kCloudRecordBytes;
  sdk::PayloadView view = sdk::makePayloadView(std::move(bytes_));
  cloud.data = view.bytes;
  cloud.anchor = std::move(view.anchor);
  return {std::move(cloud), std::move(charge_)};
}

}  // namespace PJ::scripting::native
