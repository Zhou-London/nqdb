#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nq/db/secmaster/calendar.h>
#include <nq/db/secmaster/error.h>
#include <nq/db/secmaster/history.h>
#include <nq/db/secmaster/loader.h>
#include <nq/db/secmaster/records.h>
#include <nq/db/secmaster/schema.h>
#include <nq/db/secmaster/text.h>
#include <nq/db/secmaster/time_zone.h>
#include <nq/db/sqlite.h>

namespace nq::db::secmaster {

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

struct venue_amendment {
  std::optional<std::string> code;
  std::optional<std::string> mic;
  std::optional<std::string> name;
  std::optional<std::string> country;
  std::optional<std::string> timezone;
};

struct issuer_amendment {
  std::optional<std::string> name;
  std::optional<std::string> lei;
  std::optional<std::string> country;
};

struct tick_rule_amendment {
  std::optional<std::string> name;
  std::optional<std::vector<tick_band>> bands;
};

struct instrument_amendment {
  std::optional<secmaster::asset_class> asset_class;
  std::optional<std::string> name;
  std::optional<issuer_id> issuer;
  std::optional<std::string> cfi;
  std::optional<instrument_spec> spec;
};

struct listing_amendment {
  std::optional<std::string> currency;
  std::optional<bool> is_primary;
  std::optional<std::chrono::sys_days> listed_on;
  std::optional<std::chrono::sys_days> delisted_on;
};

struct listing_spec_period {
  validity valid;
  listing_spec spec;
};

struct identifier_period {
  validity valid;
  std::string value;
};

namespace detail {

inline void require_a_changed_row(sqlite::database& database, std::string_view record,
                                  std::int64_t id) {
  if (database.changed_rows() == 0) throw error(std::format("there is no {} {}", record, id));
}

inline void require_row(sqlite::database& database, std::string_view table, std::int64_t id) {
  sqlite::statement row =
      database.prepare(std::format("SELECT 1 FROM {0} WHERE {0}_id = ?1", table));
  row.bind(1, id);
  if (!row.next_row()) throw error(std::format("there is no {} {}", table, id));
}

inline std::size_t spec_alternative_of(asset_class asset_class) noexcept {
  if (describes<future_spec>(asset_class)) return instrument_spec{future_spec{}}.index();
  if (describes<option_spec>(asset_class)) return instrument_spec{option_spec{}}.index();
  if (describes<pair_spec>(asset_class)) return instrument_spec{pair_spec{}}.index();
  return instrument_spec{no_spec{}}.index();
}

inline std::optional<std::string> optional_iso_date(std::optional<std::chrono::sys_days> day) {
  return day.transform([](std::chrono::sys_days known) { return iso_date(known); });
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
      .bind(4, iso_date(spec.expiry))
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
      .bind(3, iso_date(spec.expiry))
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
                          const validity& valid) {
  database
      .prepare("INSERT INTO listing_symbol (listing_id, source, symbol, valid_from, valid_until) "
               "VALUES (?1, ?2, ?3, ?4, ?5)")
      .bind(1, listing)
      .bind(2, symbol_source)
      .bind(3, symbol)
      .bind(4, iso_date(valid.from))
      .bind(5, optional_iso_date(valid.until))
      .run();
}

inline void insert_listing_spec(sqlite::database& database, listing_id listing,
                                const listing_spec& spec, const validity& valid) {
  database
      .prepare("INSERT INTO listing_spec (listing_id, valid_from, valid_until, lot_size_nanos, "
               "min_qty_nanos, tick_rule_id) VALUES (?1, ?2, ?3, ?4, ?5, ?6)")
      .bind(1, listing)
      .bind(2, iso_date(valid.from))
      .bind(3, optional_iso_date(valid.until))
      .bind(4, spec.lot_size)
      .bind(5, spec.min_qty)
      .bind(6, spec.tick_rule)
      .run();
}

inline void insert_identifier(sqlite::database& database, instrument_id instrument,
                              std::string_view scheme, std::string_view value,
                              const validity& valid) {
  database
      .prepare("INSERT INTO instrument_identifier (instrument_id, scheme, value, valid_from, "
               "valid_until) VALUES (?1, ?2, ?3, ?4, ?5)")
      .bind(1, instrument)
      .bind(2, scheme)
      .bind(3, value)
      .bind(4, iso_date(valid.from))
      .bind(5, optional_iso_date(valid.until))
      .run();
}

inline void insert_tick_bands(sqlite::database& database, tick_rule_id rule,
                              std::span<const tick_band> bands) {
  for (const tick_band& band : bands) {
    database
        .prepare("INSERT INTO tick_band (tick_rule_id, price_from_nanos, tick_size_nanos) "
                 "VALUES (?1, ?2, ?3)")
        .bind(1, rule)
        .bind(2, band.price_from)
        .bind(3, band.tick_size)
        .run();
  }
}

inline void delete_specs(sqlite::database& database, instrument_id instrument) {
  for (const char* spec_table : {"future_spec", "option_spec", "pair_spec"}) {
    database.prepare(std::format("DELETE FROM {} WHERE instrument_id = ?1", spec_table))
        .bind(1, instrument)
        .run();
  }
}

inline std::vector<session_hours> by_ascending_open(std::span<const session_hours> sessions) {
  std::vector<session_hours> sorted(sessions.begin(), sessions.end());
  std::ranges::sort(sorted, {}, &session_hours::opens);
  return sorted;
}

inline void delete_day_rules(sqlite::database& database, venue_id venue,
                             std::chrono::sys_days day) {
  for (const char* day_table : {"closed_day", "special_session"}) {
    database.prepare(std::format("DELETE FROM {} WHERE venue_id = ?1 AND day = ?2", day_table))
        .bind(1, venue)
        .bind(2, iso_date(day))
        .run();
  }
}

inline void require_sessions_in_turn_around(sqlite::database& database, venue_id venue,
                                            std::chrono::sys_days first_changed_day,
                                            std::chrono::days changed_days) {
  constexpr std::chrono::days reach_of_a_session{2};
  load_calendar(database, venue,
                {.from = first_changed_day - reach_of_a_session,
                 .until = first_changed_day + changed_days + reach_of_a_session});
}

template <typename Statement>
Statement& bind_corporate_action(Statement&& row, const new_corporate_action& action) {
  return row.bind(1, action.instrument)
      .bind(2, name_of(action.type, corporate_action_type_names))
      .bind(3, iso_date(action.ex_date))
      .bind(4, optional_iso_date(action.announce_date))
      .bind(5, optional_iso_date(action.record_date))
      .bind(6, optional_iso_date(action.pay_date))
      .bind(7, action.cash_per_share)
      .bind(8, action.currency)
      .bind(9, action.new_shares)
      .bind(10, action.old_shares)
      .bind(11, action.related_instrument);
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
  zone_named(venue.timezone);
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
  detail::insert_tick_bands(database, rule, bands);
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
  detail::insert_identifier(database, instrument, scheme, value,
                            {.from = valid_from, .until = std::nullopt});
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
  const validity from_the_first_day{.from = listing.listed_on, .until = std::nullopt};
  detail::insert_symbol(database, id, venue_symbol_source, listing.symbol, from_the_first_day);
  detail::insert_listing_spec(database, id, listing.spec, from_the_first_day);
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
  detail::insert_symbol(database, listing, symbol_source, symbol,
                        {.from = valid_from, .until = std::nullopt});
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
  detail::insert_listing_spec(database, listing, spec, {.from = valid_from, .until = std::nullopt});
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
  for (const std::string_view validity_table : validity_tables_of_a_listing) {
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

inline corporate_action_id add_corporate_action(sqlite::database& database,
                                                   const new_corporate_action& action) {
  detail::bind_corporate_action(
      database.prepare("INSERT INTO corporate_action (instrument_id, action_type, ex_date, "
                       "announce_date, record_date, pay_date, cash_per_share_nanos, currency, "
                       "new_shares, old_shares, related_instrument_id) "
                       "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)"),
      action)
      .run();
  return corporate_action_id{database.last_inserted_id()};
}

inline void amend_venue(sqlite::database& database, venue_id venue,
                        const venue_amendment& amendment) {
  if (amendment.timezone) zone_named(*amendment.timezone);
  database
      .prepare("UPDATE venue SET code = coalesce(?2, code), mic = coalesce(?3, mic), "
               "name = coalesce(?4, name), country = coalesce(?5, country), "
               "timezone = coalesce(?6, timezone) WHERE venue_id = ?1")
      .bind(1, venue)
      .bind(2, amendment.code)
      .bind(3, amendment.mic)
      .bind(4, amendment.name)
      .bind(5, amendment.country)
      .bind(6, amendment.timezone)
      .run();
  detail::require_a_changed_row(database, "venue", venue.value());
}

inline void amend_issuer(sqlite::database& database, issuer_id issuer,
                         const issuer_amendment& amendment) {
  database
      .prepare("UPDATE issuer SET name = coalesce(?2, name), lei = coalesce(?3, lei), "
               "country = coalesce(?4, country) WHERE issuer_id = ?1")
      .bind(1, issuer)
      .bind(2, amendment.name)
      .bind(3, amendment.lei)
      .bind(4, amendment.country)
      .run();
  detail::require_a_changed_row(database, "issuer", issuer.value());
}

inline void amend_tick_rule(sqlite::database& database, tick_rule_id rule,
                            const tick_rule_amendment& amendment) {
  if (amendment.bands && amendment.bands->empty()) {
    throw error(std::format("tick rule {} needs at least one band", rule.value()));
  }
  sqlite::transaction rule_with_its_bands(database);
  database.prepare("UPDATE tick_rule SET name = coalesce(?2, name) WHERE tick_rule_id = ?1")
      .bind(1, rule)
      .bind(2, amendment.name)
      .run();
  detail::require_a_changed_row(database, "tick rule", rule.value());
  if (amendment.bands) {
    database.prepare("DELETE FROM tick_band WHERE tick_rule_id = ?1").bind(1, rule).run();
    detail::insert_tick_bands(database, rule, *amendment.bands);
  }
  rule_with_its_bands.commit();
}

inline void amend_instrument(sqlite::database& database, instrument_id instrument,
                             const instrument_amendment& amendment) {
  sqlite::transaction instrument_with_its_spec(database);
  sqlite::statement stored =
      database.prepare("SELECT asset_class FROM instrument WHERE instrument_id = ?1");
  stored.bind(1, instrument);
  if (!stored.next_row()) throw error(std::format("there is no instrument {}", instrument.value()));
  const asset_class stored_asset_class =
      enum_named(stored.get<std::string_view>(0), asset_class_names);
  const asset_class amended_asset_class = amendment.asset_class.value_or(stored_asset_class);
  const std::size_t given_or_stored_spec =
      amendment.spec ? amendment.spec->index() : detail::spec_alternative_of(stored_asset_class);
  if (given_or_stored_spec != detail::spec_alternative_of(amended_asset_class)) {
    throw error(std::format("{} does not take this kind of spec",
                            name_of(amended_asset_class, asset_class_names)));
  }
  database
      .prepare("UPDATE instrument SET asset_class = ?2, name = coalesce(?3, name), "
               "issuer_id = coalesce(?4, issuer_id), cfi = coalesce(?5, cfi) "
               "WHERE instrument_id = ?1")
      .bind(1, instrument)
      .bind(2, name_of(amended_asset_class, asset_class_names))
      .bind(3, amendment.name)
      .bind(4, amendment.issuer)
      .bind(5, amendment.cfi)
      .run();
  if (amendment.spec) {
    detail::delete_specs(database, instrument);
    std::visit([&](const auto& spec) { detail::insert_spec(database, instrument, spec); },
               *amendment.spec);
  }
  instrument_with_its_spec.commit();
}

inline void amend_listing(sqlite::database& database, listing_id listing,
                          const listing_amendment& amendment) {
  sqlite::transaction listing_with_the_ends_of_its_symbols_and_specs(database);
  sqlite::statement stored =
      database.prepare("SELECT listed_on, delisted_on FROM listing WHERE listing_id = ?1");
  stored.bind(1, listing);
  if (!stored.next_row()) throw error(std::format("there is no listing {}", listing.value()));
  const auto stored_listed_on = stored.get<std::string>(0);
  const auto stored_delisted_on = stored.get<std::optional<std::string>>(1);
  if (amendment.delisted_on && !stored_delisted_on) {
    throw error(std::format("listing {} is listed, delist it first", listing.value()));
  }
  database
      .prepare("UPDATE listing SET currency = coalesce(?2, currency), "
               "is_primary = coalesce(?3, is_primary), listed_on = coalesce(?4, listed_on), "
               "delisted_on = coalesce(?5, delisted_on) WHERE listing_id = ?1")
      .bind(1, listing)
      .bind(2, amendment.currency)
      .bind(3, amendment.is_primary)
      .bind(4, detail::optional_iso_date(amendment.listed_on))
      .bind(5, detail::optional_iso_date(amendment.delisted_on))
      .run();
  for (const std::string_view validity_table : validity_tables_of_a_listing) {
    if (amendment.listed_on) {
      database
          .prepare(std::format("UPDATE {} SET valid_from = ?1 "
                               "WHERE listing_id = ?2 AND valid_from = ?3",
                               validity_table))
          .bind(1, iso_date(*amendment.listed_on))
          .bind(2, listing)
          .bind(3, stored_listed_on)
          .run();
    }
    if (amendment.delisted_on) {
      database
          .prepare(std::format("UPDATE {} SET valid_until = ?1 "
                               "WHERE listing_id = ?2 AND valid_until = ?3",
                               validity_table))
          .bind(1, iso_date(*amendment.delisted_on))
          .bind(2, listing)
          .bind(3, *stored_delisted_on)
          .run();
    }
    sqlite::statement outside_the_lifetime = database.prepare(std::format(
        "SELECT 1 FROM {} period JOIN listing USING (listing_id) "
        "WHERE listing_id = ?1 AND (period.valid_from < listed_on OR (delisted_on IS NOT NULL "
        "AND (period.valid_until IS NULL OR delisted_on < period.valid_until)))",
        validity_table));
    outside_the_lifetime.bind(1, listing);
    if (outside_the_lifetime.next_row()) {
      throw error(std::format("a {} row of listing {} is outside the amended lifetime",
                              validity_table, listing.value()));
    }
  }
  listing_with_the_ends_of_its_symbols_and_specs.commit();
}

inline void undelist(sqlite::database& database, listing_id listing) {
  sqlite::transaction listing_reopens_with_its_symbols_and_spec(database);
  sqlite::statement stored =
      database.prepare("SELECT delisted_on FROM listing WHERE listing_id = ?1");
  stored.bind(1, listing);
  if (!stored.next_row()) throw error(std::format("there is no listing {}", listing.value()));
  const auto delisted_on = stored.get<std::optional<std::string>>(0);
  if (!delisted_on) throw error(std::format("listing {} is not delisted", listing.value()));
  database.prepare("UPDATE listing SET delisted_on = NULL WHERE listing_id = ?1")
      .bind(1, listing)
      .run();
  for (const std::string_view validity_table : validity_tables_of_a_listing) {
    database
        .prepare(std::format("UPDATE {} SET valid_until = NULL "
                             "WHERE listing_id = ?1 AND valid_until = ?2",
                             validity_table))
        .bind(1, listing)
        .bind(2, *delisted_on)
        .run();
  }
  listing_reopens_with_its_symbols_and_spec.commit();
}

inline void set_symbols(sqlite::database& database, listing_id listing,
                            std::string_view symbol_source,
                            std::span<const symbol_period> periods) {
  sqlite::transaction every_symbol_of_the_source(database);
  detail::require_row(database, "listing", listing.value());
  database.prepare("DELETE FROM listing_symbol WHERE listing_id = ?1 AND source = ?2")
      .bind(1, listing)
      .bind(2, symbol_source)
      .run();
  for (const symbol_period& period : periods) {
    detail::insert_symbol(database, listing, symbol_source, period.symbol, period.valid);
  }
  every_symbol_of_the_source.commit();
}

inline void set_listing_specs(sqlite::database& database, listing_id listing,
                                  std::span<const listing_spec_period> periods) {
  sqlite::transaction every_spec_of_the_listing(database);
  detail::require_row(database, "listing", listing.value());
  database.prepare("DELETE FROM listing_spec WHERE listing_id = ?1").bind(1, listing).run();
  for (const listing_spec_period& period : periods) {
    detail::insert_listing_spec(database, listing, period.spec, period.valid);
  }
  every_spec_of_the_listing.commit();
}

inline void set_identifiers(sqlite::database& database, instrument_id instrument,
                                std::string_view scheme,
                                std::span<const identifier_period> periods) {
  sqlite::transaction every_identifier_of_the_scheme(database);
  detail::require_row(database, "instrument", instrument.value());
  database.prepare("DELETE FROM instrument_identifier WHERE instrument_id = ?1 AND scheme = ?2")
      .bind(1, instrument)
      .bind(2, scheme)
      .run();
  for (const identifier_period& period : periods) {
    detail::insert_identifier(database, instrument, scheme, period.value, period.valid);
  }
  every_identifier_of_the_scheme.commit();
}

inline void amend_corporate_action(sqlite::database& database, corporate_action_id action,
                                   const new_corporate_action& amended) {
  detail::bind_corporate_action(
      database.prepare("UPDATE corporate_action SET instrument_id = ?1, action_type = ?2, "
                       "ex_date = ?3, announce_date = ?4, record_date = ?5, pay_date = ?6, "
                       "cash_per_share_nanos = ?7, currency = ?8, new_shares = ?9, "
                       "old_shares = ?10, related_instrument_id = ?11 "
                       "WHERE corporate_action_id = ?12"),
      amended)
      .bind(12, action)
      .run();
  detail::require_a_changed_row(database, "corporate action", action.value());
}

inline void remove_corporate_action(sqlite::database& database, corporate_action_id action) {
  database.prepare("DELETE FROM corporate_action WHERE corporate_action_id = ?1")
      .bind(1, action)
      .run();
  detail::require_a_changed_row(database, "corporate action", action.value());
}

inline void set_week(sqlite::database& database, venue_id venue,
                                std::chrono::sys_days valid_from,
                                std::span<const weekly_session> sessions) {
  for (const std::chrono::weekday weekday :
       {std::chrono::Monday, std::chrono::Tuesday, std::chrono::Wednesday, std::chrono::Thursday,
        std::chrono::Friday, std::chrono::Saturday, std::chrono::Sunday}) {
    std::vector<session_hours> of_the_weekday;
    for (const weekly_session& session : sessions) {
      if (session.weekday == weekday) of_the_weekday.push_back(session.hours);
    }
    require_sessions_without_overlap(detail::by_ascending_open(of_the_weekday), valid_from);
  }
  sqlite::transaction old_week_ends_as_new_one_starts(database);
  detail::require_row(database, "venue", venue.value());
  database.prepare("DELETE FROM weekly_session WHERE venue_id = ?1 AND valid_from >= ?2")
      .bind(1, venue)
      .bind(2, iso_date(valid_from))
      .run();
  database
      .prepare("UPDATE weekly_session SET valid_until = ?2 "
               "WHERE venue_id = ?1 AND (valid_until IS NULL OR valid_until > ?2)")
      .bind(1, venue)
      .bind(2, iso_date(valid_from))
      .run();
  for (const weekly_session& session : sessions) {
    database
        .prepare("INSERT INTO weekly_session (venue_id, weekday, opens_minute, closes_minute, "
                 "valid_from) VALUES (?1, ?2, ?3, ?4, ?5)")
        .bind(1, venue)
        .bind(2, session.weekday.iso_encoding())
        .bind(3, session.hours.opens.count())
        .bind(4, session.hours.closes.count())
        .bind(5, iso_date(valid_from))
        .run();
  }
  detail::require_sessions_in_turn_around(database, venue, valid_from, std::chrono::days{7});
  old_week_ends_as_new_one_starts.commit();
}

inline void close_day(sqlite::database& database, venue_id venue, std::chrono::sys_days day) {
  sqlite::transaction day_without_sessions(database);
  detail::require_row(database, "venue", venue.value());
  detail::delete_day_rules(database, venue, day);
  database.prepare("INSERT INTO closed_day (venue_id, day) VALUES (?1, ?2)")
      .bind(1, venue)
      .bind(2, iso_date(day))
      .run();
  day_without_sessions.commit();
}

inline void set_day(sqlite::database& database, venue_id venue,
                                 std::chrono::sys_days day,
                                 std::span<const session_hours> sessions) {
  if (sessions.empty()) throw error(std::format("{} needs at least one special session", day));
  require_sessions_without_overlap(detail::by_ascending_open(sessions), day);
  sqlite::transaction day_with_its_own_sessions(database);
  detail::require_row(database, "venue", venue.value());
  detail::delete_day_rules(database, venue, day);
  for (const session_hours& hours : sessions) {
    database
        .prepare("INSERT INTO special_session (venue_id, day, opens_minute, closes_minute) "
                 "VALUES (?1, ?2, ?3, ?4)")
        .bind(1, venue)
        .bind(2, iso_date(day))
        .bind(3, hours.opens.count())
        .bind(4, hours.closes.count())
        .run();
  }
  detail::require_sessions_in_turn_around(database, venue, day, std::chrono::days{1});
  day_with_its_own_sessions.commit();
}

inline void reset_day(sqlite::database& database, venue_id venue,
                                std::chrono::sys_days day) {
  sqlite::transaction day_with_its_weekly_sessions(database);
  detail::require_row(database, "venue", venue.value());
  detail::delete_day_rules(database, venue, day);
  detail::require_sessions_in_turn_around(database, venue, day, std::chrono::days{1});
  day_with_its_weekly_sessions.commit();
}

}  // namespace nq::db::secmaster
