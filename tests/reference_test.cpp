#include <nq/db.h>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <nq/db/secmaster/writer.h>

#include "fictional_market.h"
#include "temporary_directory.h"

namespace {

using namespace std::chrono_literals;
using namespace nqdb_test;
using nq::db::day_range;
using nq::db::listing;
using nq::db::market;
using nq::db::nanos_per_unit;
using nq::db::price_nanos;
using nq::db::quantity_nanos;
using nq::db::reference;
using nq::db::sqlite::database;
using std::chrono::days;
using std::chrono::sys_days;
namespace secmaster = nq::db::secmaster;

constexpr sys_days first_listing_day{2020y / 1 / 1};
constexpr sys_days before_rename{2024y / 5 / 31};
constexpr sys_days rename_day{2024y / 6 / 1};
constexpr sys_days monday_after_rename{2024y / 6 / 3};
constexpr sys_days dividend_day{2024y / 9 / 2};
constexpr sys_days delisting_day{2025y / 1 / 1};
constexpr day_range every_day{.from = first_listing_day, .until = sys_days{2026y / 1 / 1}};

constexpr nq::db::session_hours full_day{.opens = 9h + 30min, .closes = 16h};

database market_with_a_calendar_and_actions() {
  database secmaster = fictional_market();
  secmaster::amend_venue(secmaster, secmaster::venue_id{1},
                         {.code = {}, .mic = {}, .name = {}, .country = {},
                          .timezone = "America/New_York"});
  std::vector<secmaster::weekly_session> weekdays;
  for (const std::chrono::weekday weekday : {std::chrono::Monday, std::chrono::Tuesday,
                                             std::chrono::Wednesday, std::chrono::Thursday,
                                             std::chrono::Friday}) {
    weekdays.push_back({weekday, full_day});
  }
  secmaster::set_week(secmaster, secmaster::venue_id{1}, first_listing_day, weekdays);
  secmaster::close_day(secmaster, secmaster::venue_id{1}, sys_days{2024y / 6 / 4});

  secmaster::add_corporate_action(secmaster, {.instrument = acme,
                                                 .type = secmaster::corporate_action_type::split,
                                                 .ex_date = rename_day,
                                                 .announce_date = {},
                                                 .record_date = {},
                                                 .pay_date = {},
                                                 .cash_per_share = {},
                                                 .currency = {},
                                                 .new_shares = 2,
                                                 .old_shares = 1,
                                                 .related_instrument = {}});
  secmaster::add_corporate_action(
      secmaster, {.instrument = acme,
                  .type = secmaster::corporate_action_type::cash_dividend,
                  .ex_date = dividend_day,
                  .announce_date = {},
                  .record_date = {},
                  .pay_date = {},
                  .cash_per_share = price_nanos{nanos_per_unit},
                  .currency = "TST",
                  .new_shares = {},
                  .old_shares = {},
                  .related_instrument = {}});
  return secmaster;
}

reference whole_market() {
  database secmaster = market_with_a_calendar_and_actions();
  return reference::load(secmaster, every_day, nq::db::every_listing);
}

TEST(Reference, FindsAListingByTheSymbolItHadOnTheDay) {
  const reference ref = whole_market();
  const std::optional<listing> acme_listing = ref.find(acme_on_test_exchange);

  ASSERT_TRUE(acme_listing.has_value());
  EXPECT_EQ(ref.id(*acme_listing), acme_on_test_exchange);
  EXPECT_EQ(ref.find("XTST", "ACME", before_rename), acme_listing);
  EXPECT_EQ(ref.find("XTST", "ACME", rename_day), std::nullopt);
  EXPECT_EQ(ref.find("XTST", "ACMX", rename_day), acme_listing);
  EXPECT_EQ(ref.find("XALT", "ACMX", rename_day), ref.find(acme_on_alternative));
  EXPECT_EQ(ref.find("XTST", "ACME.T", rename_day, "VENDOR"), acme_listing);
  EXPECT_EQ(ref.find("XTST", "ACMX", rename_day, "VENDOR"), std::nullopt);
  EXPECT_EQ(ref.find("NONE", "ACME", before_rename), std::nullopt);
  EXPECT_EQ(ref.find("XTST", "NONE", before_rename), std::nullopt);
  EXPECT_EQ(ref.find(nq::db::listing_id{99}), std::nullopt);
}

TEST(Reference, ListingsAreNumberedFromZeroInIdOrder) {
  const reference ref = whole_market();

  ASSERT_EQ(ref.listing_count(), 6u);
  for (std::size_t number = 0; number < ref.listing_count(); ++number) {
    EXPECT_EQ(ref.listing_at(number).number(), number);
    EXPECT_EQ(ref.find(ref.id(ref.listing_at(number))), ref.listing_at(number));
  }
  EXPECT_EQ(ref.id(ref.listing_at(0)), acme_on_test_exchange);
  EXPECT_EQ(ref.id(ref.listing_at(5)), pair_on_test_exchange);
  EXPECT_EQ(ref.days(), every_day);
  EXPECT_EQ(ref.change_days().front(), rename_day);
}

TEST(Reference, GivesTheLastingFactsOfAListing) {
  const reference ref = whole_market();
  const listing alternative = *ref.find(acme_on_alternative);
  const listing bolt = *ref.find(bolt_on_test_exchange);

  EXPECT_EQ(ref.instrument_of(alternative).name, "Acme");
  EXPECT_EQ(ref.instrument_of(alternative).id, acme);
  EXPECT_EQ(ref.instrument_of(bolt).asset_class, nq::db::asset_class::etf);
  EXPECT_EQ(ref.venue_of(alternative).code, "XALT");
  EXPECT_EQ(ref.currency(alternative), "ALT");
  EXPECT_FALSE(ref.is_primary(alternative));
  EXPECT_TRUE(ref.is_primary(bolt));

  EXPECT_FALSE(ref.is_listed(alternative, before_rename));
  EXPECT_TRUE(ref.is_listed(alternative, rename_day));
  EXPECT_TRUE(ref.is_listed(bolt, delisting_day - days{1}));
  EXPECT_FALSE(ref.is_listed(bolt, delisting_day));

  const listing future = *ref.find(future_on_test_exchange);
  EXPECT_EQ(std::get<nq::db::future_spec>(ref.instrument_of(future).spec).root, "T1");
}

TEST(Reference, GivesTheSymbolAndTheLotSizeOfTheDay) {
  const reference ref = whole_market();
  const listing acme_listing = *ref.find(acme_on_test_exchange);
  const listing pair = *ref.find(pair_on_test_exchange);

  EXPECT_EQ(ref.symbol(acme_listing, before_rename), "ACME");
  EXPECT_EQ(ref.symbol(acme_listing, rename_day), "ACMX");
  EXPECT_EQ(ref.symbol(acme_listing, rename_day, "VENDOR"), "ACME.T");
  EXPECT_EQ(ref.symbol(pair, rename_day, "VENDOR"), std::nullopt);

  EXPECT_EQ(ref.lot_size(acme_listing, before_rename), quantity_nanos{100 * nanos_per_unit});
  EXPECT_EQ(ref.lot_size(acme_listing, rename_day), quantity_nanos{nanos_per_unit});
  EXPECT_EQ(ref.min_qty(acme_listing, rename_day), std::nullopt);
  EXPECT_EQ(ref.min_qty(pair, rename_day), quantity_nanos{5000});
}

TEST(Reference, GivesTheTickSizeOfAPriceOnTheDay) {
  const reference ref = whole_market();
  const listing acme_listing = *ref.find(acme_on_test_exchange);
  const price_nanos half{nanos_per_unit / 2};
  const price_nanos five{5 * nanos_per_unit};
  const price_nanos cent{10'000'000};

  EXPECT_EQ(ref.tick_size(acme_listing, before_rename, half), cent);
  EXPECT_EQ(ref.tick_size(acme_listing, before_rename, five), cent);
  EXPECT_EQ(ref.tick_size(acme_listing, rename_day, half), price_nanos{100'000});
  EXPECT_EQ(ref.tick_size(acme_listing, rename_day, five), cent);

  EXPECT_EQ(ref.tick_bands(acme_listing, before_rename).size(), 1u);
  const std::span<const nq::db::tick_band> banded = ref.tick_bands(acme_listing, rename_day);
  ASSERT_EQ(banded.size(), 2u);
  EXPECT_EQ(banded[1].price_from, price_nanos{nanos_per_unit});
  EXPECT_EQ(banded[1].tick_size, cent);
}

TEST(Reference, KnowsNothingOfAListingOutsideItsDaysAndItsLifetime) {
  const reference ref = whole_market();
  const listing acme_listing = *ref.find(acme_on_test_exchange);
  const listing bolt = *ref.find(bolt_on_test_exchange);
  const price_nanos five{5 * nanos_per_unit};

  for (const sys_days outside : {sys_days{2019y / 12 / 31}, every_day.until}) {
    EXPECT_FALSE(ref.is_listed(acme_listing, outside));
    EXPECT_EQ(ref.symbol(acme_listing, outside), std::nullopt);
    EXPECT_EQ(ref.lot_size(acme_listing, outside), std::nullopt);
    EXPECT_EQ(ref.tick_size(acme_listing, outside, five), std::nullopt);
    EXPECT_TRUE(ref.tick_bands(acme_listing, outside).empty());
  }
  EXPECT_EQ(ref.symbol(bolt, delisting_day), std::nullopt);
  EXPECT_EQ(ref.lot_size(bolt, delisting_day), std::nullopt);
  EXPECT_EQ(ref.tick_size(bolt, delisting_day, five), std::nullopt);
  EXPECT_EQ(ref.symbol(bolt, delisting_day - days{1}), "BOLT");
}

TEST(Reference, GivesTheTradingDaysAndSessionsOfAMarket) {
  const reference ref = whole_market();
  const market test_exchange = *ref.find_market("XTST");
  const sys_days friday{2024y / 5 / 31};
  const sys_days closed_tuesday{2024y / 6 / 4};
  const sys_days wednesday{2024y / 6 / 5};

  EXPECT_EQ(ref.market_of(*ref.find(acme_on_test_exchange)), test_exchange);
  EXPECT_EQ(ref.venue_of(test_exchange).code, "XTST");
  EXPECT_EQ(ref.find_market("NONE"), std::nullopt);

  EXPECT_TRUE(ref.is_trading_day(test_exchange, friday));
  EXPECT_FALSE(ref.is_trading_day(test_exchange, rename_day));
  EXPECT_FALSE(ref.is_trading_day(test_exchange, closed_tuesday));
  EXPECT_EQ(ref.next_trading_day(test_exchange, friday), monday_after_rename);
  EXPECT_EQ(ref.next_trading_day(test_exchange, monday_after_rename), wednesday);
  EXPECT_EQ(ref.previous_trading_day(test_exchange, wednesday), monday_after_rename);
  EXPECT_EQ(ref.trading_days(test_exchange).front(), first_listing_day);

  ASSERT_EQ(ref.local_sessions(test_exchange, friday).size(), 1u);
  EXPECT_EQ(ref.local_sessions(test_exchange, friday).front(), full_day);
  ASSERT_EQ(ref.sessions(test_exchange, friday).size(), 1u);
  EXPECT_EQ(ref.sessions(test_exchange, friday).front().opens, friday + 13h + 30min);
  EXPECT_EQ(ref.sessions(test_exchange, friday).front().closes, friday + 20h);
  EXPECT_TRUE(ref.sessions(test_exchange, closed_tuesday).empty());
}

TEST(Reference, FindsTheTradingDayOfAUtcTime) {
  const reference ref = whole_market();
  const market test_exchange = *ref.find_market("XTST");
  const market alternative = *ref.find_market("XALT");

  EXPECT_TRUE(ref.is_open(test_exchange, monday_after_rename + 13h + 30min));
  EXPECT_FALSE(ref.is_open(test_exchange, monday_after_rename + 13h + 29min));
  EXPECT_FALSE(ref.is_open(test_exchange, monday_after_rename + 20h));
  EXPECT_EQ(ref.trading_day(test_exchange, monday_after_rename + 15h), monday_after_rename);
  EXPECT_EQ(ref.trading_day(test_exchange, monday_after_rename + 21h), std::nullopt);
  EXPECT_EQ(ref.trading_day(test_exchange, rename_day + 15h), std::nullopt);

  EXPECT_FALSE(ref.is_trading_day(alternative, monday_after_rename));
  EXPECT_FALSE(ref.is_open(alternative, monday_after_rename + 15h));
  EXPECT_TRUE(ref.trading_days(alternative).empty());
}

TEST(Reference, AdjustsEarlierPricesAndVolumesForSplits) {
  const reference ref = whole_market();
  const listing acme_listing = *ref.find(acme_on_test_exchange);
  const listing alternative = *ref.find(acme_on_alternative);
  const listing bolt = *ref.find(bolt_on_test_exchange);

  EXPECT_DOUBLE_EQ(ref.price_factor(acme_listing, before_rename), 0.5);
  EXPECT_DOUBLE_EQ(ref.volume_factor(acme_listing, before_rename), 2.0);
  EXPECT_DOUBLE_EQ(ref.price_factor(acme_listing, rename_day), 1.0);
  EXPECT_DOUBLE_EQ(ref.price_factor(acme_listing, before_rename, before_rename), 1.0);
  EXPECT_DOUBLE_EQ(ref.price_factor(acme_listing, before_rename, rename_day), 0.5);
  EXPECT_DOUBLE_EQ(ref.volume_factor(acme_listing, before_rename, before_rename), 1.0);
  EXPECT_DOUBLE_EQ(ref.price_factor(alternative, before_rename), 0.5);
  EXPECT_DOUBLE_EQ(ref.price_factor(bolt, before_rename), 1.0);

  EXPECT_EQ(ref.corporate_actions(acme_listing).size(), 2u);
  EXPECT_EQ(ref.corporate_actions(acme_listing).back().ex_date, dividend_day);
  EXPECT_TRUE(ref.corporate_actions(bolt).empty());
}

TEST(Reference, AdjustsForDividendsWithTheClosesOfTheCaller) {
  const reference ref = whole_market();
  const listing acme_listing = *ref.find(acme_on_test_exchange);
  const auto close_of_fifty = [](sys_days) {
    return std::optional{price_nanos{50 * nanos_per_unit}};
  };

  const nq::db::adjustments adjusted =
      ref.adjustments_with_dividends(acme_listing, close_of_fifty);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(rename_day), 0.98);
  EXPECT_DOUBLE_EQ(adjusted.price_factor_on(before_rename), 0.49);
  EXPECT_DOUBLE_EQ(adjusted.volume_factor_on(before_rename), 2.0);

  EXPECT_THROW(ref.adjustments_with_dividends(*ref.find(acme_on_alternative), close_of_fifty),
               nq::db::error);
  EXPECT_THROW(ref.adjustments_with_dividends(
                   acme_listing, [](sys_days) { return std::optional<price_nanos>{}; }),
               nq::db::error);
}

TEST(Reference, LoadsOnlyTheListingsOfTheUniverse) {
  database secmaster = market_with_a_calendar_and_actions();
  const reference ref = reference::load(
      secmaster, every_day,
      [](const nq::db::venue& venue, const nq::db::instrument&) { return venue.code == "XALT"; });

  ASSERT_EQ(ref.listing_count(), 1u);
  EXPECT_EQ(ref.id(ref.listing_at(0)), acme_on_alternative);
  EXPECT_EQ(ref.find("XTST", "BOLT", before_rename), std::nullopt);
  EXPECT_TRUE(ref.find_market("XTST").has_value());
}

TEST(Reference, LoadRejectsWhatItsPartsReject) {
  database secmaster = market_with_a_calendar_and_actions();
  EXPECT_THROW(reference::load(secmaster, {.from = rename_day, .until = rename_day},
                               nq::db::every_listing),
               nq::db::error);
  secmaster.run_script("DELETE FROM listing_spec WHERE listing_id = 4");
  EXPECT_THROW(reference::load(secmaster, every_day, nq::db::every_listing), nq::db::error);
}

TEST(Reference, OpenReadsADatabaseFile) {
  const temporary_directory directory;
  const std::filesystem::path file = directory.path() / "secmaster.db";
  market_with_a_calendar_and_actions().copy_to(file);

  const reference ref = nq::db::open(file, every_day);
  EXPECT_EQ(ref.listing_count(), 6u);
  EXPECT_EQ(ref.symbol(*ref.find(acme_on_test_exchange), rename_day), "ACMX");

  const reference etfs = nq::db::open(
      file, every_day, [](const nq::db::venue&, const nq::db::instrument& instrument) {
        return instrument.asset_class == nq::db::asset_class::etf;
      });
  EXPECT_EQ(etfs.listing_count(), 1u);

  EXPECT_THROW(nq::db::open(directory.path() / "none.db", every_day), std::runtime_error);
  {
    database written = database::open(file);
    written.run_script("PRAGMA user_version = 999");
  }
  EXPECT_THROW(nq::db::open(file, every_day), nq::db::error);
}

class ReferenceWithEnvironment : public testing::Test {
 protected:
  ReferenceWithEnvironment() : previous_home_(std::getenv("HOME")) {
    setenv("HOME", "/home/of/the/test", 1);
    unsetenv("NQDB_PATH");
  }
  ~ReferenceWithEnvironment() override {
    setenv("HOME", previous_home_.c_str(), 1);
    unsetenv("NQDB_PATH");
  }

  std::string previous_home_;
};

TEST_F(ReferenceWithEnvironment, TheDefaultPathIsUnderHomeUnlessNqdbPathNamesAFile) {
  EXPECT_EQ(nq::db::default_path(), "/home/of/the/test/.nqdb/secmaster.db");
  setenv("NQDB_PATH", "/elsewhere/reference.db", 1);
  EXPECT_EQ(nq::db::default_path(), "/elsewhere/reference.db");
  setenv("NQDB_PATH", "", 1);
  EXPECT_EQ(nq::db::default_path(), "/home/of/the/test/.nqdb/secmaster.db");
}

}  // namespace
