#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"

namespace PJ {

// Every flag bit this host understands. The ABI contract (plugin_data_api.h's
// `flags` paragraph) requires a host to reject an unknown/reserved bit rather
// than silently drop it — silently ignoring one is indistinguishable, from the
// plugin's side, from the host applying it and the effect just not taking.
constexpr uint32_t kKnownFlags = PJ_DATA_PROCESSOR_FLAG_EPHEMERAL | PJ_DATA_PROCESSOR_FLAG_HISTORY_EXEMPT;

/// The output names of a decoded typed create for a kind whose outputs are untyped and
/// whose inputs resolve by name (transform and markers, until the catalog v2 work):
/// the views borrow `request`. Refuses, naming `kind`, a typed output or a dataset_id.
[[nodiscard]] inline Expected<std::vector<PJ_string_view_t>> untypedOutputNames(
    const PJ_data_processor_request_t& request, std::string_view kind) {
  if (request.dataset_id != 0) {
    return unexpected("kind '" + std::string(kind) + "' takes no dataset_id yet: qualify the inputs with '<source>:'");
  }
  std::vector<PJ_string_view_t> names;
  names.reserve(request.output_count);
  for (uint64_t index = 0; index < request.output_count; ++index) {
    const PJ_data_processor_output_t& output = request.outputs[index];
    if (output.type.size != 0) {
      return unexpected(
          "kind '" + std::string(kind) + "' takes untyped outputs: '" + std::string(sdk::toStringView(output.name)) +
          "' has type '" + std::string(sdk::toStringView(output.type)) + "'");
    }
    names.push_back(output.name);
  }
  return names;
}

}  // namespace PJ
