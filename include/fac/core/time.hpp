// Facility Admission Control - time.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Time enters the runtime through an injected Clock. The engine never reads a
// wall clock itself, so an evaluation is reproducible from the same inputs and
// the same decision timestamp. Freshness, expiry and fencing are all decided by
// comparing explicit timestamps, never by elapsed real time.

#ifndef FAC_CORE_TIME_HPP
#define FAC_CORE_TIME_HPP

#include <cstdint>
#include <string>

#include "fac/core/error.hpp"

namespace fac {

using Nanos = std::int64_t;

// Nanosecond timestamps are held in a signed 64-bit count, so the usable range
// ends in 2262. The library refuses anything outside a bounded window that
// leaves room for arithmetic on durations without overflow.
inline constexpr Nanos kMinTimestampNanos = 0;
inline constexpr Nanos kMaxTimestampNanos = 9'200'000'000'000'000'000LL;

[[nodiscard]] bool is_valid_timestamp_nanos(Nanos value) noexcept;

class Duration {
 public:
  constexpr Duration() noexcept = default;
  constexpr explicit Duration(Nanos nanos) noexcept : nanos_(nanos) {}

  [[nodiscard]] static constexpr Duration from_nanos(Nanos nanos) noexcept { return Duration(nanos); }
  [[nodiscard]] static constexpr Duration from_micros(Nanos micros) noexcept {
    return Duration(micros * 1000);
  }
  [[nodiscard]] static constexpr Duration from_millis(Nanos millis) noexcept {
    return Duration(millis * 1000);
  }
  [[nodiscard]] static constexpr Duration from_seconds(Nanos seconds) noexcept {
    return Duration(seconds * 1000000000);
  }

  [[nodiscard]] static Result<Duration> from_nanos_checked(Nanos nanos) {
    if (nanos < 0 || nanos > kMaxTimestampNanos) {
      return make_error(ErrorCode::out_of_range, "duration is outside the supported range");
    }
    return Duration(nanos);
  }

  [[nodiscard]] constexpr Nanos nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }

  [[nodiscard]] Result<Duration> checked_add(Duration other) const {
    Nanos sum = 0;
    if (nanos_ > kMaxTimestampNanos - other.nanos_) {
      return make_error(ErrorCode::overflow, "duration addition overflowed");
    }
    sum = nanos_ + other.nanos_;
    return Duration(sum);
  }

  [[nodiscard]] std::string to_string() const;

  friend constexpr bool operator==(const Duration&, const Duration&) noexcept = default;
  friend constexpr auto operator<=>(const Duration&, const Duration&) noexcept = default;

 private:
  Nanos nanos_ = 0;
};

class Timestamp {
 public:
  constexpr Timestamp() noexcept = default;
  constexpr explicit Timestamp(Nanos unix_nanos) noexcept : nanos_(unix_nanos) {}

  [[nodiscard]] static Result<Timestamp> from_unix_nanos(Nanos unix_nanos) {
    if (!is_valid_timestamp_nanos(unix_nanos)) {
      return make_error(ErrorCode::out_of_range, "timestamp is outside the supported range");
    }
    return Timestamp(unix_nanos);
  }

  [[nodiscard]] constexpr Nanos unix_nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr bool is_unset() const noexcept { return nanos_ == 0; }

  [[nodiscard]] Result<Timestamp> checked_add(Duration delta) const {
    if (delta.nanos() < 0) {
      return make_error(ErrorCode::underflow, "cannot add a negative duration to a timestamp");
    }
    if (nanos_ > kMaxTimestampNanos - delta.nanos()) {
      return make_error(ErrorCode::overflow, "timestamp addition overflowed");
    }
    return Timestamp(nanos_ + delta.nanos());
  }

  [[nodiscard]] std::string to_string() const;

  friend constexpr bool operator==(const Timestamp&, const Timestamp&) noexcept = default;
  friend constexpr auto operator<=>(const Timestamp&, const Timestamp&) noexcept = default;

 private:
  Nanos nanos_ = 0;
};

// Returns the non-negative age of a timestamp, or an error when the timestamp
// is in the future relative to now. Callers that tolerate clock skew handle the
// error explicitly rather than receiving a negative age.
[[nodiscard]] Result<Duration> age_of(Timestamp observed, Timestamp now);

class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock() = default;

  [[nodiscard]] virtual Timestamp now() const = 0;
};

// Reads the system clock. Used by tools and by tests that need real time.
class SystemClock final : public Clock {
 public:
  [[nodiscard]] Timestamp now() const override;
};

// A clock the caller sets explicitly. Deterministic tests use this so an
// evaluation can be reproduced byte for byte.
class FixedClock final : public Clock {
 public:
  FixedClock() = default;
  explicit FixedClock(Timestamp value) : value_(value) {}

  void set(Timestamp value) noexcept { value_ = value; }
  [[nodiscard]] Timestamp now() const override { return value_; }

 private:
  Timestamp value_{};
};

}  // namespace fac

#endif  // FAC_CORE_TIME_HPP
