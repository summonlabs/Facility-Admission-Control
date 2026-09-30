// Facility Admission Control - text validation and escaping.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Text that arrives from outside is untrusted. Identifiers are restricted to a
// small ASCII alphabet, reasons must be valid UTF-8 without control characters
// and inside a fixed bound, and anything the runtime prints about untrusted
// input is escaped first.

#ifndef FAC_CORE_TEXT_HPP
#define FAC_CORE_TEXT_HPP

#include <string>
#include <string_view>

namespace fac {

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;
[[nodiscard]] bool contains_control_characters(std::string_view text) noexcept;
[[nodiscard]] bool is_valid_identifier(std::string_view text) noexcept;
[[nodiscard]] bool is_valid_reason(std::string_view text) noexcept;

// Renders arbitrary bytes as printable ASCII: UTF-8 continuation bytes and
// control characters become \xNN escapes. The result is bounded and safe to
// place in a log line, a diagnostic or a CLI response.
[[nodiscard]] std::string escape_text(std::string_view text);
[[nodiscard]] std::string to_lower_ascii(std::string_view text);
[[nodiscard]] bool iequals_ascii(std::string_view a, std::string_view b) noexcept;

}  // namespace fac

#endif  // FAC_CORE_TEXT_HPP
