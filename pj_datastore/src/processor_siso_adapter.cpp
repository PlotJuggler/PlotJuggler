// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/processor_siso_adapter.hpp"

#include <optional>
#include <utility>

namespace PJ::proc {

ProcessorSisoAdapter::ProcessorSisoAdapter(std::shared_ptr<DataProcessor> processor)
    : processor_(std::move(processor)) {}

void ProcessorSisoAdapter::reset() {
  processor_->reset();
}

PJ::StorageKind ProcessorSisoAdapter::outputKind(PJ::StorageKind input_kind) const {
  const PJ::StorageKind in[1] = {input_kind};
  // A SISO processor has exactly one output.
  return processor_->outputKinds(in).front();
}

bool ProcessorSisoAdapter::calculate(
    PJ::Timestamp time, const PJ::VarValue& input, PJ::Timestamp& out_time, PJ::VarValue& out_value) {
  std::optional<Sample> out = processor_->calculateNextPoint(Sample::scalar(time, input));
  if (!out) {
    return false;
  }
  out_time = out->raw_ts_ns;
  out_value = std::move(out->value());
  return true;
}

}  // namespace PJ::proc
