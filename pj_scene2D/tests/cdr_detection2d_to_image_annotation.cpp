// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "cdr_detection2d_to_image_annotation.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "marker_palette.h"
#include "nanocdr/nanocdr.hpp"

namespace pj_demos {

PJ::Expected<PJ::ImageAnnotation> cdrDetection2DArrayToImageAnnotation(const uint8_t* data, size_t size) {
  if (data == nullptr || size < 4) {
    return PJ::unexpected(std::string("CDR Detection2DArray: buffer too small"));
  }
  try {
    nanocdr::Decoder dec(nanocdr::ConstBuffer(data, size));

    // Outer std_msgs/Header
    uint32_t sec = 0;
    uint32_t nanosec = 0;
    dec.decode(sec);
    dec.decode(nanosec);
    std::string outer_frame_id;
    dec.decode(outer_frame_id);
    const PJ::Timestamp top_ts = static_cast<int64_t>(sec) * 1'000'000'000LL + static_cast<int64_t>(nanosec);

    // detections[]
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
      // per-detection Header
      uint32_t det_sec = 0;
      uint32_t det_nanosec = 0;
      dec.decode(det_sec);
      dec.decode(det_nanosec);
      std::string det_frame_id;
      dec.decode(det_frame_id);

      // results[] : ObjectHypothesisWithPose = {class_id (string), score (double),
      //                                          PoseWithCovariance (7 + 36 doubles)}.
      // Capture the FIRST hypothesis's class_id and score for the label.
      uint32_t n_results = 0;
      dec.decode(n_results);
      std::string first_class_id;
      double first_score = std::nan("");
      for (uint32_t j = 0; j < n_results; ++j) {
        std::string class_id;
        dec.decode(class_id);
        double score = 0;
        dec.decode(score);
        if (j == 0) {
          first_class_id = std::move(class_id);
          first_score = score;
        }
        for (int k = 0; k < 7 + 36; ++k) {
          double dummy = 0;
          dec.decode(dummy);
        }
      }

      // BoundingBox2D: center(Pose2D = x, y, theta) + size_x + size_y
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

      // id
      std::string id;
      dec.decode(id);

      // Stable colour per class: hashing the class_id keeps the same class on
      // the same colour even when the publisher re-orders detections between
      // frames. Fall back to the bbox index when no hypothesis exists.
      const PJ::ColorRGBA bbox_color = first_class_id.empty()
                                           ? colorForClass(static_cast<int32_t>(i))
                                           : colorForClass(static_cast<int32_t>(fnv1a32(first_class_id)));

      if (!first_class_id.empty()) {
        labels.push_back(
            makeBboxLabel(formatLabel(first_class_id, first_score), cx, cy, sx / 2.0, sy / 2.0, bbox_color));
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
    return PJ::unexpected(std::string("CDR Detection2DArray decode failed: ") + e.what());
  } catch (...) {
    return PJ::unexpected(std::string("CDR Detection2DArray decode failed: unknown error"));
  }
}

}  // namespace pj_demos
