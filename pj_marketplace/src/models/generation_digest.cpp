// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_marketplace/generation_digest.hpp"

#include <algorithm>
#include <iterator>

namespace PJ {
namespace {

GenerationRejection rejection(DescriptorRejectionCode code, std::string message) {
  return {.domain = RejectionDomain::kDescriptor, .code = code, .message = std::move(message)};
}

char asciiLower(char character) {
  return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : character;
}

bool isHexDigit(char character) {
  return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
         (character >= 'A' && character <= 'F');
}

}  // namespace

std::string_view rejectionDomainToString(RejectionDomain domain) noexcept {
  switch (domain) {
    case RejectionDomain::kDescriptor:
      return "descriptor";
    case RejectionDomain::kPluginCheck:
      return "plugin_check";
    case RejectionDomain::kStore:
      return "store";
    case RejectionDomain::kProfile:
      return "profile";
    case RejectionDomain::kResolution:
      return "resolution";
  }
  return {};
}

std::string_view descriptorRejectionCodeToString(DescriptorRejectionCode code) noexcept {
  switch (code) {
    case DescriptorRejectionCode::kInvalidJson:
      return "invalid_json";
    case DescriptorRejectionCode::kLimitExceeded:
      return "limit_exceeded";
    case DescriptorRejectionCode::kRootNotObject:
      return "root_not_object";
    case DescriptorRejectionCode::kUnsupportedSchema:
      return "unsupported_schema";
    case DescriptorRejectionCode::kMissingField:
      return "missing_field";
    case DescriptorRejectionCode::kWrongType:
      return "wrong_type";
    case DescriptorRejectionCode::kUnknownField:
      return "unknown_field";
    case DescriptorRejectionCode::kInvalidExtensionId:
      return "invalid_extension_id";
    case DescriptorRejectionCode::kInvalidModuleName:
      return "invalid_module_name";
    case DescriptorRejectionCode::kInvalidVersion:
      return "invalid_version";
    case DescriptorRejectionCode::kInvalidCompatibilityVersion:
      return "invalid_compatibility_version";
    case DescriptorRejectionCode::kInvalidAbiMajor:
      return "invalid_abi_major";
    case DescriptorRejectionCode::kInvalidDigest:
      return "invalid_digest";
    case DescriptorRejectionCode::kUnknownOrigin:
      return "unknown_origin";
    case DescriptorRejectionCode::kUnknownPluginFamily:
      return "unknown_plugin_family";
    case DescriptorRejectionCode::kInvalidRelativePath:
      return "invalid_relative_path";
    case DescriptorRejectionCode::kInvalidText:
      return "invalid_text";
    case DescriptorRejectionCode::kEmptyModules:
      return "empty_modules";
    case DescriptorRejectionCode::kDuplicateModuleName:
      return "duplicate_module_name";
    case DescriptorRejectionCode::kDuplicateModuleLibrary:
      return "duplicate_module_library";
    case DescriptorRejectionCode::kDuplicateFilePath:
      return "duplicate_file_path";
    case DescriptorRejectionCode::kModuleFileMissing:
      return "module_file_missing";
    case DescriptorRejectionCode::kModuleFileDigestMismatch:
      return "module_file_digest_mismatch";
    case DescriptorRejectionCode::kMultiFamilyDso:
      return "multi_family_dso";
    case DescriptorRejectionCode::kReleaseMutation:
      return "release_mutation";
  }
  return {};
}

GenerationDigest::GenerationDigest(std::string canonical) : canonical_(std::move(canonical)) {}

Expected<GenerationDigest, GenerationRejection> GenerationDigest::parse(std::string_view text) {
  constexpr std::string_view prefix = "sha256:";
  std::string_view hexadecimal = text;
  const bool has_sha256_prefix =
      text.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), text.begin(), [](char left, char right) {
        return asciiLower(left) == asciiLower(right);
      });
  if (has_sha256_prefix) {
    hexadecimal.remove_prefix(prefix.size());
  } else if (text.find(':') != std::string_view::npos) {
    return unexpected(rejection(DescriptorRejectionCode::kInvalidDigest, "digest uses an unsupported prefix"));
  }
  if (hexadecimal.size() != 64 || !std::all_of(hexadecimal.begin(), hexadecimal.end(), isHexDigit)) {
    return unexpected(rejection(
        DescriptorRejectionCode::kInvalidDigest, "SHA-256 digest must contain exactly 64 hexadecimal digits"));
  }
  std::string canonical(prefix);
  canonical.reserve(prefix.size() + hexadecimal.size());
  std::transform(hexadecimal.begin(), hexadecimal.end(), std::back_inserter(canonical), [](char character) {
    return asciiLower(character);
  });
  return GenerationDigest(std::move(canonical));
}

const std::string& GenerationDigest::str() const noexcept {
  return canonical_;
}

std::string objectDirectoryNameFor(const GenerationDigest& digest) {
  return digest.str().substr(std::string_view("sha256:").size());
}

}  // namespace PJ
