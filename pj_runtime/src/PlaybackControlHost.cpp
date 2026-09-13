// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/PlaybackControlHost.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>
#include <utility>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/ServiceRegistration.h"
#include "pj_runtime/Time.h"

namespace PJ {

namespace {
constexpr const char* kDomain = "playback";
constexpr int32_t kErrorRejected = PJ_ERROR_CODE_REJECTED;  // bad argument / unknown topic
constexpr int32_t kErrorInternal = PJ_ERROR_CODE_INTERNAL;  // unexpected host-side exception at the boundary
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
  auto* self = static_cast<PlaybackControlHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    self->engine_.play();
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  }
}

bool PlaybackControlHost::onPause(void* ctx, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<PlaybackControlHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    self->engine_.pause();
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  }
}

bool PlaybackControlHost::onSeek(void* ctx, double time_s, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<PlaybackControlHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    if (!std::isfinite(time_s)) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "seek time must be finite");
      return false;
    }
    // The engine clamps into [rangeMin, rangeMax]; callers read the clamped
    // cursor back via get_state.
    self->engine_.setCurrentTime(displaySeconds(time_s));
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  }
}

bool PlaybackControlHost::onSetRate(void* ctx, double rate, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<PlaybackControlHost*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    if (!std::isfinite(rate) || rate <= 0.0) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "playback rate must be a finite value > 0");
      return false;
    }
    // Host policy bound (the ABI says "the host may clamp"): keep any consumer
    // from driving the engine at absurd multipliers. Consumers see the applied
    // value via get_state.
    self->engine_.setPlaybackRate(std::clamp(rate, 0.001, 1000.0));
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  }
}

bool PlaybackControlHost::onGetState(void* ctx, PJ_playback_state_t* out_state, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<PlaybackControlHost*>(ctx);
  if (self == nullptr || out_state == nullptr) {
    sdk::fillError(out_error, kErrorRejected, kDomain, "get_state requires a non-null out_state");
    return false;
  }
  try {
    *out_state = PJ_playback_state_t{
        .is_playing = self->engine_.isPlaying(),
        .current_time_s = toAxisDouble(self->engine_.currentTime()),
        .range_min_s = toAxisDouble(self->engine_.rangeMin()),
        .range_max_s = toAxisDouble(self->engine_.rangeMax()),
        .playback_rate = self->engine_.playbackRate(),
    };
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  }
}

bool PlaybackControlHost::onToDisplayTime(
    void* ctx, PJ_string_view_t topic, int64_t absolute_ns, double* out_display_s, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<PlaybackControlHost*>(ctx);
  if (self == nullptr || out_display_s == nullptr) {
    sdk::fillError(out_error, kErrorRejected, kDomain, "to_display_time requires a non-null out_display_s");
    return false;
  }
  try {
    if (!self->mapper_) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "this host does not support display-time conversion");
      return false;
    }
    const auto topic_view = sdk::toStringView(topic);
    const auto display_s = self->mapper_(topic_view, absolute_ns);
    if (!display_s.has_value()) {
      sdk::fillError(
          out_error, kErrorRejected, kDomain, "unknown or ambiguous topic '" + std::string(topic_view) + "'");
      return false;
    }
    *out_display_s = *display_s;
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  }
}

bool PlaybackControlHost::onToDisplayTimeForSource(
    void* ctx, PJ_data_source_handle_t source, int64_t absolute_ns, double* out_display_s,
    PJ_error_t* out_error) noexcept {
  auto* self = static_cast<PlaybackControlHost*>(ctx);
  if (self == nullptr || out_display_s == nullptr || source.id == 0) {
    sdk::fillError(out_error, kErrorRejected, kDomain, "source conversion requires a valid source and output pointer");
    return false;
  }
  try {
    if (!self->source_mapper_) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "this host does not support source display-time conversion");
      return false;
    }
    const auto display_s = self->source_mapper_(source, absolute_ns);
    if (!display_s.has_value()) {
      sdk::fillError(out_error, kErrorRejected, kDomain, "unknown or unloaded source " + std::to_string(source.id));
      return false;
    }
    *out_display_s = *display_s;
    return true;
  } catch (const std::exception& e) {
    sdk::fillError(out_error, kErrorInternal, kDomain, e.what());
    return false;
  }
}

}  // namespace PJ
