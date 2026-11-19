#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <CLI/CLI.hpp>

#include <nq/db.h>
#include <nq/db/secmaster/writer.h>

namespace nq::db::cli {

using namespace nq::db::secmaster;

struct context {
  std::filesystem::path database_file;
  std::ostream& out;

  std::filesystem::path file() const {
    return std::filesystem::absolute(database_file.empty() ? default_path() : database_file);
  }
  sqlite::database writable() const { return secmaster::open(file()); }
  sqlite::database read_only() const { return secmaster::open_read_only(file()); }
};

template <typename Parse>
auto parse_optional(const std::optional<std::string>& text, Parse parse) {
  return text.transform([&](const std::string& given) { return parse(given); });
}

template <typename Id>
std::optional<Id> optional_id(const std::optional<std::int64_t>& id) {
  return id.transform([](std::int64_t given) { return Id{given}; });
}

inline std::chrono::sys_days today() {
  return std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
}

inline day_range one_day(std::chrono::sys_days day) {
  return {.from = day, .until = day + std::chrono::days{1}};
}

template <typename Enum, std::size_t Count>
CLI::IsMember one_of(const std::array<enum_name<Enum>, Count>& names) {
  std::vector<std::string> allowed;
  for (const enum_name<Enum>& name : names) allowed.emplace_back(name.first);
  return CLI::IsMember(allowed);
}

inline CLI::IsMember asset_class_described_by(bool (*describes_asset_class)(asset_class)) {
  std::vector<std::string> allowed;
  for (const enum_name<asset_class>& name : asset_class_names) {
    if (describes_asset_class(name.second)) allowed.emplace_back(name.first);
  }
  return CLI::IsMember(allowed);
}

inline std::vector<std::string_view> parse_fields(std::string_view colon_separated,
                                                  std::size_t field_count,
                                                  std::string_view example) {
  std::vector<std::string_view> fields;
  std::string_view rest = colon_separated;
  while (fields.size() + 1 < field_count) {
    const std::size_t colon = rest.find(':');
    if (colon == std::string_view::npos) {
      throw error(std::format("'{}' is not a period like {}", colon_separated, example));
    }
    fields.push_back(rest.substr(0, colon));
    rest.remove_prefix(colon + 1);
  }
  fields.push_back(rest);
  return fields;
}

inline validity parse_validity(std::string_view from, std::string_view until_or_nothing) {
  return {.from = parse_iso_date(from),
          .until = until_or_nothing.empty() ? std::nullopt
                                            : std::optional{parse_iso_date(until_or_nothing)}};
}

inline venue_id venue_coded(sqlite::database& database, std::string_view code) {
  const std::optional<venue_id> venue = find_venue(database, code);
  if (!venue) throw error(std::format("there is no venue {}", code));
  return *venue;
}

inline tick_rule_id tick_rule_named(sqlite::database& database, std::string_view name) {
  const std::optional<tick_rule_id> tick_rule = find_tick_rule(database, name);
  if (!tick_rule) throw error(std::format("there is no tick rule {}", name));
  return *tick_rule;
}

struct date_option {
  std::string date = iso_date(today());

  void add_to(CLI::App& command,
              std::string_view description = "Day to read (default today, UTC)") {
    command.add_option("--date", date, std::string(description));
  }
  std::chrono::sys_days day() const { return parse_iso_date(date); }
};

struct day_range_options {
  std::string from = "0001-01-01";
  std::string until = "9999-12-31";

  void add_to(CLI::App& command) {
    command.add_option("--from", from, "First day of the range (default: the earliest day)");
    command.add_option("--until", until, "First day after the range (default: no end)");
  }
  day_range days() const { return {.from = parse_iso_date(from), .until = parse_iso_date(until)}; }
};

struct universe_options {
  std::optional<std::string> venue_code;
  std::optional<std::string> asset_class_name;

  void add_to(CLI::App& command) {
    command.add_option("--venue", venue_code, "Only this venue code");
    command.add_option("--asset-class", asset_class_name, "Only this asset class")
        ->check(one_of(asset_class_names));
  }

  bool operator()(const venue& venue, const instrument& instrument) const {
    return (!venue_code || venue.code == *venue_code) &&
           (!asset_class_name ||
            instrument.asset_class == enum_named(*asset_class_name, asset_class_names));
  }
};

}  // namespace nq::db::cli
