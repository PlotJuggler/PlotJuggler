#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

#include "pj_base/expected.hpp"
#include "pj_base/plugin_data_api.h"

namespace PJ {

class PlaybackEngine;
class ServiceRegistryBuilder;

/// Host-side bridge exposing the `pj.playback.v1` SDK service over the session's
/// PlaybackEngine: play/pause/seek/rate, a state snapshot, and absolute-ns ->
/// display-seconds conversion. All vtable slots are `[main-thread]` (the ABI
/// contract), matching the engine's GUI-thread-only discipline — no marshalling,
/// headless-testable.
///
/// The topic->display-time mapping is injected (the per-dataset display offset
/// lives in SessionManager, which this class deliberately does not know), so the
/// bridge stays a pure engine adapter. Not movable: the vtable fat pointer
/// stores `this`.
class PlaybackControlHost {
 public:
  /// Maps (topic, absolute_ns) -> display-axis seconds using the display offset
  /// of the dataset owning `topic`; empty topic = the host's representative
  /// dataset. Returns nullopt for an unknown or ambiguous topic. [main-thread]
  using TimeMapper = std::function<std::optional<double>(std::string_view topic, int64_t absolute_ns)>;
  /// Select a live catalog source directly; invalid/unloaded sources return nullopt.
  using SourceTimeMapper = std::function<std::optional<double>(PJ_data_source_handle_t source, int64_t absolute_ns)>;

  /// @param engine  the session's playback engine (must outlive this).
  /// @param mapper  topic-scoped absolute->display conversion (may be empty:
  ///                to_display_time then reports "not supported").
  PlaybackControlHost(PlaybackEngine& engine, TimeMapper mapper, SourceTimeMapper source_mapper = {});

  PlaybackControlHost(const PlaybackControlHost&) = delete;
  PlaybackControlHost& operator=(const PlaybackControlHost&) = delete;
  PlaybackControlHost(PlaybackControlHost&&) = delete;
  PlaybackControlHost& operator=(PlaybackControlHost&&) = delete;

  /// Register the `pj.playback.v1` service into the plugin's registry.
  [[nodiscard]] Status registerServices(ServiceRegistryBuilder& registry);

  /// The raw C-ABI fat pointer (for direct wiring / tests).
  [[nodiscard]] PJ_playback_host_t raw() const noexcept {
    return raw_;
  }

 private:
  static bool onPlay(void* ctx, PJ_error_t* out_error) noexcept;
  static bool onPause(void* ctx, PJ_error_t* out_error) noexcept;
  static bool onSeek(void* ctx, double time_s, PJ_error_t* out_error) noexcept;
  static bool onSetRate(void* ctx, double rate, PJ_error_t* out_error) noexcept;
  static bool onGetState(void* ctx, PJ_playback_state_t* out_state, PJ_error_t* out_error) noexcept;
  static bool onToDisplayTime(
      void* ctx, PJ_string_view_t topic, int64_t absolute_ns, double* out_display_s, PJ_error_t* out_error) noexcept;
  static bool onToDisplayTimeForSource(
      void* ctx, PJ_data_source_handle_t source, int64_t absolute_ns, double* out_display_s,
      PJ_error_t* out_error) noexcept;

  PlaybackEngine& engine_;
  TimeMapper mapper_;
  SourceTimeMapper source_mapper_;
  PJ_playback_host_vtable_t vtable_;
  PJ_playback_host_t raw_;
};

}  // namespace PJ
