#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nq/db/secmaster/calendar.h>
#include <nq/db/secmaster/error.h>
#include <nq/db/secmaster/history.h>
#include <nq/db/secmaster/records.h>
#include <nq/db/secmaster/text.h>
#include <nq/db/sqlite.h>

namespace nq::db::secmaster {

namespace detail {

inline history::venue_table read_venues(sqlite::database& database) {
  history::venue_table venues;
  sqlite::statement row = database.prepare(
      "SELECT venue_id, code, mic, name, country, timezone FROM venue ORDER BY venue_id");
  while (row.next_row()) {
    venues.insert(venue{.id = row.get<venue_id>(0),
                        .code = row.get<std::string>(1),
                        .mic = row.get<std::optional<std::string>>(2),
                        .name = row.get<std::string>(3),
                        .country = row.get<std::optional<std::string>>(4),
                        .timezone = row.get<std::string>(5)});
  }
  return venues;
}

class instrument_reader {
 public:
  explicit instrument_reader(sqlite::database& database)
      : instrument_(database.prepare(
            "SELECT asset_class, name FROM instrument WHERE instrument_id = ?1")),
        future_spec_(database.prepare(
            "SELECT underlying_id, root, expiry, multiplier_nanos, settlement "
            "FROM future_spec WHERE instrument_id = ?1")),
        option_spec_(database.prepare(
            "SELECT underlying_id, expiry, strike_nanos, option_right, exercise_style, "
            "multiplier_nanos FROM option_spec WHERE instrument_id = ?1")),
        pair_spec_(database.prepare(
            "SELECT base_currency, quote_currency FROM pair_spec WHERE instrument_id = ?1")) {}

  instrument read(instrument_id id) {
    seek(instrument_, id, "instrument");
    const asset_class asset_class =
        enum_named(instrument_.get<std::string_view>(0), asset_class_names);
    return instrument{.id = id,
                      .asset_class = asset_class,
                      .name = instrument_.get<std::string>(1),
                      .spec = read_spec(asset_class, id)};
  }

 private:
  instrument_spec read_spec(asset_class asset_class, instrument_id id) {
    if (describes<future_spec>(asset_class)) return read_future_spec(id);
    if (describes<option_spec>(asset_class)) return read_option_spec(id);
    if (describes<pair_spec>(asset_class)) return read_pair_spec(id);
    return no_spec{};
  }

  future_spec read_future_spec(instrument_id id) {
    seek(future_spec_, id, "future_spec");
    return {.underlying = future_spec_.get<instrument_id>(0),
            .root = future_spec_.get<std::string>(1),
            .expiry = parse_iso_date(future_spec_.get<std::string_view>(2)),
            .multiplier = future_spec_.get<multiplier_nanos>(3),
            .settlement =
                enum_named(future_spec_.get<std::string_view>(4), settlement_method_names)};
  }

  option_spec read_option_spec(instrument_id id) {
    seek(option_spec_, id, "option_spec");
    return {.underlying = option_spec_.get<instrument_id>(0),
            .expiry = parse_iso_date(option_spec_.get<std::string_view>(1)),
            .strike = option_spec_.get<price_nanos>(2),
            .right = enum_named(option_spec_.get<std::string_view>(3), option_right_names),
            .style = enum_named(option_spec_.get<std::string_view>(4), exercise_style_names),
            .multiplier = option_spec_.get<multiplier_nanos>(5)};
  }

  pair_spec read_pair_spec(instrument_id id) {
    seek(pair_spec_, id, "pair_spec");
    return {.base_currency = pair_spec_.get<std::string>(0),
            .quote_currency = pair_spec_.get<std::string>(1)};
  }

  static void seek(sqlite::statement& row, instrument_id id, std::string_view table) {
    row.rewind();
    row.bind(1, id);
    if (!row.next_row()) {
      throw error(std::format("instrument {} has no {} row", id.value(), table));
    }
  }

