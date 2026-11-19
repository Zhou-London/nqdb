#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nq/db/secmaster/adjustment.h>
#include <nq/db/secmaster/calendar.h>
#include <nq/db/secmaster/error.h>
#include <nq/db/secmaster/history.h>
#include <nq/db/secmaster/loader.h>
#include <nq/db/secmaster/records.h>
#include <nq/db/secmaster/schema.h>
#include <nq/db/sqlite.h>

namespace nq::db {

using secmaster::adjustments;
using secmaster::asset_class;
using secmaster::corporate_action;
using secmaster::corporate_action_type;
using secmaster::day_range;
using secmaster::error;
using secmaster::exercise_style;
using secmaster::future_spec;
using secmaster::instrument;
using secmaster::instrument_id;
using secmaster::instrument_spec;
using secmaster::listing_id;
using secmaster::multiplier_nanos;
using secmaster::nanos_per_unit;
using secmaster::no_spec;
using secmaster::option_right;
using secmaster::option_spec;
using secmaster::pair_spec;
using secmaster::price_nanos;
using secmaster::quantity_nanos;
using secmaster::session_hours;
using secmaster::session_instants;
using secmaster::settlement_method;
using secmaster::tick_band;
using secmaster::venue;
using secmaster::venue_symbol_source;

inline constexpr auto every_listing = [](const venue&, const instrument&) { return true; };

inline std::filesystem::path default_path() {
  const auto set = [](const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : nullptr;
  };
  if (const char* named = set("NQDB_PATH")) return named;
  const char* home = set("HOME");
  if (home == nullptr) throw error("neither NQDB_PATH nor HOME is set, name the database file");
  return std::filesystem::path(home) / ".nqdb" / "secmaster.db";
}

template <typename Index>
class handle {
 public:
  std::size_t number() const noexcept { return index_.value(); }

  friend constexpr auto operator<=>(handle, handle) noexcept = default;

 private:
  friend class reference;

  constexpr explicit handle(Index index) noexcept : index_(index) {}

  Index index_;
};

using listing = handle<secmaster::listing_index>;
using market = handle<secmaster::venue_index>;

class reference {
 public:
  template <std::predicate<const venue&, const instrument&> Universe>
  static reference load(sqlite::database& database, day_range days, Universe in_universe) {
    const sqlite::transaction one_consistent_read(database);
    secmaster::history listings = secmaster::load_history(database, days, std::move(in_universe));
    std::vector<secmaster::calendar> calendar_of_each_venue;
    for (const venue& venue : listings.venues()) {
      calendar_of_each_venue.push_back(secmaster::load_calendar(database, venue.id, days));
    }
    std::vector<std::vector<corporate_action>> actions_of_each_instrument;
    std::vector<adjustments> split_adjustments_of_each_instrument;
    for (const instrument& instrument : listings.instruments()) {
      actions_of_each_instrument.push_back(
          secmaster::read_corporate_actions(database, instrument.id));
      split_adjustments_of_each_instrument.push_back(
          secmaster::split_adjustments(actions_of_each_instrument.back()));
    }
    return reference(std::move(listings), std::move(calendar_of_each_venue),
                     std::move(actions_of_each_instrument),
                     std::move(split_adjustments_of_each_instrument));
  }

  day_range days() const noexcept { return listings_.days(); }

  std::span<const std::chrono::sys_days> change_days() const noexcept {
    return listings_.change_days();
  }

  std::size_t listing_count() const noexcept { return listings_.listings().size(); }

  listing listing_at(std::size_t number) const noexcept {
    return listing{secmaster::listing_index{static_cast<std::uint32_t>(number)}};
  }

  std::optional<listing> find(listing_id id) const noexcept {
    return listings_.listings().find(id).transform(as_listing);
  }

  std::optional<listing> find(std::string_view venue_code, std::string_view symbol,
                              std::chrono::sys_days day,
                              std::string_view symbol_source = venue_symbol_source) const noexcept {
    const std::optional<secmaster::venue_index> venue = listings_.find_venue(venue_code);
    if (!venue) return std::nullopt;
    return listings_.find_listing(symbol_source, *venue, symbol, day).transform(as_listing);
  }

  listing_id id(listing listing) const noexcept { return record_of(listing).id; }

  const instrument& instrument_of(listing listing) const noexcept {
    return listings_.instruments()[record_of(listing).instrument];
  }

  const venue& venue_of(listing listing) const noexcept {
    return listings_.venues()[record_of(listing).venue];
  }

  std::string_view currency(listing listing) const noexcept { return record_of(listing).currency; }

  bool is_primary(listing listing) const noexcept { return record_of(listing).is_primary; }

  bool is_listed(listing listing, std::chrono::sys_days day) const noexcept {
    return listings_.is_listed(listing.index_, day);
  }

  std::optional<std::string_view> symbol(
      listing listing, std::chrono::sys_days day,
      std::string_view symbol_source = venue_symbol_source) const noexcept {
    const secmaster::symbol_period* period =
        listings_.symbol_on(symbol_source, listing.index_, day);
    return period == nullptr ? std::nullopt : std::optional<std::string_view>{period->symbol};
  }

  std::optional<quantity_nanos> lot_size(listing listing,
                                         std::chrono::sys_days day) const noexcept {
    const secmaster::spec_period* spec = listings_.spec_on(listing.index_, day);
    return spec == nullptr ? std::nullopt : std::optional{spec->lot_size};
  }

