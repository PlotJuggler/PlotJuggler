#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QString>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "pj_widgets/Fnv1a.h"

namespace PJ {

/// Plain 8-bit RGB triple so the palette + hash stay usable without QtGui
/// (mirrors Colormap.h's ColormapRgb convention).
struct StateRgb {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
};

/// Categorical palette for state-transition bands. Hand-picked, colorblind-aware
/// (Okabe-Ito plus two extensions), medium saturation so white/black label text
/// stays legible on both themes. Order matters: stateColorIndex() indexes into
/// it, so reordering silently recolors every saved screenshot — append, don't
/// shuffle.
inline constexpr std::array<StateRgb, 10> kStatePalette = {{
    {.r = 0x00, .g = 0x72, .b = 0xB2},  // blue
    {.r = 0xD5, .g = 0x5E, .b = 0x00},  // vermillion
    {.r = 0x00, .g = 0x9E, .b = 0x73},  // bluish green
    {.r = 0xE6, .g = 0x9F, .b = 0x00},  // orange
    {.r = 0x56, .g = 0xB4, .b = 0xE9},  // sky blue
    {.r = 0xCC, .g = 0x79, .b = 0xA7},  // reddish purple
    {.r = 0xF0, .g = 0xE4, .b = 0x42},  // yellow
    {.r = 0x99, .g = 0x99, .b = 0x99},  // grey
    {.r = 0x17, .g = 0xBE, .b = 0xCF},  // teal
    {.r = 0xBC, .g = 0xBD, .b = 0x22},  // olive
}};

/// Stable palette index for a state value: FNV-1a over the UTF-8 bytes, modulo
/// the palette size. Deterministic across sessions/platforms — the same string
/// maps to the same color everywhere in the app, with zero configuration.
/// Distinct values CAN collide (10 buckets); that is accepted for v1.
[[nodiscard]] constexpr std::size_t stateColorIndex(std::string_view value) noexcept {
  return static_cast<std::size_t>(fnv1a(value) % kStatePalette.size());
}

/// The palette color for a state value (Qt-free form).
[[nodiscard]] constexpr StateRgb stateRgb(std::string_view value) noexcept {
  return kStatePalette[stateColorIndex(value)];
}

/// The palette color for a state value as a QColor. Hashes the UTF-8 encoding,
/// so it agrees with stateRgb()/stateColorIndex() on any input.
[[nodiscard]] inline QColor stateColor(const QString& value) {
  const QByteArray utf8 = value.toUtf8();
  const StateRgb rgb = stateRgb(std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
  return {rgb.r, rgb.g, rgb.b};
}

}  // namespace PJ
