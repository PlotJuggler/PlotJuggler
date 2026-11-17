// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/pointcloud_codecs.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cloudini_lib/cloudini.hpp"
#include "draco/attributes/geometry_attribute.h"
#include "draco/compression/decode.h"
#include "draco/compression/point_cloud/point_cloud_decoder.h"
#include "draco/compression/point_cloud/point_cloud_kd_tree_decoder.h"
#include "draco/compression/point_cloud/point_cloud_sequential_decoder.h"
#include "draco/core/decoder_buffer.h"
#include "draco/metadata/geometry_metadata.h"
#include "draco/metadata/metadata_decoder.h"
#include "draco/point_cloud/point_cloud.h"

namespace pj::scene3d {
namespace {

using PJ::sdk::PointField;

// Cloudini FieldType 1..8 maps 1:1 to PJ::sdk::PointField::Datatype. INT64/UINT64/UNKNOWN
// have no PJ equivalent -> dropped (nullopt); their bytes still occupy point_step so the
// surviving fields keep their offsets. ROS-origin clouds never hit this (max FLOAT64=8).
std::optional<PointField::Datatype> mapCloudiniType(Cloudini::FieldType t) {
  switch (t) {
    case Cloudini::FieldType::INT8:
      return PointField::Datatype::kInt8;
    case Cloudini::FieldType::UINT8:
      return PointField::Datatype::kUint8;
    case Cloudini::FieldType::INT16:
      return PointField::Datatype::kInt16;
    case Cloudini::FieldType::UINT16:
      return PointField::Datatype::kUint16;
    case Cloudini::FieldType::INT32:
      return PointField::Datatype::kInt32;
    case Cloudini::FieldType::UINT32:
      return PointField::Datatype::kUint32;
    case Cloudini::FieldType::FLOAT32:
      return PointField::Datatype::kFloat32;
    case Cloudini::FieldType::FLOAT64:
      return PointField::Datatype::kFloat64;
    case Cloudini::FieldType::UNKNOWN:
    case Cloudini::FieldType::INT64:
    case Cloudini::FieldType::UINT64:
    default:
      return std::nullopt;
  }
}

// Wrap an owned byte buffer as a zero-copy Span + BufferAnchor on the cloud.
void adoptBuffer(PJ::sdk::PointCloud& out, std::vector<uint8_t>&& bytes) {
  auto owned = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
  out.data = PJ::Span<const uint8_t>(owned->data(), owned->size());
  out.anchor = owned;
}

// Per-component field names for a Draco attribute. Named attributes
// (POSITION/NORMAL/COLOR/TEX_COORD) use their canonical component names. Everything
// else (GENERIC — e.g. intensity, ring, time) uses `meta_name` when the bitstream
// carries one (Foxglove/draco_point_cloud_transport store the original field name in
// Draco attribute metadata), falling back to generic_<id> when it doesn't.
std::vector<std::string> dracoFieldNames(
    draco::GeometryAttribute::Type type, int num_components, int att_index, const std::string& meta_name) {
  static constexpr const char* kPos[] = {"x", "y", "z"};
  static constexpr const char* kNormal[] = {"nx", "ny", "nz"};
  static constexpr const char* kColor[] = {"red", "green", "blue", "alpha"};  // Foxglove color-field convention
  static constexpr const char* kTex[] = {"u", "v", "w"};
  std::span<const char* const> base;
  switch (type) {
    case draco::GeometryAttribute::POSITION:
      base = kPos;
      break;
    case draco::GeometryAttribute::NORMAL:
      base = kNormal;
      break;
    case draco::GeometryAttribute::COLOR:
      base = kColor;
      break;
    case draco::GeometryAttribute::TEX_COORD:
      base = kTex;
      break;
    default:
      break;
  }
  const std::string fallback = !meta_name.empty() ? meta_name : ("generic_" + std::to_string(att_index));
  std::vector<std::string> names;
  names.reserve(static_cast<size_t>(num_components));
  for (int c = 0; c < num_components; ++c) {
    if (c < std::ssize(base)) {
      names.emplace_back(base[static_cast<size_t>(c)]);
    } else if (num_components == 1) {
      names.emplace_back(fallback);
    } else {
      names.emplace_back(fallback + "_" + std::to_string(c));
    }
  }
  return names;
}

// row_step = point_step * width, computed in uint64 and clamped to UINT32_MAX so an adversarial product cannot wrap the
// uint32 field; the clamp keeps the published canonical object's row_step honest for any consumer that addresses rows
// by it.
uint32_t rowStepFor(uint32_t point_step, uint32_t width) {
  const uint64_t row_step = static_cast<uint64_t>(point_step) * static_cast<uint64_t>(width);
  return row_step > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                         : static_cast<uint32_t>(row_step);
}

bool exceedsLimit(std::uint64_t value, std::uint64_t limit) {
  return limit != 0 && value > limit;
}

struct DracoPreflight {
  std::uint32_t point_count = 0;
  std::uint8_t encoder_method = 0;
};

PJ::Expected<DracoPreflight> dracoPreflight(const PJ::sdk::CompressedPointCloud& cloud) {
  draco::DecoderBuffer buffer;
  buffer.Init(reinterpret_cast<const char*>(cloud.data.data()), cloud.data.size());
  draco::DracoHeader header;
  const draco::Status header_status = draco::PointCloudDecoder::DecodeHeader(&buffer, &header);
  if (!header_status.ok()) {
    return PJ::unexpected(std::string("draco header failed: ") + header_status.error_msg());
  }
  if (header.encoder_type != draco::POINT_CLOUD) {
    return PJ::unexpected(std::string("draco: encoded geometry is not a point cloud"));
  }
  if (header.encoder_method != draco::POINT_CLOUD_SEQUENTIAL_ENCODING &&
      header.encoder_method != draco::POINT_CLOUD_KD_TREE_ENCODING) {
    return PJ::unexpected(std::string("draco: unsupported point-cloud encoding method"));
  }
  if ((header.flags & METADATA_FLAG_MASK) != 0) {
    draco::GeometryMetadata metadata;
    draco::MetadataDecoder metadata_decoder;
    if (!metadata_decoder.DecodeGeometryMetadata(&buffer, &metadata)) {
      return PJ::unexpected(std::string("draco: malformed geometry metadata"));
    }
  }
  std::int32_t point_count = 0;
  if (!buffer.Decode(&point_count) || point_count < 0) {
    return PJ::unexpected(std::string("draco: malformed point count"));
  }
  return DracoPreflight{
      .point_count = static_cast<std::uint32_t>(point_count),
      .encoder_method = header.encoder_method,
  };
}

template <typename DecoderBase>
class BudgetedDracoDecoder final : public DecoderBase {
 public:
  BudgetedDracoDecoder(std::uint64_t max_decoded_bytes, std::string* budget_error)
      : max_decoded_bytes_(max_decoded_bytes), budget_error_(budget_error) {}

