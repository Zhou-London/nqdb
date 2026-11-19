#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include <nqdb/secmaster/date.h>
#include <nqdb/secmaster/error.h>
#include <nqdb/secmaster/records.h>
#include <nqdb/secmaster/snapshot.h>
#include <nqdb/sqlite.h>

namespace nqdb::secmaster {

struct new_venue {
  std::string code;
  std::optional<std::string> mic;
  std::string name;
  std::optional<std::string> country;
  std::string timezone;
};

struct new_issuer {
  std::string name;
  std::optional<std::string> lei;
  std::optional<std::string> country;
};

struct new_instrument {
  secmaster::asset_class asset_class;
  std::string name;
  std::optional<issuer_id> issuer;
  std::optional<std::string> cfi;
  instrument_spec spec;
};

struct listing_spec {
  quantity_nanos lot_size;
  std::optional<quantity_nanos> min_qty;
  tick_rule_id tick_rule;
};

struct new_listing {
  instrument_id instrument;
  venue_id venue;
  std::string currency;
  bool is_primary;
  std::chrono::sys_days listed_on;
  std::string symbol;
  listing_spec spec;
};

struct new_corporate_action {
  instrument_id instrument;
  corporate_action_type type;
  std::chrono::sys_days ex_date;
  std::optional<std::chrono::sys_days> announce_date;
  std::optional<std::chrono::sys_days> record_date;
  std::optional<std::chrono::sys_days> pay_date;
  std::optional<price_nanos> cash_per_share;
  std::optional<std::string> currency;
  std::optional<std::int64_t> new_shares;
  std::optional<std::int64_t> old_shares;
  std::optional<instrument_id> related_instrument;
};

namespace detail {

inline std::optional<std::string> optional_iso_date(std::optional<std::chrono::sys_days> day) {
  return day.transform([](std::chrono::sys_days known) { return secmaster::iso_date(known); });
}

template <typename Id>
std::optional<Id> find_id(sqlite::database& database, std::string_view select_id_by_text,
                          std::string_view text) {
  sqlite::statement row = database.prepare(select_id_by_text);
  row.bind(1, text);
  return row.next_row() ? std::optional{row.get<Id>(0)} : std::nullopt;
}

inline void insert_spec(sqlite::database&, instrument_id, const no_spec&) {}

inline void insert_spec(sqlite::database& database, instrument_id id, const future_spec& spec) {
  database
      .prepare("INSERT INTO future_spec (instrument_id, underlying_id, root, expiry, "
               "multiplier_nanos, settlement) VALUES (?1, ?2, ?3, ?4, ?5, ?6)")
      .bind(1, id)
      .bind(2, spec.underlying)
      .bind(3, spec.root)
      .bind(4, secmaster::iso_date(spec.expiry))
      .bind(5, spec.multiplier)
      .bind(6, name_of(spec.settlement, settlement_method_names))
      .run();
}

inline void insert_spec(sqlite::database& database, instrument_id id, const option_spec& spec) {
  database
      .prepare("INSERT INTO option_spec (instrument_id, underlying_id, expiry, strike_nanos, "
               "option_right, exercise_style, multiplier_nanos) "
               "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)")
      .bind(1, id)
      .bind(2, spec.underlying)
      .bind(3, secmaster::iso_date(spec.expiry))
      .bind(4, spec.strike)
      .bind(5, name_of(spec.right, option_right_names))
      .bind(6, name_of(spec.style, exercise_style_names))
      .bind(7, spec.multiplier)
      .run();
}

inline void insert_spec(sqlite::database& database, instrument_id id, const pair_spec& spec) {
  database
      .prepare("INSERT INTO pair_spec (instrument_id, base_currency, quote_currency) "
               "VALUES (?1, ?2, ?3)")
      .bind(1, id)
      .bind(2, spec.base_currency)
      .bind(3, spec.quote_currency)
      .run();
}

inline void insert_symbol(sqlite::database& database, listing_id listing,
                          std::string_view symbol_source, std::string_view symbol,
                          std::chrono::sys_days valid_from) {
  database
      .prepare("INSERT INTO listing_symbol (listing_id, source, symbol, valid_from) "
               "VALUES (?1, ?2, ?3, ?4)")
      .bind(1, listing)
      .bind(2, symbol_source)
      .bind(3, symbol)
      .bind(4, secmaster::iso_date(valid_from))
      .run();
}

inline void insert_listing_spec(sqlite::database& database, listing_id listing,
                                const listing_spec& spec, std::chrono::sys_days valid_from) {
  database
      .prepare("INSERT INTO listing_spec (listing_id, valid_from, lot_size_nanos, min_qty_nanos, "
               "tick_rule_id) VALUES (?1, ?2, ?3, ?4, ?5)")
      .bind(1, listing)
      .bind(2, secmaster::iso_date(valid_from))
      .bind(3, spec.lot_size)
      .bind(4, spec.min_qty)
      .bind(5, spec.tick_rule)
      .run();
}

}  // namespace detail

inline std::optional<venue_id> find_venue(sqlite::database& database, std::string_view code) {
  return detail::find_id<venue_id>(database, "SELECT venue_id FROM venue WHERE code = ?1", code);
}

inline std::optional<tick_rule_id> find_tick_rule(sqlite::database& database,
                                                  std::string_view name) {
  return detail::find_id<tick_rule_id>(
      database, "SELECT tick_rule_id FROM tick_rule WHERE name = ?1", name);
}

inline venue_id add_venue(sqlite::database& database, const new_venue& venue) {
  database
      .prepare("INSERT INTO venue (code, mic, name, country, timezone) "
               "VALUES (?1, ?2, ?3, ?4, ?5)")
      .bind(1, venue.code)
      .bind(2, venue.mic)
      .bind(3, venue.name)
      .bind(4, venue.country)
      .bind(5, venue.timezone)
      .run();
  return venue_id{database.last_inserted_id()};
}

inline issuer_id add_issuer(sqlite::database& database, const new_issuer& issuer) {
  database.prepare("INSERT INTO issuer (name, lei, country) VALUES (?1, ?2, ?3)")
      .bind(1, issuer.name)
      .bind(2, issuer.lei)
      .bind(3, issuer.country)
      .run();
  return issuer_id{database.last_inserted_id()};
}

inline tick_rule_id add_tick_rule(sqlite::database& database, std::string_view name,
                                  std::span<const tick_band> bands) {
  if (bands.empty()) throw error(std::format("tick rule {} needs at least one band", name));
  sqlite::transaction rule_with_its_bands(database);
  database.prepare("INSERT INTO tick_rule (name) VALUES (?1)").bind(1, name).run();
  const tick_rule_id rule{database.last_inserted_id()};
  for (const tick_band& band : bands) {
    database
        .prepare("INSERT INTO tick_band (tick_rule_id, price_from_nanos, tick_size_nanos) "
                 "VALUES (?1, ?2, ?3)")
        .bind(1, rule)
        .bind(2, band.price_from)
        .bind(3, band.tick_size)
        .run();
  }
  rule_with_its_bands.commit();
  return rule;
}

inline instrument_id add_instrument(sqlite::database& database, const new_instrument& instrument) {
  const bool spec_describes_asset_class = std::visit(
      [&]<typename Spec>(const Spec&) { return describes<Spec>(instrument.asset_class); },
      instrument.spec);
  if (!spec_describes_asset_class) {
    throw error(std::format("{} does not take this kind of spec",
                            name_of(instrument.asset_class, asset_class_names)));
  }
  sqlite::transaction instrument_with_its_spec(database);
  database
      .prepare("INSERT INTO instrument (asset_class, issuer_id, cfi, name) "
               "VALUES (?1, ?2, ?3, ?4)")
      .bind(1, name_of(instrument.asset_class, asset_class_names))
      .bind(2, instrument.issuer)
      .bind(3, instrument.cfi)
      .bind(4, instrument.name)
      .run();
  const instrument_id id{database.last_inserted_id()};
  std::visit([&](const auto& spec) { detail::insert_spec(database, id, spec); }, instrument.spec);
  instrument_with_its_spec.commit();
  return id;
}

inline void assign_identifier(sqlite::database& database, instrument_id instrument,
                              std::string_view scheme, std::string_view value,
                              std::chrono::sys_days valid_from) {
  sqlite::transaction old_identifier_ends_as_new_one_starts(database);
  database
      .prepare("UPDATE instrument_identifier SET valid_until = ?1 "
               "WHERE instrument_id = ?2 AND scheme = ?3 AND valid_until IS NULL")
      .bind(1, iso_date(valid_from))
      .bind(2, instrument)
      .bind(3, scheme)
      .run();
  database
      .prepare("INSERT INTO instrument_identifier (instrument_id, scheme, value, valid_from) "
               "VALUES (?1, ?2, ?3, ?4)")
      .bind(1, instrument)
      .bind(2, scheme)
      .bind(3, value)
      .bind(4, iso_date(valid_from))
      .run();
  old_identifier_ends_as_new_one_starts.commit();
}

inline listing_id add_listing(sqlite::database& database, const new_listing& listing) {
  sqlite::transaction listing_with_its_symbol_and_spec(database);
  database
      .prepare("INSERT INTO listing (instrument_id, venue_id, currency, is_primary, listed_on) "
               "VALUES (?1, ?2, ?3, ?4, ?5)")
      .bind(1, listing.instrument)
      .bind(2, listing.venue)
      .bind(3, listing.currency)
      .bind(4, listing.is_primary)
      .bind(5, iso_date(listing.listed_on))
      .run();
  const listing_id id{database.last_inserted_id()};
  detail::insert_symbol(database, id, venue_symbol_source, listing.symbol, listing.listed_on);
  detail::insert_listing_spec(database, id, listing.spec, listing.listed_on);
  listing_with_its_symbol_and_spec.commit();
  return id;
}

inline void assign_symbol(sqlite::database& database, listing_id listing,
                          std::string_view symbol_source, std::string_view symbol,
                          std::chrono::sys_days valid_from) {
  sqlite::transaction old_symbol_ends_as_new_one_starts(database);
  database
      .prepare("UPDATE listing_symbol SET valid_until = ?1 "
               "WHERE listing_id = ?2 AND source = ?3 AND valid_until IS NULL")
      .bind(1, iso_date(valid_from))
      .bind(2, listing)
      .bind(3, symbol_source)
      .run();
  detail::insert_symbol(database, listing, symbol_source, symbol, valid_from);
  old_symbol_ends_as_new_one_starts.commit();
}

inline void assign_listing_spec(sqlite::database& database, listing_id listing,
                                const listing_spec& spec, std::chrono::sys_days valid_from) {
  sqlite::transaction old_spec_ends_as_new_one_starts(database);
  database
      .prepare("UPDATE listing_spec SET valid_until = ?1 "
               "WHERE listing_id = ?2 AND valid_until IS NULL")
      .bind(1, iso_date(valid_from))
      .bind(2, listing)
      .run();
  detail::insert_listing_spec(database, listing, spec, valid_from);
  old_spec_ends_as_new_one_starts.commit();
}

inline void delist(sqlite::database& database, listing_id listing,
                   std::chrono::sys_days delisted_on) {
  sqlite::transaction listing_ends_with_its_symbols_and_spec(database);
  database
      .prepare("UPDATE listing SET delisted_on = ?1 WHERE listing_id = ?2 AND delisted_on IS NULL")
      .bind(1, iso_date(delisted_on))
      .bind(2, listing)
      .run();
  if (database.changed_rows() == 0) {
    throw error(std::format("listing {} is not listed", listing.value()));
  }
  for (const char* validity_table : {"listing_symbol", "listing_spec"}) {
    database
        .prepare(std::format("UPDATE {} SET valid_until = ?1 "
                             "WHERE listing_id = ?2 AND valid_until IS NULL",
                             validity_table))
        .bind(1, iso_date(delisted_on))
        .bind(2, listing)
        .run();
  }
  listing_ends_with_its_symbols_and_spec.commit();
}

inline corporate_action_id record_corporate_action(sqlite::database& database,
                                                   const new_corporate_action& action) {
  database
      .prepare("INSERT INTO corporate_action (instrument_id, action_type, ex_date, "
               "announce_date, record_date, pay_date, cash_per_share_nanos, currency, "
               "new_shares, old_shares, related_instrument_id) "
               "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)")
      .bind(1, action.instrument)
      .bind(2, name_of(action.type, corporate_action_type_names))
      .bind(3, iso_date(action.ex_date))
      .bind(4, detail::optional_iso_date(action.announce_date))
      .bind(5, detail::optional_iso_date(action.record_date))
      .bind(6, detail::optional_iso_date(action.pay_date))
      .bind(7, action.cash_per_share)
      .bind(8, action.currency)
      .bind(9, action.new_shares)
      .bind(10, action.old_shares)
      .bind(11, action.related_instrument)
      .run();
  return corporate_action_id{database.last_inserted_id()};
}

}  // namespace nqdb::secmaster
