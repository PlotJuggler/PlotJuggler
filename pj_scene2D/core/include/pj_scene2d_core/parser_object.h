// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <any>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"

namespace PJ {

class MessageParserPluginBase;

enum class ParserObjectErrorKind {
  kParseFailed,
  kWrongObjectKind,
  kAnyCastFailed,
};

struct ParserObjectError {
  ParserObjectErrorKind kind = ParserObjectErrorKind::kParseFailed;
  std::string message;
  sdk::BuiltinObjectType actual_type = sdk::BuiltinObjectType::kNone;
};

[[nodiscard]] Expected<sdk::ObjectRecord, ParserObjectError> parseObjectRecordAs(
    MessageParserPluginBase& parser, const std::shared_ptr<std::mutex>& parser_mutex, Timestamp timestamp_ns,
    sdk::PayloadView payload, sdk::BuiltinObjectType expected_type);

template <typename TObject>
struct ParsedObject {
  explicit ParsedObject(sdk::ObjectRecord record_in) : record(std::move(record_in)) {
    value = record.object.get<TObject>();
  }

  ParsedObject(ParsedObject&& other) noexcept : record(std::move(other.record)) {
    value = record.object.get<TObject>();
  }

  ParsedObject& operator=(ParsedObject&& other) noexcept {
    if (this != &other) {
      record = std::move(other.record);
      value = record.object.get<TObject>();
    }
    return *this;
  }

  ParsedObject(const ParsedObject&) = delete;
  ParsedObject& operator=(const ParsedObject&) = delete;

  sdk::ObjectRecord record;
  const TObject* value = nullptr;
};

template <typename TObject>
[[nodiscard]] Expected<ParsedObject<TObject>, ParserObjectError> parseObjectAs(
    MessageParserPluginBase& parser, const std::shared_ptr<std::mutex>& parser_mutex, Timestamp timestamp_ns,
    sdk::PayloadView payload, sdk::BuiltinObjectType expected_type, std::string_view type_name) {
  auto record_or = parseObjectRecordAs(parser, parser_mutex, timestamp_ns, std::move(payload), expected_type);
  if (!record_or.has_value()) {
    return unexpected(record_or.error());
  }

  ParsedObject<TObject> parsed(std::move(*record_or));
  if (parsed.value == nullptr) {
    return unexpected(
        ParserObjectError{
            ParserObjectErrorKind::kAnyCastFailed,
            "object cast to " + std::string(type_name) + " failed (parser contract violation)",
            expected_type,
        });
  }
  return parsed;
}

}  // namespace PJ