 protected:
  bool DecodeAllAttributes() override {
    std::uint64_t point_step = 0;
    for (int index = 0; index < this->point_cloud()->num_attributes(); ++index) {
      const draco::PointAttribute* attribute = this->point_cloud()->attribute(index);
      if (attribute == nullptr) {
        continue;
      }
      const std::uint64_t components = static_cast<std::uint64_t>(attribute->num_components());
      if (components > (std::numeric_limits<std::uint64_t>::max() - point_step) / sizeof(float)) {
        *budget_error_ = "draco: packed point layout overflows";
        return false;
      }
      point_step += components * sizeof(float);
    }
    const std::uint64_t point_count = static_cast<std::uint64_t>(this->point_cloud()->num_points());
    const std::uint64_t decoded_bytes =
        point_step != 0 && point_count > std::numeric_limits<std::uint64_t>::max() / point_step
            ? std::numeric_limits<std::uint64_t>::max()
            : point_count * point_step;
    if (exceedsLimit(decoded_bytes, max_decoded_bytes_)) {
      *budget_error_ = std::string("draco: decoded payload ") + std::to_string(decoded_bytes) +
                       " bytes exceeds limit " + std::to_string(max_decoded_bytes_);
      return false;
    }
    return draco::PointCloudDecoder::DecodeAllAttributes();
  }