  std::optional<quantity_nanos> min_qty(listing listing,
                                        std::chrono::sys_days day) const noexcept {
    const secmaster::spec_period* spec = listings_.spec_on(listing.index_, day);
    return spec == nullptr ? std::nullopt : spec->min_qty;
  }

  std::span<const tick_band> tick_bands(listing listing,
                                        std::chrono::sys_days day) const noexcept {
    const secmaster::spec_period* spec = listings_.spec_on(listing.index_, day);
    if (spec == nullptr) return {};
    return listings_.tick_rules()[spec->tick_rule].bands_by_ascending_price;
  }

  std::optional<price_nanos> tick_size(listing listing, std::chrono::sys_days day,
                                       price_nanos price) const noexcept {
    const secmaster::spec_period* spec = listings_.spec_on(listing.index_, day);
    if (spec == nullptr) return std::nullopt;
    return listings_.tick_rules()[spec->tick_rule].tick_size_at(price);
  }

  std::optional<market> find_market(std::string_view venue_code) const noexcept {
    return listings_.find_venue(venue_code).transform(as_market);
  }

  market market_of(listing listing) const noexcept { return market{record_of(listing).venue}; }

  const venue& venue_of(market market) const noexcept { return listings_.venues()[market.index_]; }

  bool is_trading_day(market market, std::chrono::sys_days day) const noexcept {
    return calendar_of(market).is_trading_day(day);
  }

  std::span<const std::chrono::sys_days> trading_days(market market) const noexcept {
    return calendar_of(market).trading_days();
  }

  std::optional<std::chrono::sys_days> next_trading_day(
      market market, std::chrono::sys_days after) const noexcept {
    return calendar_of(market).next_trading_day(after);
  }

  std::optional<std::chrono::sys_days> previous_trading_day(
      market market, std::chrono::sys_days before) const noexcept {
    return calendar_of(market).previous_trading_day(before);
  }

  std::span<const session_hours> local_sessions(market market,
                                                std::chrono::sys_days day) const noexcept {
    return calendar_of(market).sessions_on(day);
  }

  std::span<const session_instants> sessions(market market,
                                             std::chrono::sys_days day) const noexcept {
    return calendar_of(market).instants_on(day);
  }

  bool is_open(market market, std::chrono::sys_seconds utc_time) const noexcept {
    return calendar_of(market).is_open_at(utc_time);
  }

  std::optional<std::chrono::sys_days> trading_day(
      market market, std::chrono::sys_seconds utc_time) const noexcept {
    const secmaster::calendar& calendar = calendar_of(market);
    return calendar.trading_day_at(utc_time).transform(
        [&](secmaster::trading_day_index index) { return calendar.day_at(index); });
  }

  std::span<const corporate_action> corporate_actions(listing listing) const noexcept {
    return actions_of_each_instrument_[record_of(listing).instrument.value()];
  }

  double price_factor(listing listing, std::chrono::sys_days day,
                      std::optional<std::chrono::sys_days> as_of = {}) const noexcept {
    return split_adjustments_of(listing).price_factor_on(day, as_of);
  }

  double volume_factor(listing listing, std::chrono::sys_days day,
                       std::optional<std::chrono::sys_days> as_of = {}) const noexcept {
    return split_adjustments_of(listing).volume_factor_on(day, as_of);
  }

  template <typename CloseBefore>
    requires std::is_invocable_r_v<std::optional<price_nanos>, CloseBefore, std::chrono::sys_days>
  adjustments adjustments_with_dividends(listing listing, CloseBefore close_before) const {
    return secmaster::split_and_dividend_adjustments(corporate_actions(listing),
                                                     currency(listing), std::move(close_before));
  }

 private:
  reference(secmaster::history listings, std::vector<secmaster::calendar> calendar_of_each_venue,
            std::vector<std::vector<corporate_action>> actions_of_each_instrument,
            std::vector<adjustments> split_adjustments_of_each_instrument)
      : listings_(std::move(listings)),
        calendar_of_each_venue_(std::move(calendar_of_each_venue)),
        actions_of_each_instrument_(std::move(actions_of_each_instrument)),
        split_adjustments_of_each_instrument_(std::move(split_adjustments_of_each_instrument)) {}

  static listing as_listing(secmaster::listing_index index) noexcept { return listing{index}; }
  static market as_market(secmaster::venue_index index) noexcept { return market{index}; }

  const secmaster::listing& record_of(listing listing) const noexcept {
    return listings_.listings()[listing.index_];
  }

  const secmaster::calendar& calendar_of(market market) const noexcept {
    return calendar_of_each_venue_[market.index_.value()];
  }

  const adjustments& split_adjustments_of(listing listing) const noexcept {
    return split_adjustments_of_each_instrument_[record_of(listing).instrument.value()];
  }

  secmaster::history listings_;
  std::vector<secmaster::calendar> calendar_of_each_venue_;
  std::vector<std::vector<corporate_action>> actions_of_each_instrument_;
  std::vector<adjustments> split_adjustments_of_each_instrument_;
};

template <std::predicate<const venue&, const instrument&> Universe = decltype(every_listing)>
reference open(const std::filesystem::path& file, day_range days,
               Universe in_universe = every_listing) {
  sqlite::database database = secmaster::open_read_only(file);
  return reference::load(database, days, std::move(in_universe));
}

}  // namespace nq::db
