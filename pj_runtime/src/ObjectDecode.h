// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <utility>

#include "pj_base/builtin/builtin_object_codec.hpp"
#include "pj_base/builtin/point_cloud_codec.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/parse_locked.h"

namespace PJ {

/// Decodes one object entry of `type`: through the topic's parser, under its per-topic lock, when one is
/// bound; else as canonical bytes (a source or toolbox that pushed the object itself), a point cloud as a
/// view over the anchored payload. A parser that returns another type is an error. Any thread: the binding's
/// keepalive holds the parser.
inline Expected<sdk::BuiltinObject> decodeObjectEntry(
    const SessionManager::ParserBinding& parser, sdk::BuiltinObjectType type, Timestamp ts,
    const sdk::PayloadView& payload) {
  if (!parser && type == sdk::BuiltinObjectType::kPointCloud) {
    auto cloud = deserializePointCloudView(payload.bytes.data(), payload.bytes.size(), payload.anchor);
    if (!cloud) {
      return unexpected(std::move(cloud).error());
    }
    return sdk::BuiltinObject(std::move(*cloud));
  }
  if (!parser) {
    return deserializeBuiltinObject(type, payload.bytes.data(), payload.bytes.size());
  }
  auto record = parseLocked(parser, ts, payload);
  if (!record) {
    return unexpected(std::move(record).error());
  }
  if (record->object.type() != type) {
    return unexpected(
        "the parser returned a " + std::string(sdk::name(record->object.type())) + ", not a " +
        std::string(sdk::name(type)));
  }
  return std::move(record->object);
}

}  // namespace PJ
