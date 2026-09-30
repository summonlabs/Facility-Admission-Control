// Facility Admission Control - error rendering.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/core/error.hpp"

#include "fac/core/text.hpp"

namespace fac {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::ok: return "ok";
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::out_of_range: return "out_of_range";
    case ErrorCode::buffer_too_small: return "buffer_too_small";
    case ErrorCode::truncated_input: return "truncated_input";
    case ErrorCode::malformed_input: return "malformed_input";
    case ErrorCode::trailing_bytes: return "trailing_bytes";
    case ErrorCode::unsupported_version: return "unsupported_version";
    case ErrorCode::reserved_field_nonzero: return "reserved_field_nonzero";
    case ErrorCode::duplicate_field: return "duplicate_field";
    case ErrorCode::invalid_utf8: return "invalid_utf8";
    case ErrorCode::iteration_limit_exceeded: return "iteration_limit_exceeded";
    case ErrorCode::digest_mismatch: return "digest_mismatch";
    case ErrorCode::checksum_mismatch: return "checksum_mismatch";
    case ErrorCode::io_error: return "io_error";
    case ErrorCode::file_not_found: return "file_not_found";
    case ErrorCode::path_invalid: return "path_invalid";
    case ErrorCode::path_too_long: return "path_too_long";
    case ErrorCode::already_exists: return "already_exists";
    case ErrorCode::not_found: return "not_found";
    case ErrorCode::conflict: return "conflict";
    case ErrorCode::writer_lock_held: return "writer_lock_held";
    case ErrorCode::read_only_store: return "read_only_store";
    case ErrorCode::unsupported_platform: return "unsupported_platform";
    case ErrorCode::corrupt_store: return "corrupt_store";
    case ErrorCode::manifest_missing: return "manifest_missing";
    case ErrorCode::manifest_invalid: return "manifest_invalid";
    case ErrorCode::journal_truncated: return "journal_truncated";
    case ErrorCode::journal_ahead_of_manifest: return "journal_ahead_of_manifest";
    case ErrorCode::snapshot_ahead_of_journal: return "snapshot_ahead_of_journal";
    case ErrorCode::rollback_detected: return "rollback_detected";
    case ErrorCode::format_revision_unsupported: return "format_revision_unsupported";
    case ErrorCode::state_unverified: return "state_unverified";
    case ErrorCode::overflow: return "overflow";
    case ErrorCode::underflow: return "underflow";
    case ErrorCode::limit_exceeded: return "limit_exceeded";
    case ErrorCode::internal: return "internal";
  }
  return "unrecognized_error_code";
}

std::string Error::to_string() const {
  std::string result = fac::to_string(code);
  if (!detail.empty()) {
    result.append(": ");
    result.append(escape_text(detail));
  }
  return result;
}

}  // namespace fac
