// Facility Admission Control - error model.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Errors are values, never exceptions and never silently ignored. A refusal
// decision produced by the engine is not an error: it is a successful
// evaluation with a Refuse or Defer verdict. ErrorCode describes a failure to
// produce a decision at all.

#ifndef FAC_CORE_ERROR_HPP
#define FAC_CORE_ERROR_HPP

#include <cstdint>
#include <string>
#include <utility>

namespace fac {

enum class ErrorCode : std::uint16_t {
  ok = 0,

  // Structural input problems. Detected before any semantic evaluation.
  invalid_argument = 1,
  out_of_range = 2,
  buffer_too_small = 3,
  truncated_input = 4,
  malformed_input = 5,
  trailing_bytes = 6,
  unsupported_version = 7,
  reserved_field_nonzero = 8,
  duplicate_field = 9,
  invalid_utf8 = 10,
  iteration_limit_exceeded = 11,

  // Integrity problems.
  digest_mismatch = 12,
  checksum_mismatch = 13,

  // Environment problems.
  io_error = 14,
  file_not_found = 15,
  path_invalid = 16,
  path_too_long = 17,
  already_exists = 18,
  not_found = 19,
  conflict = 20,
  writer_lock_held = 21,
  read_only_store = 22,
  unsupported_platform = 23,

  // Durable-state problems. Every one of these fails closed.
  corrupt_store = 24,
  manifest_missing = 25,
  manifest_invalid = 26,
  journal_truncated = 27,
  journal_ahead_of_manifest = 28,
  snapshot_ahead_of_journal = 29,
  rollback_detected = 30,
  format_revision_unsupported = 31,
  state_unverified = 32,

  // Bounds and arithmetic.
  overflow = 33,
  underflow = 34,
  limit_exceeded = 35,

  internal = 36,
};

[[nodiscard]] const char* to_string(ErrorCode code) noexcept;

// A failure with a bounded, human-readable detail. The detail never contains
// file contents, secrets or unbounded external input: callers pass literals or
// escaped identifiers.
struct Error {
  ErrorCode code = ErrorCode::ok;
  std::string detail;

  Error() = default;
  explicit Error(ErrorCode c) : code(c) {}
  Error(ErrorCode c, std::string d) : code(c), detail(std::move(d)) {}

  [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::ok; }
  [[nodiscard]] std::string to_string() const;
};

// A successful or failed value. There is no default-constructed success: a
// Result is always built from a value or from an error.
template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : value_(std::move(value)), ok_(true) {}
  Result(Error error) : error_(std::move(error)), ok_(false) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok_; }

  // Precondition: ok(). Callers that ignore ok() are violating the contract,
  // which is why the accessors are deliberately not defensive.
  [[nodiscard]] const T& value() const noexcept { return value_; }
  [[nodiscard]] T& value() noexcept { return value_; }
  [[nodiscard]] T take() noexcept { return std::move(value_); }

  [[nodiscard]] const Error& error() const noexcept { return error_; }

 private:
  T value_{};
  Error error_{};
  bool ok_ = false;
};

// A result with no payload.
class [[nodiscard]] Status {
 public:
  Status() = default;
  Status(Error error) : error_(std::move(error)) {}

  [[nodiscard]] bool ok() const noexcept { return error_.ok(); }
  explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }

  static Status success() { return Status(); }

 private:
  Error error_{};
};

[[nodiscard]] inline Error make_error(ErrorCode code, std::string detail) {
  return Error(code, std::move(detail));
}

}  // namespace fac

#endif  // FAC_CORE_ERROR_HPP
