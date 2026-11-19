#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <CLI/CLI.hpp>

#include "cli/arguments.h"

namespace nq::db::cli {

inline constexpr std::array<std::string_view, 7> weekday_names{"MON", "TUE", "WED", "THU",
                                                              "FRI", "SAT", "SUN"};

inline std::string_view name_of(std::chrono::weekday weekday) {
  return weekday_names[weekday.iso_encoding() - 1];
}

inline unsigned parse_iso_weekday(std::string_view name) {
  const auto found = std::ranges::find(weekday_names, name);
  if (found == weekday_names.end()) {
    throw error(std::format("'{}' is not a weekday like MON", name));
  }
  return static_cast<unsigned>(found - weekday_names.begin()) + 1;
}

inline std::vector<std::chrono::weekday> parse_weekdays(std::string_view day_or_first_dash_last) {
  const std::size_t dash = day_or_first_dash_last.find('-');
  const unsigned first = parse_iso_weekday(day_or_first_dash_last.substr(0, dash));
  const unsigned last = dash == std::string_view::npos
                            ? first
                            : parse_iso_weekday(day_or_first_dash_last.substr(dash + 1));
  if (last < first) {
    throw error(std::format("'{}' is not a run of weekdays like MON-FRI", day_or_first_dash_last));
  }
  std::vector<std::chrono::weekday> weekdays;
  for (unsigned iso_weekday = first; iso_weekday <= last; ++iso_weekday) {
    weekdays.emplace_back(iso_weekday);
  }
  return weekdays;
}

inline session_hours parse_session(std::string_view opens_dash_closes) {
  const std::size_t dash = opens_dash_closes.find('-', 1);
  if (dash == std::string_view::npos) {
    throw error(std::format("'{}' is not a session like 09:30-16:00", opens_dash_closes));
  }
  return {.opens = parse_clock_time(opens_dash_closes.substr(0, dash)),
          .closes = parse_clock_time(opens_dash_closes.substr(dash + 1))};
}

inline std::vector<session_hours> parse_sessions(const std::vector<std::string>& sessions) {
  std::vector<session_hours> hours;
  for (const std::string& session : sessions) hours.push_back(parse_session(session));
  return hours;
}

struct calendar_commands {
  std::string week_venue;
  std::string week_valid_from;
  std::vector<std::string> week_hours;

  std::string closed_venue;
  std::vector<std::string> closed_days;

  std::string special_venue;
  std::string special_day;
  std::vector<std::string> special_sessions;

  std::string regular_venue;
  std::vector<std::string> regular_days;

  std::string rules_venue;

  std::string listed_venue;
  std::string listed_from;
  std::string listed_until;

  std::string asked_venue;
  std::string asked_time;

  static std::vector<weekly_session> parse_weekly_sessions(const std::vector<std::string>& hours) {
    std::vector<weekly_session> sessions;
    for (const std::string& days_colon_session : hours) {
      const std::size_t colon = days_colon_session.find(':');
      if (colon == std::string::npos) {
        throw error(std::format("'{}' is not hours like MON-FRI:09:30-16:00", days_colon_session));
      }
      const std::string_view text = days_colon_session;
      const session_hours session = parse_session(text.substr(colon + 1));
      for (const std::chrono::weekday weekday : parse_weekdays(text.substr(0, colon))) {
        sessions.push_back({.weekday = weekday, .hours = session});
      }
    }
    return sessions;
  }

  static void print_rules(std::ostream& out, const calendar_rules& rules) {
    out << "kind\tday\tuntil\tweekday\topens\tcloses\n";
    for (const weekly_session_period& period : rules.weekly_sessions) {
      out << "WEEK\t" << iso_date(period.valid.from) << '\t'
          << period.valid.until.transform(iso_date).value_or("") << '\t'
          << name_of(period.session.weekday) << '\t' << clock_time(period.session.hours.opens)
          << '\t' << clock_time(period.session.hours.closes) << '\n';
    }
    for (const std::chrono::sys_days day : rules.closed_days) {
      out << "CLOSED\t" << iso_date(day) << "\t\t" << name_of(std::chrono::weekday{day})
          << "\t\t\n";
    }
    for (const special_session& session : rules.special_sessions) {
      out << "SPECIAL\t" << iso_date(session.day) << "\t\t"
          << name_of(std::chrono::weekday{session.day}) << '\t'
          << clock_time(session.hours.opens) << '\t' << clock_time(session.hours.closes) << '\n';
    }
  }

  static void print_sessions(std::ostream& out, const calendar& loaded) {
    out << "trading_day\tweekday\topens\tcloses\topens_utc\tcloses_utc\n";
    for (const std::chrono::sys_days day : loaded.trading_days()) {
      const std::span<const session_hours> sessions = loaded.sessions_on(day);
      const std::span<const session_instants> instants = loaded.instants_on(day);
      for (std::size_t session = 0; session < sessions.size(); ++session) {
        out << iso_date(day) << '\t' << name_of(std::chrono::weekday{day}) << '\t'
            << clock_time(sessions[session].opens) << '\t' << clock_time(sessions[session].closes)
            << '\t' << iso_instant(instants[session].opens) << '\t'
            << iso_instant(instants[session].closes) << '\n';
      }
    }
  }

