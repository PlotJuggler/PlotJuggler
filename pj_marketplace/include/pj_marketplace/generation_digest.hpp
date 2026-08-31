#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Content identity for immutable store objects, plus the rejection vocabulary
// its parsing shares with the store layer.
//
// A SHA-256 over an artifact's bytes names the directory that artifact lives in,
// so identity (what the bytes are) is decoupled from provenance (where they came
// from). Carries no Qt, filesystem, or loader types.

#include <compare>
#include <string>
#include <string_view>

#include "pj_base/expected.hpp"

namespace PJ {

/// The layer that refused an operation. Carried on every rejection so a caller
/// can route by origin instead of matching on message text.
enum class RejectionDomain {
  kDescriptor,
  kPluginCheck,
  kStore,
  kProfile,
  kResolution,
};

[[nodiscard]] std::string_view rejectionDomainToString(RejectionDomain domain) noexcept;

/// Machine-readable reasons a generation descriptor or transition was refused.
enum class DescriptorRejectionCode {
  kInvalidJson,
  kLimitExceeded,
  kRootNotObject,
  kUnsupportedSchema,
  kMissingField,
  kWrongType,
  kUnknownField,
  kInvalidExtensionId,
  kInvalidModuleName,
  kInvalidVersion,
  kInvalidCompatibilityVersion,
  kInvalidAbiMajor,
  kInvalidDigest,
  kUnknownOrigin,
  kUnknownPluginFamily,
  kInvalidRelativePath,
  kInvalidText,
  kEmptyModules,
  kDuplicateModuleName,
  kDuplicateModuleLibrary,
  kDuplicateFilePath,
  kModuleFileMissing,
  kModuleFileDigestMismatch,
  // Admission-domain codes will claim these when that domain exists. They stay
  // here while the descriptor model remains their only caller.
  kMultiFamilyDso,
  kReleaseMutation,
};

[[nodiscard]] std::string_view descriptorRejectionCodeToString(DescriptorRejectionCode code) noexcept;

/// A stable rejection category accompanied by operator-facing detail.
struct GenerationRejection {
  RejectionDomain domain;
  DescriptorRejectionCode code;
  std::string message;
};

/// Canonical SHA-256 content identity.
///
/// Parsing accepts either 64 hexadecimal digits or an ASCII-case-insensitive
/// `sha256:` prefix followed by those digits. Storage and serialization always
/// use a lower-case prefixed value, so digest equality needs no special state.
class GenerationDigest {
 public:
  /// Parse a bare or prefixed SHA-256 digest and normalize it.
  [[nodiscard]] static Expected<GenerationDigest, GenerationRejection> parse(std::string_view text);

  /// Return the lower-case `sha256:<64-hex>` representation.
  [[nodiscard]] const std::string& str() const noexcept;

  [[nodiscard]] auto operator<=>(const GenerationDigest&) const noexcept = default;

 private:
  explicit GenerationDigest(std::string canonical);

  std::string canonical_;
};

/// The directory NAME an immutable store object uses for this digest: the
/// bare hex, no "sha256:" prefix. The one home of the objects-root layout
/// token — ArtifactStore and the resolver adapter both call this; nothing
/// may strip the prefix by hand.
[[nodiscard]] std::string objectDirectoryNameFor(const GenerationDigest& digest);

}  // namespace PJ
