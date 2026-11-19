#include <nq/db/secmaster/adjustment.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nq/db/secmaster/loader.h>
#include <nq/db/secmaster/writer.h>

#include "secmaster_test_database.h"

namespace {

using namespace std::chrono_literals;
using namespace nq::db::secmaster;
using nq::db::sqlite::database;
using std::chrono::days;
using std::chrono::sys_days;

constexpr instrument_id acme{1};
constexpr instrument_id bolt{2};
constexpr sys_days first_split_day{2022y / 3 / 1};
constexpr sys_days dividend_day{2023y / 6 / 1};
constexpr sys_days second_split_day{2024y / 9 / 2};

corporate_action action_of(std::int64_t id, corporate_action_type type, sys_days ex_date) {
  return {.id = corporate_action_id{id},
          .instrument = acme,
          .type = type,
          .ex_date = ex_date,
          .announce_date = {},
          .record_date = {},
          .pay_date = {},
          .cash_per_share = {},
          .currency = {},
          .new_shares = {},
          .old_shares = {},
          .related_instrument = {},
          .recorded_at = "2026-01-31T12:00:00.000Z"};
}

corporate_action split(std::int64_t id, sys_days ex_date, std::int64_t new_shares,
                       std::int64_t old_shares) {
  corporate_action action = action_of(id, corporate_action_type::split, ex_date);
  action.new_shares = new_shares;
  action.old_shares = old_shares;
  return action;
}

corporate_action cash_dividend(std::int64_t id, sys_days ex_date, std::int64_t cash_nanos,
                               const std::string& currency = "TST") {
  corporate_action action = action_of(id, corporate_action_type::cash_dividend, ex_date);
  action.cash_per_share = price_nanos{cash_nanos};
  action.currency = currency;
  return action;
}

constexpr auto close_of_one_hundred = [](sys_days) {
  return std::optional{price_nanos{100 * nanos_per_unit}};
};

TEST(SecmasterAdjustment, WithoutStepsEveryFactorIsOne) {
  const adjustments none;
  EXPECT_TRUE(none.steps().empty());
  EXPECT_DOUBLE_EQ(none.price_factor_on(first_split_day), 1.0);
  EXPECT_DOUBLE_EQ(none.volume_factor_on(first_split_day), 1.0);
  EXPECT_DOUBLE_EQ(none.price_factor_on(first_split_day, second_split_day), 1.0);
}

TEST(SecmasterAdjustment, ASplitScalesThePricesAndVolumesBeforeItsExDate) {
  const std::vector two_for_one{split(1, first_split_day, 2, 1)};
  const adjustments adjusted = split_adjustments(two_for_one);

  EXPECT_EQ(adjusted.steps().size(), 1u);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(first_split_day - days{1}), 0.5);
  EXPECT_DOUBLE_EQ(adjusted.volume_factor_on(first_split_day - days{1}), 2.0);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(first_split_day), 1.0);
  EXPECT_DOUBLE_EQ(adjusted.volume_factor_on(first_split_day), 1.0);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(sys_days{2000y / 1 / 1}), 0.5);
}

TEST(SecmasterAdjustment, LaterSplitsMultiplyIntoEarlierDays) {
  const std::vector splits{split(2, second_split_day, 3, 2), split(1, first_split_day, 2, 1)};
  const adjustments adjusted = split_adjustments(splits);

  EXPECT_EQ(adjusted.steps().front().ex_date, first_split_day);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(first_split_day - days{1}), 1.0 / 3.0);
  EXPECT_DOUBLE_EQ(adjusted.volume_factor_on(first_split_day - days{1}), 3.0);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(first_split_day), 2.0 / 3.0);
  EXPECT_DOUBLE_EQ(adjusted.volume_factor_on(dividend_day), 1.5);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(second_split_day), 1.0);
}

TEST(SecmasterAdjustment, AReverseSplitAndAStockDividendChangeTheShareCount) {
  corporate_action stock_dividend =
      action_of(2, corporate_action_type::stock_dividend, second_split_day);
  stock_dividend.new_shares = 11;
  stock_dividend.old_shares = 10;
  const std::vector actions{split(1, first_split_day, 1, 10), stock_dividend};
  const adjustments adjusted = split_adjustments(actions);

  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(dividend_day), 10.0 / 11.0);
  EXPECT_DOUBLE_EQ(adjusted.volume_factor_on(dividend_day), 1.1);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(first_split_day - days{1}), 100.0 / 11.0);
  EXPECT_DOUBLE_EQ(adjusted.volume_factor_on(first_split_day - days{1}), 0.11);
}

