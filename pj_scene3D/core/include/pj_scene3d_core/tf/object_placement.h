#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "pj_base/time.hpp"
#include "pj_scene3d_core/tf/view_sampling.h"
#include "pj_tf/tf_buffer.h"
#include "pj_tf/transform.h"

namespace pj::scene3d {

using pj::tf::TimePoint;
using pj::tf::Transform;
using pj::tf::TransformBuffer;

// An object's own stamp on the TF header clock: its header time plus the store
// entry's payload_stamp_shift (a time-shifted merge moves TF header stamps by the
// same amount at ingest). nullopt for an unstamped object (header time 0).
[[nodiscard]] inline std::optional<TimePoint> objectStamp(int64_t header_ns, int64_t payload_stamp_shift) {
  if (header_ns == 0) {
    return std::nullopt;
  }
  return PJ::fromRaw(header_ns + payload_stamp_shift);
}

// How far an object's stamp may run past the newest header-clock TF sample on its
// chain and still be placed by holding that sample. Live TF arrives a little after
// the data it places; further past it the two clocks disagree (e.g. data stamped
// in wall time, TF in simulated time), and holding would pin the object to one pose.
inline constexpr PJ::Duration kMaxStampPastTf = std::chrono::seconds(1);

// The anchor<-source a stamped object was last placed with, kept by its layer
// (placeObject()). Keyed by what fixes that transform: the anchor, the source frame
// and the object's stamp.
struct PlacementHold {
  std::string anchor;
  std::string source;
  TimePoint stamp{};
  Transform anchor_from_source;
  // Set by a layer that still draws an object the cursor has left, because its successor is not drawable yet (it is
  // decoding on a worker): the last cursor the drawn object was current at. placeObject() places it at that cursor,
  // so it stays where it was drawn; placed at the new cursor it would jump by the fixed frame's motion until replaced.
  std::optional<TimePoint> cursor;
};

// fixed<-source for an object stamped `object_stamp`, composed through the anchor: the
// nearest frame the fixed and the source frame share (TransformBuffer::commonAncestor).
// It is fixed<-anchor at `cursor` in the entry clock, times anchor<-source at the
// object's stamp in the header clock (where the frame was when the data was measured).
// The motion of the anchor itself never enters, so:
//  - with the tree's root as the fixed frame this is the header-clock lookup at the
//    stamp: a scan stays where it was captured in the world;
//  - in a fixed frame on a moving body, an object measured on that body (a scan in
//    the lidar frame seen from base_link) stays where it was measured on the body.
//    Composed through the root instead, it slid back by the body's motion between
//    its stamp and the cursor, in steps at the TF rate, and jumped forward at each
//    new scan;
//  - an object in a frame outside that body (a map-frame plan seen from base_link)
//    stays put in the world and moves in the view as the body moves.
//
// Every lookup reads between two TF samples as the views do (viewSampling()).
//
// Falls back to fixed<-source at `cursor` in the entry clock for an unstamped object,
// a stamp before the first TF sample on the chain, or a stamp more than
// kMaxStampPastTf past the newest one; such an object stays visible where the cursor
// puts it. `hold` (nullable) keeps the last anchor<-source placed: when live TF has
// since evicted the sample at that same stamp, it is reused instead of the fallback,
// so a stopped scan does not jump. A hold with a `cursor` replaces the cursor by it.
// Everything that places one layer (drawing, scene bounds, render keys) passes that
// layer's hold, so they agree.
[[nodiscard]] inline std::optional<Transform> placeObject(
    const TransformBuffer& tf, const std::string& fixed, const std::string& source,
    std::optional<TimePoint> object_stamp, TimePoint cursor, PlacementHold* hold = nullptr) {
  using pj::tf::Clock;
  using pj::tf::LookupError;
  if (hold != nullptr && hold->cursor.has_value()) {
    cursor = *hold->cursor;
  }
  // Without a shared frame the lookup at the cursor below fails the same way.
  if (std::optional<std::string> anchor; object_stamp.has_value() && (anchor = tf.commonAncestor(fixed, source))) {
    if (const auto fixed_from_anchor = tf.tryLookupTransform(fixed, *anchor, cursor, Clock::kEntry, viewSampling())) {
      const auto anchor_from_source =
          tf.tryLookupTransform(*anchor, source, *object_stamp, Clock::kHeader, viewSampling());
      if (anchor_from_source) {
        const auto newest = tf.latestCommonTime(*anchor, source, Clock::kHeader);
        // TimePoint{}: every edge on the chain is single-sample (static), which bounds nothing.
        const bool past_the_tf =
            newest.has_value() && *newest != TimePoint{} && *object_stamp > *newest + kMaxStampPastTf;
        if (!past_the_tf) {
          if (hold != nullptr) {
            hold->anchor = *anchor;
            hold->source = source;
            hold->stamp = *object_stamp;
            hold->anchor_from_source = *anchor_from_source;
          }
          return *fixed_from_anchor * *anchor_from_source;
        }
      } else if (
          hold != nullptr && anchor_from_source.error() == LookupError::kNoSampleAtTime &&
          hold->stamp == *object_stamp && hold->source == source && hold->anchor == *anchor) {
        return *fixed_from_anchor * hold->anchor_from_source;
      }
    }
  }
  const auto at_cursor = tf.tryLookupTransform(fixed, source, cursor, Clock::kEntry, viewSampling());
  return at_cursor ? std::optional<Transform>(*at_cursor) : std::nullopt;
}

}  // namespace pj::scene3d
