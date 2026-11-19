#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <CLI/CLI.hpp>

#include "cli/arguments.h"

namespace nq::db::cli {

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
    return {.lot_size = parse_decimal<quantity_nanos>(lot_size),
            .min_qty = parse_optional(min_qty, parse_decimal<quantity_nanos>),
            .tick_rule = tick_rule_named(database, tick_rule)};
  }
};

struct listing_query_commands {
  date_option list_date;
  universe_options list_universe;
  date_option find_date;
  std::string find_venue_code;
  std::string find_symbol;
  std::string find_symbol_source{venue_symbol_source};
  date_option tick_date;
  std::string tick_venue_code;
  std::string tick_symbol;
  std::string tick_symbol_source{venue_symbol_source};
  std::string tick_price;
  day_range_options history_days;
  universe_options history_universe;

  static void print_listings(std::ostream& out, const history& loaded,
                             std::chrono::sys_days day) {
    out << "listing_id\tinstrument_id\tvenue\tsymbol\tasset_class\tcurrency\tlot_size\t"
           "tick_rule\tname\n";
    for (const secmaster::listing& listing : loaded.listings()) {
      const listing_index index = *loaded.listings().find(listing.id);
      if (!loaded.is_listed(index, day)) continue;
      const instrument& instrument = loaded.instruments()[listing.instrument];
      const spec_period& spec = *loaded.spec_on(index, day);
      out << listing.id.value() << '\t' << instrument.id.value() << '\t'
          << loaded.venues()[listing.venue].code << '\t'
          << loaded.symbol_on(venue_symbol_source, index, day)->symbol << '\t'
          << name_of(instrument.asset_class, asset_class_names) << '\t' << listing.currency
          << '\t' << decimal(spec.lot_size) << '\t' << loaded.tick_rules()[spec.tick_rule].name
          << '\t' << instrument.name << '\n';
    }
  }

  static void print_period(std::ostream& out, const history& loaded,
                           const secmaster::listing& listing, std::string_view kind,
                           std::string_view symbol_source, const validity& valid,
                           std::string_view symbol, const spec_period* spec) {
    out << listing.id.value() << '\t' << loaded.venues()[listing.venue].code << '\t' << kind
        << '\t' << symbol_source << '\t' << iso_date(valid.from) << '\t'
        << valid.until.transform(iso_date).value_or("") << '\t' << symbol << '\t';
    if (spec == nullptr) {
      out << "\t\t\n";
      return;
    }
    out << decimal(spec->lot_size) << '\t'
        << spec->min_qty.transform([](quantity_nanos min_qty) { return decimal(min_qty); })
               .value_or("")
        << '\t' << loaded.tick_rules()[spec->tick_rule].name << '\n';
  }

  static void print_periods(std::ostream& out, const history& loaded) {
    out << "listing_id\tvenue\tkind\tsource\tvalid_from\tvalid_until\tsymbol\tlot_size\t"
           "min_qty\ttick_rule\n";
    for (const secmaster::listing& listing : loaded.listings()) {
      const listing_index index = *loaded.listings().find(listing.id);
      print_period(out, loaded, listing, "LISTING", "", listing.listed, "", nullptr);
      for (const std::string_view symbol_source : loaded.symbol_sources()) {
        for (const symbol_period& period : loaded.symbols_of(symbol_source, index)) {
          print_period(out, loaded, listing, "SYMBOL", symbol_source, period.valid, period.symbol,
                       nullptr);
        }
      }
      for (const spec_period& period : loaded.specs_of(index)) {
        print_period(out, loaded, listing, "SPEC", "", period.valid, "", &period);
      }
    }
  }

  static listing_index listing_named(const history& loaded, std::string_view symbol_source,
                                     std::string_view venue_code, std::string_view symbol,
                                     std::chrono::sys_days day) {
    const std::optional<venue_index> venue = loaded.find_venue(venue_code);
    const std::optional<listing_index> listing =
        venue ? loaded.find_listing(symbol_source, *venue, symbol, day) : std::nullopt;
    if (!listing) {
      throw error(std::format("no listing has {} symbol {} on {} on {}", symbol_source, symbol,
                              venue_code, iso_date(day)));
    }
    return *listing;
  }

  static history listings_on_venue(sqlite::database& database, std::string_view venue_code,
                                   std::chrono::sys_days day) {
    return load_history(database, one_day(day),
                        [venue_code](const venue& venue, const instrument&) {
                          return venue.code == venue_code;
                        });
  }