TEST(SecmasterAdjustment, AsOfADayOnlyTheExDatesUntilThatDayApply) {
  const std::vector splits{split(1, first_split_day, 2, 1), split(2, second_split_day, 3, 1)};
  const adjustments adjusted = split_adjustments(splits);
  const sys_days early = first_split_day - days{1};

  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(early, early), 1.0);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(early, first_split_day), 0.5);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(early, dividend_day), 0.5);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(early, second_split_day), 1.0 / 6.0);
  EXPECT_DOUBLE_EQ(adjusted.volume_factor_on(early, dividend_day), 2.0);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(dividend_day, dividend_day), 1.0);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(dividend_day, second_split_day), 1.0 / 3.0);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(second_split_day, first_split_day), 1.0);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(early, sys_days{2100y / 1 / 1}),
                   adjusted.price_factor_on(early));
}

TEST(SecmasterAdjustment, SplitAdjustmentsLeaveOutEveryOtherKindOfAction) {
  const std::vector actions{cash_dividend(1, dividend_day, nanos_per_unit),
                            action_of(2, corporate_action_type::merger, dividend_day),
                            action_of(3, corporate_action_type::spinoff, dividend_day),
                            action_of(4, corporate_action_type::rights, dividend_day),
                            action_of(5, corporate_action_type::delisting, dividend_day),
                            split(6, second_split_day, 2, 1)};
  EXPECT_EQ(split_adjustments(actions).steps().size(), 1u);
  EXPECT_EQ(split_and_dividend_adjustments(actions, "TST", close_of_one_hundred).steps().size(),
            2u);
}

TEST(SecmasterAdjustment, ACashDividendScalesPricesByTheCloseBeforeItsExDate) {
  const std::vector actions{cash_dividend(1, dividend_day, nanos_per_unit)};
  const adjustments adjusted =
      split_and_dividend_adjustments(actions, "TST", close_of_one_hundred);

  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(dividend_day - days{1}), 0.99);
  EXPECT_DOUBLE_EQ(adjusted.volume_factor_on(dividend_day - days{1}), 1.0);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(dividend_day), 1.0);
}

TEST(SecmasterAdjustment, EachDividendUsesTheCloseBeforeItsOwnExDate) {
  const std::vector actions{split(1, first_split_day, 2, 1),
                            cash_dividend(2, dividend_day, 2 * nanos_per_unit),
                            cash_dividend(3, second_split_day, nanos_per_unit)};
  const std::map<sys_days, price_nanos> closes{
      {dividend_day, price_nanos{50 * nanos_per_unit}},
      {second_split_day, price_nanos{40 * nanos_per_unit}}};
  std::vector<sys_days> asked;
  const adjustments adjusted =
      split_and_dividend_adjustments(actions, "TST", [&](sys_days ex_date) {
        asked.push_back(ex_date);
        return std::optional{closes.at(ex_date)};
      });

  EXPECT_EQ(asked, (std::vector{dividend_day, second_split_day}));
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(dividend_day), 39.0 / 40.0);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(first_split_day), 48.0 / 50.0 * 39.0 / 40.0);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(first_split_day - days{1}),
                   0.5 * 48.0 / 50.0 * 39.0 / 40.0);
  EXPECT_DOUBLE_EQ(adjusted.volume_factor_on(first_split_day - days{1}), 2.0);
}

TEST(SecmasterAdjustment, RejectsADividendWithoutACloseInItsCurrencyAboveItsCash) {
  const std::vector in_another_currency{cash_dividend(1, dividend_day, nanos_per_unit, "ALT")};
  const std::vector whole_price{cash_dividend(1, dividend_day, 100 * nanos_per_unit)};
  const std::vector ordinary{cash_dividend(1, dividend_day, nanos_per_unit)};
  const auto no_close = [](sys_days) { return std::optional<price_nanos>{}; };

  EXPECT_THROW(split_and_dividend_adjustments(in_another_currency, "TST", close_of_one_hundred),
               error);
  EXPECT_THROW(split_and_dividend_adjustments(whole_price, "TST", close_of_one_hundred), error);
  EXPECT_THROW(split_and_dividend_adjustments(ordinary, "TST", no_close), error);
  EXPECT_NO_THROW(split_and_dividend_adjustments(ordinary, "TST", close_of_one_hundred));
  EXPECT_NO_THROW(split_adjustments(in_another_currency));
}

