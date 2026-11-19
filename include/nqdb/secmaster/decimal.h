#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include <nqdb/secmaster/error.h>
#include <nqdb/secmaster/records.h>

namespace nqdb::secmaster {

inline constexpr std::size_t nano_digits = 9;

inline bool is_digits(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, [](char c) { return '0' <= c && c <= '9'; });
}

inline std::int64_t nanos_from_decimal(std::string_view decimal) {
  const bool negative = decimal.starts_with('-');
  const std::string_view magnitude = decimal.substr(negative ? 1 : 0);
  const std::size_t point = magnitude.find('.');
  const bool has_fraction = point != std::string_view::npos;
  const std::string_view whole = magnitude.substr(0, point);
  const std::string_view fraction = has_fraction ? magnitude.substr(point + 1) : "";
  const bool fraction_is_well_formed =
      !has_fraction || (is_digits(fraction) && fraction.size() <= nano_digits);
  const bool well_formed = is_digits(whole) && fraction_is_well_formed;

  std::int64_t nanos = 0;
  bool fits = false;
  if (well_formed) {
    const std::string zeros_up_to_nanos(nano_digits - fraction.size(), '0');
    const std::string nano_digits_of_magnitude =
        std::string(whole) + std::string(fraction) + zeros_up_to_nanos;
    fits = std::from_chars(nano_digits_of_magnitude.data(),
                           nano_digits_of_magnitude.data() + nano_digits_of_magnitude.size(), nanos)
               .ec == std::errc{};
  }
  if (!fits) {
    throw error(std::format("'{}' is not a decimal number with at most {} fraction digits",
                            decimal, nano_digits));
  }
  return negative ? -nanos : nanos;
}

inline std::string decimal_from_nanos(std::int64_t nanos) {
  const std::uint64_t magnitude =
      nanos < 0 ? -static_cast<std::uint64_t>(nanos) : static_cast<std::uint64_t>(nanos);
  const auto magnitude_per_unit = static_cast<std::uint64_t>(nanos_per_unit);
  std::string decimal =
      std::format("{}{}.{:0{}}", nanos < 0 ? "-" : "", magnitude / magnitude_per_unit,
                  magnitude % magnitude_per_unit, nano_digits);
  decimal.erase(decimal.find_last_not_of('0') + 1);
  if (decimal.ends_with('.')) decimal.pop_back();
  return decimal;
}

}  // namespace nqdb::secmaster