  void add_to(CLI::App& commands, const context& context) {
    CLI::App* list = commands.add_subcommand("list", "Print the listings that trade on one day");
    list_date.add_to(*list);
    list_universe.add_to(*list);
    list->callback([this, &context] {
      sqlite::database database = context.read_only();
      print_listings(context.out,
                     load_history(database, one_day(list_date.day()), list_universe),
                     list_date.day());
    });

    CLI::App* find =
        commands.add_subcommand("find", "Print the listing id that a symbol names on one day");
    find_date.add_to(*find);
    find->add_option("--venue", find_venue_code, "Venue code")->required();
    find->add_option("--symbol", find_symbol, "Symbol")->required();
    find->add_option("--source", find_symbol_source, "Symbol source (default VENUE)");
    find->callback([this, &context] {
      sqlite::database database = context.read_only();
      const history loaded = listings_on_venue(database, find_venue_code, find_date.day());
      const listing_index listing = listing_named(loaded, find_symbol_source, find_venue_code,
                                                  find_symbol, find_date.day());
      context.out << loaded.listings()[listing].id.value() << '\n';
    });

    CLI::App* tick_size = commands.add_subcommand(
        "tick-size", "Print the tick size of a symbol at a price on one day");
    tick_date.add_to(*tick_size);
    tick_size->add_option("--venue", tick_venue_code, "Venue code")->required();
    tick_size->add_option("--symbol", tick_symbol, "Symbol")->required();
    tick_size->add_option("--price", tick_price, "Price to trade at")->required();
    tick_size->add_option("--source", tick_symbol_source, "Symbol source (default VENUE)");
    tick_size->callback([this, &context] {
      sqlite::database database = context.read_only();
      const history loaded = listings_on_venue(database, tick_venue_code, tick_date.day());
      const listing_index listing = listing_named(loaded, tick_symbol_source, tick_venue_code,
                                                  tick_symbol, tick_date.day());
      const tick_rule& rule =
          loaded.tick_rules()[loaded.spec_on(listing, tick_date.day())->tick_rule];
      context.out << decimal(rule.tick_size_at(parse_decimal<price_nanos>(tick_price))) << '\n';
    });

    CLI::App* history_command = commands.add_subcommand(
        "history", "Print the lifetime, symbols and specs that listings had over a range of days");
    history_days.add_to(*history_command);
    history_universe.add_to(*history_command);
    history_command->callback([this, &context] {
      sqlite::database database = context.read_only();
      print_periods(context.out, load_history(database, history_days.days(), history_universe));
    });
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

  std::int64_t amended_listing = 0;
  std::optional<std::string> amended_currency;
  bool amended_to_primary = false;
  bool amended_to_not_primary = false;
  std::optional<std::string> amended_listed_on;
  std::optional<std::string> amended_delisted_on;

  std::int64_t relisted_listing = 0;

  std::int64_t resymboled_listing = 0;
  std::string replaced_symbol_source{venue_symbol_source};
  std::vector<std::string> symbol_periods;

  std::int64_t every_spec_listing = 0;
  std::vector<std::string> spec_periods;

  listing_query_commands queries;

  void add_to(CLI::App& app, const context& context) {
    CLI::App* commands = app.add_subcommand("listing", "Listings")->require_subcommand(1);
    queries.add_to(*commands, context);

    CLI::App* add = commands->add_subcommand(
        "add", "List an instrument on a venue with its symbol and spec, and print the listing id");
    add->add_option("--instrument", listed_instrument, "Instrument id")->required();
    add->add_option("--venue", venue_code, "Venue code")->required();
    add->add_option("--currency", currency, "Trading currency")->required();
    add->add_option("--symbol", symbol, "Symbol on the venue")->required();
    add->add_option("--listed-on", listed_on, "First trading day")->required();
    add->add_flag("--primary", is_primary, "This is the instrument's primary listing");
    first_spec.add_to(*add);
    add->callback([this, &context] {
      sqlite::database database = context.writable();
      context.out << add_listing(database, {.instrument = instrument_id{listed_instrument},
                                            .venue = venue_coded(database, venue_code),
                                            .currency = currency,
                                            .is_primary = is_primary,
                                            .listed_on = parse_iso_date(listed_on),
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
    assign_symbol_command->callback([this, &context] {
      sqlite::database database = context.writable();
      assign_symbol(database, listing_id{renamed_listing}, new_symbol_source, new_symbol,
                    parse_iso_date(new_symbol_valid_from));
    });

    CLI::App* assign_spec_command = commands->add_subcommand(
        "assign-spec", "Give a listing a lot size and tick rule, ending its previous spec");
    assign_spec_command->add_option("--listing", respecified_listing, "Listing id")->required();
    assign_spec_command->add_option("--from", new_spec_valid_from, "First day the spec is valid")
        ->required();
    new_spec.add_to(*assign_spec_command);
    assign_spec_command->callback([this, &context] {
      sqlite::database database = context.writable();
      assign_listing_spec(database, listing_id{respecified_listing}, new_spec.spec(database),
                          parse_iso_date(new_spec_valid_from));
    });

    CLI::App* delist_command =
        commands->add_subcommand("delist", "End a listing together with its symbols and spec");
    delist_command->add_option("--listing", delisted_listing, "Listing id")->required();
    delist_command
        ->add_option("--delisted-on", delisted_on, "First day the listing no longer trades")
        ->required();
    delist_command->callback([this, &context] {
      sqlite::database database = context.writable();
      delist(database, listing_id{delisted_listing}, parse_iso_date(delisted_on));
    });

    CLI::App* amend = commands->add_subcommand(
        "amend", "Correct the fields given of a listing; its symbols and spec follow its days");
    amend->add_option("--listing", amended_listing, "Listing id")->required();
    amend->add_option("--currency", amended_currency, "Trading currency");
    CLI::Option* primary =
        amend->add_flag("--primary", amended_to_primary, "The instrument's primary listing");
    amend->add_flag("--not-primary", amended_to_not_primary, "Not the primary listing")
        ->excludes(primary);
    amend->add_option("--listed-on", amended_listed_on, "First trading day");
    amend->add_option("--delisted-on", amended_delisted_on,
                      "First day a delisted listing no longer trades");
    amend->callback([this, &context] {
      sqlite::database database = context.writable();
      amend_listing(database, listing_id{amended_listing},
                    {.currency = amended_currency,
                     .is_primary = amended_to_primary       ? std::optional{true}
                                   : amended_to_not_primary ? std::optional{false}
                                                            : std::nullopt,
                     .listed_on = parse_optional(amended_listed_on, parse_iso_date),
                     .delisted_on = parse_optional(amended_delisted_on, parse_iso_date)});
    });

    CLI::App* undelist_command = commands->add_subcommand(
        "undelist", "List a delisted listing again with the symbols and spec it ended with");
    undelist_command->add_option("--listing", relisted_listing, "Listing id")->required();
    undelist_command->callback([this, &context] {
      sqlite::database database = context.writable();
      undelist(database, listing_id{relisted_listing});
    });

    CLI::App* set_symbols_command = commands->add_subcommand(
        "set-symbols", "Replace every symbol that a listing had from one source");
    set_symbols_command->add_option("--listing", resymboled_listing, "Listing id")->required();
    set_symbols_command->add_option("--source", replaced_symbol_source,
                                    "Symbol source (default VENUE)");
    set_symbols_command->add_option("--period", symbol_periods,
                            "FROM:UNTIL:SYMBOL, once for each period; empty UNTIL has no end");
    set_symbols_command->callback([this, &context] {
      std::vector<symbol_period> periods;
      for (const std::string& period : symbol_periods) {
        const auto fields = parse_fields(period, 3, "2020-01-02:2024-06-01:EXMP");
        periods.push_back({.valid = parse_validity(fields[0], fields[1]),
                           .symbol = std::string(fields[2])});
      }
      sqlite::database database = context.writable();
      set_symbols(database, listing_id{resymboled_listing}, replaced_symbol_source, periods);
    });

    CLI::App* set_specs_command =
        commands->add_subcommand("set-specs", "Replace every spec that a listing had");
    set_specs_command->add_option("--listing", every_spec_listing, "Listing id")->required();
    set_specs_command->add_option("--period", spec_periods,
                          "FROM:UNTIL:LOT_SIZE:MIN_QTY:TICK_RULE, once for each period; "
                          "empty UNTIL has no end, empty MIN_QTY has no minimum");
    set_specs_command->callback([this, &context] {
      sqlite::database database = context.writable();
      std::vector<listing_spec_period> periods;
      for (const std::string& period : spec_periods) {
        const auto fields = parse_fields(period, 5, "2020-01-02:2024-06-01:100::US_EQUITY");
        periods.push_back(
            {.valid = parse_validity(fields[0], fields[1]),
             .spec = {.lot_size = parse_decimal<quantity_nanos>(fields[2]),
                      .min_qty = fields[3].empty()
                                     ? std::nullopt
                                     : std::optional{parse_decimal<quantity_nanos>(fields[3])},
                      .tick_rule = tick_rule_named(database, fields[4])}});
      }
      set_listing_specs(database, listing_id{every_spec_listing}, periods);
    });
  }
};

}  // namespace nq::db::cli