  sqlite::statement instrument_;
  sqlite::statement future_spec_;
  sqlite::statement option_spec_;
  sqlite::statement pair_spec_;
};

template <typename Universe>
std::optional<instrument_index> index_if_in_universe(history::instrument_table& instruments,
                                                     instrument_reader& instrument_reader,
                                                     instrument_id id, const venue& venue,
                                                     Universe& in_universe) {
  if (const std::optional<instrument_index> loaded = instruments.find(id)) {
    return in_universe(venue, instruments[*loaded]) ? loaded : std::nullopt;
  }
  instrument unloaded = instrument_reader.read(id);
  if (!in_universe(venue, unloaded)) return std::nullopt;
  return instruments.insert(std::move(unloaded));
}

inline constexpr std::string_view overlaps_days =
    "valid_from < ?2 AND (valid_until IS NULL OR ?1 < valid_until)";

inline validity read_validity(const sqlite::statement& row, int from_column, int until_column) {
  return {.from = parse_iso_date(row.get<std::string_view>(from_column)),
          .until = row.is_null(until_column)
                       ? std::nullopt
                       : std::optional{parse_iso_date(row.get<std::string_view>(until_column))}};
}

struct listed_instruments {
  history::instrument_table instruments;
  history::listing_table listings;
};

template <std::predicate<const venue&, const instrument&> Universe>
listed_instruments read_listings(sqlite::database& database, day_range days,
                                                  const history::venue_table& venues,
                                                  Universe in_universe) {
  sqlite::statement row = database.prepare(
      "SELECT listing_id, instrument_id, venue_id, currency, is_primary, listed_on, delisted_on "
      "FROM listing "
      "WHERE listed_on < ?2 AND (delisted_on IS NULL OR ?1 < delisted_on) "
      "ORDER BY listing_id");
  row.bind(1, iso_date(days.from)).bind(2, iso_date(days.until));

  listed_instruments listed;
  instrument_reader instrument_reader(database);
  while (row.next_row()) {
    const venue_index venue = *venues.find(row.get<venue_id>(2));
    const std::optional<instrument_index> instrument = index_if_in_universe(
        listed.instruments, instrument_reader, row.get<instrument_id>(1), venues[venue],
        in_universe);
    if (!instrument) continue;
    listed.listings.insert(listing{.id = row.get<listing_id>(0),
                                          .instrument = *instrument,
                                          .venue = venue,
                                          .currency = row.get<std::string>(3),
                                          .is_primary = row.get<bool>(4),
                                          .listed = read_validity(row, 5, 6)});
  }
  return listed;
}

inline history::symbol_periods_by_source read_symbol_periods(
    sqlite::database& database, day_range days, const history::listing_table& listings) {
  history::symbol_periods_by_source symbols;
  sqlite::statement row = database.prepare(
      std::format("SELECT source, listing_id, valid_from, valid_until, symbol FROM listing_symbol "
                  "WHERE {} ORDER BY listing_id, valid_from",
                  overlaps_days));
  row.bind(1, iso_date(days.from)).bind(2, iso_date(days.until));
  while (row.next_row()) {
    if (const auto loaded = listings.find(row.get<listing_id>(1))) {
      symbols.try_emplace(row.get<std::string>(0), listings.size())
          .first->second.append_in_listing_then_day_order(
              *loaded, {.valid = read_validity(row, 2, 3), .symbol = row.get<std::string>(4)});
    }
  }
  return symbols;
}

inline history::spec_periods read_spec_periods(sqlite::database& database, day_range days,
                                               const history::listing_table& listings,
                                               const history::tick_rule_table& tick_rules) {
  history::spec_periods specs(listings.size());
  sqlite::statement row = database.prepare(
      std::format("SELECT listing_id, valid_from, valid_until, lot_size_nanos, min_qty_nanos, "
                  "tick_rule_id FROM listing_spec WHERE {} ORDER BY listing_id, valid_from",
                  overlaps_days));
  row.bind(1, iso_date(days.from)).bind(2, iso_date(days.until));
  while (row.next_row()) {
    if (const auto loaded = listings.find(row.get<listing_id>(0))) {
      specs.append_in_listing_then_day_order(
          *loaded, {.valid = read_validity(row, 1, 2),
                    .lot_size = row.get<quantity_nanos>(3),
                    .min_qty = row.get<std::optional<quantity_nanos>>(4),
                    .tick_rule = *tick_rules.find(row.get<tick_rule_id>(5))});
    }
  }
  return specs;
}

template <typename Period>
std::optional<std::chrono::sys_days> first_day_without_a_period(
    std::span<const Period> periods_by_ascending_day, day_range days) {
  std::chrono::sys_days first_unchecked_day = days.from;
  for (const Period& period : periods_by_ascending_day) {
    if (first_unchecked_day < period.valid.from) return first_unchecked_day;
    if (!period.valid.until || days.until <= *period.valid.until) return std::nullopt;
    first_unchecked_day = std::max(first_unchecked_day, *period.valid.until);
  }
  return first_unchecked_day;
}

inline void require_a_symbol_and_a_spec_on_every_listed_day(const history& loaded) {
  for (const listing& listing : loaded.listings()) {
    const listing_index index = *loaded.listings().find(listing.id);
    const day_range listed_days{
        .from = std::max(loaded.days().from, listing.listed.from),
        .until = listing.listed.until ? std::min(loaded.days().until, *listing.listed.until)
                                      : loaded.days().until};
    if (const auto day = first_day_without_a_period(
            loaded.symbols_of(venue_symbol_source, index), listed_days)) {
      throw error(std::format("listing {} has no {} symbol on {}", listing.id.value(),
                              venue_symbol_source, *day));
    }
    if (const auto day = first_day_without_a_period(loaded.specs_of(index), listed_days)) {
      throw error(std::format("listing {} has no listing_spec on {}", listing.id.value(), *day));
    }
  }
}

inline std::vector<tick_band> read_tick_bands(sqlite::database& database, tick_rule_id rule) {
  std::vector<tick_band> bands_by_ascending_price;
  sqlite::statement row = database.prepare(
      "SELECT price_from_nanos, tick_size_nanos FROM tick_band "
      "WHERE tick_rule_id = ?1 ORDER BY price_from_nanos");
  row.bind(1, rule);
  while (row.next_row()) {
    bands_by_ascending_price.push_back(
        {.price_from = row.get<price_nanos>(0), .tick_size = row.get<price_nanos>(1)});
  }
  if (bands_by_ascending_price.empty()) {
    throw error(std::format("tick_rule {} has no tick_band", rule.value()));
  }
  return bands_by_ascending_price;
}

}  // namespace detail

inline history::tick_rule_table read_tick_rules(sqlite::database& database) {
  history::tick_rule_table tick_rules;
  sqlite::statement row =
      database.prepare("SELECT tick_rule_id, name FROM tick_rule ORDER BY tick_rule_id");
  while (row.next_row()) {
    const auto id = row.get<tick_rule_id>(0);
    tick_rules.insert(tick_rule{.id = id,
                                .name = row.get<std::string>(1),
                                .bands_by_ascending_price = detail::read_tick_bands(database, id)});
  }
  return tick_rules;
}

inline std::vector<issuer> read_issuers(sqlite::database& database) {
  std::vector<issuer> issuers;
  sqlite::statement row =
      database.prepare("SELECT issuer_id, name, lei, country FROM issuer ORDER BY issuer_id");
  while (row.next_row()) {
    issuers.push_back({.id = row.get<issuer_id>(0),
                       .name = row.get<std::string>(1),
                       .lei = row.get<std::optional<std::string>>(2),
                       .country = row.get<std::optional<std::string>>(3)});
  }
  return issuers;
}

inline std::vector<corporate_action> read_corporate_actions(
    sqlite::database& database, std::optional<instrument_id> only_instrument = {}) {
  const auto optional_day = [](const sqlite::statement& row, int column) {
    return row.is_null(column)
               ? std::nullopt
               : std::optional{parse_iso_date(row.get<std::string_view>(column))};
  };
  std::vector<corporate_action> actions;
  sqlite::statement row = database.prepare(
      "SELECT corporate_action_id, instrument_id, action_type, ex_date, announce_date, "
      "       record_date, pay_date, cash_per_share_nanos, currency, new_shares, old_shares, "
      "       related_instrument_id, recorded_at "
      "FROM corporate_action WHERE ?1 IS NULL OR instrument_id = ?1 "
      "ORDER BY instrument_id, ex_date, corporate_action_id");
  row.bind(1, only_instrument);
  while (row.next_row()) {
    actions.push_back(
        {.id = row.get<corporate_action_id>(0),
         .instrument = row.get<instrument_id>(1),
         .type = enum_named(row.get<std::string_view>(2), corporate_action_type_names),
         .ex_date = parse_iso_date(row.get<std::string_view>(3)),
         .announce_date = optional_day(row, 4),
         .record_date = optional_day(row, 5),
         .pay_date = optional_day(row, 6),
         .cash_per_share = row.get<std::optional<price_nanos>>(7),
         .currency = row.get<std::optional<std::string>>(8),
         .new_shares = row.get<std::optional<std::int64_t>>(9),
         .old_shares = row.get<std::optional<std::int64_t>>(10),
         .related_instrument = row.get<std::optional<instrument_id>>(11),
         .recorded_at = row.get<std::string>(12)});
  }
  return actions;
}

inline std::vector<change> read_changes(sqlite::database& database,
                                        std::optional<std::string_view> only_table = {}) {
  std::vector<change> changes;
  sqlite::statement row = database.prepare(
      "SELECT change_id, changed_at, table_name, operation, old_row, new_row FROM change_log "
      "WHERE ?1 IS NULL OR table_name = ?1 ORDER BY change_id");
  row.bind(1, only_table);
  while (row.next_row()) {
    changes.push_back({.id = row.get<std::int64_t>(0),
                       .changed_at = row.get<std::string>(1),
                       .table = row.get<std::string>(2),
                       .operation = row.get<std::string>(3),
                       .old_row = row.get<std::optional<std::string>>(4),
                       .new_row = row.get<std::optional<std::string>>(5)});
  }
  return changes;
}

inline std::vector<instrument> read_instruments(sqlite::database& database) {
  std::vector<instrument> instruments;
  detail::instrument_reader instrument_reader(database);
  sqlite::statement row =
      database.prepare("SELECT instrument_id FROM instrument ORDER BY instrument_id");
  while (row.next_row()) instruments.push_back(instrument_reader.read(row.get<instrument_id>(0)));
  return instruments;
}

template <std::predicate<const venue&, const instrument&> Universe>
history load_history(sqlite::database& database, day_range days, Universe in_universe) {
  if (!(days.from < days.until)) {
    throw error(std::format("a history from {} until {} has no day", days.from, days.until));
  }
  const sqlite::transaction one_consistent_read(database);
  history::venue_table venues = detail::read_venues(database);
  history::tick_rule_table tick_rules = read_tick_rules(database);
  detail::listed_instruments listed =
      detail::read_listings(database, days, venues, std::move(in_universe));
  history::symbol_periods_by_source symbols =
      detail::read_symbol_periods(database, days, listed.listings);
  history::spec_periods specs =
      detail::read_spec_periods(database, days, listed.listings, tick_rules);
  history loaded(days, std::move(venues), std::move(listed.instruments), std::move(tick_rules),
                 std::move(listed.listings), std::move(symbols), std::move(specs));
  detail::require_a_symbol_and_a_spec_on_every_listed_day(loaded);
  return loaded;
}

inline calendar_rules read_calendar_rules(sqlite::database& database, venue_id venue) {
  const auto hours_at = [](const sqlite::statement& row, int opens_column) {
    return session_hours{.opens = std::chrono::minutes{row.get<std::int64_t>(opens_column)},
                         .closes = std::chrono::minutes{row.get<std::int64_t>(opens_column + 1)}};
  };
  calendar_rules rules;

  sqlite::statement weekly = database.prepare(
      "SELECT valid_from, valid_until, weekday, opens_minute, closes_minute FROM weekly_session "
      "WHERE venue_id = ?1 ORDER BY valid_from, weekday, opens_minute");
  weekly.bind(1, venue);
  while (weekly.next_row()) {
    rules.weekly_sessions.push_back(
        {.valid = detail::read_validity(weekly, 0, 1),
         .session = {.weekday = std::chrono::weekday{weekly.get<unsigned>(2)},
                     .hours = hours_at(weekly, 3)}});
  }

  sqlite::statement closed =
      database.prepare("SELECT day FROM closed_day WHERE venue_id = ?1 ORDER BY day");
  closed.bind(1, venue);
  while (closed.next_row()) {
    rules.closed_days.push_back(parse_iso_date(closed.get<std::string_view>(0)));
  }

  sqlite::statement special = database.prepare(
      "SELECT day, opens_minute, closes_minute FROM special_session "
      "WHERE venue_id = ?1 ORDER BY day, opens_minute");
  special.bind(1, venue);
  while (special.next_row()) {
    rules.special_sessions.push_back({.day = parse_iso_date(special.get<std::string_view>(0)),
                                      .hours = hours_at(special, 1)});
  }
  return rules;
}

inline calendar load_calendar(sqlite::database& database, venue_id venue, day_range days) {
  if (!(days.from < days.until)) {
    throw error(std::format("a calendar from {} until {} has no day", days.from, days.until));
  }
  const sqlite::transaction one_consistent_read(database);
  sqlite::statement stored = database.prepare("SELECT timezone FROM venue WHERE venue_id = ?1");
  stored.bind(1, venue);
  if (!stored.next_row()) throw error(std::format("there is no venue {}", venue.value()));
  return calendar(venue, zone_named(stored.get<std::string_view>(0)), days,
                  read_calendar_rules(database, venue));
}

}  // namespace nq::db::secmaster