new_corporate_action recorded(instrument_id instrument, corporate_action_type type,
                              sys_days ex_date) {
  return {.instrument = instrument,
          .type = type,
          .ex_date = ex_date,
          .announce_date = {},
          .record_date = {},
          .pay_date = {},
          .cash_per_share = {},
          .currency = {},
          .new_shares = {},
          .old_shares = {},
          .related_instrument = {}};
}

database secmaster_with_actions() {
  database secmaster = nqdb_test::secmaster_with_venues();
  secmaster.run_script("INSERT INTO instrument (instrument_id, asset_class, name) VALUES "
                       "(1, 'EQUITY', 'Acme'), (2, 'EQUITY', 'Bolt')");
  new_corporate_action late_split = recorded(acme, corporate_action_type::split, second_split_day);
  late_split.new_shares = 3;
  late_split.old_shares = 2;
  new_corporate_action dividend =
      recorded(acme, corporate_action_type::cash_dividend, dividend_day);
  dividend.announce_date = dividend_day - days{30};
  dividend.record_date = dividend_day + days{1};
  dividend.pay_date = dividend_day + days{14};
  dividend.cash_per_share = price_nanos{nanos_per_unit / 4};
  dividend.currency = "TST";
  new_corporate_action spinoff = recorded(bolt, corporate_action_type::spinoff, first_split_day);
  spinoff.related_instrument = acme;
  new_corporate_action early_split = recorded(acme, corporate_action_type::split, first_split_day);
  early_split.new_shares = 2;
  early_split.old_shares = 1;
  for (const new_corporate_action& action : {late_split, dividend, spinoff, early_split}) {
    add_corporate_action(secmaster, action);
  }
  return secmaster;
}

TEST(SecmasterAdjustment, ReadsCorporateActionsByInstrumentThenExDate) {
  database secmaster = secmaster_with_actions();
  const std::vector<corporate_action> actions = read_corporate_actions(secmaster);

  ASSERT_EQ(actions.size(), 4u);
  EXPECT_EQ(actions[0].instrument, acme);
  EXPECT_EQ(actions[0].ex_date, first_split_day);
  EXPECT_EQ(actions[0].id, corporate_action_id{4});
  EXPECT_EQ(actions[1].type, corporate_action_type::cash_dividend);
  EXPECT_EQ(actions[2].ex_date, second_split_day);
  EXPECT_EQ(actions[3].instrument, bolt);

  const corporate_action& dividend = actions[1];
  EXPECT_EQ(dividend.ex_date, dividend_day);
  EXPECT_EQ(dividend.announce_date, dividend_day - days{30});
  EXPECT_EQ(dividend.record_date, dividend_day + days{1});
  EXPECT_EQ(dividend.pay_date, dividend_day + days{14});
  EXPECT_EQ(dividend.cash_per_share, price_nanos{nanos_per_unit / 4});
  EXPECT_EQ(dividend.currency, "TST");
  EXPECT_EQ(dividend.new_shares, std::nullopt);
  EXPECT_EQ(dividend.related_instrument, std::nullopt);
  EXPECT_EQ(dividend.recorded_at.size(), std::string("2026-01-31T12:00:00.000Z").size());

  EXPECT_EQ(actions[2].new_shares, 3);
  EXPECT_EQ(actions[2].old_shares, 2);
  EXPECT_EQ(actions[2].announce_date, std::nullopt);
  EXPECT_EQ(actions[3].type, corporate_action_type::spinoff);
  EXPECT_EQ(actions[3].related_instrument, acme);
}

TEST(SecmasterAdjustment, ReadsTheCorporateActionsOfOneInstrument) {
  database secmaster = secmaster_with_actions();

  EXPECT_EQ(read_corporate_actions(secmaster, acme).size(), 3u);
  EXPECT_EQ(read_corporate_actions(secmaster, bolt).size(), 1u);
  EXPECT_TRUE(read_corporate_actions(secmaster, instrument_id{99}).empty());

  const std::vector<corporate_action> of_acme = read_corporate_actions(secmaster, acme);
  const adjustments adjusted = split_adjustments(of_acme);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(first_split_day - days{1}), 1.0 / 3.0);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(dividend_day), 2.0 / 3.0);
}

}  // namespace
