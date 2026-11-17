#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/span.hpp"
#include "pj_datastore/column_buffer.hpp"  // PJ::StorageKind
#include "pj_datastore/sample.hpp"

namespace PJ::proc {

/// Behaviour bits a processor ORs together. Declared by processors; the engine does not read them.
using TraitMask = std::uint32_t;

inline constexpr TraitMask kStatelessOneToOne = 1u << 0;  ///< pure map (scale/abs)
inline constexpr TraitMask kCausalStateful = 1u << 1;     ///< integral/derivative/window
inline constexpr TraitMask kFiniteLookback = 1u << 2;     ///< bounded look-back window
inline constexpr TraitMask kFullRecompute = 1u << 3;      ///< re-runs over the whole input on any change
inline constexpr TraitMask kMimoExactJoin = 1u << 4;      ///< N inputs sharing a timestamp
inline constexpr TraitMask kMaySuppress = 1u << 5;        ///< may return nullopt
inline constexpr TraitMask kPreservesOrder = 1u << 6;     ///< output timestamps strictly ascending

/// Host-internal base for a data processor (filter or transform) with a C++, Lua or Python implementation; it never
/// crosses the plugin ABI. The N-input/M-output shape is declared, but ProcessorSisoAdapter, the only adapter, installs
/// single-input single-output processors. Nothing in the host reads traits(), isStreamSafe(), numInputs() or
/// bracketLabel().
///
/// Sequential contract (that of ISISOTransform): `calculateNextPoint` is called once per sample in strictly ascending
/// timestamp order. State persists across calls and is cleared only by `reset()`, which the engine calls before a full
/// recompute, never between commits.
class DataProcessor {
 public:
  virtual ~DataProcessor() = default;

  [[nodiscard]] virtual const char* id() const = 0;            ///< recipe key (`processor_id`)
  [[nodiscard]] virtual const char* bracketLabel() const = 0;  ///< legend suffix, e.g. "Integral"
  [[nodiscard]] virtual TraitMask traits() const = 0;
  [[nodiscard]] virtual bool isStreamSafe() const = 0;
  [[nodiscard]] virtual int numInputs() const {
    return 1;
  }
  [[nodiscard]] virtual int numOutputs() const {
    return 1;
  }

  virtual void reset() = 0;

  /// nullopt emits no row at this timestamp (e.g. the first sample of a derivative).
  [[nodiscard]] virtual std::optional<Sample> calculateNextPoint(const Sample& in) = 0;

  /// Feeds `tail` through calculateNextPoint(), appending the emitted rows to `out`. The engine's commit path does not
  /// use it: it calls calculateNextPoint() per sample through ProcessorSisoAdapter.
  virtual void appendTail(const std::vector<Sample>& tail, std::vector<Sample>& out);

  /// `reset()` then `appendTail()`: one shot over a whole series (the filter preview).
  [[nodiscard]] virtual std::vector<Sample> applyBatch(const std::vector<Sample>& in);

  /// Sticky: true once an unrecoverable error (e.g. a script runtime error) leaves the processor unable to produce
  /// output. ProcessorSisoAdapter forwards it so the engine reports a failed sample instead of reading it as a
  /// suppressed row.
  [[nodiscard]] virtual bool failed() const {
    return false;
  }
  /// The reason for `failed()`, or empty.
  [[nodiscard]] virtual const std::string& error() const {
    static const std::string kNone;
    return kNone;
  }

  /// One kind per output, given the input kinds. Default: kFloat64 per output.
  [[nodiscard]] virtual std::vector<PJ::StorageKind> outputKinds(PJ::Span<const PJ::StorageKind> in) const;

  /// Parameters as JSON, stored in the recipe.
  [[nodiscard]] virtual std::string saveParams() const {
    return "{}";
  }
  virtual void loadParams(const std::string& json) {
    (void)json;
  }
};

}  // namespace PJ::proc
