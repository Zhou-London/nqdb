#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include "cli/arguments.h"

namespace nq::db::cli {

struct venue_commands {
  new_venue venue;
  std::string amended_venue;
  venue_amendment amendment;

  void add_to(CLI::App& app, const context& context) {
    CLI::App* commands = app.add_subcommand("venue", "Venues")->require_subcommand(1);
    CLI::App* add = commands->add_subcommand("add", "Add a venue and print its id");
    add->add_option("--code", venue.code, "Short unique name, for example XNAS")->required();
    add->add_option("--name", venue.name, "Full name")->required();
    add->add_option("--timezone", venue.timezone, "IANA name, for example America/New_York")
        ->required();
    add->add_option("--mic", venue.mic, "ISO 10383 code");
    add->add_option("--country", venue.country, "ISO 3166-1 alpha-2 code");
    add->callback([this, &context] {
      sqlite::database database = context.writable();
      context.out << add_venue(database, venue).value() << '\n';
    });

    CLI::App* amend = commands->add_subcommand("amend", "Correct the fields given of a venue");
    amend->add_option("--venue", amended_venue, "Code of the venue to correct")->required();
    amend->add_option("--code", amendment.code, "Short unique name");
    amend->add_option("--name", amendment.name, "Full name");
    amend->add_option("--timezone", amendment.timezone, "IANA name");
    amend->add_option("--mic", amendment.mic, "ISO 10383 code");
    amend->add_option("--country", amendment.country, "ISO 3166-1 alpha-2 code");
    amend->callback([this, &context] {
      sqlite::database database = context.writable();
      amend_venue(database, venue_coded(database, amended_venue), amendment);
    });
  }
};

struct issuer_commands {
  new_issuer issuer;
  std::int64_t amended_issuer = 0;
  issuer_amendment amendment;

  void add_to(CLI::App& app, const context& context) {
    CLI::App* commands = app.add_subcommand("issuer", "Issuers")->require_subcommand(1);
    CLI::App* add = commands->add_subcommand("add", "Add an issuer and print its id");
    add->add_option("--name", issuer.name, "Legal name")->required();
    add->add_option("--lei", issuer.lei, "Legal entity identifier");
    add->add_option("--country", issuer.country, "ISO 3166-1 alpha-2 code");
    add->callback([this, &context] {
      sqlite::database database = context.writable();
      context.out << add_issuer(database, issuer).value() << '\n';
    });

    CLI::App* amend = commands->add_subcommand("amend", "Correct the fields given of an issuer");
    amend->add_option("--issuer", amended_issuer, "Issuer id")->required();
    amend->add_option("--name", amendment.name, "Legal name");
    amend->add_option("--lei", amendment.lei, "Legal entity identifier");
    amend->add_option("--country", amendment.country, "ISO 3166-1 alpha-2 code");
    amend->callback([this, &context] {
      sqlite::database database = context.writable();
      amend_issuer(database, issuer_id{amended_issuer}, amendment);
    });

    commands->add_subcommand("list", "Print every issuer")->callback([&context] {
      sqlite::database database = context.read_only();
      context.out << "issuer_id\tname\tlei\tcountry\n";
      for (const secmaster::issuer& issuer : read_issuers(database)) {
        context.out << issuer.id.value() << '\t' << issuer.name << '\t'
                    << issuer.lei.value_or("") << '\t' << issuer.country.value_or("") << '\n';
      }
    });
  }
};

struct tick_rule_commands {
  std::string name;
  std::vector<std::string> bands;
  std::string amended_tick_rule;
  std::optional<std::string> amended_name;
  std::vector<std::string> amended_bands;

  static tick_band parse_band(std::string_view price_from_colon_tick_size) {
    const std::size_t colon = price_from_colon_tick_size.find(':');
    if (colon == std::string_view::npos) {
      throw error(std::format("'{}' is not a band like 0:0.01", price_from_colon_tick_size));
    }
    return {.price_from = parse_decimal<price_nanos>(price_from_colon_tick_size.substr(0, colon)),
            .tick_size = parse_decimal<price_nanos>(price_from_colon_tick_size.substr(colon + 1))};
  }

  static std::vector<tick_band> parse_bands(const std::vector<std::string>& bands) {
    std::vector<tick_band> tick_bands;
    for (const std::string& band : bands) tick_bands.push_back(parse_band(band));
    return tick_bands;
  }

  void add_to(CLI::App& app, const context& context) {
    CLI::App* commands = app.add_subcommand("tick-rule", "Tick size rules")->require_subcommand(1);
    CLI::App* add = commands->add_subcommand("add", "Add a tick rule and print its id");
    add->add_option("--name", name, "Unique name")->required();
    add->add_option("--band", bands, "PRICE_FROM:TICK_SIZE, for example 0:0.0001 and 1:0.01")
        ->required();
    add->callback([this, &context] {
      sqlite::database database = context.writable();
      context.out << add_tick_rule(database, name, parse_bands(bands)).value() << '\n';
    });

    CLI::App* amend = commands->add_subcommand(
        "amend", "Correct the name of a tick rule, or replace every band of it");
    amend->add_option("--tick-rule", amended_tick_rule, "Name of the tick rule to correct")
        ->required();
    amend->add_option("--name", amended_name, "Unique name");
    amend->add_option("--band", amended_bands, "PRICE_FROM:TICK_SIZE, once for each band");
    amend->callback([this, &context] {
      sqlite::database database = context.writable();
      amend_tick_rule(database, tick_rule_named(database, amended_tick_rule),
                      {.name = amended_name,
                       .bands = amended_bands.empty()
                                    ? std::nullopt
                                    : std::optional{parse_bands(amended_bands)}});
    });

    commands->add_subcommand("list", "Print every band of every tick rule")
        ->callback([&context] {
          sqlite::database database = context.read_only();
          context.out << "tick_rule_id\tname\tprice_from\ttick_size\n";
          for (const tick_rule& rule : read_tick_rules(database)) {
            for (const tick_band& band : rule.bands_by_ascending_price) {
              context.out << rule.id.value() << '\t' << rule.name << '\t'
                          << decimal(band.price_from) << '\t' << decimal(band.tick_size) << '\n';
            }
          }
        });
  }
};

}  // namespace nq::db::cli
