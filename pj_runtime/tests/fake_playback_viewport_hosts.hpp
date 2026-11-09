// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstdint>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>

namespace PJ::testing {

// Fake pj.playback.v1 host: records the last call, serves a settable state,
// and converts absolute ns -> display seconds with a fixed offset so the
// read_series enrichment is observable.
struct FakePlaybackHost {
  bool play_called = false;
  bool pause_called = false;
  double last_seek_s = -1.0;
  double last_rate = -1.0;
  PJ_playback_state_t state{
      .is_playing = false, .current_time_s = 3.0, .range_min_s = 0.0, .range_max_s = 10.0, .playback_rate = 1.0};
  bool fail_state = false;
  std::string last_topic;
  std::int64_t display_offset_ns = 0;

  static bool tPlay(void* ctx, PJ_error_t*) noexcept {
    static_cast<FakePlaybackHost*>(ctx)->play_called = true;
    return true;
  }
  static bool tPause(void* ctx, PJ_error_t*) noexcept {
    static_cast<FakePlaybackHost*>(ctx)->pause_called = true;
    return true;
  }
  static bool tSeek(void* ctx, double time_s, PJ_error_t*) noexcept {
    auto* self = static_cast<FakePlaybackHost*>(ctx);
    self->last_seek_s = time_s;
    // Mirror the real host: clamp into range and reflect it in the state the
    // subsequent get_state echo reads.
    self->state.current_time_s = std::clamp(time_s, self->state.range_min_s, self->state.range_max_s);
    return true;
  }
  static bool tSetRate(void* ctx, double rate, PJ_error_t*) noexcept {
    static_cast<FakePlaybackHost*>(ctx)->last_rate = rate;
    return true;
  }
  static bool tGetState(void* ctx, PJ_playback_state_t* out_state, PJ_error_t* err) noexcept {
    auto* self = static_cast<FakePlaybackHost*>(ctx);
    if (self->fail_state) {
      PJ::sdk::fillError(err, 2, "fake", "state boom");
      return false;
    }
    *out_state = self->state;
    return true;
  }
  static bool tToDisplayTime(
      void* ctx, PJ_string_view_t topic, std::int64_t absolute_ns, double* out_display_s, PJ_error_t*) noexcept {
    auto* self = static_cast<FakePlaybackHost*>(ctx);
    self->last_topic = std::string(PJ::sdk::toStringView(topic));
    *out_display_s = static_cast<double>(absolute_ns - self->display_offset_ns) * 1e-9;
    return true;
  }
  // Source-scoped conversion, same fixed offset as tToDisplayTime -- there is
  // no per-source concept here, just enough to observe the OFFSET a caller
  // (e.g. evaluate's object path) derives from one forward conversion.
  static bool tToDisplayTimeForSource(
      void* ctx, PJ_data_source_handle_t source, std::int64_t absolute_ns, double* out_display_s,
      PJ_error_t*) noexcept {
    auto* self = static_cast<FakePlaybackHost*>(ctx);
    self->last_source_id = source.id;
    *out_display_s = static_cast<double>(absolute_ns - self->display_offset_ns) * 1e-9;
    return true;
  }

  std::uint32_t last_source_id = 0;

  PJ::sdk::PlaybackHostView view() {
    static const PJ_playback_host_vtable_t vtable = [] {
      PJ_playback_host_vtable_t v{};
      v.protocol_version = 1;
      v.struct_size = sizeof(PJ_playback_host_vtable_t);
      v.play = &FakePlaybackHost::tPlay;
      v.pause = &FakePlaybackHost::tPause;
      v.seek = &FakePlaybackHost::tSeek;
      v.set_playback_rate = &FakePlaybackHost::tSetRate;
      v.get_state = &FakePlaybackHost::tGetState;
      v.to_display_time = &FakePlaybackHost::tToDisplayTime;
      v.to_display_time_for_source = &FakePlaybackHost::tToDisplayTimeForSource;
      return v;
    }();
    return PJ::sdk::PlaybackHostView(PJ_playback_host_t{this, &vtable});
  }
};

// Fake pj.viewport.v1 host: records the last zoom call.
struct FakeViewportHost {
  int zoom_calls = 0;
  double last_t0_s = 0.0;
  double last_t1_s = 0.0;
  bool reset_called = false;

  static bool tZoom(void* ctx, double t0_s, double t1_s, PJ_error_t*) noexcept {
    auto* self = static_cast<FakeViewportHost*>(ctx);
    ++self->zoom_calls;
    self->last_t0_s = t0_s;
    self->last_t1_s = t1_s;
    return true;
  }
  static bool tReset(void* ctx, PJ_error_t*) noexcept {
    static_cast<FakeViewportHost*>(ctx)->reset_called = true;
    return true;
  }

  PJ::sdk::ViewportHostView view() {
    static const PJ_viewport_host_vtable_t vtable = [] {
      PJ_viewport_host_vtable_t v{};
      v.protocol_version = 1;
      v.struct_size = sizeof(PJ_viewport_host_vtable_t);
      v.zoom_to_time_range = &FakeViewportHost::tZoom;
      v.zoom_reset = &FakeViewportHost::tReset;
      return v;
    }();
    return PJ::sdk::ViewportHostView(PJ_viewport_host_t{this, &vtable});
  }
};

}  // namespace PJ::testing
