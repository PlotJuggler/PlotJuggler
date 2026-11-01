#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

#include "pj_base/expected.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host/widget_host_bridge.hpp"
#include "pj_runtime/WireTarget.h"

namespace PJ {

class DataReader;
class ServiceRegistryBuilder;

/// Host side of the `pj.widget_host.v1` SDK service for ONE open Widget panel.
///
/// A widget never sees topics, datasets or the data store: the dock owns the wiring (which series
/// feeds which input slot) and tells this host through a WireResolver. This class turns a plugin's
/// `query_history(input, slot, from, to, max)` into a read of that wire and hands each sample to the
/// plugin's sink, in ascending time, converted to `PJ_widget_value_t` on the display time axis, and
/// reports how many samples the range held before the cap (`out_total`).
/// `inputs_changed` and `report_message` are forwarded to the callbacks given at construction.
///
/// The C-ABI skeleton (null context, exceptions at the noexcept boundary) is WidgetHostBridge's; this
/// class only supplies the three behaviours. Every call is main-thread, like the rest of the widget
/// contract. Not movable: the registered fat pointer refers to this object, and it must outlive the
/// WidgetHandle bound to it.
class WidgetRuntimeHost {
 public:
  /// A connected wire: where it reads from, and the display name of its series ("topic/field") that
  /// every value's `label` refers to. `label` must stay valid for the history call that resolved it.
  struct ResolvedWire {
    WireTarget target;
    std::string_view label;
  };

  /// Resolved on every history call, so the dock can rewire without telling the host. nullopt = the
  /// wire is not connected (unknown input/slot, or the series did not resolve yet).
  using WireResolver = std::function<std::optional<ResolvedWire>(uint32_t input, uint32_t slot)>;

  struct Callbacks {
    /// The widget's inputs() changed; the dock re-reads inputs() and reconciles by name. Never
    /// delivered from inside on_update by contract, but the dock still defers it.
    std::function<void()> inputs_changed;
    /// A diagnostic from the widget, for the host's message list.
    std::function<void(WidgetMessageLevel level, std::string_view message)> report_message;
  };

  /// Samples delivered when the plugin passes `max_samples == 0`.
  static constexpr uint32_t kDefaultMaxSamples = 10'000;
  /// Hard cap on one call whatever the plugin asks for, so a widget cannot pull a whole dataset.
  static constexpr uint32_t kMaxSamplesLimit = 100'000;

  /// @param reader   scalar data (must outlive this host).
  /// @param objects  object topics (must outlive this host).
  /// @param resolver wire lookup, called on the main thread inside each history call.
  WidgetRuntimeHost(
      const DataReader& reader, const ObjectStore& objects, WireResolver resolver, Callbacks callbacks = {});

  WidgetRuntimeHost(const WidgetRuntimeHost&) = delete;
  WidgetRuntimeHost& operator=(const WidgetRuntimeHost&) = delete;
  WidgetRuntimeHost(WidgetRuntimeHost&&) = delete;
  WidgetRuntimeHost& operator=(WidgetRuntimeHost&&) = delete;

  /// Register `pj.widget_host.v1` into the registry passed to `WidgetHandle::bind`.
  [[nodiscard]] Status registerServices(ServiceRegistryBuilder& registry);

  /// The raw fat pointer (direct wiring and tests).
  [[nodiscard]] PJ_widget_host_t raw() {
    return bridge_.host();
  }

 private:
  /// The samples of the range before decimation (the ABI's `out_total`), or an error before the sink ran.
  /// A sink returning false ends the delivery; the total is still the range's.
  Expected<uint32_t> history(const WidgetHistoryQuery& query, const WidgetValueSink& sink) const;
  Expected<uint32_t> historyNumber(
      const WidgetHistoryQuery& query, const ResolvedWire& wire, const WidgetValueSink& sink) const;
  Expected<uint32_t> historyText(
      const WidgetHistoryQuery& query, const ResolvedWire& wire, const WidgetValueSink& sink) const;
  Expected<uint32_t> historyObject(
      const WidgetHistoryQuery& query, const ResolvedWire& wire, const WidgetValueSink& sink) const;

  const DataReader& reader_;
  const ObjectStore& objects_;
  WireResolver resolver_;
  WidgetHostBridge bridge_;
};

}  // namespace PJ
