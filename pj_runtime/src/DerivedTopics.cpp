// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/DerivedTopics.h"

#include "pj_datastore/marker_topic_metadata.hpp"

namespace PJ {

bool isDerivedObjectTopic(const ObjectTopicDescriptor& descriptor) {
  return isDerivedOfKind(descriptor.metadata_json, sdk::kDerivedOnDemandValue);
}

}  // namespace PJ
