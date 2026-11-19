#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <string_view>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include "cli/arguments.h"

namespace nq::db::cli {

struct action_options {
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

  void add_to(CLI::App& command) {
    command.add_option("--instrument", instrument, "Instrument id")->required();
    command.add_option("--type", type, "Kind of action")
        ->required()
        ->check(one_of(corporate_action_type_names));
    command.add_option("--ex-date", ex_date, "Ex date")->required();
    command.add_option("--announce-date", announce_date, "Announcement date");
    command.add_option("--record-date", record_date, "Record date");
    command.add_option("--pay-date", pay_date, "Payment date");
    command.add_option("--cash-per-share", cash_per_share, "Cash per share");
    command.add_option("--currency", currency, "Currency of the cash");
    command.add_option("--new-shares", new_shares, "Shares after, per --old-shares before");
    command.add_option("--old-shares", old_shares, "Shares before");
    command.add_option("--related-instrument", related_instrument,
                       "Spun-off or acquiring instrument id");
  }

  new_corporate_action action() const {
    return {.instrument = instrument_id{instrument},
            .type = enum_named(type, corporate_action_type_names),
            .ex_date = parse_iso_date(ex_date),
            .announce_date = parse_optional(announce_date, parse_iso_date),
            .record_date = parse_optional(record_date, parse_iso_date),
            .pay_date = parse_optional(pay_date, parse_iso_date),
            .cash_per_share = parse_optional(cash_per_share, parse_decimal<price_nanos>),
            .currency = currency,
            .new_shares = new_shares,
            .old_shares = old_shares,
            .related_instrument = optional_id<instrument_id>(related_instrument)};
  }
};

struct action_commands {
  action_options added;
  std::int64_t amended_action = 0;
  action_options amended;
  std::int64_t removed_action = 0;
  std::optional<std::int64_t> listed_instrument;
  std::int64_t adjusted_instrument = 0;
  std::optional<std::string> adjusted_as_of;
  std::optional<std::string> price_currency;
  std::vector<std::string> closes_before_ex_dates;

  static void print_actions(std::ostream& out, std::span<const corporate_action> actions) {
    const auto day_or_nothing = [](std::optional<std::chrono::sys_days> day) {
      return day.transform(iso_date).value_or("");
    };
    const auto number_or_nothing = [](std::optional<std::int64_t> number) {
      return number.transform([](std::int64_t known) { return std::to_string(known); })
          .value_or("");
    };
    out << "action_id\tinstrument_id\ttype\tex_date\tannounce_date\trecord_date\tpay_date\t"
           "cash_per_share\tcurrency\tnew_shares\told_shares\trelated_instrument_id\n";
    for (const corporate_action& action : actions) {
      out << action.id.value() << '\t' << action.instrument.value() << '\t'
          << name_of(action.type, corporate_action_type_names) << '\t'
          << iso_date(action.ex_date) << '\t' << day_or_nothing(action.announce_date) << '\t'
          << day_or_nothing(action.record_date) << '\t' << day_or_nothing(action.pay_date)
          << '\t'
          << action.cash_per_share
                 .transform([](price_nanos cash) { return decimal(cash); })
                 .value_or("")
          << '\t' << action.currency.value_or("") << '\t' << number_or_nothing(action.new_shares)
          << '\t' << number_or_nothing(action.old_shares) << '\t'
          << number_or_nothing(action.related_instrument.transform(&instrument_id::value))
          << '\n';
    }
  }

  static std::map<std::chrono::sys_days, price_nanos> parse_closes(
      const std::vector<std::string>& ex_date_colon_close) {
    std::map<std::chrono::sys_days, price_nanos> closes;
    for (const std::string& each : ex_date_colon_close) {
      const std::size_t colon = each.find(':');
      if (colon == std::string::npos) {
        throw error(std::format("'{}' is not a close like 2024-06-03:187.5", each));
      }
      const std::string_view text = each;
      closes.insert_or_assign(parse_iso_date(text.substr(0, colon)),
                              parse_decimal<price_nanos>(text.substr(colon + 1)));
    }
    return closes;
  }

