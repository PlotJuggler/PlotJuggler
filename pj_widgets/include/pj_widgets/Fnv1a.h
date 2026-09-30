#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <bit>
#include <cstdint>
#include <type_traits>

namespace PJ {

/// 64-bit FNV-1a: cheap, stable, non-cryptographic. Used for in-process
/// identities (render keys, cache keys, change signatures) and for the palette
/// index of StateColors.h, whose pinned assignments depend on these exact values.
inline constexpr std::uint64_t kFnv1aOffsetBasis = 0xcbf29ce484222325ULL;

/// One FNV-1a step that folds `value` in as a single word, not byte by byte:
/// the word-wise variant the render keys and fingerprints over numbers use.
[[nodiscard]] constexpr std::uint64_t fnv1aMix(std::uint64_t hash, std::uint64_t value) noexcept {
  return (hash ^ value) * 0x100000001b3ULL;
}

/// fnv1aMix() over the bit pattern of a float (32 bits) or double (64 bits).
template <typename Float>
  requires std::is_floating_point_v<Float> && (sizeof(Float) == 4 || sizeof(Float) == 8)
[[nodiscard]] constexpr std::uint64_t fnv1aMixBits(std::uint64_t hash, Float value) noexcept {
  if constexpr (sizeof(Float) == 4) {
    return fnv1aMix(hash, std::bit_cast<std::uint32_t>(value));
  } else {
    return fnv1aMix(hash, std::bit_cast<std::uint64_t>(value));
  }
}

/// The standard byte-wise FNV-1a of `bytes`, any range of char or 8-bit
/// elements (std::string_view, std::vector<std::uint8_t>, std::span, ...).
template <typename Bytes>
[[nodiscard]] constexpr std::uint64_t fnv1a(const Bytes& bytes) noexcept {
  std::uint64_t hash = kFnv1aOffsetBasis;
  for (const auto byte : bytes) {
    static_assert(sizeof(byte) == 1, "fnv1a() hashes bytes");
    hash = fnv1aMix(hash, static_cast<std::uint8_t>(byte));
  }
  return hash;
}

}  // namespace PJ
