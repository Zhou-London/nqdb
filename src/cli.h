#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <CLI/CLI.hpp>

#include <nqdb/secmaster/date.h>
#include <nqdb/secmaster/decimal.h>
#include <nqdb/secmaster/error.h>
#include <nqdb/secmaster/loader.h>
#include <nqdb/secmaster/records.h>
#include <nqdb/secmaster/schema.h>
#include <nqdb/secmaster/writer.h>
#include <nqdb/sqlite.h>

namespace nqdb::cli {

using namespace nqdb::secmaster;

inline std::filesystem::path default_database_file() {
  const char* home = std::getenv("HOME");
  if (home == nullptr) throw error("HOME is not set, pass --db");
  return std::filesystem::path(home) / ".nqdb" / "secmaster.db";
}

struct session {
  std::filesystem::path database_file;
  std::ostream& out;

  std::filesystem::path file() const {
    return std::filesystem::absolute(database_file.empty() ? default_database_file()
                                                           : database_file);
  }
  sqlite::database writable() const { return open(file()); }
  sqlite::database read_only() const { return open_read_only(file()); }
};

inline std::chrono::sys_days day_given(std::string_view yyyy_mm_dd) {
  constexpr std::size_t iso_date_length = 10;
  const bool is_calendar_day = yyyy_mm_dd.size() == iso_date_length &&
                               iso_date(parse_iso_date(yyyy_mm_dd)) == yyyy_mm_dd;
  if (!is_calendar_day) throw error(std::format("'{}' is not a date like 2026-01-31", yyyy_mm_dd));
  return parse_iso_date(yyyy_mm_dd);
}

inline std::optional<std::chrono::sys_days> optional_day_given(
    const std::optional<std::string>& date) {
  return date.transform([](const std::string& given) { return day_given(given); });
}

inline std::string today() {
  return iso_date(std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
}

template <typename Nanos>
Nanos nanos_given(std::string_view decimal) {
  return Nanos{nanos_from_decimal(decimal)};
}

template <typename Nanos>
std::optional<Nanos> optional_nanos_given(const std::optional<std::string>& decimal) {
  return decimal.transform([](const std::string& given) { return nanos_given<Nanos>(given); });
}

template <typename Id>
std::optional<Id> optional_id_given(const std::optional<std::int64_t>& id) {
  return id.transform([](std::int64_t given) { return Id{given}; });
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

inline tick_band band_given(std::string_view price_from_colon_tick_size) {
  const std::size_t colon = price_from_colon_tick_size.find(':');
  if (colon == std::string_view::npos) {
    throw error(std::format("'{}' is not a band like 0:0.01", price_from_colon_tick_size));
  }
  return {.price_from = nanos_given<price_nanos>(price_from_colon_tick_size.substr(0, colon)),
          .tick_size = nanos_given<price_nanos>(price_from_colon_tick_size.substr(colon + 1))};
}

inline venue_id venue_given(sqlite::database& database, std::string_view code) {
  const std::optional<venue_id> venue = find_venue(database, code);
  if (!venue) throw error(std::format("there is no venue {}", code));
  return *venue;
}

inline tick_rule_id tick_rule_given(sqlite::database& database, std::string_view name) {
  const std::optional<tick_rule_id> tick_rule = find_tick_rule(database, name);
  if (!tick_rule) throw error(std::format("there is no tick rule {}", name));
  return *tick_rule;
}

struct as_of_option {
  std::string date = today();

  void add_to(CLI::App& command) {
    command.add_option("--date", date, "Day to read (default today, UTC)");
  }
  std::chrono::sys_days day() const { return day_given(date); }
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

struct database_commands {
  as_of_option archive_as_of;
  std::filesystem::path archive_directory;

  void add_to(CLI::App& app, const session& session) {
    app.add_subcommand("init", "Create an empty database")->callback([&session] {
      std::filesystem::create_directories(session.file().parent_path());
      sqlite::database created = sqlite::database::create(session.file());
      create_schema(created);
      session.out << session.file().string() << '\n';
    });

    CLI::App* archive_command =
        app.add_subcommand("archive", "Write a copy of the database named by a date");
    archive_command->add_option("--date", archive_as_of.date,
                                "Date in the copy's name (default today, UTC)");
    archive_command->add_option("--dir", archive_directory,
                                "Directory of the copy (default archive/ beside the database)");
    archive_command->callback([this, &session] {
      const std::filesystem::path directory =
          archive_directory.empty() ? session.file().parent_path() / "archive"
                                    : std::filesystem::absolute(archive_directory);
      std::filesystem::create_directories(directory);
      sqlite::database database = session.read_only();
      session.out << archive(database, directory, archive_as_of.day()).string() << '\n';
    });
  }
};

struct day_commands {
  as_of_option check_as_of;
  universe_options check_universe;
  as_of_option list_as_of;
  universe_options list_universe;
  as_of_option find_as_of;
  std::string find_venue_code;
  std::string find_symbol;
  std::string find_symbol_source{venue_symbol_source};

  static void print_listings(std::ostream& out, const snapshot& loaded) {
    out << "listing_id\tinstrument_id\tvenue\tsymbol\tasset_class\tcurrency\tlot_size\t"
           "tick_rule\tname\n";
    for (const listing& listing : loaded.listings()) {
      const instrument& instrument = loaded.instruments()[listing.instrument];
      out << listing.id.value() << '\t' << instrument.id.value() << '\t'
          << loaded.venues()[listing.venue].code << '\t' << listing.symbol << '\t'
          << name_of(instrument.asset_class, asset_class_names) << '\t' << listing.currency
          << '\t' << decimal_from_nanos(listing.lot_size.value()) << '\t'
          << loaded.tick_rules()[listing.tick_rule].name << '\t' << instrument.name << '\n';
    }
  }

  void add_to(CLI::App& app, const session& session) {
    CLI::App* check_command = app.add_subcommand(
        "check", "Load one day and fail if a listing lacks its symbol or its spec");
    check_as_of.add_to(*check_command);
    check_universe.add_to(*check_command);
    check_command->callback([this, &session] {
      sqlite::database database = session.read_only();
      const snapshot loaded = load_snapshot(database, check_as_of.day(), check_universe);
      session.out << loaded.listings().size() << " listings of " << loaded.instruments().size()
                  << " instruments load on " << check_as_of.date << '\n';
    });

    CLI::App* list_command =
        app.add_subcommand("list", "Print the listings that trade on one day");
    list_as_of.add_to(*list_command);
    list_universe.add_to(*list_command);
    list_command->callback([this, &session] {
      sqlite::database database = session.read_only();
      print_listings(session.out, load_snapshot(database, list_as_of.day(), list_universe));
    });

    CLI::App* find_command =
        app.add_subcommand("find", "Print the listing id that a symbol names on one day");
    find_as_of.add_to(*find_command);
    find_command->add_option("--venue", find_venue_code, "Venue code")->required();
    find_command->add_option("--symbol", find_symbol, "Symbol")->required();
    find_command->add_option("--source", find_symbol_source, "Symbol source (default VENUE)");
    find_command->callback([this, &session] {
      sqlite::database database = session.read_only();
      const snapshot loaded = load_snapshot(
          database, find_as_of.day(),
          [this](const venue& venue, const instrument&) { return venue.code == find_venue_code; });
      const std::optional<venue_index> venue = loaded.find_venue(find_venue_code);
      const std::optional<listing_index> listing =
          venue ? loaded.find_listing(find_symbol_source, *venue, find_symbol) : std::nullopt;
      if (!listing) {
        throw error(std::format("no listing has {} symbol {} on {} on {}", find_symbol_source,
                                find_symbol, find_venue_code, find_as_of.date));
      }
      session.out << loaded.listings()[*listing].id.value() << '\n';
    });
  }
};

struct venue_commands {
  new_venue venue;

  void add_to(CLI::App& app, const session& session) {
    CLI::App* commands = app.add_subcommand("venue", "Venues")->require_subcommand(1);
    CLI::App* add = commands->add_subcommand("add", "Add a venue and print its id");
    add->add_option("--code", venue.code, "Short unique name, for example XNAS")->required();
    add->add_option("--name", venue.name, "Full name")->required();
    add->add_option("--timezone", venue.timezone, "IANA name, for example America/New_York")
        ->required();
    add->add_option("--mic", venue.mic, "ISO 10383 code");
    add->add_option("--country", venue.country, "ISO 3166-1 alpha-2 code");
    add->callback([this, &session] {
      sqlite::database database = session.writable();
      session.out << add_venue(database, venue).value() << '\n';
    });
  }
};

struct issuer_commands {
  new_issuer issuer;

  void add_to(CLI::App& app, const session& session) {
    CLI::App* commands = app.add_subcommand("issuer", "Issuers")->require_subcommand(1);
    CLI::App* add = commands->add_subcommand("add", "Add an issuer and print its id");
    add->add_option("--name", issuer.name, "Legal name")->required();
    add->add_option("--lei", issuer.lei, "Legal entity identifier");
    add->add_option("--country", issuer.country, "ISO 3166-1 alpha-2 code");
    add->callback([this, &session] {
      sqlite::database database = session.writable();
      session.out << add_issuer(database, issuer).value() << '\n';
    });

    commands->add_subcommand("list", "Print every issuer")->callback([&session] {
      sqlite::database database = session.read_only();
      session.out << "issuer_id\tname\tlei\tcountry\n";
      for (const secmaster::issuer& issuer : read_issuers(database)) {
        session.out << issuer.id.value() << '\t' << issuer.name << '\t'
                    << issuer.lei.value_or("") << '\t' << issuer.country.value_or("") << '\n';
      }
    });
  }
};

struct tick_rule_commands {
  std::string name;
  std::vector<std::string> bands;

  void add_to(CLI::App& app, const session& session) {
    CLI::App* commands = app.add_subcommand("tick-rule", "Tick size rules")->require_subcommand(1);
    CLI::App* add = commands->add_subcommand("add", "Add a tick rule and print its id");
    add->add_option("--name", name, "Unique name")->required();
    add->add_option("--band", bands, "PRICE_FROM:TICK_SIZE, for example 0:0.0001 and 1:0.01")
        ->required();
    add->callback([this, &session] {
      std::vector<tick_band> tick_bands;
      for (const std::string& band : bands) tick_bands.push_back(band_given(band));
      sqlite::database database = session.writable();
      session.out << add_tick_rule(database, name, tick_bands).value() << '\n';
    });
  }
};

struct instrument_options {
  std::string asset_class_name;
  std::string name;
  std::optional<std::int64_t> issuer;
  std::optional<std::string> cfi;

  void add_to(CLI::App& command) {
    command.add_option("--name", name, "Display name")->required();
    command.add_option("--issuer", issuer, "Issuer id");
    command.add_option("--cfi", cfi, "ISO 10962 code");
  }

  new_instrument instrument_with(instrument_spec spec) const {
    return {.asset_class = enum_named(asset_class_name, asset_class_names),
            .name = name,
            .issuer = optional_id_given<issuer_id>(issuer),
            .cfi = cfi,
            .spec = std::move(spec)};
  }
};

struct future_options {
  std::int64_t underlying = 0;
  std::string root;
  std::string expiry;
  std::string multiplier;
  std::string settlement;

  void add_to(CLI::App& command) {
    command.add_option("--underlying", underlying, "Underlying instrument id")->required();
    command.add_option("--root", root, "Product root, for example ES")->required();
    command.add_option("--expiry", expiry, "Last trading day")->required();
    command.add_option("--multiplier", multiplier, "Contract multiplier")->required();
    command.add_option("--settlement", settlement, "Settlement")
        ->required()
        ->check(one_of(settlement_method_names));
  }

  future_spec spec() const {
    return {.underlying = instrument_id{underlying},
            .root = root,
            .expiry = day_given(expiry),
            .multiplier = nanos_given<multiplier_nanos>(multiplier),
            .settlement = enum_named(settlement, settlement_method_names)};
  }
};

struct option_options {
  std::int64_t underlying = 0;
  std::string expiry;
  std::string strike;
  std::string right;
  std::string style;
  std::string multiplier;

  void add_to(CLI::App& command) {
    command.add_option("--underlying", underlying, "Underlying instrument id")->required();
    command.add_option("--expiry", expiry, "Last trading day")->required();
    command.add_option("--strike", strike, "Strike price")->required();
    command.add_option("--right", right, "Option right")
        ->required()
        ->check(one_of(option_right_names));
    command.add_option("--style", style, "Exercise style")
        ->required()
        ->check(one_of(exercise_style_names));
    command.add_option("--multiplier", multiplier, "Contract multiplier")->required();
  }

  option_spec spec() const {
    return {.underlying = instrument_id{underlying},
            .expiry = day_given(expiry),
            .strike = nanos_given<price_nanos>(strike),
            .right = enum_named(right, option_right_names),
            .style = enum_named(style, exercise_style_names),
            .multiplier = nanos_given<multiplier_nanos>(multiplier)};
  }
};

struct instrument_commands {
  instrument_options plain;
  instrument_options future{.asset_class_name{name_of(asset_class::future, asset_class_names)}};
  future_options future_spec_options;
  instrument_options option{.asset_class_name{name_of(asset_class::option, asset_class_names)}};
  option_options option_spec_options;
  instrument_options pair;
  pair_spec pair_spec_options;
  std::int64_t identified_instrument = 0;
  std::string scheme;
  std::string identifier;
  std::string identifier_valid_from;

  void add_to(CLI::App& app, const session& session) {
    CLI::App* commands = app.add_subcommand("instrument", "Instruments")->require_subcommand(1);
    const auto add_and_print_id = [&session](const new_instrument& instrument) {
      sqlite::database database = session.writable();
      session.out << add_instrument(database, instrument).value() << '\n';
    };

    CLI::App* add = commands->add_subcommand(
        "add", "Add an instrument that needs no contract spec and print its id");
    add->add_option("--asset-class", plain.asset_class_name, "Asset class")
        ->required()
        ->check(asset_class_described_by(describes<no_spec>));
    plain.add_to(*add);
    add->callback([this, add_and_print_id] { add_and_print_id(plain.instrument_with(no_spec{})); });

    CLI::App* add_future = commands->add_subcommand("add-future", "Add a future and print its id");
    future.add_to(*add_future);
    future_spec_options.add_to(*add_future);
    add_future->callback([this, add_and_print_id] {
      add_and_print_id(future.instrument_with(future_spec_options.spec()));
    });

    CLI::App* add_option = commands->add_subcommand("add-option", "Add an option and print its id");
    option.add_to(*add_option);
    option_spec_options.add_to(*add_option);
    add_option->callback([this, add_and_print_id] {
      add_and_print_id(option.instrument_with(option_spec_options.spec()));
    });

    CLI::App* add_pair =
        commands->add_subcommand("add-pair", "Add a currency pair and print its id");
    add_pair->add_option("--asset-class", pair.asset_class_name, "Asset class")
        ->required()
        ->check(asset_class_described_by(describes<pair_spec>));
    pair.add_to(*add_pair);
    add_pair->add_option("--base", pair_spec_options.base_currency, "Base currency")->required();
    add_pair->add_option("--quote", pair_spec_options.quote_currency, "Quote currency")
        ->required();
    add_pair->callback(
        [this, add_and_print_id] { add_and_print_id(pair.instrument_with(pair_spec_options)); });

    CLI::App* assign_identifier_command = commands->add_subcommand(
        "assign-identifier",
        "Give an instrument an identifier, ending its previous one of that scheme");
    assign_identifier_command->add_option("--instrument", identified_instrument, "Instrument id")
        ->required();
    assign_identifier_command->add_option("--scheme", scheme, "For example ISIN, CUSIP, FIGI")
        ->required();
    assign_identifier_command->add_option("--value", identifier, "The identifier")->required();
    assign_identifier_command
        ->add_option("--from", identifier_valid_from, "First day the identifier is valid")
        ->required();
    assign_identifier_command->callback([this, &session] {
      sqlite::database database = session.writable();
      assign_identifier(database, instrument_id{identified_instrument}, scheme, identifier,
                        day_given(identifier_valid_from));
    });

    commands->add_subcommand("list", "Print every instrument")->callback([&session] {
      sqlite::database database = session.read_only();
      session.out << "instrument_id\tasset_class\tname\n";
      for (const instrument& instrument : read_instruments(database)) {
        session.out << instrument.id.value() << '\t'
                    << name_of(instrument.asset_class, asset_class_names) << '\t'
                    << instrument.name << '\n';
      }
    });
  }
};

struct listing_spec_options {
  std::string lot_size;
  std::optional<std::string> min_qty;
  std::string tick_rule;

  void add_to(CLI::App& command) {
    command.add_option("--lot-size", lot_size, "Smallest tradable quantity step")->required();
    command.add_option("--min-qty", min_qty, "Smallest order quantity");
    command.add_option("--tick-rule", tick_rule, "Name of the tick rule")->required();
  }

  listing_spec spec(sqlite::database& database) const {
    return {.lot_size = nanos_given<quantity_nanos>(lot_size),
            .min_qty = optional_nanos_given<quantity_nanos>(min_qty),
            .tick_rule = tick_rule_given(database, tick_rule)};
  }
};

struct listing_commands {
  std::int64_t listed_instrument = 0;
  std::string venue_code;
  std::string currency;
  std::string symbol;
  std::string listed_on;
  bool is_primary = false;
  listing_spec_options first_spec;

  std::int64_t renamed_listing = 0;
  std::string new_symbol;
  std::string new_symbol_source{venue_symbol_source};
  std::string new_symbol_valid_from;

  std::int64_t respecified_listing = 0;
  listing_spec_options new_spec;
  std::string new_spec_valid_from;

  std::int64_t delisted_listing = 0;
  std::string delisted_on;

  void add_to(CLI::App& app, const session& session) {
    CLI::App* commands = app.add_subcommand("listing", "Listings")->require_subcommand(1);

    CLI::App* add = commands->add_subcommand(
        "add", "List an instrument on a venue with its symbol and spec, and print the listing id");
    add->add_option("--instrument", listed_instrument, "Instrument id")->required();
    add->add_option("--venue", venue_code, "Venue code")->required();
    add->add_option("--currency", currency, "Trading currency")->required();
    add->add_option("--symbol", symbol, "Symbol on the venue")->required();
    add->add_option("--listed-on", listed_on, "First trading day")->required();
    add->add_flag("--primary", is_primary, "This is the instrument's primary listing");
    first_spec.add_to(*add);
    add->callback([this, &session] {
      sqlite::database database = session.writable();
      session.out << add_listing(database, {.instrument = instrument_id{listed_instrument},
                                            .venue = venue_given(database, venue_code),
                                            .currency = currency,
                                            .is_primary = is_primary,
                                            .listed_on = day_given(listed_on),
                                            .symbol = symbol,
                                            .spec = first_spec.spec(database)})
                         .value()
                  << '\n';
    });

    CLI::App* assign_symbol_command = commands->add_subcommand(
        "assign-symbol", "Give a listing a symbol, ending its previous one from that source");
    assign_symbol_command->add_option("--listing", renamed_listing, "Listing id")->required();
    assign_symbol_command->add_option("--symbol", new_symbol, "The new symbol")->required();
    assign_symbol_command
        ->add_option("--from", new_symbol_valid_from, "First day the symbol is valid")
        ->required();
    assign_symbol_command->add_option("--source", new_symbol_source,
                                      "Symbol source (default VENUE)");
    assign_symbol_command->callback([this, &session] {
      sqlite::database database = session.writable();
      assign_symbol(database, listing_id{renamed_listing}, new_symbol_source, new_symbol,
                    day_given(new_symbol_valid_from));
    });

    CLI::App* assign_spec_command = commands->add_subcommand(
        "assign-spec", "Give a listing a lot size and tick rule, ending its previous spec");
    assign_spec_command->add_option("--listing", respecified_listing, "Listing id")->required();
    assign_spec_command->add_option("--from", new_spec_valid_from, "First day the spec is valid")
        ->required();
    new_spec.add_to(*assign_spec_command);
    assign_spec_command->callback([this, &session] {
      sqlite::database database = session.writable();
      assign_listing_spec(database, listing_id{respecified_listing}, new_spec.spec(database),
                          day_given(new_spec_valid_from));
    });

    CLI::App* delist_command =
        commands->add_subcommand("delist", "End a listing together with its symbols and spec");
    delist_command->add_option("--listing", delisted_listing, "Listing id")->required();
    delist_command
        ->add_option("--delisted-on", delisted_on, "First day the listing no longer trades")
        ->required();
    delist_command->callback([this, &session] {
      sqlite::database database = session.writable();
      delist(database, listing_id{delisted_listing}, day_given(delisted_on));
    });
  }
};

struct action_commands {
  std::int64_t instrument = 0;
  std::string type;
  std::string ex_date;
  std::optional<std::string> announce_date;
  std::optional<std::string> record_date;
  std::optional<std::string> pay_date;
  std::optional<std::string> cash_per_share;
  std::optional<std::string> currency;
  std::optional<std::int64_t> new_shares;
  std::optional<std::int64_t> old_shares;
  std::optional<std::int64_t> related_instrument;

  void add_to(CLI::App& app, const session& session) {
    CLI::App* commands = app.add_subcommand("action", "Corporate actions")->require_subcommand(1);
    CLI::App* record =
        commands->add_subcommand("record", "Record a corporate action and print its id");
    record->add_option("--instrument", instrument, "Instrument id")->required();
    record->add_option("--type", type, "Kind of action")
        ->required()
        ->check(one_of(corporate_action_type_names));
    record->add_option("--ex-date", ex_date, "Ex date")->required();
    record->add_option("--announce-date", announce_date, "Announcement date");
    record->add_option("--record-date", record_date, "Record date");
    record->add_option("--pay-date", pay_date, "Payment date");
    record->add_option("--cash-per-share", cash_per_share, "Cash per share");
    record->add_option("--currency", currency, "Currency of the cash");
    record->add_option("--new-shares", new_shares, "Shares after, per --old-shares before");
    record->add_option("--old-shares", old_shares, "Shares before");
    record->add_option("--related-instrument", related_instrument,
                       "Spun-off or acquiring instrument id");
    record->callback([this, &session] {
      sqlite::database database = session.writable();
      session.out << record_corporate_action(
                         database,
                         {.instrument = instrument_id{instrument},
                          .type = enum_named(type, corporate_action_type_names),
                          .ex_date = day_given(ex_date),
                          .announce_date = optional_day_given(announce_date),
                          .record_date = optional_day_given(record_date),
                          .pay_date = optional_day_given(pay_date),
                          .cash_per_share = optional_nanos_given<price_nanos>(cash_per_share),
                          .currency = currency,
                          .new_shares = new_shares,
                          .old_shares = old_shares,
                          .related_instrument =
                              optional_id_given<instrument_id>(related_instrument)})
                         .value()
                  << '\n';
    });
  }
};

inline int run(int argc, const char* const* argv, std::ostream& out, std::ostream& err) {
  CLI::App app{"nqdb: the security master database"};
  app.require_subcommand(1);

  session session{.database_file = {}, .out = out};
  app.add_option("--db", session.database_file, "Database file (default ~/.nqdb/secmaster.db)")
      ->envname("NQDB_PATH");

  database_commands database_commands;
  day_commands day_commands;
  venue_commands venue_commands;
  issuer_commands issuer_commands;
  tick_rule_commands tick_rule_commands;
  instrument_commands instrument_commands;
  listing_commands listing_commands;
  action_commands action_commands;
  database_commands.add_to(app, session);
  day_commands.add_to(app, session);
  venue_commands.add_to(app, session);
  issuer_commands.add_to(app, session);
  tick_rule_commands.add_to(app, session);
  instrument_commands.add_to(app, session);
  listing_commands.add_to(app, session);
  action_commands.add_to(app, session);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& parse_error) {
    return app.exit(parse_error, out, err);
  } catch (const std::exception& failure) {
    err << "nqdb: " << failure.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

}  // namespace nqdb::cli
