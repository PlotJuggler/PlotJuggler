#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <memory>

#include "pj_datastore/data_processor.hpp"
#include "pj_datastore/incremental_engine.hpp"  // PJ::ISISOTransform, PJ::VarValue

namespace PJ::proc {

/// Adapts a host-internal `DataProcessor` to the engine's `ISISOTransform`, so a
/// processor can be installed as a `IncrementalEngine` node
/// (`IncrementalEngine::addSisoTransform`) and run EAGERLY at commit time. Bridges
/// the two per-sample forms: the engine's `(Timestamp, VarValue)` out-param style
/// and the processor's `Sample` value style.
///
/// Holds a `shared_ptr` to the processor so the host (the forthcoming
/// `DataProcessorService` / `ProcessorRuntime`) can keep its own handle — for
/// preview, reconfigure, or teardown — while the engine owns this adapter. The
/// processor lives as long as either side holds a reference; dropping the node
/// (`removeNode`) and the host handle frees it.
class ProcessorSisoAdapter : public PJ::ISISOTransform {
 public:
  explicit ProcessorSisoAdapter(std::shared_ptr<DataProcessor> processor);

  void reset() override;
  [[nodiscard]] PJ::StorageKind outputKind(PJ::StorageKind input_kind) const override;
  bool calculate(
      PJ::Timestamp time, const PJ::VarValue& input, PJ::Timestamp& out_time, PJ::VarValue& out_value) override;
  // Forward the processor's sticky failure so the engine can distinguish a failed
  // sample from a legitimately-suppressed row (the core of the silent-failure fix).
  [[nodiscard]] bool failed() const override {
    return processor_->failed();
  }
  [[nodiscard]] const std::string& error() const override {
    return processor_->error();
  }

 private:
  std::shared_ptr<DataProcessor> processor_;
};

}  // namespace PJ::proc
