#include <nqdb/secmaster/loader.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include "secmaster_test_database.h"
#include "temporary_directory.h"

namespace {

using namespace std::chrono_literals;
using namespace nqdb::secmaster;
using nqdb::sqlite::database;
using std::chrono::sys_days;

constexpr sys_days before_rename{2024y / 5 / 31};
constexpr sys_days rename_day{2024y / 6 / 1};
constexpr sys_days after_delisting{2025y / 1 / 1};

constexpr instrument_id acme{1}, bolt_fund{2}, test_index{3}, index_future{4}, acme_call{5},
    foo_bar_pair{6};
constexpr listing_id acme_on_test_exchange{1}, bolt_on_test_exchange{2}, acme_on_alternative{3},
    pair_on_test_exchange{6};

constexpr auto everything = [](const venue&, const instrument&) { return true; };

database fictional_market() {
  database secmaster = nqdb_test::secmaster_with_venues();
  secmaster.run_script(R"sql(
INSERT INTO instrument (instrument_id, asset_class, name) VALUES
  (1, 'EQUITY', 'Acme'), (2, 'ETF', 'Bolt Fund'), (3, 'INDEX', 'Test 100'),
  (4, 'FUTURE', 'Test 100 Dec 2026'), (5, 'OPTION', 'Acme Call 50'), (6, 'CRYPTO', 'FOO/BAR');
INSERT INTO future_spec VALUES (4, 3, 'T1', '2026-12-18', 50000000000, 'CASH');
INSERT INTO option_spec VALUES (5, 1, '2026-12-18', 50000000000, 'CALL', 'AMERICAN', 100000000000);
INSERT INTO pair_spec VALUES (6, 'FOO', 'BAR');

INSERT INTO listing (listing_id, instrument_id, venue_id, currency, is_primary, listed_on,
                     delisted_on) VALUES
  (1, 1, 1, 'TST', 1, '2020-01-01', NULL),
  (2, 2, 1, 'TST', 1, '2020-01-01', '2025-01-01'),
  (3, 1, 2, 'ALT', 0, '2024-06-01', NULL),
  (4, 4, 1, 'TST', 1, '2020-01-01', NULL),
  (5, 5, 1, 'TST', 1, '2020-01-01', NULL),
  (6, 6, 1, 'BAR', 1, '2020-01-01', NULL);

INSERT INTO listing_symbol (listing_id, source, symbol, valid_from, valid_until) VALUES
  (1, 'VENUE', 'ACME', '2020-01-01', '2024-06-01'),
  (1, 'VENUE', 'ACMX', '2024-06-01', NULL),
  (1, 'VENDOR', 'ACME.T', '2020-01-01', NULL),
  (2, 'VENUE', 'BOLT', '2020-01-01', '2025-01-01'),
  (3, 'VENUE', 'ACMX', '2024-06-01', NULL),
  (4, 'VENUE', 'T1Z6', '2020-01-01', NULL),
  (5, 'VENUE', 'ACMX 261218C50', '2020-01-01', NULL),
  (6, 'VENUE', 'FOOBAR', '2020-01-01', NULL);

INSERT INTO listing_spec (listing_id, valid_from, valid_until, lot_size_nanos, min_qty_nanos,
                          tick_rule_id) VALUES
  (1, '2020-01-01', '2024-06-01', 100000000000, NULL, 1),
  (1, '2024-06-01', NULL, 1000000000, NULL, 2),
  (2, '2020-01-01', '2025-01-01', 1000000000, NULL, 1),
  (3, '2024-06-01', NULL, 1000000000, NULL, 1),
  (4, '2020-01-01', NULL, 1000000000, NULL, 1),
  (5, '2020-01-01', NULL, 1000000000, NULL, 1),
  (6, '2020-01-01', NULL, 1000, 5000, 1);
)sql");
  return secmaster;
}

const listing& listing_of(const snapshot& loaded, listing_id id) {
  return loaded.listings()[*loaded.listings().find(id)];
}

const instrument& instrument_of(const snapshot& loaded, instrument_id id) {
  return loaded.instruments()[*loaded.instruments().find(id)];
}

std::vector<std::string> symbols(const snapshot& loaded) {
  std::vector<std::string> symbols;
  for (const listing& each : loaded.listings()) symbols.push_back(each.symbol);
  return symbols;
}

TEST(SecmasterLoader, LoadsTheListingsThatTradeOnTheDay) {
  database secmaster = fictional_market();

  const snapshot early = load_snapshot(secmaster, before_rename, everything);
  EXPECT_EQ(early.as_of(), before_rename);
  EXPECT_EQ(symbols(early), (std::vector<std::string>{
                                "ACME", "BOLT", "T1Z6", "ACMX 261218C50", "FOOBAR"}));

  const snapshot late = load_snapshot(secmaster, after_delisting, everything);
  EXPECT_EQ(symbols(late), (std::vector<std::string>{
                               "ACMX", "ACMX", "T1Z6", "ACMX 261218C50", "FOOBAR"}));
}

TEST(SecmasterLoader, IndexesAreDenseAndFollowPermanentIdOrder) {
  database secmaster = fictional_market();
  const snapshot loaded = load_snapshot(secmaster, after_delisting, everything);

  EXPECT_EQ(loaded.listings().find(acme_on_test_exchange), listing_index{0});
  EXPECT_EQ(loaded.listings().find(acme_on_alternative), listing_index{1});
  EXPECT_EQ(loaded.listings().find(pair_on_test_exchange), listing_index{4});
  EXPECT_EQ(loaded.listings().find(bolt_on_test_exchange), std::nullopt);

  EXPECT_EQ(loaded.instruments().size(), 4u);
  EXPECT_EQ(loaded.instruments().find(acme), instrument_index{0});
  EXPECT_EQ(loaded.instruments().find(index_future), instrument_index{1});
  EXPECT_EQ(loaded.instruments().find(bolt_fund), std::nullopt);

  EXPECT_EQ(loaded.venues().find(venue_id{1}), venue_index{0});
  EXPECT_EQ(loaded.venues().find(venue_id{2}), venue_index{1});
  EXPECT_EQ(loaded.tick_rules().find(tick_rule_id{1}), tick_rule_index{0});
  EXPECT_EQ(loaded.tick_rules().find(tick_rule_id{2}), tick_rule_index{1});
}

TEST(SecmasterLoader, ListingCarriesTheSymbolAndSpecOfTheDay) {
  database secmaster = fictional_market();

  const snapshot early = load_snapshot(secmaster, before_rename, everything);
  const listing& acme_before = listing_of(early, acme_on_test_exchange);
  EXPECT_EQ(acme_before.symbol, "ACME");
  EXPECT_EQ(acme_before.lot_size, quantity_nanos{100 * nanos_per_unit});
  EXPECT_EQ(early.tick_rules()[acme_before.tick_rule].name, "CENT");

  const snapshot renamed = load_snapshot(secmaster, rename_day, everything);
  const listing& acme_after = listing_of(renamed, acme_on_test_exchange);
  EXPECT_EQ(acme_after.symbol, "ACMX");
  EXPECT_EQ(acme_after.currency, "TST");
  EXPECT_TRUE(acme_after.is_primary);
  EXPECT_EQ(acme_after.lot_size, quantity_nanos{nanos_per_unit});
  EXPECT_EQ(acme_after.min_qty, std::nullopt);
  EXPECT_EQ(renamed.tick_rules()[acme_after.tick_rule].name, "BANDED");
  EXPECT_EQ(renamed.instruments()[acme_after.instrument].name, "Acme");
  EXPECT_EQ(renamed.venues()[acme_after.venue].code, "XTST");

  EXPECT_FALSE(listing_of(renamed, acme_on_alternative).is_primary);

  const listing& pair = listing_of(renamed, pair_on_test_exchange);
  EXPECT_EQ(pair.lot_size, quantity_nanos{1000});
  EXPECT_EQ(pair.min_qty, quantity_nanos{5000});
}

TEST(SecmasterLoader, FindsAListingByTheSymbolOfAnySource) {
  database secmaster = fictional_market();
  const snapshot loaded = load_snapshot(secmaster, rename_day, everything);
  const venue_index test_exchange = *loaded.find_venue("XTST");
  const venue_index alternative = *loaded.find_venue("XALT");

  const auto acme_listing = loaded.listings().find(acme_on_test_exchange);
  EXPECT_EQ(loaded.find_listing("VENUE", test_exchange, "ACMX"), acme_listing);
  EXPECT_EQ(loaded.find_listing("VENDOR", test_exchange, "ACME.T"), acme_listing);
  EXPECT_EQ(loaded.find_listing("VENUE", alternative, "ACMX"),
            loaded.listings().find(acme_on_alternative));
  EXPECT_EQ(loaded.find_listing("VENUE", test_exchange, "ACME"), std::nullopt);
  EXPECT_EQ(loaded.find_listing("VENDOR", alternative, "ACME.T"), std::nullopt);
  EXPECT_EQ(loaded.find_venue("NONE"), std::nullopt);
}

TEST(SecmasterLoader, VenuesKeepTheirOptionalFields) {
  database secmaster = fictional_market();
  const snapshot loaded = load_snapshot(secmaster, rename_day, everything);

  const venue& test_exchange = loaded.venues()[*loaded.find_venue("XTST")];
  EXPECT_EQ(test_exchange.id, venue_id{1});
  EXPECT_EQ(test_exchange.mic, "XTST");
  EXPECT_EQ(test_exchange.country, "ZZ");
  EXPECT_EQ(test_exchange.timezone, "Etc/UTC");

  const venue& alternative = loaded.venues()[*loaded.find_venue("XALT")];
  EXPECT_EQ(alternative.mic, std::nullopt);
  EXPECT_EQ(alternative.country, std::nullopt);
}

TEST(SecmasterLoader, InstrumentsCarryTheSpecOfTheirAssetClass) {
  database secmaster = fictional_market();
  const snapshot loaded = load_snapshot(secmaster, rename_day, everything);

  EXPECT_EQ(instrument_of(loaded, acme).asset_class, asset_class::equity);
  EXPECT_TRUE(std::holds_alternative<no_spec>(instrument_of(loaded, acme).spec));

  const future_spec& future = std::get<future_spec>(instrument_of(loaded, index_future).spec);
  EXPECT_EQ(future.underlying, test_index);
  EXPECT_EQ(future.root, "T1");
  EXPECT_EQ(future.expiry, sys_days{2026y / 12 / 18});
  EXPECT_EQ(future.multiplier, multiplier_nanos{50 * nanos_per_unit});
  EXPECT_EQ(future.settlement, settlement_method::cash);
  EXPECT_EQ(loaded.instruments().find(future.underlying), std::nullopt);

  const option_spec& option = std::get<option_spec>(instrument_of(loaded, acme_call).spec);
  EXPECT_EQ(option.underlying, acme);
  EXPECT_EQ(option.expiry, sys_days{2026y / 12 / 18});
  EXPECT_EQ(option.strike, price_nanos{50 * nanos_per_unit});
  EXPECT_EQ(option.right, option_right::call);
  EXPECT_EQ(option.style, exercise_style::american);
  EXPECT_EQ(option.multiplier, multiplier_nanos{100 * nanos_per_unit});

  const pair_spec& pair = std::get<pair_spec>(instrument_of(loaded, foo_bar_pair).spec);
  EXPECT_EQ(pair.base_currency, "FOO");
  EXPECT_EQ(pair.quote_currency, "BAR");
}

TEST(SecmasterLoader, ReadsEveryNameTheSchemaAllows) {
  database secmaster = nqdb_test::secmaster_with_venues();
  secmaster.run_script(R"sql(
INSERT INTO instrument (instrument_id, asset_class, name) VALUES
  (1, 'EQUITY', 'Acme'), (2, 'ETF', 'Bolt Fund'), (3, 'FUTURE', 'Cash Future'),
  (4, 'OPTION', 'Acme Put'), (5, 'FX', 'FOO/BAR'), (6, 'CRYPTO', 'BAZ/BAR'),
  (7, 'BOND', 'Acme 2030'), (8, 'INDEX', 'Test 100'), (9, 'FUTURE', 'Physical Future'),
  (10, 'OPTION', 'Acme Call');
INSERT INTO future_spec VALUES
  (3, 8, 'T1', '2026-12-18', 1000000000, 'CASH'),
  (9, 8, 'T2', '2026-12-18', 1000000000, 'PHYSICAL');
INSERT INTO option_spec VALUES
  (4, 1, '2026-12-18', 1000000000, 'PUT', 'EUROPEAN', 1000000000),
  (10, 1, '2026-12-18', 1000000000, 'CALL', 'AMERICAN', 1000000000);
INSERT INTO pair_spec VALUES (5, 'FOO', 'BAR'), (6, 'BAZ', 'BAR');
INSERT INTO listing (listing_id, instrument_id, venue_id, currency, listed_on)
  SELECT instrument_id, instrument_id, 1, 'TST', '2020-01-01' FROM instrument;
INSERT INTO listing_symbol (listing_id, source, symbol, valid_from)
  SELECT listing_id, 'VENUE', 'SYMBOL' || listing_id, '2020-01-01' FROM listing;
INSERT INTO listing_spec (listing_id, valid_from, lot_size_nanos, tick_rule_id)
  SELECT listing_id, '2020-01-01', 1000000000, 1 FROM listing;
)sql");
  const snapshot loaded = load_snapshot(secmaster, rename_day, everything);
  const auto asset_class_of = [&](std::int64_t id) {
    return instrument_of(loaded, instrument_id{id}).asset_class;
  };
  const auto spec_of = [&](std::int64_t id) {
    return instrument_of(loaded, instrument_id{id}).spec;
  };

  EXPECT_EQ(asset_class_of(1), asset_class::equity);
  EXPECT_EQ(asset_class_of(2), asset_class::etf);
  EXPECT_EQ(asset_class_of(3), asset_class::future);
  EXPECT_EQ(asset_class_of(4), asset_class::option);
  EXPECT_EQ(asset_class_of(5), asset_class::fx);
  EXPECT_EQ(asset_class_of(6), asset_class::crypto);
  EXPECT_EQ(asset_class_of(7), asset_class::bond);
  EXPECT_EQ(asset_class_of(8), asset_class::index);

  EXPECT_EQ(std::get<future_spec>(spec_of(3)).settlement, settlement_method::cash);
  EXPECT_EQ(std::get<future_spec>(spec_of(9)).settlement, settlement_method::physical);
  EXPECT_EQ(std::get<option_spec>(spec_of(4)).right, option_right::put);
  EXPECT_EQ(std::get<option_spec>(spec_of(4)).style, exercise_style::european);
  EXPECT_EQ(std::get<option_spec>(spec_of(10)).right, option_right::call);
  EXPECT_EQ(std::get<option_spec>(spec_of(10)).style, exercise_style::american);
  EXPECT_EQ(std::get<pair_spec>(spec_of(5)).base_currency, "FOO");
  EXPECT_EQ(std::get<pair_spec>(spec_of(6)).base_currency, "BAZ");
  EXPECT_TRUE(std::holds_alternative<no_spec>(spec_of(2)));
  EXPECT_TRUE(std::holds_alternative<no_spec>(spec_of(7)));
  EXPECT_TRUE(std::holds_alternative<no_spec>(spec_of(8)));
}

TEST(SecmasterLoader, TickSizeFollowsThePriceBand) {
  database secmaster = fictional_market();
  const snapshot loaded = load_snapshot(secmaster, rename_day, everything);
  const tick_rule& banded =
      loaded.tick_rules()[listing_of(loaded, acme_on_test_exchange).tick_rule];

  EXPECT_EQ(banded.tick_size_at(price_nanos{-1}), price_nanos{100'000});
  EXPECT_EQ(banded.tick_size_at(price_nanos{0}), price_nanos{100'000});
  EXPECT_EQ(banded.tick_size_at(price_nanos{nanos_per_unit - 1}), price_nanos{100'000});
  EXPECT_EQ(banded.tick_size_at(price_nanos{nanos_per_unit}), price_nanos{10'000'000});
  EXPECT_EQ(banded.tick_size_at(price_nanos{500 * nanos_per_unit}), price_nanos{10'000'000});
}

TEST(SecmasterLoader, UniverseSelectsListingsByVenueAndInstrument) {
  database secmaster = fictional_market();
  const snapshot stocks_on_test_exchange =
      load_snapshot(secmaster, rename_day, [](const venue& venue, const instrument& instrument) {
        return venue.code == "XTST" && (instrument.asset_class == asset_class::equity ||
                                        instrument.asset_class == asset_class::etf);
      });

  EXPECT_EQ(symbols(stocks_on_test_exchange), (std::vector<std::string>{"ACMX", "BOLT"}));
  EXPECT_EQ(stocks_on_test_exchange.instruments().size(), 2u);
  EXPECT_EQ(stocks_on_test_exchange.venues().size(), 2u);
  const venue_index alternative = *stocks_on_test_exchange.find_venue("XALT");
  EXPECT_EQ(stocks_on_test_exchange.find_listing("VENUE", alternative, "ACMX"), std::nullopt);
}

TEST(SecmasterLoader, RejectsAnIncompleteListingOnlyInsideTheUniverse) {
  database secmaster = fictional_market();
  secmaster.run_script("DELETE FROM listing_spec WHERE listing_id = 3;"
                       "DELETE FROM listing_symbol WHERE listing_id = 4;");
  const auto on_venue = [](std::string_view code, asset_class wanted) {
    return [=](const venue& venue, const instrument& instrument) {
      return venue.code == code && instrument.asset_class == wanted;
    };
  };

  EXPECT_NO_THROW(load_snapshot(secmaster, rename_day, on_venue("XTST", asset_class::equity)));
  EXPECT_THROW(load_snapshot(secmaster, rename_day, on_venue("XALT", asset_class::equity)), error);
  EXPECT_THROW(load_snapshot(secmaster, rename_day, on_venue("XTST", asset_class::future)), error);
}

TEST(SecmasterLoader, RejectsADerivativeWithoutItsSpec) {
  database secmaster = fictional_market();
  secmaster.run_script("DELETE FROM option_spec");
  EXPECT_THROW(load_snapshot(secmaster, rename_day, everything), error);
}

TEST(SecmasterLoader, RejectsATickRuleWithoutBands) {
  database secmaster = fictional_market();
  secmaster.run_script("INSERT INTO tick_rule (tick_rule_id, name) VALUES (3, 'EMPTY')");
  EXPECT_THROW(load_snapshot(secmaster, rename_day, everything), error);
}

TEST(SecmasterLoader, RejectsADatabaseOfAnotherSchemaVersion) {
  database secmaster = fictional_market();
  secmaster.run_script("PRAGMA user_version = 999");
  EXPECT_THROW(load_snapshot(secmaster, rename_day, everything), error);

  database without_tables = database::in_memory();
  EXPECT_THROW(load_snapshot(without_tables, rename_day, everything), error);
}

TEST(SecmasterLoader, LoadsFromAnArchivedCopy) {
  const nqdb_test::temporary_directory directory;
  database secmaster = fictional_market();
  database archived = database::open_read_only(archive(secmaster, directory.path(), rename_day));

  EXPECT_EQ(symbols(load_snapshot(archived, rename_day, everything)),
            symbols(load_snapshot(secmaster, rename_day, everything)));
}

}  // namespace
