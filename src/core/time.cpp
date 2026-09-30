// Facility Admission Control - time formatting and the system clock.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/core/time.hpp"

#include <chrono>
#include <cstdio>

namespace fac {
namespace {

// Howard Hinnant's days-from-civil inverse: converts a day count since
// 1970-01-01 into a proleptic Gregorian date. Deterministic and locale free.
void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month, unsigned& day) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto doe = static_cast<std::uint64_t>(days - era * 146097);
  const std::uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const std::uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::uint64_t mp = (5 * doy + 2) / 153;
  const std::uint64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::uint64_t m = mp < 10 ? mp + 3 : mp - 9;
  year = y + (m <= 2 ? 1 : 0);
  month = static_cast<unsigned>(m);
  day = static_cast<unsigned>(d);
}

}  // namespace

bool is_valid_timestamp_nanos(Nanos value) noexcept {
  return value >= kMinTimestampNanos && value <= kMaxTimestampNanos;
}

std::string Duration::to_string() const {
  std::string out = std::to_string(nanos_);
  out.append("ns");
  return out;
}

std::string Timestamp::to_string() const {
  constexpr Nanos kNanosPerSecond = 1000000000LL;
  const std::int64_t seconds = nanos_ / kNanosPerSecond;
  const std::int64_t fraction = nanos_ % kNanosPerSecond;
  const std::int64_t days = seconds / 86400;
  std::int64_t seconds_of_day = seconds % 86400;
  if (seconds_of_day < 0) {
    seconds_of_day += 86400;
  }

  std::int64_t year = 1970;
  unsigned month = 1;
  unsigned day = 1;
  civil_from_days(days, year, month, day);

  const auto hour = static_cast<unsigned>(seconds_of_day / 3600);
  const auto minute = static_cast<unsigned>((seconds_of_day % 3600) / 60);
  const auto second = static_cast<unsigned>(seconds_of_day % 60);
  const auto nanos = static_cast<unsigned>(fraction);

  char buffer[64];
  const int written = std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02u:%02u:%02u.%09uZ",
                                    static_cast<long long>(year), month, day, hour, minute, second, nanos);
  if (written <= 0) {
    return "invalid-timestamp";
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

Result<Duration> age_of(Timestamp observed, Timestamp now) {
  const Nanos difference = now.unix_nanos() - observed.unix_nanos();
  if (difference < 0) {
    return make_error(ErrorCode::underflow, "observed timestamp is in the future");
  }
  return Duration(difference);
}

Timestamp SystemClock::now() const {
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
  if (!is_valid_timestamp_nanos(nanos)) {
    return Timestamp(kMinTimestampNanos);
  }
  return Timestamp(nanos);
}

}  // namespace fac