  adjustments adjustments_of(std::span<const corporate_action> actions) const {
    if (!price_currency) return split_adjustments(actions);
    const std::map<std::chrono::sys_days, price_nanos> closes =
        parse_closes(closes_before_ex_dates);
    return split_and_dividend_adjustments(
        actions, *price_currency, [&](std::chrono::sys_days ex_date) {
          const auto close = closes.find(ex_date);
          return close == closes.end() ? std::nullopt : std::optional{close->second};
        });
  }

  void print_factors(std::ostream& out, const adjustments& adjusted) const {
    const std::optional<std::chrono::sys_days> as_of =
        parse_optional(adjusted_as_of, parse_iso_date);
    out << "ex_date\tprice_factor\tvolume_factor\tcumulative_price_factor\t"
           "cumulative_volume_factor\n";
    for (const adjustment& step : adjusted.steps()) {
      if (as_of && *as_of < step.ex_date) continue;
      const std::chrono::sys_days day_before = step.ex_date - std::chrono::days{1};
      out << std::format(
          "{}\t{}\t{}\t{}\t{}\n", iso_date(step.ex_date), step.price_factor, step.volume_factor,
          adjusted.price_factor_on(day_before, as_of),
          adjusted.volume_factor_on(day_before, as_of));
    }
  }

  void add_to(CLI::App& app, const context& context) {
    CLI::App* commands = app.add_subcommand("action", "Corporate actions")->require_subcommand(1);

    CLI::App* add = commands->add_subcommand("add", "Add a corporate action and print its id");
    added.add_to(*add);
    add->callback([this, &context] {
      sqlite::database database = context.writable();
      context.out << add_corporate_action(database, added.action()).value() << '\n';
    });

    CLI::App* amend = commands->add_subcommand(
        "amend", "Replace every field of a corporate action with the options given");
    amend->add_option("--action", amended_action, "Corporate action id")->required();
    amended.add_to(*amend);
    amend->callback([this, &context] {
      sqlite::database database = context.writable();
      amend_corporate_action(database, corporate_action_id{amended_action}, amended.action());
    });

    CLI::App* remove = commands->add_subcommand("remove", "Delete a corporate action");
    remove->add_option("--action", removed_action, "Corporate action id")->required();
    remove->callback([this, &context] {
      sqlite::database database = context.writable();
      remove_corporate_action(database, corporate_action_id{removed_action});
    });

    CLI::App* list = commands->add_subcommand("list", "Print the corporate actions");
    list->add_option("--instrument", listed_instrument, "Only this instrument id");
    list->callback([this, &context] {
      sqlite::database database = context.read_only();
      const std::vector<corporate_action> actions = read_corporate_actions(
          database, optional_id<instrument_id>(listed_instrument));
      print_actions(context.out, actions);
    });

    CLI::App* factors = commands->add_subcommand(
        "factors",
        "Print the factors that adjust earlier prices and volumes of an instrument for its "
        "splits and stock dividends, and for its cash dividends when --currency is given");
    factors->add_option("--instrument", adjusted_instrument, "Instrument id")->required();
    factors->add_option("--as-of", adjusted_as_of, "Apply only the ex dates until this day");
    CLI::Option* currency = factors->add_option(
        "--currency", price_currency, "Currency of the prices; adjusts for cash dividends too");
    factors
        ->add_option("--close", closes_before_ex_dates,
                     "EX_DATE:PRICE, the last close before the ex date of each cash dividend")
        ->needs(currency);
    factors->callback([this, &context] {
      sqlite::database database = context.read_only();
      const std::vector<corporate_action> actions =
          read_corporate_actions(database, instrument_id{adjusted_instrument});
      print_factors(context.out, adjustments_of(actions));
    });
  }
};

}  // namespace nq::db::cli
