#pragma once

#include <chrono>
#include <format>
#include <string>
#include <string_view>

#include <cctz/civil_time.h>
#include <cctz/time_zone.h>

#include <nq/db/secmaster/error.h>

namespace nq::db::secmaster {

using time_zone = cctz::time_zone;

inline time_zone zone_named(std::string_view iana_name) {
  time_zone zone;
  if (!cctz::load_time_zone(std::string(iana_name), &zone)) {
    throw error(std::format("'{}' is not an IANA time zone like America/New_York", iana_name));
  }
  return zone;
}

inline std::chrono::sys_seconds utc_instant(const time_zone& zone, std::chrono::sys_days local_day,
                                            std::chrono::minutes after_local_midnight) {
  const std::chrono::year_month_day day{local_day};
  return cctz::convert(
      cctz::civil_second(static_cast<int>(day.year()), static_cast<unsigned>(day.month()),
                         static_cast<unsigned>(day.day()), 0, after_local_midnight.count(), 0),
      zone);
}

}  // namespace nq::db::secmaster
