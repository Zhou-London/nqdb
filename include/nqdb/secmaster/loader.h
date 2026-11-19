#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nqdb/secmaster/date.h>
#include <nqdb/secmaster/error.h>
#include <nqdb/secmaster/records.h>
#include <nqdb/secmaster/schema.h>
#include <nqdb/secmaster/snapshot.h>
#include <nqdb/sqlite.h>

namespace nqdb::secmaster {

namespace detail {

inline snapshot::venue_table read_venues(sqlite::database& database) {
  snapshot::venue_table venues;
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

inline snapshot::tick_rule_table read_tick_rules(sqlite::database& database) {
  snapshot::tick_rule_table tick_rules;
  sqlite::statement row =
      database.prepare("SELECT tick_rule_id, name FROM tick_rule ORDER BY tick_rule_id");
  while (row.next_row()) {
    const auto id = row.get<tick_rule_id>(0);
    tick_rules.insert(tick_rule{.id = id,
                                .name = row.get<std::string>(1),
                                .bands_by_ascending_price = read_tick_bands(database, id)});
  }
  return tick_rules;
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

struct listed_instruments {
  snapshot::instrument_table instruments;
  snapshot::listing_table listings;
};

template <std::predicate<const venue&, const instrument&> Universe>
listed_instruments read_listings(sqlite::database& database, std::chrono::sys_days as_of,
                                 const snapshot::venue_table& venues,
                                 const snapshot::tick_rule_table& tick_rules,
                                 Universe in_universe) {
  enum column : int {
    listing_id_column,
    instrument_id_column,
    venue_id_column,
    currency_column,
    is_primary_column,
    venue_symbol_column,
    lot_size_column,
    min_qty_column,
    tick_rule_id_column,
  };
  sqlite::statement row = database.prepare(
      "SELECT listing.listing_id, listing.instrument_id, listing.venue_id, listing.currency, "
      "       listing.is_primary, venue_symbol.symbol, "
      "       spec.lot_size_nanos, spec.min_qty_nanos, spec.tick_rule_id "
      "FROM listing "
      "LEFT JOIN listing_symbol venue_symbol "
      "  ON venue_symbol.listing_id = listing.listing_id AND venue_symbol.source = ?2 "
      "  AND venue_symbol.valid_from <= ?1 "
      "  AND (venue_symbol.valid_until IS NULL OR ?1 < venue_symbol.valid_until) "
      "LEFT JOIN listing_spec spec "
      "  ON spec.listing_id = listing.listing_id AND spec.valid_from <= ?1 "
      "  AND (spec.valid_until IS NULL OR ?1 < spec.valid_until) "
      "WHERE listing.listed_on <= ?1 AND (listing.delisted_on IS NULL OR ?1 < listing.delisted_on) "
      "ORDER BY listing.listing_id");
  row.bind(1, iso_date(as_of)).bind(2, venue_symbol_source);

  listed_instruments listed;
  instrument_reader instrument_reader(database);
  while (row.next_row()) {
    const auto id = row.get<listing_id>(listing_id_column);
    const auto listed_instrument = row.get<instrument_id>(instrument_id_column);
    const venue_index venue = *venues.find(row.get<venue_id>(venue_id_column));

    const std::optional<instrument_index> loaded = listed.instruments.find(listed_instrument);
    std::optional<instrument> unloaded;
    const instrument& candidate =
        loaded ? listed.instruments[*loaded]
               : unloaded.emplace(instrument_reader.read(listed_instrument));
    if (!in_universe(venues[venue], candidate)) continue;

    const bool has_venue_symbol = !row.is_null(venue_symbol_column);
    const bool has_listing_spec = !row.is_null(lot_size_column);
    if (!has_venue_symbol) {
      throw error(std::format("listing {} has no {} symbol on {}", id.value(),
                              venue_symbol_source, as_of));
    }
    if (!has_listing_spec) {
      throw error(std::format("listing {} has no listing_spec on {}", id.value(), as_of));
    }
    listed.listings.insert(listing{
        .id = id,
        .instrument = loaded ? *loaded : listed.instruments.insert(std::move(*unloaded)),
        .venue = venue,
        .symbol = row.get<std::string>(venue_symbol_column),
        .currency = row.get<std::string>(currency_column),
        .is_primary = row.get<bool>(is_primary_column),
        .lot_size = row.get<quantity_nanos>(lot_size_column),
        .min_qty = row.get<std::optional<quantity_nanos>>(min_qty_column),
        .tick_rule = *tick_rules.find(row.get<tick_rule_id>(tick_rule_id_column))});
  }
  return listed;
}

inline snapshot::symbol_map read_symbols(sqlite::database& database, std::chrono::sys_days as_of,
                                         const snapshot::listing_table& listings) {
  snapshot::symbol_map listing_by_symbol;
  sqlite::statement row = database.prepare(
      "SELECT source, listing_id, symbol FROM listing_symbol "
      "WHERE valid_from <= ?1 AND (valid_until IS NULL OR ?1 < valid_until)");
  row.bind(1, iso_date(as_of));
  while (row.next_row()) {
    if (const auto loaded = listings.find(row.get<listing_id>(1))) {
      listing_by_symbol.emplace(
          snapshot::symbol_key{row.get<std::string>(0), listings[*loaded].venue,
                               row.get<std::string>(2)},
          *loaded);
    }
  }
  return listing_by_symbol;
}

}  // namespace detail

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

inline std::vector<instrument> read_instruments(sqlite::database& database) {
  std::vector<instrument> instruments;
  detail::instrument_reader instrument_reader(database);
  sqlite::statement row =
      database.prepare("SELECT instrument_id FROM instrument ORDER BY instrument_id");
  while (row.next_row()) instruments.push_back(instrument_reader.read(row.get<instrument_id>(0)));
  return instruments;
}

template <std::predicate<const venue&, const instrument&> Universe>
snapshot load_snapshot(sqlite::database& database, std::chrono::sys_days as_of,
                       Universe in_universe) {
  const sqlite::transaction one_consistent_read(database);
  require_schema_version(database);
  snapshot::venue_table venues = detail::read_venues(database);
  snapshot::tick_rule_table tick_rules = detail::read_tick_rules(database);
  detail::listed_instruments listed =
      detail::read_listings(database, as_of, venues, tick_rules, std::move(in_universe));
  snapshot::symbol_map listing_by_symbol = detail::read_symbols(database, as_of, listed.listings);
  return snapshot(as_of, std::move(venues), std::move(listed.instruments), std::move(tick_rules),
                  std::move(listed.listings), std::move(listing_by_symbol));
}

}  // namespace nqdb::secmaster
