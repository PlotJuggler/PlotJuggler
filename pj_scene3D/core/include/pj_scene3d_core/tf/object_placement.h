#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "pj_base/time.hpp"
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

// The root<-source a stamped object was last placed with, kept by its layer
// (placeObject()). Keyed by what fixes that transform: the root, the source frame
// and the object's stamp.
struct PlacementHold {
  std::string root;
  std::string source;
  TimePoint stamp{};
  Transform root_from_source;
};

// fixed<-source for an object stamped `object_stamp`, composed through the root of
// the frames' tree: fixed<-root at `cursor` in the entry clock, times root<-source at
// the object's stamp in the header clock (where the frame was when the data was
// measured, as RViz places it). A stamped object therefore stays where it was
// captured in the world, also in a moving fixed frame; with the root as the fixed
// frame this is the header-clock lookup at the stamp.
//
// Falls back to fixed<-source at `cursor` in the entry clock for an unstamped object,
// a stamp before the first TF sample on the chain, or a stamp more than
// kMaxStampPastTf past the newest one; such an object stays visible where the cursor
// puts it. `hold` (nullable) keeps the last root<-source placed: when live TF has
// since evicted the sample at that same stamp, it is reused instead of the fallback,
// so a stopped scan does not jump. Everything that places one layer (drawing, scene
// bounds, render keys) passes that layer's hold, so they agree.
[[nodiscard]] inline std::optional<Transform> placeObject(
    const TransformBuffer& tf, const std::string& fixed, const std::string& source,
    std::optional<TimePoint> object_stamp, TimePoint cursor, PlacementHold* hold = nullptr) {
  using pj::tf::Clock;
  using pj::tf::LookupError;
  if (object_stamp.has_value()) {
    std::string root = tf.rootOf(source);
    const auto fixed_from_root = tf.tryLookupTransform(fixed, root, cursor, Clock::kEntry);
    if (!fixed_from_root && fixed_from_root.error() != LookupError::kNoSampleAtTime) {
      return std::nullopt;  // unknown or disconnected frames fail identically at the cursor
    }
    if (fixed_from_root) {
      const auto root_from_source = tf.tryLookupTransform(root, source, *object_stamp, Clock::kHeader);
      if (root_from_source) {
        const auto newest = tf.latestCommonTime(root, source, Clock::kHeader);
        // TimePoint{}: every edge on the chain is single-sample (static), which bounds nothing.
        const bool past_the_tf =
            newest.has_value() && *newest != TimePoint{} && *object_stamp > *newest + kMaxStampPastTf;
        if (!past_the_tf) {
          if (hold != nullptr) {
            *hold = PlacementHold{std::move(root), source, *object_stamp, *root_from_source};
          }
          return *fixed_from_root * *root_from_source;
        }
      } else if (
          hold != nullptr && root_from_source.error() == LookupError::kNoSampleAtTime && hold->stamp == *object_stamp &&
          hold->source == source && hold->root == root) {
        return *fixed_from_root * hold->root_from_source;
      }
    }
  }
  const auto at_cursor = tf.tryLookupTransform(fixed, source, cursor, Clock::kEntry);
  return at_cursor ? std::optional<Transform>(*at_cursor) : std::nullopt;
}

}  // namespace pj::scene3d