  void add_to(CLI::App& app, const context& context) {
    CLI::App* commands =
        app.add_subcommand("calendar", "Trading calendars")->require_subcommand(1);

    CLI::App* set_week_command = commands->add_subcommand(
        "set-week", "Give a venue its weekly sessions from a day on, in the venue's local time");
    set_week_command->add_option("--venue", week_venue, "Venue code")->required();
    set_week_command->add_option("--from", week_valid_from, "First day of the weekly sessions")
        ->required();
    set_week_command->add_option("--hours", week_hours,
                         "DAYS:OPENS-CLOSES, for example MON-FRI:09:30-16:00, once for each "
                         "session; a venue with none has no regular trading day");
    set_week_command->callback([this, &context] {
      sqlite::database database = context.writable();
      set_week(database, venue_coded(database, week_venue), parse_iso_date(week_valid_from),
               parse_weekly_sessions(week_hours));
    });

    CLI::App* close_day_command =
        commands->add_subcommand("close-day", "Close a venue for whole days");
    close_day_command->add_option("--venue", closed_venue, "Venue code")->required();
    close_day_command->add_option("--date", closed_days, "Day without trading, once for each day")
        ->required();
    close_day_command->callback([this, &context] {
      sqlite::database database = context.writable();
      const venue_id venue = venue_coded(database, closed_venue);
      sqlite::transaction every_day_or_none(database);
      for (const std::string& day : closed_days) close_day(database, venue, parse_iso_date(day));
      every_day_or_none.commit();
    });

    CLI::App* set_day_command = commands->add_subcommand(
        "set-day", "Give one day its own sessions in place of the weekly ones");
    set_day_command->add_option("--venue", special_venue, "Venue code")->required();
    set_day_command->add_option("--date", special_day, "The day")->required();
    set_day_command->add_option("--session", special_sessions,
                        "OPENS-CLOSES, for example 09:30-13:00, once for each session")
        ->required();
    set_day_command->callback([this, &context] {
      sqlite::database database = context.writable();
      set_day(database, venue_coded(database, special_venue), parse_iso_date(special_day),
              parse_sessions(special_sessions));
    });

    CLI::App* reset_day_command = commands->add_subcommand(
        "reset-day", "Give days their weekly sessions back after close-day or set-day");
    reset_day_command->add_option("--venue", regular_venue, "Venue code")->required();
    reset_day_command->add_option("--date", regular_days, "The day, once for each day")->required();
    reset_day_command->callback([this, &context] {
      sqlite::database database = context.writable();
      const venue_id venue = venue_coded(database, regular_venue);
      sqlite::transaction every_day_or_none(database);
      for (const std::string& day : regular_days) reset_day(database, venue, parse_iso_date(day));
      every_day_or_none.commit();
    });

    CLI::App* rules = commands->add_subcommand(
        "rules", "Print the weekly sessions, closed days and special sessions of a venue");
    rules->add_option("--venue", rules_venue, "Venue code")->required();
    rules->callback([this, &context] {
      sqlite::database database = context.read_only();
      print_rules(context.out,
                  read_calendar_rules(database, venue_coded(database, rules_venue)));
    });

    CLI::App* list = commands->add_subcommand(
        "list", "Print the sessions of each trading day in a range of days");
    list->add_option("--venue", listed_venue, "Venue code")->required();
    list->add_option("--from", listed_from, "First day of the range")->required();
    list->add_option("--until", listed_until, "First day after the range")->required();
    list->callback([this, &context] {
      sqlite::database database = context.read_only();
      print_sessions(context.out,
                     load_calendar(database, venue_coded(database, listed_venue),
                                   {.from = parse_iso_date(listed_from),
                                    .until = parse_iso_date(listed_until)}));
    });

    CLI::App* at = commands->add_subcommand(
        "at", "Print the trading day of a UTC time, and fail if the venue is closed then");
    at->add_option("--venue", asked_venue, "Venue code")->required();
    at->add_option("--time", asked_time, "UTC time, for example 2026-01-31T14:30:00Z")
        ->required();
    at->callback([this, &context] {
      constexpr std::chrono::days reach_of_a_session{2};
      sqlite::database database = context.read_only();
      const std::chrono::sys_seconds instant = parse_iso_instant(asked_time);
      const std::chrono::sys_days utc_day = std::chrono::floor<std::chrono::days>(instant);
      const calendar loaded = load_calendar(database, venue_coded(database, asked_venue),
                                            {.from = utc_day - reach_of_a_session,
                                             .until = utc_day + reach_of_a_session});
      const std::optional<trading_day_index> trading_day = loaded.trading_day_at(instant);
      if (!trading_day) throw error(std::format("{} is closed at {}", asked_venue, asked_time));
      context.out << iso_date(loaded.day_at(*trading_day)) << '\n';
    });
  }
};

}  // namespace nq::db::cli
