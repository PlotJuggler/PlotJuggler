#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

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

/// The `validate_data_processor_script` slot's function type.
using ValidateScriptSlot = decltype(PJ_data_processors_host_vtable_t::validate_data_processor_script);

/// `check_data_processor_script` answered by a validate: its pass is VALID, its refusal INVALID with its reason. A
/// validate cannot tell a script that does not compile from a call that could not be made, so a caller that can runs
/// its kind and language guards first (the markers and transform bridges); the router uses it as is for a backend
/// without the slot, with the SDK client's own fallback's limitation. A null `ctx` or `out_status` is refused.
inline bool checkByValidate(
    ValidateScriptSlot validate, void* ctx, PJ_string_view_t kind, PJ_string_view_t language, PJ_string_view_t script,
    PJ_string_view_t params_json, uint32_t* out_status, PJ_error_t* out_error) {
  if (ctx == nullptr || out_status == nullptr) {
    return reject(out_error, "data_processors", ctx == nullptr ? "null host context" : "out_status is NULL");
  }
  const bool valid = validate(ctx, kind, language, script, params_json, out_error);
  *out_status = valid ? PJ_SCRIPT_CHECK_VALID : PJ_SCRIPT_CHECK_INVALID;
  return true;
}

/// Count-then-fill return of a string list: `*out_count` gets the full size, `out` the first min(capacity, size)
/// views, which borrow `storage` (keep it alive until the next call on the vtable). Either pointer may be null.
inline void fillViews(
    const std::vector<std::string>& storage, PJ_string_view_t* out, uint64_t capacity, uint64_t* out_count) {
  if (out_count != nullptr) {
    *out_count = storage.size();
  }
  if (out != nullptr) {
    const uint64_t count = std::min<uint64_t>(capacity, storage.size());
    for (uint64_t index = 0; index < count; ++index) {
      out[index] = sdk::toAbiString(storage[index]);
    }
  }
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
