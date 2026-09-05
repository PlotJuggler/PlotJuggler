// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_core/parser_object.h"

#include <mutex>
#include <string>
#include <utility>

#include "pj_plugins/sdk/message_parser_plugin_base.hpp"

namespace PJ {

Expected<sdk::ObjectRecord, ParserObjectError> parseObjectRecordAs(
    MessageParserPluginBase& parser, const std::shared_ptr<std::mutex>& parser_mutex, Timestamp timestamp_ns,
    sdk::PayloadView payload, sdk::BuiltinObjectType expected_type) {
  // MessageParser plugins aren't thread-safe (fastcdr et al. keep stateful
  // scratch). Consumers sharing a parser singleton must share the same mutex and
  // hold it only across parseObject; downcast/decode work happens after unlock.
  auto invoke_parser = [&] {
    if (parser_mutex) {
      std::lock_guard<std::mutex> lock(*parser_mutex);
      return parser.parseObject(timestamp_ns, payload);
    }
    return parser.parseObject(timestamp_ns, payload);
  };

  auto object_or = invoke_parser();
  if (!object_or.has_value()) {
    return unexpected(
        ParserObjectError{
            ParserObjectErrorKind::kParseFailed,
            "parseObject failed: " + object_or.error(),
            sdk::BuiltinObjectType::kNone,
        });
  }

  const sdk::BuiltinObjectType actual_type = sdk::typeOf(object_or->object);
  if (actual_type != expected_type) {
    return unexpected(
        ParserObjectError{
            ParserObjectErrorKind::kWrongObjectKind,
            "parseObject returned wrong object_kind (expected " + std::string(sdk::name(expected_type)) + ")",
            actual_type,
        });
  }

  return std::move(*object_or);
}

}  // namespace PJ
