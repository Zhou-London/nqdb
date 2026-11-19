#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <nq/db/secmaster/error.h>
#include <nq/db/secmaster/history.h>
#include <nq/db/secmaster/records.h>
#include <nq/db/secmaster/time_zone.h>

namespace nq::db::secmaster {

struct weekly_session_period {
  validity valid;
  weekly_session session;

  friend bool operator==(const weekly_session_period&, const weekly_session_period&) = default;
};

struct special_session {
  std::chrono::sys_days day;
  session_hours hours;

  friend bool operator==(const special_session&, const special_session&) = default;
};

struct calendar_rules {
  std::vector<weekly_session_period> weekly_sessions;
  std::vector<std::chrono::sys_days> closed_days;
  std::vector<special_session> special_sessions;
};

inline void require_sessions_without_overlap(std::span<const session_hours> by_ascending_open,
                                             std::chrono::sys_days day) {
  const auto overlapping = std::ranges::adjacent_find(
      by_ascending_open, [](const session_hours& earlier, const session_hours& later) {
        return later.opens < earlier.closes;
      });
  if (overlapping != by_ascending_open.end()) {
    throw error(std::format("two sessions overlap on {}", day));
  }
}

class calendar {
 public:
  calendar(venue_id venue, time_zone zone, day_range days, calendar_rules rules)
      : venue_(venue), zone_(std::move(zone)), days_(days) {
    std::ranges::sort(rules.closed_days);
    std::ranges::sort(rules.special_sessions, {}, &special_session::day);
    first_session_of_.push_back(0);
    for (std::chrono::sys_days day = days.from; day < days.until; day += std::chrono::days{1}) {
      const std::size_t sessions_before = sessions_.size();
      append_sessions_on(day, rules);
      if (sessions_.size() == sessions_before) continue;
      const std::span todays = std::span{sessions_}.subspan(sessions_before);
      std::ranges::sort(todays, {}, &session_hours::opens);
      require_sessions_without_overlap(todays, day);
      for (const session_hours& hours : todays) {
        instants_.push_back({.opens = utc_instant(zone_, day, hours.opens),
                             .closes = utc_instant(zone_, day, hours.closes)});
      }
      const bool opens_before_the_previous_day_closes =
          sessions_before > 0 &&
          instants_[sessions_before].opens < instants_[sessions_before - 1].closes;
      if (opens_before_the_previous_day_closes) {
        throw error(std::format("a session of {} opens before the trading day before it closes",
                                day));
      }
      trading_days_.push_back(day);
      first_session_of_.push_back(static_cast<std::uint32_t>(sessions_.size()));
    }
  }

  venue_id venue() const noexcept { return venue_; }
  const time_zone& zone() const noexcept { return zone_; }
  day_range days() const noexcept { return days_; }

  std::span<const std::chrono::sys_days> trading_days() const noexcept { return trading_days_; }

  std::optional<trading_day_index> index_of(std::chrono::sys_days day) const noexcept {
    const auto found = std::ranges::lower_bound(trading_days_, day);
    if (found == trading_days_.end() || *found != day) return std::nullopt;
    return trading_day_index{static_cast<std::uint32_t>(found - trading_days_.begin())};
  }

  bool is_trading_day(std::chrono::sys_days day) const noexcept {
    return index_of(day).has_value();
  }

  std::chrono::sys_days day_at(trading_day_index index) const noexcept {
    return trading_days_[index.value()];
  }

  std::span<const session_hours> sessions_of(trading_day_index index) const noexcept {
    const std::uint32_t first = first_session_of_[index.value()];
    return std::span{sessions_}.subspan(first, first_session_of_[index.value() + 1] - first);
  }

  std::span<const session_hours> sessions_on(std::chrono::sys_days day) const noexcept {
    const std::optional<trading_day_index> index = index_of(day);
    return index ? sessions_of(*index) : std::span<const session_hours>{};
  }

  std::span<const session_instants> instants_of(trading_day_index index) const noexcept {
    const std::uint32_t first = first_session_of_[index.value()];
    return std::span{instants_}.subspan(first, first_session_of_[index.value() + 1] - first);
  }

  std::span<const session_instants> instants_on(std::chrono::sys_days day) const noexcept {
    const std::optional<trading_day_index> index = index_of(day);
    return index ? instants_of(*index) : std::span<const session_instants>{};
  }

  std::optional<trading_day_index> trading_day_at(std::chrono::sys_seconds instant) const noexcept {
    const auto first_to_open_later =
        std::ranges::upper_bound(instants_, instant, {}, &session_instants::opens);
    if (first_to_open_later == instants_.begin()) return std::nullopt;
    const auto latest_to_open = std::prev(first_to_open_later);
    if (latest_to_open->closes <= instant) return std::nullopt;
    const auto session_number = static_cast<std::uint32_t>(latest_to_open - instants_.begin());
    const auto first_session_of_the_next_day =
        std::ranges::upper_bound(first_session_of_, session_number);
    return trading_day_index{
        static_cast<std::uint32_t>(first_session_of_the_next_day - first_session_of_.begin()) - 1};
  }

  bool is_open_at(std::chrono::sys_seconds instant) const noexcept {
    return trading_day_at(instant).has_value();
  }

  std::optional<std::chrono::sys_days> next_trading_day(
      std::chrono::sys_days after) const noexcept {
    const auto next = std::ranges::upper_bound(trading_days_, after);
    return next == trading_days_.end() ? std::nullopt : std::optional{*next};
  }

  std::optional<std::chrono::sys_days> previous_trading_day(
      std::chrono::sys_days before) const noexcept {
    const auto first_not_before = std::ranges::lower_bound(trading_days_, before);
    return first_not_before == trading_days_.begin() ? std::nullopt
                                                     : std::optional{*std::prev(first_not_before)};
  }

 private:
  void append_sessions_on(std::chrono::sys_days day, const calendar_rules& rules) {
    if (std::ranges::binary_search(rules.closed_days, day)) return;
    const auto special = std::ranges::equal_range(rules.special_sessions, day, {},
                                                  &special_session::day);
    if (!special.empty()) {
      for (const special_session& session : special) sessions_.push_back(session.hours);
      return;
    }
    const std::chrono::weekday weekday{day};
    for (const weekly_session_period& period : rules.weekly_sessions) {
      if (period.session.weekday == weekday && period.valid.covers(day)) {
        sessions_.push_back(period.session.hours);
      }
    }
  }

  venue_id venue_;
  time_zone zone_;
  day_range days_;
  std::vector<std::chrono::sys_days> trading_days_;
  std::vector<std::uint32_t> first_session_of_;
  std::vector<session_hours> sessions_;
  std::vector<session_instants> instants_;
};

}  // namespace nq::db::secmaster
