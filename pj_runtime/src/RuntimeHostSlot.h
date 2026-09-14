#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <exception>
#include <string_view>

#include "pj_base/expected.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"

namespace PJ::host_slot {

/// Fill `out_error` as a refused call (`PJ_ERROR_CODE_REJECTED`) and return false —
/// the one-line form of "bad argument / unsupported / the callback said no".
inline bool reject(PJ_error_t* out_error, std::string_view domain, std::string_view message) {
  sdk::fillError(out_error, PJ_ERROR_CODE_REJECTED, domain, message);
  return false;
}

/// A slot that ends with a Status: success is true, failure is `reject` with its reason.
inline bool finish(PJ_error_t* out_error, std::string_view domain, const Status& status) {
  return status.has_value() ? true : reject(out_error, domain, status.error());
}

/// Run one C-ABI slot body with the skeleton every bridge shares: a null `ctx` is
/// refused (`out_error` still says why — it does not depend on `ctx`), and an
/// exception escaping `body` becomes `PJ_ERROR_CODE_INTERNAL` instead of crossing
/// the noexcept boundary. `body(Self&)` returns the slot's own bool result.
template <typename Self, typename Body>
bool guarded(void* ctx, PJ_error_t* out_error, std::string_view domain, Body&& body) noexcept {
  auto* self = static_cast<Self*>(ctx);
  if (self == nullptr) {
    return reject(out_error, domain, "null host context");
  }
  try {
    return body(*self);
  } catch (const std::exception& error) {
    sdk::fillError(out_error, PJ_ERROR_CODE_INTERNAL, domain, error.what());
    return false;
  } catch (...) {
    sdk::fillError(out_error, PJ_ERROR_CODE_INTERNAL, domain, "unknown error");
    return false;
  }
}

}  // namespace PJ::host_slot
