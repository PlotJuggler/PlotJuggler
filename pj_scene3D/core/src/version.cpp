// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/version.h"
namespace pj::scene3d {
// Anchors the kPhase self-assert; kPhase has no consumer (see version.h).
static_assert(kPhase == 1, "Phase mismatch");
}  // namespace pj::scene3d
