#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <memory>

#include "pj_datastore/data_processor.hpp"
#include "pj_datastore/incremental_engine.hpp"  // PJ::ISISOTransform, PJ::VarValue

namespace PJ::proc {

/// Adapts a `DataProcessor` to the engine's `ISISOTransform`, so it can be installed as an `IncrementalEngine` node
/// (`addSisoTransform`) and run at commit time. The processor is shared: the host keeps its own handle (for preview and
/// reconfiguration) while the engine owns the adapter, and it lives until both are dropped.
class ProcessorSisoAdapter : public PJ::ISISOTransform {
 public:
  explicit ProcessorSisoAdapter(std::shared_ptr<DataProcessor> processor);

  void reset() override;
  [[nodiscard]] PJ::StorageKind outputKind(PJ::StorageKind input_kind) const override;
  bool calculate(
      PJ::Timestamp time, const PJ::VarValue& input, PJ::Timestamp& out_time, PJ::VarValue& out_value) override;
  // Forwards the processor's sticky failure, so the engine tells a failed sample from a suppressed row.
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
