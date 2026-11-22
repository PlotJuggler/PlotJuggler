#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <atomic>

#include "pj_tf/tf_buffer.h"

namespace pj::scene3d {

// How every 3D view reads a TF edge between two of its samples: the "Interpolate transforms" preference. Scripts
// never read it: their lookups hold, so a result does not depend on a view preference.
// ponytail: process-wide, because the preference is; make it a view property if a per-dock choice is ever wanted.
inline std::atomic<pj::tf::Sampling> g_view_sampling{pj::tf::Sampling::kInterpolate};

[[nodiscard]] inline pj::tf::Sampling viewSampling() {
  return g_view_sampling.load(std::memory_order_relaxed);
}

inline void setViewSampling(pj::tf::Sampling sampling) {
  g_view_sampling.store(sampling, std::memory_order_relaxed);
}

}  // namespace pj::scene3d
