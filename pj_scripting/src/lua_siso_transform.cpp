// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scripting/lua_siso_transform.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

#include "pj_datastore/processor_detail.hpp"  // proc::detail::toDouble / mirrorInputKind

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

// Finite-guarded double -> uint64: negative or non-finite -> 0, >= ~2^64 -> UINT64_MAX. The high band truncates
// because llround overflows near 2^63 (such values are already past a double's 53-bit integers).
std::uint64_t toUint64(double d) {
  if (!std::isfinite(d) || d <= 0.0) {
    return 0;
  }
  if (d >= 1.8446744073709552e19) {  // ~2^64
    return std::numeric_limits<std::uint64_t>::max();
  }
  if (d < 9.0e18) {
    return static_cast<std::uint64_t>(std::llround(d));  // small: correct rounding
  }
  return static_cast<std::uint64_t>(d);  // high band: truncate
}
}  // namespace

LuaSisoTransform::LuaSisoTransform(std::shared_ptr<ScriptEngine> engine, FilterClass klass, std::string params_json)
    : engine_(std::move(engine)), klass_(std::move(klass)), params_json_(std::move(params_json)) {
  bracket_label_ = klass_.name.empty() ? klass_.id : klass_.name;
  if (klass_.output_kind == "int64") {
    out_mode_ = OutputMode::kInt64;
  } else if (klass_.output_kind == "same" || klass_.output_kind == "mirror") {
    out_mode_ = OutputMode::kMirror;
  } else if (klass_.output_kind == "string") {
    out_mode_ = OutputMode::kString;
  } else {
    out_mode_ = OutputMode::kDouble;
  }
  rebuildInstance();
}

PJ::VarValue LuaSisoTransform::makeOutputValue(double computed, const PJ::VarValue& input) const {
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
          input);
    case OutputMode::kDouble:
    default:
      return PJ::VarValue{computed};
  }
}

void LuaSisoTransform::rebuildInstance() {
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

const char* LuaSisoTransform::id() const {
  return klass_.id.c_str();
}

const char* LuaSisoTransform::bracketLabel() const {
  return bracket_label_.c_str();
}

proc::TraitMask LuaSisoTransform::traits() const {
  // Every script filter is treated as causal and stateful, so the engine runs it eagerly: correct for integral,
  // derivative and window filters, harmless for stateless ones.
  return proc::kCausalStateful;
}

bool LuaSisoTransform::isStreamSafe() const {
  // False: a recompute starts from the beginning and never runs lazily, as arbitrary script state requires.
  return false;
}

void LuaSisoTransform::reset() {
  // A fresh VM is the only guaranteed-clean reset of arbitrary script state; the engine calls this before a batch
  // recompute.
  rebuildInstance();
}

std::optional<proc::Sample> LuaSisoTransform::calculateNextPoint(const proc::Sample& in) {
  if (!instance_) {
    return std::nullopt;
  }
  if (instance_->failed()) {
    error_ = instance_->error();
    return std::nullopt;
  }

  // The time origin is the first sample; subtract it on the int64 spine before the double cast (see the header).
  if (!has_session_start_) {
    session_start_ns_ = in.raw_ts_ns;
    has_session_start_ = true;
  }
  const double t_sec = static_cast<double>(in.raw_ts_ns - session_start_ns_) * 1e-9;
  const double v = proc::detail::toDouble(in.value());

  const FilterInstance::Result r = instance_->calculate(t_sec, v);
  if (instance_->failed()) {
    error_ = instance_->error();
    return std::nullopt;
  }
  if (r.suppress) {
    return std::nullopt;
  }

  // Rebuild the absolute int64 timestamp. The script's out_time is untrusted: NaN, infinity and an out-of-range or
  // overflowing value suppress the row instead of producing a corrupt timestamp.
  PJ::Timestamp out_ns = in.raw_ts_ns;
  if (r.out_time.has_value()) {
    const double ot_sec = *r.out_time;
    const double rel_ns = ot_sec * 1e9;
    if (!std::isfinite(ot_sec) || rel_ns > 9.0e18 || rel_ns < -9.0e18) {
      return std::nullopt;  // malformed / out-of-int64-range script timestamp → suppress
    }
    const std::int64_t rel = std::llround(rel_ns);
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    if ((rel > 0 && session_start_ns_ > kMax - rel) || (rel < 0 && session_start_ns_ < kMin - rel)) {
      return std::nullopt;  // the origin plus `rel` would overflow int64
    }
    out_ns = session_start_ns_ + rel;
  }
  if (out_mode_ == OutputMode::kString) {
    return proc::Sample::scalar(out_ns, PJ::VarValue{r.text});
  }
  return proc::Sample::scalar(out_ns, makeOutputValue(r.value, in.value()));
}

std::vector<PJ::StorageKind> LuaSisoTransform::outputKinds(PJ::Span<const PJ::StorageKind> in) const {
  switch (out_mode_) {
    case OutputMode::kInt64:
      return {PJ::StorageKind::kInt64};
    case OutputMode::kMirror:
      return proc::detail::mirrorInputKind(in);
    case OutputMode::kString:
      return {PJ::StorageKind::kString};
    case OutputMode::kDouble:
    default:
      return {PJ::StorageKind::kFloat64};
  }
}

std::string LuaSisoTransform::saveParams() const {
  return params_json_;
}

void LuaSisoTransform::loadParams(const std::string& json) {
  params_json_ = json;
  rebuildInstance();
}

bool LuaSisoTransform::failed() const {
  return !instance_ || instance_->failed();
}

const std::string& LuaSisoTransform::error() const {
  return error_;
}

}  // namespace PJ::scripting
