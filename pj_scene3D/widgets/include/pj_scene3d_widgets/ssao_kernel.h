#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <random>
#include <vector>

namespace pj::scene3d {

// Deterministic SSAO hemisphere kernel, so every backend samples identical
// occlusion patterns.
// Denser near the origin (LearnOpenGL lerp-scale); fixed seed — the kernel is
// a fixed quality knob, not entropy.
inline std::vector<glm::vec3> ssaoHemisphereKernel(int size) {
  std::mt19937 rng(0x55A0U);
  std::uniform_real_distribution<float> uniform(0.0F, 1.0F);
  std::vector<glm::vec3> kernel;
  kernel.reserve(static_cast<std::size_t>(size));
  for (int index = 0; index < size; ++index) {
    glm::vec3 sample(uniform(rng) * 2.0F - 1.0F, uniform(rng) * 2.0F - 1.0F, uniform(rng));
    sample = glm::normalize(sample) * uniform(rng);
    const float position = static_cast<float>(index) / static_cast<float>(size);
    sample *= 0.1F + 0.9F * position * position;
    kernel.push_back(sample);
  }
  return kernel;
}

}  // namespace pj::scene3d