 private:
  std::uint64_t max_decoded_bytes_ = 0;
  std::string* budget_error_ = nullptr;
};

PJ::Expected<std::unique_ptr<draco::PointCloud>> decodeBudgetedDraco(
    const PJ::sdk::CompressedPointCloud& cloud, const DracoPreflight& preflight, std::uint64_t max_decoded_bytes) {
  std::string budget_error;
  std::unique_ptr<draco::PointCloudDecoder> decoder;
  if (preflight.encoder_method == draco::POINT_CLOUD_SEQUENTIAL_ENCODING) {
    decoder =
        std::make_unique<BudgetedDracoDecoder<draco::PointCloudSequentialDecoder>>(max_decoded_bytes, &budget_error);
  } else if (preflight.encoder_method == draco::POINT_CLOUD_KD_TREE_ENCODING) {
    decoder = std::make_unique<BudgetedDracoDecoder<draco::PointCloudKdTreeDecoder>>(max_decoded_bytes, &budget_error);
  } else {
    return PJ::unexpected(std::string("draco: unsupported point-cloud encoding method"));
  }

  draco::DecoderBuffer buffer;
  buffer.Init(reinterpret_cast<const char*>(cloud.data.data()), cloud.data.size());
  auto point_cloud = std::make_unique<draco::PointCloud>();
  const draco::DecoderOptions options;
  const draco::Status status = decoder->Decode(options, &buffer, point_cloud.get());
  if (!status.ok()) {
    if (!budget_error.empty()) {
      return PJ::unexpected(std::move(budget_error));
    }
    return PJ::unexpected(std::string("draco decode failed: ") + status.error_msg());
  }
  return point_cloud;
}

}  // namespace

PJ::Expected<PJ::sdk::PointCloud> decodeCloudiniImpl(
    const PJ::sdk::CompressedPointCloud& cloud, const PointCloudDecodeLimits* limits);
PJ::Expected<PJ::sdk::PointCloud> decodeDracoImpl(
    const PJ::sdk::CompressedPointCloud& cloud, const PointCloudDecodeLimits* limits);

PJ::Expected<PJ::sdk::PointCloud> decodeCloudini(const PJ::sdk::CompressedPointCloud& cloud) {
  return decodeCloudiniImpl(cloud, nullptr);
}

PJ::Expected<PJ::sdk::PointCloud> decodeCloudiniImpl(
    const PJ::sdk::CompressedPointCloud& cloud, const PointCloudDecodeLimits* limits) {
  if (cloud.data.empty()) {
    return PJ::unexpected(std::string("cloudini: empty data"));
  }
  try {
    Cloudini::ConstBufferView input(cloud.data.data(), cloud.data.size());
    const Cloudini::EncodingInfo info = Cloudini::DecodeHeader(input);  // advances `input` past the header

    const std::uint64_t point_count = static_cast<std::uint64_t>(info.width) * info.height;
    const std::uint64_t decoded_bytes =
        info.point_step != 0 && point_count > std::numeric_limits<std::uint64_t>::max() / info.point_step
            ? std::numeric_limits<std::uint64_t>::max()
            : point_count * info.point_step;
    if (limits != nullptr && exceedsLimit(point_count, limits->max_points)) {
      return PJ::unexpected(
          std::string("cloudini: decoded point count ") + std::to_string(point_count) + " exceeds limit " +
          std::to_string(limits->max_points));
    }
    if (limits != nullptr && exceedsLimit(decoded_bytes, limits->max_decoded_bytes)) {
      return PJ::unexpected(
          std::string("cloudini: decoded payload ") + std::to_string(decoded_bytes) + " bytes exceeds limit " +
          std::to_string(limits->max_decoded_bytes));
    }

    // The header is untrusted and cloudini's own decoder writes each field at
    // dest + offset UNCHECKED — a hostile offset is a heap OOB write. Reject any
    // field that doesn't fit inside point_step before running the decode.
    for (const auto& f : info.fields) {
      if (f.offset == Cloudini::kDecodeButSkipStore) {
        continue;  // decoded for prediction only; never written to the output buffer
      }
      const uint64_t field_end = static_cast<uint64_t>(f.offset) + static_cast<uint64_t>(Cloudini::SizeOf(f.type));
      if (field_end > info.point_step) {
        return PJ::unexpected(
            std::string("cloudini: field '") + f.name + "' offset out of range (" + std::to_string(f.offset) + " + " +
            std::to_string(Cloudini::SizeOf(f.type)) + " > point_step " + std::to_string(info.point_step) + ")");
      }
    }

    std::vector<uint8_t> raw;
    Cloudini::PointcloudDecoder decoder;
    decoder.decode(info, input, raw);  // resizes raw to width*height*point_step

    PJ::sdk::PointCloud out;
    out.timestamp_ns = cloud.timestamp_ns;
    out.frame_id = cloud.frame_id;
    out.width = info.width;
    // height passes through verbatim: cloudini sized the buffer as width*height*point_step,
    // so mapping a (malformed) height==0 to 1 would claim width points backed by 0 bytes.
    out.height = info.height;
    out.point_step = info.point_step;
    out.row_step = rowStepFor(info.point_step, info.width);
    out.is_bigendian = false;
    out.is_dense = true;

    for (const auto& f : info.fields) {
      if (f.offset == Cloudini::kDecodeButSkipStore) {
        continue;  // decoded for prediction but not materialised in the output buffer
      }
      const std::optional<PointField::Datatype> dt = mapCloudiniType(f.type);
      if (!dt.has_value()) {
        continue;  // INT64/UINT64/UNKNOWN: dropped (see mapCloudiniType)
      }
      out.fields.push_back(PointField{f.name, f.offset, *dt, 1});
    }

    adoptBuffer(out, std::move(raw));
    return out;
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("cloudini decode failed: ") + e.what());
  } catch (...) {
    return PJ::unexpected(std::string("cloudini decode failed: unknown exception"));
  }
}

