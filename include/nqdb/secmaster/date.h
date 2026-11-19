#pragma once

#include <charconv>
#include <chrono>
#include <format>
#include <string>
#include <string_view>

namespace nqdb::secmaster {

inline std::string iso_date(std::chrono::sys_days day) { return std::format("{:%F}", day); }

inline std::chrono::sys_days parse_iso_date(std::string_view yyyy_mm_dd) {
  const auto number = [&](std::size_t first, std::size_t digits) {
    int value = 0;
    std::from_chars(yyyy_mm_dd.data() + first, yyyy_mm_dd.data() + first + digits, value);
    return value;
  };
  return std::chrono::year{number(0, 4)} / number(5, 2) / number(8, 2);
}

}  // namespace nqdb::secmaster
