// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "cdr_yolo_to_image_annotation.h"

#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "marker_palette.h"
#include "nanocdr/nanocdr.hpp"

namespace pj_demos {

PJ::Expected<PJ::ImageAnnotation> cdrYoloDetectionArrayToImageAnnotation(const uint8_t* data, size_t size) {
  if (data == nullptr || size < 4) {
    return PJ::unexpected(std::string("CDR yolo DetectionArray: buffer too small"));
  }
  try {
    nanocdr::Decoder dec(nanocdr::ConstBuffer(data, size));

    // Top-level std_msgs/Header
    uint32_t sec = 0;
    uint32_t nanosec = 0;
    dec.decode(sec);
    dec.decode(nanosec);
    std::string outer_frame_id;
    dec.decode(outer_frame_id);
    const PJ::Timestamp top_ts = static_cast<int64_t>(sec) * 1'000'000'000LL + static_cast<int64_t>(nanosec);

    std::vector<PJ::PointsAnnotation> bboxes;
    std::vector<PJ::TextAnnotation> labels;
    uint32_t n_detections = 0;
    dec.decode(n_detections);
    // Cap reserve at a sane bound: untrusted wire bytes mean a malformed
    // length field could otherwise trigger a multi-GB allocation here.
    constexpr uint32_t kReserveCap = 100'000;
    const uint32_t reserve_n = n_detections < kReserveCap ? n_detections : kReserveCap;
    bboxes.reserve(reserve_n);
    labels.reserve(reserve_n);

    for (uint32_t i = 0; i < n_detections; ++i) {
      int32_t class_id = 0;
      dec.decode(class_id);
      std::string class_name;
      dec.decode(class_name);
      double score = 0;
      dec.decode(score);
      std::string id;
      dec.decode(id);

      // BoundingBox2D = Pose2D{Point2D{x,y}, theta} + Vector2{x,y}
      double cx = 0;
      double cy = 0;
      double theta = 0;
      double sx = 0;
      double sy = 0;
      dec.decode(cx);
      dec.decode(cy);
      dec.decode(theta);
      dec.decode(sx);
      dec.decode(sy);

      // BoundingBox3D = Pose{Point3, Quaternion} + Vector3 + frame_id
      // 3 + 4 + 3 = 10 doubles, then string
      for (int k = 0; k < 10; ++k) {
        double dummy = 0;
        dec.decode(dummy);
      }
      std::string bbox3d_frame_id;
      dec.decode(bbox3d_frame_id);

      // Mask = int32 height + int32 width + Point2D[] data
      int32_t mask_h = 0;
      int32_t mask_w = 0;
      dec.decode(mask_h);
      dec.decode(mask_w);
      uint32_t n_mask = 0;
      dec.decode(n_mask);
      for (uint32_t j = 0; j < n_mask; ++j) {
        double mx = 0;
        double my = 0;
        dec.decode(mx);
        dec.decode(my);
      }

      // KeyPoint2DArray = KeyPoint2D[] data, where KeyPoint2D = int32 id + Point2D + double score
      uint32_t n_kp2 = 0;
      dec.decode(n_kp2);
      for (uint32_t j = 0; j < n_kp2; ++j) {
        int32_t kp_id = 0;
        dec.decode(kp_id);
        double kx = 0;
        double ky = 0;
        double ks = 0;
        dec.decode(kx);
        dec.decode(ky);
        dec.decode(ks);
      }

      // KeyPoint3DArray = KeyPoint3D[] data + frame_id
      uint32_t n_kp3 = 0;
      dec.decode(n_kp3);
      for (uint32_t j = 0; j < n_kp3; ++j) {
        int32_t kp_id = 0;
        dec.decode(kp_id);
        double px = 0;
        double py = 0;
        double pz = 0;
        double ks = 0;
        dec.decode(px);
        dec.decode(py);
        dec.decode(pz);
        dec.decode(ks);
      }
      std::string kp3_frame_id;
      dec.decode(kp3_frame_id);

      const PJ::ColorRGBA bbox_color = colorForClass(class_id);
      if (!class_name.empty()) {
        labels.push_back(makeBboxLabel(formatLabel(class_name, score), cx, cy, sx / 2.0, sy / 2.0, bbox_color));
      }
      bboxes.push_back(makeBboxLineLoop(cx, cy, sx, sy, bbox_color));
    }

    PJ::ImageAnnotation ia;
    ia.timestamp_ns = top_ts;
    ia.image_topic = outer_frame_id;
    ia.points = std::move(bboxes);
    ia.texts = std::move(labels);
    return ia;
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("CDR yolo DetectionArray decode failed: ") + e.what());
  } catch (...) {
    return PJ::unexpected(std::string("CDR yolo DetectionArray decode failed: unknown error"));
  }
}

}  // namespace pj_demos
