#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>

#include "cli/arguments.h"

namespace nq::db::cli {

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
            .issuer = optional_id<issuer_id>(issuer),
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
            .expiry = parse_iso_date(expiry),
            .multiplier = parse_decimal<multiplier_nanos>(multiplier),
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
            .expiry = parse_iso_date(expiry),
            .strike = parse_decimal<price_nanos>(strike),
            .right = enum_named(right, option_right_names),
            .style = enum_named(style, exercise_style_names),
            .multiplier = parse_decimal<multiplier_nanos>(multiplier)};
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

  std::int64_t amended_instrument = 0;
  std::optional<std::string> amended_asset_class;
  std::optional<std::string> amended_name;
  std::optional<std::int64_t> amended_issuer;
  std::optional<std::string> amended_cfi;
  future_options amended_future_spec;
  option_options amended_option_spec;
  pair_spec amended_pair_spec;

  std::int64_t reidentified_instrument = 0;
  std::string replaced_scheme;
  std::vector<std::string> identifier_periods;

  void add_to(CLI::App& app, const context& context) {
    CLI::App* commands = app.add_subcommand("instrument", "Instruments")->require_subcommand(1);
    const auto add_and_print_id = [&context](const new_instrument& instrument) {
      sqlite::database database = context.writable();
      context.out << add_instrument(database, instrument).value() << '\n';
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
    assign_identifier_command->callback([this, &context] {
      sqlite::database database = context.writable();
      assign_identifier(database, instrument_id{identified_instrument}, scheme, identifier,
                        parse_iso_date(identifier_valid_from));
    });

    CLI::App* amend =
        commands->add_subcommand("amend", "Correct the fields given of an instrument");
    amend->add_option("--instrument", amended_instrument, "Instrument id")->required();
    amend
        ->add_option("--asset-class", amended_asset_class,
                     "Asset class that takes the same kind of spec")
        ->check(one_of(asset_class_names));
    amend->add_option("--name", amended_name, "Display name");
    amend->add_option("--issuer", amended_issuer, "Issuer id");
    amend->add_option("--cfi", amended_cfi, "ISO 10962 code");
    amend->callback([this, &context] {
      sqlite::database database = context.writable();
      amend_instrument(
          database, instrument_id{amended_instrument},
          {.asset_class = amended_asset_class.transform([](const std::string& name) {
             return enum_named(name, asset_class_names);
           }),
           .name = amended_name,
           .issuer = optional_id<issuer_id>(amended_issuer),
           .cfi = amended_cfi,
           .spec = {}});
    });

    const auto replace_spec = [this, &context](instrument_spec spec) {
      sqlite::database database = context.writable();
      amend_instrument(database, instrument_id{amended_instrument},
                       {.asset_class = {}, .name = {}, .issuer = {}, .cfi = {},
                        .spec = std::move(spec)});
    };

    CLI::App* amend_future =
        commands->add_subcommand("amend-future", "Replace the contract spec of a future");
    amend_future->add_option("--instrument", amended_instrument, "Instrument id")->required();
    amended_future_spec.add_to(*amend_future);
    amend_future->callback([this, replace_spec] { replace_spec(amended_future_spec.spec()); });

    CLI::App* amend_option =
        commands->add_subcommand("amend-option", "Replace the contract spec of an option");
    amend_option->add_option("--instrument", amended_instrument, "Instrument id")->required();
    amended_option_spec.add_to(*amend_option);
    amend_option->callback([this, replace_spec] { replace_spec(amended_option_spec.spec()); });

    CLI::App* amend_pair =
        commands->add_subcommand("amend-pair", "Replace the currencies of a currency pair");
    amend_pair->add_option("--instrument", amended_instrument, "Instrument id")->required();
    amend_pair->add_option("--base", amended_pair_spec.base_currency, "Base currency")->required();
    amend_pair->add_option("--quote", amended_pair_spec.quote_currency, "Quote currency")
        ->required();
    amend_pair->callback([this, replace_spec] { replace_spec(amended_pair_spec); });

    CLI::App* set_identifiers_command = commands->add_subcommand(
        "set-identifiers", "Replace every identifier of one scheme that an instrument had");
    set_identifiers_command->add_option("--instrument", reidentified_instrument, "Instrument id")
        ->required();
    set_identifiers_command
        ->add_option("--scheme", replaced_scheme, "For example ISIN, CUSIP, FIGI")
        ->required();
    set_identifiers_command->add_option("--period", identifier_periods,
                                "FROM:UNTIL:VALUE, once for each period; empty UNTIL has no end");
    set_identifiers_command->callback([this, &context] {
      std::vector<identifier_period> periods;
      for (const std::string& period : identifier_periods) {
        const auto fields = parse_fields(period, 3, "2020-01-02:2024-06-01:US0000000001");
        periods.push_back({.valid = parse_validity(fields[0], fields[1]),
                           .value = std::string(fields[2])});
      }
      sqlite::database database = context.writable();
      set_identifiers(database, instrument_id{reidentified_instrument}, replaced_scheme,
                          periods);
    });

    commands->add_subcommand("list", "Print every instrument")->callback([&context] {
      sqlite::database database = context.read_only();
      context.out << "instrument_id\tasset_class\tname\n";
      for (const instrument& instrument : read_instruments(database)) {
        context.out << instrument.id.value() << '\t'
                    << name_of(instrument.asset_class, asset_class_names) << '\t'
                    << instrument.name << '\n';
      }
    });
  }
};

}  // namespace nq::db::cli
