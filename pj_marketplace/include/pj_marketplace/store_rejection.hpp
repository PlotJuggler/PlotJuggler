#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file store_rejection.hpp
 * @brief Stable rejection vocabulary for marketplace artifact storage.
 */

#include <string>
#include <utility>

#include "pj_marketplace/generation_digest.hpp"

namespace PJ {

/// Stable rejection vocabulary for transport, extraction, and object publication.
enum class StoreRejectionCode {
  kLimitExceeded,
  kUnsafePath,
  kUnsupportedEntryType,
  kArchiveInvalid,
  kIoFailure,
  kChecksumMismatch,
  kDigestMismatch,
  kWriterLeaseUnavailable,
  kFilesystemMismatch,
  kPostExtractionMutation,
  kMissingArtifact,
  kCorruptResidentArtifact,
  kCancelled,
};

[[nodiscard]] constexpr std::string_view storeRejectionCodeToString(StoreRejectionCode code) noexcept {
  switch (code) {
    case StoreRejectionCode::kLimitExceeded:
      return "limit_exceeded";
    case StoreRejectionCode::kUnsafePath:
      return "unsafe_path";
    case StoreRejectionCode::kUnsupportedEntryType:
      return "unsupported_entry_type";
    case StoreRejectionCode::kArchiveInvalid:
      return "archive_invalid";
    case StoreRejectionCode::kIoFailure:
      return "io_failure";
    case StoreRejectionCode::kChecksumMismatch:
      return "checksum_mismatch";
    case StoreRejectionCode::kDigestMismatch:
      return "digest_mismatch";
    case StoreRejectionCode::kWriterLeaseUnavailable:
      return "writer_lease_unavailable";
    case StoreRejectionCode::kFilesystemMismatch:
      return "filesystem_mismatch";
    case StoreRejectionCode::kPostExtractionMutation:
      return "post_extraction_mutation";
    case StoreRejectionCode::kMissingArtifact:
      return "missing_artifact";
    case StoreRejectionCode::kCorruptResidentArtifact:
      return "corrupt_resident_artifact";
    case StoreRejectionCode::kCancelled:
      return "cancelled";
  }
  return {};
}

/// Store-domain rejection with a stable code and operator-facing detail.
struct StoreRejection {
  RejectionDomain domain = RejectionDomain::kStore;
  StoreRejectionCode code = StoreRejectionCode::kIoFailure;
  std::string message;
};

/// Stable rejection vocabulary for profile values, recovery, and profile-aware GC.
enum class ProfileRejectionCode {
  kInvalidJson,
  kLimitExceeded,
  kRootNotObject,
  kUnsupportedSchema,
  kMissingField,
  kWrongType,
  kUnknownField,
  kInvalidValue,
  kDuplicateIdentity,
  kUnknownDesiredState,
  kUnknownOperationKind,
  kUnknownOperationPhase,
  kMalformedExtensionEntry,
  kInvalidSelection,
  kCorruptProfile,
  kMissingArtifact,
  kIoFailure,
  kWriterLeaseUnavailable,
};

[[nodiscard]] constexpr std::string_view profileRejectionCodeToString(ProfileRejectionCode code) noexcept {
  switch (code) {
    case ProfileRejectionCode::kInvalidJson:
      return "invalid_json";
    case ProfileRejectionCode::kLimitExceeded:
      return "limit_exceeded";
    case ProfileRejectionCode::kRootNotObject:
      return "root_not_object";
    case ProfileRejectionCode::kUnsupportedSchema:
      return "unsupported_schema";
    case ProfileRejectionCode::kMissingField:
      return "missing_field";
    case ProfileRejectionCode::kWrongType:
      return "wrong_type";
    case ProfileRejectionCode::kUnknownField:
      return "unknown_field";
    case ProfileRejectionCode::kInvalidValue:
      return "invalid_value";
    case ProfileRejectionCode::kDuplicateIdentity:
      return "duplicate_identity";
    case ProfileRejectionCode::kUnknownDesiredState:
      return "unknown_desired_state";
    case ProfileRejectionCode::kUnknownOperationKind:
      return "unknown_operation_kind";
    case ProfileRejectionCode::kUnknownOperationPhase:
      return "unknown_operation_phase";
    case ProfileRejectionCode::kMalformedExtensionEntry:
      return "malformed_extension_entry";
    case ProfileRejectionCode::kInvalidSelection:
      return "invalid_selection";
    case ProfileRejectionCode::kCorruptProfile:
      return "corrupt_profile";
    case ProfileRejectionCode::kMissingArtifact:
      return "missing_artifact";
    case ProfileRejectionCode::kIoFailure:
      return "io_failure";
    case ProfileRejectionCode::kWriterLeaseUnavailable:
      return "writer_lease_unavailable";
  }
  return {};
}

/// Profile-domain rejection with a stable code and operator-facing detail.
struct ProfileRejection {
  RejectionDomain domain = RejectionDomain::kProfile;
  ProfileRejectionCode code = ProfileRejectionCode::kIoFailure;
  std::string message;
};

/// Construct a profile-domain rejection without repeating its invariant domain.
inline ProfileRejection profileRejection(ProfileRejectionCode code, std::string message) {
  return {.domain = RejectionDomain::kProfile, .code = code, .message = std::move(message)};
}

/// Construct a store-domain rejection without repeating its invariant domain.
inline StoreRejection storeRejection(StoreRejectionCode code, std::string message) {
  return {.domain = RejectionDomain::kStore, .code = code, .message = std::move(message)};
}

}  // namespace PJ
