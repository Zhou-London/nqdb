#pragma once

#include <algorithm>
#include <charconv>
#include <concepts>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include <nq/db/secmaster/error.h>
#include <nq/db/secmaster/records.h>

namespace nq::db::secmaster {

inline bool is_digits(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, [](char c) { return '0' <= c && c <= '9'; });
}

inline std::string iso_date(std::chrono::sys_days day) { return std::format("{:%F}", day); }

inline std::chrono::sys_days parse_iso_date(std::string_view yyyy_mm_dd) {
  const auto number = [&](std::size_t first, std::size_t digits) {
    const std::string_view field = yyyy_mm_dd.substr(first, digits);
    int value = 0;
    std::from_chars(field.data(), field.data() + field.size(), value);
    return is_digits(field) ? value : 0;
  };
  const bool has_the_shape =
      yyyy_mm_dd.size() == 10 && yyyy_mm_dd[4] == '-' && yyyy_mm_dd[7] == '-';
  const std::chrono::year_month_day day{std::chrono::year{number(0, 4)},
                                        std::chrono::month(number(5, 2)),
                                        std::chrono::day(number(8, 2))};
  if (!has_the_shape || !day.ok()) {
    throw error(std::format("'{}' is not a date like 2026-01-31", yyyy_mm_dd));
  }
  return day;
}

inline std::string clock_time(std::chrono::minutes after_midnight) {
  const auto minutes = std::chrono::abs(after_midnight).count();
  return std::format("{}{:02}:{:02}", after_midnight.count() < 0 ? "-" : "", minutes / 60,
                     minutes % 60);
}

inline std::chrono::minutes parse_clock_time(std::string_view hh_mm) {
  const bool is_before_midnight = hh_mm.starts_with('-');
  const std::string_view digits = hh_mm.substr(is_before_midnight ? 1 : 0);
  const auto number = [&](std::size_t first) {
    const std::string_view field = digits.substr(first, 2);
    int value = -1;
    std::from_chars(field.data(), field.data() + field.size(), value);
    return is_digits(field) ? value : -1;
  };
  const bool is_clock_time = digits.size() == 5 && digits[2] == ':' && number(0) >= 0 &&
                             number(3) >= 0 && number(3) < 60;
  if (!is_clock_time) throw error(std::format("'{}' is not a time like 09:30", hh_mm));
  const std::chrono::minutes after_midnight{number(0) * 60 + number(3)};
  return is_before_midnight ? -after_midnight : after_midnight;
}

inline std::string iso_instant(std::chrono::sys_seconds instant) {
  return std::format("{:%FT%TZ}", instant);
}

inline std::chrono::sys_seconds parse_iso_instant(std::string_view utc_timestamp) {
  constexpr std::string_view example = "2026-01-31T14:30:00Z";
  const auto fail = [&] {
    return error(std::format("'{}' is not a UTC time like {}", utc_timestamp, example));
  };
  const bool has_the_shape = utc_timestamp.size() == example.size() && utc_timestamp[10] == 'T' &&
                             utc_timestamp[16] == ':' && utc_timestamp.back() == 'Z';
  if (!has_the_shape) throw fail();
  const std::string_view second_digits = utc_timestamp.substr(17, 2);
  const int second =
      is_digits(second_digits) ? (second_digits[0] - '0') * 10 + (second_digits[1] - '0') : 60;
  const std::chrono::minutes after_midnight = parse_clock_time(utc_timestamp.substr(11, 5));
  const bool is_time_of_day = after_midnight >= std::chrono::minutes{0} &&
                              after_midnight < std::chrono::hours{24} && second < 60;
  if (!is_time_of_day) throw fail();
  return parse_iso_date(utc_timestamp.substr(0, 10)) + after_midnight +
         std::chrono::seconds{second};
}

inline constexpr std::size_t nano_digits = 9;

inline std::string decimal(std::int64_t nanos) {
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

template <typename Nanos>
  requires requires(Nanos nanos) {
    { nanos.value() } -> std::convertible_to<std::int64_t>;
  }
std::string decimal(Nanos nanos) {
  return decimal(nanos.value());
}

inline std::int64_t parse_decimal(std::string_view decimal) {
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

template <typename Nanos>
Nanos parse_decimal(std::string_view decimal) {
  return Nanos{parse_decimal(decimal)};
}

}  // namespace nq::db::secmaster
