// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/PlaybackControlHost.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

#include "RuntimeHostSlot.h"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/ServiceRegistration.h"
#include "pj_runtime/Time.h"

namespace PJ {

namespace {
using host_slot::guarded;
using host_slot::reject;
// Rejected: bad argument / unknown topic. Internal: an exception at the boundary.
constexpr std::string_view kDomain{"playback"};
}  // namespace

PlaybackControlHost::PlaybackControlHost(PlaybackEngine& engine, TimeMapper mapper, SourceTimeMapper source_mapper)
    : engine_(engine),
      mapper_(std::move(mapper)),
      source_mapper_(std::move(source_mapper)),
      vtable_{
          .protocol_version = 1,
          .struct_size = sizeof(PJ_playback_host_vtable_t),
          .play = &PlaybackControlHost::onPlay,
          .pause = &PlaybackControlHost::onPause,
          .seek = &PlaybackControlHost::onSeek,
          .set_playback_rate = &PlaybackControlHost::onSetRate,
          .get_state = &PlaybackControlHost::onGetState,
          .to_display_time = &PlaybackControlHost::onToDisplayTime,
          .to_display_time_for_source = &PlaybackControlHost::onToDisplayTimeForSource,
      },
      raw_{this, &vtable_} {}

Status PlaybackControlHost::registerServices(ServiceRegistryBuilder& registry) {
  return registerRequiredService<sdk::PlaybackHostService>(registry, raw_);
}

bool PlaybackControlHost::onPlay(void* ctx, PJ_error_t* out_error) noexcept {
  return guarded<PlaybackControlHost>(ctx, out_error, kDomain, [](PlaybackControlHost& self) {
    self.engine_.play();
    return true;
  });
}

bool PlaybackControlHost::onPause(void* ctx, PJ_error_t* out_error) noexcept {
  return guarded<PlaybackControlHost>(ctx, out_error, kDomain, [](PlaybackControlHost& self) {
    self.engine_.pause();
    return true;
  });
}

bool PlaybackControlHost::onSeek(void* ctx, double time_s, PJ_error_t* out_error) noexcept {
  return guarded<PlaybackControlHost>(ctx, out_error, kDomain, [&](PlaybackControlHost& self) {
    if (!std::isfinite(time_s)) {
      return reject(out_error, kDomain, "seek time must be finite");
    }
    // The engine clamps into [rangeMin, rangeMax]; callers read the clamped
    // cursor back via get_state.
    self.engine_.setCurrentTime(displaySeconds(time_s));
    return true;
  });
}

bool PlaybackControlHost::onSetRate(void* ctx, double rate, PJ_error_t* out_error) noexcept {
  return guarded<PlaybackControlHost>(ctx, out_error, kDomain, [&](PlaybackControlHost& self) {
    if (!std::isfinite(rate) || rate <= 0.0) {
      return reject(out_error, kDomain, "playback rate must be a finite value > 0");
    }
    // Host policy bound (the ABI says "the host may clamp"): keep any consumer
    // from driving the engine at absurd multipliers. Consumers see the applied
    // value via get_state.
    self.engine_.setPlaybackRate(std::clamp(rate, 0.001, 1000.0));
    return true;
  });
}

bool PlaybackControlHost::onGetState(void* ctx, PJ_playback_state_t* out_state, PJ_error_t* out_error) noexcept {
  return guarded<PlaybackControlHost>(ctx, out_error, kDomain, [&](PlaybackControlHost& self) {
    if (out_state == nullptr) {
      return reject(out_error, kDomain, "get_state requires a non-null out_state");
    }
    *out_state = PJ_playback_state_t{
        .is_playing = self.engine_.isPlaying(),
        .current_time_s = toAxisDouble(self.engine_.currentTime()),
        .range_min_s = toAxisDouble(self.engine_.rangeMin()),
        .range_max_s = toAxisDouble(self.engine_.rangeMax()),
        .playback_rate = self.engine_.playbackRate(),
    };
    return true;
  });
}

bool PlaybackControlHost::onToDisplayTime(
    void* ctx, PJ_string_view_t topic, int64_t absolute_ns, double* out_display_s, PJ_error_t* out_error) noexcept {
  return guarded<PlaybackControlHost>(ctx, out_error, kDomain, [&](PlaybackControlHost& self) {
    if (out_display_s == nullptr) {
      return reject(out_error, kDomain, "to_display_time requires a non-null out_display_s");
    }
    if (!self.mapper_) {
      return reject(out_error, kDomain, "this host does not support display-time conversion");
    }
    const auto topic_view = sdk::toStringView(topic);
    const auto display_s = self.mapper_(topic_view, absolute_ns);
    if (!display_s.has_value()) {
      return reject(out_error, kDomain, "unknown or ambiguous topic '" + std::string(topic_view) + "'");
    }
    *out_display_s = *display_s;
    return true;
  });
}

bool PlaybackControlHost::onToDisplayTimeForSource(
    void* ctx, PJ_data_source_handle_t source, int64_t absolute_ns, double* out_display_s,
    PJ_error_t* out_error) noexcept {
  return guarded<PlaybackControlHost>(ctx, out_error, kDomain, [&](PlaybackControlHost& self) {
    if (out_display_s == nullptr || source.id == 0) {
      return reject(out_error, kDomain, "source conversion requires a valid source and output pointer");
    }
    if (!self.source_mapper_) {
      return reject(out_error, kDomain, "this host does not support source display-time conversion");
    }
    const auto display_s = self.source_mapper_(source, absolute_ns);
    if (!display_s.has_value()) {
      return reject(out_error, kDomain, "unknown or unloaded source " + std::to_string(source.id));
    }
    *out_display_s = *display_s;
    return true;
  });
}

}  // namespace PJ