PJ::Expected<PJ::sdk::PointCloud> decodeDraco(const PJ::sdk::CompressedPointCloud& cloud) {
  return decodeDracoImpl(cloud, nullptr);
}

PJ::Expected<PJ::sdk::PointCloud> decodeDracoImpl(
    const PJ::sdk::CompressedPointCloud& cloud, const PointCloudDecodeLimits* limits) {
  if (cloud.data.empty()) {
    return PJ::unexpected(std::string("draco: empty data"));
  }
  // Exception barrier: draco allocates from header-declared counts with no bounds
  // check against the buffer size, so a corrupt blob can throw bad_alloc/length_error
  // deep inside the decoder (as can our own output allocation below). The async
  // caller runs this on a QtConcurrent pool thread — an escaped exception would be
  // rethrown by QFutureWatcher::result() on the GUI thread and terminate the app.
  try {
    std::optional<DracoPreflight> preflight;
    if (limits != nullptr) {
      auto inspected = dracoPreflight(cloud);
      if (!inspected.has_value()) {
        return PJ::unexpected(inspected.error());
      }
      preflight = inspected.value();
      if (exceedsLimit(preflight->point_count, limits->max_points)) {
        return PJ::unexpected(
            std::string("draco: decoded point count ") + std::to_string(preflight->point_count) + " exceeds limit " +
            std::to_string(limits->max_points));
      }
    }

    std::unique_ptr<draco::PointCloud> pc;
    if (preflight.has_value()) {
      auto decoded = decodeBudgetedDraco(cloud, *preflight, limits->max_decoded_bytes);
      if (!decoded.has_value()) {
        return PJ::unexpected(decoded.error());
      }
      pc = std::move(decoded.value());
    } else {
      draco::DecoderBuffer buffer;
      buffer.Init(reinterpret_cast<const char*>(cloud.data.data()), cloud.data.size());
      draco::Decoder decoder;
      auto status_or = decoder.DecodePointCloudFromBuffer(&buffer);
      if (!status_or.ok()) {
        return PJ::unexpected(std::string("draco decode failed: ") + status_or.status().error_msg());
      }
      pc = std::move(status_or).value();
    }
    const uint32_t num_points = pc->num_points();

    // Plan the packed (interleaved, all-float32) layout. Every Draco attribute becomes a
    // run of float32 components so the existing convertCanonical() reads it uniformly.
    struct PlannedAttr {
      const draco::PointAttribute* attr;
      int num_components;
      uint32_t offset;
      bool direct_copy;  // source is already packed float32 -> memcpy instead of ConvertValue
    };
    const draco::GeometryMetadata* metadata = pc->GetMetadata();
    std::vector<PlannedAttr> plan;
    std::vector<PointField> fields;
    uint32_t offset = 0;
    for (int att_index = 0; att_index < pc->num_attributes(); ++att_index) {
      const draco::PointAttribute* attr = pc->attribute(att_index);
      if (attr == nullptr) {
        continue;
      }
      const int nc = attr->num_components();
      std::string meta_name;
      if (metadata != nullptr) {
        const draco::AttributeMetadata* am =
            metadata->GetAttributeMetadataByUniqueId(static_cast<int32_t>(attr->unique_id()));
        if (am != nullptr && !am->GetEntryString("name", &meta_name)) {
          am->GetEntryString("attribute_name", &meta_name);
        }
      }
      const std::vector<std::string> names = dracoFieldNames(attr->attribute_type(), nc, att_index, meta_name);
      for (int c = 0; c < nc; ++c) {
        fields.push_back(
            PointField{
                names[static_cast<size_t>(c)], offset + static_cast<uint32_t>(c) * 4u, PointField::Datatype::kFloat32,
                1});
      }
      const bool direct_copy =
          attr->data_type() == draco::DT_FLOAT32 && attr->byte_stride() == static_cast<int64_t>(nc) * 4;
      plan.push_back(PlannedAttr{attr, nc, offset, direct_copy});
      offset += static_cast<uint32_t>(nc) * 4u;
    }
    const uint32_t point_step = offset;
    const std::uint64_t decoded_bytes = static_cast<std::uint64_t>(num_points) * point_step;
    if (limits != nullptr && exceedsLimit(decoded_bytes, limits->max_decoded_bytes)) {
      return PJ::unexpected(
          std::string("draco: decoded payload ") + std::to_string(decoded_bytes) + " bytes exceeds limit " +
          std::to_string(limits->max_decoded_bytes));
    }

    PJ::sdk::PointCloud out;
    out.timestamp_ns = cloud.timestamp_ns;
    out.frame_id = cloud.frame_id;
    out.width = num_points;
    out.height = 1;
    out.point_step = point_step;
    out.row_step = rowStepFor(point_step, num_points);
    out.is_bigendian = false;
    out.is_dense = true;
    out.fields = std::move(fields);

    if (num_points == 0 || point_step == 0) {
      adoptBuffer(out, std::vector<uint8_t>{});
      return out;
    }

    std::vector<uint8_t> raw(static_cast<size_t>(num_points) * point_step, 0);
    for (uint32_t i = 0; i < num_points; ++i) {
      uint8_t* point_base = raw.data() + static_cast<size_t>(i) * point_step;
      for (const PlannedAttr& pa : plan) {
        const draco::AttributeValueIndex avi = pa.attr->mapped_index(draco::PointIndex(i));
        auto* dst = point_base + pa.offset;
        if (pa.direct_copy) {
          // Typical bitstream case (everything float32): a plain copy, skipping ConvertValue's per-call data_type
          // switch and per-component dispatch.
          std::memcpy(dst, pa.attr->GetAddress(avi), static_cast<size_t>(pa.num_components) * 4u);
        } else {
          // ConvertValue returns false only on an unsupported source type; the buffer is
          // zero-initialised, so a failed component is left as 0 rather than garbage.
          pa.attr->ConvertValue<float>(avi, static_cast<int8_t>(pa.num_components), reinterpret_cast<float*>(dst));
        }
      }
    }
    adoptBuffer(out, std::move(raw));
    return out;
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("draco decode failed: ") + e.what());
  } catch (...) {
    return PJ::unexpected(std::string("draco decode failed: unknown exception"));
  }
}

PJ::Expected<PJ::sdk::PointCloud> decodeCompressedPointCloud(const PJ::sdk::CompressedPointCloud& cloud) {
  std::string format = cloud.format;
  std::transform(
      format.begin(), format.end(), format.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (format == "cloudini") {
    return decodeCloudini(cloud);
  }
  if (format == "draco") {
    return decodeDraco(cloud);
  }
  return PJ::unexpected(std::string("unsupported CompressedPointCloud format: '") + cloud.format + "'");
}

PJ::Expected<PJ::sdk::PointCloud> decodeCompressedPointCloud(
    const PJ::sdk::CompressedPointCloud& cloud, PointCloudDecodeLimits limits) {
  std::string format = cloud.format;
  std::transform(
      format.begin(), format.end(), format.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (format == "cloudini") {
    return decodeCloudiniImpl(cloud, &limits);
  }
  if (format == "draco") {
    return decodeDracoImpl(cloud, &limits);
  }
  return PJ::unexpected(std::string("unsupported CompressedPointCloud format: '") + cloud.format + "'");
}

}  // namespace pj::scene3d
