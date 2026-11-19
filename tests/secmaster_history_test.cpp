#include <nq/db/secmaster/loader.h>

#include <gtest/gtest.h>

#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fictional_market.h"
#include "secmaster_test_database.h"

namespace {

using namespace std::chrono_literals;
using namespace nq::db::secmaster;
using namespace nqdb_test;
using nq::db::sqlite::database;
using std::chrono::sys_days;

constexpr sys_days first_listing_day{2020y / 1 / 1};
constexpr sys_days before_rename{2024y / 5 / 31};
constexpr sys_days rename_day{2024y / 6 / 1};
constexpr sys_days before_delisting{2024y / 12 / 31};
constexpr sys_days delisting_day{2025y / 1 / 1};

constexpr day_range every_day{.from = first_listing_day, .until = sys_days{2026y / 1 / 1}};
constexpr day_range until_rename{.from = sys_days{2024y / 1 / 1}, .until = rename_day};
constexpr day_range from_delisting{.from = delisting_day, .until = sys_days{2025y / 2 / 1}};

constexpr auto everything = [](const venue&, const instrument&) { return true; };
constexpr auto on_alternative = [](const venue& venue, const instrument&) {
  return venue.code == "XALT";
};

std::vector<listing_id> listing_ids(const history& loaded) {
  std::vector<listing_id> ids;
  for (const listing& each : loaded.listings()) ids.push_back(each.id);
  return ids;
}

template <typename Period>
std::vector<Period> copy_of(std::span<const Period> periods) {
  return {periods.begin(), periods.end()};
}

std::vector<sys_days> change_days(database& secmaster, day_range days, auto in_universe) {
  const history loaded = load_history(secmaster, days, in_universe);
  return {loaded.change_days().begin(), loaded.change_days().end()};
}

TEST(SecmasterHistory, LoadsTheListingsListedOnAnyDayOfTheRange) {
  database secmaster = fictional_market();

  const history early = load_history(secmaster, until_rename, everything);
  EXPECT_EQ(early.days(), until_rename);
  EXPECT_EQ(listing_ids(early),
            (std::vector{acme_on_test_exchange, bolt_on_test_exchange, future_on_test_exchange,
                         call_on_test_exchange, pair_on_test_exchange}));

  EXPECT_EQ(listing_ids(load_history(secmaster, from_delisting, everything)),
            (std::vector{acme_on_test_exchange, acme_on_alternative, future_on_test_exchange,
                         call_on_test_exchange, pair_on_test_exchange}));

  EXPECT_EQ(listing_ids(load_history(secmaster, every_day, everything)).size(), 6u);
}

TEST(SecmasterHistory, IndexesAreDenseAndFollowPermanentIdOrder) {
  database secmaster = fictional_market();
  const history loaded = load_history(secmaster, from_delisting, everything);

  EXPECT_EQ(loaded.listings().find(acme_on_test_exchange), listing_index{0});
  EXPECT_EQ(loaded.listings().find(acme_on_alternative), listing_index{1});
  EXPECT_EQ(loaded.listings().find(pair_on_test_exchange), listing_index{4});
  EXPECT_EQ(loaded.listings().find(bolt_on_test_exchange), std::nullopt);

  EXPECT_EQ(loaded.instruments().size(), 4u);
  EXPECT_EQ(loaded.instruments().find(acme), instrument_index{0});
  EXPECT_EQ(loaded.instruments().find(bolt_fund), std::nullopt);
  EXPECT_EQ(loaded.venues().size(), 2u);
  EXPECT_EQ(loaded.tick_rules().size(), 2u);
}

TEST(SecmasterHistory, AListingCarriesItsLifetimeAndItsPermanentFacts) {
  database secmaster = fictional_market();
  const history loaded = load_history(secmaster, every_day, everything);

  const listing& bolt = loaded.listings()[*loaded.listings().find(bolt_on_test_exchange)];
  EXPECT_EQ(bolt.listed, (validity{.from = first_listing_day, .until = delisting_day}));
  EXPECT_EQ(bolt.currency, "TST");
  EXPECT_TRUE(bolt.is_primary);
  EXPECT_EQ(loaded.instruments()[bolt.instrument].name, "Bolt Fund");
  EXPECT_EQ(loaded.venues()[bolt.venue].code, "XTST");

  const listing& alternative =
      loaded.listings()[*loaded.listings().find(acme_on_alternative)];
  EXPECT_EQ(alternative.listed, (validity{.from = rename_day, .until = std::nullopt}));
  EXPECT_FALSE(alternative.is_primary);
  EXPECT_EQ(loaded.venues()[alternative.venue].code, "XALT");
}

TEST(SecmasterHistory, IsListedFollowsTheLifetime) {
  database secmaster = fictional_market();
  const history loaded = load_history(secmaster, every_day, everything);
  const listing_index bolt = *loaded.listings().find(bolt_on_test_exchange);
  const listing_index alternative = *loaded.listings().find(acme_on_alternative);

  EXPECT_TRUE(loaded.is_listed(bolt, first_listing_day));
  EXPECT_TRUE(loaded.is_listed(bolt, before_delisting));
  EXPECT_FALSE(loaded.is_listed(bolt, delisting_day));
  EXPECT_FALSE(loaded.is_listed(alternative, before_rename));
  EXPECT_TRUE(loaded.is_listed(alternative, rename_day));
}

TEST(SecmasterHistory, SymbolAndSpecAreThoseOfTheDayAsked) {
  database secmaster = fictional_market();
  const history loaded = load_history(secmaster, every_day, everything);
  const listing_index acme_listing = *loaded.listings().find(acme_on_test_exchange);
  const listing_index pair = *loaded.listings().find(pair_on_test_exchange);

  EXPECT_EQ(loaded.symbol_on("VENUE", acme_listing, first_listing_day)->symbol, "ACME");
  EXPECT_EQ(loaded.symbol_on("VENUE", acme_listing, before_rename)->symbol, "ACME");
  EXPECT_EQ(loaded.symbol_on("VENUE", acme_listing, rename_day)->symbol, "ACMX");
  EXPECT_EQ(loaded.symbol_on("VENDOR", acme_listing, rename_day)->symbol, "ACME.T");
  EXPECT_EQ(loaded.symbol_on("VENDOR", pair, rename_day), nullptr);
  EXPECT_EQ(loaded.symbol_on("NONE", acme_listing, rename_day), nullptr);

  const spec_period& before = *loaded.spec_on(acme_listing, before_rename);
  EXPECT_EQ(before.lot_size, quantity_nanos{100 * nanos_per_unit});
  EXPECT_EQ(loaded.tick_rules()[before.tick_rule].name, "CENT");

  const spec_period& after = *loaded.spec_on(acme_listing, rename_day);
  EXPECT_EQ(after.lot_size, quantity_nanos{nanos_per_unit});
  EXPECT_EQ(after.min_qty, std::nullopt);
  EXPECT_EQ(loaded.tick_rules()[after.tick_rule].name, "BANDED");

  EXPECT_EQ(loaded.spec_on(pair, rename_day)->min_qty, quantity_nanos{5000});
}

TEST(SecmasterHistory, ListsEveryPeriodOfAListingThatOverlapsTheRange) {
  database secmaster = fictional_market();
  const symbol_period named_acme{.valid = {.from = first_listing_day, .until = rename_day},
                                 .symbol = "ACME"};
  const symbol_period named_acmx{.valid = {.from = rename_day, .until = std::nullopt},
                                 .symbol = "ACMX"};

  const history whole = load_history(secmaster, every_day, everything);
  const listing_index acme_listing = *whole.listings().find(acme_on_test_exchange);
  EXPECT_EQ(copy_of(whole.symbols_of("VENUE", acme_listing)),
            (std::vector{named_acme, named_acmx}));
  EXPECT_EQ(whole.symbols_of("VENDOR", acme_listing).size(), 1u);
  EXPECT_TRUE(whole.symbols_of("NONE", acme_listing).empty());
  EXPECT_EQ(whole.specs_of(acme_listing).size(), 2u);
  EXPECT_EQ(whole.specs_of(acme_listing)[1].valid, named_acmx.valid);
  for (const listing& other : whole.listings()) {
    if (other.id == acme_on_test_exchange) continue;
    const listing_index index = *whole.listings().find(other.id);
    EXPECT_TRUE(whole.symbols_of("VENDOR", index).empty());
    EXPECT_EQ(whole.symbols_of("VENUE", index).size(), 1u);
    EXPECT_EQ(whole.specs_of(index).size(), 1u);
  }

  const history early = load_history(secmaster, until_rename, everything);
  EXPECT_EQ(copy_of(early.symbols_of("VENUE", *early.listings().find(acme_on_test_exchange))),
            (std::vector{named_acme}));

  const history late = load_history(secmaster, from_delisting, everything);
  EXPECT_EQ(copy_of(late.symbols_of("VENUE", *late.listings().find(acme_on_test_exchange))),
            (std::vector{named_acmx}));
}

TEST(SecmasterHistory, FindsTheListingASymbolNamesOnTheDayAsked) {
  database secmaster = fictional_market();
  const history loaded = load_history(secmaster, every_day, everything);
  const venue_index test_exchange = *loaded.find_venue("XTST");
  const venue_index alternative = *loaded.find_venue("XALT");
  const auto acme_listing = loaded.listings().find(acme_on_test_exchange);

  EXPECT_EQ(loaded.find_listing("VENUE", test_exchange, "ACME", before_rename), acme_listing);
  EXPECT_EQ(loaded.find_listing("VENUE", test_exchange, "ACME", rename_day), std::nullopt);
  EXPECT_EQ(loaded.find_listing("VENUE", test_exchange, "ACMX", before_rename), std::nullopt);
  EXPECT_EQ(loaded.find_listing("VENUE", test_exchange, "ACMX", rename_day), acme_listing);
  EXPECT_EQ(loaded.find_listing("VENDOR", test_exchange, "ACME.T", before_rename), acme_listing);
  EXPECT_EQ(loaded.find_listing("VENUE", alternative, "ACMX", rename_day),
            loaded.listings().find(acme_on_alternative));
  EXPECT_EQ(loaded.find_listing("VENUE", alternative, "ACMX", before_rename), std::nullopt);
  EXPECT_EQ(loaded.find_listing("VENUE", alternative, "ACME", before_rename), std::nullopt);
  EXPECT_EQ(loaded.find_listing("VENDOR", alternative, "ACME.T", rename_day), std::nullopt);
  EXPECT_EQ(loaded.find_listing("VENUE", test_exchange, "AAAA", rename_day), std::nullopt);
  EXPECT_EQ(loaded.find_listing("VENUE", test_exchange, "ZZZZ", rename_day), std::nullopt);
  EXPECT_EQ(loaded.find_listing("NONE", test_exchange, "ACME", before_rename), std::nullopt);
  EXPECT_EQ(loaded.find_venue("NONE"), std::nullopt);
}

TEST(SecmasterHistory, ASymbolTakenAgainNamesTheListingOfTheDayAsked) {
  database secmaster = fictional_market();
  secmaster.run_script(R"sql(
INSERT INTO instrument (instrument_id, asset_class, name) VALUES (7, 'EQUITY', 'Bolt Works');
INSERT INTO listing (listing_id, instrument_id, venue_id, currency, listed_on)
  VALUES (7, 7, 1, 'TST', '2025-01-01');
INSERT INTO listing_symbol (listing_id, source, symbol, valid_from)
  VALUES (7, 'VENUE', 'BOLT', '2025-01-01');
INSERT INTO listing_spec (listing_id, valid_from, lot_size_nanos, tick_rule_id)
  VALUES (7, '2025-01-01', 1000000000, 1);
)sql");
  const history loaded = load_history(secmaster, every_day, everything);
  const venue_index test_exchange = *loaded.find_venue("XTST");

  EXPECT_EQ(loaded.find_listing("VENUE", test_exchange, "BOLT", before_delisting),
            loaded.listings().find(bolt_on_test_exchange));
  EXPECT_EQ(loaded.find_listing("VENUE", test_exchange, "BOLT", delisting_day),
            loaded.listings().find(listing_id{7}));
}

TEST(SecmasterHistory, ASymbolThatEndedNamesNothingUntilItsSuccessorStarts) {
  database secmaster = fictional_market();
  secmaster.run_script(R"sql(
INSERT INTO listing_symbol (listing_id, source, symbol, valid_from, valid_until) VALUES
  (2, 'VENDOR', 'BOLT.T', '2021-01-01', '2022-01-01'),
  (2, 'VENDOR', 'BOLT.X', '2023-01-01', '2025-01-01');
)sql");
  const history loaded = load_history(secmaster, every_day, everything);
  const listing_index bolt = *loaded.listings().find(bolt_on_test_exchange);
  const venue_index test_exchange = *loaded.find_venue("XTST");

  EXPECT_EQ(loaded.symbol_on("VENDOR", bolt, sys_days{2020y / 12 / 31}), nullptr);
  EXPECT_EQ(loaded.symbol_on("VENDOR", bolt, sys_days{2021y / 12 / 31})->symbol, "BOLT.T");
  EXPECT_EQ(loaded.symbol_on("VENDOR", bolt, sys_days{2022y / 1 / 1}), nullptr);
  EXPECT_EQ(loaded.symbol_on("VENDOR", bolt, sys_days{2023y / 1 / 1})->symbol, "BOLT.X");
  EXPECT_EQ(loaded.find_listing("VENDOR", test_exchange, "BOLT.T", sys_days{2022y / 6 / 1}),
            std::nullopt);
  EXPECT_EQ(loaded.find_listing("VENDOR", test_exchange, "BOLT.X", sys_days{2023y / 6 / 1}), bolt);

  EXPECT_EQ(change_days(secmaster, every_day, everything),
            (std::vector{sys_days{2021y / 1 / 1}, sys_days{2022y / 1 / 1}, sys_days{2023y / 1 / 1},
                         rename_day, delisting_day}));
}

TEST(SecmasterHistory, KnowsNothingOutsideItsDays) {
  database secmaster = fictional_market();
  const day_range around_rename{.from = sys_days{2024y / 5 / 1}, .until = sys_days{2024y / 7 / 1}};
  const history loaded = load_history(secmaster, around_rename, everything);
  const listing_index acme_listing = *loaded.listings().find(acme_on_test_exchange);
  const venue_index test_exchange = *loaded.find_venue("XTST");

  for (const sys_days outside : {sys_days{2024y / 4 / 30}, around_rename.until}) {
    EXPECT_FALSE(loaded.is_listed(acme_listing, outside));
    EXPECT_EQ(loaded.symbol_on("VENUE", acme_listing, outside), nullptr);
    EXPECT_EQ(loaded.spec_on(acme_listing, outside), nullptr);
    EXPECT_EQ(loaded.find_listing("VENUE", test_exchange, "ACME", outside), std::nullopt);
    EXPECT_EQ(loaded.find_listing("VENUE", test_exchange, "ACMX", outside), std::nullopt);
  }
  EXPECT_TRUE(loaded.is_listed(acme_listing, around_rename.from));
  EXPECT_EQ(loaded.symbol_on("VENUE", acme_listing, sys_days{2024y / 6 / 30})->symbol, "ACMX");
}

TEST(SecmasterHistory, ChangeDaysAreTheDaysAfterTheFirstOnWhichSomethingChanges) {
  database secmaster = fictional_market();

  EXPECT_EQ(change_days(secmaster, every_day, everything),
            (std::vector{rename_day, delisting_day}));
  EXPECT_EQ(change_days(secmaster, {.from = before_rename, .until = sys_days{2025y / 1 / 2}},
                        everything),
            (std::vector{rename_day, delisting_day}));
  EXPECT_TRUE(
      change_days(secmaster, {.from = rename_day, .until = delisting_day}, everything).empty());
  EXPECT_EQ(change_days(secmaster, every_day, on_alternative), (std::vector{rename_day}));
  EXPECT_TRUE(change_days(secmaster, every_day, [](const venue&, const instrument& instrument) {
                return instrument.asset_class == asset_class::future;
              }).empty());
}

TEST(SecmasterHistory, UniverseSelectsListingsByVenueAndInstrument) {
  database secmaster = fictional_market();

  const history alternative_only = load_history(secmaster, every_day, on_alternative);
  EXPECT_EQ(listing_ids(alternative_only), (std::vector{acme_on_alternative}));
  EXPECT_EQ(alternative_only.instruments().size(), 1u);
  EXPECT_EQ(alternative_only.symbol_sources(), (std::vector<std::string_view>{"VENUE"}));
  EXPECT_EQ(alternative_only.find_listing("VENUE", *alternative_only.find_venue("XTST"), "BOLT",
                                          before_rename),
            std::nullopt);

  const history whole = load_history(secmaster, every_day, everything);
  EXPECT_EQ(whole.symbol_sources(), (std::vector<std::string_view>{"VENDOR", "VENUE"}));
}

TEST(SecmasterHistory, RejectsAListingWithoutItsSymbolOrSpecOnAListedDayOfTheRange) {
  database secmaster = fictional_market();
  secmaster.run_script(
      "DELETE FROM listing_spec WHERE listing_id = 1 AND valid_from = '2024-06-01';"
      "DELETE FROM listing_symbol WHERE listing_id = 2 AND source = 'VENUE';"
      "DELETE FROM listing_symbol WHERE listing_id = 4;"
      "INSERT INTO listing_symbol (listing_id, source, symbol, valid_from) "
      "VALUES (4, 'VENUE', 'T1Z6', '2023-01-01');");
  const auto only = [](asset_class wanted) {
    return [=](const venue&, const instrument& instrument) {
      return instrument.asset_class == wanted;
    };
  };
  const day_range last_day_before_rename{.from = before_rename, .until = rename_day};
  const day_range first_day_of_rename{.from = rename_day, .until = sys_days{2024y / 6 / 2}};
  const day_range since_the_future_has_a_symbol{.from = sys_days{2023y / 1 / 1},
                                                .until = sys_days{2024y / 1 / 1}};
  const day_range one_day_too_early{.from = sys_days{2022y / 12 / 31},
                                    .until = sys_days{2024y / 1 / 1}};

  EXPECT_NO_THROW(load_history(secmaster, every_day, only(asset_class::option)));
  EXPECT_NO_THROW(load_history(secmaster, last_day_before_rename, only(asset_class::equity)));
  EXPECT_THROW(load_history(secmaster, first_day_of_rename, only(asset_class::equity)), error);
  EXPECT_THROW(load_history(secmaster, until_rename, only(asset_class::etf)), error);
  EXPECT_NO_THROW(
      load_history(secmaster, since_the_future_has_a_symbol, only(asset_class::future)));
  EXPECT_THROW(load_history(secmaster, one_day_too_early, only(asset_class::future)), error);
  EXPECT_THROW(load_history(secmaster, every_day, everything), error);
}

TEST(SecmasterHistory, RejectsARangeWithoutADay) {
  database secmaster = fictional_market();
  EXPECT_THROW(load_history(secmaster, {.from = rename_day, .until = rename_day}, everything),
               error);
  EXPECT_THROW(load_history(secmaster, {.from = rename_day, .until = before_rename}, everything),
               error);
  EXPECT_NO_THROW(
      load_history(secmaster, {.from = before_rename, .until = rename_day}, everything));
}

}  // namespace
