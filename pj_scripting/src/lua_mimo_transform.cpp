// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scripting/lua_mimo_transform.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "pj_datastore/processor_detail.hpp"  // proc::detail::numberOrText / mirrorInputKind

namespace PJ::scripting {
namespace {
// Finite-guarded, range-clamped double → int64 (llround of NaN/out-of-range is UB).
std::int64_t toInt64(double d) {
  if (!std::isfinite(d)) {
    return 0;
  }
  if (d >= 9.0e18) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (d <= -9.0e18) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return static_cast<std::int64_t>(std::llround(d));
}

// Finite-guarded double → uint64. Negative/non-finite → 0; high band truncates to
// avoid llround-overflow UB near 2^63 (values there already past double precision).
std::uint64_t toUint64(double d) {
  if (!std::isfinite(d) || d <= 0.0) {
    return 0;
  }
  if (d >= 1.8446744073709552e19) {  // ~2^64
    return std::numeric_limits<std::uint64_t>::max();
  }
  if (d < 9.0e18) {
    return static_cast<std::uint64_t>(std::llround(d));
  }
  return static_cast<std::uint64_t>(d);
}
}  // namespace

LuaMimoTransform::LuaMimoTransform(
    std::shared_ptr<ScriptEngine> engine, FilterClass klass, std::string params_json, std::size_t num_outputs)
    : engine_(std::move(engine)),
      klass_(std::move(klass)),
      params_json_(std::move(params_json)),
      num_outputs_(num_outputs) {
  if (klass_.output_kind == "int64") {
    out_mode_ = OutputMode::kInt64;
  } else if (klass_.output_kind == "same" || klass_.output_kind == "mirror") {
    out_mode_ = OutputMode::kMirror;
  } else if (klass_.textOutput()) {
    out_mode_ = OutputMode::kString;
  } else {
    out_mode_ = OutputMode::kDouble;
  }
  rebuildInstance();
}

PJ::VarValue LuaMimoTransform::makeOutputValue(double computed, const PJ::VarValue& mirror_input) const {
  switch (out_mode_) {
    case OutputMode::kInt64:
      return PJ::VarValue{toInt64(computed)};
    case OutputMode::kMirror:
      return std::visit(
          [computed](auto&& arg) -> PJ::VarValue {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::int64_t>) {
              return PJ::VarValue{toInt64(computed)};
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
              return PJ::VarValue{toUint64(computed)};
            } else {
              return PJ::VarValue{computed};  // double or string-input → double output
            }
          },
          mirror_input);
    case OutputMode::kDouble:
    default:
      return PJ::VarValue{computed};
  }
}

void LuaMimoTransform::rebuildInstance() {
  has_session_start_ = false;
  instance_.reset();
  error_.clear();
  if (!engine_) {
    error_ = "no script engine";
    return;
  }
  auto made = engine_->createInstance(klass_, params_json_);
  if (!made.has_value()) {
    error_ = made.error();
    return;
  }
  instance_ = std::move(made).value();
}

void LuaMimoTransform::reset() {
  // A fresh VM is the only guaranteed-clean reset of arbitrary script state; the engine calls this before a batch
  // recompute.
  rebuildInstance();
}

std::vector<PJ::StorageKind> LuaMimoTransform::outputKinds(PJ::Span<const PJ::StorageKind> input_kinds) const {
  switch (out_mode_) {
    case OutputMode::kInt64:
      return std::vector<PJ::StorageKind>(num_outputs_, PJ::StorageKind::kInt64);
    case OutputMode::kMirror: {
      // One `output` mode for the whole class: every output mirrors input 0's kind.
      PJ::StorageKind mirrored = PJ::StorageKind::kFloat64;
      if (!input_kinds.empty()) {
        const std::vector<PJ::StorageKind> mapped = proc::detail::mirrorInputKind(input_kinds.subspan(0, 1));
        if (!mapped.empty()) {
          mirrored = mapped.front();
        }
      }
      return std::vector<PJ::StorageKind>(num_outputs_, mirrored);
    }
    case OutputMode::kString:
      return std::vector<PJ::StorageKind>(num_outputs_, PJ::StorageKind::kString);
    case OutputMode::kDouble:
    default:
      return std::vector<PJ::StorageKind>(num_outputs_, PJ::StorageKind::kFloat64);
  }
}

bool LuaMimoTransform::calculate(
    PJ::Timestamp time, PJ::Span<const PJ::VarValue> inputs, PJ::Timestamp& out_time,
    std::vector<PJ::VarValue>& output) {
  out_time = time;  // the outputs share the joined input timestamp
  if (!instance_ || inputs.empty()) {
    if (instance_) {
      error_ = instance_->error();
    }
    return false;
  }
  if (instance_->failed()) {
    error_ = instance_->error();
    return false;
  }

  // The time origin is the first sample; subtract it on the int64 spine before the double cast (see the header).
  if (!has_session_start_) {
    session_start_ns_ = time;
    has_session_start_ = true;
  }
  const double t_sec = static_cast<double>(time - session_start_ns_) * 1e-9;

  script_inputs_.clear();
  for (const PJ::VarValue& v : inputs) {
    script_inputs_.push_back(proc::detail::numberOrText(v));
  }

  FilterInstance::MimoResult r = instance_->calculateMimo(t_sec, script_inputs_);
  if (instance_->failed()) {
    error_ = instance_->error();
    return false;
  }
  if (r.suppress) {
    return false;
  }
  const std::size_t returned = out_mode_ == OutputMode::kString ? r.texts.size() : r.values.size();
  if (returned != num_outputs_) {
    error_ = "filter returned " + std::to_string(returned) + " value(s) but " + std::to_string(num_outputs_) +
             " output(s) are declared";
    return false;
  }
  if (out_mode_ == OutputMode::kString) {
    for (std::size_t k = 0; k < num_outputs_; ++k) {
      output[k] = PJ::VarValue{std::move(r.texts[k])};
    }
    return true;
  }

  // `output` is pre-sized to num_outputs_ by the engine; in mirror mode every output follows input 0.
  const PJ::VarValue& mirror_input = inputs[0];
  for (std::size_t k = 0; k < num_outputs_; ++k) {
    output[k] = makeOutputValue(r.values[k], mirror_input);
  }
  return true;
}

}  // namespace PJ::scripting
